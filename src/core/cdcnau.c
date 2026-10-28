/* cdcnau.c - Nuvoton codecs and amplifiers on the I2S ports of the Intel DSP:
   NAU88L25 (nau8825, Chromebooks: headset codec, speakers on a separate amplifier),
   NAU88L21 (nau8821, headset codec), NAU88L24 (nau8824, Cherry Trail tablets: speaker
   amplifier, headphones, internal and headset microphones) and the NAU8315 / NAU8318
   class-D amplifiers that only have an enable pin. 16-bit registers, 16-bit values.
   Register sequences after the Linux nau8825, nau8821, nau8824, nau8315 drivers and the
   sof_nau8825 / cht_bsw_nau8824 machine drivers. The jack interrupts are not wired up:
   the IRQ status registers are polled. */
#include "dspcdc.h"

/* registers at the same address on the three codecs */
#define RESET           0x00
#define ENA_CTRL        0x01
#define CLK_DIV         0x03
#define FLL1            0x04
#define FLL2            0x05
#define FLL3            0x06
#define FLL4            0x07
#define FLL5            0x08
#define FLL6            0x09
#define JACK_DET        0x0D
#define IRQ_MASK        0x0F            /* nau8824: INTERRUPT_SETTING_1 */
#define IRQ_STATUS      0x10
#define IRQ_CLEAR       0x11
#define IRQ_DIS         0x12            /* nau8824: INTERRUPT_SETTING */
#define SAR_CTRL        0x13
#define GPIO12          0x1A
#define I2S_CTRL1       0x1C
#define I2S_CTRL2       0x1D
#define LEFT_SLOT       0x1E
#define DEVICE_ID       0x58
#define SAR_DATA        0x59
#define BIAS_ADJ        0x66
#define TRIM            0x68
#define ANA_CTRL1       0x69
#define ANA_CTRL2       0x6A
#define ANA_ADC1        0x71
#define ANA_ADC2        0x72
#define RDAC            0x73
#define MIC_BIAS        0x74
#define BOOST           0x76
#define FEPGA           0x77
#define PWR_UP          0x7F
#define CHARGE_PUMP     0x80
#define GEN_STATUS      0x82

/* nau8825 */
#define N25_IIC_ADDR    0x02
#define N25_HSD_CTRL    0x0C
#define N25_ADC_RATE    0x2B
#define N25_DAC_CTRL1   0x2C
#define N25_ADC_DGAIN   0x30
#define N25_DACL_CTRL   0x33
#define N25_DACR_CTRL   0x34
#define N25_CLASSG      0x50
#define N25_FLL2_LOWER  0x5A
#define N25_FLL2_UPPER  0x5B

/* nau8821 */
#define N21_FLL7        0x0A
#define N21_FLL8        0x0B
#define N21_ADC_RATE    0x2B
#define N21_DAC_CTRL1   0x2C
#define N21_MUTE_CTRL   0x31
#define N21_DAC_VOL     0x34
#define N21_ADC_VOL     0x35
#define N21_CLASSG      0x4B
#define N21_PGA_GAIN    0x7E

/* nau8824 */
#define N24_CLK_GATING  0x02
#define N24_TDM_CTRL    0x20
#define N24_ADC_FILTER  0x24
#define N24_DAC_FILTER  0x25
#define N24_ADC0_DGAIN  0x2D
#define N24_ADC1_DGAIN  0x2E
#define N24_DAC_MUTE    0x31
#define N24_DAC0_DGAIN  0x32
#define N24_DAC1_DGAIN  0x33
#define N24_CLASSG      0x50
#define N24_ENABLE_LO   0x6B
#define N24_CLASSD1     0x6D
#define N24_CLASSD2     0x6E
#define N24_FEPGA2      0x78

/* chips */
#define T25             1
#define T21             2
#define T24             3

/* c->priv[] */
#define P_CHIP          0               /* T25 / T21 / T24 */
#define P_REV           1               /* nau8825: software id (1: rev C) */
#define P_STRM          2               /* running streams: bit 0 playback, bit 1 capture */
#define P_JACK          3               /* 0 empty, 1 type detection running, 2 headphones, 3 headset */
#define P_CNT           4               /* polls in the current jack state */
#define P_MISS          5               /* polls with the jack pin reading "empty" */
#define P_VOL           6               /* MX_MASTER level (left | right << 16) */

#define FLL_SRC_MCLK    0
#define FLL_SRC_BCLK    2
#define FLL_SRC_FS      3

/* ---------------------------------------------------------------- shared helpers */

static void irq_clear_all(struct cdc *c)
{
    int st = cdc_rd(c, IRQ_STATUS), i;
    if (st <= 0) return;
    for (i = 0; i < 16; i++)
        if (st & (1 << i)) cdc_wr(c, IRQ_CLEAR, 1UL << i);
}

struct fll { u32 src, ratio, fint, frac, refdiv; };

/* FLL parameters for 256 * 48 kHz out of fin (the Linux nau88xx_calc_fll_param):
   FREF = fin / 1, 2, 4, 8 <= 13.5 MHz, ratio by FREF, FDCO = 512 * Fs * scaling as high as
   the VCO allows, FDCO / (FREF * ratio) as 10.fbits fixed point */
static int fll_calc(u32 fin, u32 vmax, int nscale, int fbits, struct fll *f)
{
    static const u8 sc_div[13] = { 1, 2, 4, 8, 16, 32, 3, 6, 12, 24, 48, 96, 5 };
    static const u8 sc_val[13] = { 0x0, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0xA, 0xB, 0xC, 0xD, 0xE, 0xF };
    static const u32 ratio_min[7] = { 512000UL, 256000UL, 128000UL, 64000UL, 32000UL, 8000UL, 4000UL };
    u32 fref = 0, v, vbest = 0, den, r, frac = 0;
    int i;
    for (i = 0; i < 4; i++) {
        fref = fin >> i;
        if (fref <= 13500000UL) break;
    }
    if (i == 4) return -1;
    f->refdiv = (u32)i;
    for (i = 0; i < 7; i++)
        if (fref >= ratio_min[i]) break;
    if (i == 7) return -1;
    f->ratio = 1UL << i;
    f->src = 0;
    for (i = 0; i < nscale; i++) {
        v = 256UL * CDC_RATE * 2 * sc_div[i];
        if (v > 90000000UL && v < vmax && v > vbest) {
            vbest = v;
            f->src = sc_val[i];
        }
    }
    if (!vbest) return -1;
    den = fref * f->ratio;
    f->fint = (vbest / den) & 0x3FF;
    r = vbest % den;
    for (i = 0; i < fbits; i++) {           /* (r << fbits) / den without 64-bit math */
        r <<= 1;
        frac <<= 1;
        if (r >= den) {
            r -= den;
            frac |= 1;
        }
    }
    f->frac = frac;
    return 0;
}

/* system clock from the FLL: BCLK (or FS on the nau8824, as the Cherry Trail machine driver
   does) when the SSP drives the clocks, else MCLK */
