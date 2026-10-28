/* sbemu.c - Sound Blaster 16 emulation for MS-DOS programs.
   The DSP (version 4.05) takes its sound from the emulated DMA channels and plays it as a
   stream of the sound engine; the OPL3 synthesizer (opl3.c) is a second stream; MPU-401 UART
   bytes go to the built-in synthesizer. The interrupt and the DMA counters follow the time
   the sound device actually played, so a program sees its blocks finish at the right rate.
   The platform delivers the port accesses (VxD I/O handlers, DOS QPI port traps), the DMA
   channel state (VxD: VDMAD) and the interrupt. */
#include "audio.h"
#include "opl3.h"
#include "sbemu.h"

#define SBRING      65536       /* DSP stream: 16-bit stereo, 371 ms at 44.1 kHz */
#define OPLRING     32768       /* FM stream: 16-bit stereo, 165 ms */
#define IDLE_MS     2000        /* an unused stream stops after this */
#define OPLQ        512
#define DQ          512

static struct { struct winring h; u8 d[SBRING]; } sbr;
static struct { struct winring h; u8 d[OPLRING]; } oplr;
static int sb_sid = -1, opl_sid = -1;
static int sb_run, opl_run;
static u32 sb_last, opl_last;       /* plat_ms() of the last use */
static struct winsb *sbc;
static int on, vdma_ext;

/* ---- DSP ---- */
static u8 cmd, nargs, need, args[4];
static u8 oq[64];
static int oqh, oqt;
static u8 rst_latch, highspeed, uart, speaker = 1, testreg;
static u8 tconst = 0xD2;
static u32 orate = 22050;
static u32 blksize = 0x800;
static u8 istat;                    /* bit 0 8-bit, bit 1 16-bit, bit 2 MPU interrupt */
static u8 irqlvl;

struct xfer {
    u8 active, autoinit, bits16, stereo, sign, input, silence, pause, exitai;
    u8 adpcm;                       /* bits per sample (2, 3, 4), 0 PCM */
    u8 dma;                         /* 0 8-bit channel, 1 16-bit channel */
    u32 rate;                       /* frames per second */
    u32 blklen, blkpos;             /* bytes */
    u32 frac;
};
static struct xfer x;
static u8 adsub;                    /* ADPCM: samples left in the current byte */

/* direct DAC (command 10h) */
static u8 dq[DQ];
static int dqn;
static u32 direct_ms;

/* ---- mixer ---- */
static u8 mix[256];
static const u32 gain5[32] = {
    0, 66, 83, 104, 131, 165, 207, 261, 328, 414, 521, 655, 825, 1039, 1308, 1646, 2072,
    2609, 3285, 4135, 5206, 6554, 8250, 10387, 13076, 16462, 20724, 26090, 32846, 41350,
    52057, 65536
};

/* ---- DMA channels: 0 the 8-bit one, 1 the 16-bit one ---- */
struct sbdma {
    u32 lin, len, pos;              /* bytes */
    u8 autoinit, masked, tc, valid;
    u8 rtc;                         /* VxD: state last reported */
    u32 rpos;
    /* DOS: 8237 registers */
    u16 baddr, bcnt;
    u8 page, mode;
};
static struct sbdma dma[2];
static u8 dmaff[2];                 /* flip-flops of the two controllers */
static u8 dmatc[2];                 /* status: terminal count seen, by controller */

/* ---- FM ---- */
static struct { u32 us; u16 reg; u8 val; u8 rsv; } oplq[OPLQ];
static int oplqn;
static u32 opl_us, opl_frac;
static u8 oplidx[2];
static u8 t1, t2, tctl, tstat;
static u32 t1us, t2us;

/* ---- MPU-401 ---- */
static u8 mpu_uart, mpu_ack;

struct winsb *sb_cfg(void)
{
    return sbc;
}

static void mirror(void)
{
    if (!sbc) return;
    sbc->rdstat = (u8)(oqh != oqt ? 0xFF : 0x7F);
    sbc->mpustat = (u8)(mpu_ack ? 0x00 : 0x80);
    sbc->irqline = irqlvl;
}

static void irq_update(void)
{
    u8 l = (u8)(istat != 0);
    if (l == irqlvl) return;
    irqlvl = l;
    if (l && sbc) sbc->irqs++;
    plat_sb_irq(l);
    mirror();
}

static void raise(u8 bit)
{
    istat |= bit;
    irq_update();
}

static void ack(u8 bit)
{
    istat &= (u8)~bit;
    irq_update();
}

