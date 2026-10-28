/* sdwrt7l.c - the older Realtek SoundWire codecs: RT700 and RT711 (headphones + headset
   microphone on a combo jack; the RT700 also drives a speaker from the headphone DAC),
   RT714 / RT715 (digital microphone codecs, HD Audio style in version 2, SDCA in version 3).
   The non-SDCA chips are HD Audio codecs behind SoundWire: verbs and the private "index"
   registers go through 8-bit SoundWire registers the way the Linux rt700/rt711/rt715 regmap
   read/write functions encode them. After the Linux rt700, rt711, rt715, rt715-sdca drivers,
   the sdw_utils machine code and the alsa-ucm-conf boot settings of these codecs. */
#include "sdw.h"

/* ---- HD Audio verbs over SoundWire (rt7xx_sdw_read / rt7xx_sdw_write) ---- */
#define VREG            0x20        /* vendor register NID */
#define FUNC_RESET      0xFF01UL
#define READ_HDA_3      0x2012UL    /* response of a get verb, 0x2012..0x2015 MSB first */
#define AFG_PWR         0x3501UL    /* set power state of the audio function group */
#define PIN_SENSE_HP    0xB921UL    /* get pin sense of the headphone pin (NID 21h) */
#define FMT48           0x0031UL    /* converter format: 48 kHz, 24 bits, 2 channels */
#define STREAMID(nid)   (0x3600UL | (nid))
#define CONNSEL(nid)    (0x3100UL | (nid))
/* private (index) register idx of nid: the regmap address the Linux drivers use */
#define IDX(nid, idx)   (((0x7500UL | (u32)(nid)) << 8) | (u32)(idx))

/* index registers of NID 20h */
#define I_VD_CLEAR      0x01        /* RT715 */
#define I_JD_CTL1       0x08
#define I_JD_CTL2       0x09
#define I_JD_CTL4       0x0B
#define I_CC_DET1       0x11
#define I_PARA_VERB     0x1A
#define I_COMBO_CTL2    0x46
#define I_FSM_CTL       0x6F
#define I_SDW_INSEL     0x39        /* RT715: data port input selection */
#define VCALI           0x58        /* RT711 calibration NID, index 0: DAC DC calibration */

/* reg > 0xFFFF: index register (NID in bits 15..8 of reg >> 8), 16-bit value; 16-bit verbs
   at 0x7xxx (left / both) and 0x83xx (right channel amplifier); the rest 8-bit */
static int hda_wr(struct sdwdev *s, u32 reg, u32 val)
{
    u32 r, r3;
    if (reg > 0xFFFFUL) {
        r = reg >> 8;
        r3 = 0x7400UL | (r & 0xFF);
        return (sdw_wr(s, r, 0) || sdw_wr(s, (r + 0x1000) | 0x80, (u8)reg) ||
                sdw_wr(s, r3, (u8)(val >> 8)) || sdw_wr(s, (r3 + 0x1000) | 0x80, (u8)val)) ? -1 : 0;
    }
    if (reg < 0x4FFF || reg == FUNC_RESET)
        return sdw_wr(s, reg, (u8)val) ? -1 : 0;
    if ((reg & 0xF000) == 0x7000)
        return (sdw_wr(s, reg, (u8)(val >> 8)) || sdw_wr(s, (reg + 0x1000) | 0x80, (u8)val)) ? -1 : 0;
    if ((reg & 0xFF00) == 0x8300)
        return (sdw_wr(s, (reg - 0x1000) & ~0x80UL, (u8)(val >> 8)) || sdw_wr(s, reg, (u8)val)) ? -1 : 0;
    return -1;
}

/* index register or get verb (0xBxxx) with payload 0; the 32-bit response */
static int hda_rd(struct sdwdev *s, u32 reg, u32 *val)
{
    u32 r, r3;
    int i, b;
    if (reg > 0xFFFFUL) {
        r = reg >> 8;
        r3 = 0x9C00UL | (r & 0xFF);
        if (sdw_wr(s, r, 0) || sdw_wr(s, (r + 0x1000) | 0x80, (u8)reg) ||
            sdw_wr(s, r3, 0) || sdw_wr(s, (r3 + 0x1000) | 0x80, 0))
            return -1;
    } else if (sdw_wr(s, reg, 0))
        return -1;
    *val = 0;
    for (i = 0; i < 4; i++) {
        b = sdw_rd(s, READ_HDA_3 + (u32)i);
        if (b < 0)
            return -1;
        *val = (*val << 8) | (u32)b;
    }
    return 0;
}

