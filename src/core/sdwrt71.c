/* sdwrt71.c - Realtek SDCA codecs on SoundWire: RT711-SDCA (headset codec), RT712 / RT713 /
   RT716 / RT717 (headset codec, RT712/716/717 with a stereo class-D speaker amplifier, the VB
   silicon with a DMIC function on DP8), the RT712 VA companion DMIC peripheral (part 0x1712 ..)
   and RT722 (headset codec, speaker amplifier and DMIC in one chip). Register sequences after
   the Linux rt711-sdca, rt712-sdca, rt712-sdca-dmic and rt722-sdca drivers.
   SDCA functions: 1 jack codec, 2 microphone array, 3 HID, 4 amplifier. Realtek private
   ("index") registers are 16-bit MBQ registers at nid << 20 | index. Jack detection: the GE
   entity 0x49 of the jack function reports its detected mode (0 nothing, 3 headphones,
   5 headset), polled here; the SDCA interrupt status bits are cleared by the poll. */
#include "sdw.h"

/* priv[] */
#define P_FLAGS     0
#define P_ON        1               /* SF_* streams running */
#define P_OUT       2               /* MX_MASTER level, inverted (0: full volume) */
#define P_IN        3               /* MX_INGAIN level, inverted */
#define P_DET       4               /* last GE detected mode, 0xFF: not read yet */

#define PF_INIT     0x01            /* initialized once */
#define PF_SETOWN   0x02            /* HID ownership back with HIDTX_SET_OWNER_TO_DEVICE */

/* SDCA functions, controls */
#define FN_JACK     1
#define FN_MIC      2
#define FN_HID      3
#define FN_AMP      4
#define C_MUTE      0x01            /* FU mute, PDE requested power state, GE selected mode */
#define C_VOL       0x02            /* FU volume, GE detected mode */
#define C_GAIN      0x0B
#define C_FREQ      0x10            /* CS sample frequency index, PDE actual power state,
                                       function status, HID current owner */
#define C_SETOWN    0x11
#define C_VENDOR    0x30
#define FREQ_48K    0x09
#define FS_NEEDS_INIT   0x20
#define ENT_GE      0x49
#define ENT_HID     0x01
#define CTL(f, e, c, ch)    SDW_SDCA_CTL(f, e, c, ch)

/* index register in an init table (written 16 bit), plain registers are written 8 bit */
#define IX(nid, reg)    (0x80000000UL | ((u32)(nid) << 20) | (u32)(reg))
#define IXFLAG          0x80000000UL

#define NID_VENDOR  0x20
#define NID_CHGPUMP 0x57
#define NID_CALI    0x58
#define NID_USD     0x59
#define NID_IMS     0x5B
#define NID_VAD     0x5E
#define NID_ANALOG  0x5F
#define NID_HDA     0x61
#define BUS_CLOCK_BASE  0x4D        /* SCP_BusClock_Base: 3 = 24.576 MHz */

/* RT711 jack detection source as in the machine driver quirks: 0 / 1 JD1 (default),
   2 JD2 (most Dell machines), 3 JD2 with the 100k decoder */
int sdw_rt711jd = 0;

/* ---- register helpers ---- */

static int ix_rd(struct sdwdev *s, u32 nid, u32 reg)
{
    return sdw_rd16(s, (nid << 20) | reg);
}

static void ix_wr(struct sdwdev *s, u32 nid, u32 reg, u16 val)
{
    sdw_wr16(s, (nid << 20) | reg, val);
}

static void ix_upd(struct sdwdev *s, u32 nid, u32 reg, u16 mask, u16 val)
{
    int v = ix_rd(s, nid, reg);
    if (v < 0) return;
    ix_wr(s, nid, reg, (u16)(((u16)v & ~mask) | val));
}

/* poll an index register until the mask bits are clear, 10 ms steps, 1 s */
static int ix_wait(struct sdwdev *s, u32 nid, u32 reg, u16 mask)
{
    int i, v = ix_rd(s, nid, reg);
    for (i = 0; i < 100 && v >= 0 && (v & mask); i++) {
        mdelay(10);
        v = ix_rd(s, nid, reg);
    }
    return (v >= 0 && !(v & mask)) ? 0 : -1;
}

static void rt_seq(struct sdwdev *s, const u32 *t, int n)
{
    int i;
    for (i = 0; i < n; i++, t += 2)
        if (t[0] & IXFLAG) sdw_wr16(s, t[0] & ~IXFLAG, (u16)t[1]);
        else sdw_wr(s, t[0], (u8)t[1]);
}

/* hidden register and HDA legacy reset */
static void rt_reset(struct sdwdev *s)
{
    ix_upd(s, NID_VENDOR, 0x1A, 0x4000, 0x4000);
    ix_upd(s, NID_HDA, 0x08, 0x0001, 0x0001);
}

/* power domain entity to PS0 / PS3, wait for the actual power state */
static void rt_pde(struct sdwdev *s, int fn, int ent, int ps)
{
    int i, v;
    sdw_wr(s, CTL(fn, ent, C_MUTE, 0), (u8)ps);
    for (i = 0; i < 100; i++) {
        v = sdw_rd(s, CTL(fn, ent, C_FREQ, 0));
        if (v < 0 || v == ps) break;
        mdelay(1);
    }
}

