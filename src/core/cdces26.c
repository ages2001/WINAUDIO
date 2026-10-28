/* cdces26.c - Everest ES8326 headphone codec (Alder Lake .. Meteor Lake laptops, machine driver
   sof_es8336 in Linux). As with the ES8336 the speakers hang on the headphone outputs behind an
   amplifier that the first GpioIo of the codec's ACPI device switches on (high = on, off while
   headphones are plugged). The headset microphone is the analog input (MIC1); without a headset
   the ADCs take the digital microphone on the codec's SDIN2 pin (internal microphone, where the
   board wires one there). Register sequences after the Linux es8326 driver. */
#include "dspcdc.h"

#define RESET           0x00
#define CLK_CTL         0x01
#define CLK_INV         0x02
#define CLK_RESAMPLE    0x03
#define CLK_DIV1        0x04            /* 0x04 .. 0x0B: clock coefficients */
#define CLK_DLL         0x06
#define CLK_MUX         0x07
#define CLK_DAC_SEL     0x09
#define CLK_DIV_CPC     0x0C
#define CLK_TRI         0x0E
#define CLK_DIV_LRCK    0x0F
#define CLK_VMIDS1      0x10
#define CLK_VMIDS2      0x11
#define CLK_CAL_TIME    0x12
#define FMT             0x13
#define DAC_MUTE        0x14
#define ADC_MUTE        0x15
#define ANA_PDN         0x16
#define PGA_PDN         0x17
#define VMIDSEL         0x18
#define ANA_LP          0x19
#define ANA_MICBIAS     0x1B
#define ANA_VSEL        0x1C
#define SYS_BIAS        0x1D
#define VMIDLOW         0x22
#define PGAGAIN         0x23
#define HP_DRIVER       0x24
#define DAC2HPMIX       0x25
#define HP_VOL          0x26
#define HP_CAL          0x27
#define HP_DRIVER_REF   0x28
#define ADC1_SRC        0x2A
#define ADC2_SRC        0x2B
#define ADC1_VOL        0x2C
#define ADC2_VOL        0x2D
#define HP_OFFSET_CAL   0x4A
#define HPL_OFFSET_INI  0x4B
#define HPR_OFFSET_INI  0x4C
#define DAC_DSM         0x4D
#define DAC_RAMPRATE    0x4E
#define DAC_VPPSCALE    0x4F
#define DACL_VOL        0x50
#define DAC_CROSSTALK   0x55
#define HPJACK_TIMER    0x56
#define HPDET_TYPE      0x57
#define INT_SOURCE      0x58
#define INTOUT_IO       0x59
#define SDINOUT1_IO     0x5A
#define SDINOUT23_IO    0x5B
#define DACR_VOL        0xF4
#define HP_MISC         0xF7
#define PULLUP_CTL      0xF9
#define CSM_I2C_STA     0xFA
#define HPDET_STA       0xFB
#define CSM_MUTE_STA    0xFC
#define CHIP_ID1        0xFD
#define CHIP_ID2        0xFE
#define CHIP_VERSION    0xFF

#define VERSION_B       3
#define HPINSERT_FLAG   0x02            /* HPDET_STA */
#define HPBUTTON_FLAG   0x01            /* set: no microphone on the sleeve */
#define INT_SRC_BUTTON  0x04
#define INT_SRC_PIN9    0x08
#define DET_SRC_PIN9    0x10
#define JACK_POL        0x01            /* headset type auto (Linux default) */
#define HP_ON           0x77
#define HP_FORCE_CAL    0x88
#define MIC1_SEL        0x10

/* priv[] */
#define P_VER   0                       /* chip version */
#define P_AMP   1                       /* speaker amplifier state + 1, 0 unknown */
#define P_JACK  2                       /* 0 out, 1 type check running, 2 reported, 3 unknown */
#define P_JRES  3                       /* reported jack bits */
#define P_CAL   4                       /* headphone offsets calibrated */
#define P_USE   5                       /* streams running: bit 0 playback, bit 1 capture */

