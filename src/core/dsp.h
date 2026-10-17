/* dsp.h - Intel audio DSP (Smart Sound Technology / Sound Open Firmware) behind the HD Audio
   controller: Skylake and Kaby Lake (cAVS 1.5, Intel firmware, IPC4), Apollo Lake to Ice Lake
   and Jasper / Elkhart Lake (cAVS 1.5+ to 2.0, SOF firmware, IPC3), Tiger Lake to Raptor Lake
   (cAVS 2.5, SOF IPC4), Meteor Lake to Nova Lake (ACE 1.x-4.x, SOF IPC4); and the older SST
   DSPs without HD Audio (ACPI devices): Bay Trail, Cherry Trail / Braswell, Broadwell (SOF IPC3).

   dsp.c      platforms, controller capabilities, DSP cores, firmware boot, IPC transport
   dspsst.c   Bay Trail / Cherry Trail / Broadwell: SST shim, firmware load, host page tables
   dspipc4.c  IPC4: pipelines of copiers (host DMA <-> HDA link / SSP / DMIC / SoundWire)
   dspipc3.c  IPC3: SOF topology messages (host, buffer, DAI components, DAI configuration)
   dspdev.c   the sound devices: HD Audio codecs through the DSP, I2S codecs, digital mics,
              SoundWire codecs
   nhlt.c     ACPI NHLT: the endpoints of the machine and their gateway configuration blobs */
#ifndef DSP_H
#define DSP_H
#include "audio.h"

/* ---- firmware image (platform: plat_fw_load) ----
   The file in memory: virtually contiguous for parsing, physical segments for DMA. */
#define FW_MAXSEG 64
struct fwimg {
    u8 *v;
    u32 size;
    int nseg;
    struct { u32 phys, len; } seg[FW_MAXSEG];
};
int plat_fw_load(const char *name, struct fwimg *img);  /* 0 = loaded; -1 = no such file */
void plat_fw_request(const char *name);                 /* DOS: the DOS driver loads it into XMS */

/* ---- platforms ---- */
#define DG_BYT  1                   /* Bay Trail, Cherry Trail, Broadwell: SST shim (dspsst.c) */
#define DG_SKL  2                   /* Skylake, Kaby Lake: cAVS 1.5, code loader DMA */
#define DG_APL  3                   /* Apollo Lake, Gemini Lake: cAVS 1.5+ */
#define DG_CNL  4                   /* Cannon / Coffee / Comet Lake: cAVS 1.8 */
#define DG_ICL  5                   /* Ice Lake, Jasper Lake: cAVS 2.0 */
#define DG_TGL  6                   /* Tiger / Alder / Raptor Lake, Elkhart Lake: cAVS 2.5 */
#define DG_MTL  7                   /* Meteor / Arrow Lake: ACE 1.x */
#define DG_LNL  8                   /* Lunar Lake: ACE 2.0 */
#define DG_PTL  9                   /* Panther / Wildcat Lake: ACE 3.0 */
#define DG_NVL  10                  /* Nova Lake: ACE 4.0 */

#define DPF_IPC4    0x01            /* firmware speaks IPC4 */
#define DPF_AVS     0x02            /* Intel's own firmware (AVS), else SOF */
#define DPF_ICCMAX  0x04            /* boot with the ICCMAX capture stream (ICL, TGL family) */
#define DPF_PROCEN  0x08            /* stream format written with PROCEN off (APL quirk) */

struct dspplat {
    const char *name;
    u8 gen, flags;
    u8 cores, hostcores;            /* host managed core mask */
    u8 ssps;                        /* SSP ports */
    u8 rsv;
    u16 rominit;                    /* ROM init timeout, ms */
    const char *fw;                 /* 8.3 file name: production (Intel) signed */
    const char *fwc;                /* community signed (Chromebooks) */
};

/* ---- audio formats (IPC4 audio_format layout) ---- */
struct afmt {
    u32 rate;
    u32 bits;                       /* container */
    u32 chmap;
    u32 chcfg;
    u32 ileave;
    u32 cfg;                        /* channels | valid bits << 8 | sample type << 16 */
};
#define AF_CH(f)    ((int)((f)->cfg & 0xFF))
#define AF_VBITS(f) ((int)(((f)->cfg >> 8) & 0xFF))
void afmt_set(struct afmt *f, u32 rate, int bits, int vbits, int ch);
u32 afmt_bytes_ms(const struct afmt *f);    /* bytes per millisecond */
u16 afmt_hda(const struct afmt *f);         /* HD Audio stream format word */

/* ---- endpoints: the paths the machine has ---- */
#define EP_HDA      1               /* HD Audio codec on the link (link DMA) */
#define EP_SSP      2               /* I2S codec / amplifier on an SSP port */
#define EP_DMIC     3               /* digital microphones on the PDM interface */
#define EP_SDW      4               /* SoundWire codec (ALH streams) */

#define MAXEP       12

/* I2S port settings (IPC3 firmware computes the SSP registers from these; IPC4 firmware
   takes the NHLT blob) */
