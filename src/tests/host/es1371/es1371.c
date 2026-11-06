/* es1371.c - host test of es137x.c with an emulated Ensoniq ES1371 / ES1373 / CT5880.

   QEMU has no ES1371. This program emulates the parts of the chip the driver uses, as the
   Linux driver (sound/pci/ens1370.c) programs them: the CONTROL / STATUS / SERIAL registers,
   the memory pages of the DAC2 and ADC frames, the sample rate converter RAM behind
   ES_REG_1371_SMPRATE (with the busy / state bits the driver waits on), the AC'97 codec
   register interface (with the rule that the codec may be touched only in the converter's
   safe state 0x00010000), and DAC2 / ADC bus mastering in loop mode with the frame counter.
   The sample rates come from the converter RAM the driver wrote (DAC2: frequency word, ADC:
   the TRUNC_N divider and the frequency word), so a wrong rate shows as a wrong pitch.

   The core's sound engine (aeng.c) runs on top: a 440 / 880 Hz stream is played through
   the emulated DAC2, a 1 kHz tone the emulated ADC records is captured. Output: dac2.raw
   (16-bit stereo at the DAC2 rate), rec.raw (the captured stream), the checks on stdout. */
#include "audio.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>

struct wincb cbs, *cb = &cbs;
u32 plat_mode = PM_VXD;

/* ---------------------------------------------------------------- platform */

static double now_ms;
u32 plat_ms(void) { now_ms += 0.001; return (u32)now_ms; }
void mdelay(u32 ms) { now_ms += ms; }
void udelay(u32 us) { now_ms += us / 1000.0; }
void plat_udelay(u32 us) { udelay(us); }
void kprintf(const char *f, ...) { va_list a; va_start(a, f); vprintf(f, a); va_end(a); }
int ksnprintf(char *o, u32 n, const char *f, ...) { int r; va_list a; va_start(a, f); r = vsnprintf(o, n, f, a); va_end(a); return r; }
void strlcpy(char *d, const char *s, u32 n) { u32 i; for (i = 0; i + 1 < n && s[i]; i++) d[i] = s[i]; d[i] = 0; }
u32 irq_save(void) { return 0; }
void plat_vol_saved(const void *recs, u32 len) { (void)recs; (void)len; }
struct winbridge *plat_bridge_peer(void) { return NULL; }
void *plat_bridge_ring(struct winbridge *p) { (void)p; return NULL; }
void plat_bridge_publish(struct winbridge *own) { (void)own; }
u32 crc32(u32 crc, const void *p, u32 n) { (void)p; (void)n; return crc; }
void irq_restore(u32 f) { (void)f; }

static u8 *rings[16];
void *plat_sysrm(u32 so) { return so < 16 ? rings[so] : NULL; }
void *plat_rm(u32 so) { return plat_sysrm(so); }

#define POOLSIZE (4UL << 20)
#define POOLPHYS 0x00400000UL
static u8 *pool;
static u32 pool_used;
void *kmalloc_align(u32 size, u32 align)
{
    u32 o = (pool_used + align - 1) & ~(align - 1);
    if (o + size > POOLSIZE) return NULL;
    pool_used = o + size;
    return pool + o;
}
void *kmalloc(u32 size) { return kmalloc_align(size, 16); }
void kfree(void *p) { (void)p; }
u32 vphys(const void *p) { return (u32)((const u8 *)p - pool) + POOLPHYS; }
static u8 *host(u32 phys) { return phys >= POOLPHYS && phys < POOLPHYS + POOLSIZE ? pool + (phys - POOLPHYS) : NULL; }

#define IOBASE 0xE000
u32 pci_bar_io(struct pcidev *p, int bar) { (void)p; return bar ? 0 : IOBASE; }
void pci_enable(struct pcidev *p, int mem, int io, int master) { (void)p; (void)mem; (void)io; (void)master; }
u8 pci_rd8(int b, int d, int f, int o) { (void)b; (void)d; (void)f; (void)o; return 0; }
u16 pci_rd16(int b, int d, int f, int o) { (void)b; (void)d; (void)f; (void)o; return 0; }
u32 pci_rd32(int b, int d, int f, int o) { (void)b; (void)d; (void)f; (void)o; return 0; }
void pci_wr8(int b, int d, int f, int o, u8 v) { (void)b; (void)d; (void)f; (void)o; (void)v; }

