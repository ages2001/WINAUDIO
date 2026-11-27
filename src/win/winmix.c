/* winmix.c - WINMIX.EXE: volume control for the sound devices of WINAUDIO.SYS and WINUSB.SYS
   (master, wave, synthesizer, CD, line, recording level and recording source).
   Windows 3.0 and 3.1. */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "winapi.h"

DWORD WINAPI GlobalDosAlloc(DWORD cb);
UINT WINAPI GlobalDosFree(UINT sel);

#define MK_FP(s, o) ((void __far *)(((DWORD)(s) << 16) | (WORD)(o)))
#define NCORE   2
#define MAXDEV  16
#define NCTL    6
#define IDD_MIX 500   /* the dialog template of winmix.rc */
#define ID_DEV  100
#define ID_SRC  101
#define ID_VAL  200   /* the six level fields */
#define ID_LOCK 300
#define ID_MUTE 301
#define ID_TONE 302
#define ID_REPORT 303
#define ID_TXT  400   /* the status line */

struct mdev {
    BYTE core, id, caps, insrcs, insrc, hwvol, type, gone;
    char name[48];
};

static const BYTE mplexes[NCORE] = { WIN_MPLEX_AUDIO, WIN_MPLEX_USB };
static const WORD vxdids[NCORE] = { WIN_VXD_AUDIO, WIN_VXD_USB };
static BYTE present[NCORE];
static DWORD vxdentry[NCORE];
static WORD reqsel, reqseg;
static struct winapireq FAR *rq;
static struct mdev devs[MAXDEV], nd2[MAXDEV];
static int ndev, cur;
static HWND hmain, hdev, hsrc, hval[NCTL], hlock, hmute, hstat;
static DWORD vals[NCTL];
static const char *const ctlnames[NCTL] = { "Master", "Wave", "Synth", "Rec level", "CD", "Line" };
static const char *const srcnames[5] = { "Microphone", "Line in", "CD", "Aux", "Stereo mix" };
static HINSTANCE hinst;
static HBRUSH hface;

static void fmemset(void FAR *d, BYTE v, WORD n)
{
    BYTE FAR *p = d;
    while (n--) *p++ = v;
}

static int realmode;          /* Windows 3.0 real mode: no DPMI */

static int api(int k, WORD fn, DWORD a0, DWORD a1, DWORD a2, DWORD buf, DWORD len)
{
    WORD rseg = reqseg, mx = ((WORD)mplexes[k] << 8) | 1;
    DWORD ent = vxdentry[k];
    if (!rq || !present[k]) return WINE_NOCORE;
    fmemset(rq, 0, sizeof(*rq));
    rq->fn = fn;
    rq->status = 0xFFFF;
    rq->arg0 = a0;
    rq->arg1 = a1;
    rq->arg2 = a2;
    rq->buf = buf;
    rq->buflen = len;
    if (ent) {
        _asm {
            mov ax, 3
            mov cx, rseg
            xor dx, dx
            call dword ptr ent
        }
    } else if (realmode) {
        /* real mode (Windows 3.0 WIN /R): the driver is called directly, ES:BX -> request */
        _asm {
            push es
            mov ax, mx
            mov es, rseg
            xor bx, bx
            int 2Fh
            pop es
        }
    } else {
        static struct {
            DWORD edi, esi, ebp, rsv, ebx, edx, ecx, eax;
            WORD flags, es, ds, fs, gs, ip, cs, sp, ss;
        } rmc;
        fmemset(&rmc, 0, sizeof(rmc));
        rmc.eax = mx;
        rmc.es = rseg;
        _asm {
            push es
            push di
            push ds
            pop es
            lea di, rmc
            mov ax, 0300h
            mov bx, 002Fh
            xor cx, cx
            int 31h
            pop di
            pop es
        }
    }
    return rq->status;
}

static int same(const struct mdev *a, const struct mdev *b)
{
    return a->core == b->core && a->id == b->id && a->gone == b->gone && a->caps == b->caps &&
           !lstrcmp(a->name, b->name);
}

