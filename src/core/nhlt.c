/* nhlt.c - ACPI NHLT (Non HD Audio Link Table): the audio endpoints the firmware describes
   for the Intel DSP (I2S codecs on SSP ports, the digital microphone array, Bluetooth), each
   with its formats and the gateway configuration blob the DSP firmware takes as it is. */
#include "dsp.h"

#define MAXNEP 16

static const u8 *nhlt;              /* the table (mapped) */
static u32 nhltlen;
static struct nhlt_ep eps[MAXNEP];
static const u8 *epptr[MAXNEP];
static int neps = -1;

/* endpoint descriptor: length(4) link(1) instance(1) vid(2) did(2) rev(2) subsys(4) devtype(1)
   dir(1) bus(1) specific config (size(4) + data), then the format list:
   count(1), per format: WAVEFORMATEXTENSIBLE (40 bytes) + specific config (size(4) + blob) */
#define EP_HDRLEN   19

static const u8 *ep_formats(const u8 *ep)
{
    u32 cs = *(const u32 *)(ep + EP_HDRLEN);
    return ep + EP_HDRLEN + 4 + cs;
}

int nhlt_load(void)
{
    const u8 *p, *end;
    int i, n;
    if (neps >= 0) return neps;
    neps = 0;
    nhlt = acpi_table("NHLT", 0);
    if (!nhlt) {
        DBG("nhlt: no table\n");
        return 0;
    }
    nhltlen = *(const u32 *)(nhlt + 4);
    n = nhlt[36];
    p = nhlt + 37;
    end = nhlt + nhltlen;
    for (i = 0; i < n && neps < MAXNEP && p + EP_HDRLEN + 4 <= end; i++) {
        u32 len = *(const u32 *)p;
        struct nhlt_ep *e = &eps[neps];
        const u8 *f;
        if (len < EP_HDRLEN + 5 || p + len > end) break;
        e->link = p[4];
        e->vid = *(const u16 *)(p + 6);
        e->did = *(const u16 *)(p + 8);
        e->dev = p[16];
        e->dir = p[17];
        e->bus = p[18];
        f = ep_formats(p);
        e->nfmt = f < p + len ? f[0] : 0;
        e->mics = 0;
        if (e->link == NHLT_LINK_PDM) {
            /* microphone array: the most channels of its formats */
            const u8 *c = f + 1;
            int k;
            for (k = 0; k < e->nfmt && c + 44 <= p + len; k++) {
                u16 ch = *(const u16 *)(c + 2);
                if (ch > e->mics) e->mics = (u8)ch;
                c += 40 + 4 + *(const u32 *)(c + 40);
            }
        }
        epptr[neps] = p;
        kprintf("nhlt: endpoint %d: %s, %s %s, port %u, %u format(s)%s\n", neps,
                e->link == NHLT_LINK_SSP ? "SSP" : e->link == NHLT_LINK_PDM ? "DMIC" :
                e->link == NHLT_LINK_HDA ? "HDA" : "DSP",
                e->dev == NHLT_DEV_BT ? "Bluetooth" : e->dev == NHLT_DEV_DMIC ? "microphones" :
                e->dev == NHLT_DEV_I2S ? "I2S codec" : "device",
                e->dir ? "capture" : "render", e->bus, e->nfmt, e->mics > 2 ? ", 4 microphones" : "");
        neps++;
        p += len;
    }
    return neps;
}

int nhlt_count(void)
{
    return neps < 0 ? nhlt_load() : neps;
}

const struct nhlt_ep *nhlt_get(int i)
{
    return i >= 0 && i < neps ? &eps[i] : NULL;
}

static int ep_match(int i, int link, int dev, int dir, int bus)
{
    const struct nhlt_ep *e = &eps[i];
    if (e->link != link || e->dir != dir) return 0;
    if (link == NHLT_LINK_PDM) return 1;                    /* DMIC: no device type / bus check */
    return e->bus == bus && (dev < 0 || e->dev == dev);
}

