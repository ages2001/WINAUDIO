/* afunc.c - names of HD Audio codecs and the functions behind a sound controller, for the
   Plug and Play enumerator of the Windows 95/98/ME VxD (one child devnode per function) */
#include "core.h"
#include "audio.h"
#include "dsp.h"

/* codec vendors: how the chip name is made from the device id
   NM_HEX   prefix + device id in hex without leading zeros (Realtek ALC887, ALC1220)
   NM_DEC4  prefix + four digits when the id has no letter (Analog Devices AD1988, Cirrus Logic
            CS4206), else NM_ID
   NM_ID    "HD Audio Codec" and the device id: the part number is not in the id
   NM_FIXED the prefix is the whole name (HDMI / DisplayPort codecs, virtual machines) */
#define NM_HEX      0
#define NM_DEC4     1
#define NM_ID       2
#define NM_FIXED    3
static const struct { u16 vendor; u8 mode; const char *maker, *prefix; } makers[] = {
    { 0x10EC, NM_HEX, "Realtek", "ALC" },
    { 0x14F1, NM_ID, "Conexant", "" },
    { 0x111D, NM_ID, "IDT", "" },
    { 0x8384, NM_ID, "SigmaTel", "" },
    { 0x11D4, NM_DEC4, "Analog Devices", "AD" },
    { 0x1106, NM_ID, "VIA", "" },
    { 0x1013, NM_DEC4, "Cirrus Logic", "CS" },
    { 0x434D, NM_ID, "C-Media", "" },
    { 0x1AEC, NM_ID, "Wolfson", "" },
    { 0x1102, NM_ID, "Creative", "" },
    { 0x17E8, NM_FIXED, "Chrontel", "HDMI Audio" },
    { 0x11C1, NM_FIXED, "LSI / Agere", "HD Audio Modem" },
    { 0x1057, NM_FIXED, "Motorola", "HD Audio Modem" },
    { 0x8086, NM_FIXED, "Intel", "HDMI / DisplayPort Audio" },
    { 0x10DE, NM_FIXED, "NVIDIA", "HDMI / DisplayPort Audio" },
    { 0x1002, NM_FIXED, "AMD", "HDMI / DisplayPort Audio" },
    { 0x1022, NM_FIXED, "AMD", "HD Audio Codec" },
    { 0x1D17, NM_FIXED, "Zhaoxin", "HDMI / DisplayPort Audio" },
    { 0x6549, NM_FIXED, "Teradici", "HD Audio Codec" },
    { 0x1AF4, NM_FIXED, "QEMU", "HD Audio Codec" },
    { 0x15AD, NM_FIXED, "VMware", "HD Audio Codec" },
    { 0x80EE, NM_FIXED, "VirtualBox", "HD Audio Codec" },
};

/* Realtek parts whose name is not their device id */
static const struct { u16 device; const char *name; } realtek[] = {
    { 0x0899, "ALC898" }, { 0x0900, "ALC1150" }, { 0x0862, "ALC861-VD" },
};

void codec_name(u32 vendev, char *out, u32 max)
{
    u16 v = (u16)(vendev >> 16), d = (u16)vendev;
    int i;
    for (i = 0; i < (int)ARRAY_SIZE(makers); i++) {
        if (makers[i].vendor != v) continue;
        if (v == 0x10EC) {
            int k;
            for (k = 0; k < (int)ARRAY_SIZE(realtek); k++)
                if (realtek[k].device == d) {
                    ksnprintf(out, max, "Realtek %s", realtek[k].name);
                    return;
                }
        }
        switch (makers[i].mode) {
        case NM_HEX: ksnprintf(out, max, "%s %s%X", makers[i].maker, makers[i].prefix, d); break;
        case NM_DEC4:
            if ((d & 0x000F) < 10 && (d & 0x00F0) < 0xA0 && (d & 0x0F00) < 0xA00 && d < 0xA000) {
                ksnprintf(out, max, "%s %s%04X", makers[i].maker, makers[i].prefix, d);
                break;
            }
            /* fall through */
        case NM_ID: ksnprintf(out, max, "%s HD Audio Codec %04X", makers[i].maker, d); break;
        default: ksnprintf(out, max, "%s %s", makers[i].maker, makers[i].prefix); break;
        }
        return;
    }
    ksnprintf(out, max, "HD Audio Codec %04X:%04X", v, d);
}

/* every function behind a controller; bdf 0xFFFFFFFF: the audio DSP of Bay Trail / Cherry
   Trail / Broadwell given by ACPI (no PCI function) */
int audio_functions(u32 bdf, struct afunc *out, int max)
{
    struct dsp *d;
    int n = hda_functions(bdf, out, max);
    if (n) return n;
    for (d = dsp_list; d; d = d->next) {
        if (!d->shim) continue;         /* cAVS / ACE DSPs are listed by hda_functions */
        if (bdf == 0xFFFFFFFFUL ? d->pci.bus || d->pci.dev || d->pci.fn || d->pci.cls : PCI_BDF(&d->pci) != bdf)
            continue;
        n += dsp_functions(d, out + n, max - n);
    }
    return n;
}
