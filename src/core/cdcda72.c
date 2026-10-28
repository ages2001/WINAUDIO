/* cdcda72.c - Dialog DA7219 headset codec (Chromebooks: Gemini Lake, Comet / Jasper / Alder /
   Raptor Lake, Meteor Lake) and DA7212 / DA7213 codec (Cherry Trail tablets). Both are I2S
   consumers clocked from MCLK through their PLL in sample rate matching (SRM) mode, which
   locks the audio clock to the WCLK of the SSP. Register sequences after the Linux da7219,
   da7219-aad and da7213 drivers and the sof_da7219 / bytcht_da7213 machine drivers. */
#include "dspcdc.h"

/* ---------------------------------------------------------------- shared helpers */

/* PLL input divider: the smallest power of two that brings MCLK to 4.5 MHz or below */
static u32 pll_indiv(u32 mclk, u32 *div)
{
    u32 n = 0;
    *div = 1;
    while (n < 4 && mclk > 4500000UL * *div) {
        *div <<= 1;
        n++;
    }
    return n;
}

/* PLL feedback divider for fout from ref: integer part and 13-bit fraction */
static u32 pll_fbdiv(u32 fout, u32 ref, u32 *frac)
{
    u32 rem = fout % ref, f = 0;
    int i;
    for (i = 0; i < 13; i++) {
        rem <<= 1;
        f <<= 1;
        if (rem >= ref) {
            rem -= ref;
            f |= 1;
        }
    }
    *frac = f;
    return fout / ref;
}

/* DAI_CLK_MODE and DAI_CTRL fields (the same bits on both chips) from the SSP settings */
static void dai_bits(struct cdc *c, u8 *clkmode, u8 *ctrl, u16 *offset)
{
    u32 frame = (u32)c->ssp.slots * c->ssp.width;
    *clkmode = 0x01;                            /* 64 BCLK per WCLK */
    *ctrl = 0x08;                               /* 24 bit words */
    *offset = 0;
    if (frame == 32) *clkmode = 0x00;
    else if (frame == 128) *clkmode = 0x02;
    else if (frame == 256) *clkmode = 0x03;
    if (c->ssp.provider) *clkmode |= 0x80;      /* the codec drives BCLK / WCLK */
    switch (c->ssp.vbits) {
    case 16: *ctrl = 0x00; break;
    case 20: *ctrl = 0x04; break;
    case 32: *ctrl = 0x0C; break;
    default: break;
    }
    switch (c->ssp.fmt) {
    case 2: *ctrl |= 0x02; break;               /* right justified */
    case 3: *ctrl |= 0x01; break;               /* left justified */
    case 4:                                     /* DSP A: data one BCLK after the frame pulse */
        *offset = 1;
        /* fall through */
    case 5:                                     /* DSP: BCLK inverted to the ASoC convention */
        *ctrl |= 0x03;
        *clkmode |= 0x04;
        break;
    default: break;                             /* I2S */
    }
}

/* ---------------------------------------------------------------- DA7219 */

