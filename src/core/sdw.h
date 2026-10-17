/* sdw.h - SoundWire on the Intel audio controller.
   sdw.c      the links (Intel shim + Cadence manager IP), enumeration of the peripherals,
              register access, streams (port programming, bank switch) and the sound devices
              built from the functions the peripherals offer
   sdw*.c     the peripheral drivers (codecs, amplifiers, microphone codecs)
   Audio data of a stream goes through the ALH gateways of the DSP firmware (EP_SDW
   endpoints in dsp.h). */
#ifndef SDW_H
#define SDW_H
#include "dspcdc.h"

/* ---- functions of a peripheral (sdwdrv.funcs, sdwport.func) ---- */
#define SF_JACK_OUT     0x01        /* headphones (headset jack) */
#define SF_JACK_IN      0x02        /* headset microphone */
#define SF_SPK          0x04        /* speaker amplifier (the codec's own or an amplifier chip) */
#define SF_DMIC         0x08        /* digital microphones behind the peripheral */

/* a data port of the peripheral serving one function */
struct sdwport {
    u8 func;                        /* SF_* (one bit) */
    u8 port;                        /* DPn number (1..14) */
    u8 ch;                          /* channels the function uses on it */
    u8 flags;                       /* SPF_* */
};
#define SPF_FULL        0x00        /* full data port (block offset 2, block packing) */
#define SPF_SIMPLE      0x01        /* simplified data port */
#define SPF_REDUCED     0x02        /* reduced data port */
#define SPF_TYPEMASK    0x03
#define SPF_PREPARE     0x10        /* channel prepare is not "simple": DPn_PrepareCtrl and wait
                                       for DPn_PrepareStatus */

struct sdwdev;
struct sdwdrv {
    const char *name;
    u16 mfr, part;                  /* manufacturer id and part id of the device id */
    u8 funcs;                       /* SF_* the chip offers */
    u8 nports;
    const struct sdwport *ports;
    /* after (re)enumeration: implementation-defined init, codec setup; 0 on success */
    int (*init)(struct sdwdev *s);
    /* a function's stream: on = 1 before its ports are enabled (power up the path, sample
       rate 48 kHz, unmute), on = 0 after they are disabled (mute, power down) */
    void (*stream)(struct sdwdev *s, int func, int on);
    /* MX_MASTER (playback of the function SF_JACK_OUT / SF_SPK it carries) or MX_INGAIN;
       lr = left 0..65535 | right << 16; NULL: the sound engine scales */
    void (*volume)(struct sdwdev *s, int ctl, u32 lr);
    /* polled every 300 ms: bit 0 headphones, bit 1 headset microphone, -1 error */
    int (*jack)(struct sdwdev *s);
    void (*off)(struct sdwdev *s);
};

struct sdwlink;
struct sdwdev {
    const struct sdwdrv *drv;
    struct sdwlink *link;
    struct dsp *d;
    u8 linkid;                      /* SoundWire link (manager) number */
    u8 devnum;                      /* device number on the link, 1..11 */
    u8 unique;                      /* unique id of the device id */
    u8 ver;                         /* SoundWire version of the device id */
    u8 cls;                         /* class id (1: SDCA) */
    u8 jackstate;
    u8 chan;                        /* amplifiers: index among all amplifiers (0, 1, ...) */
    u8 namps;                       /* amplifiers on the machine */
    u16 mfr, part;
    u8 attached;                    /* 1 while the device is attached to the bus */
    u8 page1, page2;                /* current SCP_AddrPage values (sdw.c) */
    u8 rsv;                         /* sdw.c flags */
    u32 jacktime;
    u32 priv[16];                   /* free for the driver */
    u32 busclk;                     /* bus clock of the link, Hz (sdw.c sets it) */
    u8 nopage;                      /* 0x8000..0xFFFF addressed directly (no SCP_AddrPage) */
    u8 rsv2[3];
};

/* ---- register access (32-bit SoundWire register addresses; paging is handled) ---- */
int sdw_rd(struct sdwdev *s, u32 reg);                  /* 0..255, -1 on error */
int sdw_wr(struct sdwdev *s, u32 reg, u8 val);          /* 0 on success */
int sdw_upd(struct sdwdev *s, u32 reg, u8 mask, u8 val);
void sdw_seq(struct sdwdev *s, const u32 *pairs, int n);    /* n (reg, value) pairs */
/* SDCA multi-byte controls: the MSB goes through SDW_SDCA_MBQ(reg), then the LSB to reg */
int sdw_rd16(struct sdwdev *s, u32 reg);                /* -1 on error */
int sdw_wr16(struct sdwdev *s, u32 reg, u16 val);

/* SDCA control address: function, entity, control selector, channel number */
#define SDW_SDCA_CTL(fun, ent, ctl, ch)  (0x40000000UL |                  \
        (((u32)(fun) & 7) << 22) | (((u32)(ent) & 0x40) << 15) |          \
        (((u32)(ent) & 0x3F) << 7) | (((u32)(ctl) & 0x30) << 15) |        \
        (((u32)(ctl) & 0x0F) << 3) | (((u32)(ch) & 0x38) << 12) |         \
        ((u32)(ch) & 7))
#define SDW_SDCA_MBQ(reg)   ((reg) | 0x2000UL)

/* peripheral registers every driver may need */
#define SDW_SCP_INT1            0x40
#define SDW_SCP_INTMASK1        0x41
#define SDW_SCP_SDCA_INT1       0x58
#define SDW_SCP_SDCA_INT2       0x59
#define SDW_SCP_SDCA_INT3       0x5A
#define SDW_SCP_SDCA_INT4       0x5B
#define SDW_SCP_SDCA_INTMASK1   0x5C
#define SDW_SCP_SDCA_INTMASK2   0x5D
#define SDW_SCP_SDCA_INTMASK3   0x5E
#define SDW_SCP_SDCA_INTMASK4   0x5F

/* dspdev.c / dspcdc.h use sdw_setup() and sdw_present() */
#endif
