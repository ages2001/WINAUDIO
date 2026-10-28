/* sdwcs.c - Cirrus Logic SoundWire peripherals:
   CS42L43 / CS42L43B  headset codec with speaker amplifier and digital microphone inputs
                       (32-bit registers; an MCU runs from ROM or a patch until the driver
                       disables it)
   CS35L56 / CS35L57 / CS35L62 / CS35L63  speaker amplifiers (32-bit registers at +0x8000; a
                       HALO DSP plays through its ROM firmware when no tuning is loaded)
   CS42L42             headset codec (8-bit registers at +0x8000 with delayed reads)
   32-bit registers go over the bus as four 8-bit registers, least significant byte first, as
   regmap-sdw does. Sequences after the Linux cs42l43 (mfd and codec), cs35l56(-shared, -sdw),
   cs42l42(-sdw) drivers and the sdw_utils machine helpers. The board _DSD properties of these
   chips (cirrus,* jack settings) are not read: the Linux defaults are used. */
#include "sdw.h"

#define MFR_CIRRUS      0x01FA

/* bus clock the core runs (no SCP_BusClock registers on these chips): 4.8 MHz from a 38.4 or
   19.2 MHz crystal, 6 MHz from 24 MHz */
static u32 bus_hz(struct sdwdev *s)
{
    if (s->busclk) return s->busclk;
    return (s->d && dsp_xtal(s->d) == 24000000UL) ? 6000000UL : 4800000UL;
}

/* ---------------------------------------------------------------- 32-bit registers */

static int rd32(struct sdwdev *s, u32 reg, u32 *v)
{
    u32 r = 0;
    int i, b;
    for (i = 0; i < 4; i++) {
        b = sdw_rd(s, reg + (u32)i);
        if (b < 0) return -1;
        r |= (u32)b << (8 * i);
    }
    *v = r;
    return 0;
}

static int wr32(struct sdwdev *s, u32 reg, u32 v)
{
    int i;
    for (i = 0; i < 4; i++)
        if (sdw_wr(s, reg + (u32)i, (u8)(v >> (8 * i)))) return -1;
    return 0;
}

static int upd32(struct sdwdev *s, u32 reg, u32 mask, u32 val)
{
    u32 v;
    if (rd32(s, reg, &v)) return -1;
    return wr32(s, reg, (v & ~mask) | (val & mask));
}

static void seq32(struct sdwdev *s, const u32 *p, int n)
{
    int i;
    for (i = 0; i < n; i++) wr32(s, p[2 * i], p[2 * i + 1]);
}

/* (reg & mask) == val within ms milliseconds */
static int poll32(struct sdwdev *s, u32 reg, u32 mask, u32 val, u32 ms)
{
    u32 t, v;
    for (t = 0; t <= ms; t++) {
        if (!rd32(s, reg, &v) && (v & mask) == val) return 0;
        mdelay(1);
    }
    return -1;
}

/* ================================================================ CS42L43 */

#define L43_DEVID       0x3000
#define L43_REVID       0x3004
#define L43_SFT_RESET   0x3020
#define L43_MCLK_SRC    0x7004
#define L43_RATE1       0x7014
#define L43_PLL_CONTROL 0x7034
#define L43_OSC_DIV     0x7068
#define L43_ADC_B_CTRL1 0x8000
#define L43_DECIM12     0x8050
#define L43_DECIM34     0x8054
#define L43_INTP_VOL1   0x9008
#define L43_INTP_VOL2   0x900C
#define L43_OTP_REV     0xB02C
#define L43_DP1_CH1     0xC280
#define L43_DP1_CH2     0xC290
#define L43_DP2_CH1     0xC2C0
#define L43_DP2_CH2     0xC2D0
#define L43_AMP1MIX1    0xC620
#define L43_AMP2MIX1    0xC630
#define L43_AMP3MIX1    0xC640
#define L43_AMP4MIX1    0xC650
#define L43_PDNCNTL     0x10000
#define L43_TIP_DEB     0x10020
#define L43_TIPRING_STS 0x10028
#define L43_HS2         0x10040
#define L43_HS_STAT     0x10048
#define L43_MCU_SW_INT  0x10094
#define L43_SMIC_CTRL   0x100A4
#define L43_SMIC_CLAMP  0x100C4
#define L43_BLOCK_EN3   0x10108
#define L43_BLOCK_EN6   0x10114
#define L43_BLOCK_EN8   0x1011C
#define L43_BLOCK_EN10  0x10124
#define L43_MICDET1     0x11074
#define L43_HSB_AUTO    0x11090
#define L43_CTRL_REG    0x14000
#define L43_FDIV_FRAC   0x14004
#define L43_CAL_RATIO   0x14008
#define L43_DACCNFG1    0x17000
#define L43_DACCNFG2    0x17004
#define L43_HPPATHVOL   0x1700C
#define L43_PGAVOL      0x17014
#define L43_LOADDET_RES 0x17018
#define L43_LOADDET_EN  0x17024
#define L43_CTRL        0x17028
#define L43_SOFT_INT    0x1B010
#define L43_SOFT_SHADOW 0x1B310
#define L43_BOOT_CTRL   0x101000
#define L43_MCU_SW_REV  0x114000
#define L43_PATCH_START 0x114004
#define L43_NEED_CFG    0x11400C
#define L43_BOOT_STATUS 0x11401C
#define L43_SH_NEED_CFG 0x11F8F8
#define L43_MC_NEED_CFG 0x11FE00
#define L43_MC_HAVE_CFG 0x11FE04
#define L43_MM_CTRL_SEL 0x11FE0C
#define L43_MM_MCU_CFG  0x11FE10
/* CS42L43B */
#define L43B_DECIM12    0x8280
#define L43B_DECIM56    0x8290
#define L43B_MCU_SW_REV 0x117314
#define L43B_NEED_CFG   0x117320
#define L43B_BOOT_STATUS 0x117330
#define L43B_MM_CTRL_SEL 0x117E0C
#define L43B_MM_MCU_CFG 0x117E10

#define L43_ID_A        0x042A43UL
#define L43_ID_B        0x42A43BUL

/* interrupt status registers (cleared by reading) */
#define EV_PLL          0           /* PLL_INT: bit 0 ready */
#define EV_MSM          1           /* MSM_INT */
#define EV_AMP          2           /* CLASS_D_AMP_INT */
#define EV_HP           3           /* HPOUT_INT: bit 0 load detect done */
#define MSM_HP_UP       0x0800
#define MSM_HP_DOWN     0x0400
#define MSM_HSDET_DONE  0x0200
#define AMP_DOWN        0x0300      /* AMP2 / AMP1 shutdown done */
#define AMP_UP          0x00C0      /* AMP2 / AMP1 startup done */

/* priv[] */
#define P_VAR           0           /* 1: CS42L43B */
#define P_RST           1           /* soft resets in a row during the boot */
#define P_GAIN          2           /* capture level | bit 16 once set */
#define P_ON            3           /* SF_* powered */
#define P_EV            4           /* 4..7: pending interrupt status bits */
#define P_JST           8           /* jack detection state */
#define P_JCNT          9           /* polls in the state */
#define P_JTYPE         10          /* 0, 1 headphones, 3 headset */
#define P_JMIC          11          /* load detection for a 4-pole plug */
#define P_DCA           12          /* decimator settings saved while a path powers up */
#define P_DCD           13
#define P_SPKV          14          /* output volume steps: left | right << 8 | bit 16 */
#define P_ROM           15          /* the MCU runs its ROM firmware (no patch) */

#define J_IDLE          0
#define J_BIAS          1           /* headset bias ramping for the type detection */
#define J_TYPE          2           /* type detection running */
#define J_LOAD          3           /* headphone load detection running */
#define J_DONE          4

static const u32 l43_evregs[4] = { 0x1B00CUL, 0x1B018UL, 0x1B030UL, 0x1B03CUL };

/* revision A register patch (cs42l43_reva_patch) */
static const u32 l43_patch[] = {
    0x4000, 0x55, 0x4000, 0xAA, 0x10084, 0, 0x1741C, 0x00CD2000UL, 0x1718C, 3,
    0x4000, 0, 0x7010, 2, L43_HPPATHVOL, 0x011B011BUL, L43_OSC_DIV, 1,
    L43_DACCNFG2, 5, 0x110B0, 0x80790079UL, 0x300C, 0x0F
};
/* headphone load detection settings for 3-pole and 4-pole plugs */
static const u32 l43_3pole[] = {
    0x4000, 0x55, 0x4000, 0xAA, 0x17420, 0x8500F300UL, 0x17424, 0x36003E00UL, 0x4000, 0
};
static const u32 l43_4pole[] = {
    0x4000, 0x55, 0x4000, 0xAA, 0x17420, 0x7800E600UL, 0x17424, 0x36003800UL, 0x4000, 0
};

