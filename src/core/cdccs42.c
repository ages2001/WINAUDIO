/* cdccs42.c - Cirrus Logic CS42L42 headset codec (Gemini Lake, Jasper Lake, Alder Lake, Raptor
   Lake and Meteor Lake Chromebooks, machine driver sof_cs42l42 in Linux; the speakers are on a
   separate amplifier). The registers are paged: a register number is 16 bit, its high byte the
   page, selected through register 0x00 of every page. The chip has no MCLK pin: it runs from its
   internal oscillator until SCLK (BCLK) is present, then from SCLK through its PLL. The jack is
   polled: tip sense, then the automatic headset type detection with the manual fallback of the
   Linux cs42l42 driver. */
#include "dspcdc.h"

/* board settings (the _DSD properties of the Chromebook firmware, which is where this codec is
   found): tip sense switch shorted to tip when plugged, HS bias short sense off */
#define TS_INV          1
#define HSBIAS_SENSE    0
#define TS_DBNC_RISE    5               /* 1000 ms */
#define TS_DBNC_FALL    0               /* 0 ms */
#define BIAS_LVL0       0x0F            /* HS_DET_LEVEL_15 */
#define HSBIAS_RAMP     2               /* slow */
#define HSBIAS_RAMP_MS  90

#define PAGE_REG        0x00
#define DEVID_AB        0x1001
#define DEVID_CD        0x1002
#define DEVID_E         0x1003
#define REVID           0x1005
#define MCLK_CTL        0x1009
#define PWR_CTL1        0x1101
#define PWR_CTL2        0x1102
#define OSC_SWITCH      0x1107
#define TSENSE_CTL      0x1113
#define HSDET_CTL1      0x111F
#define HSDET_CTL2      0x1120
#define HS_SWITCH_CTL   0x1121
#define HS_DET_STATUS   0x1124
#define HS_CLAMP_DIS    0x1129
#define MCLK_SRC_SEL    0x1201
#define FSYNC_PW_LOWER  0x1203
#define FSYNC_PW_UPPER  0x1204
#define FSYNC_P_LOWER   0x1205
#define FSYNC_P_UPPER   0x1206
#define ASP_CLK_CFG     0x1207
#define ASP_FRM_CFG     0x1208
#define FS_RATE_EN      0x1209
#define IN_ASRC_CLK     0x120A
#define OUT_ASRC_CLK    0x120B
#define PLL_DIV_CFG1    0x120C
#define CODEC_STATUS    0x1308
#define PLL_LOCK_STATUS 0x130E
#define TSRS_PLUG_STATUS 0x130F
#define ADC_OVFL_INT_MASK 0x1316
#define MIXER_INT_MASK  0x1317
#define SRC_INT_MASK    0x1318
#define ASP_RX_INT_MASK 0x1319
#define ASP_TX_INT_MASK 0x131A
#define CODEC_INT_MASK  0x131B
#define SRCPL_INT_MASK  0x131C
#define VPMON_INT_MASK  0x131E
#define PLL_LOCK_INT_MASK 0x131F
#define TSRS_PLUG_INT_MASK 0x1320
#define PLL_CTL1        0x1501
#define PLL_DIV_FRAC0   0x1502
#define PLL_DIV_FRAC1   0x1503
#define PLL_DIV_FRAC2   0x1504
#define PLL_DIV_INT     0x1505
#define PLL_CTL3        0x1508
#define PLL_CAL_RATIO   0x150A
#define PLL_CTL4        0x151B
#define HSBIAS_SC_AUTOCTL 0x1B70
#define WAKE_CTL        0x1B71
#define TIPSENSE_CTL    0x1B73
#define MISC_DET_CTL    0x1B74
#define MIC_DET_CTL1    0x1B75
#define DET_INT1_MASK   0x1B79
#define DET_INT2_MASK   0x1B7A
#define HS_BIAS_CTL     0x1C03
#define ADC_VOLUME      0x1D03
#define DAC_CTL2        0x1F06
#define HP_CTL          0x2001
#define MIXER_CHA_VOL   0x2301
#define MIXER_ADC_VOL   0x2302
#define MIXER_CHB_VOL   0x2303
#define SP_RX_CH_SEL    0x2501
#define ASP_TX_SZ_EN    0x2901
#define ASP_TX_CH_EN    0x2902
#define ASP_TX_CH_AP_RES 0x2903
#define ASP_RX_DAI0_EN  0x2A01
#define ASP_RX_CH1_AP_RES 0x2A02
#define ASP_RX_CH2_AP_RES 0x2A05

