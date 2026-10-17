/* dsp.c - Intel audio DSP: platforms, HD Audio controller in DSP mode, DSP cores, firmware
   boot and the IPC transport (polled; the controller interrupt stays off).

   Boot, as the Linux SOF and AVS drivers do it:
   - Skylake / Kaby Lake: the ROM pulls the image through the code loader DMA of the DSP BAR
     (128K ring, software position in buffer), then reports BASEFW_ENTERED.
   - cAVS 1.5+ and ACE: a host output DMA stream holds the whole image; the ROM is told the
     stream with a ROM_CONTROL purge IPC, runs, and reports FW_ENTERED.
   The firmware then sends FW_READY; dspipc4.c / dspipc3.c take over. */
#include "dsp.h"

/* ---- HD Audio stream descriptor (HDA BAR; DSP BAR 0x80 for the SKL code loader) ---- */
#define SD(n)           (0x80 + (n) * 0x20)
#define SD_CTL          0x00
#define SD_STS          0x03
#define SD_LPIB         0x04
#define SD_CBL          0x08
#define SD_LVI          0x0C
#define SD_FMT          0x12
#define SD_BDLPL        0x18
#define SD_BDLPU        0x1C
#define SD_DPIB(n)      (0x1084 + (n) * 0x20)   /* DMA position in buffer of the host DMA */
#define SDC_SRST        0x01
#define SDC_RUN         0x02
#define SDC_INTS        0x1C            /* IOCE FEIE DEIE */
#define SDS_BCIS        0x04
#define GCAP            0x00
#define HDA_EM2         0x1030
#define EM2_L1SEN       (1UL << 13)
#define EM2_DUM         (1UL << 23)             /* capture position after the data reached memory */
#define HDA_LTRP        0x1048          /* LTR guardband */
#define HDA_LLCH        0x14

/* PCI configuration (Intel HD Audio) */
#define PCI_PGCTL       0x44
#define PGCTL_ADSPPGD   0x04
#define PGCTL_LSRMD     0x10
#define PCI_CGCTL       0x48
#define CGCTL_ADSPDCGE  0x02
#define CGCTL_MISCBDCGE 0x40

/* DSP BAR: cAVS general registers */
#define ADSPCS          0x04
#define ADSPIC          0x08
#define ADSPIS          0x0C
#define CS_CRST(m)      ((u32)(m))
#define CS_CSTALL(m)    ((u32)(m) << 8)
#define CS_SPA(m)       ((u32)(m) << 16)
#define CS_CPA(m)       ((u32)(m) << 24)

/* SKL code loader DMA */
#define CL_SD           0x80
#define CL_SPBFCCTL     0xA4
#define CL_SPIB         0xA8
#define CL_BUFSIZE      0x20000UL

/* ACE (Meteor Lake and later) */
#define MTL_HFDSSCS     0x1000
#define MTL_DSS_SPA     (1UL << 16)
#define MTL_DSS_CPA     (1UL << 24)
#define MTL_HFPWRCTL    0x1D18
#define MTL_HFPWRSTS    0x1D1C
#define PTL_HFPWRCTL2   0x1D20
#define PTL_HFPWRSTS2   0x1D24
#define MTL_DSP2CXCTL   0x178D04
#define MTL_CX_SPA      0x01
#define MTL_CX_CPA      0x100

/* ROM states (firmware status register, low 24 bits; the SKL ROM: low 8 bits) */
#define FSR_MASK            0xFFFFFFUL
#define FSR_MASK_SKL        0xFFUL
#define FSR_INIT_DONE       0x1
#define FSR_FW_ENTERED      0x5
#define FSR_BASEFW_ENTERED  0xF         /* SKL */
#define ROM_IPC_CONTROL     0x01000000UL
#define ROM_IPC_PURGE       0x00004000UL
#define BOOT_ATTEMPTS       3

#define IPC4_NOTIFY         27          /* global message type of notifications */
#define IPC4_FW_READY       8

struct dsp *dsp_list;

/* ---------------------------------------------------------------- platforms */

static const struct dspplat pl_skl = { "skl", DG_SKL, DPF_IPC4 | DPF_AVS, 2, 3, 6, 0, 1000, "DSPSKL.BIN", NULL };
static const struct dspplat pl_apl = { "apl", DG_APL, DPF_PROCEN, 2, 3, 6, 0, 150, "SOFAPL.RI", "SOFAPLC.RI" };
static const struct dspplat pl_glk = { "glk", DG_APL, DPF_PROCEN, 2, 3, 6, 0, 150, "SOFAPL.RI", "SOFAPLC.RI" };
static const struct dspplat pl_cnl = { "cnl", DG_CNL, 0, 4, 15, 3, 0, 300, "SOFCNL.RI", "SOFCNLC.RI" };
static const struct dspplat pl_cfl = { "cfl", DG_CNL, 0, 4, 15, 3, 0, 300, "SOFCNL.RI", "SOFCNLC.RI" };
static const struct dspplat pl_cml = { "cml", DG_CNL, 0, 4, 15, 3, 0, 300, "SOFCNL.RI", "SOFCNLC.RI" };
static const struct dspplat pl_icl = { "icl", DG_ICL, DPF_ICCMAX, 4, 15, 6, 0, 300, "SOFICL.RI", "SOFICLC.RI" };
static const struct dspplat pl_jsl = { "jsl", DG_ICL, 0, 2, 3, 6, 0, 300, "SOFJSL.RI", "SOFJSLC.RI" };
static const struct dspplat pl_ehl = { "ehl", DG_TGL, DPF_ICCMAX, 4, 1, 3, 0, 300, "SOFEHL.RI", "SOFEHLC.RI" };
static const struct dspplat pl_tgl = { "tgl", DG_TGL, DPF_IPC4 | DPF_ICCMAX, 4, 1, 3, 0, 300, "S4TGL.RI", "S4TGLC.RI" };
static const struct dspplat pl_tglh = { "tglh", DG_TGL, DPF_IPC4 | DPF_ICCMAX, 2, 1, 3, 0, 300, "S4TGLH.RI", "S4TGLHC.RI" };
static const struct dspplat pl_adl = { "adl", DG_TGL, DPF_IPC4 | DPF_ICCMAX, 4, 1, 3, 0, 300, "S4ADL.RI", "S4TGLC.RI" };
static const struct dspplat pl_adls = { "adls", DG_TGL, DPF_IPC4 | DPF_ICCMAX, 2, 1, 3, 0, 300, "S4ADLS.RI", "S4TGLHC.RI" };
static const struct dspplat pl_adln = { "adln", DG_TGL, DPF_IPC4 | DPF_ICCMAX, 4, 1, 3, 0, 300, "S4ADLN.RI", "S4TGLC.RI" };
static const struct dspplat pl_rpl = { "rpl", DG_TGL, DPF_IPC4 | DPF_ICCMAX, 4, 1, 3, 0, 300, "S4ADL.RI", "S4TGLC.RI" };
static const struct dspplat pl_rpls = { "rpls", DG_TGL, DPF_IPC4 | DPF_ICCMAX, 2, 1, 3, 0, 300, "S4ADLS.RI", "S4TGLHC.RI" };
static const struct dspplat pl_mtl = { "mtl", DG_MTL, DPF_IPC4, 3, 1, 3, 0, 300, "S4MTL.RI", "S4MTLC.RI" };
static const struct dspplat pl_arls = { "arls", DG_MTL, DPF_IPC4, 2, 1, 3, 0, 300, "S4ARLS.RI", "S4MTLC.RI" };
static const struct dspplat pl_lnl = { "lnl", DG_LNL, DPF_IPC4, 5, 1, 3, 0, 300, "S4LNL.RI", "S4LNLC.RI" };
static const struct dspplat pl_ptl = { "ptl", DG_PTL, DPF_IPC4, 5, 1, 3, 0, 300, "S4PTL.RI", "S4PTLC.RI" };
static const struct dspplat pl_wcl = { "wcl", DG_PTL, DPF_IPC4, 3, 1, 3, 0, 300, "S4WCL.RI", "S4WCLC.RI" };
static const struct dspplat pl_nvl = { "nvl", DG_NVL, DPF_IPC4, 4, 1, 3, 0, 300, "S4NVL.RI", "S4NVLC.RI" };
static const struct dspplat pl_nvls = { "nvls", DG_NVL, DPF_IPC4, 2, 1, 3, 0, 300, "S4NVLS.RI", "S4NVLSC.RI" };

