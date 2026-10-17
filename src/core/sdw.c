/* sdw.c - SoundWire on the Intel audio controller: setup of the links, enumeration of the
   peripherals, their register access, the peripheral driver table.

   At setup every link the hardware has (and ACPI does not disable) is powered and its bus
   started; the peripherals that attach get device numbers (from their 48-bit device id, as
   the Linux bus code does) and are initialized (bus clock base / scale, interrupt masks).
   Links without peripherals are powered down again. Peripherals are matched to a driver by
   manufacturer and part id; sdwadv.c then groups their functions into sound devices.
   Bus clock and frame shape come from the ACPI _DSD of the SoundWire controller when it can
   be read without an interpreter, else from the Linux defaults of the platform. */
#include "sdwint.h"

/* ================================================================ peripheral drivers
   The table of the peripheral drivers (sdwrt71.c, sdwrt7l.c, sdwamp.c, sdwcs.c, sdwmisc.c).
   Several drivers may claim one manufacturer / part id: they are tried in this order (SDCA
   devices prefer drivers named "...sdca..."), and the next one is taken when init fails. */
extern const struct sdwdrv sdw_rt711sdca, sdw_rt712sdca, sdw_rt713, sdw_rt716, sdw_rt717, sdw_rt722sdca;
extern const struct sdwdrv sdw_rt712dmic, sdw_rt713dmic, sdw_rt716dmic, sdw_rt717dmic;
extern const struct sdwdrv sdw_rt700, sdw_rt711, sdw_rt714, sdw_rt715, sdw_rt715sdca;
extern const struct sdwdrv sdw_rt1308, sdw_rt1316, sdw_rt1318, sdw_rt1017, sdw_rt1320, sdw_rt1321;
extern const struct sdwdrv sdw_cs42l43, sdw_cs42l43b, sdw_cs35l56, sdw_cs35l57, sdw_cs35l62, sdw_cs35l63, sdw_cs42l42;
extern const struct sdwdrv sdw_max98373, sdw_rt5682;
extern int sdw_rt711jd;                    /* sdwrt71.c */
extern u8 sdw_rt711_jdsrc;                 /* sdwrt7l.c */

static const struct sdwdrv *const drivers[] = {
    &sdw_rt711sdca, &sdw_rt712sdca, &sdw_rt713, &sdw_rt716, &sdw_rt717, &sdw_rt722sdca,
    &sdw_rt712dmic, &sdw_rt713dmic, &sdw_rt716dmic, &sdw_rt717dmic,
    &sdw_rt700, &sdw_rt711, &sdw_rt714, &sdw_rt715, &sdw_rt715sdca,
    &sdw_rt1308, &sdw_rt1316, &sdw_rt1318, &sdw_rt1017, &sdw_rt1320, &sdw_rt1321,
    &sdw_cs42l43, &sdw_cs42l43b, &sdw_cs35l56, &sdw_cs35l57, &sdw_cs35l62, &sdw_cs35l63, &sdw_cs42l42,
    &sdw_max98373, &sdw_rt5682,
    NULL
};
/* ================================================================ */

/* peripheral registers of the core */
#define SCP_INT1            0x40
#define SCP_INTMASK1        0x41
#define SCP_INT1_PARITY     0x01
#define SCP_INT1_BUS_CLASH  0x02
#define SCP_DEVNUMBER       0x46
#define SCP_ADDRPAGE1       0x48
#define SCP_BUS_CLOCK_BASE  0x4D
#define SCP_DEVID_0         0x50
#define SCP_BUSCLOCK_SCALE_B0 0x62
#define SCP_BUSCLOCK_SCALE_B1 0x72

static const u16 frame_rows[24] = { 48, 50, 60, 64, 75, 80, 125, 147, 96, 100, 120, 128, 150, 160, 250, 0,
                                    192, 200, 240, 256, 72, 144, 90, 180 };
static const u8 frame_cols[8] = { 2, 4, 6, 8, 10, 12, 14, 16 };

/* ---------------------------------------------------------------- ACPI */

static int pci_adr(u32 a)
{
    return a != 0xFFFFFFFFUL && (a >> 16) <= 0x1F && (a & 0xFFFF) <= 7;
}

