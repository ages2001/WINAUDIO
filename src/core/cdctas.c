/* cdctas.c - Texas Instruments TAS2563 smart amplifier (TXNW2563, Intel SOF laptops with a
   Realtek headset codec, sof_rt5682 + sof_ti_common). Linux drives it with the tas2781
   driver and TI's RCA/DSP firmware files; without them the chip runs in its ROM mode, which
   takes the TAS2562-compatible book 0 / page 0 registers (Linux tas2562 driver): TDM slot
   select, word / slot length, sample rate, power mode. Registers are paged (page register
   0x00, book register 0x7F of page 0); IV sense stays off. With two amplifiers c->chan 0 plays
   the left channel, 1 the right one; a single amplifier plays (L+R)/2. The optional GpioIo of
   the ACPI device is the SDZ (shutdown, low active) pin, driven high to run. */
#include "dspcdc.h"

#define TAS_REG(pg, r)  ((pg) * 128 + (r))
#define PAGE_CTRL       0x00
#define BOOK_CTRL       0x7F                    /* page 0 */
#define SW_RESET        TAS_REG(0, 0x01)
#define PWR_CTRL        TAS_REG(0, 0x02)
#define PB_CFG1         TAS_REG(0, 0x03)
#define TDM_CFG0        TAS_REG(0, 0x06)
#define TDM_CFG1        TAS_REG(0, 0x07)
#define TDM_CFG2        TAS_REG(0, 0x08)
#define TDM_CFG3        TAS_REG(0, 0x09)
#define TDM_CFG5        TAS_REG(0, 0x0B)
#define TDM_CFG6        TAS_REG(0, 0x0C)
#define REV_ID          TAS_REG(0, 0x7D)
#define DVC_CFG1        TAS_REG(2, 0x0C)        /* 32-bit volume coefficient, MSB first */

#define PWR_ISNS_PD     0x08
#define PWR_VSNS_PD     0x04
#define MODE_ACTIVE     0x00
#define MODE_SHUTDOWN   0x02

/* priv[0]: selected page + 1 (0 unknown) */

static int tas_page(struct cdc *c, u32 page)
{
    if (c->priv[0] == page + 1) return 0;
    c->priv[0] = 0;
    if (cdc_wr(c, PAGE_CTRL, page)) return -1;
    c->priv[0] = page + 1;
    return 0;
}

static int tas_wr(struct cdc *c, u32 reg, u32 val)
{
    if (tas_page(c, reg / 128)) return -1;
    return cdc_wr(c, reg % 128, val);
}

static int tas_rd(struct cdc *c, u32 reg)
{
    if (tas_page(c, reg / 128)) return -1;
    return cdc_rd(c, reg % 128);
}

/* book 0, page 0 */
static int tas_book0(struct cdc *c)
{
    c->priv[0] = 0;
    if (tas_page(c, 0)) return -1;
    return cdc_wr(c, BOOK_CTRL, 0);
}

static int tas_probe(struct cdc *c)
{
    int v;
    if (!c->bus) return -1;
    if (cdc_ngpio(c) > 0) {
        cdc_gpio(c, 0, 1);                      /* out of shutdown to answer */
        mdelay(2);
    }
    if (tas_book0(c)) return -1;
    v = tas_rd(c, REV_ID);
    if (v < 0 || cdc_rd(c, PAGE_CTRL) != 0) return -1;
    DBG("tas2563: revision %02x\n", (u8)v);
    return 0;
}

/* the n-th slot of the playback mask (n-th set bit), or n when the mask is shorter */
static u32 tas_slot(struct cdc *c, u32 n)
{
    u32 i, k = 0;
    for (i = 0; i < 16; i++) {
        if (!(c->ssp.txmask & (1UL << i))) continue;
        if (k++ == n) return i;
    }
    return n;
}

