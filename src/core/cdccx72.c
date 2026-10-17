/* cdccx72.c - Conexant CX20721 / CX20723 codec (14F10720: Bay Trail / Cherry Trail tablets,
   bytcht_cx2072x). An HD Audio style codec on I2C: 16-bit register addresses, 1, 2 or 4 byte
   little endian values. Machine wiring after the Linux machine driver: SSP2 in I2S, 2 x 25 bit
   slots (BCLK = 50 x fs), codec consumer, PLL from the 19.2 MHz MCLK (pmc_plt_clk_3);
   headphones on port A, speakers on port G (class-D), internal microphone on port C, headset
   microphone on port D; DAC1 feeds both outputs, ADC1 records the selected microphone. Jack:
   port A presence and the headset type from the codec's detection, polled. Register sequences
   after the Linux cx2072x driver. */
#include "dspcdc.h"

#define VENDOR_ID               0x0200
#define AFG_POWER_STATE         0x0414
#define UM_RESPONSE             0x0420
#define GPIO_ENABLE             0x0458
#define GPIO_UM_ENABLE          0x0464
#define GPIO_STICKY_MASK        0x0468
#define DAC1_AMP_GAIN_RIGHT     0x41c0
#define DAC1_AMP_GAIN_LEFT      0x41e0
#define DAC1_CONVERTER_FORMAT   0x43c8
#define DAC1_POWER_STATE        0x4014
#define DAC2_POWER_STATE        0x4414
#define ADC1_AMP_GAIN_RIGHT_1   0x4d84          /* port D input */
#define ADC1_AMP_GAIN_RIGHT_2   0x4d88          /* port C input */
#define ADC1_AMP_GAIN_LEFT_1    0x4da4
#define ADC1_AMP_GAIN_LEFT_2    0x4da8
#define ADC1_CONNECTION_SELECT  0x4c04
#define ADC1_POWER_STATE        0x4c14
#define ADC1_CONVERTER_FORMAT   0x4fc8
#define ADC2_POWER_STATE        0x5014
#define MIXER_POWER_STATE       0x5414
#define PORTA_CONNECTION_SELECT 0x5804
#define PORTA_POWER_STATE       0x5814
#define PORTA_PIN_CTRL          0x581c
#define PORTA_UNSOL_RESPONSE    0x5820
#define PORTA_PIN_SENSE         0x5824
#define PORTB_POWER_STATE       0x6014
#define PORTC_POWER_STATE       0x6814
#define PORTC_PIN_CTRL          0x681c
#define PORTC_GAIN_RIGHT        0x6980
#define PORTC_GAIN_LEFT         0x69a0
#define PORTD_POWER_STATE       0x6414
#define PORTD_PIN_CTRL          0x641c
#define PORTD_GAIN_RIGHT        0x6580
#define PORTD_GAIN_LEFT         0x65a0
#define PORTE_POWER_STATE       0x7414
#define PORTG_POWER_STATE       0x5c14
#define PORTG_PIN_CTRL          0x5c1c
#define PORTG_CONNECTION_SELECT 0x5c04
#define PORTG_EAPD_BTL          0x5c30
#define PORTM_POWER_STATE       0x8814
#define DIGITAL_BIOS_TEST0      0x6d80
#define DIGITAL_BIOS_TEST2      0x6d84
#define I2SPCM_CONTROL1         0x6e00
#define I2SPCM_CONTROL2         0x6e04
#define I2SPCM_CONTROL3         0x6e08
#define I2SPCM_CONTROL4         0x6e0c
#define I2SPCM_CONTROL5         0x6e10
#define UM_INTERRUPT_CRTL_E     0x6e14
#define I2SPCM_CONTROL6         0x6e18
#define CODEC_TEST9             0x7124
#define CODEC_TESTXX            0x7290
#define CODEC_TEST20            0x7310
#define CODEC_TEST24            0x731c
#define CODEC_TEST26            0x7328
#define ANALOG_TEST3            0x718c
#define ANALOG_TEST4            0x7190
#define ANALOG_TEST6            0x7198
#define ANALOG_TEST7            0x719c
#define ANALOG_TEST8            0x71a0
#define ANALOG_TEST9            0x71a4
#define ANALOG_TEST10           0x71a8
#define ANALOG_TEST11           0x71ac
#define ANALOG_TEST12           0x71b0
#define DIGITAL_TEST0           0x7200
#define DIGITAL_TEST1           0x7204
#define DIGITAL_TEST11          0x722c
#define DIGITAL_TEST12          0x7230
#define DIGITAL_TEST15          0x723c
#define DIGITAL_TEST16          0x7080
#define DIGITAL_TEST17          0x7084
#define DIGITAL_TEST18          0x7088
#define DIGITAL_TEST19          0x708c
#define DIGITAL_TEST20          0x7090