#define CHIP_ID         0x42A42UL
#define TS_PLUG         3               /* TSRS_PLUG_STATUS bits 3:2 */
#define TS_UNPLUG       0
#define TS_TRANS        1
#define PLUG_CTIA       0               /* HS_DET_STATUS type */
#define PLUG_OMTP       1
#define PLUG_HEADPHONE  2
#define PLUG_INVALID    3
#define SW_TYPE1        0xA6            /* HS_SWITCH_CTL: also the comparator 1 setting */
#define SW_TYPE2        0x59            /* also the comparator 2 setting */
#define SW_TYPE3        0xF3
#define HSDET_MAN_ACTIVE 0x40           /* HSDET_CTL2 */
#define HSDET_MAN_OFF   0x00
#define HSDET_MAN_SET2  0x23
#define HSDET_AUTO_OFF  0xA3
#define HSDET_AUTO_ON   0xE3

/* priv[] */
#define P_PAGE  0                       /* page selected in the chip, 0x100 unknown */
#define P_USE   1                       /* streams: bit 0 playback, bit 1 capture */
#define P_CLK   2                       /* 1: running from SCLK */
#define P_HPM   3                       /* headphone mute state + 1, 0 unknown */
#define P_PLL   4                       /* PLL table entry */
#define P_PLUG  5                       /* tip sense state */
#define P_TYPE  6                       /* plug type */
#define P_DET   7                       /* polls since the type detection started, 0 idle */
#define P_HPPWR 8                       /* headphone amplifier power state + 1 */

/* PLL settings for each SCLK (datasheet table 4-5, as in Linux) */
struct pll {
    u32 sclk;
    u8 src, prediv, div_int;
    u32 frac;
    u8 mode, divout;
    u32 mclk_int;
    u8 cal, n;
};
static const struct pll plls[] = {
    {  1411200UL, 1, 0x00, 0x80, 0x000000UL, 0x03, 0x10, 11289600UL, 128, 2 },
    {  1536000UL, 1, 0x00, 0x7D, 0x000000UL, 0x03, 0x10, 12000000UL, 125, 2 },
    {  2304000UL, 1, 0x00, 0x55, 0xC00000UL, 0x02, 0x10, 12288000UL,  85, 2 },
    {  2400000UL, 1, 0x00, 0x50, 0x000000UL, 0x03, 0x10, 12000000UL,  80, 2 },
    {  2822400UL, 1, 0x00, 0x40, 0x000000UL, 0x03, 0x10, 11289600UL, 128, 1 },
    {  3000000UL, 1, 0x00, 0x40, 0x000000UL, 0x03, 0x10, 12000000UL, 128, 1 },
    {  3072000UL, 1, 0x00, 0x3E, 0x800000UL, 0x03, 0x10, 12000000UL, 125, 1 },
    {  4000000UL, 1, 0x00, 0x30, 0x800000UL, 0x03, 0x10, 12000000UL,  96, 1 },
    {  4096000UL, 1, 0x00, 0x2E, 0xE00000UL, 0x03, 0x10, 12000000UL,  94, 1 },
    {  4800000UL, 1, 0x01, 0x50, 0x000000UL, 0x03, 0x10, 12000000UL,  80, 2 },
    {  4800000UL, 1, 0x01, 0x50, 0x000000UL, 0x01, 0x10, 12288000UL,  82, 2 },
    {  5644800UL, 1, 0x01, 0x40, 0x000000UL, 0x03, 0x10, 11289600UL, 128, 1 },
    {  6000000UL, 1, 0x01, 0x40, 0x000000UL, 0x03, 0x10, 12000000UL, 128, 1 },
    {  6144000UL, 1, 0x01, 0x3E, 0x800000UL, 0x03, 0x10, 12000000UL, 125, 1 },
    {  6144000UL, 1, 0x01, 0x40, 0x000000UL, 0x03, 0x10, 12288000UL, 128, 1 },
    {  9600000UL, 1, 0x02, 0x50, 0x000000UL, 0x03, 0x10, 12000000UL,  80, 2 },
    {  9600000UL, 1, 0x02, 0x50, 0x000000UL, 0x01, 0x10, 12288000UL,  82, 2 },
    { 11289600UL, 0, 0x00, 0x00, 0x000000UL, 0x00, 0x00, 11289600UL,   0, 1 },
    { 12000000UL, 0, 0x00, 0x00, 0x000000UL, 0x00, 0x00, 12000000UL,   0, 1 },
    { 12288000UL, 0, 0x00, 0x00, 0x000000UL, 0x00, 0x00, 12288000UL,   0, 1 },
    { 19200000UL, 1, 0x03, 0x50, 0x000000UL, 0x03, 0x10, 12000000UL,  80, 2 },
    { 19200000UL, 1, 0x03, 0x50, 0x000000UL, 0x01, 0x10, 12288000UL,  82, 2 },
    { 22579200UL, 1, 0x03, 0x40, 0x000000UL, 0x03, 0x10, 11289600UL, 128, 1 },
    { 24000000UL, 1, 0x03, 0x40, 0x000000UL, 0x03, 0x10, 12000000UL, 128, 1 },
    { 24576000UL, 1, 0x03, 0x40, 0x000000UL, 0x03, 0x10, 12288000UL, 128, 1 }
};