/* 48 kHz clock coefficients (registers 0x04 .. 0x0B) for each MCLK, from the Linux tables */
struct coeff { u32 mclk; u8 r[8]; };
static const struct coeff coeff_v0[] = {
    {  3072000UL, { 0xE0, 0x00, 0x03, 0x2D, 0x4A, 0x0A, 0x1F, 0x1F } },
    {  6144000UL, { 0xE0, 0x01, 0x03, 0x2D, 0x4A, 0x0A, 0x1F, 0x1F } },
    { 12288000UL, { 0xE0, 0x00, 0x30, 0x2D, 0x4A, 0x0A, 0x1F, 0x1F } },
    { 19200000UL, { 0xE9, 0x04, 0x0F, 0x6D, 0x4A, 0x0A, 0x1F, 0x1F } },
    { 24000000UL, { 0xF8, 0x04, 0x3F, 0x6D, 0x4A, 0x0A, 0x1F, 0x1F } },
    { 24576000UL, { 0xE0, 0x00, 0x00, 0x2D, 0x4A, 0x0A, 0x1F, 0x1F } }
};
static const struct coeff coeff_v3[] = {
    {  1536000UL, { 0x00, 0x00, 0x13, 0x2D, 0x8A, 0x0A, 0x1F, 0x1F } },
    {  2304000UL, { 0x00, 0x02, 0x0D, 0x2D, 0x8A, 0x0A, 0x1F, 0x1F } },
    {  3072000UL, { 0xE0, 0x00, 0x31, 0x2D, 0xCA, 0x0A, 0x1F, 0x1F } },
    {  4800000UL, { 0x04, 0x04, 0x3F, 0x6D, 0xB8, 0x08, 0x4F, 0x1F } },
    {  6000000UL, { 0x04, 0x04, 0x1F, 0x2D, 0x8A, 0x0A, 0x27, 0x27 } },
    {  6144000UL, { 0xE0, 0x00, 0x01, 0x2D, 0x48, 0x08, 0x1F, 0x1F } },
    {  9600000UL, { 0x04, 0x04, 0x0F, 0x2D, 0xCA, 0x0A, 0x1F, 0x1F } },
    { 12000000UL, { 0x04, 0x04, 0x0F, 0x2D, 0xCA, 0x0A, 0x27, 0x27 } },
    { 12288000UL, { 0xE0, 0x01, 0x01, 0x2D, 0x48, 0x08, 0x1F, 0x1F } },
    { 18432000UL, { 0x00, 0x02, 0x01, 0x2D, 0x8A, 0x0A, 0x1F, 0x1F } },
    { 19200000UL, { 0xE4, 0x04, 0x35, 0x6D, 0xCA, 0x0A, 0x1F, 0x1F } },
    { 24000000UL, { 0xF8, 0x04, 0x3F, 0x6D, 0xCA, 0x0A, 0x1F, 0x1F } },
    { 24576000UL, { 0xE0, 0x00, 0x00, 0x2D, 0x48, 0x08, 0x1F, 0x1F } },
    { 38400000UL, { 0x00, 0x18, 0x13, 0x2D, 0x8A, 0x0A, 0x1F, 0x1F } }
};

static int es_probe(struct cdc *c)
{
    int id1 = cdc_rd(c, CHIP_ID1), id2 = cdc_rd(c, CHIP_ID2);
    if (id1 < 0 || id2 < 0) return -1;
    /* the ACPI id names the chip; Linux does not check the id registers */
    if (id1 != 0x83 || id2 != 0x26) DBG("es8326: chip id %02x %02x\n", id1, id2);
    return 0;
}

static void spk_amp(struct cdc *c, int on)
{
    if (c->priv[P_AMP] == (u32)on + 1) return;
    c->priv[P_AMP] = (u32)on + 1;
    cdc_gpio(c, 0, on);
}

static void micbias(struct cdc *c, int on)
{
    cdc_upd(c, ANA_MICBIAS, 0x0C, on ? 0x0C : 0x00);
}

