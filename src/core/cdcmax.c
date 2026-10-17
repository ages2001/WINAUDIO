/* cdcmax.c - Maxim speaker amplifiers on the SSP ports: MAX98373, MAX98390, MAX98927 (I2C,
   16-bit register addresses, 8-bit values, BCLK clocked, I2S or TDM consumer) and the
   MAX98357A / MAX98360A (no control bus, only the optional SD_MODE GpioIo). Register
   settings after the Linux max98373, max98390, max98927 and max98357a drivers and the Intel
   machine drivers (sof_maxim_common, avs max98373/max98927): with two amplifiers ACPI
   instance :00 is the right speaker and :01 the left one (MAX98390 on Comet Lake: :00 left);
   every amplifier receives the left and right slots of the frame and picks one. IV sense
   (speaker current / voltage feedback) stays off, the MAX98390 runs without its DSM
   calibration file (DSM off) as Linux does when no dsm_param file is found. */
#include "dspcdc.h"

/* priv[0]: speaker amplifier running */

/* ---------------------------------------------------------------- common */

/* the first two playback slots of the SSP: left and right channel of the frame */
static void mx_slots(struct cdc *c, u32 *l, u32 *r)
{
    u32 i;
    int n = 0;
    *l = 0;
    *r = 1;
    for (i = 0; i < 16 && n < 2; i++) {
        if (!(c->ssp.txmask & (1UL << i))) continue;
        if (n++) *r = i;
        else *l = i;
    }
    if (n == 1) *r = *l;
}

/* speaker channel of this amplifier: 0 left, 1 right, 2 (L+R)/2 with one amplifier;
   first_left: the first ACPI instance is the left speaker */
static u32 mx_side(struct cdc *c, int first_left)
{
    if (c->namps < 2) return 2;
    return ((c->chan & 1) != 0) == (first_left != 0) ? 1 : 0;
}

/* channel (slot) size field, bits 7:6 of the PCM mode register */
static u32 mx_chansz(struct cdc *c)
{
    switch (c->ssp.width) {
    case 16: return 1 << 6;
    case 24: return 2 << 6;
    case 32: return 3 << 6;
    }
    kprintf("%s: %u bit slots not supported\n", c->drv->name, c->ssp.width);
    return 3 << 6;
}

/* interface format field (bits 5:3): I2S 0, left justified 1, TDM mode 0 (DSP B) 3,
   TDM mode 1 (DSP A) 4 */
static u32 mx_fmt(struct cdc *c)
{
    if (c->ssp.provider) kprintf("%s: clock provider mode not supported\n", c->drv->name);
    switch (c->ssp.fmt) {
    case 3: return 1 << 3;
    case 4: return 4 << 3;
    case 5: return 3 << 3;
    }
    return 0;
}

/* BCLK per frame select: index in the chip's table + 2 */
static u32 mx_bsel(struct cdc *c, const u16 *tab, int n)
{
    u32 bpf = (u32)c->ssp.slots * c->ssp.width;
    int i;
    for (i = 0; i < n; i++)
        if (tab[i] == bpf) return (u32)i + 2;
    kprintf("%s: %lu BCLK per frame not supported\n", c->drv->name, bpf);
    return 4;                                   /* 64 */
}

static int mx_probe(struct cdc *c, u32 revreg)
{
    int v;
    if (!c->bus) return -1;
    v = cdc_rd(c, revreg);
    if (v < 0) return -1;
    DBG("%s: revision %02x\n", c->drv->name, v);
    return 0;
}

/* ---------------------------------------------------------------- MAX98373 */

#define M73_RESET       0x2000
#define M73_TX_HIZ1     0x2020
#define M73_TX_HIZ2     0x2021
#define M73_FMT         0x2024
#define M73_CLK         0x2026
#define M73_SR1         0x2027
#define M73_SR2         0x2028
#define M73_MIX1        0x2029
#define M73_MIX2        0x202A
#define M73_RX_EN       0x202B
#define M73_TX_EN       0x202C
#define M73_DIG_VOL     0x203D
#define M73_PATH_GAIN   0x203E
#define M73_DSP_CFG     0x203F
#define M73_AMP_EN      0x2043
#define M73_IV_DSP      0x2046
#define M73_IV_EN       0x2047
#define M73_RESTART     0x20FE
#define M73_GLOBAL      0x20FF
#define M73_REV_ID      0x21FF

static int m73_probe(struct cdc *c)
{
    return mx_probe(c, M73_REV_ID);
}

