/* cdcm90.c - Maxim MAX98090 / MAX98091 codec (193C9890: Braswell / Cherry Trail Chromebooks
   and tablets, cht_bsw_max98090_ti). Machine wiring after the Linux machine driver: SSP2 in
   I2S, 2 x 16 bit slots, codec consumer with its PLL locked to LRCLK, system clock from the
   19.2 MHz MCLK (pmc_plt_clk_3, pmc_plt_clk_0 on a few boards); stereo speakers on SPKL/SPKR,
   headphones on HPL/HPR (straight from the DAC), internal digital microphone on DMICL/R,
   headset microphone on IN34 (MIC2) with MICBIAS. Jack: the codec's own JACKSNS detection,
   polled (headphones / headset). Register settings after the Linux max98090 driver. */
#include "dspcdc.h"

#define SOFTWARE_RESET      0x00
#define DEVICE_STATUS       0x01
#define JACK_STATUS         0x02
#define INPUT_MODE          0x0F
#define MIC2_INPUT_LEVEL    0x11
#define MIC_BIAS_VOLTAGE    0x12
#define DIGITAL_MIC_ENABLE  0x13
#define DIGITAL_MIC_CONFIG  0x14
#define LEFT_ADC_MIXER      0x15
#define RIGHT_ADC_MIXER     0x16
#define LEFT_ADC_LEVEL      0x17
#define RIGHT_ADC_LEVEL     0x18
#define SYSTEM_CLOCK        0x1B
#define CLOCK_MODE          0x1C
#define CLOCK_RATIO_NI_MSB  0x1D
#define CLOCK_RATIO_NI_LSB  0x1E
#define MASTER_MODE         0x21
#define INTERFACE_FORMAT    0x22
#define TDM_CONTROL         0x23
#define TDM_FORMAT          0x24
#define IO_CONFIGURATION    0x25
#define FILTER_CONFIG       0x26
#define DAI_PLAYBACK_LEVEL  0x27
#define LEFT_HP_MIXER       0x29
#define RIGHT_HP_MIXER      0x2A
#define HP_CONTROL          0x2B
#define LEFT_HP_VOLUME      0x2C
#define RIGHT_HP_VOLUME     0x2D
#define LEFT_SPK_MIXER      0x2E
#define RIGHT_SPK_MIXER     0x2F
#define SPK_CONTROL         0x30
#define LEFT_SPK_VOLUME     0x31
#define RIGHT_SPK_VOLUME    0x32
#define JACK_DETECT         0x3D
#define INPUT_ENABLE        0x3E
#define OUTPUT_ENABLE       0x3F
#define BIAS_CONTROL        0x42
#define DAC_CONTROL         0x43
#define ADC_CONTROL         0x44
#define DEVICE_SHUTDOWN     0x45
#define REVISION_ID         0xFF

#define ST_ULK              0x20                /* DEVICE_STATUS: PLL unlocked */
#define JS_LSNS             0x04
#define JS_JKSNS            0x02
#define SHDNN               0x80
#define DVM                 0x80                /* DAI playback mute */
#define VOL_MUTE            0x80                /* headphone / speaker volume mute */
#define IN_MBEN             0x10
#define IN_ADC              0x03
#define OUT_HP              0xC0
#define OUT_SPK             0x30
#define OUT_DAC             0x03

/* priv[0]: jack state (bit 0 headphones, bit 1 headset microphone), priv[1]: playing,
   priv[2]: DMIC clock divider field */

static int m90_probe(struct cdc *c)
{
    int v;
    if (!c->bus) return -1;
    v = cdc_rd(c, REVISION_ID);
    if (v >= 0x40 && v <= 0x4F) DBG("max98090: revision %02x\n", v);
    else if (v >= 0x50 && v <= 0x5F) DBG("max98091: revision %02x\n", v);
    else return -1;
    return 0;
}

