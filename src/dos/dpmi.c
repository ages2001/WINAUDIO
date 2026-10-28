/* dpmi.c - DPMI 0.9 host of the WINAUDIO V86 monitor.
   Protected-mode programs (DOS/4GW, PMODE/W, DOS/32A, DJGPP and Borland 16-bit programs) find
   it through INT 2Fh AX=1687h. Clients run in ring 3 with IOPL 0, so the Sound Blaster ports
   are trapped for them the same way as for real-mode programs; CLI and STI are emulated.
   Memory comes from XMS and is mapped from 40000000h on; interrupts are reflected between
   the modes as the DPMI specification describes. Up to six clients can be nested (a DPMI
   program that starts another one). */
#include "core.h"
#include "audio.h"
#include "sbemu.h"
#include "vmon.h"

#define NLDT        4096
#define MAXCL       6
#define NMEM        256
#define NCHUNK      512
#define NDOSM       64
#define ARENA_LO    0x40000000UL
#define ARENA_HI    0x7FC00000UL
#define DPMI_PARAS  0x100               /* host data of a client: its real-mode stack */
#define LPMS_SIZE   16384
#define STUB_EXCDEF 0x200               /* + exception: the host's default exception handler */

struct dpmicl {
    u8 is32, idx;
    u16 psp, envseg, envsel;
    u16 hseg;                           /* host data (real-mode stack) */
    u32 nest;                           /* vm_nest when it started */
    int nrm;                            /* V86 contexts below it then */
    u32 pmoff[256];
    u16 pmsel[256];
    u32 exoff[32];
    u16 exsel[32];
    struct { u8 vec; u32 old, cur; } rmv[32];      /* real-mode vectors set by 0201h */
    int nrmv;
};

static struct dpmicl cls[MAXCL];
static int ncl;
static u16 mcb0;                        /* the first DOS memory block */
struct dpmicl *dcl;

static u64 *ldt;
static u8 lown[NLDT];                   /* 0 free, client + 1 */
static u8 lfix[NLDT];                   /* made by 0002h: cannot be changed or freed */
static u32 stub_lin, lpms_lin, lpms_cur;
static u8 xms3, xmsver;

static struct { u8 used, owner; u16 pmsel; u32 pmoff; u16 rsel; u32 roff; } cbs[VM_NCB];
static struct { u16 seg, sel; u8 owner; } s2s[64];
static struct { u16 sel, seg, n; u8 owner; } dosm[NDOSM];

struct mblk { u32 lin, size, rsv; u16 first; u8 owner, phys; };
struct mchunk { u16 xh, next; u32 phys, bytes, off; u8 used; };
static struct mblk mb[NMEM];
static struct mchunk mc[NCHUNK];
static u32 *ptv[256];                   /* page tables of 40000000h..7FFFFFFFh */

#define HANDLE(i)   (0x44500000UL | (u32)(i))

static u16 stubsel(struct dpmicl *c) { return c->is32 ? SEL_STUB32 : SEL_STUB16; }
static u16 lpmssel(struct dpmicl *c) { return c->is32 ? SEL_LPMS32 : SEL_LPMS16; }

/* ---------------------------------------------------------------- descriptors */

static u64 *gdtp(void)
{
    return *(u64 **)(vm_gdtr + 2);
}

static u64 *descp(u32 sel)
{
    u32 i = (sel & 0xFFFF) >> 3;
    if (sel & 4) return i < NLDT ? &ldt[i] : NULL;
    return i && i < VM_NGDT ? &gdtp()[i] : NULL;
}

static u32 d_base(u64 d)
{
    return (u32)((d >> 16) & 0xFFFFFF) | ((u32)(d >> 56) << 24);
}

static u32 d_limit(u64 d)
{
    u32 l = (u32)(d & 0xFFFF) | (u32)((d >> 32) & 0xF0000UL);
    if (d & (1ULL << 55)) l = (l << 12) | 0xFFF;
    return l;
}

u32 sel_base(u32 sel, int *big, u32 *limit)
{
    u64 *d = descp(sel), v;
    if (!d || !(sel & 0xFFFC)) {
        if (big) *big = 0;
        if (limit) *limit = 0;
        return 0;
    }
    v = *d;
    if (big) *big = (v & (1ULL << 54)) != 0;
    if (limit) *limit = d_limit(v);
    return d_base(v);
}

static u16 mksel(int i)
{
    return (u16)((i << 3) | 7);
}

static int ldt_alloc(int n, int from)
{
    int i, k;
    for (i = from; i + n <= NLDT; i++) {
        for (k = 0; k < n && !lown[i + k]; k++) ;
        if (k == n) return i;
        i += k;
    }
    return -1;
}

static void ldt_set(int i, u64 d, struct dpmicl *c)
{
    ldt[i] = d;
    lown[i] = (u8)(c->idx + 1);
}

static u16 newsel(struct dpmicl *c, u32 base, u32 limit, u32 access, int big)
{
    int i = ldt_alloc(1, 16);
    if (i < 0) return 0;
    ldt_set(i, mkdesc(base, limit, access, big ? 0x40 : 0), c);
    return mksel(i);
}

static int mysel(struct dpmicl *c, u32 sel)
{
    u32 i = (sel & 0xFFFF) >> 3;
    return (sel & 4) && i && i < NLDT && lown[i] == c->idx + 1;
}

/* a segment register of the frame that holds a freed selector becomes 0 */
static void selgone(struct vmregs *f, u32 sel)
{
    sel &= 0xFFF8;
    if ((f->ds & 0xFFF8) == sel) f->ds = 0;
    if ((f->es & 0xFFF8) == sel) f->es = 0;
    if ((f->fs & 0xFFF8) == sel) f->fs = 0;
    if ((f->gs & 0xFFF8) == sel) f->gs = 0;
}

/* ---------------------------------------------------------------- client memory */

static u32 cptr(struct vmregs *f, u32 sel, u32 off, u32 len)
{
    u32 l = sel_base(sel, NULL, NULL) + (dcl->is32 ? off : off & 0xFFFF);
    (void)f;
    return lin_ok(l, len) ? l : 0;
}

/* push / pop on the stack SS:ESP of t (16- or 32-bit stack segment) */
static void spush(struct vmregs *t, u32 v, int size)
{
    int big;
    u32 b = sel_base(t->ss, &big, NULL), a;
    if (big) {
        t->esp -= (u32)size;
        a = b + t->esp;
    } else {
        u16 sp = (u16)(t->esp - (u32)size);
        t->esp = (t->esp & 0xFFFF0000UL) | sp;
        a = b + sp;
    }
    if (size == 4) lin_wr32(a, v);
    else lin_wr16(a, (u16)v);
}

static u32 spop(struct vmregs *t, int size)
{
    int big;
    u32 b = sel_base(t->ss, &big, NULL), a, v;
    a = b + (big ? t->esp : (t->esp & 0xFFFF));
    v = size == 4 ? lin_rd32(a) : lin_rd16(a);
    if (big) t->esp += (u32)size;
    else t->esp = (t->esp & 0xFFFF0000UL) | (u16)(t->esp + (u32)size);
    return v;
}

u32 dpmi_lin_end(void)
{
    return ARENA_HI;
}

int dpmi_mapped(u32 lin, u32 len)
{
    u32 a;
    if (lin < ARENA_LO || lin + len > ARENA_HI || lin + len < lin) return 0;
    for (a = lin & ~4095UL; a < lin + len; a += 4096) {
        u32 *pt = ptv[(a >> 22) - 256];
        if (!pt || !(pt[(a >> 12) & 1023] & 1)) return 0;
    }
    return 1;
}

static u32 *pte(u32 lin)
{
    u32 i = lin >> 22;
    u32 *pt;
    if (i < 256 || i > 510) return NULL;
    pt = ptv[i - 256];
    if (!pt) {
        pt = kmalloc_align(4096, 4096);
        if (!pt) return NULL;
        memset(pt, 0, 4096);
        ptv[i - 256] = pt;
        ((u32 *)CORE_WIN)[i] = vphys(pt) | 7;
        wr_cr3(rd_cr3());
    }
    return &pt[(lin >> 12) & 1023];
}