/* ---- the audio paths of a function ---- */

struct rtpath {
    u8 sf;                          /* SF_* */
    u8 fn;                          /* SDCA function */
    u8 pde;                         /* its power domain entity */
    u8 fu;                          /* feature unit: mute and volume */
    u8 nch;                         /* FU channels */
    u8 cs[2];                       /* clock sources (0: none) */
};

static const struct rtpath rt711_paths[] = {
    { SF_JACK_OUT, FN_JACK, 0x28, 0x05, 2, { 0x01, 0x11 } },
    { SF_JACK_IN,  FN_JACK, 0x29, 0x0F, 2, { 0x01, 0x11 } }
};
static const struct rtpath rt712_paths[] = {
    { SF_JACK_OUT, FN_JACK, 0x40, 0x05, 2, { 0x01, 0x11 } },
    { SF_JACK_IN,  FN_JACK, 0x12, 0x0F, 2, { 0x01, 0x11 } },
    { SF_SPK,      FN_AMP,  0x23, 0x06, 2, { 0x31, 0x00 } },
    { SF_DMIC,     FN_MIC,  0x11, 0x1E, 4, { 0x1F, 0x1C } }
};
static const struct rtpath rt722_paths[] = {
    { SF_JACK_OUT, FN_JACK, 0x40, 0x05, 2, { 0x01, 0x11 } },
    { SF_JACK_IN,  FN_JACK, 0x12, 0x0F, 2, { 0x01, 0x11 } },
    { SF_SPK,      FN_AMP,  0x23, 0x06, 2, { 0x31, 0x00 } },
    { SF_DMIC,     FN_MIC,  0x2A, 0x1E, 4, { 0x1F, 0x00 } }
};

static const struct rtpath *rt_paths(struct sdwdev *s, int *n)
{
    switch (s->part) {
    case 0x711: *n = ARRAY_SIZE(rt711_paths); return rt711_paths;
    case 0x722: *n = ARRAY_SIZE(rt722_paths); return rt722_paths;
    default:    *n = ARRAY_SIZE(rt712_paths); return rt712_paths;   /* the DMIC parts too */
    }
}

static const struct rtpath *rt_path(struct sdwdev *s, int func)
{
    int i, n;
    const struct rtpath *p = rt_paths(s, &n);
    if (!(s->drv->funcs & func)) return NULL;
    for (i = 0; i < n; i++)
        if (p[i].sf == func) return &p[i];
    return NULL;
}

/* FU volume (Q7.8 dB): playback -65.25 .. 0 dB, capture -17.25 .. +30 dB, 0.75 dB steps */
static u16 rt_vol(u32 lvl, int out)
{
    if (out) return (u16)((0UL - (87UL - cdc_step(lvl, 87)) * 0xC0UL) & 0xFFFF);
    return (u16)((0x1E00UL - (63UL - cdc_step(lvl, 63)) * 0xC0UL) & 0xFFFF);
}

/* volume and mute of a path: muted while its stream is off or at level 0 */
static void rt_apply(struct sdwdev *s, const struct rtpath *p)
{
    int out = (p->sf & (SF_JACK_OUT | SF_SPK)) != 0, ch;
    u32 lr = ~s->priv[out ? P_OUT : P_IN], lvl;
    int on = (s->priv[P_ON] & p->sf) != 0;
    for (ch = 1; ch <= p->nch; ch++) {
        lvl = (ch & 1) ? CDC_L(lr) : CDC_R(lr);
        sdw_wr16(s, CTL(p->fn, p->fu, C_VOL, ch), rt_vol(lvl, out));
        sdw_wr(s, CTL(p->fn, p->fu, C_MUTE, ch), (u8)((on && lvl) ? 0 : 1));
    }
}

static void rt_power(struct sdwdev *s, const struct rtpath *p, int on)
{
    int i;
    if (on)
        for (i = 0; i < 2; i++)
            if (p->cs[i]) sdw_wr(s, CTL(p->fn, p->cs[i], C_FREQ, 0), FREQ_48K);
    rt_pde(s, p->fn, p->pde, on ? 0 : 3);
}

static void rt_stream(struct sdwdev *s, int func, int on)
{
    const struct rtpath *p = rt_path(s, func);
    if (!p) return;
    if (on) {
        rt_power(s, p, 1);
        s->priv[P_ON] |= p->sf;
        rt_apply(s, p);
        if (s->part == 0x722 && p->sf == SF_DMIC) mdelay(150);    /* DMIC settling */
    } else {
        s->priv[P_ON] &= ~(u32)p->sf;
        rt_apply(s, p);
        rt_power(s, p, 0);
    }
}