/* digital microphone clock about 2.5 MHz from PCLK, and its compensation filter at 48 kHz */
static void m90_dmic(struct cdc *c, u32 pclk)
{
    static const u32 pclks[5] = { 11289600, 12000000, 12288000, 13000000, 19200000 };
    static const u8 divs[6] = { 2, 3, 4, 5, 6, 8 };
    static const u8 freq[6] = { 2, 1, 0, 0, 0, 0 };
    static const u8 comp[5][6] = {
        { 3, 2, 3, 6, 3, 3 }, { 3, 2, 3, 6, 3, 3 }, { 3, 2, 3, 6, 3, 3 },
        { 1, 0, 1, 5, 1, 1 }, { 0, 1, 6, 3, 2, 6 }
    };
    u32 best = 0xFFFFFFFFUL, d;
    int p = 0, m = 0, i;
    for (i = 0; i < 5; i++) {
        d = pclk > pclks[i] ? pclk - pclks[i] : pclks[i] - pclk;
        if (d < best) {
            best = d;
            p = i;
        }
    }
    best = 0xFFFFFFFFUL;
    for (i = 0; i < 6; i++) {
        u32 f = pclk / divs[i];
        d = f > 2500000UL ? f - 2500000UL : 2500000UL - f;
        if (d < best) {
            best = d;
            m = i;
        }
    }
    c->priv[2] = (u32)m << 4;
    cdc_wr(c, DIGITAL_MIC_CONFIG, ((u32)comp[p][m] << 4) | freq[m]);
}

/* headset microphone (IN34 -> MIC2 -> ADC) or internal digital microphone */
static void m90_mic(struct cdc *c, int headset)
{
    if (headset) {
        cdc_wr(c, DIGITAL_MIC_ENABLE, c->priv[2]);
        cdc_wr(c, MIC2_INPUT_LEVEL, (2 << 5) | 0x14);           /* +20 dB preamp, PGA 0 dB */
        cdc_wr(c, INPUT_ENABLE, IN_MBEN | IN_ADC);
    } else {
        cdc_wr(c, INPUT_ENABLE, 0);
        cdc_wr(c, MIC2_INPUT_LEVEL, 0x14);
        cdc_wr(c, DIGITAL_MIC_ENABLE, c->priv[2] | 0x03);       /* DMIC left and right */
    }
}

/* outputs for the jack state and the playback state */
static void m90_out(struct cdc *c)
{
    u32 o = OUT_DAC;
    if (c->priv[0] & 1) o |= OUT_HP;
    else if (c->priv[1]) o |= OUT_SPK;
    cdc_wr(c, OUTPUT_ENABLE, o);
}

/* the record path takes a new source after a short shutdown */
static void m90_restart(struct cdc *c)
{
    cdc_upd(c, DEVICE_SHUTDOWN, SHDNN, 0);
    mdelay(40);
    cdc_upd(c, DEVICE_SHUTDOWN, SHDNN, SHDNN);
}

