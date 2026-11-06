/* emu10k.c - Creative EMU10K1: Sound Blaster Live!, Live! Value, Live! 5.1, PCI512.

   Playback: voices 0 and 1 read one 16-bit stereo ring (stereo voice pair, left on effects
   bus 0, right on bus 1) at 48 kHz, round and round from the loop start to the loop end;
   the FX8010 DSP program sends the two buses to the AC'97 front channels (the codec's mixer
   adds its analog inputs, CD and line), to the headphone and rear outputs. Recording: the
   DSP passes the AC'97 input to the ADC capture channel, which writes a 16-bit stereo ring
   at 48 kHz. The voice memory is reached through the chip's page table: the rings sit in
   pages 1-16 of the voice address space, every other page maps a silent page.

   Not tried on a real card (QEMU has no EMU10K1); register use as in the Linux and FreeBSD
   drivers. The EMU10K2 (Audigy) has another register layout and is not driven. */
#include "audio.h"

#define MAXEMU      2
#define RINGSIZE    65536UL
#define PERIOD      4096UL
#define NPAGES      8192            /* page table entries */
#define RINGPAGE    1               /* first voice page of the playback ring */
#define NVOICE      64

/* I/O registers */
#define PTR         0x00
#define DATA        0x04
#define IPR         0x08
#define INTE        0x0C
#define HCFG        0x14
#define AC97DATA    0x1C
#define AC97ADDR    0x1E

/* HCFG */
#define HCFG_AUDIOENABLE        0x00000001UL
#define HCFG_MUTEBUTTONENABLE   0x00000002UL
#define HCFG_LOCKTANKCACHE      0x00000004UL
#define HCFG_LOCKSOUNDCACHE     0x00000008UL
#define HCFG_AUTOMUTE           0x00000010UL
#define HCFG_JOYENABLE          0x00000200UL

/* pointer registers (per voice where it applies) */
#define CPF         0x00
#define PTRX        0x01
#define CVCF        0x02
#define VTFT        0x03
#define Z2          0x04
#define Z1          0x05
#define PSST        0x06
#define DSL         0x07
#define CCCA        0x08
#define CCR         0x09
#define FXRT        0x0B
#define MAPA        0x0C
#define MAPB        0x0D
#define ENVVOL      0x10
#define ATKHLDV     0x11
#define DCYSUSV     0x12
#define LFOVAL1     0x13
#define ENVVAL      0x14
#define ATKHLDM     0x15
#define DCYSUSM     0x16
#define LFOVAL2     0x17
#define IP          0x18
#define IFATN       0x19
#define PEFE        0x1A
#define FMMOD       0x1B
#define TREMFRQ     0x1C
#define FM2FRQ2     0x1D
#define TEMPENV     0x1E
#define CD0         0x20
#define PTB         0x40
#define TCB         0x41
#define ADCCR       0x42
#define TCBS        0x44
#define MICBA       0x45
#define ADCBA       0x46
#define FXBA        0x47
#define MICBS       0x49
#define ADCBS       0x4A
#define FXBS        0x4B
#define EMU_DBG         0x52
#define CLIEL       0x58
#define CLIEH       0x59
#define SOLEL       0x5C
#define SOLEH       0x5D
#define ADCIDX      0x64
#define GPRBASE     0x100
#define CODEBASE    0x400

#define CPF_STEREO          0x00008000UL
#define CCCA_CURRADDR       0x00FFFFFFUL
#define MAP_PTI             0x00001FFFUL
#define ADCBS_64K           0x1F
#define ADCCR_ENABLE        0x00000018UL    /* left and right channel, 48 kHz */
#define DBG_SINGLE_STEP     0x00008000UL
#define PITCH_48000         0x4000UL        /* pitch target of 48 kHz */
#define IP_48000            0xE000UL        /* initial pitch of 48 kHz */
#define CCIS                28              /* cache invalidate size: 16-bit stereo */

/* FX8010 operands and instructions */
#define FXBUS(x)    (0x00 + (x))
#define EXTIN(x)    (0x10 + (x))
#define EXTOUT(x)   (0x20 + (x))
#define C_0         0x40
#define OP_ACC3     6