static int idx_upd(struct sdwdev *s, u32 nid, u32 idx, u32 mask, u32 val)
{
    u32 v;
    if (hda_rd(s, IDX(nid, idx), &v))
        return -1;
    return hda_wr(s, IDX(nid, idx), (v & ~mask) | (val & mask));
}

static void hda_seq(struct sdwdev *s, const u32 *p, int n)
{
    int i;
    for (i = 0; i < n; i++)
        hda_wr(s, p[2 * i], p[2 * i + 1]);
}

static void afg(struct sdwdev *s, int d0)
{
    hda_wr(s, AFG_PWR, d0 ? 0 : 3);
}

/* amplifier gain / mute of nid (out: output amplifier, else input); v = left | right << 8,
   each gain | mute << 7 */
static void amp(struct sdwdev *s, u32 nid, int out, u32 v)
{
    u32 dir = out ? 0x80 : 0x40, l = v & 0xFF, r = (v >> 8) & 0xFF;
    if (l == r) {
        hda_wr(s, 0x7300UL | nid, ((dir | 0x30) << 8) | l);
    } else {
        hda_wr(s, 0x7300UL | nid, ((dir | 0x20) << 8) | l);
        hda_wr(s, 0x8380UL | nid, ((dir | 0x10) << 8) | r);
    }
}

/* ---- driver state ---- */
#define P_ACT       0               /* SF_* functions streaming */
#define P_JTYPE     1               /* jack type: 0 undecided, 1 headphones, 3 headset */
#define P_JWAIT     2               /* polls waiting for the combo jack decision */
#define P_VOUT      3               /* playback level, bit 31: set by volume() */
#define P_VIN       4               /* capture level, bit 31: set by volume() */
#define P_SPK       5               /* RT700 speaker unmuted */
#define P_HP        6               /* headphones plugged at the last poll */
#define VSET        0x80000000UL
#define JWAITMAX    17              /* ~5 s, as the Linux 500 x 10 ms headset detection */

static u32 vget(struct sdwdev *s, int i, u32 def)
{
    return (s->priv[i] & VSET) ? s->priv[i] : (def | def << 8);
}

/* MX_* level to HD Audio amplifier gain steps, level 0 = mute */
static u32 hda_lvl(u32 lr, u32 steps, int vol)
{
    u32 l = vol ? cdc_vstep(CDC_L(lr), steps, 750) : cdc_step(CDC_L(lr), steps);
    u32 r = vol ? cdc_vstep(CDC_R(lr), steps, 750) : cdc_step(CDC_R(lr), steps);
    return (l ? l : 0x80) | ((r ? r : 0x80) << 8) | VSET;
}

/* bus clock to the codec's clock divider (rt7xx_clock_config) */
static void clk_cfg(struct sdwdev *s)
{
    u32 v;
    switch (s->busclk ? s->busclk : 4800000UL) {
    case 12000000UL: v = 0; break;
    case 6000000UL:  v = 1; break;
    case 9600000UL:  v = 2; break;
    case 4800000UL:  v = 3; break;
    case 2400000UL:  v = 4; break;
    case 12288000UL: v = 5; break;
    default: return;
    }
    hda_wr(s, 0xE0, v);
    hda_wr(s, 0xF0, v);
}

#define IS_SDCA(s)  ((s)->cls == 1 || (s)->ver >= 3)

/* RT711 jack detection source (Linux "realtek,jd-src", sof_sdw quirk; default JD1):
   1 JD1, 2 JD2, 3 JD2 100K, 4 JD2 1.8 V 1-port */
extern u8 sdw_rt711_jdsrc;
u8 sdw_rt711_jdsrc = 1;

/* ==== RT700 / RT711: headset codecs ==== */
struct rtcf {
    u8 dac, adc, mux, spk;          /* playback DAC, headset microphone ADC and its mux,
                                       speaker pin (0: none) */
};
static const struct rtcf cf700 = { 0x02, 0x09, 0x22, 0x14 };
static const struct rtcf cf711 = { 0x03, 0x08, 0x23, 0x00 };
#define CF(s)   ((s)->part == 0x700 ? &cf700 : &cf711)