static void oput(u8 v)
{
    int n = (oqh + 1) % (int)sizeof(oq);
    if (n == oqt) return;
    oq[oqh] = v;
    oqh = n;
    mirror();
}

static u8 oget(void)
{
    u8 v;
    if (oqh == oqt) return 0xFF;
    v = oq[oqt];
    oqt = (oqt + 1) % (int)sizeof(oq);
    mirror();
    return v;
}

/* ---------------------------------------------------------------- mixer */

static void mix_reset(void)
{
    int i;
    memset(mix, 0, sizeof(mix));
    /* full level: most programs never set the mixer of a Sound Blaster 16 */
    for (i = 0x30; i <= 0x35; i++) mix[i] = 0xF8;
    mix[0x36] = mix[0x37] = mix[0x38] = mix[0x39] = 0;
    mix[0x3B] = 0x80;
    mix[0x3C] = 0x1F;
    mix[0x3D] = 0x15;
    mix[0x3E] = 0x0B;
    mix[0x3F] = mix[0x40] = mix[0x41] = mix[0x42] = 0;
    mix[0x44] = mix[0x45] = mix[0x46] = mix[0x47] = 0x80;
}

static u8 nib2(u8 a, u8 b)
{
    return (u8)((a & 0xF0) | (b >> 4));
}

static u8 mix_read(u8 i)
{
    if (!sbc) return 0;
    switch (i) {
    case 0x04: return nib2(mix[0x32], mix[0x33]);
    case 0x0A: return (u8)(mix[0x3A] >> 5);
    case 0x22: return nib2(mix[0x30], mix[0x31]);
    case 0x26: return nib2(mix[0x34], mix[0x35]);
    case 0x28: return nib2(mix[0x36], mix[0x37]);
    case 0x2E: return nib2(mix[0x38], mix[0x39]);
    case 0x80:
        switch (sbc->irq) {
        case 2: case 9: return 1;
        case 5: return 2;
        case 7: return 4;
        default: return 8;
        }
    case 0x81:
        return (u8)((sbc->dma8 == 0 ? 1 : sbc->dma8 == 1 ? 2 : 8) |
                    (sbc->dma16 >= 5 ? 1 << sbc->dma16 : 0));
    case 0x82: return (u8)(istat | 0x10);   /* bit 4: version of the SB16 */
    }
    return mix[i];
}

static void mix_pair(u8 l, u8 v)
{
    mix[l] = (u8)((v & 0xF0) | 0x08);
    mix[l + 1] = (u8)(((v & 0x0F) << 4) | 0x08);
}

static void mix_write(u8 i, u8 v)
{
    switch (i) {
    case 0x00: mix_reset(); return;
    case 0x04: mix_pair(0x32, v); return;
    case 0x0A: mix[0x3A] = (u8)((v & 7) << 5); return;
    case 0x22: mix_pair(0x30, v); return;
    case 0x26: mix_pair(0x34, v); return;
    case 0x28: mix_pair(0x36, v); return;
    case 0x2E: mix_pair(0x38, v); return;
    case 0x80: case 0x81: case 0x82: return;
    }
    mix[i] = v;
}

static u32 g2(u8 a, u8 b)
{
    return (u32)(((u64)gain5[a >> 3] * gain5[b >> 3]) >> 16);
}

/* ---------------------------------------------------------------- streams */

static void ring_init(struct winring *r, u32 size)
{
    memset(r, 0, 32);
    r->magic = WINRING_MAGIC;
    r->size = size;
}

static void sb_stream(int run)
{
    if (sb_sid < 0) {
        ring_init(&sbr.h, SBRING);
        sb_sid = aeng_rt_open(&sbr.h, x.rate ? x.rate : orate, MX_WAVE);
        if (sb_sid < 0) return;
    }
    if (run) sb_last = plat_ms();
    if (run == sb_run) return;
    sb_run = run;
    if (run) sbr.h.rd = sbr.h.wr = sbr.h.pos = 0;
    aeng_rt_run(sb_sid, run);
}

static void opl_stream(int run)
{
    if (opl_sid < 0) {
        ring_init(&oplr.h, OPLRING);
        opl_sid = aeng_rt_open(&oplr.h, OPL_RATE, MX_SYNTH);
        if (opl_sid < 0) return;
    }
    if (run) opl_last = plat_ms();
    if (run == opl_run) return;
    opl_run = run;
    if (run) {
        oplr.h.rd = oplr.h.wr = oplr.h.pos = 0;
        opl_us = plat_us();
        opl_frac = 0;
    }
    aeng_rt_run(opl_sid, run);
}

