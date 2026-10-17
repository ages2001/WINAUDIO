/* pnpaud.c - Windows 95/98/ME Plug and Play, the WINAUDIO part. An HD Audio controller is a bus
   (System devices, WINAUDIO.INF): every codec on its link and every device of its Intel audio
   DSP becomes a child devnode (Sound, video and game controllers, WINAUDM.INF), named after the
   chip. AC'97 controllers have no bus: their devnode is the sound device (WINAUDM.INF). The
   audio DSP of Bay Trail / Cherry Trail / Broadwell comes from ACPI (or is a PCI function) and
   is a bus of its I2S devices the same way. */
#include "pnpint.h"
#include "dsp.h"

#define ACPI_KEY    0xFFFFFFFFUL        /* audio_functions(): the ACPI audio DSP */
#define MAXFUNC     16

int pkg_enumerates(void)
{
    return 1;
}

static const char *const acpi_ids[] = { "ACPI\\80860F28", "ACPI\\808622A8", "ACPI\\INT3438" };

/* WINAUDIO\... (functions), ACPI\80860F28 ... (the ACPI audio DSP) */
int pkg_devnode_kind(const char *id)
{
    int i;
    if (!strncmp(id, "WINAUDIO\\", 9)) return PK_CHILD;
    for (i = 0; i < (int)ARRAY_SIZE(acpi_ids); i++)
        if (!strncmp(id, acpi_ids[i], strlen(acpi_ids[i]))) return PK_LEGACY;
    return 0;
}

static const struct { u16 vendor; const char *name; } vendors[] = {
    { 0x8086, "Intel" }, { 0x10DE, "NVIDIA" }, { 0x1002, "AMD" }, { 0x1022, "AMD" }, { 0x1106, "VIA" },
    { 0x1039, "SiS" }, { 0x10B9, "ULi" }, { 0x1D17, "Zhaoxin" }, { 0x1D94, "Hygon" }, { 0x17F3, "RDC" },
    { 0x15AD, "VMware" }, { 0x80EE, "VirtualBox" }, { 0x1102, "Creative" }, { 0x6549, "Teradici" },
    { 0x1274, "Ensoniq" }, { 0x1013, "Cirrus Logic" }, { 0x125D, "ESS" },
};

static const char *vendor_name(u32 vid)
{
    int i;
    for (i = 0; i < (int)ARRAY_SIZE(vendors); i++)
        if (vendors[i].vendor == vid) return vendors[i].name;
    return NULL;
}

/* Device Manager name of a controller: the bus (HD Audio, Intel audio DSP) or the AC'97
   sound device, with the maker of the chip set */
const char *pkg_desc(u32 bdf)
{
    static char buf[96];
    int b = (int)(bdf >> 8), d = (int)((bdf >> 3) & 0x1F), f = (int)(bdf & 7);
    u32 cls = pci_rd32(b, d, f, 8) >> 8, id = pci_rd32(b, d, f, 0);
    u16 vid = (u16)id, dev = (u16)(id >> 16);
    const char *v = vendor_name(vid);
    struct pcidev p;
    char vs[16];
    if (!v) {
        ksnprintf(vs, sizeof(vs), "PCI %04X", vid);
        v = vs;
    }
    memset(&p, 0, sizeof(p));
    p.vendor = vid;
    p.device = dev;
    if (sst_pci(&p)) {
        strlcpy(buf, "WINAUDIO Intel SST Audio DSP (I2S Audio Bus)", sizeof(buf));
        return buf;
    }
    if (dsp_platform(vid, dev)) {
        strlcpy(buf, "WINAUDIO Intel Smart Sound Technology Audio DSP (HD Audio Bus)", sizeof(buf));
        return buf;
    }
    if ((cls >> 8) == 0x0403) {
        ksnprintf(buf, sizeof(buf), "WINAUDIO %s High Definition Audio Bus Controller", v);
        return buf;
    }
    if ((cls >> 8) == 0x0401) {
        ksnprintf(buf, sizeof(buf), "WINAUDIO %s AC'97 Audio Controller", v);
        return buf;
    }
    return NULL;
}

/* compatible id of a function: its own INF entry and name */
static const char *compat_of(int kind)
{
    switch (kind) {
    case AF_HDA_AUDIO: return "WINAUDIO\\HDA_AUDIO";
    case AF_HDA_HDMI: return "WINAUDIO\\HDA_HDMI";
    case AF_HDA_MODEM: return "WINAUDIO\\HDA_MODEM";
    case AF_DSP_DMIC: return "WINAUDIO\\DSP_DMIC";
    case AF_DSP_I2S: return "WINAUDIO\\DSP_I2S";
    case AF_DSP_SDW: return "WINAUDIO\\DSP_SDW";
    case AF_DSP_HDA: return "WINAUDIO\\DSP_HDA";
    default: return "WINAUDIO\\HDA_OTHER";
    }
}

