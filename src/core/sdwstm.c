/* sdwstm.c - SoundWire streams: the data path of one function (headphones, speakers,
   headset microphone, digital microphones) from the DSP ALH gateways over one or more links
   to the data ports of the peripherals.

   As the Linux stream code does it: the transport parameters of every prepared or running
   stream part of a link are computed together (generic bandwidth allocation: 48 kHz, 24-bit
   samples, one lane, the bus clock fixed: horizontal window at the right of the frame,
   block offsets one after the other, block packing per port), written to the register bank
   not in use of the manager and of the peripherals, and a bank switch (SSP-synchronized
   over all links of the stream) makes them current.
   start: PDIs, shim / ALH (or HD Audio link DMA on ACE 2+), ports, bank switch, channel
          prepare, function power-up (driver), DSP pipeline, channels on (bank switch)
   stop:  channels off (bank switch), function power-down, DSP pipeline, deprepare and
          the remaining streams recomputed (bank switch), PDIs freed */
#include "sdwint.h"

#define DPN(n)              (0x100UL * (n))
#define DPN_PORTCTRL        0x02
#define DPN_BLOCKCTRL1      0x03
#define DPN_PREPARESTATUS   0x04
#define DPN_PREPARECTRL     0x05
#define DPN_CHANNELEN       0x20
#define DPN_SAMPLECTRL1     0x22
#define DPN_SAMPLECTRL2     0x23
#define DPN_OFFSETCTRL1     0x24
#define DPN_OFFSETCTRL2     0x25
#define DPN_HCTRL           0x26
#define DPN_BLOCKCTRL3      0x27
#define BANK1               0x10

struct sdwpe *sdw_pe_new(struct sdwlink *l, struct sdwstrm *st)
{
    int i;
    for (i = 0; i < SDW_MAXPE; i++)
        if (!l->pe[i].used) {
            struct sdwpe *pe = &l->pe[i];
            memset(pe, 0, sizeof(*pe));
            pe->used = 1;
            pe->l = l;
            pe->st = st;
            return pe;
        }
    return NULL;
}

/* bandwidth allocation of a link: the prepared and enabled stream parts share the columns
   at the right end of the frame (column 0 carries the control word) */
static int compute(struct sdwlink *l)
{
    u32 full = (u32)l->rows * l->cols, payload = 0, hw;
    u16 bo = 1;
    int i;
    for (i = 0; i < SDW_MAXPE; i++)
        if (l->pe[i].used && l->pe[i].state != PE_IDLE) payload += (u32)SDW_BPS * l->pe[i].ch;
    if (!payload) return 0;
    hw = (l->cols * payload + full - 1) / full;
    if (hw > (u32)l->cols - 1) {
        kprintf("sdw: link %u: %lu bits per frame needed, a %ux%u frame has %lu\n", l->id, payload, l->rows, l->cols,
                (u32)l->rows * (l->cols - 1));
        return -1;
    }
    for (i = 0; i < SDW_MAXPE; i++) {
        struct sdwpe *pe = &l->pe[i];
        if (!pe->used || pe->state == PE_IDLE) continue;
        pe->hstop = (u8)(l->cols - 1);
        pe->hstart = (u8)(l->cols - hw);
        pe->bo = bo;
        bo = (u16)(bo + SDW_BPS * pe->ch);
    }
    return 0;
}

/* a peripheral data port in bank 'bank' (the registers of the simplified and reduced port
   types as Linux writes them) */
static void sp_program(struct sdwlink *l, struct sdwpe *pe, struct sdwsp *sp, int bank)
{
    struct sdwdev *s = sp->s;
    u32 n = DPN(sp->port), b = bank ? BANK1 : 0, si = (u32)l->rows * l->cols - 1;
    u16 bo = (u16)(pe->bo + SDW_BPS * sp->first);
    int type = sp->flags & SPF_TYPEMASK;
    if (!s->attached) return;
    sdw_upd(s, n + DPN_PORTCTRL, 0x0F, 0);                      /* isochronous, normal data */
    sdw_wr(s, n + DPN_BLOCKCTRL1, SDW_BPS - 1);
    sdw_wr(s, n + b + DPN_SAMPLECTRL1, (u8)si);
    sdw_wr(s, n + b + DPN_OFFSETCTRL1, (u8)bo);
    if (type != SPF_SIMPLE) {
        sdw_wr(s, n + b + DPN_OFFSETCTRL2, (u8)(bo >> 8));
        sdw_wr(s, n + b + DPN_BLOCKCTRL3, 0);                   /* block packing per port */
        if (type == SPF_FULL) {
            sdw_wr(s, n + b + DPN_SAMPLECTRL2, (u8)(si >> 8));
            sdw_wr(s, n + b + DPN_HCTRL, (u8)((pe->hstart << 4) | pe->hstop));
        }
    }
    sdw_wr(s, n + b + DPN_CHANNELEN, (u8)(pe->state == PE_ON ? (1U << sp->ch) - 1 : 0));
}

