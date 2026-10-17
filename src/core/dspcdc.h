/* dspcdc.h - I2S codecs and amplifiers on the SSP ports of the Intel DSP (dspcdc.c finds
   them in the ACPI tables, the cdc*.c drivers program them over I2C), and the I2C master of
   the Intel LPSS (dwi2c.c). */
#ifndef DSPCDC_H
#define DSPCDC_H
#include "dsp.h"

/* ---- I2C (DesignWare controllers of the Intel LPSS) and GPIO pads ---- */
#include "i2cbus.h"

/* ---- codecs ---- */
#define CK_HEADSET  1               /* headphones / headset microphone (+ speakers on some) */
#define CK_AMP      2               /* speaker amplifier */
#define CK_SPKCODEC 3               /* codec with its own speaker amplifier (tablets) */

struct cdc;
struct cdcdrv {
    const char *name;
    const char *hid[6];             /* ACPI ids */
    u8 kind;
    u8 regbits;                     /* register address width: 8 or 16 */
    u8 valbits;                     /* register value width: 8 or 16 */
    u8 slotw;                       /* IPC3: I2S slot width the chip wants (0: 25 bit, 2.4 MHz) */
    int (*probe)(struct cdc *c);                    /* 0: the chip answers */
    int (*init)(struct cdc *c);                     /* reset, configure for c->ssp */
    void (*stream)(struct cdc *c, int dir, int on);
    void (*volume)(struct cdc *c, int ctl, u32 lr); /* MX_*: 0..65535 left | right << 16 */
    void (*source)(struct cdc *c, int src);         /* AS_* */
    int (*jack)(struct cdc *c);                     /* bit 0 headphones, bit 1 headset microphone */
    void (*off)(struct cdc *c);
};

struct cdc {
    const struct cdcdrv *drv;
    struct i2cbus *bus;
    struct acpi_dev *ad;
    struct dsp *d;
    u8 addr;
    u8 port;                        /* SSP */
    u8 jackstate;
    u8 started;
    u8 chan;                        /* amplifiers: index on the port (0 left / first) */
    u8 namps;                       /* amplifiers on the machine */
    u8 gpiodone;
    u8 aif;                         /* codec interface wired to the SSP: 0 / 1 AIF1, 2 AIF2 */
    u32 mclk;                       /* MCLK from the SSP, 0 when the codec runs from BCLK */
    struct sspcfg ssp;
    u32 jacktime;
    u32 priv[16];
};

/* register access in the width of the chip */
int cdc_rd(struct cdc *c, u32 reg);                         /* -1 on error */
int cdc_wr(struct cdc *c, u32 reg, u32 val);
int cdc_upd(struct cdc *c, u32 reg, u32 mask, u32 val);
void cdc_seq(struct cdc *c, const u16 *pairs, int n);      /* reg, value pairs */
int cdc_gpio(struct cdc *c, int idx, int val);              /* drive the idx-th GpioIo of the codec */
int cdc_ngpio(struct cdc *c);                               /* GpioIo resources of the codec */
/* MX_* level (0..65535) to a gain step 0..steps, linear in steps as with the HD Audio
   amplifiers (steps of equal dB), 0 only for level 0 */
u32 cdc_step(u32 v, u32 steps);
#define CDC_L(lr)   ((lr) & 0xFFFF)
#define CDC_R(lr)   ((lr) >> 16)
#define CDC_RATE    48000UL             /* the rate of every I2S endpoint */

/* dspdev.c calls these */
int cdc_setup(struct dsp *d);
int cdc_present(void);              /* ACPI lists a known I2S codec */
void cdc_stream(struct cdc *c, int dir, int on);
void cdc_volume(struct cdc *c, int ctl, u32 lr);
void cdc_source(struct cdc *c, int src);
void cdc_poll(struct cdc *c);
void cdc_off(struct cdc *c);
int cdc_has_volume(struct cdc *c);

/* dspdev.c */
struct dspep *dsp_ep_new(struct dsp *d, int type, int dir, int port);
int dsp_ep_start(struct dspep *e, struct ahw *hw);
void dsp_ep_stop(struct dspep *e);
u32 dsp_ep_pos(struct dspep *e);
struct adev *dsp_adev_new(struct dsp *d, const char *name, struct dspep *out, struct dspep *in,
                          struct cdc **cdcs, int ncdc);
void dsp_adev_jack(struct adev *a, struct dspep *hp, struct dspep *spk, struct cdc *jack);
int ipc3_trigger(struct dspep *e, int start);

/* SoundWire (sdw.c) */
int sdw_setup(struct dsp *d);
int sdw_present(void);

#endif