#define D19_MIC_1_SELECT        0x10
#define D19_CIF_CTRL            0x13
#define D19_SR                  0x17
#define D19_PLL_CTRL            0x20
#define D19_PLL_FRAC_TOP        0x22
#define D19_PLL_FRAC_BOT        0x23
#define D19_PLL_INTEGER         0x24
#define D19_PLL_SRM_STS         0x25
#define D19_DIG_ROUTING_DAI     0x2A
#define D19_DAI_CLK_MODE        0x2B
#define D19_DAI_CTRL            0x2C
#define D19_DAI_TDM_CTRL        0x2D
#define D19_DIG_ROUTING_DAC     0x2E
#define D19_DAI_OFFSET_LOWER    0x30
#define D19_DAI_OFFSET_UPPER    0x31
#define D19_REFERENCES          0x32
#define D19_MIXIN_L_SELECT      0x33
#define D19_MIXIN_L_GAIN        0x34
#define D19_ADC_L_GAIN          0x36
#define D19_MIC_1_GAIN          0x39
#define D19_ST_OUTFILT_1L       0x3C
#define D19_ST_OUTFILT_1R       0x3D
#define D19_DAC_L_GAIN          0x45
#define D19_DAC_R_GAIN          0x46
#define D19_CP_CTRL             0x47
#define D19_HP_L_GAIN           0x48
#define D19_HP_R_GAIN           0x49
#define D19_MIXOUT_L_SELECT     0x4B
#define D19_MIXOUT_R_SELECT     0x4C
#define D19_SYSTEM_MODES_INPUT  0x50
#define D19_SYSTEM_MODES_OUTPUT 0x51
#define D19_MICBIAS_CTRL        0x62
#define D19_MIC_1_CTRL          0x63
#define D19_MIXIN_L_CTRL        0x65
#define D19_ADC_L_CTRL          0x67
#define D19_DAC_L_CTRL          0x69
#define D19_DAC_R_CTRL          0x6A
#define D19_HP_L_CTRL           0x6B
#define D19_HP_R_CTRL           0x6C
#define D19_MIXOUT_L_CTRL       0x6E
#define D19_MIXOUT_R_CTRL       0x6F
#define D19_CHIP_ID1            0x81
#define D19_CHIP_ID2            0x82
#define D19_IO_CTRL             0x91
#define D19_GAIN_RAMP_CTRL      0x92
#define D19_PC_COUNT            0x94
#define D19_ACC_STATUS_A        0xC0
#define D19_ACC_EVENT_A         0xC2
#define D19_ACC_EVENT_B         0xC3
#define D19_ACC_MASK_A          0xC4
#define D19_ACC_MASK_B          0xC5
#define D19_ACC_CONFIG_1        0xC6
#define D19_ACC_CONFIG_2        0xC7
#define D19_ACC_CONFIG_3        0xC8
#define D19_ACC_CONFIG_4        0xC9
#define D19_ACC_CONFIG_5        0xCA
#define D19_ACC_CONFIG_6        0xCB
#define D19_ACC_CONFIG_7        0xCC
#define D19_SYSTEM_STATUS       0xE0
#define D19_TEST_UNLOCK         0xF0
#define D19_GND_SWITCH          0xFB
#define D19_SYSTEM_ACTIVE       0xFD

#define D19_STS_INSERTED        0x01    /* ACC_STATUS_A */
#define D19_STS_4POLE           0x02
#define D19_STS_MICBIAS_UP      0x08
#define D19_E_REMOVED           0x02    /* ACC_EVENT_A */
#define D19_E_DETECT_DONE       0x04

#define AMP_EN      0x80                /* *_CTRL registers */
#define MUTE_EN     0x40
#define RAMP_EN     0x20
#define OUT_EN      0x08                /* HP / LINE output drive; mixer enable on mixin / mixout */
#define MIN_GAIN    0x04                /* DA7219 HP */

/* priv[0]: jack 0 out / 1 detecting / 2 detected, priv[1]: polls while detecting,
   priv[2]: jack result, priv[3]: running streams (bit per direction) */

static int d19_probe(struct cdc *c)
{
    return cdc_rd(c, D19_CHIP_ID1) == 0x23 && cdc_rd(c, D19_CHIP_ID2) == 0x93 ? 0 : -1;
}

/* stop the paths through the system controller (as Linux does before a reset) */
static void d19_quiesce(struct cdc *c)
{
    int i;
    cdc_wr(c, D19_GAIN_RAMP_CTRL, 0x01);
    cdc_wr(c, D19_SYSTEM_MODES_INPUT, 0x00);
    cdc_wr(c, D19_SYSTEM_MODES_OUTPUT, 0x01);
    for (i = 0; i < 6 && cdc_rd(c, D19_SYSTEM_STATUS) > 0; i++) mdelay(50);
}

/* system clock: PLL in SRM mode from MCLK (sof_da7219), bypass for 12.288 / 24.576 MHz */
static void d19_clock(struct cdc *c)
{
    u32 mclk = c->ssp.mclk, div, ind, in, frac = 0;
    u8 mode = 0x80;                             /* SRM */
    if (!mclk || mclk < 2000000UL || mclk > 54000000UL) {
        kprintf("da7219: MCLK %lu Hz not usable, internal oscillator\n", mclk);
        cdc_wr(c, D19_PLL_CTRL, 0x00);
        return;
    }
    ind = pll_indiv(mclk, &div);
    if (mclk == 12288000UL || mclk == 24576000UL) {
        cdc_wr(c, D19_PLL_CTRL, (u8)(ind << 2));
        return;
    }
    if (c->ssp.provider) mode = 0x40;           /* normal PLL when the codec drives the clocks */
    in = pll_fbdiv(98304000UL, mclk / div, &frac);
    cdc_wr(c, D19_PLL_FRAC_TOP, (frac >> 8) & 0x1F);
    cdc_wr(c, D19_PLL_FRAC_BOT, frac & 0xFF);
    cdc_wr(c, D19_PLL_INTEGER, in & 0x7F);
    cdc_wr(c, D19_PLL_CTRL, mode | (u8)(ind << 2));
}