/* the SoundWire controller of the HD Audio device: Device () with _ADR 0x40000000 below a
   PCI device, with the manager count / list property (link type 4 in bits 31..28) */
int sdw_present(void)
{
    static int known = -1;
    u32 up[2], v;
    if (known >= 0) return known;
    known = 0;
    if (acpi_find_adr(0x40000000UL, up, 2)) return 0;
    if (!pci_adr(up[0]) && !pci_adr(up[1])) return 0;
    if (acpi_prop_int(NULL, "mipi-sdw-manager-list", &v) && acpi_prop_int(NULL, "mipi-sdw-master-count", &v))
        return 0;
    known = 1;
    DBG("sdw: ACPI SoundWire controller below PCI %08lx\n", pci_adr(up[0]) ? up[0] : up[1]);
    return 1;
}

/* property of a link: its subproperties package, else the first one anywhere */
static int link_prop(struct sdwlink *l, const char *key, u32 *v)
{
    if (l->subprop[0] && !acpi_prop_int(l->subprop, key, v)) return 0;
    return acpi_prop_int(NULL, key, v);
}

static int shape_set(struct sdwlink *l, int r, int c)
{
    l->rows = frame_rows[r];
    l->cols = frame_cols[c];
    l->rowidx = (u8)r;
    l->colidx = (u8)c;
    return 0;
}

/* frame of 'bits' bits: rows x columns from the tables of the specification, the ACPI row
   count first, else the most columns */
static int frame_shape(struct sdwlink *l, u32 bits, u32 prow)
{
    int r, c;
    if (prow && !(bits % prow))
        for (r = 0; r < 24; r++)
            for (c = 0; c < 8 && frame_rows[r] == prow; c++)
                if (frame_cols[c] == bits / prow) return shape_set(l, r, c);
    for (c = 7; c >= 0; c--) {
        if (bits % frame_cols[c]) continue;
        for (r = 0; r < 24; r++)
            if (frame_rows[r] && frame_rows[r] == bits / frame_cols[c]) return shape_set(l, r, c);
    }
    return -1;
}

/* clocks and frame of a link */
static int link_clocks(struct sdwlink *l)
{
    u32 v, maxclk, prow = 0, div, div0, rate = SDW_RATE;
    char key[40];
    ksnprintf(key, sizeof(key), "mipi-sdw-link-%u-subproperties", l->id);
    if (acpi_prop_str(NULL, key, l->subprop, sizeof(l->subprop))) l->subprop[0] = 0;
    l->ipclk = dsp_xtal(l->d);                          /* 24 MHz CNL / CML, 38.4 MHz later */
    if (!link_prop(l, "intel-sdw-ip-clock", &v) && v >= 6000000UL && v <= 200000000UL) l->ipclk = v;
    l->mclk = l->ipclk / 2;
    maxclk = l->mclk / (l->mclk >= 19200000UL ? 4 : 2);  /* 4.8 MHz / 6 MHz */
    if (!link_prop(l, "mipi-sdw-max-clock-frequency", &v) && v >= 1000000UL && v <= l->mclk) maxclk = v;
    if (!link_prop(l, "mipi-sdw-default-frame-rate", &v) && v && v != SDW_RATE)
        DBG("sdw: link %u: frame rate %lu in ACPI, using 48000\n", l->id, v);
    if (!link_prop(l, "mipi-sdw-default-frame-row-size", &v)) prow = v;
    l->quirk = 0;
    if (l->subprop[0]) acpi_prop_int(l->subprop, "intel-quirk-mask", &l->quirk);
    /* the clock divider as Linux computes it (manager clock / maximum, rounded down), else
       the next slower clocks: a whole number of bits per 48 kHz frame that the frame
       tables allow */
    div0 = l->mclk / maxclk;
    if (!div0) div0 = 1;
    for (div = div0; div <= 32; div++) {
        u32 bc = l->mclk / div;
        if (l->mclk % div || (2 * bc) % rate) continue;
        if (frame_shape(l, 2 * bc / rate, prow)) continue;
        l->busclk = bc;
        return 0;
    }
    kprintf("sdw: link %u: no bus clock / frame for a %lu Hz IP clock\n", l->id, l->ipclk);
    return -1;
}