static const struct { u16 id; const struct dspplat *pl; } plats[] = {
    { 0x9D70, &pl_skl }, { 0xA170, &pl_skl }, { 0x9D71, &pl_skl }, { 0xA171, &pl_skl }, { 0xA2F0, &pl_skl },
    { 0x5A98, &pl_apl }, { 0x1A98, &pl_apl }, { 0x3198, &pl_glk },
    { 0x9DC8, &pl_cnl }, { 0xA348, &pl_cfl }, { 0x02C8, &pl_cml }, { 0x06C8, &pl_cml }, { 0xA3F0, &pl_cml },
    { 0xF1C8, &pl_cml },                            /* Rocket Lake-S (CNL-style PCH) */
    { 0x34C8, &pl_icl }, { 0x3DC8, &pl_icl }, { 0x38C8, &pl_jsl }, { 0x4DC8, &pl_jsl },
    { 0x4B55, &pl_ehl }, { 0x4B58, &pl_ehl },
    { 0xA0C8, &pl_tgl }, { 0x43C8, &pl_tglh }, { 0xF0C8, &pl_tgl },    /* Comet Lake-R uses TGL */
    { 0x51C8, &pl_adl }, { 0x51C9, &pl_adl }, { 0x51CC, &pl_adl }, { 0x51CD, &pl_adl },
    { 0x51CA, &pl_rpl }, { 0x51CB, &pl_rpl }, { 0x51CE, &pl_rpl }, { 0x51CF, &pl_rpl },
    { 0x7AD0, &pl_adls }, { 0x7A50, &pl_rpls }, { 0x54C8, &pl_adln },
    { 0x7E28, &pl_mtl }, { 0x7728, &pl_mtl }, { 0x7F50, &pl_arls },
    { 0xA828, &pl_lnl }, { 0xE428, &pl_ptl }, { 0xE328, &pl_ptl }, { 0x4D28, &pl_wcl },
    { 0xD328, &pl_nvl }, { 0x6E50, &pl_nvls },
    { 0, NULL }
};

const struct dspplat *dsp_platform(u16 vendor, u16 device)
{
    int i;
    if (vendor != 0x8086) return NULL;
    for (i = 0; plats[i].id; i++)
        if (plats[i].id == device) return plats[i].pl;
    return NULL;
}

u32 dsp_xtal(struct dsp *d)
{
    switch (d->pl->gen) {
    case DG_BYT: return d->shim == SST_BDW ? 24000000 : 19200000;
    case DG_SKL: case DG_CNL: return 24000000;
    case DG_APL: return 19200000;
    default: return 38400000;
    }
}

/* ---------------------------------------------------------------- registers */

static u32 drd(struct dsp *d, u32 o) { return RD32(d->bar + o); }
static void dwr(struct dsp *d, u32 o, u32 v) { WR32(d->bar + o, v); }
static void dupd(struct dsp *d, u32 o, u32 mask, u32 v) { dwr(d, o, (drd(d, o) & ~mask) | v); }
static u32 hrd(struct dsp *d, u32 o) { return RD32(d->hda + o); }
static void hwr(struct dsp *d, u32 o, u32 v) { WR32(d->hda + o, v); }
static void hupd(struct dsp *d, u32 o, u32 mask, u32 v) { hwr(d, o, (hrd(d, o) & ~mask) | v); }

static int dpoll(struct dsp *d, u32 o, u32 mask, u32 val, u32 ms)
{
    u32 t0 = plat_ms();
    for (;;) {
        u32 v = drd(d, o);
        if (v != 0xFFFFFFFFUL && (v & mask) == val) return 0;
        if (plat_ms() - t0 > ms) return -1;
        udelay(10);
    }
}

static u32 fsr_mask(struct dsp *d)
{
    return d->pl->gen == DG_SKL ? FSR_MASK_SKL : FSR_MASK;
}

static int fsr_wait(struct dsp *d, u32 state, u32 ms)
{
    u32 t0 = plat_ms(), m = fsr_mask(d);
    for (;;) {
        u32 v = drd(d, d->r_rom);
        if ((v & m) == state) return 0;
        if (plat_ms() - t0 > ms) return -1;
        udelay(20);
    }
}

void dsp_mbox_write(struct dsp *d, u32 off, const void *p, u32 n)
{
    const u32 *s = p;
    u32 i;
    for (i = 0; i < (n + 3) / 4; i++) dwr(d, off + i * 4, s[i]);
}

void dsp_mbox_read(struct dsp *d, u32 off, void *p, u32 n)
{
    u32 *s = p, i, w;
    for (i = 0; i < n / 4; i++) s[i] = drd(d, off + i * 4);
    if (n & 3) {
        w = drd(d, off + i * 4);
        memcpy((u8 *)p + i * 4, &w, n & 3);
    }
}

static void dump_state(struct dsp *d, const char *what)
{
    if (d->shim) {
        /* SST shim: CSR, IPCX, IPCD (high dwords hold BUSY / DONE on Bay Trail) */
        kprintf("dsp: %s: CSR %08lx IPCX %08lx:%08lx IPCD %08lx:%08lx\n", what, drd(d, d->r_ctl),
                drd(d, d->r_req), d->r_reqext ? drd(d, d->r_reqext) : 0UL, drd(d, d->r_rsp),
                d->r_rspext ? drd(d, d->r_rspext) : 0UL);
        return;
    }
    kprintf("dsp: %s: status %08lx error %08lx, ADSPCS %08lx\n", what, drd(d, d->r_rom), drd(d, d->r_romerr),
            d->pl->gen >= DG_MTL ? drd(d, MTL_DSP2CXCTL) : drd(d, ADSPCS));
}

/* ---------------------------------------------------------------- HD Audio capabilities */

static void find_caps(struct dsp *d)
{
    u32 off = hrd(d, HDA_LLCH) & 0xFFFF;
    int guard = 16;
    while (off && off < 0x4000 && guard--) {
        u32 h = hrd(d, off);
        switch ((h >> 16) & 0xFFF) {
        case 2: d->ml = d->hda + off; break;
        case 3: d->pp = d->hda + off; break;
        case 4: d->spib = d->hda + off; break;
        case 5: d->drsm = d->hda + off; break;
        }
        off = h & 0xFFFF;
    }
}

/* ACE 2+ multi-link: the extended (alternate) link of an IP (SSP 0xC0, DMIC 0xC1, UAOL 0xC2) */
#define ML_LCAP         0x00
#define ML_LCTL         0x04
#define ML_LOSIDV       0x08
#define ML_LEPTR        0x20
#define ML_LCAP_ALT     (1UL << 28)
#define ML_LCAP_OFLS    (1UL << 26)
#define ML_LCTL_OFLEN   (1UL << 4)
#define ML_ID_SSP       0xC0
#define ML_ID_DMIC      0xC1
#define ML_ID_UAOL      0xC2

static u8 *ml_link(struct dsp *d, u32 id)
{
    u32 n, i;
    if (!d->ml || d->pl->gen < DG_LNL) return NULL;
    n = (RD32(d->ml + 4) & 0xF) + 1;
    for (i = 0; i < n; i++) {
        u8 *m = d->ml + 0x40 + 0x40 * i;
        u32 lcap = RD32(m + ML_LCAP);
        if (lcap != 0xFFFFFFFFUL && (lcap & ML_LCAP_ALT) && (RD32(m + ML_LEPTR) >> 24) == id) return m;
    }
    return NULL;
}

