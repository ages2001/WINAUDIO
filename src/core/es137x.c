/* es137x.c - Ensoniq AudioPCI: ES1370 (AK4531 codec) and ES1371 / ES1373, Creative CT5880,
   Ectiva EV1938 (AC'97 codec, sample rate converter). The DAC2 channel plays and the ADC
   channel records one 16-bit stereo ring each, round and round in loop mode; the position is
   the frame counter of the channel. The ES1370 runs at 44100 Hz (DAC2 and ADC share its
   clock divider), the ES1371 at 48000 Hz through its rate converter. */
#include "audio.h"

#define MAXES       4
#define RINGSIZE    65536UL         /* bytes of a ring: at most 0x10000 dwords */
#define PERIOD      4096UL

/* registers (I/O BAR 0) */
#define ES_CONTROL      0x00
#define ES_STATUS       0x04
#define ES_MEMPAGE      0x0C
#define ES_1370_CODEC   0x10
#define ES_1371_SMPRATE 0x10
#define ES_1371_CODEC   0x14
#define ES_1371_LEGACY  0x18
#define ES_SERIAL       0x20
#define ES_DAC2_COUNT   0x28
#define ES_ADC_COUNT    0x2C
#define ES_FRAME        0x30        /* page 0Ch: DAC1, page 0Dh: ADC */
#define ES_SIZE         0x34
#define ES_DAC2_FRAME   0x38        /* page 0Ch */
#define ES_DAC2_SIZE    0x3C
#define ES_PHANTOM_FRAME 0x38       /* page 0Dh */
#define ES_PHANTOM_COUNT 0x3C
#define ES_PAGE_DAC     0x0C
#define ES_PAGE_ADC     0x0D

/* CONTROL */
#define C_1370_PCLKDIV(d)   (((u32)(d) & 0x1FFF) << 16)
#define C_1371_SYNC_RES     (1UL << 14)
#define C_DAC2_EN           (1UL << 5)
#define C_ADC_EN            (1UL << 4)
#define C_1370_CDC_EN       (1UL << 1)
#define C_1370_SERR_DIS     (1UL << 0)

/* STATUS */
#define S_1370_CSTAT        (1UL << 10)
#define S_1371_AC97_RST     (1UL << 29)

/* SERIAL */
#define SE_P2_END_INC(o)    (((u32)(o) & 7) << 19)
#define SE_P2_ST_INC(o)     (((u32)(o) & 7) << 16)
#define SE_R1_MODE(m)       (((u32)(m) & 3) << 4)
#define SE_P2_MODE(m)       (((u32)(m) & 3) << 2)

/* ES1371 codec and rate converter */
#define K_CODEC_RDY         (1UL << 31)
#define K_CODEC_WIP         (1UL << 30)
#define K_EV1938_MAGIC      (1UL << 26)
#define K_CODEC_PIRD        (1UL << 23)
#define R_SRC_WE            (1UL << 24)
#define R_SRC_BUSY          (1UL << 23)
#define R_SRC_DISABLE       (1UL << 22)
#define R_DIS_P1            (1UL << 21)
#define R_DIS_P2            (1UL << 20)
#define R_DIS_R1            (1UL << 19)
#define R_KEEP              (R_SRC_DISABLE | R_DIS_P1 | R_DIS_P2 | R_DIS_R1)
#define SRC_DAC1            0x70
#define SRC_DAC2            0x74
#define SRC_ADC             0x78
#define SRC_VOL_ADC         0x6C
#define SRC_VOL_DAC1        0x7C
#define SRC_VOL_DAC2        0x7E
#define SRC_TRUNC_N         0x00
#define SRC_INT_REGS        0x01
#define SRC_VFREQ_FRAC      0x03

#define CHIP_1370   0
#define CHIP_1371   1

/* AK4531 (ES1370) */
#define AK_MASTER_L 0x00
#define AK_VOICE_L  0x02
#define AK_FM_L     0x04
#define AK_CD_L     0x06
#define AK_LINE_L   0x08
#define AK_AUX_L    0x0A
#define AK_MONO1    0x0C
#define AK_MONO2    0x0D
#define AK_MIC      0x0E
#define AK_MONOOUT  0x0F
#define AK_OMIX1    0x10
#define AK_OMIX2    0x11
#define AK_LIN1     0x12
#define AK_RIN1     0x13
#define AK_LIN2     0x14
#define AK_RIN2     0x15
#define AK_RESET    0x16
#define AK_CLOCK    0x17
#define AK_ADSEL    0x18
#define AK_MICAMP   0x19