static int tas_init(struct cdc *c)
{
    static const u8 maxw[3] = { 0, 2, 3 };      /* longest word per slot length */
    u32 l, r, scfg, slen, wlen, cfg0, off;
    if (cdc_ngpio(c) > 0) {
        cdc_gpio(c, 0, 0);
        mdelay(1);
        cdc_gpio(c, 0, 1);
        mdelay(2);
    }
    tas_book0(c);
    tas_wr(c, SW_RESET, 1);
    mdelay(2);
    c->priv[0] = 0;
    tas_book0(c);
    tas_wr(c, PWR_CTRL, PWR_ISNS_PD | PWR_VSNS_PD | MODE_SHUTDOWN);
    if (c->ssp.provider) kprintf("tas2563: clock provider mode not supported\n");
    /* channel: this amplifier's slot, or the stereo pair mixed down */
    if (c->namps > 2 && c->ssp.slots > 2) {
        l = r = tas_slot(c, c->chan);
        scfg = 1;                               /* left (both slot fields the same) */
    } else {
        l = tas_slot(c, 0);
        r = tas_slot(c, 1);
        if (c->namps < 2) scfg = 3;             /* (L+R)/2 */
        else scfg = (c->chan & 1) ? 2 : 1;      /* right : left */
    }
    switch (c->ssp.width) {
    case 16: slen = 0; break;
    case 24: slen = 1; break;
    default: slen = 2; break;                   /* 32 */
    }
    if (c->ssp.width != 16 && c->ssp.width != 24 && c->ssp.width != 32)
        kprintf("tas2563: %u bit slots not supported\n", c->ssp.width);
    switch (c->ssp.vbits) {
    case 16: wlen = 0; break;
    case 20: wlen = 1; break;
    case 32: wlen = 3; break;
    default: wlen = 2; break;                   /* 24 */
    }
    if (wlen > maxw[slen]) wlen = maxw[slen];   /* word no longer than the slot */
    /* frame start on the falling FSYNC edge for I2S, rising for the others; data one BCLK
       after the frame start for I2S and DSP A */
    cfg0 = 0x08 | (c->ssp.fmt == 1 ? 1 : 0);    /* 44.1/48 kHz */
    off = (c->ssp.fmt == 1 || c->ssp.fmt == 4) ? 1 : 0;
    tas_wr(c, TDM_CFG0, cfg0);
    tas_wr(c, TDM_CFG1, off << 1);              /* receive on the rising BCLK edge */
    tas_wr(c, TDM_CFG2, (scfg << 4) | (wlen << 2) | slen);
    tas_wr(c, TDM_CFG3, (r << 4) | l);
    tas_wr(c, TDM_CFG5, 0x00);                  /* no V/I sense data on SDOUT */
    tas_wr(c, TDM_CFG6, 0x00);
    tas_wr(c, PB_CFG1, 0x20);                   /* amplifier level 16.5 dBV */
    /* digital volume 0 dB (1.0 in Q2.30), LSB last: it latches the value */
    tas_wr(c, DVC_CFG1, 0x40);
    tas_wr(c, DVC_CFG1 + 1, 0x00);
    tas_wr(c, DVC_CFG1 + 2, 0x00);
    tas_wr(c, DVC_CFG1 + 3, 0x00);
    tas_page(c, 0);
    DBG("tas2563: chan %u, slots %lu/%lu, mode %lu\n", c->chan, l, r, scfg);
    return 0;
}

static void tas_stream(struct cdc *c, int dir, int on)
{
    if (dir) return;
    tas_wr(c, PWR_CTRL, PWR_ISNS_PD | PWR_VSNS_PD | (on ? MODE_ACTIVE : MODE_SHUTDOWN));
}

static void tas_off(struct cdc *c)
{
    tas_wr(c, PWR_CTRL, PWR_ISNS_PD | PWR_VSNS_PD | MODE_SHUTDOWN);
    if (cdc_ngpio(c) > 0) cdc_gpio(c, 0, 0);
}

const struct cdcdrv cdc_tas2563 = { "tas2563", { "TXNW2563", NULL }, CK_AMP, 8, 8, 32,
                                    tas_probe, tas_init, tas_stream, NULL, NULL, NULL, tas_off };
