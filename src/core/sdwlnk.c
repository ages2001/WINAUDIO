/* sdwlnk.c - SoundWire on Intel: the links (managers) and the Cadence manager IP.
   - cAVS (Cannon Lake .. Raptor Lake) and ACE 1.x (Meteor / Arrow Lake): registers in the
     DSP BAR; one shim for all links (power, sync, IO control, PCM channel maps), the ALH
     (audio link hub) stream configuration, Cadence IP at 0x30000 + 0x10000 * link.
   - ACE 2.x+ (Lunar Lake and later): registers in the HD Audio BAR at the pointer of the
     SoundWire extended link of the HD Audio multi-link capability; the links are its
     sublinks (power, sync); each link has a shim, the Cadence IP (+0x100) and a vendor
     shim (+0x6000), its PCM data interfaces go to HD Audio link DMA (stream channel map).
   Everything is polled: the command FIFO answers are counted in the FIFO status. */
#include "sdwint.h"

struct sdwctl sdwc;

/* ---- shim (cAVS / ACE1), offsets from the shim base ---- */
#define SHIM_CAVS       0x2C000UL
#define ALH_CAVS        0x2C800UL
#define SHIM_ACE        0x38000UL
#define ALH_ACE         0x24000UL
#define LINK_BASE       0x30000UL
#define LINK_SIZE       0x10000UL
#define SH_LCAP         0x00
#define SH_LCTL         0x04
#define SH_SYNC         0x0C
#define SYNC_CPU        (1UL << 15)
#define SYNC_CMDSYNC(l) (1UL << (16 + (l)))
#define SYNC_GO         (1UL << 24)
#define SH_PCMSCAP(x)       (0x020 + 0x60 * (x))
#define SH_PCMSYCHM(x, y)   (0x022 + 0x60 * (x) + 2 * (y))
#define SH_PCMSYCHC(x, y)   (0x042 + 0x60 * (x) + 2 * (y))
#define SH_IOCTL(x)         (0x06C + 0x60 * (x))
#define SH_CTMCTL(x)        (0x06E + 0x60 * (x))
#define IO_MIF          0x01
#define IO_COE          0x04
#define IO_DO           0x08
#define IO_DOE          0x10
#define IO_BKE          0x20
#define IO_WPDD         0x40
#define CT_DACTQE       0x01
#define CT_DODS         0x02
#define CHM_DIR         0x8000

/* ---- ACE2: HD Audio multi-link SoundWire link, link shims ---- */
#define ML_LCAP         0x00
#define ML_LCTL         0x04
#define ML_LOSIDV       0x08
#define ML_LSYNC        0x1C
#define ML_LEPTR        0x20
#define LSYNC_SYNCPU    (1UL << 20)
#define LSYNC_GO        (1UL << 23)
#define LSYNC_CMDSYNC(l) (1UL << (24 + (l)))
#define S2_PCMSCAP      0x10
#define S2_PCMSYCHC(y)  (0x14 + 4 * (y))
#define S2_PCMSYCHM(y)  (0x16 + 4 * (y))
#define VS_LVSCTL       0x04
#define VS_ACTMCTL      0x0E

