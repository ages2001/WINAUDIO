# WINAUDIO 1.2

## Introduction

WINAUDIO is a sound driver for DOS, Windows 3.x, Windows 95, 98 and ME. It is written from scratch and works with the sound hardware built into PCs: Intel High Definition Audio, AC'97, the Intel audio DSP, the Ensoniq AudioPCI and the Sound Blaster Live!.

It provides wave playback and recording, a General MIDI synthesizer, a mixer, and Sound Blaster 16 emulation for DOS games.

WINAUDIO is one of the four WINDRV packages. Each package is in its own repository and can be used on its own or with the others.

* WINDISK: IDE, SATA AHCI and NVMe disks, IDE and AHCI CD/DVD drives, SD card readers and eMMC.
* WINUSB: USB host controllers and devices, PS/2 mouse and keyboard, touchpads, USB4 and Thunderbolt.
* WINAUDIO: sound on the hardware listed above.
* WINUDF: the UDF file system, read and write.

## Features

* Intel High Definition Audio on any vendor's controller, with a generic codec parser. Realtek codecs get the start-up settings of the Linux driver.
* AC'97 controllers and codecs.
* The Intel audio DSP (SST and SOF), including digital microphones, I2S codecs and SoundWire.
* The Ensoniq AudioPCI family: ES1370, ES1371, ES1373, Creative CT5880 and Ectiva EV1938.
* The Sound Blaster Live! (EMU10K1). The Audigy is not supported, and the Live! driver has not been tried on a real card yet.
* Wave playback and recording at 8 to 32 bits, including 32-bit float, with 1 to 8 channels and 2 to 192 kHz.
* A 32-stream mixer and a 32-voice General MIDI synthesizer.
* WINSOUND.DRV for Windows 3.x and Windows 95, 98 and ME, with a mixer driver and DirectSound hardware buffers.
* USB Audio 1.0 and 2.0 devices and USB MIDI through WINUSB.
* Sound Blaster 16 emulation with OPL3 FM and MPU-401, in DOS and in the MS-DOS boxes of Windows.
* WINMIX for the mixer: a window of standard controls that a screen reader can read and reach with Tab, and `/MUTE`, `/UNMUTE`, `/MASTER`, `/TONE` and `/DUMP` for the command line.
* AUDTEST for testing and diagnosis.
* Mixer settings that are kept across restarts.

## Supported systems

* MS-DOS 5.0 to 8.0 and FreeDOS: sound through AUDTEST, and Sound Blaster 16 emulation for real-mode and protected-mode (DPMI) games.
* Windows 3.0 Real Mode: system sounds and PLAY / SOUND music through WINBEEP.DRV.
* Windows 3.0 Standard and 386 Enhanced Mode: everything, with the Multimedia Extensions. Sound Blaster emulation works in MS-DOS boxes.
* Windows 3.1 and Windows for Workgroups 3.11: everything. Sound Blaster emulation works in MS-DOS boxes.
* Windows 95, 98 and ME: everything as Plug and Play devices, with DirectSound hardware buffers. Sound Blaster emulation works in MS-DOS boxes.

## System requirements

* A Pentium or later processor with a time stamp counter. Vortex86 DX, MX and EX processors are supported even though they do not report the counter through CPUID.
* MS-DOS 5.0 to 8.0 or FreeDOS, with HIMEM.SYS or another XMS driver. For Sound Blaster emulation in plain DOS, do not load EMM386 before WINAUDIO.SYS; WINAUDIO.SYS brings its own V86 monitor, DPMI host and VCPI server. QEMM 7.03 or later, and JEMM386 with QPIEMU, also work.
* Windows 3.0 with the Multimedia Extensions, Windows 3.1, Windows for Workgroups 3.11, or Windows 95, 98 or ME.
* A supported sound controller on the PCI bus.

## Installation

Run the installer from DOS, not from inside Windows 3.x. Start in `bin\DOS`.

```text
INSTAUD                            install into the Windows directory
INSTAUD C:\DRV                     another directory
INSTAUD /NOWIN                     the DOS driver only
INSTAUD /DOSONLY                   the DOS driver only, Windows keeps its own driver
INSTAUD /O /SB:A220,I7,D1,H5,P330  driver options; everything after /O goes to WINAUDIO.SYS
```

INSTAUD finds every Windows installation on the machine, copies the files and adds the driver to CONFIG.SYS after HIMEM.SYS. It sets `wave=`, `midi=` and `aux=winsound.drv` in SYSTEM.INI for Windows 3.x, and `wave=`, `midi=` and `mixer=winsound.drv` for Windows 95, 98 and ME. It points the MIDI Mapper at the synthesizer and adds `SET BLASTER=A220 I5 D1 H5 P330 T6` to AUTOEXEC.BAT. The original files are saved as `*.WNA`.

On Windows 95, 98 and ME, INSTAUD also prepares the Plug and Play driver. On Windows 98 and ME, open Device Manager, select the sound controller, choose Update Driver, then Have Disk and `C:\WINDOWS\INF`. On Windows 95, remove the sound controller and refresh; Windows finds the driver.

Uninstall with `UNINAUD`. On Windows 95, 98 and ME, give the sound device its own driver back in Device Manager first. UNINAUD removes the files from the directory named in CONFIG.SYS. If WINAUDIO was installed for Windows only, its directory is left in place and UNINAUD names it.

