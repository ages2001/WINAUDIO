/* sdwmisc.c - other SoundWire 1.1 peripherals:
   MAX98373  speaker amplifier (8-bit registers at 16-bit addresses); every amplifier gets the
             stereo stream on DP1 and its mono mixer picks the side: with several amplifiers
             an even s->chan plays the left channel, an odd one the right channel. The I/V sense
             capture port (DP3) is not used.
   RT5682    headset codec (16-bit registers behind an indirect window of 8-bit SoundWire
             registers): DP1 headphones, DP2 headset microphone, system clock from PLL2 locked
             to the SoundWire clock, jack polled (JD1 and the combo jack type detection).
   Sequences after the Linux max98373(-sdw), rt5682(-sdw) drivers and the sdw_utils machine
   helpers (soc_sdw_maxim, soc_sdw_rt5682). */
#include "sdw.h"

/* bus clock the core runs (these chips have no SCP_BusClock registers): 4.8 MHz from a 38.4
   or 19.2 MHz crystal, 6 MHz from 24 MHz */
static u32 bus_hz(struct sdwdev *s)
{
    if (s->busclk) return s->busclk;
    return (s->d && dsp_xtal(s->d) == 24000000UL) ? 6000000UL : 4800000UL;
}

/* ================================================================ MAX98373 */

#define M73_RESET       0x2000
#define M73_TX_HIZ1     0x2020
#define M73_TX_HIZ2     0x2021
#define M73_TX_SRC1     0x2022
#define M73_TX_SRC2     0x2023
#define M73_FMT         0x2024
#define M73_IF_MODE     0x2025
#define M73_SR2         0x2028
#define M73_MIX1        0x2029
#define M73_MIX2        0x202A
#define M73_RX_EN       0x202B
#define M73_SDW_CTRL    0x2036
#define M73_DIG_VOL     0x203D
#define M73_DSP_CFG     0x203F
#define M73_AMP_EN      0x2043
#define M73_IV_DSP      0x2046
#define M73_IV_EN       0x2047
#define M73_BDE_EN      0x20B5
#define M73_LIMITER_EN  0x20E2
#define M73_GLOBAL      0x20FF
#define M73_REV_ID      0x21FF

/* priv[] */
#define P73_ON          0
#define P73_VOL         1           /* volume register value + 1 (0: not set) */

static const struct sdwport m73_ports[] = { { SF_SPK, 1, 2, SPF_FULL } };

/* max98373_io_init after the software reset: SoundWire mode, IV ADC, sample rates 48 kHz,
   IV slots (V 0, I 1, speaker feedback 2), DC blockers, speaker on, brownout and limiter */
static const u32 m73_init_seq[] = {
    M73_IF_MODE, 0x03, M73_IV_EN, 0x03, M73_SR2, 0x88, M73_TX_HIZ1, 0xFF, M73_TX_HIZ2, 0xFF,
    M73_DSP_CFG, 0x03, M73_IV_DSP, 0x07, M73_TX_SRC1, 0x10, M73_TX_HIZ1, 0xFC,
    M73_TX_SRC2, 0x02
};

/* SWCLK setting (max98373_clock_calculate): a clock of the family 7.68 .. 13 MHz divided by
   1, 2, 4 or 8 */
static u8 m73_clock(struct sdwdev *s)
{
    static const u32 fam[7] = { 7680000UL, 8400000UL, 9600000UL, 11289600UL, 12000000UL,
                                12288000UL, 13000000UL };
    u32 hz = bus_hz(s);
    int x, y;
    for (x = 0; x < 4; x++)
        for (y = 0; y < 7; y++)
            if (hz == (fam[y] >> x)) return (u8)((x << 3) + y);
    return 0x05;
}

