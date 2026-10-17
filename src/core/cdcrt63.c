/* cdcrt63.c - Realtek RT5663 headset codec (Kaby Lake Chromebooks and laptops): headphones
   and the combo jack's headset microphone. Two silicon versions with different sequences:
   id 0x6406 (version 0) and 0x6451 (version 1, "v2" in Linux). Sequences after the Linux
   rt5663 driver (calibration, jack type detection) and the avs rt5663 machine board (system
   clock = MCLK 24.576 MHz, ASRC for the stereo DAC / ADC filters, I2S consumer); other MCLK
   rates go through the codec PLL (from BCLK when known). */
#include "dspcdc.h"

#define HP_OUT_EN       0x0002
#define HP_AMP_2        0x0003          /* v2 */
#define HP_LCH_DRE      0x0005          /* v2 */
#define HP_RCH_DRE      0x0006          /* v2 */
#define AEC_BST         0x000B          /* v2 */
#define RECMIX          0x0010
#define CBJ_TYPE_2      0x0011          /* v2 */
#define CBJ_TYPE_3      0x0012          /* v2 */
#define DAC_VOL         0x0019
#define ADC_VOL         0x001C
#define STO1_ADC_MIX    0x0026
#define AD_DA_MIX       0x0029
#define STO_DAC_MIX     0x002A
#define BYPASS_STO_DAC  0x002D
#define RECMIX1L        0x003C          /* v2 */
#define PWR_DIG_1       0x0061
#define PWR_DIG_2       0x0062
#define PWR_ANLG_1      0x0063
#define PWR_ANLG_2      0x0064
#define PWR_ANLG_3      0x0065
#define PWR_VOL         0x0067          /* v2 */
#define SIG_CLK_DET     0x006B
#define PRE_DIV_GATE_1  0x006E
#define PRE_DIV_GATE_2  0x006F
#define I2S1_SDP        0x0070
#define ADDA_CLK_1      0x0073
#define ADDA_RST        0x0074
#define TDM_1           0x0077
#define TDM_2           0x0078
#define GLB_CLK         0x0080
#define PLL_1           0x0081
#define PLL_2           0x0082
#define ASRC_1          0x0083
#define ASRC_2          0x0084
#define ASRC_3          0x0085
#define DEPOP_1         0x008E
#define DEPOP_2         0x008F
#define HP_CHG_PUMP_1   0x0091
#define HP_CHG_PUMP_2   0x0092
#define RC_CLK          0x0094
#define AUTO_1MRC_CLK   0x009F
#define IRQ_1           0x00B6
#define INT_ST_1        0x00BE
#define INT_ST_2        0x00BF
#define GPIO_1          0x00C0
#define GPIO_2          0x00C1
#define GPIO_3          0x00C2
#define IL_CMD_2        0x00DC
#define IL_CMD_5        0x00DF
#define EM_JACK_TYPE_1  0x00E6
#define EM_JACK_TYPE_2  0x00E7
#define STO1_HPF_ADJ1   0x00EC
#define DIG_MISC        0x00FA
#define VENDOR_ID_2     0x00FF
#define DIG_VOL_ZCD     0x0100
#define ANA_BIAS_CUR_1  0x0108
#define ANA_BIAS_CUR_4  0x010B
#define BIAS_CUR_8      0x0111
#define DACREF_LDO      0x0112
#define VREFADJ_OP      0x0117
#define VREF_RECMIX     0x0118
#define CHARGE_PUMP_1   0x0125
#define CHARGE_PUMP_2   0x0128
#define CHOP_DAC_L      0x013A
#define CHOP_ADC        0x013B
#define CALIB_ADC       0x013C
#define STO_DRE_1       0x0160
#define STO_DRE_9       0x0168
#define STO_DRE_10      0x0169
#define HP_IMP_SEN_19   0x01D2
#define HP_LOGIC_2      0x01DB
#define HP_CALIB_1      0x01DD
#define HP_CALIB_1_1    0x01DE
#define HP_CALIB_2      0x01DF
#define HP_CALIB_3      0x01E0
#define HP_CALIB_7      0x01E5
#define HP_BIAS         0x0202
#define CBJ_1           0x0250          /* version 0 */
#define CBJ_2           0x0251          /* version 0 */
#define DUMMY_1         0x02FA
#define DUMMY_2         0x02FB