struct es {
    struct adev a;
    struct pcidev pci;
    int chip;
    u16 io;
    u32 ctrl, sctrl;
    u8 *ring[2];
    u8 *phantom;                /* ES1370: the phantom frame of its DMA */
    struct ac97 *codec;         /* ES1371 */
    u8 cd, line;                /* ES1370: the CD and line inputs are heard */
};

static struct es *chips[MAXES];
static int nchips;

static void es_page(struct es *e, int page)
{
    outl((u16)(e->io + ES_MEMPAGE), (u32)page);
}

/* ---------------------------------------------------------------- ES1370: AK4531 */

static void ak_wr(struct es *e, int reg, u8 v)
{
    u32 t0 = plat_ms();
    while ((inl((u16)(e->io + ES_STATUS)) & S_1370_CSTAT) && plat_ms() - t0 < 5) ;
    outw((u16)(e->io + ES_1370_CODEC), (u16)((reg << 8) | v));
}

/* 0..31 steps of 2 dB from the top of a volume linear in amplitude (31 for silence) */
static u32 ak_att(u32 v)
{
    u32 a = aeng_att_cdb(v);
    if (a == 0xFFFFFFFFUL) return 31;
    a = (a + 100) / 200;
    return a > 31 ? 31 : a;
}

/* a gain register (+12 dB .. -50 dB, 0 dB = 6) of a stereo pair, muted at 0 */
static void ak_gain(struct es *e, int reg, u32 lr)
{
    u32 l = lr & 0xFFFF, r = lr >> 16, gl = 6 + ak_att(l), gr = 6 + ak_att(r);
    ak_wr(e, reg, (u8)(l ? MIN(gl, 31UL) : 0x9F));
    ak_wr(e, reg + 1, (u8)(r ? MIN(gr, 31UL) : 0x9F));
}

static void ak_omix(struct es *e)
{
    ak_wr(e, AK_OMIX1, (u8)((e->line ? 0x18 : 0) | (e->cd ? 0x06 : 0)));
    ak_wr(e, AK_OMIX2, 0x0C);       /* voice (the DACs) */
}

static void ak_init(struct es *e)
{
    int i;
    ak_wr(e, AK_RESET, 0x03);       /* out of reset, powered */
    mdelay(1);
    ak_wr(e, AK_CLOCK, 0x00);
    for (i = AK_MASTER_L; i <= AK_MONO2; i++) ak_wr(e, i, 0x9F);   /* muted */
    ak_wr(e, AK_MASTER_L, 0x00);
    ak_wr(e, AK_MASTER_L + 1, 0x00);
    ak_wr(e, AK_VOICE_L, 0x06);     /* 0 dB */
    ak_wr(e, AK_VOICE_L + 1, 0x06);
    ak_wr(e, AK_MIC, 0x86);
    ak_wr(e, AK_MONOOUT, 0x80);
    ak_wr(e, AK_ADSEL, 0x00);       /* the ADC records the input mixer */
    ak_wr(e, AK_MICAMP, 0x00);
    e->cd = 1;
    e->line = 0;
    ak_gain(e, AK_CD_L, 0xC000C000UL);
    ak_omix(e);
}

/* ---------------------------------------------------------------- ES1371: rate converter, codec */

static u32 src_ready(struct es *e)
{
    u32 r = 0, t0 = plat_ms();
    while (plat_ms() - t0 < 10) {
        r = inl((u16)(e->io + ES_1371_SMPRATE));
        if (!(r & R_SRC_BUSY)) return r;
    }
    return r;
}

static void src_wr(struct es *e, int reg, u16 v)
{
    u32 r = src_ready(e) & R_KEEP;
    outl((u16)(e->io + ES_1371_SMPRATE), r | ((u32)reg << 25) | v | R_SRC_WE);
}

static u16 src_rd(struct es *e, int reg)
{
    u32 r = src_ready(e) & R_KEEP, t, t0;
    outl((u16)(e->io + ES_1371_SMPRATE), r | ((u32)reg << 25) | 0x10000UL);
    t = src_ready(e);
    if ((t & 0x00870000UL) != 0x00010000UL) {
        t0 = plat_ms();
        while (((t = inl((u16)(e->io + ES_1371_SMPRATE))) & 0x00870000UL) != 0x00010000UL && plat_ms() - t0 < 10) ;
    }
    r = src_ready(e) & R_KEEP;
    outl((u16)(e->io + ES_1371_SMPRATE), r | ((u32)reg << 25));
    return (u16)t;
}