static int m73_init(struct sdwdev *s)
{
    int i, rev = -1;
    s->priv[P73_ON] = 0;
    sdw_upd(s, M73_RESET, 0x01, 0x01);
    for (i = 0; i < 3 && rev < 0; i++) {
        mdelay(10);
        rev = sdw_rd(s, M73_REV_ID);
    }
    if (rev < 0) {
        kprintf("max98373: no answer after the reset\n");
        return -1;
    }
    DBG("max98373: revision %02x\n", rev);
    sdw_seq(s, m73_init_seq, ARRAY_SIZE(m73_init_seq) / 2);
    sdw_wr(s, M73_SDW_CTRL, m73_clock(s));
    /* mono mixer: slot 0 and 1, this amplifier's side (DAI Sel Left / Right, one amplifier:
       the Linux default LeftRight) */
    sdw_wr(s, M73_MIX1, (u8)((s->namps > 1 ? (s->chan & 1) : 2) << 6));
    sdw_wr(s, M73_MIX2, 0x01);
    sdw_upd(s, M73_AMP_EN, 0x01, 0x01);
    sdw_wr(s, M73_BDE_EN, 0x01);
    sdw_wr(s, M73_LIMITER_EN, 0x01);
    if (s->priv[P73_VOL]) sdw_wr(s, M73_DIG_VOL, (u8)(s->priv[P73_VOL] - 1));
    return 0;
}

/* hw_params (24-bit words, 48 kHz) and the "Amp Enable" DAPM widget */
static void m73_stream(struct sdwdev *s, int func, int on)
{
    if (func != SF_SPK || s->priv[P73_ON] == (u32)on) return;
    s->priv[P73_ON] = (u32)on;
    if (on) {
        sdw_upd(s, M73_FMT, 0xC0, 0x80);
        sdw_upd(s, M73_SR2, 0xFF, 0x88);
        sdw_wr(s, M73_RX_EN, 0x01);
        sdw_upd(s, M73_GLOBAL, 0x01, 0x01);
        mdelay(30);
    } else {
        sdw_upd(s, M73_GLOBAL, 0x01, 0);
        mdelay(30);
        sdw_wr(s, M73_RX_EN, 0x00);
    }
}

/* digital volume: 0.5 dB steps 0 .. -63.5 dB, this amplifier's side of the stereo level */
static void m73_volume(struct sdwdev *s, int ctl, u32 lr)
{
    u32 v;
    if (ctl != MX_MASTER) return;
    v = 127 - cdc_vstep((s->namps > 1 && (s->chan & 1)) ? CDC_R(lr) : CDC_L(lr), 127, 500);
    s->priv[P73_VOL] = v + 1;
    sdw_wr(s, M73_DIG_VOL, (u8)v);
}

static void m73_off(struct sdwdev *s)
{
    if (s->priv[P73_ON]) m73_stream(s, SF_SPK, 0);
    sdw_upd(s, M73_AMP_EN, 0x01, 0);
}

const struct sdwdrv sdw_max98373 = {
    "max98373", 0x019F, 0x8373, SF_SPK, 1, m73_ports, m73_init, m73_stream, m73_volume, NULL, m73_off
};

/* ================================================================ RT5682 */

/* indirect access window */
#define R82_ADDR_L      0x3000
#define R82_ADDR_H      0x3001
#define R82_DATA_L      0x3004
#define R82_DATA_H      0x3005
#define R82_CMD         0x3008

#define RESET           0x0000
#define HP_CTRL_1       0x0002
#define HP_CTRL_2       0x0003
#define I2C_CTRL        0x0008
#define CBJ_BST_CTRL    0x000B
#define CBJ_CTRL_1      0x0010
#define CBJ_CTRL_2      0x0011
#define CBJ_CTRL_3      0x0012
#define CBJ_CTRL_5      0x0014
#define CBJ_CTRL_7      0x0016
#define DAC1_DIG_VOL    0x0019
#define ADC_DIG_VOL     0x001C
#define STO1_ADC_MIXER  0x0026
#define AD_DA_MIXER     0x0029
#define A_DAC1_MUX      0x002B
#define REC_MIXER       0x003C
#define CAL_REC         0x0044
#define PWR_DIG_1       0x0061
#define PWR_DIG_2       0x0062
#define PWR_ANLG_1      0x0063
#define PWR_ANLG_2      0x0064
#define PWR_ANLG_3      0x0065
#define CLK_DET         0x006B
#define ADDA_CLK_1      0x0073
#define GLB_CLK         0x0080
#define PLL_TRACK_2     0x0084
#define PLL_TRACK_3     0x0085
#define SDW_REF_CLK     0x008D
#define DEPOP_1         0x008E
#define HP_CP_1         0x0091
#define MICBIAS_2       0x0094
#define PLL2_CTRL_1     0x009B
#define PLL2_CTRL_2     0x009C
#define PLL2_CTRL_3     0x009D
#define PLL2_CTRL_4     0x009E
#define RC_CLK_CTRL     0x009F
#define IRQ_CTRL_2      0x00B7
#define GPIO_CTRL_1     0x00C0
#define IL_CMD_1        0x00DB
#define BTN_IL_CMD_2    0x00E3
#define AJD1_CTRL       0x00F0
#define DEVICE_ID       0x00FF
#define DAC_ADC_DIG_VOL1 0x0100
#define BIAS_CUR_8      0x0111
#define CHARGE_PUMP_1   0x0125
#define CHOP_DAC        0x013A
#define CHOP_ADC        0x013B
#define CALIB_ADC       0x013C
#define TEST_MODE_1     0x0145
#define PLL2_INTERNAL   0x0156
#define HP_IMP_SENS_19  0x01C1
#define HP_LOGIC_CTRL_2 0x01DB
#define HP_CALIB_CTRL_1 0x01DE
#define HP_CALIB_CTRL_2 0x01DF
#define HP_CALIB_CTRL_3 0x01E0
#define HP_CALIB_ST_1   0x01EA
#define SAR_IL_CMD_1    0x0210
#define SAR_IL_CMD_2    0x0211
#define SAR_IL_CMD_3    0x0212
#define SAR_IL_CMD_6    0x0215
#define SAR_IL_CMD_13   0x021C