static int nau_fll(struct cdc *c)
{
    u32 t = c->priv[P_CHIP], fin, src, sdm;
    struct fll f;
    if (c->ssp.provider) {
        src = FLL_SRC_MCLK;
        fin = c->ssp.mclk ? c->ssp.mclk : c->mclk;
    } else if (t == T24) {
        src = FLL_SRC_FS;
        fin = CDC_RATE;
    } else if (c->ssp.bclk) {
        src = FLL_SRC_BCLK;
        fin = c->ssp.bclk;
    } else {
        src = FLL_SRC_MCLK;
        fin = c->ssp.mclk ? c->ssp.mclk : c->mclk;
    }
    if (!fin || fll_calc(fin, t == T25 ? 124000000UL : 100000000UL, t == T24 ? 10 : 13,
                         t == T21 || (t == T25 && c->priv[P_REV]) ? 24 : 16, &f)) {
        kprintf("%s: no FLL setting for a %lu Hz reference\n", c->drv->name, fin);
        return -1;
    }
    DBG("%s: FLL from %s %lu Hz: int %lu frac %lx ratio %lu refdiv %lu scale %lx\n", c->drv->name,
        src == FLL_SRC_BCLK ? "BCLK" : src == FLL_SRC_FS ? "FS" : "MCLK", fin, f.fint, f.frac,
        f.ratio, f.refdiv, f.src);
    /* reference: low frequency sources get the most sensitive gain error correction */
    if (t == T24) cdc_upd(c, FLL3, 0x0C00, src << 10);
    else cdc_upd(c, FLL3, 0xFC00, (src == FLL_SRC_MCLK ? 0 : 0xF000) | (src << 10));
    /* MCLK as system clock while the FLL is set up; DAC and ADC clocks 1/4 (OSR 64) */
    cdc_upd(c, CLK_DIV, 0x80FF, 0xA0 | f.src);
    cdc_upd(c, FLL1, t == T24 ? 0x7F : 0x1C7F, f.ratio | (t == T24 ? 0 : 6UL << 10));
    if (t == T21) {
        cdc_wr(c, N21_FLL7, (f.frac >> 16) & 0xFF);
        cdc_wr(c, N21_FLL8, f.frac & 0xFFFF);
    } else if (t == T25 && c->priv[P_REV]) {
        cdc_wr(c, N25_FLL2_LOWER, f.frac & 0xFFFF);
        cdc_wr(c, N25_FLL2_UPPER, (f.frac >> 16) & 0xFF);
    } else {
        cdc_wr(c, FLL2, f.frac);
    }
    cdc_upd(c, FLL3, 0x3FF, f.fint);
    cdc_upd(c, FLL4, t == T21 ? 0x8C00 : 0x0C00, (t == T21 ? 0x8000 : 0) | (f.refdiv << 10));
    cdc_upd(c, FLL5, 0x2000, 0);                /* divided VCO input */
    cdc_upd(c, FLL6, 0x8000, 0);                /* no free running */
    sdm = t == T24 ? 0x4000 : 0x6000;
    if (f.frac) {
        cdc_upd(c, FLL5, 0xD000, 0xC000);       /* loop filter, 500 kHz cutoff */
        cdc_upd(c, FLL6, sdm, sdm);
    } else {
        cdc_upd(c, FLL5, 0xD000, 0x1000);
        cdc_upd(c, FLL6, sdm, 0);
    }
    mdelay(2);
    cdc_upd(c, CLK_DIV, 0x8000, 0x8000);        /* system clock from the VCO */
    return 0;
}

/* no system clock (the jack detection works without one until a jack is in) */
static void clk_off(struct cdc *c)
{
    cdc_upd(c, CLK_DIV, 0x8000, 0);
    cdc_upd(c, FLL6, 0x8000, 0);
    if (c->priv[P_CHIP] != T24) cdc_upd(c, FLL1, 0x1C00, 0);
}

/* free running VCO: the clock the jack and microphone detection need */
static void clk_internal(struct cdc *c)
{
    cdc_upd(c, FLL6, 0x8000, 0x8000);
    cdc_upd(c, CLK_DIV, 0x8000, 0x8000);
    if (c->priv[P_CHIP] != T24) {
        cdc_upd(c, CLK_DIV, 0x000F, 0x000F);
        cdc_upd(c, FLL1, 0x1C7F, 0x0010);
        cdc_upd(c, FLL6, 0x4000, 0x4000);
    }
}

/* clock when no stream runs */
static void clk_idle(struct cdc *c)
{
    if (!c->priv[P_JACK]) {
        clk_off(c);
        return;
    }
    clk_internal(c);
    if (c->priv[P_CHIP] == T25 && c->priv[P_JACK] == 1) cdc_upd(c, CLK_DIV, 0x000F, 0);
}

/* I2S port: format, word length, consumer (or provider with the BCLK and LRCK dividers) */
static void nau_i2s(struct cdc *c)
{
    u32 v, n, bfs = c->ssp.bclk / CDC_RATE;
    switch (c->ssp.fmt) {
    case 2: v = 0x00; break;                    /* right justified */
    case 3: v = 0x01; break;                    /* left justified */
    case 4: v = 0x03; break;                    /* DSP A */
    case 5: v = 0x43; break;                    /* DSP B */
    default: v = 0x02; break;                   /* I2S */
    }
    switch (c->ssp.vbits) {
    case 16: break;
    case 20: v |= 1 << 2; break;
    case 32: v |= 3 << 2; break;
    default: v |= 2 << 2; break;
    }
    cdc_upd(c, I2S_CTRL1, 0xCF, v);
    if (!c->ssp.provider) {
        cdc_upd(c, I2S_CTRL2, 0x0008, 0);
        return;
    }
    n = bfs <= 32 ? 3 : bfs <= 64 ? 2 : bfs <= 128 ? 1 : 0;
    if (c->priv[P_CHIP] == T25) v = (n << 12) | (n ? n - 1 : 0);
    else v = (n << 12) | n;
    cdc_upd(c, I2S_CTRL2, 0x300F, 0x0008 | v);
}

static int stream_change(struct cdc *c, int dir, int on)
{
    u32 bit = dir ? 2 : 1;
    if (!on == !(c->priv[P_STRM] & bit)) return 0;
    if (on) {
        if (!c->priv[P_STRM] && nau_fll(c)) return 0;
        c->priv[P_STRM] |= bit;
    } else {
        c->priv[P_STRM] &= ~bit;
    }
    return 1;
}

/* DAC digital volume as 0.5 dB steps below 0 dB at reg0db, the upper 64 dB; 0 mutes */
static u32 dac_vol(u32 level, u32 reg0db)
{
    u32 s = cdc_vstep(level, 128, 500);
    return s ? reg0db - (128 - s) : 0;
}

static int nau_probe_id(struct cdc *c, u32 chip)
{
    int v = cdc_rd(c, DEVICE_ID);
    if (v < 0 || v == 0xFFFF) return -1;
    if (chip == T25 && (v & 3) > 1) return -1;      /* software id: NAU88L25 / rev C */
    c->priv[P_CHIP] = chip;
    c->priv[P_VOL] = 0xFFFFFFFFUL;
    return 0;
}

/* ---------------------------------------------------------------- NAU88L25 */

static const u16 n25_revc_patch[] = {
    FLL2, 0x0000, FLL4, 0x8010, 0x0A, 0x0BC0, IRQ_MASK, 0x0800,
    N25_DACL_CTRL, 0x00CF, N25_DACR_CTRL, 0x02CF, 0x51, 0x0400,
    N25_FLL2_LOWER, 0x26E9, N25_FLL2_UPPER, 0x0031, ANA_CTRL2, 0x0020,
    ANA_ADC2, 0x0220, MIC_BIAS, 0x0046
};

static int n25_probe(struct cdc *c)
{
    return nau_probe_id(c, T25);
}