/* every interrupt source masked (the status registers latch anyway), then the datasheet
   power-down sequence */
static const u16 irq_masks[] = {
    ADC_OVFL_INT_MASK, 0x01, MIXER_INT_MASK, 0x0F, SRC_INT_MASK, 0x0F, ASP_RX_INT_MASK, 0x1F,
    ASP_TX_INT_MASK, 0x0F, CODEC_INT_MASK, 0x03, SRCPL_INT_MASK, 0x7F, VPMON_INT_MASK, 0x01,
    PLL_LOCK_INT_MASK, 0x01, TSRS_PLUG_INT_MASK, 0x0F, DET_INT1_MASK, 0xE0, DET_INT2_MASK, 0xFF
};
static const u16 shutdown_seq[] = {
    MIC_DET_CTL1, 0x9F, WAKE_CTL, 0xE1, MIXER_CHA_VOL, 0x3F, MIXER_ADC_VOL, 0x3F,
    MIXER_CHB_VOL, 0x3F, HP_CTL, 0x0F, ASP_RX_DAI0_EN, 0x00, ASP_CLK_CFG, 0x00, HSDET_CTL2, 0x00,
    PWR_CTL1, 0xFE, PWR_CTL2, 0x8C, DAC_CTL2, 0x02, HS_CLAMP_DIS, 0x00, MISC_DET_CTL, 0x03,
    TIPSENSE_CTL, 0x02, HSBIAS_SC_AUTOCTL, 0x03, PWR_CTL1, 0xFF
};

/* ---- paged register access */

static int cs_page(struct cdc *c, u32 reg)
{
    u8 w[2];
    if (c->priv[P_PAGE] == (reg >> 8)) return 0;
    w[0] = PAGE_REG;
    w[1] = (u8)(reg >> 8);
    if (i2c_xfer(c->bus, c->addr, w, 2, NULL, 0)) {
        c->priv[P_PAGE] = 0x100;
        return -1;
    }
    c->priv[P_PAGE] = reg >> 8;
    return 0;
}

