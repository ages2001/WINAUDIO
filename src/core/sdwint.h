/* sdwint.h - internals of the SoundWire core (sdw.c, sdwlnk.c, sdwstm.c, sdwadv.c); the
   peripheral drivers only use sdw.h.

   sdwlnk.c   the Intel links: shim / HD Audio multi-link power, the Cadence manager IP
              (command FIFO, bank switch, data ports and PDIs)
   sdw.c      setup, enumeration, register access of the peripherals, driver table
   sdwstm.c   streams: transport parameters (bandwidth allocation), port programming, bank
              switch, channel prepare / enable, the DSP endpoint of a stream
   sdwadv.c   the sound devices built from the functions of the peripherals */
#ifndef SDWINT_H
#define SDWINT_H
#include "sdw.h"

#define SDW_MAXLINK     5
#define SDW_MAXPE       6           /* stream parts per link */
#define SDW_MAXSP       4           /* peripheral ports per stream part */
#define SDW_MAXDEV      16          /* peripherals of the machine */
#define SDW_BPS         24          /* bits per sample on the bus */
#define SDW_RATE        48000UL

/* register layouts */
#define LT_CAVS         1           /* CNL .. RPL: DSP BAR, shim 0x2C000, ALH 0x2C800 */
#define LT_ACE1         2           /* MTL / ARL: DSP BAR, shim 0x38000, ALH 0x24000 */
#define LT_ACE2         3           /* LNL and later: HD Audio BAR, multi-link sublinks */

/* state of a stream part */
#define PE_IDLE         0
#define PE_PREP         1           /* ports configured and prepared, channels off */
#define PE_ON           2           /* channels enabled */

/* a peripheral port of a stream part */
struct sdwsp {
    struct sdwdev *s;
    u8 port;                        /* DPn */
    u8 ch;                          /* channels of the port */
    u8 first;                       /* first channel of the link stream it carries */
    u8 flags;                       /* SPF_* */
    u8 att;                         /* attach count of the device when the stream started */
    u8 rsv[3];
};

struct sdwstrm;
struct sdwlink;

/* the part of a stream on one link: one manager port (PDI) and the peripheral ports */
struct sdwpe {
    struct sdwstrm *st;
    struct sdwlink *l;
    u8 used;
    u8 state;                       /* PE_* */
    u8 pdi;                         /* manager data port = PDI (2..), 0 none */
    u8 ch;                          /* channels of the link stream */
    u32 chmask;                     /* channels of the endpoint this link carries */
    u16 bo;                         /* block offset (bandwidth allocation) */
    u8 hstart, hstop;
    u8 nsp;
    u8 rsv;
    struct sdwsp sp[SDW_MAXSP];
};

/* a stream: one function of one or more peripherals, on one or more links */
struct sdwstrm {
    u8 func;                        /* SF_* */
    u8 dir;                         /* 0 playback, 1 capture */
    u8 npe;
    u8 running;
    struct sdwpe *pe[SDW_MAXLINK];
    struct dspep *ep;
    struct dsp *d;
};

struct sdwlink {
    struct dsp *d;
    u8 id;
    u8 lt;                          /* LT_* */
    u8 up;                          /* powered and running */
    u8 bank;                        /* current register bank */
    u8 *cdns;                       /* Cadence manager registers */
    u32 ipoff;                      /* offset of the Cadence IP registers */
    u8 *shim;                       /* cAVS / ACE1: shim of all links; ACE2: this link's shim */
    u8 *vs;                         /* ACE2: vendor specific shim */
    u8 *alh;                        /* cAVS / ACE1: ALH */
    u32 ipclk;                      /* IP clock (Hz) */
    u32 mclk;                       /* manager clock: IP clock / 2 */
    u32 busclk;                     /* bus clock (Hz) */
    u16 rows, cols;
    u8 rowidx, colidx;
    u8 npdi;                        /* PCM data interfaces (bidirectional) */
    u8 msgcount;                    /* RX FIFO watermark programmed */
    u8 pdich[16];                   /* channels of each PDI */
    u16 pdiused;
    u16 assigned;                   /* device numbers in use */
    u8 ndev;
    u8 rsv;
    struct sdwdev *dev[12];         /* by device number */
    struct sdwpe pe[SDW_MAXPE];
    u32 polltime;
    u32 quirk;                      /* intel-quirk-mask */
    char subprop[8];                /* ACPI name of the link subproperties */
};

