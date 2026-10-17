/* cdcssm.c - Analog Devices SSM4567 mono class-D amplifier (INT343B: Skylake / Kaby Lake
   boards with two of them, skl_nau88l25_ssm4567 / avs ssm4567). The Linux machine drivers
   run SSP0 in DSP A mode, inverted BCLK, 4 TDM slots of 24 bits; the amplifiers take it as 2
   slots of 48 BCLKs: ACPI instance :00 (c->chan 0) the left speaker in the first half of the
   frame, :01 (c->chan 1) the right speaker in the second half. Here the slot of each
   amplifier is found from the SSP playback mask (the chan-th slot set). Register settings
   after the Linux ssm4567 driver; V/I/battery sense stays off. */
#include "dspcdc.h"

#define POWER_CTRL      0x00
#define DAC_CTRL        0x02
#define DAC_VOLUME      0x03
#define SAI_CTRL_1      0x04
#define SAI_CTRL_2      0x05
#define SOFT_RESET      0xFF

/* POWER_CTRL: auto power down, sense blocks down; boost and amplifier up when playing */
#define PWR_RUN         0xF0
#define PWR_IDLE        0xFC                    /* boost and amplifier down */
#define PWR_OFF         0xFD                    /* and software power down */

#define DAC_MUTE        0x40
#define DAC_48K         0x32                    /* HPF, low power DAC, 32..48 kHz */

#define SAI1_BCLK_INV   0x40
#define SAI1_FSYNC_INV  0x08
#define SAI1_LJ         0x04
#define SAI1_TDM        0x02

static int ssm_probe(struct cdc *c)
{
    if (!c->bus) return -1;
    /* no id register: the chip must answer reads of its control registers */
    if (cdc_rd(c, POWER_CTRL) < 0) return -1;
    return cdc_rd(c, DAC_CTRL) < 0 ? -1 : 0;
}

/* TDM slot of this amplifier and the slot length field (32, 48, 64 BCLKs) */
static u32 ssm_slot(struct cdc *c, u32 *bclks)
{
    u32 width = c->ssp.width ? c->ssp.width : 24, frame, n, s, k, off, w;
    frame = (u32)(c->ssp.slots ? c->ssp.slots : 2) * width;
    n = c->namps > 1 ? c->chan : 0;
    for (s = 0, k = 0; s < 32; s++) {
        if (!(c->ssp.txmask & (1UL << s))) continue;
        if (k++ == n) break;
    }
    if (s == 32) s = n;
    off = s * width;
    /* the shortest amplifier slot that starts at the data and divides the frame */
    for (w = 32; w <= 64; w += 16)
        if (w >= width && off % w == 0 && frame % w == 0 && off / w < 8) {
            *bclks = (w - 32) / 16;
            return off / w;
        }
    /* else the frame in halves (left, right) as the Skylake boards set it up */
    w = frame / 2;
    *bclks = w == 48 ? 1 : w == 64 ? 2 : 0;
    if (w != 32 && w != 48 && w != 64)
        kprintf("ssm4567: no amplifier slot fits %lu x %lu bit TDM\n", (u32)c->ssp.slots, width);
    return n & 1;
}

static int ssm_init(struct cdc *c)
{
    u32 ctrl1, bclks = 0, slot;
    cdc_wr(c, SOFT_RESET, 0x00);
    mdelay(1);
    cdc_wr(c, POWER_CTRL, PWR_IDLE);
    cdc_wr(c, DAC_CTRL, DAC_48K | DAC_MUTE);
    cdc_wr(c, DAC_VOLUME, 0x40);                /* 0 dB */
    if (c->ssp.provider) kprintf("ssm4567: clock provider mode not supported\n");
    slot = ssm_slot(c, &bclks);
    switch (c->ssp.fmt) {
    case 3: ctrl1 = SAI1_LJ | SAI1_FSYNC_INV; break;            /* left justified */
    case 4: ctrl1 = SAI1_TDM | SAI1_BCLK_INV; break;            /* DSP A, as Linux sets it */
    case 5: ctrl1 = SAI1_TDM | SAI1_LJ | SAI1_BCLK_INV; break;  /* DSP B */
    default: ctrl1 = 0; break;                                  /* I2S */
    }
    if (c->ssp.fmt != 4 && c->ssp.fmt != 5) slot = c->namps > 1 ? c->chan & 1 : 0;
    cdc_wr(c, SAI_CTRL_1, ctrl1 | (bclks << 4));
    cdc_wr(c, SAI_CTRL_2, slot);                /* fixed slot, no auto slot from ADDR */
    DBG("ssm4567: chan %u, slot %lu, %lu BCLK slots\n", c->chan, slot, 32 + 16 * bclks);
    return 0;
}

static void ssm_stream(struct cdc *c, int dir, int on)
{
    if (dir) return;
    if (on) {
        cdc_wr(c, POWER_CTRL, PWR_RUN);
        cdc_wr(c, DAC_CTRL, DAC_48K);
    } else {
        cdc_wr(c, DAC_CTRL, DAC_48K | DAC_MUTE);
        cdc_wr(c, POWER_CTRL, PWR_IDLE);
    }
}

static void ssm_off(struct cdc *c)
{
    cdc_wr(c, DAC_CTRL, DAC_48K | DAC_MUTE);
    cdc_wr(c, POWER_CTRL, PWR_OFF);
}

const struct cdcdrv cdc_ssm4567 = { "ssm4567", { "INT343B", NULL }, CK_AMP, 8, 8, 32,
                                    ssm_probe, ssm_init, ssm_stream, NULL, NULL, NULL, ssm_off };