/* headphone driver offset calibration of the version B and later chips */
static void calibrate_b(struct cdc *c)
{
    int l, r;
    cdc_wr(c, CLK_INV, 0xC0);
    cdc_wr(c, CLK_DIV1, 0x03);
    cdc_wr(c, CLK_DLL, 0x30);
    cdc_wr(c, CLK_MUX, 0xED);
    cdc_wr(c, CLK_DAC_SEL, 0x08);
    cdc_wr(c, CLK_TRI, 0xC1);
    cdc_wr(c, DAC_MUTE, 0x03);
    cdc_wr(c, ANA_VSEL, 0x7F);
    cdc_wr(c, VMIDLOW, 0x23);
    cdc_wr(c, DAC2HPMIX, 0x88);
    mdelay(15);
    cdc_wr(c, HP_OFFSET_CAL, 0x8C);
    mdelay(15);
    cdc_wr(c, RESET, 0xC0);
    mdelay(15);
    cdc_wr(c, HP_OFFSET_CAL, 0x00);
    if ((cdc_rd(c, CSM_MUTE_STA) & 0xF0) != 0x40) mdelay(50);
    cdc_wr(c, HP_CAL, 0xD4);
    mdelay(200);
    cdc_wr(c, HP_CAL, 0x4D);
    mdelay(200);
    cdc_wr(c, HP_CAL, 0x00);
    l = cdc_rd(c, HPL_OFFSET_INI);
    r = cdc_rd(c, HPR_OFFSET_INI);
    cdc_wr(c, HP_OFFSET_CAL, 0x8C);
    if (l >= 0) cdc_wr(c, HPL_OFFSET_INI, (u32)l);
    if (r >= 0) cdc_wr(c, HPR_OFFSET_INI, (u32)r);
    cdc_wr(c, CLK_INV, 0x00);
    c->priv[P_CAL] = 1;
}

/* the older chips calibrate once at the first playback, with the clocks running */
static void calibrate_v0(struct cdc *c)
{
    int l, r;
    cdc_wr(c, HP_CAL, HP_FORCE_CAL);
    mdelay(30);
    cdc_wr(c, HP_CAL, 0x00);
    l = cdc_rd(c, HPL_OFFSET_INI);
    r = cdc_rd(c, HPR_OFFSET_INI);
    cdc_wr(c, HP_OFFSET_CAL, 0x8C);
    if (l >= 0) cdc_wr(c, HPL_OFFSET_INI, (u32)l);
    if (r >= 0) cdc_wr(c, HPR_OFFSET_INI, (u32)r);
    c->priv[P_CAL] = 1;
}

/* bias levels as Linux switches them: ON while a stream runs, STANDBY otherwise (internal
   clock for the jack detection) */
static void bias_on(struct cdc *c)
{
    cdc_upd(c, RESET, 0x02, 0x02);
    mdelay(5);
    cdc_wr(c, INTOUT_IO, 0x00);
    cdc_wr(c, SDINOUT1_IO, 0x90);               /* SDINOUT1 = DMIC clock */
    cdc_wr(c, PGA_PDN, 0x40);
    cdc_wr(c, ANA_PDN, 0x00);
    cdc_upd(c, CLK_CTL, 0x20, 0x20);
    cdc_upd(c, RESET, 0x02, 0x00);
    if (c->priv[P_VER] > VERSION_B) {
        cdc_upd(c, VMIDSEL, 0x40, 0x40);
        cdc_upd(c, ANA_MICBIAS, 0x70, 0x30);
    }
}

static void bias_standby(struct cdc *c)
{
    cdc_wr(c, ANA_PDN, 0x3B);
    cdc_upd(c, CLK_CTL, 0x20, 0x00);
    cdc_wr(c, SDINOUT1_IO, 0x00);
    if (c->priv[P_VER] > VERSION_B) {
        cdc_upd(c, VMIDSEL, 0x40, 0x40);
        cdc_upd(c, ANA_MICBIAS, 0x70, 0x10);
    }
    cdc_upd(c, CLK_INV, 0xC0, 0xC0);
    cdc_upd(c, CLK_MUX, 0x80, 0x80);
}

static const struct coeff *find_coeff(const struct coeff *t, int n, u32 clk)
{
    int i;
    for (i = 0; i < n; i++)
        if (t[i].mclk == clk) return &t[i];
    return NULL;
}

/* clock dividers for 48 kHz from MCLK, or from BCLK used as MCLK */
static int es_clock(struct cdc *c)
{
    const struct coeff *t = c->priv[P_VER] ? coeff_v3 : coeff_v0, *k;
    int n = c->priv[P_VER] ? (int)ARRAY_SIZE(coeff_v3) : (int)ARRAY_SIZE(coeff_v0), i, bclk = 0;
    u32 mclk = c->ssp.mclk ? c->ssp.mclk : 19200000UL;
    k = find_coeff(t, n, mclk);
    if (!k && c->ssp.bclk) {
        k = find_coeff(t, n, c->ssp.bclk);
        bclk = k != NULL;
    }
    if (!k) {
        kprintf("es8326: no 48 kHz clock setting for MCLK %lu / BCLK %lu Hz\n", mclk, c->ssp.bclk);
        return -1;
    }
    for (i = 0; i < 8; i++) cdc_wr(c, CLK_DIV1 + (u32)i, k->r[i]);
    cdc_upd(c, CLK_INV, 0x08, bclk ? 0x08 : 0x00);
    if (bclk) DBG("es8326: BCLK %lu Hz as MCLK\n", c->ssp.bclk);
    return 0;
}