/* ---------------------------------------------------------------- the emulated chip */

static struct {
    u16 devid;
    u32 pad0;
    u8 rev;
    u32 ctrl, status, page, serial, legacy, dac2cnt, adccnt;
    u32 dac2frame, dac2size, adcframe, adcsize;    /* size: count << 16 | dwords - 1 */
    u16 src[128];
    u32 srcaddr, srcctl, srcdata, srcstate, srcreads;
    u16 ac97[64];
    u32 codecval;
    u32 unsafe, codec_wr, codec_rd, src_wr, warm_reset, ac97rst, nomagic;
    double dacacc, adcacc;
    double adcphase;
} es;

static void ac97_defaults(void)
{
    memset(es.ac97, 0, sizeof(es.ac97));
    es.ac97[0x00 / 2] = 0x0D50;
    es.ac97[0x02 / 2] = 0x8000;
    es.ac97[0x04 / 2] = 0x8000;
    es.ac97[0x06 / 2] = 0x8000;
    es.ac97[0x0E / 2] = 0x8008;
    es.ac97[0x10 / 2] = 0x8808;
    es.ac97[0x12 / 2] = 0x8808;
    es.ac97[0x16 / 2] = 0x8808;
    es.ac97[0x18 / 2] = 0x8808;
    es.ac97[0x1C / 2] = 0x8000;
    es.ac97[0x26 / 2] = 0x000F;
    es.ac97[0x28 / 2] = 0x0001;            /* VRA */
    es.ac97[0x2C / 2] = 48000;
    es.ac97[0x32 / 2] = 48000;
    es.ac97[0x7C / 2] = 0x8384;            /* SigmaTel STAC9708 */
    es.ac97[0x7E / 2] = 0x7608;
}

static void ac97_write(int reg, u16 v)
{
    reg &= 0x7E;
    if (reg == 0x00) {
        ac97_defaults();
        return;
    }
    if (reg == 0x26) {
        es.ac97[0x26 / 2] = (u16)((v & 0xFF00) | 0x000F);
        return;
    }
    if (reg == 0x7C || reg == 0x7E || reg == 0x28) return;
    if (reg == 0x02 || reg == 0x04) {               /* 5-bit attenuators: bit 5 reads as 1Fh */
        if (v & 0x2000) v = (u16)((v & ~0x3F00) | 0x1F00);
        if (v & 0x0020) v = (u16)((v & ~0x003F) | 0x001F);
    }
    es.ac97[reg / 2] = v;
}

/* sample rate of the DAC2 / ADC from the converter RAM the driver wrote */
static double dac2_rate(void)
{
    u32 freq = ((u32)(es.src[0x74 + 1] & 0xFC00) << 5) | (es.src[0x74 + 3] & 0x7FFF);
    return freq * 3000.0 / 32768.0;
}

static double adc_rate(void)
{
    u32 n = (es.src[0x78 + 0] >> 4) & 0x1F;
    u32 freq = ((u32)(es.src[0x78 + 1] & 0xFC00) << 5) | (es.src[0x78 + 3] & 0x7FFF);
    if (!n || !freq) return 0;
    return (48000.0 * 32768.0) * n / freq;
}

static u32 smprate_read(void)
{
    /* the state field cycles (state 1 every other read), as the real chip's does */
    es.srcstate = (es.srcreads++ & 1) ? 0x00010000UL : 0;
    return (es.srcaddr << 25) | es.srcctl | es.srcstate | es.srcdata;
}

static void smprate_write(u32 v)
{
    es.srcaddr = v >> 25;
    es.srcctl = v & (0x00780000UL);                 /* disable, DIS_P1, DIS_P2, DIS_R1 */
    if (v & 0x01000000UL) {
        es.src[es.srcaddr] = (u16)v;
        es.src_wr++;
    }
    es.srcdata = es.src[es.srcaddr];
}

