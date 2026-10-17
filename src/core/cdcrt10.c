/* cdcrt10.c - Realtek I2S speaker amplifiers: ALC1011 (RT1011), ALC1015 (RT1015), ALC1019
   (RT1019), ALC1308 (RT1308, I2C mode) programmed over I2C, and the RT1015P / RT1019P auto-mode
   amplifiers that only have a shutdown pin. The I2C chips run their system clock from a PLL
   locked to the SSP clock (BCLK as the Linux machine drivers do; MCLK for the RT1308 when the
   SSP drives one). Channels: with several amplifiers, even c->chan plays the left channel, odd
   the right one (the stereo pair is taken from slots 0/1, also in TDM as the Linux
   sof_realtek_common.c masks do); a single amplifier plays (L+R)/2. Register sequences after
   the Linux rt1011, rt1015, rt1015p, rt1019 and rt1308 drivers. */
#include "dspcdc.h"

/* priv[0]: amplifier on, priv[1]: RT1015P power-up calibration done */

/* ---------------------------------------------------------------- common */

/* the RL6231 PLL: fout = fin * (N + 2) / ((M + 2) * (K + 2)) */
struct rlpll { u32 n, m, k; u8 mbp, kbp; };

static u32 gcd32(u32 a, u32 b)
{
    while (b) {
        u32 t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static u32 best_div(u32 in, u32 max, u32 div)
{
    u32 d;
    if (in <= max) return 1;
    d = in / max + (in % max ? 1 : 0);
    while (div % d) d++;
    return d;
}

static u32 absdiff(u32 a, u32 b)
{
    return a > b ? a - b : b - a;
}

/* the same search as rl6231_pll_calc() so that the chips get the codes Linux gives them */
static int rl_pll(u32 fin, u32 fout, struct rlpll *p)
{
    static const u32 pre[5][6] = {          /* in, out, k, n, m, m bypass */
        { 19200000UL, 4096000UL, 23, 14, 1, 0 },
        { 19200000UL, 24576000UL, 3, 30, 3, 0 },
        { 48000000UL, 3840000UL, 23, 2, 0, 0 },
        { 3840000UL, 24576000UL, 3, 30, 0, 1 },
        { 3840000UL, 22579200UL, 3, 5, 0, 1 }
    };
    s32 kt, mink, maxk, k = 0;
    u32 nt, mt, n = 0, m = 0, red, redt, fi, fo, in_t, out_t;
    int i, mbp = 0;
    if (fin > 50000000UL || fin < 256000UL || !fout) return -1;
    for (i = 0; i < 5; i++) {
        if (fin == pre[i][0] && fout == pre[i][1]) {
            k = (s32)pre[i][2];
            n = pre[i][3];
            m = pre[i][4];
            mbp = (int)pre[i][5];
            goto found;
        }
    }
    redt = absdiff(fout, fin);
    mink = (s32)(80000000UL / fout) - 2;
    maxk = (s32)(150000000UL / fout) - 2;
    if (maxk > 31) maxk = 31;
    if (mink > 31) mink = maxk = 31;
    i = (int)best_div(fin, 0xFFFFFFFFUL / 511, gcd32(fin, fout));
    fi = fin / (u32)i;
    fo = fout / (u32)i;
    k = mink;
    if (mink < -1) mink = -1;
    for (kt = mink; kt <= maxk; kt++) {
        for (nt = 0; nt <= 511; nt++) {
            in_t = fi * (nt + 2);
            if (in_t == fo * (u32)(kt + 2)) {
                mbp = 1;
                n = nt;
                k = kt;
                goto found;
            }
            red = absdiff(fo, in_t / (u32)(kt + 2));
            if (red < redt) {
                mbp = 1;
                n = nt;
                m = 0;
                k = kt;
                if (!red) goto found;
                redt = red;
            }
            for (mt = 0; mt <= 15; mt++) {
                out_t = in_t / ((mt + 2) * (u32)(kt + 2));
                red = absdiff(fo, out_t);
                if (red < redt) {
                    mbp = 0;
                    n = nt;
                    m = mt;
                    k = kt;
                    if (!red) goto found;
                    redt = red;
                }
            }
        }
    }
found:
    p->kbp = 0;
    if (k == -1) {
        p->kbp = 1;
        k = 0;
    }
    p->mbp = (u8)mbp;
    p->m = mbp ? 0 : m;
    p->n = n;
    p->k = (u32)k;
    return 0;
}

/* BCLK of the port (from the slots when the SSP setup did not give it) */
static u32 rt_bclk(struct cdc *c)
{
    u32 s = c->ssp.slots ? c->ssp.slots : 2, w = c->ssp.width ? c->ssp.width : 32;
    return c->ssp.bclk ? c->ssp.bclk : s * w * CDC_RATE;
}

static int rt_pll(struct cdc *c, u32 fin, u32 fout, struct rlpll *p)
{
    if (rl_pll(fin, fout, p)) {
        kprintf("%s: no PLL setting for %lu -> %lu Hz\n", c->drv->name, fin, fout);
        return -1;
    }
    DBG("%s: PLL %lu -> %lu Hz: n %lu m %lu%s k %lu\n", c->drv->name, fin, fout, p->n, p->m,
        p->mbp ? " (bypass)" : "", p->k);
    return 0;
}

/* DSP A / DSP B: TDM frame */
static int rt_dsp(struct cdc *c)
{
    return c->ssp.fmt == 4 || c->ssp.fmt == 5;
}

/* serial format code of all four chips: 0 I2S, 1 left justified, 2 DSP A, 3 DSP B */
static u32 rt_df(struct cdc *c)
{
    switch (c->ssp.fmt) {
    case 3: return 1;
    case 4: return 2;
    case 5: return 3;
    default: return 0;
    }
}

/* data / slot length code: 0 16 bit, 1 20, 2 24, 3 32 */
static u32 rt_len(u32 bits)
{
    if (!bits) bits = 24;
    return bits >= 32 ? 3 : bits >= 24 ? 2 : bits >= 20 ? 1 : 0;
}

/* TDM slot count code: 0 2 slots, 1 4, 2 6, 3 8 */
static u32 rt_nslot(struct cdc *c)
{
    u32 s = c->ssp.slots;
    return s >= 8 ? 3 : s >= 6 ? 2 : s >= 4 ? 1 : 0;
}

/* mono source: 0 left, 1 right, 2 (L+R)/2 */
static u32 rt_mono(struct cdc *c)
{
    return c->namps <= 1 ? 2 : (u32)(c->chan & 1);
}

static int rt_was_on(struct cdc *c, int on)
{
    if (c->priv[0] == (u32)on) return 1;
    c->priv[0] = (u32)on;
    return 0;
}

/* ---------------------------------------------------------------- RT1011 */

#define R11_RESET           0x0000
#define R11_CLK_2           0x0004
#define R11_PLL_1           0x000A
#define R11_PLL_2           0x000C
#define R11_CLK_DET         0x0020
#define R11_SIL_DET         0x0022
#define R11_DEVICE_ID       0x007D
#define R11_DAC_SET_1       0x0102
#define R11_DAC_SET_2       0x0104
#define R11_DAC_SET_3       0x0106
#define R11_ADC_SET_1       0x0108
#define R11_ADC_SET_4       0x010E
#define R11_ADC_SET_5       0x0110
#define R11_TDM_TOTAL       0x0111
#define R11_TDM1_SET_1      0x0114
#define R11_TDM1_SET_2      0x0116
#define R11_TDM1_SET_4      0x011A
#define R11_TDM2_SET_1      0x011E
#define R11_TDM2_SET_2      0x0120
#define R11_MIXER_1         0x0300
#define R11_A_TIMING_1      0x0313
#define R11_A_TEMP_SEN      0x0316
#define R11_POWER_1         0x0322
#define R11_POWER_2         0x0324
#define R11_POWER_3         0x0326
#define R11_POWER_4         0x0328
#define R11_POWER_6         0x032A
#define R11_POWER_7         0x032B
#define R11_POWER_8         0x032C
#define R11_POWER_9         0x032D
#define R11_CLASS_D_POS     0x032E
#define R11_BOOST_CON_1     0x0330
#define R11_STP_0           0x050C
#define R11_STP_4           0x0510
#define R11_STP_6           0x0512
#define R11_STP_7           0x0516
#define R11_DC_DET_1        0x0519
#define R11_DC_DET_2        0x051A
#define R11_BAT_GAIN_1      0x0536
#define R11_CROSS_BQ_1      0x0702
#define R11_SYS_RESET_1     0x1007
#define R11_SYS_RESET_2     0x1008
#define R11_SYS_RESET_3     0x1009
#define R11_DC_CALIB_3      0x1204
#define R11_CLASSD_INT_1    0x1300
#define R11_SINE_GEN_1      0x1500
#define R11_R0_REG_HI       0x1538
#define R11_R0_REG_LO       0x1539
#define R11_R0_SYN_HI       0x1560
#define R11_R0_SYN_LO       0x1562
#define R11_EFUSE_CTRL_1    0x17BB

/* playback supplies (DAPM supply widgets of the DAC) */
#define R11_PWR1_BITS       0xE0A0      /* LDO2, DAC, CLK12M, I sense, V sense */
#define R11_PWR2_BITS       0x0007      /* PLL, bandgap, bandgap bias */
#define R11_PWR3_BITS       0x5FF5      /* VBAT detect, bias, sense ADCs, mixers, VREF */
#define R11_PWR4_BITS       0x1600      /* boost regulator */

static int r11_probe(struct cdc *c)
{
    return cdc_rd(c, R11_DEVICE_ID) == 0x1011 ? 0 : -1;
}

/* speaker resistance (R0) measurement and depop that Linux runs at every start; leaves the
   chip reset with the measured R0 loaded for the temperature protection */
static void r11_calibrate(struct cdc *c)
{
    static const u16 seq1[] = {
        R11_RESET, 0x0000, R11_SYS_RESET_3, 0x740F, R11_SYS_RESET_3, 0x770F,
        R11_CLK_2, 0x9400, R11_PLL_1, 0x0800, R11_PLL_2, 0x0020, R11_CLK_DET, 0x0800,
        R11_ADC_SET_5, 0x0A20, R11_DAC_SET_2, 0xE232, R11_ADC_SET_4, 0xC000,
        R11_DC_DET_1, 0xB00C, R11_DC_DET_2, 0xCCCC,
        R11_POWER_1, 0xE0E0, R11_POWER_3, 0x5003, R11_POWER_9, 0xA860, R11_DAC_SET_2, 0xA032,
        R11_POWER_2, 0x0007, R11_POWER_3, 0x5FF7, R11_A_TEMP_SEN, 0x7F44, R11_A_TIMING_1, 0x4054,
        R11_BAT_GAIN_1, 0x309C, R11_DC_CALIB_3, 0xCB00, R11_BOOST_CON_1, 0xE080,
        R11_POWER_4, 0x16F2, R11_POWER_6, 0x36AD, R11_MIXER_1, 0x3F1D,
        R11_EFUSE_CTRL_1, 0x0D0A
    };
    static const u16 seq2[] = {
        R11_ADC_SET_1, 0x2925, R11_CLASS_D_POS, 0x010E, R11_CLASSD_INT_1, 0x1701,
        R11_STP_0, 0x8000, R11_STP_7, 0xF000, R11_STP_4, 0x4040, R11_STP_0, 0xC000,
        R11_STP_6, 0x07C2
    };
    static const u16 depop[] = {
        R11_POWER_9, 0xA840, R11_STP_6, 0x0702, R11_MIXER_1, 0xFFDD, R11_CLASSD_INT_1, 0x0701,
        R11_DAC_SET_3, 0xE004, R11_A_TEMP_SEN, 0x7F40, R11_POWER_1, 0x0000, R11_POWER_2, 0x0000,
        R11_POWER_3, 0x0002, R11_POWER_4, 0x00F2, R11_RESET, 0x0000
    };
    u32 r0[3];
    int i, n = 0, hi, lo, ok = 0;
    cdc_seq(c, seq1, (int)ARRAY_SIZE(seq1) / 2);
    mdelay(30);
    cdc_seq(c, seq2, (int)ARRAY_SIZE(seq2) / 2);
    /* wait for three equal non-zero readings of the measured resistance */
    r0[0] = r0[1] = r0[2] = 0;
    for (i = 0; i < 50 && !ok; i++) {
        mdelay(100);
        hi = cdc_rd(c, R11_R0_SYN_HI);
        lo = cdc_rd(c, R11_R0_SYN_LO);
        if (hi < 0 || lo < 0) break;
        r0[n % 3] = ((u32)hi << 16) | (u32)lo;
        if (!r0[n % 3]) continue;
        n++;
        ok = n >= 3 && r0[0] == r0[1] && r0[1] == r0[2];
    }
    cdc_wr(c, R11_STP_0, 0x0000);
    mdelay(400);
    cdc_seq(c, depop, (int)ARRAY_SIZE(depop) / 2);
    if (ok) {
        DBG("rt1011: R0 register %lx\n", r0[0]);
        cdc_wr(c, R11_R0_REG_HI, (r0[0] >> 16) & 0x1FF);
        cdc_wr(c, R11_R0_REG_LO, r0[0] & 0xFFFF);
        cdc_wr(c, R11_STP_4, 0x4080);
    } else {
        kprintf("rt1011: speaker calibration failed\n");
    }
}

static int r11_init(struct cdc *c)
{
    static const u16 init[] = {
        R11_POWER_9, 0xA840, R11_ADC_SET_5, 0x0A20, R11_DAC_SET_2, 0xA032,
        R11_DC_DET_1, 0xB00C, R11_DC_DET_2, 0xCCCC, R11_A_TIMING_1, 0x6054,
        R11_POWER_7, 0x3E55, R11_POWER_8, 0x0520, R11_BOOST_CON_1, 0xE188, R11_POWER_4, 0x16F2,
        R11_CROSS_BQ_1, 0x0004, R11_SIL_DET, 0xC313, R11_SINE_GEN_1, 0x0707,
        R11_DC_CALIB_3, 0xCB00, R11_DAC_SET_1, 0xE702, R11_DAC_SET_3, 0x2004,
        R11_ADC_SET_1, 0x2925
    };
    struct rlpll p;
    u32 len = rt_len(c->ssp.vbits), cl = rt_len(c->ssp.width);
    c->priv[0] = 0;
    r11_calibrate(c);
    cdc_seq(c, init, (int)ARRAY_SIZE(init) / 2);
    /* consumer, serial format, normal BCLK */
    cdc_upd(c, R11_TDM_TOTAL, 0x4007, rt_df(c));
    cdc_upd(c, R11_TDM1_SET_1, 0x8000, 0);
    cdc_upd(c, R11_TDM2_SET_1, 0x8000, 0);
    /* system clock 256 fs from PLL1 on BCLK, MCLK detection off */
    if (rt_pll(c, rt_bclk(c), 256 * CDC_RATE, &p)) return -1;
    cdc_upd(c, R11_CLK_DET, 0x8000, 0);
    cdc_upd(c, R11_CLK_2, 0xE070, 0xA000);      /* PLL1 -> fs_sys, BCLK -> PLL1, divider 1 */
    cdc_wr(c, R11_PLL_1, (p.m << 12) | ((u32)p.mbp << 11) | p.n);
    cdc_wr(c, R11_PLL_2, p.k);
    /* data and channel lengths, channel of this amplifier from the stereo pair in slots 0/1 */
    cdc_upd(c, R11_TDM_TOTAL, 0x07E0, (len << 8) | (len << 5));
    cdc_upd(c, R11_TDM1_SET_1, 0x0077, (cl << 4) | cl);
    cdc_upd(c, R11_CROSS_BQ_1, 0x0060, rt_mono(c) << 5);
    cdc_upd(c, R11_TDM1_SET_4, 0x7700, 0x0100);
    if (rt_dsp(c)) {
        u32 v = (rt_nslot(c) << 10) | (rt_nslot(c) << 8) | (cl << 4) | cl;
        cdc_upd(c, R11_TDM1_SET_1, 0x0F77, v);
        cdc_upd(c, R11_TDM2_SET_1, 0x0F77, v);
        cdc_upd(c, R11_TDM1_SET_2, 0x0008, 0x0008);
        cdc_upd(c, R11_TDM2_SET_2, 0x0008, 0x0008);
    }
    /* no I/V sense data back to the SSP: ADCDAT pins as inputs */
    cdc_upd(c, R11_TDM_TOTAL, 0x0018, 0x0018);
    /* DAC muted until playback */
    cdc_upd(c, R11_DAC_SET_3, 0x8000, 0x8000);
    return 0;
}

static void r11_stream(struct cdc *c, int dir, int on)
{
    if (dir || rt_was_on(c, on)) return;
    if (on) {
        cdc_upd(c, R11_POWER_1, R11_PWR1_BITS, R11_PWR1_BITS);
        cdc_upd(c, R11_POWER_2, R11_PWR2_BITS, R11_PWR2_BITS);
        cdc_upd(c, R11_POWER_3, R11_PWR3_BITS, R11_PWR3_BITS);
        cdc_upd(c, R11_POWER_4, R11_PWR4_BITS, R11_PWR4_BITS);
        cdc_upd(c, R11_A_TEMP_SEN, 0x0004, 0x0004);
        cdc_upd(c, R11_DAC_SET_3, 0x8000, 0);
        /* temperature protection, class D out of shutdown, driver ready */
        cdc_upd(c, R11_STP_0, 0xC000, 0xC000);
        cdc_upd(c, R11_POWER_9, 0x0020, 0x0020);
        mdelay(50);
        cdc_upd(c, R11_CLASSD_INT_1, 0x1000, 0x1000);
    } else {
        cdc_upd(c, R11_POWER_9, 0x0020, 0);
        cdc_upd(c, R11_STP_0, 0xC000, 0);
        mdelay(200);
        cdc_upd(c, R11_CLASSD_INT_1, 0x1000, 0);
        cdc_upd(c, R11_DAC_SET_3, 0x8000, 0x8000);
        cdc_upd(c, R11_A_TEMP_SEN, 0x0004, 0);
        cdc_upd(c, R11_POWER_4, R11_PWR4_BITS, 0);
        cdc_upd(c, R11_POWER_3, R11_PWR3_BITS, 0);
        cdc_upd(c, R11_POWER_2, R11_PWR2_BITS, 0);
        cdc_upd(c, R11_POWER_1, R11_PWR1_BITS, 0);
        /* digital reset pulse of the bias-off state */
        cdc_wr(c, R11_SYS_RESET_1, 0x0000);
        cdc_wr(c, R11_SYS_RESET_2, 0x0000);
        cdc_wr(c, R11_SYS_RESET_3, 0x0001);
        cdc_wr(c, R11_SYS_RESET_1, 0x003F);
        cdc_wr(c, R11_SYS_RESET_2, 0x7FD7);
        cdc_wr(c, R11_SYS_RESET_3, 0x770F);
    }
}

static void r11_off(struct cdc *c)
{
    r11_stream(c, 0, 0);
    cdc_wr(c, R11_RESET, 0);
}

/* ---------------------------------------------------------------- RT1015 */

#define R15_RESET           0x0000
#define R15_CLK2            0x0004
#define R15_PLL1            0x000A
#define R15_PLL2            0x000C
#define R15_CLK_DET         0x0020
#define R15_DEVICE_ID       0x007D
#define R15_PAD_DRV2        0x00F2
#define R15_GAT_BOOST       0x00F3
#define R15_TDM_MASTER      0x0111
#define R15_TDM1_1          0x0114
#define R15_TDM1_4          0x011A
#define R15_PWR1            0x0322
#define R15_PWR9            0x032D
#define R15_DC_DETECT1      0x0519
#define R15_SYS_RST1        0x1007
#define R15_SYS_RST2        0x1009

static int r15_probe(struct cdc *c)
{
    int id = cdc_rd(c, R15_DEVICE_ID);
    return id == 0x1011 || id == 0x1015 ? 0 : -1;
}

/* DAC and boost off (the state after the Linux DAC power-down event), PLL supply off */
static void r15_down(struct cdc *c)
{
    cdc_wr(c, R15_PWR9, 0xA800);
    cdc_wr(c, R15_SYS_RST1, 0x05F5);
    cdc_wr(c, R15_SYS_RST2, 0x0B9A);
    cdc_upd(c, R15_PWR1, 0x0200, 0);
}

static int r15_init(struct cdc *c)
{
    struct rlpll p;
    u32 len = rt_len(c->ssp.vbits);
    if (len > 2) len = 2;                       /* 24 bit at most (MSBs of 32-bit samples) */
    c->priv[0] = 0;
    cdc_wr(c, R15_RESET, 0);
    mdelay(10);
    r15_down(c);
    /* consumer, serial format, normal BCLK */
    cdc_upd(c, R15_TDM_MASTER, 0x4007, rt_df(c));
    cdc_upd(c, R15_TDM1_1, 0x8000, 0);
    /* system clock 256 fs from the PLL on BCLK */
    if (rt_pll(c, rt_bclk(c), 256 * CDC_RATE, &p)) return -1;
    cdc_upd(c, R15_CLK2, 0xE070, 0xA000);       /* PLL -> sys clock, BCLK -> PLL, divider 1 */
    cdc_wr(c, R15_PLL1, (p.m << 12) | ((u32)p.mbp << 11) | p.n);
    cdc_wr(c, R15_PLL2, p.k);
    cdc_upd(c, R15_TDM_MASTER, 0x0700, len << 8);
    if (rt_dsp(c)) {
        /* TDM: slot count and length, stereo pair from slots 0/1 */
        cdc_upd(c, R15_TDM1_4, 0x7700, 0x0100);
        cdc_upd(c, R15_TDM1_1, 0x0F77, (rt_nslot(c) << 10) | (rt_len(c->ssp.width) << 4));
    }
    cdc_upd(c, R15_PAD_DRV2, 0x0030, rt_mono(c) << 4);
    return 0;
}

static void r15_stream(struct cdc *c, int dir, int on)
{
    if (dir || rt_was_on(c, on)) return;
    if (on) {
        cdc_upd(c, R15_PWR1, 0x0200, 0x0200);
        /* DAC and boost on */
        cdc_wr(c, R15_SYS_RST1, 0x05F7);
        cdc_wr(c, R15_SYS_RST2, 0x0B0A);
        cdc_wr(c, R15_GAT_BOOST, 0xACFE);
        cdc_wr(c, R15_PWR9, 0xAA00);
        cdc_wr(c, R15_GAT_BOOST, 0xECFE);
        /* BCLK and class D DC detection on, then the amplifier start-up time */
        cdc_upd(c, R15_CLK_DET, 0x8000, 0x8000);
        cdc_upd(c, R15_DC_DETECT1, 0x1000, 0x1000);
        mdelay(50);
    } else {
        r15_down(c);
    }
}

static void r15_off(struct cdc *c)
{
    r15_stream(c, 0, 0);
    cdc_wr(c, R15_RESET, 0);
}

/* ---------------------------------------------------------------- RT1015P / RT1019P */

static int r15p_init(struct cdc *c)
{
    c->priv[0] = 0;
    c->priv[1] = 0;
    cdc_gpio(c, 0, 0);                          /* sdb low: shut down */
    return 0;
}

static void r15p_stream(struct cdc *c, int dir, int on)
{
    if (dir || rt_was_on(c, on)) return;
    cdc_gpio(c, 0, on);
    if (on && !c->priv[1]) {
        mdelay(300);                            /* calibration after the first power-up */
        c->priv[1] = 1;
    }
}

static void r15p_off(struct cdc *c)
{
    r15p_stream(c, 0, 0);
    c->priv[1] = 0;
}

/* ---------------------------------------------------------------- RT1019 (16-bit reg, 8-bit val) */

#define R19_IDS_CTRL        0x0011
#define R19_PWR_STRP_2      0x0019
#define R19_DEV_ID_1        0x0061
#define R19_DEV_ID_2        0x0062
#define R19_SDB_CTRL        0x0066
#define R19_CLK_TREE_1      0x0100
#define R19_CLK_TREE_2      0x0101
#define R19_CLK_TREE_3      0x0102
#define R19_PLL_1           0x0311
#define R19_PLL_2           0x0312
#define R19_PLL_3           0x0313
#define R19_TDM_1           0x0400
#define R19_TDM_2           0x0401
#define R19_TDM_3           0x0402

static int r19_probe(struct cdc *c)
{
    int h = cdc_rd(c, R19_DEV_ID_1), l = cdc_rd(c, R19_DEV_ID_2), id;
    if (h < 0 || l < 0) return -1;
    id = (h << 8) | l;
    return id == 0x1019 || id == 0x6731 ? 0 : -1;
}

static int r19_init(struct cdc *c)
{
    struct rlpll p;
    c->priv[0] = 0;
    cdc_wr(c, R19_SDB_CTRL, 0x0A);              /* shut down */
    /* serial format, normal BCLK (always a consumer) */
    cdc_upd(c, R19_TDM_2, 0x38, rt_df(c) << 3);
    cdc_upd(c, R19_TDM_1, 0x40, 0);
    /* system clock 256 fs from the PLL on BCLK, manual clock and length selection */
    if (rt_pll(c, rt_bclk(c), 256 * CDC_RATE, &p)) return -1;
    cdc_upd(c, R19_CLK_TREE_1, 0x10, 0);
    cdc_upd(c, R19_PWR_STRP_2, 0x30, 0);
    cdc_upd(c, R19_PLL_1, 0xF3, (p.m << 4) | ((u32)p.mbp << 1) | ((p.n >> 8) & 1));
    cdc_wr(c, R19_PLL_2, p.n & 0xFF);
    cdc_upd(c, R19_PLL_3, 0x1F, p.k);
    cdc_upd(c, R19_CLK_TREE_1, 0x80, 0x80);
    if (rt_dsp(c)) {
        cdc_upd(c, R19_TDM_3, 0x77, 0x01);      /* stereo pair from slots 0/1 */
        cdc_upd(c, R19_TDM_1, 0x07, rt_len(c->ssp.width));
        cdc_upd(c, R19_TDM_2, 0xC0, rt_nslot(c) << 6);
    }
    /* data length, dividers for a 256 fs system clock */
    cdc_upd(c, R19_TDM_2, 0x07, rt_len(c->ssp.vbits));
    cdc_upd(c, R19_CLK_TREE_1, 0x0C, 0);
    cdc_upd(c, R19_CLK_TREE_2, 0xEF, 0x40);
    cdc_upd(c, R19_CLK_TREE_3, 0xC0, 0);
    cdc_upd(c, R19_IDS_CTRL, 0x03, rt_mono(c));
    return 0;
}

static void r19_stream(struct cdc *c, int dir, int on)
{
    if (dir || rt_was_on(c, on)) return;
    cdc_wr(c, R19_SDB_CTRL, on ? 0x0B : 0x0A);
}

static void r19_off(struct cdc *c)
{
    r19_stream(c, 0, 0);
}

/* ---------------------------------------------------------------- RT1308 (8-bit reg, 32-bit val) */

#define R08_RESET           0x00
#define R08_PLL_1           0x03
#define R08_CLK_1           0x06
#define R08_DATA_PATH       0x07
#define R08_CLK_2           0x08
#define R08_SIL_DET         0x09
#define R08_CLK_DET         0x0A
#define R08_DAC_SET         0x10
#define R08_I2S_SET_1       0x15
#define R08_I2S_SET_2       0x16
#define R08_I2C_I2S_SDW     0x17
#define R08_IV_SENSE        0x1A
#define R08_DC_CAL_2        0x21
#define R08_PVDD_OFFSET     0x24
#define R08_CLASS_D_SET_2   0x31
#define R08_POWER           0x32
#define R08_VREF            0x34
#define R08_POWER_STATUS    0x36
#define R08_VEN_DEV_ID      0x70
#define R08_EFUSE_1         0x7F
#define R08_PADS_1          0x90
#define R08_DUMMY           0xF0

#define R08_PWR_BITS        0xFF371F00UL        /* biases, LDOs, DACs, ADCs, mixers, PLL2 */
#define R08_PDB_BITS        0x03000000UL        /* class D out of power-down */

static int r08_rd(struct cdc *c, u8 reg, u32 *v)
{
    u8 r[4];
    if (i2c_xfer(c->bus, c->addr, &reg, 1, r, 4)) return -1;
    *v = ((u32)r[0] << 24) | ((u32)r[1] << 16) | ((u32)r[2] << 8) | r[3];
    return 0;
}

static void r08_wr(struct cdc *c, u8 reg, u32 v)
{
    u8 w[5];
    w[0] = reg;
    w[1] = (u8)(v >> 24);
    w[2] = (u8)(v >> 16);
    w[3] = (u8)(v >> 8);
    w[4] = (u8)v;
    i2c_xfer(c->bus, c->addr, w, 5, NULL, 0);
}

static void r08_upd(struct cdc *c, u8 reg, u32 mask, u32 val)
{
    u32 v;
    if (!r08_rd(c, reg, &v)) r08_wr(c, reg, (v & ~mask) | (val & mask));
}

static int r08_probe(struct cdc *c)
{
    u32 id;
    return !r08_rd(c, R08_VEN_DEV_ID, &id) && (id & 0xFFFFFF00UL) == 0x10EC1300UL ? 0 : -1;
}

static int r08_init(struct cdc *c)
{
    static const struct { u8 reg; u32 val; } init[] = {
        { R08_I2C_I2S_SDW, 0x01014005UL }, { R08_CLASS_D_SET_2, 0x227F5501UL },
        { R08_PADS_1, 0x50150505UL }, { R08_VREF, 0x18100000UL }, { R08_IV_SENSE, 0x87010000UL },
        { R08_DUMMY, 0x00000200UL }, { R08_SIL_DET, 0xE1C30000UL }, { R08_DC_CAL_2, 0x00FFFF00UL },
        { R08_CLK_DET, 0x01000000UL }, { R08_POWER_STATUS, 0x08800000UL },
        { R08_DAC_SET, 0xAFAF0700UL }
    };
    static const u32 path[3] = { 0x01000000UL, 0x03000000UL, 0 };      /* LL, RR, LR */
    struct rlpll p;
    u32 fin, len = rt_len(c->ssp.vbits);
    int i;
    if (len > 2) len = 2;
    c->priv[0] = 0;
    /* reset and efuse load */
    r08_wr(c, R08_RESET, 0);
    r08_wr(c, R08_POWER_STATUS, 0x01800000UL);
    mdelay(100);
    r08_wr(c, R08_EFUSE_1, 0x44FE0F00UL);
    mdelay(20);
    r08_wr(c, R08_PVDD_OFFSET, 0x10000000UL);
    for (i = 0; i < (int)ARRAY_SIZE(init); i++) r08_wr(c, init[i].reg, init[i].val);
    /* serial format, normal BCLK */
    r08_upd(c, R08_I2S_SET_1, 0x3000, rt_df(c) << 12);
    r08_upd(c, R08_I2S_SET_2, 0x4000, 0);
    /* system clock 512 fs from the PLL: MCLK as the Linux machine driver, else BCLK */
    fin = c->ssp.mclk ? c->ssp.mclk : rt_bclk(c);
    if (rt_pll(c, fin, 512 * CDC_RATE, &p)) return -1;
    if (p.n > 0x7F) {
        kprintf("rt1308: PLL N %lu out of range for %lu Hz\n", p.n, fin);
        return -1;
    }
    if (c->ssp.mclk) {
        r08_upd(c, R08_CLK_2, 0x07000000UL, 0);
        r08_upd(c, R08_CLK_DET, 0x02000000UL, 0x02000000UL);
    } else {
        r08_upd(c, R08_CLK_2, 0x07000000UL, 0x01000000UL);
    }
    r08_wr(c, R08_PLL_1, (p.k << 24) | ((u32)p.mbp << 23) | (p.m << 16) | (p.n << 8));
    /* PLL -> sys clock, divider 2 (512 fs -> 256 fs), data length */
    r08_upd(c, R08_CLK_1, 0xF7000000UL, 0x12000000UL);
    r08_upd(c, R08_I2S_SET_2, 0x07000000UL, len << 24);
    /* stereo chip: both channels, or the one of this amplifier when there are several */
    r08_upd(c, R08_DATA_PATH, 0x03000000UL, path[c->namps <= 1 ? 2 : c->chan & 1]);
    return 0;
}

static void r08_stream(struct cdc *c, int dir, int on)
{
    if (dir || rt_was_on(c, on)) return;
    if (on) {
        r08_upd(c, R08_POWER, R08_PWR_BITS, R08_PWR_BITS);
        mdelay(30);
        r08_upd(c, R08_POWER_STATUS, R08_PDB_BITS, R08_PDB_BITS);
        mdelay(40);
    } else {
        r08_upd(c, R08_POWER_STATUS, R08_PDB_BITS, 0);
        mdelay(150);
        r08_upd(c, R08_POWER, R08_PWR_BITS, 0);
    }
}

static void r08_off(struct cdc *c)
{
    r08_stream(c, 0, 0);
    r08_wr(c, R08_RESET, 0);
}

/* ---------------------------------------------------------------- drivers */

const struct cdcdrv cdc_rt1011 = { "rt1011", { "10EC1011", NULL }, CK_AMP, 16, 16, 32,
                                   r11_probe, r11_init, r11_stream, NULL, NULL, NULL, r11_off };
const struct cdcdrv cdc_rt1015 = { "rt1015", { "10EC1015", NULL }, CK_AMP, 16, 16, 32,
                                   r15_probe, r15_init, r15_stream, NULL, NULL, NULL, r15_off };
const struct cdcdrv cdc_rt1015p = { "rt1015p", { "RTL1015", "RTL1019", NULL }, CK_AMP, 8, 8, 32,
                                    NULL, r15p_init, r15p_stream, NULL, NULL, NULL, r15p_off };
const struct cdcdrv cdc_rt1019 = { "rt1019", { "10EC1019", NULL }, CK_AMP, 16, 8, 32,
                                   r19_probe, r19_init, r19_stream, NULL, NULL, NULL, r19_off };
/* regbits/valbits unused: the RT1308 has 32-bit registers (r08_rd / r08_wr) */
const struct cdcdrv cdc_rt1308i2s = { "rt1308", { "10EC1308", NULL }, CK_AMP, 8, 8, 32,
                                      r08_probe, r08_init, r08_stream, NULL, NULL, NULL, r08_off };
