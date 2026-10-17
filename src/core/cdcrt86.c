/* cdcrt86.c - Realtek ALC286 / ALC288 (RT286 / RT288, ACPI INT343A or 10EC0286: Broadwell
   machines with the DSP in I2S mode, e.g. the Dell XPS 13 9343). An HD Audio codec behind an
   I2C bridge (Realtek RL6347A): every access is one 32-bit HD Audio verb sent big endian (a
   read returns 32 bits), the vendor registers sit behind the coefficient verbs of node 20h.
   Machine wiring after Linux bdw_rt286: SSP0 in I2S, the codec is clock consumer and runs
   its PLL from the 24 MHz MCLK of the DSP; speakers on pin 14h (class D, EAPD), headphones on
   pin 21h, internal digital microphone on DMIC1 (12h), headset microphone on MIC1 (18h).
   Jack: pin sense of 21h and 18h; the RT288 (Dell) has a combo jack whose microphone the
   CBJ detector of the codec tells, stepped through the polls. Register sequences after the
   Linux rt286 and rl6347a drivers. */
#include "dspcdc.h"

/* HD Audio nodes */
#define AFG             0x01
#define DAC_OUT1        0x02
#define DAC_OUT2        0x03
#define ADC_IN2         0x08
#define ADC_IN1         0x09
#define MIXER_OUT1      0x0C
#define DMIC1           0x12
#define DMIC2           0x13
#define SPK_OUT         0x14
#define MIC1            0x18
#define VENDOR          0x20
#define HP_OUT          0x21
#define MIXER_IN1       0x22
#define MIXER_IN2       0x23

/* register addresses as the Linux driver's regmap: node << 20 | verb << 8 | payload */
#define VERB(v, n, d)   (((u32)(n) << 20) | ((u32)(v) << 8) | (u32)(d))
#define SET_POWER(n)    VERB(0x705, n, 0)
#define CONNECT_SEL(n)  VERB(0x701, n, 0)
#define PIN_CTL(n)      VERB(0x707, n, 0)
#define EAPD(n)         VERB(0x70C, n, 0)
#define PIN_SENSE(n)    VERB(0xF09, n, 0)
#define AMP(n, d)       VERB(0x300, n, d)       /* 15 out, 14 in, 13 left, 12 right */
#define FORMAT(n)       VERB(0x200, n, 0)
#define VENDOR_ID       VERB(0xF00, 0, 0)
#define DMIC2_DEFAULT   VERB(0x71F, DMIC2, 0)
#define GPIO_MASK       VERB(0x716, AFG, 0)
#define GPIO_DIR        VERB(0x717, AFG, 0)
#define GPIO_DATA       VERB(0x715, AFG, 0)
#define COEF_INDEX      VERB(0x500, VENDOR, 0)
#define PROC_COEF       VERB(0x400, VENDOR, 0)

/* vendor (index) registers */
#define A_BIAS_CTRL2    0x02
#define POWER_CTRL1     0x03
#define A_BIAS_CTRL3    0x04
#define POWER_CTRL2     0x08
#define I2S_CTRL1       0x09
#define I2S_CTRL2       0x0A
#define DC_GAIN         0x0D
#define MIC1_DET_CTRL   0x19
#define MISC_CTRL1      0x20
#define GPIO_CTRL       0x29
#define PLL_CTRL1       0x49
#define CBJ_CTRL1       0x4F
#define CBJ_CTRL2       0x50
#define PLL_CTRL        0x63
#define DEPOP_CTRL2     0x67
#define DEPOP_CTRL3     0x68
#define DEPOP_CTRL4     0x69

/* c->priv */
#define P_288           0               /* RT288: combo jack (as Linux for the Dell machines) */
#define P_RUN           1               /* playback running */
#define P_CBJ           2               /* combo jack detection step */
#define P_JACK          3               /* jack bits applied to the outputs and the ADC */

static const u16 index_def[][2] = {
    { 0x01, 0xAAAA }, { 0x02, 0x8AAA }, { 0x03, 0x0002 }, { 0x04, 0xAF01 }, { 0x08, 0x000D },
    { 0x09, 0xD810 }, { 0x0A, 0x0120 }, { 0x0B, 0x0000 }, { 0x0D, 0x2800 }, { 0x0F, 0x0000 },
    { 0x19, 0x0A17 }, { 0x20, 0x0020 }, { 0x33, 0x0208 }, { 0x49, 0x0004 }, { 0x4F, 0x50E9 },
    { 0x50, 0x2000 }, { 0x63, 0x2902 }, { 0x67, 0x1111 }, { 0x68, 0x1016 }, { 0x69, 0x273F },
};