/* jack pin against its polarity */
static int n25_inserted(struct cdc *c)
{
    int j = cdc_rd(c, JACK_DET), s = cdc_rd(c, DEVICE_ID);
    if (j < 0 || s < 0) return -1;
    return !(j & 0x02) == !(s & 0x80);
}

static void n25_set_vol(struct cdc *c, u32 lr)
{
    cdc_upd(c, N25_DACL_CTRL, 0xFF, dac_vol(CDC_L(lr), 0xCF));
    cdc_upd(c, N25_DACR_CTRL, 0xFF, dac_vol(CDC_R(lr), 0xCF));
}

/* DAC -> headphones, in the order of the Linux DAPM sequence */
static void n25_play(struct cdc *c, int on)
{
    u32 pdac = c->priv[P_REV] ? 0x0300 : 0;     /* DAC power bits are inverted on rev C */
    if (on) {
        cdc_upd(c, ENA_CTRL, 0x0640, 0x0640);   /* DAC clock, DACL, DACR */
        cdc_upd(c, N25_CLASSG, 0x0006, 0x0006); /* headphone amplifiers */
        cdc_upd(c, CHARGE_PUMP, 0x0020, 0x0020);
        mdelay(10);
        cdc_upd(c, CHARGE_PUMP, 0x0400, 0x0400);
        cdc_upd(c, RDAC, 0x3000, 0x3000);       /* analog DACs, then their clocks */
        cdc_upd(c, RDAC, 0x0300, 0x0300);
        cdc_upd(c, PWR_UP, 0x0030, 0x0030);     /* output driver stages 1, 2, 3 */
        cdc_upd(c, PWR_UP, 0x000C, 0x000C);
        cdc_upd(c, PWR_UP, 0x0003, 0x0003);
        cdc_upd(c, BIAS_ADJ, 0x0300, 0);        /* test DAC off: the DAC signal passes */
        cdc_upd(c, CHARGE_PUMP, 0x0300, pdac);
        cdc_upd(c, N25_HSD_CTRL, 0x0003, 0);    /* HPOL / HPOR pull-downs off */
        cdc_upd(c, BOOST, 0x0200, 0);           /* boost driver */
        cdc_upd(c, N25_CLASSG, 0x0001, 0x0001); /* class G */
        n25_set_vol(c, c->priv[P_VOL]);
    } else {
        n25_set_vol(c, 0);
        cdc_upd(c, N25_CLASSG, 0x0001, 0);
        cdc_upd(c, BOOST, 0x0200, 0x0200);
        cdc_upd(c, N25_HSD_CTRL, 0x0003, 0x0003);
        cdc_upd(c, BIAS_ADJ, 0x0300, 0x0300);
        cdc_upd(c, CHARGE_PUMP, 0x0300, pdac ^ 0x0300);
        cdc_upd(c, PWR_UP, 0x0003, 0);
        cdc_upd(c, PWR_UP, 0x000C, 0);
        cdc_upd(c, PWR_UP, 0x0030, 0);
        cdc_upd(c, RDAC, 0x0300, 0);
        cdc_upd(c, RDAC, 0x3000, 0);
        cdc_upd(c, CHARGE_PUMP, 0x0400, 0);
        cdc_upd(c, CHARGE_PUMP, 0x0020, 0);
        cdc_upd(c, N25_CLASSG, 0x0006, 0);
        cdc_upd(c, ENA_CTRL, 0x0640, 0);
    }
}

/* headset microphone -> frontend PGA -> ADC -> I2S */
static void n25_rec(struct cdc *c, int on)
{
    if (on) {
        cdc_upd(c, ENA_CTRL, 0x0080, 0x0080);   /* ADC clock */
        cdc_upd(c, ANA_ADC2, 0x0040, 0x0040);   /* ADC power */
        cdc_upd(c, I2S_CTRL2, 0x8000, 0);       /* ADC data out of tristate */
        cdc_upd(c, PWR_UP, 0x4000, 0x4000);     /* frontend PGA, its input capacitors charged */
        cdc_upd(c, FEPGA, 0xC000, 0xC000);
        cdc_upd(c, BOOST, 0x0800, 0x0800);
        mdelay(40);
        cdc_upd(c, BOOST, 0x0800, 0);
        cdc_upd(c, FEPGA, 0xC000, 0);
        mdelay(125);
        cdc_upd(c, ENA_CTRL, 0x0100, 0x0100);   /* ADC */
    } else {
        if (!c->priv[P_JACK]) cdc_upd(c, ENA_CTRL, 0x0100, 0);    /* else the jack detection uses it */
        cdc_upd(c, PWR_UP, 0x4000, 0);
        cdc_upd(c, I2S_CTRL2, 0x8000, 0x8000);
        cdc_upd(c, ANA_ADC2, 0x0040, 0);
        cdc_upd(c, ENA_CTRL, 0x0080, 0);
    }
}

static void n25_micbias(struct cdc *c, int on)
{
    cdc_upd(c, MIC_BIAS, 0x0100, on ? 0x0100 : 0);
    cdc_upd(c, SAR_CTRL, 0x1000, on ? 0x1000 : 0);
}

/* jack out: microphone detection resistors off, outputs and MICGND grounded, insertion
   detection without debounce */
static void n25_eject(struct cdc *c)
{
    n25_micbias(c, 0);
    cdc_upd(c, MIC_BIAS, 0x5000, 0);
    cdc_upd(c, N25_HSD_CTRL, 0x000F, 0x000F);
    irq_clear_all(c);
    cdc_upd(c, IRQ_DIS, 0x0005, 0x0004);
    cdc_upd(c, IRQ_MASK, 0x0C05, 0x0C04);
    cdc_upd(c, JACK_DET, 0x0100, 0x0100);
    if (!(c->priv[P_STRM] & 2)) cdc_upd(c, ENA_CTRL, 0x0100, 0);
    c->priv[P_JACK] = 0;
    if (!c->priv[P_STRM]) clk_off(c);
}

/* jack in: automatic headset type detection (HSD), its completion polled */
static void n25_detect(struct cdc *c)
{
    cdc_upd(c, IRQ_DIS, 0x0001, 0x0001);        /* insertion detection off */
    cdc_upd(c, IRQ_MASK, 0x0001, 0x0001);
    cdc_upd(c, N25_HSD_CTRL, 0x0040, 0x0040);
    cdc_upd(c, IRQ_MASK, 0x0404, 0);            /* type detection done, ejection */
    c->priv[P_JACK] = 1;
    if (!c->priv[P_STRM]) {
        clk_internal(c);
        cdc_upd(c, CLK_DIV, 0x000F, 0);         /* fast internal clock for the detection */
    }
    cdc_upd(c, ENA_CTRL, 0x0100, 0x0100);       /* the ADC takes part */
    if (!c->priv[P_STRM] && !c->ssp.provider) {
        /* one frame clock: master mode on and off */
        cdc_upd(c, I2S_CTRL2, 0x0008, 0x0008);
        cdc_upd(c, I2S_CTRL2, 0x0008, 0);
    }
    cdc_upd(c, JACK_DET, 0x0100, 0);            /* debounce */
    cdc_wr(c, IRQ_DIS, 0);
    cdc_upd(c, JACK_DET, 0x0200, 0x0200);       /* restart the detection */
    cdc_upd(c, JACK_DET, 0x0200, 0);
}