/* ---- Cadence manager ---- */
#define MCP_CONFIG          0x00
#define MCP_CONTROL         0x04
#define MCP_CMDCTRL         0x08
#define MCP_FRAME_SHAPE_INIT 0x14
#define MCP_CONFIG_UPDATE   0x18
#define MCP_SSP_CTRL0       0x20
#define MCP_SSP_CTRL1       0x28
#define MCP_CLK_CTRL0       0x30
#define MCP_CLK_CTRL1       0x38
#define MCP_STAT            0x40
#define MCP_INTSTAT         0x44
#define MCP_INTMASK         0x48
#define MCP_SLAVE_STAT      0x50
#define MCP_SLAVE_INTSTAT0  0x54
#define MCP_SLAVE_INTSTAT1  0x58
#define MCP_SLAVE_INTMASK0  0x5C
#define MCP_SLAVE_INTMASK1  0x60
#define MCP_FIFOLEVEL       0x78
#define MCP_FIFOSTAT        0x7C
#define MCP_CMD_BASE        0x80        /* + IP offset */
#define CFG_BUS_REL         (1UL << 6)
#define IPCFG_MMASTER       (1UL << 7)
#define IPCFG_SNIFFER       (1UL << 5)
#define IPCFG_CMD           (1UL << 3)
#define IPCFG_OP            7UL
#define CTL_CMD_RST         (1UL << 7)
#define CTL_SOFT_RST        (1UL << 6)
#define CTL_HW_RST          (1UL << 4)
#define CTL_CLK_STOP_CLR    (1UL << 2)
#define IPCTL_SW_RST        (1UL << 5)
#define IPCTL_CMD_ACCEPT    (1UL << 1)
#define IPCTL_BLOCK_WAKEUP  1UL
#define STAT_ACTIVE_BANK    (1UL << 20)
#define INT_IRQ             (1UL << 31)
#define INT_SLAVE_MASK      (0xFUL << 12)
#define INT_CTRL_CLASH      (1UL << 10)
#define INT_DATA_CLASH      (1UL << 9)
#define INT_PARITY          (1UL << 8)
#define INT_RX_WL           (1UL << 2)
#define CMD_SSP_TAG         (1UL << 31)
#define CMD_READ            2UL
#define CMD_WRITE           3UL
#define RSP_ACK             1UL
#define RSP_NACK            2UL
#define DPN_BASE(n, b)      (((b) ? 0x118UL : 0x100UL) + 0x80UL * (n))
#define DPN_CONFIG          0x00
#define DPN_CH_EN           0x04
#define DPN_SAMPLE_CTRL     0x08
#define DPN_OFFSET_CTRL     0x0C
#define DPN_HCTRL           0x10
#define PORTCTRL(n)         (0x130UL + 0x80UL * (n))
#define PORTCTRL_DIRN       (1UL << 7)
#define PORTCTRL_TEST_FAILED (1UL << 1)
#define PORTCTRL_BULK       (1UL << 16)
#define PDI_CONFIG(n)       (0x1100UL + 16UL * (n))
#define PDI_SOFT_RESET      (1UL << 24)

#define FRAME_GSYNC_HZ      4000UL

static u32 crd(struct sdwlink *l, u32 o) { return RD32(l->cdns + o); }
static void cwr(struct sdwlink *l, u32 o, u32 v) { WR32(l->cdns + o, v); }
static void cupd(struct sdwlink *l, u32 o, u32 mask, u32 v) { cwr(l, o, (crd(l, o) & ~mask) | v); }

int sdw_wait32(u8 *p, u32 mask, u32 val, u32 ms)
{
    u32 t0 = plat_ms();
    for (;;) {
        u32 v = RD32(p);
        if (v != 0xFFFFFFFFUL && (v & mask) == val) return 0;
        if (plat_ms() - t0 > ms) return -1;
        udelay(10);
    }
}

/* ---------------------------------------------------------------- discovery */