#define OUT_DEF     0x57            /* DAC 0 dB (UCM: 87) */
#define IN_DEF      0x3F            /* ADC +30 dB (UCM: 63) */

static const u32 r700_tab[] = {
    FUNC_RESET, 0, 0x7520, 0x001A, 0x7420, 0xC003,
    AFG_PWR, 0,
    0x3721, 0x40, 0x3714, 0x40, 0x3C14, 0x02,       /* HP, SPK pins out, EAPD */
    0x3712, 0x20, 0x3713, 0x20, 0x3719, 0x20,       /* DMIC1/2, MIC2 pins in */
    0x4F12, 0x91, 0x4E12, 0xD6, 0x4D12, 0x11, 0x4C12, 0x20,
    0x4F13, 0x91, 0x4E13, 0xD6, 0x4D13, 0x11, 0x4C13, 0x21,
    0x4F19, 0x02, 0x4E19, 0xA1, 0x4D19, 0x90, 0x4C19, 0x80,
    0x371B, 0x40, 0x731B, 0xB0, 0x839B, 0x00,       /* line 2 */
    IDX(VREG, 0x4A), 0x201B, IDX(VREG, 0x45), 0x5089,
    IDX(VREG, 0x6B), 0x5064, IDX(VREG, 0x48), 0xD249,
    AFG_PWR, 3
};

static const u32 r711_tab[] = {
    AFG_PWR, 0,
    0x3719, 0x25, 0x3721, 0xC0, 0x3712, 0x20, 0x3713, 0x20, 0x371A, 0x20, 0x371B, 0x20,
    0x7321, 0xA080, 0x7321, 0x9080,                 /* mute HP, ADC 08, ADC 09 */
    0x7308, 0x6080, 0x7308, 0x5080, 0x7309, 0x6080, 0x7309, 0x5080,
    0x4F12, 0x91, 0x4E12, 0xD6, 0x4D12, 0x11, 0x4C12, 0x20,
    0x4F13, 0x91, 0x4E13, 0xD6, 0x4D13, 0x11, 0x4C13, 0x21,
    0x4C21, 0xF0, 0x4D21, 0x11, 0x4E21, 0x11, 0x4F21, 0x01,
    IDX(VREG, 0x91), 0x0154,                        /* data port arrangement */
    IDX(VREG, 0x4A), 0x201B, IDX(VREG, 0x45), 0x5089,
    IDX(VREG, 0x6B), 0x5064, IDX(VREG, 0x48), 0xD249,
    AFG_PWR, 3
};

/* jack detection on (rt7xx_jack_init): unsolicited responses MIC2, HP, inline command */
static const u32 hs_jkinit[] = {
    0x3819, 0x82, 0x3821, 0x81, 0x3855, 0x83,
    IDX(VREG, 0x10), 0x2420, IDX(VREG, 0x19), 0x2E11
};

static void r711_jdsrc(struct sdwdev *s)
{
    switch (sdw_rt711_jdsrc) {
    case 2:
        idx_upd(s, VREG, I_JD_CTL2, 0x2002, 0x2002);
        break;
    case 3:
        idx_upd(s, VREG, I_JD_CTL2, 0x1C03, 0x0402);
        break;
    case 4:
        idx_upd(s, VREG, I_JD_CTL1, 0x0002, 0x0000);
        idx_upd(s, VREG, I_JD_CTL2, 0x0C02, 0x0402);
        idx_upd(s, VREG, I_JD_CTL4, 0x000B, 0x000A);
        break;
    default:
        return;                     /* JD1: the chip default */
    }
    idx_upd(s, VREG, I_CC_DET1, 0x0400, 0x0400);
}

/* DAC DC offset calibration, then depop mode */
static void r711_cali(struct sdwdev *s)
{
    u32 v = 0;
    int n;
    afg(s, 1);
    idx_upd(s, VREG, I_FSM_CTL, 0xF, 0x0);
    idx_upd(s, VCALI, 0x00, 0x8000, 0x8000);
    for (n = 0; n < 500; n++) {
        if (hda_rd(s, IDX(VCALI, 0x00), &v) || !(v & 0x8000))
            break;
        mdelay(10);
    }
    if (n >= 500)
        DBG("sdw rt711: calibration time-out\n");
    idx_upd(s, VREG, I_FSM_CTL, 0xF, 0x3);
    afg(s, 0);
}