struct sspcfg {
    u32 mclk, bclk;                 /* Hz */
    u16 slots, width, vbits;        /* TDM slots, slot width, valid bits */
    u8 fmt;                         /* 1 I2S, 2 right justified, 3 left justified, 4 DSP A, 5 DSP B */
    u8 provider;                    /* 1: the codec drives BCLK and FSYNC */
    u32 txmask, rxmask;
    u8 mclkid;                      /* MCLK output of the audio subsystem (0 or 1) */
};

struct dsp;
struct dspep {
    struct dsp *d;
    u8 type;                        /* EP_* */
    u8 dir;                         /* 0 playback, 1 capture */
    u8 port;                        /* SSP number / DMIC 0 / SoundWire link */
    u8 running;
    u8 prepared;
    u8 hidx, htag;                  /* host DMA stream descriptor index and tag */
    u8 lidx, ltag;                  /* HD Audio link DMA (EP_HDA; ACE 2+ SSP/DMIC) */
    u8 hppl, dppl;                  /* IPC4 pipelines: host copier, DAI copier */
    u16 hinst, dinst;               /* IPC4 copier instances */
    u32 c_host, c_buf1, c_buf2, c_dai, c_pipe;     /* IPC3 component ids */
    struct afmt hfmt;               /* host side (16 bit stereo for the sound engine) */
    struct afmt lfmt;               /* DAI / gateway side */
    const u8 *blob;                 /* gateway configuration (NHLT), dwords */
    u32 bloblen;                    /* bytes */
    u32 alhstream[4];               /* SoundWire: ALH stream numbers (PDI) */
    int nalh;
    u32 alhdev[4];                  /* SoundWire: link << 8 | PDI of each ALH stream */
    u32 alhmask[4];                 /* SoundWire: channels of the endpoint each ALH stream carries */
    u8 alhlidx[4], alhltag[4];      /* SoundWire on ACE 2+: HD Audio link DMA of each ALH stream */
    u8 alhgrp;                      /* IPC4: multi-gateway group + 1 (aggregated ALH streams) */
    struct sspcfg ssp;
    struct ahw *hw;                 /* ring of the sound engine */
    u32 *bdl;                       /* HD Audio BDL; SST: the compressed page table of the ring */
    u32 posoff;                     /* IPC3: position mailbox offset */
    u16 sstch;                      /* SST: host DMA controller << 8 | channel + 1, 0 not found */
    u16 rsv2;
};

/* ---- the DSP of one controller ---- */
struct dsp {
    const struct dspplat *pl;
    struct pcidev pci;
    u8 *hda;                        /* HD Audio registers (BAR 0) */
    u8 *bar;                        /* DSP registers and memory windows (BAR 4) */
    u8 *pp, *spib, *ml, *drsm;      /* HD Audio extended capabilities */
    int nin, nout;                  /* streams: capture 0..nin-1, playback nin.. */
    u16 streams_used;               /* host streams in use (bit per descriptor) */
    u16 links_used;                 /* link streams in use */
    /* IPC registers */
    u32 r_req, r_reqext, r_ack, r_ackdone, r_rsp, r_rspext, r_rspack, r_ctl;
    u32 r_rom, r_romerr;            /* ROM / firmware status */
    u32 win0, winsz;                /* memory window 0 offset, window size */
    u32 hostbox, dspbox;            /* host -> DSP, DSP -> host mailboxes */
    u32 hostboxsz, dspboxsz;
    u32 streambox;                  /* IPC3: stream position window */
    u8 ipc4, avs;
    u8 booted, failed;
    u8 shim;                        /* SST_* IPC doorbells of the SST shim, 0 cAVS / ACE */
    u8 bytcr;                       /* Bay Trail CR: the codec is on SSP0 */
    u8 *pcicfg;                     /* SST: PCI configuration space as memory (ACPI resource) */
    u32 barlen;                     /* SST: bytes of the DSP BAR */
    const u32 *xwin;                /* SST: memory windows of the firmware's extended manifest */
    u32 rsp_primary, rsp_ext;       /* last message from the DSP */
    /* firmware */
    struct fwimg fw;
    u32 payload;                    /* offset of the image the ROM loads */
    const char *fwname;
    /* IPC4 module table (manifest) */
    u16 copier_id;
    u16 copier_cfg, copier_ncfg;    /* module configurations (cpc, ibs, obs) */
    const u8 *modcfg;               /* manifest module configs (44 bytes each: par[4], is_pages, cps,
                                       ibs, obs, flags, cpc, obls) */
    u8 next_ppl;
    u16 next_inst;
    /* IPC3 */
    u32 next_comp;
    u32 abi;
    /* endpoints */
    struct dspep ep[MAXEP];
    int nep;
    u8 *buf;                        /* scratch: message payload (4K) */
    struct adev *adevs[6];          /* sound devices of the DSP */
    int nadev;
    u32 *iccbdl;                    /* ICCMAX stream */
    u8 *iccbuf;
    struct dsp *next;
};

