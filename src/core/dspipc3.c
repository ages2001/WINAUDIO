/* dspipc3.c - SOF IPC3 (Apollo Lake to Ice Lake / Jasper Lake / Elkhart Lake, Bay Trail,
   Cherry Trail, Broadwell): an endpoint is one pipeline host -> buffer -> DAI (playback) or
   DAI -> buffer -> host (capture). The DAI component converts between the 16-bit host
   format and the interface format; the firmware computes the SSP and DMIC registers from the
   DAI configuration. */
#include "dspcdc.h"

#define GLB_TPLG        0x30000000UL
#define GLB_STREAM      0x60000000UL
#define GLB_DAI         0x80000000UL
#define TPLG_COMP_NEW   0x00010000UL
#define TPLG_COMP_FREE  0x00020000UL
#define TPLG_CONNECT    0x00030000UL
#define TPLG_PIPE_NEW   0x00100000UL
#define TPLG_PIPE_FREE  0x00110000UL
#define TPLG_PIPE_DONE  0x00130000UL
#define TPLG_BUF_NEW    0x00200000UL
#define TPLG_BUF_FREE   0x00210000UL
#define DAI_CONFIG      0x00010000UL
#define STR_PCM_PARAMS  0x00010000UL
#define STR_PCM_FREE    0x00030000UL
#define STR_START       0x00040000UL
#define STR_STOP        0x00050000UL

#define COMP_HOST       1
#define COMP_DAI        2
#define COMP_BUFFER     12

#define DAI_SSP         1
#define DAI_DMIC        2
#define DAI_HDA         3
#define DAI_ALH         4

#define FRAME_S16       0
#define FRAME_S24_4     1
#define FRAME_S32       2

static u32 msg[96];                 /* one message (the firmware takes at most 384 bytes) */
static u32 reply[16];

static int send(struct dsp *d, u32 cmd, u32 words)
{
    msg[0] = words * 4;
    msg[1] = cmd;
    return dsp_ipc3(d, msg, words * 4, reply, sizeof(reply));
}

/* struct sof_ipc_comp + sof_ipc_comp_config */
static u32 comp_hdr(u32 id, u32 type, u32 ppl, int frame)
{
    msg[2] = id;
    msg[3] = type;
    msg[4] = ppl;
    msg[5] = 0;                     /* core */
    msg[6] = 0;                     /* ext data */
    msg[7] = 36;                    /* config.hdr.size */
    msg[8] = 0;
    msg[9] = 2;                     /* periods sink */
    msg[10] = 2;                    /* periods source */
    msg[11] = 0;
    msg[12] = (u32)frame;
    msg[13] = 0;                    /* xrun action */
    msg[14] = msg[15] = 0;
    return 16;
}

/* DAI index of the firmware: SSP / DMIC port; ALH: link << 8 | PDI, counted from PDI 2 */
static u32 dai_index(const struct dspep *e)
{
    if (e->type == EP_HDA) return 0;
    if (e->type == EP_SDW) return e->alhdev[0] >= 2 ? e->alhdev[0] - 2 : 0;
    return e->port;
}

/* sample format of the DAI's DMA: the DMIC FIFO packs 16 or 32-bit words (24 valid bits are
   MSB aligned: S32), the SSP shifts the valid bits LSB aligned (S24_4LE for 24 in 32) */
static int frame_of(const struct dspep *e)
{
    const struct afmt *f = &e->lfmt;
    if (f->bits == 16) return FRAME_S16;
    if (e->type != EP_DMIC && AF_VBITS(f) == 24 && f->bits == 32) return FRAME_S24_4;
    return FRAME_S32;
}

/* the memory windows (struct sof_ipc_window after its 12-byte header: count, then 24-byte
   elements) from FW_READY or from the extended manifest */
static void windows(struct dsp *d, const u32 *w, u32 *inbox, u32 *insz, u32 *outbox, u32 *outsz)
{
    u32 n = w[0], i;
    for (i = 0; i < n && i < 16; i++) {
        const u32 *e = w + 1 + i * 6;
        u32 base = d->win0 + e[2] * d->winsz + e[5];
        if (e[1] == 1) { *inbox = base; *insz = e[4]; }        /* UPBOX: DSP -> host */
        if (e[1] == 0) { *outbox = base; *outsz = e[4]; }      /* DOWNBOX: host -> DSP */
        if (e[1] == 4) d->streambox = base;                    /* STREAM: positions */
    }
}