static void rt_volume(struct sdwdev *s, int ctl, u32 lr)
{
    int i, n, mask;
    const struct rtpath *p = rt_paths(s, &n);
    if (ctl == MX_MASTER) {
        s->priv[P_OUT] = ~lr;
        mask = SF_JACK_OUT | SF_SPK;
    } else if (ctl == MX_INGAIN) {
        s->priv[P_IN] = ~lr;
        mask = SF_JACK_IN | SF_DMIC;
    } else return;
    for (i = 0; i < n; i++)
        if (p[i].sf & mask & s->drv->funcs) rt_apply(s, &p[i]);
}

/* end of init: volumes and mutes again, running streams powered again (re-attach) */
static void rt_done(struct sdwdev *s)
{
    int i, n;
    const struct rtpath *p = rt_paths(s, &n);
    s->priv[P_FLAGS] |= PF_INIT;
    s->priv[P_DET] = 0xFF;
    for (i = 0; i < n; i++) {
        if (!(p[i].sf & s->drv->funcs)) continue;
        if (s->priv[P_ON] & p[i].sf) rt_power(s, &p[i], 1);
        rt_apply(s, &p[i]);
    }
}

/* polled jack detection: clear the SDCA interrupts (jack: SDCA_0, HID button: SDCA_8, the
   HID message buffer handed back to the device), GE detected mode -> selected mode */
static int rt_jack(struct sdwdev *s)
{
    int i1 = sdw_rd(s, SDW_SCP_SDCA_INT1), i2 = sdw_rd(s, SDW_SCP_SDCA_INT2), det;
    if (i1 < 0 || i2 < 0) return -1;
    if (i1 & 0x01) sdw_wr(s, SDW_SCP_SDCA_INT1, 0x01);
    if (i2 & 0x01) {
        if (sdw_rd(s, CTL(FN_HID, ENT_HID, C_FREQ, 0)) == 0)
            sdw_wr(s, CTL(FN_HID, ENT_HID, (s->priv[P_FLAGS] & PF_SETOWN) ? C_SETOWN : C_FREQ, 0),
                   0x01);
        sdw_wr(s, SDW_SCP_SDCA_INT2, 0x01);
    }
    det = sdw_rd(s, CTL(FN_JACK, ENT_GE, C_VOL, 0));
    if (det < 0) return -1;
    if ((u32)det != s->priv[P_DET]) {
        s->priv[P_DET] = (u32)det;
        if (det) sdw_wr(s, CTL(FN_JACK, ENT_GE, C_MUTE, 0), (u8)det);
    }
    return det == 5 ? 3 : det == 3 ? 1 : 0;
}

static void rt_off(struct sdwdev *s)
{
    int i, n;
    const struct rtpath *p = rt_paths(s, &n);
    if (s->drv->jack) {
        sdw_wr(s, SDW_SCP_SDCA_INTMASK1, 0);
        sdw_wr(s, SDW_SCP_SDCA_INTMASK2, 0);
    }
    for (i = 0; i < n; i++) {
        if (!(p[i].sf & s->drv->funcs)) continue;
        s->priv[P_ON] &= ~(u32)p[i].sf;
        rt_apply(s, &p[i]);
        rt_pde(s, p[i].fn, p[i].pde, 3);
    }
}

/* SDCA interrupts on: jack (SDCA_0) and HID button (SDCA_8) */
static void rt_intmask(struct sdwdev *s)
{
    sdw_wr(s, SDW_SCP_SDCA_INTMASK1, 0x01);
    sdw_wr(s, SDW_SCP_SDCA_INTMASK2, 0x01);
}

/* ---- RT711-SDCA ---- */

static const u32 rt711_vd0a[] = {
    IX(NID_VENDOR, 0x6D), 0x0E00, IX(NID_HDA, 0x0A), 0x0008, 0x2F5A, 0x01,
    IX(NID_VENDOR, 0xB7), 0x8728, IX(NID_VENDOR, 0x47), 0xA472, 0x2F50, 0x02
};
static const u32 rt711_vd1[] = {
    IX(NID_VENDOR, 0x47), 0xA43E, 0x2F5A, 0x05, IX(NID_VENDOR, 0x6A), 0x0500,
    IX(NID_VENDOR, 0x06), 0x6173
};
static const u32 rt711_tail[] = {
    IX(NID_HDA, 0x06), 0x0000, IX(NID_VAD, 0x10), 0x0050
};

static void rt711_cali(struct sdwdev *s, int vd0)
{
    int i, v = sdw_rd(s, 0x320C);           /* RC calibration status */
    if (v >= 0 && !(v & 0x40)) ix_upd(s, NID_ANALOG, 0x01, 0x0010, 0x0010);
    for (i = 0; i < 100 && v >= 0 && !(v & 0x40); i++) {
        mdelay(10);
        v = sdw_rd(s, 0x320C);
    }
    if (v >= 0) {
        /* headphone DC calibration, manual mode */
        ix_upd(s, NID_VENDOR, 0x6F, 0x2000, 0x2000);
        ix_upd(s, NID_VENDOR, 0x6F, 0x000F, 0x0000);
        ix_upd(s, NID_CALI, 0x00, 0x0008, 0x0000);
        ix_upd(s, NID_CALI, 0x00, 0x0008, 0x0008);
        if (vd0) ix_upd(s, NID_CALI, 0x00, 0x4000, 0x4000);
        ix_upd(s, NID_CALI, 0x00, 0x8000, 0x8000);
        if (ix_wait(s, NID_CALI, 0x00, 0x8000)) kprintf("rt711: calibration timeout\n");
    }
    ix_upd(s, NID_VENDOR, 0x6F, 0x0040, 0x0040);    /* impedance sense on */
    ix_wr(s, NID_VENDOR, 0x4A, 0x201B);             /* release HP-JD, start the FSM */
}