static void desc_of(const struct afunc *f, char *out, u32 max)
{
    const char *what = "";
    switch (f->kind) {
    case AF_HDA_AUDIO: what = " High Definition Audio"; break;
    case AF_HDA_MODEM: what = " (modem function)"; break;
    case AF_HDA_OTHER: what = " (audio function)"; break;
    default: break;
    }
    ksnprintf(out, max, "%s%s%s", f->name, what,
              f->hasdev ? "" : f->kind == AF_HDA_HDMI ? " (no WINAUDIO sound device: HDMI)" : " (no WINAUDIO sound device)");
}

static int is_hda(int kind)
{
    return kind == AF_HDA_AUDIO || kind == AF_HDA_HDMI || kind == AF_HDA_MODEM || kind == AF_HDA_OTHER;
}

/* WINAUDIO\HDAUDIO&FUNC_01&VEN_10EC&DEV_0887&SUBSYS_10438445&REV_1003\B00D1BF0C0 (codec at
   address 0 of 00:1B.0); WINAUDIO\DSP_SDW&VEN_8086&DEV_51C8\B00D1FF3D1 (DSP device 1) */
static void create_child(struct pnode *parent, u32 key, const struct afunc *f)
{
    char id[96], inst[24], hw[200], desc[96];
    struct pnode *p;
    u32 bdf = key == ACPI_KEY ? 0 : key;
    int fg = f->kind == AF_HDA_MODEM ? 2 : 1;
    if (key == ACPI_KEY) ksnprintf(inst, sizeof(inst), "ACPI%c%X", is_hda(f->kind) ? 'C' : 'D', f->index);
    else
        ksnprintf(inst, sizeof(inst), "B%02lXD%02lXF%lX%c%X", bdf >> 8, (bdf >> 3) & 0x1F, bdf & 7,
                  is_hda(f->kind) ? 'C' : 'D', f->index);
    if (is_hda(f->kind)) {
        u16 v = (u16)(f->vendev >> 16), d = (u16)f->vendev;
        ksnprintf(id, sizeof(id), "WINAUDIO\\HDAUDIO&FUNC_%02X&VEN_%04X&DEV_%04X\\%s", fg, v, d, inst);
        ksnprintf(hw, sizeof(hw),
                  "WINAUDIO\\HDAUDIO&FUNC_%02X&VEN_%04X&DEV_%04X&SUBSYS_%08lX&REV_%04lX,"
                  "WINAUDIO\\HDAUDIO&FUNC_%02X&VEN_%04X&DEV_%04X&SUBSYS_%08lX,"
                  "WINAUDIO\\HDAUDIO&FUNC_%02X&VEN_%04X&DEV_%04X",
                  fg, v, d, f->subsys, f->rev & 0xFFFF, fg, v, d, f->subsys, fg, v, d);
    } else {
        const char *c = compat_of(f->kind) + 9;
        ksnprintf(id, sizeof(id), "WINAUDIO\\%s&VEN_%04X&DEV_%04X\\%s", c, (u16)(f->vendev >> 16), (u16)f->vendev, inst);
        ksnprintf(hw, sizeof(hw), "WINAUDIO\\%s&VEN_%04X&DEV_%04X", c, (u16)(f->vendev >> 16), (u16)f->vendev);
    }
    desc_of(f, desc, sizeof(desc));
    p = pnp_child(parent, id, hw, compat_of(f->kind), desc, PK_CHILD,
                  (key == ACPI_KEY ? 0xFFFF00UL : bdf << 8) | (is_hda(f->kind) ? 0 : 0x80) | f->index);
    if (!p) return;
    if (p->state != (u8)(f->hasdev + 1)) {
        /* installed (the INF wrote its generic name), or the sound device came or went */
        pnp_set_desc(p->dn, desc);
        p->state = (u8)(f->hasdev + 1);
    }
}

static void sync_bus(struct pnode *parent, u32 key)
{
    struct afunc *f;
    int i, n;
    f = vmm_heap_alloc(MAXFUNC * sizeof(*f));
    if (!f) {
        for (i = 0; i < pnp_nnodes; i++)
            if (pnp_nodes[i].parent == parent->dn) pnp_nodes[i].seen = 1;
        return;
    }
    vmm_begin_crit();
    n = audio_functions(key, f, MAXFUNC);
    vmm_end_crit();
    for (i = 0; i < n; i++) create_child(parent, key, &f[i]);
    vmm_heap_free(f);
}

void pkg_sync(void)
{
    int i;
    for (i = 0; i < pnp_nnodes; i++) {
        struct pnode *p = &pnp_nodes[i];
        if (!p->dn) continue;
        if (p->kind == PK_PCI && p->started == 2) sync_bus(p, p->bdf);
        else if (p->kind == PK_LEGACY && p->started) sync_bus(p, ACPI_KEY);
    }
}

/* the functions and the ACPI audio DSP have nothing to start: the controller does the work */
void pkg_child_config(struct pnode *p, u32 func)
{
    if (func == CONFIG_START) {
        p->started = 1;
        /* the INF wrote its generic name over the chip name at install */
        if (p->kind == PK_CHILD) p->state = 0;
    } else if (func == CONFIG_STOP || func == CONFIG_REMOVE)
        p->started = 0;
}