#define GAIN_MUTE               0x80
#define GAIN_0DB                0x4a            /* 74 steps of 1 dB from -74 dB */

/* priv[0]: jack state (bit 0 headphones, bit 1 headset microphone), priv[1]: playing */

/* ---------------------------------------------------------------- register access */

static const u16 regs4[] = {
    0x0200, 0x0208, 0x5824, 0x6024, 0x6424, 0x7424, 0x7824, 0x6e00, 0x6e04, 0x6e08,
    0x6e0c, 0x6e10, 0x6e14, 0x6e18, 0x6d0c, 0x6d14, 0x6d18, 0x6d80, 0x6d84
};
static const u16 regs2[] = {
    0x6d00, 0x6d02, 0x6d04, 0x6d06, 0x6d08, 0x6d0a, 0x43c8, 0x47c8, 0x4fc8, 0x53c8,
    0x7108, 0x7124, 0x7310, 0x7328, 0x718c, 0x7190, 0x7194, 0x7198, 0x719c, 0x71a0,
    0x71a4, 0x71a8, 0x71ac, 0x71b0, 0x71b4, 0x7200, 0x7204, 0x722c, 0x7230, 0x723c,
    0x7080, 0x7084, 0x7088, 0x708c, 0x7090
};

/* value width of a register: 4, 2 or (the rest) 1 byte */
static int cx_size(u32 reg)
{
    int i;
    for (i = 0; i < (int)ARRAY_SIZE(regs4); i++)
        if (regs4[i] == reg) return 4;
    for (i = 0; i < (int)ARRAY_SIZE(regs2); i++)
        if (regs2[i] == reg) return 2;
    return 1;
}

static int cx_wr(struct cdc *c, u32 reg, u32 val)
{
    u8 w[6];
    int i, n = cx_size(reg);
    if (reg == UM_INTERRUPT_CRTL_E) {           /* only the top byte is written */
        reg += 3;
        n = 1;
        val >>= 24;
    }
    w[0] = (u8)(reg >> 8);
    w[1] = (u8)reg;
    for (i = 0; i < n; i++) w[2 + i] = (u8)(val >> (8 * i));
    return i2c_xfer(c->bus, c->addr, w, 2 + n, NULL, 0);
}

static int cx_rd(struct cdc *c, u32 reg, u32 *val)
{
    u8 w[2], r[4];
    int i, n = cx_size(reg);
    w[0] = (u8)(reg >> 8);
    w[1] = (u8)reg;
    if (i2c_xfer(c->bus, c->addr, w, 2, r, n)) return -1;
    *val = 0;
    for (i = 0; i < n; i++) *val |= (u32)r[i] << (8 * i);
    return 0;
}

static int cx_upd(struct cdc *c, u32 reg, u32 mask, u32 val)
{
    u32 v;
    if (cx_rd(c, reg, &v)) return -1;
    return cx_wr(c, reg, (v & ~mask) | (val & mask));
}

/* ---------------------------------------------------------------- setup */

static int cx_probe(struct cdc *c)
{
    u32 id;
    if (!c->bus || cx_rd(c, VENDOR_ID, &id)) return -1;
    if ((id >> 16) != 0x14F1 && (id & 0xFFFF) != 0x14F1) {
        DBG("cx2072x: vendor id %08lx\n", id);
        return -1;
    }
    DBG("cx2072x: id %08lx\n", id);
    return 0;
}