/* device list of both drivers; returns 1 when it changed (USB audio plugged in or removed) */
static int list_devs(void)
{
    int k, i, n = 0, changed;
    for (k = 0; k < NCORE; k++) {
        int nd;
        struct winauddev FAR *d;
        if (!present[k] || api(k, WINF_AUD_DEVS, 0, 0, 0, ((DWORD)reqseg << 16) | 64, 1024 - 64)) continue;
        nd = (int)(rq->retlen / sizeof(struct winauddev));
        d = MK_FP(reqsel, 64);
        for (i = 0; i < nd && n < MAXDEV; i++, d++) {
            struct mdev *m = &nd2[n++];
            m->core = (BYTE)k;
            m->id = d->id;
            m->caps = d->caps;
            m->insrcs = d->insrcs;
            m->insrc = d->insrc;
            m->hwvol = d->hwvol;
            m->type = d->type;
            m->gone = d->gone;
            lstrcpy(m->name, d->name);
        }
    }
    changed = n != ndev;
    for (i = 0; i < n && !changed; i++)
        if (!same(&nd2[i], &devs[i])) changed = 1;
    for (i = 0; i < n; i++) devs[i] = nd2[i];
    ndev = n;
    return changed;
}

static void scan(void)
{
    int k;
    DWORD r = GlobalDosAlloc(8192);   /* room for the driver log too */
    realmode = !(GetWinFlags() & WF_PMODE);
    if (!r) return;
    reqsel = LOWORD(r);
    reqseg = HIWORD(r);
    rq = MK_FP(reqsel, 0);
    for (k = 0; k < NCORE; k++) {
        BYTE id = mplexes[k], r_al = 0;
        WORD r_bx = 0, vid = vxdids[k], es_ = 0, di_ = 0;
        _asm {
            push es
            push di
            mov ah, id
            mov al, 0
            xor bx, bx
            int 2Fh
            mov r_al, al
            mov r_bx, bx
            pop di
            pop es
        }
        if (r_al == 0xFF && r_bx == 0x3357) present[k] = 1;
        if (GetWinFlags() & WF_ENHANCED) {
            _asm {
                push es
                push di
                xor di, di
                mov es, di
                mov ax, 1684h
                mov bx, vid
                int 2Fh
                mov es_, es
                mov di_, di
                pop di
                pop es
            }
            if (es_ || di_) {
                DWORD ent = ((DWORD)es_ << 16) | di_;
                WORD started = 0;
                vxdentry[k] = ent;
                _asm {                  /* Windows 95/98/ME: the VxD may run without the DOS driver */
                    xor ax, ax
                    xor bx, bx
                    call dword ptr ent
                    mov started, bx
                }
                if (started) present[k] = 1;
            }
        }
    }
    list_devs();
}

/* ---------------------------------------------------------------- without the window

   WINMIX /UNMUTE            master unmuted (and at 100%), all devices
   WINMIX /MUTE              master muted
   WINMIX /MASTER 0..100     master level, all devices
   WINMIX /TONE              a 2 s 440 Hz tone through the driver itself (not MMSYSTEM)
   WINMIX /DUMP [file]       the devices, every mixer value, the mute state and the driver
                             log, as text (C:\\WINMIX.TXT when no file is given)

   A screen reader can read the report and a batch file can set the volumes; nothing of the
   window is needed. The same API the window uses is called. */

#define RINGDATA 32768UL

static HFILE dumph = HFILE_ERROR;
static OFSTRUCT dumpof;

static void dwr(const void *p, UINT n)
{
    if (dumph != HFILE_ERROR) _lwrite(dumph, (const void __huge *)p, n);
}

static void dstr(const char *s) { dwr(s, (UINT)lstrlen(s)); }

static void dfmt(const char *fmt, ...)
{
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsprintf(b, fmt, ap);
    va_end(ap);
    dstr(b);
}

static int pct(DWORD v) { return (int)((v * 100UL + 32767UL) / 65535UL); }

