/* cdcrt56.c - Realtek RT5640 (RT5639, RT5642), RT5645 / RT5650 and RT5651 codecs of the Bay
   Trail / Cherry Trail tablets: speakers on the class-D outputs (RT5651: on the line out behind
   an amplifier switched by the codec's first GpioIo), headphones, internal microphone and
   headset microphone. System clock 512 x fs from the codec PLL fed by MCLK (19.2 MHz on these
   machines) or by BCLK when no MCLK is known. Sequences after the Linux rt5640, rt5645, rt5651
   and rl6231 drivers and the default quirks of the machine drivers bytcr_rt5640 (DMIC1 on
   IN1P, JD2_IN4N inverted), cht_bsw_rt5645 (jack mode 3, internal mic IN2 / RT5650 DMIC2) and
   bytcr_rt5651 (JD1_1, internal mic IN2P, headset mic IN3P). The three chips answer to
   overlapping ACPI ids (10EC5640 is used for all of them): probe sorts them by the id
   register. */
#include "dspcdc.h"

/* registers of the family (same address in all three unless named R40_ / R45_ / R51_) */
#define RESET           0x00
#define SPK_VOL         0x01            /* RT5640, RT5645 */
#define HP_VOL          0x02
#define LOUT_CTL        0x03            /* RT5651 */
#define DAC1_VOL        0x19
#define ADC_VOL         0x1C
#define STO_ADC_MIX     0x27
#define AD_DA_MIX       0x29
#define STO_DAC_MIX     0x2A
#define REC_L2          0x3C
#define REC_R2          0x3E
#define HPO_MIX         0x45
#define PWR_DIG1        0x61
#define PWR_DIG2        0x62
#define PWR_ANLG1       0x63
#define PWR_ANLG2       0x64
#define PWR_MIXER       0x65
#define PWR_VOL         0x66
#define PRIV_INDEX      0x6A
#define PRIV_DATA       0x6C
#define I2S1_SDP        0x70
#define ADDA_CLK1       0x73
#define DMIC_CTL        0x75
#define GLB_CLK         0x80
#define PLL_CTRL1       0x81
#define PLL_CTRL2       0x82
#define ASRC_1          0x83
#define ASRC_2          0x84
#define DEPOP_M1        0x8E
#define DEPOP_M2        0x8F
#define DEPOP_M3        0x90
#define CHARGE_PUMP     0x91
#define MICBIAS         0x93
#define INT_IRQ_ST      0xBF
#define GPIO_CTRL1      0xC0
#define DEVICE_ID       0xFF

#define R40_IN3_IN4     0x0E
#define R40_DSP_PATH2   0x2E
#define R40_SPK_L_MIX   0x46
#define R40_SPK_R_MIX   0x47
#define R40_SPO_L_MIX   0x48
#define R40_SPO_R_MIX   0x49
#define R40_OUT_L3      0x4F
#define R40_OUT_R3      0x52
#define R40_JD_CTRL     0xBB
#define R40_IRQ_CTRL2   0xBE
#define R40_GPIO_CTRL3  0xC2
#define R40_GCTL1       0xFA
#define R40_GCTL2       0xFB

#define R45_IN1_CTRL1   0x0A
#define R45_IN1_CTRL2   0x0B
#define R45_IN1_CTRL3   0x0C
#define R45_IN2_CTRL    0x0D
#define R45_HPOMIXL     0x40
#define R45_HPOMIXR     0x42
#define R45_SPK_L_MIX   0x46
#define R45_SPK_R_MIX   0x47
#define R45_SPO_MIX     0x48
#define R45_TDM_CTRL1   0x77
#define R45_ASRC_4      0x8A
#define R45_A_JD_CTRL1  0x94
#define R45_CLSD_OUT    0xA0
#define R45_CLSD_OUT1   0xA1
#define R45_IRQ_CTRL2   0xBD
#define R45_BASS_BACK   0xCF
#define R45_IL_CMD3     0xDD
#define R45_JD_CTRL3    0xF8
#define R45_GEN_CTRL1   0xFA
#define R45_GEN_CTRL2   0xFB
#define R45_GEN_CTRL3   0xFC

#define R51_IN1_IN2     0x0D
#define R51_IN3         0x0E
#define R51_OUT_L3      0x4F
#define R51_OUT_R3      0x52
#define R51_LOUT_MIX    0x53
#define R51_PLL_MODE2   0x84
#define R51_JD_CTRL2    0xBC
#define R51_IRQ_CTRL1   0xBD
#define R51_IRQ_CTRL2   0xBE
#define R51_D_MISC      0xFA

/* private (index / data) registers */
#define PR_BIAS_CUR4    0x15
#define PR_CHPUMP_INT1  0x24
#define PR_MAMP_INT2    0x37
#define PR_HP_DCC_INT1  0x77

/* chips */
#define T_5640          1
#define T_5645          2
#define T_5650          3
#define T_5651          4

