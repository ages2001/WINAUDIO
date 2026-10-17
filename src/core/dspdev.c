/* dspdev.c - sound devices behind the Intel DSP:
   - the digital microphone array (NHLT PDM endpoint): a recording device
   - I2S codecs and amplifiers on SSP ports (dspcdc.c finds them in ACPI)
   - SoundWire codecs (sdw.c)
   - HD Audio codecs routed through the DSP (hda.c with /DSP)
   An endpoint is a host DMA stream over the ring of the sound engine (16-bit, 48 kHz) and a
   firmware path to the interface. Microphone arrays with more than two microphones are
   captured in their own ring and folded to stereo here. */
#include "dspcdc.h"

#define HOSTPERIOD  1024            /* bytes per BDL entry of a private ring */

/* ---------------------------------------------------------------- endpoints */

struct dspep *dsp_ep_new(struct dsp *d, int type, int dir, int port)
{
    struct dspep *e;
    if (d->nep >= MAXEP) return NULL;
    e = &d->ep[d->nep++];
    memset(e, 0, sizeof(*e));
    e->d = d;
    e->type = (u8)type;
    e->dir = (u8)dir;
    e->port = (u8)port;
    afmt_set(&e->hfmt, 48000, 16, 16, 2);
    e->lfmt = e->hfmt;
    return e;
}

/* private ring of a microphone array with more channels than the engine's stereo ring */
struct fold {
    u8 *buf;
    u32 size, done;                 /* bytes of the private ring; folded up to here */
    u32 out;                        /* bytes written to the engine ring (total) */
    u32 *bdl;
};
static struct fold folds[MAXEP];

static struct fold *fold_of(struct dspep *e)
{
    return &folds[e - e->d->ep];
}

/* SoundWire on ACE 2+: the HD Audio link DMA of each ALH stream (set up by sdw.c) */
static void sdw_link_dma(struct dspep *e, int run)
{
    int k;
    if (e->type != EP_SDW) return;
    for (k = 0; k < e->nalh && k < 4; k++)
        if (e->alhltag[k]) dsp_link_run(e->d, e->alhlidx[k], run);
}

/* stream format of the link DMA. HD Audio codecs: the interface format. ACE 2+ SSP / DMIC:
   the sample container (Linux generic_calc_stream_format: physical width); 16-bit
   microphones are packed two samples per 32-bit word (dmic_calc_stream_format) */
static u16 link_fmt(const struct dspep *e)
{
    struct afmt f = e->lfmt;
    int ch = AF_CH(&f);
    if (e->type == EP_HDA) return afmt_hda(&f);
    if (e->type == EP_DMIC && f.bits == 16 && ch >= 2) {
        ch /= 2;
        f.bits = 32;
    }
    f.cfg = (f.cfg & ~0xFFFFUL) | (u32)ch | (f.bits << 8);
    return afmt_hda(&f);
}