static void dump_log(int k, const char *name)
{
    DWORD len, q;
    BYTE FAR *p;
    if (!present[k]) return;
    dfmt("\r\n%s driver log:\r\n", name);
    if (api(k, WINF_LOG, 0, 0, 0, ((DWORD)reqseg << 16) | 64, 8192UL - 64)) {
        dfmt("  (not available)\r\n");
        return;
    }
    len = rq->retlen;
    p = MK_FP(reqsel, 64);
    if (!len) {
        dfmt("  (empty)\r\n");
        return;
    }
    for (q = 0; q < len; q++) {
        char c = (char)p[q];
        if (c == '\n') dwr("\r\n", 2);
        else if (c != '\r') dwr(&c, 1);
    }
    dfmt("\r\n");
}

static void dump_all(void)
{
    static const char *const tn[] = { "?", "HD Audio", "AC'97", "USB Audio", "AudioPCI", "EMU10K1" };
    int k, i, j, n = 0;
    dfmt("WINMIX 1.2 report\r\n");
    dfmt("WINAUDIO.SYS: %s\r\n", present[0] ? "loaded" : "not loaded");
    dfmt("WINUSB.SYS: %s\r\n", present[1] ? "loaded" : "not loaded");
    for (k = 0; k < NCORE; k++) {
        if (!present[k]) continue;
        for (i = 0; i < ndev; i++) {
            struct mdev *m = &devs[i];
            if (m->core != k) continue;
            dfmt("\r\ndevice %d: %s%s\r\n", n++, m->name, m->gone ? " (removed)" : "");
            dfmt("  type: %s\r\n", m->type < 6 ? tn[m->type] : "?");
            for (j = 0; j < MX_COUNT; j++)
                if (!api(k, WINF_AUD_MIX, (DWORD)m->id, (DWORD)j, 0, 0, 0))
                    dfmt("  %s: left %d%%, right %d%%\r\n", ctlnames[j], pct(rq->retlen & 0xFFFF),
                         pct(rq->retlen >> 16));
            if (!api(k, WINF_AUD_MIX, (DWORD)m->id, (DWORD)MX_MUTE, 0, 0, 0))
                dfmt("  mute: %s\r\n", rq->retlen ? "ON" : "off");
            if (!api(k, WINF_AUD_MIX, (DWORD)m->id, (DWORD)MX_SRC, 0, 0, 0))
                for (j = 0; j < 5; j++)
                    if (rq->retlen & (1UL << j)) dfmt("  recording source: %s\r\n", srcnames[j]);
        }
    }
    dump_log(0, "WINAUDIO");
    dump_log(1, "WINUSB");
}

static int set_mute(int on)
{
    int k, i, n = 0;
    for (k = 0; k < NCORE; k++)
        if (present[k])
            for (i = 0; i < ndev; i++)
                if (devs[i].core == k) {
                    api(k, WINF_AUD_MIX, (DWORD)devs[i].id, (DWORD)MX_MUTE | 0x80000000UL, (DWORD)on, 0, 0);
                    if (!on) api(k, WINF_AUD_MIX, (DWORD)devs[i].id, (DWORD)MX_MASTER | 0x80000000UL,
                                 0xFFFFFFFFUL, 0, 0);
                    n++;
                }
    return n;
}

static int set_master(int pc)
{
    DWORD v = (DWORD)pc * 65535UL / 100UL;
    int k, i, n = 0;
    for (k = 0; k < NCORE; k++)
        if (present[k])
            for (i = 0; i < ndev; i++)
                if (devs[i].core == k) {
                    api(k, WINF_AUD_MIX, (DWORD)devs[i].id, (DWORD)MX_MASTER | 0x80000000UL,
                        (v << 16) | v, 0, 0);
                    n++;
                }
    return n;
}

static void rput(struct winring FAR *r, const BYTE FAR *p, UINT n)
{
    BYTE FAR *d = (BYTE FAR *)r + 32;
    DWORD sz = r->size;
    while (n) {
        UINT off = (UINT)(r->wr % sz), k = (UINT)(sz - off < n ? sz - off : n);
        _fmemcpy(d + off, p, k);
        r->wr += k;
        p += k;
        n -= k;
    }
}