/* c->priv */
#define P_TYPE          0
#define P_DET           1               /* jack detection state, D_* or a step count */
#define P_CNT           2               /* over-current samples: count | last << 8 */
#define P_JACK          3               /* jack bits last applied to the outputs */
#define P_RUN           4               /* bit 0 playback, bit 1 capture running */
#define P_BCLK          5               /* PLL runs from BCLK: RC clock while idle */
#define P_MUTE          6               /* bit 0 left, bit 1 right at volume 0 */
#define D_HP            0x40            /* headphones (no microphone) */
#define D_HS            0x41            /* headset */

extern const struct cdcdrv cdc_rt5640, cdc_rt5645, cdc_rt5651;

/* ---------------------------------------------------------------- helpers */

static void pr_wr(struct cdc *c, u32 idx, u32 v)
{
    cdc_wr(c, PRIV_INDEX, idx);
    cdc_wr(c, PRIV_DATA, v);
}

static int pr_rd(struct cdc *c, u32 idx)
{
    cdc_wr(c, PRIV_INDEX, idx);
    return cdc_rd(c, PRIV_DATA);
}

static void pr_upd(struct cdc *c, u32 idx, u32 mask, u32 v)
{
    int o = pr_rd(c, idx);
    if (o >= 0) pr_wr(c, idx, ((u32)o & ~mask) | v);
}

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
   bypassed; gives the PLL_CTRL1 / PLL_CTRL2 values */
static int rl6231_pll(u32 fin, u32 fout, u16 *ctrl1, u16 *ctrl2)
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
    /* common divider that keeps fin * (N + 2) in 32 bits */
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
    if (k < 0) k = 0;                           /* K bypass: not used by these codecs */
    *ctrl1 = (u16)(((u32)n << 7) | (u32)k);
    *ctrl2 = (u16)(((u32)(mbp ? 0 : m) << 12) | ((u32)mbp << 11));
    return 0;
}

/* system clock: PLL to 512 x fs from MCLK (the machine drivers' setting) or else BCLK;
   with BCLK the codec runs from its RC clock until a stream starts */
static int rt_clock(struct cdc *c, u32 srcmask, u32 srcbclk)
{
    u32 fin = c->ssp.mclk;
    u16 c1, c2;
    int bclk = 0;
    if (!fin && !c->ssp.provider) {
        fin = c->ssp.bclk;
        bclk = 1;
    }
    if (!fin || rl6231_pll(fin, 512UL * CDC_RATE, &c1, &c2)) {
        kprintf("%s: no PLL setting for %lu Hz %s\n", c->drv->name, fin, bclk ? "BCLK" : "MCLK");
        return -1;
    }
    cdc_upd(c, GLB_CLK, srcmask, bclk ? srcbclk : 0);
    cdc_wr(c, PLL_CTRL1, c1);
    cdc_wr(c, PLL_CTRL2, c2);
    cdc_upd(c, PWR_ANLG2, 0x0200, 0x0200);
    cdc_upd(c, GLB_CLK, 0xC000, bclk ? 0x8000 : 0x4000);
    c->priv[P_BCLK] = (u32)bclk;
    /* I2S pre-divider: 24.576 MHz / 2 = 256 x 48 kHz */
    cdc_upd(c, ADDA_CLK1, 0x7000, 0x1000);
    DBG("%s: PLL from %s %lu Hz, %04x %04x\n", c->drv->name, bclk ? "BCLK" : "MCLK", fin, c1, c2);
    return 0;
}

/* I2S1 interface: consumer (or provider), format, data length */
static void rt_fmt(struct cdc *c, int dlsft, u32 bpbit)
{
    u32 v = c->ssp.provider ? 0 : 0x8000;
    switch (c->ssp.fmt) {
    case 3: v |= 1; break;                      /* left justified */
    case 4: v |= 2; break;                      /* DSP A */
    case 5: v |= 3; break;                      /* DSP B */
    default: break;                             /* I2S */
    }
    switch (c->ssp.vbits) {
    case 16: break;
    case 20: v |= 1UL << dlsft; break;
    default: v |= 2UL << dlsft; break;          /* 24 bit */
    }
    cdc_upd(c, I2S1_SDP, 0x8003 | bpbit | (3UL << dlsft), v);
}

/* references and bias (bias level standby) */
static void rt_bias(struct cdc *c)
{
    cdc_upd(c, PWR_ANLG1, 0xA810, 0xA810);     /* VREF1, MBIAS, bandgap, VREF2 */
    mdelay(10);
    cdc_upd(c, PWR_ANLG1, 0x4008, 0x4008);     /* fast VREF1 / VREF2 */
}

/* RT5640 / RT5651 headphone amplifier: charge pump, depop, amplifier on, unmute sequence */
static void rt_hpamp(struct cdc *c)
{
    pr_upd(c, PR_CHPUMP_INT1, 0x0700, 0x0200);
    cdc_upd(c, DEPOP_M2, 0x2000, 0x2000);       /* manual depop */
    cdc_upd(c, DEPOP_M1, 0x000D, 0x0009);       /* charge pump on, signal gen off, bias on */
    pr_wr(c, PR_HP_DCC_INT1, 0x9F00);
    cdc_upd(c, PWR_ANLG1, 0x4008, 0);
    cdc_upd(c, PWR_ANLG1, 0x0020, 0x0020);      /* HP amplifier */
    mdelay(10);
    cdc_upd(c, PWR_ANLG1, 0x4008, 0x4008);
    cdc_upd(c, PWR_ANLG1, 0x00C0, 0x00C0);      /* HP L / R */
    cdc_upd(c, DEPOP_M2, 0x2040, 0x0040);       /* auto depop, digital depop */
    cdc_upd(c, CHARGE_PUMP, 0x0300, 0x0200);    /* high voltage */
    cdc_upd(c, DEPOP_M3, 0x0777, 0x0737);       /* charge pump 192 / 12 / 192 kHz */
    pr_wr(c, PR_MAMP_INT2, 0x1C00);
    cdc_upd(c, DEPOP_M1, 0x000C, 0x0004);
    pr_upd(c, PR_CHPUMP_INT1, 0x0700, 0x0400);
    mdelay(80);
}