/* ---------------------------------------------------------------- register access */

static int page_set(struct sdwdev *s, u8 p1, u8 p2)
{
    u8 b[2];
    if (s->page1 == p1 && s->page2 == p2) return 0;
    b[0] = p1;
    b[1] = p2;
    if (lnk_cmds(s->link, s->devnum, 1, SCP_ADDRPAGE1, b, 2) != SR_OK) {
        s->page1 = s->page2 = 0xFF;
        return -1;
    }
    s->page1 = p1;
    s->page2 = p2;
    return 0;
}

/* addresses from 0x8000 through SCP_AddrPage1/2 (bits 30..23 / 22..15) and bit 15 set, as
   Linux does; 0x8000..0xFFFF directly on the devices without paging */
static int xfer(struct sdwdev *s, int wr, u32 reg, u8 *b, int n)
{
    u16 a;
    if (!s || !s->link || !s->attached || !s->devnum) return -1;
    if (reg >= 0x10000UL || (reg >= 0x8000UL && !s->nopage)) {
        if (page_set(s, (u8)(reg >> 23), (u8)(reg >> 15))) return -1;
        a = (u16)((reg & 0x7FFF) | 0x8000);
    } else {
        a = (u16)reg;
    }
    return lnk_cmds(s->link, s->devnum, wr, a, b, n) == SR_OK ? 0 : -1;
}

int sdw_rd(struct sdwdev *s, u32 reg)
{
    u8 v;
    if (xfer(s, 0, reg, &v, 1)) return -1;
    return v;
}

int sdw_wr(struct sdwdev *s, u32 reg, u8 val)
{
    return xfer(s, 1, reg, &val, 1);
}

int sdw_upd(struct sdwdev *s, u32 reg, u8 mask, u8 val)
{
    int v = sdw_rd(s, reg);
    if (v < 0) return -1;
    return sdw_wr(s, reg, (u8)(((u8)v & ~mask) | (val & mask)));
}

void sdw_seq(struct sdwdev *s, const u32 *pairs, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (sdw_wr(s, pairs[2 * i], (u8)pairs[2 * i + 1]))
            DBG("%s: write %08lx failed\n", s->drv ? s->drv->name : "sdw", pairs[2 * i]);
}

/* multi-byte quantities: the LSB read first latches the MSB in the MBQ register; writes
   go MSB (MBQ) first, the LSB write commits */
int sdw_rd16(struct sdwdev *s, u32 reg)
{
    int lo = sdw_rd(s, reg), hi;
    if (lo < 0) return -1;
    hi = sdw_rd(s, SDW_SDCA_MBQ(reg));
    if (hi < 0) return -1;
    return (hi << 8) | lo;
}

int sdw_wr16(struct sdwdev *s, u32 reg, u16 val)
{
    if (sdw_wr(s, SDW_SDCA_MBQ(reg), (u8)(val >> 8))) return -1;
    return sdw_wr(s, reg, (u8)val);
}

/* ---------------------------------------------------------------- peripherals */

const struct sdwport *sdw_dev_port(struct sdwdev *s, int func)
{
    int i;
    if (!s->drv) return NULL;
    for (i = 0; i < s->drv->nports; i++)
        if (s->drv->ports[i].func & func) return &s->drv->ports[i];
    return NULL;
}

static int is_sdca_name(const char *n)
{
    for (; *n; n++)
        if (!strncmp(n, "sdca", 4)) return 1;
    return 0;
}

/* driver for a device: the 'skip'-th candidate of its ids (SDCA devices: SDCA drivers first) */
static const struct sdwdrv *drv_match(struct sdwdev *s, int skip)
{
    int pass, i;
    for (pass = 0; pass < 2; pass++)
        for (i = 0; drivers[i]; i++) {
            const struct sdwdrv *dr = drivers[i];
            if (dr->mfr != s->mfr || dr->part != s->part) continue;
            if ((is_sdca_name(dr->name) == (s->cls != 0)) != (pass == 0)) continue;
            if (!skip--) return dr;
        }
    return NULL;
}