static void dac2_rate(struct es *e, u32 rate)
{
    u32 freq = ((rate << 15) + 1500) / 3000, r;
    r = (src_ready(e) & (R_SRC_DISABLE | R_DIS_P1 | R_DIS_R1)) | R_DIS_P2;
    outl((u16)(e->io + ES_1371_SMPRATE), r);
    src_wr(e, SRC_DAC2 + SRC_INT_REGS, (u16)((src_rd(e, SRC_DAC2 + SRC_INT_REGS) & 0x00FF) | ((freq >> 5) & 0xFC00)));
    src_wr(e, SRC_DAC2 + SRC_VFREQ_FRAC, (u16)(freq & 0x7FFF));
    r = src_ready(e) & (R_SRC_DISABLE | R_DIS_P1 | R_DIS_R1);
    outl((u16)(e->io + ES_1371_SMPRATE), r);
}

static void adc_rate(struct es *e, u32 rate)
{
    u32 n = rate / 3000, truncm, freq, r;
    if ((1UL << n) & ((1UL << 15) | (1UL << 13) | (1UL << 11) | (1UL << 9))) n--;
    truncm = (21 * n - 1) | 1;
    freq = ((48000UL << 15) / rate) * n;
    r = (src_ready(e) & (R_SRC_DISABLE | R_DIS_P1 | R_DIS_P2)) | R_DIS_R1;
    outl((u16)(e->io + ES_1371_SMPRATE), r);
    if (rate >= 24000) {
        if (truncm > 239) truncm = 239;
        src_wr(e, SRC_ADC + SRC_TRUNC_N, (u16)((((239 - truncm) >> 1) << 9) | (n << 4)));
    } else {
        if (truncm > 119) truncm = 119;
        src_wr(e, SRC_ADC + SRC_TRUNC_N, (u16)(0x8000 | (((119 - truncm) >> 1) << 9) | (n << 4)));
    }
    src_wr(e, SRC_ADC + SRC_INT_REGS, (u16)((src_rd(e, SRC_ADC + SRC_INT_REGS) & 0x00FF) | ((freq >> 5) & 0xFC00)));
    src_wr(e, SRC_ADC + SRC_VFREQ_FRAC, (u16)(freq & 0x7FFF));
    src_wr(e, SRC_VOL_ADC, (u16)(n << 8));
    src_wr(e, SRC_VOL_ADC + 1, (u16)(n << 8));
    r = src_ready(e) & (R_SRC_DISABLE | R_DIS_P1 | R_DIS_P2);
    outl((u16)(e->io + ES_1371_SMPRATE), r);
}

/* the codec register interface shares the bus with the rate converter: an access waits for
   the converter's safe state, as Ensoniq's and Linux's drivers do */
static int codec_safe(struct es *e, u32 *saved)
{
    u32 t0 = plat_ms();
    while (inl((u16)(e->io + ES_1371_CODEC)) & K_CODEC_WIP)
        if (plat_ms() - t0 > 10) return -1;
    *saved = src_ready(e);
    outl((u16)(e->io + ES_1371_SMPRATE), (*saved & R_KEEP) | 0x00010000UL);
    t0 = plat_ms();
    while ((inl((u16)(e->io + ES_1371_SMPRATE)) & 0x00870000UL) != 0 && plat_ms() - t0 < 2) ;
    t0 = plat_ms();
    while ((inl((u16)(e->io + ES_1371_SMPRATE)) & 0x00870000UL) != 0x00010000UL && plat_ms() - t0 < 2) ;
    return 0;
}

static void codec_restore(struct es *e, u32 saved)
{
    src_ready(e);
    outl((u16)(e->io + ES_1371_SMPRATE), saved);
}

static void es_cwr(void *ctx, int reg, u16 v)
{
    struct es *e = ctx;
    u32 saved;
    if (codec_safe(e, &saved)) return;
    outl((u16)(e->io + ES_1371_CODEC), ((u32)(reg & 0x7F) << 16) | v);
    codec_restore(e, saved);
}

