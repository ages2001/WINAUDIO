/* dspcdc.c - I2S codecs and amplifiers of the DSP machines: found in ACPI by _HID, checked
   over I2C, put on SSP ports (the NHLT endpoints tell which; else the usual ports of the
   reference designs) and joined to one sound device: speakers from the amplifier or the
   codec, headphones and the headset microphone from the headset codec, switched by its jack
   detection. */
#include "dspcdc.h"

extern const struct cdcdrv cdc_rt5682, cdc_rt5682s, cdc_rt5640, cdc_rt5645, cdc_rt5651, cdc_rt5663;
extern const struct cdcdrv cdc_rt1011, cdc_rt1015, cdc_rt1015p, cdc_rt1019, cdc_rt1308i2s;
extern const struct cdcdrv cdc_max98357a, cdc_max98360a, cdc_max98373, cdc_max98390, cdc_max98927;
extern const struct cdcdrv cdc_da7219, cdc_da7213;
extern const struct cdcdrv cdc_es8316, cdc_es8326, cdc_es8336;
extern const struct cdcdrv cdc_nau8825, cdc_nau8824, cdc_nau8821, cdc_nau8318, cdc_nau8315;
extern const struct cdcdrv cdc_cs42l42, cdc_ssm4567, cdc_cx2072x, cdc_max98090, cdc_tas2563;
extern const struct cdcdrv cdc_rt286;

static const struct cdcdrv *const drivers[] = {
    &cdc_rt5682, &cdc_rt5682s, &cdc_rt5640, &cdc_rt5645, &cdc_rt5651, &cdc_rt5663,
    &cdc_rt1011, &cdc_rt1015, &cdc_rt1015p, &cdc_rt1019, &cdc_rt1308i2s,
    &cdc_max98357a, &cdc_max98360a, &cdc_max98373, &cdc_max98390, &cdc_max98927,
    &cdc_da7219, &cdc_da7213,
    &cdc_es8316, &cdc_es8326, &cdc_es8336,
    &cdc_nau8825, &cdc_nau8824, &cdc_nau8821, &cdc_nau8318, &cdc_nau8315,
    &cdc_cs42l42, &cdc_ssm4567, &cdc_cx2072x, &cdc_max98090, &cdc_tas2563,
    &cdc_rt286,
    NULL
};

#define MAXCDC 6
static struct acpi_dev adevs[MAXCDC * 2];
static int nadevs = -1;
static struct cdc cdcs[MAXCDC];
static int ncdc;

/* the next driver for an ACPI id from index *at on (several drivers may claim one id) */
static const struct cdcdrv *drv_of(const char *hid, int *at)
{
    int i, k;
    for (i = *at; drivers[i]; i++)
        for (k = 0; k < 6 && drivers[i]->hid[k]; k++)
            if (!strncmp(drivers[i]->hid[k], hid, 9)) {
                *at = i + 1;
                return drivers[i];
            }
    *at = i;
    return NULL;
}

static int find_acpi(void)
{
    static const char *hids[64];
    int i, k, n = 0;
    if (nadevs >= 0) return nadevs;
    for (i = 0; drivers[i]; i++)
        for (k = 0; k < 6 && drivers[i]->hid[k] && n < 63; k++) hids[n++] = drivers[i]->hid[k];
    hids[n] = NULL;
    nadevs = acpi_find_devices(hids, adevs, (int)ARRAY_SIZE(adevs));
    for (i = 0; i < nadevs; i++)
        kprintf("acpi: %s (%s) I2C %s %02x, %u GPIO\n", adevs[i].hid, adevs[i].path,
                adevs[i].ni2c ? adevs[i].i2cbus : "-", adevs[i].ni2c ? adevs[i].i2caddr[0] : 0, adevs[i].ngpio);
    return nadevs;
}

int cdc_present(void)
{
    return find_acpi() > 0;
}

/* ---------------------------------------------------------------- register access */

int cdc_rd(struct cdc *c, u32 reg)
{
    u8 w[2], r[2];
    int wn = 0, rn = c->drv->valbits == 16 ? 2 : 1;
    if (c->drv->regbits == 16) w[wn++] = (u8)(reg >> 8);
    w[wn++] = (u8)reg;
    if (i2c_xfer(c->bus, c->addr, w, wn, r, rn)) return -1;
    return rn == 2 ? (r[0] << 8) | r[1] : r[0];
}

int cdc_wr(struct cdc *c, u32 reg, u32 val)
{
    u8 w[4];
    int n = 0;
    if (c->drv->regbits == 16) w[n++] = (u8)(reg >> 8);
    w[n++] = (u8)reg;
    if (c->drv->valbits == 16) w[n++] = (u8)(val >> 8);
    w[n++] = (u8)val;
    return i2c_xfer(c->bus, c->addr, w, n, NULL, 0);
}