/* DP5 headphones, DP6 speakers (DPn RX), DP2 headset microphone, DP1 digital microphones */
static const struct sdwport l43_ports[] = {
    { SF_JACK_OUT, 5, 2, SPF_FULL | SPF_PREPARE },
    { SF_SPK, 6, 2, SPF_FULL | SPF_PREPARE },
    { SF_JACK_IN, 2, 2, SPF_FULL | SPF_PREPARE },
    { SF_DMIC, 1, 2, SPF_FULL | SPF_PREPARE }
};

#define IS_B(s)         ((s)->priv[P_VAR] != 0)
#define L43_DEC_ADC(s)  (IS_B(s) ? L43B_DECIM12 : L43_DECIM12)
#define L43_DEC_DMIC(s) (IS_B(s) ? L43B_DECIM56 : L43_DECIM34)

static u32 ev_get(struct sdwdev *s, int i)
{
    u32 v;
    if (!rd32(s, l43_evregs[i], &v)) s->priv[P_EV + i] |= v;
    return s->priv[P_EV + i];
}

static void ev_clear(struct sdwdev *s, int i, u32 bits)
{
    ev_get(s, i);
    s->priv[P_EV + i] &= ~bits;
}

static int ev_wait(struct sdwdev *s, int i, u32 bits, u32 ms)
{
    u32 t;
    for (t = 0; t <= ms; t++) {
        if ((ev_get(s, i) & bits) == bits) {
            s->priv[P_EV + i] &= ~bits;
            return 0;
        }
        mdelay(1);
    }
    return -1;
}

static int fls32(u32 x)
{
    int n = 0;
    while (x) {
        n++;
        x >>= 1;
    }
    return n;
}

/* ---- MCU boot (cs42l43_mcu_update) */

/* soft reset: the chip leaves the bus and enumerates again */
static void l43_reset(struct sdwdev *s)
{
    wr32(s, L43_SFT_RESET, 0x5A000000UL);
    mdelay(20);
}

static struct fwimg l43_img;

/* the patch file next to the driver: 0 loaded, 1 none */
static int l43_fwfile(void)
{
    memset(&l43_img, 0, sizeof(l43_img));
    if (plat_fw_load("CS42L43.BIN", &l43_img) || l43_img.size < 32) return 1;
    return 0;
}

/* download cs42l43.bin into the MCU patch RAM: 0 applied, 1 no file, -1 error */
static int l43_patchfw(struct sdwdev *s)
{
    const u8 *p;
    u32 load, n, i;
    if (l43_fwfile()) return 1;
    p = l43_img.v;
    if ((p[0] | ((u32)p[1] << 8)) != 3) {
        kprintf("cs42l43: cs42l43.bin: bad format %lu\n", p[0] | ((u32)p[1] << 8));
        return -1;
    }
    load = p[28] | ((u32)p[29] << 8) | ((u32)p[30] << 16) | ((u32)p[31] << 24);
    kprintf("cs42l43: loading cs42l43.bin (%lu bytes)\n", l43_img.size);
    wr32(s, L43_PATCH_START, load);
    n = l43_img.size & ~3UL;
    for (i = 0; i < n; i++)
        if (sdw_wr(s, load + 0x100000UL + i, p[i])) return -1;
    wr32(s, L43_MCU_SW_INT, 0x01);
    wr32(s, L43_MCU_SW_INT, 0);
    if (poll32(s, L43_SOFT_SHADOW, 0x01, 0x01, 500)) {
        kprintf("cs42l43: patch not applied\n");
        return -1;
    }
    return 0;
}

/* one step of the firmware state machine: 0 done, 1 again, 2 soft reset issued, -1 error */
static int l43_step(struct sdwdev *s)
{
    u32 st, raw, mcu, bios, sec, dummy, need;
    u32 bootreg = IS_B(s) ? L43B_BOOT_STATUS : L43_BOOT_STATUS;
    int patched, shadow, r;
    rd32(s, L43_SOFT_INT, &dummy);
    if (rd32(s, bootreg, &st) || rd32(s, IS_B(s) ? L43B_MCU_SW_REV : L43_MCU_SW_REV, &raw) ||
        rd32(s, L43_BOOT_CTRL, &sec)) return -1;
    bios = (((raw & 0x000F0000UL) << 12) | ((raw & 0x00F00000UL) << 4) |
            ((raw & 0xFF000000UL) >> 8)) >> 16;
    mcu = ((raw & 0x0FUL) << 12) | ((raw & 0xF0UL) << 4) | ((raw & 0xFF00UL) >> 8);
    patched = mcu != 0x2001 || bios != 0;
    shadow = mcu >= 0x2200 || bios >= 0x1002;
    DBG("cs42l43: firmware %04lx bios %04lx, boot stage %lu\n", mcu, bios, st);
    if (!patched && (sec & 0x02)) {
        kprintf("cs42l43: unpatched secure device\n");
        return -1;
    }
    switch (st) {
    case 2:
        if (!patched) {
            r = l43_patchfw(s);
            if (r < 0) return -1;
            if (!r) return 1;
            /* no patch: let the ROM firmware go on to stage 3 and keep it */
            kprintf("cs42l43: no cs42l43.bin, ROM firmware %04lx keeps running\n", mcu);
            s->priv[P_ROM] = 1;
        }
        need = IS_B(s) ? L43B_NEED_CFG : shadow ? L43_SH_NEED_CFG : L43_NEED_CFG;
        wr32(s, need, 0);
        if (poll32(s, bootreg, 0xFFFFFFFFUL, 3, 25)) {
            kprintf("cs42l43: no boot stage 3\n");
            return -1;
        }
        return s->priv[P_ROM] ? 0 : 1;
    case 3:
        if (patched) {
            if (mcu < 0x2105 && bios < 0x0001) {
                kprintf("cs42l43: firmware too old to be disabled\n");
                return -1;
            }
            /* disable the MCU so it leaves the registers alone, then reset */
            wr32(s, IS_B(s) ? L43B_MM_MCU_CFG : L43_MM_MCU_CFG, 0xF05AA50FUL);
            wr32(s, IS_B(s) ? L43B_MM_CTRL_SEL : L43_MM_CTRL_SEL, 0x01);
            wr32(s, L43_MCU_SW_INT, 0x04);
            wr32(s, L43_MCU_SW_INT, 0);
            if (poll32(s, L43_SOFT_SHADOW, 0x10, 0x10, 25)) {
                kprintf("cs42l43: firmware not disabled\n");
                return -1;
            }
            l43_reset(s);
            return 2;
        }
        if (!l43_fwfile()) {
            /* back to stage 2 to take the patch */
            wr32(s, L43_MC_NEED_CFG, 0x80000000UL);
            wr32(s, L43_MC_HAVE_CFG, 0);
            l43_reset(s);
            return 2;
        }
        s->priv[P_ROM] = 1;
        kprintf("cs42l43: no cs42l43.bin, ROM firmware %04lx keeps running\n", mcu);
        return 0;
    case 4:
        return 0;
    default:
        kprintf("cs42l43: boot stage %lu\n", st);
        return -1;
    }
}

/* ---- clocks and power */

/* PLL from the SoundWire clock (cs42l43_enable_pll / disable_pll) */
static int l43_pll(struct sdwdev *s, int on)
{
    static const u32 cfg[3][4] = {
        { 2400000UL, 0x50000000UL, 1, 0xA4 },
        { 3000000UL, 0x40000000UL, 1, 0x83 },
        { 3072000UL, 0x40000000UL, 3, 0x80 }
    };
    u32 hz = bus_hz(s), f;
    int i, div;
    if (!on) {
        wr32(s, L43_MCLK_SRC, 0);
        udelay(5);
        wr32(s, L43_OSC_DIV, 1);
        upd32(s, L43_CTRL_REG, 0x01, 0);
        upd32(s, L43_PLL_CONTROL, 0x08, 0);
        return 0;
    }
    div = fls32(hz) - fls32(3072000UL);
    if (div < 0) div = 0;
    f = hz >> div;
    for (i = 0; i < 3; i++)
        if (cfg[i][0] == f) break;
    if (i == 3 || div > 3) {
        kprintf("cs42l43: no PLL setting for a %lu Hz bus clock\n", hz);
        return -1;
    }
    upd32(s, L43_PLL_CONTROL, 0x07, ((u32)div << 1) | 1);  /* reference: SoundWire clock */
    wr32(s, L43_FDIV_FRAC, cfg[i][1]);
    upd32(s, L43_CTRL_REG, 0x06, cfg[i][2] << 1);
    upd32(s, L43_CAL_RATIO, 0xFF, cfg[i][3]);
    upd32(s, L43_PLL_CONTROL, 0x08, 0x08);
    ev_clear(s, EV_PLL, 0x01);
    upd32(s, L43_CTRL_REG, 0x01, 0x01);
    if (ev_wait(s, EV_PLL, 0x01, 200)) {
        upd32(s, L43_CTRL_REG, 0x01, 0);
        upd32(s, L43_PLL_CONTROL, 0x08, 0);
        kprintf("cs42l43: PLL does not lock\n");
        return -1;
    }
    /* no reads until the clock runs from the PLL */
    wr32(s, L43_OSC_DIV, 0);
    wr32(s, L43_MCLK_SRC, 1);
    udelay(5);
    return 0;
}