/* the SoundWire 1.1 Realtek codecs and the CS42L42 have no paging registers (Linux
   paging_support false): their 0x8000..0xFFFF registers are addressed directly */
static int no_paging(struct sdwdev *s)
{
    if (s->mfr == 0x01FA && s->part == 0x4242) return 1;
    if (s->mfr != 0x025D || s->cls) return 0;
    return s->part == 0x700 || s->part == 0x711 || s->part == 0x714 || s->part == 0x715 || s->part == 0x5682;
}

/* SCP_BusClock_Base and the scale of both banks (SDCA devices), as Linux maps them */
static void set_frequency(struct sdwdev *s)
{
    u32 m = s->link->mclk, cur = s->link->busclk, scale;
    u8 base, idx;
    if (!m || !cur) return;
    if (!(19200000UL % m)) { m = 19200000UL; base = 1; }
    else if (!(22579200UL % m)) { m = 22579200UL; base = 4; }
    else if (!(24576000UL % m)) { m = 24576000UL; base = 3; }
    else if (!(32000000UL % m)) { m = 32000000UL; base = 5; }
    else if (!(96000000UL % m)) { m = 24000000UL; base = 2; }
    else return;
    if (m % cur) return;
    scale = m / cur;
    for (idx = 0; idx < 7 && (1UL << idx) != scale; idx++) ;
    if (idx == 7) return;
    idx++;
    sdw_wr(s, SCP_BUS_CLOCK_BASE, base);
    sdw_wr(s, SCP_BUSCLOCK_SCALE_B0, idx);
    sdw_wr(s, SCP_BUSCLOCK_SCALE_B1, idx);
}

static const char *dev_name(struct sdwdev *s)
{
    return s->drv ? s->drv->name : "device";
}

/* the driver's implementation-defined init; another driver of the same ids when it fails */
int sdw_dev_init(struct sdwdev *s)
{
    int k, r;
    if (!s->drv || !s->drv->init || !(s->rsv & SDF_READY)) return 0;
    s->rsv &= (u8)~(SDF_RESET | SDF_FAILED);
    for (k = 0; drv_match(s, k) && drv_match(s, k) != s->drv; k++) ;
    for (k++; ; k++) {
        const struct sdwdrv *alt;
        r = s->drv->init(s);
        if (!r) return 0;
        if (r == 1) {
            /* the driver reset the device: it comes back on the bus and is set up again */
            s->rsv |= SDF_RESET;
            return 1;
        }
        alt = drv_match(s, k);
        if (!alt) break;
        DBG("sdw: %s init failed, trying %s\n", s->drv->name, alt->name);
        s->drv = alt;
    }
    kprintf("sdw: %s (link %u device %u): setup failed\n", s->drv->name, s->linkid, s->devnum);
    s->rsv |= SDF_FAILED;
    return -1;
}

static int link_service(struct sdwlink *l, int alerts);

/* devices whose driver reset them: serve the links until they are set up again (3 s) */
void sdw_wait_resets(void)
{
    u32 t0 = plat_ms();
    int i, pend;
    for (;;) {
        pend = 0;
        for (i = 0; i < sdwc.ndev; i++)
            if (sdwc.dev[i].rsv & SDF_RESET) pend = 1;
        if (!pend || plat_ms() - t0 > 3000) break;
        for (i = 0; i < sdwc.nlink; i++)
            if (sdwc.link[i].up) link_service(&sdwc.link[i], 0);
        mdelay(5);
    }
    for (i = 0; i < sdwc.ndev; i++)
        if (sdwc.dev[i].rsv & SDF_RESET) {
            kprintf("sdw: %s (link %u device %u) did not come back after its reset\n", dev_name(&sdwc.dev[i]),
                    sdwc.dev[i].linkid, sdwc.dev[i].devnum);
            sdwc.dev[i].rsv |= SDF_FAILED;
        }
}

/* a device reports ATTACHED on its device number: bus setup of the device (sdw_initialize_slave),
   then the driver */