static void d19_hp_drive(struct cdc *c, int on)
{
    cdc_upd(c, D19_HP_L_CTRL, OUT_EN, on ? OUT_EN : 0);
    cdc_upd(c, D19_HP_R_CTRL, OUT_EN, on ? OUT_EN : 0);
}

static int d19_init(struct cdc *c)
{
    u8 clkmode, ctrl, tdm = 0x40;               /* DAI output enabled */
    u16 off;
    int a = cdc_rd(c, D19_SYSTEM_ACTIVE);
    if (a < 0) return -1;
    /* soft reset */
    if (a & 1) d19_quiesce(c);
    cdc_upd(c, D19_ACC_CONFIG_1, 0x01, 0);
    cdc_upd(c, D19_CIF_CTRL, 0x80, 0x80);
    cdc_wr(c, D19_SYSTEM_ACTIVE, 0);
    cdc_wr(c, D19_SYSTEM_ACTIVE, 1);
    mdelay(10);
    cdc_wr(c, D19_IO_CTRL, 0x00);               /* VDDIO range as Linux leaves it on ACPI */
    cdc_wr(c, D19_REFERENCES, 0x08);            /* master bias */
    cdc_wr(c, D19_MICBIAS_CTRL, 0x05);          /* 2.6 V, off until a headset is in */
    cdc_wr(c, D19_MIC_1_SELECT, 0x00);          /* differential microphone input */
    cdc_wr(c, D19_PC_COUNT, 0x03);              /* free running, auto resync */
    c->priv[0] = c->priv[3] = 0;
    /* accessory detection (the Chromebook firmware settings): 500 ohm microphone threshold,
       20 ms insertion debounce, 32-64 ms detection rate, button thresholds, no IRQ pin */
    cdc_wr(c, D19_ACC_CONFIG_1, 0xD0);
    cdc_wr(c, D19_ACC_CONFIG_2, 0x04);
    cdc_wr(c, D19_ACC_CONFIG_3, 0x0A);
    cdc_wr(c, D19_ACC_CONFIG_4, 0x16);
    cdc_wr(c, D19_ACC_CONFIG_5, 0x21);
    cdc_wr(c, D19_ACC_CONFIG_6, 0x3E);
    cdc_wr(c, D19_ACC_CONFIG_7, 0x02);
    cdc_wr(c, D19_TEST_UNLOCK, 0x8B);           /* jack insertion detect active low */
    cdc_wr(c, 0x75, 0x80);
    cdc_wr(c, D19_TEST_UNLOCK, 0x00);
    cdc_wr(c, D19_ACC_MASK_A, 0xFF);
    cdc_wr(c, D19_ACC_MASK_B, 0xFF);
    /* clocks and the I2S port: 48 kHz, 2 channels */
    cdc_wr(c, D19_SR, 0x0B);
    d19_clock(c);
    dai_bits(c, &clkmode, &ctrl, &off);
    if (c->ssp.slots > 2 && c->ssp.txmask) {
        /* TDM: the two slots from the first one of the SSP's playback mask */
        u32 m = c->ssp.txmask;
        while (!(m & 1)) {
            m >>= 1;
            off = (u16)(off + c->ssp.width);
        }
        tdm |= 0x80 | (u8)(m & 3 ? m & 3 : 1);
    }
    cdc_wr(c, D19_DAI_CLK_MODE, clkmode);
    cdc_wr(c, D19_DAI_OFFSET_LOWER, off & 0xFF);
    cdc_wr(c, D19_DAI_OFFSET_UPPER, (off >> 8) & 0x07);
    cdc_wr(c, D19_DAI_TDM_CTRL, tdm);
    cdc_wr(c, D19_DAI_CTRL, 0x80 | 0x20 | ctrl);
    /* routing: DAI L/R -> DAC L/R -> output filters -> mixout; ADC -> both DAI channels */
    cdc_wr(c, D19_DIG_ROUTING_DAC, 0x32);
    cdc_wr(c, D19_ST_OUTFILT_1L, 0x01);
    cdc_wr(c, D19_ST_OUTFILT_1R, 0x02);
    cdc_wr(c, D19_MIXOUT_L_SELECT, 0x01);
    cdc_wr(c, D19_MIXOUT_R_SELECT, 0x01);
    cdc_wr(c, D19_DIG_ROUTING_DAI, 0x00);
    cdc_wr(c, D19_MIXIN_L_SELECT, 0x01);
    /* playback power up in the DAPM order: charge pump, headphone amplifiers, DACs, mixout */
    cdc_wr(c, D19_GAIN_RAMP_CTRL, 0x01);
    cdc_wr(c, D19_DAC_L_GAIN, 0x6F);            /* 0 dB */
    cdc_wr(c, D19_DAC_R_GAIN, 0x6F);
    cdc_wr(c, D19_HP_L_GAIN, 0x39);
    cdc_wr(c, D19_HP_R_GAIN, 0x39);
    cdc_wr(c, D19_CP_CTRL, 0xA0);               /* on, tracking the DAC volume */
    mdelay(40);
    cdc_wr(c, D19_HP_L_CTRL, AMP_EN | RAMP_EN | MIN_GAIN);
    cdc_wr(c, D19_HP_R_CTRL, AMP_EN | RAMP_EN | MIN_GAIN);
    mdelay(40);
    cdc_wr(c, D19_DAC_L_CTRL, AMP_EN | MUTE_EN | RAMP_EN);
    cdc_wr(c, D19_DAC_R_CTRL, AMP_EN | MUTE_EN | RAMP_EN);
    mdelay(40);
    cdc_upd(c, D19_MIXOUT_L_CTRL, AMP_EN, AMP_EN);
    cdc_upd(c, D19_MIXOUT_R_CTRL, AMP_EN, AMP_EN);
    cdc_upd(c, D19_HP_L_CTRL, MIN_GAIN, 0);
    cdc_upd(c, D19_HP_R_CTRL, MIN_GAIN, 0);
    /* capture: MIC1 (headset) -> mic PGA -> mixin -> ADC */
    cdc_wr(c, D19_MIC_1_GAIN, 0x03);            /* +12 dB */
    cdc_wr(c, D19_MIXIN_L_GAIN, 0x03);          /* 0 dB */
    cdc_wr(c, D19_ADC_L_GAIN, 0x6F);            /* 0 dB */
    cdc_wr(c, D19_MIC_1_CTRL, AMP_EN);
    cdc_wr(c, D19_MIXIN_L_CTRL, AMP_EN | RAMP_EN | OUT_EN);
    mdelay(40);
    cdc_wr(c, D19_ADC_L_CTRL, AMP_EN | RAMP_EN);
    cdc_wr(c, D19_GAIN_RAMP_CTRL, 0x00);
    /* start accessory detection, clear old events */
    cdc_wr(c, D19_ACC_EVENT_A, 0xFF);
    cdc_wr(c, D19_ACC_EVENT_B, 0xFF);
    cdc_upd(c, D19_ACC_CONFIG_1, 0x01, 0x01);
    DBG("da7219: PLL %02x, SRM status %02x\n", cdc_rd(c, D19_PLL_CTRL), cdc_rd(c, D19_PLL_SRM_STS));
    return 0;
}