/* headphone amplifier (BLOCK_EN8), waiting for its startup / shutdown */
static void l43_hp(struct sdwdev *s, int on)
{
    u32 bit = on ? MSM_HP_UP : MSM_HP_DOWN;
    ev_clear(s, EV_MSM, bit);
    upd32(s, L43_BLOCK_EN8, 0x01, on ? 0x01 : 0);
    if (ev_wait(s, EV_MSM, bit, 2000)) kprintf("cs42l43: headphone %s timed out\n", on ? "startup" : "shutdown");
}

/* output volumes from priv[P_SPKV] (steps left | right << 8, bit 16 set once known):
   headphones 0.5 dB steps -63.5 .. 0 dB (step 0 at the muting minimum), speakers 0.5 dB steps
   up to 0 dB as the machine driver limits them, muted while the speaker path is off; the
   speaker volume is latched by the volume update bit */
static void l43_outvol(struct sdwdev *s, int hp)
{
    u32 l = s->priv[P_SPKV] & 0xFF, r = (s->priv[P_SPKV] >> 8) & 0xFF;
    int on = (s->priv[P_ON] & SF_SPK) != 0;
    if (hp) wr32(s, L43_HPPATHVOL, (l ? (l - 128) & 0x1FF : 0x11B) | ((r ? (r - 128) & 0x1FF : 0x11B) << 16));
    wr32(s, L43_INTP_VOL1, (on && l ? 0 : 0x100) | (l ? l : 0x80));
    wr32(s, L43_INTP_VOL2, (on && r ? 0 : 0x100) | (r ? r : 0x80));
    upd32(s, L43_INTP_VOL1, 0x200, 0x200);
    upd32(s, L43_INTP_VOL1, 0x200, 0);
}

/* capture gain from priv[P_GAIN] (level | bit 16): headset microphone ADC PGA -6 .. +18 dB in
   6 dB steps, digital microphone decimators -20 .. +12 dB in 0.5 dB steps; level 0 mutes the
   decimators */
static void l43_ingain(struct sdwdev *s)
{
    u32 lv = s->priv[P_GAIN] & 0xFFFF, g = cdc_step(lv, 5), d = cdc_step(lv, 64), v;
    upd32(s, L43_ADC_B_CTRL1, 0x0F, g ? (g - 2) & 0x0F : 0x0F);
    upd32(s, L43_DEC_ADC(s), 0x8000, g ? 0 : 0x8000);
    v = (0x80 - 40 + d) & 0xFF;
    upd32(s, L43_DEC_DMIC(s), 0x80008000UL | 0x3FC03FC0UL, (d ? 0 : 0x80008000UL) | (v << 6) | (v << 22));
}

/* decimator paths: muted while the input powers up, then the saved settings back
   (cs42l43_mic_ev) */
static void l43_mic(struct sdwdev *s, u32 reg, u32 mute, u32 ramp, u32 enbits, u32 *save, int on)
{
    if (!on) {
        if (s->priv[P_JST] != J_LOAD || !(enbits & 0x03)) upd32(s, L43_BLOCK_EN3, enbits, 0);
        return;
    }
    if (rd32(s, reg, save)) *save = 0;
    upd32(s, reg, mute | ramp, mute);
    if (s->priv[P_JST] != J_LOAD || !(enbits & 0x03)) upd32(s, L43_BLOCK_EN3, enbits, enbits);
    upd32(s, reg, mute | ramp, *save);
}

static void l43_func(struct sdwdev *s, int func, int on)
{
    switch (func) {
    case SF_JACK_OUT:
        if (s->priv[P_JST] != J_LOAD) l43_hp(s, on);
        break;
    case SF_SPK:
        if (on) {
            ev_clear(s, EV_AMP, AMP_UP);
            upd32(s, L43_BLOCK_EN10, 0x03, 0x03);
            if (ev_wait(s, EV_AMP, AMP_UP, 100)) kprintf("cs42l43: speaker startup timed out\n");
        } else {
            ev_clear(s, EV_AMP, AMP_DOWN);
            upd32(s, L43_BLOCK_EN10, 0x03, 0);
            ev_wait(s, EV_AMP, AMP_DOWN, 100);
        }
        break;
    case SF_JACK_IN:
        l43_mic(s, L43_DEC_ADC(s), 0x8000, 0x38, 0x01, &s->priv[P_DCA], on);
        break;
    case SF_DMIC:
        l43_mic(s, L43_DEC_DMIC(s), 0x80008000UL, 0x00380038UL, 0x30, &s->priv[P_DCD], on);
        break;
    }
}

static void l43_stream(struct sdwdev *s, int func, int on)
{
    u32 was = s->priv[P_ON];
    if (on ? (was & (u32)func) != 0 : !(was & (u32)func)) return;
    if (on) {
        if (!was) {
            l43_pll(s, 1);
            upd32(s, L43_BLOCK_EN6, 0x01, 0x01);            /* mixer core */
        }
        upd32(s, L43_RATE1, 0x1F, 0x03);                    /* 48 kHz */
        l43_func(s, func, 1);
        s->priv[P_ON] |= (u32)func;
        if (func == SF_SPK) l43_outvol(s, 0);
        return;
    }
    s->priv[P_ON] &= ~(u32)func;
    if (func == SF_SPK) l43_outvol(s, 0);
    l43_func(s, func, 0);
    if (!s->priv[P_ON]) {
        upd32(s, L43_BLOCK_EN6, 0x01, 0);
        l43_pll(s, 0);
    }
}

static void l43_volume(struct sdwdev *s, int ctl, u32 lr)
{
    if (ctl == MX_MASTER) {
        s->priv[P_SPKV] = 0x10000UL | cdc_vstep(CDC_L(lr), 128, 500) | (cdc_vstep(CDC_R(lr), 128, 500) << 8);
        l43_outvol(s, 1);
    } else if (ctl == MX_INGAIN) {
        s->priv[P_GAIN] = 0x10000UL | CDC_L(lr);
        l43_ingain(s);
    }
}

/* ---- jack (cs42l43-jack.c; polled instead of the tip sense and detection interrupts) */

static int l43_tip(struct sdwdev *s)
{
    u32 v;
    if (rd32(s, L43_TIPRING_STS, &v)) return -1;
    return ((v >> 2) & 3) == 3;
}

static void l43_bias_on(struct sdwdev *s, int type_detect)
{
    upd32(s, L43_HS2, 0x10000000UL, 0x10000000UL);          /* clamp off */
    if (!type_detect) upd32(s, L43_HSB_AUTO, 0xC0, 0xC0);   /* bias sense, auto clamp */
    upd32(s, L43_MICDET1, 0x06, 0x06);
}

static void l43_bias_off(struct sdwdev *s)
{
    upd32(s, L43_MICDET1, 0x06, 0x02);
    upd32(s, L43_HS2, 0x10000000UL, 0);
    upd32(s, L43_HSB_AUTO, 0xC0, 0);
}

static void l43_type_end(struct sdwdev *s)
{
    upd32(s, L43_HS2, 0x18000UL, 0x10000UL);                 /* detect mode 2 */
    l43_bias_off(s);
    upd32(s, L43_SMIC_CLAMP, 0x02, 0);
}

static void l43_load_start(struct sdwdev *s, int mic)
{
    if (s->priv[P_ON] & SF_JACK_OUT) l43_hp(s, 0);
    seq32(s, mic ? l43_4pole : l43_3pole, 5);
    upd32(s, L43_BLOCK_EN3, 0x03, 0);
    upd32(s, L43_DACCNFG2, 0x01, 0);
    upd32(s, L43_MICDET1, 0x06, 0);
    upd32(s, L43_CTRL, 0x07, 0x04);
    upd32(s, L43_PGAVOL, 0x0F0F, 0x0006);
    upd32(s, L43_DACCNFG1, 0x08, 0);
    upd32(s, L43_HS2, 0x10000000UL, 0x10000000UL);
    ev_clear(s, EV_HP, 0x01);
    upd32(s, L43_LOADDET_EN, 0x01, 0x01);
}

