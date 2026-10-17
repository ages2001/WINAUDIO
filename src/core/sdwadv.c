/* sdwadv.c - the sound devices of the SoundWire peripherals, grouped as the Linux sof_sdw
   machine driver does:
   - "Intel SoundWire <codec>[+<amplifier>]": playback on the headphones of the jack codec
     (SF_JACK_OUT) or on the speakers (SF_SPK: the amplifier chips, aggregated over their
     links, else the codec's own amplifier), switched by the jack detection of the codec;
     recording from the headset microphone (SF_JACK_IN).
   - "Intel SoundWire Digital Microphones": the SF_DMIC function of a peripheral, a recording
     device of its own; when the machine has no headset microphone it is the recording side
     of the first device instead.
   Speaker amplifiers get their channel index (s->chan, s->namps) before their driver init:
   - amplifiers on several links: one ALH stream per link (aggregated gateways); an
     amplifier with a stereo port receives both channels on its link and selects its side
     itself (chan even: left, odd: right), an amplifier with a mono port receives its own
     channel (a link of mono amplifiers of one side carries only that channel);
   - several amplifiers on one link: one stereo stream, each mono amplifier on the block
     offset of its channel, stereo amplifiers on both. */
#include "sdwint.h"

#define PERIOD      2048
#define NPERIOD     32
#define RINGSIZE    (PERIOD * 2 * NPERIOD)
#define MAXSTRM     8
#define MAXAMP      8

static struct sdwstrm strms[MAXSTRM];
static int nstrm;

struct sdwadev {
    struct adev a;
    struct dsp *d;
    struct sdwstrm *out, *in;       /* current playback / recording stream */
    struct sdwstrm *hp, *spk;       /* headphones and speakers, switched by the jack */
    struct sdwdev *jack;            /* device with the jack detection */
    u8 hpon;
    u8 nvol, ncap, ndev;
    struct sdwdev *vol[MAXAMP + 2]; /* chips on the playback paths (MX_MASTER) */
    struct sdwdev *cap[2];          /* chip of the recording path (MX_INGAIN) */
    struct sdwdev *devs[MAXAMP + 4];
    u8 *ring[2];
};

/* ---------------------------------------------------------------- streams */

static struct sdwstrm *strm_new(struct dsp *d, int func, int dir)
{
    struct sdwstrm *st;
    if (nstrm >= MAXSTRM) return NULL;
    st = &strms[nstrm++];
    memset(st, 0, sizeof(*st));
    st->func = (u8)func;
    st->dir = (u8)dir;
    st->d = d;
    return st;
}

static void strm_drop(struct sdwstrm *st)
{
    int i;
    for (i = 0; i < st->npe; i++) st->pe[i]->used = 0;
    st->npe = 0;
    if (st == &strms[nstrm - 1]) nstrm--;
}

/* the DSP endpoint: 48 kHz, 24 bits in 32 on the gateway side; recording with other than
   two channels is folded to stereo by dspdev.c */
static int strm_ep(struct sdwstrm *st, int ch)
{
    struct dspep *e;
    if (!st->npe) return -1;
    e = dsp_ep_new(st->d, EP_SDW, st->dir, st->pe[0]->l->id);
    if (!e) {
        kprintf("sdw: no DSP endpoint left\n");
        return -1;
    }
    afmt_set(&e->lfmt, SDW_RATE, 32, SDW_BPS, ch);
    if (st->dir && ch != 2) afmt_set(&e->hfmt, SDW_RATE, 16, 16, ch);
    e->nalh = st->npe;
    st->ep = e;
    return 0;
}