/* the driver's own wave path: a tone that does not go through MMSYSTEM and WINSOUND.DRV */
static DWORD tone_rd, tone_pos, tone_total;

static int tone(int secs)
{
    DWORD r, t0, total, done = 0, devid = 0;
    WORD sel, seg;
    struct winring FAR *ring;
    static short sbuf[1024];
    unsigned ph = 0, step = (unsigned)((440UL << 16) / 44100UL);
    MSG msg;
    int i, k, core = -1, sid, st;
    for (k = 0; k < NCORE && core < 0; k++)
        if (present[k])
            for (i = 0; i < ndev; i++)
                if (devs[i].core == k) {
                    devid = devs[i].id;
                    core = k;
                    break;
                }
    if (core < 0) return -2;
    r = GlobalDosAlloc(RINGDATA + 32);
    if (!r) return -1;
    sel = LOWORD(r);
    seg = HIWORD(r);
    ring = MK_FP(sel, 0);
    fmemset(ring, 0, 32);
    ring->magic = WINRING_MAGIC;
    ring->size = RINGDATA;
    st = api(core, WINF_AUD_OPEN, devid, 44100UL, 16UL | (2UL << 8), (DWORD)seg << 16, 0);
    if (st) {
        GlobalDosFree(sel);
        return st;
    }
    sid = (int)rq->retlen;
    api(core, WINF_AUD_CTL, (DWORD)sid, AC_START, 0, 0, 0);
    total = 44100UL * (DWORD)secs;
    t0 = GetTickCount();
    while (done < total) {
        if (RINGDATA - (ring->wr - ring->rd) >= sizeof(sbuf)) {
            k = (int)(total - done < 512 ? total - done : 512);
            for (i = 0; i < k; i++) {
                long x = (long)(ph & 0x7FFF) - 16384;
                long y = 32767L - (x * x) / 8192L;
                short v = (short)(((ph & 0x8000) ? -y : y) / 4);
                sbuf[i * 2] = v;
                sbuf[i * 2 + 1] = v;
                ph += step;
            }
            rput(ring, (const BYTE FAR *)sbuf, (UINT)k * 4);
            done += k;
        }
        if (GetTickCount() - t0 > (DWORD)secs * 1000UL + 3000UL) break;
        PeekMessage(&msg, NULL, 0, 0, PM_REMOVE);
    }
    while (ring->pos != ring->wr && GetTickCount() - t0 < (DWORD)secs * 1000UL + 5000UL)
        PeekMessage(&msg, NULL, 0, 0, PM_REMOVE);
    tone_rd = ring->rd;
    tone_pos = ring->pos;
    tone_total = done * 4;
    api(core, WINF_AUD_CTL, (DWORD)sid, AC_CLOSE, 0, 0, 0);
    GlobalDosFree(sel);
    return 0;
}