static int map(u32 lin, u32 phys, u32 bytes, u32 fl)
{
    u32 o;
    for (o = 0; o < bytes; o += 4096) {
        u32 *p = pte(lin + o);
        if (!p) return -1;
        *p = (phys + o) | fl;
        invlpg(lin + o);
    }
    return 0;
}

static void unmap(u32 lin, u32 bytes)
{
    u32 o;
    for (o = 0; o < bytes; o += 4096) {
        u32 a = lin + o, *pt = ptv[(a >> 22) - 256];
        if (pt) {
            pt[(a >> 12) & 1023] = 0;
            invlpg(a);
        }
    }
}

/* a free range of linear addresses */
static u32 lin_find(u32 size, int skip)
{
    u32 a = ARENA_LO;
    int i, moved;
    do {
        moved = 0;
        for (i = 0; i < NMEM; i++) {
            if (!mb[i].size || i == skip) continue;
            if (a < mb[i].lin + mb[i].rsv && a + size > mb[i].lin) {
                a = (mb[i].lin + mb[i].rsv + 0xFFFF) & ~0xFFFFUL;
                moved = 1;
            }
        }
    } while (moved && a + size <= ARENA_HI && a + size > a);
    return a + size <= ARENA_HI && a + size > a ? a : 0;
}

/* ---------------------------------------------------------------- XMS */

static void xms_call(struct vmregs *f, struct vmregs *t)
{
    u32 e = vmc->xms;
    t->cs = e >> 16;
    t->eip = e & 0xFFFF;
    rm_call(t, 0);
    (void)f;
}

static int xms_prep(struct vmregs *t, struct vmregs *f)
{
    if (rm_setup(t, (f && (f->eflags & EFL_VM)) ? f : NULL)) return -1;
    if (!xmsver) {
        struct vmregs u = *t;
        u.eax = 0;
        xms_call(f, &u);
        xmsver = 1;
        xms3 = (u.eax & 0xFFFF) >= 0x0300;
        rm_setup(t, (f && (f->eflags & EFL_VM)) ? f : NULL);
    }
    return 0;
}

static int xms_alloc(struct vmregs *f, u32 kb, u16 *h, u32 *phys)
{
    struct vmregs t;
    if (xms_prep(&t, f)) return -1;
    if (xms3) {
        t.eax = 0x8900;
        t.edx = kb;
    } else {
        if (kb > 0xFFFF) return -1;
        t.eax = 0x0900;
        t.edx = kb;
    }
    xms_call(f, &t);
    if ((t.eax & 0xFFFF) != 1) return -1;
    *h = (u16)t.edx;
    rm_setup(&t, (f && (f->eflags & EFL_VM)) ? f : NULL);
    t.eax = 0x0C00;
    t.edx = *h;
    xms_call(f, &t);
    if ((t.eax & 0xFFFF) != 1) {
        rm_setup(&t, (f && (f->eflags & EFL_VM)) ? f : NULL);
        t.eax = 0x0A00;
        t.edx = *h;
        xms_call(f, &t);
        return -1;
    }
    *phys = ((t.edx & 0xFFFF) << 16) | (t.ebx & 0xFFFF);
    return 0;
}

static void xms_free(struct vmregs *f, u16 h)
{
    struct vmregs t;
    if (xms_prep(&t, f)) return;
    t.eax = 0x0D00;
    t.edx = h;
    xms_call(f, &t);
    rm_setup(&t, (f && (f->eflags & EFL_VM)) ? f : NULL);
    t.eax = 0x0A00;
    t.edx = h;
    xms_call(f, &t);
}

static void xms_avail(struct vmregs *f, u32 *largest, u32 *total)
{
    struct vmregs t;
    *largest = *total = 0;
    if (xms_prep(&t, f)) return;
    t.eax = xms3 ? 0x8800 : 0x0800;
    xms_call(f, &t);
    if (xms3) {
        *largest = t.eax;
        *total = t.edx;
    } else {
        *largest = t.eax & 0xFFFF;
        *total = t.edx & 0xFFFF;
    }
}

/* ---------------------------------------------------------------- memory blocks */

static int chunk_new(struct vmregs *f, u32 bytes, u32 off)
{
    int i;
    for (i = 1; i < NCHUNK && mc[i].used; i++) ;
    if (i == NCHUNK) return -1;
    if (xms_alloc(f, bytes >> 10, &mc[i].xh, &mc[i].phys)) return -1;
    mc[i].used = 1;
    mc[i].bytes = bytes;
    mc[i].off = off;
    mc[i].next = 0;
    return i;
}

static void blk_map(struct mblk *b)
{
    u16 k;
    for (k = b->first; k; k = mc[k].next) {
        u32 n = mc[k].bytes;
        if (mc[k].off >= b->size) continue;
        if (mc[k].off + n > b->size) n = b->size - mc[k].off;
        map(b->lin + mc[k].off, mc[k].phys, n, 7);
    }
}

static int mem_alloc(struct vmregs *f, u32 size, u32 *lin)
{
    u32 bytes = (size + 4095) & ~4095UL, rsv;
    int i, k;
    if (!bytes) return -2;
    for (i = 0; i < NMEM && mb[i].size; i++) ;
    if (i == NMEM) return -3;
    rsv = bytes < 0x400000UL ? 0x400000UL : bytes;
    mb[i].lin = lin_find(rsv, -1);
    if (!mb[i].lin) {
        rsv = bytes;
        mb[i].lin = lin_find(rsv, -1);
        if (!mb[i].lin) return -1;
    }
    k = chunk_new(f, bytes, 0);
    if (k < 0) return -2;
    mb[i].size = bytes;
    mb[i].rsv = rsv;
    mb[i].first = (u16)k;
    mb[i].owner = (u8)(dcl->idx + 1);
    mb[i].phys = 0;
    blk_map(&mb[i]);
    *lin = mb[i].lin;
    return i;
}

static void mem_free(struct vmregs *f, int i)
{
    u16 k, n;
    unmap(mb[i].lin, mb[i].size);
    for (k = mb[i].first; k; k = n) {
        n = mc[k].next;
        if (!mb[i].phys) xms_free(f, mc[k].xh);
        mc[k].used = 0;
    }
    memset(&mb[i], 0, sizeof(mb[i]));
}