struct emu {
    struct adev a;
    struct pcidev pci;
    u16 io;
    u8 *ring[2];
    u32 *ptb;                   /* page table */
    u8 *silent;
    struct ac97 *codec;
    u32 sa, ea;                 /* loop start / end of the playback voices (frames) */
};

static struct emu *cards[MAXEMU];
static int ncards;

static void ptr_wr(struct emu *e, u32 reg, int ch, u32 v)
{
    u32 f = irq_save();
    outl((u16)(e->io + PTR), (reg << 16) | ((u32)ch & 0x3F));
    outl((u16)(e->io + DATA), v);
    irq_restore(f);
}

static u32 ptr_rd(struct emu *e, u32 reg, int ch)
{
    u32 f = irq_save(), v;
    outl((u16)(e->io + PTR), (reg << 16) | ((u32)ch & 0x3F));
    v = inl((u16)(e->io + DATA));
    irq_restore(f);
    return v;
}

static u16 emu_crd(void *ctx, int reg)
{
    struct emu *e = ctx;
    u32 f = irq_save();
    u16 v;
    outb((u16)(e->io + AC97ADDR), (u8)reg);
    v = inw((u16)(e->io + AC97DATA));
    irq_restore(f);
    return v;
}

static void emu_cwr(void *ctx, int reg, u16 v)
{
    struct emu *e = ctx;
    u32 f = irq_save();
    outb((u16)(e->io + AC97ADDR), (u8)reg);
    outw((u16)(e->io + AC97DATA), v);
    irq_restore(f);
}

/* ---------------------------------------------------------------- voices */

static void voice_init(struct emu *e, int ch)
{
    ptr_wr(e, DCYSUSV, ch, 0);
    ptr_wr(e, IP, ch, 0);
    ptr_wr(e, VTFT, ch, 0xFFFF);
    ptr_wr(e, CVCF, ch, 0xFFFF);
    ptr_wr(e, PTRX, ch, 0);
    ptr_wr(e, CPF, ch, 0);
    ptr_wr(e, CCR, ch, 0);
    ptr_wr(e, PSST, ch, 0);
    ptr_wr(e, DSL, ch, 0x10);
    ptr_wr(e, CCCA, ch, 0);
    ptr_wr(e, Z1, ch, 0);
    ptr_wr(e, Z2, ch, 0);
    ptr_wr(e, FXRT, ch, 0x32100000UL);
    ptr_wr(e, ATKHLDM, ch, 0);
    ptr_wr(e, DCYSUSM, ch, 0);
    ptr_wr(e, IFATN, ch, 0xFFFF);
    ptr_wr(e, PEFE, ch, 0);
    ptr_wr(e, FMMOD, ch, 0);
    ptr_wr(e, TREMFRQ, ch, 24);
    ptr_wr(e, FM2FRQ2, ch, 24);
    ptr_wr(e, TEMPENV, ch, 0);
    ptr_wr(e, LFOVAL2, ch, 0);
    ptr_wr(e, LFOVAL1, ch, 0);
    ptr_wr(e, ATKHLDV, ch, 0);
    ptr_wr(e, ENVVOL, ch, 0);
    ptr_wr(e, ENVVAL, ch, 0);
}

/* the stereo pair: voice 0 the left channel (send A to bus 0), voice 1 the right (send B
   to bus 1) */