static int cs_rd(struct cdc *c, u32 reg)
{
    u8 w[1], r[1];
    if (cs_page(c, reg)) return -1;
    w[0] = (u8)reg;
    if (i2c_xfer(c->bus, c->addr, w, 1, r, 1)) {
        c->priv[P_PAGE] = 0x100;
        return -1;
    }
    return r[0];
}

static int cs_wr(struct cdc *c, u32 reg, u32 val)
{
    u8 w[2];
    if (cs_page(c, reg)) return -1;
    w[0] = (u8)reg;
    w[1] = (u8)val;
    if (i2c_xfer(c->bus, c->addr, w, 2, NULL, 0)) {
        c->priv[P_PAGE] = 0x100;
        return -1;
    }
    return 0;
}

static int cs_upd(struct cdc *c, u32 reg, u32 mask, u32 val)
{
    int v = cs_rd(c, reg);
    if (v < 0) return -1;
    return cs_wr(c, reg, ((u32)v & ~mask) | (val & mask));
}

static void cs_seq(struct cdc *c, const u16 *p, int n)
{
    int i;
    for (i = 0; i + 1 < n; i += 2) cs_wr(c, p[i], p[i + 1]);
}

static u32 cs_devid(struct cdc *c)
{
    int ab = cs_rd(c, DEVID_AB), cd = cs_rd(c, DEVID_CD), e = cs_rd(c, DEVID_E);
    if (ab < 0 || cd < 0 || e < 0) return 0;
    return ((u32)ab << 12) | ((u32)cd << 4) | ((u32)e >> 4);
}

static int cs_probe(struct cdc *c)
{
    c->priv[P_PAGE] = 0x100;
    if (cs_devid(c) == CHIP_ID) return 0;
    if (!cdc_ngpio(c)) return -1;
    /* held in reset: the GpioIo is the active low reset line */
    cdc_gpio(c, 0, 1);
    mdelay(3);
    return cs_devid(c) == CHIP_ID ? 0 : -1;
}

/* ---- clocks */

/* SCLK as the clock source, through the PLL when the SCLK rate needs it; fails while no SCLK
   comes in (the PLL does not lock), the next jack poll tries again */
static void to_sclk(struct cdc *c)
{
    const struct pll *p = &plls[c->priv[P_PLL]];
    if (p->src) {
        int i, lock = 0;
        if (cs_upd(c, PLL_CTL1, 0x01, 0x01)) return;
        if (p->n > 1) {
            udelay(800);
            cs_wr(c, PLL_CTL3, p->divout);
        }
        for (i = 0; i < 6 && !lock; i++) {
            int s;
            udelay(250);
            s = cs_rd(c, PLL_LOCK_STATUS);
            lock = s > 0 && (s & 1);
        }
        if (!lock) {
            cs_upd(c, PLL_CTL1, 0x01, 0x00);
            cs_wr(c, PLL_CTL3, (u32)p->divout * p->n);
            return;
        }
        cs_upd(c, MCLK_SRC_SEL, 0x01, 0x01);
    }
    if (cs_wr(c, OSC_SWITCH, 0x01)) return;     /* SCLK present, oscillator off */
    udelay(150);
    c->priv[P_CLK] = 1;
}

/* back to the internal oscillator: needs SCLK still running, else the chip does not answer
   until SCLK comes back */
static void to_osc(struct cdc *c)
{
    const struct pll *p = &plls[c->priv[P_PLL]];
    if (cs_wr(c, OSC_SWITCH, 0x00)) return;
    udelay(150);
    cs_upd(c, MCLK_SRC_SEL, 0x01, 0x00);
    udelay(150);
    cs_upd(c, PLL_CTL1, 0x01, 0x00);
    if (p->src) cs_wr(c, PLL_CTL3, (u32)p->divout * p->n);
    c->priv[P_CLK] = 0;
}

static void hp_mute(struct cdc *c, int m)
{
    if (c->priv[P_HPM] == (u32)m + 1) return;
    if (!cs_upd(c, HP_CTL, 0x0C, m ? 0x0C : 0x00)) c->priv[P_HPM] = (u32)m + 1;
}

