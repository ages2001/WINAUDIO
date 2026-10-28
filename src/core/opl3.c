/* opl3.c - Yamaha YMF262 (OPL3) / YM3812 (OPL2) FM synthesizer, rendered at the chip's own
   rate (49716 Hz). Integer only. Operators work in the log domain like the chip: a log-sine
   lookup plus the attenuation (envelope, total level, key scale level, tremolo), then an
   exponent lookup. */
#include "core.h"
#include "opl3.h"
#include "opltab.h"

#define NOPS    36
#define NCH     18

enum { EG_OFF, EG_ATTACK, EG_DECAY, EG_SUSTAIN, EG_RELEASE };

struct oplop {
    u32 phase, inc;
    u16 env;                    /* 0 loudest .. 511 silent, 0.1875 dB steps */
    u16 tll;                    /* total level + key scale level, same units */
    u8 state;
    u8 key;                     /* bit 0 channel key on, bit 1 rhythm key on */
    u8 am, vib, egt, ksr, multx2, ksl, tl, ar, dr, sl, rr, ws;
    u8 rof;                     /* key scale rate offset */
    s16 out, prev;              /* last two outputs (feedback of the first operator) */
};

struct oplch {
    u16 fnum;
    u8 block, fb, cnt, lr;      /* lr: bit 0 left, bit 1 right */
    u8 pair;                    /* 4-operator: 1 first channel of a pair, 2 second */
    u8 rsv;
};

static struct oplop op[NOPS];
static struct oplch ch[NCH];
static u8 reg_bd, reg_08, reg_01, reg_104, reg_105;
static u32 egcnt;               /* envelope clock, one per sample */
static u16 amcnt;               /* tremolo position, 0..209, every 64 samples */
static u8 vibpos;               /* vibrato position, 0..7, every 1024 samples */
static u32 noise = 1;
static u16 trem;                /* current tremolo attenuation */
static u8 wmask;                /* waveforms available: 0 (OPL2 without WSE), 3 or 7 */

static const u8 multx2[16] = { 1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30 };
/* key scale level of octave 7 by the top 4 bits of the frequency, 0.1875 dB units */
static const u8 ksl7[16] = { 0, 48, 64, 74, 80, 86, 90, 94, 96, 100, 102, 104, 106, 108, 110, 112 };
/* envelope increments per sub-rate, 8 steps */
static const u8 egpat[4][8] = {
    { 0, 1, 0, 1, 0, 1, 0, 1 }, { 0, 1, 0, 1, 1, 1, 0, 1 },
    { 0, 1, 1, 1, 0, 1, 1, 1 }, { 0, 1, 1, 1, 1, 1, 1, 1 }
};
static const u8 egfast[4][8] = {
    { 1, 1, 1, 1, 1, 1, 1, 1 }, { 1, 1, 1, 2, 1, 1, 1, 2 },
    { 1, 2, 1, 2, 1, 2, 1, 2 }, { 1, 2, 2, 2, 1, 2, 2, 2 }
};

/* register offset (0..0x15 within a bank) -> operator, -1 none */
static int slot_op(int off)
{
    int g = off >> 3, s = off & 7;
    if (s > 5 || g > 2) return -1;
    return g * 6 + s;
}

/* operator index -> channel (2-operator view) */
static int op_ch(int o)
{
    int bank = o / 18, r = o % 18;
    return bank * 9 + (r / 6) * 3 + (r % 3);
}

static int ch_op1(int c)
{
    int bank = c / 9, r = c % 9;
    return bank * 18 + (r / 3) * 6 + (r % 3);
}

static int opl3(void)
{
    return reg_105 & 1;
}

/* the channel that holds frequency and key of a 4-operator pair */
static int freq_ch(int c)
{
    if (ch[c].pair == 2) return c - 3;
    return c;
}

