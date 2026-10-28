/* ac97.c - AC'97 controllers: Intel ICH family and compatibles (ICH0..ICH7, 440MX, ESB,
   nVidia nForce..MCP51, AMD 768/8111, SiS 7012), VIA 82C686 and VT8233/8235/8237.
   Any AC'97 codec; the codec runs at 48 kHz and the engine converts everything else. */
#include "audio.h"

#define MAXAC       2
#define NBDL        32
#define PERIOD      2048
#define RINGSIZE    (PERIOD * 2 * NBDL)

/* codec registers */
#define CR_RESET    0x00
#define AC_MASTER   0x02
#define AC_HP       0x04
#define AC_MONO     0x06
#define AC_MIC      0x0E
#define AC_LINE     0x10
#define AC_CD       0x12
#define AC_AUX      0x16
#define AC_PCM      0x18
#define AC_RECSEL   0x1A
#define AC_RECGAIN  0x1C
#define AC_POWER    0x26
#define AC_EXTID    0x28
#define AC_EXTCTL   0x2A
#define AC_DACRATE  0x2C
#define AC_ADCRATE  0x32
#define AC_VID1     0x7C
#define AC_VID2     0x7E

/* ICH bus master */
#define PI          0x00
#define PO          0x10
#define BDBAR       0x00
#define CIV         0x04
#define LVI         0x05
#define XSR         0x06
#define PICB        0x08
#define XCR         0x0B
#define GLOB_CNT    0x2C
#define GLOB_STA    0x30
#define CAS         0x34

#define K_ICH   0
#define K_SIS   1
#define K_NV    2
#define K_VIA   3       /* 82C686 */
#define K_VIA8  4       /* VT8233 and later */

struct ac97 {
    struct adev a;
    struct pcidev pci;
    int kind;
    u16 nam, nabm;
    u32 *bdl[2];
    u8 *ringbuf[2];
    u8 mbits;           /* master attenuation steps: 31 or 63 */
    u8 hbits;
    u8 vra;
    u16 coff;           /* ICH: codec register block (0 primary, 80h secondary codec) */
};

static struct ac97 *ctrls[MAXAC];
static int nctrl;

/* ---------------------------------------------------------------- codec access */

static int via_wait(struct ac97 *c)
{
    u32 t0 = plat_ms();
    while (inl((u16)(c->nam + 0x80)) & 0x01000000UL)
        if (plat_ms() - t0 > 5) return -1;
    return 0;
}

static void ich_cas(struct ac97 *c)
{
    u32 t0 = plat_ms();
    if (c->kind == K_SIS || c->kind == K_NV) return;
    while (inb((u16)(c->nabm + CAS)) & 1)
        if (plat_ms() - t0 > 2) break;
}

static u16 crd(struct ac97 *c, int reg)
{
    if (c->kind >= K_VIA) {
        u32 v, t0;
        via_wait(c);
        outl((u16)(c->nam + 0x80), ((u32)reg << 16) | 0x00800000UL);
        udelay(20);
        via_wait(c);
        t0 = plat_ms();
        while (!((v = inl((u16)(c->nam + 0x80))) & 0x02000000UL))
            if (plat_ms() - t0 > 5) return 0xFFFF;
        udelay(25);
        return (u16)inl((u16)(c->nam + 0x80));
    }
    ich_cas(c);
    return inw((u16)(c->nam + c->coff + reg));
}

static void cwr(struct ac97 *c, int reg, u16 v)
{
    if (c->kind >= K_VIA) {
        via_wait(c);
        outl((u16)(c->nam + 0x80), ((u32)reg << 16) | v);
        via_wait(c);
        return;
    }
    ich_cas(c);
    outw((u16)(c->nam + c->coff + reg), v);
}

/* ---------------------------------------------------------------- ThinkPad
   IBM ThinkPads have a volume level and mute switch in the embedded controller (register 30h:
   level in bits 3:0, mute in bit 6) behind the codec, set with the volume buttons. A muted
   or zero level keeps the speaker and headphones silent whatever the codec plays: the driver
   unmutes it at the start, as IBM's own driver does. */
static int ec_wait(u8 mask, u8 want)
{
    u32 t0 = plat_ms();
    while ((inb(0x66) & mask) != want)
        if (plat_ms() - t0 > 5) return -1;
    return 0;
}

