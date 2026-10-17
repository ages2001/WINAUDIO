/* dspsst.c - the audio DSPs of Bay Trail (LPE, ACPI 80860F28), Cherry Trail / Braswell
   (808622A8) and Broadwell (ADSP, INT3438), run with Sound Open Firmware (IPC3). They have no
   HD Audio controller: the host writes the firmware into the DSP memories and talks to it
   through the SST shim.
   - Bay Trail / Cherry Trail: a 2 MB BAR with IRAM at 0C0000, DRAM at 100000, the shim at
     140000 and the mailbox at 144000; 64-bit IPC doorbells (BUSY bit 63, DONE bit 62)
   - Broadwell: a 1 MB BAR with DRAM at 0, IRAM at 0A0000, the mailbox at 09E000 and the shim
     at 0FB000; 32-bit doorbells; clocks and power gating in the PCI configuration space,
     which ACPI gives as a second memory range
   The image (.ri: extended manifest, "Reef" header, modules of IRAM / DRAM blocks) is copied
   into the DSP with the core in reset and stalled, then the core runs and sends FW_READY.
   The DSP's DW-DMA reads and writes the ring of the sound engine through a page table of
   20-bit page numbers; the position comes from the DMA channel that walks the ring, else
   from the firmware's stream window. Codec clocks: Bay Trail / Cherry Trail SSP2 (Bay Trail
   CR: SSP0) and the 19.2 MHz PMC platform clock 3 as MCLK; Broadwell SSP0 and the 24 MHz
   MCLK of the DSP. Sequences after Linux sof/intel/byt.c, bdw.c, atom.c and the machine
   drivers (bytcr_rt5640.c, bdw_rt286.c, clk-pmc-atom.c). */
#include "dspcdc.h"

/* memory map of the DSP BAR */
struct sstmap {
    u32 iram, iramsz, dram, dramsz;
    u32 shim, mbox;
    u32 dmac[3];                    /* DW-DMA controllers (host transfers on one of them) */
    u32 barlen;
};
static const struct sstmap map_byt = { 0xC0000UL, 0x14000UL, 0x100000UL, 0x28000UL, 0x140000UL, 0x144000UL,
                                       { 0x98000UL, 0x9C000UL, 0 }, 0x200000UL };
static const struct sstmap map_cht = { 0xC0000UL, 0x14000UL, 0x100000UL, 0x28000UL, 0x140000UL, 0x144000UL,
                                       { 0x98000UL, 0x9C000UL, 0x94000UL }, 0x200000UL };
static const struct sstmap map_bdw = { 0xA0000UL, 0x50000UL, 0, 0xA0000UL, 0xFB000UL, 0x9E000UL,
                                       { 0xFE000UL, 0xFF000UL, 0 }, 0x100000UL };

static const struct dspplat pl_byt = { "byt", DG_BYT, 0, 1, 1, 3, 0, 0, "SOFBYT.RI", NULL };
static const struct dspplat pl_cht = { "cht", DG_BYT, 0, 1, 1, 6, 0, 0, "SOFCHT.RI", NULL };
static const struct dspplat pl_bdw = { "bdw", DG_BYT, 0, 1, 1, 2, 0, 0, "SOFBDW.RI", NULL };

static const struct { const char *hid; u16 pciid; const struct dspplat *pl; const struct sstmap *map; } sstids[] = {
    { "80860F28", 0x0F28, &pl_byt, &map_byt },
    { "808622A8", 0x22A8, &pl_cht, &map_cht },
    { "INT3438", 0x9CB6, &pl_bdw, &map_bdw },
};

/* shim registers */
#define CSR             0x00
#define IMRX            0x28
#define IMRD            0x30
#define IPCX            0x38
#define IPCD            0x40
#define CLKCTL          0x78
#define CSR2            0x80
#define HMDC            0xE8

/* Bay Trail / Cherry Trail CSR */
#define BCSR_RST        0x01
#define BCSR_VSEL       0x02            /* reset vector in IRAM */
#define BCSR_STALL      0x04
#define BCSR_PWAIT      0x08            /* core in wait mode */

