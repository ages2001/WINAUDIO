/* hda.c - Intel High Definition Audio: controller, CORB/RIRB, generic codec parser.
   Every PCI class 04/03 function (and Intel controllers in DSP mode, 04/01 and 04/80 with
   HDA registers). The codec's widget graph is searched for paths from the line-out,
   speaker and headphone pins to a DAC and from the input pins to an ADC; all output pins
   play the same stream, the speakers are muted while headphones are plugged in. */
#include "audio.h"
#include "dsp.h"

#define MAXHDA      4
#define MAXNID      128

#define GCAP        0x00
#define VMIN        0x02
#define VMAJ        0x03
#define GCTL        0x08
#define STATESTS    0x0E
#define INTCTL      0x20
#define CORBLBASE   0x40
#define CORBUBASE   0x44
#define CORBWP      0x48
#define CORBRP      0x4A
#define CORBCTL     0x4C
#define CORBSIZE    0x4E
#define RIRBLBASE   0x50
#define RIRBUBASE   0x54
#define RIRBWP      0x58
#define RINTCNT     0x5A
#define RIRBCTL     0x5C
#define RIRBSTS     0x5D
#define RIRBSIZE    0x5E
#define ICOI        0x60
#define ICII        0x64
#define ICIS        0x68
#define DPLBASE     0x70
#define DPUBASE     0x74
#define SD(n)       (0x80 + (n) * 0x20)
#define SDCTL       0x00
#define SDSTS       0x03
#define SDLPIB      0x04
#define SDCBL       0x08
#define SDLVI       0x0C
#define SDFMT       0x12
#define SDBDPL      0x18
#define SDBDPU      0x1C

/* widget types */
#define WT_OUT      0
#define WT_IN       1
#define WT_MIX      2
#define WT_SEL      3
#define WT_PIN      4
#define WT_POWER    5
#define WT_VOL      6
#define WT_BEEP     7

/* pin default devices */
#define DD_LINEOUT  0
#define DD_SPEAKER  1
#define DD_HP       2
#define DD_CD       3
#define DD_LINEIN   8
#define DD_AUX      9
#define DD_MIC      10

#define PERIOD      2048
#define NPERIOD     32
#define RINGSIZE    (PERIOD * 2 * NPERIOD)

struct hwid {
    u32 caps, pincap, cfg, ampin, ampout;
    u8 type, nconn;
    u8 conn[16];
};

struct hda {
    struct adev a;
    struct pcidev pci;
    u8 *r;
    u32 *corb, *rirb;
    u16 corbn, rirbn, rp;
    u8 imm;
    u8 cad, afg, nstart, nend;
    struct hwid w[MAXNID];
    u8 dac[4], ndac;
    u8 opin[8], otype[8], nopin;
    u8 volnid[4], nvol, volsteps[4];
    u8 adc;
    u8 srcpath[5][6], srclen[5];
    u8 mixin;                   /* stereo mix: input of the mixer with the playback */
    u8 hp_present, hp_known;
    u32 hp_time;
    int osd, isd;
    u32 *bdl[2];
    u8 *ringbuf[2];
    u32 *posbuf;
    u8 useposbuf;
    u16 fmt;
    struct dsp *dsp;            /* Intel DSP of the controller (SST / SOF) */
    struct dspep *dspep[2];     /* the codec's streams through the DSP (/DSP) */
    u8 ncodec;                  /* codecs on the link (Plug and Play enumerator) */
    u8 started;                 /* the sound device is registered */
    struct hcodec { u8 cad, kind; u32 vid, sub, rev; } codec[15];
};

static struct hda *ctrls[MAXHDA];
static int nctrl;

#define R8(h, o)        RD8((h)->r + (o))
#define R16(h, o)       (*(volatile u16 *)((h)->r + (o)))
#define R32(h, o)       RD32((h)->r + (o))
#define W8(h, o, v)     WR8((h)->r + (o), v)
#define W16(h, o, v)    (*(volatile u16 *)((h)->r + (o)) = (u16)(v))
#define W32(h, o, v)    WR32((h)->r + (o), v)

static int wait16(struct hda *h, int o, u16 mask, u16 val, u32 ms)
{
    u32 t0 = plat_ms();
    while ((R16(h, o) & mask) != val)
        if (plat_ms() - t0 > ms) return -1;
    return 0;
}

/* ---------------------------------------------------------------- verbs */

static u32 cmd_imm(struct hda *h, u32 v)
{
    u32 t0 = plat_ms();
    while (R16(h, ICIS) & 1)
        if (plat_ms() - t0 > 10) return 0xFFFFFFFFUL;
    W16(h, ICIS, 2);
    W32(h, ICOI, v);
    W16(h, ICIS, 1);
    t0 = plat_ms();
    while ((R16(h, ICIS) & 3) != 2)
        if (plat_ms() - t0 > 10) return 0xFFFFFFFFUL;
    return R32(h, ICII);
}

static void corb_stop(struct hda *h)
{
    W8(h, CORBCTL, R8(h, CORBCTL) & ~2);
    W8(h, RIRBCTL, R8(h, RIRBCTL) & ~3);
}

static u32 cmd(struct hda *h, u32 v)
{
    u32 t0, resp;
    u16 wp;
    if (h->imm) return cmd_imm(h, v);
    wp = (u16)((R16(h, CORBWP) + 1) % h->corbn);
    h->corb[wp] = v;
    if (!h->a.out.noflush) aeng_flush_dma(&h->corb[wp], 4);
    W16(h, CORBWP, wp);
    t0 = plat_ms();
    for (;;) {
        if ((R16(h, RIRBWP) & 0xFF) != h->rp) {
            h->rp = (u16)((h->rp + 1) % h->rirbn);
            if (!h->a.out.noflush) aeng_flush_dma(&h->rirb[h->rp * 2], 8);
            resp = h->rirb[h->rp * 2];
            if (h->rirb[h->rp * 2 + 1] & 0x10) continue;       /* unsolicited */
            W8(h, RIRBSTS, 5);
            return resp;
        }
        if (plat_ms() - t0 > 50) break;
    }
    kprintf("hda: no response on CORB/RIRB, using immediate commands\n");
    corb_stop(h);
    h->imm = 1;
    return cmd_imm(h, v);
}

#define VERB(h, nid, v, p)  (((u32)(h)->cad << 28) | ((u32)(nid) << 20) | ((u32)(v) << 8) | (u32)(p))
#define VERB4(h, nid, v, p) (((u32)(h)->cad << 28) | ((u32)(nid) << 20) | ((u32)(v) << 16) | (u32)(p))

static u32 param(struct hda *h, int nid, int id)
{
    return cmd(h, VERB(h, nid, 0xF00, id));
}

static void setamp(struct hda *h, int nid, int out, int idx, int mute, int gain)
{
    u32 p = (out ? 0x8000 : 0x4000) | 0x3000 | ((u32)idx << 8) | (mute ? 0x80 : 0) | (u32)(gain & 0x7F);
    cmd(h, VERB4(h, nid, 0x3, p));
}

static u32 ampcaps(struct hda *h, int nid, int out)
{
    struct hwid *w = &h->w[nid];
    u32 c = out ? w->ampout : w->ampin;
    if (!(w->caps & 8) || !c) c = param(h, h->afg, out ? 0x12 : 0x0D);
    return c;
}

/* ---------------------------------------------------------------- codec parsing */