int sb_active(void)
{
    return on && (sb_run || opl_run);
}

/* ---------------------------------------------------------------- DSP */

static void xfer_stop(void)
{
    x.active = 0;
}

static void dsp_reset(void)
{
    xfer_stop();
    highspeed = 0;
    uart = 0;
    oqh = oqt = 0;
    nargs = need = 0;
    cmd = 0;
    istat = 0;
    irq_update();
    oput(0xAA);
}

static void xfer_start(int bits16, int autoinit, int input, u32 len, int stereo, int sign)
{
    memset(&x, 0, sizeof(x));
    x.bits16 = (u8)bits16;
    x.autoinit = (u8)autoinit;
    x.input = (u8)input;
    x.stereo = (u8)stereo;
    x.sign = (u8)sign;
    x.dma = (u8)(bits16 && sbc->dma16 >= 4 ? 1 : 0);
    x.blklen = len;
    x.rate = orate;
    if (x.rate < 4000) x.rate = 4000;
    if (x.rate > 48000) x.rate = 48000;
    x.active = 1;
    sb_stream(1);
    aeng_rt_rate(sb_sid, x.rate);
}

/* 8-bit commands of the older DSPs: rate from the time constant, SB Pro stereo bit */
static void legacy8(int autoinit, int input, u32 len)
{
    int st = (mix[0x0E] & 2) != 0;
    orate = 1000000UL / (256 - tconst);
    if (st) orate /= 2;
    xfer_start(0, autoinit, input, len, st, 0);
}

static int cmd_args(u8 c)
{
    switch (c) {
    case 0x10: case 0x38: case 0x40: case 0xE0: case 0xE2: case 0xE4: case 0xF9:
    case 0x0F: case 0x04:
        return 1;
    case 0x14: case 0x15: case 0x16: case 0x17: case 0x24: case 0x41: case 0x42: case 0x48:
    case 0x74: case 0x75: case 0x76: case 0x77: case 0x80: case 0x05: case 0x0E:
        return 2;
    }
    if (c >= 0xB0 && c <= 0xCF) return 3;
    return 0;
}

static void adpcm(int bits, int autoinit, u32 len)
{
    orate = 1000000UL / (256 - tconst);
    xfer_start(0, autoinit, 0, len, 0, 0);
    x.adpcm = (u8)bits;
    adsub = 0;
}

static void dsp_exec(void)
{
    u32 len = (u32)args[0] | ((u32)args[1] << 8);
    switch (cmd) {
    case 0x10:
        if (dqn < DQ) dq[dqn++] = args[0];
        direct_ms = plat_ms();
        sb_stream(1);
        break;
    case 0x14: case 0x15: legacy8(0, 0, len + 1); break;
    case 0x1C: legacy8(1, 0, blksize); break;
    case 0x24: legacy8(0, 1, len + 1); break;
    case 0x2C: legacy8(1, 1, blksize); break;
    case 0x90: legacy8(1, 0, blksize); highspeed = 1; break;
    case 0x91: legacy8(0, 0, blksize); highspeed = 1; break;
    case 0x98: legacy8(1, 1, blksize); highspeed = 1; break;
    case 0x99: legacy8(0, 1, blksize); highspeed = 1; break;
    case 0x16: case 0x17: adpcm(2, 0, len + 1); break;
    case 0x1F: adpcm(2, 1, blksize); break;
    case 0x74: case 0x75: adpcm(4, 0, len + 1); break;
    case 0x7D: adpcm(4, 1, blksize); break;
    case 0x76: case 0x77: adpcm(3, 0, len + 1); break;
    case 0x7F: adpcm(3, 1, blksize); break;
    case 0x80:
        orate = 1000000UL / (256 - tconst);
        xfer_start(0, 0, 0, len + 1, 0, 0);
        x.silence = 1;
        break;
    case 0x20: oput(0x80); break;
    case 0x30: case 0x31: break;
    case 0x34: case 0x35: case 0x36: case 0x37: uart = 1; break;
    case 0x38: aeng_midi(args, 1); break;
    case 0x40: tconst = args[0]; break;
    case 0x41: case 0x42: orate = ((u32)args[0] << 8) | args[1]; break;
    case 0x45: case 0x47: if (x.active) x.autoinit = 1; break;
    case 0x48: blksize = len + 1; break;
    case 0xD0: case 0xD5: x.pause = 1; break;
    case 0xD4: case 0xD6: x.pause = 0; break;
    case 0xD1: speaker = 1; break;
    case 0xD3: speaker = 0; break;
    case 0xD8: oput(speaker ? 0xFF : 0x00); break;
    case 0xD9: case 0xDA: if (x.active) x.exitai = 1; break;
    case 0xE0: oput((u8)~args[0]); break;
    case 0xE1: oput(4); oput(5); break;
    case 0xE3: {
        static const char cr[] = "COPYRIGHT (C) CREATIVE TECHNOLOGY LTD, 1992.";
        const char *s = cr;
        while (*s) oput((u8)*s++);
        oput(0);
        break;
    }
    case 0xE4: testreg = args[0]; break;
    case 0xE8: oput(testreg); break;
    case 0xF2: raise(1); break;
    case 0xF3: raise(2); break;
    case 0xF8: oput(0); break;
    case 0xF9: oput(0); break;
    case 0x0F: oput(0); break;
    default:
        if (cmd >= 0xB0 && cmd <= 0xCF) {
            /* SB16: Bxh 16-bit / Cxh 8-bit; bit 3 input, bit 2 auto-init; mode bit 4 signed,
               bit 5 stereo; length in samples - 1 */
            u32 n = ((u32)args[1] | ((u32)args[2] << 8)) + 1;
            int b16 = cmd < 0xC0;
            if (cmd & 1) break;
            xfer_start(b16, (cmd >> 2) & 1, (cmd >> 3) & 1, b16 ? n * 2 : n,
                       (args[0] >> 5) & 1, (args[0] >> 4) & 1);
        }
        break;
    }
}