static const u32 reg_def[][2] = {
    { 0x00170500UL, 0x0400 }, { 0x00220000UL, 0x0031 }, { 0x00239000UL, 0x007F }, { 0x0023A000UL, 0x007F },
    { 0x00270500UL, 0x0400 }, { 0x00370500UL, 0x0400 }, { 0x00830000UL, 0x00C3 }, { 0x00870500UL, 0x0400 },
    { 0x00920000UL, 0x0031 }, { 0x00930000UL, 0x00C3 }, { 0x00935000UL, 0x00C3 }, { 0x00936000UL, 0x00C3 },
    { 0x00970500UL, 0x0400 }, { 0x00B37000UL, 0x0097 }, { 0x00B37200UL, 0x0097 }, { 0x00B37300UL, 0x0097 },
    { 0x00C37000UL, 0x0000 }, { 0x00C37100UL, 0x0080 }, { 0x01270500UL, 0x0400 }, { 0x01270700UL, 0x0000 },
    { 0x01370500UL, 0x0400 }, { 0x01371F00UL, 0x411111F0UL }, { 0x01439000UL, 0x0080 }, { 0x0143A000UL, 0x0080 },
    { 0x01470100UL, 0x0000 }, { 0x01470500UL, 0x0400 }, { 0x01470700UL, 0x0000 }, { 0x01470C00UL, 0x0000 },
    { 0x01837000UL, 0x0000 }, { 0x01870500UL, 0x0400 }, { 0x01870700UL, 0x0020 }, { 0x02050000UL, 0x0000 },
    { 0x02139000UL, 0x0080 }, { 0x0213A000UL, 0x0080 }, { 0x02170100UL, 0x0000 }, { 0x02170500UL, 0x0400 },
    { 0x02170700UL, 0x0000 }, { 0x02270100UL, 0x0000 }, { 0x02370100UL, 0x0000 },
};

static const u8 power_nodes[] = { DAC_OUT1, DAC_OUT2, ADC_IN1, ADC_IN2, MIC1, DMIC1, DMIC2, SPK_OUT, HP_OUT };

/* ---------------------------------------------------------------- access (rl6347a) */

static int wr(struct cdc *c, u32 reg, u32 val)
{
    u8 b[4];
    if (reg <= 0xFF) {                          /* vendor register: index, then value */
        if (wr(c, COEF_INDEX, reg)) return -1;
        reg = PROC_COEF;
    }
    b[0] = (u8)(reg >> 24);
    b[1] = (u8)(reg >> 16);
    b[2] = (u8)((reg >> 8) | (val >> 8));
    b[3] = (u8)val;
    return i2c_xfer(c->bus, c->addr, b, 4, NULL, 0);
}

static int rd(struct cdc *c, u32 reg, u32 *val)
{
    u8 b[4], r[4];
    if (reg <= 0xFF) {
        if (wr(c, COEF_INDEX, reg)) return -1;
        reg = PROC_COEF;
    }
    reg |= 0x80000UL;                           /* the get verb of the set verb */
    if ((reg & 0xF0000UL) == 0xB0000UL)         /* amplifier gain: index in the low bits */
        reg = (reg & ~0xF0FUL) | ((reg >> 8) & 0xF);
    b[0] = (u8)(reg >> 24);
    b[1] = (u8)(reg >> 16);
    b[2] = (u8)(reg >> 8);
    b[3] = (u8)reg;
    if (i2c_xfer(c->bus, c->addr, b, 4, r, 4)) return -1;
    *val = ((u32)r[0] << 24) | ((u32)r[1] << 16) | ((u32)r[2] << 8) | r[3];
    return 0;
}

/* vendor register update */
static void upd(struct cdc *c, u32 reg, u32 mask, u32 val)
{
    u32 v;
    if (rd(c, reg, &v)) return;
    wr(c, reg, ((v & ~mask) | (val & mask)) & 0xFFFF);
}

/* ---------------------------------------------------------------- driver */

static int rt_probe(struct cdc *c)
{
    u32 id;
    if (rd(c, VENDOR_ID, &id)) return -1;
    if (id != 0x10EC0286UL && id != 0x10EC0288UL) return -1;
    c->priv[P_288] = id == 0x10EC0288UL;
    return 0;
}

/* outputs and the capture source for the jack bits: speakers while playback runs without
   headphones, the headset microphone when one is plugged in, else the digital microphone */
static void route(struct cdc *c)
{
    u32 j = c->priv[P_JACK], spk = !(j & 1) && c->priv[P_RUN];
    wr(c, PIN_CTL(SPK_OUT), spk ? 0x40 : 0);
    wr(c, EAPD(SPK_OUT), spk ? 2 : 0);
    wr(c, PIN_CTL(HP_OUT), (j & 1) ? 0x40 : 0);
    if (j & 2) {
        wr(c, CONNECT_SEL(MIXER_IN1), 0);               /* ADC 0 from MIC1 */
        wr(c, PIN_CTL(DMIC1), 0);
    } else {
        wr(c, CONNECT_SEL(MIXER_IN1), 5);               /* ADC 0 from DMIC1 */
        wr(c, PIN_CTL(DMIC1), 0x20);
    }
}