static int mem_resize(struct vmregs *f, int i, u32 size)
{
    struct mblk *b = &mb[i];
    u32 nb = (size + 4095) & ~4095UL, nl;
    u16 k, last;
    if (!nb) return -2;
    if (nb > b->size) {
        if (b->lin + nb > b->lin + b->rsv) {
            u32 rsv = nb < 0x400000UL ? 0x400000UL : nb * 2;
            nl = lin_find(rsv, i);
            if (!nl) {
                rsv = nb;
                nl = lin_find(rsv, i);
            }
            if (!nl) return -1;
            unmap(b->lin, b->size);
            b->lin = nl;
            b->rsv = rsv;
            blk_map(b);
        }
        for (last = b->first; mc[last].next; last = mc[last].next) ;
        if (mc[last].off + mc[last].bytes >= nb) {
            b->size = nb;
        } else {
            u32 from = mc[last].off + mc[last].bytes;
            int c = chunk_new(f, nb - from, from);
            if (c < 0) return -2;
            mc[last].next = (u16)c;
            b->size = nb;
        }
        blk_map(b);
    } else if (nb < b->size) {
        unmap(b->lin + nb, b->size - nb);
        b->size = nb;
        for (k = b->first; k; k = mc[k].next) {
            while (mc[k].next && mc[mc[k].next].off >= nb) {
                u16 d = mc[k].next;
                mc[k].next = mc[d].next;
                xms_free(f, mc[d].xh);
                mc[d].used = 0;
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- calls into the client */

void pm_call(struct vmregs *t);

static int isdef(struct dpmicl *c, int v)
{
    return c->pmsel[v] == stubsel(c) && c->pmoff[v] == STUB_DEF + (u32)v;
}

int dpmi_hooked(int vec)
{
    return dcl && !isdef(dcl, vec);
}

/* stack for a call of the host into the client: the client's own in protected mode, else the
   locked stack */
static void pm_stack(struct vmregs *t, struct vmregs *f, struct dpmicl *c)
{
    if (!(f->eflags & EFL_VM)) {
        t->ss = f->ss;
        t->esp = f->esp;
    } else {
        t->ss = lpmssel(c);
        t->esp = lpms_cur;
    }
}

static void lpms_track(struct vmregs *t, u32 *old)
{
    *old = lpms_cur;
    if ((t->ss & 0xFFFF) == SEL_LPMS32 || (t->ss & 0xFFFF) == SEL_LPMS16) lpms_cur = t->esp & ~3UL;
}

/* a hardware interrupt goes to the client's protected-mode handler */
int dpmi_hwint(struct vmregs *f, int vec)
{
    struct dpmicl *c = dcl;
    struct vmregs t, save;
    u32 old;
    int sz;
    if (!c || isdef(c, vec)) return 0;
    save = *f;
    t = *f;
    sz = c->is32 ? 4 : 2;
    if (f->eflags & EFL_VM) {
        rmstk_push(f->ss, f->esp);
        t.ds = t.es = t.fs = t.gs = 0;
    }
    pm_stack(&t, f, c);
    t.eflags = (f->eflags & ~(EFL_VM | EFL_IOPL | EFL_NT | EFL_RF)) | 2;
    spush(&t, t.eflags, sz);
    spush(&t, stubsel(c), sz);
    spush(&t, STUB_IRET, sz);
    t.cs = c->pmsel[vec];
    t.eip = c->pmoff[vec];
    t.eflags &= ~(EFL_IF | EFL_TF);
    lpms_track(&t, &old);
    pm_call(&t);
    lpms_cur = old;
    if (save.eflags & EFL_VM) rmstk_pop();
    *f = save;
    return 1;
}

/* INT 1Ch, 23h, 24h in real mode: to the client's protected-mode handler */
int dpmi_rmint(struct vmregs *f, int vec)
{
    struct dpmicl *c = dcl;
    struct vmregs t;
    u32 old;
    int sz;
    if (!c || isdef(c, vec)) return 0;
    t = *f;
    sz = c->is32 ? 4 : 2;
    rmstk_push(f->ss, f->esp);
    t.ds = t.es = t.fs = t.gs = 0;
    pm_stack(&t, f, c);
    t.eflags = (f->eflags & ~(EFL_VM | EFL_IOPL | EFL_NT | EFL_RF)) | 2;
    spush(&t, t.eflags, sz);
    spush(&t, stubsel(c), sz);
    spush(&t, STUB_IRET, sz);
    t.cs = c->pmsel[vec];
    t.eip = c->pmoff[vec];
    t.eflags &= ~EFL_TF;
    if (vec != 0x23 && vec != 0x24) t.eflags &= ~EFL_IF;
    lpms_track(&t, &old);
    pm_call(&t);
    lpms_cur = old;
    rmstk_pop();
    f->eax = t.eax;
    f->ebx = t.ebx;
    f->ecx = t.ecx;
    f->edx = t.edx;
    f->esi = t.esi;
    f->edi = t.edi;
    f->ebp = t.ebp;
    f->eflags = (f->eflags & ~EFL_ARITH) | (t.eflags & EFL_ARITH);
    return 1;
}

/* a hardware interrupt in protected mode without a handler there: real mode */
void dpmi_rmirq(struct vmregs *f, int vec)
{
    struct vmregs t, save = *f;
    if (rm_setup(&t, NULL)) return;
    rm_int(&t, vec);
    *f = save;
}

/* software interrupt from protected mode to real mode with the client's registers */
static void pm_reflect(struct vmregs *f, int v)
{
    struct vmregs t;
    if (rm_setup(&t, NULL)) return;
    t.eax = f->eax;
    t.ebx = f->ebx;
    t.ecx = f->ecx;
    t.edx = f->edx;
    t.esi = f->esi;
    t.edi = f->edi;
    t.ebp = f->ebp;
    t.eflags = (f->eflags & (EFL_ARITH | EFL_DF)) | EFL_IF | 2;
    rm_int(&t, v);
    f->eax = t.eax;
    f->ebx = t.ebx;
    f->ecx = t.ecx;
    f->edx = t.edx;
    f->esi = t.esi;
    f->edi = t.edi;
    f->ebp = t.ebp;
    f->eflags = (f->eflags & ~(EFL_ARITH | EFL_DF)) | (t.eflags & (EFL_ARITH | EFL_DF));
}

/* ---------------------------------------------------------------- client start and end */

static int rd_psp(struct vmregs *f)
{
    struct vmregs t;
    if (rm_setup(&t, f)) return 0;
    t.eax = 0x6200;
    rm_int(&t, 0x21);
    return (int)(t.ebx & 0xFFFF);
}

/* owner of the DOS memory block holding lin: a PSP, 0 free, 0xFFFF not in a block */
static u16 mem_owner(u32 lin)
{
    u16 seg = mcb0;
    int n;
    for (n = 0; seg && n < 4096; n++) {
        u32 m = (u32)seg << 4;
        u8 sig = lin_rd8(m);
        u16 size = lin_rd16(m + 3);
        if (sig != 'M' && sig != 'Z') break;
        if (lin >= m + 16 && lin < m + 16 + ((u32)size << 4)) return lin_rd16(m + 1);
        seg = (u16)(seg + size + 1);
        if (sig == 'Z' && lin_rd8((u32)seg << 4) != 'M') break;     /* the UMB chain may follow */
    }
    return 0xFFFF;
}

/* A real-mode vector the client set with 0201h goes back only while it still points to the
   client: to its memory (being freed), to a free block or to a callback of the host. A
   client that hooked a vector through DOS and set it back with 0201h when it ended (DOS/4GW:
   INT 15h, 1Bh) must keep the value it put back. */
static int rmv_stale(struct dpmicl *c, int i)
{
    u32 v = lin_rd32((u32)c->rmv[i].vec * 4), lin;
    u16 own;
    if (v != c->rmv[i].cur) return 0;
    if ((v >> 16) == vmc->drvseg) return 1;
    lin = ((v >> 16) << 4) + (v & 0xFFFF);
    own = mem_owner(lin);
    return own == c->psp || own == 0;
}

static void cleanup(struct vmregs *f, struct dpmicl *c)
{
    int i, own = c->idx + 1;
    for (i = 0; i < NMEM; i++)
        if (mb[i].size && mb[i].owner == own) mem_free(f, i);
    for (i = 0; i < VM_NCB; i++)
        if (cbs[i].used && cbs[i].owner == own) cbs[i].used = 0;
    for (i = c->nrmv - 1; i >= 0; i--)
        if (rmv_stale(c, i)) lin_wr32((u32)c->rmv[i].vec * 4, c->rmv[i].old);
    if (c->envsel && lin_rd16(((u32)c->psp << 4) + 0x2C) == c->envsel)
        lin_wr16(((u32)c->psp << 4) + 0x2C, c->envseg);
    for (i = 0; i < NLDT; i++)
        if (lown[i] == own) {
            lown[i] = 0;
            lfix[i] = 0;
            ldt[i] = 0;
        }
    for (i = 0; i < (int)ARRAY_SIZE(s2s); i++)
        if (s2s[i].owner == own) s2s[i].sel = 0;
    for (i = 0; i < NDOSM; i++)
        if (dosm[i].owner == own) dosm[i].sel = 0;
    vm_setredir(0x1C, 0);
    vm_setredir(0x23, 0);
    vm_setredir(0x24, 0);
    ncl = c->idx;
    dcl = ncl ? &cls[ncl - 1] : NULL;
    if (dcl) {
        vm_setredir(0x1C, !isdef(dcl, 0x1C));
        vm_setredir(0x23, !isdef(dcl, 0x23));
        vm_setredir(0x24, !isdef(dcl, 0x24));
    }
    vmc->pmode = (u8)(dcl != NULL);
}

/* the client ends with INT 21h AH=4Ch AL */
static void dpmi_exit(struct vmregs *f, u8 al)
{
    struct dpmicl *c = dcl;
    struct vmregs t;
    u32 nest = c->nest;
    int nrm = c->nrm;
    memset(&t, 0, sizeof(t));
    t.ss = c->hseg;
    t.esp = DPMI_PARAS * 16 - 16;
    t.ds = t.es = c->psp;
    cleanup(f, c);
    t.eax = 0x4C00 | al;
    t.cs = vmc->drvseg;
    t.eip = vmc->o_term;
    t.eflags = EFL_VM | EFL_IOPL | EFL_IF | 2;
    rmstk_set(nrm);
    vm_jump(&t, nest);
}

void dpmi_kill(struct vmregs *f)
{
    dpmi_exit(f, 0xFF);
}

/* INT 21h AH=4Ch in real mode: the client may end from there */
void dpmi_rmexit(struct vmregs *f)
{
    struct dpmicl *c = dcl;
    u32 nest;
    if (!c || rd_psp(f) != c->psp) return;
    nest = c->nest;
    cleanup(f, c);
    rmstk_set(c->nrm);
    v86_reflect(f, 0x21);
    vm_jump(f, nest);
}

/* a client left behind (ended through DOS without telling the host): not an ancestor of psp */
static void gc(struct vmregs *f, int psp)
{
    while (dcl) {
        int p = psp, k, alive = 0;
        for (k = 0; k < 32; k++) {
            int q;
            if (p == dcl->psp) {
                alive = 1;
                break;
            }
            q = lin_rd16(((u32)p << 4) + 0x16);
            if (!q || q == p) break;
            p = q;
        }
        if (alive && psp != dcl->psp) return;
        cleanup(f, dcl);
    }
}

/* HC_DPMI: the client's far call to the entry of INT 2Fh AX=1687h; returns in protected mode */
void dpmi_enter(struct vmregs *f)
{
    struct dpmicl *c;
    u16 ip, cs, csel, dsel, ssel, psel;
    int psp, v;
    u32 envl;
    psp = rd_psp(f);
    if (!mcb0) {
        struct vmregs t;
        if (!rm_setup(&t, f)) {
            t.eax = 0x5200;
            rm_int(&t, 0x21);
            mcb0 = lin_rd16((((u32)t.es & 0xFFFF) << 4) + ((t.ebx - 2) & 0xFFFF));
        }
    }
    gc(f, psp);
    if (ncl >= MAXCL) {
        f->eflags |= EFL_CF;
        f->eax = (f->eax & ~0xFFFFUL) | 0x8011;
        return;
    }
    c = &cls[ncl];
    memset(c, 0, sizeof(*c));
    c->idx = (u8)ncl;
    c->is32 = (u8)(f->eax & 1);
    c->hseg = (u16)f->es;
    c->psp = (u16)psp;
    c->nest = vm_nest;
    c->nrm = rmstk_depth();
    for (v = 0; v < 256; v++) {
        c->pmsel[v] = stubsel(c);
        c->pmoff[v] = STUB_DEF + (u32)v;
    }
    ip = v86_pop(f);
    cs = v86_pop(f);
    csel = newsel(c, (u32)cs << 4, 0xFFFF, 0xFA, 0);
    dsel = newsel(c, (f->ds & 0xFFFF) << 4, 0xFFFF, 0xF2, 0);
    ssel = (f->ss & 0xFFFF) == (f->ds & 0xFFFF) ? dsel : newsel(c, (f->ss & 0xFFFF) << 4, 0xFFFF, 0xF2, 0);
    psel = newsel(c, (u32)psp << 4, 0xFF, 0xF2, 0);
    c->envseg = lin_rd16(((u32)psp << 4) + 0x2C);
    if (c->envseg) {
        envl = (u32)lin_rd16(((u32)(c->envseg - 1) << 4) + 3) << 4;
        if (!envl || envl > 0x10000UL) envl = 0x10000UL;
        c->envsel = newsel(c, (u32)c->envseg << 4, envl - 1, 0xF2, 0);
        if (c->envsel) lin_wr16(((u32)psp << 4) + 0x2C, c->envsel);
    }
    if (!csel || !dsel || !ssel || !psel) {
        cleanup(f, c);
        v86_push(f, cs);
        v86_push(f, ip);
        f->eflags |= EFL_CF;
        f->eax = (f->eax & ~0xFFFFUL) | 0x8011;
        return;
    }
    ncl++;
    dcl = c;
    vmc->clients++;
    vmc->pmode = 1;
    f->eip = ip;
    f->cs = csel;
    f->ss = ssel;
    f->esp &= 0xFFFF;
    f->ds = dsel;
    f->es = psel;
    f->fs = f->gs = 0;
    f->eflags = (f->eflags & ~(EFL_VM | EFL_IOPL | EFL_NT | EFL_CF | EFL_TF)) | 2;
    kprintf("dpmi: client %d (%d-bit), PSP %04x\n", c->idx, c->is32 ? 32 : 16, psp);
}

/* HC_RAWPM: AX DS, CX ES, DX SS, (E)BX (E)SP, SI CS, (E)DI (E)IP */
void dpmi_rawpm(struct vmregs *f)
{
    struct dpmicl *c = dcl;
    if (!c || !cpu_lar(f->esi & 0xFFFF) || !cpu_lar(f->edx & 0xFFFF)) {
        vm_kill(f, "WINAUDIO DPMI: bad mode switch");
        return;
    }
    f->ds = f->eax & 0xFFFF;
    f->es = f->ecx & 0xFFFF;
    f->ss = f->edx & 0xFFFF;
    f->esp = c->is32 ? f->ebx : f->ebx & 0xFFFF;
    f->cs = f->esi & 0xFFFF;
    f->eip = c->is32 ? f->edi : f->edi & 0xFFFF;
    f->fs = f->gs = 0;
    f->eflags = (f->eflags & ~(EFL_VM | EFL_IOPL | EFL_NT)) | 2;
}

/* HC_CB + i: a real-mode callback */
void dpmi_callback(struct vmregs *f, int i)
{
    struct dpmicl *c = dcl;
    struct vmregs t;
    u32 s, old, fl;
    int sz;
    if (!c || !cbs[i].used) {
        vm_kill(f, "WINAUDIO DPMI: call of a freed callback");
        return;
    }
    s = sel_base(cbs[i].rsel, NULL, NULL) + cbs[i].roff;
    memcpy((void *)s, f, 32);
    lin_wr32(s + 12, 0);
    lin_wr16(s + 0x20, (u16)f->eflags);
    lin_wr16(s + 0x22, (u16)f->es);
    lin_wr16(s + 0x24, (u16)f->ds);
    lin_wr16(s + 0x26, (u16)f->fs);
    lin_wr16(s + 0x28, (u16)f->gs);
    lin_wr16(s + 0x2A, (u16)f->eip);
    lin_wr16(s + 0x2C, (u16)f->cs);
    lin_wr16(s + 0x2E, (u16)f->esp);
    lin_wr16(s + 0x30, (u16)f->ss);
    gdtp()[SEL_RMSS >> 3] = mkdesc((f->ss & 0xFFFF) << 4, 0xFFFF, 0xF2, 0);
    rmstk_push(f->ss, f->esp);
    memset(&t, 0, sizeof(t));
    sz = c->is32 ? 4 : 2;
    t.ds = SEL_RMSS;
    t.esi = f->esp & 0xFFFF;
    t.es = cbs[i].rsel;
    t.edi = cbs[i].roff;
    t.ss = lpmssel(c);
    t.esp = lpms_cur;
    t.eflags = 2;
    spush(&t, 2, sz);
    spush(&t, stubsel(c), sz);
    spush(&t, STUB_CB, sz);
    t.cs = cbs[i].pmsel;
    t.eip = cbs[i].pmoff;
    lpms_track(&t, &old);
    pm_call(&t);
    lpms_cur = old;
    rmstk_pop();
    s = sel_base(t.es, NULL, NULL) + (c->is32 ? t.edi : t.edi & 0xFFFF);
    if (!lin_ok(s, 0x32)) {
        vm_kill(f, "WINAUDIO DPMI: bad callback register structure");
        return;
    }
    memcpy(f, (void *)s, 32);
    fl = lin_rd16(s + 0x20);
    f->eflags = EFL_VM | EFL_IOPL | 2 | (fl & (EFL_ARITH | EFL_DF | EFL_IF | EFL_TF));
    f->es = lin_rd16(s + 0x22);
    f->ds = lin_rd16(s + 0x24);
    f->fs = lin_rd16(s + 0x26);
    f->gs = lin_rd16(s + 0x28);
    f->eip = lin_rd16(s + 0x2A);
    f->cs = lin_rd16(s + 0x2C);
    f->esp = lin_rd16(s + 0x2E);
    f->ss = lin_rd16(s + 0x30);
}

/* ---------------------------------------------------------------- INT 31h */

static void ok(struct vmregs *f)
{
    f->eflags &= ~EFL_CF;
}

static void fail(struct vmregs *f, u16 e)
{
    f->eflags |= EFL_CF;
    f->eax = (f->eax & ~0xFFFFUL) | e;
}

static void setax(struct vmregs *f, u32 v) { f->eax = (f->eax & ~0xFFFFUL) | (v & 0xFFFF); }
static void setbx(struct vmregs *f, u32 v) { f->ebx = (f->ebx & ~0xFFFFUL) | (v & 0xFFFF); }
static void setcx(struct vmregs *f, u32 v) { f->ecx = (f->ecx & ~0xFFFFUL) | (v & 0xFFFF); }
static void setdx(struct vmregs *f, u32 v) { f->edx = (f->edx & ~0xFFFFUL) | (v & 0xFFFF); }
static void setsi(struct vmregs *f, u32 v) { f->esi = (f->esi & ~0xFFFFUL) | (v & 0xFFFF); }
static void setdi(struct vmregs *f, u32 v) { f->edi = (f->edi & ~0xFFFFUL) | (v & 0xFFFF); }
#define BX (f->ebx & 0xFFFF)
#define CX (f->ecx & 0xFFFF)
#define DX (f->edx & 0xFFFF)
#define SI (f->esi & 0xFFFF)
#define DI (f->edi & 0xFFFF)
#define BL ((u8)f->ebx)
#define EDX(c) ((c)->is32 ? f->edx : f->edx & 0xFFFF)
#define EDI(c) ((c)->is32 ? f->edi : f->edi & 0xFFFF)

/* far pointer CX:(E)DX of a 16- or 32-bit client */
static void setcxedx(struct vmregs *f, struct dpmicl *c, u16 sel, u32 off)
{
    setcx(f, sel);
    if (c->is32) f->edx = off;
    else setdx(f, off);
}

static void rmcall(struct vmregs *f, struct dpmicl *c, int how)
{
    u32 s = cptr(f, f->es, f->edi, 0x32), n = CX, src, i, fl;
    struct vmregs t;
    int big;
    (void)c;
    if (!s) {
        fail(f, 0x8021);
        return;
    }
    memset(&t, 0, sizeof(t));
    memcpy(&t, (void *)s, 32);
    fl = lin_rd16(s + 0x20);
    t.es = lin_rd16(s + 0x22);
    t.ds = lin_rd16(s + 0x24);
    t.fs = lin_rd16(s + 0x26);
    t.gs = lin_rd16(s + 0x28);
    t.eip = lin_rd16(s + 0x2A);
    t.cs = lin_rd16(s + 0x2C);
    t.esp = lin_rd16(s + 0x2E);
    t.ss = lin_rd16(s + 0x30);
    if (!t.ss && !t.esp && rm_stack(&t.ss, &t.esp)) {
        fail(f, 0x8012);
        return;
    }
    src = sel_base(f->ss, &big, NULL) + (big ? f->esp : f->esp & 0xFFFF);
    for (i = n; i > 0; i--) v86_push(&t, lin_rd16(src + (i - 1) * 2));
    t.eflags = (fl & (EFL_ARITH | EFL_DF | EFL_IF | EFL_TF)) | 2;
    if (how == 0) {
        rm_int(&t, BL);
    } else {
        rm_call(&t, how == 2);
    }
    memcpy((void *)(s), &t, 12);
    memcpy((void *)(s + 16), &t.ebx, 16);
    lin_wr16(s + 0x20, (u16)(t.eflags & ~(EFL_IOPL | EFL_NT)));
    lin_wr16(s + 0x22, (u16)t.es);
    lin_wr16(s + 0x24, (u16)t.ds);
    lin_wr16(s + 0x26, (u16)t.fs);
    lin_wr16(s + 0x28, (u16)t.gs);
    ok(f);
}

static int dosm_find(u16 sel)
{
    int i;
    for (i = 0; i < NDOSM; i++)
        if (dosm[i].sel && dosm[i].sel == sel) return i;
    return -1;
}

static void dos_descs(struct dpmicl *c, int first, u16 seg, u32 paras, int n)
{
    u32 total = paras * 16, k;
    for (k = 0; k < (u32)n; k++) {
        u32 rest = total - k * 0x10000UL, lim;
        lim = (k == 0 && c->is32) ? total - 1 : (rest > 0x10000UL ? 0xFFFF : rest - 1);
        ldt_set(first + (int)k, mkdesc(((u32)seg << 4) + k * 0x10000UL, lim, 0xF2, 0), c);
    }
}

static void meminfo(struct vmregs *f, struct dpmicl *c)
{
    u32 s = cptr(f, f->es, f->edi, 0x30), lg, tot, i;
    if (!s) {
        fail(f, 0x8021);
        return;
    }
    xms_avail(f, &lg, &tot);
    for (i = 0; i < 12; i++) lin_wr32(s + i * 4, 0xFFFFFFFFUL);
    lin_wr32(s + 0x00, lg * 1024);
    lin_wr32(s + 0x04, lg / 4);
    lin_wr32(s + 0x08, lg / 4);
    lin_wr32(s + 0x0C, (ARENA_HI - ARENA_LO) >> 12);
    lin_wr32(s + 0x10, tot / 4);
    lin_wr32(s + 0x14, tot / 4);
    lin_wr32(s + 0x18, tot / 4);
    lin_wr32(s + 0x1C, (ARENA_HI - ARENA_LO) >> 12);
    lin_wr32(s + 0x20, 0);
    (void)c;
    ok(f);
}

static int handle_of(struct vmregs *f, struct dpmicl *c)
{
    u32 h = (SI << 16) | DI, i = h & 0xFFFF;
    if ((h & 0xFFFF0000UL) != 0x44500000UL || i >= NMEM || !mb[i].size || mb[i].owner != c->idx + 1) return -1;
    return (int)i;
}

static void dpmi_api(struct vmregs *f)
{
    struct dpmicl *c = dcl;
    u16 fn = (u16)f->eax;
    u64 *d;
    u32 l, v, i;
    int k, n;
    switch (fn) {
    case 0x0000:
        n = (int)CX;
        k = n ? ldt_alloc(n, 16) : -1;
        if (k < 0) {
            fail(f, n ? 0x8011 : 0x8021);
            return;
        }
        for (i = 0; i < (u32)n; i++) ldt_set(k + (int)i, mkdesc(0, 0, 0xF2, c->is32 ? 0x40 : 0), c);
        setax(f, mksel(k));
        ok(f);
        return;
    case 0x0001:
        if (!mysel(c, BX) || lfix[BX >> 3]) {
            fail(f, 0x8022);
            return;
        }
        k = (int)(BX >> 3);
        ldt[k] = 0;
        lown[k] = 0;
        selgone(f, BX);
        ok(f);
        return;
    case 0x0002:
        for (i = 0; i < ARRAY_SIZE(s2s); i++)
            if (s2s[i].sel && s2s[i].seg == BX) {
                setax(f, s2s[i].sel);
                ok(f);
                return;
            }
        k = ldt_alloc(1, 16);
        if (k < 0) {
            fail(f, 0x8011);
            return;
        }
        ldt_set(k, mkdesc(BX << 4, 0xFFFF, 0xF2, 0), c);
        lfix[k] = 1;
        for (i = 0; i < ARRAY_SIZE(s2s) && s2s[i].sel; i++) ;
        if (i < ARRAY_SIZE(s2s)) {
            s2s[i].seg = (u16)BX;
            s2s[i].sel = mksel(k);
            s2s[i].owner = (u8)(c->idx + 1);
        }
        setax(f, mksel(k));
        ok(f);
        return;
    case 0x0003:
        setax(f, 8);
        ok(f);
        return;
    case 0x0006:
        d = descp(BX);
        if (!d || !(BX & 4)) {
            fail(f, 0x8022);
            return;
        }
        v = d_base(*d);
        setcx(f, v >> 16);
        setdx(f, v);
        ok(f);
        return;
    case 0x0007:
    case 0x0008:
    case 0x0009:
        if (!mysel(c, BX) || lfix[BX >> 3]) {
            fail(f, 0x8022);
            return;
        }
        d = &ldt[BX >> 3];
        if (fn == 0x0007) {
            v = (CX << 16) | DX;
            if (!c->is32) v &= 0x00FFFFFFUL;
            *d = (*d & 0x00FFFF000000FFFFULL) | ((u64)(v & 0xFFFFFF) << 16) | ((u64)(v >> 24) << 56);
        } else if (fn == 0x0008) {
            v = (CX << 16) | DX;
            if (v > 0xFFFFF && (v & 0xFFF) != 0xFFF) {
                fail(f, 0x8021);
                return;
            }
            if (v > 0xFFFFF) {
                v >>= 12;
                *d |= 1ULL << 55;
            } else {
                *d &= ~(1ULL << 55);
            }
            *d = (*d & ~0x000F00000000FFFFULL) | (v & 0xFFFF) | ((u64)((v >> 16) & 0xF) << 48);
        } else {
            u8 a = (u8)CX, x = (u8)(CX >> 8);
            if (!(a & 0x10) || (a & 0x60) != 0x60) {
                fail(f, 0x8021);
                return;
            }
            *d = (*d & ~0x00F0FF0000000000ULL) | ((u64)a << 40) | ((u64)(x & 0xF0) << 48);
        }
        ok(f);
        return;
    case 0x000A:
        d = descp(BX);
        if (!d || !(BX & 4) || !lown[BX >> 3]) {
            fail(f, 0x8022);
            return;
        }
        k = ldt_alloc(1, 16);
        if (k < 0) {
            fail(f, 0x8011);
            return;
        }
        ldt_set(k, (*d & ~0x00000F0000000000ULL) | (0x2ULL << 40), c);
        setax(f, mksel(k));
        ok(f);
        return;
    case 0x000B:
        d = descp(BX);
        l = cptr(f, f->es, f->edi, 8);
        if (!d || !(BX & 4) || !l) {
            fail(f, 0x8022);
            return;
        }
        *(u64 *)l = *d;
        ok(f);
        return;
    case 0x000C:
        l = cptr(f, f->es, f->edi, 8);
        if (!mysel(c, BX) || lfix[BX >> 3] || !l) {
            fail(f, 0x8022);
            return;
        }
        v = lin_rd8(l + 5);
        if ((v & 0x80) && (!(v & 0x10) || (v & 0x60) != 0x60)) {
            fail(f, 0x8021);
            return;
        }
        ldt[BX >> 3] = *(u64 *)l;
        ok(f);
        return;
    case 0x000D:
        k = (int)(BX >> 3);
        if (!(BX & 4) || k < 1 || k >= 16 || lown[k]) {
            fail(f, 0x8011);
            return;
        }
        ldt_set(k, mkdesc(0, 0, 0xF2, c->is32 ? 0x40 : 0), c);
        ok(f);
        return;
    case 0x0100:
    case 0x0102: {
        struct vmregs t;
        int di = -1;
        u32 paras = BX;
        if (fn == 0x0102) {
            di = dosm_find((u16)DX);
            if (di < 0) {
                fail(f, 0x8022);
                return;
            }
        }
        if (rm_setup(&t, NULL)) {
            fail(f, 0x8010);
            return;
        }
        if (fn == 0x0100) {
            t.eax = 0x4800;
        } else {
            t.eax = 0x4A00;
            t.es = dosm[di].seg;
        }
        t.ebx = paras;
        rm_int(&t, 0x21);
        if (t.eflags & EFL_CF) {
            setbx(f, t.ebx);
            fail(f, (u16)t.eax);
            return;
        }
        n = (int)((paras * 16 + 0xFFFF) >> 16);
        if (!n) n = 1;
        if (fn == 0x0100) {
            k = ldt_alloc(n, 16);
            for (i = 0; i < NDOSM && dosm[i].sel; i++) ;
            if (k < 0 || i == NDOSM) {
                u16 seg = (u16)t.eax;
                if (!rm_setup(&t, NULL)) {
                    t.eax = 0x4900;
                    t.es = seg;
                    rm_int(&t, 0x21);
                }
                fail(f, 0x8011);
                return;
            }
            dos_descs(c, k, (u16)t.eax, paras, n);
            dosm[i].sel = mksel(k);
            dosm[i].seg = (u16)t.eax;
            dosm[i].n = (u16)n;
            dosm[i].owner = (u8)(c->idx + 1);
            setax(f, t.eax);
            setdx(f, mksel(k));
        } else {
            k = (int)(dosm[di].sel >> 3);
            if (n > dosm[di].n) {
                int j;
                for (j = dosm[di].n; j < n && k + j < NLDT && !lown[k + j]; j++) ;
                if (j < n) {
                    fail(f, 0x8011);
                    return;
                }
            }
            for (i = (u32)n; i < dosm[di].n; i++) {
                ldt[k + (int)i] = 0;
                lown[k + (int)i] = 0;
            }
            dos_descs(c, k, dosm[di].seg, paras, n);
            dosm[di].n = (u16)n;
        }
        ok(f);
        return;
    }
    case 0x0101: {
        struct vmregs t;
        int di = dosm_find((u16)DX);
        if (di < 0) {
            fail(f, 0x8022);
            return;
        }
        if (rm_setup(&t, NULL)) {
            fail(f, 0x8010);
            return;
        }
        t.eax = 0x4900;
        t.es = dosm[di].seg;
        rm_int(&t, 0x21);
        if (t.eflags & EFL_CF) {
            fail(f, (u16)t.eax);
            return;
        }
        k = (int)(dosm[di].sel >> 3);
        for (i = 0; i < dosm[di].n; i++) {
            selgone(f, mksel(k + (int)i));
            ldt[k + (int)i] = 0;
            lown[k + (int)i] = 0;
        }
        dosm[di].sel = 0;
        ok(f);
        return;
    }
    case 0x0200:
        v = lin_rd32((u32)BL * 4);
        setcx(f, v >> 16);
        setdx(f, v);
        ok(f);
        return;
    case 0x0201:
        for (k = 0; k < c->nrmv && c->rmv[k].vec != BL; k++) ;
        if (k == c->nrmv && k < (int)ARRAY_SIZE(c->rmv)) {
            c->rmv[k].vec = BL;
            c->rmv[k].old = lin_rd32((u32)BL * 4);
            c->nrmv++;
        }
        lin_wr32((u32)BL * 4, (CX << 16) | DX);
        if (k < c->nrmv) c->rmv[k].cur = (CX << 16) | DX;
        ok(f);
        return;
    case 0x0202:
        if (BL >= 32) {
            fail(f, 0x8021);
            return;
        }
        if (c->exsel[BL]) setcxedx(f, c, c->exsel[BL], c->exoff[BL]);
        else setcxedx(f, c, stubsel(c), STUB_EXCDEF + BL);
        ok(f);
        return;
    case 0x0203:
        if (BL >= 32 || !cpu_lar(CX)) {
            fail(f, 0x8021);
            return;
        }
        if (CX == stubsel(c) && EDX(c) == STUB_EXCDEF + (u32)BL) {
            c->exsel[BL] = 0;
        } else {
            c->exsel[BL] = (u16)CX;
            c->exoff[BL] = EDX(c);
        }
        ok(f);
        return;
    case 0x0204:
        setcxedx(f, c, c->pmsel[BL], c->pmoff[BL]);
        ok(f);
        return;
    case 0x0205:
        if (!cpu_lar(CX)) {
            fail(f, 0x8022);
            return;
        }
        c->pmsel[BL] = (u16)CX;
        c->pmoff[BL] = EDX(c);
        if (BL == 0x1C || BL == 0x23 || BL == 0x24) vm_setredir(BL, !isdef(c, BL));
        ok(f);
        return;
    case 0x0300:
    case 0x0301:
    case 0x0302:
        rmcall(f, c, fn - 0x0300);
        return;
    case 0x0303:
        for (k = 0; k < VM_NCB && cbs[k].used; k++) ;
        if (k == VM_NCB) {
            fail(f, 0x8015);
            return;
        }
        cbs[k].used = 1;
        cbs[k].owner = (u8)(c->idx + 1);
        cbs[k].pmsel = (u16)f->ds;
        cbs[k].pmoff = c->is32 ? f->esi : SI;
        cbs[k].rsel = (u16)f->es;
        cbs[k].roff = EDI(c);
        setcx(f, vmc->drvseg);
        setdx(f, vmc->o_cbs + k * 2);
        ok(f);
        return;
    case 0x0304:
        k = (int)(DX - vmc->o_cbs) / 2;
        if (CX != vmc->drvseg || DX < vmc->o_cbs || k >= VM_NCB || !cbs[k].used) {
            fail(f, 0x8024);
            return;
        }
        cbs[k].used = 0;
        ok(f);
        return;
    case 0x0305:
        setax(f, 0);
        setbx(f, vmc->drvseg);
        setcx(f, vmc->o_rmnull);
        setsi(f, stubsel(c));
        if (c->is32) f->edi = STUB_RETF;
        else setdi(f, STUB_RETF);
        ok(f);
        return;
    case 0x0306:
        setbx(f, vmc->drvseg);
        setcx(f, vmc->o_rawpm);
        setsi(f, stubsel(c));
        if (c->is32) f->edi = STUB_RAWRM;
        else setdi(f, STUB_RAWRM);
        ok(f);
        return;
    case 0x0400:
        setax(f, 0x005A);
        setbx(f, 0x0003);
        f->ecx = (f->ecx & ~0xFFUL) | vmc->cpu;
        setdx(f, ((u32)vm_mbase << 8) | vm_sbase);
        ok(f);
        return;
    case 0x0500:
        meminfo(f, c);
        return;
    case 0x0501:
        k = mem_alloc(f, (BX << 16) | CX, &l);
        if (k < 0) {
            fail(f, k == -1 ? 0x8012 : k == -3 ? 0x8016 : k == -2 && !((BX << 16) | CX) ? 0x8021 : 0x8013);
            return;
        }
        setbx(f, l >> 16);
        setcx(f, l);
        setsi(f, HANDLE(k) >> 16);
        setdi(f, HANDLE(k));
        ok(f);
        return;
    case 0x0502:
        k = handle_of(f, c);
        if (k < 0) {
            fail(f, 0x8023);
            return;
        }
        mem_free(f, k);
        ok(f);
        return;
    case 0x0503:
        k = handle_of(f, c);
        if (k < 0 || mb[k].phys) {
            fail(f, 0x8023);
            return;
        }
        n = mem_resize(f, k, (BX << 16) | CX);
        if (n) {
            fail(f, n == -1 ? 0x8012 : 0x8013);
            return;
        }
        setbx(f, mb[k].lin >> 16);
        setcx(f, mb[k].lin);
        ok(f);
        return;
    case 0x0600: case 0x0601: case 0x0602: case 0x0603:
    case 0x0700: case 0x0701: case 0x0702: case 0x0703:
        ok(f);
        return;
    case 0x0604:
        setbx(f, 0);
        setcx(f, 4096);
        ok(f);
        return;
    case 0x0800:
        l = (BX << 16) | CX;
        v = (SI << 16) | DI;
        if (!v) {
            fail(f, 0x8021);
            return;
        }
        if (l + v <= 0x110000UL) {
            setbx(f, l >> 16);
            setcx(f, l);
            ok(f);
            return;
        }
        for (k = 0; k < NMEM && mb[k].size; k++) ;
        if (k < NMEM) {
            u32 first = l & ~4095UL, bytes = ((l + v + 4095) & ~4095UL) - first, lin = lin_find(bytes, -1);
            if (lin && !map(lin, first, bytes, 0x1F)) {
                mb[k].lin = lin;
                mb[k].size = bytes;
                mb[k].rsv = bytes;
                mb[k].phys = 1;
                mb[k].owner = (u8)(c->idx + 1);
                mb[k].first = 0;
                lin += l & 4095;
                setbx(f, lin >> 16);
                setcx(f, lin);
                ok(f);
                return;
            }
        }
        fail(f, 0x8012);
        return;
    case 0x0801:
        l = (BX << 16) | CX;
        for (k = 0; k < NMEM; k++)
            if (mb[k].size && mb[k].phys && mb[k].owner == c->idx + 1 && (l & ~4095UL) == mb[k].lin) {
                mem_free(f, k);
                break;
            }
        ok(f);
        return;
    case 0x0900:
    case 0x0901:
    case 0x0902:
        f->eax = (f->eax & ~0xFFUL) | ((f->eflags & EFL_IF) ? 1 : 0);
        if (fn == 0x0900) f->eflags &= ~EFL_IF;
        if (fn == 0x0901) f->eflags |= EFL_IF;
        ok(f);
        return;
    }
    fail(f, 0x8001);
}

/* ---------------------------------------------------------------- protected-mode traps */

/* the default action of a protected-mode interrupt; stub: reached through the host's
   default handler, IRET frame on the client's stack */
static void pm_default(struct vmregs *f, int v, int stub)
{
    struct dpmicl *c = dcl;
    if (v == 0x31) {
        dpmi_api(f);
    } else if (v == 0x21 && (u8)(f->eax >> 8) == 0x4C) {
        dpmi_exit(f, (u8)f->eax);
        return;
    } else if (v == 0x2F && (u16)f->eax == 0x1686) {
        setax(f, 0);
    } else if (v == 0x2F && (u16)f->eax == 0x168A) {
        /* no vendor API */
    } else {
        pm_reflect(f, v);
    }
    if (stub) {
        struct vmregs t = *f;
        int sz = c->is32 ? 4 : 2;
        u32 fl;
        f->eip = spop(&t, sz);
        f->cs = spop(&t, sz) & 0xFFFF;
        fl = spop(&t, sz);
        f->esp = t.esp;
        f->eflags = (f->eflags & (EFL_ARITH | 0xFFFF0000UL)) | (fl & ~(EFL_ARITH | EFL_IOPL | EFL_NT) & 0xFFFF);
        f->eflags &= ~(EFL_VM | EFL_IOPL);
        f->eflags |= 2;
    }
}

static void pm_swint(struct vmregs *f, int v)
{
    struct dpmicl *c = dcl;
    int sz = c->is32 ? 4 : 2;
    if (isdef(c, v)) {
        pm_default(f, v, 0);
        return;
    }
    spush(f, f->eflags & ~(EFL_IOPL | EFL_NT), sz);
    spush(f, f->cs, sz);
    spush(f, f->eip, sz);
    f->cs = c->pmsel[v];
    f->eip = c->pmoff[v];
    f->eflags &= ~(EFL_TF | EFL_NT);
}

static void unhandled(struct vmregs *f, int v)
{
    char b[96];
    kprintf("dpmi: exception %x cs:eip %lx:%lx ss:esp %lx:%lx ds %lx es %lx fs %lx gs %lx\n"
            "  eax %lx ebx %lx ecx %lx edx %lx esi %lx edi %lx ebp %lx fl %lx\n", v, f->cs, f->eip, f->ss,
            f->esp, f->ds, f->es, f->fs, f->gs, f->eax, f->ebx, f->ecx, f->edx, f->esi, f->edi, f->ebp,
            f->eflags);
    {
        u32 i, b0 = sel_base(f->cs, NULL, NULL) + f->eip;
        for (i = 0; i < 12; i++) kprintf("%02x ", lin_rd8(b0 + i));
        kprintf(" ds base %lx es base %lx ss base %lx\n", sel_base(f->ds, NULL, NULL), sel_base(f->es, NULL, NULL),
                sel_base(f->ss, NULL, NULL));
    }
    if (v == 14)
        ksnprintf(b, sizeof(b), "WINAUDIO DPMI: page fault at %08lX (error %lX)", rd_cr2(), f->err & 0xFFFF);
    else
        ksnprintf(b, sizeof(b), "WINAUDIO DPMI: exception %02X (error %04lX)", v, f->err & 0xFFFF);
    vm_kill(f, b);
}

/* an exception of the client: its handler (0203h), the interrupt handler for 0..7, or the end */
static void pm_exception(struct vmregs *f, int v)
{
    struct dpmicl *c = dcl;
    struct vmregs t;
    u32 old;
    int sz = c->is32 ? 4 : 2;
    if (!c->exsel[v]) {
        if (v < 8 && v != 2 && !isdef(c, v)) {
            pm_swint(f, v);
            return;
        }
        if (v == 1) {                   /* single step nobody wants */
            f->eflags &= ~EFL_TF;
            return;
        }
        unhandled(f, v);
        return;
    }
    t = *f;
    if ((f->ss & 0xFFFF) != lpmssel(c)) {
        t.ss = lpmssel(c);
        t.esp = lpms_cur;
    }
    spush(&t, f->ss, sz);
    spush(&t, f->esp, sz);
    spush(&t, f->eflags & ~(EFL_IOPL | EFL_NT), sz);
    spush(&t, f->cs, sz);
    spush(&t, f->eip, sz);
    spush(&t, f->err, sz);
    spush(&t, stubsel(c), sz);
    spush(&t, STUB_EXC, sz);
    t.cs = c->exsel[v];
    t.eip = c->exoff[v];
    t.eflags &= ~(EFL_IF | EFL_TF);
    lpms_track(&t, &old);
    pm_call(&t);
    lpms_cur = old;
    /* the handler returned with RETF: SS:ESP at the error code of the frame it may have changed */
    *f = t;
    (void)spop(&t, sz);
    f->eip = spop(&t, sz);
    f->cs = spop(&t, sz) & 0xFFFF;
    f->eflags = (spop(&t, sz) & ~(EFL_VM | EFL_IOPL | EFL_NT)) | 2;
    f->esp = spop(&t, sz);
    f->ss = spop(&t, sz) & 0xFFFF;
    if (!c->is32) f->esp &= 0xFFFF;
}

static void pm_stub(struct vmregs *f)
{
    u32 o = f->eip;
    if (o == STUB_IRET || o == STUB_EXC || o == STUB_CB) {
        pm_return(f);
        return;
    }
    if (o == STUB_RAWRM) {                      /* AX DS, CX ES, DX SS, (E)BX SP, SI CS, (E)DI IP */
        f->ds = f->eax & 0xFFFF;
        f->es = f->ecx & 0xFFFF;
        f->ss = f->edx & 0xFFFF;
        f->esp = f->ebx & 0xFFFF;
        f->cs = f->esi & 0xFFFF;
        f->eip = f->edi & 0xFFFF;
        f->fs = f->gs = 0;
        f->eflags = (f->eflags & ~EFL_NT) | EFL_VM | EFL_IOPL | 2;
        return;
    }
    if (o >= STUB_DEF && o < STUB_DEF + 256) {
        pm_default(f, (int)(o - STUB_DEF), 1);
        return;
    }
    if (o >= STUB_EXCDEF && o < STUB_EXCDEF + 32) {
        struct vmregs t = *f;
        int sz = dcl->is32 ? 4 : 2;
        (void)spop(&t, sz);
        (void)spop(&t, sz);
        f->err = spop(&t, sz);
        f->eip = spop(&t, sz);
        f->cs = spop(&t, sz);
        unhandled(f, (int)(o - STUB_EXCDEF));
        return;
    }
    vm_kill(f, "WINAUDIO DPMI: bad jump into the host");
}

static int pm_softint(struct vmregs *f, int v)
{
    int big;
    u32 b = sel_base(f->cs, &big, NULL), ip = f->eip - 2;
    if (!big) ip &= 0xFFFF;
    return lin_rd8(b + ip) == 0xCD && lin_rd8(b + ip + 1) == (u8)v;
}

static void pm_gp(struct vmregs *f)
{
    int big, i;
    u32 base = sel_base(f->cs, &big, NULL), ip = f->eip;
    u8 b[8];
    if (f->err & 2) {                           /* INT n through a gate of DPL 0 (n < 20h) */
        u8 op = lin_rd8(base + ip);
        ip += op == 0xCD ? 2 : 1;
        f->eip = big ? ip : ip & 0xFFFF;
        pm_swint(f, (int)(f->err >> 3));
        return;
    }
    if (f->err) {
        pm_exception(f, 13);
        return;
    }
    if ((f->cs & 0xFFFC) == (SEL_STUB32 & 0xFFFC) || (f->cs & 0xFFFC) == (SEL_STUB16 & 0xFFFC)) {
        pm_stub(f);
        return;
    }
    if (io_insn(f, base, big)) return;
    for (i = 0; i < 8; i++) b[i] = lin_rd8(base + (big ? ip + i : (u16)(ip + i)));
    for (i = 0; i < 4 && (b[i] == 0x66 || b[i] == 0x67 || b[i] == 0xF0 || b[i] == 0xF2 || b[i] == 0xF3); i++) ;
    switch (b[i]) {
    case 0xFA:
        f->eflags &= ~EFL_IF;
        break;
    case 0xFB:
        f->eflags |= EFL_IF;
        break;
    case 0xF4:
        ip += (u32)i + 1;
        f->eip = big ? ip : ip & 0xFFFF;
        vm_hlt(f);
        return;
    default:
        pm_exception(f, 13);
        return;
    }
    ip += (u32)i + 1;
    f->eip = big ? ip : ip & 0xFFFF;
}

void pm_trap(struct vmregs *f)
{
    int v = (int)(f->vec & 0xFF), err = (f->vec & 0x100) != 0;
    if (!dcl) {
        vm_panic((u32 *)&f->vec);
        for (;;) ;
    }
    if (v == 13) {
        pm_gp(f);
        return;
    }
    if (!err && irq_of(v) >= 0 && !(v >= 0x20 && pm_softint(f, v))) {
        irq_entry(f, v);
        return;
    }
    if (v < 0x20) {
        pm_exception(f, v);
        return;
    }
    pm_swint(f, v);
}

int dpmi_hoststack(u32 *ss, u32 *sp)
{
    if (!dcl) return -1;
    *ss = dcl->hseg;
    *sp = DPMI_PARAS * 16;
    return 0;
}

u32 dpmi_setup(u32 *ldtlin, u32 stubs, u32 lpms)
{
    ldt = kmalloc_align(NLDT * 8, 8);
    if (!ldt) return 0;
    memset(ldt, 0, NLDT * 8);
    stub_lin = stubs;
    lpms_lin = lpms;
    lpms_cur = LPMS_SIZE - 16;
    *ldtlin = (u32)ldt;
    return NLDT;
}