/* high impedance headsets: the SAR ADC compares MICGND1 and MICGND2 */
static int n25_himp(struct cdc *c)
{
    int m1, m2;
    cdc_upd(c, N25_HSD_CTRL, 0x000F, 0x000C);
    cdc_upd(c, ANA_CTRL1, 0xC000, 0xC000);
    cdc_wr(c, TRIM, 0x0006);
    cdc_upd(c, MIC_BIAS, 0x0407, 0x0400);
    cdc_upd(c, SAR_CTRL, 0x0FFF, 0x0090);       /* VDDMIC, 70 kOhm */
    n25_micbias(c, 1);
    cdc_upd(c, N25_HSD_CTRL, 0x000F, 0x0004);
    cdc_upd(c, MIC_BIAS, 0x5000, 0x1000);
    m1 = cdc_rd(c, SAR_DATA);
    cdc_upd(c, MIC_BIAS, 0x5000, 0);
    cdc_upd(c, N25_HSD_CTRL, 0x000F, 0x000F);
    cdc_upd(c, N25_HSD_CTRL, 0x000F, 0x0008);
    cdc_upd(c, MIC_BIAS, 0x5000, 0x4000);
    cdc_upd(c, SAR_CTRL, 0x0800, 0x0800);
    m2 = cdc_rd(c, SAR_DATA);
    n25_micbias(c, 0);
    cdc_upd(c, MIC_BIAS, 0x4407, 0x0006);
    cdc_upd(c, N25_HSD_CTRL, 0x000F, 0x000F);
    cdc_upd(c, ANA_CTRL1, 0xC000, 0xC000);
    cdc_wr(c, TRIM, 0);
    cdc_upd(c, SAR_CTRL, 0x078F, 0);            /* SAR at VDDA, 500 ns compare, 2 us sampling */
    if (m1 < 0 || m2 < 0 || m1 == m2) return -1;
    if (m1 > m2) {                              /* OMTP: microphone ground on MICGND1 */
        cdc_upd(c, N25_HSD_CTRL, 0x000C, 0x0004);
        cdc_upd(c, MIC_BIAS, 0x5000, 0x1000);
        cdc_upd(c, SAR_CTRL, 0x0800, 0);
    } else {                                    /* CTIA: on MICGND2 */
        cdc_upd(c, N25_HSD_CTRL, 0x000C, 0x0008);
        cdc_upd(c, MIC_BIAS, 0x5000, 0x4000);
        cdc_upd(c, SAR_CTRL, 0x0800, 0x0800);
    }
    return 0;
}

/* headset type detection done: 2 headphones, 3 headset */
static int n25_type(struct cdc *c)
{
    int st = cdc_rd(c, GEN_STATUS), t = 2;
    switch (st < 0 ? 0 : (st >> 10) & 3) {
    case 1:                                     /* OMTP */
        cdc_upd(c, N25_HSD_CTRL, 0x000C, 0x0004);
        cdc_upd(c, MIC_BIAS, 0x5000, 0x1000);
        cdc_upd(c, SAR_CTRL, 0x0800, 0);
        t = 3;
        break;
    case 2:                                     /* CTIA */
        cdc_upd(c, N25_HSD_CTRL, 0x000C, 0x0008);
        cdc_upd(c, MIC_BIAS, 0x5000, 0x4000);
        cdc_upd(c, SAR_CTRL, 0x0800, 0x0800);
        t = 3;
        break;
    case 3:                                     /* both: high impedance headset? */
        if (!n25_himp(c)) t = 3;
        break;
    }
    if (t == 3) n25_micbias(c, 1);
    if (!c->priv[P_STRM]) cdc_upd(c, CLK_DIV, 0x000F, 0x000F);
    cdc_upd(c, N25_HSD_CTRL, 0x0040, 0);        /* HSD off */
    if (c->priv[P_STRM] & 1) cdc_upd(c, N25_HSD_CTRL, 0x0003, 0);  /* playing: outputs on */
    return t;
}

static int n25_jack(struct cdc *c)
{
    int in = n25_inserted(c), st = cdc_rd(c, IRQ_STATUS);
    if (in < 0 || st < 0) return -1;
    if (!c->priv[P_JACK]) {
        if (st) irq_clear_all(c);
        if (!in) {
            c->priv[P_CNT] = 0;
            return 0;
        }
        if (++c->priv[P_CNT] < 2) return 0;     /* the pin stays "in" for 2 polls */
        n25_detect(c);
        c->priv[P_CNT] = c->priv[P_MISS] = 0;
        return 1;
    }
    c->priv[P_MISS] = in ? 0 : c->priv[P_MISS] + 1;
    if ((st & 0x000C) == 0x0004 || c->priv[P_MISS] >= 2) {
        n25_eject(c);
        return 0;
    }
    if (c->priv[P_JACK] == 1) {
        if (st & 0x0400) {
            c->priv[P_JACK] = (u32)n25_type(c);
            cdc_wr(c, IRQ_CLEAR, 0x0400);
        } else if (++c->priv[P_CNT] > 10) {
            /* no result in 3 s: headphones */
            DBG("nau8825: headset detection timed out\n");
            c->priv[P_JACK] = 2;
            if (!c->priv[P_STRM]) cdc_upd(c, CLK_DIV, 0x000F, 0x000F);
            cdc_upd(c, N25_HSD_CTRL, 0x0040, 0);
            if (c->priv[P_STRM] & 1) cdc_upd(c, N25_HSD_CTRL, 0x0003, 0);
        }
    } else if (st & ~0x000F) {
        cdc_wr(c, IRQ_CLEAR, (u32)st & ~0x000FUL);  /* buttons and the like */
    }
    return c->priv[P_JACK] == 3 ? 3 : 1;
}