int dsp_ep_start(struct dspep *e, struct ahw *hw)
{
    struct dsp *d = e->d;
    int idx, ret, ch = AF_CH(&e->hfmt);
    struct fold *f = fold_of(e);
    u8 *ring = hw->buf;
    u32 size = hw->size, period = hw->period;
    if (d->shim) return sst_ep_start(e, hw);       /* no HD Audio streams: DSP DMA on a page table */
    e->hw = hw;
    idx = dsp_stream_get(d, e->dir);
    if (idx < 0) {
        kprintf("dsp: no free host DMA stream\n");
        return -1;
    }
    e->hidx = (u8)idx;
    e->htag = (u8)dsp_link_tag(d, idx);
    if (ch != 2) {
        /* fold N channels to two (mono: doubled): the DMA writes its own ring of N / 2
           times the size */
        size = hw->size / 2 * (u32)ch;
        period = HOSTPERIOD * (u32)ch / 2;
        if (!f->buf || f->size != size) {
            kfree(f->buf);
            f->buf = kmalloc_align(size, 4096);
            f->size = size;
        }
        if (!f->bdl) f->bdl = kmalloc_align(16 * 256, 128);
        if (!f->buf || !f->bdl) goto fail;
        f->done = f->out = 0;
        ring = f->buf;
    }
    if (!e->bdl) e->bdl = kmalloc_align(16 * 256, 128);
    if (!e->bdl || size / period > 256) goto fail;
    if (dsp_stream_prepare(d, idx, ch != 2 ? f->bdl : e->bdl, ring, size, period, afmt_hda(&e->hfmt))) goto fail;
    aeng_flush_dma(ch != 2 ? f->bdl : e->bdl, size / period * 16);
    if (e->type == EP_HDA || (d->pl->gen >= DG_LNL && (e->type == EP_SSP || e->type == EP_DMIC))) {
        int l = dsp_link_get(d, e->dir);
        if (l < 0) {
            kprintf("dsp: no free link DMA stream\n");
            goto fail;
        }
        e->lidx = (u8)l;
        e->ltag = (u8)dsp_link_tag(d, l);
        dsp_link_prepare(d, l, e->ltag, link_fmt(e));
        if (e->type == EP_SSP && !e->dir) dsp_link_ssp_out(d, e->ltag, 1);
    }
    if (d->ipc4) {
        dsp_stream_run(d, idx, 1);
        if (e->ltag) dsp_link_run(d, e->lidx, 1);
        sdw_link_dma(e, 1);
        ret = ipc4_ep_start(e);
    } else {
        ret = ipc3_ep_start(e);
        if (!ret) {
            dsp_stream_run(d, idx, 1);
            if (e->ltag) dsp_link_run(d, e->lidx, 1);
            sdw_link_dma(e, 1);
            ret = ipc3_trigger(e, 1);
        }
    }
    if (ret) {
        dsp_stream_run(d, idx, 0);
        if (e->ltag) dsp_link_run(d, e->lidx, 0);
        sdw_link_dma(e, 0);
        if (!d->ipc4) ipc3_ep_stop(e);
        goto fail;
    }
    e->running = 1;
    return 0;
fail:
    if (e->ltag) {
        if (e->type == EP_SSP && !e->dir) dsp_link_ssp_out(d, e->ltag, 0);
        dsp_link_put(d, e->lidx);
    }
    e->ltag = 0;
    dsp_stream_put(d, idx);
    e->htag = 0;
    return -1;
}

void dsp_ep_stop(struct dspep *e)
{
    struct dsp *d = e->d;
    if (!e->running) return;
    if (d->shim) {
        sst_ep_stop(e);
        return;
    }
    if (d->ipc4) {
        ipc4_ep_stop(e);
    } else {
        ipc3_trigger(e, 0);
        ipc3_ep_stop(e);
    }
    dsp_stream_run(d, e->hidx, 0);
    sdw_link_dma(e, 0);
    if (e->ltag) {
        dsp_link_run(d, e->lidx, 0);
        if (e->type == EP_SSP && !e->dir) dsp_link_ssp_out(d, e->ltag, 0);
        dsp_link_put(d, e->lidx);
        e->ltag = 0;
    }
    dsp_stream_put(d, e->hidx);
    e->running = 0;
}

/* position in the engine's ring; folds new microphone frames first */
u32 dsp_ep_pos(struct dspep *e)
{
    struct dsp *d = e->d;
    struct fold *f = fold_of(e);
    int ch = AF_CH(&e->hfmt), k;
    u32 pos;
    if (!e->running) return 0;
    if (d->shim) return sst_ep_pos(e);
    pos = dsp_stream_pos(d, e->hidx);
    if (ch == 2) return pos < e->hw->size ? pos : 0;
    if (pos >= f->size) pos = 0;
    pos -= pos % (u32)(2 * ch);
    if (!e->hw->noflush) aeng_flush_dma(f->buf, f->size);
    while (f->done != pos) {
        s16 *src = (s16 *)(f->buf + f->done);
        s16 *dst = (s16 *)(e->hw->buf + f->out % e->hw->size);
        s32 l = 0, r = 0;
        if (ch == 1) {
            dst[0] = dst[1] = src[0];
        } else {
            for (k = 0; k + 1 < ch; k += 2) {
                l += src[k];
                r += src[k + 1];
            }
            dst[0] = (s16)(l / (ch / 2));
            dst[1] = (s16)(r / (ch / 2));
        }
        f->done = (f->done + 2 * (u32)ch) % f->size;
        f->out += 4;
    }
    return f->out % e->hw->size;
}