static int m90_init(struct cdc *c)
{
    u32 mclk = c->ssp.mclk ? c->ssp.mclk : 19200000UL, ifmt, pclk;
    cdc_wr(c, SOFTWARE_RESET, 0x80);
    mdelay(20);
    cdc_wr(c, DEVICE_SHUTDOWN, 0);              /* configure while shut down */
    /* clocks: PCLK = MCLK / 1, 2 or 4 (10..20 MHz); consumer with the PLL on LRCLK */
    if (mclk > 40000000UL) {
        cdc_wr(c, SYSTEM_CLOCK, 3 << 4);
        pclk = mclk / 4;
    } else if (mclk > 20000000UL) {
        cdc_wr(c, SYSTEM_CLOCK, 2 << 4);
        pclk = mclk / 2;
    } else {
        cdc_wr(c, SYSTEM_CLOCK, 1 << 4);
        pclk = mclk;
    }
    if (mclk < 10000000UL || mclk > 60000000UL) kprintf("max98090: MCLK %lu Hz not supported\n", mclk);
    cdc_wr(c, CLOCK_MODE, 0);
    cdc_wr(c, CLOCK_RATIO_NI_MSB, 0);
    cdc_wr(c, CLOCK_RATIO_NI_LSB, 0);
    cdc_wr(c, MASTER_MODE, 0);
    if (c->ssp.provider) kprintf("max98090: clock provider mode not supported\n");
    /* format: 16-bit words (the MSBs of longer slots) */
    if (c->ssp.fmt == 4) {
        /* DSP A: TDM mode, the stereo pair in the first two playback slots */
        u32 i, l = 0, r = 1, n = 0;
        for (i = 0; i < 4 && n < 2; i++)
            if (c->ssp.txmask & (1UL << i)) {
                if (n++) r = i;
                else l = i;
            }
        cdc_wr(c, INTERFACE_FORMAT, 0x08);      /* BCI: inverted logic in TDM mode */
        cdc_wr(c, TDM_FORMAT, (l << 6) | (r << 4));
        cdc_wr(c, TDM_CONTROL, 0x01);
    } else {
        if (c->ssp.fmt != 1 && c->ssp.fmt != 3)
            kprintf("max98090: SSP format %u not supported, using I2S\n", c->ssp.fmt);
        ifmt = c->ssp.fmt == 3 ? 0x00 : 0x04;   /* left justified : I2S (DLY) */
        cdc_wr(c, INTERFACE_FORMAT, ifmt);
        cdc_wr(c, TDM_FORMAT, 0);
        cdc_wr(c, TDM_CONTROL, 0);
    }
    cdc_wr(c, IO_CONFIGURATION, 0x03);          /* SDIN, SDOUT */
    cdc_wr(c, FILTER_CONFIG, 0xC0);             /* music filters, record DC blocking, < 50 kHz */
    cdc_wr(c, DAI_PLAYBACK_LEVEL, DVM);         /* muted until playback, 0 dB */
    cdc_wr(c, DAC_CONTROL, 0x01);               /* high performance DAC */
    cdc_wr(c, ADC_CONTROL, 0x07);               /* high performance ADC */
    cdc_wr(c, BIAS_CONTROL, 0x01);              /* VCM from the bandgap */
    cdc_wr(c, MIC_BIAS_VOLTAGE, 0x03);          /* 2.8 V */
    /* playback: DAC straight to the headphones, DAC L / R into the speaker mixers */
    cdc_wr(c, HP_CONTROL, 0x00);
    cdc_wr(c, LEFT_HP_MIXER, 0x00);
    cdc_wr(c, RIGHT_HP_MIXER, 0x00);
    cdc_wr(c, LEFT_SPK_MIXER, 0x01);
    cdc_wr(c, RIGHT_SPK_MIXER, 0x02);
    cdc_wr(c, SPK_CONTROL, 0x00);               /* speaker mixers 0 dB */
    cdc_wr(c, LEFT_HP_VOLUME, 0x1A);            /* 0 dB */
    cdc_wr(c, RIGHT_HP_VOLUME, 0x1A);
    cdc_wr(c, LEFT_SPK_VOLUME, 0x2C);           /* 0 dB */
    cdc_wr(c, RIGHT_SPK_VOLUME, 0x2C);
    /* capture: MIC2 from IN34 into both ADCs at 0 dB; or the digital microphone */
    cdc_wr(c, INPUT_MODE, 0x00);
    cdc_wr(c, LEFT_ADC_MIXER, 0x40);
    cdc_wr(c, RIGHT_ADC_MIXER, 0x40);
    cdc_wr(c, LEFT_ADC_LEVEL, 0x03);
    cdc_wr(c, RIGHT_ADC_LEVEL, 0x03);
    m90_dmic(c, pclk);
    /* jack detection on, strong pull-up (headset microphone detection), 25 ms debounce */
    cdc_wr(c, JACK_DETECT, 0x80);
    cdc_rd(c, DEVICE_STATUS);                   /* clear old events */
    c->priv[0] = 0;
    c->priv[1] = 0;
    m90_mic(c, 0);
    m90_out(c);
    cdc_wr(c, DEVICE_SHUTDOWN, SHDNN);
    return 0;
}

static void m90_stream(struct cdc *c, int dir, int on)
{
    if (dir) return;
    c->priv[1] = (u32)on;
    if (!on) cdc_wr(c, DAI_PLAYBACK_LEVEL, DVM);
    m90_out(c);
    if (on) cdc_wr(c, DAI_PLAYBACK_LEVEL, 0);
}

/* largest volume register step at or below the level (centi-dB); the chip's dB tables are
   given as groups: first step, step count, dB of the first step, dB per step */