static void d19_stream(struct cdc *c, int dir, int on)
{
    u32 m = c->priv[3];
    if (on) m |= (u32)1 << dir;
    else m &= ~((u32)1 << dir);
    /* the phase counter follows the DAI while a stream runs, free running otherwise */
    if (m && !c->priv[3]) cdc_upd(c, D19_PC_COUNT, 0x01, 0x00);
    if (!m && c->priv[3]) cdc_upd(c, D19_PC_COUNT, 0x01, 0x01);
    c->priv[3] = m;
    if (!dir) {
        cdc_upd(c, D19_DAC_L_CTRL, MUTE_EN, on ? 0 : MUTE_EN);
        cdc_upd(c, D19_DAC_R_CTRL, MUTE_EN, on ? 0 : MUTE_EN);
    }
}

static void d19_volume(struct cdc *c, int ctl, u32 lr)
{
    if (ctl == MX_MASTER) {
        /* headphone amplifier: 1 dB steps, -56 .. 0 dB */
        u32 l = cdc_vstep(CDC_L(lr), 0x39, 1000), r = cdc_vstep(CDC_R(lr), 0x39, 1000);
        cdc_wr(c, D19_HP_L_GAIN, l);
        cdc_wr(c, D19_HP_R_GAIN, r);
        cdc_upd(c, D19_HP_L_CTRL, MUTE_EN, l ? 0 : MUTE_EN);
        cdc_upd(c, D19_HP_R_CTRL, MUTE_EN, r ? 0 : MUTE_EN);
    } else if (ctl == MX_INGAIN) {
        u32 g = cdc_step(CDC_L(lr), 7);         /* mic PGA 0 .. +36 dB in 6 dB steps */
        if (g) cdc_wr(c, D19_MIC_1_GAIN, g);
        cdc_upd(c, D19_ADC_L_CTRL, MUTE_EN, g ? 0 : MUTE_EN);
    }
}