static void l43_load_stop(struct sdwdev *s)
{
    upd32(s, L43_LOADDET_EN, 0x01, 0);
    upd32(s, L43_HS2, 0x10000000UL, 0);
    upd32(s, L43_DACCNFG1, 0x08, 0x08);
    upd32(s, L43_PGAVOL, 0x0F0F, 0x0400);
    upd32(s, L43_CTRL, 0x07, 0x07);
    upd32(s, L43_MICDET1, 0x06, 0x02);
    upd32(s, L43_DACCNFG2, 0x01, 0x01);
    upd32(s, L43_BLOCK_EN3, 0x03, (s->priv[P_ON] & SF_JACK_IN) ? 0x01 : 0);
    s->priv[P_JST] = J_DONE;
    if (s->priv[P_ON] & SF_JACK_OUT) l43_hp(s, 1);
}

/* jack removed: detection stopped, back to the idle settings (cs42l43_clear_jack) */
static void l43_clear(struct sdwdev *s)
{
    if (s->priv[P_JST] == J_LOAD) l43_load_stop(s);
    else if (s->priv[P_JST] == J_BIAS || s->priv[P_JST] == J_TYPE) l43_type_end(s);
    upd32(s, L43_MICDET1, 0x19, 0x01);                      /* button detection off */
    l43_bias_off(s);
    upd32(s, L43_ADC_B_CTRL1, 0x80, 0);
    upd32(s, L43_ADC_B_CTRL1 + 4, 0x80, 0);
    upd32(s, L43_SMIC_CTRL, 0x03, 0);
    upd32(s, L43_SMIC_CLAMP, 0x01, 0x01);
    upd32(s, L43_HS2, 0x1E000UL, 0x10000UL);
    s->priv[P_JST] = J_IDLE;
    s->priv[P_JTYPE] = 0;
}

static int l43_jack(struct sdwdev *s)
{
    u32 v;
    int tip = l43_tip(s);
    if (tip < 0) return -1;
    if (!tip) {
        if (s->priv[P_JST] != J_IDLE || s->priv[P_JTYPE]) l43_clear(s);
        return 0;
    }
    switch (s->priv[P_JST]) {
    case J_IDLE:
        /* plugged: headset bias up for the type detection, it ramps until the next poll */
        upd32(s, L43_SMIC_CLAMP, 0x02, 0x02);
        l43_bias_on(s, 1);
        s->priv[P_JST] = J_BIAS;
        break;
    case J_BIAS:
        ev_clear(s, EV_MSM, MSM_HSDET_DONE);
        upd32(s, L43_HS2, 0x18000UL, 0x18000UL);            /* detect mode 3: run */
        s->priv[P_JST] = J_TYPE;
        s->priv[P_JCNT] = 0;
        break;
    case J_TYPE:
        if (!(ev_get(s, EV_MSM) & MSM_HSDET_DONE) && ++s->priv[P_JCNT] < 3) break;
        s->priv[P_EV + EV_MSM] &= ~(u32)MSM_HSDET_DONE;
        l43_type_end(s);
        if (s->priv[P_JCNT] >= 3 || rd32(s, L43_HS_STAT, &v)) {
            kprintf("cs42l43: jack type detection timed out\n");
            s->priv[P_JTYPE] = 1;
            s->priv[P_JST] = J_DONE;
            break;
        }
        DBG("cs42l43: jack type %lu\n", v & 7);
        switch (v & 7) {
        case 0: case 1: case 4:                             /* CTIA, OMTP */
            s->priv[P_JMIC] = 1;
            break;
        case 2:                                             /* 3-pole */
            s->priv[P_JMIC] = 0;
            break;
        default:                                            /* open: extension cable */
            s->priv[P_JTYPE] = 0;
            s->priv[P_JST] = J_DONE;
            return 0;
        }
        l43_load_start(s, (int)s->priv[P_JMIC]);
        s->priv[P_JST] = J_LOAD;
        s->priv[P_JCNT] = 0;
        break;
    case J_LOAD:
        if (!(ev_get(s, EV_HP) & 0x01) && ++s->priv[P_JCNT] < 4) break;
        s->priv[P_EV + EV_HP] &= ~1UL;
        l43_load_stop(s);
        if (rd32(s, L43_LOADDET_RES, &v)) v = 0;
        DBG("cs42l43: headphone load %lu\n", v & 3);
        if (s->priv[P_JMIC]) {
            /* headset: bias for the microphone, button detection */
            l43_bias_on(s, 0);
            mdelay(170);
            upd32(s, L43_MICDET1, 0x19, 0x18);
            s->priv[P_JTYPE] = 3;
        } else {
            s->priv[P_JTYPE] = 1;                           /* headphones or line out */
        }
        break;
    }
    return (int)s->priv[P_JTYPE];
}

/* ---- init */

/* accessory detection with the Linux defaults (cs42l43_set_jack): tip sense inverted, 500 ms
   debounce, 50 ms detect time, 170 ms bias ramp, 14 uA bias sense, pull-up on the tip */
static void l43_jack_setup(struct sdwdev *s)
{
    upd32(s, L43_TIP_DEB, 0xBF, 0x80 | (3 << 3) | 3);
    upd32(s, L43_HS2, 0x0C018700UL, (3UL << 26) | (2UL << 15) | (4UL << 8));
    upd32(s, L43_HSB_AUTO, 0xC0800007UL, (3UL << 30) | 0x00800000UL);
    upd32(s, L43_PDNCNTL, 0x02, 0);
}

static int l43_init(struct sdwdev *s)
{
    u32 id, rev = 0, otp = 0, dmic;
    int i, r = -1;
    wr32(s, L43_OSC_DIV, 1);                                /* needed before reads */
    if (rd32(s, L43_DEVID, &id)) return -1;
    id &= 0xFFFFFFUL;
    if (id != (s->part == 0x2A3B ? L43_ID_B : L43_ID_A)) {
        kprintf("cs42l43: device id %06lx\n", id);
        return -1;
    }
    s->priv[P_VAR] = id == L43_ID_B;
    rd32(s, L43_REVID, &rev);
    rd32(s, L43_OTP_REV, &otp);
    DBG("cs42l43: devid %06lx rev %02lx otp %02lx\n", id, rev, otp);
    s->priv[P_ROM] = 0;
    for (i = 0; i < 5; i++) {
        r = l43_step(s);
        if (r != 1) break;
        wr32(s, L43_OSC_DIV, 1);
    }
    if (r == 2) {
        /* soft reset: init runs again after the chip has enumerated again */
        if (++s->priv[P_RST] > 3) return -1;
        return 1;
    }
    if (r) return -1;
    s->priv[P_RST] = 0;
    seq32(s, l43_patch, ARRAY_SIZE(l43_patch) / 2);
    /* routes (UCM sets them on Linux): DP5 to the headphone mixers, DP6 to the speaker mixers (0 dB),
       decimator 1 (ADC1 IN1, the headset microphone) to DP2, PDM2 decimators to DP1 */
    wr32(s, L43_AMP3MIX1, 0x00800028UL);
    wr32(s, L43_AMP4MIX1, 0x00800029UL);
    wr32(s, L43_AMP1MIX1, 0x0080002AUL);
    wr32(s, L43_AMP2MIX1, 0x0080002BUL);
    wr32(s, L43_DP2_CH1, 0x10);
    wr32(s, L43_DP2_CH2, 0x10);
    dmic = IS_B(s) ? 0x80 : 0x12;
    wr32(s, L43_DP1_CH1, dmic);
    wr32(s, L43_DP1_CH2, dmic + 1);
    upd32(s, L43_ADC_B_CTRL1, 0x10, 0);                     /* ADC1 from IN1 */
    upd32(s, L43_RATE1, 0x1F, 0x03);
    s->priv[P_ON] = 0;
    for (i = 0; i < 4; i++) s->priv[P_EV + i] = 0;
    if (!(s->priv[P_SPKV] & 0x10000UL)) s->priv[P_SPKV] = 0x10000UL | 128 | (128 << 8);
    l43_outvol(s, 1);                                       /* 0 dB until volume() */
    if (s->priv[P_GAIN] & 0x10000UL) l43_ingain(s);
    l43_jack_setup(s);
    s->priv[P_JST] = J_IDLE;
    s->priv[P_JTYPE] = 0;
    return 0;
}

static void l43_off(struct sdwdev *s)
{
    static const int funcs[4] = { SF_JACK_OUT, SF_SPK, SF_JACK_IN, SF_DMIC };
    int i;
    for (i = 0; i < 4; i++)
        if (s->priv[P_ON] & (u32)funcs[i]) l43_stream(s, funcs[i], 0);
    l43_clear(s);
}

const struct sdwdrv sdw_cs42l43 = {
    "cs42l43", MFR_CIRRUS, 0x4243, SF_JACK_OUT | SF_JACK_IN | SF_SPK | SF_DMIC, 4, l43_ports,
    l43_init, l43_stream, l43_volume, l43_jack, l43_off
};