static void codec_write(u32 v)
{
    /* the driver may write the codec register only in the converter's state 1 */
    if (es.srcstate != 0x00010000UL) es.unsafe++;
    if (v & 0x00800000UL) {
        int reg = (int)((v >> 16) & 0x7F);
        if (es.devid == 0x8938 && !(v & 0x04000000UL)) es.nomagic++;     /* EV1938: reads carry bit 26 */
        es.codecval = 0x80000000UL | ((u32)reg << 16) | es.ac97[(reg & 0x7E) / 2];
        es.codec_rd++;
    } else {
        ac97_write((int)((v >> 16) & 0x7F), (u16)v);
        es.codecval = v & 0x00FFFFFFUL;
        es.codec_wr++;
    }
}

static u32 reg_read(u16 off)
{
    switch (off & 0x3C) {
    case 0x00: return es.ctrl;
    case 0x04: return es.status;
    case 0x0C: return es.page;
    case 0x10: return smprate_read();
    case 0x14: return es.codecval;
    case 0x18: return es.legacy;
    case 0x20: return es.serial;
    case 0x28: return es.dac2cnt;
    case 0x2C: return es.adccnt;
    case 0x30: return es.page == 0x0D ? es.adcframe : 0;
    case 0x34: return es.page == 0x0D ? es.adcsize : 0;
    case 0x38: return es.page == 0x0C ? es.dac2frame : 0;
    case 0x3C: return es.page == 0x0C ? es.dac2size : 0;
    }
    return 0;
}

static void reg_write(u16 off, u32 v)
{
    switch (off & 0x3C) {
    case 0x00:
        if ((v & (1UL << 14)) && !(es.ctrl & (1UL << 14))) es.warm_reset++;
        if ((v & 0x20) && !(es.ctrl & 0x20)) {      /* DAC2 starts at the frame start */
            es.dac2size &= 0xFFFF;
            es.dacacc = 0;
        }
        if ((v & 0x10) && !(es.ctrl & 0x10)) {
            es.adcsize &= 0xFFFF;
            es.adcacc = 0;
        }
        es.ctrl = v;
        break;
    case 0x04:
        if (v & (1UL << 29)) es.ac97rst++;
        es.status = v;
        break;
    case 0x0C: es.page = v & 0x0F; break;
    case 0x10: smprate_write(v); break;
    case 0x14: codec_write(v); break;
    case 0x18: es.legacy = v; break;
    case 0x20: es.serial = v; break;
    case 0x28: es.dac2cnt = v; break;
    case 0x2C: es.adccnt = v; break;
    case 0x30: if (es.page == 0x0D) es.adcframe = v; break;
    case 0x34: if (es.page == 0x0D) es.adcsize = v & 0xFFFF; break;
    case 0x38: if (es.page == 0x0C) es.dac2frame = v; break;
    case 0x3C: if (es.page == 0x0C) es.dac2size = v & 0xFFFF; break;
    }
}

u32 inl(u16 port) { return reg_read((u16)(port - IOBASE)); }
u16 inw(u16 port) { return (u16)(reg_read((u16)((port - IOBASE) & ~3)) >> (((port - IOBASE) & 2) * 8)); }
u8 inb(u16 port) { return (u8)(reg_read((u16)((port - IOBASE) & ~3)) >> (((port - IOBASE) & 3) * 8)); }
void outl(u16 port, u32 v) { reg_write((u16)(port - IOBASE), v); }
void outw(u16 port, u16 v) { (void)port; (void)v; es.unsafe += 1000; }   /* the driver uses dwords only */
void outb(u16 port, u8 v) { (void)port; (void)v; }

/* ---------------------------------------------------------------- the DMA engines */

static FILE *dacout;
static u32 dac_frames, dac_zero;