/* accessory detection, polled: insertion, then 3 / 4 pole once the detection completed */
static int d19_jack(struct cdc *c)
{
    int ev = cdc_rd(c, D19_ACC_EVENT_A), evb = cdc_rd(c, D19_ACC_EVENT_B);
    int st = cdc_rd(c, D19_ACC_STATUS_A), i;
    if (ev < 0 || evb < 0 || st < 0) return -1;
    if (ev) cdc_wr(c, D19_ACC_EVENT_A, (u32)ev);
    if (evb) cdc_wr(c, D19_ACC_EVENT_B, (u32)evb);
    if (c->priv[0] && (!(st & D19_STS_INSERTED) || (ev & D19_E_REMOVED))) {
        /* removed: stop driving the headphones, mic bias and ground switch off */
        d19_hp_drive(c, 0);
        cdc_upd(c, D19_MICBIAS_CTRL, 0x08, 0x00);
        cdc_upd(c, D19_GND_SWITCH, 0x01, 0x00);
        c->priv[0] = 0;
    }
    if (!(st & D19_STS_INSERTED)) return 0;
    if (!c->priv[0]) {
        c->priv[0] = 1;
        c->priv[1] = 0;
    }
    if (c->priv[0] == 1) {
        if (!(ev & D19_E_DETECT_DONE) && ++c->priv[1] < 4) {
            cdc_upd(c, D19_GND_SWITCH, 0x01, 0x01);     /* type detection still running */
            return 0;
        }
        cdc_upd(c, D19_GND_SWITCH, 0x01, 0x00);
        d19_hp_drive(c, 1);
        c->priv[2] = 1;
        if (st & D19_STS_4POLE) {
            cdc_upd(c, D19_MICBIAS_CTRL, 0x08, 0x08);
            for (i = 0; i < 5 && !(cdc_rd(c, D19_ACC_STATUS_A) & D19_STS_MICBIAS_UP); i++) mdelay(10);
            c->priv[2] = 3;
        }
        c->priv[0] = 2;
    }
    return (int)c->priv[2];
}

static void d19_off(struct cdc *c)
{
    cdc_upd(c, D19_ACC_CONFIG_1, 0x01, 0x00);
    cdc_upd(c, D19_GND_SWITCH, 0x01, 0x00);
    d19_hp_drive(c, 0);
    d19_quiesce(c);
    cdc_wr(c, D19_MICBIAS_CTRL, 0x05);
    cdc_wr(c, D19_DAI_CTRL, 0x28);
    cdc_wr(c, D19_PLL_CTRL, 0x00);
    cdc_wr(c, D19_REFERENCES, 0x00);
    cdc_wr(c, D19_SYSTEM_ACTIVE, 0);
}

const struct cdcdrv cdc_da7219 = { "da7219", { "DLGS7219", NULL }, CK_HEADSET, 8, 8, 0,
                                   d19_probe, d19_init, d19_stream, d19_volume, NULL, d19_jack, d19_off };

/* ---------------------------------------------------------------- DA7212 / DA7213 */