const struct sdwdrv sdw_cs42l43b = {
    "cs42l43b", MFR_CIRRUS, 0x2A3B, SF_JACK_OUT | SF_JACK_IN | SF_SPK | SF_DMIC, 4, l43_ports,
    l43_init, l43_stream, l43_volume, l43_jack, l43_off
};

/* ================================================================ CS35L56 / 57 / 62 / 63 */

#define L56(r)          ((r) + 0x8000UL)    /* register addresses on SoundWire */
#define L56_DEVID       0x0000000UL
#define L56_REVID       0x0000004UL
#define L56_OTPID       0x0000010UL
#define L56_ASP1_CTRL3  0x000480CUL
#define L56_DP3_CH1     0x0004C70UL
#define L56_IRQ1_STATUS 0x000E004UL
#define L56_IRQ1_MASK1  0x000E090UL
#define L56_IRQ1_MASK8  0x000E0ACUL
#define L56_IRQ1_MASK18 0x000E0D4UL
#define L56_IRQ1_MASK20 0x000E0DCUL
#define L56_MBOX1       0x0011020UL
#define L56_RESTRICT    0x00190F0UL
#define L56_FW_VER      0x2800010UL

#define MBOX_PLAY       0x0B000001UL
#define MBOX_PAUSE      0x0B000002UL
#define MBOX_NO_HIB     0x02000003UL

/* firmware registers (cs35l56_fw_reg tables) */
#define R_HALO          0
#define R_PROT          1
#define R_PS            2
#define R_MUTE          3
#define R_VOL           4
#define R_POSTURE       5

/* priv[] */
#define P56_TYPE        0           /* 0x56, 0x57, 0x62, 0x63 */
#define P56_REV         1
#define P56_ON          2
#define P56_VOL         3           /* volume step + 1 (0: not set) */

static const struct sdwport l56_ports[] = { { SF_SPK, 1, 2, SPF_FULL | SPF_PREPARE } };

static u32 l56_fwreg(struct sdwdev *s, int r)
{
    static const u32 l56[6] = { 0x2803D20UL, 0x34000D8UL, 0x3400150UL, 0x3400024UL, 0x340002CUL, 0x3400094UL };
    static const u32 l63[6] = { 0x2803C04UL, 0x340009CUL, 0x34000F4UL, 0x3400020UL, 0x3400028UL, 0x3400068UL };
    if (s->priv[P56_TYPE] == 0x62 || s->priv[P56_TYPE] == 0x63) return l63[r];
    if (r == R_HALO && s->priv[P56_REV] == 0xB0) return 0x28021E0UL;
    return l56[r];
}

static int l56_rd(struct sdwdev *s, u32 reg, u32 *v)
{
    return rd32(s, L56(reg), v);
}

static int l56_wr(struct sdwdev *s, u32 reg, u32 v)
{
    return wr32(s, L56(reg), v);
}

/* mailbox command to the firmware, acknowledged by MBOX_1 going back to 0 */
static int l56_mbox(struct sdwdev *s, u32 cmd)
{
    l56_wr(s, L56_MBOX1, cmd);
    if (poll32(s, L56(L56_MBOX1), 0xFFFFFFFFUL, 0, 5)) {
        kprintf("%s: mailbox command %08lx failed\n", s->drv->name, cmd);
        return -1;
    }
    return 0;
}

static int l56_init(struct sdwdev *s)
{
    u32 v, id, sec = 0, otp = 0, prot = 0, ver = 0, t;
    s->priv[P56_TYPE] = s->part & 0xFF;
    s->priv[P56_ON] = 0;
    /* wake: two accesses 1 ms apart, then the control port ready time */
    l56_rd(s, L56_IRQ1_STATUS, &v);
    mdelay(1);
    l56_rd(s, L56_IRQ1_STATUS, &v);
    udelay(2200);
    if (l56_rd(s, L56_REVID, &v)) return -1;
    s->priv[P56_REV] = v & 0xFF;
    /* the ROM firmware boots by itself: HALO_STATE reaches BOOT_DONE */
    for (t = 0; t <= 250; t++) {
        if (!l56_rd(s, l56_fwreg(s, R_HALO), &v) && v >= 2 && v < 0xFFFF) break;
        mdelay(1);
    }
    if (t > 250) {
        kprintf("%s: firmware boot timed out\n", s->drv->name);
        return -1;
    }
    if (l56_rd(s, L56_DEVID, &id)) return -1;
    id &= 0xFFFFFFUL;
    if (id != 0x35A54UL && id != 0x35A56UL && id != 0x35A57UL && id != 0x35A630UL && id != 0x35A620UL) {
        kprintf("%s: unknown device %06lx\n", s->drv->name, id);
        return -1;
    }
    l56_rd(s, L56_RESTRICT, &sec);
    l56_rd(s, L56_OTPID, &otp);
    l56_rd(s, l56_fwreg(s, R_PROT), &prot);
    l56_rd(s, L56_FW_VER, &ver);
    kprintf("%s: CS35L%02lX%s rev %02lX OTP%lu firmware %lu.%lu.%lu (%s)\n", s->drv->name,
            s->priv[P56_TYPE], (sec & 7) ? "s" : "", s->priv[P56_REV], otp, ver >> 16, (ver >> 8) & 0xFF,
            ver & 0xFF, (prot & 1) ? "ROM, no tuning" : "patched");
    /* wake source and blocked interrupts masked, short / overtemperature unmasked */
    l56_wr(s, L56_IRQ1_MASK20, 0xFFFFFFFFUL);
    upd32(s, L56(L56_IRQ1_MASK1), 0x80000000UL, 0);
    upd32(s, L56(L56_IRQ1_MASK8), 0x80000000UL, 0);
    /* cs35l56_set_patch: registers the firmware may have changed back to defaults */
    l56_wr(s, L56_DP3_CH1, 0x18);
    l56_wr(s, L56_DP3_CH1 + 4, 0x19);
    l56_wr(s, L56_DP3_CH1 + 8, 0x29);
    l56_wr(s, L56_DP3_CH1 + 12, 0x28);
    l56_wr(s, L56_IRQ1_MASK18, 0x1F7DF0FFUL);
    l56_wr(s, l56_fwreg(s, R_MUTE), 0);
    l56_wr(s, l56_fwreg(s, R_VOL), 0);
    l56_wr(s, l56_fwreg(s, R_POSTURE), 0);
    /* the driver controls the power states: no auto hibernation */
    if (l56_mbox(s, MBOX_NO_HIB)) return -1;
    upd32(s, L56(L56_ASP1_CTRL3), 0x03, 0x03);              /* ASP1 DOUT Hi-Z when idle */
    if (s->priv[P56_VOL]) {
        v = s->priv[P56_VOL] - 1;
        l56_wr(s, l56_fwreg(s, R_VOL), ((u32)((s32)v - 256) & 0x3FF) << 6);
        l56_wr(s, l56_fwreg(s, R_MUTE), v ? 0 : 1);
    }
    return 0;
}

/* the SoundWire data path (DP1 -> DSP1 -> amplifier) is set up by the firmware */
static void l56_stream(struct sdwdev *s, int func, int on)
{
    if (func != SF_SPK || s->priv[P56_ON] == (u32)on) return;
    s->priv[P56_ON] = (u32)on;
    if (on) {
        l56_wr(s, L56_MBOX1, MBOX_PLAY);
        if (poll32(s, L56(l56_fwreg(s, R_PS)), 0xFFFFFFFFUL, 0, 50))
            kprintf("%s: no PS0 after play\n", s->drv->name);
    } else {
        l56_mbox(s, MBOX_PAUSE);
    }
}

/* firmware volume: 0.25 dB steps, -64 .. 0 dB (the machine driver limit), this amplifier's
   side of the stereo level */
static void l56_volume(struct sdwdev *s, int ctl, u32 lr)
{
    u32 st;
    if (ctl != MX_MASTER) return;
    st = cdc_vstep((s->namps > 1 && (s->chan & 1)) ? CDC_R(lr) : CDC_L(lr), 256, 125);
    s->priv[P56_VOL] = st + 1;
    l56_wr(s, l56_fwreg(s, R_VOL), ((u32)((s32)st - 256) & 0x3FF) << 6);
    l56_wr(s, l56_fwreg(s, R_MUTE), st ? 0 : 1);
}

static void l56_off(struct sdwdev *s)
{
    if (s->priv[P56_ON]) l56_stream(s, SF_SPK, 0);
}

