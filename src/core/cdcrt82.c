/* cdcrt82.c - Realtek ALC5682 headset codecs on the SSP ports: RT5682 (ALC5682I-VD/VE,
   device id 0x6530) and RT5682S (ALC5682I-VS, device id 0x6749). I2C, 16-bit registers and
   values, I2S consumer. Both chips answer at the same ACPI ids ("RTL5682" is used for both),
   so one probe reads the device id and the chip type picks the register sequences. Playback:
   DAC1 -> headphone amplifier (charge pump, depop sequence); capture: headset microphone on the
   combo jack (CBJ -> BST1 -> RECMIX1L -> ADC1 L, both I2S channels); jack type by the combo
   jack detection, polled. System clock 24.576 MHz (512 fs) from PLL1 (RT5682S: PLLA / PLLB)
   as sof_rt5682 sets it. Sequences after the Linux rt5682, rt5682-i2c, rt5682s, rl6231 and
   sof_rt5682 drivers. */
#include "dspcdc.h"

/* registers of both chips */
#define RESET           0x0000
#define HP_CTRL_1       0x0002
#define HP_CTRL_2       0x0003
#define I2C_CTRL        0x0008
#define CBJ_CTRL_1      0x0010
#define CBJ_CTRL_2      0x0011
#define CBJ_CTRL_3      0x0012
#define CBJ_CTRL_5      0x0014
#define CBJ_CTRL_6      0x0015
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
#define DMIC_CTRL_1     0x006E
#define I2S1_SDP        0x0070
#define ADDA_CLK_1      0x0073
#define TDM_CTRL        0x0079
#define TDM_ADDA_CTRL_2 0x007B
#define TDM_TCON_CTRL   0x007E
#define GLB_CLK         0x0080
#define PLL_TRACK_1     0x0083
#define PLL_TRACK_2     0x0084
#define PLL_TRACK_3     0x0085
#define PLL_TRACK_11    0x008C
#define DEPOP_1         0x008E
#define MICBIAS_2       0x0094
#define RC_CLK_CTRL     0x009F
#define IRQ_CTRL_2      0x00B7
#define GPIO_CTRL_1     0x00C0
#define IL_CMD_1        0x00DB
#define BTN_IL_CMD_2    0x00E3
#define BTN_IL_CMD_4    0x00E5          /* .. 7 at 0x00E8 */
#define AJD1_CTRL       0x00F0
#define DEVICE_ID       0x00FF
#define DAC_ADC_DIG_VOL1 0x0100
#define CHOP_DAC        0x013A          /* RT5682S: CHOP_DAC_2 */
#define CHOP_ADC        0x013B
#define HP_LOGIC_CTRL_2 0x01DB
#define HP_CALIB_CTRL_1 0x01DE
#define HP_CALIB_CTRL_2 0x01DF
#define HP_CALIB_CTRL_3 0x01E0
#define HP_CALIB_CTRL_9 0x01E6
#define HP_CALIB_ST_1   0x01EA
#define SAR_IL_CMD_1    0x0210
#define SAR_IL_CMD_2    0x0211
#define SAR_IL_CMD_3    0x0212
#define SAR_IL_CMD_6    0x0215
#define SAR_IL_CMD_13   0x021C
/* RT5682 only */
#define R_CBJ_BST_CTRL  0x000B
#define R_PLL_CTRL_1    0x0081
#define R_PLL_CTRL_2    0x0082
#define R_HP_CP_1       0x0091          /* HP_CHARGE_PUMP_1 */
#define R_BIAS_CUR_8    0x0111
#define R_CHARGE_PUMP_1 0x0125
#define R_CALIB_ADC     0x013C
#define R_TEST_MODE_1   0x0145
#define R_PLL2_INTERNAL 0x0156
#define R_HP_IMP_SENS_19 0x01C1
#define R_INT_DEVICE_ID 0x00F9
#define R_I2C_MODE      0xFFFF
/* RT5682S only */
#define S_HP_CP_2       0x0091          /* HP_CHARGE_PUMP_2 */
#define S_PLL_CTRL_1    0x0098
#define S_PLL_CTRL_2    0x0099
#define S_PLL_CTRL_3    0x009A
#define S_PLL_CTRL_4    0x009B
#define S_PLL_CTRL_6    0x009D
#define S_PLL_CTRL_7    0x009E
#define S_HP_AMP_DET_1  0x00D0
#define S_VREF_REC_1    0x0117
#define S_VREF_REC_2    0x0118
#define S_DIG_IN_CTRL_1 0x0132
#define S_BIAS_CUR_11   0x0610
#define S_BIAS_CUR_12   0x0611

/* PWR_ANLG_1 */
#define VREF1           0x8000
#define FV1             0x4000
#define VREF2           0x2000
#define FV2             0x1000
#define MB              0x0200
#define BG              0x0080

#define SYSCLK          (512UL * CDC_RATE)      /* 24.576 MHz */

/* priv[0]: chip (0 RT5682, 1 RT5682S), [1]: jack (0, 1 headphones, 3 headset), [2]: running
   streams (bit 0 playback, bit 1 capture), [3]: system clock source (0 MCLK, 1 PLL1 / PLLA,
   2 PLL2 / PLLB), [4]: ASRC on for the stereo filters */
#define IS_S(c)         ((c)->priv[0] != 0)
#define ASRC(c)         ((c)->priv[4] != 0)

/* ---------------------------------------------------------------- clocks */

struct pllcode { u32 n, m, k; int mbp; };