### By hand

1. Copy `bin\DOS` to a directory, for example `C:\WINDRV`.
2. Add these lines to CONFIG.SYS:

   ```text
   DEVICE=C:\DOS\HIMEM.SYS
   DEVICE=C:\WINDRV\WINAUDIO.SYS
   ```

   Add `SET BLASTER=A220 I5 D1 H5 P330 T6` to AUTOEXEC.BAT.

3. For Windows 3.x, copy `bin\WIN3X\WINAUDIO.386` next to WINAUDIO.SYS. Copy `WINSOUND.DRV` to `WINDOWS\SYSTEM` and add this to SYSTEM.INI:

   ```ini
   [drivers]
   wave=winsound.drv
   midi=winsound.drv
   aux=winsound.drv
   ```

   Without WINAUDIO.SYS, put WINAUDIO.386 in `WINDOWS\SYSTEM` and add `device=winaudio.386` to `[386Enh]` in SYSTEM.INI. For Windows 3.0 Real Mode, copy `WINBEEP.DRV` to `WINDOWS\SYSTEM` and add `sound.drv=winbeep.drv` to `[boot]`.

4. For Windows 95, 98 and ME, open Device Manager, select the sound controller, choose Update Driver, then Have Disk and `bin\WIN9X`.

### Sound Blaster emulation

DOS programs find a Sound Blaster 16 at port 220h, IRQ 5, DMA 1 and 5, with OPL3 FM at 388h and an MPU-401 at 330h. This works in MS-DOS boxes in Windows 3.x 386 Enhanced Mode and in Windows 95, 98 and ME. It also works in plain DOS for real-mode and protected-mode games.

WINAUDIO.SYS brings its own V86 monitor, DPMI host and VCPI server, so plain DOS needs no EMM386, JEMM or SBEMU. QEMM 7.03 or later and JEMM386 with QPIEMU also work.

Use `AUDTEST` to see the settings and `AUDTEST SB` to test the emulated card.

### Driver options

* `/SB:A220,I5,D1,H5,P330`: Sound Blaster port, IRQ, DMA, 16-bit DMA and MPU-401 port. The IRQ can be 2, 5, 7 or 10.
* `/NOSB`: no Sound Blaster emulation.
* `/NOV86`: no V86 monitor.
* `/VCPI`: VCPI for every program.
* `/NOHDA`, `/NOAC97`: leave that kind of controller alone.
* `/GPIO`, `/EAPD`: notebooks with silent speakers.
* `/NOSYNTH`: no MIDI synthesizer.
* `/EMS:n`, `/NOEMS`: expanded memory size in MB (default 16), or off.
* `/S`: show the driver log at boot.
* `/Q`: only error messages while the driver loads.

## Building

You need [Open Watcom v2](https://github.com/open-watcom/open-watcom-v2/releases), [NASM](https://www.nasm.us/) and [Python 3](https://www.python.org/downloads/). Install Open Watcom v2 for your system and put NASM and Python on your PATH.

On Linux:

```sh
cd src
export WATCOM=/opt/watcom
./build.sh
```

On Windows, in a Command Prompt:

```bat
cd src
set WATCOM=C:\WATCOM
build.bat
```

`build.bat` looks for Open Watcom in `WATCOM`, then `..\ow`, then `C:\WATCOM`.

The object files go to `obj`. The finished files are sorted into `bin/DOS`, `bin/WIN3X` and `bin/WIN9X` with `src/bin-layout.txt`. The firmware in `src/firmware` is copied to `bin/WIN9X/WINDRVFW`.

`src/buildtools` holds the helper scripts. `fixvxd.py` converts the VxD files into the form Windows 3.1 expects, and `oslayout.py` sorts the output by system.

## Repository layout

* `bin\DOS`: the DOS driver, its 32-bit core, the installer, the uninstaller, AUDTEST and the DOS documentation.
* `bin\WIN3X`: the Windows 3.0, 3.1 and Windows for Workgroups 3.11 drivers and programs.
* `bin\WIN9X`: the Windows 95, 98 and ME Plug and Play drivers, INF files and firmware.
* `src`: the source code, build scripts and build helpers.

The installer runs from `bin\DOS` and takes the files for the other systems from the folders next to it. Keep the `bin` tree together.

## Documentation

`MANUAL.md` is the complete manual: every option, the hardware and chipset notes, the programming interface, tests, known limitations and the full changelog. In the package, `bin\DOS\MANUAL.TXT` is the DOS copy and `WINAUDIO.TXT` in `bin\WIN3X` and `bin\WIN9X` is the Windows copy.

## Version history

* **1.2 (2 November 2026):** DirectSound hardware secondary buffers, Ensoniq AudioPCI and Sound Blaster Live! support, WINMIX command modes and a window a screen reader can use, mixer settings kept across restarts, expanded memory in the V86 monitor, a Windows build script, and many fixes.
* **1.1 (26 October 2026):** Sound Blaster 16 emulation, the built-in V86 monitor, the DPMI host and the VCPI server.
* **1.0 (5 October 2026):** first release.

The full changelog is in `MANUAL.md`.

## License

WINAUDIO is free software under the GNU General Public License version 3. See `LICENSE`.