#define D13_PLL_STATUS          0x03
#define D13_DIG_ROUTING_DAI     0x21
#define D13_SR                  0x22
#define D13_REFERENCES          0x23
#define D13_PLL_FRAC_TOP        0x24
#define D13_PLL_FRAC_BOT        0x25
#define D13_PLL_INTEGER         0x26
#define D13_PLL_CTRL            0x27
#define D13_DAI_CLK_MODE        0x28
#define D13_DAI_CTRL            0x29
#define D13_DIG_ROUTING_DAC     0x2A
#define D13_MIXIN_L_SELECT      0x32
#define D13_MIXIN_R_SELECT      0x33
#define D13_MIXIN_L_GAIN        0x34
#define D13_MIXIN_R_GAIN        0x35
#define D13_ADC_L_GAIN          0x36
#define D13_ADC_R_GAIN          0x37
#define D13_MIC_1_GAIN          0x39
#define D13_MIC_2_GAIN          0x3A
#define D13_DAC_L_GAIN          0x45
#define D13_DAC_R_GAIN          0x46
#define D13_CP_CTRL             0x47
#define D13_HP_L_GAIN           0x48
#define D13_HP_R_GAIN           0x49
#define D13_LINE_GAIN           0x4A
#define D13_MIXOUT_L_SELECT     0x4B
#define D13_MIXOUT_R_SELECT     0x4C
#define D13_SYSTEM_MODES_INPUT  0x50
#define D13_SYSTEM_MODES_OUTPUT 0x51
#define D13_MICBIAS_CTRL        0x62
#define D13_MIC_1_CTRL          0x63
#define D13_MIC_2_CTRL          0x64
#define D13_MIXIN_L_CTRL        0x65
#define D13_MIXIN_R_CTRL        0x66
#define D13_ADC_L_CTRL          0x67
#define D13_ADC_R_CTRL          0x68
#define D13_DAC_L_CTRL          0x69
#define D13_DAC_R_CTRL          0x6A
#define D13_HP_L_CTRL           0x6B
#define D13_HP_R_CTRL           0x6C
#define D13_LINE_CTRL           0x6D
#define D13_MIXOUT_L_CTRL       0x6E
#define D13_MIXOUT_R_CTRL       0x6F
#define D13_GAIN_RAMP_CTRL      0x92
#define D13_PC_COUNT            0x94
#define D13_DAI_OFFSET          0x98

/* priv[3]: running streams (bit per direction) */

static int d13_probe(struct cdc *c)
{
    /* no id register: the chip answers and the PLL control register reads back */
    return cdc_rd(c, D13_PLL_CTRL) < 0 ? -1 : 0;
}

/* system clock: PLL in SRM mode (94.3104 MHz) from MCLK, bypass for 12.288 / 24.576 / 49.152 MHz;
   the Cherry Trail machine driver runs the codec from the 19.2 MHz platform clock */
static void d13_clock(struct cdc *c)
{
    u32 mclk = c->ssp.mclk, div, ind, in, frac = 0;
    u8 mode = 0xC0;                             /* PLL on, SRM */
    if (!mclk) mclk = 19200000UL;
    if (mclk < 5000000UL || mclk > 54000000UL) {
        kprintf("da7213: MCLK %lu Hz out of the PLL range\n", mclk);
        return;
    }
    ind = pll_indiv(mclk, &div);
    if (div < 2) {
        div = 2;
        ind = 1;
    }
    if (mclk == 12288000UL || mclk == 24576000UL || mclk == 49152000UL) {
        cdc_upd(c, D13_PLL_CTRL, 0xFC, (ind - 1) << 2);
        return;
    }
    if (c->ssp.provider) mode = 0x80;           /* normal PLL when the codec drives the clocks */
    in = pll_fbdiv(mode & 0x40 ? 94310400UL : 98304000UL, mclk / div, &frac);
    cdc_wr(c, D13_PLL_FRAC_TOP, (frac >> 8) & 0x1F);
    cdc_wr(c, D13_PLL_FRAC_BOT, frac & 0xFF);
    cdc_wr(c, D13_PLL_INTEGER, in & 0x7F);
    cdc_upd(c, D13_PLL_CTRL, 0xFC, mode | ((ind - 1) << 2));   /* MCLK not squared */
}

