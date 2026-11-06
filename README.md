# WINAUDIO 1.2 — sound driver for DOS, Windows 3.x / 95 / 98 / ME

WINAUDIO plays sound through **Intel High Definition Audio** (any vendor's HD Audio
controller and codec), **AC'97**, the **Intel audio DSP** of newer notebooks, the **Ensoniq
AudioPCI** (ES1370 / ES1371 / ES1373, CT5880) and the **Sound Blaster Live!** (EMU10K1), on DOS,
Windows 3.0 (with the Multimedia Extensions), 3.1 / WfW 3.11 and Windows 95 / 98 / ME: wave
playback and recording, a **General MIDI synthesizer**, a mixer, and a **Sound Blaster 16
emulation** (with OPL3 FM and MPU-401) for DOS games.

WINAUDIO is one of the four WINDRV packages, each in its own repository and usable alone or
together with the others:

| Package | What it drives |
|---------|----------------|
| **WINDISK**  | IDE, SATA AHCI and NVMe disks, IDE/AHCI CD/DVD drives, SD card readers and eMMC |
| **WINUSB**   | USB host controllers and devices, PS/2 mouse and keyboard, touchpads, USB4 / Thunderbolt |
| **WINAUDIO** | HD Audio, AC'97, the Intel audio DSP (SST / SOF), Ensoniq AudioPCI, Sound Blaster Live!, General MIDI synthesizer, Sound Blaster 16 emulation for DOS programs |
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
| Windows 95 / 98 / ME | Sound Blaster emulation in MS-DOS boxes; everything, as Plug and Play devices: `WINAUDIO.VXD` (HD Audio bus + codec devices, Intel DSP, AC'97, AudioPCI, Live!; DirectSound with hardware buffers) (§3a) |

## Installation

From DOS (not inside Windows 3.x), in `bin\DOS` of this package:

```
INSTAUD                      into the Windows directory, CONFIG.SYS, SYSTEM.INI, AUTOEXEC.BAT updated
INSTAUD C:\DRV               another directory
INSTAUD /NOWIN               the DOS driver only
INSTAUD /O /SB:I7            with driver options (everything after /O goes to WINAUDIO.SYS)
```

The installer finds every Windows of the machine, copies the files, adds the driver to
CONFIG.SYS after HIMEM.SYS (also on plain MS-DOS 7.x / 8.0 and FreeDOS), sets
`wave=` / `midi=` / `aux=winsound.drv` (Windows 95 / 98 / ME: `wave=` / `midi=` /
`mixer=winsound.drv`) in SYSTEM.INI, routes the MIDI Mapper to the
synthesizer and writes `SET BLASTER=A220 I5 D1 H5 P330 T6` to AUTOEXEC.BAT; the originals are
saved as `*.WNA`. On Windows 95 / 98 / ME it prepares the Plug and Play driver: then pick
the sound controller in **Device Manager** (Windows 98 / ME: Update Driver → Have Disk →
`C:\WINDOWS\INF`; Windows 95: Remove + Refresh). Uninstall: `UNINAUD`.

### By hand

1. Copy `bin\DOS` to a directory, e.g. `C:\WINDRV`.
2. CONFIG.SYS (the Sound Blaster emulation in plain DOS needs nothing else: WINAUDIO.SYS
   brings its own V86 monitor and DPMI host; do not load EMM386 before it):
   ```
   DEVICE=C:\DOS\HIMEM.SYS
   DEVICE=C:\WINDRV\WINAUDIO.SYS
   ```
   AUTOEXEC.BAT: `SET BLASTER=A220 I5 D1 H5 P330 T6`
3. Windows 3.x: copy `bin\WIN3X\WINAUDIO.386` next to WINAUDIO.SYS (loaded by itself in 386
   Enhanced Mode; without WINAUDIO.SYS put it into `WINDOWS\SYSTEM` and add
   `device=winaudio.386` to `[386Enh]` of SYSTEM.INI), copy `WINSOUND.DRV` to
   `WINDOWS\SYSTEM` and set in SYSTEM.INI:
   ```
   [drivers]
   wave=winsound.drv
   midi=winsound.drv
   aux=winsound.drv
   ```
   Windows 3.0 Real Mode: `sound.drv=winbeep.drv` in `[boot]` (with `WINBEEP.DRV` in
   `WINDOWS\SYSTEM`) for the system sounds.
4. Windows 95 / 98 / ME: Device Manager → the sound controller → Update Driver → Have Disk →
   `bin\WIN9X` of this package.

### Sound Blaster emulation

DOS programs find a Sound Blaster 16 at 220h, IRQ 5, DMA 1 and 5, with OPL3 FM at 388h and an
MPU-401 at 330h: in every MS-DOS box of Windows 3.x (386 Enhanced Mode) and Windows 95 / 98 /
ME, and in plain DOS for real-mode games and for protected-mode games (DOS/4GW, DOS/32A,
PMODE/W, CauseWay, DJGPP): WINAUDIO.SYS runs DOS under a V86 monitor of its own, with a DPMI
host and a VCPI server, so no EMM386, JEMM or SBEMU is needed (QEMM or JEMM + QPIEMU work
too). `AUDTEST` shows the settings, `AUDTEST SB` tries the card. Details: MANUAL.md §3b.

### Most used options (WINAUDIO.SYS)

| Option | Meaning |
|--------|---------|
| `/SB:A220,I5,D1,H5,P330` | Sound Blaster settings (port, IRQ 2/5/7/10, DMA, 16-bit DMA, MPU-401) |
| `/NOSB` | no Sound Blaster emulation |
| `/NOV86` | no V86 monitor (for programs that need real mode itself) |
| `/VCPI` | VCPI for every program (DOS extenders that cannot use DPMI) |
| `/NOHDA`, `/NOAC97` | leave that kind of controller alone |
| `/GPIO`, `/EAPD` | notebooks with silent speakers (amplifier switched by GPIO / EAPD wired the other way) |
| `/NOSYNTH` | no MIDI synthesizer |
| `/S` | show the driver log at boot (diagnosis) |

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

## More

**MANUAL.md** (`MANUAL.TXT` in `bin\DOS`, `WINAUDIO.TXT` for Windows) describes everything
else: all options, how the driver works, hardware and chipset notes, the programming
interface, building from source, tests and known limitations.

## Version history

**1.2 (2 November 2026)**

Changelog:

- **Sound Blaster emulation under QEMM 7.5:** QEMM's own trap callback was taken for another emulator (SBEMU) and the emulation stayed off; WINAUDIO.SYS now chains to it. QEMM traps the DMA mode and mask registers for its DMA translation: trapping them too broke floppy disk transfers (crash), so they stay with QEMM and the card's channel counts as auto-init. QEMM before 7.03, whose QPI cannot trap ports, gets its own message (`AUDTEST` likewise names QEMM 7.03+), and AUDTEST names AudioPCI / EMU10K1 devices.
- Program Manager (Windows 3.x): an **Uninstall WINAUDIO** item with a red cross on the speaker icon (Windows closes, `UNINAUD /REBOOT` runs and restarts the computer); Windows 95/98/ME: the programs as desktop shortcuts too, removed by the uninstaller. The banner lists the systems (DOS, Windows 3.x/95/98/ME).
- **DirectSound hardware secondary buffers:** the DirectSound driver now also offers up to 16 secondary buffers per device in hardware (`DSBCAPS_LOCHARDWARE`; 8 / 16 bit, mono / stereo, 100 Hz–100 kHz): the sound engine mixes them like the wave streams, each with its own rate (SetFrequency), volume and pan (SetVolumePan), position (SetPosition), playing once or looping, and duplicates (DuplicateSoundBuffer) share the memory of their original. DirectSound mixes in software only 3D buffers and what does not fit. Verified in Windows 98 on AC'97, the Ensoniq ES1370 and a USB Audio device.
- DirectSound primary buffer: the write cursor stands 40 ms after what the sound engine has taken, and what it has taken becomes silence. The engine takes the buffer in steps of its service; DirectSound's mixer (and programs writing the primary buffer, such as the farbrausch music disk fr-028 "Brullwurfel") wrote only a few ms ahead of the old write cursor, and a late step played the sound of one turn of the buffer ago (crackling).
- **Ensoniq AudioPCI:** ES1370 (AK4531 codec, 44.1 kHz) and ES1371 / ES1373, Creative CT5880, Ectiva EV1938 (AC'97 codec, 48 kHz through the rate converter): playback, recording, mixer, Sound Blaster emulation, DirectSound. The ES1370 was tried in QEMU (DOS, WfW 3.11, Windows 98 with DirectSound); the ES1371 family, which QEMU lacks, against an emulation of the chip written from the Linux driver (`src/tests/host/es1371`).
- **Creative Sound Blaster Live! (EMU10K1):** playback through a stereo voice pair and a small DSP program (front, headphone and rear outputs), recording through the ADC channel, the AC'97 mixer. Written from the Linux and FreeBSD drivers and not tried on a real card yet (QEMU has none); the Audigy (EMU10K2) is not supported.
- `INSTAUD /DOSONLY`: only the DOS driver, with `/NOVXD`. WINAUDIO.SYS lets go of the card when Windows starts and loads nothing into it, so Windows keeps its own driver for the card (the AudioPCI and Sound Blaster Live! drivers of Windows 95/98, say), and the Sound Blaster emulation is there again in DOS and MS-DOS mode.
- Uninstalling WINUSB or WINAUDIO on Windows 95/98/ME leaves SYSTEM.INI `[drivers] wave= / midi= / mixer=winsound.drv` alone while the other package is installed (the sound devices of WINAUDIO had no wave driver after UNINUSB).
- The mixer settings are kept across restarts on every system: Windows 95/98/ME in the registry (set before the sound devices start, WINMIX and Volume Control alike), Windows 3.x in SYSTEM.INI as before, DOS in `WINAUDIO.VOL` next to WINAUDIO.SYS (written by `AUDTEST MIX ...`, set again when WINAUDIO.SYS starts). Volumes were back at maximum after every restart in Windows 98.
- The built-in V86 monitor gives expanded memory (LIM EMS 4.0, device `EMMXXXX0`, 64 KB page frame in a free part of the upper memory area, pages from XMS when a program asks): programs that need EMS no longer need EMM386, which leaves no Sound Blaster emulation in DOS. `/EMS:n` sets the most it gives (MB, default 16), `/NOEMS` turns it off.
- Sound Blaster emulation: the emulated card is reset when the program that used it ends (DOS and Windows MS-DOS boxes): no loud buzzing after a game exits with its sound still running.
- USB Audio through WINUSB: devices such as C-Media 0D8C:0008 whose streaming descriptors were not accepted (see WINUSB 1.2).
- WINSOUND.DRV: 64 buffers per stream and sample-accurate positions for DirectSound in emulation mode and players that queue many small buffers.
- **DirectSound driver (Windows 95/98/ME with DirectX):** every sound device registers a DirectSound driver with DSOUND.VXD. Its primary buffer is played by the sound engine as a card plays its DMA buffer, and DirectSound's kernel mixer mixes the programs' sound buffers into it: DirectSound runs on the hardware abstraction layer instead of emulation through the wave driver (lower latency, exact positions). The driver is also the one of the default device: WINAUDM.INF makes MMDEVLDR.VXD the DevLoader of the sound devices and lists WINSOUND.DRV in their devnodes, so MMSYSTEM knows which wave device belongs to which devnode. After updating, choose Update Driver for the sound device in Device Manager once (Have Disk, `WINDOWS\INF`).
- DPMI host of the V86 monitor: 16-bit clients get the MS-DOS extensions of Windows (INT 21h calls with protected-mode pointers are copied through a buffer in conventional memory; PSP, DTA, vectors, DOS memory as selectors). Borland's RTM (Borland Pascal / C++ programs in protected mode, e.g. Jazz Jackrabbit, Tyrian 2000) refused to start ("Loader error (0025)") or hung.
- DPMI host: segment bases of 16-bit clients have all 32 bits (their memory is at 40000000h; the high byte was cut off and the client wrote over the interrupt vectors); the processor type is given as Windows gives it (486 at most: older clients look it up in a table).
- DPMI host: an interrupt that came while a handler on the locked stack had called the host's default handler (PUSHF / CALL FAR) overwrote that handler's return address (crash when leaving Tyrian's setup).
- Sound Blaster emulation: the silence transfer (DSP command 80h) ends with its interrupt like other transfers (Tyrian: "ERROR 253: Sound Effects disabled").
- Sound Blaster emulation: the time between a single-cycle block and the next one the program starts is not played as silence but made up by the next block (crackling in Epic Pinball and other games that play block by block).
- The sound engine mixes further ahead when a controller fetches more than it plays (the AC'97 / HD Audio of virtual machines take tens of ms at once): no more periodic gaps there.
- The hint about EMM386 in WINAUDIO.SYS and AUDTEST fits the screen and names the EMS of the V86 monitor.
- The installer lists the Windows installations it finds, numbered, and asks which of them to set up (`1`, `1,3`, `A` for all, `0` for none: the DOS driver only); `/ALL` sets up all of them without asking, `/W:dir` only that one.
- The DOS driver takes its extended memory above 16 MB when there is room there: Windows 3.x Standard Mode (and other 286 DOS extenders) can use only the memory below 16 MB ("Insufficient extended memory" with all WINDRV drivers loaded).
- `/Q`: only error messages on the screen while the DOS driver loads.
- Fixed "CPU without CPUID / time stamp counter" on Vortex86 DX / MX / EX processors, whose CPUID instruction works although the ID flag of EFLAGS cannot be changed: the time stamp counter is tried directly.
- WININFO finds the driver also when it was loaded with DEVLOAD and a program (the command shell) later put back an INT 2Fh vector it had saved before: the driver hooks INT 2Fh again within a few seconds, and WININFO waits for that when the driver's device header is there.
- Fixed a crash at boot ("CORE EXCEPTION 0000000E") with `DEVICEHIGH=` under JEMM386 / JEMMEX: the driver used the memory after its image as a buffer while it started, and a small upper memory block ends right there; the buffer now fits in the block DOS gave.
- The texts of the DOS drivers, the VxDs and the documentation name everything the drivers support now.


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