int cdc_upd(struct cdc *c, u32 reg, u32 mask, u32 val)
{
    int v = cdc_rd(c, reg);
    if (v < 0) return -1;
    return cdc_wr(c, reg, ((u32)v & ~mask) | (val & mask));
}

void cdc_seq(struct cdc *c, const u16 *pairs, int n)
{
    int i;
    for (i = 0; i < n; i++) cdc_wr(c, pairs[2 * i], pairs[2 * i + 1]);
}

/* the idx-th GpioIo resource of the codec's ACPI device (GpioInt lines are skipped) */
static int gpio_io(struct cdc *c, int idx)
{
    int i;
    if (!c->ad) return -1;
    for (i = 0; i < c->ad->ngpio; i++)
        if (c->ad->gpio[i].io && !idx--) return i;
    return -1;
}

u32 cdc_step(u32 v, u32 steps)
{
    u32 s = (v * steps + 32767) / 65535;
    return !s && v ? 1 : s;
}

int cdc_ngpio(struct cdc *c)
{
    int i, n = 0;
    if (!c->ad) return 0;
    for (i = 0; i < c->ad->ngpio; i++) n += c->ad->gpio[i].io ? 1 : 0;
    return n;
}

int cdc_gpio(struct cdc *c, int idx, int val)
{
    int i = gpio_io(c, idx);
    if (i < 0) return -1;
    return gpio_set(c->ad->gpio[i].ctl, c->ad->gpio[i].pin, val);
}

/* ---------------------------------------------------------------- calls from dspdev.c */

void cdc_stream(struct cdc *c, int dir, int on)
{
    if (c->drv->stream) c->drv->stream(c, dir, on);
}

void cdc_volume(struct cdc *c, int ctl, u32 lr)
{
    if (c->drv->volume) c->drv->volume(c, ctl, lr);
}

void cdc_source(struct cdc *c, int src)
{
    if (c->drv->source) c->drv->source(c, src);
}

void cdc_poll(struct cdc *c)
{
    int j;
    if (!c->drv->jack || plat_ms() - c->jacktime < 300) return;
    c->jacktime = plat_ms();
    j = c->drv->jack(c);
    if (j < 0 || j == c->jackstate) return;
    c->jackstate = (u8)j;
    kprintf("%s: headphones %s%s\n", c->drv->name, (j & 1) ? "in" : "out", (j & 2) ? ", headset microphone" : "");
}

void cdc_off(struct cdc *c)
{
    if (c->drv->off) c->drv->off(c);
}

int cdc_has_volume(struct cdc *c)
{
    return c->drv->volume != NULL;
}

/* ---------------------------------------------------------------- machine */

/* MCLK of the SSP on IPC3 platforms (the crystal), Hz */
static u32 plat_mclk(struct dsp *d)
{
    switch (d->pl->gen) {
    case DG_BYT: return d->shim == SST_BDW ? 24000000 : 19200000;  /* PMC clock 3 / DSP MCLK */
    case DG_APL: return 19200000;
    case DG_CNL: return 24000000;
    case DG_ICL: return d->pl->ssps == 6 && d->pl->cores == 2 ? 24000000 : 19200000;   /* JSL / ICL */
    case DG_TGL: return 38400000;
    default: return 19200000;
    }
}

/* SSP ports from NHLT: the port with I2S capture (headset codec) and render-only ports */
static int nhlt_ports(int *headset, int *amp)
{
    int i, n = nhlt_count(), any = 0;
    *headset = *amp = -1;
    for (i = 0; i < n; i++) {
        const struct nhlt_ep *e = nhlt_get(i);
        if (e->link != NHLT_LINK_SSP || e->dev == NHLT_DEV_BT) continue;
        any = 1;
        if (e->dir == 1 && *headset < 0) *headset = e->bus;
    }
    for (i = 0; i < n; i++) {
        const struct nhlt_ep *e = nhlt_get(i);
        if (e->link != NHLT_LINK_SSP || e->dev == NHLT_DEV_BT || e->dir) continue;
        if (e->bus != *headset && *amp < 0) *amp = e->bus;
        if (*headset < 0 && *amp >= 0 && e->bus != *amp) *headset = e->bus;
    }
    return any;
}