static int m73_init(struct cdc *c)
{
    static const u16 bpf[] = { 32, 48, 64, 96, 128, 192, 256, 384, 512, 320 };
    u32 l, r;
    int i;
    cdc_wr(c, M73_RESET, 0x01);
    for (i = 0; i < 3; i++) {
        mdelay(10);
        if (cdc_rd(c, M73_REV_ID) >= 0) break;
    }
    cdc_wr(c, M73_GLOBAL, 0x00);
    /* IV sense off: every transmit slot Hi-Z */
    cdc_wr(c, M73_TX_HIZ1, 0xFF);
    cdc_wr(c, M73_TX_HIZ2, 0xFF);
    cdc_wr(c, M73_TX_EN, 0x00);
    cdc_wr(c, M73_IV_EN, 0x00);
    cdc_wr(c, M73_DSP_CFG, 0x03);               /* DC blocker, dither */
    cdc_wr(c, M73_IV_DSP, 0x07);
    cdc_wr(c, M73_RESTART, 0x0F);               /* clock monitor, auto restart after faults */
    /* interface: consumer, slot size, format, BCLK per frame, 48 kHz (also IV ADC) */
    cdc_wr(c, M73_FMT, mx_chansz(c) | mx_fmt(c));
    cdc_upd(c, M73_CLK, 0x1F, mx_bsel(c, bpf, (int)ARRAY_SIZE(bpf)));
    cdc_upd(c, M73_SR1, 0x0F, 0x08);
    cdc_wr(c, M73_SR2, 0x88);
    /* left / right slot into the mono mixer, this speaker's channel */
    mx_slots(c, &l, &r);
    cdc_wr(c, M73_MIX1, mx_side(c, 0) << 6 | l);
    cdc_wr(c, M73_MIX2, r);
    /* digital volume 0 dB, speaker gain 0 dB, full scale +16 dB (the reset values) */
    cdc_wr(c, M73_DIG_VOL, 0x00);
    cdc_wr(c, M73_PATH_GAIN, 0x08);
    cdc_wr(c, M73_AMP_EN, 0x01);                /* speaker on, feedback off */
    cdc_wr(c, M73_RX_EN, 0x00);
    c->priv[0] = 0;
    return 0;
}

static void m73_stream(struct cdc *c, int dir, int on)
{
    if (dir || c->priv[0] == (u32)on) return;
    c->priv[0] = (u32)on;
    if (on) {
        cdc_wr(c, M73_RX_EN, 0x01);
        cdc_wr(c, M73_GLOBAL, 0x01);
        mdelay(30);
    } else {
        cdc_wr(c, M73_GLOBAL, 0x00);
        mdelay(30);
        cdc_wr(c, M73_RX_EN, 0x00);
    }
}

static void m73_off(struct cdc *c)
{
    cdc_wr(c, M73_GLOBAL, 0x00);
    cdc_wr(c, M73_RX_EN, 0x00);
    cdc_wr(c, M73_AMP_EN, 0x00);
    c->priv[0] = 0;
}

/* ---------------------------------------------------------------- MAX98390 */

#define M90_RESET       0x2000
#define M90_CLK_MON     0x2012
#define M90_DAT_MON     0x2014
#define M90_RX_EN_A     0x201B
#define M90_RX_EN_B     0x201C
#define M90_TX_EN_A     0x201D
#define M90_TX_EN_B     0x201E
#define M90_TX_HIZ_A    0x201F
#define M90_TX_HIZ_B    0x2020
#define M90_CH_SRC1     0x2021
#define M90_MODE        0x2024
#define M90_MASTER      0x2025
#define M90_CLK         0x2026
#define M90_SR          0x2027
#define M90_AMP_EN      0x203A
#define M90_PWR_GATE    0x2050
#define M90_ENV_HEAD    0x2076
#define M90_BST_BYP1    0x207C
#define M90_FET_SCALE3  0x2081
#define M90_DSP_EN      0x23E1
#define M90_GLOBAL      0x23FF
#define M90_REV_ID      0x24FF

static int m90_probe(struct cdc *c)
{
    return mx_probe(c, M90_REV_ID);
}