static void read_conn(struct hda *h, int nid)
{
    struct hwid *w = &h->w[nid];
    u32 len = param(h, nid, 0x0E), n = len & 0x7F, i, prev = 0;
    int lng = (len & 0x80) != 0;
    w->nconn = 0;
    if (!(w->caps & 0x100) || len == 0xFFFFFFFFUL) return;
    for (i = 0; i < n && w->nconn < 16; ) {
        u32 v = cmd(h, VERB(h, nid, 0xF02, i)), k, per = lng ? 2 : 4;
        for (k = 0; k < per && i < n; k++, i++) {
            u32 e = lng ? (v >> (16 * k)) & 0xFFFF : (v >> (8 * k)) & 0xFF;
            u32 range = lng ? (e & 0x8000) : (e & 0x80), id = lng ? (e & 0x7FFF) : (e & 0x7F), j;
            if (range && prev) {
                for (j = prev + 1; j <= id && w->nconn < 16; j++) w->conn[w->nconn++] = (u8)j;
            } else if (w->nconn < 16) {
                w->conn[w->nconn++] = (u8)id;
            }
            prev = id;
        }
    }
}

static int valid_nid(struct hda *h, int nid)
{
    return nid >= h->nstart && nid < h->nend && nid < MAXNID;
}

/* depth-first search from 'nid' to a widget of type 'target' (or to node 'want');
   path[0] = start node */
static int find_path(struct hda *h, int nid, int target, int want, u8 *path, int depth)
{
    struct hwid *w;
    int i;
    if (!valid_nid(h, nid) || depth >= 6) return 0;
    w = &h->w[nid];
    path[depth] = (u8)nid;
    if (depth > 0) {
        if (want && nid == want) return depth + 1;
        if (!want && w->type == target && !(w->caps & 0x200)) return depth + 1;
        if (w->type == WT_PIN || w->type == WT_OUT || w->type == WT_IN || w->type == WT_BEEP) return 0;
    }
    for (i = 0; i < w->nconn; i++) {
        int r;
        if (depth > 0 && w->type != WT_MIX && w->type != WT_SEL) break;
        r = find_path(h, w->conn[i], target, want, path, depth + 1);
        if (r) return r;
    }
    return 0;
}

static int conn_index(struct hwid *w, int nid)
{
    int i;
    for (i = 0; i < w->nconn; i++) if (w->conn[i] == nid) return i;
    return 0;
}

/* path[0] .. path[n-1]: route each node to the next one and open the amplifiers */
static void apply_path(struct hda *h, const u8 *path, int n)
{
    int i, k;
    for (i = 0; i < n; i++) {
        int nid = path[i];
        struct hwid *w = &h->w[nid];
        if (w->caps & 0x400) cmd(h, VERB(h, nid, 0x705, 0));       /* power D0 */
        if (i + 1 < n) {
            int idx = conn_index(w, path[i + 1]);
            if (w->type == WT_MIX) {
                if (w->caps & 2)
                    for (k = 0; k < w->nconn; k++) setamp(h, nid, 0, k, k != idx, (int)(ampcaps(h, nid, 0) & 0x7F));
            } else {
                if (w->nconn > 1) cmd(h, VERB(h, nid, 0x701, idx));
                if (w->caps & 2) setamp(h, nid, 0, idx, 0, (int)(ampcaps(h, nid, 0) & 0x7F));
            }
        }
        if (w->caps & 4) setamp(h, nid, 1, 0, 0, (int)(ampcaps(h, nid, 1) & 0x7F));
    }
}

/* Realtek: processing coefficients (COEF registers of the vendor widget 0x20, a few
   on other nodes) as the Linux driver sets them before it uses a codec. EAPD control
   moved to the EAPD verb, PLL gating off, headphone amplifier at full power, the old
   ALC260/88x amplifier bits; the GPIO lines of external amplifiers from the assembly ID
   (the codec subsystem ID or pin 0x1D). Returns the GPIO lines to drive high. */