static void dsp_write(u8 v)
{
    if (uart) {
        aeng_midi(&v, 1);
        return;
    }
    if (highspeed) return;
    if (need) {
        args[nargs++] = v;
        if (nargs < need) return;
        need = 0;
        dsp_exec();
        return;
    }
    cmd = v;
    nargs = 0;
    need = (u8)cmd_args(v);
    if (!need) dsp_exec();
}

/* ---------------------------------------------------------------- DMA (DOS: 8237 registers) */

static int dma_index(int ch)
{
    if (!sbc) return -1;
    if (ch == sbc->dma8) return 0;
    if (sbc->dma16 >= 4 && ch == sbc->dma16) return 1;
    return -1;
}

static int dma_ch(int i)
{
    return i ? sbc->dma16 : sbc->dma8;
}

/* emulated channel i exists and is on controller ctl (0: channels 0-3, 1: 4-7) */
static int on_ctl(int i, int ctl)
{
    int c = dma_ch(i);
    if (i && c < 4) return 0;
    return (c >= 4) == ctl;
}

static void dma_calc(int i)
{
    struct sbdma *d = &dma[i];
    if (i == 0) {
        d->lin = ((u32)d->page << 16) | d->baddr;
        d->len = (u32)d->bcnt + 1;
    } else {
        d->lin = ((u32)(d->page & 0xFE) << 16) | ((u32)d->baddr << 1);
        d->len = ((u32)d->bcnt + 1) * 2;
    }
    d->autoinit = (u8)((d->mode >> 4) & 1);
    d->valid = 1;
}

/* current address / count as the program reads them */
static u16 dma_cur(int i, int count)
{
    struct sbdma *d = &dma[i];
    u32 u = i ? d->pos / 2 : d->pos;
    if (count) return (u16)(d->tc && !d->autoinit ? 0xFFFF : d->bcnt - u);
    return (u16)(d->baddr + u);
}

static const u8 page_ch[16] = {   /* page register 80h + n -> channel, 0xFF none */
    0xFF, 2, 3, 1, 0xFF, 0xFF, 0xFF, 0, 0xFF, 6, 7, 5, 0xFF, 0xFF, 0xFF, 4
};

/* Windows: the status register bits of the emulated channels (terminal count, read once;
   request while the card transfers), for the VxD's trap of ports 08h / D0h */
u8 sb_dma_status(int ctl)
{
    u8 st = dmatc[ctl];
    int i;
    for (i = 0; i < 2; i++)
        if (on_ctl(i, ctl) && x.active && x.dma == i && !dma[i].masked) st |= (u8)(0x10 << (dma_ch(i) & 3));
    dmatc[ctl] = 0;
    return st;
}