/* the clock and the headphone mute as the running streams want them */
static void cs_sync(struct cdc *c)
{
    u32 use = c->priv[P_USE];
    if (!(use & 1)) hp_mute(c, 1);
    if (use && !c->priv[P_CLK]) to_sclk(c);
    else if (!use && c->priv[P_CLK]) to_osc(c);
    if ((use & 1) && c->priv[P_CLK]) hp_mute(c, 0);
}

static int cs_pll(struct cdc *c, u32 sclk)
{
    int i;
    for (i = 0; i < (int)ARRAY_SIZE(plls); i++) {
        const struct pll *p = &plls[i];
        if (p->sclk != sclk || p->mclk_int % CDC_RATE) continue;
        c->priv[P_PLL] = (u32)i;
        /* internal sample rate: 12 / 24 MHz based or not */
        cs_upd(c, MCLK_CTL, 0x02, p->mclk_int != 12000000UL && p->mclk_int != 24000000UL ? 0x02 : 0x00);
        if (!p->src) {
            cs_upd(c, PLL_CTL1, 0x01, 0x00);    /* SCLK straight through */
            return 0;
        }
        cs_upd(c, PLL_DIV_CFG1, 0x03, p->prediv);
        cs_wr(c, PLL_DIV_INT, p->div_int);
        cs_wr(c, PLL_DIV_FRAC0, p->frac & 0xFF);
        cs_wr(c, PLL_DIV_FRAC1, (p->frac >> 8) & 0xFF);
        cs_wr(c, PLL_DIV_FRAC2, (p->frac >> 16) & 0xFF);
        cs_upd(c, PLL_CTL4, 0x03, p->mode);
        cs_wr(c, PLL_CTL3, (u32)p->divout * p->n);
        cs_wr(c, PLL_CAL_RATIO, p->cal);
        return 0;
    }
    return -1;
}

/* ---- headset type detection */

static void hp_power(struct cdc *c, int on)
{
    if (c->priv[P_HPPWR] == (u32)on + 1) return;
    if (cs_upd(c, PWR_CTL1, 0x08, on ? 0x00 : 0x08)) return;
    c->priv[P_HPPWR] = (u32)on + 1;
    if (on) mdelay(20);
}

/* tip plugged: HS bias up, automatic type detection started */
static void detect_start(struct cdc *c)
{
    cs_upd(c, MISC_DET_CTL, 0x07, 0x03);        /* HS bias and level detect off */
    cs_upd(c, HSBIAS_SC_AUTOCTL, 0xE7, 0x03);
    cs_upd(c, HSDET_CTL2, 0xFB, HSDET_MAN_SET2);
    cs_upd(c, DAC_CTL2, 0xFF, 0x87);            /* HP outputs pulled down and clamped */
    cs_upd(c, MISC_DET_CTL, 0x07, 0x07);        /* HS bias 2.7 V */
    mdelay(HSBIAS_RAMP_MS);
    (void)cs_rd(c, CODEC_STATUS);               /* drop an old "done" */
    cs_upd(c, HSDET_CTL2, 0xFB, HSDET_AUTO_ON);
}

/* manual detection with the two comparators (when the automatic one found no headset) */
static u32 detect_manual(struct cdc *c)
{
    int s;
    u32 c1, c2, type, sw;
    cs_upd(c, HSDET_CTL2, 0xFB, HSDET_MAN_ACTIVE);
    cs_upd(c, HSDET_CTL1, 0xFF, 0x2C);          /* 1.25 V and 1.75 V comparators */
    cs_wr(c, HS_SWITCH_CTL, SW_TYPE1);
    mdelay(100);
    s = cs_rd(c, HS_DET_STATUS);
    if (s < 0) s = 0;
    c1 = ((u32)s >> 6) & 1;
    c2 = ((u32)s >> 7) & 1;
    cs_wr(c, HS_SWITCH_CTL, SW_TYPE2);
    mdelay(100);
    s = cs_rd(c, HS_DET_STATUS);
    if (s < 0) s = 0;
    c1 |= (((u32)s >> 6) & 1) << 1;
    c2 |= (((u32)s >> 7) & 1) << 1;
    if (c1 == 1 || (c1 != 2 && c2 == 1)) {
        type = PLUG_CTIA;
        sw = SW_TYPE1;
    } else if (c1 == 2 || c2 == 2) {
        type = PLUG_OMTP;
        sw = SW_TYPE2;
    } else {
        type = PLUG_HEADPHONE;                  /* also type 3 / 4 headsets */
        sw = SW_TYPE3;
    }
    cs_wr(c, HS_SWITCH_CTL, sw);
    cs_upd(c, HSDET_CTL2, 0xFB, HSDET_MAN_OFF);
    cs_upd(c, HSDET_CTL1, 0xFF, 0x77);
    return type;
}