static void op_update(struct oplop *p, int c)
{
    struct oplch *h = &ch[freq_ch(c)];
    u32 f = h->fnum, k;
    int kslv;
    if (p->vib) {
        /* vibrato: the frequency moves by up to 1/128 (deep) or 1/256 of itself */
        static const s8 vt[8] = { 0, 1, 2, 1, 0, -1, -2, -1 };
        s32 d = (s32)(f >> 7) * vt[vibpos];
        if (!(reg_bd & 0x40)) d >>= 1;
        f = (u32)((s32)f + d) & 0x3FF;
    }
    p->inc = ((f << h->block) * p->multx2) << 11;
    k = (u32)(h->block << 1) | ((reg_08 & 0x40) ? (h->fnum >> 8) & 1 : (h->fnum >> 9) & 1);
    p->rof = (u8)(p->ksr ? k : k >> 2);
    kslv = (int)ksl7[h->fnum >> 6] - 16 * (7 - h->block);
    if (kslv < 0 || !p->ksl) kslv = 0;
    else if (p->ksl == 1) ;             /* 3 dB / octave */
    else if (p->ksl == 2) kslv >>= 1;   /* 1.5 dB / octave */
    else kslv <<= 1;                    /* 6 dB / octave */
    p->tll = (u16)(p->tl * 4 + kslv);
}

static void ch_update(int c)
{
    int o = ch_op1(c);
    op_update(&op[o], c);
    op_update(&op[o + 3], c);
    if (ch[c].pair == 1) {
        op_update(&op[ch_op1(c + 3)], c + 3);
        op_update(&op[ch_op1(c + 3) + 3], c + 3);
    }
}

static void key(struct oplop *p, u8 bit, int on)
{
    u8 was = p->key;
    if (on) p->key |= bit;
    else p->key &= (u8)~bit;
    if (!was && p->key) {
        p->phase = 0;
        p->state = EG_ATTACK;
        if (p->ar >= 15) p->env = 0;
    } else if (was && !p->key) {
        p->state = EG_RELEASE;
    }
}

static void ch_key(int c, int on)
{
    int o = ch_op1(c);
    key(&op[o], 1, on);
    key(&op[o + 3], 1, on);
    if (ch[c].pair == 1) {
        int o2 = ch_op1(c + 3);
        key(&op[o2], 1, on);
        key(&op[o2 + 3], 1, on);
    }
}

static void set_pairs(void)
{
    static const u8 first[6] = { 0, 1, 2, 9, 10, 11 };
    int i;
    for (i = 0; i < NCH; i++) ch[i].pair = 0;
    if (!opl3()) return;
    for (i = 0; i < 6; i++)
        if (reg_104 & (1 << i)) {
            ch[first[i]].pair = 1;
            ch[first[i] + 3].pair = 2;
        }
    for (i = 0; i < NCH; i++) ch_update(i);
}

void opl_reset(void)
{
    int i;
    memset(op, 0, sizeof(op));
    memset(ch, 0, sizeof(ch));
    for (i = 0; i < NOPS; i++) {
        op[i].env = 511;
        op[i].state = EG_OFF;
        op[i].multx2 = 1;
    }
    reg_bd = reg_08 = reg_01 = reg_104 = reg_105 = 0;
    egcnt = 0;
    amcnt = 0;
    vibpos = 0;
    noise = 1;
}