struct rtcoef {
    u16 id;                     /* 0x10ECxxxx */
    u8 nid, idx;
    u16 mask, set;              /* mask 0xFFFF: write 'set' */
};
static const struct rtcoef rtcoefs[] = {
    { 0x0262, 0x20, 0x07, 0, 1 << 5 },
    { 0x0267, 0x20, 0x07, 0, 1 << 13 }, { 0x0268, 0x20, 0x07, 0, 1 << 13 },
    { 0x0280, 0x20, 0x04, 1 << 15, 0 }, { 0x0284, 0x20, 0x04, 1 << 15, 0 },
    { 0x0290, 0x20, 0x04, 1 << 15, 0 }, { 0x0292, 0x20, 0x04, 1 << 15, 0 },
    { 0x0225, 0x20, 0x67, 0xF000, 0x3000 }, { 0x0295, 0x20, 0x67, 0xF000, 0x3000 },
    { 0x0299, 0x20, 0x67, 0xF000, 0x3000 },
    { 0x0225, 0x20, 0x36, 1 << 13, 0 }, { 0x0295, 0x20, 0x36, 1 << 13, 0 }, { 0x0299, 0x20, 0x36, 1 << 13, 0 },
    { 0x0215, 0x20, 0x36, 1 << 13, 0 }, { 0x0236, 0x20, 0x36, 1 << 13, 0 }, { 0x0245, 0x20, 0x36, 1 << 13, 0 },
    { 0x0256, 0x20, 0x36, 1 << 13, 0 }, { 0x0257, 0x20, 0x36, 1 << 13, 0 }, { 0x0285, 0x20, 0x36, 1 << 13, 0 },
    { 0x0289, 0x20, 0x36, 1 << 13, 0 },
    { 0x0225, 0x20, 0x10, 1 << 9, 0 }, { 0x0295, 0x20, 0x10, 1 << 9, 0 }, { 0x0299, 0x20, 0x10, 1 << 9, 0 },
    { 0x0215, 0x20, 0x10, 1 << 9, 0 }, { 0x0236, 0x20, 0x10, 1 << 9, 0 }, { 0x0245, 0x20, 0x10, 1 << 9, 0 },
    { 0x0256, 0x20, 0x10, 1 << 9, 0 }, { 0x0257, 0x20, 0x10, 1 << 9, 0 }, { 0x0285, 0x20, 0x10, 1 << 9, 0 },
    { 0x0289, 0x20, 0x10, 1 << 9, 0 }, { 0x0230, 0x20, 0x10, 1 << 9, 0 }, { 0x0233, 0x20, 0x10, 1 << 9, 0 },
    { 0x0235, 0x20, 0x10, 1 << 9, 0 }, { 0x0255, 0x20, 0x10, 1 << 9, 0 }, { 0x0282, 0x20, 0x10, 1 << 9, 0 },
    { 0x0283, 0x20, 0x10, 1 << 9, 0 }, { 0x0286, 0x20, 0x10, 1 << 9, 0 }, { 0x0288, 0x20, 0x10, 1 << 9, 0 },
    { 0x0298, 0x20, 0x10, 1 << 9, 0 }, { 0x0300, 0x20, 0x10, 1 << 9, 0 }, { 0x0287, 0x20, 0x10, 1 << 9, 0 },
    { 0x0287, 0x20, 0x08, 0xFFFF, 0x4AB7 },
    { 0x0275, 0x20, 0x0E, 0, 1 },
    { 0x0293, 0x20, 0x0A, 1 << 13, 0 },
    { 0x0234, 0x20, 0x6E, 0xFFFF, 0x0C25 }, { 0x0274, 0x20, 0x6E, 0xFFFF, 0x0C25 },
    { 0x0234, 0x20, 0x10, 1 << 15, 0 }, { 0x0274, 0x20, 0x10, 1 << 15, 0 }, { 0x0294, 0x20, 0x10, 1 << 15, 0 },
    { 0x0700, 0x20, 0x10, 1 << 15, 0 }, { 0x0701, 0x20, 0x10, 1 << 15, 0 }, { 0x0703, 0x20, 0x10, 1 << 15, 0 },
    { 0x0711, 0x20, 0x10, 1 << 15, 0 },
    { 0x0272, 0x20, 0x0D, 0, 1 << 14 }, { 0x0273, 0x20, 0x0D, 0, 1 << 14 }, { 0x0663, 0x20, 0x0D, 0, 1 << 14 },
    { 0x0665, 0x20, 0x0D, 0, 1 << 14 }, { 0x0670, 0x20, 0x0D, 0, 1 << 14 }, { 0x0671, 0x20, 0x0D, 0, 1 << 14 },
    { 0x0672, 0x20, 0x0D, 0, 1 << 14 },
    { 0x0222, 0x20, 0x19, 1 << 13, 0 }, { 0x0623, 0x20, 0x19, 1 << 13, 0 },
    { 0x0668, 0x20, 0x07, 3 << 13, 0 },
    { 0x0867, 0x20, 0x04, 1 << 10, 0 },
    { 0x0892, 0x20, 0x07, 1 << 5, 0 }, { 0x0897, 0x20, 0x07, 1 << 5, 0 },
    { 0x0899, 0x20, 0x07, 1 << 1, 0 }, { 0x0900, 0x20, 0x07, 1 << 1, 0 }, { 0x0B00, 0x20, 0x07, 1 << 1, 0 },
    { 0x1168, 0x20, 0x07, 1 << 1, 0 }, { 0x1220, 0x20, 0x07, 1 << 1, 0 },
    /* headset jack mic vref by verb, combo jack auto trigger off */
    { 0x0234, 0x20, 0x6B, 0x0018, 0x0018 }, { 0x0274, 0x20, 0x6B, 0x0018, 0x0018 },
    { 0x0294, 0x20, 0x6B, 0x0018, 0x0018 },
    { 0x0700, 0x20, 0x4A, 1 << 15, 0 }, { 0x0701, 0x20, 0x4A, 1 << 15, 0 }, { 0x0703, 0x20, 0x4A, 1 << 15, 0 },
    { 0x0711, 0x20, 0x4A, 1 << 15, 0 },
    /* PLL gating off: ALC662 family, ALC883 family */
    { 0x0272, 0x20, 0x04, 1 << 15, 0 }, { 0x0662, 0x20, 0x04, 1 << 15, 0 }, { 0x0663, 0x20, 0x04, 1 << 15, 0 },
    { 0x0665, 0x20, 0x04, 1 << 15, 0 }, { 0x0667, 0x20, 0x04, 1 << 15, 0 }, { 0x0668, 0x20, 0x04, 1 << 15, 0 },
    { 0x0670, 0x20, 0x04, 1 << 15, 0 }, { 0x0671, 0x20, 0x04, 1 << 15, 0 }, { 0x0867, 0x20, 0x04, 1 << 15, 0 },
    { 0x0892, 0x20, 0x04, 1 << 15, 0 }, { 0x0897, 0x20, 0x04, 1 << 15, 0 },
    { 0x0883, 0x20, 0x0A, 1 << 10, 0 }, { 0x0887, 0x20, 0x0A, 1 << 10, 0 }, { 0x0888, 0x20, 0x0A, 1 << 10, 0 },
    { 0x0889, 0x20, 0x0A, 1 << 10, 0 }, { 0x0899, 0x20, 0x0A, 1 << 10, 0 }, { 0x1168, 0x20, 0x0A, 1 << 10, 0 },
    /* ALC255/256 family: 3k pull-down off, headphone amplifier at high power,
       pin 0x1A as headphone mic instead of PC beep */
    { 0x0230, 0x20, 0x46, 3 << 12, 0 }, { 0x0235, 0x20, 0x46, 3 << 12, 0 }, { 0x0236, 0x20, 0x46, 3 << 12, 0 },
    { 0x0255, 0x20, 0x46, 3 << 12, 0 }, { 0x0256, 0x20, 0x46, 3 << 12, 0 }, { 0x0257, 0x20, 0x46, 3 << 12, 0 },
    { 0x0230, 0x57, 0x04, 0x0007, 4 }, { 0x0235, 0x57, 0x04, 0x0007, 4 }, { 0x0236, 0x57, 0x04, 0x0007, 4 },
    { 0x0255, 0x57, 0x04, 0x0007, 4 }, { 0x0256, 0x57, 0x04, 0x0007, 4 }, { 0x0257, 0x57, 0x04, 0x0007, 4 },
    { 0x0230, 0x20, 0x36, 0xFFFF, 0x5757 }, { 0x0235, 0x20, 0x36, 0xFFFF, 0x5757 },
    { 0x0236, 0x20, 0x36, 0xFFFF, 0x5757 }, { 0x0255, 0x20, 0x36, 0xFFFF, 0x5757 },
    { 0x0256, 0x20, 0x36, 0xFFFF, 0x5757 }, { 0x0257, 0x20, 0x36, 0xFFFF, 0x5757 },
    /* ALC225 family: 3k pull-down off, headphone amplifier at high power */
    { 0x0225, 0x20, 0x4A, 3 << 10, 0 }, { 0x0295, 0x20, 0x4A, 3 << 10, 0 }, { 0x0299, 0x20, 0x4A, 3 << 10, 0 },
    { 0x0215, 0x20, 0x4A, 3 << 10, 0 }, { 0x0245, 0x20, 0x4A, 3 << 10, 0 }, { 0x0285, 0x20, 0x4A, 3 << 10, 0 },
    { 0x0289, 0x20, 0x4A, 3 << 10, 0 }, { 0x0287, 0x20, 0x4A, 3 << 10, 0 },
    { 0x0225, 0x57, 0x04, 0x0007, 4 }, { 0x0295, 0x57, 0x04, 0x0007, 4 }, { 0x0299, 0x57, 0x04, 0x0007, 4 },
    { 0x0215, 0x57, 0x04, 0x0007, 4 }, { 0x0245, 0x57, 0x04, 0x0007, 4 }, { 0x0285, 0x57, 0x04, 0x0007, 4 },
    { 0x0289, 0x57, 0x04, 0x0007, 4 }, { 0x0287, 0x57, 0x04, 0x0007, 4 },
};

/* ALC1220 notebooks (MSI, Clevo P950 and PB51 series): PCI subsystem device IDs */
static const u16 msi1220[] = { 0x11F7, 0x1228, 0x1229, 0x1275, 0x1276, 0x1293 };
static const u16 clevo1220[] = { 0x3702, 0x50D3, 0x5802, 0x65D1, 0x65D2, 0x65E1, 0x65E5, 0x65F1, 0x65F5,
    0x66A2, 0x66A6, 0x67D1, 0x67E1, 0x67E5, 0x67F1, 0x67F5, 0x70D1, 0x7714, 0x7715, 0x9501, 0x9506, 0x950A,
    0x95E1, 0x95E2, 0x95E3, 0x95E4, 0x95E5, 0x95E6, 0x96E1, 0x97E1, 0x97E2, 0xD502 };

static u32 coef_rd(struct hda *h, int nid, int idx)
{
    cmd(h, VERB4(h, nid, 0x5, idx));
    return cmd(h, VERB(h, nid, 0xC00, 0));         /* 0xFFFFFFFF: no answer */
}

static void coef_wr(struct hda *h, int nid, int idx, u32 v)
{
    cmd(h, VERB4(h, nid, 0x5, idx));
    cmd(h, VERB4(h, nid, 0x4, v & 0xFFFF));
}