int lnk_probe(struct dsp *d)
{
    struct sdwctl *c = &sdwc;
    int i, n = 0;
    memset(c->link, 0, sizeof(c->link));
    c->d = d;
    c->lmask = 0;
    c->lt = d->pl->gen >= DG_LNL ? LT_ACE2 : d->pl->gen == DG_MTL ? LT_ACE1 : LT_CAVS;
    if (c->lt != LT_ACE2) {
        u8 *shim = d->bar + (c->lt == LT_ACE1 ? SHIM_ACE : SHIM_CAVS);
        u8 *alh = d->bar + (c->lt == LT_ACE1 ? ALH_ACE : ALH_CAVS);
        u32 lcap = RD32(shim + SH_LCAP);
        if (lcap == 0xFFFFFFFFUL) return 0;
        n = (int)(lcap & 7);
        c->mlcs = (u8)((lcap >> 8) & 1);
        for (i = 0; i < n && i < SDW_MAXLINK; i++) {
            c->link[i].cdns = d->bar + LINK_BASE + LINK_SIZE * (u32)i;
            c->link[i].shim = shim;
            c->link[i].alh = alh;
        }
    } else {
        u32 cnt, lcap, leptr, phys, ptr;
        if (!d->ml) return 0;
        cnt = RD32(d->ml + 4) + 1;
        for (i = 0; i < (int)cnt && i < 16; i++) {
            u8 *m = d->ml + 0x40 + 0x40 * i;
            lcap = RD32(m + ML_LCAP);
            if (lcap == 0xFFFFFFFFUL || !(lcap & (1UL << 28))) continue;     /* alternate link */
            leptr = RD32(m + ML_LEPTR);
            if ((leptr >> 24) != 0) continue;                                 /* SoundWire */
            c->eml = m;
            c->lss = (u8)((lcap >> 23) & 1);
            n = (int)((lcap >> 20) & 7) + 1;
            ptr = leptr & 0xFFFFFUL;
            if (!ptr) ptr = 0x30000UL;
            phys = pci_bar_mem(&d->pci, 0, NULL);
            if (n > SDW_MAXLINK) n = SDW_MAXLINK;
            c->base = phys ? plat_mmio(phys + ptr, 0x8000UL * (u32)n) : NULL;
            if (!c->base) return 0;
            DBG("sdw: multi-link %d: SoundWire, %d sublinks at %05lx%s\n", i, n, ptr, c->lss ? ", sync" : "");
            break;
        }
        for (i = 0; i < n; i++) {
            u8 *b = c->base + 0x8000UL * (u32)i;
            c->link[i].shim = b;
            c->link[i].cdns = b + 0x100;
            c->link[i].vs = b + 0x6000;
            c->link[i].ipoff = 0x4000;
        }
    }
    if (n > SDW_MAXLINK) n = SDW_MAXLINK;
    for (i = 0; i < n; i++) {
        c->link[i].d = d;
        c->link[i].id = (u8)i;
        c->link[i].lt = c->lt;
    }
    c->nlink = (u8)n;
    /* links left powered (an earlier start of the driver): off, the setup starts clean */
    if (n && c->lt == LT_ACE2) {
        u32 m = ((1UL << n) - 1), v = RD32(c->eml + ML_LCTL);
        if (v & (m << 16)) {
            WR32(c->eml + ML_LCTL, v & ~(m << 16));
            sdw_wait32(c->eml + ML_LCTL, m << 23, 0, 5);
        }
    } else if (n) {
        u8 *shim = c->link[0].shim;
        u32 v = RD32(shim + SH_LCTL);
        if (v & 0xF) {
            for (i = 0; i < n && i < 4; i++)
                if (v & (1UL << i)) {
                    u8 *io = shim + SH_IOCTL(i);
                    WR16(io, (u16)(RD16(io) | IO_BKE | IO_COE));
                    udelay(15);
                    WR16(io, (u16)(RD16(io) & ~IO_MIF));
                    udelay(15);
                }
            WR32(shim + SH_LCTL, v & ~0xFUL);
            sdw_wait32(shim + SH_LCTL, 0xF00, 0, 5);
        }
    }
    return n;
}

/* ---------------------------------------------------------------- power */

/* sync period of the 4 kHz SSP counter and the link clock source for the IP clock */
static int clk_sel(struct sdwlink *l, u32 *syncprd, u32 *src)
{
    u32 m = l->mclk;
    int sel = l->lt == LT_ACE2 || sdwc.mlcs;
    if (m % 6000000UL) {
        if (m % 2400000UL) {
            if (!sel) return -1;
            *syncprd = 24576 / 4 - 1;
            *src = 1;                                   /* cardinal clock */
        } else {
            *syncprd = 38400 / 4 - 1;
            *src = 0;                                   /* crystal */
        }
    } else if (sel) {
        *syncprd = 96000 / 4 - 1;
        *src = 2;                                       /* audio PLL */
    } else {
        *syncprd = 24000 / 4 - 1;
        *src = 0;
    }
    return 0;
}