void opl_write(int r, u8 v)
{
    int bank = (r >> 8) & 1, lo = r & 0xFF, o, c;
    struct oplop *p;
    if (bank && lo == 0x04) {
        reg_104 = (u8)(v & 0x3F);
        set_pairs();
        return;
    }
    if (bank && lo == 0x05) {
        reg_105 = (u8)(v & 1);
        set_pairs();
        return;
    }
    if (!bank && lo == 0x01) { reg_01 = v; return; }
    if (!bank && lo == 0x08) {
        reg_08 = v;
        for (c = 0; c < NCH; c++) ch_update(c);
        return;
    }
    if (!bank && lo == 0xBD) {
        u8 old = reg_bd;
        reg_bd = v;
        if (v & 0x20) {
            key(&op[12], 2, v & 0x10);     /* bass drum: both operators of channel 6 */
            key(&op[15], 2, v & 0x10);
            key(&op[13], 2, v & 0x01);     /* hi-hat */
            key(&op[16], 2, v & 0x08);     /* snare */
            key(&op[14], 2, v & 0x04);     /* tom-tom */
            key(&op[17], 2, v & 0x02);     /* cymbal */
        } else if (old & 0x20) {
            for (o = 12; o < 18; o++) key(&op[o], 2, 0);
        }
        return;
    }
    if (lo >= 0x20 && lo < 0xA0) {
        o = slot_op(lo & 0x1F);
        if (o < 0) return;
        o += bank * 18;
        p = &op[o];
        c = op_ch(o);
        switch (lo & 0xE0) {
        case 0x20:
            p->am = (u8)(v >> 7);
            p->vib = (u8)((v >> 6) & 1);
            p->egt = (u8)((v >> 5) & 1);
            p->ksr = (u8)((v >> 4) & 1);
            p->multx2 = multx2[v & 15];
            break;
        case 0x40:
            p->ksl = (u8)(v >> 6);
            p->tl = (u8)(v & 0x3F);
            break;
        case 0x60:
            p->ar = (u8)(v >> 4);
            p->dr = (u8)(v & 15);
            break;
        case 0x80:
            p->sl = (u8)(v >> 4);
            p->rr = (u8)(v & 15);
            break;
        }
        op_update(p, c);
        return;
    }
    if (lo >= 0xE0 && lo < 0xF6) {
        o = slot_op(lo - 0xE0);
        if (o < 0) return;
        op[o + bank * 18].ws = (u8)(v & 7);
        return;
    }
    c = lo & 15;
    if (c > 8) return;
    c += bank * 9;
    switch (lo & 0xF0) {
    case 0xA0:
        ch[c].fnum = (u16)((ch[c].fnum & 0x300) | v);
        ch_update(c);
        break;
    case 0xB0:
        ch[c].fnum = (u16)((ch[c].fnum & 0xFF) | ((v & 3) << 8));
        ch[c].block = (u8)((v >> 2) & 7);
        ch_update(c);
        if (ch[c].pair != 2) ch_key(c, v & 0x20);
        break;
    case 0xC0:
        ch[c].fb = (u8)((v >> 1) & 7);
        ch[c].cnt = (u8)(v & 1);
        ch[c].lr = (u8)(((v >> 4) | (v >> 6)) & 3);
        break;
    }
}

/* ---------------------------------------------------------------- rendering */

static void eg_step(struct oplop *p)
{
    u8 rate;
    int r, sub, s;
    u32 inc;
    switch (p->state) {
    case EG_ATTACK: rate = p->ar; break;
    case EG_DECAY: rate = p->dr; break;
    case EG_SUSTAIN:
        if (p->egt) return;
        rate = p->rr;
        break;
    case EG_RELEASE: rate = p->rr; break;
    default: return;
    }
    if (!rate) return;
    r = rate * 4 + p->rof;
    if (r > 63) r = 63;
    sub = r & 3;
    s = r >> 2;
    if (s <= 12) {
        u32 m = (1UL << (12 - s)) - 1;
        if (egcnt & m) return;
        inc = egpat[sub][(egcnt >> (12 - s)) & 7];
    } else {
        inc = (u32)egfast[sub][egcnt & 7] << (s - 13);
    }
    if (!inc) return;
    if (p->state == EG_ATTACK) {
        if (r >= 60) {
            p->env = 0;
        } else {
            /* exponential approach to 0 */
            s32 e = (s32)p->env - (s32)((((u32)p->env + 1) * inc + 7) >> 3);
            p->env = (u16)(e < 0 ? 0 : e);
        }
        if (!p->env) p->state = EG_DECAY;
        return;
    }
    p->env = (u16)(p->env + inc);
    if (p->env >= 511) {
        p->env = 511;
        p->state = EG_OFF;
        return;
    }
    if (p->state == EG_DECAY) {
        u16 sl = (u16)(p->sl == 15 ? 31 * 16 : p->sl * 16);
        if (p->env >= sl) {
            p->env = sl;
            p->state = EG_SUSTAIN;
        }
    }
}