static void coef_upd(struct hda *h, int nid, int idx, u32 mask, u32 set)
{
    u32 v = coef_rd(h, nid, idx);
    if (v != 0xFFFFFFFFUL) coef_wr(h, nid, idx, (v & ~mask) | set);
}

static int in_list(const u16 *l, int n, u16 v)
{
    while (n--) if (*l++ == v) return 1;
    return 0;
}

static u32 realtek_init(struct hda *h)
{
    u32 vid = param(h, 0, 0x00), coef0, ass, sub, gpio = 0, t;
    u16 id = (u16)vid, sv, sd;
    unsigned i;
    int c882;
    if ((vid >> 16) != 0x10EC) return 0;
    sub = pci_rd32(h->pci.bus, h->pci.dev, h->pci.fn, 0x2C);
    sv = (u16)sub;
    sd = (u16)(sub >> 16);
    coef0 = coef_rd(h, 0x20, 0) & 0xFFFF;
    for (i = 0; i < sizeof(rtcoefs) / sizeof(rtcoefs[0]); i++) {
        const struct rtcoef *c = &rtcoefs[i];
        if (c->id != id) continue;
        if (c->mask == 0xFFFF) coef_wr(h, c->nid, c->idx, c->set);
        else coef_upd(h, c->nid, c->idx, c->mask, c->set);
    }
    switch (id) {
    case 0x0269:                                    /* ALC269 VA/VB/VC/VD */
        if ((coef0 & 0xF0) == 0x10) {
            coef_upd(h, 0x20, 0x0D, 0, 1 << 14);
            if ((coef0 & 0xFF) < 0x15) {
                coef_wr(h, 0x20, 0x0F, 0x960B);
                coef_wr(h, 0x20, 0x0E, 0x8817);
            } else if ((coef0 & 0xFF) == 0x16) {
                coef_wr(h, 0x20, 0x0F, 0x960B);
                coef_wr(h, 0x20, 0x0E, 0x8814);
            } else if ((coef0 & 0xFF) == 0x17) {
                coef_upd(h, 0x20, 0x04, 0, 1 << 11);
            } else if ((coef0 & 0xFF) == 0x18) {
                t = coef_rd(h, 0x20, 0x0D);
                if (t != 0xFFFFFFFFUL && ((t >> 10) & 3) != 1) coef_wr(h, 0x20, 0x0D, t | (1 << 10));
                t = coef_rd(h, 0x20, 0x17);
                if (t != 0xFFFFFFFFUL && ((t >> 6) & 7) != 4) coef_wr(h, 0x20, 0x17, t | (1 << 7));
            }
            coef_upd(h, 0x20, 0x04, 0, 1 << 11);
        } else if ((coef0 & 0xF0) == 0x20) {
            coef_upd(h, 0x20, 0x04, 1 << 15, 0);
        } else if ((coef0 & 0xF0) == 0x30) {
            coef_upd(h, 0x20, 0x10, 1 << 9, 0);
        } else {
            coef_upd(h, 0x20, 0x04, 1 << 15, 0);    /* VA: PLL */
        }
        break;
    case 0x0662:
        if ((coef0 & 0xF0) == 0x30) coef_upd(h, 0x20, 0x04, 1 << 10, 0);
        break;
    case 0x0888:
        if ((coef0 & 0xF0) == 0x20 || (coef0 & 0xF0) == 0x30) coef_upd(h, 0x20, 0x07, 1 << 5, 0);
        break;
    }

    /* ALC882 family boards */
    c882 = id == 0x0882 || id == 0x0883 || id == 0x0885 || id == 0x0887 || id == 0x0888 || id == 0x0889 ||
           id == 0x0899 || id == 0x0900 || id == 0x0B00 || id == 0x1168 || id == 0x1220;
    if (c882) {
        if ((sv == 0x1462 && in_list(msi1220, sizeof(msi1220) / 2, sd)) ||
            (sv == 0x1558 && in_list(clevo1220, sizeof(clevo1220) / 2, sd))) {
            coef_upd(h, 0x20, 0x07, 0, 0x03C3);
        } else if (sv == 0x1458 && (sd == 0xA0CD || sd == 0xA0CE || sd == 0xA0D5)) {
            coef_wr(h, 0x20, 0x07, 0x03C0);         /* Gigabyte X570 */
            coef_wr(h, 0x20, 0x1A, 0x01C1);
            coef_wr(h, 0x20, 0x1B, 0x0202);
            coef_wr(h, 0x20, 0x43, 0x3005);
        } else if (sv == 0x1462 && sd != 0x7350 && sd != 0xCC34 && sd != 0xDA57) {
            gpio = 3;                               /* MSI */
        } else if (sv == 0x1558 && sd != 0x7709) {
            coef_wr(h, 0x20, 0x07, 0x3060);         /* Clevo: EAPD mode */
        }
    }

    /* assembly ID: a valid codec subsystem ID, else the checksummed config of pin 0x1D */
    ass = cmd(h, VERB(h, h->afg, 0xF20, 0)) & 0xFFFF;
    if (ass == sd || !(ass & 1)) {
        int pn = id == 0x0260 ? 0x17 : 0x1D, k;
        ass = valid_nid(h, pn) ? h->w[pn].cfg : 0;
        for (t = 0, k = 1; k < 16; k++) t += (ass >> k) & 1;
        if (!(ass & 1) || (ass >> 30) != 1 || ((ass >> 16) & 0xF) != t) ass = 0;
    }
    if (ass && !gpio) {
        t = (ass >> 3) & 7;
        gpio = t == 1 ? 1 : t == 3 ? 2 : t == 7 ? 4 : 0;
    }
    if (!gpio) {                                    /* amplifier bits of the old codecs */
        if (id == 0x0260) coef_upd(h, 0x1A, 0x07, 0, 0x2010);
        else if (id == 0x0880 || id == 0x0882 || id == 0x0883 || id == 0x0885) coef_upd(h, 0x20, 0x07, 0, 0x2030);
        else if (id == 0x0888 && (coef0 & 0xF0) <= 0x10) coef_upd(h, 0x20, 0x07, 0, 0x2030);
    }
    return gpio;
}

/* kind of a codec without parsing it (AF_*): modem / audio function group, HDMI or DP pins */
static int codec_kind(struct hda *h, int cad)
{
    u32 v, w, fv;
    int i, n, nid, k, kn, knid, kind = AF_HDA_OTHER;
    u8 save = h->cad;
    h->cad = (u8)cad;
    v = param(h, 0, 0x04);
    for (i = 0, n = (int)(v & 0xFF), nid = (int)((v >> 16) & 0xFF); v != 0xFFFFFFFFUL && i < n && i < 8; i++, nid++) {
        fv = param(h, nid, 0x05) & 0xFF;
        if (fv == 2) {
            kind = AF_HDA_MODEM;
            continue;
        }
        if (fv != 1) continue;
        w = param(h, nid, 0x04);
        for (k = 0, kn = (int)(w & 0xFF), knid = (int)((w >> 16) & 0xFF); w != 0xFFFFFFFFUL && k < kn && k < 96; k++, knid++) {
            u32 caps = param(h, knid, 0x09);
            if (((caps >> 20) & 0xF) == WT_PIN && (param(h, knid, 0x0C) & 0x01000080UL)) {
                kind = AF_HDA_HDMI;
                break;
            }
        }
        break;
    }
    h->cad = save;
    return kind;
}

