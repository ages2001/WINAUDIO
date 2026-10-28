/* sbemu.h - Sound Blaster 16 emulation: DSP 4.05, CT1745 mixer, OPL3, MPU-401 UART, 8237 DMA
   channels and the interrupt, played through the sound engine */
#ifndef SBEMU_H
#define SBEMU_H

#define SBIO_FWD    0x100       /* sb_io: the platform does the real I/O (DOS: other DMA channels);
                                   a read ORs the low byte into what the hardware returns */

int sb_start(int vdma);         /* vdma: the platform virtualizes the DMA channels itself (VxD) */
u8 sb_dma_status(int ctl);         /* VxD: 8237 status bits of the emulated channels, ctl 0 / 1 */
void sb_stop(void);
int sb_active(void);            /* the emulation is in use: the engine keeps the device running */
u32 sb_io(u32 port, u32 val, int out);
void sb_vdma(int which, u32 lin, u32 len, int autoinit, int masked);   /* VxD: 0 8-bit, 1 16-bit channel */
void sb_produce(u32 frames, u32 rate);  /* the engine: device frames played since the last call */
void sb_reset_owner(void);      /* the program that used it is gone */
struct winsb *sb_cfg(void);

/* platform */
int plat_sb_start(void);                        /* traps, DMA channels, interrupt: 0 ok */
void plat_sb_irq(int level);                    /* interrupt line of the emulated card */
void plat_sb_dma(int which, u32 left, int tc);  /* VxD: bytes left in the DMA cycle, terminal count */
u8 *plat_sb_mem(u32 lin, u32 len);              /* DMA buffer, NULL if not reachable */
u32 plat_us(void);

#endif