static void hs_common(struct sdwdev *s)
{
    const struct rtcf *c = CF(s);
    s->priv[P_ACT] = 0;
    s->priv[P_JTYPE] = 0;
    s->priv[P_JWAIT] = 0;
    s->priv[P_SPK] = 0;
    s->priv[P_HP] = 0;
    afg(s, 1);
    hda_seq(s, hs_jkinit, sizeof(hs_jkinit) / sizeof(hs_jkinit[0]) / 2);
    if (s->part == 0x711)
        r711_jdsrc(s);
    /* UCM boot settings: headset microphone on the ADC, MIC2 boost +10 dB, HPO from DAC */
    hda_wr(s, CONNSEL(c->mux), 0);
    if (c->spk)
        hda_wr(s, CONNSEL(0x21), 0);
    hda_wr(s, 0x7319, 0x7001);
    afg(s, 0);
    clk_cfg(s);
}

static int r700_init(struct sdwdev *s)
{
    hda_seq(s, r700_tab, sizeof(r700_tab) / sizeof(r700_tab[0]) / 2);
    hs_common(s);
    return 0;
}

static int r711_init(struct sdwdev *s)
{
    /* version 3 / SDCA class: the rt711-sdca driver */
    if (IS_SDCA(s))
        return -1;
    hda_wr(s, FUNC_RESET, 0);
    idx_upd(s, VREG, I_PARA_VERB, 0x4000, 0x4000);
    hda_seq(s, r711_tab, sizeof(r711_tab) / sizeof(r711_tab[0]) / 2);
    r711_cali(s);
    hs_common(s);
    return 0;
}

static void spk_set(struct sdwdev *s, int on)
{
    if (s->priv[P_SPK] == (u32)on)
        return;
    s->priv[P_SPK] = (u32)on;
    hda_wr(s, 0x7300UL | cf700.spk, on ? 0xB000 : 0xB080);
}

static void hs_stream(struct sdwdev *s, int func, int on)
{
    const struct rtcf *c = CF(s);
    if (on) {
        if (!s->priv[P_ACT])
            afg(s, 1);
        s->priv[P_ACT] |= (u32)func;
        hda_wr(s, 0x7203, FMT48);
        hda_wr(s, 0x7209, FMT48);
        if (s->part == 0x711)
            hda_wr(s, 0x7208, FMT48);
        if (func == SF_JACK_OUT) {
            hda_wr(s, STREAMID(c->dac), 0x10);
            amp(s, c->dac, 1, vget(s, P_VOUT, OUT_DEF));
            hda_wr(s, 0x7321, 0xB000);              /* HP unmute */
            if (c->spk)
                spk_set(s, !s->priv[P_HP]);
        } else if (func == SF_JACK_IN) {
            hda_wr(s, STREAMID(c->adc), 0x10);
            amp(s, c->adc, 0, vget(s, P_VIN, IN_DEF));
        }
        return;
    }
    if (func == SF_JACK_OUT) {
        if (c->spk)
            spk_set(s, 0);
        hda_wr(s, 0x7321, 0xB080);
        mdelay(50);
        hda_wr(s, STREAMID(c->dac), 0);
    } else if (func == SF_JACK_IN) {
        hda_wr(s, STREAMID(c->adc), 0);
    }
    s->priv[P_ACT] &= ~(u32)func;
    if (!s->priv[P_ACT])
        afg(s, 0);
}

static void hs_volume(struct sdwdev *s, int ctl, u32 lr)
{
    const struct rtcf *c = CF(s);
    int tmp = !s->priv[P_ACT];
    if (ctl == MX_MASTER)
        s->priv[P_VOUT] = hda_lvl(lr, 0x57, 1);
    else if (ctl == MX_INGAIN)
        s->priv[P_VIN] = hda_lvl(lr, 0x3F, 0);
    else
        return;
    /* amplifier writes need D0, as the Linux put handlers do */
    if (tmp)
        afg(s, 1);
    if (ctl == MX_MASTER)
        amp(s, c->dac, 1, s->priv[P_VOUT]);
    else
        amp(s, c->adc, 0, s->priv[P_VIN]);
    if (tmp)
        afg(s, 0);
}