static u32 dma_io(u32 port, u32 val, int out)
{
    int ctl, ch, i, reg;
    if (port >= 0x80 && port <= 0x8F) {
        ch = page_ch[port & 15];
        i = ch == 0xFF ? -1 : dma_index(ch);
        if (i < 0) return SBIO_FWD;
        if (!out) return dma[i].page;
        dma[i].page = (u8)val;
        dma_calc(i);
        return 0;
    }
    if (port < 0x10) {
        ctl = 0;
        reg = (int)port;
    } else if (port >= 0xC0 && port <= 0xDF) {
        ctl = 1;
        if (port & 1) return SBIO_FWD;
        reg = (int)((port - 0xC0) >> 1);
    } else {
        return SBIO_FWD;
    }
    if (reg < 8) {                                  /* address / count of one channel */
        ch = (reg >> 1) + ctl * 4;
        i = dma_index(ch);
        if (i < 0) {
            dmaff[ctl] ^= 1;
            return SBIO_FWD;
        }
        if (out) {
            u16 *r = (reg & 1) ? &dma[i].bcnt : &dma[i].baddr;
            if (!dmaff[ctl]) *r = (u16)((*r & 0xFF00) | (val & 0xFF));
            else *r = (u16)((*r & 0x00FF) | ((val & 0xFF) << 8));
            dmaff[ctl] ^= 1;
            dma[i].pos = 0;
            dma[i].tc = 0;
            dma_calc(i);
            return 0;
        } else {
            u16 v = dma_cur(i, reg & 1);
            u8 b = (u8)(dmaff[ctl] ? v >> 8 : v);
            dmaff[ctl] ^= 1;
            return b;
        }
    }
    switch (reg) {
    case 8:                                         /* status */
        if (!out) {
            u8 st = dmatc[ctl];
            for (i = 0; i < 2; i++)
                if (on_ctl(i, ctl) && x.active && x.dma == i && !dma[i].masked)
                    st |= (u8)(0x10 << (dma_ch(i) & 3));
            dmatc[ctl] = 0;
            return SBIO_FWD | st;
        }
        return SBIO_FWD;
    case 10:                                        /* single mask */
        if (out) {
            i = dma_index((int)(val & 3) + ctl * 4);
            if (i >= 0) {
                dma[i].masked = (u8)((val >> 2) & 1);
                return 0;
            }
        }
        return SBIO_FWD;
    case 11:                                        /* mode */
        if (out) {
            i = dma_index((int)(val & 3) + ctl * 4);
            if (i >= 0) {
                dma[i].mode = (u8)val;
                dma_calc(i);
                return 0;
            }
        }
        return SBIO_FWD;
    case 12:                                        /* clear flip-flop */
        if (out) dmaff[ctl] = 0;
        return SBIO_FWD;
    case 13:                                        /* master clear */
        if (out) {
            dmaff[ctl] = 0;
            for (i = 0; i < 2; i++)
                if (on_ctl(i, ctl)) dma[i].masked = 1;
        }
        return SBIO_FWD;
    case 14:                                        /* clear all masks */
    case 15:                                        /* write all masks */
        if (out) {
            for (i = 0; i < 2; i++)
                if (on_ctl(i, ctl)) dma[i].masked = (u8)(reg == 14 ? 0 : (val >> (dma_ch(i) & 3)) & 1);
        }
        return SBIO_FWD;
    }
    return SBIO_FWD;
}

/* VxD: VDMAD hands over the channel state the program set */
void sb_vdma(int which, u32 lin, u32 len, int autoinit, int masked)
{
    struct sbdma *d = &dma[which & 1];
    if (d->lin != lin || d->len != len || (!masked && d->masked && d->tc)) {
        d->pos = 0;
        d->tc = 0;
    }
    d->lin = lin;
    d->len = len;
    d->autoinit = (u8)autoinit;
    d->masked = (u8)masked;
    d->valid = len != 0;
}

/* ---------------------------------------------------------------- FM ports */

static u8 opl_status(void)
{
    u32 now = plat_us();
    if ((tctl & 1) && !(tctl & 0x40) && now - t1us >= (u32)(256 - t1) * 80) tstat |= 0xC0;
    if ((tctl & 2) && !(tctl & 0x20) && now - t2us >= (u32)(256 - t2) * 320) tstat |= 0xA0;
    return tstat;
}

static void opl_data(int bank, u8 v)
{
    int reg = oplidx[bank] | (bank << 8);
    if (sbc->flags & SBF_NOOPL) return;
    if (!bank) {
        switch (oplidx[0]) {
        case 2: t1 = v; break;
        case 3: t2 = v; break;
        case 4:
            if (v & 0x80) {
                tstat = 0;
                return;
            }
            if ((v & 1) && !(tctl & 1)) t1us = plat_us();
            if ((v & 2) && !(tctl & 2)) t2us = plat_us();
            tctl = v;
            tstat &= (u8)~(v & 0x60);
            if (!(tstat & 0x60)) tstat = 0;
            return;
        }
    }
    opl_stream(1);
    if (oplqn < OPLQ) {
        oplq[oplqn].us = plat_us();
        oplq[oplqn].reg = (u16)reg;
        oplq[oplqn].val = v;
        oplqn++;
    } else {
        opl_write(reg, v);
    }
}

