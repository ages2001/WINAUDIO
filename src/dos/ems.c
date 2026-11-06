/* ems.c - expanded memory (LIM EMS 4.0) of the WINAUDIO V86 monitor.
   Without EMM386 the Sound Blaster emulation runs DOS under the monitor of WINAUDIO.SYS (EMM386
   cannot trap ports for another driver), and programs that need expanded memory get it here:
   a 64 KB page frame in a free part of the upper memory area, logical pages from XMS memory
   (one XMS block for each handle, allocated when the program asks), mapped through the page
   table of the first megabyte. INT 67h (the VCPI functions DEh stay with vmon.c), the device
   name EMMXXXX0 is WINAUDIO.SYS's own while expanded memory is offered. */
#include "core.h"
#include "vmon.h"

#define EMS_MAXH    64
#define EMS_PAGE    16384UL
#define EMS_NPHYS   4

struct emsh {
    u8 used, saved;
    u16 xh;                     /* XMS handle (0: no pages) */
    u32 pages;
    u32 phys;                   /* physical address of the XMS block */
    char name[8];
    u16 smap[EMS_NPHYS];        /* page map saved by function 47h */
    u8 sh[EMS_NPHYS];
};

static struct emsh eh[EMS_MAXH];
static u16 frame;               /* segment of the page frame, 0: no expanded memory */
static u32 limit;               /* pages offered */
static u32 used;                /* pages allocated */
static u8 mh[EMS_NPHYS];        /* handle mapped in each physical page, 0xFF none */
static u16 ml[EMS_NPHYS];       /* its logical page */
static u32 orig[EMS_NPHYS * 4]; /* page table entries of the frame when nothing is mapped */
static u32 *ptab;
static u8 *xbuf;

static void setah(struct vmregs *f, u8 ah)
{
    f->eax = (f->eax & ~0xFF00UL) | ((u32)ah << 8);
}

static void setbx(struct vmregs *f, u32 v)
{
    f->ebx = (f->ebx & ~0xFFFFUL) | (v & 0xFFFF);
}

static void setdx(struct vmregs *f, u32 v)
{
    f->edx = (f->edx & ~0xFFFFUL) | (v & 0xFFFF);
}

static u32 lin_of(u32 seg, u32 off)
{
    return ((seg & 0xFFFF) << 4) + (off & 0xFFFF);
}

/* physical page p shows logical page l of handle h (h 0xFF: the frame's own memory again) */
static void map(int p, u8 h, u16 l)
{
    u32 k, base = ((u32)frame >> 8) + (u32)p * 4;
    mh[p] = h;
    ml[p] = l;
    for (k = 0; k < 4; k++) {
        ptab[base + k] = h == 0xFF ? orig[p * 4 + k] : (eh[h].phys + l * EMS_PAGE + k * 4096UL) | 7;
        invlpg((base + k) << 12);
    }
}

/* physical address of a linear address of the first megabyte (the frame mapped or not) */
static u32 phys_of(u32 lin)
{
    return (ptab[lin >> 12] & ~4095UL) | (lin & 4095);
}

/* the 2 KB blocks of C0000h-EFFFFh that option ROMs take (55h AAh, length in 512 bytes) */
static u32 roms[3];

static void scan_roms(void)
{
    u32 a = 0xC0000UL, n;
    memset(roms, 0, sizeof(roms));
    while (a < 0xF0000UL) {
        if (lin_rd8(a) == 0x55 && lin_rd8(a + 1) == 0xAA && lin_rd8(a + 2)) {
            n = ((u32)lin_rd8(a + 2) * 512 + 2047) / 2048;
            while (n-- && a < 0xF0000UL) {
                u32 b = (a - 0xC0000UL) >> 11;
                roms[b >> 5] |= 1UL << (b & 31);
                a += 2048;
            }
            continue;
        }
        a += 2048;
    }
}

/* a free 64 KB in the upper memory area: no option ROM, every byte the same, FFh (nothing
   there) or 00h (shadow RAM nobody uses) */
static u16 find_frame(void)
{
    static const u16 cand[] = { 0xD000, 0xE000, 0xD400, 0xC800, 0xCC00, 0xD800, 0xDC00 };
    int i;
    u32 k, v;
    scan_roms();
    for (i = 0; i < (int)ARRAY_SIZE(cand); i++) {
        u32 a = (u32)cand[i] << 4;
        for (k = 0; k < 65536UL; k += 2048) {
            u32 b = (a + k - 0xC0000UL) >> 11;
            if (roms[b >> 5] & (1UL << (b & 31))) break;
        }
        if (k < 65536UL) continue;
        v = lin_rd32(a);
        if (v != 0xFFFFFFFFUL && v != 0) continue;
        for (k = 0; k < 65536UL; k += 4)
            if (lin_rd32(a + k) != v) break;
        if (k == 65536UL) return cand[i];
    }
    return 0;
}