/* SSP, DMIC and UAOL links handed to the DSP (Linux hdac_bus_eml_enable_offload, lnl.c) */
static void ml_offload(struct dsp *d, int on)
{
    static const u8 ids[3] = { ML_ID_SSP, ML_ID_DMIC, ML_ID_UAOL };
    int i;
    for (i = 0; i < 3; i++) {
        u8 *m = ml_link(d, ids[i]);
        if (!m || !(RD32(m + ML_LCAP) & ML_LCAP_OFLS)) continue;
        if (on) WR32(m + ML_LCTL, RD32(m + ML_LCTL) | ML_LCTL_OFLEN);
        else WR32(m + ML_LCTL, RD32(m + ML_LCTL) & ~ML_LCTL_OFLEN);
    }
}

/* ACE 2+: output stream id valid on the SSP link for a playback link DMA stream tag
   (snd_hdac_ext_bus_link_set_stream_id / clear_stream_id) */
void dsp_link_ssp_out(struct dsp *d, int tag, int on)
{
    u8 *m = ml_link(d, ML_ID_SSP);
    u16 v;
    if (!m || tag <= 0 || tag > 15) return;
    v = RD16(m + ML_LOSIDV);
    if (on) v |= (u16)(1U << tag);
    else v &= (u16)~(1U << tag);
    WR16(m + ML_LOSIDV, v);
}

/* processing pipe registers of stream 'idx': host (PPHC) and link (PPLC) */
#define PPCTL           0x04
#define PPCTL_GPROCEN   (1UL << 30)
#define PPLC(d, i)      ((d)->pp + 0x10 + 0x10 * ((d)->nin + (d)->nout) + 0x10 * (i))
#define PPLCCTL         0x00
#define PPLCFMT         0x04
#define SPIB_CTL        0x04
#define SPIB_POS(i)     (0x08 + 0x08 * (i))

/* ---------------------------------------------------------------- host DMA streams */

/* a free host stream descriptor, the first one or (top) the last one */
static int stream_alloc(struct dsp *d, int dir, int top)
{
    int i, lo = dir ? 0 : d->nin, hi = dir ? d->nin : d->nin + d->nout;
    for (i = lo; i < hi; i++) {
        int k = top ? hi - 1 - (i - lo) : i;
        if (!(d->streams_used & (1U << k))) {
            d->streams_used |= (u16)(1U << k);
            return k;
        }
    }
    return -1;
}

/* streams of the sound devices from the last descriptor down: hda.c plays and records
   through the first output and input descriptors (coupled) */
int dsp_stream_get(struct dsp *d, int dir)
{
    return stream_alloc(d, dir, 1);
}

/* host and link DMA of a descriptor both free: coupled again (plain HD Audio stream) */
static void recouple(struct dsp *d, int idx)
{
    if (d->pp && !((d->streams_used | d->links_used) & (1U << idx)))
        WR32(d->pp + PPCTL, RD32(d->pp + PPCTL) & ~(1UL << idx));
}

void dsp_stream_put(struct dsp *d, int idx)
{
    if (idx < 0) return;
    d->streams_used &= (u16)~(1U << idx);
    recouple(d, idx);
}

static int stream_tag(struct dsp *d, int idx)
{
    return idx < d->nin ? idx + 1 : idx - d->nin + 1;
}

static int sd_wait_run(struct dsp *d, int idx, int run)
{
    u32 t0 = plat_ms();
    while (((RD8(d->hda + SD(idx) + SD_CTL) & SDC_RUN) != 0) != (run != 0))
        if (plat_ms() - t0 > 20) return -1;
    return 0;
}

static int sd_reset(struct dsp *d, int idx)
{
    u8 *s = d->hda + SD(idx);
    u32 t0;
    WR8(s + SD_CTL, RD8(s + SD_CTL) | SDC_SRST);
    t0 = plat_ms();
    while (!(RD8(s + SD_CTL) & SDC_SRST)) if (plat_ms() - t0 > 20) return -1;
    WR8(s + SD_CTL, RD8(s + SD_CTL) & ~SDC_SRST);
    t0 = plat_ms();
    while (RD8(s + SD_CTL) & SDC_SRST) if (plat_ms() - t0 > 20) return -1;
    return 0;
}

/* host stream 'idx' decoupled for the DSP, BDL over buf (period bytes per entry, or the
   firmware image segments when buf is NULL), format and software position in buffer */
static int stream_setup(struct dsp *d, int idx, u32 *bdl, int nbdl, u32 size, u16 fmt, int spib)
{
    u8 *s = d->hda + SD(idx);
    u32 mask = 1UL << idx;
    WR32(d->pp + PPCTL, RD32(d->pp + PPCTL) | mask);           /* decouple host and link DMA */
    WR8(s + SD_CTL, RD8(s + SD_CTL) & ~(SDC_RUN | SDC_INTS));
    sd_wait_run(d, idx, 0);
    WR8(s + SD_STS, 0x1C);
    if (sd_reset(d, idx)) return -1;
    WR32(s + SD_BDLPL, 0);
    WR32(s + SD_BDLPU, 0);
    WR32(s + 0, (RD32(s + 0) & ~0x00F00000UL) | ((u32)stream_tag(d, idx) << 20));
    WR32(s + SD_CBL, size);
    if (d->pl->flags & DPF_PROCEN) WR32(d->pp + PPCTL, RD32(d->pp + PPCTL) & ~mask);
    WR16(s + SD_FMT, fmt);
    if (d->pl->flags & DPF_PROCEN) WR32(d->pp + PPCTL, RD32(d->pp + PPCTL) | mask);
    WR16(s + SD_LVI, (u16)(nbdl - 1));
    WR32(s + SD_BDLPL, vphys(bdl));
    WR32(s + SD_BDLPU, 0);
    if (d->spib) {
        if (spib) {
            WR32(d->spib + SPIB_CTL, RD32(d->spib + SPIB_CTL) | mask);
            WR32(d->spib + SPIB_POS(idx), size);
        } else {
            WR32(d->spib + SPIB_POS(idx), 0);
            WR32(d->spib + SPIB_CTL, RD32(d->spib + SPIB_CTL) & ~mask);
        }
    }
    return 0;
}

int dsp_stream_prepare(struct dsp *d, int idx, u32 *bdl, u8 *buf, u32 size, u32 period, u16 fmt)
{
    u32 n = size / period, i;
    for (i = 0; i < n; i++) {
        bdl[i * 4] = vphys(buf + i * period);
        bdl[i * 4 + 1] = 0;
        bdl[i * 4 + 2] = period;
        bdl[i * 4 + 3] = 0;
    }
    if (!n) return -1;
    return stream_setup(d, idx, bdl, (int)n, size, fmt, 0);
}

void dsp_stream_run(struct dsp *d, int idx, int run)
{
    u8 *s = d->hda + SD(idx);
    if (run) {
        WR8(s + SD_STS, 0x1C);
        WR8(s + SD_CTL, RD8(s + SD_CTL) | SDC_RUN);
    } else {
        WR8(s + SD_CTL, RD8(s + SD_CTL) & ~SDC_RUN);
        sd_wait_run(d, idx, 0);
        WR8(s + SD_STS, 0x1C);
    }
}

/* decoupled host DMA: the DMA position in buffer register (LPIB follows the link side) */
u32 dsp_stream_pos(struct dsp *d, int idx)
{
    return RD32(d->hda + SD_DPIB(idx));
}

/* ---------------------------------------------------------------- HD Audio link DMA */

int dsp_link_get(struct dsp *d, int dir)
{
    int i, lo = dir ? 0 : d->nin, hi = dir ? d->nin : d->nin + d->nout;
    for (i = hi - 1; i >= lo; i--)
        if (!(d->links_used & (1U << i))) {
            d->links_used |= (u16)(1U << i);
            return i;
        }
    return -1;
}

void dsp_link_put(struct dsp *d, int idx)
{
    if (idx < 0) return;
    d->links_used &= (u16)~(1U << idx);
    recouple(d, idx);
}