int ipc3_ready(struct dsp *d)
{
    u32 off = d->dspbox, *fr = (u32 *)d->buf, ext, guard = 16;
    u32 inbox = 0, insz = 0, outbox = 0, outsz = 0;
    dsp_mbox_read(d, off, fr, 108);
    d->abi = fr[16];
    kprintf("dsp: SOF %u.%u.%u build %u, ABI %lu.%lu.%lu\n", (u16)fr[7], (u16)(fr[7] >> 16), (u16)fr[8],
            (u16)(fr[8] >> 16), (d->abi >> 24) & 0xFF, (d->abi >> 12) & 0xFFF, d->abi & 0xFFF);
    ext = off + 108;
    while (guard--) {
        u32 h[3];
        dsp_mbox_read(d, ext, h, 12);
        if ((h[1] & 0xF0000000UL) != 0x70000000UL || h[0] < 12 || h[0] > 1024) break;
        if (h[2] == 1) {                            /* SOF_IPC_EXT_WINDOW */
            u32 *w = fr + 32;
            dsp_mbox_read(d, ext + 12, w, 4);
            if (w[0] > 16) w[0] = 16;
            dsp_mbox_read(d, ext + 16, w + 1, w[0] * 24);
            windows(d, w, &inbox, &insz, &outbox, &outsz);
        }
        ext += h[0];
    }
    /* SST firmware: the windows are only in the extended manifest of the image */
    if ((!inbox || !outbox) && d->xwin) windows(d, d->xwin, &inbox, &insz, &outbox, &outsz);
    if (!inbox || !outbox) {
        kprintf("dsp: firmware gave no mailbox windows\n");
        return -1;
    }
    d->dspbox = inbox;
    d->dspboxsz = insz;
    d->hostbox = outbox;
    d->hostboxsz = outsz;
    d->next_comp = 1;
    DBG("dsp: mailboxes up %05lx (%lu) down %05lx (%lu)\n", inbox, insz, outbox, outsz);
    return 0;
}

/* sof_ipc_dai_config with the parameters of the interface */
static int dai_config(struct dsp *d, struct dspep *e, int flags)
{
    u32 n, i;
    memset(msg, 0, sizeof(msg));
    msg[2] = e->type == EP_SSP ? DAI_SSP : e->type == EP_DMIC ? DAI_DMIC : e->type == EP_HDA ? DAI_HDA : DAI_ALH;
    msg[3] = dai_index(e);
    /* format(16) group(8) flags(8), reserved[8] */
    n = 13;
    switch (e->type) {
    case EP_SSP: {
        const struct sspcfg *c = &e->ssp;
        u32 fmt = c->fmt ? c->fmt : 1;
        fmt |= c->provider ? 0 : (4UL << 12);       /* codec clock and frame consumer */
        msg[4] = fmt | ((u32)flags << 24);
        msg[n++] = 0;                               /* reserved0 */
        msg[n++] = (u32)c->mclkid << 16;            /* reserved1, mclk_id */
        msg[n++] = c->mclk;
        msg[n++] = e->lfmt.rate;                    /* fsync */
        msg[n++] = c->bclk;
        msg[n++] = c->slots;
        msg[n++] = c->rxmask;
        msg[n++] = c->txmask;
        msg[n++] = c->vbits;
        msg[n++] = c->width;                        /* tdm_slot_width, reserved2 */
        msg[n++] = 0;                               /* mclk direction */
        /* frame pulse width; DSP A / B: slots of tdm_slot_width bits (the valid bits padded
           in every slot) instead of packed valid bits */
        msg[n++] = c->fmt >= 4 && c->width > c->vbits ? 1UL << 16 : 0;
        msg[n++] = 0;                               /* clks_control */
        msg[n++] = 0;                               /* quirks */
        msg[n++] = 0;                               /* bclk delay */
        break;
    }
    case EP_DMIC: {
        int pdms = AF_CH(&e->lfmt) > 2 ? 2 : 1;
        msg[4] = 6 | ((u32)flags << 24);            /* SOF_DAI_FMT_PDM */
        msg[n++] = 0;
        msg[n++] = 1;                               /* driver IPC version */
        msg[n++] = 500000;                          /* PDM clock min */
        msg[n++] = 4800000;                         /* PDM clock max */
        msg[n++] = e->lfmt.rate;                    /* FIFO A rate */
        msg[n++] = 0;
        msg[n++] = e->lfmt.bits == 16 ? 16 : 32;    /* fifo_bits, fifo_bits_b */
        msg[n++] = 40 | (60UL << 16);               /* duty min / max (%) */
        msg[n++] = (u32)pdms;
        msg[n++] = 0;                               /* wake up time */
        msg[n++] = 0;                               /* min clock on time */
        msg[n++] = 200;                             /* unmute ramp, ms */
        n += 5;
        for (i = 0; i < 4; i++) {
            msg[n++] = 0;
            msg[n++] = i | ((i < (u32)pdms ? 1UL : 0) << 16);   /* id, enable mic A */
            msg[n++] = (i < (u32)pdms ? 1UL : 0);               /* enable mic B, polarity A */
            msg[n++] = 0;                                       /* polarity B, clock edge */
            msg[n++] = 0;                                       /* skew, reserved */
            msg[n++] = 0;
        }
        break;
    }
    case EP_HDA:
        msg[4] = (u32)flags << 24;
        msg[n++] = 0;
        msg[n++] = (u32)(e->ltag - 1);              /* link DMA channel */
        msg[n++] = e->lfmt.rate;
        msg[n++] = (u32)AF_CH(&e->lfmt);
        break;
    default:                                        /* ALH (SoundWire) */
        msg[4] = (u32)flags << 24;
        msg[n++] = 0;
        msg[n++] = e->alhstream[0];
        msg[n++] = e->lfmt.rate;
        msg[n++] = (u32)AF_CH(&e->lfmt);
        n += 13;
        break;
    }
    return send(d, GLB_DAI | DAI_CONFIG, n);
}