/* cAVS / ACE1: IO control and timing of the link, the master IP takes the pins */
static void shim_init(struct sdwlink *l)
{
    u8 *io = l->shim + SH_IOCTL(l->id), *ct = l->shim + SH_CTMCTL(l->id);
    u16 v = IO_BKE, a;
    WR16(io, v);
    udelay(15);
    v |= IO_WPDD;
    WR16(io, v);
    udelay(15);
    v |= IO_DO;
    WR16(io, v);
    udelay(15);
    v |= IO_DOE;
    WR16(io, v);
    udelay(15);
    /* glue logic to master IP */
    v = RD16(io);
    v &= (u16)~IO_DOE;
    WR16(io, v);
    udelay(15);
    v &= (u16)~IO_DO;
    WR16(io, v);
    udelay(15);
    v |= IO_MIF;
    WR16(io, v);
    udelay(15);
    v &= (u16)~(IO_BKE | IO_COE);
    WR16(io, v);
    udelay(15);
    a = RD16(ct);
    a = (u16)((a & ~0x18) | (1 << 3) | CT_DACTQE | CT_DODS);
    WR16(ct, a);
    udelay(15);
}

/* ACE2: data output timing from the ACPI properties (hardware defaults otherwise) */
static void vs_init(struct sdwlink *l)
{
    u32 doaise = 1, doais = 3, dodse = 0, dods = 1;
    u16 a;
    if (l->subprop[0]) {
        acpi_prop_int(l->subprop, "intel-sdw-doaise", &doaise);
        acpi_prop_int(l->subprop, "intel-sdw-doais", &doais);
        acpi_prop_int(l->subprop, "intel-sdw-dodse", &dodse);
        acpi_prop_int(l->subprop, "intel-sdw-dods", &dods);
    }
    a = RD16(l->vs + VS_ACTMCTL);
    a &= 0x0001;
    a = (u16)(a | ((doaise & 1) << 5) | ((doais & 3) << 3) | ((dodse & 1) << 2) | ((dods & 1) << 1) | 1);
    WR16(l->vs + VS_ACTMCTL, a);
    udelay(15);
}

int lnk_power(struct sdwlink *l, int on)
{
    struct sdwctl *c = &sdwc;
    u32 bit = 1UL << l->id, syncprd = 0, src = 0, v;
    int first = c->lmask == 0;
    if (on) {
        if (clk_sel(l, &syncprd, &src)) {
            kprintf("sdw: link %u: IP clock %lu Hz not usable\n", l->id, l->ipclk);
            return -1;
        }
        if (l->lt == LT_ACE2) {
            u8 *lctl = c->eml + ML_LCTL, *lsync = c->eml + ML_LSYNC;
            WR32(lctl, RD32(lctl) | (1UL << (16 + l->id)));
            udelay(10);
            if (sdw_wait32(lctl, 1UL << (23 + l->id), 1UL << (23 + l->id), 5)) {
                kprintf("sdw: link %u does not power up\n", l->id);
                return -1;
            }
            v = RD32(l->vs + VS_LVSCTL);
            WR32(l->vs + VS_LVSCTL, (v & ~(7UL << 27)) | (src << 27));
            if (first && c->lss) {
                v = RD32(lsync);
                WR32(lsync, (v & ~0xFFFFFUL) | syncprd | LSYNC_SYNCPU);
                if (sdw_wait32(lsync, LSYNC_SYNCPU, 0, 5)) DBG("sdw: SYNCPU stays set\n");
            }
            vs_init(l);
        } else {
            u8 *lctl = l->shim + SH_LCTL, *sync = l->shim + SH_SYNC;
            if (first) {
                v = RD32(sync);
                WR32(sync, (v & ~0x7FFFUL) | syncprd | SYNC_CPU);
            }
            WR32(lctl, RD32(lctl) | bit);
            if (sdw_wait32(lctl, bit << 8, bit << 8, 5)) {
                kprintf("sdw: link %u does not power up\n", l->id);
                return -1;
            }
            if (first) {
                if (sdw_wait32(sync, SYNC_CPU, 0, 5)) DBG("sdw: SYNCCPU stays set\n");
                if (c->mlcs) WR32(lctl, (RD32(lctl) & ~(7UL << 27)) | (src << 27));
            }
            shim_init(l);
        }
        c->lmask |= bit;
        return 0;
    }
    if (!(c->lmask & bit)) return 0;
    c->lmask &= ~bit;
    l->up = 0;
    if (l->lt == LT_ACE2) {
        u8 *lctl = c->eml + ML_LCTL;
        WR32(lctl, RD32(lctl) & ~(1UL << (16 + l->id)));
        return sdw_wait32(lctl, 1UL << (23 + l->id), 0, 5);
    } else {
        u8 *io = l->shim + SH_IOCTL(l->id), *lctl = l->shim + SH_LCTL;
        u16 w = RD16(io);
        /* master IP to glue logic */
        w |= IO_BKE | IO_COE;
        WR16(io, w);
        udelay(15);
        w &= (u16)~IO_MIF;
        WR16(io, w);
        udelay(15);
        WR32(lctl, RD32(lctl) & ~bit);
        return sdw_wait32(lctl, bit << 8, 0, 5);
    }
}