/* ---------------------------------------------------------------- HD Audio codecs */

struct dspep *dspdev_hda_ep(struct dsp *d, int dir)
{
    int i;
    for (i = 0; i < d->nep; i++)
        if (d->ep[i].type == EP_HDA && d->ep[i].dir == dir) return &d->ep[i];
    return dsp_ep_new(d, EP_HDA, dir, 0);
}

int dsp_hda_start(struct dspep *e, struct ahw *hw, int *linktag)
{
    afmt_set(&e->hfmt, hw->rate, 16, 16, 2);
    e->lfmt = e->hfmt;
    if (dsp_ep_start(e, hw)) return -1;
    *linktag = e->ltag;
    return 0;
}

void dsp_hda_stop(struct dspep *e)
{
    dsp_ep_stop(e);
}

u32 dsp_hda_pos(struct dspep *e)
{
    return dsp_ep_pos(e);
}

/* ---------------------------------------------------------------- devices */

#define PERIOD      2048
#define NPERIOD     32
#define RINGSIZE    (PERIOD * 2 * NPERIOD)

struct dspadev {
    struct adev a;
    struct dsp *d;
    struct dspep *out, *in;
    struct dspep *hp, *spk;         /* headphone and speaker paths, switched by the jack */
    struct cdc *jack;
    u8 hpon;
    struct cdc *cdcs[6];
    int ncdc;
    u8 *ring[2];
};

static int dev_start(struct adev *a, int dir)
{
    struct dspadev *v = a->priv;
    struct dspep *e = dir ? v->in : v->out;
    int i;
    if (!e) return -1;
    for (i = 0; i < v->ncdc; i++) cdc_stream(v->cdcs[i], dir, 1);
    if (dsp_ep_start(e, dir ? &a->in : &a->out)) {
        for (i = 0; i < v->ncdc; i++) cdc_stream(v->cdcs[i], dir, 0);
        return -1;
    }
    return 0;
}

static void dev_stop(struct adev *a, int dir)
{
    struct dspadev *v = a->priv;
    struct dspep *e = dir ? v->in : v->out;
    int i;
    if (!e) return;
    for (i = 0; i < v->ncdc; i++) cdc_stream(v->cdcs[i], dir, 0);
    dsp_ep_stop(e);
}

static u32 dev_pos(struct adev *a, int dir)
{
    struct dspadev *v = a->priv;
    struct dspep *e = dir ? v->in : v->out;
    return e ? dsp_ep_pos(e) : 0;
}

static void dev_set_vol(struct adev *a, int ctl, u32 lr)
{
    struct dspadev *v = a->priv;
    int i;
    if (ctl == MX_MASTER && !a->hwvol) return;     /* the sound engine scales */
    for (i = 0; i < v->ncdc; i++) cdc_volume(v->cdcs[i], ctl, lr);
}

static void dev_set_src(struct adev *a, int src)
{
    struct dspadev *v = a->priv;
    int i;
    for (i = 0; i < v->ncdc; i++) cdc_source(v->cdcs[i], src);
}