/* RT5645 / RT5650 headphone amplifier on and unmute sequence (hp_amp_power, hp_event) */
static void rt45_hpamp(struct cdc *c, int t50)
{
    int i, v;
    if (t50) {
        cdc_wr(c, DEPOP_M2, 0x3100);
        cdc_wr(c, CHARGE_PUMP, 0x0E06);
        cdc_wr(c, DEPOP_M1, 0x000D);
        pr_wr(c, PR_HP_DCC_INT1, 0x9F01);
        for (i = 0; i < 20; i++) {              /* DC calibration */
            mdelay(1);
            v = pr_rd(c, PR_HP_DCC_INT1);
            if (v < 0 || !(v & 0x8000)) break;
        }
        cdc_upd(c, DEPOP_M1, 0x0010, 0x0010);
        pr_wr(c, 0x3E, 0x7400);
        cdc_wr(c, DEPOP_M3, 0x0737);
        pr_wr(c, PR_MAMP_INT2, 0xFC00);
        cdc_wr(c, DEPOP_M2, 0x1140);
        cdc_upd(c, PWR_ANLG1, 0x00C0, 0x00C0);
        mdelay(90);
        return;
    }
    cdc_upd(c, DEPOP_M2, 0x2000, 0x2000);
    cdc_wr(c, DEPOP_M1, 0x000D);
    pr_wr(c, PR_HP_DCC_INT1, 0x9F01);
    mdelay(150);
    cdc_upd(c, PWR_ANLG1, 0x4008, 0);
    cdc_upd(c, PWR_VOL, 0x0C00, 0x0C00);
    cdc_upd(c, PWR_ANLG1, 0x00E0, 0x00E0);      /* HP L / R, amplifier */
    mdelay(5);
    cdc_upd(c, PWR_ANLG1, 0x4008, 0x4008);
    cdc_upd(c, DEPOP_M1, 0x0014, 0x0014);
    pr_wr(c, 0x14, 0x1AAA);
    pr_wr(c, PR_CHPUMP_INT1, 0x0430);
    cdc_upd(c, DEPOP_M3, 0x0777, 0x0737);
    pr_wr(c, PR_MAMP_INT2, 0xFC00);
    cdc_upd(c, DEPOP_M1, 0x8000, 0x8000);       /* smart trigger */
    cdc_upd(c, DEPOP_M1, 0x0040, 0x0040);
    cdc_upd(c, DEPOP_M1, 0x0340, 0x0300);
    mdelay(40);
    cdc_upd(c, DEPOP_M1, 0x0304, 0x0000);
}

/* DAC input: muted while no playback runs and per channel at volume 0 */
static void rt_dac(struct cdc *c)
{
    u32 v = 0x8080, idle = !(c->priv[P_RUN] & 1);
    if (idle || (c->priv[P_MUTE] & 1)) v |= 0x4000;
    if (idle || (c->priv[P_MUTE] & 2)) v |= 0x0040;
    cdc_wr(c, AD_DA_MIX, v);
}

/* outputs for the jack state: speakers while no headphones are plugged */
static void rt_outs(struct cdc *c)
{
    int hp = (int)(c->priv[P_JACK] & 1);
    cdc_wr(c, HP_VOL, hp ? 0x0808 : 0xC8C8);
    if (c->priv[P_TYPE] == T_5651) {
        /* line out (DAC direct, OUTVOL off) and the speaker amplifier */
        cdc_wr(c, LOUT_CTL, hp ? 0xC8C8 : 0x4848);
        cdc_gpio(c, 0, !hp && (c->priv[P_RUN] & 1));
    } else {
        cdc_wr(c, SPK_VOL, hp ? 0xC8C8 : 0x0808);
    }
}

/* input: internal or headset microphone */
static void rt_mic(struct cdc *c, int hs)
{
    switch (c->priv[P_TYPE]) {
    case T_5640:                                /* DMIC1 by ADC2, or IN2P by ADC1 */
        cdc_wr(c, STO_ADC_MIX, hs ? 0x3020 : 0x5040);
        break;
    case T_5650:                                /* DMIC2 by ADC2, or IN1 (BST1) by ADC1 */
        if (!hs) {
            cdc_wr(c, STO_ADC_MIX, 0x5940);
            break;
        }
        cdc_wr(c, REC_L2, 0x007D);
        cdc_wr(c, REC_R2, 0x007D);
        cdc_wr(c, STO_ADC_MIX, 0x3020);
        break;
    case T_5645:                                /* IN2 (BST2) or IN1 (BST1) */
        cdc_wr(c, REC_L2, hs ? 0x007D : 0x007B);
        cdc_wr(c, REC_R2, hs ? 0x007D : 0x007B);
        cdc_wr(c, STO_ADC_MIX, 0x3020);
        break;
    default:                                    /* RT5651: IN2P (BST2) or IN3P (BST3) */
        cdc_wr(c, REC_L2, hs ? 0x0067 : 0x006B);
        cdc_wr(c, REC_R2, hs ? 0x0067 : 0x006B);
        break;
    }
}