int dsp_link_tag(struct dsp *d, int idx)
{
    return stream_tag(d, idx);
}

void dsp_link_prepare(struct dsp *d, int idx, int tag, u16 fmt)
{
    u8 *l = PPLC(d, idx);
    u32 t0;
    WR32(d->pp + PPCTL, RD32(d->pp + PPCTL) | (1UL << idx));   /* decoupled */
    WR32(l + PPLCCTL, RD32(l + PPLCCTL) | 1);                  /* link stream reset */
    t0 = plat_ms();
    while (!(RD32(l + PPLCCTL) & 1) && plat_ms() - t0 < 20) ;
    WR32(l + PPLCCTL, RD32(l + PPLCCTL) & ~1UL);
    t0 = plat_ms();
    while ((RD32(l + PPLCCTL) & 1) && plat_ms() - t0 < 20) ;
    WR32(l + PPLCCTL, (RD32(l + PPLCCTL) & ~0x00F00000UL) | ((u32)tag << 20));
    WR16(l + PPLCFMT, fmt);
}

void dsp_link_run(struct dsp *d, int idx, int run)
{
    u8 *l = PPLC(d, idx);
    u32 t0 = plat_ms();
    if (run) WR32(l + PPLCCTL, RD32(l + PPLCCTL) | 2);
    else WR32(l + PPLCCTL, RD32(l + PPLCCTL) & ~2UL);
    while (((RD32(l + PPLCCTL) & 2) != 0) != (run != 0) && plat_ms() - t0 < 20) ;
}

/* ---------------------------------------------------------------- DSP cores (cAVS) */

static int core_power(struct dsp *d, u32 mask, int on)
{
    dupd(d, ADSPCS, CS_SPA(mask), on ? CS_SPA(mask) : 0);
    mdelay(1);
    return dpoll(d, ADSPCS, CS_CPA(mask), on ? CS_CPA(mask) : 0, 20);
}

static int core_reset(struct dsp *d, u32 mask, int reset)
{
    dupd(d, ADSPCS, CS_CRST(mask), reset ? CS_CRST(mask) : 0);
    return dpoll(d, ADSPCS, CS_CRST(mask), reset ? CS_CRST(mask) : 0, 20);
}

static int core_stall(struct dsp *d, u32 mask, int stall)
{
    dupd(d, ADSPCS, CS_CSTALL(mask), stall ? CS_CSTALL(mask) : 0);
    if (dpoll(d, ADSPCS, CS_CSTALL(mask), stall ? CS_CSTALL(mask) : 0, 20)) return -1;
    mdelay(1);
    return 0;
}

static int core_enabled(struct dsp *d, u32 mask)
{
    u32 v = drd(d, ADSPCS);
    return v != 0xFFFFFFFFUL && (v & (CS_CPA(mask) | CS_CRST(mask) | CS_CSTALL(mask) | CS_SPA(mask))) ==
           (CS_CPA(mask) | CS_SPA(mask));
}

static int core_run(struct dsp *d, u32 mask)
{
    if (core_reset(d, mask, 0)) return -1;
    core_stall(d, mask, 0);
    return core_enabled(d, mask) ? 0 : -1;
}

static void core_down(struct dsp *d, u32 mask)
{
    if (!mask) return;
    core_stall(d, mask, 1);
    core_reset(d, mask, 1);
    core_power(d, mask, 0);
}

/* ---------------------------------------------------------------- DSP subsystem (ACE) */

static int ace_core_up(struct dsp *d)
{
    if (drd(d, MTL_DSP2CXCTL) & MTL_CX_CPA) return 0;
    dupd(d, MTL_DSP2CXCTL, 3UL << 24, 2UL << 24);             /* owner: host CPU */
    dupd(d, MTL_DSP2CXCTL, MTL_CX_SPA, MTL_CX_SPA);
    mdelay(1);
    return dpoll(d, MTL_DSP2CXCTL, MTL_CX_CPA, MTL_CX_CPA, 50);
}

/* primary core off, the DSP subsystem stays powered (mtl_dsp_core_power_down) */
static void ace_core_down(struct dsp *d)
{
    dupd(d, MTL_DSP2CXCTL, MTL_CX_SPA, 0);
    mdelay(1);
    dpoll(d, MTL_DSP2CXCTL, MTL_CX_CPA, 0, 50);
}

static void ace_down(struct dsp *d)
{
    ace_core_down(d);
    dupd(d, MTL_HFDSSCS, MTL_DSS_SPA, 0);
    mdelay(1);
    dpoll(d, MTL_HFDSSCS, MTL_DSS_CPA, 0, 50);
}

static int ace_subsystem_up(struct dsp *d)
{
    u32 ctl = d->pl->gen >= DG_PTL ? PTL_HFPWRCTL2 : MTL_HFPWRCTL;
    u32 sts = d->pl->gen >= DG_PTL ? PTL_HFPWRSTS2 : MTL_HFPWRSTS;
    if (drd(d, MTL_HFDSSCS) & MTL_DSS_CPA) ace_down(d);
    dupd(d, MTL_HFDSSCS, MTL_DSS_SPA, MTL_DSS_SPA);
    mdelay(1);
    if (dpoll(d, MTL_HFDSSCS, MTL_DSS_CPA, MTL_DSS_CPA, 50)) return -1;
    dupd(d, ctl, 1, 1);                                         /* DSP0 power domain on */
    mdelay(1);
    if (dpoll(d, sts, 1, 1, 50)) return -1;
    if (d->pl->gen >= DG_MTL) dupd(d, MTL_HFPWRCTL, 0xF00, 0xF00);  /* I/O (SoundWire) domains */
    return 0;
}

/* ---------------------------------------------------------------- IPC transport */

static void ipc_ack_done(struct dsp *d)
{
    /* the DSP's acknowledge of our request: write the DONE bit back (SST: clear it) */
    if (d->shim) dwr(d, d->r_ack, drd(d, d->r_ack) & ~d->r_ackdone);
    else dwr(d, d->r_ack, drd(d, d->r_ack) | d->r_ackdone);
}

static void ipc_host_done(struct dsp *d)
{
    /* we took the DSP's message (SST: BUSY cleared and DONE set, the DSP clears DONE) */
    if (d->shim) {
        dwr(d, d->r_rsp, (drd(d, d->r_rsp) & ~0x80000000UL) | 0x40000000UL);
        return;
    }
    dwr(d, d->r_rsp, drd(d, d->r_rsp) | 0x80000000UL);
    if (d->r_rspack) {
        if (d->pl->gen >= DG_MTL) dwr(d, d->r_rspack, drd(d, d->r_rspack) & ~0x80000000UL);
        else dwr(d, d->r_rspack, drd(d, d->r_rspack) | 0x80000000UL);
    }
}

/* a message from the DSP: 1 if one was there (primary / ext in d->rsp_*) */
static int ipc_rx(struct dsp *d)
{
    u32 v = drd(d, d->r_rsp);
    if (v == 0xFFFFFFFFUL || !(v & 0x80000000UL)) return 0;
    d->rsp_primary = v & 0x7FFFFFFFUL;
    d->rsp_ext = d->r_rspext ? drd(d, d->r_rspext) & (d->shim ? 0xFFFFFFFFUL : 0x3FFFFFFFUL) : 0;
    return 1;
}

static void ipc4_notification(struct dsp *d, u32 primary, u32 ext)
{
    u32 type = (primary >> 16) & 0xFF;
    (void)ext;
    if (type == 5) DBG("dsp: resource event %08lx %08lx\n", primary, ext);
    else if (type == 10) {
        kprintf("dsp: firmware exception (%08lx %08lx)\n", primary, ext);
        d->failed = 1;
    }
    else if (type != 6) DBG("dsp: notification %lu (%08lx %08lx)\n", type, primary, ext);
}

/* IPC4 request: payload tx, reply payload into rx (rxmax bytes); returns the status (0 ok),
   -1 on timeout */
