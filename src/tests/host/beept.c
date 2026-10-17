/* beept.c - test of the SOUND driver (WINBEEP.DRV): MessageBeep and the voice API.
   Writes C:\BEEPT.TXT. Works in every mode of Windows 3.0, real mode included. */
#include <windows.h>
#include <stdio.h>

static void logf_(const char *fmt, int a, int b)
{
    FILE *f = fopen("C:\\BEEPT.TXT", "a");
    if (!f) return;
    fprintf(f, fmt, a, b);
    fclose(f);
}

static void pause_ms(DWORD ms)
{
    DWORD t0 = GetTickCount();
    MSG m;
    while (GetTickCount() - t0 < ms)
        if (PeekMessage(&m, NULL, 0, 0, PM_REMOVE)) DispatchMessage(&m);
}

int PASCAL WinMain(HANDLE hi, HANDLE hp, LPSTR cmd, int show)
{
    int r, i;
    DWORD t0;
    static const int scale[8] = { 37, 39, 41, 42, 44, 46, 48, 49 };   /* C major from middle C */
    FILE *f = fopen("C:\\BEEPT.TXT", "w");
    (void)hi; (void)hp; (void)cmd; (void)show;
    if (f) fclose(f);
    logf_("mode flags %x\n", (int)GetWinFlags(), 0);
    pause_ms(2000);
    MessageBeep(0);
    logf_("MessageBeep(0)\n", 0, 0);
    pause_ms(1000);
    MessageBeep(MB_ICONASTERISK);
    logf_("MessageBeep(asterisk)\n", 0, 0);
    pause_ms(1000);
    r = OpenSound();
    logf_("OpenSound %d\n", r, 0);
    if (r > 0) {
        logf_("SetVoiceQueueSize %d\n", SetVoiceQueueSize(1, 192), 0);
        logf_("SetVoiceAccent %d\n", SetVoiceAccent(1, 240, 200, S_NORMAL, 0), 0);
        for (i = 0; i < 8; i++) {
            r = SetVoiceNote(1, scale[i], 4, 0);
            if (r) logf_("SetVoiceNote %d -> %d\n", i, r);
        }
        SetVoiceNote(1, 0, 4, 0);
        SetVoiceSound(1, 440UL << 16, 9);     /* 0.5 s of 440 Hz */
        logf_("CountVoiceNotes %d\n", CountVoiceNotes(1), 0);
        t0 = GetTickCount();
        logf_("StartSound %d\n", StartSound(), 0);
        logf_("WaitSoundState %d", WaitSoundState(S_QUEUEEMPTY), 0);
        logf_(" after %d ms\n", (int)(GetTickCount() - t0), 0);
        logf_("CountVoiceNotes %d\n", CountVoiceNotes(1), 0);
        CloseSound();
    }
    pause_ms(500);
    logf_("DONE\n", 0, 0);
    return 0;
}