static u16 es_crd(void *ctx, int reg)
{
    struct es *e = ctx;
    u32 saved, x, t0;
    if (codec_safe(e, &saved)) return 0xFFFF;
    x = ((u32)(reg & 0x7F) << 16) | K_CODEC_PIRD;
    if (e->pci.vendor == 0x1102) x |= K_EV1938_MAGIC;
    outl((u16)(e->io + ES_1371_CODEC), x);
    codec_restore(e, saved);
    t0 = plat_ms();
    while ((inl((u16)(e->io + ES_1371_CODEC)) & K_CODEC_WIP) && plat_ms() - t0 < 10) ;
    t0 = plat_ms();
    while (!((x = inl((u16)(e->io + ES_1371_CODEC))) & K_CODEC_RDY))
        if (plat_ms() - t0 > 10) return 0xFFFF;
    return (u16)x;
}

/* CT5880 rev. C / D / E (5880h rev. 2-4), CT5880 rev. A (1371h rev. 7) and ES1373 rev. 8: the
   codec wakes up only after the AC'97 reset bit (Linux's es1371_ac97_reset_hack list) */
static int ac97_reset_hack(struct pcidev *p)
{
    if (p->vendor != 0x1274) return 0;
    if (p->device == 0x5880) return p->rev >= 2 && p->rev <= 4;
    return p->device == 0x1371 && (p->rev == 7 || p->rev == 8);
}

static void src_init(struct es *e)
{
    int i;
    src_ready(e);
    outl((u16)(e->io + ES_1371_SMPRATE), R_SRC_DISABLE);
    for (i = 0; i < 0x80; i++) src_wr(e, i, 0);
    src_wr(e, SRC_DAC1 + SRC_TRUNC_N, 16 << 4);
    src_wr(e, SRC_DAC1 + SRC_INT_REGS, 16 << 10);
    src_wr(e, SRC_DAC2 + SRC_TRUNC_N, 16 << 4);
    src_wr(e, SRC_DAC2 + SRC_INT_REGS, 16 << 10);
    src_wr(e, SRC_VOL_ADC, 1 << 12);
    src_wr(e, SRC_VOL_ADC + 1, 1 << 12);
    src_wr(e, SRC_VOL_DAC1, 1 << 12);
    src_wr(e, SRC_VOL_DAC1 + 1, 1 << 12);
    src_wr(e, SRC_VOL_DAC2, 1 << 12);
    src_wr(e, SRC_VOL_DAC2 + 1, 1 << 12);
    adc_rate(e, 48000);
    dac2_rate(e, 48000);
    src_ready(e);
    outl((u16)(e->io + ES_1371_SMPRATE), 0);
}

/* ---------------------------------------------------------------- channels */

static int es_start(struct adev *a, int dir)
{
    struct es *e = a->priv;
    struct ahw *h = dir ? &a->in : &a->out;
    u32 frames = h->size / 4;
    if (dir) {
        e->ctrl &= ~C_ADC_EN;
        outl((u16)(e->io + ES_CONTROL), e->ctrl);
        es_page(e, ES_PAGE_ADC);
        outl((u16)(e->io + ES_FRAME), vphys(h->buf));
        outl((u16)(e->io + ES_SIZE), frames - 1);
        e->sctrl = (e->sctrl & ~(SE_R1_MODE(3) | (1UL << 15) | (1UL << 10))) | SE_R1_MODE(3);
        outl((u16)(e->io + ES_SERIAL), e->sctrl);
        outl((u16)(e->io + ES_ADC_COUNT), h->period / 4 - 1);
        e->ctrl |= C_ADC_EN;
    } else {
        e->ctrl &= ~C_DAC2_EN;
        outl((u16)(e->io + ES_CONTROL), e->ctrl);
        es_page(e, ES_PAGE_DAC);
        outl((u16)(e->io + ES_DAC2_FRAME), vphys(h->buf));
        outl((u16)(e->io + ES_DAC2_SIZE), frames - 1);
        /* loop mode, play, 16-bit stereo, no interrupt */
        e->sctrl &= ~(SE_P2_END_INC(7) | SE_P2_ST_INC(7) | SE_P2_MODE(3) | (1UL << 14) | (1UL << 12) |
                      (1UL << 9) | (1UL << 6));
        e->sctrl |= SE_P2_MODE(3) | SE_P2_END_INC(2) | SE_P2_ST_INC(0);
        outl((u16)(e->io + ES_SERIAL), e->sctrl);
        outl((u16)(e->io + ES_DAC2_COUNT), h->period / 4 - 1);
        e->ctrl |= C_DAC2_EN;
    }
    outl((u16)(e->io + ES_CONTROL), e->ctrl);
    return 0;
}