/* PWR_ANLG_1 */
#define VREF1           0x8000
#define FV1             0x4000
#define VREF2           0x2000
#define FV2             0x1000
#define MB              0x0200
#define BG              0x0080

#define R82_ID          0x6530

/* priv[] */
#define P82_JACK        0           /* 0, 1 headphones, 3 headset */
#define P82_USE         1           /* SF_* running */
#define P82_DVOL        2           /* DAC1_DIG_VOL + 1 (0: not set) */
#define P82_AVOL        3           /* ADC_DIG_VOL + 1 */

static const struct sdwport r82_ports[] = {
    { SF_JACK_OUT, 1, 2, SPF_FULL },
    { SF_JACK_IN, 2, 2, SPF_FULL }
};

static int r82_wr(struct sdwdev *s, u32 reg, u32 val)
{
    if (sdw_wr(s, R82_CMD, 1) || sdw_wr(s, R82_ADDR_H, (u8)(reg >> 8)) ||
        sdw_wr(s, R82_ADDR_L, (u8)reg) || sdw_wr(s, R82_DATA_H, (u8)(val >> 8))) return -1;
    return sdw_wr(s, R82_DATA_L, (u8)val);
}

static int r82_rd(struct sdwdev *s, u32 reg)
{
    int h, l;
    if (sdw_wr(s, R82_CMD, 0) || sdw_wr(s, R82_ADDR_H, (u8)(reg >> 8)) ||
        sdw_wr(s, R82_ADDR_L, (u8)reg)) return -1;
    h = sdw_rd(s, R82_DATA_H);
    l = sdw_rd(s, R82_DATA_L);
    if (h < 0 || l < 0) return -1;
    return (h << 8) | l;
}

static int r82_upd(struct sdwdev *s, u32 reg, u32 mask, u32 val)
{
    int v = r82_rd(s, reg);
    if (v < 0) return -1;
    return r82_wr(s, reg, ((u32)v & ~mask) | (val & mask));
}

static void r82_seq(struct sdwdev *s, const u16 *p, int n)
{
    int i;
    for (i = 0; i < n; i++) r82_wr(s, p[2 * i], p[2 * i + 1]);
}

/* ---- jack */

static void r82_buttons(struct sdwdev *s, int on)
{
    if (on) {
        r82_upd(s, SAR_IL_CMD_1, 0x8000, 0x8000);
        r82_upd(s, SAR_IL_CMD_13, 0x003F, 0x003F);
        r82_wr(s, IL_CMD_1, 0x0040);
        r82_upd(s, BTN_IL_CMD_2, 0xC000, 0xC000);
    } else {
        r82_upd(s, SAR_IL_CMD_1, 0x8000, 0);
        r82_upd(s, BTN_IL_CMD_2, 0x8000, 0);
        r82_upd(s, BTN_IL_CMD_2, 0x4000, 0);
        r82_upd(s, SAR_IL_CMD_13, 0x003F, 0);
    }
}

