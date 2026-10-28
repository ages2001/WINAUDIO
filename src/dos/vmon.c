/* vmon.c - the V86 monitor of the WINAUDIO DOS core.
   Without EMM386 or JEMM, WINAUDIO.SYS has the core run DOS in virtual 8086 mode. The ports of
   the emulated Sound Blaster (DSP, mixer, FM, MPU-401) and of its DMA channels are trapped
   through the I/O permission bitmap and emulated by sbemu.c; the card's interrupt is given to
   the program the way the PIC would. HIMEM keeps working: A20 is emulated through the page
   tables and INT 15h AH=87h is done here. Protected-mode programs get a VCPI server and a DPMI
   host (dpmi.c), whose clients run in ring 3 with the same ports trapped. Windows 386
   enhanced mode gets real mode back through the switch routine of INT 2Fh AX=1605h. */
#include "core.h"
#include "audio.h"
#include "sbemu.h"
#include "vmon.h"

u32 __cdecl ent_dos(u32 pkt);
u32 __cdecl ent_poll(u32 arg);
u32 __cdecl ent_irq(u32 irq);
u32 __cdecl ent_api(u32 req);
u32 __cdecl ent_shutdown(u32 how);
u32 __cdecl ent_resume(u32 arg);
u32 __cdecl ent_filedone(u32 arg);
u32 __cdecl ent_sbio(u32 arg);
u32 __cdecl ent_sbtick(u32 arg);

struct winvm *vmc;
u32 drv_lin;
u8 vm_mbase = 8, vm_sbase = 0x70;

#define SCRATCH     0x7FC00000UL        /* page table 511: physical memory windows */

static u8 *tss, *iopb, *redir;
static u64 *gdt, *idt;
static u32 *pd, *pt0, *spt;
static u8 *stubpg;
static u8 started;
static struct winsb *sbc;
static u8 sb_irq, sb_vec;
static u8 vpic;                         /* the emulated IRQ in service: bit 0 master, bit 1 slave */
static u8 vocw[2] = { 0x0A, 0x0A };     /* read register selected through OCW3 */
static u8 a20v = 1, kbc_cmd, kbc_rdop;
static u8 rtc_div, rtc_on, rtc_oldb;
static u32 pie_ms;

/* V86 contexts below the protected-mode code that runs now (their stacks take the real-mode
   calls of the host) */
static struct { u16 ss, sp; } rmstk[48];
static int nrmstk;

/* what each waiting vm_enter is: 1 real-mode call */
static u8 nkind[48];

static void vm_stop(struct vmregs *f);

/* ---------------------------------------------------------------- memory */

u8 lin_rd8(u32 lin) { return *(volatile u8 *)lin; }
u16 lin_rd16(u32 lin) { return *(volatile u16 *)lin; }
u32 lin_rd32(u32 lin) { return *(volatile u32 *)lin; }
void lin_wr8(u32 lin, u8 v) { *(volatile u8 *)lin = v; }
void lin_wr16(u32 lin, u16 v) { *(volatile u16 *)lin = v; }
void lin_wr32(u32 lin, u32 v) { *(volatile u32 *)lin = v; }

int lin_ok(u32 lin, u32 len)
{
    if (lin + len < lin) return 0;
    if (lin + len <= 0x110000UL) return 1;
    return dpmi_mapped(lin, len);
}

/* a window onto physical memory: slot 0 or 1, up to 34 pages each */
static u8 *pmap(u32 phys, u32 len, int slot)
{
    u32 first = phys & ~4095UL, pages = (((phys + len + 4095) & ~4095UL) - first) >> 12, i, b = slot * 40;
    for (i = 0; i < pages && i < 40; i++) {
        spt[b + i] = (first + i * 4096UL) | 3;
        invlpg(SCRATCH + (b + i) * 4096UL);
    }
    return (u8 *)(SCRATCH + b * 4096UL + (phys & 4095));
}

static void phys_copy(u32 dst, u32 src, u32 n)
{
    u8 *d = pmap(dst, n, 1), *s = pmap(src, n, 0);
    u32 i;
    if (dst > src && dst < src + n) {
        for (i = n; i > 0; i--) d[i - 1] = s[i - 1];
    } else {
        memcpy(d, s, n);
    }
}

u64 mkdesc(u32 base, u32 limit, u32 access, u32 flags)
{
    if (limit > 0xFFFFF) {
        limit >>= 12;
        flags |= 0x80;
    }
    return (u64)(limit & 0xFFFF) | ((u64)(base & 0xFFFFFF) << 16) | ((u64)(access & 0xFF) << 40) |
           ((u64)((flags & 0xF0) | ((limit >> 16) & 0xF)) << 48) | ((u64)(base >> 24) << 56);
}

/* ---------------------------------------------------------------- V86 helpers */

void v86_push(struct vmregs *f, u16 v)
{
    u16 sp = (u16)(f->esp - 2);
    lin_wr16(((f->ss & 0xFFFF) << 4) + sp, v);
    f->esp = (f->esp & 0xFFFF0000UL) | sp;
}

u16 v86_pop(struct vmregs *f)
{
    u16 sp = (u16)f->esp;
    u16 v = lin_rd16(((f->ss & 0xFFFF) << 4) + sp);
    f->esp = (f->esp & 0xFFFF0000UL) | (u16)(sp + 2);
    return v;
}

void v86_reflect(struct vmregs *f, int vec)
{
    u32 v = lin_rd32((u32)vec * 4);
    v86_push(f, (u16)f->eflags);
    v86_push(f, (u16)f->cs);
    v86_push(f, (u16)f->eip);
    f->cs = v >> 16;
    f->eip = v & 0xFFFF;
    f->eflags &= ~(EFL_IF | EFL_TF | EFL_AC);
    vmc->ints++;
}

void rmstk_push(u32 ss, u32 sp)
{
    if (nrmstk < (int)ARRAY_SIZE(rmstk)) {
        rmstk[nrmstk].ss = (u16)ss;
        rmstk[nrmstk].sp = (u16)sp;
    }
    nrmstk++;
}

void rmstk_pop(void)
{
    if (nrmstk) nrmstk--;
}

int rmstk_depth(void)
{
    return nrmstk;
}

void rmstk_set(int n)
{
    nrmstk = n;
}