static void pair_setup(struct emu *e)
{
    u32 silent = ((u32)vphys(e->silent) << 1) | MAP_PTI;
    int v;
    for (v = 0; v < 2; v++) {
        u32 sends = v ? 0x00FFUL : 0xFF00UL;            /* PTRX: send A amount << 8 | send B */
        ptr_wr(e, CPF, v, CPF_STEREO);
        ptr_wr(e, FXRT, v, 0x32100000UL);               /* sends A..D to buses 0..3 */
        ptr_wr(e, PTRX, v, sends);
        ptr_wr(e, DSL, v, e->ea);
        ptr_wr(e, PSST, v, e->sa);
        ptr_wr(e, CCCA, v, e->sa + CCIS);
        ptr_wr(e, Z1, v, 0);
        ptr_wr(e, Z2, v, 0);
        ptr_wr(e, MAPA, v, silent);
        ptr_wr(e, MAPB, v, silent);
        ptr_wr(e, CVCF, v, 0x0000FFFFUL);
        ptr_wr(e, VTFT, v, 0x0000FFFFUL);
        ptr_wr(e, ATKHLDM, v, 0);
        ptr_wr(e, DCYSUSM, v, 0x7F);
        ptr_wr(e, LFOVAL1, v, 0x8000);
        ptr_wr(e, LFOVAL2, v, 0x8000);
        ptr_wr(e, FMMOD, v, 0);
        ptr_wr(e, TREMFRQ, v, 0);
        ptr_wr(e, FM2FRQ2, v, 0);
        ptr_wr(e, ENVVAL, v, 0x8000);
        ptr_wr(e, ATKHLDV, v, 0x7F7F);
        ptr_wr(e, ENVVOL, v, 0x8000);
        ptr_wr(e, PEFE, v, 0x7F);                       /* filter amount, no pitch amount */
    }
}

static void pair_go(struct emu *e, int go)
{
    int v, i;
    for (v = 0; v < 2; v++) {
        if (go) {
            for (i = 0; i < 4; i++) ptr_wr(e, CD0 + i, v, 0);       /* silence in the cache */
            ptr_wr(e, CCR, v, 64UL << 16);                         /* read address 64 */
            ptr_wr(e, CCR, v, ((u32)CCIS << 25) | (64UL << 16));   /* cache invalidate size */
            ptr_wr(e, IFATN, v, 0xFF00);                            /* filter open, 0 dB */
            ptr_wr(e, VTFT, v, 0xFFFFFFFFUL);
            ptr_wr(e, CVCF, v, 0xFFFFFFFFUL);
            ptr_wr(e, DCYSUSV, v, 0x7F7F);
            ptr_wr(e, PTRX, v, (PITCH_48000 << 16) | (v ? 0x00FFUL : 0xFF00UL));
            ptr_wr(e, CPF, v, (PITCH_48000 << 16) | CPF_STEREO);
            ptr_wr(e, IP, v, IP_48000);
        } else {
            ptr_wr(e, PTRX, v, 0);
            ptr_wr(e, CPF, v, CPF_STEREO);
            ptr_wr(e, IFATN, v, 0xFFFF);
            ptr_wr(e, VTFT, v, 0x0000FFFFUL);
            ptr_wr(e, CVCF, v, 0x0000FFFFUL);
            ptr_wr(e, IP, v, 0);
            ptr_wr(e, DCYSUSV, v, 0);
        }
    }
}

/* ---------------------------------------------------------------- FX8010 */

static void dsp_op(struct emu *e, int *pc, u32 op, u32 r, u32 a, u32 x, u32 y)
{
    ptr_wr(e, CODEBASE + (u32)*pc * 2, 0, ((x & 0x3FF) << 10) | (y & 0x3FF));
    ptr_wr(e, CODEBASE + (u32)*pc * 2 + 1, 0, ((op & 0x0F) << 20) | ((r & 0x3FF) << 10) | (a & 0x3FF));
    (*pc)++;
}

static void dsp_load(struct emu *e)
{
    int pc = 0, i;
    ptr_wr(e, EMU_DBG, 0, DBG_SINGLE_STEP);                 /* stopped while it is written */
    for (i = 0; i < 256; i++) ptr_wr(e, GPRBASE + (u32)i, 0, 0);
    dsp_op(e, &pc, OP_ACC3, EXTOUT(0x00), C_0, FXBUS(0), C_0);     /* AC'97 front */
    dsp_op(e, &pc, OP_ACC3, EXTOUT(0x01), C_0, FXBUS(1), C_0);
    dsp_op(e, &pc, OP_ACC3, EXTOUT(0x06), C_0, FXBUS(0), C_0);     /* headphone */
    dsp_op(e, &pc, OP_ACC3, EXTOUT(0x07), C_0, FXBUS(1), C_0);
    dsp_op(e, &pc, OP_ACC3, EXTOUT(0x08), C_0, FXBUS(0), C_0);     /* rear */
    dsp_op(e, &pc, OP_ACC3, EXTOUT(0x09), C_0, FXBUS(1), C_0);
    dsp_op(e, &pc, OP_ACC3, EXTOUT(0x0A), C_0, EXTIN(0x00), C_0);  /* AC'97 input -> ADC capture */
    dsp_op(e, &pc, OP_ACC3, EXTOUT(0x0B), C_0, EXTIN(0x01), C_0);
    while (pc < 512) dsp_op(e, &pc, OP_ACC3, C_0, C_0, C_0, C_0);
    ptr_wr(e, EMU_DBG, 0, 0);                               /* run */
}