/* rt5682_headset_detect: 3 headset, 1 headphones, 0 removed */
static int r82_detect(struct sdwdev *s, int in)
{
    int n, v = 0;
    if (!in) {
        r82_buttons(s, 0);
        r82_upd(s, CBJ_CTRL_1, 0x0008, 0);
        if (!s->priv[P82_USE]) r82_upd(s, PWR_ANLG_1, VREF2 | MB, 0);
        r82_upd(s, PWR_ANLG_3, 0x0200, 0);
        r82_upd(s, MICBIAS_2, 0x0300, 0);
        r82_upd(s, CBJ_CTRL_1, 0x0080, 0);
        return 0;
    }
    r82_upd(s, PWR_ANLG_1, VREF2 | MB, VREF2 | MB);
    r82_upd(s, PWR_ANLG_1, FV2, 0);
    mdelay(15);
    r82_upd(s, PWR_ANLG_1, FV2, FV2);
    r82_upd(s, PWR_ANLG_3, 0x0200, 0x0200);         /* CBJ */
    r82_upd(s, HP_CP_1, 0x0C00, 0);                 /* output switches off */
    r82_buttons(s, 0);
    r82_upd(s, CBJ_CTRL_1, 0x0008, 0);              /* trigger the detection */
    mdelay(55);
    r82_upd(s, CBJ_CTRL_1, 0x0008, 0x0008);
    for (n = 0; n <= 50; n++) {
        v = r82_rd(s, CBJ_CTRL_2);
        v = v < 0 ? 0 : v & 3;
        if (v || n == 50) break;
        mdelay(10);
    }
    if (v == 1 || v == 2) {
        r82_upd(s, CBJ_CTRL_1, 0x0080, 0x0080);     /* fast off */
        r82_buttons(s, 1);
        v = 3;
    } else {
        v = 1;
    }
    r82_upd(s, HP_CP_1, 0x0C00, 0x0C00);
    r82_upd(s, MICBIAS_2, 0x0300, 0x0300);          /* 25 MHz and 1 MHz clocks */
    return v;
}

/* JD1 status (rt5682_jack_detect_handler, polled instead of the implementation defined
   SoundWire interrupt, which is acknowledged here as the bus code would) */
static int r82_jack(struct sdwdev *s)
{
    int v = sdw_rd(s, SDW_SCP_INT1);
    if (v > 0 && (v & 0x04)) sdw_wr(s, SDW_SCP_INT1, 0x04);
    v = r82_rd(s, AJD1_CTRL);
    if (v < 0) return -1;
    if (!(v & 0x0010) && !s->priv[P82_JACK]) {
        mdelay(50);
        v = r82_rd(s, AJD1_CTRL);
        if (v < 0 || (v & 0x0010)) return 0;
        s->priv[P82_JACK] = (u32)r82_detect(s, 1);
    } else if ((v & 0x0010) && s->priv[P82_JACK]) {
        r82_detect(s, 0);
        s->priv[P82_JACK] = 0;
    }
    return (int)s->priv[P82_JACK];
}

/* ---- init */

/* rt5682_calibrate (SoundWire: no I2C mode register, ADC calibration in CALIB_ADC) */
static void r82_calibrate(struct sdwdev *s)
{
    static const u16 cal[] = {
        PWR_ANLG_1, 0xF2AF, MICBIAS_2, 0x0300, GLB_CLK, 0x8000, PWR_DIG_1, 0x0100,
        HP_IMP_SENS_19, 0x3800, CHOP_DAC, 0x3000, CALIB_ADC, 0x7005, STO1_ADC_MIXER, 0x686C,
        CAL_REC, 0x0D0D, HP_CALIB_CTRL_2, 0x0321, HP_LOGIC_CTRL_2, 0x0004,
        HP_CALIB_CTRL_1, 0x7C00, HP_CALIB_CTRL_3, 0x06A1, A_DAC1_MUX, 0x0311,
        HP_CALIB_CTRL_1, 0x7C00, HP_CALIB_CTRL_1, 0xFC00
    };
    static const u16 restore[] = {
        PWR_ANLG_1, 0x002F, MICBIAS_2, 0x0080, GLB_CLK, 0x0000, PWR_DIG_1, 0x0000,
        CHOP_DAC, 0x2000, CALIB_ADC, 0x2005, STO1_ADC_MIXER, 0xC0C4, CAL_REC, 0x0C0C
    };
    int i, v;
    r82_wr(s, RESET, 0);
    r82_wr(s, I2C_CTRL, 0x000F);
    r82_wr(s, PWR_ANLG_1, 0xA2AF);
    mdelay(15);
    r82_seq(s, cal, (int)ARRAY_SIZE(cal) / 2);
    for (i = 0; i < 60; i++) {
        v = r82_rd(s, HP_CALIB_ST_1);
        if (v >= 0 && !(v & 0x8000)) break;
        mdelay(10);
    }
    if (i == 60) kprintf("rt5682: headphone calibration failed\n");
    r82_seq(s, restore, (int)ARRAY_SIZE(restore) / 2);
}