/* Broadwell CSR, CLKCTL, CSR2, HMDC, IMRD */
#define CSR_RST         0x02
#define CSR_SBCS1       0x08
#define CSR_DCS_MASK    0x70
#define CSR_DCS(x)      ((u32)(x) << 4)
#define CSR_STALL       0x400
#define CSR_S1IOCS      0x800000UL
#define CSR_LPCS        0x80000000UL
#define CLK_SMOS_MASK   (3UL << 24)     /* MCLK 24 MHz */
#define CLK_DCPLCG      (1UL << 18)
#define CLK_SCOE0       (1UL << 16)
#define CSR2_SDFD_SSP1  0x04
#define HMDC_ALLCH      (0xFUL | (0xFUL << 7))
#define IMRD_BDW        (0x03UL | (1UL << 16) | (3UL << 21))   /* DONE, BUSY, SSP0, DMAC0/1 */

/* PCI configuration (Broadwell: through the memory mirror) */
#define PCI_PMCS        0x84
#define VDRTCTL0        0xA0
#define VDRTCTL2        0xA8
#define VD0_D3PGD       0x01
#define VD2_DCLCGE      0x02
#define VD2_DTCGE       0x400
#define VD2_APLLSE      0x80000000UL

/* DW-DMA channel registers */
#define DW_CH(ch)       ((u32)(ch) * 0x58)
#define DW_SAR          0x00
#define DW_DAR          0x08

/* the found device */
struct sstdev {
    const struct dspplat *pl;
    const struct sstmap *map;
    char path[8];                   /* ACPI device name, empty for a PCI function */
    struct pcidev pci;
    int ispci;
};

/* DMA channel addresses when an endpoint started: a channel is taken for the endpoint's
   once it moves inside the ring */
static u32 snap[MAXEP][24];

static const struct sstmap *map_of(struct dsp *d)
{
    return d->pl == &pl_bdw ? &map_bdw : d->pl == &pl_cht ? &map_cht : &map_byt;
}

static u32 srd(struct dsp *d, u32 o) { return RD32(d->bar + map_of(d)->shim + o); }
static void swr(struct dsp *d, u32 o, u32 v) { WR32(d->bar + map_of(d)->shim + o, v); }
static void supd(struct dsp *d, u32 o, u32 mask, u32 v) { swr(d, o, (srd(d, o) & ~mask) | (v & mask)); }

/* PCI configuration dword: the ACPI memory mirror, else the PCI function */
static u32 crd(struct dsp *d, u32 o)
{
    if (d->pcicfg) return RD32(d->pcicfg + o);
    if (d->pci.vendor) return pci_rd32(d->pci.bus, d->pci.dev, d->pci.fn, (int)o);
    return 0xFFFFFFFFUL;
}

static void cupd(struct dsp *d, u32 o, u32 mask, u32 v)
{
    u32 x = (crd(d, o) & ~mask) | (v & mask);
    if (d->pcicfg) WR32(d->pcicfg + o, x);
    else if (d->pci.vendor) pci_wr32(d->pci.bus, d->pci.dev, d->pci.fn, (int)o, x);
}

/* ---------------------------------------------------------------- the device */

static int ident(struct pcidev *pcis, int n, struct sstdev *s)
{
    static const char *const hids[] = { "80860F28", "808622A8", "INT3438", NULL };
    struct acpi_dev ad[2];
    int i, k;
    memset(s, 0, sizeof(*s));
    if (acpi_find_devices(hids, ad, 2) > 0) {
        for (k = 0; k < (int)ARRAY_SIZE(sstids); k++)
            if (!strncmp(ad[0].hid, sstids[k].hid, 9)) {
                s->pl = sstids[k].pl;
                s->map = sstids[k].map;
                strlcpy(s->path, ad[0].path, sizeof(s->path));
            }
    }
    for (i = 0; i < n; i++) {
        if (pcis[i].vendor != 0x8086) continue;
        for (k = 0; k < (int)ARRAY_SIZE(sstids); k++)
            if (pcis[i].device == sstids[k].pciid) {
                s->pl = sstids[k].pl;
                s->map = sstids[k].map;
                s->pci = pcis[i];
                s->ispci = 1;
            }
    }
    return s->pl != NULL;
}

int sst_pci(const struct pcidev *p)
{
    int k;
    if (p->vendor != 0x8086) return 0;
    if (p->device == 0x9C36 || p->device == 0x8C36) return 1;     /* Haswell (no SOF support) */
    for (k = 0; k < (int)ARRAY_SIZE(sstids); k++)
        if (p->device == sstids[k].pciid) return 1;
    return 0;
}