/* ms of DAC2 playback: loop mode, 16-bit stereo, the frame counter in the size register */
static void dac2_run(double ms)
{
    double rate = dac2_rate();
    u32 n, i;
    if (!(es.ctrl & 0x20) || (es.srcctl & 0x00500000UL)) return;   /* stopped, converter off / P2 frozen */
    es.dacacc += rate * ms / 1000.0;
    n = (u32)es.dacacc;
    es.dacacc -= n;
    for (i = 0; i < n; i++) {
        u32 cnt = es.dac2size >> 16, size = (es.dac2size & 0xFFFF) + 1;
        u8 *p = host(es.dac2frame + cnt * 4);
        s16 lr[2] = { 0, 0 };
        int mute = (es.ac97[0x02 / 2] & 0x8000) || (es.ac97[0x18 / 2] & 0x8000);
        if (p && ((es.serial >> 2) & 3) == 3 && !mute) {
            lr[0] = (s16)(p[0] | (p[1] << 8));
            lr[1] = (s16)(p[2] | (p[3] << 8));
        }
        if (!lr[0] && !lr[1]) dac_zero++;
        fwrite(lr, 2, 2, dacout);
        dac_frames++;
        cnt = (cnt + 1) % size;
        es.dac2size = (cnt << 16) | (size - 1);
    }
}

/* ms of ADC recording: a 1 kHz sine (left) and 2 kHz (right) at the ADC's rate */
static void adc_run(double ms)
{
    double rate = adc_rate();
    u32 n, i;
    if (!(es.ctrl & 0x10) || rate <= 0) return;
    es.adcacc += rate * ms / 1000.0;
    n = (u32)es.adcacc;
    es.adcacc -= n;
    for (i = 0; i < n; i++) {
        u32 cnt = es.adcsize >> 16, size = (es.adcsize & 0xFFFF) + 1;
        u8 *p = host(es.adcframe + cnt * 4);
        s16 l = (s16)(12000 * sin(es.adcphase)), r = (s16)(12000 * sin(es.adcphase * 2));
        es.adcphase += 2 * 3.14159265358979 * 1000.0 / rate;
        if (p) {
            p[0] = (u8)l; p[1] = (u8)(l >> 8); p[2] = (u8)r; p[3] = (u8)(r >> 8);
        }
        cnt = (cnt + 1) % size;
        es.adcsize = (cnt << 16) | (size - 1);
    }
}

/* ---------------------------------------------------------------- test */

static int fail;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) fail++;
}

static int nrun;