/* rt7xx_jack_detect_handler: pin sense of the headphone pin, then the combo jack decision */
static int hs_jack(struct sdwdev *s)
{
    u32 pin, buf;
    int v, st;
    /* the implementation-defined interrupt Linux handles: clear it */
    v = sdw_rd(s, SDW_SCP_INT1);
    if (v > 0 && (v & 0x04))
        sdw_wr(s, SDW_SCP_INT1, 0x04);
    if (hda_rd(s, PIN_SENSE_HP, &pin))
        return -1;
    if (!(pin & 0x80000000UL)) {
        s->priv[P_JTYPE] = 0;
        s->priv[P_JWAIT] = 0;
        st = 0;
    } else {
        if (!s->priv[P_JTYPE] && s->priv[P_JWAIT] < JWAITMAX) {
            if (hda_rd(s, IDX(VREG, I_COMBO_CTL2), &buf))
                return -1;
            if ((buf & 0x0800) && (buf & 0x0400))           /* TRS */
                s->priv[P_JTYPE] = 1;
            else if ((buf & 0x0800) && (buf & 0x0300))      /* CTIA / OMTP */
                s->priv[P_JTYPE] = 3;
            else
                s->priv[P_JWAIT]++;
        }
        st = s->priv[P_JTYPE] ? (int)s->priv[P_JTYPE] : 1;
    }
    s->priv[P_HP] = (u32)(st & 1);
    if (CF(s)->spk && (s->priv[P_ACT] & SF_JACK_OUT))
        spk_set(s, !(st & 1));
    return st;
}

static void hs_off(struct sdwdev *s)
{
    const struct rtcf *c = CF(s);
    if (c->spk)
        spk_set(s, 0);
    hda_wr(s, 0x7321, 0xB080);
    hda_wr(s, STREAMID(c->dac), 0);
    hda_wr(s, STREAMID(c->adc), 0);
    s->priv[P_ACT] = 0;
    afg(s, 0);
}

/* ==== RT714 / RT715 version 2: HD Audio microphone codec ==== */
#define DM_DEF      58              /* ADC gain step (UCM: 58, +26.25 dB) */

static const u32 r715_tab[] = {
    0x7308, 0xB080, 0x7309, 0xB080, 0x7307, 0xB080, 0x7327, 0xB080,
    0x3712, 0x20, 0x3713, 0x20, 0x371D, 0x20, 0x3729, 0x20,
    STREAMID(0x08), 0x10, STREAMID(0x09), 0x10, STREAMID(0x07), 0x10, STREAMID(0x27), 0x10,
    0x4C12, 0xD0, 0x4D12, 0x11, 0x4E12, 0xA1, 0x4F12, 0x81,
    0x4C13, 0xD1, 0x4D13, 0x11, 0x4E13, 0xA1, 0x4F13, 0x81,
    0x4C1D, 0xD0, 0x4D1D, 0x11, 0x4E1D, 0xA1, 0x4F1D, 0x81,
    0x4C29, 0xD1, 0x4D29, 0x11, 0x4E29, 0xA1, 0x4F29, 0x81,
    AFG_PWR, 3,
    /* UCM: DMIC3/4 boost +20 dB, ADC 07 from DMIC3, ADC 27 from DMIC4 */
    AFG_PWR, 0,
    0x731D, 0x7002, 0x7329, 0x7002, CONNSEL(0x24), 4, CONNSEL(0x25), 5,
    AFG_PWR, 3
};

static int r715_init(struct sdwdev *s)
{
    s->priv[P_ACT] = 0;
    hda_wr(s, FUNC_RESET, 0);
    idx_upd(s, VREG, I_VD_CLEAR, 0x8000, 0x8000);
    hda_seq(s, r715_tab, sizeof(r715_tab) / sizeof(r715_tab[0]) / 2);
    clk_cfg(s);
    return 0;
}

/* DP4: ADC 07 and ADC 27 */
static void r715_gain(struct sdwdev *s)
{
    u32 v = vget(s, P_VIN, DM_DEF);
    amp(s, 0x07, 0, v);
    amp(s, 0x27, 0, v);
}