static void detect_done(struct cdc *c)
{
    int s = cs_rd(c, HS_DET_STATUS);
    u32 type = s < 0 ? PLUG_INVALID : (u32)s & 3;
    cs_upd(c, HSDET_CTL2, 0xFB, HSDET_AUTO_OFF);
    if (type == PLUG_INVALID || type == PLUG_HEADPHONE) type = detect_manual(c);
    if (type == PLUG_CTIA || type == PLUG_OMTP) {
        /* headset: HS bias on for the microphone */
        cs_upd(c, HSBIAS_SC_AUTOCTL, 0xE7, 0x03);
        cs_upd(c, MIC_DET_CTL1, 0xFF, 0x80 | BIAS_LVL0);
        cs_upd(c, HSBIAS_SC_AUTOCTL, 0xE7, (HSBIAS_SENSE << 7) | 0x40 | 0x03);
        cs_upd(c, MISC_DET_CTL, 0x07, 0x06);
    } else {
        cs_upd(c, MISC_DET_CTL, 0x07, 0x03);
    }
    cs_upd(c, DAC_CTL2, 0xFF, 0x02);            /* HP outputs released, DAC high pass on */
    c->priv[P_TYPE] = type;
}

static void detect_cancel(struct cdc *c)
{
    cs_upd(c, MISC_DET_CTL, 0x07, 0x03);        /* HS bias grounded */
    cs_upd(c, HSBIAS_SC_AUTOCTL, 0xE7, 0x03);
    cs_upd(c, HSDET_CTL2, 0xFB, HSDET_MAN_SET2);
    c->priv[P_TYPE] = PLUG_INVALID;
}

/* ---- driver */