static int n25_init(struct cdc *c)
{
    int id;
    cdc_wr(c, RESET, 0);
    cdc_wr(c, RESET, 0);
    mdelay(1);
    id = cdc_rd(c, DEVICE_ID);
    if (id < 0) return -1;
    c->priv[P_REV] = (u32)id & 3;
    c->priv[P_STRM] = c->priv[P_JACK] = c->priv[P_CNT] = c->priv[P_MISS] = 0;
    if (c->priv[P_REV]) cdc_seq(c, n25_revc_patch, (int)ARRAY_SIZE(n25_revc_patch) / 2);
    cdc_wr(c, N25_IIC_ADDR, 0x0001);            /* latch the address LSB */
    cdc_upd(c, BIAS_ADJ, 0x0040, 0x0040);       /* VMID */
    cdc_upd(c, BOOST, 0x1000, 0x1000);          /* global bias */
    cdc_upd(c, BIAS_ADJ, 0x0030, 0x0020);       /* VMID tie-off 125 kOhm */
    cdc_upd(c, BOOST, 0x2340, 0x2340);          /* boost driver off, short circuit protection */
    /* jack pin (Chromebook settings): input, no pull, active low, 512 ms debounce */
    cdc_upd(c, GPIO12, 0x0B00, 0x0200);
    cdc_upd(c, JACK_DET, 0x00FE, (7 << 5) | (7 << 2));
    cdc_upd(c, IRQ_MASK, 0x6000, 0x6000);       /* IRQ pin pull-up */
    cdc_upd(c, IRQ_MASK, 0x07FF, 0x07FF);
    cdc_upd(c, MIC_BIAS, 0x0007, 0x0006);       /* 1.53 * VDDA */
    cdc_upd(c, SAR_CTRL, 0x070F, 0);
    cdc_upd(c, N25_ADC_RATE, 0x0013, 0x0001);   /* ADC decimation 64 */
    cdc_upd(c, N25_DAC_CTRL1, 0x0087, 0x0080);  /* DAC oversampling 64, CIC clip off */
    if (!c->priv[P_REV]) cdc_upd(c, CHARGE_PUMP, 0x0300, 0x0300);
    cdc_upd(c, BIAS_ADJ, 0x0300, 0x0300);       /* test DAC: silence while powering up */
    cdc_upd(c, ANA_CTRL2, 0x1003, 0x1003);
    cdc_upd(c, N25_CLASSG, 0x3F00, 0x2000);     /* class G timer 64 ms */
    cdc_upd(c, RDAC, 0x007C, 0x002C);           /* DAC clock delay 2 ns, VREF */
    cdc_upd(c, N25_DACL_CTRL, 0x0200, 0);       /* left from the left slot */
    cdc_upd(c, N25_DACR_CTRL, 0x0200, 0x0200);
    cdc_upd(c, LEFT_SLOT, 0x2000, 0x2000);      /* no short frame detection */
    cdc_upd(c, CHARGE_PUMP, 0x1000, 0);
    cdc_upd(c, N25_HSD_CTRL, 0x0043, 0x0043);   /* HSD, outputs grounded */
    nau_i2s(c);
    cdc_upd(c, I2S_CTRL2, 0x8000, 0x8000);
    /* capture: frontend PGA +18 dB, ADC 0 dB */
    cdc_upd(c, PWR_UP, 0x3F00, 19 << 8);
    cdc_upd(c, N25_ADC_DGAIN, 0x00FF, 0x00CF);
    n25_set_vol(c, 0);
    /* idle: no clock, jack insertion and ejection detection */
    clk_off(c);
    irq_clear_all(c);
    cdc_upd(c, IRQ_MASK, 0x0C05, 0x0C00);
    cdc_upd(c, JACK_DET, 0x0100, 0x0100);
    cdc_upd(c, IRQ_DIS, 0x0005, 0);
    return 0;
}

static void n25_stream(struct cdc *c, int dir, int on)
{
    if (!stream_change(c, dir, on)) return;
    if (dir) n25_rec(c, on);
    else n25_play(c, on);
    if (!c->priv[P_STRM]) clk_idle(c);
}

static void n25_volume(struct cdc *c, int ctl, u32 lr)
{
    if (ctl == MX_MASTER) {
        c->priv[P_VOL] = lr;
        if (c->priv[P_STRM] & 1) n25_set_vol(c, lr);
    } else if (ctl == MX_INGAIN) {
        cdc_upd(c, PWR_UP, 0x3F00, cdc_step(CDC_L(lr), 37) << 8);   /* -1 .. +36 dB */
        cdc_upd(c, N25_ADC_DGAIN, 0x00FF, CDC_L(lr) ? 0x00CF : 0);
    }
}

static void n25_off(struct cdc *c)
{
    u32 s = c->priv[P_STRM];
    c->priv[P_STRM] = 0;
    if (s & 1) n25_play(c, 0);
    if (s & 2) n25_rec(c, 0);
    n25_micbias(c, 0);
    cdc_upd(c, MIC_BIAS, 0x5000, 0);
    cdc_upd(c, N25_HSD_CTRL, 0x000F, 0x000F);
    cdc_wr(c, IRQ_DIS, 0xFFFF);
    cdc_upd(c, ENA_CTRL, 0x0100, 0);
    clk_off(c);
}

/* ---------------------------------------------------------------- NAU88L21 */

static int n21_probe(struct cdc *c)
{
    return nau_probe_id(c, T21);
}

static int n21_inserted(struct cdc *c)
{
    int j = cdc_rd(c, JACK_DET), s = cdc_rd(c, GEN_STATUS);
    if (j < 0 || s < 0) return -1;
    return !(j & 0x02) == !(s & 0x02);
}

static void n21_set_vol(struct cdc *c, u32 lr)
{
    cdc_wr(c, N21_DAC_VOL, (dac_vol(CDC_R(lr), 0xCF) << 8) | dac_vol(CDC_L(lr), 0xCF));
}

static void n21_play(struct cdc *c, int on)
{
    if (on) {
        cdc_upd(c, ENA_CTRL, 0x0C00, 0x0C00);   /* DACL, DACR */
        cdc_upd(c, N21_CLASSG, 0x0006, 0x0006); /* headphone amplifiers */
        cdc_upd(c, CHARGE_PUMP, 0x0020, 0x0020);
        mdelay(20);
        cdc_upd(c, CHARGE_PUMP, 0x0400, 0x0400);
        cdc_upd(c, RDAC, 0x3000, 0x3000);
        cdc_upd(c, RDAC, 0x0300, 0x0300);
        cdc_upd(c, PWR_UP, 0x0030, 0x0030);
        cdc_upd(c, PWR_UP, 0x000C, 0x000C);
        cdc_upd(c, PWR_UP, 0x0003, 0x0003);
        cdc_upd(c, BIAS_ADJ, 0x0300, 0);        /* test DAC off */
        cdc_upd(c, CHARGE_PUMP, 0x0300, 0x0300);    /* output DACs */
        cdc_upd(c, JACK_DET, 0xC000, 0xC000);   /* HPOL / HPOR released */
        cdc_upd(c, BOOST, 0x0200, 0);
        cdc_upd(c, N21_CLASSG, 0x0001, 0x0001);
        n21_set_vol(c, c->priv[P_VOL]);
        cdc_upd(c, N21_MUTE_CTRL, 0x0200, 0);
    } else {
        cdc_upd(c, N21_MUTE_CTRL, 0x0200, 0x0200);
        n21_set_vol(c, 0);
        cdc_upd(c, N21_CLASSG, 0x0001, 0);
        cdc_upd(c, BOOST, 0x0200, 0x0200);
        cdc_upd(c, JACK_DET, 0xC000, 0);
        cdc_upd(c, CHARGE_PUMP, 0x0300, 0);
        cdc_upd(c, BIAS_ADJ, 0x0300, 0x0300);
        cdc_upd(c, PWR_UP, 0x0003, 0);
        cdc_upd(c, PWR_UP, 0x000C, 0);
        cdc_upd(c, PWR_UP, 0x0030, 0);
        cdc_upd(c, RDAC, 0x0300, 0);
        cdc_upd(c, RDAC, 0x3000, 0);
        cdc_upd(c, CHARGE_PUMP, 0x0400, 0);
        cdc_upd(c, CHARGE_PUMP, 0x0020, 0);
        cdc_upd(c, N21_CLASSG, 0x0006, 0);
        cdc_upd(c, ENA_CTRL, 0x0C00, 0);
    }
}

/* MICL / MICR -> frontend PGAs -> ADCs */
static void n21_rec(struct cdc *c, int on)
{
    if (on) {
        cdc_upd(c, MIC_BIAS, 0x0100, 0x0100);
        cdc_upd(c, I2S_CTRL2, 0x8000, 0);
        cdc_upd(c, ENA_CTRL, 0x0300, 0x0300);   /* ADC digital paths */
        mdelay(125);
        cdc_upd(c, PWR_UP, 0xC000, 0xC000);     /* frontend PGAs */
        cdc_upd(c, ANA_ADC2, 0x0050, 0x0050);   /* ADCs */
    } else {
        cdc_upd(c, ANA_ADC2, 0x0050, 0);
        cdc_upd(c, PWR_UP, 0xC000, 0);
        cdc_upd(c, ENA_CTRL, 0x0300, 0);
        cdc_upd(c, I2S_CTRL2, 0x8000, 0x8000);
        cdc_upd(c, MIC_BIAS, 0x0100, 0);
    }
}

