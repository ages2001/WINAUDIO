# WINAUDIO 1.1 — HD Audio / AC'97 / Intel audio DSP sound driver for DOS, Windows 3.x and Windows 95 / 98 / ME

The complete manual. **README.md** has the short overview and the installation.

WINAUDIO is a driver written from scratch that brings built-in PC sound hardware to MS-DOS
5 / 6, Windows 3.0 (Multimedia Extensions) / Windows 3.1 / WfW 3.11 and Windows 95 / 98 / ME:

* **Intel High Definition Audio:** every controller of PCI class 04/03, whatever its
  programming interface byte (04/03/00, 04/03/80 …; Intel ICH6 → PCH → Skylake … Arrow /
  Lunar Lake, Intel controllers in DSP mode; AMD / ATI, nVidia, VIA, SiS, ULi, Zhaoxin,
  Hygon …) with a **generic codec parser**: the codec's widget graph is walked, every
  line-out / speaker / headphone output is enabled, the speakers are muted when headphones
  are plugged in, EAPD (external amplifier) is turned on. **Realtek** codecs (ALC2xx,
  ALC6xx, ALC8xx, ALC1150 / 1220 …) get the start-up settings of the Linux driver (§6).
* **Intel audio DSP (SST / Smart Sound Technology, §6a):** on Intel controllers in DSP mode
  from Skylake to Nova Lake (PCI 04/01, 04/03/80, 04/80) and on the SST DSP of Bay Trail /
  Cherry Trail / Broadwell the DSP firmware (Sound Open Firmware; Intel AVS on Skylake /
  Kaby Lake) is loaded and these paths are driven: the **digital microphone array** (DMIC),
  **I2S codecs and amplifiers** (Realtek, Maxim, Dialog, Everest, Nuvoton, Cirrus, TI, ADI,
  Conexant — 32 chips), **SoundWire** codecs and amplifiers (Realtek RT7xx / RT13xx, Cirrus
  CS42L43 / CS35L56 …). HD Audio codecs (Realtek, Conexant …) run directly on the HD Audio
  link by default; the DSP is used only for devices not on that link (e.g. a notebook's
  internal digital microphones).
* **AC'97:** Intel ICH / ICH0 … ICH7 / 440MX / ESB / ESB2, nVidia nForce … MCP51, AMD 768 /
  8111, SiS 7012, VIA 82C686 / VT8233 / 8235 / 8237 and other ICH-compatible controllers;
  all AC'97 codecs.
* **Software mixer:** 16 streams (programs) at a time + the MIDI synthesizer.
* **Any PCM format:** 8 / 16 / 24 / 32 bit, 32-bit float, 1–8 channels, 2–192 kHz (polyphase
  sinc sample rate converter, no drift).
* **Recording:** microphone / line in / CD / aux and (when the codec has it) stereo mix,
  recording level.
* **Built-in General MIDI synthesizer:** 32 voices, two-operator FM, 128 instruments + drum
  kit (channel 10), pitch bend, modulation (vibrato), sustain pedal, RPN bend range, GM / GS /
  XG reset.
* **Windows driver WINSOUND.DRV:** wave output / input, MIDI output, aux volume; Windows 3.1
  and Windows 3.0 + MME (MMSYSTEM driver interface), Standard and 386 Enhanced Mode, Windows
  95 / 98 / ME with a mixer driver (Volume Control, the volume icon of the taskbar). The
  volumes are kept across restarts.
* **Sound Blaster 16 emulation** for DOS programs and games (§3b): DSP 4.05 with 8-bit and
  16-bit DMA sound, the CT1745 mixer, an **OPL3 FM synthesizer** (AdLib / Sound Blaster FM
  music) and an **MPU-401** (UART) on the General MIDI synthesizer, the interrupt on IRQ 5 or
  7 and DMA 1 / 5: in every MS-DOS box of Windows 3.x (386 Enhanced Mode) and Windows 95 /
  98 / ME, and in plain DOS for real-mode and protected-mode games (DOS/4GW, DOS/32A,
  PMODE/W, CauseWay, DJGPP) with **nothing else to load**: WINAUDIO.SYS brings its own V86
  monitor, VCPI server and DPMI host (QEMM or JEMM + QPIEMU work too).
* **WINMIX.EXE** (mixer) and **AUDTEST.EXE** (DOS test / diagnosis tool).

USB Audio 1.0 / 2.0 devices and USB MIDI work through the same WINSOUND.DRV with the USB
stack of **WINUSB** (both packages can be used together).

WINAUDIO is one of the four WINDRV packages, each in its own repository and usable alone or
together with the others:

| Package | What it drives |
|---------|----------------|
| **WINDISK**  | IDE, SATA AHCI and NVMe disks, IDE/AHCI CD/DVD drives, SD card readers and eMMC |
| **WINUSB**   | USB host controllers and devices, PS/2 mouse and keyboard, touchpads, USB4 / Thunderbolt |
| **WINAUDIO** | HD Audio, AC'97 and the Intel audio DSP (SST / SOF), General MIDI synthesizer, Sound Blaster 16 emulation for DOS programs |
| **WINUDF**   | the UDF file system (DVD, Blu-ray, packet-written CDs, UDF disks), read and write |

---

## Supported systems