static int parse_codec(struct hda *h, int cad)
{
    u32 v;
    int i, n, nid, k;
    u8 path[6];
    h->cad = (u8)cad;
    v = param(h, 0, 0x04);
    if (v == 0xFFFFFFFFUL) return -1;
    h->afg = 0;
    for (i = 0, n = (int)(v & 0xFF), nid = (int)((v >> 16) & 0xFF); i < n; i++, nid++)
        if ((param(h, nid, 0x05) & 0xFF) == 1) {
            h->afg = (u8)nid;
            break;
        }
    if (!h->afg) return -1;
    cmd(h, VERB(h, h->afg, 0x705, 0));                              /* AFG power D0 */
    mdelay(10);
    v = param(h, h->afg, 0x04);
    h->nstart = (u8)((v >> 16) & 0xFF);
    h->nend = (u8)MIN(h->nstart + (v & 0xFF), MAXNID);
    for (nid = h->nstart; nid < h->nend; nid++) {
        struct hwid *w = &h->w[nid];
        w->caps = param(h, nid, 0x09);
        w->type = (u8)((w->caps >> 20) & 0xF);
        if (w->type == WT_PIN) {
            w->pincap = param(h, nid, 0x0C);
            w->cfg = cmd(h, VERB(h, nid, 0xF1C, 0));
        }
        if (w->caps & 8) {
            w->ampin = param(h, nid, 0x0D);
            w->ampout = param(h, nid, 0x12);
        }
        read_conn(h, nid);
    }

    /* every widget powered up (some Realtek codecs keep unused converters in D3 and
       route through them), external amplifiers on through EAPD on every pin that has it */
    for (nid = h->nstart; nid < h->nend; nid++) {
        struct hwid *w = &h->w[nid];
        if (w->caps & 0x400) cmd(h, VERB(h, nid, 0x705, 0));
        if (w->type == WT_PIN && (w->pincap & 0x10000UL)) cmd(h, VERB(h, nid, 0x70C, 2));
    }
    {
        u32 ngpio = param(h, h->afg, 0x11) & 0xFF, all = ngpio >= 4 ? 0x0F : (1UL << ngpio) - 1;
        u32 m = realtek_init(h) & all;
        if (cb->opts & OPT_HDAGPIO) m = all;
        if (m) {
            cmd(h, VERB(h, h->afg, 0x716, m));          /* GPIO enable */
            cmd(h, VERB(h, h->afg, 0x717, m));          /* direction: out */
            cmd(h, VERB(h, h->afg, 0x715, m));          /* data: high */
        }
    }

    /* outputs: line-out first, then speakers, then headphones; pins whose default
       configuration says "no connection" only when no pin is configured at all */
    h->ndac = h->nopin = h->nvol = 0;
    for (k = 0; k < 6; k++) {
        static const u8 order[6] = { DD_LINEOUT, DD_SPEAKER, DD_HP, DD_LINEOUT, DD_SPEAKER, DD_HP };
        int nc = k >= 3;
        if (nc && h->nopin) break;
        for (nid = h->nstart; nid < h->nend && h->nopin < 8; nid++) {
            struct hwid *w = &h->w[nid];
            int len, j, vn = 0;
            if (w->type != WT_PIN || !(w->pincap & 0x10) || (w->pincap & 0x80)) continue;   /* output, not HDMI */
            if (((w->cfg >> 30) == 1) != nc || ((w->cfg >> 20) & 0xF) != order[k]) continue;
            len = h->ndac ? find_path(h, nid, WT_OUT, h->dac[0], path, 0) : 0;
            if (!len) len = find_path(h, nid, WT_OUT, 0, path, 0);
            if (!len) continue;
            /* path runs pin -> ... -> DAC: apply it from the pin */
            apply_path(h, path, len);
            cmd(h, VERB(h, nid, 0x707, 0x40 | (order[k] == DD_HP && (w->pincap & 8) ? 0x80 : 0)));
            if (w->pincap & 0x10000UL) cmd(h, VERB(h, nid, 0x70C, 2));            /* EAPD */
            h->opin[h->nopin] = (u8)nid;
            h->otype[h->nopin++] = order[k];
            for (j = 0; j < h->ndac && h->dac[j] != path[len - 1]; j++) ;
            if (j == h->ndac && h->ndac < 4) h->dac[h->ndac++] = path[len - 1];
            /* volume: the amplifier with steps closest to the DAC */
            for (j = len - 1; j >= 0; j--)
                if ((h->w[path[j]].caps & 4) && ((ampcaps(h, path[j], 1) >> 8) & 0x7F)) {
                    vn = path[j];
                    break;
                }
            if (vn && h->nvol < 4) {
                for (j = 0; j < h->nvol && h->volnid[j] != vn; j++) ;
                if (j == h->nvol) {
                    h->volnid[h->nvol] = (u8)vn;
                    h->volsteps[h->nvol++] = (u8)((ampcaps(h, vn, 1) >> 8) & 0x7F);
                }
            }
        }
    }
    if (!h->ndac) return -1;

    /* input: the analog ADC that reaches the most kinds of input pins (Realtek ALC256 and
       later: the first ADC only sees the digital microphone) and a path to each kind */
    h->adc = 0;
    for (k = 0, i = -1; k < 2; k++) {
        int adc, best = i;
        for (adc = h->nstart; adc < h->nend; adc++) {
            int cnt = 0;
            if (h->w[adc].type != WT_IN || (h->w[adc].caps & 0x200)) continue;
            if (k && adc != h->adc) continue;
            memset(h->srclen, 0, sizeof(h->srclen));
            for (nid = h->nstart; nid < h->nend; nid++) {
                struct hwid *w = &h->w[nid];
                int src, dd, len;
                if (w->type != WT_PIN || !(w->pincap & 0x20) || (w->cfg >> 30) == 1) continue;
                dd = (int)((w->cfg >> 20) & 0xF);
                src = dd == DD_MIC ? 0 : dd == DD_LINEIN ? 1 : dd == DD_CD ? 2 : dd == DD_AUX ? 3 : -1;
                if (src < 0 || h->srclen[src]) continue;
                len = find_path(h, adc, 0, nid, h->srcpath[src], 0);
                if (len) {
                    h->srclen[src] = (u8)len;
                    cnt++;
                }
            }
            if (!k && cnt > best) {
                best = cnt;
                h->adc = (u8)adc;
            }
        }
        i = best;
    }
    if (h->adc) {
        for (k = 0; k < 4; k++)
            if (h->srclen[k]) h->a.insrcs |= (u8)(1 << k);
        /* stereo mix: a mixer the ADC can reach with a playback DAC on one of its inputs
           (not the Realtek analog loopback mixer 0x0B: it mixes what comes in on the pins) */
        for (nid = h->nstart; nid < h->nend && !h->srclen[4]; nid++) {
            struct hwid *w = &h->w[nid];
            int len, j;
            if (w->type != WT_MIX) continue;
            for (k = 0; k < w->nconn; k++) {
                for (j = 0; j < h->ndac && h->dac[j] != w->conn[k]; j++) ;
                if (j < h->ndac) break;
            }
            if (k == w->nconn) continue;
            len = find_path(h, h->adc, 0, nid, h->srcpath[4], 0);
            if (len) {
                h->srclen[4] = (u8)len;
                h->mixin = (u8)k;
                h->a.insrcs |= AS_MIX;
            }
        }
    }
    return 0;
}