int rm_stack(u32 *ss, u32 *sp)
{
    if (nrmstk > 0 && nrmstk <= (int)ARRAY_SIZE(rmstk)) {
        *ss = rmstk[nrmstk - 1].ss;
        *sp = rmstk[nrmstk - 1].sp;
        return 0;
    }
    return dpmi_hoststack(ss, sp);
}

/* a real-mode context for a call of the host made while f was trapped */
int rm_setup(struct vmregs *t, struct vmregs *f)
{
    memset(t, 0, sizeof(*t));
    if (f && (f->eflags & EFL_VM)) {
        t->ss = f->ss & 0xFFFF;
        t->esp = f->esp & 0xFFFF;
    } else if (rm_stack(&t->ss, &t->esp)) {
        return -1;
    }
    t->eflags = EFL_IF | 2;
    return 0;
}

void rm_call(struct vmregs *t, int how)
{
    int n = (int)vm_nest;
    if (how) v86_push(t, (u16)t->eflags);
    v86_push(t, vmc->drvseg);
    v86_push(t, vmc->o_ret);
    t->eflags = (t->eflags & ~(EFL_NT | EFL_RF | EFL_AC | EFL_IOPL)) | EFL_VM | EFL_IOPL | 2;
    if (how) t->eflags &= ~(EFL_IF | EFL_TF);
    if (n < (int)ARRAY_SIZE(nkind)) nkind[n] = 1;
    vm_enter(t);
}

void rm_int(struct vmregs *t, int vec)
{
    u32 v = lin_rd32((u32)vec * 4);
    t->cs = v >> 16;
    t->eip = v & 0xFFFF;
    rm_call(t, 1);
}

void pm_call(struct vmregs *t)
{
    int n = (int)vm_nest;
    if (n < (int)ARRAY_SIZE(nkind)) nkind[n] = 2;
    vm_enter(t);
}

void pm_return(struct vmregs *f)
{
    int n = (int)vm_nest - 1;
    if (n < 0 || (n < (int)ARRAY_SIZE(nkind) && nkind[n] != 2)) {
        vm_kill(f, "WINAUDIO DPMI: unexpected return to the host");
        return;
    }
    vm_resume(f);
}

/* HC_RET: a real-mode call of the host is over */
static void rm_return(struct vmregs *f)
{
    int n = (int)vm_nest - 1;
    if (n < 0 || (n < (int)ARRAY_SIZE(nkind) && nkind[n] != 1)) {
        vm_kill(f, "WINAUDIO: unexpected return to the V86 monitor");
        return;
    }
    vm_resume(f);
}

/* the CPU waits for an interrupt (protected mode, interrupts enabled) */
void vm_hlt(struct vmregs *f)
{
    int v;
    if (!(f->eflags & EFL_IF)) return;
    v = vm_halt();
    if (v >= 0 && irq_of(v) >= 0) irq_entry(f, v);
    else if (v == 2 && (f->eflags & EFL_VM)) v86_reflect(f, 2);
}

/* text through INT 10h AH=0Eh */
void vm_print(struct vmregs *f, const char *s)
{
    struct vmregs t;
    kprintf("%s", s);
    while (*s) {
        if (rm_setup(&t, f)) return;
        t.eax = 0x0E00 | (u8)*s++;
        t.ebx = 0x0007;
        rm_int(&t, 0x10);
    }
}

static void vm_reset(void)
{
    lin_wr16(0x472, 0x1234);
    outb(0x64, 0xFE);
    udelay(50000);
    outb(0xCF9, 0x02);
    outb(0xCF9, 0x06);
    for (;;) ;
}

/* a program did something the monitor cannot do: message, then the program ends */
void vm_kill(struct vmregs *f, const char *msg)
{
    char b[120];
    ksnprintf(b, sizeof(b), "\r\n%s at %04X:%04lX\r\n", msg, (u16)f->cs, f->eip);
    vm_print(f, b);
    if (!(f->eflags & EFL_VM) && dcl) {
        dpmi_kill(f);
        return;
    }
    f->eax = 0x4CFF;
    f->cs = vmc->drvseg;
    f->eip = vmc->o_term;
    f->eflags = EFL_VM | EFL_IOPL | EFL_IF | 2;
}

void __cdecl vm_panic(u32 *p)
{
    char b[120];
    u32 i;
    ksnprintf(b, sizeof(b), "WINAUDIO V86 MONITOR FAULT %lX: %lX %lX %lX %lX CR2 %lX", p[0], p[1], p[2], p[3], p[4],
              rd_cr2());
    kprintf("%s\n", b);
    for (i = 0; b[i]; i++) *(volatile u16 *)(0xB8000UL + i * 2) = (u16)(0x4F00 | (u8)b[i]);
}

/* ---------------------------------------------------------------- the emulated interrupt */

static u8 pic_real_isr(int sl)
{
    u16 p = sl ? 0xA0 : 0x20;
    u8 v;
    outb(p, 0x0B);
    v = inb(p);
    outb(p, vocw[sl]);
    return v;
}

static int vlevel(int sl)
{
    return sl ? sb_irq - 8 : (sb_irq < 8 ? sb_irq : 2);
}

static void trap(u16 p, int on)
{
    if (on) iopb[p >> 3] |= (u8)(1 << (p & 7));
    else iopb[p >> 3] &= (u8)~(1 << (p & 7));
}

static int trapped(u16 p)
{
    return (iopb[p >> 3] >> (p & 7)) & 1;
}

static void vpic_trap(int on)
{
    trap(0x20, on);
    if (sb_irq >= 8) trap(0xA0, on);
}

static u8 pic_rd(int sl)
{
    u8 v = inb(sl ? 0xA0 : 0x20);
    if (vocw[sl] == 0x0B && (vpic & (1 << sl))) v |= (u8)(1 << vlevel(sl));
    return v;
}

static void pic_wr(int sl, u8 v)
{
    u16 p = sl ? 0xA0 : 0x20;
    if (!(v & 0x10) && (v & 0x18) == 0x08) {               /* OCW3 */
        if (v & 2) vocw[sl] = (u8)(0x0A | (v & 1));
    } else if (!(v & 0x18) && (vpic & (1 << sl))) {         /* OCW2 */
        u8 c = (u8)(v & 0xE0), real = pic_real_isr(sl);
        if ((c == 0x20 && !real) || (c == 0x60 && (v & 7) == vlevel(sl) && !(real & (1 << vlevel(sl))))) {
            vpic &= (u8)~(1 << sl);
            if (!vpic) vpic_trap(0);
            return;
        }
    }
    outb(p, v);
}