const struct sdwdrv sdw_cs35l56 = {
    "cs35l56", MFR_CIRRUS, 0x3556, SF_SPK, 1, l56_ports, l56_init, l56_stream, l56_volume, NULL, l56_off
};
const struct sdwdrv sdw_cs35l57 = {
    "cs35l57", MFR_CIRRUS, 0x3557, SF_SPK, 1, l56_ports, l56_init, l56_stream, l56_volume, NULL, l56_off
};
const struct sdwdrv sdw_cs35l62 = {
    "cs35l62", MFR_CIRRUS, 0x3562, SF_SPK, 1, l56_ports, l56_init, l56_stream, l56_volume, NULL, l56_off
};
const struct sdwdrv sdw_cs35l63 = {
    "cs35l63", MFR_CIRRUS, 0x3563, SF_SPK, 1, l56_ports, l56_init, l56_stream, l56_volume, NULL, l56_off
};

/* ================================================================ CS42L42 */

/* registers: page << 8 | register, at +0x8000 on SoundWire (no paging) */
#define L42_OFS         0x8000UL
#define L42_MEM_STATUS  0xD0        /* bridge: bit 3 last read late, 2 busy, 0 data ready */
#define L42_MEM_DATA    0xD8

#define DEVID_AB        0x1001
#define DEVID_CD        0x1002
#define DEVID_E         0x1003
#define REVID           0x1005
#define MCLK_CTL        0x1009
#define PWR_CTL1        0x1101
#define PWR_CTL2        0x1102
#define PWR_CTL3        0x1103
#define OSC_SWITCH      0x1107
#define TSENSE_CTL      0x1113
#define HSDET_CTL1      0x111F
#define HSDET_CTL2      0x1120
#define HS_SWITCH_CTL   0x1121
#define HS_DET_STATUS   0x1124
#define HS_CLAMP_DIS    0x1129
#define MCLK_SRC_SEL    0x1201
#define FS_RATE_EN      0x1209
#define IN_ASRC_CLK     0x120A
#define OUT_ASRC_CLK    0x120B
#define PLL_DIV_CFG1    0x120C
#define CODEC_STATUS    0x1308
#define PLL_LOCK_STATUS 0x130E
#define TSRS_PLUG_STATUS 0x130F
#define ADC_OVFL_INT_MASK 0x1316
#define MIXER_INT_MASK  0x1317
#define SRC_INT_MASK    0x1318
#define ASP_RX_INT_MASK 0x1319
#define ASP_TX_INT_MASK 0x131A
#define CODEC_INT_MASK  0x131B
#define SRCPL_INT_MASK  0x131C
#define VPMON_INT_MASK  0x131E
#define PLL_LOCK_INT_MASK 0x131F
#define TSRS_PLUG_INT_MASK 0x1320
#define PLL_CTL1        0x1501
#define PLL_DIV_FRAC0   0x1502
#define PLL_DIV_FRAC1   0x1503
#define PLL_DIV_FRAC2   0x1504
#define PLL_DIV_INT     0x1505
#define PLL_CTL3        0x1508
#define PLL_CAL_RATIO   0x150A
#define PLL_CTL4        0x151B
#define HSBIAS_SC_AUTOCTL 0x1B70
#define WAKE_CTL        0x1B71
#define TIPSENSE_CTL    0x1B73
#define MISC_DET_CTL    0x1B74
#define MIC_DET_CTL1    0x1B75
#define DET_INT1_MASK   0x1B79
#define DET_INT2_MASK   0x1B7A
#define HS_BIAS_CTL     0x1C03
#define ADC_VOLUME      0x1D03
#define DAC_CTL2        0x1F06
#define HP_CTL          0x2001
#define MIXER_CHA_VOL   0x2301
#define MIXER_ADC_VOL   0x2302
#define MIXER_CHB_VOL   0x2303
#define ASP_RX_DAI0_EN  0x2A01
#define ASP_CLK_CFG     0x1207
#define SOFT_RESET_REBOOT 0x2701

#define L42_CHIP_ID     0x42A42UL
#define TS_PLUG         3               /* TSRS_PLUG_STATUS bits 3:2 */
#define TS_UNPLUG       0
#define TS_TRANS        1
#define PLUG_CTIA       0               /* HS_DET_STATUS type */
#define PLUG_OMTP       1
#define PLUG_HEADPHONE  2
#define PLUG_INVALID    3
#define SW_TYPE1        0xA6            /* HS_SWITCH_CTL: also the comparator 1 setting */
#define SW_TYPE2        0x59
#define SW_TYPE3        0xF3
#define HSDET_MAN_ACTIVE 0x40           /* HSDET_CTL2 */
#define HSDET_MAN_OFF   0x00
#define HSDET_MAN_SET2  0x23
#define HSDET_AUTO_OFF  0xA3
#define HSDET_AUTO_ON   0xE3
#define BIAS_LVL0       0x0F            /* HS_DET_LEVEL_15 */
#define HSBIAS_RAMP_MS  90

/* priv[] */
#define P42_USE         0               /* SF_* running */
#define P42_CLK         1               /* 1: running from the SoundWire clock */
#define P42_PLL         2               /* PLL table entry + 1, 0 none */
#define P42_PLUG        3               /* tip sense state */
#define P42_TYPE        4               /* plug type */
#define P42_DET         5               /* polls since the type detection started, 0 idle */
#define P42_SEEN        6               /* initialised before: reboot first */

/* DP2 playback, DP1 capture (one channel) */
static const struct sdwport l42_ports[] = {
    { SF_JACK_OUT, 2, 2, SPF_FULL | SPF_PREPARE },
    { SF_JACK_IN, 1, 1, SPF_FULL | SPF_PREPARE }
};

/* PLL settings for each SCLK (= SoundWire clock; datasheet table 4-5, as in Linux) */
struct l42pll {
    u32 sclk;
    u8 src, prediv, div_int;
    u32 frac;
    u8 mode, divout;
    u32 mclk_int;
    u8 cal, n;
};
static const struct l42pll l42_plls[] = {
    {  2400000UL, 1, 0x00, 0x50, 0x000000UL, 0x03, 0x10, 12000000UL,  80, 2 },
    {  3000000UL, 1, 0x00, 0x40, 0x000000UL, 0x03, 0x10, 12000000UL, 128, 1 },
    {  3072000UL, 1, 0x00, 0x3E, 0x800000UL, 0x03, 0x10, 12000000UL, 125, 1 },
    {  4800000UL, 1, 0x01, 0x50, 0x000000UL, 0x03, 0x10, 12000000UL,  80, 2 },
    {  6000000UL, 1, 0x01, 0x40, 0x000000UL, 0x03, 0x10, 12000000UL, 128, 1 },
    {  6144000UL, 1, 0x01, 0x3E, 0x800000UL, 0x03, 0x10, 12000000UL, 125, 1 },
    {  9600000UL, 1, 0x02, 0x50, 0x000000UL, 0x03, 0x10, 12000000UL,  80, 2 },
    { 12000000UL, 0, 0x00, 0x00, 0x000000UL, 0x00, 0x00, 12000000UL,   0, 1 },
    { 12288000UL, 0, 0x00, 0x00, 0x000000UL, 0x00, 0x00, 12288000UL,   0, 1 }
};

/* interrupt sources masked (the status registers latch anyway), datasheet power down */
static const u16 l42_irq_masks[] = {
    ADC_OVFL_INT_MASK, 0x01, MIXER_INT_MASK, 0x0F, SRC_INT_MASK, 0x0F, ASP_RX_INT_MASK, 0x1F,
    ASP_TX_INT_MASK, 0x0F, CODEC_INT_MASK, 0x03, SRCPL_INT_MASK, 0x7F, VPMON_INT_MASK, 0x01,
    PLL_LOCK_INT_MASK, 0x01, TSRS_PLUG_INT_MASK, 0x0F, DET_INT1_MASK, 0xE0, DET_INT2_MASK, 0xFF
};
static const u16 l42_shutdown[] = {
    MIC_DET_CTL1, 0x9F, WAKE_CTL, 0xE1, MIXER_CHA_VOL, 0x3F, MIXER_ADC_VOL, 0x3F,
    MIXER_CHB_VOL, 0x3F, HP_CTL, 0x0F, ASP_RX_DAI0_EN, 0x00, ASP_CLK_CFG, 0x00, HSDET_CTL2, 0x00,
    PWR_CTL1, 0xFE, PWR_CTL2, 0x8C, DAC_CTL2, 0x02, HS_CLAMP_DIS, 0x00, MISC_DET_CTL, 0x03,
    TIPSENSE_CTL, 0x02, HSBIAS_SC_AUTOCTL, 0x03, PWR_CTL1, 0xFF
};

/* ---- register access through the Cirrus bus bridge */

static int l42_idle(struct sdwdev *s, u8 mask, u8 match)
{
    int i, v;
    for (i = 0; i < 100; i++) {
        v = sdw_rd(s, L42_MEM_STATUS);
        if (v < 0) return -1;
        if ((v & mask) == match) return v;
        udelay(1);
    }
    return -1;
}