static void rt711_jackinit(struct sdwdev *s)
{
    /* HID1 event, button RTC mode, GE mode change and push button events */
    ix_upd(s, NID_HDA, 0x3C, 0x80F0, 0x8000);
    ix_upd(s, NID_HDA, 0x38, 0x11DD, 0x11DD);
    ix_wr(s, NID_HDA, 0x3D, 0xFFFF);
    ix_upd(s, NID_HDA, 0x3F, 0xF000, 0x0000);
    ix_upd(s, NID_HDA, 0x35, 0x0C00, 0x0C00);
    if (sdw_rt711jd == 2) {
        ix_upd(s, NID_VENDOR, 0x08, 0x0002, 0x0002);
        ix_upd(s, NID_VENDOR, 0x09, 0x2002, 0x2002);
        ix_upd(s, NID_VENDOR, 0x11, 0x0400, 0x0400);
    } else if (sdw_rt711jd == 3) {
        ix_wr(s, NID_VENDOR, 0x47, 0xA47E);
        ix_upd(s, NID_VENDOR, 0x08, 0x0002, 0x0002);
        ix_upd(s, NID_VENDOR, 0x09, 0x3002, 0x0002);
        ix_upd(s, NID_VENDOR, 0x11, 0x0420, 0x0400);
    }
    rt_intmask(s);
    /* GE automatic jack type detection */
    ix_upd(s, NID_VENDOR, 0x45, 0x8400, 0x0000);
    ix_upd(s, NID_HDA, 0x36, 0x0010, 0x0000);
}

static int rt711_init(struct sdwdev *s)
{
    int v, vd0;
    if (s->cls != 1) return -1;             /* class 0: the non-SDCA RT711 */
    s->priv[P_FLAGS] |= PF_SETOWN;
    rt_reset(s);
    v = ix_rd(s, NID_VENDOR, 0x00);
    if (v < 0) return -1;
    vd0 = (v & 0x0F) == 0;
    if (vd0) {
        rt_seq(s, rt711_vd0a, ARRAY_SIZE(rt711_vd0a) / 2);
        ix_upd(s, NID_ANALOG, 0x05, 0x6000, 0x6000);
        ix_upd(s, NID_VENDOR, 0x47, 0x000C, 0x000C);
    } else {
        ix_upd(s, NID_HDA, 0x03, 0x0300, 0x0000);
        rt_seq(s, rt711_vd1, ARRAY_SIZE(rt711_vd1) / 2);
    }
    rt_seq(s, rt711_tail, ARRAY_SIZE(rt711_tail) / 2);
    ix_upd(s, NID_VENDOR, 0xB0, 0x1800, 0x0800);    /* DP4 mux from 08_filter_out */
    ix_upd(s, NID_HDA, 0x36, 0x0020, 0x0000);       /* ge_exclusive_inbox_en off */
    rt711_cali(s, vd0);
    sdw_wr(s, CTL(FN_JACK, 0x06, C_VENDOR, 0), 0x04);   /* HP output on */
    rt711_jackinit(s);
    rt_done(s);
    return 0;
}

/* ---- RT712 / RT713 / RT716 / RT717 ---- */

static const u32 rt712_va1[] = {
    IX(NID_VENDOR, 0x04), 0xAA81, IX(NID_VENDOR, 0x0E), 0xA1E0, IX(NID_IMS, 0x29), 0x0000,
    IX(NID_IMS, 0x2A), 0x0000, IX(NID_ANALOG, 0x08), 0x0000, 0x3201, 0x23
};
static const u32 rt712_va2[] = {
    IX(NID_HDA, 0x26), 0x1112, IX(NID_HDA, 0x23), 0x3412, IX(NID_HDA, 0x22), 0x4040
};
static const u32 rt712_va3[] = {
    0x2F50, 0x00, 0x2F54, 0x00, CTL(FN_JACK, 0x09, C_VENDOR, 0), 0x01
};
static const u32 rt712_vaspk[] = {
    IX(NID_HDA, 0x2C), 0x2323, IX(NID_HDA, 0x55), 0x0002, CTL(FN_AMP, 0x42, C_VENDOR, 0), 0x04
};
static const u32 rt712_vbmic[] = {
    IX(NID_HDA, 0xA6), 0x1526, IX(NID_HDA, 0xB1), 0x0304, IX(NID_HDA, 0xA2), 0x1F1E,
    IX(NID_HDA, 0xB0), 0x0304, IX(NID_HDA, 0x06), 0x8010, CTL(FN_MIC, 0x26, C_VENDOR, 0), 0x01,
    IX(NID_USD, 0x1E), 0x3200, 0x3201, 0x23, CTL(FN_MIC, 0, C_FREQ, 0), FS_NEEDS_INIT
};
static const u32 rt712_vbjack[] = {
    IX(NID_IMS, 0x23), 0x042A, IX(NID_CHGPUMP, 0x0C), 0x1FFF, IX(NID_VENDOR, 0x0C), 0xEC67,
    IX(NID_VENDOR, 0x04), 0xAA81, IX(NID_VENDOR, 0x0E), 0xA1E0, IX(NID_IMS, 0x29), 0x0000,
    IX(NID_IMS, 0x2A), 0x0000, 0x3201, 0x23, IX(NID_VENDOR, 0x09), 0x2802,
    IX(NID_VENDOR, 0x3C), 0xF215
};
static const u32 rt712_vbspk[] = {
    IX(NID_VENDOR, 0x0C), 0xEC63, IX(NID_VENDOR, 0x37), 0xFFF5, IX(NID_HDA, 0x55), 0x0002,
    CTL(FN_AMP, 0x42, C_VENDOR, 0), 0x04
};