/* ---------------------------------------------------------------- multi-link sync */

static void sync_arm(struct sdwlink *l)
{
    if (l->lt == LT_ACE2) {
        if (sdwc.lss) WR32(sdwc.eml + ML_LSYNC, RD32(sdwc.eml + ML_LSYNC) | LSYNC_CMDSYNC(l->id));
    } else {
        WR32(l->shim + SH_SYNC, RD32(l->shim + SH_SYNC) | SYNC_CMDSYNC(l->id));
    }
}

/* SyncGO when a link is armed: the armed links apply their pending command together */
static void sync_go(void)
{
    struct sdwctl *c = &sdwc;
    if (c->lt == LT_ACE2) {
        u32 mask = ((1UL << c->nlink) - 1) << 24;
        if (c->lss && (RD32(c->eml + ML_LSYNC) & mask))
            WR32(c->eml + ML_LSYNC, RD32(c->eml + ML_LSYNC) | LSYNC_GO);
    } else if (c->nlink) {
        u8 *sync = c->link[0].shim + SH_SYNC;
        if (RD32(sync) & (0xFUL << 16)) WR32(sync, RD32(sync) | SYNC_GO);
    }
}

/* ---------------------------------------------------------------- Cadence IP */

/* commit MCP configuration; wait: until the hardware took it */
static int cfg_update(struct sdwlink *l, int wait)
{
    cwr(l, MCP_CONFIG_UPDATE, 1);
    if (!wait) return 0;
    return sdw_wait32(l->cdns + MCP_CONFIG_UPDATE, 1, 0, 5);
}

static void pdi_caps(struct sdwlink *l)
{
    u16 cap;
    int y, n;
    if (l->lt == LT_ACE2) cap = RD16(l->shim + S2_PCMSCAP);
    else cap = RD16(l->shim + SH_PCMSCAP(l->id));
    n = (cap >> 8) & 0x1F;                              /* bidirectional streams */
    if (n > 16) n = 16;
    l->npdi = (u8)n;
    for (y = 0; y < n; y++) {
        u16 c;
        if (l->lt == LT_ACE2) c = (u16)(RD16(l->shim + S2_PCMSYCHC(y)) & 0xF);
        else c = (u16)(y == 2 ? 7 : RD16(l->shim + SH_PCMSYCHC(l->id, y)) & 0xF);    /* PDI 2 reports 1 */
        l->pdich[y] = (u8)(c + 1);
    }
}