static int es_init(struct cdc *c)
{
    u32 ver;
    u8 iface, len;
    int v = cdc_rd(c, CHIP_VERSION), ret;
    ver = v < 0 ? 0 : (u32)v;
    memset(c->priv, 0, sizeof(c->priv));
    c->priv[P_VER] = ver;
    c->priv[P_JACK] = 3;
    /* chip setup (es8326_init) */
    cdc_wr(c, RESET, 0x1F);
    cdc_wr(c, VMIDSEL, 0x3E);
    cdc_wr(c, ANA_LP, 0xF0);
    mdelay(10);
    cdc_wr(c, HPJACK_TIMER, 0xD9);
    cdc_wr(c, ANA_MICBIAS, 0xD8);
    cdc_wr(c, HPDET_TYPE, 0x83);                /* headphone default type and detect pin */
    cdc_wr(c, CLK_RESAMPLE, 0x05);
    cdc_wr(c, CLK_DIV_CPC, 0x89);               /* charge pump from the internal oscillator */
    cdc_wr(c, CLK_CTL, 0x7F);
    cdc_wr(c, RESET, 0x17);                     /* clock manager out of reset */
    cdc_wr(c, HP_MISC, 0x3D);                   /* headphone detection: half scan mode */
    cdc_wr(c, PULLUP_CTL, 0x00);
    cdc_wr(c, HP_VOL, 0xC4);                    /* headphone driver on */
    cdc_wr(c, HP_DRIVER, 0xA7);
    mdelay(2);
    cdc_wr(c, HP_DRIVER_REF, 0x23);
    cdc_wr(c, HP_DRIVER_REF, 0x33);
    cdc_wr(c, HP_DRIVER, 0xA1);
    cdc_wr(c, CLK_INV, 0x00);
    cdc_wr(c, CLK_VMIDS1, 0xC4);
    cdc_wr(c, CLK_VMIDS2, 0x81);
    cdc_wr(c, CLK_CAL_TIME, 0x00);
    if (ver >= VERSION_B) calibrate_b(c);
    cdc_wr(c, DAC_CROSSTALK, 0xAA);
    cdc_wr(c, DAC_RAMPRATE, 0x00);
    cdc_wr(c, HP_CAL, 0x00);                    /* headphone output off */
    cdc_wr(c, ANA_LP, 0xF0);                    /* ADC and DAC low power */
    cdc_wr(c, ANA_VSEL, 0x7F);
    cdc_wr(c, VMIDLOW, 0x03);                   /* micbias from VDDA */
    cdc_wr(c, DAC_DSM, 0x08);                   /* DAC dsmclip */
    cdc_wr(c, DAC_VPPSCALE, 0x15);
    cdc_wr(c, HPDET_TYPE, 0x80 | DET_SRC_PIN9 | JACK_POL | (ver >= VERSION_B ? 0 : 0x04));
    mdelay(5);
    micbias(c, 1);
    mdelay(50);
    cdc_upd(c, HPDET_TYPE, 0x03, 0x00);
    cdc_wr(c, INTOUT_IO, 0x00);
    cdc_wr(c, SDINOUT1_IO, 0x00);
    cdc_wr(c, SDINOUT23_IO, 0x00);
    cdc_wr(c, ANA_PDN, 0x00);
    cdc_wr(c, RESET, 0x80);                     /* current state machine on */
    cdc_upd(c, PGAGAIN, 0x30, MIC1_SEL);
    cdc_upd(c, DAC_MUTE, 0x03, 0x03);
    cdc_wr(c, ADC_MUTE, 0x0F);
    cdc_wr(c, CLK_DIV_LRCK, 0xFF);
    cdc_wr(c, ADC1_SRC, 0x44);                  /* digital microphone until a headset shows up */
    cdc_wr(c, ADC2_SRC, 0x66);
    micbias(c, 0);
    if (ver > VERSION_B) {
        cdc_upd(c, ANA_MICBIAS, 0x73, 0x10);
        cdc_upd(c, VMIDSEL, 0x40, 0x40);
    }
    mdelay(200);
    cdc_wr(c, INT_SOURCE, INT_SRC_PIN9);
    /* I2S consumer, sample size, clocks (es8326_set_dai_fmt / hw_params) */
    if (c->ssp.provider) kprintf("es8326: codec clock provider mode not supported, consumer used\n");
    switch (c->ssp.fmt) {
    case 3: iface = 0x01; break;                /* left justified */
    case 4: iface = 0x03; break;                /* DSP A */
    case 5: iface = 0x23; break;                /* DSP B */
    default: iface = 0x00; break;               /* I2S */
    }
    switch (c->ssp.vbits) {
    case 16: len = 0x0C; break;
    case 18: len = 0x08; break;
    case 20: len = 0x04; break;
    case 32: len = 0x10; break;
    default: len = 0x00; break;                 /* 24 */
    }
    cdc_upd(c, FMT, 0x3F, (u32)(iface | len));
    ret = es_clock(c);
    /* volumes: separate left / right headphone volume, DAC and ADC 0 dB, PGA +18 dB */
    cdc_upd(c, HP_MISC, 0xC0, 0x40);
    cdc_wr(c, DACL_VOL, 0xBF);
    cdc_wr(c, DACR_VOL, 0xBF);
    cdc_wr(c, ADC1_VOL, 0xBF);
    cdc_wr(c, ADC2_VOL, 0xBF);
    cdc_upd(c, PGAGAIN, 0x0F, 0x06);
    bias_standby(c);
    spk_amp(c, 1);
    return ret;
}