#define SST_BYT     1               /* Bay Trail / Cherry Trail: 64-bit IPCX / IPCD, BUSY bit 63, DONE 62 */
#define SST_BDW     2               /* Broadwell: 32-bit IPCX / IPCD, BUSY bit 31, DONE 30 */

/* dsp.c */
const struct dspplat *dsp_platform(u16 vendor, u16 device);
int dsp_wait_fw_ready(struct dsp *d);
struct dsp *dsp_attach(struct pcidev *p, u8 *hda, int with_codecs);
void dsp_detach(struct dsp *d);
int dsp_ipc(struct dsp *d, u32 primary, u32 ext, const void *tx, u32 txlen, void *rx, u32 rxmax, u32 *rxlen);
int dsp_ipc3(struct dsp *d, const void *tx, u32 txlen, void *rx, u32 rxmax);
void dsp_mbox_write(struct dsp *d, u32 off, const void *p, u32 n);
void dsp_mbox_read(struct dsp *d, u32 off, void *p, u32 n);
int dsp_stream_get(struct dsp *d, int dir);        /* host stream descriptor, -1 if none */
void dsp_stream_put(struct dsp *d, int idx);
int dsp_stream_prepare(struct dsp *d, int idx, u32 *bdl, u8 *buf, u32 size, u32 period, u16 fmt);
void dsp_stream_run(struct dsp *d, int idx, int run);
u32 dsp_stream_pos(struct dsp *d, int idx);
int dsp_link_get(struct dsp *d, int dir);          /* HD Audio link DMA, -1 if none */
int dsp_link_tag(struct dsp *d, int idx);
void dsp_link_put(struct dsp *d, int idx);
void dsp_link_prepare(struct dsp *d, int idx, int tag, u16 fmt);
void dsp_link_run(struct dsp *d, int idx, int run);
void dsp_link_ssp_out(struct dsp *d, int tag, int on);  /* ACE 2+: SSP link output stream id valid */
void dsp_fw_prefetch(struct pcidev *pcis, int n);  /* DOS start: ask for firmware files */
void dsp_poll_all(void);
int dsp_wanted(void);               /* NHLT / ACPI show I2S, DMIC or SoundWire endpoints */
void dsp_remove(u32 bdf);           /* controller taken away (Plug and Play) */
void dsp_shutdown_all(void);
int dsp_functions(struct dsp *d, struct afunc *out, int max);   /* dspdev.c: Plug and Play */
extern struct dsp *dsp_list;

/* dspipc4.c / dspipc3.c: firmware ready, endpoints up and down */
int ipc4_ready(struct dsp *d);
int ipc4_ep_start(struct dspep *e);
void ipc4_ep_stop(struct dspep *e);
int ipc3_ready(struct dsp *d);
int ipc3_ep_start(struct dspep *e);
void ipc3_ep_stop(struct dspep *e);

/* dspsst.c: Bay Trail, Cherry Trail / Braswell, Broadwell */
int sst_probe(struct pcidev *pcis, int n);         /* ACPI (or PCI) SST devices started */
int sst_pci(const struct pcidev *p);               /* the PCI function is an SST DSP (not HD Audio) */
void sst_fw_prefetch(struct pcidev *pcis, int n);
void sst_down(struct dsp *d);
int sst_ep_start(struct dspep *e, struct ahw *hw);
void sst_ep_stop(struct dspep *e);
u32 sst_ep_pos(struct dspep *e);

/* dspdev.c */
int dspdev_setup(struct dsp *d, int with_codecs);
struct dspep *dspdev_hda_ep(struct dsp *d, int dir);   /* HD Audio codec path through the DSP */
int dsp_hda_start(struct dspep *e, struct ahw *hw, int *linktag);
void dsp_hda_stop(struct dspep *e);
u32 dsp_hda_pos(struct dspep *e);

/* nhlt.c */
#define NHLT_LINK_HDA   0
#define NHLT_LINK_DSP   1
#define NHLT_LINK_PDM   2
#define NHLT_LINK_SSP   3
#define NHLT_DEV_BT     0
#define NHLT_DEV_DMIC   1
#define NHLT_DEV_I2S    4
struct nhlt_ep {
    u8 link, dev, dir, bus;         /* link type, device type, 0 render / 1 capture, SSP port */
    u16 vid, did;
    u8 nfmt, mics;
};
int nhlt_load(void);                /* endpoints found */
int nhlt_count(void);
const struct nhlt_ep *nhlt_get(int i);
/* the gateway blob for a format; NULL if none matches. ch/rate/bits as wanted, vbits valid bits */
const u8 *nhlt_blob(int link, int dev, int dir, int bus, int ch, u32 rate, int bits, int vbits, u32 *len,
                    struct afmt *got);
int nhlt_best(int link, int dev, int dir, int bus, struct afmt *f);    /* preferred format */
int nhlt_ssp_decode(const u8 *blob, u32 len, u32 rate, u32 xtal, struct sspcfg *s);
u32 dsp_xtal(struct dsp *d);
int sdw_present(void);              /* sdw.c: the machine has SoundWire */        /* crystal clock of the audio subsystem, Hz */

#endif