int ems_init(u32 *pt0, u32 mb)
{
    int i;
    ptab = pt0;
    frame = find_frame();
    if (!frame) {
        kprintf("ems: no free 64 KB in the upper memory area, no expanded memory\n");
        return -1;
    }
    xbuf = kmalloc(4096);
    if (!xbuf) {
        frame = 0;
        return -1;
    }
    limit = (mb ? mb : 16) * 64;
    for (i = 0; i < EMS_NPHYS * 4; i++) orig[i] = ptab[((u32)frame >> 8) + i];
    for (i = 0; i < EMS_NPHYS; i++) mh[i] = 0xFF;
    memset(eh, 0, sizeof(eh));
    eh[0].used = 1;                     /* the system handle */
    kprintf("ems: page frame %04x, up to %lu KB\n", frame, limit * 16);
    return 0;
}

u16 ems_frame(void)
{
    return frame;
}

/* Windows 386 enhanced mode takes over: the frame shows its own memory, the pages go back */
void ems_drop(struct vmregs *f)
{
    int i;
    if (!frame) return;
    for (i = 0; i < EMS_NPHYS; i++) map(i, 0xFF, 0);
    for (i = 1; i < EMS_MAXH; i++)
        if (eh[i].used) {
            if (eh[i].xh) vm_xms_free(f, eh[i].xh);
            memset(&eh[i], 0, sizeof(eh[i]));
        }
    used = 0;
}

static int set_pages(struct vmregs *f, int h, u32 pages)
{
    struct emsh *e = &eh[h];
    u16 nh = 0;
    u32 np = 0, i;
    if (pages > e->pages && pages - e->pages > limit - used) return 0x88;
    if (pages) {
        if (vm_xms_alloc(f, pages * 16, &nh, &np)) return 0x88;
        for (i = 0; i < MIN(pages, e->pages); i++) vm_phys_copy(np + i * EMS_PAGE, e->phys + i * EMS_PAGE, EMS_PAGE);
    }
    if (e->xh) vm_xms_free(f, e->xh);
    used = used - e->pages + pages;
    e->xh = nh;
    e->phys = np;
    e->pages = pages;
    for (i = 0; i < EMS_NPHYS; i++)
        if (mh[i] == h) map((int)i, (u8)(ml[i] < pages ? h : 0xFF), ml[i]);
    return 0;
}

static int new_handle(struct vmregs *f, u32 pages)
{
    int h, r;
    if (pages > limit) return -0x87;
    if (pages > limit - used) return -0x88;
    for (h = 1; h < EMS_MAXH && eh[h].used; h++) ;
    if (h == EMS_MAXH) return -0x85;
    memset(&eh[h], 0, sizeof(eh[h]));
    eh[h].used = 1;
    r = set_pages(f, h, pages);
    if (r) {
        eh[h].used = 0;
        return -r;
    }
    return h;
}

static int handle_ok(u32 h)
{
    return h < EMS_MAXH && eh[h].used;
}

/* function 57h: move (AL 0) or exchange (AL 1) a region; DS:SI the description */
static u8 move(struct vmregs *f, int xchg)
{
    u32 d = lin_of(f->ds, f->esi), len = lin_rd32(d), done = 0, k;
    u32 at[2];
    u8 type[2];
    u16 hd[2], off[2], sp[2];
    for (k = 0; k < 2; k++) {
        u32 b = d + 4 + k * 7;
        type[k] = lin_rd8(b);
        hd[k] = lin_rd16(b + 1);
        off[k] = lin_rd16(b + 3);
        sp[k] = lin_rd16(b + 5);
        if (type[k] > 1) return 0x98;
        if (type[k] == 1) {
            if (!handle_ok(hd[k])) return 0x83;
            if (off[k] >= EMS_PAGE) return 0x95;
            if ((u32)sp[k] * EMS_PAGE + off[k] + len > eh[hd[k]].pages * EMS_PAGE) return 0x93;
            at[k] = (u32)sp[k] * EMS_PAGE + off[k];
        } else {
            at[k] = lin_of(sp[k], off[k]);
            if (at[k] + len > 0x110000UL) return 0xA2;
        }
    }
    if (len > 0x100000UL) return 0x96;
    while (done < len) {
        u32 p[2], n = len - done;
        for (k = 0; k < 2; k++) {
            u32 a = at[k] + done, room;
            if (type[k]) {
                p[k] = eh[hd[k]].phys + a;
                room = EMS_PAGE - (a % EMS_PAGE);
            } else {
                p[k] = phys_of(a);
                room = 4096 - (a & 4095);
            }
            if (n > room) n = room;
        }
        if (n > 4096) n = 4096;
        if (xchg) {
            memcpy(xbuf, vm_phys_ptr(p[0], n), n);
            vm_phys_copy(p[0], p[1], n);
            memcpy(vm_phys_ptr(p[1], n), xbuf, n);
        } else {
            vm_phys_copy(p[1], p[0], n);
        }
        done += n;
    }
    return 0;
}