static int l42_rd(struct sdwdev *s, u32 reg)
{
    int v, st;
    if (l42_idle(s, 0x04, 0) < 0) return -1;
    v = sdw_rd(s, reg + L42_OFS);
    if (v < 0) return -1;
    st = sdw_rd(s, L42_MEM_STATUS);
    if (st < 0) return -1;
    if (!(st & 0x08)) return v;                     /* not a delayed read */
    if (!(st & 0x01) && l42_idle(s, 0x01, 0x01) < 0) return -1;
    return sdw_rd(s, L42_MEM_DATA);
}

static int l42_wr(struct sdwdev *s, u32 reg, u32 val)
{
    if (l42_idle(s, 0x04, 0) < 0) return -1;
    return sdw_wr(s, reg + L42_OFS, (u8)val);
}

static int l42_upd(struct sdwdev *s, u32 reg, u32 mask, u32 val)
{
    int v = l42_rd(s, reg);
    if (v < 0) return -1;
    return l42_wr(s, reg, ((u32)v & ~mask) | (val & mask));
}

static void l42_seq(struct sdwdev *s, const u16 *p, int n)
{
    int i;
    for (i = 0; i + 1 < n; i += 2) l42_wr(s, p[i], p[i + 1]);
}

static u32 l42_devid(struct sdwdev *s)
{
    int ab = l42_rd(s, DEVID_AB), cd = l42_rd(s, DEVID_CD), e = l42_rd(s, DEVID_E);
    if (ab < 0 || cd < 0 || e < 0) return 0;
    return ((u32)ab << 12) | ((u32)cd << 4) | ((u32)e >> 4);
}

/* ---- clocks (cs42l42_pll_config, cs42l42_mute_stream) */

static int l42_pll_config(struct sdwdev *s)
{
    const struct l42pll *p;
    u32 sclk = bus_hz(s);
    int i;
    for (i = 0; i < (int)ARRAY_SIZE(l42_plls); i++)
        if (l42_plls[i].sclk == sclk) break;
    if (i == (int)ARRAY_SIZE(l42_plls)) {
        kprintf("cs42l42: SoundWire clock %lu Hz not supported\n", sclk);
        return -1;
    }
    p = &l42_plls[i];
    s->priv[P42_PLL] = (u32)i + 1;
    l42_upd(s, MCLK_CTL, 0x02, p->mclk_int != 12000000UL ? 0x02 : 0x00);
    if (!p->src) {
        l42_upd(s, PLL_CTL1, 0x01, 0x00);           /* SCLK straight through */
        return 0;
    }
    l42_upd(s, PLL_DIV_CFG1, 0x03, p->prediv);
    l42_wr(s, PLL_DIV_INT, p->div_int);
    l42_wr(s, PLL_DIV_FRAC0, p->frac & 0xFF);
    l42_wr(s, PLL_DIV_FRAC1, (p->frac >> 8) & 0xFF);
    l42_wr(s, PLL_DIV_FRAC2, (p->frac >> 16) & 0xFF);
    l42_upd(s, PLL_CTL4, 0x03, p->mode);
    l42_wr(s, PLL_CTL3, (u32)p->divout * p->n);
    l42_wr(s, PLL_CAL_RATIO, p->cal);
    return 0;
}

/* the codec clock from the SoundWire clock (through the PLL when needed) */
static void l42_to_sclk(struct sdwdev *s)
{
    const struct l42pll *p;
    int i, v, lock = 0;
    if (!s->priv[P42_PLL]) return;
    p = &l42_plls[s->priv[P42_PLL] - 1];
    if (p->src) {
        l42_upd(s, PLL_CTL1, 0x01, 0x01);
        if (p->n > 1) {
            udelay(800);
            l42_wr(s, PLL_CTL3, p->divout);
        }
        for (i = 0; i < 6 && !lock; i++) {
            udelay(250);
            v = l42_rd(s, PLL_LOCK_STATUS);
            lock = v > 0 && (v & 1);
        }
        if (!lock) kprintf("cs42l42: PLL does not lock\n");
        l42_upd(s, MCLK_SRC_SEL, 0x01, 0x01);
    }
    l42_wr(s, OSC_SWITCH, 0x01);                     /* SCLK present, oscillator off */
    udelay(150);
    s->priv[P42_CLK] = 1;
}

static void l42_to_osc(struct sdwdev *s)
{
    const struct l42pll *p;
    l42_wr(s, OSC_SWITCH, 0x00);
    udelay(150);
    l42_upd(s, MCLK_SRC_SEL, 0x01, 0x00);
    udelay(150);
    l42_upd(s, PLL_CTL1, 0x01, 0x00);
    if (s->priv[P42_PLL]) {
        p = &l42_plls[s->priv[P42_PLL] - 1];
        if (p->src) l42_wr(s, PLL_CTL3, (u32)p->divout * p->n);
    }
    s->priv[P42_CLK] = 0;
}

/* ---- headset type detection (polled, after the Linux cs42l42 jack code) */

static void l42_detect_start(struct sdwdev *s)
{
    l42_upd(s, MISC_DET_CTL, 0x07, 0x03);           /* HS bias and level detect off */
    l42_upd(s, HSBIAS_SC_AUTOCTL, 0xE7, 0x03);
    l42_upd(s, HSDET_CTL2, 0xFB, HSDET_MAN_SET2);
    l42_upd(s, DAC_CTL2, 0xFF, 0x87);               /* HP outputs pulled down and clamped */
    l42_upd(s, MISC_DET_CTL, 0x07, 0x07);           /* HS bias 2.7 V */
    mdelay(HSBIAS_RAMP_MS);
    (void)l42_rd(s, CODEC_STATUS);                  /* drop an old "done" */
    l42_upd(s, HSDET_CTL2, 0xFB, HSDET_AUTO_ON);
}

/* manual detection with the two comparators (the automatic one found no headset) */
static u32 l42_detect_manual(struct sdwdev *s)
{
    int v;
    u32 c1, c2, type, sw;
    l42_upd(s, HSDET_CTL2, 0xFB, HSDET_MAN_ACTIVE);
    l42_upd(s, HSDET_CTL1, 0xFF, 0x2C);             /* 1.25 V and 1.75 V comparators */
    l42_wr(s, HS_SWITCH_CTL, SW_TYPE1);
    mdelay(100);
    v = l42_rd(s, HS_DET_STATUS);
    if (v < 0) v = 0;
    c1 = ((u32)v >> 6) & 1;
    c2 = ((u32)v >> 7) & 1;
    l42_wr(s, HS_SWITCH_CTL, SW_TYPE2);
    mdelay(100);
    v = l42_rd(s, HS_DET_STATUS);
    if (v < 0) v = 0;
    c1 |= (((u32)v >> 6) & 1) << 1;
    c2 |= (((u32)v >> 7) & 1) << 1;
    if (c1 == 1 || (c1 != 2 && c2 == 1)) {
        type = PLUG_CTIA;
        sw = SW_TYPE1;
    } else if (c1 == 2 || c2 == 2) {
        type = PLUG_OMTP;
        sw = SW_TYPE2;
    } else {
        type = PLUG_HEADPHONE;
        sw = SW_TYPE3;
    }
    l42_wr(s, HS_SWITCH_CTL, sw);
    l42_upd(s, HSDET_CTL2, 0xFB, HSDET_MAN_OFF);
    l42_upd(s, HSDET_CTL1, 0xFF, 0x77);
    return type;
}

static void l42_detect_done(struct sdwdev *s)
{
    int v = l42_rd(s, HS_DET_STATUS);
    u32 type = v < 0 ? PLUG_INVALID : (u32)v & 3;
    l42_upd(s, HSDET_CTL2, 0xFB, HSDET_AUTO_OFF);
    if (type == PLUG_INVALID || type == PLUG_HEADPHONE) type = l42_detect_manual(s);
    if (type == PLUG_CTIA || type == PLUG_OMTP) {
        /* headset: HS bias on for the microphone, short sense on */
        l42_upd(s, HSBIAS_SC_AUTOCTL, 0xE7, 0x03);
        l42_upd(s, MIC_DET_CTL1, 0xFF, 0x80 | BIAS_LVL0);
        l42_upd(s, HSBIAS_SC_AUTOCTL, 0xE7, 0x80 | 0x40 | 0x03);
        l42_upd(s, MISC_DET_CTL, 0x07, 0x06);
    } else {
        l42_upd(s, MISC_DET_CTL, 0x07, 0x03);
    }
    l42_upd(s, DAC_CTL2, 0xFF, 0x02);               /* HP outputs released, DAC high pass on */
    s->priv[P42_TYPE] = type;
}