int ipc3_ep_start(struct dspep *e)
{
    struct dsp *d = e->d;
    u32 n, ppl, buf = 0, size;
    int ret;
    ppl = d->next_comp++;
    e->c_pipe = ppl;
    e->c_host = d->next_comp++;
    e->c_buf1 = d->next_comp++;
    e->c_dai = d->next_comp++;
    /* host component */
    n = comp_hdr(e->c_host, COMP_HOST, ppl, FRAME_S16);
    msg[n++] = e->dir;
    msg[n++] = 0;                                   /* no_irq */
    msg[n++] = 0;
    if ((ret = send(d, GLB_TPLG | TPLG_COMP_NEW, n)) != 0) goto fail;
    /* buffer: two periods of 1 ms at the interface format (frames per period rounded up as
       the firmware does: 45 at 44.1 kHz) */
    size = 2 * ((e->lfmt.rate + 999) / 1000) * (e->lfmt.bits / 8) * (u32)AF_CH(&e->lfmt);
    memset(msg, 0, 64);
    msg[2] = e->c_buf1;
    msg[3] = COMP_BUFFER;
    msg[4] = ppl;
    msg[7] = size;
    msg[8] = 1;                                     /* SOF_MEM_CAPS_RAM */
    if ((ret = send(d, GLB_TPLG | TPLG_BUF_NEW, 11)) != 0) goto fail;
    buf = 1;
    /* DAI component */
    n = comp_hdr(e->c_dai, COMP_DAI, ppl, frame_of(e));
    msg[n++] = e->dir;
    msg[n++] = dai_index(e);
    msg[n++] = e->type == EP_SSP ? DAI_SSP : e->type == EP_DMIC ? DAI_DMIC : e->type == EP_HDA ? DAI_HDA : DAI_ALH;
    msg[n++] = 0;
    if ((ret = send(d, GLB_TPLG | TPLG_COMP_NEW, n)) != 0) goto fail;
    /* pipeline, scheduled by the DAI, 1 ms */
    memset(msg, 0, 64);
    msg[2] = ppl;
    msg[3] = ppl;
    msg[4] = e->c_dai;
    msg[5] = 0;                                     /* core */
    msg[6] = 1000;                                  /* period, us */
    msg[7] = 0;                                     /* priority */
    msg[8] = 5000;                                  /* period MIPS */
    msg[9] = 0;
    msg[10] = 5;                                    /* xrun limit, us */
    msg[11] = d->shim ? 0 : 1;                      /* SST: DMA domain (as the topologies), else timer */
    if ((ret = send(d, GLB_TPLG | TPLG_PIPE_NEW, 12)) != 0) goto fail;
    /* connections */
    msg[2] = e->dir ? e->c_dai : e->c_host;
    msg[3] = e->c_buf1;
    if ((ret = send(d, GLB_TPLG | TPLG_CONNECT, 4)) != 0) goto fail;
    msg[2] = e->c_buf1;
    msg[3] = e->dir ? e->c_host : e->c_dai;
    if ((ret = send(d, GLB_TPLG | TPLG_CONNECT, 4)) != 0) goto fail;
    msg[2] = ppl;
    if ((ret = send(d, GLB_TPLG | TPLG_PIPE_DONE, 3)) != 0) goto fail;
    if ((ret = dai_config(d, e, 1)) != 0) {
        kprintf("dsp: DAI configuration refused (%d)\n", ret);
        goto fail;
    }
    /* stream parameters: host format, the ring of the sound engine */
    memset(msg, 0, sizeof(msg));
    msg[2] = e->c_host;
    msg[3] = 0;                                     /* flags */
    msg[6] = 84;                                    /* params.hdr.size (with channel map) */
    n = 7;
    msg[n++] = 28;                                  /* buffer.hdr.size */
    msg[n++] = d->shim ? vphys(e->bdl) : 0;         /* SST: page table; HD Audio DMA: unused */
    msg[n++] = (e->hw->size + 4095) / 4096;
    msg[n++] = e->hw->size;
    n += 3;
    msg[n++] = e->dir;
    msg[n++] = FRAME_S16;
    msg[n++] = 0;                                   /* interleaved */
    msg[n++] = e->hfmt.rate;
    msg[n++] = (u32)e->htag | ((u32)AF_CH(&e->hfmt) << 16);
    msg[n++] = 2 | (2UL << 16);                     /* valid / container bytes */
    msg[n++] = e->hw->period;                       /* host period bytes */
    /* no stream position messages; SST: the position window updated at every copy */
    msg[n++] = d->shim ? 1 | (1UL << 16) : 1;
    msg[n++] = 0;
    msg[n++] = 0 | (1UL << 16);                     /* channel map 0, 1 */
    n += 3;
    if ((ret = send(d, GLB_STREAM | STR_PCM_PARAMS, n)) != 0) {
        kprintf("dsp: stream parameters refused (%d)\n", ret);
        goto fail;
    }
    e->posoff = reply[4];
    e->prepared = 1;
    return 0;
fail:
    kprintf("dsp: IPC3 endpoint setup failed (%d)\n", ret);
    /* the components first, the pipeline last (the firmware's pipeline_free expects it) */
    msg[2] = e->c_host;
    send(d, GLB_TPLG | TPLG_COMP_FREE, 3);
    msg[2] = e->c_dai;
    send(d, GLB_TPLG | TPLG_COMP_FREE, 3);
    if (buf) {
        msg[2] = e->c_buf1;
        send(d, GLB_TPLG | TPLG_BUF_FREE, 3);
    }
    msg[2] = ppl;
    send(d, GLB_TPLG | TPLG_PIPE_FREE, 3);
    return -1;
}

int ipc3_trigger(struct dspep *e, int start)
{
    msg[2] = e->c_host;
    return send(e->d, GLB_STREAM | (start ? STR_START : STR_STOP), 3);
}

void ipc3_ep_stop(struct dspep *e)
{
    struct dsp *d = e->d;
    if (!e->prepared) return;
    msg[2] = e->c_host;
    send(d, GLB_STREAM | STR_PCM_FREE, 3);
    dai_config(d, e, 2);                            /* HW_FREE */
    msg[2] = e->c_host;
    send(d, GLB_TPLG | TPLG_COMP_FREE, 3);
    msg[2] = e->c_dai;
    send(d, GLB_TPLG | TPLG_COMP_FREE, 3);
    msg[2] = e->c_buf1;
    send(d, GLB_TPLG | TPLG_BUF_FREE, 3);
    msg[2] = e->c_pipe;
    send(d, GLB_TPLG | TPLG_PIPE_FREE, 3);
    e->prepared = 0;
}
