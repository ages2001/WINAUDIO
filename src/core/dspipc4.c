/* dspipc4.c - IPC4 (Intel cAVS / ACE firmware and SOF in IPC4 mode): every endpoint is two
   pipelines, a host copier (HD Audio host DMA gateway) and a DAI copier (HD Audio link DMA,
   SSP, DMIC or SoundWire ALH gateway with its NHLT configuration), bound to each other. The
   copiers convert between the 16-bit host format and the format of the interface. */
#include "dsp.h"

/* global messages */
#define G_CREATE_PIPELINE   17
#define G_DELETE_PIPELINE   18
#define G_SET_PIPELINE      19
#define G_LOAD_LIBRARY      24
/* module messages */
#define M_INIT              0
#define M_LARGE_GET         3
#define M_LARGE_SET         4
#define M_BIND              5
#define M_UNBIND            6
#define M_DELETE            11

#define PPL_RESET           2
#define PPL_PAUSED          3
#define PPL_RUNNING         4

/* gateway node types */
#define N_HOST_OUT          0
#define N_HOST_IN           1
#define N_LINK_OUT          8
#define N_LINK_IN           9
#define N_DMIC_IN           11
#define N_SSP_OUT           12
#define N_SSP_IN            13
#define N_ALH_OUT           16
#define N_ALH_IN            17

#define GTW_DMA_CONFIG_ID   0x1000
#define ALH_MULTI_GTW_BASE  0x50        /* node index of aggregated ALH gateways (8 groups) */

static u32 glb(int type) { return (u32)type << 24; }
static u32 mod(int type, int id, int inst) { return (1UL << 30) | ((u32)type << 24) | (u32)id | ((u32)inst << 16); }

static int pipeline_create(struct dsp *d, int id, int pages)
{
    return dsp_ipc(d, glb(G_CREATE_PIPELINE) | (u32)pages | ((u32)id << 16), 0, NULL, 0, NULL, 0, NULL);
}

static int pipeline_state(struct dsp *d, int id, int state)
{
    return dsp_ipc(d, glb(G_SET_PIPELINE) | (u32)state | ((u32)id << 16), 0, NULL, 0, NULL, 0, NULL);
}

static int pipeline_delete(struct dsp *d, int id)
{
    return dsp_ipc(d, glb(G_DELETE_PIPELINE) | ((u32)id << 16), 0, NULL, 0, NULL, 0, NULL);
}

static int bind(struct dsp *d, int unbind, int src, int srcinst, int dst, int dstinst)
{
    u32 ext = (u32)dst | ((u32)dstinst << 16);         /* queues 0 */
    return dsp_ipc(d, mod(unbind ? M_UNBIND : M_BIND, src, srcinst), ext, NULL, 0, NULL, 0, NULL);
}

/* ---- pipeline and instance ids ---- */
static u32 ppl_used, inst_used;

static int id_get(u32 *map)
{
    int i;
    for (i = 0; i < 32; i++)
        if (!(*map & (1UL << i))) {
            *map |= 1UL << i;
            return i;
        }
    return -1;
}

static void id_put(u32 *map, int i)
{
    if (i >= 0 && i < 32) *map &= ~(1UL << i);
}

/* ---- ALH (SoundWire): aggregated gateways share a multi-gateway group ---- */
static u32 alh_groups;

static void alh_grp_put(struct dspep *e)
{
    if (e->alhgrp) id_put(&alh_groups, e->alhgrp - 1);
    e->alhgrp = 0;
}

/* channel mask of the k-th ALH stream: given by sdw.c, else an even split */
static u32 alh_mask(const struct dspep *e, int k)
{
    int nk = e->nalh > 0 ? e->nalh : 1, ch = AF_CH(&e->lfmt) / nk;
    if (e->alhmask[k]) return e->alhmask[k];
    return ((1UL << ch) - 1) << (k * ch);
}

/* the device of the k-th ALH stream: ACE 2+ names the link and PDI (link DMA), earlier
   generations the ALH node */
static u32 alh_dev(const struct dsp *d, const struct dspep *e, int k)
{
    if (d->pl->gen >= DG_LNL) return e->alhdev[k];
    return ((u32)(e->dir ? N_ALH_IN : N_ALH_OUT) << 8) | e->alhstream[k];
}

/* cycles per chunk: the module configuration of the manifest with these buffer sizes */
static u32 copier_cpc(struct dsp *d, u32 ibs, u32 obs)
{
    u32 i;
    const u8 *c = d->modcfg;
    if (c)
        for (i = 0; i < d->copier_ncfg; i++) {
            const u32 *m = (const u32 *)(c + (d->copier_cfg + i) * 44);
            if (m[6] == ibs && m[7] == obs && m[9]) return m[9];
        }
    return d->avs ? 12000 : 0;
}

/* struct copier_cfg: base config, out format, feature mask, gateway: node, DMA buffer,
   config length, config (gateway attributes or NHLT blob), [DMA config TLV] */