/* every prepared / enabled stream part of a link into the next bank */
static void program(struct sdwlink *l)
{
    int i, k, bank = l->bank ^ 1;
    lnk_clk_bank(l, bank);
    for (i = 0; i < SDW_MAXPE; i++) {
        struct sdwpe *pe = &l->pe[i];
        if (!pe->used || pe->state == PE_IDLE || !pe->pdi) continue;
        for (k = 0; k < pe->nsp; k++) sp_program(l, pe, &pe->sp[k], bank);
        lnk_dpn(l, pe, bank);
    }
}

/* channel prepare of the ports with the full prepare state machine */
static void sp_prepare(struct sdwsp *sp, int on)
{
    struct sdwdev *s = sp->s;
    u8 mask = (u8)((1U << sp->ch) - 1);
    u32 t0;
    int v;
    if (!(sp->flags & SPF_PREPARE) || !s->attached) return;
    if (sdw_wr(s, DPN(sp->port) + DPN_PREPARECTRL, on ? mask : 0) || !on) return;
    t0 = plat_ms();
    for (;;) {
        v = sdw_rd(s, DPN(sp->port) + DPN_PREPARESTATUS);
        if (v < 0 || !(v & mask)) break;
        if (plat_ms() - t0 > 100) {
            kprintf("sdw: %s: port %u not prepared (%02x)\n", s->drv ? s->drv->name : "device", sp->port, v);
            break;
        }
        mdelay(1);
    }
}

/* the driver's power-up / power-down of the function, once per device */
static void stream_cb(struct sdwstrm *st, int on)
{
    struct sdwdev *done[8];
    int nd = 0, i, k, j;
    for (i = 0; i < st->npe; i++)
        for (k = 0; k < st->pe[i]->nsp; k++) {
            struct sdwdev *s = st->pe[i]->sp[k].s;
            for (j = 0; j < nd && done[j] != s; j++) ;
            if (j < nd) continue;
            if (nd < 8) done[nd++] = s;
            if (s->attached && s->drv && s->drv->stream) s->drv->stream(s, st->func, on);
        }
}

static int links_of(struct sdwstrm *st, struct sdwlink **ls)
{
    int i;
    for (i = 0; i < st->npe; i++) ls[i] = st->pe[i]->l;
    return st->npe;
}

/* ports deprepared, the rest of the links recomputed, link side released (PDIs, ACE2
   link DMA) of the first 'n' parts */
static void release(struct sdwstrm *st, int n, int ports)
{
    struct sdwlink *ls[SDW_MAXLINK];
    struct dspep *e = st->ep;
    int i, k;
    if (ports) {
        for (i = 0; i < n; i++) {
            for (k = 0; k < st->pe[i]->nsp; k++) sp_prepare(&st->pe[i]->sp[k], 0);
            st->pe[i]->state = PE_IDLE;
        }
        for (i = 0; i < n; i++) {
            compute(st->pe[i]->l);
            program(st->pe[i]->l);
        }
        links_of(st, ls);
        lnk_bank_switch(ls, n);
    }
    for (i = 0; i < n; i++) {
        struct sdwpe *pe = st->pe[i];
        struct sdwlink *l = pe->l;
        pe->state = PE_IDLE;
        if (e && e->alhltag[i]) {
            lnk_ace2_map(l, pe->pdi, 0, 0, st->dir);
            if (!st->dir) lnk_ace2_losidv(e->alhltag[i], 0);
            dsp_link_put(st->d, e->alhlidx[i]);
            e->alhltag[i] = 0;
        }
        lnk_pdi_put(l, pe->pdi);
        pe->pdi = 0;
    }
}