/* ---------------------------------------------------------------- channels */

static int emu_start(struct adev *a, int dir)
{
    struct emu *e = a->priv;
    if (dir) {
        ptr_wr(e, ADCCR, 0, 0);
        ptr_wr(e, ADCBA, 0, vphys(e->a.in.buf));
        ptr_wr(e, ADCBS, 0, ADCBS_64K);
        ptr_wr(e, ADCCR, 0, ADCCR_ENABLE);
        return 0;
    }
    pair_setup(e);
    pair_go(e, 1);
    return 0;
}

static void emu_stop(struct adev *a, int dir)
{
    struct emu *e = a->priv;
    if (dir) {
        ptr_wr(e, ADCCR, 0, 0);
        ptr_wr(e, ADCBS, 0, 0);
        return;
    }
    pair_go(e, 0);
}

static u32 emu_pos(struct adev *a, int dir)
{
    struct emu *e = a->priv;
    u32 p;
    if (dir) {
        p = ptr_rd(e, ADCIDX, 0) & 0xFFFF;
        return p < a->in.size ? p & ~3UL : 0;
    }
    /* the current address runs CCIS frames ahead of the sound: the cache */
    p = ptr_rd(e, CCCA, 0) & CCCA_CURRADDR;
    if (p < e->sa + CCIS || p >= e->ea + CCIS) return 0;
    p = (p - e->sa - CCIS) * 4;
    return p < a->out.size ? p : 0;
}

static void emu_vol(struct adev *a, int ctl, u32 lr)
{
    struct emu *e = a->priv;
    if (e->codec) ac97_ext_vol(e->codec, ctl, lr);
}

static void emu_src(struct adev *a, int src)
{
    struct emu *e = a->priv;
    if (e->codec) ac97_ext_src(e->codec, src);
}

static void emu_shutdown(struct adev *a)
{
    struct emu *e = a->priv;
    pair_go(e, 0);
    ptr_wr(e, ADCCR, 0, 0);
    outl((u16)(e->io + HCFG), HCFG_LOCKSOUNDCACHE | HCFG_LOCKTANKCACHE | HCFG_MUTEBUTTONENABLE);
}

static struct adevops emu_ops = { emu_start, emu_stop, emu_pos, emu_vol, emu_src, NULL, emu_shutdown };

/* ---------------------------------------------------------------- probe */

static void alloc_ring(struct ahw *h, u8 *buf)
{
    memset(h, 0, sizeof(*h));
    h->buf = buf;
    h->period = PERIOD;
    h->size = RINGSIZE;
    h->rate = 48000;
    h->present = 1;
    h->noflush = 1;                 /* PCI bus master, cache snooped */
}