/* ---------------------------------------------------------------- port access */

u32 sb_io(u32 port, u32 val, int out)
{
    u32 b;
    u8 v = (u8)val;
    if (!on) return out ? 0 : 0xFF;
    if (port < 0x100) return dma_io(port, val, out);
    if (port >= 0xF000) {           /* DOS: FM register write F0rrh (bank 0) / F1rrh (bank 1) */
        oplidx[(port >> 8) & 1] = (u8)port;
        opl_data((int)((port >> 8) & 1), v);
        return 0;
    }
    b = port - sbc->base;
    if (sbc->mpu && (port == sbc->mpu || port == sbc->mpu + 1u) && !(sbc->flags & SBF_NOMPU)) {
        if (port == sbc->mpu) {
            if (out) {
                if (mpu_uart) aeng_midi(&v, 1);
                return 0;
            }
            if (mpu_ack) {
                mpu_ack = 0;
                mirror();
                return 0xFE;
            }
            return 0xFF;
        }
        if (!out) return mpu_ack ? 0x00 : 0x80;
        if (v == 0xFF) {
            if (!mpu_uart) mpu_ack = 1;
            mpu_uart = 0;
        } else {
            if (v == 0x3F) mpu_uart = 1;
            mpu_ack = 1;
        }
        mirror();
        return 0;
    }
    if (port >= 0x388 && port <= 0x38B) b = (port & 2) ? 2 + (port & 1) : 8 + (port & 1);
    else if (b >= 16) return out ? 0 : 0xFF;
    switch (b) {
    case 0x0: case 0x2: case 0x8:
        if (out) {
            oplidx[b == 2] = v;
            return 0;
        }
        return (sbc->flags & SBF_NOOPL) ? 0xFF : opl_status();
    case 0x1: case 0x3: case 0x9:
        if (out) opl_data(b == 3, v);
        return 0xFF;
    case 0x4:
        if (out) mix[0xFF] = v;     /* index */
        return mix[0xFF];
    case 0x5:
        if (out) {
            mix_write(mix[0xFF], v);
            return 0;
        }
        return mix_read(mix[0xFF]);
    case 0x6:
        if (out) {
            if (v & 1) {
                rst_latch = 1;
                if (highspeed) {
                    highspeed = 0;
                    xfer_stop();
                    rst_latch = 0;
                }
            } else if (rst_latch) {
                rst_latch = 0;
                dsp_reset();
            }
        }
        return 0xFF;
    case 0xA:
        if (out) return 0;
        return oget();
    case 0xC:
        if (out) {
            dsp_write(v);
            return 0;
        }
        return 0x7F;
    case 0xE:
        if (out) return 0;
        ack(1);
        return oqh != oqt ? 0xFF : 0x7F;
    case 0xF:
        if (out) return 0;
        ack(2);
        return 0xFF;
    }
    return out ? 0 : 0xFF;
}

/* ---------------------------------------------------------------- production */

static s16 cv8(u8 b, int sign)
{
    return (s16)(sign ? (s16)(s8)b << 8 : ((s16)b - 128) << 8);
}

/* one frame from the DMA buffer; 0 when the channel cannot deliver */
static int fetch(u8 *mem, s16 *l, s16 *r)
{
    struct sbdma *d = &dma[x.dma];
    int nb = (x.bits16 ? 2 : 1) * (x.stereo ? 2 : 1), k;
    u8 bt[4];
    if (x.silence) {
        *l = *r = 0;
        return 1;
    }
    if (!d->valid || d->masked || !mem || (d->tc && !d->autoinit) || !d->len) return 0;
    if (x.adpcm) {
        /* Creative ADPCM: 2, 3 or 4 samples a byte; played as silence */
        if (adsub) {
            adsub--;
            *l = *r = 0;
            return 1;
        }
        adsub = (u8)(x.adpcm == 4 ? 1 : x.adpcm == 3 ? 2 : 3);
        nb = 1;
    }
    for (k = 0; k < nb; k++) {
        if (x.input) mem[d->pos] = (u8)(x.bits16 || x.sign ? 0 : 0x80);
        bt[k] = mem[d->pos];
        if (++d->pos >= d->len) {
            d->pos = 0;
            d->tc = 1;
            dmatc[dma_ch(x.dma) >= 4] |= (u8)(1 << (dma_ch(x.dma) & 3));
            if (!d->autoinit) {
                d->pos = d->len;
                d->masked = 1;
                k++;
                break;
            }
        }
    }
    x.blkpos += (u32)k;
    if (x.input || x.adpcm) {
        *l = *r = 0;
    } else if (x.bits16) {
        s16 a = (s16)(bt[0] | (bt[1] << 8));
        if (!x.sign) a = (s16)(a ^ 0x8000);
        *l = a;
        *r = x.stereo ? (s16)((bt[2] | (bt[3] << 8)) ^ (x.sign ? 0 : 0x8000)) : a;
    } else {
        *l = cv8(bt[0], x.sign);
        *r = x.stereo ? cv8(bt[1], x.sign) : *l;
    }
    return 1;
}