int dsp_ipc(struct dsp *d, u32 primary, u32 ext, const void *tx, u32 txlen, void *rx, u32 rxmax, u32 *rxlen)
{
    u32 t0;
    int acked = 0;
    if (d->failed) return -1;
    /* SOF: the DSP still owns the previous request until it clears BUSY (Linux
       hda_ipc4_tx_is_busy); a DONE that came after its reply is acknowledged here */
    t0 = plat_ms();
    while (!d->avs && (drd(d, d->r_req) & 0x80000000UL)) {
        if (drd(d, d->r_ack) & d->r_ackdone) ipc_ack_done(d);
        if (plat_ms() - t0 > 100) {
            kprintf("dsp: ipc %08lx/%08lx: DSP busy\n", primary, ext);
            dump_state(d, "ipc");
            return -1;
        }
        udelay(5);
    }
    if (drd(d, d->r_ack) & d->r_ackdone) ipc_ack_done(d);
    if (txlen) dsp_mbox_write(d, d->hostbox, tx, txlen);
    dwr(d, d->r_reqext, ext & 0x3FFFFFFFUL);
    dwr(d, d->r_req, (primary & 0x7FFFFFFFUL) | 0x80000000UL);
    t0 = plat_ms();
    for (;;) {
        if (!acked && (drd(d, d->r_ack) & d->r_ackdone)) {
            ipc_ack_done(d);
            acked = 1;
        }
        if (ipc_rx(d)) {
            u32 p = d->rsp_primary, e = d->rsp_ext;
            /* a reply: direction bit set and not a notification (global type 27) */
            if ((p & (1UL << 29)) && ((p >> 24) & 0x1F) != IPC4_NOTIFY) {
                u32 st = p & 0xFFFFFF;
                if (rx && rxmax) {
                    u32 n = rxmax;
                    /* LARGE_CONFIG_GET: the size comes in the extension */
                    if (((p >> 24) & 0x1F) == 3 && (p & (1UL << 30))) n = MIN(rxmax, e & 0xFFFFF);
                    dsp_mbox_read(d, d->dspbox, rx, n);
                    if (rxlen) *rxlen = n;
                }
                ipc_host_done(d);
                if (!acked && (drd(d, d->r_ack) & d->r_ackdone)) ipc_ack_done(d);
                if (st) DBG("dsp: ipc %08lx/%08lx status %lu\n", primary, ext, st);
                return (int)st;
            }
            ipc4_notification(d, p, e);
            ipc_host_done(d);
        }
        if (plat_ms() - t0 > 500) {
            kprintf("dsp: ipc %08lx/%08lx timeout\n", primary, ext);
            dump_state(d, "ipc");
            return -1;
        }
        udelay(5);
    }
}

/* SOF IPC3 panic: magic 0x0dead in bits 12-27 of the message word (SOF_IPC_PANIC_MAGIC);
   the firmware is dead, no more requests */
static int sof_panic(struct dsp *d)
{
    u32 m = d->shim == SST_BYT ? d->rsp_ext : d->rsp_primary;
    if ((m & 0x0FFFF000UL) != 0x0DEAD000UL) return 0;
    kprintf("dsp: firmware panic %08lx\n", m);
    d->failed = 1;
    return 1;
}

/* IPC3 request: the message (sof_ipc_cmd_hdr first) in tx, the reply into rx; returns the
   reply's error field (0 ok), -1 on timeout */
int dsp_ipc3(struct dsp *d, const void *tx, u32 txlen, void *rx, u32 rxmax)
{
    u32 t0, rep[3];
    if (d->failed) return -1;
    dsp_mbox_write(d, d->hostbox, tx, txlen);
    if (d->shim == SST_BYT) dwr(d, d->r_reqext, 0);   /* 64-bit IPCX: low dword first */
    dwr(d, d->r_req, 0x80000000UL);
    t0 = plat_ms();
    for (;;) {
        if (drd(d, d->r_ack) & d->r_ackdone) {
            dsp_mbox_read(d, d->hostbox, rep, sizeof(rep));
            if (rx && rxmax) dsp_mbox_read(d, d->hostbox, rx, MIN(rxmax, rep[0] ? rep[0] : rxmax));
            ipc_ack_done(d);
            if ((s32)rep[2] < 0) DBG("dsp: ipc3 %08lx error %ld\n", ((u32 *)tx)[1], (s32)rep[2]);
            return (s32)rep[2] < 0 ? (int)(s32)rep[2] : 0;
        }
        if (ipc_rx(d)) {
            /* position / trace messages: not used */
            int dead = sof_panic(d);
            ipc_host_done(d);
            if (dead) return -1;
        }
        if (plat_ms() - t0 > 500) {
            kprintf("dsp: ipc3 %08lx timeout\n", ((u32 *)tx)[1]);
            dump_state(d, "ipc3");
            return -1;
        }
        udelay(5);
    }
}

/* FW_READY from the firmware after it started */
int dsp_wait_fw_ready(struct dsp *d)
{
    u32 t0 = plat_ms();
    for (;;) {
        if (ipc_rx(d)) {
            u32 p = d->rsp_primary;
            if (d->ipc4) {
                if (((p >> 24) & 0x1F) == IPC4_NOTIFY && ((p >> 16) & 0xFF) == IPC4_FW_READY) {
                    ipc_host_done(d);
                    return 0;
                }
                ipc_host_done(d);
            } else {
                u32 hdr[2];
                dsp_mbox_read(d, d->dspbox, hdr, 8);
                if ((hdr[1] & 0xF0000000UL) == 0x70000000UL) return 0;     /* SOF_IPC_FW_READY */
                ipc_host_done(d);
            }
        }
        if (plat_ms() - t0 > 3000) return -1;
        udelay(20);
    }
}

/* ---------------------------------------------------------------- firmware image */

/* the image the ROM loads starts at the first 4K of the buffer: the extended manifest
   ($AE1 for IPC4, XMan for SOF IPC3) is removed in place (the buffer keeps that form, a
   second boot finds the bare image) */
#define FW_STRIPPED 0x50525453UL                     /* "STRP" */

static int fw_strip(struct dsp *d)
{
    u32 m = *(u32 *)d->fw.v, off = 0, *tail;
    if (d->fw.size < 0x1000) return -1;
    tail = (u32 *)(d->fw.v + ((d->fw.size - 8) & ~3UL));
    if (m == 0x31454124UL) off = ((u32 *)d->fw.v)[1];          /* $AE1 */
    else if (m == 0x6E614D58UL) off = ((u32 *)d->fw.v)[1];     /* XMan: full size */
    else if (tail[0] == FW_STRIPPED && tail[1] < d->fw.size) d->fw.size = tail[1];  /* done before */
    if (off >= d->fw.size) return -1;
    if (off) {
        u32 i, n = d->fw.size - off;
        u32 *dst = (u32 *)d->fw.v, *src = (u32 *)(d->fw.v + off);
        if (off & 3) {
            for (i = 0; i < n; i++) d->fw.v[i] = d->fw.v[off + i];
        } else {
            for (i = 0; i < (n + 3) / 4; i++) dst[i] = src[i];
        }
        /* the image is shared (DOS loads it once for the DOS core and the VxD): the new size
           goes to the end of the old one for the next start */
        if (off >= 8) {
            tail[0] = FW_STRIPPED;
            tail[1] = n;
        }
        d->fw.size = n;
    }
    m = *(u32 *)d->fw.v;
    if (m != 6 && m != 0x44504324UL) {                          /* SKL manifest / $CPD */
        kprintf("dsp: %s: unknown image (%08lx)\n", d->fwname, m);
        return -1;
    }
    return 0;
}