static int ec_read(u8 reg, u8 *v)
{
    if (ec_wait(2, 0)) return -1;
    outb(0x66, 0x80);
    if (ec_wait(2, 0)) return -1;
    outb(0x62, reg);
    if (ec_wait(1, 1)) return -1;
    *v = inb(0x62);
    return 0;
}

static int ec_write(u8 reg, u8 v)
{
    if (ec_wait(2, 0)) return -1;
    outb(0x66, 0x81);
    if (ec_wait(2, 0)) return -1;
    outb(0x62, reg);
    if (ec_wait(2, 0)) return -1;
    outb(0x62, v);
    return ec_wait(2, 0);
}

static void tp_unmute(void)
{
    u8 v = 0, n;
    u32 f;
    if (cb->opts2 & OPT2_NOTPVOL) return;
    if (inb(0x66) == 0xFF) return;                          /* no embedded controller */
    f = irq_save();
    if (ec_read(0x30, &v) == 0 && ((v & 0x40) || !(v & 0x0F))) {
        n = (u8)((v & ~0x40) | ((v & 0x0F) ? 0 : 0x07));
        if (ec_write(0x30, n) == 0) {
            irq_restore(f);
            kprintf("ac97: ThinkPad volume %u%s -> %u (volume buttons)\n", v & 0x0F, (v & 0x40) ? " muted" : "",
                    n & 0x0F);
            return;
        }
    }
    irq_restore(f);
}

/* ---------------------------------------------------------------- mixer */

/* attenuation register value (1.5 dB steps from the top) of a volume, linear in amplitude */
static u16 att(u32 v, int steps)
{
    u32 a = aeng_att_cdb(v);
    a = a == 0xFFFFFFFFUL ? (u32)steps : (a + 75) / 150;
    return (u16)(a > (u32)steps ? (u32)steps : a);
}

static u16 stereo_att(u32 lr, int steps)
{
    u32 l = lr & 0xFFFF, r = lr >> 16;
    if (!l && !r) return 0x8000;
    return (u16)((att(l, steps) << 8) | att(r, steps));
}

static void set_vol(struct adev *a, int ctl, u32 lr)
{
    struct ac97 *c = a->priv;
    switch (ctl) {
    case MX_MASTER:
        cwr(c, AC_MASTER, stereo_att(lr, c->mbits));
        cwr(c, AC_HP, stereo_att(lr, c->hbits));
        cwr(c, AC_MONO, (u16)(lr ? att(((lr & 0xFFFF) + (lr >> 16)) / 2, c->mbits) : 0x8000));
        break;
    case MX_INGAIN:
    {
        u32 l = (lr & 0xFFFF) * 15 / 65535, r = (lr >> 16) * 15 / 65535;
        cwr(c, AC_RECGAIN, (u16)((l << 8) | r));
        break;
    }
    case MX_CD:
        cwr(c, AC_CD, stereo_att(lr, 31));
        break;
    case MX_LINE:
        cwr(c, AC_LINE, stereo_att(lr, 31));
        break;
    }
}

static void set_src(struct adev *a, int src)
{
    struct ac97 *c = a->priv;
    u16 v = src == AS_MIC ? 0 : src == AS_CD ? 1 : src == AS_AUX ? 3 : src == AS_LINE ? 4 : 5;
    cwr(c, AC_RECSEL, (u16)((v << 8) | v));
    cwr(c, AC_MIC, src == AS_MIC ? 0x8048 : 0x8008);        /* +20 dB boost while recording from it */
}

static int steps_of(struct ac97 *c, int reg)
{
    u16 v;
    cwr(c, reg, 0x2020);
    v = crd(c, reg);
    return (v & 0x20) ? 63 : 31;
}