static void block_check(void)
{
    if (x.blkpos < x.blklen) return;
    x.blkpos = 0;
    raise(x.bits16 ? 2 : 1);
    if (!x.autoinit || x.exitai) {
        xfer_stop();
        highspeed = 0;
    }
}

static void ring_put(struct winring *r, const s16 *f, u32 n)
{
    u8 *d = (u8 *)r + 32;
    u32 i;
    if (r->size - (r->wr - r->rd) < n * 4) return;
    for (i = 0; i < n; i++) {
        u32 off = r->wr % r->size;
        *(s16 *)(d + off) = f[i * 2];
        *(s16 *)(d + off + 2) = f[i * 2 + 1];
        r->wr += 4;
    }
}

static void sb_frames(u32 n)
{
    s16 buf[256 * 2];
    u8 *mem = NULL;
    u32 done = 0;
    struct sbdma *d = &dma[x.dma];
    if (x.active && d->valid) mem = plat_sb_mem(d->lin, d->len);
    while (done < n) {
        u32 k = MIN(n - done, 256), i;
        if (dqn && !x.active) {
            /* direct DAC: the bytes of this period spread over it */
            for (i = 0; i < k; i++) {
                u32 j = (u32)(((u64)(done + i) * (u32)dqn) / n);
                buf[i * 2] = buf[i * 2 + 1] = cv8(dq[j], 0);
            }
        } else {
            for (i = 0; i < k; i++) {
                s16 l = 0, r = 0;
                if (x.active && !x.pause) {
                    if (fetch(mem, &l, &r)) block_check();
                }
                buf[i * 2] = l;
                buf[i * 2 + 1] = r;
            }
        }
        ring_put(&sbr.h, buf, k);
        done += k;
    }
    dqn = 0;
    if (vdma_ext && d->valid && (d->pos != d->rpos || d->tc != d->rtc)) {
        d->rpos = d->pos;
        d->rtc = d->tc;
        plat_sb_dma(x.dma, d->len - d->pos, d->tc);
    }
}

static void opl_frames(u32 n)
{
    s16 buf[256 * 2];
    u32 now = plat_us(), span = now - opl_us, done = 0;
    int e = 0;
    while (done < n) {
        u32 lim = n;
        if (e < oplqn) {
            u32 t = oplq[e].us - opl_us;
            if ((s32)t < 0) t = 0;
            lim = span ? (u32)(((u64)t * n) / span) : 0;
            if (lim > n) lim = n;
            if (lim <= done) {
                opl_write(oplq[e].reg, oplq[e].val);
                e++;
                continue;
            }
        }
        while (done < lim) {
            u32 k = MIN(lim - done, 256);
            opl_render(buf, (int)k);
            ring_put(&oplr.h, buf, k);
            done += k;
        }
    }
    for (; e < oplqn; e++) opl_write(oplq[e].reg, oplq[e].val);
    oplqn = 0;
    opl_us = now;
}

void sb_produce(u32 frames, u32 rate)
{
    u32 n;
    if (!on || !rate) return;
    if (sb_run) {
        u64 t = (u64)frames * (x.rate ? x.rate : orate) + x.frac;
        n = (u32)(t / rate);
        x.frac = (u32)(t % rate);
        aeng_rt_vol(sb_sid, g2(mix[0x30], mix[0x32]), g2(mix[0x31], mix[0x33]));
        if (n) sb_frames(n);
        if (x.active || dqn || plat_ms() - direct_ms < 200) sb_last = plat_ms();
        else if (plat_ms() - sb_last > IDLE_MS) sb_stream(0);
    }
    if (opl_run) {
        u64 t = (u64)frames * OPL_RATE + opl_frac;
        n = (u32)(t / rate);
        opl_frac = (u32)(t % rate);
        aeng_rt_vol(opl_sid, g2(mix[0x30], mix[0x34]), g2(mix[0x31], mix[0x35]));
        if (n) opl_frames(n);
        if (opl_busy() || oplqn) opl_last = plat_ms();
        else if (plat_ms() - opl_last > IDLE_MS) opl_stream(0);
    }
}