/* the card's interrupt may be delivered to f: unmasked, nothing in service but the real
   interrupt irq (-1 none) that is being given to the program right now */
static int virq_ready(struct vmregs *f, int irq)
{
    u8 m = 0, s = 0;
    if (!sbc || !sbc->irqedge || vpic || !(f->eflags & EFL_IF)) return 0;
    if (sb_irq < 8) {
        if (inb(0x21) & (1 << sb_irq)) return 0;
    } else {
        if ((inb(0xA1) & (1 << (sb_irq - 8))) || (inb(0x21) & 4)) return 0;
    }
    if (irq >= 0 && irq < 8) m = (u8)(1 << irq);
    if (irq >= 8) {
        m = 4;
        s = (u8)(1 << (irq - 8));
    }
    if (pic_real_isr(0) & ~m) return 0;
    if ((sb_irq >= 8 || irq >= 8) && (pic_real_isr(1) & ~s)) return 0;
    sbc->irqedge = 0;
    vpic = (u8)(sb_irq < 8 ? 1 : 3);
    vpic_trap(1);
    return 1;
}

/* deliver the card's interrupt to f when the PIC would */
static void virq_check(struct vmregs *f)
{
    if (!virq_ready(f, -1)) return;
    if (dcl && dpmi_hwint(f, sb_vec)) return;
    if (f->eflags & EFL_VM) v86_reflect(f, sb_vec);
    else dpmi_rmirq(f, sb_vec);
}

/* ---------------------------------------------------------------- A20 */

static void a20_set(int on)
{
    u32 i;
    if (on == a20v) return;
    a20v = (u8)on;
    for (i = 0; i < 16; i++) {
        pt0[0x100 + i] = ((on ? 0x100000UL : 0) + i * 4096UL) | 7;
        invlpg(0x100000UL + i * 4096UL);
    }
}

static u8 a20_rd(u16 p)
{
    u8 v = inb(p);
    if (p == 0x92) return (u8)((v & ~2) | (a20v << 1));
    if (p == 0x60 && kbc_rdop) {
        kbc_rdop = 0;
        return (u8)((v & ~2) | (a20v << 1));
    }
    return v;
}

static void a20_wr(u16 p, u8 v)
{
    if (p == 0x92) {
        a20_set((v >> 1) & 1);
        outb(0x92, (u8)(v | 2));
        return;
    }
    if (p == 0x64) {
        kbc_cmd = 0;
        if (v == 0xD1) kbc_cmd = 0xD1;
        else if (v == 0xD0) kbc_rdop = 1;
        else if (v == 0xDD) { a20_set(0); v = 0xDF; }
        else if (v == 0xDF) a20_set(1);
        outb(0x64, v);
        return;
    }
    if (kbc_cmd == 0xD1) {
        kbc_cmd = 0;
        a20_set((v >> 1) & 1);
        v |= 2;
    }
    outb(0x60, v);
}

/* ---------------------------------------------------------------- trapped ports */

static u8 port_in(u16 p)
{
    u32 r;
    if (!trapped(p)) return inb(p);
    switch (p) {
    case 0x20: return pic_rd(0);
    case 0xA0: return pic_rd(1);
    case 0x60: case 0x64: case 0x92: return a20_rd(p);
    }
    r = sb_io(p, 0, 0);
    if (r & SBIO_FWD) return (u8)(inb(p) | r);
    return (u8)r;
}

static void port_out(u16 p, u8 v)
{
    if (!trapped(p)) {
        outb(p, v);
        return;
    }
    switch (p) {
    case 0x20: pic_wr(0, v); return;
    case 0xA0: pic_wr(1, v); return;
    case 0x60: case 0x64: case 0x92: a20_wr(p, v); return;
    }
    if (sb_io(p, v, 1) & SBIO_FWD) outb(p, v);
}

static u32 segbase(struct vmregs *f, int seg)
{
    u32 s;
    switch (seg) {
    case 0: s = f->es; break;
    case 1: s = f->cs; break;
    case 2: s = f->ss; break;
    case 4: s = f->fs; break;
    case 5: s = f->gs; break;
    default: s = f->ds; break;
    }
    if (f->eflags & EFL_VM) return (s & 0xFFFF) << 4;
    return sel_base(s, NULL, NULL);
}

/* IN, OUT, INS, OUTS at CS:EIP (any prefixes): 1 done */
int io_insn(struct vmregs *f, u32 csbase, int def32)
{
    u8 b[16];
    int i, osz = def32, asz = def32, rep = 0, seg = 3, size, out, str = 0, len;
    u32 port, ip = f->eip, n, k, idx, step, v;
    for (i = 0; i < 16; i++) b[i] = lin_rd8(csbase + (def32 ? ip + i : (u16)(ip + i)));
    for (i = 0; i < 15; i++) {
        switch (b[i]) {
        case 0x66: osz = !def32; continue;
        case 0x67: asz = !def32; continue;
        case 0xF2: case 0xF3: rep = 1; continue;
        case 0xF0: continue;
        case 0x26: seg = 0; continue;
        case 0x2E: seg = 1; continue;
        case 0x36: seg = 2; continue;
        case 0x3E: seg = 3; continue;
        case 0x64: seg = 4; continue;
        case 0x65: seg = 5; continue;
        }
        break;
    }
    switch (b[i]) {
    case 0xE4: case 0xE5: case 0xE6: case 0xE7:
        port = b[i + 1];
        len = i + 2;
        break;
    case 0xEC: case 0xED: case 0xEE: case 0xEF:
        port = f->edx & 0xFFFF;
        len = i + 1;
        break;
    case 0x6C: case 0x6D: case 0x6E: case 0x6F:
        port = f->edx & 0xFFFF;
        len = i + 1;
        str = 1;
        break;
    default:
        return 0;
    }
    size = (b[i] & 1) ? (osz ? 4 : 2) : 1;
    out = str ? (b[i] & 2) != 0 : (b[i] & 2) != 0;
    vmc->traps++;
    if (!str) {
        if (out) {
            for (k = 0; k < (u32)size; k++) port_out((u16)(port + k), (u8)(f->eax >> (8 * k)));
        } else {
            v = 0;
            for (k = 0; k < (u32)size; k++) v |= (u32)port_in((u16)(port + k)) << (8 * k);
            if (size == 1) f->eax = (f->eax & ~0xFFUL) | v;
            else if (size == 2) f->eax = (f->eax & ~0xFFFFUL) | v;
            else f->eax = v;
        }
    } else {
        u32 *ix = out ? &f->esi : &f->edi;
        u32 base = out ? segbase(f, seg) : segbase(f, 0);
        n = rep ? (asz ? f->ecx : f->ecx & 0xFFFF) : 1;
        step = (f->eflags & EFL_DF) ? (u32)-size : (u32)size;
        while (n) {
            idx = asz ? *ix : *ix & 0xFFFF;
            for (k = 0; k < (u32)size; k++) {
                if (out) port_out((u16)(port + k), lin_rd8(base + idx + k));
                else lin_wr8(base + idx + k, port_in((u16)(port + k)));
            }
            if (asz) *ix += step;
            else *ix = (*ix & 0xFFFF0000UL) | (u16)(*ix + step);
            n--;
        }
        if (rep) {
            if (asz) f->ecx = 0;
            else f->ecx &= 0xFFFF0000UL;
        }
    }
    if (def32) f->eip += (u32)len;
    else f->eip = (u16)(f->eip + len);
    return 1;
}