static void rt712_clkpatch(struct sdwdev *s)
{
    ix_wr(s, NID_VENDOR, 0x65, 0x0000);
    sdw_wr(s, BUS_CLOCK_BASE, 3);
    mdelay(1);
    sdw_wr(s, BUS_CLOCK_BASE, 2);
    mdelay(1);
    /* PLL2 */
    sdw_upd(s, 0x2F3D, 0x80, 0x00);
    sdw_upd(s, 0x2F3D, 0x1F, 0x17);
    sdw_upd(s, 0x2F41, 0x10, 0x00);
    sdw_upd(s, 0x2F3A, 0x81, 0x01);
    sdw_wr(s, BUS_CLOCK_BASE, 3);
    mdelay(1);
    sdw_upd(s, 0x2F3A, 0x81, 0x81);
    sdw_upd(s, 0x2F3D, 0x80, 0x80);
    sdw_upd(s, 0x2F3D, 0x1F, 0x00);
    sdw_upd(s, 0x2F41, 0x10, 0x10);
    mdelay(1);
    ix_wr(s, NID_VENDOR, 0x65, 0x0081);
    ix_upd(s, NID_VENDOR, 0x49, 0x0800, 0x0000);
    ix_upd(s, NID_VENDOR, 0x49, 0xF000, 0x0000);
    ix_wr(s, NID_VENDOR, 0x65, 0x0000);
    ix_upd(s, NID_ANALOG, 0x0C, 0xC000, 0xC000);
    ix_upd(s, NID_ANALOG, 0x00, 0xC000, 0xC000);
    ix_wr(s, NID_VENDOR, 0x65, 0x0081);
    sdw_wr(s, BUS_CLOCK_BASE, 2);
    mdelay(1);
    sdw_wr(s, BUS_CLOCK_BASE, 3);
    mdelay(1);
    ix_wr(s, NID_VENDOR, 0x65, 0x0000);
}

/* headphone and class-D DC calibration */
static void rt712_cali(struct sdwdev *s, int va)
{
    int clk = sdw_rd(s, BUS_CLOCK_BASE), n;
    if (va) ix_wr(s, NID_VENDOR, 0x24, 0x043A);         /* HP-JD from JD1 */
    for (n = 0; n < 2; n++) {
        ix_wr(s, NID_VENDOR, 0x67, 0x4100);             /* FSM: calibration manual mode */
        ix_wr(s, NID_CALI, 0x00, 0x7883);
        ix_wr(s, NID_CALI, 0x00, 0xF893);               /* trigger */
        if (!ix_wait(s, NID_CALI, 0x00, 0x8000)) break;
        if (n) kprintf("rt712: calibration timeout\n");
        else if (clk == 3) rt712_clkpatch(s);
    }
    ix_wr(s, NID_VENDOR, 0x67, 0x4500);                 /* Rldet in the FSM */
    ix_wr(s, NID_IMS, 0x00, 0x040F);
    ix_wr(s, NID_IMS, 0x05, 0x0000);
    ix_wr(s, NID_VENDOR, 0x4A, 0x0010);                 /* release HP-JD */
}

static const struct sdwport rt712_ports[] = {
    { SF_JACK_OUT, 1, 2, SPF_FULL }, { SF_JACK_IN, 4, 2, SPF_FULL },
    { SF_SPK, 3, 2, SPF_FULL }, { SF_DMIC, 8, 2, SPF_FULL }
};
static const struct sdwport rt713_ports[] = {
    { SF_JACK_OUT, 1, 2, SPF_FULL }, { SF_JACK_IN, 4, 2, SPF_FULL }, { SF_DMIC, 8, 2, SPF_FULL }
};

static int rt712_init(struct sdwdev *s);

/* the VB silicon also has the DMIC function: init switches s->drv to these */
static const struct sdwdrv rt712vb = { "rt712-sdca", 0x025D, 0x0712,
    SF_JACK_OUT | SF_JACK_IN | SF_SPK | SF_DMIC, 4, rt712_ports,
    rt712_init, rt_stream, rt_volume, rt_jack, rt_off };