static int codec_init(struct ac97 *c)
{
    u32 t0;
    u16 ext, v1, v2;
    cwr(c, CR_RESET, 0);
    t0 = plat_ms();
    while ((crd(c, AC_POWER) & 0x0F) != 0x0F)
        if (plat_ms() - t0 > 1000) break;
    v1 = crd(c, AC_VID1);
    v2 = crd(c, AC_VID2);
    if ((v1 == 0xFFFF && v2 == 0xFFFF) || (!v1 && !v2)) return -1;
    cwr(c, AC_MASTER, 0x8808);
    if (!(crd(c, AC_MASTER) & 0x8000)) return -1;            /* a modem codec has no mixer */
    c->a.vid = v1;
    c->a.pid = v2;
    /* external amplifier on (EAPD = 0), everything powered */
    cwr(c, AC_POWER, (u16)((cb->opts & OPT_INVEAPD) ? 0x8000 : 0));
    t0 = plat_ms();
    while ((crd(c, AC_POWER) & 0x0F) != 0x0F && plat_ms() - t0 < 500) ;
    if (v1 == 0x4352 && (v2 & 0xFF00) == 0x5900)
        cwr(c, 0x5E, 0x0080);       /* Cirrus Logic CS4297A..CS4299, CS420x: AC mode (as Linux) */
    ext = crd(c, AC_EXTID);
    c->vra = (u8)(ext & 1);
    if (c->vra) {
        cwr(c, AC_EXTCTL, (u16)(crd(c, AC_EXTCTL) | 1));
        cwr(c, AC_DACRATE, 48000);
        cwr(c, AC_ADCRATE, 48000);
    }
    c->mbits = (u8)steps_of(c, AC_MASTER);
    c->hbits = (u8)steps_of(c, AC_HP);
    cwr(c, AC_PCM, 0x0808);                                 /* 0 dB */
    cwr(c, AC_AUX, 0x8808);
    cwr(c, AC_RECGAIN, 0x0808);
    set_vol(&c->a, MX_CD, 0xC000C000UL);
    set_vol(&c->a, MX_LINE, 0x8000);                        /* line-in pass-through off */
    cwr(c, AC_LINE, 0x8808);
    return 0;
}

/* ---------------------------------------------------------------- ICH bus master */

static int chan(int dir)
{
    return dir ? PI : PO;
}

static int ich_xsr(struct ac97 *c)
{
    return c->kind == K_SIS ? PICB : XSR;
}

static int ich_picb(struct ac97 *c)
{
    return c->kind == K_SIS ? XSR : PICB;
}

static void ich_reset_chan(struct ac97 *c, int base)
{
    u32 t0;
    outb((u16)(c->nabm + base + XCR), 0);
    t0 = plat_ms();
    while (!(inw((u16)(c->nabm + base + ich_xsr(c))) & 1) && plat_ms() - t0 < 10) ;
    outb((u16)(c->nabm + base + XCR), 2);
    t0 = plat_ms();
    while ((inb((u16)(c->nabm + base + XCR)) & 2) && plat_ms() - t0 < 10) ;
    outw((u16)(c->nabm + base + ich_xsr(c)), 0x1C);
}

static int ich_start(struct adev *a, int dir)
{
    struct ac97 *c = a->priv;
    struct ahw *h = dir ? &a->in : &a->out;
    int base = chan(dir), i;
    u32 *b = c->bdl[dir];
    u32 cnt = c->kind == K_SIS ? h->period : h->period / 2;
    ich_reset_chan(c, base);
    for (i = 0; i < NBDL; i++) {
        b[i * 2] = vphys(h->buf + (u32)i * h->period);
        b[i * 2 + 1] = cnt;
    }
    if (!h->noflush) aeng_flush_dma(b, NBDL * 8);
    outl((u16)(c->nabm + base + BDBAR), vphys(b));
    outb((u16)(c->nabm + base + LVI), NBDL - 1);
    outb((u16)(c->nabm + base + XCR), 1);
    return 0;
}

static void ich_stop(struct adev *a, int dir)
{
    ich_reset_chan(a->priv, chan(dir));
}

static u32 ich_pos(struct adev *a, int dir)
{
    struct ac97 *c = a->priv;
    struct ahw *h = dir ? &a->in : &a->out;
    int base = chan(dir), k;
    u32 civ = 0, picb = 0, bytes;
    for (k = 0; k < 10; k++) {
        civ = inb((u16)(c->nabm + base + CIV)) & 31;
        picb = inw((u16)(c->nabm + base + ich_picb(c)));
        if ((inb((u16)(c->nabm + base + CIV)) & 31) == civ) break;
    }
    /* keep the last valid index one behind: the ring never runs dry */
    outb((u16)(c->nabm + base + LVI), (u8)((civ + NBDL - 1) & (NBDL - 1)));
    if (inw((u16)(c->nabm + base + ich_xsr(c))) & 1) {          /* halted (we were late) */
        outw((u16)(c->nabm + base + ich_xsr(c)), 0x1C);
        outb((u16)(c->nabm + base + XCR), 1);
    }
    bytes = c->kind == K_SIS ? picb : picb * 2;
    if (bytes > h->period) bytes = h->period;
    return civ * h->period + (h->period - bytes);
}

/* ---------------------------------------------------------------- VIA SGD */