int sdw_strm_start(struct sdwstrm *st, struct ahw *hw)
{
    struct sdwlink *ls[SDW_MAXLINK];
    struct dspep *e = st->ep;
    struct dsp *d = st->d;
    int i, k, n;
    if (st->running) return 0;
    if (!e || !st->npe) return -1;
    /* PDI of every part: shim channel map and ALH stream, or link DMA on ACE 2+ */
    for (i = 0; i < st->npe; i++) {
        struct sdwpe *pe = st->pe[i];
        struct sdwlink *l = pe->l;
        int p = l->up ? lnk_pdi_get(l, pe->ch) : -1;
        e->alhltag[i] = 0;
        if (p < 0) {
            kprintf("sdw: link %u: %s\n", l->id, l->up ? "no free PCM data interface" : "not running");
            release(st, i, 0);
            return -1;
        }
        pe->pdi = (u8)p;
        lnk_pdi_setup(l, pe);
        e->alhstream[i] = lnk_alh_id(l, p);
        e->alhdev[i] = ((u32)l->id << 8) | (u32)p;
        e->alhmask[i] = pe->chmask;
        if (l->lt == LT_ACE2) {
            struct afmt f;
            int idx = dsp_link_get(d, st->dir), tag;
            if (idx < 0) {
                kprintf("sdw: no free HD Audio link DMA stream\n");
                lnk_pdi_put(l, p);
                pe->pdi = 0;
                release(st, i, 0);
                return -1;
            }
            tag = dsp_link_tag(d, idx);
            e->alhlidx[i] = (u8)idx;
            e->alhltag[i] = (u8)tag;
            afmt_set(&f, SDW_RATE, 32, SDW_BPS, pe->ch);
            dsp_link_prepare(d, idx, tag, afmt_hda(&f));
            lnk_ace2_map(l, p, pe->ch, tag, st->dir);
            if (!st->dir) lnk_ace2_losidv(tag, 1);
        }
        DBG("sdw: link %u PDI %d: %u channel(s), ALH stream %lu, mask %lx\n", l->id, p, pe->ch, e->alhstream[i],
            pe->chmask);
    }
    e->nalh = st->npe;
    n = links_of(st, ls);
    for (i = 0; i < st->npe; i++)
        for (k = 0; k < st->pe[i]->nsp; k++) st->pe[i]->sp[k].att = sdwc.attcnt[st->pe[i]->sp[k].s - sdwc.dev];
    /* ports configured in the next bank, bank switch, channel prepare */
    for (i = 0; i < st->npe; i++) st->pe[i]->state = PE_PREP;
    for (i = 0; i < st->npe; i++)
        if (compute(st->pe[i]->l)) {
            for (k = 0; k < st->npe; k++) st->pe[k]->state = PE_IDLE;
            release(st, st->npe, 0);
            return -1;
        }
    for (i = 0; i < st->npe; i++) program(st->pe[i]->l);
    if (lnk_bank_switch(ls, n)) kprintf("sdw: stream setup bank switch failed\n");
    for (i = 0; i < st->npe; i++)
        for (k = 0; k < st->pe[i]->nsp; k++) sp_prepare(&st->pe[i]->sp[k], 1);
    /* the function powered up, the DSP pipeline, then the channels */
    stream_cb(st, 1);
    if (dsp_ep_start(e, hw)) {
        stream_cb(st, 0);
        release(st, st->npe, 1);
        return -1;
    }
    for (i = 0; i < st->npe; i++) st->pe[i]->state = PE_ON;
    for (i = 0; i < st->npe; i++) program(st->pe[i]->l);
    if (lnk_bank_switch(ls, n)) kprintf("sdw: stream enable bank switch failed\n");
    st->running = 1;
    return 0;
}

void sdw_strm_stop(struct sdwstrm *st)
{
    struct sdwlink *ls[SDW_MAXLINK];
    int i, k, n;
    if (!st->running) return;
    n = links_of(st, ls);
    /* channels off; the other bank too */
    for (i = 0; i < st->npe; i++) st->pe[i]->state = PE_PREP;
    for (i = 0; i < st->npe; i++) program(st->pe[i]->l);
    lnk_bank_switch(ls, n);
    for (i = 0; i < st->npe; i++) {
        struct sdwpe *pe = st->pe[i];
        int bank = pe->l->bank ^ 1;
        lnk_dpn_off(pe->l, pe, bank);
        for (k = 0; k < pe->nsp; k++)
            if (pe->sp[k].s->attached)
                sdw_wr(pe->sp[k].s, DPN(pe->sp[k].port) + (bank ? BANK1 : 0) + DPN_CHANNELEN, 0);
    }
    stream_cb(st, 0);
    dsp_ep_stop(st->ep);
    release(st, st->npe, 1);
    st->running = 0;
}

int sdw_strm_stale(struct sdwstrm *st)
{
    int i, k;
    if (!st->running) return 0;
    for (i = 0; i < st->npe; i++)
        for (k = 0; k < st->pe[i]->nsp; k++) {
            struct sdwsp *sp = &st->pe[i]->sp[k];
            if (sp->s->attached && sp->att != sdwc.attcnt[sp->s - sdwc.dev]) return 1;
        }
    return 0;
}