static const struct sdwdrv rt713vb = { "rt713-sdca", 0x025D, 0x0713,
    SF_JACK_OUT | SF_JACK_IN | SF_DMIC, 3, rt713_ports,
    rt712_init, rt_stream, rt_volume, rt_jack, rt_off };
static const struct sdwdrv rt716vb = { "rt716-sdca", 0x025D, 0x0716,
    SF_JACK_OUT | SF_JACK_IN | SF_SPK | SF_DMIC, 4, rt712_ports,
    rt712_init, rt_stream, rt_volume, rt_jack, rt_off };
static const struct sdwdrv rt717vb = { "rt717-sdca", 0x025D, 0x0717,
    SF_JACK_OUT | SF_JACK_IN | SF_SPK | SF_DMIC, 4, rt712_ports,
    rt712_init, rt_stream, rt_volume, rt_jack, rt_off };

static void rt712_va(struct sdwdev *s, int spk)
{
    rt_seq(s, rt712_va1, ARRAY_SIZE(rt712_va1) / 2);
    if (ix_rd(s, NID_VENDOR, 0x8A) == 0) rt712_cali(s, 1);     /* not after hibernation */
    ix_upd(s, NID_HDA, 0x53, 0x3000, 0x0000);                   /* ADC 23 from MIC2 */
    rt_seq(s, rt712_va2, ARRAY_SIZE(rt712_va2) / 2);
    ix_upd(s, NID_HDA, 0x0E, 0x0001, 0x0000);
    rt_seq(s, rt712_va3, ARRAY_SIZE(rt712_va3) / 2);
    if (spk) rt_seq(s, rt712_vaspk, ARRAY_SIZE(rt712_vaspk) / 2);
}

/* per function, only when the function status asks for it (or the first time) */
static void rt712_vb(struct sdwdev *s, int spk, int first)
{
    int clk = sdw_rd(s, BUS_CLOCK_BASE);
    int jst = sdw_rd(s, CTL(FN_JACK, 0, C_FREQ, 0));
    int mst = sdw_rd(s, CTL(FN_MIC, 0, C_FREQ, 0));
    int ast = sdw_rd(s, CTL(FN_AMP, 0, C_FREQ, 0));
    ix_wr(s, NID_VENDOR, 0x0B, 0x7778);
    if (clk == 3) rt712_clkpatch(s);
    if (first || (mst & FS_NEEDS_INIT))
        rt_seq(s, rt712_vbmic, ARRAY_SIZE(rt712_vbmic) / 2);
    if (first || (jst & FS_NEEDS_INIT)) {
        rt_seq(s, rt712_vbjack, ARRAY_SIZE(rt712_vbjack) / 2);
        rt712_cali(s, 0);
        ix_upd(s, NID_HDA, 0x53, 0x3000, 0x0000);               /* ADC 23 from MIC2 */
        sdw_wr(s, CTL(FN_JACK, 0x09, C_VENDOR, 0), 0x01);
        ix_wr(s, NID_HDA, 0x72, 0x0003);
        sdw_wr(s, CTL(FN_JACK, 0, C_FREQ, 0), FS_NEEDS_INIT);
    }
    if (first || (ast & FS_NEEDS_INIT)) {
        if (spk) rt_seq(s, rt712_vbspk, ARRAY_SIZE(rt712_vbspk) / 2);
        sdw_wr(s, CTL(FN_AMP, 0, C_FREQ, 0), FS_NEEDS_INIT);
    }
}

static int rt712_init(struct sdwdev *s)
{
    int v, va, spk, first = !(s->priv[P_FLAGS] & PF_INIT);
    rt_reset(s);
    v = ix_rd(s, NID_VENDOR, 0x00);         /* hw id 15:12 (6: RT713), version 11:8 */
    if (v < 0) return -1;
    va = ((v >> 8) & 0x0F) == 0;
    spk = ((v >> 12) & 0x0F) != 6;
    if (va) {
        s->priv[P_FLAGS] |= PF_SETOWN;
        rt712_va(s, spk);
    } else {
        s->priv[P_FLAGS] &= ~(u32)PF_SETOWN;
        rt712_vb(s, spk, first);
        switch (s->part) {
        case 0x713: s->drv = &rt713vb; break;
        case 0x716: s->drv = &rt716vb; break;
        case 0x717: s->drv = &rt717vb; break;
        default:    s->drv = &rt712vb; break;
        }
    }
    /* jack: HID1 event, button RTC mode, detected mode change and push button events */
    ix_wr(s, NID_HDA, 0x65, 0xFFF0);
    ix_upd(s, NID_HDA, 0x60, 0x1100, 0x1100);
    ix_upd(s, NID_HDA, 0x67, 0xF000, 0x0000);
    ix_upd(s, NID_HDA, 0x45, 0x0C00, 0x0C00);
    ix_upd(s, NID_HDA, 0x46, 0x0020, 0x0000);
    if (va) ix_wr(s, NID_VENDOR, 0x24, 0x043A);
    rt_intmask(s);
    ix_upd(s, NID_HDA, 0x45, 0x0080, 0x0080);           /* trigger a GE event */
    ix_upd(s, NID_HDA, 0x45, 0x0080, 0x0000);
    ix_wr(s, NID_VENDOR, 0x8A, 0x0001);                 /* hibernation flag */
    rt_done(s);
    return 0;
}