/* log-domain waveform value at a 10-bit phase: attenuation in 1/256 octave, bit 15 sign;
   0x1FFF for no output */
static u16 wave(int ws, u32 ph)
{
    u32 q;
    ph &= 0x3FF;
    switch (ws) {
    case 1:
        if (ph & 0x200) return 0x1FFF;
        break;
    case 2:
        ph &= 0x1FF;
        break;
    case 3:
        if (ph & 0x100) return 0x1FFF;
        ph &= 0xFF;
        break;
    case 4:
        if (ph & 0x200) return 0x1FFF;
        ph = (ph << 1) & 0x3FF;
        break;
    case 5:
        if (ph & 0x200) return 0x1FFF;
        ph = (ph << 1) & 0x1FF;
        break;
    case 6:
        return (u16)((ph & 0x200) ? 0x8000 : 0);
    case 7:
        q = (ph & 0x200) ? (~ph & 0x1FF) : (ph & 0x1FF);
        return (u16)((q << 3) | ((ph & 0x200) ? 0x8000 : 0));
    }
    q = (ph & 0x100) ? (~ph & 0xFF) : (ph & 0xFF);
    return (u16)(opl_logsin[q] | ((ph & 0x200) ? 0x8000 : 0));
}

static s16 op_out(struct oplop *p, u32 ph)
{
    u32 att = (u32)p->env + p->tll + (p->am ? trem : 0), lev;
    u16 w;
    s32 v;
    if (att >= 511) return 0;
    w = wave(p->ws & wmask, ph);
    lev = (u32)(w & 0x1FFF) + (att << 3);
    if (lev >= 0x1000) return 0;
    v = (s32)(opl_exp[lev & 0xFF] >> (lev >> 8));
    return (s16)((w & 0x8000) ? -v : v);
}

static s16 op_run(struct oplop *p, s32 mod)
{
    return op_out(p, (p->phase >> 22) + (u32)mod);
}

static s32 fb_mod(struct oplop *p, int fb)
{
    if (!fb) return 0;
    return ((s32)p->out + p->prev) >> (9 - fb);
}

static void op_push(struct oplop *p, s16 v)
{
    p->prev = p->out;
    p->out = v;
}


static int silent(struct oplop *p)
{
    return p->state == EG_OFF;
}