static int via_chan(struct ac97 *c, int dir)
{
    if (c->kind == K_VIA) return dir ? 0x10 : 0x00;
    return dir ? 0x60 : 0x00;
}

static void via_halt(struct ac97 *c, int base)
{
    u32 t0 = plat_ms();
    outb((u16)(c->nam + base + 1), 0x40);                   /* terminate */
    while ((inb((u16)(c->nam + base)) & 0x80) && plat_ms() - t0 < 10) ;
    outb((u16)(c->nam + base + 1), 0);
    outb((u16)(c->nam + base), 0x03);
    if (c->kind == K_VIA) outb((u16)(c->nam + base + 2), 0);
}

static int via_start(struct adev *a, int dir)
{
    struct ac97 *c = a->priv;
    struct ahw *h = dir ? &a->in : &a->out;
    int base = via_chan(c, dir), i;
    u32 *b = c->bdl[dir];
    via_halt(c, base);
    for (i = 0; i < NBDL; i++) {
        b[i * 2] = vphys(h->buf + (u32)i * h->period);
        b[i * 2 + 1] = h->period | (i == NBDL - 1 ? 0x80000000UL : 0);
    }
    if (!h->noflush) aeng_flush_dma(b, NBDL * 8);
    outl((u16)(c->nam + base + 4), vphys(b));
    if (c->kind == K_VIA) {
        outb((u16)(c->nam + base + 2), 0xB0);               /* autostart, 16 bit, stereo */
        outb((u16)(c->nam + base + 1), 0x80);
    } else {
        if (!dir) {
            outb((u16)(c->nam + base + 2), 0);              /* SDX volume 0 dB */
            outb((u16)(c->nam + base + 3), 0);
        }
        outl((u16)(c->nam + base + 8), 0xFF000000UL | 0x00300000UL | (dir ? 0 : 0xFFFFFUL));
        outb((u16)(c->nam + base + 1), 0xA0);               /* start, autostart */
    }
    return 0;
}

static void via_stop(struct adev *a, int dir)
{
    struct ac97 *c = a->priv;
    via_halt(c, via_chan(c, dir));
}

static u32 via_pos(struct adev *a, int dir)
{
    struct ac97 *c = a->priv;
    struct ahw *h = dir ? &a->in : &a->out;
    int base = via_chan(c, dir);
    u32 ptr = inl((u16)(c->nam + base + 4)), cnt = inl((u16)(c->nam + base + 0x0C)), idx, left;
    if (c->kind == K_VIA8) {
        idx = cnt >> 24;
    } else {
        idx = (ptr - vphys(c->bdl[dir])) / 8;
        idx = (idx + NBDL - 1) % NBDL;                      /* the pointer is past the current entry */
    }
    left = cnt & 0xFFFFFFUL;
    if (left > h->period) left = h->period;
    return (idx % NBDL) * h->period + (h->period - left);
}

static void ac97_shutdown(struct adev *a)
{
    (void)a;
}

static struct adevops ich_ops = { ich_start, ich_stop, ich_pos, set_vol, set_src, NULL, ac97_shutdown };
static struct adevops via_ops = { via_start, via_stop, via_pos, set_vol, set_src, NULL, ac97_shutdown };

/* ---------------------------------------------------------------- probe */

static int kind_of(struct pcidev *p)
{
    static const u16 intel[] = { 0x2415, 0x2425, 0x2445, 0x2485, 0x24C5, 0x24D5, 0x25A6, 0x266E, 0x27DE,
                                 0x2698, 0x7195, 0 };
    static const u16 nv[] = { 0x01B1, 0x006A, 0x00DA, 0x00EA, 0x008A, 0x0059, 0x003A, 0x0068, 0x026B, 0 };
    int i;
    switch (p->vendor) {
    case 0x8086:
        for (i = 0; intel[i]; i++) if (intel[i] == p->device) return K_ICH;
        break;
    case 0x10DE:
        for (i = 0; nv[i]; i++) if (nv[i] == p->device) return K_NV;
        break;
    case 0x1022:
        if (p->device == 0x746D || p->device == 0x7445) return K_ICH;
        break;
    case 0x1039:
        if (p->device == 0x7012) return K_SIS;
        break;
    case 0x1106:
        if (p->device == 0x3058) return K_VIA;
        if (p->device == 0x3059) return K_VIA8;
        break;
    }
    /* another 04/01 function with the ICH register layout (two I/O BARs) */
    if (p->cls == 4 && p->sub == 1 && !(cb->opts & OPT_NOGENERIC) && pci_bar_io(p, 0) && pci_bar_io(p, 1))
        return K_ICH;
    return -1;
}