/* ---------------------------------------------------------------- interrupts */

int irq_of(int vec)
{
    if (vec >= vm_mbase && vec < vm_mbase + 8) return vec - vm_mbase;
    if (vec >= vm_sbase && vec < vm_sbase + 8) return 8 + vec - vm_sbase;
    return -1;
}

int irq_vec(int irq)
{
    return irq < 8 ? 8 + irq : 0x70 + irq - 8;
}

void vm_setredir(int vec, int on)
{
    if (on) redir[vec >> 3] |= (u8)(1 << (vec & 7));
    else redir[vec >> 3] &= (u8)~(1 << (vec & 7));
}

static void rtc_start(void)
{
    u8 a, b;
    u32 f = irq_save();
    outb(0x70, 0x0A);
    a = inb(0x71);
    if (!(a & 0x0F)) {
        outb(0x70, 0x0A);
        outb(0x71, (u8)(a | 6));
    }
    outb(0x70, 0x0B);
    b = inb(0x71);
    rtc_oldb = b;
    outb(0x70, 0x0B);
    outb(0x71, (u8)(b | 0x40));
    outb(0x70, 0x0C);
    (void)inb(0x71);
    outb(0xA1, (u8)(inb(0xA1) & ~1));
    outb(0x21, (u8)(inb(0x21) & ~4));
    rtc_on = 1;
    pie_ms = plat_ms();
    sbc->flags |= SBF_RTC;
    irq_restore(f);
}

static void rtc_stop(void)
{
    u8 b;
    if (!rtc_on) return;
    rtc_on = 0;
    sbc->flags &= (u8)~SBF_RTC;
    outb(0x70, 0x0B);
    b = inb(0x71);
    outb(0x70, 0x0B);
    outb(0x71, (u8)((b & ~0x40) | (rtc_oldb & 0x40)));
}

/* IRQ 8: the sound every other tick; 1 when nobody else wants the interrupt */
static int rtc_irq(void)
{
    if (!rtc_on) return 0;
    if ((++rtc_div & 1) && (sbc->flags & SBF_ON)) {
        sbc->tticks++;
        ent_sbtick(0);
    }
    if (lin_rd32(0x70 * 4) != vmc->bios70 || (lin_rd8(0x4A0) & 1) || (dcl && dpmi_hooked(0x70))) return 0;
    outb(0x70, 0x0C);
    (void)inb(0x71);
    outb(0xA0, 0x20);
    outb(0x20, 0x20);
    return 1;
}

/* the BIOS turns the periodic interrupt off when its own wait ends */
static void timer_irq(void)
{
    u8 b;
    u32 now;
    if (!rtc_on) return;
    now = plat_ms();
    if (now - pie_ms < 100) return;
    pie_ms = now;
    outb(0x70, 0x0B);
    b = inb(0x71);
    if (!(b & 0x40)) {
        outb(0x70, 0x0B);
        outb(0x71, (u8)(b | 0x40));
    }
}

/* the card's interrupt has a protected-mode handler of a DPMI client */
static int isdef_sb(void)
{
    return !dpmi_hooked(sb_vec);
}

/* a hardware interrupt arrived with f the code it interrupted */
void irq_entry(struct vmregs *f, int vec)
{
    int irq = irq_of(vec), rv;
    if (irq == 8 && rtc_irq()) return;
    if (irq == 0) timer_irq();
    rv = irq_vec(irq);
    if (dcl && dpmi_hwint(f, rv)) return;
    if (f->eflags & EFL_VM) {
        /* the card's interrupt, if due, runs right after the handler of this one (its frame
           goes below): the handler acknowledges its own interrupt at the PIC first */
        if (!(dcl && !isdef_sb()) && virq_ready(f, irq)) v86_reflect(f, sb_vec);
        v86_reflect(f, rv);
    } else {
        dpmi_rmirq(f, rv);
    }
}

/* INT n (not a hardware interrupt): the bytes before CS:IP are CD n */
static int is_softint(struct vmregs *f, int vec)
{
    u32 l = ((f->cs & 0xFFFF) << 4) + (u16)(f->eip - 2);
    return lin_rd8(l) == 0xCD && lin_rd8(l + 1) == (u8)vec;
}

/* ---------------------------------------------------------------- INT 15h, VCPI */

static u32 desc_base(u32 l)
{
    return lin_rd16(l + 2) | ((u32)lin_rd8(l + 4) << 16) | ((u32)lin_rd8(l + 7) << 24);
}