static int r82_init(struct sdwdev *s)
{
    /* patch list, then rt5682_io_init: LDO1 1.2 V, HP driver 5x, charge pump; PLL2 from the
       SoundWire clock as system clock; JD1 jack detection */
    static const u16 init1[] = {
        HP_IMP_SENS_19, 0x1000, DAC_ADC_DIG_VOL1, 0xA020, I2C_CTRL, 0x000F,
        PLL2_INTERNAL, 0x8266, SAR_IL_CMD_1, 0x22B7, SAR_IL_CMD_3, 0x0365,
        SAR_IL_CMD_6, 0x0110, CHARGE_PUMP_1, 0x0210, HP_LOGIC_CTRL_2, 0x0007,
        SAR_IL_CMD_2, 0xAC00, CBJ_CTRL_7, 0x0104, DEPOP_1, 0x0000
    };
    static const u16 init2[] = {
        MICBIAS_2, 0x0080, TEST_MODE_1, 0x0000,
        PLL2_INTERNAL, 0xA266, PLL2_CTRL_1, 0x1700, PLL2_CTRL_2, 0x0006, PLL2_CTRL_3, 0x2600,
        PLL2_CTRL_4, 0x0C8F, PLL_TRACK_2, 0x3000, PLL_TRACK_3, 0x4000
    };
    int i, id = -1;
    u8 c;
    if (s->ver != 2) {
        kprintf("rt5682: SoundWire version %u, Linux takes only version 2\n", s->ver);
        return -1;
    }
    s->priv[P82_JACK] = 0;
    s->priv[P82_USE] = 0;
    for (i = 0; i < 10; i++) {
        id = r82_rd(s, DEVICE_ID);
        if (id == R82_ID) break;
        mdelay(30);
    }
    if (id != R82_ID) {
        kprintf("rt5682: device id %04x\n", id);
        return -1;
    }
    r82_calibrate(s);
    r82_seq(s, init1, (int)ARRAY_SIZE(init1) / 2);
    r82_upd(s, PWR_ANLG_1, 0x003C, 0x002C);
    r82_seq(s, init2, (int)ARRAY_SIZE(init2) / 2);
    r82_upd(s, BIAS_CUR_8, 0x000C, 0x0004);         /* HP charge pump bias 3 uA */
    r82_upd(s, CHARGE_PUMP_1, 0x0030, 0x0020);      /* charge pump clock 300 kHz */
    r82_upd(s, HP_CP_1, 0x0300, 0x0200);            /* HP power mode high voltage */
    r82_upd(s, GLB_CLK, 0xEC00, 0x4800);            /* system clock PLL2, PLL2 from SoundWire */
    /* JD1: manual external JD source, combo jack, SAR, JD power, interrupt pulse */
    r82_upd(s, CBJ_CTRL_2, 0x0070, 0x0040);
    r82_wr(s, CBJ_CTRL_1, 0xD142);
    r82_upd(s, CBJ_CTRL_5, 0x0700, 0x0600);
    r82_upd(s, CBJ_CTRL_3, 0x0080, 0x0080);
    r82_upd(s, SAR_IL_CMD_1, 0x1000, 0x1000);
    r82_upd(s, RC_CLK_CTRL, 0xD000, 0xD000);
    r82_upd(s, PWR_ANLG_2, 0x0008, 0x0008);
    r82_upd(s, IRQ_CTRL_2, 0x8400, 0x8400);
    /* bus clock (rt5682_clock_config) */
    switch (bus_hz(s)) {
    case 12000000UL: c = 0; break;
    case 6000000UL: c = 1; break;
    case 9600000UL: c = 2; break;
    case 2400000UL: c = 4; break;
    case 12288000UL: c = 5; break;
    default: c = 3; break;                          /* 4.8 MHz */
    }
    sdw_wr(s, 0xE0, c);
    sdw_wr(s, 0xF0, c);
    /* paths (DAPM / UCM on Linux): SoundWire data into both DAC channels (DAC L / R Mux
       SOUND, DAC1 unmuted, stereo ADC not mixed in), DAC1 to the analog DACs; CBJ +24 dB ->
       RECMIX1L -> ADC1 L to both stereo ADC channels -> ADCDAT1 (SoundWire capture) */
    r82_wr(s, AD_DA_MIXER, 0x8580);
    r82_upd(s, A_DAC1_MUX, 0x0011, 0x0011);
    r82_upd(s, CBJ_BST_CTRL, 0x0F00, 0x0200);
    r82_wr(s, STO1_ADC_MIXER, 0x6060);
    r82_upd(s, REC_MIXER, 0x0080, 0);
    r82_upd(s, GPIO_CTRL_1, 0x0300, 0x0100);
    /* 48 kHz on both SoundWire ports, DAC / ADC oversampling 1/8 (hw_params) */
    r82_upd(s, SDW_REF_CLK, 0x00FF, 0x0000);
    r82_upd(s, ADDA_CLK_1, 0xF00F, 0x4004);
    if (s->priv[P82_DVOL]) r82_wr(s, DAC1_DIG_VOL, s->priv[P82_DVOL] - 1);
    if (s->priv[P82_AVOL]) r82_wr(s, ADC_DIG_VOL, s->priv[P82_AVOL] - 1);
    mdelay(50);
    return 0;
}