static void dev_poll(struct adev *a)
{
    struct dspadev *v = a->priv;
    int i, hp;
    if (v->d->shim) dsp_poll_all();                 /* no HD Audio controller polls the SST DSP */
    for (i = 0; i < v->ncdc; i++) cdc_poll(v->cdcs[i]);
    if (!v->jack || !v->hp || !v->spk || v->hp == v->spk) return;
    hp = v->jack->jackstate & 1;
    if (hp == v->hpon) return;
    /* headphones plugged in or out: the playback moves to the other port */
    v->hpon = (u8)hp;
    if (v->out && v->out->running) {
        for (i = 0; i < v->ncdc; i++) cdc_stream(v->cdcs[i], 0, 0);
        dsp_ep_stop(v->out);
        v->out = hp ? v->hp : v->spk;
        for (i = 0; i < v->ncdc; i++) cdc_stream(v->cdcs[i], 0, 1);
        dsp_ep_start(v->out, &a->out);
    } else {
        v->out = hp ? v->hp : v->spk;
    }
}

void dsp_adev_jack(struct adev *a, struct dspep *hp, struct dspep *spk, struct cdc *jack)
{
    struct dspadev *v;
    if (!a) return;
    v = a->priv;
    v->hp = hp;
    v->spk = spk;
    v->jack = jack;
    v->hpon = (u8)(jack && (jack->jackstate & 1));
    if (hp && spk) v->out = v->hpon ? hp : spk;
}

static void dev_shutdown(struct adev *a)
{
    struct dspadev *v = a->priv;
    int i;
    for (i = 0; i < v->ncdc; i++) cdc_off(v->cdcs[i]);
}

static struct adevops dev_ops = { dev_start, dev_stop, dev_pos, dev_set_vol, dev_set_src, dev_poll, dev_shutdown };

static void alloc_ring(struct dsp *d, struct ahw *hw, u8 *buf)
{
    hw->buf = buf;
    hw->period = plat_mode == PM_DOS ? PERIOD * 2 : PERIOD;
    hw->size = hw->period * NPERIOD;
    hw->rate = 48000;
    hw->present = 1;
    hw->noflush = d->shim ? 0 : 1;  /* Intel HD Audio snoops (set up by hda.c); SST: written back */
}

/* a sound device of playback and / or capture endpoints and the codecs behind them */
struct adev *dsp_adev_new(struct dsp *d, const char *name, struct dspep *out, struct dspep *in,
                          struct cdc **cdcs, int ncdc)
{
    struct dspadev *v = kmalloc(sizeof(*v));
    int i;
    if (!v) return NULL;
    memset(v, 0, sizeof(*v));
    v->d = d;
    v->out = out;
    v->in = in;
    for (i = 0; i < ncdc && i < 6; i++) v->cdcs[v->ncdc++] = cdcs[i];
    if (out) {
        v->ring[0] = kmalloc_align(RINGSIZE, 4096);
        if (!v->ring[0]) return NULL;
        alloc_ring(d, &v->a.out, v->ring[0]);
        v->a.caps |= WINAC_OUT;
    }
    if (in) {
        v->ring[1] = kmalloc_align(RINGSIZE, 4096);
        if (!v->ring[1]) return NULL;
        alloc_ring(d, &v->a.in, v->ring[1]);
        v->a.caps |= WINAC_IN;
        v->a.insrcs = AS_MIC;
        v->a.insrc = AS_MIC;
    }
    v->a.ops = &dev_ops;
    v->a.priv = v;
    v->a.type = AT_HDA;
    v->a.vid = d->pci.vendor;
    v->a.pid = d->pci.device;
    /* hardware master volume only when every chip on the playback path has one (an
       amplifier without volume control would stay at full level) */
    v->a.hwvol = v->ncdc ? 1 : 0;
    for (i = 0; i < v->ncdc; i++)
        if (!cdc_has_volume(v->cdcs[i])) v->a.hwvol = 0;
    strlcpy(v->a.name, name, sizeof(v->a.name));
    adev_register(&v->a);
    if (d->nadev < (int)ARRAY_SIZE(d->adevs)) d->adevs[d->nadev++] = &v->a;
    kprintf("dsp: sound device \"%s\"%s%s\n", name, out ? ", playback" : "", in ? ", recording" : "");
    return &v->a;
}