static void int15_move(struct vmregs *f)
{
    u32 g = ((f->es & 0xFFFF) << 4) + (f->esi & 0xFFFF), n = (f->ecx & 0xFFFF) * 2;
    f->eflags &= ~EFL_CF;
    f->eax &= ~0xFF00UL;
    if (n > 0x10000UL) {
        f->eax |= 0x0300;
        f->eflags |= EFL_CF;
        return;
    }
    if (n) phys_copy(desc_base(g + 0x18), desc_base(g + 0x10), n);
    f->eflags |= EFL_ZF;
}

static void setah(struct vmregs *f, u8 ah)
{
    f->eax = (f->eax & ~0xFF00UL) | ((u32)ah << 8);
}

u32 rd_dr0(void);
#pragma aux rd_dr0 = "mov eax,dr0" value [eax] modify exact [eax];
u32 rd_dr1(void);
#pragma aux rd_dr1 = "mov eax,dr1" value [eax] modify exact [eax];
u32 rd_dr2(void);
#pragma aux rd_dr2 = "mov eax,dr2" value [eax] modify exact [eax];
u32 rd_dr3(void);
#pragma aux rd_dr3 = "mov eax,dr3" value [eax] modify exact [eax];
u32 rd_dr6(void);
#pragma aux rd_dr6 = "mov eax,dr6" value [eax] modify exact [eax];
u32 rd_dr7(void);
#pragma aux rd_dr7 = "mov eax,dr7" value [eax] modify exact [eax];
void wr_dr0(u32 v);
#pragma aux wr_dr0 = "mov dr0,eax" parm [eax] modify exact [];
void wr_dr1(u32 v);
#pragma aux wr_dr1 = "mov dr1,eax" parm [eax] modify exact [];
void wr_dr2(u32 v);
#pragma aux wr_dr2 = "mov dr2,eax" parm [eax] modify exact [];
void wr_dr3(u32 v);
#pragma aux wr_dr3 = "mov dr3,eax" parm [eax] modify exact [];
void wr_dr6(u32 v);
#pragma aux wr_dr6 = "mov dr6,eax" parm [eax] modify exact [];
void wr_dr7(u32 v);
#pragma aux wr_dr7 = "mov dr7,eax" parm [eax] modify exact [];

static u32 rd_dr(int n)
{
    switch (n & 7) {
    case 0: return rd_dr0();
    case 1: return rd_dr1();
    case 2: return rd_dr2();
    case 3: return rd_dr3();
    case 4: case 6: return rd_dr6();
    }
    return rd_dr7();
}

static void wr_dr(int n, u32 v)
{
    switch (n & 7) {
    case 0: wr_dr0(v); break;
    case 1: wr_dr1(v); break;
    case 2: wr_dr2(v); break;
    case 3: wr_dr3(v); break;
    case 4: case 6: wr_dr6(v); break;
    default: wr_dr7(v);
    }
}

/* V86 mode -> the VCPI client's protected mode (AX=DE0Ch, ESI = linear of the switch data) */
static void vcpi_switch(struct vmregs *f)
{
    u32 s = f->esi, gw = drv_lin + vmc->o_gw, gdtr = lin_rd32(s + 4), idtr = lin_rd32(s + 8);
    int i;
    lin_wr32(gw + VMGW_CCR3, lin_rd32(s));
    for (i = 0; i < 6; i++) {
        lin_wr8(gw + VMGW_CGDTR + i, lin_rd8(gdtr + i));
        lin_wr8(gw + VMGW_CIDTR + i, lin_rd8(idtr + i));
    }
    lin_wr16(gw + VMGW_CLDT, lin_rd16(s + 12));
    lin_wr16(gw + VMGW_CTR, lin_rd16(s + 14));
    lin_wr32(gw + VMGW_CEIP, lin_rd32(s + 16));
    lin_wr32(gw + VMGW_CCS, lin_rd16(s + 20));
    memcpy((void *)(gw + VMGW_REGS), f, 32);
    vmc->vcpis++;
    vm_golin(drv_lin + vmc->o_gwvc);
}

static void vcpi(struct vmregs *f)
{
    u32 l, i;
    setah(f, 0);
    switch ((u8)f->eax) {
    case 0x00:
        /* With the DPMI host, VCPI is for the WINDRV drivers (BX = 5733h), Windows standard mode
           and /VCPI only: DOS extenders that prefer VCPI (DOS/32A, PMODE/W, CauseWay) would run
           in ring 0, where the Sound Blaster ports cannot be trapped; without VCPI they take
           DPMI. */
        if ((f->ebx & 0xFFFF) != 0x5733 && !(vmc->flags & (VMF_NODPMI | VMF_VCPI | VMF_WINSTD))) {
            setah(f, 0x84);
            break;
        }
        f->ebx = (f->ebx & ~0xFFFFUL) | 0x0100;
        kprintf("vm: VCPI client\n");
        break;
    case 0x01:                          /* ES:DI page table 0, DS:SI three descriptors */
        l = ((f->es & 0xFFFF) << 4) + (f->edi & 0xFFFF);
        for (i = 0; i < 0x110; i++) lin_wr32(l + i * 4, pt0[i]);
        f->edi = (f->edi & 0xFFFF0000UL) | (u16)(f->edi + 0x110 * 4);
        l = ((f->ds & 0xFFFF) << 4) + (f->esi & 0xFFFF);
        *(u64 *)l = mkdesc(0, 0xFFFFFFFFUL, 0x9A, 0x40);
        *(u64 *)(l + 8) = mkdesc(0, 0xFFFFFFFFUL, 0x92, 0x40);
        *(u64 *)(l + 16) = mkdesc(0, 0xFFFFFFFFUL, 0x92, 0x40);
        f->ebx = drv_lin + vmc->o_gwpm;
        break;
    case 0x02:
        f->edx = 0xFFFFF000UL;
        break;
    case 0x03:
        f->edx = 0;
        break;
    case 0x04:
        setah(f, 0x88);
        break;
    case 0x05:
        break;
    case 0x06:
        i = f->ecx & 0xFFFF;
        if (i >= 0x110) setah(f, 0x8B);
        else f->edx = pt0[i] & ~4095UL;
        break;
    case 0x07:
        f->ebx = rd_cr0();
        break;
    case 0x08:
    case 0x09:
        l = ((f->es & 0xFFFF) << 4) + (f->edi & 0xFFFF);
        for (i = 0; i < 8; i++) {
            if (i == 4 || i == 5) continue;
            if ((u8)f->eax == 0x08) lin_wr32(l + i * 4, rd_dr((int)i));
            else wr_dr((int)i, lin_rd32(l + i * 4));
        }
        break;
    case 0x0A:
        f->ebx = (f->ebx & ~0xFFFFUL) | vm_mbase;
        f->ecx = (f->ecx & ~0xFFFFUL) | vm_sbase;
        break;
    case 0x0B:
        vm_mbase = (u8)f->ebx;
        vm_sbase = (u8)f->ecx;
        break;
    case 0x0C:
        vcpi_switch(f);
        break;
    default:
        setah(f, 0x8F);
    }
}