static void dev_attach(struct sdwdev *s)
{
    int v;
    s->attached = 1;
    s->page1 = s->page2 = 0;
    sdwc.attcnt[s - sdwc.dev]++;
    s->rsv &= (u8)~SDF_OFF;
    if (s->cls) set_frequency(s);
    v = sdw_rd(s, SCP_INT1);
    if (v > 0 && (v & (SCP_INT1_PARITY | SCP_INT1_BUS_CLASH)))
        sdw_wr(s, SCP_INT1, (u8)(v & (SCP_INT1_PARITY | SCP_INT1_BUS_CLASH)));
    sdw_upd(s, SCP_INTMASK1, SCP_INT1_PARITY | SCP_INT1_BUS_CLASH, SCP_INT1_PARITY | SCP_INT1_BUS_CLASH);
    DBG("sdw: link %u device %u attached\n", s->linkid, s->devnum);
    sdw_dev_init(s);
}

/* a device on device number 0: read its id, give it a number (the one it had if it is
   known); 1 when a number was assigned */
static int assign(struct sdwlink *l)
{
    struct sdwctl *c = &sdwc;
    struct sdwdev *s = NULL;
    u8 id[6], b;
    u16 mfr, part;
    int r, i, dn;
    r = lnk_cmds(l, 0, 0, SCP_DEVID_0, id, 6);
    if (r != SR_OK) return 0;
    mfr = (u16)((id[1] << 8) | id[2]);
    part = (u16)((id[3] << 8) | id[4]);
    for (i = 0; i < c->ndev; i++) {
        struct sdwdev *t = &c->dev[i];
        if (t->link == l && t->mfr == mfr && t->part == part && t->cls == id[5] && t->unique == (id[0] & 15)) {
            s = t;
            break;
        }
    }
    if (!s) {
        if (c->ndev >= SDW_MAXDEV) return 0;
        for (dn = 1; dn <= 11 && (l->assigned & (1U << dn)); dn++) ;
        if (dn > 11) return 0;
        s = &c->dev[c->ndev++];
        memset(s, 0, sizeof(*s));
        s->link = l;
        s->d = l->d;
        s->linkid = l->id;
        s->devnum = (u8)dn;
        s->mfr = mfr;
        s->part = part;
        s->ver = (u8)(id[0] >> 4);
        s->unique = (u8)(id[0] & 15);
        s->cls = id[5];
        s->drv = drv_match(s, 0);
        s->busclk = l->busclk;
        s->nopage = (u8)no_paging(s);
        l->assigned |= (u16)(1U << dn);
        l->dev[dn] = s;
        l->ndev++;
        if (s->drv)
            kprintf("sdw: link %u device %u: %s (%04x:%04x version %u class %02x unique %u)\n", l->id, dn,
                    s->drv->name, mfr, part, s->ver, s->cls, s->unique);
        else
            kprintf("sdw: link %u device %u: unknown peripheral %04x:%04x version %u class %02x unique %u\n",
                    l->id, dn, mfr, part, s->ver, s->cls, s->unique);
    }
    s->attached = 0;
    b = s->devnum;
    if (lnk_cmds(l, 0, 1, SCP_DEVNUMBER, &b, 1) != SR_OK) {
        DBG("sdw: link %u: device number %u not taken\n", l->id, b);
        return 0;
    }
    return 1;
}

/* one look at the PING status of a link: devices that dropped off, attached, alerts,
   enumeration on device number 0; 1 if something changed */
static int link_service(struct sdwlink *l, int alerts)
{
    u32 st = lnk_status(l);
    int dn, chg = 0;
    if (!l->up) return 0;
    for (dn = 1; dn <= 11; dn++) {
        struct sdwdev *s = l->dev[dn];
        u32 v = (st >> (2 * dn)) & 3;
        if (!s) continue;
        if (!v && s->attached) {
            s->attached = 0;
            kprintf("sdw: %s (link %u device %u) left the bus\n", dev_name(s), l->id, dn);
            chg = 1;
        } else if ((v == 1 || v == 2) && !s->attached) {
            dev_attach(s);
            chg = 1;
        } else if (v == 2 && alerts) {
            /* bus clash / parity: clear them (implementation defined interrupts belong
               to the driver) */
            int i1 = sdw_rd(s, SCP_INT1);
            if (i1 > 0 && (i1 & (SCP_INT1_PARITY | SCP_INT1_BUS_CLASH)))
                sdw_wr(s, SCP_INT1, (u8)(i1 & (SCP_INT1_PARITY | SCP_INT1_BUS_CLASH)));
        }
    }
    if ((st & 3) && assign(l)) chg = 1;
    return chg;
}