static int m90_init(struct cdc *c)
{
    static const u16 bpf[] = { 32, 48, 64, 96, 128, 192, 256, 320, 384, 512 };
    u32 l, r, m;
    cdc_wr(c, M90_RESET, 0x01);
    mdelay(20);
    cdc_wr(c, M90_GLOBAL, 0x00);
    /* the settings of the Linux driver before its DSM parameter file */
    cdc_wr(c, M90_CLK_MON, 0x6F);
    cdc_wr(c, M90_DAT_MON, 0x00);
    cdc_wr(c, M90_PWR_GATE, 0x00);
    cdc_wr(c, M90_ENV_HEAD, 0x0E);
    cdc_wr(c, M90_BST_BYP1, 0x46);
    cdc_wr(c, M90_FET_SCALE3, 0x03);
    cdc_wr(c, M90_DSP_EN, 0x00);                /* no DSM parameters: DSM off */
    /* IV sense off */
    cdc_wr(c, M90_TX_EN_A, 0x00);
    cdc_wr(c, M90_TX_EN_B, 0x00);
    cdc_wr(c, M90_TX_HIZ_A, 0xFF);
    cdc_wr(c, M90_TX_HIZ_B, 0xFF);
    /* interface: consumer, slot size, format, BCLK per frame, 48 kHz */
    cdc_upd(c, M90_MASTER, 0x03, 0x00);
    cdc_wr(c, M90_MODE, mx_chansz(c) | mx_fmt(c));
    cdc_upd(c, M90_CLK, 0x0F, mx_bsel(c, bpf, (int)ARRAY_SIZE(bpf)));
    cdc_upd(c, M90_SR, 0x0F, 0x08);
    /* receive the left and right slots (rx mask 3 on Linux), play this speaker's one */
    mx_slots(c, &l, &r);
    m = (1UL << l) | (1UL << r);
    cdc_wr(c, M90_RX_EN_A, m & 0xFF);
    cdc_wr(c, M90_RX_EN_B, m >> 8);
    cdc_upd(c, M90_CH_SRC1, 0x0F, mx_side(c, !strncmp(c->d->pl->name, "cml", 4)) == 1 ? r : l);
    cdc_upd(c, M90_AMP_EN, 0x01, 0x00);
    c->priv[0] = 0;
    return 0;
}

static void m90_stream(struct cdc *c, int dir, int on)
{
    if (dir || c->priv[0] == (u32)on) return;
    c->priv[0] = (u32)on;
    if (on) {
        cdc_upd(c, M90_AMP_EN, 0x01, 0x01);
        cdc_upd(c, M90_GLOBAL, 0x01, 0x01);
    } else {
        cdc_upd(c, M90_GLOBAL, 0x01, 0x00);
        cdc_upd(c, M90_AMP_EN, 0x01, 0x00);
    }
}

static void m90_off(struct cdc *c)
{
    c->priv[0] = 1;
    m90_stream(c, 0, 0);
}

/* ---------------------------------------------------------------- MAX98927 */

#define M27_RX_EN_A     0x0018
#define M27_RX_EN_B     0x0019
#define M27_TX_EN_A     0x001A
#define M27_TX_EN_B     0x001B
#define M27_TX_HIZ_A    0x001C
#define M27_TX_HIZ_B    0x001D
#define M27_MODE        0x0020
#define M27_MASTER      0x0021
#define M27_CLK         0x0022
#define M27_SR1         0x0023
#define M27_SR2         0x0024
#define M27_MIX_A       0x0025
#define M27_MIX_B       0x0026
#define M27_PDM_RX      0x0035
#define M27_VOL         0x0036
#define M27_DSP_CFG     0x0037
#define M27_AMP_EN      0x003A
#define M27_SPK_SRC     0x003B
#define M27_SPK_GAIN    0x003C
#define M27_MEAS_EN     0x003E
#define M27_GLOBAL      0x00FF
#define M27_RESET       0x0100
#define M27_REV_ID      0x01FF

static int m27_probe(struct cdc *c)
{
    return mx_probe(c, M27_REV_ID);
}