static u32 m90_vreg(const s16 *t, int ng, s32 cdb)
{
    u32 best = 0;
    int g, k;
    for (g = 0; g < ng; g++, t += 4)
        for (k = 0; k < t[1]; k++)
            if ((s32)t[2] + (s32)k * t[3] <= cdb) best = (u32)(t[0] + k);
    return best;
}

static void m90_volume(struct cdc *c, int ctl, u32 lr)
{
    static const s16 hp[] = { 0, 7, -6700, 400, 7, 8, -4000, 300, 15, 7, -1700, 200,
                              22, 6, -400, 100, 28, 4, 150, 50 };
    static const s16 spk[] = { 0, 5, -4800, 400, 5, 6, -2900, 300, 11, 4, -1200, 200,
                               15, 15, -500, 100, 30, 10, 950, 50 };
    if (ctl == MX_MASTER) {
        /* 60 steps of 1 dB up to 0 dB on the headphone and speaker amplifiers */
        u32 v[2], i;
        v[0] = CDC_L(lr);
        v[1] = CDC_R(lr);
        for (i = 0; i < 2; i++) {
            u32 s = cdc_step(v[i], 60);
            s32 cdb = -((s32)(60 - s) * 100);
            cdc_wr(c, LEFT_HP_VOLUME + i, s ? m90_vreg(hp, 5, cdb) : VOL_MUTE | 0x1A);
            cdc_wr(c, LEFT_SPK_VOLUME + i, s ? 24 + m90_vreg(spk, 5, cdb) : VOL_MUTE | 0x2C);
        }
    } else if (ctl == MX_INGAIN) {
        /* record level -12 .. +21 dB: 6 dB coarse steps plus +3 .. -12 dB fine */
        s32 g = (s32)cdc_step(CDC_L(lr), 33) - 12, coarse = 0;
        u32 reg;
        if (g > 3) coarse = (g - 3 + 5) / 6;
        if (coarse > 3) coarse = 3;
        reg = ((u32)coarse << 4) | (u32)(3 - (g - 6 * coarse));
        cdc_wr(c, LEFT_ADC_LEVEL, reg);
        cdc_wr(c, RIGHT_ADC_LEVEL, reg);
    }
}

static int m90_jack(struct cdc *c)
{
    int v = cdc_rd(c, JACK_STATUS), j, st;
    if (v < 0) return -1;
    switch (v & (JS_LSNS | JS_JKSNS)) {
    case JS_LSNS | JS_JKSNS: j = 0; break;                      /* nothing plugged */
    case JS_JKSNS: j = 3; break;                                /* headset */
    case 0: j = c->priv[0] == 3 ? 3 : 1; break;                 /* headphones, or a button */
    default: j = (int)c->priv[0]; break;
    }
    if ((u32)j != c->priv[0]) {
        int mic = (j & 2) != (int)(c->priv[0] & 2);
        c->priv[0] = (u32)j;
        m90_out(c);
        if (mic) {
            m90_mic(c, j & 2);
            m90_restart(c);
        }
    }
    /* a lost PLL lock while playing: restart the codec as Linux does */
    st = c->priv[1] ? cdc_rd(c, DEVICE_STATUS) : 0;
    if (st > 0 && (st & ST_ULK)) {
        DBG("max98090: PLL unlocked\n");
        cdc_upd(c, DEVICE_SHUTDOWN, SHDNN, 0);
        cdc_upd(c, DEVICE_SHUTDOWN, SHDNN, SHDNN);
    }
    return j;
}

static void m90_off(struct cdc *c)
{
    cdc_wr(c, DAI_PLAYBACK_LEVEL, DVM);
    cdc_wr(c, OUTPUT_ENABLE, 0);
    cdc_wr(c, INPUT_ENABLE, 0);
    cdc_wr(c, DIGITAL_MIC_ENABLE, 0);
    cdc_wr(c, JACK_DETECT, 0x40);               /* weak pull-up, detection off */
    cdc_wr(c, DEVICE_SHUTDOWN, 0);
}

const struct cdcdrv cdc_max98090 = { "max98090", { "193C9890", NULL }, CK_SPKCODEC, 8, 8, 16,
                                     m90_probe, m90_init, m90_stream, m90_volume, NULL, m90_jack,
                                     m90_off };