static u32 put_fmt(u32 *p, const struct afmt *f)
{
    p[0] = f->rate;
    p[1] = f->bits;
    p[2] = f->chmap;
    p[3] = f->chcfg;
    p[4] = f->ileave;
    p[5] = f->cfg;
    return 6;
}

static int copier_init(struct dsp *d, struct dspep *e, int host, int ppl, u16 *inst)
{
    u32 *p = (u32 *)d->buf, n = 0, ibs, obs, node, dmabuf, lenidx;
    const struct afmt *in, *out;
    int i, ret;
    /* playback: host 16 bit -> interface format; capture: interface -> host */
    if (!e->dir) {
        in = host ? &e->hfmt : &e->lfmt;
        out = &e->lfmt;
    } else {
        in = &e->lfmt;
        out = &e->hfmt;
        if (host) in = &e->hfmt;
    }
    ibs = afmt_bytes_ms(in);
    obs = afmt_bytes_ms(out);
    if (!host && e->type == EP_SDW && e->nalh > 1 && !e->alhgrp) {
        int g = id_get(&alh_groups);
        if (g < 0 || g >= 8) {
            id_put(&alh_groups, g);
            kprintf("dsp: no free ALH multi-gateway group\n");
            return -1;
        }
        e->alhgrp = (u8)(g + 1);
    }
    i = id_get(&inst_used);
    if (i < 0) return -1;
    memset(p, 0, 512);
    p[n++] = copier_cpc(d, ibs, obs);
    p[n++] = ibs;
    p[n++] = obs;
    p[n++] = 0;                                 /* is_pages */
    n += put_fmt(p + n, in);
    n += put_fmt(p + n, out);
    p[n++] = 0;                                 /* feature mask */
    if (host) {
        node = ((u32)(e->dir ? N_HOST_IN : N_HOST_OUT) << 8) | (u32)(e->htag - 1);
        dmabuf = 4 * (e->dir ? obs : ibs);
    } else {
        switch (e->type) {
        case EP_HDA:  node = ((u32)(e->dir ? N_LINK_IN : N_LINK_OUT) << 8) | (u32)(e->ltag - 1); break;
        case EP_SSP:  node = ((u32)(e->dir ? N_SSP_IN : N_SSP_OUT) << 8) | ((u32)e->port << 4); break;
        case EP_DMIC: node = ((u32)N_DMIC_IN << 8) | (u32)e->port; break;
        default:
            node = ((u32)(e->dir ? N_ALH_IN : N_ALH_OUT) << 8) |
                   (e->alhgrp ? (u32)ALH_MULTI_GTW_BASE + e->alhgrp - 1 : e->alhstream[0]);
            break;
        }
        dmabuf = 2 * (e->dir ? ibs : obs);
    }
    p[n++] = node;
    p[n++] = dmabuf;
    lenidx = n++;
    if (!host && e->blob && e->bloblen >= 4) {
        u32 w = (e->bloblen + 3) / 4;
        if (n + w + 40 > 1000) return -1;
        p[n + w - 1] = 0;
        memcpy(p + n, e->blob, e->bloblen);
        n += w;
    } else if (!host && e->type == EP_SDW) {
        /* ALH: gateway attributes, stream count, (device, channel mask) per stream */
        int k, nk = e->nalh > 0 ? (e->nalh < 4 ? e->nalh : 4) : 1;
        p[n++] = 0;
        p[n++] = (u32)nk;
        for (k = 0; k < nk; k++) {
            p[n++] = alh_dev(d, e, k);
            p[n++] = alh_mask(e, k);
        }
    } else {
        p[n++] = 0;                             /* gateway attributes only */
    }
    p[lenidx] = n - lenidx - 1;
    if (!host && d->pl->gen >= DG_LNL && (e->type == EP_SSP || e->type == EP_DMIC)) {
        /* ACE 2+: SSP and DMIC run on HD Audio link DMA, named in a DMA config TLV after the
           gateway configuration (counted in its length) */
        memset(p + n, 0, 8 + 148);
        p[n++] = GTW_DMA_CONFIG_ID;
        p[n++] = 148;
        p[n] = 1 | (1UL << 8);                  /* method: HD Audio DMA, allocated by the host */
        p[n + 1] = (u32)(e->ltag - 1);          /* DMA channel */
        p[n + 2] = (u32)e->ltag;                /* stream */
        p[n + 3] = 1;                           /* one device mapping, as Linux sends it */
        n += 148 / 4;
        p[lenidx] += 2 + 148 / 4;
    }
    if (!host && d->pl->gen >= DG_LNL && e->type == EP_SDW) {
        /* ACE 2+: each ALH stream on its HD Audio link DMA, one DMA config TLV each */
        int k, nk = e->nalh > 0 ? (e->nalh < 4 ? e->nalh : 4) : 1;
        for (k = 0; k < nk; k++) {
            memset(p + n, 0, 8 + 148);
            p[n++] = GTW_DMA_CONFIG_ID;
            p[n++] = 148;
            p[n] = 1 | (1UL << 8);              /* method: HD Audio DMA, allocated by the host */
            p[n + 1] = (u32)(e->alhltag[k] - 1);
            p[n + 2] = (u32)e->alhltag[k];
            p[n + 3] = 1;                       /* one device: link << 8 | PDI */
            p[n + 4] = e->alhdev[k];
            p[n + 5] = alh_mask(e, k);
            n += 148 / 4;
            p[lenidx] += 2 + 148 / 4;
        }
    }
    ret = dsp_ipc(d, mod(M_INIT, d->copier_id, i), n | ((u32)ppl << 16), p, n * 4, NULL, 0, NULL);
    if (ret) {
        kprintf("dsp: copier (%s, %s) init failed: %d\n", host ? "host" : "interface",
                e->dir ? "capture" : "playback", ret);
        id_put(&inst_used, i);
        return -1;
    }
    *inst = (u16)i;
    return 0;
}

