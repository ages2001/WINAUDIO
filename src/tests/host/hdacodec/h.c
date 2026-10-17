/* h.c - host harness: runs the codec parser of hda.c against a codec dump (made by conv.py) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32; typedef int s32;
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAXNID 128
#define OPT_HDAGPIO 0x40000000UL
#define OPT_POSBUF 0
#define AS_MIX 0x10
#define MX_MASTER 0
#define MX_INGAIN 3
#define MX_COUNT 6
#define AF_HDA_AUDIO 1
#define AF_HDA_HDMI 2
#define AF_HDA_MODEM 3
#define AF_HDA_OTHER 4
struct pcidev { u8 bus, dev, fn, irq; u16 vendor, device; u8 cls, sub, prog, rev; u32 bar[6]; };
struct ahw { int x; };
struct adev { u8 insrcs, insrc; void *priv; u32 vol[MX_COUNT]; };
struct cbs { u32 opts; } cbv, *cb = &cbv;
static u32 now;
static u32 plat_ms(void) { return now += 1000; }
static void mdelay(u32 ms) { (void)ms; }
static u32 pcisub;
static u32 pci_rd32(int b, int d, int f, int o) { (void)b; (void)d; (void)f; return o == 0x2C ? pcisub : 0; }
static int verbose;
static void kprintf(const char *f, ...) { va_list a; va_start(a, f); if (verbose) vprintf(f, a); va_end(a); }
static void dsp_poll_all(void) { }
#include "hdadefs.c"

/* ------------------------------------------------ fake codec */
struct node { u32 wcaps, pincap, cfg, ai, ao, rates; int nconn; u8 conn[32]; int present; };
static struct node nd[256];
static u32 vid, ssid, start, count, gpio, dai, dao, drates;
static u8 sel[256], pinctl[256], eapd[256], pwr[256], aomute[256], aimute[256][32], aogain[256];
static u16 coef[256][128]; static u8 coefidx[256];
static int ncoefw; static char coeflog[2048];
static u32 gpiomask, gpiodir, gpiodata;

static u32 fake(u32 v)
{
    int nid = (v >> 20) & 0xFF, verb4 = (v >> 16) & 0xF, verb = (v >> 8) & 0xFFF, p8 = v & 0xFF, p16 = v & 0xFFFF;
    struct node *n = &nd[nid];
    if (verb4 >= 2 && verb4 <= 5) {
        if (verb4 == 3) {
            int mute = (p16 >> 7) & 1, idx = (p16 >> 8) & 0xF;
            if (p16 & 0x8000) { aomute[nid] = mute; aogain[nid] = p16 & 0x7F; }
            if (p16 & 0x4000) aimute[nid][idx] = mute;
        } else if (verb4 == 5) coefidx[nid] = p16 & 0x7F;
        else if (verb4 == 4) {
            coef[nid][coefidx[nid]] = p16;
            ncoefw++;
            sprintf(coeflog + strlen(coeflog), " %02x:%02x=%04x", nid, coefidx[nid], p16);
        }
        return 0;
    }
    switch (verb) {
    case 0xF00:
        if (nid == 0) {
            if (p8 == 0) return vid;
            if (p8 == 4) return (1 << 16) | 1;
            return 0;
        }
        if (nid == 1) {
            switch (p8) {
            case 0x04: return (start << 16) | count;
            case 0x05: return 1;
            case 0x09: return 0x300000;
            case 0x0A: return drates;
            case 0x0D: return dai;
            case 0x12: return dao;
            case 0x11: return gpio;
            }
            return 0;
        }
        if (!n->present) return 0;
        switch (p8) {
        case 0x09: return n->wcaps;
        case 0x0C: return n->pincap;
        case 0x0D: return n->ai;
        case 0x12: return n->ao;
        case 0x0A: return n->rates;
        case 0x0E: return n->nconn;
        }
        return 0;
    case 0xF02: {
        u32 r = 0; int k;
        for (k = 0; k < 4 && p8 + k < n->nconn; k++) r |= (u32)n->conn[p8 + k] << (8 * k);
        return r;
    }
    case 0xF1C: return n->cfg;
    case 0xF20: return nid == 1 ? ssid : 0;
    case 0xC00: return coef[nid][coefidx[nid]];
    case 0xF09: return 0;
    case 0x701: sel[nid] = p8; return 0;
    case 0x707: pinctl[nid] = p8; return 0;
    case 0x70C: eapd[nid] = p8; return 0;
    case 0x705: pwr[nid] = p8 + 0x10; return 0;
    case 0x715: gpiodata = p8; return 0;
    case 0x716: gpiomask = p8; return 0;
    case 0x717: gpiodir = p8; return 0;
    }
    return 0;
}