static int cs_init(struct cdc *c)
{
    u32 bclk = c->ssp.bclk, fs, w;
    int rev, ret = 0;
    memset(c->priv, 0, sizeof(c->priv));
    c->priv[P_PAGE] = 0x100;
    c->priv[P_PLUG] = TS_TRANS;                 /* a plugged jack gets detected at the first poll */
    c->priv[P_TYPE] = PLUG_INVALID;
    if (cdc_ngpio(c)) {
        cdc_gpio(c, 0, 0);
        udelay(500);
        cdc_gpio(c, 0, 1);
    }
    mdelay(3);
    if (cs_devid(c) != CHIP_ID) return -1;
    rev = cs_rd(c, REVID);
    DBG("cs42l42: revision %02x\n", rev);
    cs_seq(c, irq_masks, ARRAY_SIZE(irq_masks));
    /* internal oscillator, PLL off */
    cs_wr(c, OSC_SWITCH, 0x00);
    cs_upd(c, MCLK_SRC_SEL, 0x01, 0x00);
    cs_upd(c, PLL_CTL1, 0x01, 0x00);
    /* codec on, all blocks still down */
    cs_upd(c, PWR_CTL1, 0xF5, 0xF4);
    /* tip sense debounce, HS bias ramp */
    cs_upd(c, TSENSE_CTL, 0x3F, TS_DBNC_RISE | (TS_DBNC_FALL << 3));
    cs_upd(c, HS_BIAS_CTL, 0x03, HSBIAS_RAMP);
    /* headset detection setup */
    cs_upd(c, MISC_DET_CTL, 0x18, 0x00);        /* detect mode 0 */
    cs_upd(c, MIC_DET_CTL1, 0xFF, 0x80 | BIAS_LVL0);   /* latch to VP */
    cs_upd(c, HS_CLAMP_DIS, 0x01, 0x01);
    cs_upd(c, TSENSE_CTL, 0x80, 0x80);
    cs_upd(c, TIPSENSE_CTL, 0xE3, 0xC0 | (TS_INV ? 0x00 : 0x20) | 0x02);
    /* audio port: I2S consumer, 50 % LRCK, data 1 SCLK after the LRCK edge */
    if (c->ssp.provider) kprintf("cs42l42: codec clock provider mode not supported, consumer used\n");
    if (c->ssp.fmt && c->ssp.fmt != 1) kprintf("cs42l42: only I2S supported (SSP format %u)\n", c->ssp.fmt);
    cs_upd(c, ASP_FRM_CFG, 0x1F, 0x08 | 0x02);
    cs_upd(c, ASP_CLK_CFG, 0x3F, 0x20 | 0x0C);  /* SCLK on, normal SCLK polarity */
    switch (c->ssp.vbits) {
    case 16: w = 1; break;
    case 32: w = 3; break;
    default: w = 2; break;                      /* 24 */
    }
    cs_upd(c, ASP_TX_CH_AP_RES, 0xCF, 0x40 | (w << 2) | w);    /* capture: channel 2 on high LRCK */
    cs_upd(c, ASP_RX_CH1_AP_RES, 0x43, w);                     /* playback: 1 low, 2 high LRCK */
    cs_upd(c, ASP_RX_CH2_AP_RES, 0x43, 0x40 | w);
    cs_upd(c, SP_RX_CH_SEL, 0x0C, 0x04);                       /* channel B from channel 2 */
    cs_upd(c, ASP_RX_DAI0_EN, 0x3C, 0x0C);
    /* clocks from SCLK = BCLK (sof_cs42l42 passes the BCLK rate) */
    if (!bclk) bclk = CDC_RATE * (c->ssp.slots ? c->ssp.slots : 2) * (c->ssp.width ? c->ssp.width : 32);
    fs = bclk / CDC_RATE;
    if (cs_pll(c, bclk) || fs * CDC_RATE != bclk || (fs & 1)) {
        kprintf("cs42l42: SCLK %lu Hz not supported\n", bclk);
        ret = -1;
    } else {
        cs_wr(c, FSYNC_P_LOWER, (fs - 1) & 0xFF);
        cs_wr(c, FSYNC_P_UPPER, (fs - 1) >> 8);
        cs_wr(c, FSYNC_PW_LOWER, (fs / 2 - 1) & 0xFF);
        cs_wr(c, FSYNC_PW_UPPER, (fs / 2 - 1) >> 8);
    }
    /* sample rate converters for rates up to 96 kHz, ASRC clock 6 MHz */
    cs_upd(c, FS_RATE_EN, 0x0F, 0x03);
    cs_upd(c, IN_ASRC_CLK, 0x01, 0x00);
    cs_upd(c, OUT_ASRC_CLK, 0x01, 0x00);
    /* paths: mixer 0 dB, ADC 0 dB, no sidetone, headphones muted until a stream runs */
    cs_upd(c, HP_CTL, 0x0C, 0x0C);
    c->priv[P_HPM] = 2;
    cs_upd(c, MIXER_CHA_VOL, 0x3F, 0x00);
    cs_upd(c, MIXER_CHB_VOL, 0x3F, 0x00);
    cs_wr(c, MIXER_ADC_VOL, 0x3F);
    cs_wr(c, ADC_VOLUME, 0x00);
    cs_upd(c, ASP_TX_SZ_EN, 0x01, 0x01);
    cs_upd(c, ASP_TX_CH_EN, 0x03, 0x03);
    /* ASP in and out, mixer and ADC on (the ADC keeps FILT+ charged for the PLL); the
       headphone amplifier follows the jack */
    cs_upd(c, PWR_CTL1, 0xEC, 0x08);
    c->priv[P_HPPWR] = 1;
    mdelay(20);
    return ret;
}