/* AC-link cold reset (as the Linux driver does: a codec the firmware powered down may look
   ready and still not answer), then the primary codec or, on boards that wire the audio
   codec as the secondary one, the secondary codec. The ThinkPad R32 takes a warm reset. */
static int ich_link(struct ac97 *c)
{
    u32 t0, cnt = inl((u16)(c->nabm + GLOB_CNT)), st;
    int warm = c->pci.vendor == 0x8086 && pci_rd16(c->pci.bus, c->pci.dev, c->pci.fn, 0x2C) == 0x1014 &&
               pci_rd16(c->pci.bus, c->pci.dev, c->pci.fn, 0x2E) == 0x051F;
    /* bus masters still running from before a warm restart (DOS, Windows) stop first */
    ich_reset_chan(c, PI);
    ich_reset_chan(c, PO);
    ich_reset_chan(c, 0x20);
    outl((u16)(c->nabm + GLOB_STA), inl((u16)(c->nabm + GLOB_STA)) & 0x00000F00UL);    /* status bits */
    cnt &= ~0x00300000UL & ~8UL;                        /* 2 channels, AC-link on */
    if (c->kind == K_SIS || c->kind == K_NV || warm) {
        if (!(cnt & 2)) outl((u16)(c->nabm + GLOB_CNT), cnt | 2);
        else outl((u16)(c->nabm + GLOB_CNT), cnt | 4);  /* warm reset */
    } else {
        outl((u16)(c->nabm + GLOB_CNT), cnt & ~6UL);     /* cold reset asserted */
        udelay(20);
        outl((u16)(c->nabm + GLOB_CNT), (cnt & ~4UL) | 2);
        mdelay(1);
    }
    t0 = plat_ms();
    while ((inl((u16)(c->nabm + GLOB_CNT)) & 4) && plat_ms() - t0 < 250) ;
    t0 = plat_ms();
    while (!(inl((u16)(c->nabm + GLOB_STA)) & 0x300) && plat_ms() - t0 < 1000) ;
    t0 = plat_ms();
    while ((inl((u16)(c->nabm + GLOB_STA)) & 0x300) != 0x300 && plat_ms() - t0 < 50) ;   /* the other one */
    if (c->kind == K_SIS) outw((u16)(c->nabm + 0x4C), (u16)(inw((u16)(c->nabm + 0x4C)) | 1));
    st = inl((u16)(c->nabm + GLOB_STA));
    c->coff = 0;
    if (!(st & 0x100) && (st & 0x200) && c->kind == K_ICH) c->coff = 0x80;
    if (!(st & 0x300) && c->kind == K_ICH) {
        /* some codecs (Cirrus Logic CS4299 of the ThinkPad T23) answer although the ready bit
           never comes: the vendor ID read decides */
        kprintf("ac97: no codec ready bit (GLOB_STA %08lx), trying anyway\n", st);
        return 0;
    }
    return (st & 0x300) ? 0 : -1;
}

static int via_link(struct ac97 *c)
{
    struct pcidev *p = &c->pci;
    u32 t0;
    if (!(pci_rd8(p->bus, p->dev, p->fn, 0x40) & 1)) {
        pci_wr8(p->bus, p->dev, p->fn, 0x41, 0xE0);
        udelay(100);
        pci_wr8(p->bus, p->dev, p->fn, 0x41, 0x60);
        udelay(2);
        pci_wr8(p->bus, p->dev, p->fn, 0x41, 0xCC);
        udelay(100);
    }
    if ((pci_rd8(p->bus, p->dev, p->fn, 0x41) & 0xCC) != 0xCC) {
        pci_wr8(p->bus, p->dev, p->fn, 0x41, 0xCC);
        udelay(100);
    }
    t0 = plat_ms();
    while (!(pci_rd8(p->bus, p->dev, p->fn, 0x40) & 1))
        if (plat_ms() - t0 > 750) return -1;
    return 0;
}

static void alloc_ring(struct ahw *h, u8 *buf)
{
    memset(h, 0, sizeof(*h));
    h->buf = buf;
    h->period = plat_mode == PM_DOS ? PERIOD * 2 : PERIOD;     /* DOS: polled every 55 ms */
    h->size = h->period * NBDL;
    h->rate = 48000;
    h->present = 1;
    h->noflush = 1;         /* PCI bus masters of this generation snoop the CPU caches */
}