static void n21_eject(struct cdc *c)
{
    cdc_upd(c, ANA_ADC1, 0x0001, 0x0001);       /* microphone detection off */
    cdc_upd(c, MIC_BIAS, 0x1000, 0);
    cdc_upd(c, JACK_DET, 0xC000, 0);            /* outputs grounded */
    if (!(c->priv[P_STRM] & 2)) cdc_upd(c, MIC_BIAS, 0x0100, 0);
    cdc_upd(c, IRQ_DIS, 0x0005, 0x0005);
    cdc_upd(c, IRQ_MASK, 0x0005, 0x0005);
    irq_clear_all(c);
    cdc_upd(c, IRQ_DIS, 0x0001, 0);             /* insertion detection, no debounce */
    cdc_upd(c, IRQ_MASK, 0x0001, 0);
    cdc_upd(c, JACK_DET, 0x0100, 0x0100);
    c->priv[P_JACK] = 0;
    if (!c->priv[P_STRM]) clk_off(c);
    cdc_upd(c, N21_ADC_RATE, 0x4000, 0);
}

/* jack in: microphone detection, ejection detection with the internal clock */
static int n21_insert(struct cdc *c)
{
    int id;
    cdc_upd(c, ANA_ADC1, 0x0001, 0);            /* microphone detection on */
    cdc_upd(c, MIC_BIAS, 0x0100, 0x0100);
    cdc_upd(c, IRQ_DIS, 0x0001, 0x0001);        /* insertion detection off */
    cdc_upd(c, IRQ_MASK, 0x0001, 0x0001);
    cdc_wr(c, IRQ_CLEAR, 0x0001);
    c->priv[P_JACK] = 1;
    if (!c->priv[P_STRM]) clk_internal(c);
    if (!c->priv[P_STRM] && !c->ssp.provider) {
        cdc_upd(c, I2S_CTRL2, 0x0008, 0x0008);  /* one frame clock */
        cdc_upd(c, I2S_CTRL2, 0x0008, 0);
    }
    cdc_upd(c, JACK_DET, 0x0100, 0);
    cdc_upd(c, IRQ_MASK, 0x0004, 0);            /* ejection detection */
    cdc_upd(c, IRQ_DIS, 0x0004, 0);
    mdelay(20);
    id = cdc_rd(c, DEVICE_ID);
    if (id >= 0 && !(id & 0x80)) {              /* microphone: 2 kOhm to MICGND, right ADC input */
        cdc_upd(c, MIC_BIAS, 0x1000, 0x1000);
        cdc_upd(c, N21_ADC_RATE, 0x4000, 0x4000);
    }
    if (!(c->priv[P_STRM] & 2)) cdc_upd(c, MIC_BIAS, 0x0100, 0);
    if (c->priv[P_STRM] & 1) cdc_upd(c, JACK_DET, 0xC000, 0xC000);
    return id >= 0 && !(id & 0x80) ? 3 : 2;
}

static int n21_jack(struct cdc *c)
{
    int in = n21_inserted(c), st = cdc_rd(c, IRQ_STATUS);
    if (in < 0 || st < 0) return -1;
    if (!c->priv[P_JACK]) {
        if (st) irq_clear_all(c);
        if (!in) {
            c->priv[P_CNT] = 0;
            return 0;
        }
        if (++c->priv[P_CNT] < 2) return 0;
        c->priv[P_JACK] = (u32)n21_insert(c);
        c->priv[P_CNT] = c->priv[P_MISS] = 0;
    } else {
        c->priv[P_MISS] = in ? 0 : c->priv[P_MISS] + 1;
        if ((st & 0x000C) == 0x0004 || c->priv[P_MISS] >= 2) {
            n21_eject(c);
            return 0;
        }
        if (st) cdc_wr(c, IRQ_CLEAR, (u32)st);
    }
    return c->priv[P_JACK] == 3 ? 3 : 1;
}

static int n21_init(struct cdc *c)
{
    cdc_wr(c, RESET, 0xFFFF);
    cdc_wr(c, RESET, 0xFFFF);
    mdelay(1);
    if (cdc_rd(c, DEVICE_ID) < 0) return -1;
    c->priv[P_STRM] = c->priv[P_JACK] = c->priv[P_CNT] = c->priv[P_MISS] = 0;
    cdc_upd(c, BIAS_ADJ, 0x0040, 0x0040);       /* VMID */
    cdc_upd(c, BOOST, 0x1000, 0x1000);          /* global bias */
    cdc_upd(c, BIAS_ADJ, 0x0330, 0x0320);       /* VMID 125 kOhm, test DAC on */
    cdc_upd(c, LEFT_SLOT, 0x2000, 0x2000);
    cdc_upd(c, BOOST, 0x2340, 0x2340);
    cdc_upd(c, N21_CLASSG, 0x3F00, 0x2000);
    cdc_upd(c, ANA_CTRL2, 0x1003, 0x1003);
    cdc_upd(c, CHARGE_PUMP, 0x0300, 0);
    cdc_upd(c, RDAC, 0x007C, 0x002C);
    cdc_upd(c, MIC_BIAS, 0x0007, 0x0006);
    cdc_upd(c, N21_ADC_RATE, 0x0003, 0x0001);   /* ADC decimation 64 */
    cdc_upd(c, N21_DAC_CTRL1, 0x0007, 0);       /* DAC oversampling 64 */
    /* jack pin: input, no pull, active low, 512 ms insertion debounce */
    cdc_upd(c, GPIO12, 0x0B00, 0x0200);
    cdc_upd(c, JACK_DET, 0x00FE, 7 << 5);
    cdc_upd(c, IRQ_MASK, 0x6800, 0x6800);
    cdc_upd(c, IRQ_MASK, 0x03F5, 0x03F5);
    nau_i2s(c);
    cdc_upd(c, I2S_CTRL2, 0x8000, 0x8000);
    cdc_upd(c, N21_MUTE_CTRL, 0x0200, 0x0200);
    cdc_wr(c, N21_PGA_GAIN, (19 << 8) | 19);    /* +18 dB */
    cdc_wr(c, N21_ADC_VOL, 0xCFCF);
    n21_set_vol(c, 0);
    clk_off(c);
    irq_clear_all(c);
    cdc_upd(c, IRQ_MASK, 0x0005, 0);
    cdc_upd(c, JACK_DET, 0x0100, 0x0100);
    cdc_upd(c, IRQ_DIS, 0x0005, 0);
    return 0;
}

static void n21_stream(struct cdc *c, int dir, int on)
{
    if (!stream_change(c, dir, on)) return;
    if (dir) n21_rec(c, on);
    else n21_play(c, on);
    if (!c->priv[P_STRM]) clk_idle(c);
}