/* the sound devices of a DSP for the Plug and Play enumerator, kind from their endpoints */
int dsp_functions(struct dsp *d, struct afunc *out, int max)
{
    int i, n = 0;
    for (i = 0; i < d->nadev && n < max; i++) {
        struct adev *a = d->adevs[i];
        struct dspadev *v = a->priv;
        struct dspep *e = v->out ? v->out : v->in;
        struct afunc *f = &out[n++];
        int t = v->spk ? v->spk->type : e ? e->type : 0;
        memset(f, 0, sizeof(*f));
        f->kind = (u8)(t == EP_SDW ? AF_DSP_SDW : t == EP_SSP ? AF_DSP_I2S : t == EP_DMIC ? AF_DSP_DMIC : AF_DSP_HDA);
        f->index = (u8)i;
        f->hasdev = (u8)!a->gone;
        f->vendev = ((u32)d->pci.vendor << 16) | d->pci.device;
        strlcpy(f->name, a->name, sizeof(f->name));
    }
    return n;
}

/* digital microphones (NHLT PDM endpoint) */
static int dmic_setup(struct dsp *d)
{
    struct dspep *e;
    struct afmt f;
    u32 len;
    const u8 *blob;
    int ch;
    if (!nhlt_best(NHLT_LINK_PDM, -1, 1, 0, &f)) {
        if (!(cb->opts2 & OPT2_DMIC)) return 0;
        /* no NHLT (Chromebook firmware): IPC3 firmware computes the PDM setup itself */
        if (d->ipc4) {
            kprintf("dsp: /DMIC needs the NHLT table with IPC4 firmware\n");
            return 0;
        }
        afmt_set(&f, 48000, 32, 24, 2);
    }
    e = dsp_ep_new(d, EP_DMIC, 1, 0);
    if (!e) return 0;
    ch = AF_CH(&f);
    e->lfmt = f;
    blob = nhlt_blob(NHLT_LINK_PDM, -1, 1, 0, ch, f.rate, (int)f.bits, AF_VBITS(&f), &len, NULL);
    e->blob = blob;
    e->bloblen = blob ? len : 0;
    if (!d->ipc4 && (ch & 1)) {
        /* IPC3 firmware: every PDM controller delivers two channels (mics A and B) and the
           stream must have the channel count of the controllers */
        ch++;
        afmt_set(&e->lfmt, f.rate, (int)f.bits, AF_VBITS(&f), ch);
    }
    /* the copier converts the sample size only: the host stream keeps the channel count
       (folded to stereo in dsp_ep_pos) */
    afmt_set(&e->hfmt, 48000, 16, 16, ch);
    kprintf("dsp: digital microphones: %d channel(s), %lu-bit\n", ch, f.bits);
    dsp_adev_new(d, "Intel SST Digital Microphones", NULL, e, NULL, 0);
    return 1;
}

/* does the machine need the DSP even with HD Audio codecs on the link? */
int dsp_wanted(void)
{
    int i;
    for (i = 0; i < nhlt_load(); i++) {
        const struct nhlt_ep *e = nhlt_get(i);
        if (e->link == NHLT_LINK_PDM || (e->link == NHLT_LINK_SSP && e->dev != NHLT_DEV_BT)) return 1;
    }
    return cdc_present() || sdw_present();
}

void dsp_remove(u32 bdf)
{
    struct dsp *d;
    int i;
    for (d = dsp_list; d; d = d->next)
        if (PCI_BDF(&d->pci) == bdf) {
            for (i = 0; i < d->nadev; i++) aeng_remove(d->adevs[i]);
            dsp_detach(d);
            return;
        }
}

int dspdev_setup(struct dsp *d, int with_codecs)
{
    int n = 0;
    (void)with_codecs;
    if (d->shim) return cdc_setup(d);              /* SST: I2S codecs only */
    nhlt_load();
    n += dmic_setup(d);
    n += cdc_setup(d);
    n += sdw_setup(d);
    return n;
}