int lnk_start(struct sdwlink *l)
{
    u32 v, t0, div;
    /* soft reset */
    cupd(l, MCP_CONTROL, CTL_SOFT_RST, CTL_SOFT_RST);
    if (cfg_update(l, 1) || sdw_wait32(l->cdns + MCP_CONTROL, CTL_SOFT_RST, 0, 5)) {
        kprintf("sdw: link %u: Cadence IP soft reset timed out\n", l->id);
        return -1;
    }
    sync_arm(l);
    /* clock divider, initial frame shape, SSP interval */
    div = l->mclk / l->busclk - 1;
    cupd(l, MCP_CLK_CTRL0, 0xFF, div);
    cupd(l, MCP_CLK_CTRL1, 0xFF, div);
    cwr(l, MCP_FRAME_SHAPE_INIT, ((u32)l->rowidx << 3) | l->colidx);
    cwr(l, MCP_SSP_CTRL0, SDW_RATE / FRAME_GSYNC_HZ);
    cwr(l, MCP_SSP_CTRL1, SDW_RATE / FRAME_GSYNC_HZ);
    l->msgcount = (u8)crd(l, MCP_FIFOLEVEL);
    cupd(l, MCP_CONTROL, CTL_CMD_RST, CTL_CMD_RST);                 /* flush command FIFOs */
    cupd(l, l->ipoff + MCP_CONTROL, IPCTL_CMD_ACCEPT | IPCTL_BLOCK_WAKEUP, IPCTL_CMD_ACCEPT);
    cupd(l, MCP_CONFIG, CFG_BUS_REL, 0);
    v = crd(l, l->ipoff + MCP_CONFIG);
    v &= ~(IPCFG_OP | IPCFG_CMD | IPCFG_SNIFFER);
    v |= IPCFG_MMASTER;                                             /* multi-manager: gsync */
    cwr(l, l->ipoff + MCP_CONFIG, v);
    cfg_update(l, 0);
    sync_go();
    if (sdw_wait32(l->cdns + MCP_CONFIG_UPDATE, 1, 0, 5)) {
        kprintf("sdw: link %u: configuration update timed out\n", l->id);
        return -1;
    }
    /* status reporting (the controller interrupt itself stays off) */
    cwr(l, MCP_SLAVE_INTSTAT0, crd(l, MCP_SLAVE_INTSTAT0));
    cwr(l, MCP_SLAVE_INTSTAT1, crd(l, MCP_SLAVE_INTSTAT1));
    cwr(l, MCP_SLAVE_INTMASK0, 0xFFFFFFFFUL);
    cwr(l, MCP_SLAVE_INTMASK1, 0xFFFFUL);
    cwr(l, MCP_INTMASK, INT_SLAVE_MASK | INT_CTRL_CLASH | INT_DATA_CLASH | INT_PARITY | INT_RX_WL | INT_IRQ);
    /* exit reset: hardware generated bus reset, then normal operation */
    cupd(l, MCP_CONTROL, CTL_HW_RST, CTL_HW_RST);
    if (cfg_update(l, 1)) {
        kprintf("sdw: link %u: bus reset not committed\n", l->id);
        return -1;
    }
    mdelay(1);
    t0 = plat_ms();
    while (crd(l, MCP_CONTROL) & CTL_HW_RST) {
        if (plat_ms() - t0 > 12) {
            kprintf("sdw: link %u: bus reset does not end\n", l->id);
            return -1;
        }
        udelay(100);
    }
    v = crd(l, MCP_CONTROL);
    if ((v & (CTL_CMD_RST | CTL_SOFT_RST | CTL_CLK_STOP_CLR)) || (crd(l, l->ipoff + MCP_CONTROL) & IPCTL_SW_RST))
        DBG("sdw: link %u: reset bits still set (%08lx)\n", l->id, v);
    l->bank = (u8)((crd(l, MCP_STAT) & STAT_ACTIVE_BANK) ? 1 : 0);
    l->pdiused = 0;
    pdi_caps(l);
    l->up = 1;
    return 0;
}

/* ---------------------------------------------------------------- commands */

static void fifo_level(struct sdwlink *l, int n)
{
    if (l->msgcount != (u8)n) {
        cwr(l, MCP_FIFOLEVEL, (u32)n);
        l->msgcount = (u8)n;
    }
}

static void fifo_put(struct sdwlink *l, const u32 *w, int n)
{
    int i;
    for (i = 0; i < n; i++) cwr(l, l->ipoff + MCP_CMD_BASE + 4UL * (u32)i, w[i]);
}