static void n21_volume(struct cdc *c, int ctl, u32 lr)
{
    u32 l = cdc_step(CDC_L(lr), 37), r = cdc_step(CDC_R(lr), 37);
    if (ctl == MX_MASTER) {
        c->priv[P_VOL] = lr;
        if (c->priv[P_STRM] & 1) n21_set_vol(c, lr);
    } else if (ctl == MX_INGAIN) {
        cdc_wr(c, N21_PGA_GAIN, (l << 8) | r);
        cdc_wr(c, N21_ADC_VOL, (r ? 0xCF00 : 0) | (l ? 0x00CF : 0));
    }
}

static void n21_off(struct cdc *c)
{
    u32 s = c->priv[P_STRM];
    c->priv[P_STRM] = 0;
    if (s & 1) n21_play(c, 0);
    if (s & 2) n21_rec(c, 0);
    cdc_upd(c, JACK_DET, 0xC000, 0);
    cdc_upd(c, MIC_BIAS, 0x1100, 0);
    cdc_wr(c, IRQ_DIS, 0xFFFF);
    cdc_upd(c, IRQ_MASK, 0x0005, 0x0005);
    clk_off(c);
}

/* ---------------------------------------------------------------- NAU88L24 */

static int n24_probe(struct cdc *c)
{
    return nau_probe_id(c, T24);
}

static void n24_set_vol(struct cdc *c, u32 lr)
{
    /* DAC digital gain, 0x100 = 0 dB as on the ADC channels */
    cdc_upd(c, N24_DAC0_DGAIN, 0x01FF, dac_vol(CDC_L(lr), 0x100));
    cdc_upd(c, N24_DAC1_DGAIN, 0x01FF, dac_vol(CDC_R(lr), 0x100));
}

/* class-D speaker amplifier: only while playing and no headphones are in */
static void n24_spk(struct cdc *c)
{
    int on = (c->priv[P_STRM] & 1) && c->priv[P_JACK] < 2;
    if (on) {
        cdc_upd(c, ANA_CTRL2, 0x0008, 0x0008);  /* clamp off */
        cdc_upd(c, N24_CLASSD1, 0x0080, 0x0080);
    } else {
        cdc_upd(c, N24_CLASSD1, 0x0080, 0);
        cdc_upd(c, ANA_CTRL2, 0x0008, 0);
    }
}

/* ADC input: headset microphone (HSMIC) or the internal microphones (MIC1, MIC2) */
static void n24_mic(struct cdc *c, int headset)
{
    cdc_upd(c, FEPGA, 0x0033, headset ? 0x0011 : 0x0022);
    cdc_upd(c, N24_ADC0_DGAIN, 0x0600, headset ? 0x0200 : 0);
    cdc_upd(c, N24_ADC1_DGAIN, 0x0600, headset ? 0x0200 : 0);
}

static void n24_play(struct cdc *c, int on)
{
    if (on) {
        cdc_upd(c, RDAC, 0x0300, 0x0300);       /* DAC clocks, DACs */
        cdc_upd(c, RDAC, 0x3000, 0x3000);
        cdc_upd(c, N24_CLASSG, 0x0006, 0x0006); /* headphone mixers */
        cdc_upd(c, CHARGE_PUMP, 0x0020, 0x0020);
        mdelay(10);
        cdc_upd(c, CHARGE_PUMP, 0x8000, 0x8000);
        cdc_upd(c, N24_ENABLE_LO, 0xC000, 0);   /* test DAC off */
        cdc_upd(c, CHARGE_PUMP, 0x0300, 0);     /* output DACs */
        cdc_upd(c, PWR_UP, 0x000C, 0x000C);     /* output drivers, main drivers */
        cdc_upd(c, PWR_UP, 0x0003, 0x0003);
        cdc_upd(c, BOOST, 0x0200, 0);
        cdc_upd(c, N24_CLASSG, 0x0001, 0x0001);
        n24_spk(c);
        n24_set_vol(c, c->priv[P_VOL]);
    } else {
        n24_set_vol(c, 0);
        n24_spk(c);
        cdc_upd(c, N24_CLASSG, 0x0001, 0);
        cdc_upd(c, BOOST, 0x0200, 0x0200);
        cdc_upd(c, PWR_UP, 0x0003, 0);
        cdc_upd(c, PWR_UP, 0x000C, 0);
        cdc_upd(c, CHARGE_PUMP, 0x0300, 0x0300);
        cdc_upd(c, N24_ENABLE_LO, 0xC000, 0xC000);
        cdc_upd(c, CHARGE_PUMP, 0x8000, 0);
        cdc_upd(c, CHARGE_PUMP, 0x0020, 0);
        cdc_upd(c, N24_CLASSG, 0x0006, 0);
        cdc_upd(c, RDAC, 0x3000, 0);
        cdc_upd(c, RDAC, 0x0300, 0);
    }
}

static void n24_rec(struct cdc *c, int on)
{
    if (on) {
        cdc_upd(c, MIC_BIAS, 0x0100, 0x0100);
        cdc_upd(c, PWR_UP, 0x3000, 0x3000);     /* left / right ADC mixers */
        cdc_upd(c, ANA_ADC2, 0x00C0, 0x00C0);   /* ADCs */
    } else {
        cdc_upd(c, ANA_ADC2, 0x00C0, 0);
        cdc_upd(c, PWR_UP, 0x3000, 0);
        if (c->priv[P_JACK] != 3) cdc_upd(c, MIC_BIAS, 0x0100, 0);
    }
}

static void n24_eject(struct cdc *c)
{
    irq_clear_all(c);
    cdc_upd(c, SAR_CTRL, 0x1000, 0);
    if (!(c->priv[P_STRM] & 2)) cdc_upd(c, MIC_BIAS, 0x0100, 0);
    cdc_upd(c, IRQ_DIS, 0x002B, 0x002A);        /* insertion detection on, the rest off */
    cdc_upd(c, IRQ_MASK, 0x0300, 0x0100);
    cdc_upd(c, ENA_CTRL, 0x0400, 0x0400);       /* jack detection in sleep mode */
    c->priv[P_JACK] = 0;
    if (!c->priv[P_STRM]) clk_off(c);
}

/* jack in: ejection detection with the internal clock, microphone by the SAR ADC level */
static int n24_insert(struct cdc *c)
{
    int v;
    cdc_upd(c, IRQ_DIS, 0x0003, 0x0001);
    cdc_upd(c, IRQ_MASK, 0x0300, 0x0200);
    c->priv[P_JACK] = 1;
    if (!c->priv[P_STRM]) clk_internal(c);
    cdc_upd(c, ENA_CTRL, 0x0400, 0);
    cdc_upd(c, MIC_BIAS, 0x0100, 0x0100);
    cdc_upd(c, SAR_CTRL, 0x1000, 0x1000);
    mdelay(100);
    v = cdc_rd(c, SAR_DATA);
    if (v >= 0 && (v & 0xFF) >= 0x80) return 3;
    cdc_upd(c, SAR_CTRL, 0x1000, 0);
    if (!(c->priv[P_STRM] & 2)) cdc_upd(c, MIC_BIAS, 0x0100, 0);
    return 2;
}

static int n24_jack(struct cdc *c)
{
    int st = cdc_rd(c, IRQ_STATUS), j = (int)c->priv[P_JACK];
    if (st < 0) return -1;
    if (st & 0x0002) {
        n24_eject(c);
    } else if ((st & 0x0001) && !j) {
        c->priv[P_JACK] = (u32)n24_insert(c);
        cdc_wr(c, IRQ_CLEAR, (u32)st);
    } else if (st) {
        cdc_wr(c, IRQ_CLEAR, (u32)st);
    }
    if ((int)c->priv[P_JACK] != j) {
        n24_spk(c);
        n24_mic(c, c->priv[P_JACK] == 3);
    }
    return c->priv[P_JACK] == 3 ? 3 : c->priv[P_JACK] ? 1 : 0;
}