static int m27_init(struct cdc *c)
{
    static const u16 bpf[] = { 32, 48, 64, 96, 128, 192, 256, 384, 512 };
    static const u16 regs[] = {
        M27_GLOBAL, 0x00,
        M27_TX_EN_A, 0x00, M27_TX_EN_B, 0x00,   /* IV sense off */
        M27_TX_HIZ_A, 0xFF, M27_TX_HIZ_B, 0xFF,
        M27_MEAS_EN, 0x00,
        M27_VOL, 0x38, M27_SPK_GAIN, 0x05,      /* the Linux start volume */
        M27_DSP_CFG, 0x03,                      /* DC blocker, dither */
        0x003F, 0xF7,                           /* IMON / VMON DC blocker */
        0x0040, 0x1C, 0x0042, 0x3E,             /* boost voltage, current limit */
        0x0043, 0x04, 0x0044, 0x00, 0x0045, 0x24,   /* measurement ADC */
        0x007F, 0x06,                           /* brownout level 4 */
        0x0082, 0x08, 0x0086, 0x01, 0x0087, 0x10,   /* envelope tracking */
        M27_MASTER, 0x00,                       /* consumer */
        M27_SR2, 0x88,                          /* 48 kHz, IV ADC 48 kHz */
        M27_AMP_EN, 0x00
    };
    u32 l, r, m;
    cdc_wr(c, M27_RESET, 0x01);
    mdelay(10);
    cdc_seq(c, regs, (int)ARRAY_SIZE(regs) / 2);
    cdc_wr(c, M27_MODE, mx_chansz(c) | mx_fmt(c));
    cdc_upd(c, M27_CLK, 0x0F, mx_bsel(c, bpf, (int)ARRAY_SIZE(bpf)));
    cdc_upd(c, M27_SR1, 0x0F, 0x08);
    cdc_upd(c, M27_SPK_SRC, 0x03, 0x00);        /* speaker from PCM, not PDM */
    cdc_upd(c, M27_PDM_RX, 0x01, 0x00);
    /* left / right slot in, mono mixer picks this speaker's channel */
    mx_slots(c, &l, &r);
    m = (1UL << l) | (1UL << r);
    cdc_wr(c, M27_RX_EN_A, m & 0xFF);
    cdc_wr(c, M27_RX_EN_B, m >> 8);
    cdc_wr(c, M27_MIX_A, mx_side(c, 0) << 6 | l);
    cdc_wr(c, M27_MIX_B, r);
    c->priv[0] = 0;
    return 0;
}

static void m27_stream(struct cdc *c, int dir, int on)
{
    if (dir || c->priv[0] == (u32)on) return;
    c->priv[0] = (u32)on;
    if (on) {
        cdc_upd(c, M27_AMP_EN, 0x01, 0x01);
        cdc_wr(c, M27_GLOBAL, 0x01);
    } else {
        cdc_wr(c, M27_GLOBAL, 0x00);
        cdc_upd(c, M27_AMP_EN, 0x01, 0x00);
    }
}

static void m27_off(struct cdc *c)
{
    c->priv[0] = 1;
    m27_stream(c, 0, 0);
}

/* ---------------------------------------------------------------- MAX98357A / MAX98360A */

/* no control bus: the chip follows BCLK / LRCLK; SD_MODE (first GpioIo, active high) on
   only while playback runs */
static int m57_probe(struct cdc *c)
{
    (void)c;
    return 0;
}

static void m57_sdmode(struct cdc *c, int on)
{
    if (cdc_ngpio(c) > 0) cdc_gpio(c, 0, on);
}

static int m57_init(struct cdc *c)
{
    m57_sdmode(c, 0);
    return 0;
}

static void m57_stream(struct cdc *c, int dir, int on)
{
    if (!dir) m57_sdmode(c, on);
}

static void m57_off(struct cdc *c)
{
    m57_sdmode(c, 0);
}

const struct cdcdrv cdc_max98373 = { "max98373", { "MX98373", NULL }, CK_AMP, 16, 8, 32,
                                     m73_probe, m73_init, m73_stream, NULL, NULL, NULL, m73_off };
const struct cdcdrv cdc_max98390 = { "max98390", { "MX98390", NULL }, CK_AMP, 16, 8, 32,
                                     m90_probe, m90_init, m90_stream, NULL, NULL, NULL, m90_off };
const struct cdcdrv cdc_max98927 = { "max98927", { "MX98927", NULL }, CK_AMP, 16, 8, 32,
                                     m27_probe, m27_init, m27_stream, NULL, NULL, NULL, m27_off };
const struct cdcdrv cdc_max98357a = { "max98357a", { "MX98357A", NULL }, CK_AMP, 8, 8, 24,
                                      m57_probe, m57_init, m57_stream, NULL, NULL, NULL, m57_off };
const struct cdcdrv cdc_max98360a = { "max98360a", { "MX98360A", NULL }, CK_AMP, 8, 8, 24,
                                      m57_probe, m57_init, m57_stream, NULL, NULL, NULL, m57_off };