/* IPC4: the module table of the ADSP manifest ($AM1): the copier and its configurations */
static int fw_modules(struct dsp *d)
{
    u32 mo = *(u32 *)d->fw.v == 6 ? 0x284 : 0x2000, hlen, n, i;
    const u8 *h = d->fw.v + mo, *e;
    if (mo + 0x40 > d->fw.size || *(const u32 *)h != 0x314D4124UL) {
        kprintf("dsp: %s: no ADSP manifest\n", d->fwname);
        return -1;
    }
    hlen = *(const u32 *)(h + 4);
    n = *(const u32 *)(h + 36);
    kprintf("dsp: %s: firmware %u.%u.%u.%u, %lu modules\n", d->fwname, *(const u16 *)(h + 28),
            *(const u16 *)(h + 30), *(const u16 *)(h + 32), *(const u16 *)(h + 34), n);
    if (n > 256 || mo + hlen + n * 116 > d->fw.size) return -1;
    e = h + hlen;
    d->modcfg = e + n * 116;
    d->copier_id = 0xFFFF;
    for (i = 0; i < n; i++, e += 116) {
        static const u8 copier[16] = { 0x83, 0x0C, 0xA0, 0x9B, 0x12, 0xCA, 0x83, 0x4A,
                                       0x94, 0x3C, 0x1F, 0xA2, 0xE8, 0x2F, 0x9D, 0xDA };
        if (!memcmp(e + 12, copier, 16) || !memcmp(e + 4, "COPIER\0", 7)) {
            d->copier_id = (u16)i;
            d->copier_cfg = *(const u16 *)(e + 68);
            d->copier_ncfg = *(const u16 *)(e + 70);
        }
    }
    if (d->copier_id == 0xFFFF) {
        kprintf("dsp: %s: no copier module\n", d->fwname);
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- boot */

/* clock gating, L1 and power gating off while the firmware loads; the Intel (AVS) firmware
   also wants MISCBDCGE off and LSRMD set (Linux avs_load_firmware) */
static void gating(struct dsp *d, int on)
{
    struct pcidev *p = &d->pci;
    u32 cg = CGCTL_ADSPDCGE | (d->avs ? CGCTL_MISCBDCGE : 0), pg = PGCTL_ADSPPGD | (d->avs ? PGCTL_LSRMD : 0);
    u32 v = pci_rd32(p->bus, p->dev, p->fn, PCI_CGCTL);
    pci_wr32(p->bus, p->dev, p->fn, PCI_CGCTL, on ? v | cg : v & ~cg);
    hupd(d, HDA_EM2, EM2_L1SEN, on ? EM2_L1SEN : 0);
    v = pci_rd32(p->bus, p->dev, p->fn, PCI_PGCTL);
    pci_wr32(p->bus, p->dev, p->fn, PCI_PGCTL, on ? v & ~pg : v | pg);
}

/* SSP ports clock / frame consumer while the ROM runs (SOF does it before every boot) */
static void ssp_consumer(struct dsp *d)
{
    u32 base = d->pl->gen == DG_APL ? 0x2000 : 0x10000, i;
    if (d->pl->gen >= DG_LNL || d->pl->gen == DG_SKL) return;
    for (i = 0; i < d->pl->ssps; i++) dupd(d, base + i * 0x1000 + 4, 3UL << 24, 3UL << 24);
}

/* code loader stream stopped (hda_cldma_stop) */
static void cl_stop(struct dsp *d)
{
    u8 *cl = d->bar + CL_SD;
    u32 t0;
    WR8(cl + SD_CTL, RD8(cl + SD_CTL) & ~(SDC_RUN | SDC_INTS));
    t0 = plat_ms();
    while ((RD8(cl + SD_CTL) & SDC_RUN) && plat_ms() - t0 < 2) ;
}

/* code loader stream stopped and reset (hda_cldma_reset) */
static void cl_reset(struct dsp *d)
{
    u8 *cl = d->bar + CL_SD;
    u32 t0;
    cl_stop(d);
    WR8(cl + SD_CTL, RD8(cl + SD_CTL) | SDC_SRST);
    t0 = plat_ms();
    while (!(RD8(cl + SD_CTL) & SDC_SRST) && plat_ms() - t0 < 2) ;
    WR8(cl + SD_CTL, RD8(cl + SD_CTL) & ~SDC_SRST);
    t0 = plat_ms();
    while ((RD8(cl + SD_CTL) & SDC_SRST) && plat_ms() - t0 < 2) ;
    WR8(cl + SD_STS, 0x1C);
}

/* Skylake / Kaby Lake: code loader DMA (Linux avs_cldma_load_basefw): main core powered and
   out of reset but stalled while the code loader stream is set up, then running; the ROM
   reports INIT_DONE and pulls the image through the 128K ring up to the software position */
static int boot_skl(struct dsp *d)
{
    u8 *buf, *cl = d->bar + CL_SD;
    u32 *bdl, left = d->fw.size, pos = 0;
    int ret = -1, attempt;
    buf = kmalloc_align(CL_BUFSIZE, 4096);
    bdl = kmalloc_align(64, 128);
    if (!buf || !bdl) {
        kprintf("dsp: no memory for the code loader\n");
        kfree(buf);
        kfree(bdl);
        return -1;
    }
    for (attempt = 0; attempt < 2; attempt++) {
        core_down(d, 1);
        ret = core_power(d, 1, 1);
        if (!ret) ret = core_reset(d, 1, 0);
        if (ret) {
            dump_state(d, "core power");
            continue;
        }
        /* code loader stream: a 128K ring of two BDL entries, stream tag 1, SPIB on */
        cl_reset(d);
        bdl[0] = vphys(buf);
        bdl[1] = 0;
        bdl[2] = CL_BUFSIZE / 2;
        bdl[3] = 0;
        bdl[4] = vphys(buf + CL_BUFSIZE / 2);
        bdl[5] = 0;
        bdl[6] = CL_BUFSIZE / 2;
        bdl[7] = 1;
        aeng_flush_dma(bdl, 32);
        WR32(cl + SD_BDLPL, vphys(bdl));
        WR32(cl + SD_BDLPU, 0);
        WR32(cl + SD_CBL, CL_BUFSIZE);
        WR16(cl + SD_LVI, 1);
        WR32(cl, (RD32(cl) & ~0x00F00000UL) | (1UL << 20));
        dwr(d, CL_SPIB, 0);
        dwr(d, CL_SPBFCCTL, drd(d, CL_SPBFCCTL) | 1);
        ret = core_stall(d, 1, 0);
        if (!ret) ret = fsr_wait(d, FSR_INIT_DONE, d->pl->rominit);
        if (!ret) break;
        dump_state(d, "ROM init");
        core_down(d, 1);
    }
    if (ret) goto out;
    /* the image in 128K pieces; the ROM consumes each up to the software position */
    while (left) {
        u32 n = MIN(left, CL_BUFSIZE), t0;
        memcpy(buf, d->fw.v + pos, n);
        aeng_flush_dma(buf, n);
        WR8(cl + SD_STS, SDS_BCIS);
        dwr(d, CL_SPIB, n);
        WR8(cl + SD_CTL, RD8(cl + SD_CTL) | SDC_RUN | SDC_INTS);
        left -= n;
        pos += n;
        if (!left) break;
        t0 = plat_ms();
        while (!(RD8(cl + SD_STS) & SDS_BCIS)) {
            if ((drd(d, d->r_rom) & FSR_MASK_SKL) == FSR_BASEFW_ENTERED) {
                left = 0;               /* the ROM took all it needs */
                break;
            }
            if (plat_ms() - t0 > 500) {
                dump_state(d, "code loader");
                ret = -1;
                goto stop;
            }
        }
    }
    ret = fsr_wait(d, FSR_BASEFW_ENTERED, 3000);
    if (ret) dump_state(d, "base firmware");
stop:
    cl_stop(d);
    dwr(d, CL_SPBFCCTL, drd(d, CL_SPBFCCTL) & ~1UL);
    dwr(d, CL_SPIB, 0);
    WR32(cl, RD32(cl) & ~0x00F00000UL);
    WR32(cl + SD_BDLPL, 0);
    WR32(cl + SD_CBL, 0);
    if (ret) core_down(d, 1);
out:
    kfree(buf);
    kfree(bdl);
    return ret;
}

/* ROM init for cAVS 1.5+ (cl_dsp_init) and ACE (mtl_dsp_cl_init) */
static int rom_init(struct dsp *d, int tag)
{
    u32 ipc = 0x80000000UL | ROM_IPC_CONTROL | ROM_IPC_PURGE | ((u32)(tag - 1) << 9);
    if (d->pl->gen >= DG_MTL) {
        dwr(d, d->r_req, ipc);
        if (ace_core_up(d)) {
            dump_state(d, "core power");
            goto fail;
        }
    } else {
        if (core_power(d, d->pl->hostcores, 1)) return -1;
        ssp_consumer(d);
        dwr(d, d->r_req, ipc);
        if (core_run(d, 1)) {
            core_down(d, d->pl->hostcores);
            return -1;
        }
    }
    if (dpoll(d, d->r_ack, d->r_ackdone, d->r_ackdone, 500)) {
        dump_state(d, "ROM acknowledge");
        goto fail;
    }
    ipc_ack_done(d);
    if (d->pl->gen < DG_MTL) core_down(d, d->pl->hostcores & ~1U);
    if (fsr_wait(d, FSR_INIT_DONE, d->pl->rominit)) {
        dump_state(d, "ROM init");
        goto fail;
    }
    return 0;
fail:
    /* ACE: only the core goes down, the next attempt runs on the powered subsystem */
    if (d->pl->gen >= DG_MTL) ace_core_down(d);
    else core_down(d, d->pl->hostcores);
    return -1;
}

/* ICCMAX: a capture stream running during the boot of Ice Lake and Tiger Lake (current
   budget), LTR guardband at 95 us */
static int iccmax_start(struct dsp *d, int *idx, u8 *gb)
{
    int i = stream_alloc(d, 1, 0);
    if (i < 0) return -1;
    if (!d->iccbuf) {
        d->iccbuf = kmalloc_align(4096, 4096);
        d->iccbdl = kmalloc_align(64, 128);
        if (!d->iccbuf || !d->iccbdl) return -1;
    }
    d->iccbdl[0] = vphys(d->iccbuf);
    d->iccbdl[2] = 2048;
    d->iccbdl[4] = vphys(d->iccbuf + 2048);
    d->iccbdl[6] = 2048;
    aeng_flush_dma(d->iccbdl, 32);
    {
        u8 *s = d->hda + SD(i);
        WR32(s + SD_BDLPL, vphys(d->iccbdl));
        WR32(s + SD_BDLPU, 0);
        WR32(s + SD_CBL, 4096);
        WR16(s + SD_LVI, 1);
    }
    WR32(d->pp + PPCTL, RD32(d->pp + PPCTL) | (1UL << i));
    *gb = RD8(d->hda + HDA_LTRP) & 0x3F;
    WR8(d->hda + HDA_LTRP, (RD8(d->hda + HDA_LTRP) & ~0x3F) | 95);
    WR8(d->hda + SD(i) + SD_CTL, RD8(d->hda + SD(i) + SD_CTL) | SDC_RUN);
    *idx = i;
    return 0;
}

static void iccmax_stop(struct dsp *d, int idx, u8 gb)
{
    WR8(d->hda + SD(idx) + SD_CTL, RD8(d->hda + SD(idx) + SD_CTL) & ~SDC_RUN);
    sd_wait_run(d, idx, 0);
    WR8(d->hda + HDA_LTRP, (RD8(d->hda + HDA_LTRP) & ~0x3F) | gb);
    dsp_stream_put(d, idx);
}

/* cAVS 1.5+ and ACE: host DMA code loading */
static int boot_hostdma(struct dsp *d)
{
    u32 *bdl;
    int idx, i, n = 0, ret = -1, icc = -1;
    u8 gb = 0;
    idx = stream_alloc(d, 0, 0);
    bdl = kmalloc_align(256 * 16, 128);
    if (idx < 0 || !bdl) {
        kprintf("dsp: no stream for the code loader\n");
        dsp_stream_put(d, idx);
        kfree(bdl);
        return -1;
    }
    /* BDL over the image: physical segments in pieces of at most 64K (128-byte aligned) */
    {
        u32 left = d->fw.size;
        for (i = 0; i < d->fw.nseg && left && n < 256; i++) {
            u32 p = d->fw.seg[i].phys, l = MIN(d->fw.seg[i].len, left);
            left -= l;
            while (l && n < 256) {
                u32 c = MIN(l, 0x10000UL);
                bdl[n * 4] = p;
                bdl[n * 4 + 1] = 0;
                bdl[n * 4 + 2] = c;
                bdl[n * 4 + 3] = 0;
                n++;
                p += c;
                l -= c;
            }
        }
        if (left || n < 2) {
            /* one piece: HD Audio needs two entries */
            if (n == 1 && !left && bdl[2] >= 256) {
                u32 h = (bdl[2] / 2) & ~127UL;
                bdl[4] = bdl[0] + h;
                bdl[5] = 0;
                bdl[6] = bdl[2] - h;
                bdl[7] = 0;
                bdl[2] = h;
                n = 2;
            } else {
                kprintf("dsp: firmware image too fragmented\n");
                goto out;
            }
        }
        bdl[(n - 1) * 4 + 3] = 1;
    }
    aeng_flush_dma(d->fw.v, d->fw.size);
    aeng_flush_dma(bdl, (u32)n * 16);
    if (stream_setup(d, idx, bdl, n, d->fw.size, 0x0040, 1)) goto out;
    if (d->pl->gen >= DG_MTL && ace_subsystem_up(d)) {
        dump_state(d, "DSP subsystem power");
        goto out;
    }
    if ((d->pl->flags & DPF_ICCMAX) && iccmax_start(d, &icc, &gb)) icc = -1;
    for (i = 0; i < BOOT_ATTEMPTS; i++) {
        ret = rom_init(d, stream_tag(d, idx));
        if (!ret) break;
    }
    if (ret) {
        kprintf("dsp: ROM init failed\n");
        goto stop;
    }
    WR8(d->hda + SD(idx) + SD_CTL, RD8(d->hda + SD(idx) + SD_CTL) | SDC_RUN | SDC_INTS);
    ret = fsr_wait(d, FSR_FW_ENTERED, 3000);
    if (ret) dump_state(d, "firmware load");
stop:
    dsp_stream_run(d, idx, 0);
    WR8(d->hda + SD(idx) + SD_CTL, RD8(d->hda + SD(idx) + SD_CTL) & ~SDC_INTS);
    if (d->spib) {
        WR32(d->spib + SPIB_POS(idx), 0);
        WR32(d->spib + SPIB_CTL, RD32(d->spib + SPIB_CTL) & ~(1UL << idx));
    }
    if (icc >= 0) iccmax_stop(d, icc, gb);
out:
    dsp_stream_put(d, idx);
    kfree(bdl);
    return ret;
}

static void set_regs(struct dsp *d)
{
    int g = d->pl->gen;
    if (g == DG_SKL || g == DG_APL) {
        d->r_req = 0x48; d->r_reqext = 0x4C; d->r_ack = 0x4C; d->r_ackdone = 0x40000000UL;
        d->r_rsp = 0x40; d->r_rspext = 0x44; d->r_rspack = 0; d->r_ctl = 0x50;
    } else if (g < DG_MTL) {
        d->r_req = 0xD0; d->r_reqext = 0xD8; d->r_ack = 0xD4; d->r_ackdone = 0x80000000UL;
        d->r_rsp = 0xC0; d->r_rspext = 0xC8; d->r_rspack = 0xC4; d->r_ctl = 0xE8;
    } else {
        d->r_req = 0x73210; d->r_reqext = 0x73380; d->r_ack = 0x73214; d->r_ackdone = 0x80000000UL;
        d->r_rsp = 0x73200; d->r_rspext = 0x73300; d->r_rspack = 0x73204; d->r_ctl = 0x73228;
    }
    if (g == DG_SKL) {
        d->win0 = 0x8000; d->winsz = 0x2000;
    } else if (g < DG_MTL) {
        d->win0 = 0x80000; d->winsz = 0x20000;
    } else {
        d->win0 = 0x180000; d->winsz = 0x8000;
    }
    d->r_rom = g == DG_MTL ? 0x163200UL : g >= DG_LNL ? 0x160200UL : d->win0;
    d->r_romerr = d->r_rom + 4;
    /* IPC4 and the IPC3 FW_READY: DSP -> host in window 0 + 4K, host -> DSP window 1 */
    d->dspbox = d->win0 + 0x1000;
    d->hostbox = d->win0 + d->winsz;
    d->dspboxsz = d->hostboxsz = 0x1000;
}

static u32 bar_len(int gen)
{
    return gen == DG_SKL ? 0x10000UL : gen < DG_MTL ? 0x100000UL : 0x200000UL;
}

/* the firmware file: production signed, then community signed */
static int fw_get(struct dsp *d, int alt)
{
    const char *n = alt ? d->pl->fwc : d->pl->fw;
    if (!n) return -1;
    memset(&d->fw, 0, sizeof(d->fw));
    if (plat_fw_load(n, &d->fw)) return -1;
    d->fwname = n;
    kprintf("dsp: %s, %lu bytes\n", n, d->fw.size);
    if (fw_strip(d)) return -1;
    if (d->ipc4 && fw_modules(d)) return -1;
    return 0;
}

static int boot(struct dsp *d)
{
    int ret, alt;
    for (alt = 0; alt < 2; alt++) {
        if (fw_get(d, alt)) {
            if (!alt) kprintf("dsp: firmware %s not found next to the driver\n", d->pl->fw);
            continue;
        }
        if (d->pl->gen < DG_MTL) {
            if (core_enabled(d, d->pl->hostcores)) core_down(d, d->pl->hostcores);
            gating(d, 0);
        }
        ret = d->pl->gen == DG_SKL ? boot_skl(d) : boot_hostdma(d);
        if (!ret) {
            ret = dsp_wait_fw_ready(d);
            if (ret) dump_state(d, "FW_READY");
        }
        /* Ice Lake running on the high power ring oscillator (the SOF images, LPRO 0 in their
           manifest): core 3 powered and stalled (Linux icl_dsp_post_fw_run) */
        if (!ret && d->pl->gen == DG_ICL && d->pl->cores == 4 && !core_power(d, 8, 1) && !core_run(d, 8))
            core_stall(d, 8, 1);
        if (d->pl->gen < DG_MTL) gating(d, 1);
        if (!ret) return 0;
        kprintf("dsp: %s did not start%s\n", d->fwname, !alt && d->pl->fwc ? ", trying the community signed image" : "");
        if (d->pl->gen >= DG_MTL) ace_down(d);
        else core_down(d, d->pl->hostcores);
    }
    return -1;
}

/* ---------------------------------------------------------------- attach */

struct dsp *dsp_attach(struct pcidev *p, u8 *hda, int with_codecs)
{
    const struct dspplat *pl = dsp_platform(p->vendor, p->device);
    struct dsp *d;
    u32 phys;
    u16 gcap;
    if (!pl || pl->gen == DG_BYT) return NULL;
    phys = pci_bar_mem(p, 4, NULL);
    if (!phys) {
        kprintf("dsp: %02x:%02x.%u no DSP BAR\n", p->bus, p->dev, p->fn);
        return NULL;
    }
    d = kmalloc(sizeof(*d));
    if (!d) return NULL;
    memset(d, 0, sizeof(*d));
    d->pl = pl;
    d->pci = *p;
    d->hda = hda;
    d->bar = plat_mmio(phys, bar_len(pl->gen));
    d->buf = kmalloc_align(4096, 4096);
    if (!d->bar || !d->buf) goto fail;
    d->ipc4 = (pl->flags & DPF_IPC4) != 0;
    d->avs = (pl->flags & DPF_AVS) != 0;
    gcap = RD16(hda + GCAP);
    d->nin = (gcap >> 8) & 0xF;
    d->nout = (gcap >> 12) & 0xF;
    find_caps(d);
    if (!d->pp) {
        kprintf("dsp: %02x:%02x.%u no processing pipe capability\n", p->bus, p->dev, p->fn);
        goto fail;
    }
    pci_intx(p, 0);
    set_regs(d);
    WR32(d->pp + PPCTL, RD32(d->pp + PPCTL) | PPCTL_GPROCEN);
    ml_offload(d, 1);
    if (d->avs) hupd(d, HDA_EM2, EM2_DUM, EM2_DUM);           /* as Linux avs_hdac_bus_init_chip */
    kprintf("dsp: %02x:%02x.%u %s, %d in / %d out streams, %s firmware\n", p->bus, p->dev, p->fn, pl->name,
            d->nin, d->nout, d->avs ? "Intel (IPC4)" : d->ipc4 ? "SOF IPC4" : "SOF IPC3");
    if (boot(d)) {
        kprintf("dsp: the DSP did not start; I2S / SoundWire / digital microphones stay silent\n");
        goto fail;
    }
    d->booted = 1;
    if (d->ipc4 ? ipc4_ready(d) : ipc3_ready(d)) {
        kprintf("dsp: firmware does not answer\n");
        goto fail;
    }
    d->next = dsp_list;
    dsp_list = d;
    if (dspdev_setup(d, with_codecs) <= 0 && !with_codecs) kprintf("dsp: no endpoints\n");
    return d;
fail:
    if (d->booted) {
        if (pl->gen >= DG_MTL) ace_down(d);
        else core_down(d, pl->hostcores);
    }
    if (d->pp) {
        ml_offload(d, 0);
        WR32(d->pp + PPCTL, RD32(d->pp + PPCTL) & ~PPCTL_GPROCEN);
    }
    kfree(d->buf);
    kfree(d);
    return NULL;
}

void dsp_detach(struct dsp *d)
{
    struct dsp **pp;
    int i;
    for (i = 0; i < d->nep; i++) {
        if (!d->ep[i].running) continue;
        if (d->ipc4) ipc4_ep_stop(&d->ep[i]);
        else ipc3_ep_stop(&d->ep[i]);
    }
    if (d->shim) {
        sst_down(d);
        goto unlink;
    }
    if (d->pl->gen >= DG_MTL) ace_down(d);
    else core_down(d, d->pl->hostcores);
    ml_offload(d, 0);
    WR32(d->pp + PPCTL, RD32(d->pp + PPCTL) & ~PPCTL_GPROCEN);
unlink:
    for (pp = &dsp_list; *pp; pp = &(*pp)->next)
        if (*pp == d) {
            *pp = d->next;
            break;
        }
}

void dsp_poll_all(void)
{
    struct dsp *d;
    /* messages nobody waits for (positions, logs): taken so that the DSP can go on */
    for (d = dsp_list; d; d = d->next)
        if (d->booted && !d->failed && ipc_rx(d)) {
            if (d->ipc4) ipc4_notification(d, d->rsp_primary, d->rsp_ext);
            else sof_panic(d);
            ipc_host_done(d);
        }
}

void dsp_shutdown_all(void)
{
    while (dsp_list) dsp_detach(dsp_list);
}

/* ---------------------------------------------------------------- firmware files */

/* DOS start: the firmware files the machine's DSPs need are named in the control block;
   the DOS driver loads them into XMS before the sound devices start */
void dsp_fw_prefetch(struct pcidev *pcis, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        const struct dspplat *pl;
        if (pcis[i].cls != 4) continue;
        pl = dsp_platform(pcis[i].vendor, pcis[i].device);
        if (!pl || !pl->fw) continue;
        plat_fw_request(pl->fw);
        if (pl->fwc) plat_fw_request(pl->fwc);
        if (pl->gen >= DG_CNL && sdw_present()) plat_fw_request("CS42L43.BIN");  /* SoundWire codec patch */
    }
    sst_fw_prefetch(pcis, n);
}