static void es_stop(struct adev *a, int dir)
{
    struct es *e = a->priv;
    e->ctrl &= ~(dir ? C_ADC_EN : C_DAC2_EN);
    outl((u16)(e->io + ES_CONTROL), e->ctrl);
}

/* the frame counter: dwords done in this turn of the ring */
static u32 es_pos(struct adev *a, int dir)
{
    struct es *e = a->priv;
    struct ahw *h = dir ? &a->in : &a->out;
    u32 v;
    es_page(e, dir ? ES_PAGE_ADC : ES_PAGE_DAC);
    v = inl((u16)(e->io + (dir ? ES_SIZE : ES_DAC2_SIZE)));
    v = (v >> 16) * 4;
    return v < h->size ? v : 0;
}

static void es_vol(struct adev *a, int ctl, u32 lr)
{
    struct es *e = a->priv;
    if (e->chip == CHIP_1371) {
        if (e->codec) ac97_ext_vol(e->codec, ctl, lr);
        return;
    }
    switch (ctl) {
    case MX_MASTER:
    {
        u32 l = lr & 0xFFFF, r = lr >> 16;
        ak_wr(e, AK_MASTER_L, (u8)(l ? ak_att(l) : 0x80));
        ak_wr(e, AK_MASTER_L + 1, (u8)(r ? ak_att(r) : 0x80));
        break;
    }
    case MX_CD:
        e->cd = lr != 0;
        ak_gain(e, AK_CD_L, lr);
        ak_omix(e);
        break;
    case MX_LINE:
        e->line = lr != 0;
        ak_gain(e, AK_LINE_L, lr);
        ak_omix(e);
        break;
    }
}

static void es_src(struct adev *a, int src)
{
    struct es *e = a->priv;
    u8 l1 = 0, r1 = 0, l2 = 0, r2 = 0;
    if (e->chip == CHIP_1371) {
        if (e->codec) ac97_ext_src(e->codec, src);
        return;
    }
    /* the input mixer of the AK4531 feeds the ADC */
    switch (src) {
    case AS_LINE: l1 = 0x10; r1 = 0x08; break;
    case AS_CD: l1 = 0x04; r1 = 0x02; break;
    case AS_AUX: l2 = 0x10; r2 = 0x08; break;
    default: l1 = r1 = 0x01; break;             /* microphone */
    }
    ak_wr(e, AK_LIN1, l1);
    ak_wr(e, AK_RIN1, r1);
    ak_wr(e, AK_LIN2, l2);
    ak_wr(e, AK_RIN2, r2);
    ak_wr(e, AK_MIC, (u8)(src == AS_MIC ? 0x06 : 0x86));
}

static void es_shutdown(struct adev *a)
{
    struct es *e = a->priv;
    e->ctrl &= ~(C_DAC2_EN | C_ADC_EN);
    outl((u16)(e->io + ES_CONTROL), e->ctrl);
}

static struct adevops es_ops = { es_start, es_stop, es_pos, es_vol, es_src, NULL, es_shutdown };

/* ---------------------------------------------------------------- probe */

static int chip_of(struct pcidev *p)
{
    if (p->vendor == 0x1274 && p->device == 0x5000) return CHIP_1370;
    if (p->vendor == 0x1274 && (p->device == 0x1371 || p->device == 0x5880)) return CHIP_1371;
    if (p->vendor == 0x1102 && p->device == 0x8938) return CHIP_1371;     /* Ectiva EV1938 */
    return -1;
}

static void alloc_ring(struct ahw *h, u8 *buf, u32 rate)
{
    memset(h, 0, sizeof(*h));
    h->buf = buf;
    h->period = PERIOD;
    h->size = RINGSIZE;
    h->rate = rate;
    h->present = 1;
    h->noflush = 1;                 /* PCI bus master, cache snooped */
}