/* ---- RT712 VA DMIC peripheral (part 0x1712 / 0x1713 / 0x1716 / 0x1717) ---- */

static const u32 rtdmic_tab[] = {
    IX(NID_HDA, 0x26), 0x1112, IX(NID_HDA, 0x27), 0x1111, IX(NID_HDA, 0x2B), 0x1111,
    IX(NID_HDA, 0x2F), 0x1155, IX(NID_HDA, 0x10), 0x2626, IX(NID_HDA, 0x15), 0x1E19,
    IX(NID_HDA, 0x11), 0x1515, IX(NID_HDA, 0x18), 0x0304, IX(NID_HDA, 0x13), 0x0304,
    IX(NID_HDA, 0x06), 0x0050, CTL(FN_MIC, 0x26, C_VENDOR, 0), 0x01,
    IX(NID_USD, 0x1E), 0x3200, 0x3201, 0x23, 0x2F52, 0x00
};

static int rtdmic_init(struct sdwdev *s)
{
    rt_seq(s, rtdmic_tab, ARRAY_SIZE(rtdmic_tab) / 2);
    rt_done(s);
    return 0;
}

/* ---- RT722 ---- */

static const u32 rt722_mic[] = {
    IX(NID_HDA, 0x25), 0x2A29, IX(NID_HDA, 0x26), 0x2A00, IX(NID_HDA, 0x28), 0x2A2A,
    IX(NID_HDA, 0x10), 0x2626, IX(NID_HDA, 0x15), 0x1E00, IX(NID_HDA, 0x11), 0x1515,
    IX(NID_HDA, 0x17), 0x0304, IX(NID_HDA, 0x13), 0x0304, IX(NID_HDA, 0x06), 0x0000,
    CTL(FN_MIC, 0x26, C_VENDOR, 0), 0x01, 0x2F5C, 0x25, 0x2F03, 0x06
};
static const u32 rt722_amp[] = {
    IX(NID_VENDOR, 0x3C), 0xC215, IX(NID_CALI, 0x10000), 0x702C, IX(NID_CALI, 0x10000), 0xF02D,
    IX(NID_HDA, 0x29), 0x2323, IX(NID_HDA, 0x55), 0x0002, CTL(FN_AMP, 0x42, C_VENDOR, 8), 0x04
};
static const u32 rt722_jack1[] = {
    IX(NID_VENDOR, 0x04), 0xA081, IX(NID_HDA, 0x46), 0xA009, IX(NID_HDA, 0x64), 0xCF00,
    IX(NID_HDA, 0x65), 0x000F, IX(NID_HDA, 0x60), 0x1100, IX(NID_HDA, 0x67), 0x0C12,
    IX(NID_VENDOR, 0x09), 0x7002, IX(NID_VENDOR, 0x3C), 0xC215, IX(NID_VENDOR, 0x67), 0x4100,
    IX(NID_CALI, 0x03), 0x008D
};
static const u32 rt722_jack2[] = {
    IX(NID_HDA, 0x25), 0x2A12, IX(NID_HDA, 0x23), 0x3429, IX(NID_HDA, 0x24), 0x4112,
    IX(NID_HDA, 0x22), 0x4040, IX(NID_HDA, 0x76), 0x4141, IX(NID_HDA, 0x70), 0x0101,
    0x2F58, 0x07, 0x2F03, 0x06
};