void sst_fw_prefetch(struct pcidev *pcis, int n)
{
    struct sstdev s;
    if (ident(pcis, n, &s) && cdc_present()) plat_fw_request(s.pl->fw);
}

static int base_ok(u32 b)
{
    return b && b != 0x55AA55AAUL && b != 0xFFFFFFFFUL && !(b & 0xFFF);
}

/* Bay Trail CR (the codec on SSP0): the PMIC option the BIOS mirrors in the P-unit's
   BIOS_CONFIG (IOSF sideband through the host bridge), else as Linux: an LPE with less than
   six interrupt lines */
static int is_bytcr(const char *path)
{
    u32 v;
    int irqs;
    if (pci_rd32(0, 0, 0, 0) == 0x0F008086UL) {
        pci_wr32(0, 0, 0, 0xD8, 0);                                 /* MCRX */
        pci_wr32(0, 0, 0, 0xD0, (0x10UL << 24) | (0x04UL << 16) | (0x06UL << 8) | 0xF0);
        v = pci_rd32(0, 0, 0, 0xD4);                                /* MDR */
        if (v != 0xFFFFFFFFUL) {
            v = (v >> 26) & 3;
            DBG("dsp: P-unit BIOS_CONFIG PMIC option %lu\n", v);
            return v == 1 || v == 3;
        }
    }
    irqs = path[0] ? acpi_dev_irqs(path) : -1;
    return irqs >= 0 && irqs < 6;
}

/* PMC platform clock 'clk' on at 19.2 MHz (Atom: the codec MCLK) */
static void pmc_clock(int clk)
{
    u32 base, v, lpc = pci_rd32(0, 31, 0, 0);
    u8 *r;
    if (lpc != 0x0F1C8086UL && lpc != 0x229C8086UL) return;
    base = pci_rd32(0, 31, 0, 0x44) & 0xFFFFFE00UL;
    if (!base || !(r = plat_mmio(base, 0x100))) return;
    v = RD32(r + 0x60 + 4 * clk);
    WR32(r + 0x60 + 4 * clk, (v & ~7UL) | 4 | 1);                  /* PLL (19.2 MHz), forced on */
    DBG("dsp: PMC clock %d %08lx -> %08lx\n", clk, v, RD32(r + 0x60 + 4 * clk));
}

/* MCLK for the codecs of Bay Trail / Cherry Trail: clock 3; the MAX98090 Chromebooks feed it
   from clock 0, which is switched on too unless something already runs it at 25 MHz */
static void codec_clocks(void)
{
    static const char *const m90[] = { "193C9890", NULL };
    struct acpi_dev ad[1];
    u32 base, v, lpc = pci_rd32(0, 31, 0, 0);
    u8 *r;
    pmc_clock(3);
    if (acpi_find_devices(m90, ad, 1) <= 0 || (lpc != 0x0F1C8086UL && lpc != 0x229C8086UL)) return;
    base = pci_rd32(0, 31, 0, 0x44) & 0xFFFFFE00UL;
    if (!base || !(r = plat_mmio(base, 0x100))) return;
    v = RD32(r + 0x60);
    if ((v & 3) > 1 || (v & 4)) pmc_clock(0);                       /* off, or already 19.2 MHz */
}

/* ---------------------------------------------------------------- firmware image */

/* extended manifest (XMan) and "Reef" image: the memory windows of the manifest; with 'load'
   the blocks are written into the DSP memories. Returns 0 if the image is good. */