static void set_src(struct adev *a, int src)
{
    struct hda *h = a->priv;
    int i, s;
    for (s = 0; s < 5 && !(src & (1 << s)); s++) ;
    if (s == 5 || !h->srclen[s]) return;
    apply_path(h, h->srcpath[s], h->srclen[s]);
    i = h->srcpath[s][h->srclen[s] - 1];
    if (s == 4 && (h->w[i].caps & 2)) {
        int k;
        for (k = 0; k < h->w[i].nconn; k++) setamp(h, i, 0, k, k != h->mixin, (int)(ampcaps(h, i, 0) & 0x7F));
    }
    if (h->w[i].type == WT_PIN) {
        u32 vref = (h->w[i].pincap >> 8) & 0xFF, ctl = 0x20;
        if (s == 0) ctl |= (vref & 0x10) ? 4 : (vref & 2) ? 1 : 0;      /* mic bias 80% / 50% */
        cmd(h, VERB(h, i, 0x707, ctl));
        if (h->w[i].caps & 2) setamp(h, i, 0, 0, 0, (int)(ampcaps(h, i, 0) & 0x7F));
    }
}

static void set_vol(struct adev *a, int ctl, u32 lr)
{
    struct hda *h = a->priv;
    int i;
    if (ctl == MX_MASTER) {
        for (i = 0; i < h->nvol; i++) {
            u32 st = h->volsteps[i];
            u32 l = ((lr & 0xFFFF) * st + 32767) / 65535, r = ((lr >> 16) * st + 32767) / 65535;
            u32 base = 0x8000 | 0x0000;
            cmd(h, VERB4(h, h->volnid[i], 0x3, base | 0x2000 | (l ? l : 0x80)));
            cmd(h, VERB4(h, h->volnid[i], 0x3, base | 0x1000 | (r ? r : 0x80)));
        }
    } else if (ctl == MX_INGAIN && h->adc && (h->w[h->adc].caps & 2)) {
        u32 st = (ampcaps(h, h->adc, 0) >> 8) & 0x7F;
        u32 g = ((lr & 0xFFFF) * st + 32767) / 65535;
        setamp(h, h->adc, 0, 0, 0, (int)g);
    }
}

/* speakers off while headphones are plugged in */
static void hda_poll(struct adev *a)
{
    struct hda *h = a->priv;
    int i, hp = 0, any = 0;
    if (h->dsp) dsp_poll_all();
    if (plat_ms() - h->hp_time < 300) return;
    h->hp_time = plat_ms();
    for (i = 0; i < h->nopin; i++) {
        struct hwid *w = &h->w[h->opin[i]];
        if (h->otype[i] != DD_HP || !(w->pincap & 4) || (w->cfg & 0x100)) continue;
        any = 1;
        if (w->pincap & 2) cmd(h, VERB(h, h->opin[i], 0x709, 0));      /* trigger required */
        if (cmd(h, VERB(h, h->opin[i], 0xF09, 0)) & 0x80000000UL) hp = 1;
    }
    if (!any || (h->hp_known && hp == h->hp_present)) return;
    h->hp_known = 1;
    h->hp_present = (u8)hp;
    for (i = 0; i < h->nopin; i++)
        if (h->otype[i] == DD_SPEAKER) cmd(h, VERB(h, h->opin[i], 0x707, hp ? 0 : 0x40));
    kprintf("hda: headphones %s\n", hp ? "plugged in, speakers off" : "out, speakers on");
}

/* ---------------------------------------------------------------- streams */

static int sd_reset(struct hda *h, int sd)
{
    int o = SD(sd);
    W8(h, o + SDCTL, R8(h, o + SDCTL) & ~2);
    W8(h, o + SDCTL, R8(h, o + SDCTL) | 1);
    if (wait16(h, o + SDCTL, 1, 1, 50)) return -1;
    W8(h, o + SDCTL, R8(h, o + SDCTL) & ~1);
    return wait16(h, o + SDCTL, 1, 0, 50);
}

static int hda_start(struct adev *a, int dir)
{
    struct hda *h = a->priv;
    struct ahw *hw = dir ? &a->in : &a->out;
    int sd = dir ? h->isd : h->osd, o = SD(sd), i, n = (int)(hw->size / hw->period), tag = dir ? 2 : 1;
    u32 *b = h->bdl[dir];
    if (h->dspep[dir]) {
        /* through the DSP: host DMA into the firmware, link DMA to the codec */
        if (dsp_hda_start(h->dspep[dir], hw, &tag)) return -1;
        if (dir) {
            cmd(h, VERB4(h, h->adc, 0x2, h->fmt));
            cmd(h, VERB(h, h->adc, 0x706, tag << 4));
        } else {
            for (i = 0; i < h->ndac; i++) {
                cmd(h, VERB4(h, h->dac[i], 0x2, h->fmt));
                cmd(h, VERB(h, h->dac[i], 0x706, tag << 4));
            }
        }
        return 0;
    }
    if (sd < 0 || sd_reset(h, sd)) return -1;
    for (i = 0; i < n; i++) {
        b[i * 4] = vphys(hw->buf + (u32)i * hw->period);
        b[i * 4 + 1] = 0;
        b[i * 4 + 2] = hw->period;
        b[i * 4 + 3] = 1;
    }
    if (!a->out.noflush) aeng_flush_dma(b, (u32)n * 16);
    W32(h, o + SDCBL, hw->size);
    W16(h, o + SDLVI, n - 1);
    W16(h, o + SDFMT, h->fmt);
    W32(h, o + SDBDPL, vphys(b));
    W32(h, o + SDBDPU, 0);
    W8(h, o + SDCTL + 2, (u8)((tag << 4) | (R8(h, o + SDCTL + 2) & 0x0F)));
    W8(h, o + SDSTS, 0x1C);
    if (dir) {
        cmd(h, VERB4(h, h->adc, 0x2, h->fmt));
        cmd(h, VERB(h, h->adc, 0x706, tag << 4));
    } else {
        for (i = 0; i < h->ndac; i++) {
            cmd(h, VERB4(h, h->dac[i], 0x2, h->fmt));
            cmd(h, VERB(h, h->dac[i], 0x706, tag << 4));
        }
    }
    W8(h, o + SDCTL, R8(h, o + SDCTL) | 2);
    return 0;
}

static void hda_stop(struct adev *a, int dir)
{
    struct hda *h = a->priv;
    int sd = dir ? h->isd : h->osd, i;
    if (h->dspep[dir]) {
        dsp_hda_stop(h->dspep[dir]);
    } else {
        if (sd < 0) return;
        W8(h, SD(sd) + SDCTL, R8(h, SD(sd) + SDCTL) & ~2);
        wait16(h, SD(sd) + SDCTL, 2, 0, 20);
    }
    if (dir) cmd(h, VERB(h, h->adc, 0x706, 0));
    else for (i = 0; i < h->ndac; i++) cmd(h, VERB(h, h->dac[i], 0x706, 0));
}

static u32 hda_pos(struct adev *a, int dir)
{
    struct hda *h = a->priv;
    int sd = dir ? h->isd : h->osd;
    if (h->dspep[dir]) return dsp_hda_pos(h->dspep[dir]);
    if (h->useposbuf) {
        if (!a->out.noflush) aeng_flush_dma(&h->posbuf[sd * 2], 4);
        return h->posbuf[sd * 2];
    }
    return R32(h, SD(sd) + SDLPIB);
}

static void hda_shutdown(struct adev *a)
{
    struct hda *h = a->priv;
    corb_stop(h);
    W32(h, DPLBASE, 0);
    if (h->dsp) {
        dsp_detach(h->dsp);
        h->dsp = NULL;
        h->dspep[0] = h->dspep[1] = NULL;
    }
}

static struct adevops hda_ops = { hda_start, hda_stop, hda_pos, set_vol, set_src, hda_poll, hda_shutdown };

/* ---------------------------------------------------------------- controller */

static int intel_ich(u16 d)
{
    return d == 0x2668 || d == 0x27D8 || d == 0x269A || d == 0x284B || d == 0x293E || d == 0x293F ||
           d == 0x3A3E || d == 0x3A6E || d == 0x811B;
}