static u32 cmd(struct hda *h, u32 v) { (void)h; return fake(v); }
#define VERB(h, nid, v, p)  (((u32)(h)->cad << 28) | ((u32)(nid) << 20) | ((u32)(v) << 8) | (u32)(p))
#define VERB4(h, nid, v, p) (((u32)(h)->cad << 28) | ((u32)(nid) << 20) | ((u32)(v) << 16) | (u32)(p))
#include "hdapart.c"

/* ------------------------------------------------ verification */
static int wtype(int nid) { return (nd[nid].wcaps >> 20) & 0xF; }
static char trail[256];
/* follow the signal from nid towards a DAC with the state the driver set */
static int reach_dac(int nid, int depth)
{
    struct node *n = &nd[nid];
    int t = wtype(nid), k, r;
    char *e = trail + strlen(trail);
    if (depth > 8 || !n->present) return 0;
    sprintf(e, depth ? "->%02x" : "%02x", nid);
    if (n->wcaps & 0x400 && pwr[nid] != 0x10) { strcat(trail, "(D3!)"); }
    if (t == 0) {
        if ((n->wcaps & 4) && aomute[nid]) { strcat(trail, "(muted!)"); return 0; }
        return nid;
    }
    if (depth && t == 4) return 0;
    if ((n->wcaps & 4) && depth && aomute[nid]) { strcat(trail, "(out muted!)"); return 0; }
    if (t == 2) {
        for (k = 0; k < n->nconn; k++) {
            int l = strlen(trail);
            if ((n->wcaps & 2) && aimute[nid][k]) continue;
            r = reach_dac(n->conn[k], depth + 1);
            if (r) return r;
            trail[l] = 0;
        }
        return 0;
    }
    if (!n->nconn) return 0;
    k = n->nconn > 1 ? sel[nid] : 0;
    if (t == 3 && (n->wcaps & 2) && aimute[nid][k]) { strcat(trail, "(in muted!)"); return 0; }
    return reach_dac(n->conn[k], depth + 1);
}

/* from the ADC to an input pin */
static int reach_pin(int nid, int depth, int want)
{
    struct node *n = &nd[nid];
    int t = wtype(nid), k, r;
    char *e = trail + strlen(trail);
    if (depth > 8 || !n->present) return 0;
    sprintf(e, depth ? "->%02x" : "%02x", nid);
    if (t == 4) return nid == want ? nid : 0;
    if (depth && t == 0) return 0;
    if (t == 2) {
        for (k = 0; k < n->nconn; k++) {
            int l = strlen(trail);
            if ((n->wcaps & 2) && aimute[nid][k]) continue;
            r = reach_pin(n->conn[k], depth + 1, want);
            if (r) return r;
            trail[l] = 0;
        }
        return 0;
    }
    if (!n->nconn) return 0;
    k = n->nconn > 1 ? sel[nid] : 0;
    if ((n->wcaps & 2) && aimute[nid][k]) { strcat(trail, "(in muted!)"); return 0; }
    return reach_pin(n->conn[k], depth + 1, want);
}