static int fw_image(struct dsp *d, const struct sstmap *m, int load)
{
    const u8 *v = d->fw.v;
    u32 size = d->fw.size, off = 0, pos, end, nmod, i, j, k;
    d->xwin = NULL;
    if (size < 16) return -1;
    if (*(const u32 *)v == 0x6E614D58UL) {                          /* XMan */
        u32 hlen = ((const u32 *)v)[2], p, esz;
        off = ((const u32 *)v)[1];
        if (off >= size || hlen >= off) return -1;
        for (p = hlen; p + 8 <= off; p += esz) {
            const u32 *e = (const u32 *)(v + p);
            esz = e[1];
            if (esz < 8 || p + esz > off) break;
            /* window element: sof_ipc_window (12-byte header, count, 24-byte elements) */
            if (e[0] == 1 && esz >= 24 && e[5] <= 16 && 24 + e[5] * 24 <= esz) d->xwin = e + 5;
        }
    }
    if (off + 16 > size || memcmp(v + off, "Reef", 4) || off + 16 + ((const u32 *)(v + off))[1] != size) {
        kprintf("dsp: %s is no SOF image for this DSP\n", d->fwname);
        return -1;
    }
    nmod = ((const u32 *)(v + off))[2];
    pos = off + 16;
    for (i = 0; i < nmod; i++) {
        const u32 *mh = (const u32 *)(v + pos);
        if (pos + 12 > size || mh[1] > size - pos - 12) return -1;
        end = pos + 12 + mh[1];
        pos += 12;
        for (j = 0; j < mh[2]; j++) {
            const u32 *bh = (const u32 *)(v + pos);
            u32 t, bs, bo;
            if (pos + 12 > end) return -1;
            t = bh[0];
            bs = bh[1];
            bo = bh[2];
            pos += 12;
            if (bs > end - pos) return -1;
            if (bs && (t == 1 || t == 2 || t == 3)) {
                /* IRAM, DRAM (SRAM: either of them) */
                int in_i = bo >= m->iram && bo - m->iram <= m->iramsz && bs <= m->iramsz - (bo - m->iram);
                int in_d = bo >= m->dram && bo - m->dram <= m->dramsz && bs <= m->dramsz - (bo - m->dram);
                if ((bs & 3) || (t == 1 && !in_i) || (t == 2 && !in_d) || (t == 3 && !in_i && !in_d)) {
                    kprintf("dsp: %s: block %lu at %06lx (%lu bytes) outside the DSP memory\n", d->fwname, j, bo, bs);
                    return -1;
                }
                if (load)
                    for (k = 0; k < bs; k += 4) WR32(d->bar + bo + k, *(const u32 *)(v + pos + k));
            } else if (bs && t > 14) {
                return -1;
            }
            pos += bs;
        }
        pos = end;
    }
    return 0;
}

/* ---------------------------------------------------------------- power, reset, run */

static int byt_boot(struct dsp *d, const struct sstmap *m)
{
    int tries;
    /* host side IPC interrupts masked (polled); reset with the vector in IRAM, stalled */
    supd(d, IMRX, 3, 3);
    supd(d, CSR, BCSR_RST | BCSR_VSEL | BCSR_STALL, BCSR_RST | BCSR_VSEL | BCSR_STALL);
    udelay(15);
    supd(d, CSR, BCSR_RST, 0);
    if (fw_image(d, m, 1)) return -1;
    supd(d, CSR, BCSR_STALL, 0);
    for (tries = 0; tries < 10 && (srd(d, CSR) & BCSR_PWAIT); tries++) mdelay(100);
    if (srd(d, CSR) & BCSR_PWAIT) return -1;
    return 0;
}

static void bdw_reset(struct dsp *d)
{
    supd(d, CSR, CSR_RST | CSR_STALL, CSR_RST | CSR_STALL);
    mdelay(10);
    supd(d, CSR, CSR_RST | CSR_STALL, CSR_STALL);
}