/* polled from the sound devices: re-attached devices are set up again */
void sdw_link_poll(struct sdwlink *l)
{
    if (!l->up || plat_ms() - l->polltime < 1000) return;
    l->polltime = plat_ms();
    link_service(l, 1);
}

void sdw_links_down(void)
{
    int i;
    for (i = 0; i < sdwc.nlink; i++)
        if (sdwc.link[i].up || (sdwc.lmask & (1UL << i))) lnk_power(&sdwc.link[i], 0);
}

/* enumeration of the started links together: until nothing changed for 100 ms (at least
   200 ms, at most 1.5 s) */
static void enumerate(void)
{
    u32 t0 = plat_ms(), last = t0, now;
    int i, any;
    for (;;) {
        any = 0;
        for (i = 0; i < sdwc.nlink; i++)
            if (sdwc.link[i].up && link_service(&sdwc.link[i], 0)) any = 1;
        now = plat_ms();
        if (any) last = now;
        if (now - t0 > 1500) break;
        if (now - t0 >= 200 && now - last >= 100) break;
        mdelay(2);
    }
}

/* ---------------------------------------------------------------- setup */

int sdw_setup(struct dsp *d)
{
    struct sdwctl *c = &sdwc;
    u32 lmask = 0xFFFFFFFFUL, v;
    int i, n, nup = 0;
    if (d->pl->gen < DG_CNL || !sdw_present()) return 0;
    if (c->d) sdw_links_down();
    memset(c, 0, sizeof(*c));
    n = lnk_probe(d);
    if (n <= 0) {
        kprintf("sdw: no SoundWire links\n");
        return 0;
    }
    if (!acpi_prop_int(NULL, "mipi-sdw-manager-list", &v)) lmask = v;
    else if (!acpi_prop_int(NULL, "mipi-sdw-master-count", &v) && v && v < 32) lmask = (1UL << v) - 1;
    DBG("sdw: %d link(s) in hardware, ACPI link mask %08lx\n", n, lmask);
    for (i = 0; i < n; i++) {
        struct sdwlink *l = &c->link[i];
        if (!(lmask & (1UL << i))) continue;
        if (link_clocks(l)) continue;
        if (l->quirk & 2) {
            DBG("sdw: link %d disabled in ACPI\n", i);
            continue;
        }
        if (lnk_power(l, 1)) continue;
        if (lnk_start(l)) {
            lnk_power(l, 0);
            continue;
        }
        nup++;
    }
    if (!nup) {
        kprintf("sdw: no SoundWire link started\n");
        return 0;
    }
    enumerate();
    for (i = 0; i < n; i++) {
        struct sdwlink *l = &c->link[i];
        int k, att = 0;
        if (!l->up) continue;
        for (k = 1; k <= 11; k++) att += l->dev[k] && l->dev[k]->attached;
        kprintf("sdw: link %d: %lu kHz bus (IP %lu kHz), %ux%u frame, %d peripheral(s)\n", i, l->busclk / 1000,
                l->ipclk / 1000, l->rows, l->cols, att);
        if (!att) lnk_power(l, 0);
    }
    for (i = 0; i < c->ndev; i++) c->dev[i].rsv |= SDF_READY;
    /* RT711 jack detection source the BIOS gives ("realtek,jd-src", Linux JD enum) */
    if (!acpi_prop_int(NULL, "realtek,jd-src", &v) && v <= 4) {
        sdw_rt711jd = (int)v;
        sdw_rt711_jdsrc = (u8)v;
    }
    n = sdw_devices(d);
    if (!n) sdw_links_down();
    return n;
}