static const char *ddname[16] = { "LineOut", "Speaker", "HP", "CD", "SPDIF", "DigOut", "Modem", "Phone",
    "LineIn", "Aux", "Mic", "Telephony", "SPDIFin", "DigIn", "Rsv", "Other" };

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "r");
    char line[1024];
    static struct hda hh;
    struct hda *h = &hh;
    int i, ok, fail = 0;
    verbose = argc > 2;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == 'H') {
            sscanf(line + 2, "%x %x %x %x %x %x %x %x", &vid, &ssid, &start, &count, &gpio, &dai, &dao, &drates);
        } else if (line[0] == 'N') {
            unsigned nid, w, pc, cfg, ai, ao, rt; int nc, pos, k;
            char *p = line + 2;
            sscanf(p, "%x %x %x %x %x %x %x %d%n", &nid, &w, &pc, &cfg, &ai, &ao, &rt, &nc, &pos);
            p += pos;
            nd[nid].present = 1; nd[nid].wcaps = w; nd[nid].pincap = pc; nd[nid].cfg = cfg;
            nd[nid].ai = ai; nd[nid].ao = ao; nd[nid].rates = rt; nd[nid].nconn = nc;
            for (k = 0; k < nc; k++) { unsigned c; int q; sscanf(p, "%x%n", &c, &q); p += q; nd[nid].conn[k] = c; }
        }
    }
    /* worst case power-on state: everything muted, first input selected, pins off, D3 */
    memset(aomute, 1, sizeof(aomute));
    memset(aimute, 1, sizeof(aimute));
    memset(pwr, 0x13, sizeof(pwr));
    pcisub = (ssid >> 16) | (ssid << 16);
    h->a.priv = h;
    ok = parse_codec(h, 0);
    printf("codec %08x ssid %08x: %s, DACs", vid, ssid, ok ? "NO ANALOG OUTPUT" : "ok");
    for (i = 0; i < h->ndac; i++) printf(" %02x", h->dac[i]);
    printf(", volume");
    for (i = 0; i < h->nvol; i++) printf(" %02x(%d steps)", h->volnid[i], h->volsteps[i]);
    printf("\n");
    for (i = 0; i < h->nopin; i++) {
        int p = h->opin[i], d;
        trail[0] = 0;
        d = reach_dac(p, 0);
        printf("  out %-7s pin %02x ctl %02x eapd %d  %s  %s\n", ddname[h->otype[i]], p, pinctl[p], eapd[p], trail,
               d ? "OK" : "BROKEN");
        if (!d || !(pinctl[p] & 0x40) || ((nd[p].wcaps & 4) && aomute[p])) fail++;
    }
    /* configured output pins the parser did not take */
    for (i = start; i < start + count; i++) {
        int k, dd = (nd[i].cfg >> 20) & 0xF;
        if (wtype(i) != 4 || (nd[i].cfg >> 30) == 1 || !(nd[i].pincap & 0x10) || (nd[i].pincap & 0x80)) continue;
        if (dd > 2) continue;
        for (k = 0; k < h->nopin && h->opin[k] != i; k++) ;
        if (k == h->nopin) printf("  out %-7s pin %02x NOT USED\n", ddname[dd], i);
    }
    printf("  ADC %02x", h->adc);
    for (i = 0; i < 5; i++) {
        static const char *sn[5] = { "mic", "line", "cd", "aux", "mix" };
        int len = h->srclen[i], last, r;
        if (!len) continue;
        set_src(&h->a, 1 << i);
        last = h->srcpath[i][len - 1];
        trail[0] = 0;
        r = reach_pin(h->adc, 0, wtype(last) == 4 ? last : -1);
        if (wtype(last) != 4) {
            trail[0] = 0;
            for (r = 0; r < len; r++) sprintf(trail + strlen(trail), "%s%02x", r ? "->" : "", h->srcpath[i][r]);
            r = !aimute[last][h->mixin] || !(nd[last].wcaps & 2);
            sprintf(trail + strlen(trail), "[in %02x]", nd[last].conn[h->mixin]);
        }
        printf("  %s:%s%s", sn[i], trail, r ? "" : "(BROKEN)");
        if (!r) fail++;
    }
    printf("\n  gpio %d (set %x/%x/%x) coef writes:%s\n", gpio & 0xFF, gpiomask, gpiodir, gpiodata, ncoefw ? coeflog : " none");
    return fail ? 2 : ok ? 1 : 0;
}