static void cs_stream(struct cdc *c, int dir, int on)
{
    u32 bit = dir ? 2 : 1;
    if (on) c->priv[P_USE] |= bit;
    else c->priv[P_USE] &= ~bit;
    cs_sync(c);
}

static void cs_volume(struct cdc *c, int ctl, u32 lr)
{
    if (ctl == MX_MASTER) {
        /* mixer: 0 .. -63 dB in 1 dB steps, 0x3F mutes */
        cs_upd(c, MIXER_CHA_VOL, 0x3F, 63 - cdc_step(CDC_L(lr), 63));
        cs_upd(c, MIXER_CHB_VOL, 0x3F, 63 - cdc_step(CDC_R(lr), 63));
    } else if (ctl == MX_INGAIN) {
        /* ADC: signed dB, the -48 .. +12 dB part of it; -97 dB mutes */
        u32 g = cdc_step(CDC_L(lr), 60);
        cs_wr(c, ADC_VOLUME, g ? (g + 256 - 48) & 0xFF : 0x9F);
    }
}

static int cs_jack(struct cdc *c)
{
    int ts, s, j;
    cs_sync(c);
    ts = cs_rd(c, TSRS_PLUG_STATUS);
    if (ts < 0) return -1;
    ts = (ts >> 2) & 3;
    if (c->priv[P_DET]) {
        /* type detection running: done, or given up waiting after about 1.5 s */
        s = cs_rd(c, CODEC_STATUS);
        if ((s > 0 && (s & 0x02)) || ++c->priv[P_DET] > 5) {
            detect_done(c);
            c->priv[P_DET] = 0;
        }
    }
    if (ts == TS_PLUG) {
        if (c->priv[P_PLUG] != TS_PLUG) {
            c->priv[P_PLUG] = TS_PLUG;
            detect_start(c);
            c->priv[P_DET] = 1;
        }
    } else if (ts == TS_UNPLUG) {
        if (c->priv[P_PLUG] != TS_UNPLUG) {
            c->priv[P_PLUG] = TS_UNPLUG;
            c->priv[P_DET] = 0;
            detect_cancel(c);
        }
    } else {
        c->priv[P_PLUG] = TS_TRANS;
    }
    if (c->priv[P_PLUG] == TS_UNPLUG) j = 0;
    else if (c->priv[P_PLUG] == TS_TRANS || c->priv[P_DET]) j = c->jackstate;
    else j = c->priv[P_TYPE] == PLUG_CTIA || c->priv[P_TYPE] == PLUG_OMTP ? 3 : 1;
    hp_power(c, j & 1);
    return j;
}

static void cs_off(struct cdc *c)
{
    int i, s;
    c->priv[P_USE] = 0;
    cs_sync(c);
    cs_seq(c, irq_masks, ARRAY_SIZE(irq_masks));
    cs_seq(c, shutdown_seq, ARRAY_SIZE(shutdown_seq));
    mdelay(65);
    for (i = 0; i < 50; i++) {
        s = cs_rd(c, CODEC_STATUS);
        if (s < 0 || (s & 0x01)) break;         /* power down done */
        mdelay(5);
    }
    cs_upd(c, PWR_CTL2, 0x10, 0x10);            /* discharge FILT+ */
    if (cdc_ngpio(c)) cdc_gpio(c, 0, 0);        /* reset held */
    c->priv[P_PAGE] = 0x100;
    c->priv[P_HPPWR] = 0;
    c->priv[P_HPM] = 0;
}

const struct cdcdrv cdc_cs42l42 = { "cs42l42", { "10134242", NULL }, CK_HEADSET, 8, 8, 0,
                                    cs_probe, cs_init, cs_stream, cs_volume, NULL, cs_jack, cs_off };