/* the usual SSP ports of the reference designs: headset codec, amplifier */
static void default_ports(struct dsp *d, const struct cdc *hs, int *headset, int *amp)
{
    *headset = 0;
    *amp = 1;
    if (d->pl->gen == DG_BYT) {
        /* Bay Trail / Cherry Trail machine drivers: SSP2 (Bay Trail CR: SSP0); Broadwell: SSP0 */
        *headset = *amp = d->shim == SST_BYT && !d->bytcr ? 2 : 0;
        return;
    }
    if (d->pl->gen == DG_APL) {
        if (!strncmp(d->pl->name, "glk", 4)) {
            *headset = 2;
            *amp = 1;
        } else {
            *headset = 1;
            *amp = 5;
        }
    } else if (d->pl->gen >= DG_TGL && hs && !strncmp(hs->drv->name, "rt5682", 6)) {
        *amp = 1;
    }
    if (hs && hs->drv->kind == CK_SPKCODEC && !strncmp(hs->drv->name, "es83", 4)) *headset = d->pl->gen >= DG_TGL ? 0 : 2;
}

/* IPC3: the port settings when the firmware computes the SSP registers */
static void ssp_defaults(struct dsp *d, struct cdc *c)
{
    struct sspcfg *s = &c->ssp;
    s->mclk = plat_mclk(d);
    /* Gemini Lake boards take the codec clock from MCLK1 (the SOF topologies of these machines) */
    s->mclkid = (u8)(d->pl->gen == DG_APL && !strncmp(d->pl->name, "glk", 4) ? 1 : 0);
    s->slots = 2;
    s->txmask = s->rxmask = 3;
    s->fmt = 1;                                 /* I2S */
    s->provider = 0;                            /* the SSP drives the clocks */
    s->width = c->drv->slotw ? c->drv->slotw : 25;
    if (d->pl->gen == DG_BYT)
        /* the SSP divides its 19.2 / 24 MHz clock by an integer: 25-bit slots (BCLK 2.4 MHz),
           20-bit slots (1.92 MHz) for 16-bit chips, as the SOF topologies of these machines */
        s->width = c->drv->slotw && c->drv->slotw <= 20 ? 20 : 25;
    s->vbits = s->width >= 24 ? 24 : 16;
    s->bclk = 48000UL * s->slots * s->width;
}

/* endpoint on an SSP port with the NHLT blob of its format */
static struct dspep *ssp_ep(struct dsp *d, int port, int dir, struct cdc *c)
{
    struct dspep *e = dsp_ep_new(d, EP_SSP, dir, port);
    struct afmt f;
    struct sspcfg s;
    u32 len = 0;
    if (!e) return NULL;
    if (nhlt_best(NHLT_LINK_SSP, NHLT_DEV_I2S, dir, port, &f) ||
        nhlt_best(NHLT_LINK_SSP, -1, dir, port, &f)) {
        e->lfmt = f;
        e->blob = nhlt_blob(NHLT_LINK_SSP, -1, dir, port, AF_CH(&f), f.rate, (int)f.bits, AF_VBITS(&f), &len, NULL);
        e->bloblen = len;
        if (e->blob && !nhlt_ssp_decode(e->blob, len, f.rate, dsp_xtal(d), &s)) {
            /* the port as the firmware of the machine set it up: the codec follows it */
            c->ssp = s;
            DBG("dsp: SSP%d fmt %u, %u x %u bit slots (%u valid), BCLK %lu, MCLK %lu%s\n", port, s.fmt, s.slots,
                s.width, s.vbits, s.bclk, s.mclk, s.provider ? ", codec provides the clocks" : "");
        }
    } else if (d->ipc4) {
        kprintf("dsp: SSP%d %s: no NHLT configuration, port not usable with IPC4 firmware\n", port,
                dir ? "capture" : "playback");
        d->nep--;
        return NULL;
    } else {
        afmt_set(&e->lfmt, 48000, c->ssp.vbits == 16 ? 16 : 32, c->ssp.vbits, 2);
    }
    e->ssp = c->ssp;
    /* IPC3 firmware refuses an SSP without MCLK rate (blob without MCLK): the crystal */
    if (!d->ipc4 && !e->ssp.mclk) e->ssp.mclk = plat_mclk(d);
    if (!c->ssp.vbits) {
        c->ssp.vbits = (u16)AF_VBITS(&e->lfmt);
        c->ssp.width = (u16)e->lfmt.bits;
    }
    return e;
}