static void es_stream(struct cdc *c, int dir, int on)
{
    u32 bit = dir ? 2 : 1;
    if (on) {
        if (!c->priv[P_USE]) bias_on(c);
        c->priv[P_USE] |= bit;
        if (!c->priv[P_CAL]) calibrate_v0(c);
        cdc_upd(c, CLK_INV, 0xC0, 0x00);
        cdc_upd(c, CLK_MUX, 0x80, 0x00);
        if (!dir) {
            /* DAC -> headphone mixers -> headphone drivers */
            cdc_upd(c, DAC2HPMIX, 0x88, 0x88);
            cdc_upd(c, DAC_DSM, 0x01, 0x01);
            mdelay(2);
            cdc_upd(c, DAC_DSM, 0x01, 0x00);
            mdelay(2);
            cdc_upd(c, HP_DRIVER_REF, 0x30, 0x20);
            cdc_upd(c, HP_DRIVER_REF, 0x30, 0x30);
            cdc_wr(c, HP_DRIVER, 0xA1);
            cdc_wr(c, HP_CAL, HP_ON);
            cdc_upd(c, DAC_MUTE, 0x03, 0x00);
        } else {
            if (c->priv[P_VER] > VERSION_B) {
                cdc_upd(c, ANA_MICBIAS, 0x70, 0x70);
                cdc_upd(c, VMIDSEL, 0x40, 0x00);
            }
            mdelay(40);                         /* let the microphone bias settle */
            cdc_upd(c, ADC_MUTE, 0x0F, 0x00);
        }
    } else {
        if (!dir) {
            cdc_wr(c, HP_CAL, 0x00);
            cdc_upd(c, DAC_MUTE, 0x03, 0x03);
            cdc_upd(c, HP_DRIVER_REF, 0x30, 0x00);
            cdc_upd(c, DAC2HPMIX, 0x88, 0x00);
        } else {
            cdc_upd(c, ADC_MUTE, 0x0F, 0x0F);
            if (c->priv[P_VER] > VERSION_B) {
                cdc_upd(c, VMIDSEL, 0x40, 0x40);
                cdc_upd(c, ANA_MICBIAS, 0x70, 0x30);
            }
        }
        c->priv[P_USE] &= ~bit;
        if (!c->priv[P_USE]) bias_standby(c);
    }
}