static void r715_stream(struct sdwdev *s, int func, int on)
{
    if (func != SF_DMIC)
        return;
    if (on) {
        hda_wr(s, IDX(VREG, I_SDW_INSEL), 0xA000);
        hda_wr(s, 0x7207, FMT48);
        hda_wr(s, 0x7208, FMT48);
        hda_wr(s, 0x7209, FMT48);
        hda_wr(s, 0x7227, FMT48);
        afg(s, 1);
        mdelay(400);                                /* RT715_POWER_UP_DELAY_MS */
        hda_wr(s, STREAMID(0x07), 0x10);
        hda_wr(s, STREAMID(0x27), 0x10);
        r715_gain(s);
        s->priv[P_ACT] = SF_DMIC;
    } else {
        hda_wr(s, STREAMID(0x07), 0);
        hda_wr(s, STREAMID(0x27), 0);
        afg(s, 0);
        s->priv[P_ACT] = 0;
    }
}

static void r715_volume(struct sdwdev *s, int ctl, u32 lr)
{
    if (ctl != MX_INGAIN)
        return;
    s->priv[P_VIN] = hda_lvl(lr, 0x3F, 0);
    if (!s->priv[P_ACT])
        afg(s, 1);
    r715_gain(s);
    if (!s->priv[P_ACT])
        afg(s, 0);
}

/* ==== RT714 / RT715 version 3: SDCA microphone codec (rt715-sdca) ==== */
#define FUN_MIC         2           /* SDCA function: microphone array */
#define E_CS_FREQ       0x01        /* clock source, control 0x10: sample rate index */
#define E_FU02          0x02        /* feature unit ADC 08/09, 4 channels */
#define E_SMPU          0x05
#define E_FU0A          0x0A        /* feature unit ADC 07/27 */
#define E_CX_CLK        0x10
#define E_PDE23_24      0x18        /* power domain entity, control 0x01: requested PS */
#define C_MUTE          0x01
#define C_VOL           0x02
#define VHDA            0x61        /* NID of the HD Audio legacy mux control */
/* private register: 16-bit MBQ access at nid << 20 | idx */
#define SIDX(nid, idx)  (((u32)(nid) << 20) | (u32)(idx))
#define SV_DEF          0x2F        /* FU02 volume step: 0 dB (UCM: 47) */

static int sidx_upd(struct sdwdev *s, u32 nid, u32 idx, u16 mask, u16 val)
{
    int v = sdw_rd16(s, SIDX(nid, idx));
    if (v < 0)
        return -1;
    return sdw_wr16(s, SIDX(nid, idx), (u16)(((u16)v & ~mask) | (val & mask)));
}

static int sdca_init(struct sdwdev *s)
{
    int hw;
    s->priv[P_ACT] = 0;
    hw = sdw_rd16(s, SIDX(VREG, 0x00));             /* product number */
    if (hw < 0)
        return -1;
    hw &= 0xF;
    sdw_wr(s, SDW_SDCA_CTL(FUN_MIC, E_CX_CLK, 0x01, 0), 0x01);     /* external clock */
    /* GPIO 4/5/6 for the 3rd/4th DMIC */
    if (hw == 0) {
        sidx_upd(s, VREG, 0x36, 0x54, 0x54);
    } else if (hw == 1) {
        sidx_upd(s, VREG, 0x36, 0x55, 0x55);
        sidx_upd(s, VREG, 0x37, 0x40, 0x40);
    }
    sidx_upd(s, VREG, 0x44, 0x01, 0x01);            /* DFLL calibration trigger */
    sdw_wr(s, SDW_SDCA_CTL(FUN_MIC, E_SMPU, 0x10, 0), 0x02);       /* trigger mode: VAD */
    /* UCM: FU0A muted, ADC 22 Mux = DMIC3, ADC 23 Mux = DMIC4 (feed FU02) */
    sdw_wr(s, SDW_SDCA_CTL(FUN_MIC, E_FU0A, C_MUTE, 1), 1);
    sdw_wr(s, SDW_SDCA_CTL(FUN_MIC, E_FU0A, C_MUTE, 2), 1);
    sidx_upd(s, VHDA, 0x00, 0xFF00, 0x6700);
    return 0;
}

/* rt715_sdca_vol_gain: step to Q7.8 dB, 0x2F = 0 dB */
static u16 sdca_gain(u32 step)
{
    u32 d = step > 0x2F ? step - 0x2F : 0x2F - step;
    u32 v = ((d * 375UL) << 8) / 1000UL;
    if (step <= 0x2F)
        v = (~v + 1) & 0xFFFF;
    return (u16)v;
}