int ipc4_ready(struct dsp *d)
{
    ppl_used = inst_used = alh_groups = 0;
    kprintf("dsp: firmware ready (IPC4, copier module %u)\n", d->copier_id);
    return 0;
}

/* pipeline memory in 4K pages for a copier of ibs bytes per chunk: pipeline and task objects
   and eight queues of one chunk (the estimate of Linux sof_ipc4_update_resource_usage) */
static int ppl_pages(u32 ibs)
{
    u32 n = 1024 + 8 * (128 + ((ibs + 63) & ~63UL));
    n = (n + 4095) / 4096;
    return n < 2 ? 2 : (int)n;
}

int ipc4_ep_start(struct dspep *e)
{
    struct dsp *d = e->d;
    int hp, dp;
    hp = id_get(&ppl_used);
    dp = id_get(&ppl_used);
    if (hp < 0 || dp < 0) goto fail0;
    e->hppl = (u8)hp;
    e->dppl = (u8)dp;
    if (pipeline_create(d, hp, ppl_pages(afmt_bytes_ms(&e->hfmt))) ||
        pipeline_create(d, dp, ppl_pages(afmt_bytes_ms(&e->lfmt)))) goto fail1;
    if (copier_init(d, e, 1, hp, &e->hinst)) goto fail1;
    if (copier_init(d, e, 0, dp, &e->dinst)) goto fail2;
    if (!e->dir ? bind(d, 0, d->copier_id, e->hinst, d->copier_id, e->dinst)
                : bind(d, 0, d->copier_id, e->dinst, d->copier_id, e->hinst)) {
        kprintf("dsp: bind failed\n");
        goto fail3;
    }
    /* created, bound: reset, paused, running (Linux avs_dai_prepare, avs_dai_fe_trigger) */
    if (pipeline_state(d, dp, PPL_RESET) || pipeline_state(d, hp, PPL_RESET) ||
        pipeline_state(d, dp, PPL_PAUSED) || pipeline_state(d, hp, PPL_PAUSED)) goto fail4;
    if (e->dir ? pipeline_state(d, dp, PPL_RUNNING) || pipeline_state(d, hp, PPL_RUNNING)
               : pipeline_state(d, hp, PPL_RUNNING) || pipeline_state(d, dp, PPL_RUNNING)) {
        kprintf("dsp: pipelines do not run\n");
        goto fail4;
    }
    e->prepared = 1;
    return 0;
fail4:
    pipeline_state(d, hp, PPL_RESET);
    pipeline_state(d, dp, PPL_RESET);
    if (!e->dir) bind(d, 1, d->copier_id, e->hinst, d->copier_id, e->dinst);
    else bind(d, 1, d->copier_id, e->dinst, d->copier_id, e->hinst);
fail3:
    id_put(&inst_used, e->dinst);
fail2:
    id_put(&inst_used, e->hinst);
fail1:
    pipeline_delete(d, hp);
    pipeline_delete(d, dp);
fail0:
    id_put(&ppl_used, hp);
    id_put(&ppl_used, dp);
    alh_grp_put(e);
    return -1;
}

void ipc4_ep_stop(struct dspep *e)
{
    struct dsp *d = e->d;
    if (!e->prepared) return;
    pipeline_state(d, e->hppl, PPL_PAUSED);
    pipeline_state(d, e->dppl, PPL_PAUSED);
    pipeline_state(d, e->hppl, PPL_RESET);
    pipeline_state(d, e->dppl, PPL_RESET);
    if (!e->dir) bind(d, 1, d->copier_id, e->hinst, d->copier_id, e->dinst);
    else bind(d, 1, d->copier_id, e->dinst, d->copier_id, e->hinst);
    pipeline_delete(d, e->hppl);
    pipeline_delete(d, e->dppl);
    id_put(&inst_used, e->hinst);
    id_put(&inst_used, e->dinst);
    id_put(&ppl_used, e->hppl);
    id_put(&ppl_used, e->dppl);
    alh_grp_put(e);
    e->prepared = 0;
}