static int bdw_boot(struct dsp *d, const struct sstmap *m)
{
    int tries;
    /* D0: core clock gating off, no D3 power gating, power state D0 */
    cupd(d, VDRTCTL2, VD2_DCLCGE | VD2_DTCGE, 0);
    cupd(d, VDRTCTL0, VD0_D3PGD, VD0_D3PGD);
    cupd(d, PCI_PMCS, 3, 0);
    for (tries = 0; tries < 10 && (crd(d, PCI_PMCS) & 3); tries++) mdelay(20);
    if (crd(d, PCI_PMCS) & 3) {
        kprintf("dsp: the DSP does not enter D0 (PMCS %08lx)\n", crd(d, PCI_PMCS));
        return -1;
    }
    if (srd(d, CSR) == 0xFFFFFFFFUL) {
        kprintf("dsp: the DSP registers do not answer (ADSP disabled in the BIOS?)\n");
        return -1;
    }
    /* SSP1 19.2 MHz base clock, SSP clock 0, low power clock off; core stalled at 192 / 96 MHz */
    supd(d, CSR, CSR_S1IOCS | CSR_SBCS1 | CSR_LPCS, 0);
    supd(d, CSR, CSR_STALL | CSR_DCS_MASK, CSR_STALL | CSR_DCS(4));
    /* MCLK 24 MHz on SSP0, no local clock gating */
    supd(d, CLKCTL, CLK_SMOS_MASK | CLK_DCPLCG | CLK_SCOE0, CLK_SMOS_MASK | CLK_DCPLCG | CLK_SCOE0);
    bdw_reset(d);
    cupd(d, VDRTCTL2, VD2_DCLCGE | VD2_DTCGE, VD2_DCLCGE | VD2_DTCGE);
    udelay(55);
    cupd(d, VDRTCTL2, VD2_APLLSE, 0);                               /* audio PLL on */
    cupd(d, VDRTCTL0, 0xFFFFFFFCUL, 0);                             /* SRAM blocks powered */
    supd(d, CSR2, CSR2_SDFD_SSP1, CSR2_SDFD_SSP1);
    supd(d, HMDC, HMDC_ALLCH, HMDC_ALLCH);
    supd(d, IMRX, 3, 3);                                            /* host side: polled */
    supd(d, IMRD, IMRD_BDW, 0);
    swr(d, IPCX, 0);
    swr(d, IPCD, 0);
    /* firmware into IRAM / DRAM, the core runs (DMA engines opportunistic) */
    bdw_reset(d);
    if (fw_image(d, m, 1)) return -1;
    supd(d, HMDC, HMDC_ALLCH, 0);
    supd(d, CSR, CSR_STALL, 0);
    return 0;
}

void sst_down(struct dsp *d)
{
    if (d->shim == SST_BYT) {
        supd(d, IMRX, 3, 3);
        supd(d, IMRD, 3, 3);
        supd(d, CSR, BCSR_RST | BCSR_VSEL | BCSR_STALL, BCSR_RST | BCSR_VSEL | BCSR_STALL);
    } else {
        supd(d, CSR, CSR_RST | CSR_STALL, CSR_RST | CSR_STALL);
    }
}

static void set_regs(struct dsp *d, const struct sstmap *m)
{
    u32 s = m->shim;
    if (d->shim == SST_BYT) {
        d->r_req = s + IPCX + 4;        /* high dwords: BUSY bit 31, DONE bit 30 */
        d->r_reqext = s + IPCX;
        d->r_ack = s + IPCX + 4;
        d->r_rsp = s + IPCD + 4;
        d->r_rspext = s + IPCD;
    } else {
        d->r_req = d->r_ack = s + IPCX;
        d->r_rsp = s + IPCD;
        d->r_reqext = d->r_rspext = 0;
    }
    d->r_ackdone = 0x40000000UL;
    d->r_rspack = 0;
    d->r_ctl = s + CSR;
    /* every window is an offset into the mailbox; FW_READY at its start */
    d->win0 = m->mbox;
    d->winsz = 0;
    d->dspbox = m->mbox;
    d->hostbox = m->mbox + 0x400;
    d->dspboxsz = d->hostboxsz = 0x400;
}

/* ---------------------------------------------------------------- start */