static void sdca_gainset(struct sdwdev *s, int mute)
{
    u32 v = s->priv[P_VIN] & VSET ? s->priv[P_VIN] : (SV_DEF | SV_DEF << 8);
    u32 st;
    int ch;
    for (ch = 1; ch <= 4; ch++) {
        st = (ch & 1) ? (v & 0xFF) : ((v >> 8) & 0xFF);
        if (st)
            sdw_wr16(s, SDW_SDCA_CTL(FUN_MIC, E_FU02, C_VOL, ch), sdca_gain(st));
        sdw_wr(s, SDW_SDCA_CTL(FUN_MIC, E_FU02, C_MUTE, ch), (u8)(mute || !st));
    }
}

static void sdca_stream(struct sdwdev *s, int func, int on)
{
    if (func != SF_DMIC)
        return;
    if (on) {
        sdw_wr16(s, SIDX(VREG, I_SDW_INSEL), 0xAF00);
        sdw_wr(s, SDW_SDCA_CTL(FUN_MIC, E_CS_FREQ, 0x10, 0), 0x09);  /* 48 kHz */
        sdw_wr(s, SDW_SDCA_CTL(FUN_MIC, E_PDE23_24, 0x01, 0), 0x00); /* PS0 */
        sdca_gainset(s, 0);
        s->priv[P_ACT] = SF_DMIC;
    } else {
        sdca_gainset(s, 1);
        sdw_wr(s, SDW_SDCA_CTL(FUN_MIC, E_PDE23_24, 0x01, 0), 0x03); /* PS3 */
        s->priv[P_ACT] = 0;
    }
}

static void sdca_volume(struct sdwdev *s, int ctl, u32 lr)
{
    if (ctl != MX_INGAIN)
        return;
    s->priv[P_VIN] = cdc_step(CDC_L(lr), 0x3F) | (cdc_step(CDC_R(lr), 0x3F) << 8) | VSET;
    if (s->priv[P_ACT])
        sdca_gainset(s, 0);
}

/* ---- RT714 / RT715: one driver for both versions, as Linux matches them by version ---- */
static int dm_init(struct sdwdev *s)
{
    return IS_SDCA(s) ? sdca_init(s) : r715_init(s);
}

static void dm_stream(struct sdwdev *s, int func, int on)
{
    if (IS_SDCA(s))
        sdca_stream(s, func, on);
    else
        r715_stream(s, func, on);
}

static void dm_volume(struct sdwdev *s, int ctl, u32 lr)
{
    if (IS_SDCA(s))
        sdca_volume(s, ctl, lr);
    else
        r715_volume(s, ctl, lr);
}

static void dm_off(struct sdwdev *s)
{
    dm_stream(s, SF_DMIC, 0);
}

/* ---- the drivers ---- */
static const struct sdwport pt700[] = {
    { SF_JACK_OUT, 1, 2, SPF_FULL }, { SF_JACK_IN, 2, 2, SPF_FULL }
};
static const struct sdwport pt711[] = {
    { SF_JACK_OUT, 3, 2, SPF_FULL }, { SF_JACK_IN, 4, 2, SPF_FULL }
};
static const struct sdwport pt715[] = {
    { SF_DMIC, 4, 2, SPF_FULL }
};

const struct sdwdrv sdw_rt700 = {
    "rt700", 0x025D, 0x700, SF_JACK_OUT | SF_JACK_IN, 2, pt700,
    r700_init, hs_stream, hs_volume, hs_jack, hs_off
};
const struct sdwdrv sdw_rt711 = {
    "rt711", 0x025D, 0x711, SF_JACK_OUT | SF_JACK_IN, 2, pt711,
    r711_init, hs_stream, hs_volume, hs_jack, hs_off
};
const struct sdwdrv sdw_rt714 = {
    "rt714", 0x025D, 0x714, SF_DMIC, 1, pt715,
    dm_init, dm_stream, dm_volume, NULL, dm_off
};
const struct sdwdrv sdw_rt715 = {
    "rt715", 0x025D, 0x715, SF_DMIC, 1, pt715,
    dm_init, dm_stream, dm_volume, NULL, dm_off
};
const struct sdwdrv sdw_rt715sdca = {
    "rt715-sdca", 0x025D, 0x715, SF_DMIC, 1, pt715,
    dm_init, dm_stream, dm_volume, NULL, dm_off
};