/* the blob of the endpoint format ch/rate/bits/vbits (vbits 0: any; DMIC 32-bit: 24 or 32) */
const u8 *nhlt_blob(int link, int dev, int dir, int bus, int ch, u32 rate, int bits, int vbits, u32 *len,
                    struct afmt *got)
{
    int i, k;
    if (nhlt_count() <= 0) return NULL;
    for (i = 0; i < neps; i++) {
        const u8 *ep = epptr[i], *c, *end = ep + *(const u32 *)ep;
        if (!ep_match(i, link, dev, dir, bus)) continue;
        c = ep_formats(ep);
        k = *c++;
        for (; k > 0 && c + 44 <= end; k--) {
            u16 fch = *(const u16 *)(c + 2), fbits = *(const u16 *)(c + 14), fv = *(const u16 *)(c + 18);
            u32 frate = *(const u32 *)(c + 4), cs = *(const u32 *)(c + 40);
            int vok = !vbits || fv == vbits || (link == NHLT_LINK_PDM && bits == 32 && (fv == 24 || fv == 32));
            if (fch == ch && frate == rate && fbits == bits && vok && c + 44 + cs <= end) {
                if (len) *len = cs;
                if (got) afmt_set(got, frate, fbits, fv ? fv : fbits, fch);
                return c + 44;
            }
            c += 44 + cs;
        }
    }
    return NULL;
}

/* the preferred format of an endpoint: 48 kHz stereo, 24 in 32 bit (32 / 16 bit next),
   for microphones the fewest channels at 48 kHz; 0 when the endpoint has none */
int nhlt_best(int link, int dev, int dir, int bus, struct afmt *f)
{
    static const u8 pref[][3] = {                   /* channels, bits, valid bits */
        { 2, 32, 24 }, { 2, 32, 32 }, { 2, 16, 16 }, { 2, 24, 24 },
        { 4, 32, 24 }, { 4, 32, 32 }, { 4, 16, 16 }, { 1, 32, 24 }, { 1, 16, 16 },
    };
    int i;
    u32 len;
    for (i = 0; i < (int)ARRAY_SIZE(pref); i++)
        if (nhlt_blob(link, dev, dir, bus, pref[i][0], 48000, pref[i][1], pref[i][2], &len, f)) return 1;
    return 0;
}

/* ---- audio formats ---- */
void afmt_set(struct afmt *f, u32 rate, int bits, int vbits, int ch)
{
    f->rate = rate;
    f->bits = (u32)bits;
    f->chmap = ch == 1 ? 0xFFFFFFF0UL : ch == 2 ? 0xFFFFFF10UL : ch == 4 ? 0xFFFF3210UL :
               ch == 3 ? 0xFFFFF210UL : 0x76543210UL;
    /* channel configuration: mono 0, stereo 1, 3.0 3, quatro 5, 7.1 12 */
    f->chcfg = ch == 1 ? 0 : ch == 2 ? 1 : ch == 3 ? 3 : ch == 4 ? 5 : 12;
    f->ileave = 0;
    f->cfg = (u32)ch | ((u32)vbits << 8);
}

u32 afmt_bytes_ms(const struct afmt *f)
{
    return f->rate / 1000 * (f->bits / 8) * (u32)AF_CH(f);
}

u16 afmt_hda(const struct afmt *f)
{
    u16 v = (u16)(AF_CH(f) - 1);
    switch (f->rate) {
    case 44100: v |= 0x4000; break;
    case 96000: v |= 0x0800; break;
    case 192000: v |= 0x1800; break;
    case 16000: v |= 0x0200; break;
    default: break;                                 /* 48000 */
    }
    switch (f->bits == 32 ? AF_VBITS(f) : (int)f->bits) {
    case 8: break;
    case 16: v |= 0x10; break;
    case 20: v |= 0x20; break;
    case 24: v |= 0x30; break;
    default: v |= 0x40; break;
    }
    return v;
}

/* ---------------------------------------------------------------- SSP blobs */

#define SSP_BLOB_V15    0xEE000105
#define SSP_BLOB_V20    0xEE000200
#define SSP_BLOB_V30    0xEE000300