static int rt722_init(struct sdwdev *s)
{
    int v, vid, st, first = !(s->priv[P_FLAGS] & PF_INIT);
    v = ix_rd(s, NID_VENDOR, 0x00);
    if (v < 0) return -1;
    vid = (v >> 8) & 0x0F;                  /* 0 VA, 1 VB */
    if (first) {
        rt_reset(s);
        if (vid == 0) ix_wr(s, NID_VENDOR, 0x1A, 0xB091);
    }
    st = sdw_rd(s, CTL(FN_MIC, 0, C_FREQ, 0));
    if (first || (st & FS_NEEDS_INIT)) {
        rt_seq(s, rt722_mic, ARRAY_SIZE(rt722_mic) / 2);
        if (vid == 1) sdw_wr(s, 0x2F52, 0x00);
        sdw_wr(s, CTL(FN_MIC, 0, C_FREQ, 0), FS_NEEDS_INIT);
    }
    st = sdw_rd(s, CTL(FN_AMP, 0, C_FREQ, 0));
    if (first || (st & FS_NEEDS_INIT)) {
        rt_seq(s, rt722_amp, ARRAY_SIZE(rt722_amp) / 2);
        if (vid == 1) sdw_wr(s, 0x2F54, 0x00);
        sdw_wr(s, CTL(FN_AMP, 0, C_FREQ, 0), FS_NEEDS_INIT);
    }
    st = sdw_rd(s, CTL(FN_JACK, 0, C_FREQ, 0));
    if (first || (st & FS_NEEDS_INIT)) {
        rt_seq(s, rt722_jack1, ARRAY_SIZE(rt722_jack1) / 2);    /* ends with the HP DC
                                                                   calibration trigger */
        mdelay(10);
        if (ix_wait(s, NID_CALI, 0x03, 0x0040)) kprintf("rt722: calibration timeout\n");
        rt_seq(s, rt722_jack2, ARRAY_SIZE(rt722_jack2) / 2);
        ix_upd(s, NID_VENDOR, 0x45, 0x0200, 0x0200);    /* MIC VREFO */
        ix_upd(s, NID_VENDOR, 0x63, 0x4000, 0x4000);
        ix_wr(s, NID_VENDOR, 0x4A, 0x0010);             /* release HP-JD */
        if (vid == 1) sdw_wr(s, 0x2F51, 0x00);
        sdw_wr(s, CTL(FN_JACK, 0, C_FREQ, 0), FS_NEEDS_INIT);
    }
    /* jack detection; XU 03 / 0D not bypassed */
    rt_intmask(s);
    ix_wr(s, NID_HDA, 0x03, 0x016E);
    sdw_wr(s, CTL(FN_JACK, 0x03, C_MUTE, 0), 0x00);
    sdw_wr(s, CTL(FN_JACK, 0x0D, C_MUTE, 0), 0x00);
    ix_wr(s, NID_HDA, 0x45, 0x0000);
    ix_upd(s, NID_HDA, 0x46, 0x4000, 0x4000);           /* trigger a GE event */
    rt_done(s);
    return 0;
}

/* ---- the drivers ---- */

static const struct sdwport rt711_ports[] = {
    { SF_JACK_OUT, 3, 2, SPF_FULL }, { SF_JACK_IN, 2, 2, SPF_FULL }
};
static const struct sdwport rtdmic_ports[] = {
    { SF_DMIC, 2, 2, SPF_FULL }
};
static const struct sdwport rt722_ports[] = {
    { SF_JACK_OUT, 1, 2, SPF_FULL }, { SF_JACK_IN, 2, 2, SPF_FULL },
    { SF_SPK, 3, 2, SPF_FULL }, { SF_DMIC, 6, 2, SPF_FULL }
};

const struct sdwdrv sdw_rt711sdca = { "rt711-sdca", 0x025D, 0x0711,
    SF_JACK_OUT | SF_JACK_IN, 2, rt711_ports,
    rt711_init, rt_stream, rt_volume, rt_jack, rt_off };
const struct sdwdrv sdw_rt712sdca = { "rt712-sdca", 0x025D, 0x0712,
    SF_JACK_OUT | SF_JACK_IN | SF_SPK, 3, rt712_ports,
    rt712_init, rt_stream, rt_volume, rt_jack, rt_off };
const struct sdwdrv sdw_rt713 = { "rt713-sdca", 0x025D, 0x0713,
    SF_JACK_OUT | SF_JACK_IN, 2, rt713_ports,
    rt712_init, rt_stream, rt_volume, rt_jack, rt_off };
const struct sdwdrv sdw_rt716 = { "rt716-sdca", 0x025D, 0x0716,
    SF_JACK_OUT | SF_JACK_IN | SF_SPK, 3, rt712_ports,
    rt712_init, rt_stream, rt_volume, rt_jack, rt_off };
const struct sdwdrv sdw_rt717 = { "rt717-sdca", 0x025D, 0x0717,
    SF_JACK_OUT | SF_JACK_IN | SF_SPK, 3, rt712_ports,
    rt712_init, rt_stream, rt_volume, rt_jack, rt_off };
const struct sdwdrv sdw_rt712dmic = { "rt712-sdca-dmic", 0x025D, 0x1712, SF_DMIC, 1, rtdmic_ports,
    rtdmic_init, rt_stream, rt_volume, NULL, rt_off };
const struct sdwdrv sdw_rt713dmic = { "rt713-sdca-dmic", 0x025D, 0x1713, SF_DMIC, 1, rtdmic_ports,
    rtdmic_init, rt_stream, rt_volume, NULL, rt_off };
const struct sdwdrv sdw_rt716dmic = { "rt716-sdca-dmic", 0x025D, 0x1716, SF_DMIC, 1, rtdmic_ports,
    rtdmic_init, rt_stream, rt_volume, NULL, rt_off };
const struct sdwdrv sdw_rt717dmic = { "rt717-sdca-dmic", 0x025D, 0x1717, SF_DMIC, 1, rtdmic_ports,
    rtdmic_init, rt_stream, rt_volume, NULL, rt_off };
const struct sdwdrv sdw_rt722sdca = { "rt722-sdca", 0x025D, 0x0722,
    SF_JACK_OUT | SF_JACK_IN | SF_SPK | SF_DMIC, 4, rt722_ports,
    rt722_init, rt_stream, rt_volume, rt_jack, rt_off };