static void es_volume(struct cdc *c, int ctl, u32 lr)
{
    if (ctl == MX_MASTER) {
        /* DAC: 0.5 dB steps up to 0 dB (0xBF), the upper 64 dB of it */
        u32 l = cdc_vstep(CDC_L(lr), 128, 500), r = cdc_vstep(CDC_R(lr), 128, 500);
        cdc_wr(c, DACL_VOL, l ? 0x3F + l : 0);
        cdc_wr(c, DACR_VOL, r ? 0x3F + r : 0);
    } else if (ctl == MX_INGAIN) {
        u32 g = cdc_step(CDC_L(lr), 10);        /* analog PGA 0 .. +30 dB */
        cdc_upd(c, PGAGAIN, 0x0F, g);
        cdc_wr(c, ADC1_VOL, CDC_L(lr) ? 0xBF : 0x00);
        cdc_wr(c, ADC2_VOL, CDC_L(lr) ? 0xBF : 0x00);
    }
}

static int es_jack(struct cdc *c)
{
    int sta = cdc_rd(c, HPDET_STA), j;
    if (sta < 0) return -1;
    if (!(sta & HPINSERT_FLAG)) {
        if (c->priv[P_JACK]) {
            /* unplugged: digital microphone, back to insertion detection */
            cdc_wr(c, ADC1_SRC, 0x44);
            cdc_wr(c, ADC2_SRC, 0x66);
            cdc_upd(c, HPDET_TYPE, 0x03, 0x01);
            cdc_wr(c, SYS_BIAS, 0x0A);
            cdc_upd(c, HP_DRIVER_REF, 0x0F, 0x03);
            cdc_wr(c, INT_SOURCE, INT_SRC_PIN9);
            c->priv[P_JACK] = 0;
        }
        j = 0;
    } else if (c->priv[P_JACK] == 0 || c->priv[P_JACK] == 3) {
        /* plugged: CTIA / OMTP auto check, the result is read at the next poll */
        cdc_wr(c, INT_SOURCE, 0x00);
        cdc_upd(c, HPDET_TYPE, 0x03, 0x01);
        cdc_upd(c, HPDET_TYPE, 0x10, 0x00);
        mdelay(50);
        cdc_upd(c, HPDET_TYPE, 0x03, 0x00);
        cdc_upd(c, HPDET_TYPE, 0x10, 0x10);
        mdelay(50);
        cdc_wr(c, INT_SOURCE, INT_SRC_PIN9 | INT_SRC_BUTTON);
        cdc_wr(c, SYS_BIAS, 0x1F);
        cdc_upd(c, HP_DRIVER_REF, 0x0F, 0x0D);
        c->priv[P_JACK] = 1;
        return c->jackstate;
    } else if (c->priv[P_JACK] == 1) {
        if (sta & HPBUTTON_FLAG) {
            j = 1;                              /* headphones */
        } else {
            /* headset: analog microphone input */
            j = 3;
            cdc_upd(c, PGA_PDN, 0x08, 0x08);
            cdc_wr(c, ADC1_SRC, 0x00);
            cdc_wr(c, ADC2_SRC, 0x00);
            cdc_upd(c, PGA_PDN, 0x08, 0x00);
            mdelay(10);
        }
        c->priv[P_JACK] = 2;
        c->priv[P_JRES] = (u32)j;
    } else {
        j = (int)c->priv[P_JRES];
    }
    if (j != c->jackstate) spk_amp(c, !(j & 1));
    return j;
}

static void es_off(struct cdc *c)
{
    spk_amp(c, 0);
    cdc_wr(c, HP_CAL, 0x00);
    cdc_upd(c, DAC_MUTE, 0x03, 0x03);
    cdc_wr(c, ADC_MUTE, 0x0F);
    micbias(c, 0);
    cdc_wr(c, CLK_MUX, 0x2D);
    cdc_wr(c, DAC2HPMIX, 0x00);
    cdc_wr(c, ANA_PDN, 0x3B);
    cdc_wr(c, CLK_CTL, 0x00);
    /* register defaults back */
    cdc_wr(c, CSM_I2C_STA, 0x01);
    mdelay(2);
    cdc_wr(c, CSM_I2C_STA, 0x00);
    c->priv[P_USE] = 0;
}

const struct cdcdrv cdc_es8326 = { "es8326", { "ESSX8326", NULL }, CK_SPKCODEC, 8, 8, 0,
                                   es_probe, es_init, es_stream, es_volume, NULL, es_jack, es_off };