/* ---------------------------------------------------------------- V86 mode */

static void v86_swint(struct vmregs *f, int v)
{
    u8 ah = (u8)(f->eax >> 8);
    switch (v) {
    case 0x15:
        if (ah == 0x87) {
            int15_move(f);
            return;
        }
        if (ah == 0x89) {                       /* switch to protected mode: not here */
            setah(f, 0xFF);
            f->eflags |= EFL_CF;
            return;
        }
        break;
    case 0x67:
        if (ah == 0xDE && !(vmc->flags & VMF_NOVCPI)) {
            vcpi(f);
            return;
        }
        if (lin_rd32(0x67 * 4) == (((u32)vmc->drvseg << 16) | vmc->o_iret)) {
            setah(f, 0x84);                     /* no EMS driver */
            return;
        }
        break;
    case 0x19:
        vm_reset();
        break;
    case 0x21:
        if (ah == 0x4C && dcl) dpmi_rmexit(f);
        break;
    case 0x1C: case 0x23: case 0x24:
        if (dcl && dpmi_rmint(f, v)) return;
        break;
    }
    v86_reflect(f, v);
}

static u32 *gpr(struct vmregs *f, int r)
{
    static const u8 off[8] = { 7, 6, 5, 4, 3, 2, 1, 0 };     /* EAX ECX EDX EBX - EBP ESI EDI */
    if (r == 4) return &f->esp;
    return (u32 *)f + off[r];
}

static void hcall(struct vmregs *f, int code)
{
    switch (code) {
    case HC_CORE:
        switch (f->eax) {
        case 1: f->eax = ent_dos(f->ebx); break;
        case 2: f->eax = ent_poll(f->ebx); break;
        case 3: f->eax = ent_irq(f->ebx); break;
        case 4: f->eax = ent_api(f->ebx); break;
        case 5: f->eax = ent_shutdown(f->ebx); break;
        case 6: f->eax = ent_resume(f->ebx); break;
        case 7: f->eax = ent_filedone(f->ebx); break;
        case 8: f->eax = ent_sbio(f->ebx); break;
        case 9: f->eax = ent_sbtick(f->ebx); break;
        default: f->eax = (u32)-1;
        }
        break;
    case HC_RET:
        rm_return(f);
        break;
    case HC_DPMI:
        dpmi_enter(f);
        break;
    case HC_RAWPM:
        dpmi_rawpm(f);
        break;
    case HC_TOREAL:
        vm_stop(f);
        break;
    default:
        if (code >= HC_CB && code < HC_CB + VM_NCB) dpmi_callback(f, code - HC_CB);
        else vm_kill(f, "WINAUDIO: bad call of the V86 monitor");
    }
}

/* the privileged instructions of a real-mode program */
static void v86_gp(struct vmregs *f)
{
    u32 cs = f->cs & 0xFFFF, base = cs << 4, ip = f->eip & 0xFFFF, v;
    u8 b[8], op;
    int i, len, r, n;
    if (f->err & 2) {                   /* INT n through a gate of DPL 0 (n < 20h) */
        op = lin_rd8(base + ip);
        f->eip = (u16)(ip + (op == 0xCD ? 2 : 1));
        v86_swint(f, (int)(f->err >> 3));
        return;
    }
    if (f->err) {
        vm_kill(f, "WINAUDIO: general protection fault");
        return;
    }
    if (io_insn(f, base, 0)) return;
    for (i = 0; i < 8; i++) b[i] = lin_rd8(base + (u16)(ip + i));
    for (i = 0; i < 4 && (b[i] == 0x66 || b[i] == 0x67 || b[i] == 0xF2 || b[i] == 0xF3 || b[i] == 0xF0 ||
                          b[i] == 0x26 || b[i] == 0x2E || b[i] == 0x36 || b[i] == 0x3E || b[i] == 0x64 ||
                          b[i] == 0x65); i++) ;
    op = b[i];
    if (op == 0xF4) {                   /* HLT */
        f->eip = (u16)(ip + i + 1);
        vm_hlt(f);
        return;
    }
    if (op == 0x0F) {
        u8 o2 = b[i + 1], m = b[i + 2];
        len = i + 2;
        r = m & 7;
        n = (m >> 3) & 7;
        switch (o2) {
        case 0x06: cpu_clts(); break;
        case 0x08: cpu_invd(); break;
        case 0x09: cpu_wbinvd(); break;
        case 0x20:                      /* MOV r32, CRn */
            len++;
            v = n == 0 ? rd_cr0() : n == 2 ? rd_cr2() : n == 3 ? rd_cr3() : n == 4 && vm_cr4 ? rd_cr4() : 0;
            *gpr(f, r) = v;
            break;
        case 0x22:                      /* MOV CRn, r32: cache and FPU bits of CR0 only */
            len++;
            v = *gpr(f, r);
            if (n != 0 || (v & 1)) goto bad;
            wr_cr0((rd_cr0() & ~0x6000000EUL) | (v & 0x6000000EUL));
            break;
        case 0x21:
            len++;
            *gpr(f, r) = rd_dr(n);
            break;
        case 0x23:
            len++;
            wr_dr(n, *gpr(f, r));
            break;
        case 0x30:
            if (vm_wrmsr(f->ecx, f->eax, f->edx)) goto bad;
            break;
        case 0x32:
            if (vm_rdmsr(f->ecx, &f->eax, &f->edx)) goto bad;
            break;
        case 0x01:
            if (n == 6 && (m & 0xC0) == 0xC0) {         /* LMSW r16 */
                len++;
                v = *gpr(f, r);
                if (v & 1) goto bad;
                wr_cr0((rd_cr0() & ~0xEUL) | (v & 0xE));
                break;
            }
            goto bad;
        default:
            goto bad;
        }
        f->eip = (u16)(ip + len);
        return;
    }
bad:
    if (cs == 0xF000 || cs == 0xFFFF) vm_reset();       /* the BIOS restarts the machine */
    vm_kill(f, "WINAUDIO: this program switches to protected mode by itself and needs real mode"
               " (WINAUDIO.SYS /NOV86)");
}