void opl_render(s16 *out, int n)
{
    int i, c;
    int rhythm = (reg_bd & 0x20) != 0;
    wmask = (u8)(opl3() ? 7 : (reg_01 & 0x20) ? 3 : 0);
    for (i = 0; i < n; i++) {
        s32 l = 0, r = 0;
        /* LFOs */
        if (!(egcnt & 63)) {
            amcnt = (u16)(amcnt + 1 >= 210 ? 0 : amcnt + 1);
            trem = (u16)(amcnt < 105 ? amcnt : 209 - amcnt);
            trem = (u16)((reg_bd & 0x80) ? trem >> 2 : trem >> 4);
        }
        if (!(egcnt & 1023)) {
            int o;
            vibpos = (u8)((vibpos + 1) & 7);
            for (o = 0; o < NOPS; o++)
                if (op[o].vib) op_update(&op[o], op_ch(o));
        }
        for (c = 0; c < NOPS; c++)
            if (op[c].state != EG_OFF) eg_step(&op[c]);
        noise = (noise >> 1) ^ ((noise & 1) ? 0x400181UL : 0);
        for (c = 0; c < NCH; c++) {
            struct oplch *h = &ch[c];
            int o1 = ch_op1(c);
            struct oplop *a = &op[o1], *b = &op[o1 + 3];
            s32 v = 0;
            if (h->pair == 2) continue;
            if (rhythm && (c == 7 || c == 8)) continue;
            if (h->pair == 1) {
                struct oplch *h2 = &ch[c + 3];
                struct oplop *cc = &op[ch_op1(c + 3)], *d = &op[ch_op1(c + 3) + 3];
                s16 o1v, o2v, o3v, o4v;
                if (silent(a) && silent(b) && silent(cc) && silent(d)) goto adv4;
                o1v = op_run(a, fb_mod(a, h->fb));
                op_push(a, o1v);
                switch ((h->cnt << 1) | h2->cnt) {
                case 0:     /* 1 -> 2 -> 3 -> 4 */
                    o2v = op_run(b, o1v);
                    o3v = op_run(cc, o2v);
                    o4v = op_run(d, o3v);
                    v = o4v;
                    break;
                case 2:     /* 1 + (2 -> 3 -> 4) */
                    o2v = op_run(b, 0);
                    o3v = op_run(cc, o2v);
                    o4v = op_run(d, o3v);
                    v = (s32)o1v + o4v;
                    break;
                case 1:     /* (1 -> 2) + (3 -> 4) */
                    o2v = op_run(b, o1v);
                    o3v = op_run(cc, 0);
                    o4v = op_run(d, o3v);
                    v = (s32)o2v + o4v;
                    break;
                default:    /* 1 + (2 -> 3) + 4 */
                    o2v = op_run(b, 0);
                    o3v = op_run(cc, o2v);
                    o4v = op_run(d, 0);
                    v = (s32)o1v + o3v + o4v;
                    break;
                }
adv4:
                cc->phase += cc->inc;
                d->phase += d->inc;
            } else if (!(silent(a) && silent(b))) {
                s16 av = op_run(a, fb_mod(a, h->fb));
                op_push(a, av);
                if (rhythm && c == 6) v = 2 * (s32)op_run(b, h->cnt ? 0 : av);
                else if (h->cnt) v = (s32)av + op_run(b, 0);
                else v = op_run(b, av);
            }
            a->phase += a->inc;
            b->phase += b->inc;
            if (!opl3() || (h->lr & 1)) l += v;
            if (!opl3() || (h->lr & 2)) r += v;
        }
        if (rhythm) {
            struct oplop *hh = &op[13], *sd = &op[16], *tt = &op[14], *cy = &op[17];
            u32 p13 = hh->phase >> 22, p17 = cy->phase >> 22, ph;
            u32 b2 = (p13 >> 2) & 1, b3 = (p13 >> 3) & 1, b7 = (p13 >> 7) & 1;
            u32 c3 = (p17 >> 3) & 1, c5 = (p17 >> 5) & 1;
            u32 res1 = (b2 ^ b7) | b3, res2 = c3 ^ c5, nb = noise & 1;
            s32 v = 0;
            /* hi-hat */
            ph = (res1 | res2) ? 0x234 : 0xD0;
            if (ph & 0x200) { if (nb) ph = 0x2D0; }
            else if (nb) ph = 0x34;
            if (!silent(hh)) v += op_out(hh, ph);
            /* snare drum */
            ph = ((p13 >> 8) & 1) ? 0x200 : 0x100;
            if (nb) ph ^= 0x100;
            if (!silent(sd)) v += op_out(sd, ph);
            /* tom-tom */
            if (!silent(tt)) v += op_run(tt, 0);
            /* top cymbal */
            ph = (res1 | res2) ? 0x300 : 0x100;
            if (!silent(cy)) v += op_out(cy, ph);
            hh->phase += hh->inc; sd->phase += sd->inc;
            tt->phase += tt->inc; cy->phase += cy->inc;
            v *= 2;
            l += v;
            r += v;
        }
        egcnt++;
        if (l > 32767) l = 32767;
        if (l < -32768) l = -32768;
        if (r > 32767) r = 32767;
        if (r < -32768) r = -32768;
        out[i * 2] = (s16)l;
        out[i * 2 + 1] = (s16)r;
    }
}

int opl_busy(void)
{
    int i;
    for (i = 0; i < NOPS; i++)
        if (op[i].state != EG_OFF) return 1;
    return 0;
}