int emu10k_probe(struct pcidev *p)
{
    struct emu *e;
    u32 silent, i;
    int k;
    if (p->vendor != 0x1102 || p->device != 0x0002) return -1;
    for (k = 0; k < ncards && PCI_BDF(&cards[k]->pci) != PCI_BDF(p); k++) ;
    if (k < ncards) {
        e = cards[k];
        e->a.gone = 0;
    } else {
        if (ncards >= MAXEMU) return -1;
        e = kmalloc(sizeof(*e));
        if (!e) return -1;
        memset(e, 0, sizeof(*e));
        e->ring[0] = kmalloc_align(RINGSIZE, 4096);
        e->ring[1] = kmalloc_align(RINGSIZE, 4096);
        e->ptb = kmalloc_align(NPAGES * 4, 4096);
        e->silent = kmalloc_align(4096, 4096);
        if (!e->ring[0] || !e->ring[1] || !e->ptb || !e->silent) return -1;
        cards[ncards++] = e;
    }
    e->pci = *p;
    pci_enable(p, 0, 1, 1);
    e->io = (u16)pci_bar_io(p, 0);
    if (!e->io) return -1;
    outl((u16)(e->io + INTE), 0);
    outl((u16)(e->io + HCFG), HCFG_LOCKSOUNDCACHE | HCFG_LOCKTANKCACHE | HCFG_MUTEBUTTONENABLE);
    /* no recording buffers, no channel interrupts, no stop at the loop end */
    ptr_wr(e, MICBS, 0, 0);
    ptr_wr(e, MICBA, 0, 0);
    ptr_wr(e, FXBS, 0, 0);
    ptr_wr(e, FXBA, 0, 0);
    ptr_wr(e, ADCBS, 0, 0);
    ptr_wr(e, ADCBA, 0, 0);
    ptr_wr(e, ADCCR, 0, 0);
    ptr_wr(e, CLIEL, 0, 0);
    ptr_wr(e, CLIEH, 0, 0);
    ptr_wr(e, SOLEL, 0, 0);
    ptr_wr(e, SOLEH, 0, 0);
    for (k = 0; k < NVOICE; k++) voice_init(e, k);
    /* the page table: the playback ring in pages RINGPAGE.., the rest silent */
    memset(e->silent, 0, 4096);
    memset(e->ring[0], 0, RINGSIZE);
    silent = (u32)vphys(e->silent) << 1;
    for (i = 0; i < NPAGES; i++) e->ptb[i] = silent | i;
    for (i = 0; i < RINGSIZE / 4096; i++)
        e->ptb[RINGPAGE + i] = ((u32)vphys(e->ring[0] + i * 4096) << 1) | (RINGPAGE + i);
    ptr_wr(e, PTB, 0, vphys(e->ptb));
    ptr_wr(e, TCB, 0, 0);
    ptr_wr(e, TCBS, 0, 4);
    for (k = 0; k < NVOICE; k++) {
        ptr_wr(e, MAPA, k, (silent) | MAP_PTI);
        ptr_wr(e, MAPB, k, (silent) | MAP_PTI);
    }
    e->sa = RINGPAGE * 4096UL / 4;
    e->ea = e->sa + RINGSIZE / 4;
    dsp_load(e);
    outl((u16)(e->io + HCFG), HCFG_LOCKTANKCACHE | HCFG_AUTOMUTE | HCFG_JOYENABLE | HCFG_AUDIOENABLE);
    if (!e->codec) e->codec = ac97_ext(e, emu_crd, emu_cwr);
    if (!e->codec) {
        kprintf("emu10k: %02x:%02x.%u no AC'97 codec\n", p->bus, p->dev, p->fn);
        return -1;
    }
    {
        u32 id = ac97_ext_id(e->codec, NULL);
        e->a.vid = (u16)(id >> 16);
        e->a.pid = (u16)id;
    }
    alloc_ring(&e->a.out, e->ring[0]);
    alloc_ring(&e->a.in, e->ring[1]);
    e->a.ops = &emu_ops;
    e->a.priv = e;
    e->a.type = AT_EMU;
    e->a.caps = WINAC_OUT | WINAC_IN;
    e->a.insrcs = AS_MIC | AS_LINE | AS_CD | AS_AUX | AS_MIX;
    e->a.insrc = AS_MIC;
    e->a.hwvol = 1;
    emu_src(&e->a, AS_MIC);
    ksnprintf(e->a.name, sizeof(e->a.name), "Sound Blaster Live! (EMU10K1)");
    e->a.irq = p->irq;
    e->a.pcivid = p->vendor;
    e->a.pcidid = p->device;
    e->a.loc = (u16)((p->bus << 8) | (p->dev << 3) | p->fn);
    kprintf("emu10k: %02x:%02x.%u EMU10K1, codec %04x%04x\n", p->bus, p->dev, p->fn, e->a.vid, e->a.pid);
    adev_register(&e->a);
    return 0;
}

void emu10k_remove(u32 bdf)
{
    int i;
    for (i = 0; i < ncards; i++)
        if (PCI_BDF(&cards[i]->pci) == bdf) aeng_remove(&cards[i]->a);
}
