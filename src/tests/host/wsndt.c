/* wsndt.c - test of WINSOUND.DRV through MMSYSTEM: device caps, recording, playback with
   looping and position, MIDI out. Writes C:\WSNDT.TXT and exits. */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

static FILE *lf;
static char FAR *data[4];
static WAVEHDR hdr[4];

static void logf_(const char *fmt, ...)
{
    va_list a;
    lf = fopen("C:\\WSNDT.TXT", "a");
    if (!lf) return;
    va_start(a, fmt);
    vfprintf(lf, fmt, a);
    va_end(a);
    fclose(lf);
}

static void wait_done(WAVEHDR *h, DWORD ms)
{
    DWORD t0 = GetTickCount();
    MSG m;
    int k = 0;
    while (!(h->dwFlags & WHDR_DONE) && GetTickCount() - t0 < ms) {
        if (PeekMessage(&m, NULL, 0, 0, PM_REMOVE)) DispatchMessage(&m);
        if (!(++k & 1023)) logf_("  wait %d t %lu flags %lx\n", k, GetTickCount() - t0, h->dwFlags);
    }
}

int PASCAL WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show)
{
    WAVEFORMAT fmt;
    HWAVEIN hin;
    HWAVEOUT hout;
    HMIDIOUT hm;
    WAVEOUTCAPS oc;
    WAVEINCAPS ic;
    MIDIOUTCAPS mc;
    MMTIME t;
    UINT r, i, n;
    DWORD tot = 0, t0;
    long sum;
    (void)hi; (void)hp; (void)show;
    lf = fopen("C:\\WSNDT.TXT", "w");
    if (lf) fclose(lf);
    for (i = 0; i < 4; i++) data[i] = (char FAR *)GlobalLock(GlobalAlloc(GMEM_FIXED | GMEM_ZEROINIT, 22050));
    if (cmd && cmd[0] && cmd[1] == 'D') {
        MSG mm;
        t0 = GetTickCount();
        while (GetTickCount() - t0 < 6000) if (PeekMessage(&mm, NULL, 0, 0, PM_REMOVE)) DispatchMessage(&mm);
        logf_("delayed\n");
    }
    if (cmd && *cmd == 'L') {
        /* device list over time: USB audio devices plugged in and removed meanwhile */
        int it;
        PCMWAVEFORMAT pf;
        pf.wf.wFormatTag = WAVE_FORMAT_PCM;
        pf.wf.nChannels = 1;
        pf.wf.nSamplesPerSec = 22050;
        pf.wf.nAvgBytesPerSec = 22050;
        pf.wf.nBlockAlign = 1;
        pf.wBitsPerSample = 8;
        for (it = 0; it < 30; it++) {
            MSG mm;
            n = waveOutGetNumDevs();
            logf_("t%d out %u:", it, n);
            for (i = 0; i < n; i++) {
                waveOutGetDevCaps(i, &oc, sizeof(oc));
                r = waveOutOpen(&hout, i, (LPWAVEFORMAT)&pf, 0, 0, 0);
                if (!r) waveOutClose(hout);
                logf_(" [%s open %u]", (LPSTR)oc.szPname, r);
            }
            logf_(" in %u midi %u\n", waveInGetNumDevs(), midiOutGetNumDevs());
            t0 = GetTickCount();
            while (GetTickCount() - t0 < 3000) if (PeekMessage(&mm, NULL, 0, 0, PM_REMOVE)) DispatchMessage(&mm);
        }
        logf_("DONE\n");
        return 0;
    }
    if (cmd && *cmd == 'S') {
        char rs[128];
        DWORD e;
        t0 = GetTickCount();
        r = sndPlaySound("C:\\BELLS.WAV", SND_SYNC);
        logf_("sndPlaySound %u in %lu ms\n", r, GetTickCount() - t0);
        t0 = GetTickCount();
        e = mciSendString("play C:\\BELLS.WAV wait", rs, sizeof(rs), 0);
        logf_("mci wave %lu in %lu ms\n", e, GetTickCount() - t0);
        e = mciSendString("open C:\\CANYON.MID type sequencer alias m", rs, sizeof(rs), 0);
        logf_("mci open midi %lu\n", e);
        t0 = GetTickCount();
        e = mciSendString("play m from 0 to 6000 wait", rs, sizeof(rs), 0);
        logf_("mci play midi %lu in %lu ms\n", e, GetTickCount() - t0);
        e = mciSendString("close m", rs, sizeof(rs), 0);
        logf_("mci close %lu\nDONE\n", e);
        return 0;
    }
    n = waveOutGetNumDevs();
    logf_("waveout devs %u\n", n);
    for (i = 0; i < n; i++) {
        waveOutGetDevCaps(i, &oc, sizeof(oc));
        logf_("  out %u: %s formats %lx\n", i, (LPSTR)oc.szPname, oc.dwFormats);
    }
    n = waveInGetNumDevs();
    logf_("wavein devs %u\n", n);
    for (i = 0; i < n; i++) {
        waveInGetDevCaps(i, &ic, sizeof(ic));
        logf_("  in %u: %s\n", i, (LPSTR)ic.szPname);
    }
    n = midiOutGetNumDevs();
    logf_("midiout devs %u\n", n);
    for (i = 0; i < n; i++) {
        midiOutGetDevCaps(i, &mc, sizeof(mc));
        logf_("  midi %u: %s tech %u\n", i, (LPSTR)mc.szPname, mc.wTechnology);
    }
    logf_("auxdevs %u\n", auxGetNumDevs());

    /* record 2 s, 22050 Hz 8-bit mono, 4 buffers of 0.5 s (two queued again) */
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 1;
    fmt.nSamplesPerSec = 22050;
    fmt.nAvgBytesPerSec = 22050;
    fmt.nBlockAlign = 1;
    {
        PCMWAVEFORMAT pf;
        pf.wf = fmt;
        pf.wBitsPerSample = 8;
        r = (cmd && *cmd == 'P') ? 99 : waveInOpen(&hin, 0, (LPWAVEFORMAT)&pf, 0, 0, 0);
        logf_("waveInOpen %u\n", r);
        if (!r) {
            for (i = 0; i < 4; i++) {
                memset(&hdr[i], 0, sizeof(hdr[i]));
                hdr[i].lpData = data[i];
                hdr[i].dwBufferLength = 11025;
                waveInPrepareHeader(hin, &hdr[i], sizeof(WAVEHDR));
                waveInAddBuffer(hin, &hdr[i], sizeof(WAVEHDR));
            }
            t0 = GetTickCount();
            waveInStart(hin);
            wait_done(&hdr[3], 5000);
            logf_("recorded in %lu ms\n", GetTickCount() - t0);
            t.wType = TIME_BYTES;
            waveInGetPosition(hin, &t, sizeof(t));
            logf_("in position %lu bytes\n", t.u.cb);
            waveInReset(hin);
            for (i = 0; i < 4; i++) {
                logf_("  buf %u: %lu bytes, flags %lx\n", i, hdr[i].dwBytesRecorded, hdr[i].dwFlags);
                tot += hdr[i].dwBytesRecorded;
                waveInUnprepareHeader(hin, &hdr[i], sizeof(WAVEHDR));
            }
            logf_("waveInClose %u\n", waveInClose(hin));
        }
        /* play a 1 kHz square wave, 0.25 s buffer looped 4 times + one 0.5 s buffer */
        r = waveOutOpen(&hout, 0, (LPWAVEFORMAT)&pf, 0, 0, 0);
        logf_("waveOutOpen %u\n", r);
        if (!r) {
            for (i = 0; i < 11025; i++) data[0][i] = (char)((i / 11) & 1 ? 0xC0 : 0x40);
            memset(&hdr[0], 0, sizeof(hdr[0]));
            hdr[0].lpData = data[0];
            hdr[0].dwBufferLength = 5512;
            if (!(cmd && cmd[0] && (cmd[1] == 'N' || cmd[1] == 'D'))) {
                hdr[0].dwFlags = WHDR_BEGINLOOP | WHDR_ENDLOOP;
                hdr[0].dwLoops = 4;
            }
            memset(&hdr[1], 0, sizeof(hdr[1]));
            hdr[1].lpData = data[0];
            hdr[1].dwBufferLength = 11025;
            r = waveOutPrepareHeader(hout, &hdr[0], sizeof(WAVEHDR));
            logf_("prep0 %u flags %lx\n", r, hdr[0].dwFlags);
            r = waveOutPrepareHeader(hout, &hdr[1], sizeof(WAVEHDR));
            logf_("prep1 %u\n", r);
            t0 = GetTickCount();
            r = waveOutWrite(hout, &hdr[0], sizeof(WAVEHDR));
            logf_("write0 %u t %lu\n", r, GetTickCount() - t0);
            r = waveOutWrite(hout, &hdr[1], sizeof(WAVEHDR));
            logf_("write1 %u t %lu\n", r, GetTickCount() - t0);
            wait_done(&hdr[0], 5000);
            logf_("loop done after %lu ms\n", GetTickCount() - t0);
            wait_done(&hdr[1], 5000);
            logf_("all done after %lu ms\n", GetTickCount() - t0);
            t.wType = TIME_SAMPLES;
            waveOutGetPosition(hout, &t, sizeof(t));
            logf_("out position %lu samples (expect %u)\n", t.u.sample, 4 * 5512 + 11025);
            waveOutUnprepareHeader(hout, &hdr[0], sizeof(WAVEHDR));
            waveOutUnprepareHeader(hout, &hdr[1], sizeof(WAVEHDR));
            logf_("waveOutClose %u\n", waveOutClose(hout));
        }
    }
    r = midiOutOpen(&hm, 0, 0, 0, 0);
    logf_("midiOutOpen %u\n", r);
    if (!r) {
        midiOutShortMsg(hm, 0x7F4590L);
        t0 = GetTickCount();
        while (GetTickCount() - t0 < 500) ;
        midiOutShortMsg(hm, 0x004580L);
        logf_("midiOutClose %u\n", midiOutClose(hm));
    }
    sum = 0;
    for (i = 0; i < 4; i++) {
        UINT k;
        for (k = 0; k < 100; k++) sum += (unsigned char)data[i][k];
    }
    logf_("recorded total %lu\nDONE\n", tot);
    return 0;
}