/* PLL to 3072 x 48 kHz from MCLK (pre-divider to about 6 MHz, integer + fraction) */
static void cx_pll(struct cdc *c, u32 mclk)
{
    static const u32 premclk[] = { 6144000, 12288000, 19200000, 26000000, 28224000, 36864000,
                                   36864000, 48000000 };
    u32 pre = 8, in, out = CDC_RATE * 3072, idiv, rem, frac = 0;
    int i;
    for (i = 0; i < (int)ARRAY_SIZE(premclk); i++)
        if (mclk <= premclk[i]) {
            pre = (u32)i + 1;
            break;
        }
    in = mclk / pre;
    idiv = out / in;
    rem = out - idiv * in;
    if (rem) {
        /* fraction in 1/1000, then (4000 + f) * (2^20 - 4) / 7 / 1000 rounded */
        u32 f = rem * 125 / (in / 8);
        frac = ((4000 + f) * 149796UL + 499) / 1000;
    }
    cx_wr(c, ANALOG_TEST4, 0x40 | (((pre - 1) * 2) << 8));
    if (!rem) {
        cx_wr(c, ANALOG_TEST7, 0x100);          /* integer mode */
    } else {
        cx_wr(c, ANALOG_TEST6, frac & 0xFFF);
        cx_wr(c, ANALOG_TEST7, (frac >> 12) & 0xFF);
    }
    cx_wr(c, ANALOG_TEST8, idiv - 1);
    if (!rem) {
        cx_wr(c, DIGITAL_TEST16, 0x00);         /* no PLL tracking */
    } else {
        /* PLL tracking: 2 samples per sync, 96 clocks per sample */
        cx_wr(c, DIGITAL_TEST16, 0x20);
        cx_wr(c, DIGITAL_TEST17, 96);
        cx_wr(c, DIGITAL_TEST18, 144);
        cx_wr(c, DIGITAL_TEST19, 0x01);
        cx_wr(c, DIGITAL_TEST20, 0x02);
        cx_upd(c, DIGITAL_TEST16, 0x01, 0x01);
    }
    DBG("cx2072x: PLL from %lu Hz: /%lu x %lu frac %lu\n", mclk, pre, idiv, frac);
}

/* I2S consumer: frame of slots x width BCLKs, left channel in the first half, right channel
   in the second (byte slots, plus pause cycles for odd halves such as 25 bits) */
static void cx_i2s(struct cdc *c)
{
    u32 frame = (u32)(c->ssp.slots ? c->ssp.slots : 2) * (c->ssp.width ? c->ssp.width : 25);
    u32 bits = c->ssp.vbits == 16 ? 16 : 24, half = frame / 2, rslot, pause, dly, r1, r2, r3;
    if (c->ssp.provider) kprintf("cx2072x: clock provider mode not supported\n");
    if (c->ssp.fmt != 1 && c->ssp.fmt != 3)
        kprintf("cx2072x: SSP format %u not supported, using I2S\n", c->ssp.fmt);
    dly = c->ssp.fmt == 3 ? 0 : 1;              /* I2S: data one BCLK after LRCK */
    rslot = half / 8;
    pause = half % 8;
    /* both directions: one data line, inverted word select, pulse of half a frame */
    r1 = 1 | (1UL << 1) | ((half - 1) << 2) | ((frame / 8 - 1) << 9) | ((bits / 8 - 1) << 14);
    r1 |= r1 << 16;
    cx_wr(c, I2SPCM_CONTROL5, 0);               /* no BCLK output */
    cx_wr(c, I2SPCM_CONTROL1, r1);
    /* ADC1 L/R out in slots 0 and right; DAC1 L/R in from the same slots */
    r2 = 0x03 | (rslot << 11) | (dly << 31);
    r3 = 0x03 | (rslot << 11) | (dly << 31);
    cx_wr(c, I2SPCM_CONTROL2, r2);
    cx_wr(c, I2SPCM_CONTROL3, r3);
    cx_wr(c, I2SPCM_CONTROL4, 0);
    cx_wr(c, I2SPCM_CONTROL6, (pause << 5) | ((rslot * 8) << 8) | (pause << 21) | ((rslot * 8) << 24));
    cx_wr(c, I2SPCM_CONTROL5, 0);
    /* EAPD pad, BCLK input on and inverted for I2S (the Linux setting) */
    cx_wr(c, DIGITAL_BIOS_TEST2, 0xAC | 0x100);
    DBG("cx2072x: I2S frame %lu, %lu bit samples\n", frame, bits);
}