static int run_chip(u16 devid, u8 rev, int full)
{
    struct pcidev p;
    struct winapireq rq;
    struct winring *r, *c;
    struct adev *a;
    u32 t, total = 0, got = 0;
    int sid, cid;
    double ph1 = 0, ph2 = 0;
    FILE *rec;
    memset(&es, 0, sizeof(es));
    es.devid = devid;
    ac97_defaults();
    memset(&p, 0, sizeof(p));
    p.vendor = devid == 0x8938 ? 0x1102 : 0x1274;
    p.device = devid;
    p.rev = rev;
    p.cls = 4;
    p.sub = 1;
    p.dev = (u8)(5 + nrun++);
    printf("\n== %04x:%04x rev %u\n", p.vendor, p.device, rev);
    check(es137x_probe(&p) == 0, "probe");
    a = adev_get(nrun - 1);
    printf("     DAC2 rate %.1f Hz, ADC rate %.1f Hz, codec writes %lu reads %lu, converter writes %lu\n",
           dac2_rate(), adc_rate(), es.codec_wr, es.codec_rd, es.src_wr);
    check(es.warm_reset == 1, "AC'97 warm reset (SYNC_RES)");
    check(es.ac97rst == (u32)(((devid == 0x5880 && rev >= 2 && rev <= 4) || (devid == 0x1371 && (rev == 7 || rev == 8))) ? 1 : 0),
          "AC'97 reset bit only on ES1373 rev. 8 / CT5880");
    check(es.unsafe == 0, "codec touched only in the converter's safe state");
    check(es.nomagic == 0, "EV1938 codec reads with its extra bit");
    check(fabs(dac2_rate() - 48000) < 2, "DAC2 at 48 kHz");
    check(fabs(adc_rate() - 48000) < 2, "ADC at 48 kHz");
    check(!(es.srcctl & 0x00780000UL), "converter enabled, no channel frozen");
    check(es.src[0x7E] == 0x1000 && es.src[0x7F] == 0x1000, "DAC2 volume unity in the converter");
    check(a && a->vid == 0x8384 && a->pid == 0x7608, "codec id read");
    check(!(es.ac97[0x18 / 2] & 0x8000), "PCM out unmuted");
    if (!full || !a) return 0;

    /* master volume at half: about 6 dB down (4 steps of 1.5 dB) */
    memset(&rq, 0, sizeof(rq));
    rq.fn = WINF_AUD_MIX; rq.arg0 = 0; rq.arg1 = MX_MASTER | 0x80000000UL; rq.arg2 = 0x80008000UL;
    aeng_api(&rq, NULL);
    printf("     master register %04x\n", es.ac97[0x02 / 2]);
    check((es.ac97[0x02 / 2] & 0x3F) == 4 && !(es.ac97[0x02 / 2] & 0x8000), "master half volume = 4 steps");
    rq.arg2 = 0xFFFFFFFFUL;
    aeng_api(&rq, NULL);

    /* playback: 440 Hz left, 880 Hz right, 44.1 kHz 16-bit stereo, 2 s */
    rings[1] = calloc(1, 32768 + 32);
    r = (struct winring *)rings[1];
    r->magic = WINRING_MAGIC; r->size = 32768;
    memset(&rq, 0, sizeof(rq));
    rq.fn = WINF_AUD_OPEN; rq.arg0 = 0; rq.arg1 = 44100; rq.arg2 = 16 | (2 << 8); rq.buf = 1;
    check(aeng_api(&rq, NULL) == WINE_OK, "play stream open");
    sid = (int)rq.retlen;
    rq.fn = WINF_AUD_CTL; rq.arg0 = sid; rq.arg1 = AC_START;
    aeng_api(&rq, NULL);
    /* recording: 48 kHz 16-bit stereo */
    rings[2] = calloc(1, 32768 + 32);
    c = (struct winring *)rings[2];
    c->magic = WINRING_MAGIC; c->size = 32768;
    memset(&rq, 0, sizeof(rq));
    rq.fn = WINF_AUD_OPEN; rq.arg0 = 1 << 8; rq.arg1 = 48000; rq.arg2 = 16 | (2 << 8); rq.buf = 2;
    check(aeng_api(&rq, NULL) == WINE_OK, "record stream open");
    cid = (int)rq.retlen;
    rq.fn = WINF_AUD_CTL; rq.arg0 = cid; rq.arg1 = AC_START;
    aeng_api(&rq, NULL);
    rec = fopen("rec.raw", "wb");
    dacout = fopen("dac2.raw", "wb");
    for (t = 0; t < 2600; t += 10) {
        while (total < 44100UL * 4 * 2 && r->size - (r->wr - r->rd) >= 4) {
            u8 *d = (u8 *)r + 32 + r->wr % r->size;
            s16 l = (s16)(12000 * sin(ph1)), rr = (s16)(12000 * sin(ph2));
            ph1 += 2 * 3.14159265358979 * 440 / 44100;
            ph2 += 2 * 3.14159265358979 * 880 / 44100;
            d[0] = (u8)l; d[1] = (u8)(l >> 8); d[2] = (u8)rr; d[3] = (u8)(rr >> 8);
            r->wr += 4;
            total += 4;
        }
        while (c->wr != c->rd) {
            u32 k = MIN(c->wr - c->rd, c->size - c->rd % c->size);
            fwrite((u8 *)c + 32 + c->rd % c->size, 1, k, rec);
            c->rd += k;
            got += k;
        }
        aeng_poll();
        dac2_run(10);
        adc_run(10);
        now_ms += 10;
    }
    fclose(dacout);
    fclose(rec);
    printf("     played %lu frames (%lu silent), recorded %lu bytes, underruns %lu\n", dac_frames, dac_zero, got,
           a->out.late);
    check(r->pos >= total - 4096 && r->rd == total, "the whole stream played");
    check(got > 48000UL * 4 * 2, "recorded more than 2 s");
    return 0;
}

int main(void)
{
    pool = malloc(POOLSIZE);
    memset(pool, 0, POOLSIZE);
    cbs.magic = WIN_MAGIC;
    aeng_init();
    run_chip(0x1371, 2, 1);         /* ES1371 */
    run_chip(0x1371, 8, 0);         /* ES1373 rev. 8 */
    run_chip(0x5880, 2, 0);         /* CT5880 rev. C */
    run_chip(0x8938, 0, 0);         /* Ectiva EV1938 */
    printf("\n%s\n", fail ? "FAILED" : "all checks passed");
    return fail != 0;
}