int ac97_probe(struct pcidev *p)
{
    struct ac97 *c;
    int kind = kind_of(p);
    static const char *const kn[] = { "Intel ICH compatible", "SiS 7012", "nVidia nForce", "VIA 82C686",
                                      "VIA VT8233" };
    int i;
    if (kind < 0) return -1;
    /* the DOS core probes again after Windows: same controller, same memory */
    for (i = 0; i < nctrl && PCI_BDF(&ctrls[i]->pci) != PCI_BDF(p); i++) ;
    if (i < nctrl) {
        c = ctrls[i];
        c->a.gone = 0;
    } else {
        if (nctrl >= MAXAC) return -1;
        c = kmalloc(sizeof(*c));
        memset(c, 0, sizeof(*c));
        c->ringbuf[0] = kmalloc_align(RINGSIZE, 4096);
        c->ringbuf[1] = kmalloc_align(RINGSIZE, 4096);
        c->bdl[0] = kmalloc_align(NBDL * 8, 256);
        c->bdl[1] = kmalloc_align(NBDL * 8, 256);
        if (!c->ringbuf[1] || !c->bdl[1]) return -1;
        ctrls[nctrl++] = c;
    }
    c->pci = *p;
    c->kind = kind;
    if (p->vendor == 0x8086 && p->device >= 0x24C5 && p->device != 0x7195)
        pci_wr8(p->bus, p->dev, p->fn, 0x41, (u8)(pci_rd8(p->bus, p->dev, p->fn, 0x41) | 1));  /* ICH4+: IOSE */
    pci_enable(p, 0, 1, 1);
    c->nam = (u16)pci_bar_io(p, 0);
    c->nabm = (u16)pci_bar_io(p, 1);
    if (!c->nam || (kind < K_VIA && !c->nabm)) return -1;
    if (kind >= K_VIA ? via_link(c) : ich_link(c)) {
        kprintf("ac97: %02x:%02x.%u %s: codec not ready\n", p->bus, p->dev, p->fn, kn[kind]);
        return -1;
    }
    if (codec_init(c)) {
        if (c->coff || kind != K_ICH || !(inl((u16)(c->nabm + GLOB_STA)) & 0x200)) {
            kprintf("ac97: %02x:%02x.%u no codec\n", p->bus, p->dev, p->fn);
            return -1;
        }
        c->coff = 0x80;             /* the primary is a modem codec or absent */
        if (codec_init(c)) {
            kprintf("ac97: %02x:%02x.%u no audio codec\n", p->bus, p->dev, p->fn);
            return -1;
        }
    }
    if (c->coff) kprintf("ac97: %02x:%02x.%u audio codec is the secondary codec\n", p->bus, p->dev, p->fn);
    if (pci_rd16(p->bus, p->dev, p->fn, 0x2C) == 0x1014) tp_unmute();
    alloc_ring(&c->a.out, c->ringbuf[0]);
    alloc_ring(&c->a.in, c->ringbuf[1]);
    c->a.ops = kind >= K_VIA ? &via_ops : &ich_ops;
    c->a.priv = c;
    c->a.type = AT_AC97;
    c->a.caps = WINAC_OUT | WINAC_IN;
    c->a.insrcs = AS_MIC | AS_LINE | AS_CD | AS_AUX | AS_MIX;
    c->a.insrc = AS_MIC;
    c->a.hwvol = 1;
    set_src(&c->a, AS_MIC);
    kprintf("ac97: %02x:%02x.%u %s, codec %04x%04x, %s, master %d steps\n", p->bus, p->dev, p->fn, kn[kind],
            c->a.vid, c->a.pid, c->vra ? "VRA" : "48 kHz", c->mbits);
    ksnprintf(c->a.name, sizeof(c->a.name), "AC'97 %s", kn[kind]);
    c->a.irq = c->pci.irq;
    c->a.pcivid = c->pci.vendor;
    c->a.pcidid = c->pci.device;
    c->a.loc = (u16)((c->pci.bus << 8) | (c->pci.dev << 3) | c->pci.fn);
    adev_register(&c->a);
    return 0;
}

void ac97_remove(u32 bdf)
{
    int i;
    for (i = 0; i < nctrl; i++)
        if (PCI_BDF(&ctrls[i]->pci) == bdf) aeng_remove(&ctrls[i]->a);
}