/* the I2S port settings an SSP gateway blob programs: format, clock directions, BCLK from the
   frame layout, MCLK from the divider (xtal: the platform's crystal). Blob layouts (dwords):
   1.0: gateway attributes, 8 TDM slot groups, SSC0 SSC1 SSCTO SSPSP SSTSA SSRSA SSC2 SSPSP2
        SSC3 SSIOC, MDIVC MDIVR
   1.5: gateway attributes, version, size, 8 slot groups, the same 10 registers, MDIVCTLR,
        MDIVR count, MDIVR[]
   3.0: gateway attributes, version, size, 8 slot groups, SSC0 SSC1 SSCTO SSPSP SSC2 SSPSP2
        SSC3 SSIOC, 8 x 64-bit SSMIDyTSA, 8 x 64-bit SSMODyTSA, MDIVCTLR, MDIVR count, MDIVR[]
   (the version is the second dword, Linux intel_nhlt_ssp_mclk_mask) */
int nhlt_ssp_decode(const u8 *blob, u32 len, u32 rate, u32 xtal, struct sspcfg *s)
{
    const u32 *b = (const u32 *)blob;
    u32 ssc0, ssc1, sspsp, sspsp2, sstsa, ssrsa, mdivc, mdivr, src, dsize, pad, bits, ver;
    u32 r0, md, need;
    if (!blob || len < 84) return -1;
    ver = b[1];
    if (ver == SSP_BLOB_V15 || ver == SSP_BLOB_V30) {
        r0 = 11;
        md = ver == SSP_BLOB_V30 ? 51 : 21;
        need = md + 2;
        if (need * 4 > len) return -1;
        if (b[md + 1] >= 1) need++;
        if (need * 4 > len) return -1;
        ssc0 = b[r0];
        ssc1 = b[r0 + 1];
        sspsp = b[r0 + 3];
        if (ver == SSP_BLOB_V30) {
            sspsp2 = b[r0 + 5];
            ssrsa = b[19];                              /* SSMID0TSA, low dword */
            sstsa = b[35];                              /* SSMOD0TSA, low dword */
        } else {
            sstsa = b[r0 + 4];
            ssrsa = b[r0 + 5];
            sspsp2 = b[r0 + 7];
        }
        mdivc = b[md];
        mdivr = b[md + 1] >= 1 ? b[md + 2] : 0xFFF;
    } else {
        /* 1.0, and 2.0 with MDIVC at dword 20 */
        md = ver == SSP_BLOB_V20 ? 20 : 19;
        if ((md + 2) * 4 > len) return -1;
        ssc0 = b[md - 10];
        ssc1 = b[md - 9];
        sspsp = b[md - 7];
        sstsa = b[md - 6];
        ssrsa = b[md - 5];
        sspsp2 = b[md - 3];
        mdivc = b[md];
        mdivr = b[md + 1];
    }
    memset(s, 0, sizeof(*s));
    dsize = (ssc0 & 15) + 1 + ((ssc0 & (1UL << 20)) ? 16 : 0);
    pad = ((sspsp >> 23) & 3) | (((sspsp >> 26) & 7) << 2);
    s->slots = (u16)(((ssc0 >> 24) & 7) + 1);
    s->vbits = (u16)dsize;
    s->width = (u16)(dsize + pad);
    s->txmask = sstsa & 0xFF;
    s->rxmask = ssrsa & 0xFF;
    s->provider = (u8)((ssc1 & (3UL << 24)) ? 1 : 0);
    if (ssc0 & (1UL << 31)) {                           /* network (TDM) mode: DSP A / B */
        s->fmt = (sspsp & (1UL << 25)) ? 4 : 5;
        bits = (u32)s->slots * s->width + (sspsp2 & 0xFF);
    } else {                                            /* I2S / left justified */
        u32 fw = (sspsp >> 16) & 0x3F;
        s->fmt = (sspsp & (1UL << 25)) ? 1 : 3;
        bits = fw > 1 ? 2 * fw : (u32)s->slots * s->width;
        if (fw > 1) s->width = (u16)fw;
    }
    s->bclk = rate * bits;
    if (mdivc & 3) {
        static const u32 srcs[4] = { 0, 24576000, 96000000, 0 };
        src = (mdivc >> 16) & 3;
        s->mclk = src ? srcs[src] : xtal;
        if ((mdivr & 0xFFF) != 0xFFF) s->mclk /= (mdivr & 0xFFF) + 2;
        /* the MCLK output the blob enables: MCLK0, else MCLK1 (Linux mclk_id quirk) */
        s->mclkid = (u8)((mdivc & 1) ? 0 : 1);
    }
    return 0;
}