static int command(LPSTR cmd)
{
    char w[16], path[120];
    int i = 0, n, st;
    while (*cmd == ' ') cmd++;
    while (cmd[i] && cmd[i] != ' ' && i < 15) {
        w[i] = cmd[i];
        i++;
    }
    w[i] = 0;
    for (n = 0; w[n]; n++) if (w[n] >= 'a' && w[n] <= 'z') w[n] -= 32;
    cmd += i;
    while (*cmd == ' ') cmd++;
    if (!lstrcmp(w, "/HELP") || !lstrcmp(w, "/?")) {
        MessageBox(NULL, "WINMIX commands:\r\n"
                   "  /UNMUTE        master unmuted, at 100%\r\n"
                   "  /MUTE          master muted\r\n"
                   "  /MASTER 0..100 master level\r\n"
                   "  /TONE          two seconds of 440 Hz through the driver\r\n"
                   "  /DUMP file     levels, mute and driver log as text\r\n"
                   "  /HELP          this list",
                   "WINMIX", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    if (!lstrcmp(w, "/UNMUTE")) {
        n = set_mute(0);
        MessageBox(NULL, n ? "Master unmuted and set to 100%." : "No sound device.", "WINMIX",
                   MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    if (!lstrcmp(w, "/MUTE")) {
        n = set_mute(1);
        MessageBox(NULL, n ? "Master muted." : "No sound device.", "WINMIX", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    if (!lstrcmp(w, "/MASTER")) {
        n = atoi(cmd);
        if (n < 0) n = 0;
        if (n > 100) n = 100;
        n = set_master(n);
        MessageBox(NULL, n ? "Master volume set." : "No sound device.", "WINMIX", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    if (!lstrcmp(w, "/TONE")) {
        st = tone(2);
        if (st == -1)
            MessageBox(NULL, "WINMIX: not enough DOS memory for the tone.", "WINMIX", MB_OK | MB_ICONSTOP);
        else if (st == -2)
            MessageBox(NULL, "No sound device.", "WINMIX", MB_OK | MB_ICONINFORMATION);
        else if (st)
            MessageBox(NULL, "WINMIX: the tone could not be played.", "WINMIX", MB_OK | MB_ICONSTOP);
        else {
            char msg[192];
            wsprintf(msg, "Test tone finished: the card took %lu of %lu bytes (driver %lu).",
                     tone_pos, tone_total, tone_rd);
            MessageBox(NULL, msg, "WINMIX", MB_OK | MB_ICONINFORMATION);
        }
        return 0;
    }
    if (!lstrcmp(w, "/DUMP")) {
        n = lstrlen(cmd);
        if (n >= 2 && cmd[0] == '"' && cmd[n - 1] == '"') {
            cmd++;
            n -= 2;
        }
        if (n <= 0 || n > 110) lstrcpy(path, "C:\\WINMIX.TXT");
        else {
            for (i = 0; i < n; i++) path[i] = cmd[i];
            path[n] = 0;
        }
        dumph = OpenFile(path, &dumpof, OF_CREATE | OF_WRITE);
        if (dumph == HFILE_ERROR) {
            MessageBox(NULL, "WINMIX: cannot write the report file.", "WINMIX", MB_OK | MB_ICONSTOP);
            return 1;
        }
        dump_all();
        _lclose(dumph);
        dumph = HFILE_ERROR;
        {
            char msg[160];
            wsprintf(msg, "The report was written to %s.", path);
            MessageBox(NULL, msg, "WINMIX", MB_OK | MB_ICONINFORMATION);
        }
        return 0;
    }
    return -1;                  /* not a command: the window */
}

/* ---------------------------------------------------------------- the window

   The window is a dialog made of the standard controls. Each level is a label with an edit
   field beside it, so a screen reader has text to read, Tab reaches every field and the arrows
   change the value. The window used bare scrollbars before: no text, no focus, no name. */

static void status(const char *s)
{
    if (hstat) SetWindowText(hstat, s);
}

/* the number in a level field, -1 when no number is there */
static int read_level(int i)
{
    char t[24];
    int n, dig = 0;
    GetWindowText(hval[i], t, sizeof(t));
    for (n = 0; t[n]; n++)
        if (t[n] >= '0' && t[n] <= '9') dig = 1;
    if (!dig) return -1;
    n = atoi(t);
    if (n < 0) n = 0;
    if (n > 100) n = 100;
    return n;
}

static void show_level(int i)
{
    char t[16];
    wsprintf(t, "%d%%", pct(LOWORD(vals[i])));
    SetWindowText(hval[i], t);
}

/* the volumes of the device as WINSOUND.DRV keeps them (SYSTEM.INI [winsound.drv]): set again
   when Windows starts */
static void save(struct mdev *m)
{
    static const WORD ctls[NCTL + 2] = { 0, 1, 2, 3, 4, 5, MX_SRC, MX_MUTE };
    char key[16], val[100], old[100];
    DWORD v[NCTL + 2];
    int i;
    for (i = 0; i < NCTL + 2; i++) {
        if (api(m->core, WINF_AUD_MIX, m->id, ctls[i], 0, 0, 0)) return;
        v[i] = rq->retlen;
    }
    wsprintf(val, "%lX %lX %lX %lX %lX %lX %lX %lX", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
    wsprintf(key, "Volume%d.%d", m->core, m->id);
    GetPrivateProfileString("winsound.drv", key, "", old, sizeof(old), "SYSTEM.INI");
    if (lstrcmp(old, val)) WritePrivateProfileString("winsound.drv", key, val, "SYSTEM.INI");
}

static void set_level(int i, int pc)
{
    struct mdev *m = &devs[cur];
    DWORD v = (DWORD)pc * 65535UL / 100UL, l = v, r = v;
    if (ndev <= 0 || m->gone) return;
    if (!SendMessage(hlock, BM_GETCHECK, 0, 0)) {
        /* keep the balance: scale both sides */
        DWORD ol = LOWORD(vals[i]), or = HIWORD(vals[i]), mx = ol > or ? ol : or;
        if (mx) {
            l = ol * v / mx;
            r = or * v / mx;
        }
    }
    api(m->core, WINF_AUD_MIX, m->id, (DWORD)i | 0x80000000L, l | (r << 16), 0, 0);
    vals[i] = l | (r << 16);
    save(m);
}

static void set_source(void)
{
    struct mdev *m = &devs[cur];
    int sel = (int)SendMessage(hsrc, CB_GETCURSEL, 0, 0), s, idx = 0;
    if (ndev <= 0 || m->gone) return;
    for (s = 0; s < 5; s++)
        if (m->insrcs & (1 << s)) {
            if (idx++ == sel) {
                api(m->core, WINF_AUD_MIX, m->id, MX_SRC | 0x80000000L, 1UL << s, 0, 0);
                save(m);
                break;
            }
        }
}

static void fill_devices(void)
{
    int i;
    SendMessage(hdev, CB_RESETCONTENT, 0, 0);
    for (i = 0; i < ndev; i++) {
        char t[80];
        wsprintf(t, "%d: %s%s%s", i, (LPSTR)devs[i].name, (LPSTR)(devs[i].core ? " (USB)" : ""),
                 (LPSTR)(devs[i].gone ? " - unplugged" : ""));
        SendMessage(hdev, CB_ADDSTRING, 0, (LPARAM)(LPSTR)t);
    }
    if (!ndev) SendMessage(hdev, CB_ADDSTRING, 0, (LPARAM)(LPSTR)"No sound device");
    if (cur >= ndev) cur = 0;
    SendMessage(hdev, CB_SETCURSEL, cur, 0);
}

/* the levels, the mute and the recording source of the device in the list. While the fields
   are set again, leaving one must not write its old number to the device. */
static int refreshing;

static void show_values(void)
{
    struct mdev *m = ndev > 0 ? &devs[cur] : NULL;
    int i, s, sel = -1, idx = 0, ok = m != NULL && !m->gone;
    refreshing = 1;
    for (i = 0; i < NCTL; i++) {
        int en = 0;
        vals[i] = 0;
        if (ok && !api(m->core, WINF_AUD_MIX, m->id, i, 0, 0, 0)) {
            vals[i] = rq->retlen;
            en = 1;
        }
        if (en && i == 0 && !(m->caps & WINAC_OUT)) en = 0;
        if (en && i == 3 && !(m->caps & WINAC_IN)) en = 0;
        if (en && (i == 4 || i == 5) && m->type != 2) en = 0;
        EnableWindow(hval[i], en);
        if (en) show_level(i);
        else SetWindowText(hval[i], "n/a");
    }
    EnableWindow(hlock, ok);
    EnableWindow(hmute, FALSE);
    SendMessage(hmute, BM_SETCHECK, 0, 0);
    if (ok && !api(m->core, WINF_AUD_MIX, m->id, MX_MUTE, 0, 0, 0)) {
        SendMessage(hmute, BM_SETCHECK, rq->retlen ? 1 : 0, 0);
        EnableWindow(hmute, TRUE);
    }
    SendMessage(hsrc, CB_RESETCONTENT, 0, 0);
    if (ok && !api(m->core, WINF_AUD_MIX, m->id, MX_SRC, 0, 0, 0)) m->insrc = (BYTE)rq->retlen;
    if (ok)
        for (s = 0; s < 5; s++)
            if (m->insrcs & (1 << s)) {
                SendMessage(hsrc, CB_ADDSTRING, 0, (LPARAM)(LPSTR)srcnames[s]);
                if (m->insrc & (1 << s)) sel = idx;
                idx++;
            }
    SendMessage(hsrc, CB_SETCURSEL, sel, 0);
    EnableWindow(hsrc, idx > 1);
    refreshing = 0;
    if (!ndev) status("No sound device.");
    else if (m && m->gone) status("The sound device was removed.");
    else if (present[0] && present[1]) status("WINAUDIO.SYS and WINUSB.SYS are loaded.");
    else status(present[0] ? "WINAUDIO.SYS is loaded." : "WINUSB.SYS is loaded.");
}

/* a key pressed in a level field: the arrows and Home and End change the level, Enter keeps
   the number that was typed. Returns 1 when the key was used. */
static int level_key(HWND hw, int vk)
{
    int i, pc;
    if (!hw) return 0;
    for (i = 0; i < NCTL; i++)
        if (hval[i] == hw) break;
    if (i == NCTL) return 0;
    pc = read_level(i);
    if (pc < 0) return 1;       /* an empty field: nothing to change */
    switch (vk) {
    case VK_UP: pc += 5; break;
    case VK_DOWN: pc -= 5; break;
    case VK_PRIOR: pc += 10; break;
    case VK_NEXT: pc -= 10; break;
    case VK_HOME: pc = 0; break;
    case VK_END: pc = 100; break;
    case VK_RETURN:
        set_level(i, pc);
        show_level(i);
        return 1;
    default:
        return 0;
    }
    if (pc < 0) pc = 0;
    if (pc > 100) pc = 100;
    set_level(i, pc);
    show_level(i);
    return 1;
}

BOOL CALLBACK __export MixDlgProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    int i;
    switch (msg) {
    case WM_INITDIALOG:
    {
        HFONT f = GetStockObject(ANSI_VAR_FONT);
        HWND c;
        hdev = GetDlgItem(hw, ID_DEV);
        hsrc = GetDlgItem(hw, ID_SRC);
        hlock = GetDlgItem(hw, ID_LOCK);
        hmute = GetDlgItem(hw, ID_MUTE);
        hstat = GetDlgItem(hw, ID_TXT);
        for (i = 0; i < NCTL; i++) hval[i] = GetDlgItem(hw, ID_VAL + i);
        for (c = GetWindow(hw, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
            SendMessage(c, WM_SETFONT, (WPARAM)f, 0);
        SendMessage(hlock, BM_SETCHECK, 1, 0);
        fill_devices();
        show_values();
        SetTimer(hw, 1, 2000, NULL);
        return TRUE;
    }
    case WM_TIMER:
    {
        /* follow USB audio devices being plugged in and removed */
        BYTE oc = ndev ? devs[cur].core : 0xFF, oi = ndev ? devs[cur].id : 0xFF;
        if (list_devs()) {
            cur = 0;
            for (i = 0; i < ndev; i++)
                if (devs[i].core == oc && devs[i].id == oi) cur = i;
            fill_devices();
            show_values();
        }
        return TRUE;
    }
    case WM_COMMAND:
    {
        int id = (int)LOWORD(wp), ev = (int)HIWORD(wp);
        if (id >= ID_VAL && id < ID_VAL + NCTL) {
            if (ev == EN_KILLFOCUS && !refreshing) {
                int k = id - ID_VAL, n = read_level(k);
                if (n >= 0) set_level(k, n);
                show_level(k);
            }
            return TRUE;
        }
        switch (id) {
        case ID_DEV:
            if (ev == CBN_SELCHANGE) {
                int s = (int)SendMessage(hdev, CB_GETCURSEL, 0, 0);
                if (s >= 0 && s < ndev) cur = s;
                show_values();
            }
            return TRUE;
        case ID_SRC:
            if (ev == CBN_SELCHANGE) set_source();
            return TRUE;
        case ID_MUTE:
            if (ev == BN_CLICKED && ndev > 0 && !devs[cur].gone) {
                int on = (int)SendMessage(hmute, BM_GETCHECK, 0, 0) == 1;
                api(devs[cur].core, WINF_AUD_MIX, devs[cur].id, MX_MUTE | 0x80000000UL,
                    on ? 1UL : 0UL, 0, 0);
                save(&devs[cur]);
                status(on ? "The master mute is on: nothing is heard." : "The master mute is off.");
            }
            return TRUE;
        case ID_TONE:
            if (ev == BN_CLICKED) {
                int st = tone(2);
                if (st == -1) status("Not enough DOS memory for the tone.");
                else if (st == -2) status("No sound device.");
                else if (st) status("The test tone could not be played.");
                else {
                    char t[96];
                    wsprintf(t, "Tone: %lu of %lu bytes, driver %lu.", tone_pos, tone_total, tone_rd);
                    status(t);
                }
            }
            return TRUE;
        case ID_REPORT:
            if (ev == BN_CLICKED) {
                dumph = OpenFile("C:\\WINMIX.TXT", &dumpof, OF_CREATE | OF_WRITE);
                if (dumph == HFILE_ERROR) status("Cannot write C:\\WINMIX.TXT.");
                else {
                    dump_all();
                    _lclose(dumph);
                    dumph = HFILE_ERROR;
                    status("The report was written to C:\\WINMIX.TXT.");
                }
            }
            return TRUE;
        case IDCANCEL:
            DestroyWindow(hw);
            return TRUE;
        }
        return FALSE;
    }
    case WM_CTLCOLOR:
        if (HIWORD(lp) == CTLCOLOR_STATIC || HIWORD(lp) == CTLCOLOR_BTN) {
            SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)hface;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hw);
        return TRUE;
    case WM_DESTROY:
        KillTimer(hw, 1);
        PostQuitMessage(0);
        return TRUE;
    }
    return FALSE;
}

int PASCAL WinMain(HINSTANCE hi, HINSTANCE hprev, LPSTR cmd, int show)
{
    MSG m;
    (void)hprev;
    hinst = hi;
    hface = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
    scan();
    if (cmd && *cmd) {
        int rc = command(cmd);
        if (rc >= 0) {
            if (rq) GlobalDosFree(reqsel);
            DeleteObject(hface);
            return rc;
        }
    }
    if (!present[0] && !present[1]) {
        MessageBox(NULL, "Neither WINAUDIO.SYS nor WINUSB.SYS is loaded.", "WINMIX", MB_OK | MB_ICONINFORMATION);
        if (rq) GlobalDosFree(reqsel);
        DeleteObject(hface);
        return 0;
    }
    hmain = CreateDialog(hi, MAKEINTRESOURCE(IDD_MIX), NULL, MixDlgProc);
    if (!hmain) {
        MessageBox(NULL, "WINMIX: the window could not be created.", "WINMIX", MB_OK | MB_ICONSTOP);
        if (rq) GlobalDosFree(reqsel);
        DeleteObject(hface);
        return 0;
    }
    ShowWindow(hmain, show);
    while (GetMessage(&m, NULL, 0, 0)) {
        if (m.message == WM_KEYDOWN && level_key(GetFocus(), (int)m.wParam)) continue;
        if (!IsDialogMessage(hmain, &m)) {
            TranslateMessage(&m);
            DispatchMessage(&m);
        }
    }
    if (rq) GlobalDosFree(reqsel);
    DeleteObject(hface);
    return m.wParam;
}