/* outputs and microphone for the jack state and the playback state */
static void cx_route(struct cdc *c)
{
    int hp = (c->priv[0] & 1) != 0;
    cx_wr(c, PORTA_PIN_CTRL, hp ? 0xC0 : 0x00);                 /* headphone amp + out */
    cx_wr(c, PORTG_PIN_CTRL, !hp && c->priv[1] ? 0x40 : 0x00);  /* speaker out */
    cx_wr(c, ADC1_CONNECTION_SELECT, (c->priv[0] & 2) ? 1 : 2); /* port D : port C */
}

static int cx_init(struct cdc *c)
{
    static const u16 seq[] = {
        ANALOG_TEST9, 0x080,                    /* DC offset calibration */
        CODEC_TEST26, 0x65f,                    /* PA off */
        ANALOG_TEST10, 0x289,                   /* speaker output gain */
        CODEC_TEST20, 0xf05,
        CODEC_TESTXX, 0x380,
        CODEC_TEST26, 0xb90,
        CODEC_TEST9, 0x001,                     /* 30 Hz high pass */
        ANALOG_TEST3, 0x300,                    /* PC beep pad off */
        CODEC_TEST24, 0x100,                    /* SnM mode off */
        PORTD_PIN_CTRL, 0x020,                  /* port D input */
        GPIO_ENABLE, 0x040,                     /* GPIO7: headset button */
        GPIO_UM_ENABLE, 0x040,
        UM_RESPONSE, 0x080,
        DIGITAL_TEST12, 0x0c4,                  /* headset button */
        DIGITAL_TEST0, 0x415                    /* class-D down when idle */
    };
    static const u16 pwr_on[] = {
        DAC1_POWER_STATE, ADC1_POWER_STATE, PORTA_POWER_STATE, PORTG_POWER_STATE,
        PORTC_POWER_STATE, PORTD_POWER_STATE
    };
    static const u16 pwr_off[] = {
        DAC2_POWER_STATE, ADC2_POWER_STATE, MIXER_POWER_STATE, PORTB_POWER_STATE,
        PORTE_POWER_STATE, PORTM_POWER_STATE
    };
    int i;
    cx_wr(c, AFG_POWER_STATE, 0);
    mdelay(10);
    for (i = 0; i < (int)ARRAY_SIZE(seq); i += 2) cx_wr(c, seq[i], seq[i + 1]);
    cx_upd(c, PORTC_PIN_CTRL, 0x20, 0x20);      /* port C input */
    cx_upd(c, DIGITAL_BIOS_TEST2, 0x84, 0x84);
    cx_pll(c, c->ssp.mclk ? c->ssp.mclk : 19200000UL);
    cx_i2s(c);
    /* playback: DAC1 (24 bit stereo 48 kHz) -> port A (headphones) and port G (speakers) */
    cx_wr(c, DAC1_CONVERTER_FORMAT, 0x31);
    cx_wr(c, ADC1_CONVERTER_FORMAT, 0x31);
    cx_wr(c, PORTA_CONNECTION_SELECT, 0);
    cx_wr(c, PORTG_CONNECTION_SELECT, 0);
    cx_wr(c, PORTG_EAPD_BTL, 0x02);             /* EAPD on */
    cx_wr(c, DAC1_AMP_GAIN_LEFT, GAIN_0DB);
    cx_wr(c, DAC1_AMP_GAIN_RIGHT, GAIN_0DB);
    /* capture: port C / port D -> ADC1 at 0 dB, +12 dB boost */
    cx_wr(c, ADC1_AMP_GAIN_LEFT_1, GAIN_0DB);
    cx_wr(c, ADC1_AMP_GAIN_RIGHT_1, GAIN_0DB);
    cx_wr(c, ADC1_AMP_GAIN_LEFT_2, GAIN_0DB);
    cx_wr(c, ADC1_AMP_GAIN_RIGHT_2, GAIN_0DB);
    cx_wr(c, PORTC_GAIN_LEFT, 1);
    cx_wr(c, PORTC_GAIN_RIGHT, 1);
    cx_wr(c, PORTD_GAIN_LEFT, 1);
    cx_wr(c, PORTD_GAIN_RIGHT, 1);
    for (i = 0; i < (int)ARRAY_SIZE(pwr_on); i++) cx_wr(c, pwr_on[i], 0);
    for (i = 0; i < (int)ARRAY_SIZE(pwr_off); i++) cx_wr(c, pwr_off[i], 3);
    /* jack detection: unsolicited response on port A, headset type detection with the
       headset bias and the port D microphone bias on */
    cx_wr(c, GPIO_STICKY_MASK, 0x1f);
    cx_wr(c, UM_INTERRUPT_CRTL_E, 0x12UL << 24);
    cx_wr(c, PORTA_UNSOL_RESPONSE, 0x80);
    cx_wr(c, DIGITAL_TEST15, 0x73);             /* Nokia and Apple headsets, 275 ms */
    cx_wr(c, ANALOG_TEST12, 0x300);             /* no tip detection */
    cx_wr(c, DIGITAL_TEST1, 0);
    cx_upd(c, ANALOG_TEST11, 0x02, 0x02);
    cx_upd(c, PORTD_PIN_CTRL, 0x04, 0x04);
    c->priv[0] = 0;
    c->priv[1] = 0;
    cx_route(c);
    return 0;
}