static void l42_detect_cancel(struct sdwdev *s)
{
    l42_upd(s, MISC_DET_CTL, 0x07, 0x03);           /* HS bias grounded */
    l42_upd(s, HSBIAS_SC_AUTOCTL, 0xE7, 0x03);
    l42_upd(s, HSDET_CTL2, 0xFB, HSDET_MAN_SET2);
    s->priv[P42_TYPE] = PLUG_INVALID;
}

/* ---- driver */

static int l42_init(struct sdwdev *s)
{
    int rev, ret;
    if (s->priv[P42_SEEN]) {
        /* attached again (bus reset): a reconfiguration reset first */
        l42_wr(s, SOFT_RESET_REBOOT, 0x1E);
        mdelay(3);
    }
    memset(s->priv, 0, sizeof(s->priv));
    s->priv[P42_SEEN] = 1;
    s->priv[P42_PLUG] = TS_TRANS;                   /* a plugged jack is found at the first poll */
    s->priv[P42_TYPE] = PLUG_INVALID;
    if (l42_devid(s) != L42_CHIP_ID) {
        kprintf("cs42l42: device id %05lx\n", l42_devid(s));
        return -1;
    }
    rev = l42_rd(s, REVID);
    DBG("cs42l42: revision %02x\n", rev);
    l42_seq(s, l42_irq_masks, ARRAY_SIZE(l42_irq_masks));
    l42_wr(s, OSC_SWITCH, 0x00);                    /* internal oscillator, PLL off */
    l42_upd(s, MCLK_SRC_SEL, 0x01, 0x00);
    l42_upd(s, PLL_CTL1, 0x01, 0x00);
    /* codec on, the blocks down; SRC power by the driver (SoundWire) */
    l42_upd(s, PWR_CTL1, 0xFD, 0xFC);
    l42_upd(s, PWR_CTL2, 0x0B, 0x08);
    /* tip sense: 1 s rising, no falling debounce, not inverted (Linux defaults), HS bias slow
       ramp, headset detection setup (cs42l42_setup_hs_type_detect) */
    l42_upd(s, TSENSE_CTL, 0x3F, 5);
    l42_upd(s, HS_BIAS_CTL, 0x03, 2);
    l42_upd(s, MISC_DET_CTL, 0x18, 0x00);
    l42_upd(s, MIC_DET_CTL1, 0xFF, 0x80 | BIAS_LVL0);
    l42_upd(s, HS_CLAMP_DIS, 0x01, 0x01);
    l42_upd(s, TSENSE_CTL, 0x80, 0x80);
    l42_upd(s, TIPSENSE_CTL, 0xE3, 0xC0 | 0x20 | 0x02);
    /* clock stop not conditional on internal logic */
    l42_upd(s, PWR_CTL3, 0x60, 0x00);
    /* sample rate converters for 48 kHz (cs42l42_src_config), PLL from the bus clock */
    l42_upd(s, FS_RATE_EN, 0x0F, 0x03);
    l42_upd(s, IN_ASRC_CLK, 0x01, 0x00);
    l42_upd(s, OUT_ASRC_CLK, 0x01, 0x00);
    ret = l42_pll_config(s);
    /* mixer 0 dB, ADC 0 dB, no sidetone, headphones muted until a stream runs */
    l42_upd(s, HP_CTL, 0x0C, 0x0C);
    l42_upd(s, MIXER_CHA_VOL, 0x3F, 0x00);
    l42_upd(s, MIXER_CHB_VOL, 0x3F, 0x00);
    l42_wr(s, MIXER_ADC_VOL, 0x3F);
    l42_wr(s, ADC_VOLUME, 0x00);
    return ret;
}

/* port prepare (HP / ADC power), DAPM (mixer, SRC) and the clock switch of mute_stream */
static void l42_stream(struct sdwdev *s, int func, int on)
{
    u32 was = s->priv[P42_USE];
    if (func != SF_JACK_OUT && func != SF_JACK_IN) return;
    if (on ? (was & (u32)func) != 0 : !(was & (u32)func)) return;
    if (on) {
        if (func == SF_JACK_OUT) {
            l42_upd(s, PWR_CTL1, 0x08, 0);          /* HP */
            mdelay(20);
            l42_upd(s, PWR_CTL1, 0x20, 0);          /* mixer */
            l42_upd(s, PWR_CTL2, 0x02, 0x02);       /* DAC SRC */
        } else {
            l42_upd(s, PWR_CTL1, 0x04, 0);          /* ADC */
            mdelay(20);
            l42_upd(s, PWR_CTL2, 0x01, 0x01);       /* ADC SRC */
        }
        if (!was) l42_to_sclk(s);
        s->priv[P42_USE] |= (u32)func;
        if (func == SF_JACK_OUT) l42_upd(s, HP_CTL, 0x0C, 0x00);
        return;
    }
    if (func == SF_JACK_OUT) l42_upd(s, HP_CTL, 0x0C, 0x0C);
    s->priv[P42_USE] &= ~(u32)func;
    if (!s->priv[P42_USE] && s->priv[P42_CLK]) l42_to_osc(s);
    if (func == SF_JACK_OUT) {
        l42_upd(s, PWR_CTL2, 0x02, 0);
        l42_upd(s, PWR_CTL1, 0x28, 0x28);
    } else {
        l42_upd(s, PWR_CTL2, 0x01, 0);
        l42_upd(s, PWR_CTL1, 0x04, 0x04);
    }
}

static void l42_volume(struct sdwdev *s, int ctl, u32 lr)
{
    u32 g;
    if (ctl == MX_MASTER) {
        /* mixer: 0 .. -63 dB in 1 dB steps, 0x3F mutes */
        l42_upd(s, MIXER_CHA_VOL, 0x3F, 63 - cdc_step(CDC_L(lr), 63));
        l42_upd(s, MIXER_CHB_VOL, 0x3F, 63 - cdc_step(CDC_R(lr), 63));
    } else if (ctl == MX_INGAIN) {
        /* ADC: signed dB, the -48 .. +12 dB part of it; -97 dB mutes */
        g = cdc_step(CDC_L(lr), 60);
        l42_wr(s, ADC_VOLUME, g ? (g + 256 - 48) & 0xFF : 0x9F);
    }
}

static int l42_jack(struct sdwdev *s)
{
    int ts, v, j;
    ts = l42_rd(s, TSRS_PLUG_STATUS);
    if (ts < 0) return -1;
    ts = (ts >> 2) & 3;
    if (s->priv[P42_DET]) {
        /* type detection running: done, or given up waiting after about 1.5 s */
        v = l42_rd(s, CODEC_STATUS);
        if ((v > 0 && (v & 0x02)) || ++s->priv[P42_DET] > 5) {
            l42_detect_done(s);
            s->priv[P42_DET] = 0;
        }
    }
    if (ts == TS_PLUG) {
        if (s->priv[P42_PLUG] != TS_PLUG) {
            s->priv[P42_PLUG] = TS_PLUG;
            l42_detect_start(s);
            s->priv[P42_DET] = 1;
        }
    } else if (ts == TS_UNPLUG) {
        if (s->priv[P42_PLUG] != TS_UNPLUG) {
            s->priv[P42_PLUG] = TS_UNPLUG;
            s->priv[P42_DET] = 0;
            l42_detect_cancel(s);
        }
    } else {
        s->priv[P42_PLUG] = TS_TRANS;
    }
    if (s->priv[P42_PLUG] == TS_UNPLUG) j = 0;
    else if (s->priv[P42_PLUG] == TS_TRANS || s->priv[P42_DET]) j = s->jackstate;
    else j = s->priv[P42_TYPE] == PLUG_CTIA || s->priv[P42_TYPE] == PLUG_OMTP ? 3 : 1;
    return j;
}

static void l42_off(struct sdwdev *s)
{
    int i, v;
    if (s->priv[P42_USE] & SF_JACK_OUT) l42_stream(s, SF_JACK_OUT, 0);
    if (s->priv[P42_USE] & SF_JACK_IN) l42_stream(s, SF_JACK_IN, 0);
    l42_seq(s, l42_irq_masks, ARRAY_SIZE(l42_irq_masks));
    l42_seq(s, l42_shutdown, ARRAY_SIZE(l42_shutdown));
    mdelay(65);
    for (i = 0; i < 50; i++) {
        v = l42_rd(s, CODEC_STATUS);
        if (v < 0 || (v & 0x01)) break;             /* power down done */
        mdelay(5);
    }
    l42_upd(s, PWR_CTL2, 0x10, 0x10);               /* discharge FILT+ */
}

const struct sdwdrv sdw_cs42l42 = {
    "cs42l42", MFR_CIRRUS, 0x4242, SF_JACK_OUT | SF_JACK_IN, 2, l42_ports,
    l42_init, l42_stream, l42_volume, l42_jack, l42_off
};