/* a function of one peripheral */
static struct sdwstrm *strm_one(struct dsp *d, struct sdwdev *s, int func, int dir)
{
    const struct sdwport *p = sdw_dev_port(s, func);
    struct sdwstrm *st;
    struct sdwpe *pe;
    int ch;
    if (!p) {
        kprintf("sdw: %s: no data port for function %02x\n", s->drv->name, func);
        return NULL;
    }
    if ((st = strm_new(d, func, dir)) == NULL) return NULL;
    if ((pe = sdw_pe_new(s->link, st)) == NULL) {
        strm_drop(st);
        return NULL;
    }
    st->pe[0] = pe;
    st->npe = 1;
    ch = p->ch ? p->ch : 2;
    if (ch > 8) ch = 8;
    if (!dir && ch > 2) ch = 2;
    pe->nsp = 1;
    pe->sp[0].s = s;
    pe->sp[0].port = p->port;
    pe->sp[0].ch = (u8)ch;
    pe->sp[0].first = 0;
    pe->sp[0].flags = p->flags;
    pe->ch = (u8)(!dir && ch == 1 ? 2 : ch);     /* a mono playback port takes the left channel */
    pe->chmask = (1UL << pe->ch) - 1;
    if (strm_ep(st, pe->ch)) {
        strm_drop(st);
        return NULL;
    }
    return st;
}

/* the speaker amplifiers (sorted by link and device number) */
static struct sdwstrm *strm_amps(struct dsp *d, struct sdwdev **amps, int namp)
{
    struct sdwstrm *st = strm_new(d, SF_SPK, 0);
    struct sdwlink *ls[SDW_MAXLINK];
    int nl = 0, i, k;
    if (!st) return NULL;
    for (i = 0; i < namp; i++) {
        for (k = 0; k < nl && ls[k] != amps[i]->link; k++) ;
        if (k == nl && nl < 4) ls[nl++] = amps[i]->link;
    }
    if (nl > 1 && !d->ipc4) {
        kprintf("sdw: IPC3 firmware takes one ALH stream per endpoint: speakers on link %u only\n", ls[0]->id);
        nl = 1;
    }
    for (k = 0; k < nl; k++) {
        struct sdwpe *pe = sdw_pe_new(ls[k], st);
        u32 bits = 0;
        int two = 0;
        if (!pe) break;
        for (i = 0; i < namp; i++) {
            const struct sdwport *p = sdw_dev_port(amps[i], SF_SPK);
            if (amps[i]->link != ls[k] || !p) continue;
            if (p->ch != 1) two = 1;
            else bits |= 1UL << (amps[i]->chan & 1);
        }
        pe->ch = (u8)(nl == 1 || two || bits == 3 ? 2 : 1);
        pe->chmask = pe->ch == 2 ? 3 : bits;
        for (i = 0; i < namp && pe->nsp < SDW_MAXSP; i++) {
            const struct sdwport *p = sdw_dev_port(amps[i], SF_SPK);
            struct sdwsp *sp;
            if (amps[i]->link != ls[k] || !p) continue;
            sp = &pe->sp[pe->nsp++];
            sp->s = amps[i];
            sp->port = p->port;
            sp->flags = p->flags;
            if (p->ch != 1) {
                sp->ch = 2;
                sp->first = 0;
            } else {
                sp->ch = 1;
                sp->first = (u8)(pe->ch == 2 ? (amps[i]->chan & 1) : 0);
            }
        }
        st->pe[st->npe++] = pe;
        DBG("sdw: speakers on link %u: %u channel(s), mask %lx, %u amplifier(s)\n", ls[k]->id, pe->ch, pe->chmask,
            pe->nsp);
    }
    if (strm_ep(st, 2)) {
        strm_drop(st);
        return NULL;
    }
    return st;
}

/* ---------------------------------------------------------------- sound device */

static void jack_poll(struct sdwdev *s)
{
    int j;
    if (!s || !s->attached || !s->drv->jack || plat_ms() - s->jacktime < 300) return;
    s->jacktime = plat_ms();
    j = s->drv->jack(s);
    if (j < 0 || j == s->jackstate) return;
    s->jackstate = (u8)j;
    kprintf("%s: headphones %s%s\n", s->drv->name, (j & 1) ? "in" : "out", (j & 2) ? ", headset microphone" : "");
}

static int dev_start(struct adev *a, int dir)
{
    struct sdwadev *v = a->priv;
    struct sdwstrm *st = dir ? v->in : v->out;
    if (!st) return -1;
    return sdw_strm_start(st, dir ? &a->in : &a->out);
}