static int hypercall(struct vmregs *f)
{
    u32 l = ((f->cs & 0xFFFF) << 4) + (f->eip & 0xFFFF);
    u8 c;
    if ((f->cs & 0xFFFF) != vmc->drvseg || lin_rd8(l) != 0x63) return 0;
    c = lin_rd8(l + 1);
    if (c < 0xC0) return 0;
    f->eip = (u16)(f->eip + 2);
    hcall(f, c - 0xC0);
    return 1;
}

static void v86_trap(struct vmregs *f)
{
    int v = (int)(f->vec & 0xFF), err = (f->vec & 0x100) != 0;
    if (v == 13) {
        v86_gp(f);
        return;
    }
    if (v == 6 && hypercall(f)) return;
    if (!err && irq_of(v) >= 0 && !(v >= 0x20 && is_softint(f, v))) {
        irq_entry(f, v);
        return;
    }
    if (v < 0x20) {                     /* exceptions: as in real mode */
        v86_reflect(f, v);
        return;
    }
    v86_swint(f, v);
}

void __cdecl vm_trap(struct vmregs *f)
{
    if (f->eflags & EFL_VM) v86_trap(f);
    else pm_trap(f);
    virq_check(f);
}

/* ---------------------------------------------------------------- start, stop */

/* back to real mode for Windows 386 enhanced mode (its INT 2Fh 1605h switch routine) */
static void vm_stop(struct vmregs *f)
{
    u32 l = (f->ss & 0xFFFF) << 4, sp = (u16)(f->esp - 46), a = l + sp;
    u8 *drv = (u8 *)drv_lin;
    lin_wr32(a + 0, f->edi);
    lin_wr32(a + 4, f->esi);
    lin_wr32(a + 8, f->ebp);
    lin_wr32(a + 12, 0);
    lin_wr32(a + 16, f->ebx);
    lin_wr32(a + 20, f->edx);
    lin_wr32(a + 24, f->ecx);
    lin_wr32(a + 28, f->eax);
    lin_wr16(a + 32, (u16)f->ds);
    lin_wr16(a + 34, (u16)f->es);
    lin_wr16(a + 36, (u16)f->fs);
    lin_wr16(a + 38, (u16)f->gs);
    lin_wr16(a + 40, (u16)f->eip);
    lin_wr16(a + 42, (u16)f->cs);
    lin_wr16(a + 44, (u16)(f->eflags & ~(EFL_IOPL | EFL_NT | EFL_IF | EFL_TF | EFL_CF)));
    *(u16 *)(drv + vmc->o_rmss) = (u16)f->ss;
    *(u16 *)(drv + vmc->o_rmss + 2) = (u16)sp;
    rtc_stop();
    a20_set(1);
    drv[vmc->o_vmon] = 0;
    vmc->flags = (u16)((vmc->flags & ~VMF_ON) | VMF_WIN);
    vm_nest = 0;
    nrmstk = 0;
    kprintf("vm: real mode for Windows\n");
    if (vm_cr4) wr_cr4(vm_cr4 & ~3UL);
    vm_toreal();
}

static void ports(void)
{
    static const u8 pg8[4] = { 0x87, 0x83, 0x81, 0x82 }, pg16[3] = { 0x8B, 0x89, 0x8A };
    u16 p;
    int ch;
    for (p = 0; p < 16; p++) trap((u16)(sbc->base + p), 1);
    if (!(sbc->flags & SBF_NOOPL))
        for (p = 0x388; p <= 0x38B; p++) trap(p, 1);
    if (sbc->mpu) {
        trap(sbc->mpu, 1);
        trap((u16)(sbc->mpu + 1), 1);
    }
    ch = sbc->dma8 & 3;
    trap((u16)(ch * 2), 1);
    trap((u16)(ch * 2 + 1), 1);
    trap(pg8[ch], 1);
    trap(0x08, 1);
    for (p = 0x0A; p < 0x10; p++) trap(p, 1);
    ch = sbc->dma16;
    if (ch >= 5 && ch <= 7) {
        trap((u16)(0xC0 + (ch - 4) * 4), 1);
        trap((u16)(0xC2 + (ch - 4) * 4), 1);
        trap(pg16[ch - 5], 1);
        trap(0xD0, 1);
        for (p = 0xD4; p < 0xE0; p += 2) trap(p, 1);
    }
    trap(0x60, 1);
    trap(0x64, 1);
    trap(0x92, 1);
}

static void userpage(void *p, u32 len)
{
    u32 *pt = (u32 *)(CORE_WIN + 0x2000), i = ((u32)p - CORE_WIN) >> 12, n = (len + 4095) >> 12;
    while (n--) pt[i++] |= 4;
}