/* the controller */
struct sdwctl {
    struct dsp *d;
    u8 lt;
    u8 nlink;
    u8 mlcs;                        /* cAVS / ACE1: link clock source selectable */
    u8 lss;                         /* ACE2: link synchronization (LSYNC) */
    u8 *eml;                        /* ACE2: multi-link registers of the SoundWire link */
    u8 *base;                       /* ACE2: mapped SoundWire register space */
    u32 lmask;                      /* links powered */
    struct sdwlink link[SDW_MAXLINK];
    struct sdwdev dev[SDW_MAXDEV];
    int ndev;
    int users;                      /* sound devices using the links */
    u8 attcnt[SDW_MAXDEV];          /* attaches of each device (a re-attach loses its ports) */
};
extern struct sdwctl sdwc;

/* response of a transfer */
#define SR_OK       0
#define SR_IGNORED  1               /* no device acknowledged */
#define SR_FAIL     (-1)            /* NAK, timeout */

/* sdwlnk.c */
int lnk_probe(struct dsp *d);                       /* links in hardware */
int lnk_power(struct sdwlink *l, int on);
int lnk_start(struct sdwlink *l);                   /* bus out of reset, peripherals attach */
int lnk_cmds(struct sdwlink *l, int dev, int write, u16 addr, u8 *buf, int n);
u32 lnk_status(struct sdwlink *l);                  /* 2 bits per device number */
int lnk_bank_switch(struct sdwlink **ls, int n);
int lnk_pdi_get(struct sdwlink *l, int ch);
void lnk_pdi_put(struct sdwlink *l, int pdi);
u32 lnk_alh_id(struct sdwlink *l, int pdi);
void lnk_pdi_setup(struct sdwlink *l, struct sdwpe *pe);
void lnk_dpn(struct sdwlink *l, struct sdwpe *pe, int bank);
void lnk_dpn_off(struct sdwlink *l, struct sdwpe *pe, int bank);
void lnk_clk_bank(struct sdwlink *l, int bank);
void lnk_ace2_map(struct sdwlink *l, int pdi, int ch, int tag, int dir);
void lnk_ace2_losidv(int tag, int on);
int sdw_wait32(u8 *p, u32 mask, u32 val, u32 ms);

/* sdw.c */
const struct sdwport *sdw_dev_port(struct sdwdev *s, int func);
int sdw_dev_init(struct sdwdev *s);
void sdw_wait_resets(void);                 /* driver init: 0 done, 1 reset pending, -1 failed */
#define SDF_READY   0x01                            /* sdwdev.rsv: driver init may run */
#define SDF_OFF     0x02                            /* sdwdev.rsv: powered down */
#define SDF_FAILED  0x04                            /* sdwdev.rsv: no driver could set it up */
#define SDF_RESET   0x08                            /* sdwdev.rsv: the driver reset it, init again on attach */
void sdw_link_poll(struct sdwlink *l);
void sdw_links_down(void);

/* sdwstm.c */
int sdw_strm_start(struct sdwstrm *st, struct ahw *hw);
void sdw_strm_stop(struct sdwstrm *st);
int sdw_strm_stale(struct sdwstrm *st);            /* a device of the running stream re-attached */
struct sdwpe *sdw_pe_new(struct sdwlink *l, struct sdwstrm *st);

/* sdwadv.c */
int sdw_devices(struct dsp *d);

#endif