static void dev_stop(struct adev *a, int dir)
{
    struct sdwadev *v = a->priv;
    struct sdwstrm *st = dir ? v->in : v->out;
    if (st) sdw_strm_stop(st);
}

static u32 dev_pos(struct adev *a, int dir)
{
    struct sdwadev *v = a->priv;
    struct sdwstrm *st = dir ? v->in : v->out;
    return st && st->running ? dsp_ep_pos(st->ep) : 0;
}

static void dev_set_vol(struct adev *a, int ctl, u32 lr)
{
    struct sdwadev *v = a->priv;
    int i;
    if (ctl == MX_MASTER) {
        if (!a->hwvol) return;                          /* the sound engine scales */
        for (i = 0; i < v->nvol; i++)
            if (v->vol[i]->attached && v->vol[i]->drv->volume) v->vol[i]->drv->volume(v->vol[i], MX_MASTER, lr);
    } else if (ctl == MX_INGAIN) {
        for (i = 0; i < v->ncap; i++)
            if (v->cap[i]->attached && v->cap[i]->drv->volume) v->cap[i]->drv->volume(v->cap[i], MX_INGAIN, lr);
    }
}

static void dev_poll(struct adev *a)
{
    struct sdwadev *v = a->priv;
    int i, k, hp;
    for (i = 0; i < 2; i++) {
        struct sdwstrm *st = i ? v->in : v->out;
        if (!st) continue;
        for (k = 0; k < st->npe; k++) sdw_link_poll(st->pe[k]->l);
    }
    if (v->spk && v->out != v->spk)
        for (k = 0; k < v->spk->npe; k++) sdw_link_poll(v->spk->pe[k]->l);
    /* a peripheral that was reset lost its port setup: the stream starts again */
    for (i = 0; i < 2; i++) {
        struct sdwstrm *st = i ? v->in : v->out;
        if (!st || !sdw_strm_stale(st)) continue;
        kprintf("sdw: peripheral re-attached, %s restarts\n", i ? "recording" : "playback");
        sdw_strm_stop(st);
        sdw_strm_start(st, i ? &a->in : &a->out);
    }
    jack_poll(v->jack);
    if (!v->jack || !v->hp || !v->spk) return;
    hp = v->jack->jackstate & 1;
    if (hp == v->hpon) return;
    /* headphones plugged in or out: the playback moves to the other path */
    v->hpon = (u8)hp;
    if (v->out && v->out->running) {
        sdw_strm_stop(v->out);
        v->out = hp ? v->hp : v->spk;
        if (sdw_strm_start(v->out, &a->out)) kprintf("sdw: %s does not start\n", hp ? "headphones" : "speakers");
    } else {
        v->out = hp ? v->hp : v->spk;
    }
}

static void dev_shutdown(struct adev *a)
{
    struct sdwadev *v = a->priv;
    int i;
    if (v->out) sdw_strm_stop(v->out);
    if (v->in) sdw_strm_stop(v->in);
    for (i = 0; i < v->ndev; i++) {
        struct sdwdev *s = v->devs[i];
        if (!s->attached || (s->rsv & (SDF_OFF | SDF_FAILED))) continue;
        if (s->drv->off) s->drv->off(s);
        s->rsv |= SDF_OFF;
    }
    if (--sdwc.users <= 0) sdw_links_down();
}

static struct adevops sdw_ops = { dev_start, dev_stop, dev_pos, dev_set_vol, NULL, dev_poll, dev_shutdown, NULL };

static void add_dev(struct sdwadev *v, struct sdwdev *s)
{
    int i;
    if (!s) return;
    for (i = 0; i < v->ndev; i++)
        if (v->devs[i] == s) return;
    if (v->ndev < (u8)ARRAY_SIZE(v->devs)) v->devs[v->ndev++] = s;
}