/* the n answers of the commands in the FIFO; -1 on timeout (answers drained) */
static int fifo_get(struct sdwlink *l, int n, u32 *rsp)
{
    u32 t0 = plat_ms(), avail, r;
    int i, ret = 0;
    for (;;) {
        avail = crd(l, MCP_FIFOSTAT) & 0x3F;
        if (avail >= (u32)n) break;
        if (plat_ms() - t0 > 20) {
            ret = -1;
            break;
        }
        udelay(5);
    }
    if (avail > 10) avail = 10;
    for (i = 0; i < (int)avail; i++) {
        r = crd(l, l->ipoff + MCP_CMD_BASE + 4UL * (u32)i);
        if (i < n) rsp[i] = r;
    }
    cwr(l, MCP_INTSTAT, INT_RX_WL);
    return ret;
}

int lnk_cmds(struct sdwlink *l, int dev, int write, u16 addr, u8 *buf, int n)
{
    u32 w[8], r[8];
    int off, i, k, noack;
    if (!l->up) return SR_FAIL;
    for (off = 0; off < n; off += k) {
        k = n - off > 8 ? 8 : n - off;
        for (i = 0; i < k; i++) {
            w[i] = ((u32)dev << 24) | ((write ? CMD_WRITE : CMD_READ) << 28) |
                   ((u32)(u16)(addr + off + i) << 8) | (write ? buf[off + i] : 0);
            r[i] = 0;
        }
        fifo_level(l, k);
        fifo_put(l, w, k);
        if (fifo_get(l, k, r)) {
            DBG("sdw: link %u dev %d %04x: command timeout\n", l->id, dev, addr);
            return SR_FAIL;
        }
        noack = 0;
        for (i = 0; i < k; i++) {
            if (r[i] & RSP_NACK) {
                DBG("sdw: link %u dev %d %04x: NAK\n", l->id, dev, addr + off + i);
                return SR_FAIL;
            }
            if (!(r[i] & RSP_ACK)) noack = 1;
        }
        if (noack) return SR_IGNORED;
        if (!write)
            for (i = 0; i < k; i++) buf[off + i] = (u8)(r[i] >> 8);
    }
    return SR_OK;
}

u32 lnk_status(struct sdwlink *l)
{
    return l->up ? crd(l, MCP_SLAVE_STAT) : 0;
}

/* bank switch on the links together: FrameCtrl of the next bank broadcast at the next SSP
   of every link (multi-link sync), then both banks swap roles */
int lnk_bank_switch(struct sdwlink **ls, int n)
{
    int i, ret = 0;
    u32 r;
    for (i = 0; i < n; i++) {
        struct sdwlink *l = ls[i];
        u32 w = CMD_SSP_TAG | (15UL << 24) | (CMD_WRITE << 28) | ((l->bank ? 0x60UL : 0x70UL) << 8) |
                (((u32)l->rowidx << 3) | l->colidx);
        if (!l->up) continue;
        sync_arm(l);
        fifo_level(l, 1);
        fifo_put(l, &w, 1);
    }
    sync_go();
    for (i = 0; i < n; i++) {
        struct sdwlink *l = ls[i];
        if (!l->up) continue;
        r = 0;
        if (fifo_get(l, 1, &r)) {
            kprintf("sdw: link %u: bank switch timed out\n", l->id);
            mdelay(1);
            l->bank = (u8)((crd(l, MCP_STAT) & STAT_ACTIVE_BANK) ? 1 : 0);
            ret = -1;
            continue;
        }
        if (r & RSP_NACK) DBG("sdw: link %u: bank switch NAK\n", l->id);
        l->bank ^= 1;
    }
    return ret;
}

/* ---------------------------------------------------------------- data ports */

/* the free PDI with the fewest channels that are enough (PDI 0 and 1: bulk transfers) */
int lnk_pdi_get(struct sdwlink *l, int ch)
{
    int y, best = -1;
    for (y = 2; y < l->npdi; y++)
        if (!(l->pdiused & (1U << y)) && l->pdich[y] >= ch && (best < 0 || l->pdich[y] < l->pdich[best])) best = y;
    if (best >= 0) l->pdiused |= (u16)(1U << best);
    return best;
}

void lnk_pdi_put(struct sdwlink *l, int pdi)
{
    if (pdi > 0) l->pdiused &= (u16)~(1U << pdi);
}