/* Intel Skylake and later (CGCTL clock gating must be off during the link reset) */
static int intel_skl(u16 d)
{
    static const u16 ids[] = { 0xA170, 0x9D70, 0xA171, 0x9D71, 0xA2F0, 0xA348, 0x9DC8, 0x02C8, 0x06C8, 0xF1C8,
                               0xA3F0, 0x4DC8, 0x43C8, 0xA0C8, 0x51C8, 0x51C9, 0x51CC, 0x54C8, 0x7AD0, 0x7A50,
                               0x7E28, 0x7F50, 0xA828, 0xE428, 0x5A98, 0x1A98, 0x3198, 0x4B55, 0x4B58, 0x38C8,
                               0x34C8, 0x3DC8, 0x18D9, 0x02C9, 0x06C9, 0 };
    int i;
    for (i = 0; ids[i]; i++) if (ids[i] == d) return 1;
    return 0;
}

/* cache coherency of the DMA buffers (as the Linux driver does) */
static int snoop_setup(struct pcidev *p)
{
    u8 v;
    switch (p->vendor) {
    case 0x8086:
        v = pci_rd8(p->bus, p->dev, p->fn, 0x44);
        pci_wr8(p->bus, p->dev, p->fn, 0x44, (u8)(v & ~7));         /* TCSEL: traffic class 0 */
        if (!intel_ich(p->device) || p->device == 0x811B) {
            u16 devc = pci_rd16(p->bus, p->dev, p->fn, 0x78);
            if (devc & 0x800) pci_wr16(p->bus, p->dev, p->fn, 0x78, (u16)(devc & ~0x800));   /* NOSNOOP off */
        }
        return 1;
    case 0x1002:
    case 0x1022:
        v = pci_rd8(p->bus, p->dev, p->fn, 0x42);
        pci_wr8(p->bus, p->dev, p->fn, 0x42, (u8)((v & ~7) | 2));   /* ENABLE_SNOOP */
        return 1;
    case 0x10DE:
        v = pci_rd8(p->bus, p->dev, p->fn, 0x4E);
        pci_wr8(p->bus, p->dev, p->fn, 0x4E, (u8)((v & ~0x0F) | 0x0F));
        pci_wr8(p->bus, p->dev, p->fn, 0x4D, (u8)(pci_rd8(p->bus, p->dev, p->fn, 0x4D) | 1));
        pci_wr8(p->bus, p->dev, p->fn, 0x4C, (u8)(pci_rd8(p->bus, p->dev, p->fn, 0x4C) | 1));
        return 1;
    }
    return 0;                       /* VIA, SiS, ULi, others: caches are written back by hand */
}

static int link_reset(struct hda *h)
{
    u32 cg = 0;
    int skl = h->pci.vendor == 0x8086 && intel_skl(h->pci.device);
    if (skl) {
        cg = pci_rd32(h->pci.bus, h->pci.dev, h->pci.fn, 0x48);
        pci_wr32(h->pci.bus, h->pci.dev, h->pci.fn, 0x48, cg & ~0x40UL);     /* MISCBDCGE off */
    }
    corb_stop(h);
    W16(h, STATESTS, 0x7FFF);
    W32(h, GCTL, R32(h, GCTL) & ~1UL);
    {
        u32 t0 = plat_ms();
        while ((R32(h, GCTL) & 1) && plat_ms() - t0 < 100) ;
    }
    udelay(500);
    W32(h, GCTL, R32(h, GCTL) | 1);
    {
        u32 t0 = plat_ms();
        while (!(R32(h, GCTL) & 1) && plat_ms() - t0 < 100) ;
    }
    mdelay(2);                      /* codecs announce themselves within 521 us */
    if (skl) pci_wr32(h->pci.bus, h->pci.dev, h->pci.fn, 0x48, cg);
    return (R32(h, GCTL) & 1) ? 0 : -1;
}

static void corb_setup(struct hda *h)
{
    u8 cs = R8(h, CORBSIZE), rs = R8(h, RIRBSIZE);
    h->corbn = (cs & 0x40) ? 256 : (cs & 0x20) ? 16 : 2;
    h->rirbn = (rs & 0x40) ? 256 : (rs & 0x20) ? 16 : 2;
    memset(h->corb, 0, 1024);
    memset(h->rirb, 0, 2048);
    if (!h->a.out.noflush) {
        aeng_flush_dma(h->corb, 1024);
        aeng_flush_dma(h->rirb, 2048);
    }
    W8(h, CORBSIZE, (cs & ~3) | (h->corbn == 256 ? 2 : h->corbn == 16 ? 1 : 0));
    W8(h, RIRBSIZE, (rs & ~3) | (h->rirbn == 256 ? 2 : h->rirbn == 16 ? 1 : 0));
    W32(h, CORBLBASE, vphys(h->corb));
    W32(h, CORBUBASE, 0);
    W32(h, RIRBLBASE, vphys(h->rirb));
    W32(h, RIRBUBASE, 0);
    /* CORB read pointer reset: some controllers never show the bit, so no hard failure */
    W16(h, CORBRP, 0x8000);
    wait16(h, CORBRP, 0x8000, 0x8000, 5);
    W16(h, CORBRP, 0);
    wait16(h, CORBRP, 0x8000, 0, 5);
    W16(h, CORBWP, 0);
    W16(h, RIRBWP, 0x8000);
    W16(h, RINTCNT, 1);
    h->rp = 0;
    W8(h, CORBCTL, 2);
    /* response interrupt control on: some controllers (and QEMU) only count responses again
       after RINTFL is cleared; the line itself stays off (INTCTL = 0) */
    W8(h, RIRBCTL, 3);
}

static void alloc_ring(struct ahw *hw, u8 *buf, u32 rate)
{
    u8 nf = hw->noflush;
    hw->buf = buf;
    hw->period = plat_mode == PM_DOS ? PERIOD * 2 : PERIOD;     /* DOS: polled every 55 ms */
    hw->size = hw->period * NPERIOD;
    hw->rate = rate;
    hw->present = 1;
    hw->noflush = nf;
}