static void add_strm_devs(struct sdwadev *v, struct sdwstrm *st, int vol)
{
    int i, k;
    if (!st) return;
    for (i = 0; i < st->npe; i++)
        for (k = 0; k < st->pe[i]->nsp; k++) {
            struct sdwdev *s = st->pe[i]->sp[k].s;
            int j;
            add_dev(v, s);
            if (vol) {
                for (j = 0; j < v->nvol && v->vol[j] != s; j++) ;
                if (j == v->nvol && v->nvol < (u8)ARRAY_SIZE(v->vol)) v->vol[v->nvol++] = s;
            } else if (v->ncap < (u8)ARRAY_SIZE(v->cap)) {
                v->cap[v->ncap++] = s;
            }
        }
}

static void hw_ring(struct ahw *hw, u8 *buf)
{
    hw->buf = buf;
    hw->period = plat_mode == PM_DOS ? PERIOD * 2 : PERIOD;
    hw->size = hw->period * NPERIOD;
    hw->rate = SDW_RATE;
    hw->present = 1;
    hw->noflush = 1;                /* Intel HD Audio snoops (set up by hda.c) */
}

/* a sound device of playback (headphones / speakers) and recording streams */
static int adev_new(struct dsp *d, const char *name, struct sdwstrm *hp, struct sdwstrm *spk, struct sdwstrm *in,
                    struct sdwdev *jack)
{
    struct sdwadev *v = kmalloc(sizeof(*v));
    int i;
    if (!v) return 0;
    memset(v, 0, sizeof(*v));
    v->d = d;
    v->hp = hp;
    v->spk = spk;
    v->in = in;
    v->jack = jack && jack->drv->jack ? jack : NULL;
    v->hpon = (u8)(v->jack && (v->jack->jackstate & 1));
    v->out = hp && spk ? (v->hpon ? hp : spk) : hp ? hp : spk;
    add_strm_devs(v, hp, 1);
    add_strm_devs(v, spk, 1);
    add_strm_devs(v, in, 0);
    add_dev(v, v->jack);
    if (v->out) {
        v->ring[0] = kmalloc_align(RINGSIZE, 4096);
        if (!v->ring[0]) return 0;
        hw_ring(&v->a.out, v->ring[0]);
        v->a.caps |= WINAC_OUT;
    }
    if (in) {
        v->ring[1] = kmalloc_align(RINGSIZE, 4096);
        if (!v->ring[1]) return 0;
        hw_ring(&v->a.in, v->ring[1]);
        v->a.caps |= WINAC_IN;
        v->a.insrcs = AS_MIC;
        v->a.insrc = AS_MIC;
    }
    v->a.ops = &sdw_ops;
    v->a.priv = v;
    v->a.type = AT_HDA;
    v->a.vid = d->pci.vendor;
    v->a.pid = d->pci.device;
    /* hardware master volume only when every chip on the playback paths has one */
    v->a.hwvol = v->nvol ? 1 : 0;
    for (i = 0; i < v->nvol; i++)
        if (!v->vol[i]->drv->volume) v->a.hwvol = 0;
    strlcpy(v->a.name, name, sizeof(v->a.name));
    adev_register(&v->a);
    if (d->nadev < (int)ARRAY_SIZE(d->adevs)) d->adevs[d->nadev++] = &v->a;
    kprintf("sdw: sound device \"%s\"%s%s%s%s\n", name, hp ? ", headphones" : "", spk ? ", speakers" : "",
            in ? ", recording" : "", v->a.hwvol ? ", hardware volume" : "");
    sdwc.users++;
    return 1;
}

/* ---------------------------------------------------------------- machine */

static int has(struct sdwdev *s, int f)
{
    return s && s->drv && s->attached && !(s->rsv & SDF_FAILED) && (s->drv->funcs & f);
}