/* ---------------------------------------------------------------- probe, init */

static int rt_probe(struct cdc *c)
{
    int id = cdc_rd(c, DEVICE_ID);
    switch (id) {
    case 0x6231:
        c->priv[P_TYPE] = T_5640;
        c->drv = &cdc_rt5640;
        break;
    case 0x6308:
    case 0x6419:
        c->priv[P_TYPE] = id == 0x6419 ? T_5650 : T_5645;
        c->drv = &cdc_rt5645;
        break;
    case 0x6281:
        c->priv[P_TYPE] = T_5651;
        c->drv = &cdc_rt5651;
        break;
    default:
        return -1;
    }
    kprintf("%s: RT%s, id %04x\n", c->drv->name, id == 0x6231 ? "5640" : id == 0x6308 ? "5645" :
            id == 0x6419 ? "5650" : "5651", (u16)id);
    return 0;
}

static int rt40_init(struct cdc *c)
{
    cdc_wr(c, RESET, 0);
    mdelay(10);
    pr_wr(c, 0x3D, 0x3600);
    pr_wr(c, 0x12, 0x0AA8);
    pr_wr(c, 0x14, 0x0AAA);
    pr_wr(c, 0x21, 0xE0E0);
    pr_wr(c, 0x23, 0x1804);
    cdc_upd(c, R40_GCTL1, 0x0B01, 0x0B01);      /* MCLK detection */
    cdc_upd(c, MICBIAS, 0x0030, 0x0030);
    cdc_upd(c, R40_DSP_PATH2, 0xFC00, 0x0C00);
    cdc_upd(c, GPIO_CTRL1, 0x4000, 0x4000);     /* GPIO2 = DMIC1 clock (data on IN1P) */
    /* ASRC for the DAC and ADC filters (machine driver) */
    cdc_upd(c, ASRC_2, 0xF820, 0xF820);
    /* jack detection: JD2 on IN4N, inverted; GPIO1 = IRQ; micbias1 over-current 2 mA x 0.75 */
    cdc_upd(c, R40_JD_CTRL, 0xE003, 0x6003);
    cdc_upd(c, GPIO_CTRL1, 0x8000, 0x8000);
    cdc_upd(c, R40_GPIO_CTRL3, 0x0004, 0x0004);
    cdc_wr(c, R40_GCTL1, 0x3F41);
    pr_wr(c, PR_BIAS_CUR4, 0xA900);
    cdc_upd(c, MICBIAS, 0x0E00, 0x0C00);
    cdc_upd(c, R40_IRQ_CTRL2, 0x0800, 0x0800);  /* sticky over-current status */
    cdc_upd(c, R40_GCTL2, 0x1100, 0x1100);
    rt_bias(c);
    if (rt_clock(c, 0x3000, 0x1000)) return -1;
    rt_fmt(c, 2, 0x0080);
    /* SSP on the AIF2 pins (Bay Trail CR): interfaces swapped ("DAI select" 1:2|2:1), the IF1
       data paths and the I2S1 settings then serve AIF2 */
    if (c->aif == 2) cdc_upd(c, I2S1_SDP, 0x7000, 0x5000);
    if ((u32)c->ssp.slots * c->ssp.width > 32) cdc_upd(c, ADDA_CLK1, 0x8000, 0x8000);
    cdc_upd(c, ASRC_1, 0x8200, 0x8200);         /* ASRC: stereo filter, DMIC1 */
    /* playback: IF1 -> DAC1 -> SPK mixers -> SPKVOL -> SPO mixers -> class D;
       DAC1 -> OUT mixers -> HPOVOL -> HPO mixer -> HP amplifier */
    rt_dac(c);
    cdc_wr(c, STO_DAC_MIX, 0x1414);
    cdc_wr(c, R40_SPK_L_MIX, 0x0036);
    cdc_wr(c, R40_SPK_R_MIX, 0x0036);
    cdc_wr(c, R40_SPO_L_MIX, 0xE800);
    cdc_wr(c, R40_SPO_R_MIX, 0x2800);
    cdc_wr(c, R40_OUT_L3, 0x01FE);
    cdc_wr(c, R40_OUT_R3, 0x01FE);
    cdc_wr(c, HPO_MIX, 0xC000);
    cdc_wr(c, SPK_VOL, 0xC8C8);
    cdc_wr(c, HP_VOL, 0xC8C8);
    cdc_wr(c, DAC1_VOL, 0xAFAF);
    cdc_upd(c, PWR_DIG1, 0x9807, 0x9807);       /* I2S1, DAC1 L/R, ADC L/R, class D */
    cdc_upd(c, PWR_DIG2, 0x8000, 0x8000);       /* stereo ADC filter */
    cdc_upd(c, PWR_MIXER, 0xFC00, 0xFC00);      /* OUT, SPK, REC mixers */
    cdc_upd(c, PWR_VOL, 0xCC00, 0xCC00);        /* SPKVOL, HPOVOL */
    rt_hpamp(c);
    /* capture: DMIC1 (clock 12.288 MHz / 8); headset microphone IN2P -> BST2 -> REC mixers */
    cdc_wr(c, DMIC_CTL, 0x9D80);
    cdc_wr(c, REC_L2, 0x006F);
    cdc_wr(c, REC_R2, 0x006F);
    cdc_upd(c, R40_IN3_IN4, 0x0F00, 0x0200);    /* IN2 boost +24 dB */
    cdc_upd(c, PWR_ANLG1, 0x0004, 0x0004);      /* LDO2 */
    cdc_upd(c, PWR_ANLG2, 0x1000, 0x1000);      /* BST2 */
    cdc_wr(c, ADC_VOL, 0x2F2F);
    return 0;
}