static int n24_init(struct cdc *c)
{
    cdc_wr(c, RESET, 0);
    cdc_wr(c, RESET, 0);
    mdelay(1);
    if (cdc_rd(c, DEVICE_ID) < 0) return -1;
    c->priv[P_STRM] = c->priv[P_JACK] = 0;
    cdc_upd(c, BIAS_ADJ, 0x0070, 0x0060);       /* VMID, 125 kOhm tie-off */
    cdc_upd(c, BOOST, 0x1000, 0x1000);
    mdelay(2);
    cdc_upd(c, MIC_BIAS, 0x0007, 0x0006);
    cdc_upd(c, BOOST, 0x2340, 0x2340);
    cdc_upd(c, CLK_DIV, 0x00F0, 0x00A0);        /* DAC / ADC clock 1/4 (OSR 64) */
    cdc_upd(c, N24_DAC_MUTE, 0x0800, 0x0800);   /* zero crossing */
    cdc_upd(c, ENA_CTRL, 0x003F, 0x003F);       /* DAC and ADC channels */
    cdc_upd(c, N24_CLK_GATING, 0xF0AA, 0xF0AA);
    cdc_upd(c, N24_CLASSG, 0x3F00, 0x2000);
    cdc_upd(c, TRIM, 0x8000, 0x8000);
    cdc_upd(c, CHARGE_PUMP, 0x3300, 0x3300);    /* speaker pull-downs, DACs off */
    cdc_upd(c, N24_ENABLE_LO, 0xC000, 0xC000);  /* test DAC */
    cdc_upd(c, N24_DAC0_DGAIN, 0x0200, 0);      /* DAC channel 0 from the left slot */
    cdc_upd(c, N24_DAC1_DGAIN, 0x0200, 0x0200);
    cdc_upd(c, N24_ENABLE_LO, 0x000F, 0x0005);  /* DACL -> HPL, DACR -> HPR */
    cdc_upd(c, N24_ADC_FILTER, 0x0003, 0x0001);
    cdc_upd(c, N24_DAC_FILTER, 0x0087, 0x0080);
    cdc_upd(c, RDAC, 0x007C, 0x002C);
    cdc_upd(c, FEPGA, 0x0088, 0x0088);
    cdc_upd(c, ANA_CTRL1, 0x000F, 0x000F);
    cdc_upd(c, JACK_DET, 0x000E, 1 << 2);       /* active low, ejection debounce */
    cdc_upd(c, SAR_CTRL, 0x070F, 0x0605);       /* SAR as for the buttons (Linux defaults) */
    /* speakers: left from DACL, right from DACR (UCM levels) */
    cdc_upd(c, N24_CLASSD1, 0x1F1F, 0x0100);
    cdc_upd(c, N24_CLASSD2, 0x1F1F, 0x0001);
    cdc_upd(c, N24_FEPGA2, 0x03FF, (14 << 5) | 14);     /* microphones +28 dB */
    n24_mic(c, 0);
    nau_i2s(c);
    if (c->ssp.slots >= 4) {                    /* TDM as the Cherry Trail machine sets it */
        cdc_upd(c, N24_TDM_CTRL, 0xC0FF, 0xC000 | (1 << 4) | 0x0F);
        cdc_upd(c, LEFT_SLOT, 0x03FF, 0);
    } else {
        cdc_upd(c, N24_TDM_CTRL, 0xC0FF, 0x0003);
    }
    n24_set_vol(c, 0);
    clk_off(c);
    irq_clear_all(c);
    cdc_upd(c, ENA_CTRL, 0x0400, 0x0400);
    cdc_upd(c, IRQ_DIS, 0x03FF, 0x03FC);        /* insertion and ejection */
    cdc_upd(c, IRQ_MASK, 0x0300, 0x0300);
    return 0;
}

static void n24_stream(struct cdc *c, int dir, int on)
{
    if (!stream_change(c, dir, on)) return;
    if (dir) n24_rec(c, on);
    else n24_play(c, on);
    if (!c->priv[P_STRM]) clk_idle(c);
}

static void n24_volume(struct cdc *c, int ctl, u32 lr)
{
    u32 l = cdc_step(CDC_L(lr), 18), r = cdc_step(CDC_R(lr), 18);
    if (ctl == MX_MASTER) {
        c->priv[P_VOL] = lr;
        if (c->priv[P_STRM] & 1) n24_set_vol(c, lr);
    } else if (ctl == MX_INGAIN) {
        cdc_upd(c, N24_FEPGA2, 0x03FF, (r << 5) | l);   /* 0 .. +36 dB */
        cdc_upd(c, N24_ADC0_DGAIN, 0x01FF, l ? 0x100 : 0);
        cdc_upd(c, N24_ADC1_DGAIN, 0x01FF, r ? 0x100 : 0);
    }
}

static void n24_off(struct cdc *c)
{
    u32 s = c->priv[P_STRM];
    c->priv[P_STRM] = 0;
    if (s & 1) n24_play(c, 0);
    if (s & 2) n24_rec(c, 0);
    n24_spk(c);
    cdc_upd(c, IRQ_DIS, 0x03FF, 0x03FF);
    cdc_upd(c, IRQ_MASK, 0x0300, 0);
    cdc_upd(c, SAR_CTRL, 0x1000, 0);
    cdc_upd(c, MIC_BIAS, 0x0100, 0);
    clk_off(c);
}

/* ---------------------------------------------------------------- NAU8315 / NAU8318 */

/* no control bus: the first GpioIo is the (active high) enable pin, on while playing */
static int amp_probe(struct cdc *c)
{
    (void)c;
    return 0;
}

static int amp_init(struct cdc *c)
{
    cdc_gpio(c, 0, 0);
    return 0;
}

static void amp_stream(struct cdc *c, int dir, int on)
{
    if (!dir) cdc_gpio(c, 0, on);
}

static void amp_off(struct cdc *c)
{
    cdc_gpio(c, 0, 0);
}

const struct cdcdrv cdc_nau8825 = { "nau8825", { "10508825", NULL }, CK_HEADSET, 16, 16, 0,
                                    n25_probe, n25_init, n25_stream, n25_volume, NULL, n25_jack, n25_off };
const struct cdcdrv cdc_nau8821 = { "nau8821", { "NVTN2020", NULL }, CK_HEADSET, 16, 16, 0,
                                    n21_probe, n21_init, n21_stream, n21_volume, NULL, n21_jack, n21_off };
const struct cdcdrv cdc_nau8824 = { "nau8824", { "10508824", NULL }, CK_SPKCODEC, 16, 16, 0,
                                    n24_probe, n24_init, n24_stream, n24_volume, NULL, n24_jack, n24_off };
const struct cdcdrv cdc_nau8318 = { "nau8318", { "NVTN2012", NULL }, CK_AMP, 8, 8, 24,
                                    amp_probe, amp_init, amp_stream, NULL, NULL, NULL, amp_off };
const struct cdcdrv cdc_nau8315 = { "nau8315", { "NVTN2010", NULL }, CK_AMP, 8, 8, 24,
                                    amp_probe, amp_init, amp_stream, NULL, NULL, NULL, amp_off };