int hda_probe(struct pcidev *p)
{
    struct hda *h;
    u32 phys = pci_bar_mem(p, 0, NULL), rates;
    u8 *r;
    u16 st;
    int cad, ok = -1;
    if (!phys) return -1;
    pci_enable(p, 1, 0, 1);
    r = plat_mmio(phys, 0x4000);
    if (!r || RD8(r + VMAJ) != 1) return -1;                   /* not HDA registers (e.g. AC'97 in 04/01) */
    /* the DOS core probes again after Windows: same controller, same memory */
    for (cad = 0; cad < nctrl && PCI_BDF(&ctrls[cad]->pci) != PCI_BDF(p); cad++) ;
    if (cad < nctrl) {
        h = ctrls[cad];
        h->imm = 0;
        h->hp_known = 0;
        memset(&h->a.out, 0, sizeof(h->a.out));
        memset(&h->a.in, 0, sizeof(h->a.in));
        h->a.gone = 0;
    } else {
        if (nctrl >= MAXHDA) return -1;
        h = kmalloc(sizeof(*h));
        memset(h, 0, sizeof(*h));
        h->ringbuf[0] = kmalloc_align(RINGSIZE, 4096);
        h->ringbuf[1] = kmalloc_align(RINGSIZE, 4096);
        h->corb = kmalloc_align(1024, 1024);
        h->rirb = kmalloc_align(2048, 2048);
        h->posbuf = kmalloc_align(256, 128);
        h->bdl[0] = kmalloc_align(16 * 64, 128);
        h->bdl[1] = kmalloc_align(16 * 64, 128);
        if (!h->ringbuf[1] || !h->bdl[1]) return -1;
        ctrls[nctrl++] = h;
    }
    h->pci = *p;
    cad = 0;
    h->r = r;
    h->a.out.noflush = h->a.in.noflush = (u8)snoop_setup(p);
    if (link_reset(h)) {
        kprintf("hda: %02x:%02x.%u link reset failed\n", p->bus, p->dev, p->fn);
        return -1;
    }
    W32(h, INTCTL, 0);
    corb_setup(h);
    st = R16(h, STATESTS);
    {
        u16 gcap = R16(h, GCAP);
        int iss = (gcap >> 8) & 0xF, oss = (gcap >> 12) & 0xF;
        h->osd = oss ? iss : -1;
        h->isd = iss ? 0 : -1;
        kprintf("hda: %02x:%02x.%u %04x:%04x, codecs %04x, %d in / %d out streams\n", p->bus, p->dev, p->fn,
                p->vendor, p->device, st, iss, oss);
    }
    memset(h->posbuf, 0, 256);
    W32(h, DPLBASE, vphys(h->posbuf) | 1);
    W32(h, DPUBASE, 0);
    h->useposbuf = p->vendor == 0x1106 || (cb->opts & OPT_POSBUF);
    if (cb->opts & OPT_HDAIMM) {
        corb_stop(h);
        h->imm = 1;
    }
    h->ncodec = 0;
    h->started = 0;
    for (cad = 0; cad < 15; cad++) {
        u32 vid;
        struct hcodec *hc;
        u8 keep = h->cad;
        if (!(st & (1 << cad))) continue;
        h->cad = (u8)cad;
        vid = param(h, 0, 0x00);
        if (vid == 0xFFFFFFFFUL || !vid) {
            if (!ok) h->cad = keep;
            continue;
        }
        hc = &h->codec[h->ncodec++];
        hc->cad = (u8)cad;
        hc->vid = vid;
        hc->sub = param(h, 0, 0x01);
        hc->rev = param(h, 0, 0x02);
        if (!ok) {
            /* the sound device has its codec: the others are only listed */
            h->cad = keep;
            hc->kind = (u8)codec_kind(h, cad);
            continue;
        }
        memset(h->w, 0, sizeof(h->w));
        h->a.insrcs = 0;
        memset(h->srclen, 0, sizeof(h->srclen));
        ok = parse_codec(h, cad);
        hc->kind = (u8)(ok ? codec_kind(h, cad) : AF_HDA_AUDIO);
        kprintf("hda: codec %d %08lx: %s, %d output pin(s), %d DAC(s)%s\n", cad, vid,
                ok ? (hc->kind == AF_HDA_HDMI ? "HDMI / DisplayPort" : hc->kind == AF_HDA_MODEM ? "modem" :
                      "no analog output") : "analog", h->nopin, h->ndac, h->adc ? ", ADC" : "");
        if (!ok) {
            h->a.vid = (u16)(vid >> 16);
            h->a.pid = (u16)vid;
        }
    }
    if (!ok) {
        /* parse_codec of the analog codec left h->cad on it */
        for (cad = 0; cad < h->ncodec && h->codec[cad].kind != AF_HDA_AUDIO; cad++) ;
        if (cad < h->ncodec) h->cad = h->codec[cad].cad;
    }
    /* Intel audio DSP (SST / SOF): started when the machine has I2S codecs, digital
       microphones or SoundWire (NHLT / ACPI), when there is no HD Audio codec, or with /DSP */
    if (h->dsp) {
        dsp_detach(h->dsp);
        h->dsp = NULL;
    }
    h->dspep[0] = h->dspep[1] = NULL;
    if (dsp_platform(p->vendor, p->device) && !(cb->opts2 & OPT2_NODSP) &&
        (ok || (cb->opts2 & OPT2_DSP) || dsp_wanted())) {
        h->dsp = dsp_attach(p, r, !ok);
        if (h->dsp && !ok && (cb->opts2 & OPT2_DSP)) {
            h->dspep[0] = dspdev_hda_ep(h->dsp, 0);
            if (h->adc) h->dspep[1] = dspdev_hda_ep(h->dsp, 1);
        }
    }
    if (ok || h->osd < 0) {
        if (!st && p->vendor == 0x8086 && (p->sub == 1 || p->sub == 0x80) && !h->dsp)
            kprintf("hda: %02x:%02x.%u Intel DSP (SST/SOF) function without HD Audio codecs and without a\n"
                    "hda: working DSP firmware: set the BIOS audio mode to HD Audio (legacy) if the machine\n"
                    "hda: offers it, or put the firmware files next to the driver\n", p->bus, p->dev, p->fn);
        return h->dsp ? 0 : -1;
    }
    rates = param(h, h->dac[0], 0x0A);
    if (!rates) rates = param(h, h->afg, 0x0A);
    if ((rates & 0x40) || !(rates & 0x20)) {
        h->fmt = 0x0011;            /* 48 kHz, 16 bit, stereo */
        alloc_ring(&h->a.out, h->ringbuf[0], 48000);
    } else {
        h->fmt = 0x4011;            /* 44.1 kHz */
        alloc_ring(&h->a.out, h->ringbuf[0], 44100);
    }
    h->a.caps = WINAC_OUT;
    if (h->adc && h->isd >= 0 && h->a.insrcs) {
        alloc_ring(&h->a.in, h->ringbuf[1], h->a.out.rate);
        h->a.caps |= WINAC_IN;
    }
    h->a.in.noflush = h->a.out.noflush;
    h->a.ops = &hda_ops;
    h->a.priv = h;
    h->a.type = AT_HDA;
    h->a.hwvol = (u8)(h->nvol != 0);
    codec_name(((u32)h->a.vid << 16) | h->a.pid, h->a.name, sizeof(h->a.name));
    if (h->a.insrcs) {
        int s;
        for (s = 0; s < 5 && !(h->a.insrcs & (1 << s)); s++) ;
        h->a.insrc = (u8)(1 << s);
        set_src(&h->a, h->a.insrc);
    }
    adev_register(&h->a);
    h->started = 1;
    return 0;
}

/* the codecs of a controller (and its DSP's devices) for the Plug and Play enumerator */
int hda_functions(u32 bdf, struct afunc *out, int max)
{
    int i, k, n = 0;
    for (i = 0; i < nctrl; i++) {
        struct hda *h = ctrls[i];
        if (PCI_BDF(&h->pci) != bdf || !h->r) continue;
        for (k = 0; k < h->ncodec && n < max; k++) {
            struct afunc *f = &out[n++];
            memset(f, 0, sizeof(*f));
            f->kind = h->codec[k].kind;
            f->index = h->codec[k].cad;
            f->vendev = h->codec[k].vid;
            f->subsys = h->codec[k].sub;
            f->rev = h->codec[k].rev;
            f->hasdev = (u8)(h->started && f->kind == AF_HDA_AUDIO && h->codec[k].cad == h->cad && !h->a.gone);
            codec_name(f->vendev, f->name, sizeof(f->name));
        }
        if (h->dsp) n += dsp_functions(h->dsp, out + n, max - n);
        break;
    }
    return n;
}

void hda_remove(u32 bdf)
{
    int i;
    dsp_remove(bdf);
    for (i = 0; i < nctrl; i++)
        if (PCI_BDF(&ctrls[i]->pci) == bdf) {
            ctrls[i]->dsp = NULL;                   /* detached by dsp_remove */
            ctrls[i]->dspep[0] = ctrls[i]->dspep[1] = NULL;
            aeng_remove(&ctrls[i]->a);
        }
}
