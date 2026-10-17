/* host test of aeng.c + synth.c: fake output device, streams from files, MIDI events */
#include "audio.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

struct wincb cbs, *cb = &cbs;
u32 plat_mode = PM_VXD;
static u32 now;
static u8 *rings[16];
u32 plat_ms(void) { return now; }
void *plat_sysrm(u32 so) { return so < 16 ? rings[so] : NULL; }
void *plat_rm(u32 so) { return plat_sysrm(so); }
void kprintf(const char *f, ...) { va_list a; va_start(a, f); vprintf(f, a); va_end(a); }
void strlcpy(char *d, const char *s, u32 n) { u32 i; for (i = 0; i + 1 < n && s[i]; i++) d[i] = s[i]; d[i] = 0; }

static struct adev dev;
static u32 played;          /* bytes the fake DAC consumed */
static FILE *out;
static u32 devrate = 48000;
static int fstart(struct adev *a, int dir) { (void)a; (void)dir; played = 0; return 0; }
static void fstop(struct adev *a, int dir) { (void)a; (void)dir; }
static u32 fpos(struct adev *a, int dir) { (void)a; (void)dir; return played % a->out.size; }
static struct adevops ops = { fstart, fstop, fpos, NULL, NULL, NULL, NULL, NULL };

static void advance(u32 ms)
{
    u32 nb = devrate / 1000 * ms * 4, i;
    for (i = 0; i < nb; i += 4) {
        if (dev.out.running) fwrite(dev.out.buf + (played % dev.out.size), 1, 4, out);
        else { static u8 z[4]; fwrite(z, 1, 4, out); }
        played += 4;
    }
    now += ms;
}

int main(int argc, char **argv)
{
    /* harness out.raw rate  pcm <file> <rate> <bits> <ch> | midi <events> */
    struct winapireq rq;
    FILE *in;
    struct winring *r = NULL;
    u32 total = 0, t;
    int sid = 0;
    out = fopen(argv[1], "wb");
    devrate = atoi(argv[2]);
    dev.ops = &ops;
    dev.caps = WINAC_OUT;
    dev.out.size = 65536; dev.out.period = 2048; dev.out.rate = devrate; dev.out.noflush = 1;
    dev.out.buf = malloc(65536);
    strlcpy(dev.name, "fake", 48);
    aeng_init();
    adev_register(&dev);
    in = fopen(argv[4], "rb");
    if (argv[3][0] == 'p') {
        u32 rs = 32768 + 32;
        rings[1] = calloc(1, rs);
        r = (struct winring *)rings[1];
        r->magic = WINRING_MAGIC; r->size = 32768 - 32768 % (atoi(argv[6]) / 8 * atoi(argv[7]));
        memset(&rq, 0, sizeof(rq));
        rq.fn = WINF_AUD_OPEN; rq.arg0 = 0; rq.arg1 = atoi(argv[5]);
        rq.arg2 = atoi(argv[6]) | (atoi(argv[7]) << 8) | (argc > 8 ? 1 << 16 : 0); rq.buf = 1;
        printf("open %d\n", aeng_api(&rq, NULL));
        sid = rq.retlen;
        rq.fn = WINF_AUD_CTL; rq.arg0 = sid; rq.arg1 = AC_START;
        aeng_api(&rq, NULL);
        for (t = 0; t < 20000; t += 10) {
            /* the application refills the ring */
            while (r->size - (r->wr - r->rd) >= 1024) {
                u8 b[1024]; u32 n = fread(b, 1, 1024, in), k;
                if (!n) break;
                for (k = 0; k < n; k++) ((u8 *)r + 32)[(r->wr + k) % r->size] = b[k];
                r->wr += n; total += n;
            }
            aeng_poll();
            advance(10);
            if (feof(in) && r->pos == r->wr) break;
        }
        printf("fed %lu, pos %lu, flags %lx\n", total, r->pos, r->flags);
    } else {
        /* events: ms b0 b1 b2 */
        unsigned ms, b0, b1, b2;
        int ok = fscanf(in, "%u %x %x %x", &ms, &b0, &b1, &b2) == 4;
        for (t = 0; ; t += 10) {
            while (ok && ms <= t) {
                memset(&rq, 0, sizeof(rq));
                rq.fn = WINF_AUD_MIDI; rq.arg0 = 0xFF; rq.arg1 = b0 | (b1 << 8) | (b2 << 16);
                aeng_api(&rq, NULL);
                ok = fscanf(in, "%u %x %x %x", &ms, &b0, &b1, &b2) == 4;
            }
            aeng_poll();
            advance(10);
            if (!ok && !dev.out.running) break;
            if (t > 600000) break;
        }
    }
    fclose(out);
    return 0;
}