static void cx_stream(struct cdc *c, int dir, int on)
{
    if (dir) return;
    c->priv[1] = (u32)on;
    cx_route(c);
}

static void cx_volume(struct cdc *c, int ctl, u32 lr)
{
    if (ctl == MX_MASTER) {
        u32 l = cdc_step(CDC_L(lr), GAIN_0DB), r = cdc_step(CDC_R(lr), GAIN_0DB);
        cx_wr(c, DAC1_AMP_GAIN_LEFT, l ? l : GAIN_MUTE);
        cx_wr(c, DAC1_AMP_GAIN_RIGHT, r ? r : GAIN_MUTE);
    } else if (ctl == MX_INGAIN) {
        /* -74 .. +36 dB: ADC1 gain plus 12 dB boost steps of the microphone ports */
        u32 s = cdc_step(CDC_L(lr), 110), b = 0, g;
        if (s > GAIN_0DB) b = (s - GAIN_0DB + 11) / 12;
        if (b > 3) b = 3;
        g = s ? s - 12 * b : GAIN_MUTE;
        cx_wr(c, PORTC_GAIN_LEFT, b);
        cx_wr(c, PORTC_GAIN_RIGHT, b);
        cx_wr(c, PORTD_GAIN_LEFT, b);
        cx_wr(c, PORTD_GAIN_RIGHT, b);
        cx_wr(c, ADC1_AMP_GAIN_LEFT_1, g);
        cx_wr(c, ADC1_AMP_GAIN_RIGHT_1, g);
        cx_wr(c, ADC1_AMP_GAIN_LEFT_2, g);
        cx_wr(c, ADC1_AMP_GAIN_RIGHT_2, g);
    }
}

static int cx_jack(struct cdc *c)
{
    u32 sense, type;
    int j = 0;
    if (cx_rd(c, PORTA_PIN_SENSE, &sense) || cx_rd(c, DIGITAL_TEST11, &type)) return -1;
    if ((sense >> 24) & 0x80) j = ((type >> 8) & 0x08) ? 3 : 1;    /* CTIA headset : headphones */
    cx_wr(c, UM_INTERRUPT_CRTL_E, 0x12UL << 24);                    /* clear the event */
    if ((u32)j != c->priv[0]) {
        c->priv[0] = (u32)j;
        cx_route(c);
    }
    return j;
}

static void cx_off(struct cdc *c)
{
    static const u16 pwr[] = {
        PORTA_POWER_STATE, PORTG_POWER_STATE, PORTC_POWER_STATE, PORTD_POWER_STATE,
        DAC1_POWER_STATE, ADC1_POWER_STATE
    };
    int i;
    cx_wr(c, PORTG_PIN_CTRL, 0);
    cx_wr(c, PORTA_PIN_CTRL, 0);
    cx_wr(c, UM_INTERRUPT_CRTL_E, 0);
    cx_wr(c, PORTA_UNSOL_RESPONSE, 0);
    for (i = 0; i < (int)ARRAY_SIZE(pwr); i++) cx_wr(c, pwr[i], 3);
    cx_upd(c, DIGITAL_BIOS_TEST0, 0x10, 0x10);
    cx_wr(c, AFG_POWER_STATE, 3);
}

const struct cdcdrv cdc_cx2072x = { "cx2072x", { "14F10720", NULL }, CK_SPKCODEC, 16, 8, 0,
                                    cx_probe, cx_init, cx_stream, cx_volume, NULL, cx_jack, cx_off };