static int d13_init(struct cdc *c)
{
    u8 clkmode, ctrl;
    u16 off;
    if (cdc_rd(c, D13_PLL_CTRL) < 0) return -1;
    /* no reset: everything off first, manual (non system controller) power control */
    cdc_wr(c, D13_SYSTEM_MODES_INPUT, 0x00);
    cdc_wr(c, D13_SYSTEM_MODES_OUTPUT, 0x00);
    cdc_upd(c, D13_DAI_CTRL, 0x80, 0x00);
    cdc_upd(c, D13_PLL_CTRL, 0xE0, 0x00);
    cdc_wr(c, D13_REFERENCES, 0x88);            /* VMID and master bias */
    mdelay(30);
    cdc_wr(c, D13_PC_COUNT, 0x01);              /* phase counter free running */
    c->priv[3] = 0;
    /* clocks and the I2S port: 48 kHz */
    cdc_wr(c, D13_SR, 0x0B);
    d13_clock(c);
    dai_bits(c, &clkmode, &ctrl, &off);
    cdc_wr(c, D13_DAI_CLK_MODE, clkmode);
    cdc_wr(c, D13_DAI_OFFSET, off);
    cdc_wr(c, D13_DAI_CTRL, 0x80 | ctrl);
    /* routing: DAI L/R -> DAC L/R -> mixout L/R -> headphones; mixout R -> line out */
    cdc_wr(c, D13_DIG_ROUTING_DAC, 0x32);
    cdc_wr(c, D13_MIXOUT_L_SELECT, 0x08);
    cdc_wr(c, D13_MIXOUT_R_SELECT, 0x08);
    cdc_wr(c, D13_DIG_ROUTING_DAI, 0x10);       /* ADC L / R -> DAI L / R */
    /* playback power up */
    cdc_wr(c, D13_GAIN_RAMP_CTRL, 0x01);
    cdc_wr(c, D13_DAC_L_GAIN, 0x6F);            /* 0 dB */
    cdc_wr(c, D13_DAC_R_GAIN, 0x6F);
    cdc_wr(c, D13_HP_L_GAIN, 0x39);             /* 0 dB */
    cdc_wr(c, D13_HP_R_GAIN, 0x39);
    cdc_wr(c, D13_LINE_GAIN, 0x30);             /* 0 dB */
    cdc_upd(c, D13_CP_CTRL, 0x80, 0x80);
    cdc_upd(c, D13_DAC_L_CTRL, 0xE0, AMP_EN | MUTE_EN | RAMP_EN);
    cdc_upd(c, D13_DAC_R_CTRL, 0xE0, AMP_EN | MUTE_EN | RAMP_EN);
    cdc_upd(c, D13_MIXOUT_L_CTRL, 0x88, AMP_EN | OUT_EN);
    cdc_upd(c, D13_MIXOUT_R_CTRL, 0x88, AMP_EN | OUT_EN);
    cdc_upd(c, D13_HP_L_CTRL, 0xE8, AMP_EN | RAMP_EN | OUT_EN);
    cdc_upd(c, D13_HP_R_CTRL, 0xE8, AMP_EN | RAMP_EN | OUT_EN);
    cdc_upd(c, D13_LINE_CTRL, 0xE8, AMP_EN | RAMP_EN | OUT_EN);
    /* capture: MIC1 (headset) and MIC2 (internal) differential -> both mixins -> ADC L / R */
    cdc_wr(c, D13_MICBIAS_CTRL, 0x99);          /* both biases on, 2.2 V */
    cdc_wr(c, D13_MIC_1_GAIN, 0x03);            /* +12 dB */
    cdc_wr(c, D13_MIC_2_GAIN, 0x03);
    cdc_upd(c, D13_MIC_1_CTRL, 0xCC, AMP_EN);
    cdc_upd(c, D13_MIC_2_CTRL, 0xCC, AMP_EN);
    cdc_wr(c, D13_MIXIN_L_SELECT, 0x06);
    cdc_wr(c, D13_MIXIN_R_SELECT, 0x06);
    cdc_wr(c, D13_MIXIN_L_GAIN, 0x03);          /* 0 dB */
    cdc_wr(c, D13_MIXIN_R_GAIN, 0x03);
    cdc_upd(c, D13_MIXIN_L_CTRL, 0xE8, AMP_EN | RAMP_EN | OUT_EN);
    cdc_upd(c, D13_MIXIN_R_CTRL, 0xE8, AMP_EN | RAMP_EN | OUT_EN);
    cdc_wr(c, D13_ADC_L_GAIN, 0x6F);            /* 0 dB */
    cdc_wr(c, D13_ADC_R_GAIN, 0x6F);
    cdc_upd(c, D13_ADC_L_CTRL, 0xE0, AMP_EN | RAMP_EN);
    cdc_upd(c, D13_ADC_R_CTRL, 0xE0, AMP_EN | RAMP_EN);
    cdc_wr(c, D13_GAIN_RAMP_CTRL, 0x00);
    DBG("da7213: PLL %02x, status %02x\n", cdc_rd(c, D13_PLL_CTRL), cdc_rd(c, D13_PLL_STATUS));
    return 0;
}