static void rt45_tdm(struct cdc *c, int t50)
{
    u32 v, sl, w;
    sl = c->ssp.slots >= 8 ? 3 : c->ssp.slots >= 6 ? 2 : 1;
    w = c->ssp.width >= 32 ? 3 : c->ssp.width >= 24 ? 2 : c->ssp.width >= 20 ? 1 : 0;
    if (t50) {
        v = 0x8000 | (sl << 10) | (sl << 8) | (w << 6) | (w << 4);
        cdc_upd(c, R45_TDM_CTRL1, 0x8FF0, v);
    } else {
        cdc_upd(c, R45_BASS_BACK, 0x003F, 0x0014);
        v = 0x4000 | (sl << 12) | (w << 10);
        cdc_upd(c, R45_TDM_CTRL1, 0x7C00, v);
    }
}

static int rt45_init(struct cdc *c)
{
    int t50 = c->priv[P_TYPE] == T_5650;
    cdc_wr(c, RESET, 0);
    mdelay(10);
    cdc_wr(c, AD_DA_MIX, 0x8080);
    pr_wr(c, 0x3D, 0x3600);
    pr_wr(c, 0x1C, 0xFD70);
    pr_wr(c, 0x20, 0x611F);
    pr_wr(c, 0x21, 0x4040);
    pr_wr(c, 0x23, 0x0004);
    cdc_wr(c, R45_ASRC_4, 0x0120);
    if (t50) {
        cdc_wr(c, 0xF6, 0x0100);
        cdc_wr(c, PWR_ANLG1, 0x0002);
        cdc_wr(c, R45_IL_CMD3, 0x6728);
        pr_wr(c, 0x3A, 0x0000);
        cdc_wr(c, R45_CLSD_OUT1, 0x4059);
        cdc_wr(c, R45_GEN_CTRL3, 0x0200);
        cdc_upd(c, GPIO_CTRL1, 0x4000, 0x4000); /* GPIO2 = DMIC clock, DMIC2 data on IN2P */
        cdc_upd(c, DMIC_CTL, 0x0C00, 0x0C00);
    }
    cdc_upd(c, R45_CLSD_OUT, 0x00C0, 0x00C0);
    /* jack detection mode 3: JD1_1, IRQ logic on the internal clock, GPIO1 = IRQ */
    cdc_upd(c, R45_GEN_CTRL3, 0x1800, 0x1800);
    cdc_upd(c, MICBIAS, 0x0108, 0x0108);
    cdc_upd(c, R45_IRQ_CTRL2, 0x0200, 0x0200);
    cdc_upd(c, HPO_MIX, 0x1000, 0x1000);
    cdc_upd(c, GPIO_CTRL1, 0x8000, 0x8000);
    cdc_upd(c, R45_A_JD_CTRL1, 0x0003, 0x0002);
    cdc_upd(c, PWR_ANLG2, 0x0004, 0x0004);      /* JD power */
    cdc_upd(c, PWR_MIXER, 0x0002, 0x0002);      /* LDO2 */
    /* ASRC (I2S1 clock) for the stereo / mono DAC filters and the stereo ADC filter */
    cdc_upd(c, ASRC_2, 0xFFF0 | 0x000F, 0x1111);
    rt_bias(c);
    cdc_wr(c, DEPOP_M2, 0x1140);
    mdelay(40);
    cdc_upd(c, R45_GEN_CTRL1, 0x0001, 0x0001);  /* digital gate */
    if (rt_clock(c, 0x3800, 0x0800)) return -1;
    rt_fmt(c, t50 ? 4 : 2, t50 ? 0x0100 : 0x0080);
    if (c->ssp.slots > 2) rt45_tdm(c, t50);
    cdc_upd(c, ASRC_1, 0x0C88, 0x0C88);         /* ASRC: I2S1, DAC, DMIC, ADC */
    /* playback: IF1 -> DAC1 -> SPK mixers -> SPKVOL -> SPO mixer -> class D;
       DAC1 -> HPOVOL mixers -> HPO mixer -> HP amplifier */
    rt_dac(c);
    cdc_wr(c, STO_DAC_MIX, 0x1616);
    cdc_wr(c, R45_SPK_L_MIX, 0x003C);
    cdc_wr(c, R45_SPK_R_MIX, 0x003C);
    cdc_wr(c, R45_SPO_MIX, 0xD806);
    cdc_wr(c, R45_HPOMIXL, 0x001E);
    cdc_wr(c, R45_HPOMIXR, 0x001E);
    cdc_upd(c, HPO_MIX, 0x6000, 0x4000);
    cdc_wr(c, SPK_VOL, 0xC8C8);
    cdc_wr(c, HP_VOL, 0xC8C8);
    cdc_wr(c, DAC1_VOL, 0xAFAF);
    cdc_upd(c, PWR_DIG1, 0x9806, 0x9806);       /* I2S1, DAC1 L/R, ADC L/R */
    cdc_upd(c, PWR_DIG2, 0x8800, 0x8800);       /* stereo ADC / DAC filters */
    cdc_upd(c, PWR_MIXER, 0x3CC0, 0x3CC0);      /* SPK, REC, HPOVOL mixers */
    cdc_upd(c, PWR_VOL, 0xCC20, 0xCC20);        /* SPKVOL, HPOVOL, mic detection */
    rt45_hpamp(c, t50);
    cdc_upd(c, PWR_DIG1, 0x0301, 0x0301);       /* class D */
    /* capture: headset microphone IN1 -> BST1; RT5645 internal mic IN2 -> BST2 with
       micbias1 / 2; RT5650 internal mic DMIC2 (clock 12.288 MHz / 8) */
    cdc_upd(c, R45_IN1_CTRL1, 0xF000, 0x2000);  /* IN1 boost +24 dB */
    cdc_upd(c, PWR_ANLG2, 0x8000, 0x8000);      /* BST1 */
    if (t50) {
        cdc_upd(c, DMIC_CTL, 0x40E0, 0x4080);
    } else {
        cdc_upd(c, R45_GEN_CTRL2, 0x0030, 0x0030);
        cdc_upd(c, PWR_ANLG2, 0x0C00, 0x0C00);  /* micbias1, micbias2 */
        cdc_upd(c, R45_IN2_CTRL, 0x0F00, 0x0200);   /* IN2 boost +24 dB */
        cdc_upd(c, PWR_ANLG2, 0x4000, 0x4000);  /* BST2 */
        cdc_upd(c, PWR_ANLG2, 0x0020, 0x0020);
    }
    cdc_wr(c, ADC_VOL, 0x2F2F);
    return 0;
}