static u32 gcd32(u32 a, u32 b)
{
    while (b) {
        u32 t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static u32 adiff(u32 a, u32 b)
{
    return a > b ? a - b : b - a;
}

/* rl6231: smallest divider >= in / max that divides div */
static u32 best_div(u32 in, u32 max, u32 div)
{
    u32 d;
    if (in <= max) return 1;
    d = in / max + (in % max ? 1 : 0);
    while (div % d) d++;
    return d;
}

/* rl6231_pll_calc: Fout = Fin * (N + 2) / ((M + 2) * (K + 2)), M can be bypassed.
   1: exact, 0: approximation, -1: input out of range */
static int pll_calc(u32 fin, u32 fout, struct pllcode *p)
{
    long k, kt, mink, maxk, nt, mt, n = 0, m = 0;
    u32 red, redt, fi, fo, div, in_t, out_t;
    int mbp = 0, ex = 1;
    if (fin < 256000UL || fin > 50000000UL) return -1;
    if (fin == 19200000UL && fout == 24576000UL) {          /* preset table */
        k = 3; n = 30; m = 3;
        goto found;
    }
    if (fin == 3840000UL && fout == 24576000UL) {
        k = 3; n = 30; mbp = 1;
        goto found;
    }
    mink = (long)(80000000UL / fout) - 2;
    maxk = (long)(150000000UL / fout) - 2;
    if (maxk > 31) maxk = 31;
    if (mink > 31) mink = maxk = 31;
    div = best_div(fin, 0xFFFFFFFFUL / 511, gcd32(fin, fout));
    fi = fin / div;
    fo = fout / div;
    k = mink;
    if (mink < -1) mink = -1;
    redt = adiff(fout, fin);
    for (kt = mink; kt <= maxk; kt++) {
        for (nt = 0; nt <= 511; nt++) {
            in_t = fi * (u32)(nt + 2);
            if (in_t == fo * (u32)(kt + 2)) {
                mbp = 1; n = nt; k = kt;
                goto found;
            }
            out_t = in_t / (u32)(kt + 2);
            red = adiff(fo, out_t);
            if (red < redt) {
                mbp = 1; n = nt; m = 0; k = kt;
                if (!red) goto found;
                redt = red;
            }
            for (mt = 0; mt <= 15; mt++) {
                out_t = in_t / (u32)((mt + 2) * (kt + 2));
                red = adiff(fo, out_t);
                if (red < redt) {
                    mbp = 0; n = nt; m = mt; k = kt;
                    if (!red) goto found;
                    redt = red;
                }
            }
        }
    }
    ex = 0;
found:
    if (k == -1) k = 0;                                     /* K bypass */
    p->n = (u32)n;
    p->m = (u32)m;
    p->k = (u32)k;
    p->mbp = mbp;
    return ex;
}

/* RT5682S PLLA (same formula) and PLLB settings for a 24.576 MHz output (plla_table and
   pllb_table of rt5682s.c): input, M (0xFF bypassed), N, K */
static const u32 s_plla[][4] = {
    { 256000UL, 0xFF, 382, 2 }, { 512000UL, 0xFF, 190, 2 }, { 1024000UL, 0xFF, 94, 2 },
    { 1536000UL, 0xFF, 62, 2 }, { 2048000UL, 0xFF, 46, 2 }, { 3072000UL, 0xFF, 30, 2 },
    { 4096000UL, 0xFF, 22, 2 }, { 12288000UL, 1, 22, 2 }
};
static const u32 s_pllb[][4] = {
    { 19200000UL, 2, 6, 3 }, { 24000000UL, 3, 6, 3 }, { 38400000UL, 6, 6, 3 }, { 48000000UL, 8, 6, 3 }
};

/* RT5682S: PLLA (1) or PLLB (2, MCLK only) for fin, 0 if none */
static int s_pll(struct cdc *c, u32 fin, int mclk)
{
    const u32 *e;
    int i;
    for (i = 0; i < (int)ARRAY_SIZE(s_plla); i++) {
        e = s_plla[i];
        if (e[0] != fin) continue;
        cdc_upd(c, S_PLL_CTRL_1, 0x01FF, e[2]);
        cdc_upd(c, S_PLL_CTRL_2, 0x1F1F, ((e[1] == 0xFF ? 0 : e[1]) << 8) | e[3]);
        cdc_upd(c, S_PLL_CTRL_6, 0x00C0, e[1] == 0xFF ? 0x0080 : 0);
        return 1;
    }
    for (i = 0; mclk && i < (int)ARRAY_SIZE(s_pllb); i++) {
        e = s_pllb[i];
        if (e[0] != fin) continue;
        cdc_upd(c, S_PLL_CTRL_3, 0x03FF, e[2]);
        cdc_upd(c, S_PLL_CTRL_4, 0x1F1F, (e[1] << 8) | e[3]);
        cdc_upd(c, S_PLL_CTRL_6, 0x3C00, 0);                /* SEL_PS, BYP_PS, M / K bypass off */
        cdc_upd(c, S_PLL_CTRL_7, 0x0001, 0x0001);           /* PLLB from its own input */
        return 2;
    }
    return 0;
}

/* system clock 24.576 MHz from BCLK1 (when known and the SSP drives it) or MCLK */
static int r82_sysclk(struct cdc *c)
{
    struct pllcode p;
    u32 fin = 0, r;
    int i, src = 0, ex = 1;
    for (i = c->ssp.bclk && !c->ssp.provider ? 0 : 1; i < 2; i++) {
        fin = i ? c->ssp.mclk : c->ssp.bclk;
        if (!fin) continue;
        if (i && fin == SYSCLK) break;                      /* MCLK is 512 fs */
        if (IS_S(c)) {
            src = s_pll(c, fin, i);
            if (src) break;
        } else if ((ex = pll_calc(fin, SYSCLK, &p)) >= 0) {
            cdc_wr(c, R_PLL_CTRL_1, (p.n << 7) | p.k);
            cdc_wr(c, R_PLL_CTRL_2, ((p.mbp ? 0 : p.m) << 12) | (p.mbp ? 0x0800 : 0) | 0x0002);
            src = 1;
            break;
        }
    }
    if (i == 2) {
        kprintf("%s: no usable clock (BCLK %lu, MCLK %lu Hz)\n", c->drv->name, c->ssp.bclk, c->ssp.mclk);
        return -1;
    }
    DBG("%s: sysclk from %s %lu Hz, %s\n", c->drv->name, i ? "MCLK" : "BCLK", fin,
        src ? (src == 1 ? "PLL1" : "PLL2") : "direct");
    c->priv[3] = (u32)src;
    /* PLL input (MCLK 0, BCLK1 1) and system clock source */
    cdc_upd(c, GLB_CLK, 0xE300, ((u32)src << 13) | (i ? 0 : 0x0100));
    if (IS_S(c)) cdc_upd(c, ADDA_CLK_1, 0x0070, (u32)src << 4);
    /* ASRC (sof_rt5682 for 24 MHz MCLK): when the PLL only approximates 512 fs, or the
       frame is not 32 / 64 fs (Intel 25-bit slots) */
    r = c->ssp.bclk / CDC_RATE;
    c->priv[4] = !ex || (IS_S(c) && i && fin == 24000000UL) ||
                 (c->ssp.bclk && !c->ssp.provider && r != 32 && r != 64) ? 1 : 0;
    /* stereo DAC / ADC filters: 256 fs from 512 fs (divider 2), oversampling divider 2 */
    r = (ASRC(c) ? 0x1000 : 0) | 0x0100;
    cdc_upd(c, PLL_TRACK_2, 0x7F00, r);
    cdc_upd(c, PLL_TRACK_3, 0x7F00, r);
    cdc_upd(c, ADDA_CLK_1, 0xF00F, 0x1001);
    if (IS_S(c) && ASRC(c)) cdc_upd(c, PLL_TRACK_11, 0x0020, 0x0020);
    if (c->ssp.provider) {
        /* codec drives BCLK / LRCK: BCLK 32..256 fs, master divider 2 */
        r = c->ssp.bclk / CDC_RATE;
        r = r >= 256 ? 3 : r >= 128 ? 2 : r >= 64 ? 1 : 0;
        cdc_upd(c, TDM_TCON_CTRL, IS_S(c) ? 0x0300 : 0x0600, r << (IS_S(c) ? 8 : 9));
        cdc_upd(c, ADDA_CLK_1, 0x0F70, 0x0100 | ((u32)src << 4));
    }
    return 0;
}

/* I2S1 format, data and slot length, TDM (sof_rt5682: 2 slots, TDM off) */
static void r82_format(struct cdc *c)
{
    u32 df, dl, cl, w = c->ssp.width, vb = c->ssp.vbits, ch;
    switch (c->ssp.fmt) {
    case 3: df = 1; break;                      /* left justified */
    case 4: df = 2; break;                      /* DSP A */
    case 5: df = 3; break;                      /* DSP B */
    default: df = 0; break;                     /* I2S */
    }
    dl = vb <= 16 ? 0 : vb <= 20 ? 1 : vb <= 24 ? 2 : 3;
    cl = w >= 32 ? 3 : w >= 24 ? 2 : w >= 20 ? 1 : 0;
    cdc_upd(c, I2S1_SDP, 0x77F7, (cl << 12) | (cl << 8) | (dl << 4) | df);
    cdc_upd(c, TDM_TCON_CTRL, 0xF837, (df << 11) | (cl << 4) | (c->ssp.provider ? 1 : 0));
    ch = c->ssp.slots == 4 || c->ssp.slots == 6 || c->ssp.slots == 8 ? c->ssp.slots / 2 - 1 : 0;
    cdc_upd(c, TDM_CTRL, 0x3300, (ch << 12) | (ch << 8));
    cdc_upd(c, TDM_ADDA_CTRL_2, 0x0080, ch ? 0x0080 : 0);
}

/* ---------------------------------------------------------------- jack */

static void r_buttons(struct cdc *c, int on)
{
    if (on) {
        cdc_upd(c, SAR_IL_CMD_1, 0x8000, 0x8000);
        cdc_upd(c, SAR_IL_CMD_13, 0x003F, 0x003F);
        cdc_wr(c, IL_CMD_1, 0x0040);
        cdc_upd(c, BTN_IL_CMD_2, 0xC000, 0xC000);
    } else {
        cdc_upd(c, SAR_IL_CMD_1, 0x8000, 0);
        cdc_upd(c, BTN_IL_CMD_2, 0x8000, 0);
        cdc_upd(c, BTN_IL_CMD_2, 0x4000, 0);
        cdc_upd(c, SAR_IL_CMD_13, 0x003F, 0);
    }
}

static void s_buttons(struct cdc *c, int on)
{
    if (on) {
        cdc_upd(c, SAR_IL_CMD_13, 0x003F, 0x003F);
        cdc_upd(c, SAR_IL_CMD_1, 0xC020, 0x8020);
        cdc_wr(c, IL_CMD_1, 0x0040);
        cdc_upd(c, BTN_IL_CMD_2, 0xC000, 0xC000);
    } else {
        cdc_upd(c, BTN_IL_CMD_2, 0x8000, 0);
        cdc_upd(c, SAR_IL_CMD_13, 0x003F, 0);
        cdc_upd(c, SAR_IL_CMD_1, 0xC020, 0);
    }
}

/* RT5682S SAR (button ADC) power: 0 off, 1 normal (audio running), 2 power saving */
static void s_sar(struct cdc *c, int mode)
{
    if (mode) cdc_upd(c, CBJ_CTRL_3, 0x0080, 0x0080);
    cdc_upd(c, CBJ_CTRL_1, mode == 2 ? 0x0070 : 0x0030, 0);    /* MB1, MB2 (VREF) by FSM */
    if (mode == 1) {
        cdc_upd(c, SAR_IL_CMD_1, 0x0020, 0x0020);
        mdelay(5);
        cdc_upd(c, SAR_IL_CMD_1, 0xC000, 0x8000);
        return;
    }
    cdc_upd(c, SAR_IL_CMD_1, 0xC020, 0);
    if (mode == 2) {
        mdelay(5);
        cdc_upd(c, SAR_IL_CMD_1, 0x8000, 0x8000);
        mdelay(5);
        cdc_upd(c, SAR_IL_CMD_2, 0x0010, 0x0010);
    }
}

/* jack type after the combo jack detection: 0x1 / 0x2 headset (CTIA / OMTP) */
static int jack_type(struct cdc *c, int wait_first)
{
    int n, v = 0;
    for (n = 0; n <= 50; n++) {
        if (n || !wait_first) {
            v = cdc_rd(c, CBJ_CTRL_2);
            v = v < 0 ? 0 : v & 3;
            if (v || n == 50) break;
        }
        mdelay(10);
    }
    return v;
}

/* rt5682_headset_detect: 3 headset, 1 headphones, 0 removed */
static int r_detect(struct cdc *c, int in)
{
    int v;
    if (!in) {
        r_buttons(c, 0);
        cdc_upd(c, CBJ_CTRL_1, 0x0008, 0);
        if (!c->priv[2]) cdc_upd(c, PWR_ANLG_1, VREF2 | MB, 0);
        cdc_upd(c, PWR_ANLG_3, 0x0200, 0);
        cdc_upd(c, MICBIAS_2, 0x0300, 0);
        cdc_upd(c, CBJ_CTRL_1, 0x0080, 0);
        return 0;
    }
    cdc_upd(c, PWR_ANLG_1, VREF2 | MB, VREF2 | MB);
    cdc_upd(c, PWR_ANLG_1, FV2, 0);
    mdelay(15);
    cdc_upd(c, PWR_ANLG_1, FV2, FV2);
    cdc_upd(c, PWR_ANLG_3, 0x0200, 0x0200);                 /* CBJ */
    cdc_upd(c, R_HP_CP_1, 0x0C00, 0);                       /* output switches off */
    r_buttons(c, 0);
    cdc_upd(c, CBJ_CTRL_1, 0x0008, 0);                      /* trigger the detection */
    mdelay(55);
    cdc_upd(c, CBJ_CTRL_1, 0x0008, 0x0008);
    v = jack_type(c, 0);
    if (v == 1 || v == 2) {
        cdc_upd(c, CBJ_CTRL_1, 0x0080, 0x0080);             /* fast off */
        r_buttons(c, 1);
        v = 3;
    } else {
        v = 1;
    }
    cdc_upd(c, R_HP_CP_1, 0x0C00, 0x0C00);
    cdc_upd(c, MICBIAS_2, 0x0300, 0x0300);                  /* 25 MHz and 1 MHz clocks */
    return v;
}

/* rt5682s_headset_detect */
static int s_detect(struct cdc *c, int in)
{
    int v;
    if (!in) {
        s_sar(c, 0);
        s_buttons(c, 0);
        cdc_upd(c, CBJ_CTRL_1, 0x0008, 0);
        cdc_upd(c, PWR_ANLG_1, VREF2 | MB, 0);
        cdc_upd(c, PWR_ANLG_3, 0x0200, 0);
        cdc_upd(c, CBJ_CTRL_1, 0x0080, 0);
        cdc_upd(c, CBJ_CTRL_3, 0x0080, 0);
        return 0;
    }
    s_buttons(c, 0);
    cdc_upd(c, PWR_ANLG_1, VREF1 | VREF2 | MB, VREF1 | VREF2 | MB);
    cdc_upd(c, PWR_ANLG_1, FV1 | FV2, 0);
    mdelay(15);
    cdc_upd(c, PWR_ANLG_1, FV1 | FV2, FV1 | FV2);
    cdc_upd(c, PWR_ANLG_3, 0x0200, 0x0200);
    cdc_wr(c, SAR_IL_CMD_3, 0x0365);
    cdc_upd(c, S_HP_CP_2, 0x0C00, 0);
    cdc_upd(c, SAR_IL_CMD_13, 0x003F, 0);
    cdc_upd(c, CBJ_CTRL_3, 0x0080, 0x0080);
    cdc_upd(c, CBJ_CTRL_1, 0x0008, 0);
    mdelay(45);
    cdc_upd(c, CBJ_CTRL_1, 0x0008, 0x0008);
    v = jack_type(c, 1);
    if (v == 1 || v == 2) {
        cdc_wr(c, SAR_IL_CMD_3, 0x024C);
        cdc_upd(c, CBJ_CTRL_1, 0x0080, 0);
        cdc_upd(c, SAR_IL_CMD_1, 0x0300, (u32)v << 8);      /* MB1 / MB2 */
        s_buttons(c, 1);
        s_sar(c, c->priv[2] ? 1 : 2);
        v = 3;
    } else {
        v = 1;
    }
    cdc_upd(c, S_HP_CP_2, 0x0C00, 0x0C00);
    mdelay(35);
    return v;
}

static int r82_jack(struct cdc *c)
{
    int v = cdc_rd(c, AJD1_CTRL);
    if (v < 0) return -1;
    if (!(v & 0x0010) && !c->priv[1]) {
        /* plugged: settle, then the type detection */
        mdelay(50);
        v = cdc_rd(c, AJD1_CTRL);
        if (v < 0 || (v & 0x0010)) return 0;
        c->priv[1] = (u32)(IS_S(c) ? s_detect(c, 1) : r_detect(c, 1));
    } else if ((v & 0x0010) && c->priv[1]) {
        if (IS_S(c)) s_detect(c, 0);
        else r_detect(c, 0);
        c->priv[1] = 0;
    }
    return (int)c->priv[1];
}

/* JD1 jack detection (rt5682_set_jack_detect / rt5682s_set_jack_detect) */
static void r82_jack_setup(struct cdc *c)
{
    u32 r;
    cdc_upd(c, CBJ_CTRL_5, 0x0700, 0x0600);
    cdc_upd(c, CBJ_CTRL_2, 0x0070, 0x0040);                 /* external JD source manual */
    if (IS_S(c)) cdc_upd(c, CBJ_CTRL_1, 0x9102, 0x9002);
    else cdc_wr(c, CBJ_CTRL_1, 0xD142);
    if (!IS_S(c)) cdc_upd(c, CBJ_CTRL_3, 0x0080, 0x0080);
    cdc_upd(c, SAR_IL_CMD_1, 0x1000, 0x1000);               /* SAR power */
    cdc_upd(c, GPIO_CTRL_1, 0xC000, 0x4000);                /* GPIO1: IRQ */
    if (IS_S(c)) {
        cdc_upd(c, PWR_ANLG_3, 0x0001, 0x0001);             /* BG LDO */
        cdc_upd(c, PWR_ANLG_2, 0x0001, 0x0001);             /* JD */
        cdc_upd(c, RC_CLK_CTRL, 0xC000, 0xC000);
    } else {
        cdc_upd(c, RC_CLK_CTRL, 0xD000, 0xD000);
        cdc_upd(c, PWR_ANLG_2, 0x0008, 0x0008);             /* JDH */
    }
    cdc_upd(c, IRQ_CTRL_2, 0xA000, 0x8000);                 /* JD1 on, normal polarity */
    for (r = BTN_IL_CMD_4; r <= BTN_IL_CMD_4 + 3; r++) cdc_upd(c, r, 0x7F7F, 0x1010);
}

/* ---------------------------------------------------------------- setup */

static int calib_wait(struct cdc *c)
{
    int i, v;
    cdc_wr(c, HP_CALIB_CTRL_1, 0xFC00);
    for (i = 0; i < 60; i++) {
        v = cdc_rd(c, HP_CALIB_ST_1);
        if (v >= 0 && !(v & 0x8000)) return 0;
        mdelay(10);
    }
    kprintf("%s: headphone calibration failed\n", c->drv->name);
    return -1;
}

/* rt5682_calibrate, rt5682_apply_patch_list and the setup of rt5682_i2c_probe */
static void r_init(struct cdc *c)
{
    static const u16 cal[] = {
        PWR_ANLG_1, 0xF2AF, MICBIAS_2, 0x0300, GLB_CLK, 0x8000, PWR_DIG_1, 0x0100,
        R_HP_IMP_SENS_19, 0x3800, CHOP_DAC, 0x3000
    };
    static const u16 cal2[] = {
        STO1_ADC_MIXER, 0x686C, CAL_REC, 0x0D0D, HP_CALIB_CTRL_2, 0x0321,
        HP_LOGIC_CTRL_2, 0x0004, HP_CALIB_CTRL_1, 0x7C00, HP_CALIB_CTRL_3, 0x06A1,
        A_DAC1_MUX, 0x0311, HP_CALIB_CTRL_1, 0x7C00
    };
    static const u16 restore[] = {
        PWR_ANLG_1, 0x002F, MICBIAS_2, 0x0080, GLB_CLK, 0x0000, PWR_DIG_1, 0x0000,
        CHOP_DAC, 0x2000, STO1_ADC_MIXER, 0xC0C4, CAL_REC, 0x0C0C
    };
    static const u16 patch[] = {
        R_HP_IMP_SENS_19, 0x1000, DAC_ADC_DIG_VOL1, 0xA020, I2C_CTRL, 0x000F,
        R_PLL2_INTERNAL, 0x8266, SAR_IL_CMD_1, 0x22B7, SAR_IL_CMD_3, 0x0365,
        SAR_IL_CMD_6, 0x0110, R_CHARGE_PUMP_1, 0x0210, HP_LOGIC_CTRL_2, 0x0007,
        SAR_IL_CMD_2, 0xAC00, CBJ_CTRL_7, 0x0104
    };
    u32 adcal;
    cdc_wr(c, R_I2C_MODE, 1);
    mdelay(10);
    /* ALC5682I-VE: ADC calibration in CHOP_ADC */
    adcal = cdc_rd(c, R_INT_DEVICE_ID) == 0x6956 ? CHOP_ADC : R_CALIB_ADC;
    cdc_wr(c, RESET, 0);
    cdc_wr(c, R_I2C_MODE, 1);
    cdc_wr(c, I2C_CTRL, 0x000F);
    cdc_wr(c, PWR_ANLG_1, 0xA2AF);
    mdelay(15);
    cdc_seq(c, cal, (int)ARRAY_SIZE(cal) / 2);
    cdc_wr(c, adcal, 0x7005);
    cdc_seq(c, cal2, (int)ARRAY_SIZE(cal2) / 2);
    calib_wait(c);
    cdc_seq(c, restore, (int)ARRAY_SIZE(restore) / 2);
    cdc_wr(c, adcal, 0x2005);
    cdc_seq(c, patch, (int)ARRAY_SIZE(patch) / 2);
    cdc_wr(c, DEPOP_1, 0x0000);
    cdc_upd(c, PWR_ANLG_1, 0x003C, 0x002C);                 /* LDO1 1.2 V, HP driver 5x */
    cdc_wr(c, MICBIAS_2, 0x0080);
    cdc_upd(c, GPIO_CTRL_1, 0x03C0, 0x0140);                /* GPIO4 ADCDAT1, GPIO5 DACDAT1 */
    cdc_wr(c, R_TEST_MODE_1, 0x0000);
    cdc_upd(c, R_BIAS_CUR_8, 0x000C, 0x0004);               /* HP charge pump bias 3 uA */
    cdc_upd(c, R_CHARGE_PUMP_1, 0x0030, 0x0020);            /* charge pump clock 300 kHz */
    cdc_upd(c, R_HP_CP_1, 0x0300, 0x0200);                  /* HP power mode high voltage */
    cdc_upd(c, DMIC_CTRL_1, 0x7000, 0x1000);                /* FIFO clock / 2 */
    cdc_upd(c, R_CBJ_BST_CTRL, 0x0F00, 0x0200);             /* CBJ boost +24 dB */
}

/* rt5682s_i2c_probe: reset, patch list, calibration */
static void s_init(struct cdc *c)
{
    static const u16 patch[] = {
        I2C_CTRL, 0x0007, S_DIG_IN_CTRL_1, 0x0000, CHOP_DAC, 0x2020,
        S_VREF_REC_2, 0x0101, S_VREF_REC_1, 0x80C0, HP_CALIB_CTRL_9, 0x0002,
        DEPOP_1, 0x0000, S_HP_CP_2, 0x3C15, DAC1_DIG_VOL, 0xFEFE,
        SAR_IL_CMD_2, 0xAC00, SAR_IL_CMD_3, 0x024C, CBJ_CTRL_6, 0x0804
    };
    static const u16 cal[] = {
        PWR_ANLG_1, 0xFA80, PWR_DIG_1, 0x01C0, MICBIAS_2, 0x0380, GLB_CLK, 0x8000,
        ADDA_CLK_1, 0x1001, CHOP_DAC, 0x3030, CHOP_ADC, 0xB000, STO1_ADC_MIXER, 0x686C,
        CAL_REC, 0x5151, HP_CALIB_CTRL_2, 0x0321, HP_LOGIC_CTRL_2, 0x0004,
        HP_CALIB_CTRL_1, 0x7C00
    };
    static const u16 restore[] = {
        MICBIAS_2, 0x0180, CAL_REC, 0x5858, STO1_ADC_MIXER, 0xC0C4, HP_CALIB_CTRL_2, 0x0320,
        PWR_DIG_1, 0x00C0, PWR_ANLG_1, 0x0800, GLB_CLK, 0x0000
    };
    cdc_wr(c, RESET, 0);
    cdc_seq(c, patch, (int)ARRAY_SIZE(patch) / 2);
    cdc_upd(c, PWR_DIG_2, 0x0080, 0);                       /* DLDO current limit off */
    mdelay(20);
    cdc_wr(c, PWR_ANLG_1, 0xAA80);
    mdelay(15);
    cdc_seq(c, cal, (int)ARRAY_SIZE(cal) / 2);
    calib_wait(c);
    cdc_seq(c, restore, (int)ARRAY_SIZE(restore) / 2);
    cdc_upd(c, MICBIAS_2, 0x0300, 0x0100);                  /* 25 MHz clock off, 1 MHz on */
    cdc_upd(c, PWR_ANLG_1, BG, BG);
    cdc_upd(c, HP_LOGIC_CTRL_2, 0x0003, 0x0003);            /* HP signal: 1-bit control */
    cdc_upd(c, S_HP_CP_2, 0x0300, 0x0200);                  /* HP power mode high voltage */
    cdc_upd(c, S_HP_AMP_DET_1, 0x0070, 0x0020);             /* charge pump switch size M */
    cdc_upd(c, REC_MIXER, 0x3F00, 24 << 8);                 /* CBJ boost +24 dB */
}

static int r82_id(struct cdc *c)
{
    int id;
    /* LDO1 enable (realtek,ldo1-en) when ACPI lists an output GPIO */
    if (cdc_ngpio(c) > 0 && !c->gpiodone) {
        c->gpiodone = 1;
        cdc_gpio(c, 0, 1);
        mdelay(50);
    }
    id = cdc_rd(c, DEVICE_ID);
    if (id == 0x6749) return 1;                             /* ALC5682I-VS */
    cdc_wr(c, R_I2C_MODE, 1);
    mdelay(10);
    return cdc_rd(c, DEVICE_ID) == 0x6530 ? 0 : -1;         /* ALC5682I-VD / VE */
}

/* both chips: the first driver for "10EC5682" / "RTL5682" takes either */
static int r82_probe(struct cdc *c)
{
    int t = r82_id(c);
    if (t < 0) return -1;
    c->priv[0] = (u32)t;
    if (t) kprintf("%s: ALC5682I-VS (RT5682S)\n", c->drv->name);
    return 0;
}

static int r82s_probe(struct cdc *c)
{
    if (r82_id(c) != 1) return -1;
    c->priv[0] = 1;
    return 0;
}

static int r82_init(struct cdc *c)
{
    c->priv[1] = c->priv[2] = 0;
    if (IS_S(c)) s_init(c);
    else r_init(c);
    if (r82_sysclk(c)) return -1;
    r82_format(c);
    /* paths: DAC1 straight to the analog DACs; CBJ -> RECMIX1L -> ADC1 L to both ADC
       channels (Stereo1 ADC L/R mux ADC1 L, ADC1 switches on, DAC mix and DMIC off) */
    cdc_wr(c, AD_DA_MIXER, 0x8080);
    cdc_upd(c, A_DAC1_MUX, 0x0011, 0x0011);
    cdc_wr(c, STO1_ADC_MIXER, 0x6060);
    cdc_upd(c, REC_MIXER, 0x0080, 0);
    cdc_upd(c, I2S1_SDP, 0x8000, 0x8000);                   /* ADCDAT off until capture */
    r82_jack_setup(c);
    mdelay(50);
    return 0;
}

/* ---------------------------------------------------------------- streams */

/* bias, PLL, I2S1 and ASRC: on while a stream runs (DAPM supplies and bias levels) */
static void r82_clocks(struct cdc *c, int on)
{
    u32 pll, rst;
    if (IS_S(c)) {
        pll = c->priv[3] == 1 ? 0xA010 : c->priv[3] == 2 ? 0x5020 : 0;
        if (on) {
            cdc_upd(c, PWR_DIG_1, 0x0100, 0x0100);          /* LDO */
            cdc_upd(c, PWR_DIG_1, 0x0001, 0x0001);          /* digital gate */
            if (pll) {
                cdc_upd(c, PWR_ANLG_3, pll, pll);           /* PLL LDO, bias, power */
                rst = pll == 0xA010 ? 0x0040 : 0x0080;
                cdc_upd(c, PWR_ANLG_3, rst, rst);           /* then out of reset */
            }
        } else if (pll) {
            cdc_upd(c, PWR_ANLG_3, pll | 0x00C0, 0);
        }
    } else {
        if (on) {
            cdc_upd(c, PWR_ANLG_1, BG, BG);
            cdc_upd(c, PWR_DIG_1, 0x0101, 0x0101);
            if (c->priv[3]) cdc_upd(c, PWR_ANLG_3, 0x0040, 0x0040);    /* PLL1 */
        } else if (c->priv[3]) {
            cdc_upd(c, PWR_ANLG_3, 0x0040, 0);
        }
    }
    if (on) {
        cdc_upd(c, PWR_DIG_1, 0x8000, 0x8000);              /* I2S1 */
        if (ASRC(c)) {
            if (!IS_S(c)) cdc_upd(c, CLK_DET, 0x0001, 0x0001);
            cdc_upd(c, PLL_TRACK_1, 0x2100, 0x2100);        /* DA / AD ASRC */
        }
        if (IS_S(c) && c->priv[1] == 3) s_sar(c, 1);
        return;
    }
    if (IS_S(c) && c->priv[1] == 3) s_sar(c, 2);
    if (ASRC(c)) {
        cdc_upd(c, PLL_TRACK_1, 0x2100, 0);
        if (!IS_S(c)) cdc_upd(c, CLK_DET, 0x0001, 0);
    }
    cdc_upd(c, PWR_DIG_1, 0x8000, 0);
    cdc_upd(c, PWR_DIG_1, 0x0101, 0);                       /* bias off: LDO, digital gate */
    if (!IS_S(c)) cdc_upd(c, PWR_ANLG_1, BG, 0);
}

/* RT5682 headphone path: Vref1, DAC filter, HP amp, charge pump, capless, DAC clock, DACs,
   then rt5682_hp_event */
static void r_play(struct cdc *c, int on)
{
    if (on) {
        cdc_upd(c, PWR_ANLG_1, FV1, 0);
        cdc_upd(c, PWR_ANLG_1, VREF1, VREF1);
        mdelay(15);
        cdc_upd(c, PWR_ANLG_1, FV1, FV1);
        cdc_upd(c, PWR_DIG_2, 0x0400, 0x0400);
        cdc_upd(c, PWR_ANLG_1, 0x0003, 0x0003);
        cdc_upd(c, CLK_DET, 0x8001, 0x8001);
        if (ASRC(c)) cdc_upd(c, PLL_TRACK_1, 0x1000, 0x1000);
        cdc_upd(c, DEPOP_1, 0x0008, 0x0008);                /* charge pump */
        cdc_upd(c, DEPOP_1, 0x0001, 0x0001);                /* capless */
        cdc_upd(c, CHOP_DAC, 0x1000, 0x1000);
        cdc_upd(c, PWR_DIG_1, 0x0C00, 0x0C00);
        cdc_upd(c, HP_CTRL_2, 0x8000, 0);
        cdc_upd(c, HP_LOGIC_CTRL_2, 0x0010, 0x0010);
        cdc_upd(c, DEPOP_1, 0x0060, 0x0060);
        cdc_upd(c, DAC_ADC_DIG_VOL1, 0x00C0, 0x0080);
        cdc_upd(c, HP_CTRL_2, 0x6000, 0x6000);
        mdelay(5);
        cdc_upd(c, R_CHARGE_PUMP_1, 0x0700, 0x0400);        /* switch size L */
        cdc_upd(c, HP_CTRL_1, 0x8080, 0);
        return;
    }
    cdc_upd(c, HP_CTRL_1, 0x8080, 0x8080);
    cdc_upd(c, HP_CTRL_2, 0x6000, 0);
    cdc_upd(c, R_CHARGE_PUMP_1, 0x0700, 0x0200);            /* switch size M */
    cdc_upd(c, DEPOP_1, 0x0060, 0);
    cdc_upd(c, DAC_ADC_DIG_VOL1, 0x00C0, 0);
    cdc_upd(c, PWR_DIG_1, 0x0C00, 0);
    cdc_upd(c, CHOP_DAC, 0x1000, 0);
    cdc_upd(c, DEPOP_1, 0x0009, 0);
    if (ASRC(c)) cdc_upd(c, PLL_TRACK_1, 0x1000, 0);
    cdc_upd(c, CLK_DET, ASRC(c) ? 0x8000 : 0x8001, 0);
    cdc_upd(c, PWR_ANLG_1, 0x0003, 0);
    cdc_upd(c, PWR_DIG_2, 0x0400, 0);
    cdc_upd(c, PWR_ANLG_1, VREF1, 0);
}

/* RT5682S headphone path: DAC filter, clock detection, DACs, then rt5682s_hp_amp_event */
static void s_play(struct cdc *c, int on)
{
    if (on) {
        cdc_upd(c, PWR_DIG_2, 0x0400, 0x0400);
        cdc_upd(c, CLK_DET, 0x8000, 0x8000);
        cdc_upd(c, PWR_ANLG_2, 0x8000, 0x8000);             /* MCLK0 detection */
        if (ASRC(c)) cdc_upd(c, PLL_TRACK_1, 0x1000, 0x1000);
        cdc_upd(c, PWR_DIG_1, 0x0C00, 0x0C00);
        cdc_upd(c, DEPOP_1, 0x0060, 0x0060);                /* HP outputs */
        mdelay(15);
        cdc_upd(c, DEPOP_1, 0x001B, 0x001B);                /* pump LDO, pump, capless L / R */
        cdc_wr(c, S_BIAS_CUR_11, 0x6666);
        cdc_wr(c, S_BIAS_CUR_12, 0xA82A);
        cdc_upd(c, HP_CTRL_2, 0x6001, 0x6001);
        mdelay(5);
        cdc_upd(c, S_HP_AMP_DET_1, 0x0070, 0x0050);         /* switch size L | S */
        return;
    }
    cdc_upd(c, HP_CTRL_2, 0x6001, 0);
    cdc_upd(c, S_HP_AMP_DET_1, 0x0070, 0x0020);
    cdc_upd(c, DEPOP_1, 0x001B, 0);
    cdc_upd(c, DEPOP_1, 0x0060, 0);
    cdc_upd(c, PWR_DIG_1, 0x0C00, 0);
    if (ASRC(c)) cdc_upd(c, PLL_TRACK_1, 0x1000, 0);
    cdc_upd(c, PWR_ANLG_2, 0x8000, 0);
    cdc_upd(c, CLK_DET, 0x8000, 0);
    cdc_upd(c, PWR_DIG_2, 0x0400, 0);
}

/* capture: ADC filter, ADC1 L, ADC clock, RECMIX1L, ADCDAT1 output */
static void r82_rec(struct cdc *c, int on)
{
    u32 rm = IS_S(c) ? CAL_REC : PWR_ANLG_2, rmb = IS_S(c) ? 0x0001 : 0x0002;
    if (on) {
        cdc_upd(c, PWR_DIG_2, 0x8000, 0x8000);
        cdc_upd(c, PWR_DIG_1, 0x0010, 0x0010);
        cdc_upd(c, CHOP_ADC, 0x1000, 0x1000);
        cdc_upd(c, rm, rmb, rmb);
        if (ASRC(c)) cdc_upd(c, PLL_TRACK_1, 0x0004, 0x0004);
        cdc_upd(c, I2S1_SDP, 0x8000, 0);
        return;
    }
    cdc_upd(c, I2S1_SDP, 0x8000, 0x8000);
    if (ASRC(c)) cdc_upd(c, PLL_TRACK_1, 0x0004, 0);
    cdc_upd(c, rm, rmb, 0);
    cdc_upd(c, CHOP_ADC, 0x1000, 0);
    cdc_upd(c, PWR_DIG_1, 0x0010, 0);
    cdc_upd(c, PWR_DIG_2, 0x8000, 0);
}

static void r82_stream(struct cdc *c, int dir, int on)
{
    u32 bit = dir ? 2 : 1, was = c->priv[2];
    if (on ? (was & bit) != 0 : !(was & bit)) return;
    if (on && !was) r82_clocks(c, 1);
    if (dir) r82_rec(c, on);
    else if (IS_S(c)) s_play(c, on);
    else r_play(c, on);
    c->priv[2] = on ? was | bit : was & ~bit;
    if (!c->priv[2]) r82_clocks(c, 0);
}

static void r82_volume(struct cdc *c, int ctl, u32 lr)
{
    u32 l, r, max;
    if (ctl == MX_MASTER) {
        /* DAC1: 0.75 dB steps up to 0 dB (RT5682 from -65.25 dB, RT5682S from -95.625 dB);
           level 0 mutes DAC1 into the DAC mixer */
        max = IS_S(c) ? 127 : 87;
        l = cdc_step(CDC_L(lr), max);
        r = cdc_step(CDC_R(lr), max);
        cdc_wr(c, DAC1_DIG_VOL, (l << 9) | (r << 1));
        cdc_upd(c, AD_DA_MIXER, 0x4040, (l ? 0 : 0x4000) | (r ? 0 : 0x0040));
    } else if (ctl == MX_INGAIN) {
        /* stereo ADC: -17.25 .. +30 dB in 0.75 dB steps after the +24 dB CBJ boost */
        l = cdc_step(CDC_L(lr), 63);
        r = cdc_step(CDC_R(lr), 63);
        cdc_wr(c, ADC_DIG_VOL, (l ? l << 9 : 0x8000) | (r ? r << 1 : 0x0080));
    }
}

static void r82_off(struct cdc *c)
{
    if (c->priv[2] & 1) r82_stream(c, 0, 0);
    if (c->priv[2] & 2) r82_stream(c, 1, 0);
    if (c->priv[1]) {
        if (IS_S(c)) s_detect(c, 0);
        else r_detect(c, 0);
        c->priv[1] = 0;
    }
    cdc_wr(c, RESET, 0);
    if (!IS_S(c)) cdc_wr(c, R_I2C_MODE, 1);
}

const struct cdcdrv cdc_rt5682 = { "rt5682", { "10EC5682", "RTL5682", NULL }, CK_HEADSET, 16, 16, 0,
                                   r82_probe, r82_init, r82_stream, r82_volume, NULL, r82_jack, r82_off };
const struct cdcdrv cdc_rt5682s = { "rt5682s", { "RTL5682", NULL }, CK_HEADSET, 16, 16, 0,
                                    r82s_probe, r82_init, r82_stream, r82_volume, NULL, r82_jack, r82_off };