static int rt_init(struct cdc *c)
{
    u32 i, fmt = 0x0001, dlen = 2, mclk = c->ssp.mclk ? c->ssp.mclk : 24000000UL;
    int cbj = (int)c->priv[P_288];
    /* defaults of the vendor and verb registers (as the Linux probe restores them) */
    for (i = 0; i < ARRAY_SIZE(index_def); i++) wr(c, index_def[i][0], index_def[i][1]);
    for (i = 0; i < ARRAY_SIZE(reg_def); i++) wr(c, reg_def[i][0], reg_def[i][1]);
    wr(c, SET_POWER(AFG), 3);
    for (i = 0; i < sizeof(power_nodes); i++) wr(c, SET_POWER(power_nodes[i]), 1);
    if (!cbj) {
        wr(c, CBJ_CTRL2, 0x0000);
        wr(c, MIC1_DET_CTRL, 0x0816);
        upd(c, CBJ_CTRL1, 0xF000, 0xB000);
    } else {
        upd(c, CBJ_CTRL1, 0xF000, 0x5000);
    }
    mdelay(10);
    wr(c, DMIC2_DEFAULT, 0x40);
    mdelay(10);
    wr(c, MISC_CTRL1, 0x0000);
    upd(c, DEPOP_CTRL2, 0x403A, 0x401A);
    upd(c, DEPOP_CTRL3, 0xF777, 0x4737);
    upd(c, DEPOP_CTRL4, 0x00FF, 0x003F);
    if (cbj) {
        /* RT288 of the Dell machines: GPIO6 high (as Linux does for them) */
        wr(c, GPIO_MASK, 0x40);
        wr(c, GPIO_DIR, 0x40);
        wr(c, GPIO_DATA, 0x40);
        upd(c, GPIO_CTRL, 0x0C, 0x08);
    }
    /* interface: I2S, clock consumer, PCM */
    upd(c, I2S_CTRL1, 0x0B00, (c->ssp.provider ? 0x800 : 0) |
        (c->ssp.fmt == 3 ? 0x100 : c->ssp.fmt == 4 ? 0x200 : c->ssp.fmt == 5 ? 0x300 : 0));
    /* system clock from the PLL on MCLK (19.2 or 24 MHz) */
    if (mclk != 19200000UL && mclk != 24000000UL)
        kprintf("rt286: MCLK %lu Hz not supported by the PLL, assuming 24 MHz\n", mclk);
    upd(c, I2S_CTRL2, 0x0100, 0x0100);
    upd(c, PLL_CTRL, 0x0004, 0x0004);
    upd(c, PLL_CTRL1, 0x0020, 0x0000);
    upd(c, I2S_CTRL2, 0x0040, mclk == 19200000UL ? 0x40 : 0);
    /* 48 kHz stereo, sample size of the slots */
    switch (c->ssp.vbits) {
    case 16: fmt |= 0x10; dlen = 0; break;
    case 20: fmt |= 0x20; dlen = 1; break;
    case 32: fmt |= 0x40; break;
    default: fmt |= 0x30; break;                    /* 24 */
    }
    upd(c, I2S_CTRL1, 0x0018, dlen << 3);
    wr(c, FORMAT(DAC_OUT1), fmt);
    wr(c, FORMAT(ADC_IN1), fmt);
    /* power: function group D0 (DC gain held during the switch), HV, VREF, LDO1, LDO2 */
    wr(c, SET_POWER(AFG), 0);
    upd(c, DC_GAIN, 0x200, 0x200);
    upd(c, POWER_CTRL1, 0x3001, 0);
    upd(c, POWER_CTRL2, 0x0004, 0x0004);
    upd(c, POWER_CTRL2, 0x0038, 0x0008);
    mdelay(10);
    upd(c, DC_GAIN, 0x200, 0);
    /* playback: DAC 0 -> front mixer -> speaker and headphone pins */
    wr(c, SET_POWER(DAC_OUT1), 0);
    wr(c, AMP(MIXER_OUT1, 0x7000), 0x00);           /* DAC input unmuted */
    wr(c, AMP(MIXER_OUT1, 0x7100), 0x80);           /* recording mixer input muted */
    wr(c, CONNECT_SEL(SPK_OUT), 0);
    wr(c, CONNECT_SEL(HP_OUT), 0);
    wr(c, AMP(SPK_OUT, 0xA000), 0x00);
    wr(c, AMP(SPK_OUT, 0x9000), 0x00);
    wr(c, AMP(HP_OUT, 0xA000), 0x00);
    wr(c, AMP(HP_OUT, 0x9000), 0x00);
    wr(c, AMP(DAC_OUT1, 0xA000), 0x7F);
    wr(c, AMP(DAC_OUT1, 0x9000), 0x7F);
    /* capture: digital microphone or MIC1 (input buffer, +20 dB boost) -> ADC 0 */
    upd(c, A_BIAS_CTRL3, 0xC000, 0x8000);
    upd(c, A_BIAS_CTRL2, 0xC000, 0x8000);
    wr(c, AMP(MIC1, 0x7000), 2);
    wr(c, SET_POWER(DMIC1), 0);
    wr(c, SET_POWER(ADC_IN1), 0);
    wr(c, AMP(ADC_IN1, 0x6000), 0x57);
    wr(c, AMP(ADC_IN1, 0x5000), 0x57);
    c->priv[P_RUN] = 0;
    c->priv[P_CBJ] = 0;
    c->priv[P_JACK] = 0;
    route(c);
    return 0;
}