static int rt51_init(struct cdc *c)
{
    if (c->aif == 2) kprintf("rt5651: the SSP is on AIF2, which this driver does not route; expect silence\n");
    cdc_wr(c, RESET, 0);
    mdelay(10);
    pr_wr(c, 0x3D, 0x3E00);
    cdc_upd(c, PWR_ANLG1, 0x0003, 0x0002);      /* LDO 1.2 V */
    /* jack detection: JD1_1 active low, GPIO1 = IRQ; micbias1 over-current 2 mA x 0.75 */
    cdc_upd(c, R51_JD_CTRL2, 0x0E00, 0x0200);
    cdc_upd(c, R51_IRQ_CTRL1, 0x0280, 0x0200);
    cdc_upd(c, GPIO_CTRL1, 0x8000, 0x8000);
    cdc_upd(c, PWR_ANLG2, 0x0004, 0x0004);
    pr_wr(c, PR_BIAS_CUR4, 0xA900);
    cdc_upd(c, MICBIAS, 0x0E30, 0x0C30);
    cdc_upd(c, R51_IRQ_CTRL2, 0x0800, 0x0800);  /* sticky over-current status */
    rt_bias(c);
    cdc_upd(c, R51_D_MISC, 0x0001, 0x0001);
    if (rt_clock(c, 0x3000, 0x1000)) return -1;
    rt_fmt(c, 2, 0x0080);
    cdc_upd(c, R51_PLL_MODE2, 0xB800, 0xB800);  /* ASRC clocks: I2S1, DAC 1/2, ADC */
    /* playback: IF1 -> DAC1 -> LOUT mixer -> line out (speaker amplifier);
       DAC1 -> OUT mixers -> HPOVOL -> HPO mixer -> HP amplifier */
    rt_dac(c);
    cdc_wr(c, STO_DAC_MIX, 0x1212);
    cdc_wr(c, R51_OUT_L3, 0x0278);
    cdc_wr(c, R51_OUT_R3, 0x0278);
    cdc_wr(c, HPO_MIX, 0x4000);
    cdc_wr(c, R51_LOUT_MIX, 0x3000);
    cdc_wr(c, HP_VOL, 0xC8C8);
    cdc_wr(c, LOUT_CTL, 0xC8C8);
    cdc_wr(c, DAC1_VOL, 0xAFAF);
    cdc_upd(c, PWR_DIG1, 0x9806, 0x9806);       /* I2S1, DAC1 L/R, ADC L/R */
    cdc_upd(c, PWR_DIG2, 0x8C00, 0x8C00);       /* ADC filter, DAC filters 1 / 2 */
    cdc_upd(c, PWR_MIXER, 0xCC00, 0xCC00);      /* OUT, REC mixers */
    cdc_upd(c, PWR_VOL, 0x0C00, 0x0C00);        /* HPOVOL */
    cdc_upd(c, PWR_ANLG1, 0x1000, 0x1000);      /* LOUT mixer */
    rt_hpamp(c);
    /* capture: internal mic IN2P -> BST2, headset mic IN3P -> BST3, micbias1 */
    cdc_upd(c, PWR_ANLG1, 0x0004, 0x0004);      /* LDO */
    cdc_upd(c, PWR_ANLG2, 0x6800, 0x6800);      /* micbias1, BST2, BST3 */
    cdc_upd(c, PWR_ANLG2, 0x0018, 0x0018);
    cdc_upd(c, R51_IN1_IN2, 0x0F00, 0x0200);    /* IN2 boost +24 dB */
    cdc_upd(c, R51_IN3, 0xF000, 0x2000);        /* IN3 boost +24 dB */
    cdc_wr(c, STO_ADC_MIX, 0x3820);
    cdc_wr(c, ADC_VOL, 0x2F2F);
    return 0;
}