int es137x_probe(struct pcidev *p)
{
    struct es *e;
    int chip = chip_of(p), i;
    const char *name;
    if (chip < 0) return -1;
    for (i = 0; i < nchips && PCI_BDF(&chips[i]->pci) != PCI_BDF(p); i++) ;
    if (i < nchips) {
        e = chips[i];
        e->a.gone = 0;
    } else {
        if (nchips >= MAXES) return -1;
        e = kmalloc(sizeof(*e));
        if (!e) return -1;
        memset(e, 0, sizeof(*e));
        e->ring[0] = kmalloc_align(RINGSIZE, 4096);
        e->ring[1] = kmalloc_align(RINGSIZE, 4096);
        e->phantom = kmalloc_align(64, 64);
        if (!e->ring[0] || !e->ring[1] || !e->phantom) return -1;
        chips[nchips++] = e;
    }
    e->pci = *p;
    e->chip = chip;
    pci_enable(p, 0, 1, 1);
    e->io = (u16)pci_bar_io(p, 0);
    if (!e->io) return -1;
    e->sctrl = 0;
    outl((u16)(e->io + ES_SERIAL), 0);
    if (chip == CHIP_1370) {
        /* DAC2 and ADC at 1411200 / (30 + 2) = 44100 Hz */
        e->ctrl = C_1370_CDC_EN | C_1370_SERR_DIS | C_1370_PCLKDIV(30);
        outl((u16)(e->io + ES_CONTROL), e->ctrl);
        memset(e->phantom, 0, 64);
        es_page(e, ES_PAGE_ADC);
        outl((u16)(e->io + ES_PHANTOM_FRAME), vphys(e->phantom));
        outl((u16)(e->io + ES_PHANTOM_COUNT), 0);
        ak_init(e);
        alloc_ring(&e->a.out, e->ring[0], 44100);
        alloc_ring(&e->a.in, e->ring[1], 44100);
        e->a.insrcs = AS_MIC | AS_LINE | AS_CD | AS_AUX;
        name = "Ensoniq AudioPCI ES1370";
        e->a.vid = p->vendor;              /* no AC'97 codec: the PCI IDs */
        e->a.pid = p->device;
    } else {
        e->ctrl = 0;
        outl((u16)(e->io + ES_CONTROL), e->ctrl);
        outl((u16)(e->io + ES_1371_LEGACY), 0);
        if (ac97_reset_hack(p)) {
            outl((u16)(e->io + ES_STATUS), S_1371_AC97_RST);
            mdelay(20);
        }
        /* AC'97 warm reset: the bit clock starts */
        outl((u16)(e->io + ES_CONTROL), e->ctrl | C_1371_SYNC_RES);
        inl((u16)(e->io + ES_CONTROL));
        udelay(20);
        outl((u16)(e->io + ES_CONTROL), e->ctrl);
        src_init(e);
        if (!e->codec) e->codec = ac97_ext(e, es_crd, es_cwr);
        if (!e->codec) {
            kprintf("es137x: %02x:%02x.%u no AC'97 codec\n", p->bus, p->dev, p->fn);
            return -1;
        }
        alloc_ring(&e->a.out, e->ring[0], 48000);
        alloc_ring(&e->a.in, e->ring[1], 48000);
        e->a.insrcs = AS_MIC | AS_LINE | AS_CD | AS_AUX | AS_MIX;
        /* ES1371 revisions: 4, 6, 8 ES1373, 7 CT5880 */
        name = p->vendor == 0x1102 ? "Ectiva EV1938" : (p->device == 0x5880 || p->rev == 7) ? "Creative CT5880" :
               (p->rev == 4 || p->rev == 6 || p->rev == 8) ? "Ensoniq AudioPCI ES1373" : "Ensoniq AudioPCI ES1371";
        {
            u32 id = ac97_ext_id(e->codec, NULL);
            e->a.vid = (u16)(id >> 16);
            e->a.pid = (u16)id;
        }
    }
    e->a.ops = &es_ops;
    e->a.priv = e;
    e->a.type = AT_ENS;
    e->a.caps = WINAC_OUT | WINAC_IN;
    e->a.insrc = AS_MIC;
    e->a.hwvol = 1;
    es_src(&e->a, AS_MIC);
    ksnprintf(e->a.name, sizeof(e->a.name), "%s", name);
    e->a.irq = p->irq;
    e->a.pcivid = p->vendor;
    e->a.pcidid = p->device;
    e->a.loc = (u16)((p->bus << 8) | (p->dev << 3) | p->fn);
    kprintf("es137x: %02x:%02x.%u %s, %lu Hz\n", p->bus, p->dev, p->fn, name, e->a.out.rate);
    adev_register(&e->a);
    return 0;
}

void es137x_remove(u32 bdf)
{
    int i;
    for (i = 0; i < nchips; i++)
        if (PCI_BDF(&chips[i]->pci) == bdf) aeng_remove(&chips[i]->a);
}