static struct dsp *attach(struct sstdev *s)
{
    struct dsp *d;
    u32 base[4], len[4], bar = 0, cfg = 0, psat;
    int n = -1, ret;
    if (s->path[0]) n = acpi_dev_mem(s->path, base, len, 4);
    if (n >= 1 && base_ok(base[0])) {
        bar = base[0];
        if (n >= 2 && base_ok(base[1])) cfg = base[1];
        s->ispci = 0;
    } else if (s->ispci) {
        bar = pci_bar_mem(&s->pci, 0, NULL);
        pci_enable(&s->pci, 1, 0, 1);
        pci_intx(&s->pci, 0);
    }
    if (!bar) {
        kprintf("dsp: %s audio DSP %s: no register address (disabled in the BIOS?)\n", s->pl->name, s->path);
        return NULL;
    }
    d = kmalloc(sizeof(*d));
    if (!d) return NULL;
    memset(d, 0, sizeof(*d));
    d->pl = s->pl;
    d->shim = s->pl == &pl_bdw ? SST_BDW : SST_BYT;
    d->barlen = s->map->barlen;
    if (s->ispci) d->pci = s->pci;
    else {
        d->pci.vendor = 0x8086;                     /* shown as the sound device's id */
        d->pci.device = s->pl == &pl_byt ? 0x0F28 : s->pl == &pl_cht ? 0x22A8 : 0x9CB6;
    }
    d->bar = plat_mmio(bar, d->barlen);
    d->buf = kmalloc_align(4096, 4096);
    if (cfg) d->pcicfg = plat_mmio(cfg, 0x1000);
    if (!d->bar || !d->buf) goto fail;
    set_regs(d, s->map);
    kprintf("dsp: %s audio DSP%s%s at %08lx%s, SOF IPC3 firmware\n", s->pl->name, s->path[0] ? " " : "",
            s->path, bar, s->ispci ? " (PCI)" : "");
    /* power: D0 through the PCI configuration mirror (the _PS0 / power resource of the LPE) */
    if (d->shim == SST_BYT) {
        if (s->path[0] && !acpi_dev_field(s->path, "PSAT", &psat)) {
            u8 *p = plat_mmio(psat, 4);
            if (p && (RD32(p) & 3)) WR32(p, RD32(p) & ~3UL);
        } else if (d->pcicfg && (crd(d, PCI_PMCS) & 3) && crd(d, PCI_PMCS) != 0xFFFFFFFFUL) {
            cupd(d, PCI_PMCS, 3, 0);
        } else if (s->ispci) {
            int pm = pci_find_cap(&d->pci, 1);
            if (pm) cupd(d, (u32)pm + 4, 3, 0);
        }
        mdelay(10);
        d->bytcr = (u8)(s->pl == &pl_byt && is_bytcr(s->path));
        if (d->bytcr) kprintf("dsp: Bay Trail CR: the codec is on SSP0\n");
    } else if (!d->pcicfg && !s->ispci) {
        kprintf("dsp: no PCI configuration range for the Broadwell DSP\n");
        goto fail;
    }
    if (d->shim == SST_BYT && srd(d, CSR) == 0xFFFFFFFFUL) {
        kprintf("dsp: the DSP registers do not answer (LPE disabled in the BIOS?)\n");
        goto fail;
    }
    if (plat_fw_load(s->pl->fw, &d->fw)) {
        kprintf("dsp: firmware %s not found next to the driver (sof-%s.ri of SOF 2.2, renamed)\n", s->pl->fw,
                s->pl->name);
        goto fail;
    }
    d->fwname = s->pl->fw;
    kprintf("dsp: %s, %lu bytes\n", d->fwname, d->fw.size);
    if (fw_image(d, s->map, 0)) goto fail;
    ret = d->shim == SST_BYT ? byt_boot(d, s->map) : bdw_boot(d, s->map);
    d->booted = 1;
    if (ret || dsp_wait_fw_ready(d)) {
        kprintf("dsp: %s did not start (CSR %08lx IPCD %08lx)\n", d->fwname, srd(d, CSR), RD32(d->bar + d->r_rsp));
        goto fail;
    }
    if (ipc3_ready(d)) {
        kprintf("dsp: firmware does not answer\n");
        goto fail;
    }
    d->next = dsp_list;
    dsp_list = d;
    if (s->pl != &pl_bdw) codec_clocks();
    if (dspdev_setup(d, 1) <= 0) kprintf("dsp: no I2S codec answers; the DSP stays idle\n");
    return d;
fail:
    if (d->booted) sst_down(d);
    kfree(d->buf);
    kfree(d);
    return NULL;
}

int sst_probe(struct pcidev *pcis, int n)
{
    struct sstdev s;
    if (!ident(pcis, n, &s)) return 0;
    if (cb->opts2 & OPT2_NODSP) {
        kprintf("dsp: %s audio DSP not started (/NODSP)\n", s.pl->name);
        return 0;
    }
    if (!cdc_present()) {
        static const char *const r77[] = { "RT5677CE", NULL };
        struct acpi_dev ad[1];
        if (acpi_find_devices(r77, ad, 1) > 0)
            kprintf("dsp: RT5677 codec (Broadwell Chromebook): no driver for it, DSP not started\n");
        else
            kprintf("dsp: %s audio DSP without a known I2S codec in the ACPI tables: not started\n", s.pl->name);
        return 0;
    }
    return attach(&s) ? 1 : 0;
}

/* ---------------------------------------------------------------- endpoints */