static void d13_stream(struct cdc *c, int dir, int on)
{
    u32 m = c->priv[3];
    if (on) m |= (u32)1 << dir;
    else m &= ~((u32)1 << dir);
    if (m && !c->priv[3]) cdc_upd(c, D13_PC_COUNT, 0x01, 0x00);
    if (!m && c->priv[3]) cdc_upd(c, D13_PC_COUNT, 0x01, 0x01);
    c->priv[3] = m;
    if (!dir) {
        cdc_upd(c, D13_DAC_L_CTRL, MUTE_EN, on ? 0 : MUTE_EN);
        cdc_upd(c, D13_DAC_R_CTRL, MUTE_EN, on ? 0 : MUTE_EN);
    }
}

static void d13_volume(struct cdc *c, int ctl, u32 lr)
{
    if (ctl == MX_MASTER) {
        /* DAC digital gain (headphones and line out): 0.75 dB steps, -77.25 .. 0 dB, 0 mute */
        u32 l = cdc_vstep(CDC_L(lr), 104, 750), r = cdc_vstep(CDC_R(lr), 104, 750);
        cdc_wr(c, D13_DAC_L_GAIN, l ? l + 7 : 0);
        cdc_wr(c, D13_DAC_R_GAIN, r ? r + 7 : 0);
    } else if (ctl == MX_INGAIN) {
        u32 g = cdc_step(CDC_L(lr), 7);         /* mic PGAs 0 .. +36 dB in 6 dB steps */
        if (g) {
            cdc_wr(c, D13_MIC_1_GAIN, g);
            cdc_wr(c, D13_MIC_2_GAIN, g);
        }
        cdc_upd(c, D13_ADC_L_CTRL, MUTE_EN, g ? 0 : MUTE_EN);
        cdc_upd(c, D13_ADC_R_CTRL, MUTE_EN, g ? 0 : MUTE_EN);
    }
}

static void d13_off(struct cdc *c)
{
    cdc_upd(c, D13_DAC_L_CTRL, MUTE_EN, MUTE_EN);
    cdc_upd(c, D13_DAC_R_CTRL, MUTE_EN, MUTE_EN);
    mdelay(20);
    cdc_upd(c, D13_HP_L_CTRL, 0x88, 0x00);
    cdc_upd(c, D13_HP_R_CTRL, 0x88, 0x00);
    cdc_upd(c, D13_LINE_CTRL, 0x88, 0x00);
    cdc_upd(c, D13_MIXOUT_L_CTRL, AMP_EN, 0x00);
    cdc_upd(c, D13_MIXOUT_R_CTRL, AMP_EN, 0x00);
    cdc_upd(c, D13_DAC_L_CTRL, AMP_EN, 0x00);
    cdc_upd(c, D13_DAC_R_CTRL, AMP_EN, 0x00);
    cdc_upd(c, D13_CP_CTRL, 0x80, 0x00);
    cdc_upd(c, D13_ADC_L_CTRL, AMP_EN, 0x00);
    cdc_upd(c, D13_ADC_R_CTRL, AMP_EN, 0x00);
    cdc_upd(c, D13_MIXIN_L_CTRL, AMP_EN, 0x00);
    cdc_upd(c, D13_MIXIN_R_CTRL, AMP_EN, 0x00);
    cdc_upd(c, D13_MIC_1_CTRL, AMP_EN, 0x00);
    cdc_upd(c, D13_MIC_2_CTRL, AMP_EN, 0x00);
    cdc_upd(c, D13_MICBIAS_CTRL, 0x88, 0x00);
    cdc_upd(c, D13_DAI_CTRL, 0x80, 0x00);
    cdc_upd(c, D13_PLL_CTRL, 0xE0, 0x00);
    cdc_upd(c, D13_REFERENCES, 0x88, 0x00);
}

const struct cdcdrv cdc_da7213 = { "da7213", { "DLGS7213", "DLGS7212", NULL }, CK_SPKCODEC, 8, 8, 0,
                                   d13_probe, d13_init, d13_stream, d13_volume, NULL, NULL, d13_off };