/* INT 67h, AH 40h..5Dh */
void ems_call(struct vmregs *f)
{
    u8 ah = (u8)(f->eax >> 8), al = (u8)f->eax, r = 0;
    u32 dx = f->edx & 0xFFFF, bx = f->ebx & 0xFFFF, i;
    int h;
    switch (ah) {
    case 0x40:                          /* status */
        break;
    case 0x41:                          /* page frame */
        setbx(f, frame);
        break;
    case 0x42:                          /* free and total pages */
        setbx(f, limit - used);
        setdx(f, limit);
        break;
    case 0x43:                          /* allocate */
        if (!bx) {
            r = 0x89;
            break;
        }
        /* fall through */
    case 0x5A:                          /* allocate (zero pages allowed) */
        if (ah == 0x5A && al > 1) {
            r = 0x8F;
            break;
        }
        h = new_handle(f, bx);
        if (h < 0) r = (u8)-h;
        else setdx(f, (u32)h);
        break;
    case 0x44:                          /* map / unmap one page */
        if (!handle_ok(dx)) r = 0x83;
        else if (al >= EMS_NPHYS) r = 0x8B;
        else if (bx == 0xFFFF) map(al, 0xFF, 0);
        else if (bx >= eh[dx].pages) r = 0x8A;
        else map(al, (u8)dx, (u16)bx);
        break;
    case 0x45:                          /* deallocate */
        if (!handle_ok(dx)) r = 0x83;
        else if (eh[dx].saved) r = 0x86;
        else {
            set_pages(f, (int)dx, 0);
            for (i = 0; i < EMS_NPHYS; i++)
                if (mh[i] == dx) map((int)i, 0xFF, 0);
            if (dx) eh[dx].used = 0;
        }
        break;
    case 0x46:                          /* version */
        f->eax = (f->eax & ~0xFFUL) | 0x40;
        break;
    case 0x47:                          /* save the page map */
        if (!handle_ok(dx)) r = 0x83;
        else if (eh[dx].saved) r = 0x8D;
        else {
            for (i = 0; i < EMS_NPHYS; i++) {
                eh[dx].sh[i] = mh[i];
                eh[dx].smap[i] = ml[i];
            }
            eh[dx].saved = 1;
        }
        break;
    case 0x48:                          /* restore it */
        if (!handle_ok(dx)) r = 0x83;
        else if (!eh[dx].saved) r = 0x8E;
        else {
            for (i = 0; i < EMS_NPHYS; i++)
                map((int)i, (u8)(eh[dx].sh[i] != 0xFF && handle_ok(eh[dx].sh[i]) ? eh[dx].sh[i] : 0xFF), eh[dx].smap[i]);
            eh[dx].saved = 0;
        }
        break;
    case 0x4B:                          /* handles in use */
        for (i = 0, h = 0; i < EMS_MAXH; i++) if (eh[i].used) h++;
        setbx(f, (u32)h);
        break;
    case 0x4C:                          /* pages of a handle */
        if (!handle_ok(dx)) r = 0x83;
        else setbx(f, eh[dx].pages);
        break;
    case 0x4D:                          /* every handle and its pages */
    {
        u32 o = lin_of(f->es, f->edi);
        for (i = 0, h = 0; i < EMS_MAXH; i++)
            if (eh[i].used) {
                lin_wr16(o + h * 4, (u16)i);
                lin_wr16(o + h * 4 + 2, (u16)eh[i].pages);
                h++;
            }
        setbx(f, (u32)h);
        break;
    }
    case 0x4E:                          /* the whole page map: get, set, get and set, size */
    case 0x4F:                          /* (partial: the same, the whole map) */
        if (al == 3 || (ah == 0x4F && al == 2)) {
            f->eax = (f->eax & ~0xFFUL) | (EMS_NPHYS * 4);
            break;
        }
        if (al > 2) {
            r = 0x8F;
            break;
        }
        if (al == 0 || al == 2) {
            u32 o = lin_of(f->es, f->edi);
            for (i = 0; i < EMS_NPHYS; i++) {
                lin_wr16(o + i * 4, mh[i]);
                lin_wr16(o + i * 4 + 2, ml[i]);
            }
        }
        if (al == 1 || al == 2) {
            u32 s = lin_of(f->ds, f->esi);
            for (i = 0; i < EMS_NPHYS; i++) {
                u8 hh = (u8)lin_rd16(s + i * 4);
                u16 ll = lin_rd16(s + i * 4 + 2);
                map((int)i, (u8)(hh != 0xFF && handle_ok(hh) && ll < eh[hh].pages ? hh : 0xFF), ll);
            }
        }
        break;
    case 0x50:                          /* map several pages: AL 0 physical page numbers, 1 segments */
    {
        u32 s = lin_of(f->ds, f->esi), n = f->ecx & 0xFFFF;
        if (!handle_ok(dx)) {
            r = 0x83;
            break;
        }
        if (al > 1) {
            r = 0x8F;
            break;
        }
        for (i = 0; i < n && !r; i++) {
            u16 l = lin_rd16(s + i * 4), p = lin_rd16(s + i * 4 + 2);
            if (al == 1) p = (u16)(p >= frame && p < frame + 0x1000 && !((p - frame) & 0x3FF) ? (p - frame) >> 10 : 0xFF);
            if (p >= EMS_NPHYS) r = 0x8B;
            else if (l == 0xFFFF) map(p, 0xFF, 0);
            else if (l >= eh[dx].pages) r = 0x8A;
            else map(p, (u8)dx, l);
        }
        break;
    }
    case 0x51:                          /* reallocate */
        if (!handle_ok(dx)) r = 0x83;
        else if (bx > limit) r = 0x87;
        else r = (u8)set_pages(f, (int)dx, bx);
        setbx(f, eh[dx < EMS_MAXH ? dx : 0].pages);
        break;
    case 0x52:                          /* attributes: volatile only */
        if (al == 0 || al == 2) f->eax &= ~0xFFUL;
        else if (al == 1) r = (f->ebx & 0xFF) ? 0x91 : 0;
        else r = 0x8F;
        break;
    case 0x53:                          /* handle name */
        if (!handle_ok(dx)) r = 0x83;
        else if (al == 0) for (i = 0; i < 8; i++) lin_wr8(lin_of(f->es, f->edi) + i, (u8)eh[dx].name[i]);
        else if (al == 1) for (i = 0; i < 8; i++) eh[dx].name[i] = (char)lin_rd8(lin_of(f->ds, f->esi) + i);
        else r = 0x8F;
        break;
    case 0x54:                          /* handle directory */
        if (al == 0) {
            u32 o = lin_of(f->es, f->edi);
            for (i = 0, h = 0; i < EMS_MAXH; i++)
                if (eh[i].used) {
                    u32 k;
                    lin_wr16(o + h * 10, (u16)i);
                    for (k = 0; k < 8; k++) lin_wr8(o + h * 10 + 2 + k, (u8)eh[i].name[k]);
                    h++;
                }
            f->eax = (f->eax & ~0xFFUL) | (u8)h;
        } else if (al == 1) {
            u32 s = lin_of(f->ds, f->esi), k;
            r = 0xA0;
            for (i = 1; i < EMS_MAXH && r; i++) {
                if (!eh[i].used) continue;
                for (k = 0; k < 8 && (u8)eh[i].name[k] == lin_rd8(s + k); k++) ;
                if (k == 8) {
                    setdx(f, i);
                    r = 0;
                }
            }
        } else if (al == 2) {
            setbx(f, EMS_MAXH);
        } else {
            r = 0x8F;
        }
        break;
    case 0x57:                          /* move / exchange memory region */
        r = al <= 1 ? move(f, al) : 0x8F;
        break;
    case 0x58:                          /* mappable physical addresses */
        if (al == 0) {
            u32 o = lin_of(f->es, f->edi);
            for (i = 0; i < EMS_NPHYS; i++) {
                lin_wr16(o + i * 4, (u16)(frame + i * 0x400));
                lin_wr16(o + i * 4 + 2, (u16)i);
            }
        } else if (al != 1) {
            r = 0x8F;
            break;
        }
        f->ecx = (f->ecx & ~0xFFFFUL) | EMS_NPHYS;
        break;
    case 0x59:                          /* hardware information */
        if (al == 0) {
            u32 o = lin_of(f->es, f->edi);
            lin_wr16(o, 0x400);
            lin_wr16(o + 2, 0);
            lin_wr16(o + 4, EMS_NPHYS * 4);
            lin_wr16(o + 6, 0);
            lin_wr16(o + 8, 0);
        } else if (al == 1) {
            setbx(f, limit - used);
            setdx(f, limit);
        } else {
            r = 0x8F;
        }
        break;
    default:
        r = 0x84;
    }
    setah(f, r);
}