/* the byte offset in the ring of physical address a; -1 if a is not in the ring */
static long ring_off(struct dspep *e, u32 a)
{
    u32 i, pages = (e->hw->size + 4095) / 4096;
    for (i = 0; i < pages; i++) {
        u32 p = vphys(e->hw->buf + i * 4096);
        if (a >= p && a - p < 4096) return (long)(i * 4096 + (a - p));
        if (i == pages - 1 && a == p + (e->hw->size - i * 4096)) return 0;   /* at the end */
    }
    return -1;
}

static u32 ch_addr(struct dsp *d, const struct sstmap *m, int k, int ch, int dir)
{
    return RD32(d->bar + m->dmac[k] + DW_CH(ch) + (dir ? DW_DAR : DW_SAR));
}

int sst_ep_start(struct dspep *e, struct ahw *hw)
{
    struct dsp *d = e->d;
    const struct sstmap *m = map_of(d);
    u32 pages = (hw->size + 4095) / 4096, i, *sn = snap[e - d->ep];
    u8 *pt;
    int k, ch;
    if (!e->bdl) e->bdl = kmalloc_align(4096, 4096);
    if (!e->bdl) return -1;
    if (pages > 100 || (vphys(hw->buf) & 4095)) {
        kprintf("dsp: ring of %lu bytes not usable for the DSP DMA\n", hw->size);
        return -1;
    }
    /* compressed page table: 20-bit page numbers, 2.5 bytes each (snd_sof_create_page_table) */
    pt = (u8 *)e->bdl;
    memset(pt, 0, 256);
    for (i = 0; i < pages; i++) {
        u32 a = vphys(hw->buf + i * 4096), pfn = a >> 12, idx = (5 * i) >> 1;
        if (a >= 0x80000000UL) {
            kprintf("dsp: ring above 2 GB, out of reach of the DSP DMA\n");
            return -1;
        }
        if (i & 1) {
            pfn = (pt[idx] & 0xF) | (pfn << 4);
        }
        pt[idx] = (u8)pfn;
        pt[idx + 1] = (u8)(pfn >> 8);
        pt[idx + 2] = (u8)(pfn >> 16);
        pt[idx + 3] = (u8)(pfn >> 24);
    }
    aeng_flush_dma(pt, 256);
    if (vphys(pt) >= 0x80000000UL) return -1;
    e->hw = hw;
    e->sstch = 0;
    for (k = 0; k < 3; k++)
        for (ch = 0; ch < 8; ch++) sn[k * 8 + ch] = m->dmac[k] ? ch_addr(d, m, k, ch, e->dir) : 0;
    if (ipc3_ep_start(e)) return -1;
    if (d->streambox && e->posoff < 0x1000) WR32(d->bar + d->streambox + e->posoff + 28, 0);    /* no stale position */
    if (ipc3_trigger(e, 1)) {
        ipc3_ep_stop(e);
        return -1;
    }
    e->running = 1;
    return 0;
}

void sst_ep_stop(struct dspep *e)
{
    if (!e->running) return;
    ipc3_trigger(e, 0);
    ipc3_ep_stop(e);
    e->running = 0;
}

u32 sst_ep_pos(struct dspep *e)
{
    struct dsp *d = e->d;
    const struct sstmap *m = map_of(d);
    u32 *sn = snap[e - d->ep], a;
    long o;
    int k, ch;
    if (!e->running) return 0;
    if (e->sstch) {
        k = e->sstch >> 8;
        ch = (e->sstch & 0xFF) - 1;
        o = ring_off(e, ch_addr(d, m, k, ch, e->dir));
        if (o >= 0) return (u32)o;
        e->sstch = 0;
    }
    /* the host DMA channel: the one that moved since the start and points into the ring */
    for (k = 0; k < 3 && m->dmac[k]; k++)
        for (ch = 0; ch < 8; ch++) {
            a = ch_addr(d, m, k, ch, e->dir);
            if (a == sn[k * 8 + ch] || (o = ring_off(e, a)) < 0) continue;
            e->sstch = (u16)((k << 8) | (ch + 1));
            DBG("dsp: %s position from DMA %d channel %d\n", e->dir ? "capture" : "playback", k, ch);
            return (u32)o;
        }
    /* the stream window: host position of the firmware (reset to 0 at the end of the ring) */
    if (d->streambox && e->posoff < 0x1000) {
        a = RD32(d->bar + d->streambox + e->posoff + 28);
        return a < e->hw->size ? a & ~3UL : 0;
    }
    return 0;
}