int cdc_setup(struct dsp *d)
{
    int i, n, hsport, ampport, namp;
    struct cdc *hs = NULL, *amp = NULL, *list[MAXCDC];
    struct dspep *hout = NULL, *hin = NULL, *aout = NULL;
    char name[48];
    n = find_acpi();
    ncdc = 0;
    for (i = 0; i < n; i++) {
        struct acpi_dev *a = &adevs[i];
        const struct cdcdrv *drv;
        int j, k, na, at = 0, at2, ok;
        if ((drv = drv_of(a->hid, &at)) == NULL) continue;
        /* one ACPI device may stand for two amplifiers (two I2C addresses) */
        na = a->ni2c && drv->kind == CK_AMP ? a->ni2c : 1;
        for (j = 0; j < na && ncdc < MAXCDC; j++) {
            struct cdc *c = &cdcs[ncdc];
            for (k = 0; k < ncdc && a->ni2c; k++)
                if (cdcs[k].bus && cdcs[k].addr == a->i2caddr[j] && !strncmp(cdcs[k].ad->i2cbus, a->i2cbus, 32))
                    break;
            if (a->ni2c && k < ncdc) continue;          /* the same chip listed twice */
            memset(c, 0, sizeof(*c));
            c->ad = a;
            c->d = d;
            if (a->ni2c) {
                c->bus = i2c_bus_get(a->i2cbus);
                c->addr = (u8)a->i2caddr[j];
                if (!c->bus) continue;
            }
            /* the first driver for the id whose probe recognizes the chip */
            ok = 0;
            at2 = at;
            for (c->drv = drv; c->drv; c->drv = drv_of(a->hid, &at2)) {
                if (!c->drv->probe || !c->drv->probe(c)) {
                    ok = 1;
                    break;
                }
                DBG("%s: no answer at %02x\n", c->drv->name, c->addr);
            }
            if (!ok) continue;
            kprintf("dsp: codec %s (%s)%s\n", c->drv->name, a->hid, a->ni2c ? "" : ", no control bus");
            ncdc++;
        }
    }
    for (i = 0; i < ncdc; i++) {
        if (cdcs[i].drv->kind == CK_AMP) {
            if (!amp) amp = &cdcs[i];
        } else if (!hs) {
            hs = &cdcs[i];
        }
    }
    if (!hs && !amp) return 0;
    if (!nhlt_ports(&hsport, &ampport)) default_ports(d, hs, &hsport, &ampport);
    if (hs && !strncmp(hs->drv->name, "es83", 4) && nhlt_count()) {
        /* Everest machines: the highest SSP the NHLT lists (as Linux picks it) */
        for (i = 0; i < nhlt_count(); i++) {
            const struct nhlt_ep *e = nhlt_get(i);
            if (e->link == NHLT_LINK_SSP && e->dev != NHLT_DEV_BT && e->bus > hsport) hsport = e->bus;
        }
    }
    if (hsport < 0 || ampport < 0) {
        int h2, a2;
        default_ports(d, hs, &h2, &a2);
        if (hsport < 0) hsport = h2;
        if (ampport < 0) ampport = a2;
    }
    /* Bay Trail CR: the SSP0 link goes to the codec's AIF1 or AIF2 as the CHAN package of the
       codec's ACPI device says (AIF2 when there is none), as Linux bytcr_rt5640 / rt5651 */
    if (d->pl->gen == DG_BYT && d->bytcr && hs && hs->ad) {
        u32 chan[2];
        hs->aif = acpi_dev_pkg(hs->ad->path, "CHAN", chan, 2) >= 1 && chan[0] == 1 ? 1 : 2;
        kprintf("%s: SSP0 on the codec's AIF%u\n", hs->drv->name, hs->aif);
    }
    /* clocks and formats, codecs initialized for them */
    namp = 0;
    for (i = 0; i < ncdc; i++) {
        struct cdc *c = &cdcs[i];
        c->port = (u8)(c->drv->kind == CK_AMP ? ampport : hsport);
        if (c->drv->kind == CK_AMP) c->chan = (u8)namp++;
        if (!d->ipc4) ssp_defaults(d, c);
    }
    if (hs) {
        hout = ssp_ep(d, hsport, 0, hs);
        hin = ssp_ep(d, hsport, 1, hs);
    }
    if (amp && (!hs || ampport != hsport)) {
        aout = ssp_ep(d, ampport, 0, amp);
    } else if (amp) {
        aout = hout;                                /* amplifier on the codec's port (TDM) */
    }
    for (i = 0; i < ncdc; i++) {
        struct cdc *c = &cdcs[i];
        c->namps = (u8)namp;
        if (c->drv->kind == CK_AMP && c != amp) c->ssp = amp->ssp;     /* the other amplifiers of the port */
        if (c->drv->kind == CK_AMP && hs && ampport == hsport) c->ssp = hs->ssp;
    }
    for (i = 0; i < ncdc; i++) {
        struct cdc *c = &cdcs[i];
        if (c->drv->init && c->drv->init(c)) kprintf("%s: setup failed\n", c->drv->name);
        if (c->drv->jack) c->jackstate = (u8)c->drv->jack(c);
    }
    n = 0;
    for (i = 0; i < ncdc; i++) list[n++] = &cdcs[i];
    ksnprintf(name, sizeof(name), "Intel SST %s%s%s", hs ? hs->drv->name : "", hs && amp ? "+" : "",
              amp ? amp->drv->name : "");
    dsp_adev_jack(dsp_adev_new(d, name, aout ? aout : hout, hin, list, n), hout, aout, hs);
    return 1;
}
