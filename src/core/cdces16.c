/* cdces16.c - Everest ES8316 / ES8336 headphone codec (Cherry Trail tablets, Gemini Lake,
   Jasper Lake, Tiger / Alder Lake laptops). The speakers hang on the headphone outputs behind
   an amplifier that the first GpioIo of the codec's ACPI device switches on; the internal
   microphone is on MIC1, the headset microphone on MIC2. Register sequences after the Linux
   es8316 and sof_es8336 drivers. */
#include "dspcdc.h"

#define RESET           0x00
#define CLKSW           0x01
#define ADCOSR          0x03
#define ADCDIV1         0x04
#define ADCDIV2         0x05
#define DACDIV1         0x06
#define DACDIV2         0x07
#define SERDATA1        0x09
#define SERDATA_ADC     0x0A
#define SERDATA_DAC     0x0B
#define VMIDSEL         0x0C
#define SYS_PDN         0x0D
#define HPMIX_SEL       0x13
#define HPMIX_SWITCH    0x14
#define HPMIX_PDN       0x15
#define HPMIX_VOL       0x16
#define CPHP_OUTEN      0x17
#define CPHP_ICAL_VOL   0x18
#define CPHP_PDN1       0x19
#define CPHP_PDN2       0x1A
#define ADC_PDN_LINSEL  0x22
#define ADC_PGAGAIN     0x23
#define ADC_D2SEPGA     0x24
#define ADC_DMIC        0x25
#define ADC_MUTE        0x26
#define ADC_VOLUME      0x27
#define ADC_ALC1        0x29
#define DAC_PDN         0x2F
#define DAC_SET1        0x30
#define DAC_VOLL        0x33
#define DAC_VOLR        0x34
#define GPIO_DEBOUNCE   0x4E
#define GPIO_FLAG       0x4F

#define FLAG_GM_NOT_SHORTED     0x02
#define FLAG_HP_NOT_INSERTED    0x04

/* priv[0]: microphone input in use (0 MIC1, 1 MIC2), priv[1]: speaker amplifier state */

static int es_probe(struct cdc *c)
{
    /* no id register: the chip answers and the clock switch register reads back */
    return cdc_rd(c, CLKSW) < 0 ? -1 : 0;
}

static void spk_amp(struct cdc *c, int on)
{
    if (c->priv[1] == (u32)on + 1) return;
    c->priv[1] = (u32)on + 1;
    cdc_gpio(c, 0, on);
}

static void set_mic(struct cdc *c, int mic2)
{
    c->priv[0] = (u32)mic2;
    cdc_upd(c, ADC_PDN_LINSEL, 0x30, mic2 ? 0x10 : 0x00);
}

