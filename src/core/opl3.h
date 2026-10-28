/* opl3.h - OPL3 FM synthesizer (opl3.c) */
#ifndef OPL3_H
#define OPL3_H

#define OPL_RATE 49716

void opl_reset(void);
void opl_write(int reg, u8 val);        /* reg: bit 8 the second register bank */
void opl_render(s16 *out, int frames);  /* 16-bit stereo at OPL_RATE */
int opl_busy(void);                     /* an operator still sounds */

#endif