static int rt_init(struct cdc *c)
{
    int r;
    c->priv[P_DET] = c->priv[P_CNT] = c->priv[P_RUN] = c->priv[P_MUTE] = 0;
    switch (c->priv[P_TYPE]) {
    case T_5640: r = rt40_init(c); break;
    case T_5645:
    case T_5650: r = rt45_init(c); break;
    case T_5651: r = rt51_init(c); break;
    default: return -1;
    }
    if (r) return r;
    c->priv[P_JACK] = 0;
    rt_outs(c);
    rt_mic(c, 0);
    return 0;
}

/* ---------------------------------------------------------------- jack detection */

/* RT5640 / RT5651: a 3-pole plug shorts the microphone contact, the micbias1 over-current
   detector (sticky status) trips; three equal samples in a row decide */
static int rt_ovcd(struct cdc *c, int in)
{
    int st, oc, t40 = c->priv[P_TYPE] == T_5640;
    if (!in) {
        if (c->priv[P_DET] && t40) cdc_upd(c, PWR_ANLG2, 0x0800, 0);
        c->priv[P_DET] = 0;
        return 0;
    }
    if (c->priv[P_DET] == D_HP) return 1;
    if (c->priv[P_DET] == D_HS) return 3;
    if (!c->priv[P_DET]) {
        cdc_upd(c, PWR_ANLG2, 0x0800, 0x0800);      /* micbias1 (RT5651: always on) */
        cdc_upd(c, R40_IRQ_CTRL2, 0x0008, 0);       /* same register on the RT5651 */
        c->priv[P_DET] = 1;
        c->priv[P_CNT] = 0;
        return 1;
    }
    st = cdc_rd(c, R40_IRQ_CTRL2);
    if (st < 0) return -1;
    cdc_upd(c, R40_IRQ_CTRL2, 0x0008, 0);
    oc = (st & 0x0008) ? 1 : 0;
    if ((c->priv[P_CNT] & 0xFF) && (int)(c->priv[P_CNT] >> 8) == oc) c->priv[P_CNT]++;
    else c->priv[P_CNT] = ((u32)oc << 8) | 1;
    c->priv[P_DET]++;
    if ((c->priv[P_CNT] & 0xFF) >= 3 || c->priv[P_DET] > 12) {
        if (!oc && (c->priv[P_CNT] & 0xFF) >= 3) {
            c->priv[P_DET] = D_HS;
            return 3;
        }
        c->priv[P_DET] = D_HP;
        if (t40) cdc_upd(c, PWR_ANLG2, 0x0800, 0);
    }
    return 1;
}

/* RT5645 / RT5650: combo jack type detection (100 ms with manual JD, then 600 ms) */
static int rt45_cbj(struct cdc *c, int in)
{
    int v;
    if (!in) {
        if (c->priv[P_DET]) {
            cdc_upd(c, R45_IN1_CTRL2, 0x1000, 0x1000);
            cdc_upd(c, R45_IN1_CTRL1, 0x0004, 0);
            cdc_upd(c, INT_IRQ_ST, 0x0001, 0);
        }
        c->priv[P_DET] = 0;
        return 0;
    }
    switch (c->priv[P_DET]) {
    case 0:
        cdc_wr(c, CHARGE_PUMP, 0x0206);
        cdc_wr(c, R45_JD_CTRL3, 0x00F0);
        cdc_upd(c, R45_IN1_CTRL2, 0x1000, 0x1000);
        cdc_upd(c, R45_IN1_CTRL1, 0x0004, 0x0004);
        c->priv[P_DET] = 1;
        return 1;
    case 1:
        cdc_upd(c, R45_IN1_CTRL2, 0x1000, 0);
        c->priv[P_DET] = 2;
        return 1;
    case 2:
        c->priv[P_DET] = 3;
        return 1;
    case 3:
        v = cdc_rd(c, R45_IN1_CTRL3);
        if (v < 0) return -1;
        cdc_wr(c, CHARGE_PUMP, 0x0E06);
        v &= 7;
        /* the ALC3270 package has no headset microphone pin */
        c->priv[P_DET] = (v == 1 || v == 2) && !(c->ad && !strncmp(c->ad->hid, "10EC3270", 9)) ? D_HS : D_HP;
        break;
    default:
        break;
    }
    return c->priv[P_DET] == D_HS ? 3 : 1;
}