/* ---------------------------------------------------------------- setup */

void sb_reset_owner(void)
{
    int i;
    if (!on) return;
    dsp_reset();
    oqh = oqt = 0;
    mpu_uart = mpu_ack = 0;
    opl_reset();
    oplqn = 0;
    tctl = tstat = 0;
    mix_reset();
    for (i = 0; i < 2; i++) {
        memset(&dma[i], 0, sizeof(dma[i]));
        dma[i].masked = 1;
    }
    mirror();
}

static u32 num(const char **ps, int hex)
{
    const char *s = *ps;
    u32 v = 0;
    for (;;) {
        char c = *s;
        if (c >= '0' && c <= '9') v = v * (hex ? 16 : 10) + (u32)(c - '0');
        else if (hex && c >= 'A' && c <= 'F') v = v * 16 + (u32)(c - 'A' + 10);
        else if (hex && c >= 'a' && c <= 'f') v = v * 16 + (u32)(c - 'a' + 10);
        else break;
        s++;
    }
    *ps = s;
    return v;
}

/* A220 I5 D1 H5 P330, like the BLASTER variable (any separators) */
static void parse_spec(const char *s)
{
    const char *e = s + sizeof(sbc->spec);
    while (s < e && *s) {
        char c = *s++;
        u32 v;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c != 'A' && c != 'I' && c != 'D' && c != 'H' && c != 'P') continue;
        v = num(&s, c == 'A' || c == 'P');
        switch (c) {
        case 'A': if (v >= 0x210 && v <= 0x280 && !(v & 15)) sbc->base = (u16)v; break;
        case 'I': if (v == 2 || v == 5 || v == 7 || v == 9 || v == 10) sbc->irq = (u8)(v == 9 ? 2 : v); break;
        case 'D': if (v == 0 || v == 1 || v == 3) sbc->dma8 = (u8)v; break;
        case 'H': if (v == 0 || (v >= 5 && v <= 7)) sbc->dma16 = (u8)v; break;
        case 'P': if (!v || (v >= 0x300 && v <= 0x330 && !(v & 15))) sbc->mpu = (u16)v; break;
        }
    }
}

int sb_start(int vdma)
{
    int i;
    sbc = (struct winsb *)((u8 *)cb + WIN_SBOFF);
    if (sbc->flags & SBF_OFF) return -1;
    if (!sbc->base) {
        sbc->base = 0x220;
        sbc->irq = 5;
        sbc->dma8 = 1;
        sbc->dma16 = 5;
        sbc->mpu = 0x330;
        if (sbc->spec[0]) parse_spec(sbc->spec);
    }
    if ((sbc->flags & SBF_NOMPU)) sbc->mpu = 0;
    vdma_ext = vdma;
    memset(&x, 0, sizeof(x));
    for (i = 0; i < 2; i++) {
        memset(&dma[i], 0, sizeof(dma[i]));
        dma[i].masked = 1;
    }
    dmaff[0] = dmaff[1] = dmatc[0] = dmatc[1] = 0;
    istat = irqlvl = 0;
    oqh = oqt = 0;
    mix_reset();
    opl_reset();
    oplqn = 0;
    sb_sid = opl_sid = -1;
    sb_run = opl_run = 0;
    on = 1;
    sbc->flags |= SBF_ON;
    mirror();
    if (plat_sb_start()) {
        on = 0;
        sbc->flags &= (u8)~SBF_ON;
        return -1;
    }
    kprintf("sb: Sound Blaster 16 emulation A%x I%d D%d H%d P%x\n", sbc->base, sbc->irq, sbc->dma8,
            sbc->dma16, sbc->mpu);
    return 0;
}

void sb_stop(void)
{
    if (!on) return;
    on = 0;
    if (sb_sid >= 0) aeng_rt_close(sb_sid);
    if (opl_sid >= 0) aeng_rt_close(opl_sid);
    sb_sid = opl_sid = -1;
    sb_run = opl_run = 0;
    if (sbc) sbc->flags &= (u8)~SBF_ON;
}