static int es_init(struct cdc *c)
{
    static const u32 ratios[] = { 256, 384, 400, 500, 512, 768, 1024 };
    u32 sysclk = c->ssp.mclk ? c->ssp.mclk : 19200000UL, clk, lrck;
    int i, half = 0;
    u8 len;
    /* reset, current state machine on */
    cdc_wr(c, RESET, 0x3F);
    mdelay(6);
    cdc_wr(c, RESET, 0x80);
    mdelay(30);
    cdc_wr(c, VMIDSEL, 0xFF);
    cdc_wr(c, ADCOSR, 0x32);
    /* I2S consumer, MCLK and BCLK inputs on */
    cdc_upd(c, SERDATA1, 0xA0, c->ssp.provider ? 0x80 : 0);
    cdc_upd(c, SERDATA_ADC, 0x23, 0);
    cdc_upd(c, SERDATA_DAC, 0x23, 0);
    cdc_upd(c, CLKSW, 0x60, 0x60);
    /* LRCK divider from MCLK: halved MCLK first */
    for (clk = sysclk / 2;; clk = sysclk) {
        for (i = 0; i < (int)ARRAY_SIZE(ratios); i++)
            if (clk % ratios[i] == 0 && clk / ratios[i] == CDC_RATE) break;
        if (i < (int)ARRAY_SIZE(ratios)) break;
        if (clk == sysclk) {
            kprintf("es8316: MCLK %lu Hz does not divide to %lu Hz\n", sysclk, CDC_RATE);
            break;
        }
    }
    if (clk != sysclk) half = 1;
    if (half) cdc_upd(c, CLKSW, 0x80, 0x80);
    lrck = clk / CDC_RATE;
    switch (c->ssp.vbits) {
    case 16: len = 0x0C; break;
    case 20: len = 0x04; break;
    case 32: len = 0x10; break;
    default: len = 0x00; break;                 /* 24 */
    }
    cdc_upd(c, SERDATA_DAC, 0x1C, len);
    cdc_upd(c, SERDATA_ADC, 0x1C, len);
    cdc_upd(c, SERDATA1, 0x1F, (lrck / 4) / (c->ssp.vbits ? c->ssp.vbits : 24));
    cdc_upd(c, ADCDIV1, 0x0F, lrck >> 8);
    cdc_upd(c, ADCDIV2, 0xFF, lrck & 0xFF);
    cdc_upd(c, DACDIV1, 0x0F, lrck >> 8);
    cdc_upd(c, DACDIV2, 0xFF, lrck & 0xFF);
    /* power: bias, analog, mic bias, ADC and DAC references; DAC, ADC and charge pump clocks */
    cdc_upd(c, SYS_PDN, 0x3F, 0x00);
    cdc_upd(c, CLKSW, 0x1C, 0x1C);
    /* playback: DAC -> headphone mixer -> charge pump -> headphone drivers */
    cdc_upd(c, DAC_SET1, 0xE0, 0x20);           /* straight L / R, muted until a stream runs */
    cdc_upd(c, DAC_PDN, 0x11, 0x00);
    cdc_wr(c, HPMIX_SEL, 0x00);
    cdc_wr(c, HPMIX_SWITCH, 0x88);
    cdc_wr(c, HPMIX_VOL, 0xBB);                 /* mixer 0 dB */
    cdc_upd(c, HPMIX_PDN, 0x33, 0x00);
    cdc_upd(c, CPHP_PDN2, 0x20, 0x00);
    cdc_upd(c, CPHP_PDN1, 0x04, 0x00);
    cdc_wr(c, CPHP_ICAL_VOL, 0x00);             /* ical on, headphones 0 dB */
    cdc_upd(c, CPHP_OUTEN, 0x66, 0x66);
    cdc_wr(c, DAC_VOLL, 0x00);
    cdc_wr(c, DAC_VOLR, 0x00);
    /* capture: MIC1 -> PGA -> mono ADC -> I2S */
    cdc_wr(c, ADC_PGAGAIN, 0x80);               /* +18 dB */
    cdc_upd(c, ADC_D2SEPGA, 0x01, 0x01);        /* mic boost */
    cdc_upd(c, ADC_DMIC, 0x03, 0x00);
    cdc_wr(c, ADC_VOLUME, 0x00);
    cdc_upd(c, ADC_PDN_LINSEL, 0xF0, 0x00);
    cdc_upd(c, SERDATA_ADC, 0x40, 0x00);
    /* headphone and microphone detection */
    cdc_upd(c, GPIO_DEBOUNCE, 0x02, 0x02);
    mdelay(20);
    c->priv[1] = 0;
    spk_amp(c, 1);
    return 0;
}

static void es_stream(struct cdc *c, int dir, int on)
{
    if (!dir) cdc_upd(c, DAC_SET1, 0x20, on ? 0 : 0x20);
}

static void es_volume(struct cdc *c, int ctl, u32 lr)
{
    if (ctl == MX_MASTER) {
        /* DAC: 0.5 dB steps from 0 down, the upper 64 dB of it */
        u32 l = cdc_step(CDC_L(lr), 128), r = cdc_step(CDC_R(lr), 128);
        cdc_wr(c, DAC_VOLL, l ? 128 - l : 0xC0);
        cdc_wr(c, DAC_VOLR, r ? 128 - r : 0xC0);
    } else if (ctl == MX_INGAIN) {
        u32 g = cdc_step(CDC_L(lr), 10);        /* PGA -3.5 .. +24 dB */
        cdc_upd(c, ADC_PGAGAIN, 0xF0, g << 4);
        cdc_upd(c, ADC_MUTE, 0x20, CDC_L(lr) ? 0 : 0x20);
    }
}

static int es_jack(struct cdc *c)
{
    int f = cdc_rd(c, GPIO_FLAG), j = 0;
    if (f <= 0) return -1;
    if (!(f & FLAG_HP_NOT_INSERTED)) j = (f & FLAG_GM_NOT_SHORTED) ? 3 : 1;
    if (j != c->jackstate) {
        spk_amp(c, !(j & 1));
        set_mic(c, (j & 2) ? 1 : 0);
    }
    return j;
}

static void es_off(struct cdc *c)
{
    spk_amp(c, 0);
    cdc_upd(c, DAC_SET1, 0x20, 0x20);
    cdc_upd(c, CPHP_OUTEN, 0x66, 0x00);
    cdc_upd(c, SYS_PDN, 0x3F, 0x3F);
}

const struct cdcdrv cdc_es8316 = { "es8316", { "ESSX8316", NULL }, CK_SPKCODEC, 8, 8, 0,
                                   es_probe, es_init, es_stream, es_volume, NULL, es_jack, es_off };
const struct cdcdrv cdc_es8336 = { "es8336", { "ESSX8336", NULL }, CK_SPKCODEC, 8, 8, 0,
                                   es_probe, es_init, es_stream, es_volume, NULL, es_jack, es_off };