/* c->priv */
#define P_V2            0               /* silicon version 1 (id 0x6451) */
#define P_DET           1               /* jack detection: 0 out, step count, D_HP, D_HS */
#define P_RUN           2               /* bit 0 playback, bit 1 capture running */
#define P_BCLK          3               /* PLL on BCLK: RC clock while idle */
#define P_MUTE          4               /* bit 0 left, bit 1 right at volume 0 */
#define P_SCLK          5               /* GLB_CLK system clock source while streams run */
#define D_HP            0x40
#define D_HS            0x41

static u32 gcd32(u32 a, u32 b)
{
    u32 t;
    while (b) {
        t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static u32 adiff(u32 a, u32 b)
{
    return a > b ? a - b : b - a;
}

/* RL6231 PLL (rl6231_pll_calc): fout = fin * (N + 2) / ((M + 2) * (K + 2)), M can be
   bypassed; gives the PLL_1 / PLL_2 values */
static int rl6231_pll(u32 fin, u32 fout, u16 *pll1, u16 *pll2)
{
    static const u32 preset[][5] = {     /* in, out, K, N, M | M bypass << 8 */
        { 19200000UL, 4096000UL, 23, 14, 1 },
        { 19200000UL, 24576000UL, 3, 30, 3 },
        { 48000000UL, 3840000UL, 23, 2, 0 },
        { 3840000UL, 24576000UL, 3, 30, 0x100 },
        { 3840000UL, 22579200UL, 3, 5, 0x100 }
    };
    int i, k, n = 0, m = 0, mbp = 0, kt, nt, mt, mink, maxk;
    u32 red, redt, in_t, out_t, fi, fo, d, divt, fmax;

    if (fin > 50000000UL || fin < 256000UL || !fout) return -1;
    for (i = 0; i < (int)ARRAY_SIZE(preset); i++) {
        if (fin == preset[i][0] && fout == preset[i][1]) {
            k = (int)preset[i][2];
            n = (int)preset[i][3];
            m = (int)(preset[i][4] & 0xFF);
            mbp = (preset[i][4] >> 8) ? 1 : 0;
            goto found;
        }
    }
    redt = adiff(fout, fin);
    mink = (int)(80000000UL / fout) - 2;
    maxk = (int)(150000000UL / fout) - 2;
    if (maxk > 31) maxk = 31;
    if (mink > 31) mink = maxk = 31;
    divt = gcd32(fin, fout);
    fmax = 0xFFFFFFFFUL / 0x1FF;
    d = 1;
    if (fin > fmax) {
        d = fin / fmax + (fin % fmax ? 1 : 0);
        while (divt % d) d++;
    }
    fi = fin / d;
    fo = fout / d;
    k = mink;
    if (mink < -1) mink = -1;
    for (kt = mink; kt <= maxk; kt++) {
        for (nt = 0; nt <= 0x1FF; nt++) {
            in_t = fi * (u32)(nt + 2);
            if (in_t == fo * (u32)(kt + 2)) {
                mbp = 1;
                n = nt;
                k = kt;
                goto found;
            }
            out_t = in_t / (u32)(kt + 2);
            red = adiff(fo, out_t);
            if (red < redt) {
                mbp = 1;
                n = nt;
                m = 0;
                k = kt;
                if (!red) goto found;
                redt = red;
            }
            for (mt = 0; mt <= 15; mt++) {
                out_t = in_t / ((u32)(mt + 2) * (u32)(kt + 2));
                red = adiff(fo, out_t);
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
    if (k < 0) k = 0;
    *pll1 = (u16)(((u32)n << 7) | (u32)k);
    *pll2 = (u16)(((u32)(mbp ? 0 : m) << 12) | ((u32)mbp << 11));
    return 0;
}

/* wait until (reg & mask) == want, 10 ms steps */
static void rt63_wait(struct cdc *c, u32 reg, u32 mask, u32 want, int tries)
{
    int v;
    while (tries-- > 0) {
        v = cdc_rd(c, reg);
        if (v < 0 || ((u32)v & mask) == want) return;
        mdelay(10);
    }
}

/* version 1 (0x6451): headphone DC calibration */
static void rt63_v2_calibrate(struct cdc *c)
{
    static const u16 seq[] = {
        BIAS_CUR_8, 0xA402, PWR_DIG_1, 0x0100, RECMIX, 0x4040, DIG_MISC, 0x0001,
        RC_CLK, 0x0380, GLB_CLK, 0x8000, ADDA_CLK_1, 0x1000, CHOP_DAC_L, 0x3030,
        CALIB_ADC, 0x3C05, PWR_ANLG_1, 0xA23E
    };
    cdc_seq(c, seq, (int)ARRAY_SIZE(seq) / 2);
    mdelay(40);
    cdc_wr(c, PWR_ANLG_1, 0xF23E);
    cdc_wr(c, HP_CALIB_2, 0x0321);
    cdc_wr(c, HP_CALIB_1, 0xFC00);
    mdelay(500);
}

/* version 0 (0x6406): jack type and headphone DC calibration */
static void rt63_calibrate(struct cdc *c)
{
    static const u16 seq1[] = {
        ANA_BIAS_CUR_4, 0x00A1, RC_CLK, 0x0380, GLB_CLK, 0x8000, ADDA_CLK_1, 0x1000,
        VREF_RECMIX, 0x0032, HP_IMP_SEN_19, 0x000C, DUMMY_1, 0x0324, DIG_MISC, 0x8001,
        VREFADJ_OP, 0x0F28, PWR_ANLG_1, 0xA23B
    };
    static const u16 seq2[] = {
        PWR_ANLG_1, 0xF23B, PWR_ANLG_2, 0x8000, PWR_ANLG_3, 0x0008, PRE_DIV_GATE_1, 0xFFFF,
        PRE_DIV_GATE_2, 0xFFFF, CBJ_1, 0x8C10, IL_CMD_2, 0x00C1, EM_JACK_TYPE_1, 0xB880,
        EM_JACK_TYPE_2, 0x4110, EM_JACK_TYPE_2, 0x4118
    };
    static const u16 seq3[] = {
        HP_IMP_SEN_19, 0x0000, DEPOP_2, 0x3003, DEPOP_1, 0x0038, DEPOP_1, 0x003B,
        PWR_DIG_2, 0x8400, PWR_DIG_1, 0x8DF8, PWR_ANLG_2, 0x8003, PWR_ANLG_3, 0x018C,
        HP_CHG_PUMP_1, 0x1E32, DUMMY_2, 0x8089, DACREF_LDO, 0x3B0B
    };
    static const u16 seq4[] = {
        STO_DAC_MIX, 0x0000, BYPASS_STO_DAC, 0x000C, HP_BIAS, 0xAFAA, CHARGE_PUMP_1, 0x2224,
        HP_OUT_EN, 0x8088, STO_DRE_9, 0x0017, STO_DRE_10, 0x0017, STO1_ADC_MIX, 0x4040,
        CHOP_ADC, 0x3000, RECMIX, 0x0005, ADDA_RST, 0xC000, STO1_HPF_ADJ1, 0x3320,
        HP_CALIB_2, 0x00C9, DUMMY_1, 0x004C, ANA_BIAS_CUR_1, 0x1111, BIAS_CUR_8, 0x4402,
        CHARGE_PUMP_2, 0x3311, HP_CALIB_1, 0x0069, HP_CALIB_3, 0x06CE, HP_CALIB_1_1, 0x6800,
        CHARGE_PUMP_2, 0x1100, HP_CALIB_7, 0x0057, HP_CALIB_1_1, 0xE800
    };
    static const u16 seq5[] = {
        EM_JACK_TYPE_1, 0xB8E0, PWR_ANLG_1, 0x003B, PWR_DIG_1, 0x0000, DEPOP_1, 0x000B,
        DEPOP_1, 0x0008, PWR_ANLG_2, 0x0000
    };
    int i;
    cdc_wr(c, 0x0000, 0);
    mdelay(20);
    cdc_seq(c, seq1, (int)ARRAY_SIZE(seq1) / 2);
    mdelay(30);
    cdc_seq(c, seq2, (int)ARRAY_SIZE(seq2) / 2);
    rt63_wait(c, INT_ST_2, 0x0080, 0x0080, 200);
    cdc_seq(c, seq3, (int)ARRAY_SIZE(seq3) / 2);
    mdelay(40);
    cdc_seq(c, seq4, (int)ARRAY_SIZE(seq4) / 2);
    rt63_wait(c, HP_CALIB_1_1, 0x8000, 0, 200);
    cdc_wr(c, HP_CALIB_1_1, 0x6200);
    cdc_wr(c, HP_CALIB_7, 0x0059);
    cdc_wr(c, HP_CALIB_1_1, 0xE200);
    rt63_wait(c, HP_CALIB_1_1, 0x8000, 0, 200);
    for (i = 0; i < (int)ARRAY_SIZE(seq5) / 2; i++) {
        cdc_wr(c, seq5[2 * i], seq5[2 * i + 1]);
        mdelay(10);
    }
}

static int rt63_probe(struct cdc *c)
{
    int id = cdc_rd(c, VENDOR_ID_2);
    if (id != 0x6451 && id != 0x6406) return -1;
    c->priv[P_V2] = id == 0x6451;
    return 0;
}

/* DAC input: muted while no playback runs and per channel at volume 0 */
static void rt63_dac(struct cdc *c)
{
    u32 v = 0x8080, idle = !(c->priv[P_RUN] & 1);
    if (idle || (c->priv[P_MUTE] & 1)) v |= 0x4000;
    if (idle || (c->priv[P_MUTE] & 2)) v |= 0x0040;
    cdc_wr(c, AD_DA_MIX, v);
}

/* system clock: MCLK when it is 24.576 MHz (512 x fs), else the PLL from BCLK or MCLK */
static int rt63_clock(struct cdc *c)
{
    u32 fin, sclk;
    u16 p1, p2;
    int bclk = 0;
    c->priv[P_BCLK] = 0;
    if (c->ssp.mclk == 512UL * CDC_RATE) {
        cdc_upd(c, GLB_CLK, 0xC000, 0x0000);
        c->priv[P_SCLK] = 0x0000;
        return 0;
    }
    fin = c->ssp.mclk;
    if (c->ssp.bclk && !c->ssp.provider) {
        fin = c->ssp.bclk;
        bclk = 1;
    }
    if (!fin || rl6231_pll(fin, 512UL * CDC_RATE, &p1, &p2)) {
        kprintf("rt5663: no PLL setting for %lu Hz %s\n", fin, bclk ? "BCLK" : "MCLK");
        return -1;
    }
    if (c->priv[P_V2]) cdc_upd(c, GLB_CLK, 0x0700, bclk ? 0x0100 : 0);
    else cdc_upd(c, GLB_CLK, 0x3800, bclk ? 0x0800 : 0);
    cdc_wr(c, PLL_1, p1);
    cdc_wr(c, PLL_2, p2);
    cdc_upd(c, PWR_ANLG_3, 0x0040, 0x0040);     /* PLL */
    sclk = 0x4000;
    c->priv[P_SCLK] = sclk;
    c->priv[P_BCLK] = (u32)bclk;
    cdc_upd(c, GLB_CLK, 0xC000, bclk ? 0x8000 : sclk);
    DBG("rt5663: PLL from %s %lu Hz, %04x %04x\n", bclk ? "BCLK" : "MCLK", fin, p1, p2);
    return 0;
}

static int rt63_init(struct cdc *c)
{
    int v2 = (int)c->priv[P_V2];
    u32 v, sl, w;
    c->priv[P_DET] = c->priv[P_RUN] = c->priv[P_MUTE] = 0;
    /* reset, calibrate, reset */
    cdc_wr(c, 0x0000, 0);
    if (v2) rt63_v2_calibrate(c);
    else rt63_calibrate(c);
    cdc_wr(c, 0x0000, 0);
    if (!v2) {
        cdc_wr(c, 0x002A, 0x8020);
        cdc_wr(c, 0x0086, 0x0028);
        cdc_wr(c, 0x0100, 0xA020);
        cdc_wr(c, 0x0117, 0x0F28);
        cdc_wr(c, 0x02FB, 0x8089);
    }
    cdc_upd(c, GPIO_1, 0x8000, 0x8000);         /* GPIO1 = IRQ */
    cdc_upd(c, IL_CMD_5, 0x000C, 0x000C);       /* inline command debounce 65 ms */
    if (v2) {
        /* JD1 with power saving, combo jack detection (QFN) */
        cdc_wr(c, BIAS_CUR_8, 0xA402);
        cdc_upd(c, AUTO_1MRC_CLK, 0xC000, 0xC000);
        cdc_upd(c, PWR_ANLG_2, 0x0008, 0x0008);
        cdc_upd(c, IRQ_1, 0x0008, 0x0008);
        cdc_upd(c, HP_LOGIC_2, 0x0003, 0x0002);
        cdc_upd(c, RECMIX, 0x9040, 0x9040);
        cdc_upd(c, GPIO_2, 0x0020, 0);
        cdc_upd(c, GPIO_3, 0x2000, 0);
        cdc_upd(c, PWR_ANLG_1, 0x000F, 0x0004); /* LDO1 0.9 V, HP amp 3x */
    } else {
        cdc_upd(c, DIG_MISC, 0x0001, 0x0001);
        cdc_upd(c, AUTO_1MRC_CLK, 0x0100, 0x0100);
        cdc_upd(c, IRQ_1, 0x0040, 0x0040);      /* JD1 IRQ */
        cdc_wr(c, VREF_RECMIX, 0x0032);
        cdc_upd(c, GPIO_2, 0x0004, 0x0004);
        cdc_upd(c, RECMIX, 0x0001, 0x0000);     /* BST1 to RECMIX1 */
        cdc_upd(c, TDM_2, 0xC000, 0x8000);      /* left ADC data in both slots */
    }
    /* ASRC (I2S1 clock) for the stereo DAC and ADC filters */
    cdc_upd(c, ASRC_2, 0x7000, 0x1000);
    if (v2) cdc_upd(c, ASRC_3, 0x7000, 0x1000);
    else cdc_upd(c, ASRC_2, 0x0007, 0x0001);
    if (rt63_clock(c)) return -1;
    /* I2S: consumer (or provider), format, data length; pre-divider 512 fs / 2 = 256 fs */
    v = c->ssp.provider ? 0 : 0x8000;
    switch (c->ssp.fmt) {
    case 3: v |= 1; break;
    case 4: v |= 2; break;
    case 5: v |= 3; break;
    default: break;
    }
    v |= c->ssp.vbits == 16 ? 0x00 : c->ssp.vbits == 20 ? 0x10 : 0x20;
    cdc_upd(c, I2S1_SDP, 0x8137, v);
    cdc_upd(c, ADDA_CLK_1, 0x7000, 0x1000);
    if (c->ssp.slots > 2) {
        sl = c->ssp.slots >= 8 ? 3 : c->ssp.slots >= 6 ? 2 : 1;
        w = c->ssp.width >= 32 ? 3 : c->ssp.width >= 24 ? 2 : c->ssp.width >= 20 ? 1 : 0;
        cdc_upd(c, v2 ? TDM_2 : TDM_1, 0x8FF0, 0x8000 | (sl << 10) | (sl << 8) | (w << 6) | (w << 4));
    }
    /* bias: standby, prepare, on */
    if (v2) cdc_upd(c, DIG_MISC, 0x0001, 0);
    cdc_upd(c, PWR_ANLG_1, 0xF200, 0xA200);     /* VREF1, VREF2, MBIAS */
    mdelay(10);
    if (v2) {
        cdc_upd(c, DIG_MISC, 0x0001, 0x0001);
        cdc_upd(c, SIG_CLK_DET, 0x8001, 0x8001);
    }
    cdc_upd(c, PWR_ANLG_1, 0x5000, 0x5000);     /* fast VREF1 / VREF2 */
    /* playback: I2S -> ADDA mixer -> stereo DAC filter -> DAC -> HP amplifier */
    cdc_upd(c, ASRC_1, 0x0C08, 0x0C08);         /* ASRC: I2S1, DAC, ADC */
    if (!v2) {
        cdc_wr(c, PRE_DIV_GATE_1, 0xFF00);
        cdc_wr(c, PRE_DIV_GATE_2, 0xFFFC);
    }
    rt63_dac(c);
    cdc_upd(c, STO_DAC_MIX, 0x8020, 0x0000);
    cdc_upd(c, PWR_DIG_1, 0x8C10, 0x8C10);      /* I2S, DAC L/R, ADC L */
    cdc_upd(c, PWR_DIG_2, 0x8400, 0x8400);      /* stereo ADC / DAC filters */
    cdc_upd(c, CHOP_ADC, 0x1000, 0x1000);       /* ADC clock */
    cdc_wr(c, DAC_VOL, 0xAFAF);
    if (v2) {
        cdc_upd(c, PWR_DIG_1, 0x0100, 0x0100);  /* DAC LDO */
        cdc_upd(c, CHOP_DAC_L, 0x1000, 0x1000); /* DAC clock */
        /* HP amplifier (the Linux event's charge pump update clears bit 3) */
        cdc_upd(c, HP_CHG_PUMP_1, 0x0008, 0);
        cdc_upd(c, HP_LOGIC_2, 0x0003, 0x0003);
        cdc_upd(c, HP_AMP_2, 0x4000, 0x4000);   /* DAC to HPO */
        cdc_upd(c, HP_LCH_DRE, 0x0F00, 0);      /* 0 dB */
        cdc_upd(c, HP_RCH_DRE, 0x0F00, 0);
    } else {
        /* charge pump, then the HP amplifier */
        cdc_upd(c, DEPOP_1, 0x0030, 0x0030);
        cdc_upd(c, DEPOP_1, 0x0003, 0x0003);
        cdc_upd(c, DACREF_LDO, 0x3E0E, 0x3A0A);
        cdc_wr(c, DEPOP_2, 0x3003);
        cdc_upd(c, HP_CHG_PUMP_1, 0x0004, 0);
        cdc_wr(c, HP_CHG_PUMP_2, 0x1371);
        cdc_wr(c, HP_BIAS, 0xABBA);
        cdc_wr(c, CHARGE_PUMP_1, 0x2224);
        cdc_wr(c, ANA_BIAS_CUR_1, 0x7766);
        cdc_wr(c, HP_BIAS, 0xAFAA);
        cdc_wr(c, CHARGE_PUMP_2, 0x7777);
        cdc_upd(c, STO_DRE_1, 0x8000, 0x8000);
        cdc_upd(c, DEPOP_1, 0x3000, 0x3000);
        cdc_upd(c, DIG_VOL_ZCD, 0x00C0, 0x0080);
        cdc_upd(c, STO_DRE_9, 0x001F, 0x0007);  /* 0 dB */
        cdc_upd(c, STO_DRE_10, 0x001F, 0x0007);
    }
    /* capture: IN1 (combo jack) -> BST1 -> RECMIX1L -> ADC L -> stereo ADC mixer (L, R) */
    cdc_upd(c, PWR_ANLG_2, 0x0002, 0x0002);     /* RECMIX1L */
    if (v2) {
        cdc_upd(c, RECMIX1L, 0x0080, 0);        /* BST1 CBJ */
        cdc_upd(c, AEC_BST, 0x0F00, 0x0200);    /* +24 dB */
        cdc_wr(c, STO1_ADC_MIX, 0x6060);        /* ADC1 L / R from ADC L */
    } else {
        cdc_upd(c, CBJ_2, 0x000F, 0x0002);      /* +24 dB */
        cdc_upd(c, STO1_ADC_MIX, 0x8000, 0);
    }
    cdc_wr(c, ADC_VOL, 0x2F2F);
    return 0;
}

/* microphone bias and detection power for the headset (version 1) */
static void rt63_v2_mic(struct cdc *c, int on)
{
    cdc_upd(c, PWR_ANLG_3, 0x0204, on ? 0x0204 : 0);    /* CBJ, LDO2 */
    cdc_upd(c, PWR_ANLG_2, 0x0C00, on ? 0x0C00 : 0);    /* micbias 1, 2 */
    cdc_upd(c, PWR_VOL, 0x0020, on ? 0x0020 : 0);       /* mic detection */
}

/* jack type: version 1 reads the combo jack result after up to ~0.9 s, version 0 runs the
   embedded jack type detection and waits for its done flag (up to ~2 s) */
static int rt63_jack(struct cdc *c)
{
    int st = cdc_rd(c, INT_ST_1), v, in;
    if (st < 0) return -1;
    in = !(st & (c->priv[P_V2] ? 0x2000 : 0x1000));
    if (!in) {
        if (c->priv[P_DET]) {
            if (c->priv[P_V2]) {
                cdc_upd(c, RECMIX, 0x0008, 0);
                if (c->priv[P_DET] != D_HP) rt63_v2_mic(c, 0);
            } else {
                cdc_upd(c, PWR_ANLG_1, 0xA200, 0);
            }
        }
        c->priv[P_DET] = 0;
        return 0;
    }
    if (c->priv[P_DET] == D_HP) return 1;
    if (c->priv[P_DET] == D_HS) return 3;
    if (!c->priv[P_DET]) {
        if (c->priv[P_V2]) {
            cdc_wr(c, CBJ_TYPE_2, 0x8040);
            cdc_wr(c, CBJ_TYPE_3, 0x1484);
            rt63_v2_mic(c, 1);
            cdc_upd(c, RC_CLK, 0x0100, 0x0100);
            cdc_upd(c, RECMIX, 0x0008, 0x0008);
        } else {
            cdc_upd(c, DIG_MISC, 0x0001, 0x0001);
            cdc_upd(c, HP_CHG_PUMP_1, 0x1C00, 0x1000);
            cdc_upd(c, DUMMY_1, 0x03F8, 0x0248);
            cdc_upd(c, CBJ_1, 0x0C00, 0x0C00);
            cdc_upd(c, IL_CMD_2, 0x0001, 0x0001);   /* mic detection */
            cdc_upd(c, PWR_ANLG_2, 0x8000, 0x8000); /* BST1 */
            cdc_upd(c, EM_JACK_TYPE_1, 0x8C00, 0x8C00);
            cdc_upd(c, PWR_ANLG_1, 0x020F, 0x0204);
            cdc_upd(c, PWR_ANLG_1, 0xF000, 0xA000);
            mdelay(20);
            cdc_upd(c, PWR_ANLG_1, 0x5000, 0x5000);
            cdc_upd(c, AUTO_1MRC_CLK, 0x8000, 0x8000);
            cdc_upd(c, IRQ_1, 0x0040, 0x0040);
            cdc_upd(c, EM_JACK_TYPE_1, 0x0080, 0);      /* restart the detection */
            cdc_upd(c, EM_JACK_TYPE_1, 0x0080, 0x0080);
        }
        c->priv[P_DET] = 1;
        return 1;
    }
    if (c->priv[P_V2]) {
        v = cdc_rd(c, CBJ_TYPE_2);
        if (v < 0) return -1;
        v &= 3;
        if (!v && c->priv[P_DET] < 3) {
            c->priv[P_DET]++;
            return 1;
        }
        c->priv[P_DET] = v == 1 || v == 2 ? D_HS : D_HP;
        if (c->priv[P_DET] == D_HP) rt63_v2_mic(c, 0);
    } else {
        v = cdc_rd(c, INT_ST_2);
        if (v < 0) return -1;
        if (!(v & 0x0080) && c->priv[P_DET] < 7) {
            c->priv[P_DET]++;
            return 1;
        }
        v = cdc_rd(c, EM_JACK_TYPE_2);
        if (v < 0) return -1;
        v &= 3;
        cdc_upd(c, HP_CHG_PUMP_1, 0x0C00, 0x0C00);
        c->priv[P_DET] = v == 1 || v == 2 ? D_HS : D_HP;
    }
    return c->priv[P_DET] == D_HS ? 3 : 1;
}

static void rt63_stream(struct cdc *c, int dir, int on)
{
    u32 bit = dir ? 2 : 1, was = c->priv[P_RUN];
    if (on) c->priv[P_RUN] |= bit;
    else c->priv[P_RUN] &= ~bit;
    if (c->priv[P_BCLK] && !was != !c->priv[P_RUN])     /* PLL on BCLK while streams run */
        cdc_upd(c, GLB_CLK, 0xC000, c->priv[P_RUN] ? c->priv[P_SCLK] : 0x8000);
    if (!dir) rt63_dac(c);
}

static void rt63_volume(struct cdc *c, int ctl, u32 lr)
{
    u32 l, r, g;
    if (ctl == MX_MASTER) {
        /* DAC: 0.75 dB steps in bits 7:1, up to 0 dB */
        l = cdc_step(CDC_L(lr), 87) << 1;
        r = cdc_step(CDC_R(lr), 87) << 1;
        cdc_wr(c, DAC_VOL, (l << 8) | r);
        c->priv[P_MUTE] = (CDC_L(lr) ? 0 : 1) | (CDC_R(lr) ? 0 : 2);
        rt63_dac(c);
    } else if (ctl == MX_INGAIN) {
        /* stereo ADC digital gain -17.25 .. +18 dB, mute at 0 */
        g = cdc_step(CDC_L(lr), 47) << 1;
        cdc_wr(c, ADC_VOL, (g << 8) | g | (CDC_L(lr) ? 0 : 0x8080));
    }
}

static void rt63_off(struct cdc *c)
{
    c->priv[P_RUN] = 0;
    rt63_dac(c);
    if (c->priv[P_V2]) {
        cdc_upd(c, HP_AMP_2, 0x4000, 0);
        cdc_upd(c, HP_LOGIC_2, 0x0003, 0x0002);
        rt63_v2_mic(c, 0);
    } else {
        cdc_upd(c, DEPOP_1, 0x3000, 0);
        cdc_upd(c, HP_CHG_PUMP_1, 0x0004, 0x0004);
        cdc_upd(c, DACREF_LDO, 0x3E0E, 0);
        cdc_upd(c, DIG_VOL_ZCD, 0x00C0, 0);
        cdc_upd(c, DEPOP_1, 0x0003, 0);
        cdc_upd(c, DEPOP_1, 0x0030, 0);
    }
    cdc_wr(c, PWR_DIG_1, 0);
    cdc_wr(c, PWR_DIG_2, 0);
    cdc_upd(c, PWR_ANLG_1, 0xF200, 0);
    cdc_upd(c, PWR_ANLG_2, 0xFC03, 0);
    cdc_upd(c, PWR_ANLG_3, 0x0040, 0);
}

const struct cdcdrv cdc_rt5663 = { "rt5663", { "10EC5663", NULL }, CK_HEADSET, 16, 16, 0,
                                   rt63_probe, rt63_init, rt63_stream, rt63_volume, NULL, rt63_jack, rt63_off };