static void rt_stream(struct cdc *c, int dir, int on)
{
    if (dir) return;
    c->priv[P_RUN] = (u32)(on != 0);
    route(c);
}

static void rt_volume(struct cdc *c, int ctl, u32 lr)
{
    u32 l, r;
    if (ctl == MX_MASTER) {
        /* DAC 0: 0.5 dB steps, 7Fh = 0 dB; 0 mutes */
        l = cdc_step(CDC_L(lr), 0x7F);
        r = cdc_step(CDC_R(lr), 0x7F);
        wr(c, AMP(DAC_OUT1, 0xA000), l ? l : 0x80);
        wr(c, AMP(DAC_OUT1, 0x9000), r ? r : 0x80);
    } else if (ctl == MX_INGAIN) {
        l = cdc_step(CDC_L(lr), 0x7F);
        wr(c, AMP(ADC_IN1, 0x6000), l ? l : 0x80);
        wr(c, AMP(ADC_IN1, 0x5000), l ? l : 0x80);
    }
}

/* combo jack (RT288): the microphone ring tested with two detector settings, 300 ms apart */
static int cbj_step(struct cdc *c, int hp)
{
    u32 v;
    if (!hp) {
        if (c->priv[P_CBJ]) {
            wr(c, PIN_CTL(MIC1), 0x20);
            upd(c, CBJ_CTRL1, 0x0400, 0);
        }
        c->priv[P_CBJ] = 0;
        return 0;
    }
    switch (c->priv[P_CBJ]) {
    case 0:
        upd(c, DC_GAIN, 0x200, 0x200);
        wr(c, PIN_CTL(MIC1), 0x24);
        mdelay(50);
        upd(c, CBJ_CTRL1, 0xFCC0, 0xD400);
        c->priv[P_CBJ] = 1;
        return 1;
    case 1:
        if (rd(c, CBJ_CTRL2, &v)) return -1;
        if ((v & 0x70) == 0x70) {
            c->priv[P_CBJ] = 4;
            break;
        }
        upd(c, CBJ_CTRL1, 0xFCC0, 0xE400);
        c->priv[P_CBJ] = 2;
        return 1;
    case 2:
        if (rd(c, CBJ_CTRL2, &v)) return -1;
        if ((v & 0x70) == 0x70) {
            c->priv[P_CBJ] = 4;
        } else {
            upd(c, CBJ_CTRL1, 0xFCC0, 0xC400);
            c->priv[P_CBJ] = 3;
        }
        break;
    default:
        return c->priv[P_CBJ] == 4 ? 3 : 1;
    }
    upd(c, DC_GAIN, 0x200, 0);
    return c->priv[P_CBJ] == 4 ? 3 : 1;
}

static int rt_jack(struct cdc *c)
{
    u32 s;
    int j;
    if (rd(c, PIN_SENSE(HP_OUT), &s)) return -1;
    if (c->priv[P_288]) {
        j = cbj_step(c, (s & 0x80000000UL) != 0);
        if (j < 0) return -1;
    } else {
        j = (s & 0x80000000UL) ? 1 : 0;
        if (rd(c, PIN_SENSE(MIC1), &s)) return -1;
        if (s & 0x80000000UL) j |= 2;
    }
    if ((u32)j != c->priv[P_JACK]) {
        c->priv[P_JACK] = (u32)j;
        route(c);
    }
    return j;
}

static void rt_off(struct cdc *c)
{
    wr(c, EAPD(SPK_OUT), 0);
    wr(c, PIN_CTL(SPK_OUT), 0);
    wr(c, PIN_CTL(HP_OUT), 0);
    wr(c, SET_POWER(AFG), 3);
}

const struct cdcdrv cdc_rt286 = { "rt286", { "INT343A", "10EC0286", "10EC0288", NULL }, CK_SPKCODEC, 8, 16, 0,
                                  rt_probe, rt_init, rt_stream, rt_volume, NULL, rt_jack, rt_off };