static int rt_jack(struct cdc *c)
{
    int st = cdc_rd(c, INT_IRQ_ST), j;
    if (st < 0) return -1;
    switch (c->priv[P_TYPE]) {
    case T_5640: j = rt_ovcd(c, !(st & 0x0010)); break;
    case T_5651: j = rt_ovcd(c, !(st & 0x1000)); break;
    default: j = rt45_cbj(c, !(st & 0x1000)); break;
    }
    if (j >= 0 && (u32)j != c->priv[P_JACK]) {
        c->priv[P_JACK] = (u32)j;
        rt_outs(c);
        rt_mic(c, j & 2);
    }
    return j;
}

/* ---------------------------------------------------------------- stream, volume, off */

static void rt_stream(struct cdc *c, int dir, int on)
{
    u32 bit = dir ? 2 : 1, was = c->priv[P_RUN];
    if (on) c->priv[P_RUN] |= bit;
    else c->priv[P_RUN] &= ~bit;
    if (c->priv[P_BCLK] && !was != !c->priv[P_RUN])     /* PLL on BCLK while streams run */
        cdc_upd(c, GLB_CLK, 0xC000, c->priv[P_RUN] ? 0x4000 : 0x8000);
    if (!dir) {
        rt_dac(c);
        if (c->priv[P_TYPE] == T_5651) rt_outs(c);
    }
}

static void rt_volume(struct cdc *c, int ctl, u32 lr)
{
    int t45 = c->priv[P_TYPE] == T_5645 || c->priv[P_TYPE] == T_5650;
    u32 l, r, g;
    if (ctl == MX_MASTER) {
        /* DAC1: 0.375 dB steps up to 0 dB (RT5645: 0.75 dB in bits 7:1) */
        if (t45) {
            l = cdc_vstep(CDC_L(lr), 87, 750) << 1;
            r = cdc_vstep(CDC_R(lr), 87, 750) << 1;
        } else {
            l = cdc_vstep(CDC_L(lr), 175, 375);
            r = cdc_vstep(CDC_R(lr), 175, 375);
        }
        cdc_wr(c, DAC1_VOL, (l << 8) | r);
        c->priv[P_MUTE] = (CDC_L(lr) ? 0 : 1) | (CDC_R(lr) ? 0 : 2);
        rt_dac(c);
    } else if (ctl == MX_INGAIN) {
        /* stereo ADC digital gain -17.625 .. +18 dB, mute at 0 */
        g = t45 ? cdc_step(CDC_L(lr), 47) << 1 : cdc_step(CDC_L(lr), 95);
        cdc_wr(c, ADC_VOL, (g << 8) | g | (CDC_L(lr) ? 0 : 0x8080));
    }
}

static void rt_off(struct cdc *c)
{
    c->priv[P_RUN] = 0;
    rt_dac(c);
    cdc_wr(c, HP_VOL, 0xC8C8);
    if (c->priv[P_TYPE] == T_5651) {
        cdc_gpio(c, 0, 0);
        cdc_wr(c, LOUT_CTL, 0xC8C8);
    } else {
        cdc_wr(c, SPK_VOL, 0xC8C8);
    }
    mdelay(10);
    cdc_wr(c, DEPOP_M1, c->priv[P_TYPE] == T_5640 || c->priv[P_TYPE] == T_5651 ? 0x0004 : 0x0000);
    cdc_wr(c, DEPOP_M2, 0x1100);
    cdc_wr(c, PWR_DIG1, 0);
    cdc_wr(c, PWR_DIG2, 0);
    cdc_wr(c, PWR_VOL, 0);
    cdc_wr(c, PWR_MIXER, 0);
    cdc_upd(c, PWR_ANLG1, 0xFFFC, 0);            /* keep the LDO voltage bits */
    cdc_wr(c, PWR_ANLG2, 0);
}

/* 10EC5640 stands for all three chips (probe sorts them by id) */
const struct cdcdrv cdc_rt5640 = { "rt5640", { "10EC5640", "10EC5642", "10EC3276", "INTCCFFD", "INT33CA", NULL },
                                   CK_SPKCODEC, 8, 16, 0,
                                   rt_probe, rt_init, rt_stream, rt_volume, NULL, rt_jack, rt_off };
const struct cdcdrv cdc_rt5645 = { "rt5645", { "10EC5645", "10EC5648", "10EC5650", "10EC3270", NULL },
                                   CK_SPKCODEC, 8, 16, 0,
                                   rt_probe, rt_init, rt_stream, rt_volume, NULL, rt_jack, rt_off };
const struct cdcdrv cdc_rt5651 = { "rt5651", { "10EC5651", NULL }, CK_SPKCODEC, 8, 16, 0,
                                   rt_probe, rt_init, rt_stream, rt_volume, NULL, rt_jack, rt_off };