| System | WINAUDIO |
|--------|------|
| MS-DOS 5.0 / 6.x, 7.x, 8.0, FreeDOS | sound (`AUDTEST`), Sound Blaster 16 emulation for real-mode and protected-mode (DPMI) games, nothing else to load (§3b) |
| Windows 1.0x, 2.x | — |
| Windows 3.0 Real Mode | beeps and `PLAY` / `SOUND` music (`WINBEEP.DRV`) |
| Windows 3.0 Standard / 386 Enhanced Mode | everything (with the Multimedia Extensions); Sound Blaster emulation in MS-DOS boxes (Enhanced Mode) |
| Windows 3.1, WfW 3.11 | everything; Sound Blaster emulation in MS-DOS boxes (Enhanced Mode) |
| Windows 95 / 98 / ME | Sound Blaster emulation in MS-DOS boxes; everything, as Plug and Play devices: `WINAUDIO.VXD` (HD Audio bus + codec devices, Intel DSP, AC'97) (§3a) |

## Repository layout

```
bin/DOS     DOS: the driver, its 32-bit core, tools, installer and uninstaller (also all
            Windows 1.0x / 2.x need)
bin/WIN3X   Windows 3.0, 3.1 and Windows for Workgroups 3.11
bin/WIN9X   Windows 95, 98 and ME (Plug and Play drivers and their INF files: the folder
            to give to "Have Disk" / "Update Driver")
src         the source code (see "Building")
```

Files used by several systems are in each of their folders. The installer is run from
`bin\DOS`; it takes the files of the other systems from the folders next to it, so the
`bin` tree has to stay together (copy all of it to a floppy or a directory).

| Folder | Files |
|--------|-------|
| `bin/DOS` | `AUDTEST.EXE`, `INSTAUD.EXE`, `MANUAL.TXT`, `README.TXT`, `UNINAUD.EXE`, `WINACORE.BIN`, `WINAUDIO.SYS` |
| `bin/WIN3X` | `WIN30FIX.COM`, `WINAUDIO.386`, `WINAUDIO.TXT`, `WINBEEP.DRV`, `WINGRP.EXE`, `WINMIX.EXE`, `WINSOUND.DRV` |
| `bin/WIN9X` | `AUDTEST.EXE`, `WIN4G.VXD`, `WINAUDIO.INF`, `WINAUDIO.TXT`, `WINAUDIO.VXD`, `WINAUDM.INF`, `WINDRVFW\`, `WINGRP.EXE`, `WINMIX.EXE`, `WINSOUND.DRV` |

## 1. Requirements

* Pentium or later processor with a time stamp counter (also SoCs that have one but do not
  report it through CPUID, such as the Vortex86 DX / SX / EX), MS-DOS 5.0 / 6.x / 7.x / 8.0
  or FreeDOS, **HIMEM.SYS** (or another XMS driver: JEMMEX, HIMEMX; with or without
  EMM386 / JEMM386), PCI bus. The DOS driver can be loaded high (`DEVICEHIGH`) or with a
  device loader such as DEVLOAD.
* Windows 3.1 / WfW 3.11, Windows 3.0 + Multimedia Extensions (MMSYSTEM.DLL), or Windows 95 /
  98 / ME
* An HD Audio or AC'97 controller on PCI, or an Intel audio DSP (§6a)

## 2. Files

| File | Folder | Purpose |
|------|--------|---------|
| `WINAUDIO.SYS` | DOS | DOS character device driver `WINAUD$` (CONFIG.SYS) |
| `WINACORE.BIN` | DOS | 32-bit driver core (DOS / Standard Mode), in the directory of WINAUDIO.SYS |
| `AUDTEST.EXE`  | DOS, WIN9X | device list, test tone, WAV play / record, MIDI test, mixer, log |
| `INSTAUD.EXE`  | DOS | installer |
| `UNINAUD.EXE`  | DOS | uninstaller |
| `WINDRVFW\`    | WIN9X | firmware of the Intel audio DSPs and SoundWire codecs (`FIRMWARE.TXT`, §6a); the installer copies it next to WINAUDIO.SYS too |
| `README.TXT`   | DOS | the short README (code page 437) |
| `MANUAL.TXT`   | DOS | this manual (code page 437) |
| `WINAUDIO.386` | WIN3X | 386 Enhanced Mode VxD, in the directory of WINAUDIO.SYS (loaded automatically) |
| `WINSOUND.DRV` | WIN3X, WIN9X | MMSYSTEM driver (wave, midi, aux, mixer) — `WINDOWS\SYSTEM` |
| `WINMIX.EXE`   | WIN3X, WIN9X | volume levels and recording source |
| `WINBEEP.DRV`  | WIN3X | Windows 3.0 SOUND driver: beeps and the `PLAY` / `SOUND` API in Real Mode too |
| `WIN30FIX.COM` | WIN3X | XMS fix for Windows 3.0 Standard Mode with more than 16 MB of memory |
| `WINGRP.EXE`   | WIN3X, WIN9X | creates the Program Manager / Start menu group at the next start of Windows |
| `WINAUDIO.TXT` | WIN3X, WIN9X | this manual for Notepad ("Read Me" item of the group) |
| `WINAUDIO.VXD` | WIN9X | Windows 95/98/ME Plug and Play VxD (dynamic; HD Audio bus driver and enumerator of the codecs) |
| `WINAUDIO.INF` | WIN9X | HD Audio controllers (bus), Intel audio DSPs (System devices) |
| `WINAUDM.INF`  | WIN9X | HD Audio codec functions, DSP endpoints and AC'97 (Sound, video and game controllers) |
| `WIN4G.VXD`    | WIN9X | moves PCI resources above 4 GB below 4 GB at boot (see "PCI resources above 4 GB") |

## 3. Installation

Run the installer from DOS (not from inside Windows 3.x), from `bin\DOS` of this package.
The default target is the Windows directory (found from WIN.COM on the PATH, or given with
`/W:`; `C:\WINDOWS` otherwise):

```
INSTAUD                      installs into the Windows directory
INSTAUD D:\DRV\WIN /W:D:\WIN  another directory and Windows directory
INSTAUD /NOWIN               the DOS driver only (WINSOUND.DRV is not set up)
INSTAUD /O /WIN              with driver options (everything after /O goes to WINAUDIO.SYS)
INSTAUD /DOS                 Windows 95/98 installed: adds WINAUDIO.SYS to CONFIG.SYS too
INSTAUD /O /SB:A220,I7,D1,H5 the emulated Sound Blaster on IRQ 7 (and SET BLASTER to match)
```

INSTAUD finds every Windows 3.1 / WfW 3.11, Windows 3.0 with the Multimedia Extensions and
Windows 95 / 98 / ME installation of the machine (`/W:dir` selects one). It

* copies the files (the DSP firmware into the `WINDRVFW` subdirectory of the target) and adds
  `DEVICE=C:\WINDOWS\WINAUDIO.SYS` to CONFIG.SYS after HIMEM.SYS / EMM386 (in every block of a
  menu CONFIG.SYS that loads HIMEM),
* copies `WINSOUND.DRV` to `WINDOWS\SYSTEM` and sets `wave=winsound.drv`,
  `midi=winsound.drv`, `aux=winsound.drv` in the `[drivers]` section of SYSTEM.INI. A driver
  already in one of these keys (e.g. `sndblst.drv`) moves to the first free `wave1=`,
  `midi1=` … key and keeps working; the WIN devices become the default (first) ones. A
  description goes into `[drivers.desc]` (Control Panel → Drivers).
* points the **"General MIDI"** setup of the MIDI Mapper in `WINDOWS\SYSTEM\MIDIMAP.CFG` at
  "WIN GM Synth" for all 16 channels and makes it current, so Media Player and every program
  that plays MIDI (through the MIDI Mapper) uses the synthesizer.
* on Windows 3.0 installs `WINBEEP.DRV` (`[boot] sound.drv=winbeep.drv`, the old one is
  kept): Windows 3.0's MMSYSTEM.DLL cannot load in Real Mode; WINBEEP plays the beeps and the
  `PLAY` / `SOUND` API (SetVoiceNote, StartSound …) on the synthesizer. In Standard /
  Enhanced Mode system sounds go to MMSYSTEM (`sndPlaySound`), as MMSOUND.DRV does it.
* puts `SET BLASTER=A220 I5 D1 H5 P330 T6` at the top of AUTOEXEC.BAT (the settings of the
  Sound Blaster emulation for DOS programs, also inherited by the MS-DOS boxes of Windows) when
  no BLASTER line is there; `/O /NOSB` leaves it out, `/O /SB:...` changes the values;
* adds the **WINAUDIO** group (Volume Control, Sound Test, Read Me) to Program Manager on
  Windows 3.x and to the Start menu on Windows 95/98/ME; installing again does not duplicate
  lines or group items.

The original of every changed file is saved once (`CONFIG.WNA`, `SYSTEM.WNA`, `MIDIMAP.WNS`).
Running it again is safe; the old options are kept unless `/O` is given.

Uninstall: `C:\WINDOWS\UNINAUD` removes the CONFIG.SYS line, puts the `[drivers]` keys and the
moved drivers back, restores MIDIMAP.CFG, removes the group, WINSOUND.DRV and the package
files (`/K` keeps the files, `/W:dir` names the Windows directory). With WINUSB installed too,
WINSOUND.DRV stays for the USB sound devices and the MIDI Mapper is pointed at "WIN USB GM
Synth".

Manual installation:

1. CONFIG.SYS: `DEVICE=C:\WINDOWS\WINAUDIO.SYS` (after HIMEM.SYS)
2. `WINSOUND.DRV` → `C:\WINDOWS\SYSTEM`; SYSTEM.INI:
   ```
   [drivers]
   wave=winsound.drv
   midi=winsound.drv
   aux=winsound.drv
   ```
   (or Control Panel → Drivers → Add → *Unlisted or Updated Driver*)
3. Control Panel → MIDI Mapper → Edit: route the channels to the "WIN GM Synth" port.
4. For DOS programs that use a Sound Blaster: `SET BLASTER=A220 I5 D1 H5 P330 T6` in
   AUTOEXEC.BAT (§3b).

Without WINAUDIO.SYS (no CONFIG.SYS line, or `/NOVXD`) Windows 3.x 386 Enhanced Mode can
load the VxD by itself: copy `WINAUDIO.386` to `WINDOWS\SYSTEM` and add
`device=winaudio.386` to the `[386Enh]` section of SYSTEM.INI; options then go to
`Options=` in a `[WINAUDIO]` section of SYSTEM.INI (e.g. `Options=/SB:A220,I7`). The sound
then works in Windows only (no DOS sound, no AUDTEST in DOS).

### 3a. Windows 95 / 98 / ME

On Windows 9x WINAUDIO is a Plug and Play driver (no CONFIG.SYS line). Run `INSTAUD` from
`bin\DOS` in a DOS box: `WINAUDIO.VXD`, `WIN4G.VXD`, `WINSOUND.DRV`, `WINMIX.EXE` and
`AUDTEST.EXE` go to `WINDOWS\SYSTEM`, the DSP firmware to `WINDOWS\SYSTEM\WINDRVFW`,
`WINAUDIO.INF` and `WINAUDM.INF` to `WINDOWS\INF` (without their `CopyFiles` lines: the files
are in place already), and `wave=winsound.drv`, `midi=winsound.drv`, `mixer=winsound.drv`
are written to the `[drivers]` section of SYSTEM.INI. Then select the sound controller in Device Manager: on
Windows 95 Remove the "PCI Multimedia Audio Device" (Other devices) and Refresh ("New Hardware
Found", "Windows default driver"); on Windows 98 / ME, for the sound device with Microsoft's
driver: Driver, Update Driver, Have Disk, `C:\WINDOWS\INF` (or `bin\WIN9X` of this package).
For a controller Windows has not seen before WINAUDIO is offered at boot by itself.

**HD Audio bus.** The HD Audio controller and the Intel audio DSP are a **bus controller** in
the "System devices" class (`WINAUDIO.INF`, `DevLoader=WINAUDIO.VXD`): "WINAUDIO Intel High
Definition Audio Bus Controller" (AMD, NVIDIA, VIA, Zhaoxin … by vendor), "WINAUDIO Intel
Smart Sound Technology Audio DSP (HD Audio Bus)", "WINAUDIO Intel SST Audio DSP (I2S Audio
Bus)" and so on. The VxD starts the controller, scans the codecs on the link and creates a
**child device** under the controller for every codec function:

| Child device id (compatible id) | Name in Device Manager (Sound, video and game controllers) |
|---------------------------------|------------------------------------------------------------|
| `WINAUDIO\HDAUDIO&FUNC_01&VEN_10EC&DEV_0892&SUBSYS_…&REV_…` (`WINAUDIO\HDA_AUDIO`) | "Realtek ALC892 High Definition Audio" (the codec's own name) |
| … (`WINAUDIO\HDA_HDMI`) | "Intel HDMI / DisplayPort Audio (no WINAUDIO sound device: HDMI)" — HDMI / DP output is not supported, so it offers no sound device, and its name says so |
| … (`WINAUDIO\HDA_MODEM`), (`WINAUDIO\HDA_OTHER`) | HD Audio modem and other functions |
| `WINAUDIO\DSP_DMIC`, `DSP_I2S`, `DSP_SDW`, `DSP_HDA` | the Intel DSP's digital microphones, I2S codecs / amplifiers, SoundWire codecs, HD Audio codecs behind the DSP |

The children are installed by `WINAUDM.INF` (`DevLoader=WINAUDIO.VXD`; they have no resources,
the sound comes out of the controller's driver). AC'97 controllers are in that class
directly ("WINAUDIO AC'97 Audio Controller"). A machine with several HD Audio controllers
(the mainboard's sound and the graphics card's HDMI sound) shows each with its own bus and
codecs; two items "Intel HDA" and "HDA" of which one gives Code 2, or a working item that
offers no sound device, cannot happen with this structure. `WINSOUND.DRV` finds the VxD
itself (INT 2Fh 1684h) and offers all sound devices.

Matching ids: `PCI\CC_0403` (HD Audio), the device ids of the Intel DSPs and
`PCI\VEN_8086&CC_0480` (Skylake … Kaby Lake DSP mode), `ACPI\80860F28`, `ACPI\808622A8`,
`ACPI\INT3438` (Bay Trail, Cherry Trail, Broadwell SST), `PCI\CC_0401` (AC'97). The INF
copies the firmware files from the package's `WINDRVFW` subdirectory to
`WINDOWS\SYSTEM\WINDRVFW`. Options are the `Options` value of the controller devnode (when
empty, `Options=` of `[WINAUDIO]` in SYSTEM.INI, read at boot only). When Windows ME offers
its own Intel AC'97 driver, choose "Specify the location" → "Display a list", **Have Disk**,
and WINAUDIO.

Verified (QEMU): Windows 95 RTM with two HD Audio controllers (ICH6 + ICH9, two codecs) on a
clean installation, while Windows runs and after a restart: two bus controllers under
"System devices", two codecs under "Sound, video and game controllers", no error marks, wave
playback; 20 seconds of playback from a 32-bit program's buffers with another 32-bit process
running in between (context switches) without glitches. AC'97: wave playback (startup sound
included) on Windows 95 RTM, Windows 98 FE and Windows ME.

### WINAUDIO.SYS options

| Option | Meaning |
|--------|---------|
| `/WIN`     | run the core only in Windows 386 Enhanced Mode (no sound in DOS and Standard Mode) |
| `/NOHDA`   | do not use HD Audio controllers |
| `/NOAC97`  | do not use AC'97 controllers |
| `/NOGEN`   | do not try unknown 04/01 devices as ICH-compatible AC'97 |
| `/EAPD`    | AC'97: turn the external amplifier on with EAPD = 1 (wired the other way on some notebooks) |
| `/IMM`     | HD Audio: the immediate command interface instead of CORB / RIRB |
| `/POSBUF`  | HD Audio: the DMA position buffer instead of LPIB |
| `/GPIO`    | HD Audio: codec GPIO 0–3 as outputs, high (notebooks whose external speaker amplifier is switched by GPIO) |
| `/NOSYNTH` | no MIDI synthesizer |
| `/NOVXD`   | do not load the VxD in Enhanced Mode |
| `/S`       | the driver log goes to the screen at boot too |
| `/DSP`     | on a machine with an Intel DSP drive the HD Audio codec through the DSP too (default: directly) |
| `/NODSP`   | never start the Intel audio DSP (HD Audio codecs only) |
| `/DMIC`    | try the digital microphones with the IPC3 firmware on a machine without an NHLT table (Chromebooks) |
| `/NORELOC` | do not move PCI resources from above 4 GB at boot |
| `/ALL4G`   | move every device below 4 GB at boot, not only the WINDRV devices |
| `/NOTPVOL` | ThinkPad (IBM / Lenovo): leave the volume / mute buttons' embedded controller setting alone (by default it is unmuted and turned up at start) |
| `/USBSB`   | the Sound Blaster emulation and the synthesizer play on the USB Audio device of WINUSB even when WINAUDIO has a sound device of its own (§3b) |
| `/NOSB`    | no Sound Blaster emulation (§3b) |
| `/SB:A220,I5,D1,H5,P330` | Sound Blaster settings: port 220h–280h, IRQ 2 / 5 / 7 / 10, 8-bit DMA 0 / 1 / 3, 16-bit DMA 5 / 6 / 7 (0: the 8-bit one), MPU-401 300h–330h (P0: none); missing letters keep their default |
| `/NOOPL`   | no FM synthesizer ports (388h–38Bh, base+0..3, base+8/9) |
| `/NOMPU`   | no MPU-401 |
| `/NOV86`   | plain DOS: do not start the built-in V86 monitor (no emulation in DOS then, §3b) |
| `/NODPMI`  | the V86 monitor without its DPMI host (VCPI for every program then) |
| `/NOVCPI`  | the V86 monitor without its VCPI server |
| `/VCPI`    | VCPI for every program, not only for WINDRV drivers and Windows Standard Mode |
| `/VME`     | use the virtual-8086 mode extensions of the processor (VME): software interrupts of real-mode programs go straight to their handlers, a little faster; off by default (some early Ryzen processors without a microcode update have a VME fault) |
| `/V`, `/D`, `/COM1` | log options (`/D`: debug log to port E9h / COM1) |

## 3b. Sound Blaster 16 emulation

DOS games and programs that know only a Sound Blaster get one: the emulated card answers on
its ports like a Creative Sound Blaster 16, and everything it plays goes through the sound
engine to the HD Audio / AC'97 device (or a USB Audio device of WINUSB, see below), mixed
with Windows' own sound.

| Part | Ports | Emulated |
|------|-------|----------|
| DSP 4.05 | 220h–22Fh (`A`) | reset, version, identification, copyright, test register, speaker, direct DAC (10h), 8-bit DMA (14h, 1Ch, 90h, 91h, Cxh), 16-bit DMA (Bxh), high speed, auto-init, pause / continue / exit auto-init, time constant (40h) and sample rate (41h / 42h), silence (80h), forced interrupts (F2h / F3h), recording (returns silence), ADPCM transfers (timed, played as silence), MIDI out (38h, UART modes) |
| Mixer CT1745 | 224h / 225h | master, voice, FM, CD, line, mic levels (SB Pro registers too), IRQ / DMA setting registers 80h / 81h, interrupt status 82h, SB Pro stereo bit |
| OPL3 FM | 388h–38Bh, 220h–223h, 228h / 229h | YMF262 synthesizer of our own (2 and 4 operators, 8 waveforms, rhythm mode, tremolo, vibrato, key scale, feedback, stereo), OPL2 compatible; the timers, so the AdLib detection of games works |
| MPU-401 UART | 330h / 331h (`P`) | reset / UART mode with acknowledge; the MIDI bytes play on the General MIDI synthesizer |
| DMA | channel 1 (`D`), 5 (`H`) | address, count, page, mode, mask, flip-flop, status as the program programs them; the counters move with the playback |
| IRQ | 5 (`I`) | raised at the end of each block, acknowledged by reading 22Eh / 22Fh |

The defaults are `A220 I5 D1 H5 P330`, the usual BLASTER setting of a Sound Blaster 16:
`SET BLASTER=A220 I5 D1 H5 P330 T6` (INSTAUD writes it to AUTOEXEC.BAT). `/SB:` changes them
(e.g. `/SB:I7` for games that only know IRQ 7); keep BLASTER in step.

**Windows 3.x (386 Enhanced Mode) and Windows 95 / 98 / ME:** the VxD traps the card's ports
in every virtual machine, takes over the two DMA channels from VDMAD and the IRQ from VPICD.
Every MS-DOS box has the card; the box that used it last owns it. Nothing has to be loaded
in the box. Programs running in protected mode under Windows' DPMI (DOS/4GW, DOS32A, PMODE …)
go through the same traps (verified with real-mode test programs only).

**Plain DOS (MS-DOS 5 – 8, FreeDOS):** nothing else is needed. When DOS runs in real mode
(HIMEM.SYS or HIMEMX only, no EMM386), WINAUDIO.SYS starts a small **V86 monitor** of its
own: DOS goes on in virtual 8086 mode, and the ports of the card, of the FM synthesizer, of
the MPU-401 and of the two DMA channels are trapped by the processor's I/O permission bitmap.

```
DEVICE=C:\DOS\HIMEM.SYS
DEVICE=C:\WINDOWS\WINAUDIO.SYS
```

```
WINAUDIO: Sound Blaster 16 emulation, SET BLASTER=A220 I5 D1 H5 P330 T6
WINAUDIO: DOS programs: ports trapped by the built-in V86 monitor, VCPI and DPMI
WINAUDIO:   available, RTC interrupt 1024 Hz
```

The monitor is there for every kind of DOS program:

* **Real-mode programs** run in V86 mode with their ports trapped. HIMEM keeps working: the
  monitor does the INT 15h AH=87h block moves and emulates the A20 gate (port 92h and the
  keyboard controller) through the page tables, so "A20 off" wraps memory like a real PC.
* **Protected-mode programs** get a **DPMI 0.9 host** (INT 2Fh AX=1687h): DOS/4GW, DOS/32A,
  PMODE/W, CauseWay, DJGPP programs and Borland's 16-bit DPMI programs run as its clients in
  ring 3, with the same ports trapped, so games like DOOM, Duke Nukem 3D, Descent or Warcraft
  II have the card too. The host gives memory from XMS (functions 0501h–0503h), DOS memory
  (0100h–0102h), descriptors, real-mode calls and callbacks, raw mode switches, exception
  handlers, and routes hardware interrupts (the card's too) to the program's protected-mode
  handlers in either mode. A DPMI program may start another one.
* **VCPI** (INT 67h AX=DExxh, with the device EMMQXXX0 like EMM386 NOEMS) is served for
  WINDISK.SYS / WINUSB.SYS and for Windows 3.x Standard Mode. DOS extenders that prefer VCPI
  to DPMI (DOS/32A, PMODE/W, CauseWay) would run in ring 0 under it, where ports cannot be
  trapped, so with the DPMI host VCPI is hidden from other programs; `/VCPI` shows it to all.
* **Windows** 3.x 386 Enhanced Mode and Windows 95 / 98 / ME get the processor back in real
  mode through the switch routine of INT 2Fh AX=1605h (as with EMM386); after Windows the
  monitor runs again. Windows 3.x Standard Mode runs as a VCPI client.

On Windows 95 / 98 / ME, whose IO.SYS loads HIMEM.SYS only after the CONFIG.SYS drivers,
`DEVICE=C:\WINDOWS\HIMEM.SYS` must come before WINAUDIO.SYS (INSTAUD adds it); the monitor
then runs in "Restart in MS-DOS mode" sessions too and hands the processor to Windows when
it starts.

The card is served between timer ticks from the RTC periodic interrupt (IRQ 8, 1024 Hz),
and its interrupt reaches the program the way the PIC would deliver it: only while the
program has interrupts enabled and the IRQ unmasked, with the program's EOI and in-service
reads answered for it.

The monitor needs no EMS: there is none (like EMM386 NOEMS) and no upper memory blocks. A
memory manager in V86 mode loaded before WINAUDIO.SYS takes precedence: **QEMM** (386 or 97)
or **JEMM386 / JEMMEX** with its QPIEMU module trap the ports for WINAUDIO.SYS instead
(real-mode programs only); EMM386, which has no port trapping, leaves DOS without the
emulation (WINAUDIO.SYS says so). With JEMM:

```
DEVICE=C:\JEMM\JEMMEX.EXE            (or HIMEM.SYS + JEMM386.EXE)
DEVICE=C:\JEMM\JLOAD.EXE C:\JEMM\QPIEMU.DLL
DEVICE=C:\WINDOWS\WINAUDIO.SYS
```

Programs that need real mode itself (switching to protected mode on their own without VCPI
or DPMI, "unreal mode" programs such as Ultima VII) cannot run in V86 mode: the monitor
stops them with a message; load WINAUDIO.SYS with `/NOV86` for them (they run without the
emulation then, as under any memory manager). `AUDTEST` shows the settings and the state, `AUDTEST SB` tries the
card the way a game does (DSP, interrupt, DMA sound, FM detection, MPU-401).

**USB Audio (with WINUSB):** the emulation also plays on a USB Audio 1.0 / 2.0 device
(headset, USB speakers, USB sound card) that WINUSB drives, in plain DOS, in the MS-DOS
boxes of Windows 3.x and in Windows 95 / 98 / ME. WINAUDIO mixes the card (DSP, FM, MPU-401
on the synthesizer) into a ring in its memory, WINUSB plays that ring on its first USB Audio
output like any other sound; the card keeps the USB device's pace. It is used when WINAUDIO
has no HD Audio / AC'97 / DSP device of its own, or always with `/USBSB`.

* DOS: WINUSB.SYS (with `/USB`, USB in DOS) is loaded **before** WINAUDIO.SYS (the
  installers put the lines in this order). WINAUDIO.SYS waits up to 3 s for WINUSB to find
  a USB Audio output (devices behind hubs come late) and stays resident even without sound
  hardware of its own; a USB Audio device plugged in later is taken too. `/USB` belongs to
  WINUSB.SYS; on WINAUDIO.SYS the switch that sends the emulation to USB while there is an
  HD Audio / AC'97 device is `/USBSB`. When the bridge is not there, the start message of
  WINAUDIO.SYS says why (WINUSB.SYS not loaded before it, without `/USB`, no USB Audio
  output):
  ```
  DEVICE=C:\DOS\HIMEM.SYS
  DEVICE=C:\WINDRV\WINUSB.SYS /USB
  DEVICE=C:\WINDRV\WINAUDIO.SYS
  ```
* Windows 3.x: with both DOS drivers loaded, their VxDs find each other the same way.
* Windows 95 / 98 / ME: WINAUDIO.VXD and WINUSB.VXD find each other by themselves.
  WINAUDIO.VXD is loaded for an HD Audio / AC'97 / DSP device only, so a machine whose only
  sound is USB has the emulation in plain DOS and in Windows 3.x, not in Windows 9x.
* The sound runs about 0.3–0.5 s behind the program in DOS (WINUSB schedules the USB
  transfers 150 ms ahead) and about 0.2 s in Windows; the interrupts come at the right rate.

Notes:

* `AUDTEST SB` measures the RTC interrupt that drives the emulation in DOS (1024 per
  second). When it does not arrive (HPET legacy replacement mode, a firmware that routes
  IRQ 8 elsewhere) the emulation runs from the timer tick only and its interrupts come late.
* The sound of the DSP and of the FM synthesizer runs about 50–100 ms behind the program
  (the device plays ahead of it); the interrupts come at the right rate.
* The DMA terminal count bit of the 8237 status register is emulated everywhere: in plain
  DOS by the monitor, in Windows 95 / 98 / ME by the VxD answering the status ports 08h /
  D0h, in Windows 3.x by the VxD's fault hooks in front of VDMAD's trap of those ports (the
  other channels' bits come from VDMAD in both).
* With the emulation active under QEMM / JEMM, the 8237 registers of the other DMA channels
  are written directly, past the memory manager: floppy disk DMA into memory that QEMM / JEMM
  maps elsewhere (UMBs) would go wrong. `/NOSB` turns the emulation off. The built-in
  monitor maps memory 1:1 and has no such problem.
* Creative ADPCM transfers keep their timing but play as silence; recording returns silence.
* The mixer starts at full level (a real card starts lower), so games that never touch the
  mixer are not quiet.

## 4. How it works

```
  Windows programs (Media Player, Sound Recorder, games ...)
        | MMSYSTEM
  WINSOUND.DRV  --- a ring buffer in conventional memory per stream (GlobalDosAlloc),
        |          filled / emptied by a 10 ms multimedia timer
        | requests: protected-mode entry of the VxD (Enhanced) / INT 2Fh D5h through DPMI (Standard)
  WINAUDIO.386 (Enhanced Mode)   or   WINAUDIO.SYS + WINACORE.BIN (DOS / Standard Mode)
        | sound engine: reads the streams, converts the rate, mixes, synthesizes MIDI
  HD Audio / AC'97 DMA ring (16-bit stereo at the device rate: 48 kHz)
```

* **386 Enhanced Mode:** every 10 ms (in a VMM global event) the VxD fills the hardware ring
  up to ~70 ms ahead. Latency is low, fine for games and MIDI.
* **DOS and Standard Mode:** the core is polled from the timer interrupt (INT 08h, ~18 Hz);
  no poll can happen while DOS is inside a function, so the ring is filled ~400 ms ahead.
* **Position:** the playback position reported to programs is computed from the sample the
  hardware really plays (the device's latency is subtracted); `waveOutGetPosition` is exact
  to the sample.
* When Windows starts the DOS core lets go of the hardware and the VxD takes it over with the
  same code; when Windows exits the DOS core takes it back.
* DMA stops when sound is not used (2 seconds idle).
* No interrupts are used (polling), so there are no IRQ sharing problems.

### Devices in Windows

| Device | Name | Note |
|--------|------|------|
| Wave out | `WIN HD Audio vvvv:pppp` / `WIN AC'97 ...` / `WIN <USB product name>` | one per sound device |
| Wave in  | the same names | for devices that can record |
| MIDI out | `WIN GM Synth` (`WIN USB GM Synth` with WINUSB), USB MIDI devices | the synthesizer plays on the first sound device |
| Aux      | `WIN Master Volume`, on AC'97 `WIN CD Audio`, `WIN Line In` | through `auxSetVolume` (Windows 3.x) |
| Mixer    | the wave out name | Windows 95 / 98 / ME (`mixer=winsound.drv`) |

`waveOutSetVolume` sets the device's wave level, `midiOutSetVolume` the synthesizer level.

**Mixer (Windows 95 / 98 / ME).** One mixer per sound device, for Volume Control (SNDVOL32),
the volume icon of the taskbar (SYSTRAY; Control Panel → Multimedia → "Show volume control
on the taskbar") and every program that uses the mixer API:

| Line | Controls |
|------|----------|
| Volume Control (speakers) | master volume, mute |
| Wave | the wave level (the same value as `waveOutSetVolume`) |
| SW Synth | the synthesizer level (`midiOutSetVolume`) |
| CD Audio, Line In (AC'97) | the analog inputs' levels |
| Recording Control | recording source (microphone, line in, CD, aux, stereo mix as the device has them); each source shows the recording level (one level for all) |

Changes made through any of these (or WINMIX) are told to the open mixers, so Volume Control
and the taskbar icon follow other programs.

**Volume scale.** Windows volumes are linear in amplitude (half the value is half the
amplitude, −6 dB). The hardware amplifiers (HD Audio, AC'97, USB Audio, the codecs of the
Intel DSP) work in dB steps: the value is converted to dB, so a slider moves the loudness
evenly and the same value gives the same loudness as in the software mix.

**Saved volumes.** WINSOUND.DRV writes the volumes of each device (master, wave, synthesizer,
recording, CD, line, recording source, mute) to `Volume<core>.<device>=` in the
`[winsound.drv]` section of SYSTEM.INI when they change, and sets them again when Windows
starts. WINMIX writes the same keys.

### Synthesizer

32 voices of two-operator FM (modulator + carrier, with feedback); separate ADSR envelopes
and modulation depths for the 128 General MIDI instruments, noise for winds and effects; a
47-note drum kit on channel 10 (bass drum, snares, toms, cymbals and hi-hats, hand drums
…). When the voices run out the quietest finishing voice, else the oldest, is taken. To save
processor time it is generated at half the rate (24 kHz) and converted up. The level
follows the pitch (low notes a little louder, high notes 1.5 dB per octave softer, like the
key scaling of an OPL), the instruments go through a gentle low pass (about 6 kHz, the drum
kit not), and the envelopes change sample by sample, so notes start without a tick. The instruments
are built into the driver (no SoundFont or sample set; there is nothing to configure). It is
not the OPL3 of the Sound Blaster emulation (§3b): that one plays the FM registers DOS games
write, this one plays General MIDI (Windows MIDI, the MPU-401 of the emulation).

## 5. Tools

```
AUDTEST                      sound devices and the Sound Blaster emulation settings
AUDTEST TONE [hz] [device]   2-second test tone (left channel, then both)
AUDTEST PLAY file.wav        play a WAV (PCM 8..32 bit, float, any rate)
AUDTEST REC file.wav [s]     record (16-bit stereo, default 5 s at 44100 Hz)
AUDTEST MIDI                 synthesizer test (8 instruments, chords, drums)
AUDTEST MIX [device]         mixer values
AUDTEST MIX 0 MASTER 80      volume (MASTER WAVE SYNTH INPUT CD LINE, 0..100)
AUDTEST MIX 0 SRC LINE       recording source (MIC LINE CD AUX MIX)
AUDTEST SB                   test the Sound Blaster emulation (DSP, IRQ, DMA, FM, MPU-401)
AUDTEST LOG                  driver log (codec, paths, errors)
AUDTEST /USB ...             the USB sound devices of WINUSB.SYS
```

WINMIX (Windows): device selection, Master / Wave / Synth / Rec level / CD / Line sliders,
left-right lock, recording source. The settings are kept in SYSTEM.INI like those of the
mixer (Windows sets them again at its start).

## 6. Hardware notes

* **HD Audio controller:** link reset (with clock gating off on Intel Skylake and later),
  CORB / RIRB (falling back to the immediate command interface when no answer comes), DMA
  position from LPIB (from the position buffer on VIA). Cache coherency is set up as in the
  Linux driver: Intel TCSEL and no-snoop off on PCHs, ATI / AMD ENABLE_SNOOP, nVidia snoop
  bits; on controllers that do not snoop the DMA areas are written back with CLFLUSH /
  WBINVD. A function in D3 is brought to D0, MSI / MSI-X are turned off. AMD FCH
  controllers (Ryzen / AM4 / AM5 boards: 1022:1457, 1487, 157A, 15E3 and the older ATI SB
  ids) get the ATI snoop register, a 1-second codec answer time and repeated link resets
  until a codec answers; every HD Audio function of the machine is probed (up to 8).
* **Codec:** with several codecs the first with an analog output is used (HDMI / DisplayPort
  codecs are skipped). Paths from the line-out, speaker and headphone pins to a DAC (through
  mixers / selectors) are set up, the amplifiers on the way are opened and the volume is
  applied at the stepped amplifier closest to the DAC. When headphones are plugged in (jack
  detection) the speaker pins are turned off. For recording the analog ADC reaching the most
  input kinds is chosen (on ALC256 and later the first ADC sees only the digital microphone);
  microphone (with VREF bias), line in, CD and aux paths are connected to it. Stereo mix is
  offered only when a mixer the ADC reaches also contains a playing DAC (Realtek's 0Bh analog
  loopback mixer mixes the inputs only and is no stereo mix). Playback is 48 kHz (44.1 kHz
  when the codec supports 44.1 but not 48).
* **AC'97:** cold reset and waiting for codec ready, IOSE on ICH4+, the swapped SR / PICB
  registers and byte counts of SiS 7012, the CIV / PICB read race of nVidia, the AC-link start
  and SGD channels of VIA (686: type register, 8233+: SDX format / rate register). With VRA
  the DAC / ADC are set to 48 kHz; the master volume's 5 / 6-bit step count is measured; EAPD
  is turned on (`/EAPD` for amplifiers wired the other way). A secondary codec (at
  register offset 80h) is used when the primary one is a modem; Cirrus codecs (CS4297A /
  CS4299 of the IBM ThinkPad T2x / X2x / A2x) are switched to their normal mode first. On
  IBM / Lenovo ThinkPads the embedded controller's volume and mute (the volume buttons) are
  set to full and unmuted at start (`/NOTPVOL` leaves them).
* **Realtek codecs:** the start-up the Linux `snd-hda-codec-realtek` driver does for each
  codec is applied: moving EAPD control from COEF to the EAPD verb (ALC2xx / 6xx / 8xx / 1220
  …), PLL gating off (ALC662 and ALC883 families, ALC269VA), the ALC269VB settings, headphone
  amplifier high power and the 3k pull-down off on the ALC255 / 256 / 236 / 257 and ALC225 /
  215 / 245 / 285 / 287 families, the amplifier bits of ALC260 / ALC880–885 / 888; the external
  amplifier GPIO from the codec's assembly id (a valid codec subsystem id or the checksummed
  configuration of pin 1Dh); Linux's special COEF values for MSI / Clevo ALC1220 notebooks and
  Gigabyte X570 boards. The codec parser and these settings were run on Linux against 198
  Realtek codec dumps from real machines (ALC255, ALC256, ALC225, ALC287, ALC1220, ALC887,
  ALC892, ALC888 / 889, ALC1200, the ALC269 family, ALC262 / 268 / 272 / 282, ALC66x, ALC86x
  …): in every one the speaker / headphone / line-out paths were set up correctly from the
  powered-down state.

## 6a. Intel audio DSP (SST / Smart Sound Technology)

Since Skylake most Intel notebooks have a DSP in the audio controller. The HD Audio codec
(Realtek, Conexant …) works on the HD Audio link independently of the DSP, but the **internal
digital microphones** are on the DSP's PDM interface, and on many machines the codec and the
speaker amplifiers are on the DSP's I2S (SSP) or SoundWire links. For them WINAUDIO loads the
DSP firmware, sets up the audio pipelines in the DSP over IPC and programs the codecs over
I2C / SoundWire.

| Platform | PCI id | Firmware | IPC |
|----------|--------|----------|-----|
| Skylake, Kaby Lake (Amber / Whiskey) | 9D70, A170, 9D71, A171, A2F0 | `DSPSKL.BIN` (Intel AVS) | IPC4 |
| Apollo Lake, Gemini Lake | 5A98, 1A98, 3198 | `SOFAPL.RI` / `SOFAPLC.RI` | SOF IPC3 |
| Cannon / Coffee / Comet Lake, Rocket Lake | 9DC8, A348, 02C8, 06C8, A3F0, F1C8 | `SOFCNL.RI` / `SOFCNLC.RI` | SOF IPC3 |
| Ice Lake | 34C8, 3DC8 | `SOFICL.RI` / `SOFICLC.RI` | SOF IPC3 |
| Jasper Lake | 38C8, 4DC8 | `SOFJSL.RI` / `SOFJSLC.RI` | SOF IPC3 |
| Elkhart Lake | 4B55, 4B58 | `SOFEHL.RI` / `SOFEHLC.RI` | SOF IPC3 |
| Tiger Lake (Comet Lake-R) | A0C8, F0C8 | `S4TGL.RI` / `S4TGLC.RI` | SOF IPC4 |
| Tiger Lake-H | 43C8 | `S4TGLH.RI` / `S4TGLHC.RI` | SOF IPC4 |
| Alder Lake-P, Raptor Lake-P | 51C8…51CF | `S4ADL.RI` / `S4TGLC.RI` | SOF IPC4 |
| Alder Lake-N | 54C8 | `S4ADLN.RI` / `S4TGLC.RI` | SOF IPC4 |
| Alder Lake-S, Raptor Lake-S | 7AD0, 7A50 | `S4ADLS.RI` / `S4TGLHC.RI` | SOF IPC4 |
| Meteor Lake, Arrow Lake | 7E28, 7728 | `S4MTL.RI` / `S4MTLC.RI` | SOF IPC4 |
| Arrow Lake-S | 7F50 | `S4ARLS.RI` / `S4MTLC.RI` | SOF IPC4 |
| Lunar Lake | A828 | `S4LNL.RI` / `S4LNLC.RI` | SOF IPC4 |
| Panther Lake, Wildcat Lake | E428, E328, 4D28 | `S4PTL.RI`, `S4WCL.RI` (+`C`) | SOF IPC4 |
| Nova Lake | D328, 6E50 | `S4NVL.RI`, `S4NVLS.RI` (+`C`) | SOF IPC4 |
| Bay Trail, Cherry Trail / Braswell, Broadwell | ACPI 80860F28, 808622A8, INT3438 | `SOFBYT/SOFCHT/SOFBDW.RI` | SOF IPC3 |

* **Firmware:** the files of `WINDRVFW` are redistributable binaries of Intel and the Sound
  Open Firmware project (`FIRMWARE.TXT`, licences `LICSOF.TXT`, `LICSST.TXT`, `LICCIRR.TXT`).
  The DOS driver reads them into XMS at boot (from `WINDRVFW` in the directory of the
  WINAUDIO.SYS of CONFIG.SYS, else from that directory itself), the Windows 95/98/ME driver
  from `WINDOWS\SYSTEM\WINDRVFW`. The DSP is started with the Intel-signed file first; when
  the ROM rejects the signature (Chromebooks) the "community" signed file (name ending in `C`)
  is tried.
* **When it starts:** when the machine has no HD Audio codec, when the ACPI NHLT table
  reports a digital microphone or I2S endpoint, or when ACPI has a known I2S codec or
  SoundWire controller. `/NODSP` turns it off, `/DSP` drives the HD Audio codec through the
  DSP too.
* **Digital microphones:** 2 or 4 microphones with the PDM configuration of the NHLT; 4
  microphones are folded to stereo. A separate recording device in Windows: "Intel SST
  Digital Microphones".
* **I2S codecs and amplifiers** (found by their ACPI ids, programmed over I2C; the I2C
  controller is the Intel LPSS DesignWare, the speaker amplifier enable lines are Intel GPIO
  pins): Everest ES8316 / ES8336 / ES8326, Realtek RT5682 / RT5682S / RT5640 / RT5642 /
  RT5645 / RT5650 / RT5651 / RT5663 / RT286 / RT288, the RT1011 / RT1015 / RT1015P / RT1019 /
  RT1308 amplifiers, Maxim MAX98357A / MAX98360A / MAX98373 / MAX98390 / MAX98927 / MAX98090,
  Dialog DA7219 / DA7213, Nuvoton NAU8825 / NAU8821 / NAU8824 / NAU8318 / NAU8315, Cirrus
  CS42L42, TI TAS2563, ADI SSM4567, Conexant CX2072X. The SSP port, clock and format come from
  the NHLT blob (else Linux's defaults for that platform). Output moves to the headphones when
  they are plugged in, the headset microphone is detected.
* **SoundWire** (Comet Lake and later; Intel cAVS, ACE 1.x and ACE 2+ links): the peripherals
  are enumerated, data ports and bandwidth computed, amplifiers on several links joined into
  one stereo stream. Drivers: Realtek RT700 / RT711 / RT711-SDCA / RT712 / RT713 / RT714 /
  RT715 / RT716 / RT717 / RT722, the RT1308 / RT1316 / RT1318 / RT1320 / RT1321 / RT1017
  amplifiers, Cirrus CS42L43 (patch `CS42L43.BIN`), CS35L56 / CS35L57 / CS35L62 / CS35L63,
  CS42L42, Maxim MAX98373, Realtek RT5682.
* **Bay Trail / Cherry Trail / Broadwell:** the DSP is an ACPI device; its addresses are read
  from the BIOS's ACPI NVS area, the codec's MCLK is enabled from the PMC platform clock
  (19.2 MHz). On tablets the codec is mostly an RT5640 / RT5645 / RT5651, ES8316, NAU8824,
  DA7213, MAX98090 or CX2072X on SSP2 (SSP0 on Bay Trail CR).
* `AUDTEST LOG` shows the DSP start, the firmware version, the endpoints and codecs found.

## 7. Programming interface (INT 2Fh)

`AH = D5h`: `AL=00h` installation check (`AL=FFh`, `BX=3357h`, `ES:DI` control block),
`AL=01h` API call (`ES:BX` → `struct winapireq`, see `src/inc/winapi.h`). In Enhanced Mode
Windows programs can call the protected-mode entry of the VxD (device id 3D35h) with `AX=3`,
`CX:DX` = real-mode address of the request.

| Function | Purpose |
|----------|---------|
| `WINF_AUD_DEVS` (50h) | device list (`struct winauddev`) |
| `WINF_AUD_OPEN` (51h) | open a stream: device, direction, rate, bits, channels, float; buffer = `struct winring` |
| `WINF_AUD_CTL` (52h)  | start / pause / reset / close / stream volume |
| `WINF_AUD_MIX` (53h)  | mixer and recording source |
| `WINF_AUD_MIDI` (54h) | a MIDI message or byte string (SysEx included) |

The ring buffer (`struct winring`, 32-byte header + data) is in conventional memory; the
producer advances `wr`, the consumer `rd`, the driver writes the bytes heard / recorded into
`pos`.

## PCI resources above 4 GB

With "Above 4G Decoding" enabled in the BIOS setup, the firmware may place the registers of
some devices (64-bit BARs) and bridge windows above 4 GB, where 32-bit DOS and Windows
cannot reach them. At boot, before Windows and its drivers look at the machine, the DOS
driver (WINAUDIO.SYS; whichever WINDRV DOS driver loads first does it for all) moves those
resources of **the devices WINDRV drives** into free PCI space below 4 GB: USB controllers
(UHCI / OHCI / EHCI / xHCI / USB4), audio (HD Audio, AC'97, Intel audio DSP), disk
controllers (IDE, SATA, RAID, NVMe), SD controllers, Intel LPSS I2C and Thunderbolt NHI.
When such a device sits behind a bridge with a prefetchable window above 4 GB, the whole
tree below that bridge (nested bridges and the BARs behind them, largest first) is placed
again. **Graphics cards and all other devices stay where the firmware put them** (the log
says `pci 01:00.0 BAR above 4G left in place (not a WINDRV device; /ALL4G moves it)`), so
"Above 4G" only affects the drivers' own devices. `/ALL4G` moves every device and bridge
window (a large BAR that does not fit is shrunk with Resizable BAR), `/NORELOC` turns the
pass off (a device's BAR is then moved alone when its driver starts it). The free space is
computed from TOLUD..PCIEXBAR on Intel Core chipsets, TOP_MEM..FEC00000 on AMD, and every
BAR, window and ACPI MCFG range in the system. Each move is logged as
`pci 00:14.0 BAR 4000000000 -> 9f000000 (64K)`.

On Windows 95 / 98 / ME the same is done by **WIN4G.VXD**: the INF files (and the installer)
copy it to `SYSTEM\` and add `device=win4g.vxd` to the `[386Enh]` section of SYSTEM.INI.
This small static VxD does the move at every boot, before the Plug and Play PCI enumeration
and before all device drivers (Sys_Critical_Init), so the devices come below 4 GB on
Windows ME too, which has no CONFIG.SYS driver. One restart is needed after the first
installation. `All4G=1` in the `[WIN4G]` section of SYSTEM.INI moves all devices (like
`/ALL4G`); `Debug=1` (port E9h) or `Debug=COM1` log what it does.

## Architecture

```
            DOS / Real / Standard Mode                386 Enhanced Mode
   +-----------------------------------+   +-----------------------------------+
   | DOS, programs, Windows (real /    |   | Windows VMs (system VM + DOS)     |
   | standard mode, DOSX)              |   |                                   |
   +-----------------+-----------------+   +-----------------+-----------------+
                     | block / character device requests, INT 2Fh API
   +-----------------v-----------------+   +-----------------v-----------------+
   | WINAUDIO.SYS   (16-bit DOS driver)   |-->| WINAUDIO.386   (VxD, ring 0)          |
   |  - drive letters, INT 2Fh API     |   |  - the same 32-bit core code      |
   |  - 8042 injection (kbd / mouse)   |   |  - VPICD, VKD, WINMOUSE, VMM      |
   +-----------------+-----------------+   +-----------------------------------+
                     | switch to protected mode (directly in real mode, VCPI under EMM386)
   +-----------------v-----------------+
   | WINACORE.BIN   (32-bit core in an   |
   | XMS block, with paging)           |
   +-----------------------------------+
```

* The DOS driver is loaded by CONFIG.SYS, puts the 32-bit core into XMS memory and switches
  the processor to protected mode for a moment on every request.
* The VxD is loaded **automatically** when Windows 386 Enhanced Mode starts (INT 2Fh /
  1605h); SYSTEM.INI needs no line. The DOS core lets go of the hardware, the VxD (compiled
  from the same sources) takes it over, and the DOS core takes it back when Windows exits.
* In DOS, Real and Standard Mode the core is polled from the timer (INT 08h) and DOS idle
  (INT 28h) interrupts, only at safe moments (DOS not inside a function, no BIOS disk call
  in progress). In Enhanced Mode the VxD runs every 10 ms.
* Windows 95 / 98 / ME use the Plug and Play VxD built from the same core instead.
* An exception in the core writes `WINAUDIO CORE EXCEPTION ...` on a red background in the
  first screen line and stops; a photo of that line is enough to locate the fault.
* API: INT 2Fh, AH=D5h (see `src/inc/winapi.h`).

## Building

Needed: **Open Watcom v2** (wcc, wcc386, wrc, wlink), **NASM** and **Python 3**.

```sh
export WATCOM=/opt/watcom          # the Open Watcom installation
cd src
./build.sh                         # objects in ../obj, the files sorted into ../bin/DOS,
                                   # ../bin/WIN3X and ../bin/WIN9X (src/bin-layout.txt)
```

`src/buildtools` holds the build helpers: `fixvxd.py` (turns the LE file of wlink into the
form the Windows 3.1 VMM expects), `oslayout.py` (sorts the output by system), `genserv.py`
(VxD service numbers from the DDK INC files; `src/inc/vxdsvc.inc` comes ready made),
`genicons.py` (program icons) and `mkdisk.py` (disk images for tests).

`src/firmware` is copied into `bin/WIN9X/WINDRVFW` (the INF files and the installer take it from there).

Source files and documents are stored with CRLF line ends; `.gitattributes` keeps them so.

### Source tree

| Directory | Contents |
|-----------|----------|
| `src/core` | the 32-bit core (all systems): PCI, ACPI, HD Audio (`hda.c`, `afunc.c`), AC'97, sound engine (`aeng.c`), synthesizer, Intel DSP (`dsp*.c`, `nhlt.c`), I2S codecs (`cdc*.c`), SoundWire (`sdw*.c`), DesignWare I2C, Intel GPIO |
| `src/inc` | shared headers (`winapi.h`: API and control block), VxD service numbers |
| `src/dos` | DOS: WINAUDIO.SYS (NASM), the DOS platform layer of the core, WIN30FIX.COM |
| `src/vxd` | Windows 3.x Enhanced Mode and Windows 95/98/ME: the VxD platform layer and entries, Plug and Play (HD Audio bus), WIN4G.VXD |
| `src/inf` | Windows 95/98/ME: the INF files |
| `src/win` | Windows (16-bit, 3.x and 9x): WINSOUND.DRV, WINBEEP.DRV, WINMIX, WINGRP |
| `src/tools` | DOS programs: installer / uninstaller (`winsetup.c`), AUDTEST |
| `src/firmware` | the DSP and SoundWire firmware (`FIRMWARE.TXT`), copied to `bin/*/WINDRVFW` |
| `src/tests` | host tests: sound engine and synthesizer, HD Audio codec parser (`hdacodec`) |
| `src/buildtools` | build helpers (Python): also the sample rate converter and synthesizer table generators |

## Tests

`src/tests/host` runs the sound engine and the synthesizer on Linux (Open Watcom Linux
target): the SNR of the sample rate converter, float / 8-bit inputs, playing a MIDI file
(`run.sh [file.mid]`). `src/tests/host/hdacodec` runs the HD Audio codec parser against
Linux `/proc/asound/card*/codec#*` dumps (`run.sh dump...`): it shows the chosen output pins,
DAC paths, volume nodes, the ADC and input paths, the GPIO and COEF writes.
`src/buildtools/gensrc.py` makes the sample rate converter filters (`src/core/srctab.h`),
`gensynth.py` the synthesizer tables (`src/core/synthtab.h`), `mkgpio.py` the Intel GPIO
pad tables (`src/core/gpiotab.h`) from the Linux pinctrl drivers.

QEMU + SeaBIOS (`intel-hda` + `hda-duplex`, `AC97`; output written to a file with
`-audiodev wav` and analysed) was used to verify: HD Audio and AC'97 detection, codec paths,
the test tone and MIDI in DOS (pitch / frequency measured), recording (DOS and Windows), the
startup sound (TADA.WAV) and CANYON.MID in Media Player (synthesizer through the MIDI Mapper)
in Windows 3.1 Enhanced Mode, the same with AC'97 in Standard Mode (WIN /S), looped playback
(`WHDR_BEGINLOOP`) and sample-exact position, WINMIX, INSTAUD / UNINAUD (together with
WINUSB too); playback / recording in Windows 3.0 + Multimedia Extensions Standard and
Enhanced Mode, WINBEEP beeps and `PLAY` / `SOUND` music in Windows 3.0 Real Mode (frequencies
measured); the Windows 95 / 98 / ME cases of §3a.

## Known limitations

* **Real hardware:** the drivers were verified in QEMU and by code review. The fixes for
  particular Intel / AMD hardware were written from the Linux drivers and the datasheets.
* **Windows/386 2.x** switches the processor to V86 mode without offering VCPI; while it runs
  the 32-bit core cannot be entered and the driver serves no requests. The real-mode
  Windows 2.x (`WIN86.COM` / `WIN.COM`) works.
* **DOS file systems:** MS-DOS 5 / 6 read only FAT12 / FAT16 (2 GB per partition at most);
  MS-DOS 7.10 / 8.0 and FreeDOS read FAT32 too; NTFS, exFAT ... partitions get no drive
  letter.
* **SeaBIOS (QEMU only):** when SeaBIOS uses a USB keyboard or mouse at boot, its USB polling
  in SMM can stop the machine while Windows 2.x starts, and DOS boxes of Windows 95/98/ME
  close. Real BIOSes hand the controllers over properly (BIOS / OS semaphore, SMIs off).
* WfW 3.11 was not tried on real hardware.
* Not tried on real hardware yet; HD Audio and AC'97 were tested with QEMU's Intel ICH6 /
  ICH9 HD Audio and ICH AC'97 emulation only. The Intel DSP path (firmware loading, IPC, NHLT,
  I2S codecs, SoundWire, Bay Trail / Cherry Trail) cannot be emulated in QEMU and was written
  from the Linux drivers and the SOF firmware sources; firmware image parsing, the IPC
  messages and the SoundWire link flow were checked with simulation harnesses on the host.
  The VIA, SiS and nVidia AC'97 code and the controller-specific HD Audio settings follow the
  datasheets and the Linux driver.
* Some notebooks need a codec GPIO or a vendor-specific codec setting for the speaker
  amplifier. Of the hundreds of model-specific fixes of Linux only the generic ones are
  applied; with silent speakers try `/GPIO`. The speakers of notebooks with smart amplifiers
  on I2C next to the HD Audio codec (some Lenovo / ASUS / HP ALC287 / ALC245 models, CS35L41 /
  TAS2781) and the microphone of the combo headset jack of HD Audio notebooks (headset mode)
  are not supported.
* Intel DSP: only the basic modules (copier) of the SOF / AVS firmware are used; no audio
  effects, speaker protection / calibration (CS35L56 tuning, MAX98390 DSM, TAS2563 RCA files)
  or headset buttons. Streams are 48 kHz.
* The "Stereo Mix" of Realtek's Windows drivers is done in software; WINAUDIO has stereo mix
  only when the codec hardware offers it.
* Digital outputs (S/PDIF, HDMI) and multichannel (5.1) output are not supported; multichannel
  sources are not downmixed, the first two channels are played.
* In Windows 3.0 Real Mode MMSYSTEM does not run, so there is no wave or MIDI; only WINBEEP's
  beeps and `PLAY` / `SOUND` music play.
* No MIDI input. The synthesizer is an FM approximation, not sample based (wavetable).
* Latency is high in DOS and Standard Mode (~0.4 s; 50–100 ms while the Sound Blaster
  emulation runs from the RTC).
* Windows 95 / 98 / ME: sound through WINSOUND.DRV (MMSYSTEM); no DirectSound driver.

## Version history

**1.1 (26 October 2026)**

Changelog:

- Fixed "CPU without time stamp counter" on CPUs that have one but do not report it through CPUID (Vortex86 DX / SX / EX …): the TSC is tried directly.
- The TSC rate is measured with a time limit and with the PIT counter when the timer interrupt does not come.
- WINAUDIO.SYS loaded with DEVLOAD or another device loader finds WINACORE.BIN in its own directory; it can be loaded high (`DEVICEHIGH`).
- The installer tells Windows 98, 98 Second Edition and ME apart by the version of USER.EXE (Windows 98 was shown as Windows 95).
- Added Sound Blaster 16 emulation (DSP 4.05, mixer, OPL3 FM, MPU-401, 8/16-bit DMA, IRQ 5 or 7) for DOS programs in the MS-DOS boxes of Windows 3.x and Windows 95/98/ME, and in plain DOS with QEMM or JEMM386 / JEMMEX + QPIEMU.
- Added a built-in V86 monitor to WINAUDIO.SYS: in plain DOS (HIMEM only, no EMM386 or JEMM needed) it traps the Sound Blaster ports itself, emulates A20 and the INT 15h block move for HIMEM, and gives Windows 3.x / 9x real mode back at their start (INT 2Fh 1605h) and takes over again when they end.
- Added a DPMI 0.9 host to the V86 monitor, so protected-mode games (DOS/4GW, DOS/32A, PMODE/W, CauseWay, DJGPP) get the Sound Blaster emulation in plain DOS too: XMS memory, DOS memory, descriptors, real-mode calls and callbacks, raw mode switches, exception handlers, hardware interrupts to protected-mode handlers, nested DPMI programs.
- Added a VCPI server to the V86 monitor (with the EMMQXXX0 device) for WINDISK.SYS / WINUSB.SYS and Windows 3.x Standard Mode; hidden from other programs so that DOS extenders pick DPMI (/VCPI shows it to all).
- Added the WINAUDIO.SYS options /NOV86, /NODPMI, /NOVCPI, /VCPI and /VME.
- Fixed Windows 3.1 hanging at its start (and no sound) after a protected-mode game (DOS/4GW, Quake …) had run in DOS: the DPMI host put back the INT 15h / 1Bh vectors the program had already restored itself, pointing into freed memory.
- HD Audio and AC'97: DMA streams and the position buffer still running from before a warm restart are stopped before the controller is reset.
- Sound Blaster emulation in Windows 3.x and 95/98/ME: the DMA terminal count bit of the 8237 status registers is set for the emulated channels (ports 08h / D0h answered by the VxD).
- Added the Sound Blaster 16 emulation (and the synthesizer) on USB Audio 1.0 / 2.0 devices of WINUSB, in DOS, in Windows 3.x MS-DOS boxes and in Windows 95/98/ME: when there is no other sound device, or always with the new option /USBSB. INSTAUD puts WINAUDIO.SYS after WINUSB.SYS in CONFIG.SYS; WINAUDIO.SYS waits for WINUSB's USB Audio device, stays resident with WINUSB.SYS /USB even without sound hardware of its own, and says at the start why the bridge is not there. AUDTEST SB measures the RTC interrupt that drives the emulation in DOS.
- AC'97: the codec is tried even when the controller never reports it ready (Cirrus Logic CS4299 of the ThinkPad T23).
- Added a mixer driver to WINSOUND.DRV for Windows 95/98/ME (`mixer=winsound.drv`): Volume Control (SNDVOL32), the volume icon of the taskbar and programs using the mixer API work; master volume and mute, wave, synthesizer, CD / line (AC'97), recording level and source; changes from any program are reflected in the open mixers.
- The volumes and the recording source are kept in SYSTEM.INI `[winsound.drv]` and set again when Windows starts (they no longer go back to full after a restart).
- Volume values follow the Windows scale (linear in amplitude) on the hardware amplifiers of HD Audio, AC'97, USB Audio and the Intel DSP codecs: half volume is −6 dB, the lower half of a slider is no longer almost silent, and the mixer, programs such as Winamp and WINMIX give the same loudness for the same value.
- Fixed a short skip at the start of playback in Windows 95/98/ME (the first refill of the controller's buffer came too late).
- Fixed removed (USB) wave devices accepting format queries (DirectSound or Media Player could choose one and then fail).
- Synthesizer: fixed the harsh, noisy sound of the instruments with strong modulator feedback (organs, accordion, harmonica, overdrive / distortion guitar, strings, brass, synth leads and pads, bagpipe …), timpani played an octave low, very short woodblock, muted guitar and fret noise, breath noise not sustained, quiet hi-hats, cuica too short to be heard; basses louder and fuller, with more upper overtones (heard on small speakers too), high notes softer (key scaling, a gentle low pass like the OPL's), no tick at the start of notes (smooth envelopes, longer fade-in); MIDI real-time bytes sent as short messages no longer play notes.
- Added SET BLASTER to the installer (AUTOEXEC.BAT), the Sound Blaster settings to the WINAUDIO.SYS start message and AUDTEST, and AUDTEST SB to test the emulated card.
- WINAUDIO.SYS lists the sound devices it found with their controller, PCI location and IRQ.
- Fixed HD Audio on AMD Ryzen boards (AM4 / AM5 FCH controllers): snoop setting, codec answer time, link reset retries; every HD Audio controller is probed.
- Fixed no sound on AC'97 notebooks such as the IBM ThinkPad T23 (cold reset, secondary codec, Cirrus CS4299, ThinkPad volume / mute).
- Fixed sounds longer than 64 KB (the Windows startup sound) looping their beginning.
- Fixed old sound being played again when the driver is serviced late; loud mixes are limited softly instead of clipping.
- Added the system sounds of WIN.INI [sounds] (WAV, start and exit included) in Windows 3.0 Real Mode through WINBEEP.DRV.
- Added MS-DOS 7.0, 7.1, 8.0 and FreeDOS support; the installer adds the CONFIG.SYS lines there too.
- The documentation is split into README.md (overview, installation) and MANUAL.md (all details).

**1.0 (5 October 2026)** — first release.

**1.0 update (14 October 2026):**

* Windows 95 / 98 / ME: the HD Audio bus driver; codecs (analog, HDMI / DP, modem) and the
  Intel DSP endpoints are separate devices (`WINAUDM.INF`); AC'97.
* PCI resources above 4 GB: the boot-time move applies only to WINDRV's devices (`/ALL4G`
  for all), WIN4G.VXD on Windows 9x.
* Installer: Start menu groups on Windows 95/98/ME, "Read Me" items, no duplicate entries
  when installing again; the package is split into `bin/DOS`, `bin/WIN3X` and `bin/WIN9X`.