static int vm_setup(void)
{
    u32 feat = cpu_features(), i, gw;
    u8 *lpms;
    pd = (u32 *)CORE_WIN;
    pt0 = (u32 *)(CORE_WIN + 0x1000);
    tss = kmalloc_align(104 + 32 + 8192 + 16, 16);
    idt = kmalloc_align(256 * 8, 8);
    gdt = kmalloc_align(VM_NGDT * 8, 8);
    stubpg = kmalloc_align(4096, 4096);
    spt = kmalloc_align(4096, 4096);
    lpms = kmalloc_align(16384, 4096);
    vm_esp0 = (u32)kmalloc(65536);
    if (!tss || !idt || !gdt || !stubpg || !spt || !lpms || !vm_esp0) return -1;
    vm_esp0 = (vm_esp0 + 65536 - 16) & ~15UL;
    kprintf("vm: stubs %lx, locked stack %lx, ring 0 stack %lx, TSS %lx\n", (u32)stubpg, (u32)lpms, vm_esp0, (u32)tss);
    vm_tssp = (u32)tss;
    redir = tss + 104;
    iopb = tss + 136;
    *(u32 *)(tss + 4) = vm_esp0;
    *(u16 *)(tss + 8) = SEL_DATA32;
    *(u16 *)(tss + 102) = 136;
    iopb[8192] = 0xFF;
    vm_setredir(0x15, 1);
    vm_setredir(0x67, 1);
    vm_setredir(0x19, 1);
    pd[511] = vphys(spt) | 3;
    pd[512] |= 4;
    memset(stubpg, 0xF4, 4096);
    stubpg[STUB_RETF] = 0xCB;
    userpage(stubpg, 4096);
    userpage(lpms, 16384);
    wr_cr3(rd_cr3());
    gdt[1] = mkdesc(0, 0xFFFFFFFFUL, 0x9A, 0x40);
    gdt[2] = mkdesc(0, 0xFFFFFFFFUL, 0x92, 0x40);
    gdt[3] = mkdesc(drv_lin, 0xFFFF, 0x9A, 0);
    gdt[4] = mkdesc(drv_lin, 0xFFFF, 0x92, 0);
    gdt[5] = mkdesc((u32)tss, 136 + 8192, 0x89, 0);
    gdt[7] = mkdesc((u32)stubpg, 4095, 0xFA, 0x40);
    gdt[8] = mkdesc((u32)stubpg, 4095, 0xFA, 0);
    gdt[9] = mkdesc((u32)lpms, 16383, 0xF2, 0x40);
    gdt[10] = mkdesc((u32)lpms, 16383, 0xF2, 0);
    gdt[11] = mkdesc(0, 0xFFFF, 0xF2, 0);
    gdt[12] = mkdesc(0, 0xFFFFFFFFUL, 0xF2, 0x40);
    gdt[13] = mkdesc((u32)vm_espbuf - 0x20, 0xFFFF, 0x92, 0x40);
    for (i = 0; i < 256; i++) {
        u32 a = (u32)vm_stubs + i * 10;
        idt[i] = (u64)(a & 0xFFFF) | ((u64)SEL_CODE32 << 16) | ((u64)(i >= 0x20 ? 0xEE : 0x8E) << 40) |
                 ((u64)(a >> 16) << 48);
    }
    *(u16 *)vm_gdtr = VM_NGDT * 8 - 1;
    *(u32 *)(vm_gdtr + 2) = (u32)gdt;
    *(u16 *)vm_idtr = 256 * 8 - 1;
    *(u32 *)(vm_idtr + 2) = (u32)idt;
    vm_tssdesc = (u32)&gdt[5];
    if ((feat & 2) && (vmc->flags & VMF_USEVME) && !(vmc->flags & VMF_NOVME)) {
        vm_cr4 = rd_cr4() | 1;
        vmc->flags |= VMF_VME;
    } else if (feat & 2) {
        vm_cr4 = rd_cr4() & ~3UL;
        if (!vm_cr4) vm_cr4 = 0;
    }
    wr_cr0(rd_cr0() & ~0x40000UL);      /* no alignment checks */
    vm_rmptr[0] = vmc->o_rm16;
    if (!(vmc->flags & VMF_NODPMI)) {
        u32 ldt, n = dpmi_setup(&ldt, (u32)stubpg, (u32)lpms);
        if (n) {
            gdt[6] = mkdesc(ldt, n * 8 - 1, 0x82, 0);
            vm_ldtsel = SEL_LDT;
        } else {
            vmc->flags |= VMF_NODPMI;
        }
    }
    /* the VCPI gateway in WINAUDIO.SYS */
    gw = drv_lin + vmc->o_gw;
    lin_wr32(gw + VMGW_CR3, rd_cr3());
    lin_wr32(gw + VMGW_CR4, vm_cr4);
    memcpy((void *)(gw + VMGW_GDTR), vm_gdtr, 6);
    memcpy((void *)(gw + VMGW_IDTR), vm_idtr, 6);
    lin_wr32(gw + VMGW_TSSD, vm_tssdesc);
    lin_wr32(gw + VMGW_ESP0P, (u32)&vm_esp0);
    lin_wr32(gw + VMGW_EXIT, (u32)vm_exit);
    lin_wr16(gw + VMGW_EXIT + 4, SEL_CODE32);
    lin_wr16(gw + VMGW_LDT, (u16)vm_ldtsel);
    lin_wr16(gw + VMGW_TR, SEL_TSS);
    lin_wr16(gw + VMGW_DS, SEL_DATA32);
    sbc = sb_cfg();
    sb_irq = sbc->irq == 2 ? 9 : sbc->irq;
    sb_vec = (u8)irq_vec(sb_irq);
    ports();
    return 0;
}

/* FN_VMSTART: arg 0 the first start, 1 back from Windows */
u32 __cdecl ent_vmstart(u32 arg)
{
    struct vmregs f;
    u8 *drv;
    vmc = (struct winvm *)((u8 *)cb + WIN_VMOFF);
    drv_lin = (u32)vmc->drvseg << 4;
    drv = (u8 *)drv_lin;
    if (!sb_cfg() || !(sb_cfg()->flags & SBF_ON)) return 2;
    if (!started) {
        if (vm_setup()) return 1;
        started = 1;
        kprintf("vm: V86 monitor, VME %d, DPMI %d, VCPI %d\n", (vmc->flags & VMF_VME) != 0,
                !(vmc->flags & VMF_NODPMI), !(vmc->flags & VMF_NOVCPI));
    }
    if (arg) kprintf("vm: V86 mode again after Windows\n");
    memset(&f, 0, sizeof(f));
    f.eip = vmc->o_vret;
    f.cs = vmc->drvseg;
    f.ss = *(u16 *)(drv + vmc->o_savess);
    f.esp = *(u32 *)(drv + vmc->o_saveesp);
    f.ds = f.es = f.fs = f.gs = vmc->drvseg;
    f.eflags = EFL_VM | EFL_IOPL | 2;
    *(u32 *)(drv + vmc->o_callret) = 0;
    drv[vmc->o_vmon] = 1;
    vmc->flags = (u16)((vmc->flags | VMF_ON) & ~VMF_WIN);
    vpic = 0;
    vocw[0] = vocw[1] = 0x0A;
    vpic_trap(0);
    nrmstk = 0;
    rtc_start();
    vm_launch(&f);
    return 0;
}