/* ---- streams (DAPM supplies and widgets of the SoundWire paths) */

/* bias, LDO, digital gate and PLL2: on while a stream runs */
static void r82_clocks(struct sdwdev *s, int on)
{
    if (on) {
        r82_upd(s, PWR_ANLG_1, BG, BG);
        r82_upd(s, PWR_DIG_1, 0x0101, 0x0101);
        r82_upd(s, PWR_ANLG_3, 0x0030, 0x0030);     /* PLL2B, PLL2F */
        return;
    }
    r82_upd(s, PWR_ANLG_3, 0x0030, 0);
    r82_upd(s, PWR_DIG_1, 0x0101, 0);
    r82_upd(s, PWR_ANLG_1, BG, 0);
}

/* headphone path: Vref1, DAC filter, HP amp, clock detection, charge pump, capless, DAC
   clock, DACs, then rt5682_hp_event */
static void r82_play(struct sdwdev *s, int on)
{
    if (on) {
        r82_upd(s, PWR_ANLG_1, FV1, 0);
        r82_upd(s, PWR_ANLG_1, VREF1, VREF1);
        mdelay(15);
        r82_upd(s, PWR_ANLG_1, FV1, FV1);
        r82_upd(s, PWR_DIG_2, 0x0400, 0x0400);
        r82_upd(s, PWR_ANLG_1, 0x0003, 0x0003);
        r82_upd(s, CLK_DET, 0x8001, 0x8001);
        r82_upd(s, DEPOP_1, 0x0008, 0x0008);        /* charge pump */
        r82_upd(s, DEPOP_1, 0x0001, 0x0001);        /* capless */
        r82_upd(s, CHOP_DAC, 0x1000, 0x1000);
        r82_upd(s, PWR_DIG_1, 0x0C00, 0x0C00);
        r82_upd(s, HP_CTRL_2, 0x8000, 0);
        r82_upd(s, HP_LOGIC_CTRL_2, 0x0010, 0x0010);
        r82_upd(s, DEPOP_1, 0x0060, 0x0060);
        r82_upd(s, DAC_ADC_DIG_VOL1, 0x00C0, 0x0080);
        r82_upd(s, HP_CTRL_2, 0x6000, 0x6000);
        mdelay(5);
        r82_upd(s, CHARGE_PUMP_1, 0x0700, 0x0400);  /* switch size L */
        r82_upd(s, HP_CTRL_1, 0x8080, 0);
        return;
    }
    r82_upd(s, HP_CTRL_1, 0x8080, 0x8080);
    r82_upd(s, HP_CTRL_2, 0x6000, 0);
    r82_upd(s, CHARGE_PUMP_1, 0x0700, 0x0200);      /* switch size M */
    r82_upd(s, DEPOP_1, 0x0060, 0);
    r82_upd(s, DAC_ADC_DIG_VOL1, 0x00C0, 0);
    r82_upd(s, PWR_DIG_1, 0x0C00, 0);
    r82_upd(s, CHOP_DAC, 0x1000, 0);
    r82_upd(s, DEPOP_1, 0x0009, 0);
    r82_upd(s, CLK_DET, 0x8001, 0);
    r82_upd(s, PWR_ANLG_1, 0x0003, 0);
    r82_upd(s, PWR_DIG_2, 0x0400, 0);
    r82_upd(s, PWR_ANLG_1, VREF1, 0);
}