/* ALH stream of a PDI (bulk and PCM streams are not contiguous) */
u32 lnk_alh_id(struct sdwlink *l, int pdi)
{
    return (u32)l->id * 16 + (u32)pdi + 3 + (pdi >= 2 ? 2 : 0);
}

/* PDI of a stream part: shim channel map and ALH stream (cAVS / ACE1), Cadence port
   direction and PDI to port mapping */
void lnk_pdi_setup(struct sdwlink *l, struct sdwpe *pe)
{
    int dir = pe->st->dir, pdi = pe->pdi, ch = pe->ch;
    u32 alh = lnk_alh_id(l, pdi), v;
    if (l->lt != LT_ACE2) {
        u16 chm = (u16)((dir ? CHM_DIR : 0) | ((alh & 0x3F) << 8) | ((u32)(ch - 1) << 4));
        WR16(l->shim + SH_PCMSYCHM(l->id, pdi), chm);
        v = RD32(l->alh + 4 * alh);
        v = (v & ~0x000F00FFUL) | 3 | ((u32)(ch - 1) << 16);   /* DMA type, channels */
        WR32(l->alh + 4 * alh, v);
    }
    cupd(l, PORTCTRL(pdi), PORTCTRL_DIRN | PORTCTRL_TEST_FAILED | PORTCTRL_BULK, dir ? PORTCTRL_DIRN : 0);
    cwr(l, PDI_CONFIG(pdi), (u32)pdi | PDI_SOFT_RESET | (((1UL << ch) - 1) << 8));
}

/* manager data port of a stream part in a bank: word length, transport, channels */
void lnk_dpn(struct sdwlink *l, struct sdwpe *pe, int bank)
{
    u32 b = DPN_BASE(pe->pdi, bank), v;
    v = crd(l, b + DPN_CONFIG);
    v &= ~((1UL << 18) | (3UL << 16) | (0x1FUL << 8) | 0xFUL);     /* BPM BGC WL data mode flow */
    v |= (u32)(SDW_BPS - 1) << 8;
    cwr(l, b + DPN_CONFIG, v);
    cwr(l, b + DPN_OFFSET_CTRL, (u32)(pe->bo & 0xFF) | ((u32)(pe->bo >> 8) << 8));
    cwr(l, b + DPN_HCTRL, ((u32)pe->hstart << 4) | pe->hstop);
    cwr(l, b + DPN_SAMPLE_CTRL, (u32)l->rows * l->cols - 1);
    cwr(l, b + DPN_CH_EN, pe->state == PE_ON ? (1UL << pe->ch) - 1 : 0);
}

void lnk_dpn_off(struct sdwlink *l, struct sdwpe *pe, int bank)
{
    cwr(l, DPN_BASE(pe->pdi, bank) + DPN_CH_EN, 0);
}

/* clock divider of a bank (the bus clock does not change) */
void lnk_clk_bank(struct sdwlink *l, int bank)
{
    cupd(l, bank ? MCP_CLK_CTRL1 : MCP_CLK_CTRL0, 0xFF, l->mclk / l->busclk - 1);
}

/* ACE2: PCM stream channel map of a PDI to an HD Audio link DMA stream (ch 0: unmapped) */
void lnk_ace2_map(struct sdwlink *l, int pdi, int ch, int tag, int dir)
{
    u8 *p = l->shim + S2_PCMSYCHM(pdi);
    u16 v = RD16(p);
    v &= (u16)~0xBFFF;
    v |= (u16)((ch ? (u32)(ch - 1) << 4 : 0) | ((u32)(tag & 0x3F) << 8) | (dir ? CHM_DIR : 0));
    WR16(p, v);
}

/* ACE2: output stream id valid on the SoundWire link (playback link DMA) */
void lnk_ace2_losidv(int tag, int on)
{
    u8 *p;
    u16 v;
    if (!sdwc.eml || tag <= 0 || tag > 15) return;
    p = sdwc.eml + ML_LOSIDV;
    v = RD16(p);
    if (on) v |= (u16)(1U << tag);
    else v &= (u16)~(1U << tag);
    WR16(p, v);
}