int sdw_devices(struct dsp *d)
{
    struct sdwctl *c = &sdwc;
    struct sdwdev *list[SDW_MAXDEV], *amps[MAXAMP], *jack = NULL, *mic = NULL, *dmic = NULL, *spkc = NULL;
    struct sdwstrm *hp = NULL, *mics = NULL, *spk = NULL, *dm = NULL;
    int n = 0, namp = 0, i, k, ndev = 0;
    char name[48];
    nstrm = 0;
    /* the peripherals with a driver by link and device number */
    for (i = 0; i < SDW_MAXLINK * 16; i++)
        for (k = 0; k < c->ndev; k++)
            if (c->dev[k].linkid * 16 + c->dev[k].devnum == i && c->dev[k].drv && c->dev[k].attached)
                list[ndev++] = &c->dev[k];
    for (i = 0; i < ndev; i++) {
        struct sdwdev *s = list[i];
        if (has(s, SF_JACK_OUT) && !jack) jack = s;
        if (has(s, SF_SPK) && !has(s, SF_JACK_OUT) && namp < MAXAMP) amps[namp++] = s;
        if (has(s, SF_DMIC) && !dmic) dmic = s;
    }
    mic = has(jack, SF_JACK_IN) ? jack : NULL;
    for (i = 0; i < ndev && !mic; i++)
        if (has(list[i], SF_JACK_IN)) mic = list[i];
    if (!namp) {
        spkc = has(jack, SF_SPK) ? jack : NULL;
        for (i = 0; i < ndev && !spkc; i++)
            if (has(list[i], SF_SPK)) spkc = list[i];
    }
    for (i = 0; i < namp; i++) {
        amps[i]->chan = (u8)i;
        amps[i]->namps = (u8)namp;
    }
    if (spkc) {
        spkc->chan = 0;
        spkc->namps = 1;
    }
    /* driver init now that the channels are known */
    for (i = 0; i < ndev; i++) sdw_dev_init(list[i]);
    sdw_wait_resets();
    for (i = k = 0; i < namp; i++)
        if (!(amps[i]->rsv & SDF_FAILED)) amps[k++] = amps[i];
    if (k != namp) {
        namp = k;
        for (i = 0; i < namp; i++) {
            amps[i]->chan = (u8)i;
            amps[i]->namps = (u8)namp;
        }
    }
    /* a driver may have found a variant of its chip with other functions: classify again */
    jack = mic = dmic = spkc = NULL;
    for (i = 0; i < ndev; i++) {
        if (has(list[i], SF_JACK_OUT) && !jack) jack = list[i];
        if (has(list[i], SF_DMIC) && !dmic) dmic = list[i];
    }
    mic = has(jack, SF_JACK_IN) ? jack : NULL;
    for (i = 0; i < ndev && !mic; i++)
        if (has(list[i], SF_JACK_IN)) mic = list[i];
    if (!namp) {
        spkc = has(jack, SF_SPK) ? jack : NULL;
        for (i = 0; i < ndev && !spkc; i++)
            if (has(list[i], SF_SPK)) spkc = list[i];
    }
    /* the jack state */
    for (i = 0; i < ndev; i++) {
        struct sdwdev *s = list[i];
        if ((s->rsv & SDF_FAILED) || !s->attached) continue;
        if (s->drv->jack) {
            int j = s->drv->jack(s);
            s->jackstate = (u8)(j > 0 ? j : 0);
            s->jacktime = plat_ms();
        }
    }
    if (jack) hp = strm_one(d, jack, SF_JACK_OUT, 0);
    if (namp) spk = strm_amps(d, amps, namp);
    else if (spkc) spk = strm_one(d, spkc, SF_SPK, 0);
    if (mic) mics = strm_one(d, mic, SF_JACK_IN, 1);
    if (dmic) dm = strm_one(d, dmic, SF_DMIC, 1);
    if (hp || spk || mics) {
        const char *cn = jack ? jack->drv->name : mic ? mic->drv->name : NULL;
        const char *an = namp ? amps[0]->drv->name : spkc && spkc != jack ? spkc->drv->name : NULL;
        if (!cn && !an) cn = dmic ? dmic->drv->name : "codec";
        ksnprintf(name, sizeof(name), "Intel SoundWire %s%s%s", cn ? cn : "", cn && an ? "+" : "", an ? an : "");
        n += adev_new(d, name, hp, spk, mics ? mics : dm, jack ? jack : mic);
        if (!mics) dm = NULL;
    }
    if (dm) n += adev_new(d, "Intel SoundWire Digital Microphones", NULL, NULL, dm, NULL);
    return n;
}