/* capture: ADC filter, ADC1 L, ADC clock, RECMIX1L */
static void r82_rec(struct sdwdev *s, int on)
{
    if (on) {
        r82_upd(s, PWR_DIG_2, 0x8000, 0x8000);
        r82_upd(s, PWR_DIG_1, 0x0010, 0x0010);
        r82_upd(s, CHOP_ADC, 0x1000, 0x1000);
        r82_upd(s, PWR_ANLG_2, 0x0002, 0x0002);
        return;
    }
    r82_upd(s, PWR_ANLG_2, 0x0002, 0);
    r82_upd(s, CHOP_ADC, 0x1000, 0);
    r82_upd(s, PWR_DIG_1, 0x0010, 0);
    r82_upd(s, PWR_DIG_2, 0x8000, 0);
}

static void r82_stream(struct sdwdev *s, int func, int on)
{
    u32 bit = (u32)func, was = s->priv[P82_USE];
    if (func != SF_JACK_OUT && func != SF_JACK_IN) return;
    if (on ? (was & bit) != 0 : !(was & bit)) return;
    if (on && !was) r82_clocks(s, 1);
    if (func == SF_JACK_IN) r82_rec(s, on);
    else r82_play(s, on);
    s->priv[P82_USE] = on ? was | bit : was & ~bit;
    if (!s->priv[P82_USE]) r82_clocks(s, 0);
}

static void r82_volume(struct sdwdev *s, int ctl, u32 lr)
{
    u32 l, r, v;
    if (ctl == MX_MASTER) {
        /* DAC1: 0.75 dB steps -65.25 .. 0 dB; level 0 mutes DAC1 into the DAC mixer */
        l = cdc_vstep(CDC_L(lr), 87, 750);
        r = cdc_vstep(CDC_R(lr), 87, 750);
        v = (l << 9) | (r << 1);
        s->priv[P82_DVOL] = v + 1;
        r82_wr(s, DAC1_DIG_VOL, v);
        r82_upd(s, AD_DA_MIXER, 0x4040, (l ? 0 : 0x4000) | (r ? 0 : 0x0040));
    } else if (ctl == MX_INGAIN) {
        /* stereo ADC: -17.25 .. +30 dB in 0.75 dB steps after the +24 dB CBJ boost */
        l = cdc_step(CDC_L(lr), 63);
        r = cdc_step(CDC_R(lr), 63);
        v = (l ? l << 9 : 0x8000) | (r ? r << 1 : 0x0080);
        s->priv[P82_AVOL] = v + 1;
        r82_wr(s, ADC_DIG_VOL, v);
    }
}

static void r82_off(struct sdwdev *s)
{
    if (s->priv[P82_USE] & SF_JACK_OUT) r82_stream(s, SF_JACK_OUT, 0);
    if (s->priv[P82_USE] & SF_JACK_IN) r82_stream(s, SF_JACK_IN, 0);
    if (s->priv[P82_JACK]) {
        r82_detect(s, 0);
        s->priv[P82_JACK] = 0;
    }
    r82_wr(s, RESET, 0);
}

const struct sdwdrv sdw_rt5682 = {
    "rt5682", 0x025D, 0x5682, SF_JACK_OUT | SF_JACK_IN, 2, r82_ports,
    r82_init, r82_stream, r82_volume, r82_jack, r82_off
};
