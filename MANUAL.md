# WINAUDIO 1.2

This is the complete manual. README.md has the short overview and installation.

## Introduction

WINAUDIO is a sound driver for DOS and Windows 3.x, 95, 98 and ME. It is written from scratch and supports the sound hardware built into PCs.

* Intel High Definition Audio. Every controller in PCI class 04/03 is supported, whatever its programming interface byte. This includes Intel controllers from ICH6 to Arrow Lake and Lunar Lake, Intel controllers in DSP mode, and controllers from AMD, ATI, nVidia, VIA, SiS, ULi, Zhaoxin and Hygon. The codec parser is generic: it walks the codec's widget graph, enables every line-out, speaker and headphone output, mutes the speakers when headphones are plugged in, and turns on EAPD. Realtek codecs get the start-up settings of the Linux driver. See Hardware notes.
* Intel audio DSP (SST / Smart Sound Technology). On Intel controllers in DSP mode from Skylake to Nova Lake, and on the SST DSP of Bay Trail, Cherry Trail and Broadwell, WINAUDIO loads the DSP firmware (Sound Open Firmware, or Intel AVS on Skylake and Kaby Lake) and drives the digital microphone array, I2S codecs and amplifiers (32 chips), and SoundWire codecs and amplifiers. HD Audio codecs run directly on the HD Audio link by default. The DSP is used only for devices that are not on that link, such as a notebook's internal digital microphones. See Intel audio DSP.
* AC'97. Intel ICH, ICH0 to ICH7, 440MX, ESB and ESB2; nVidia nForce to MCP51; AMD 768 and 8111; SiS 7012; VIA 82C686, VT8233, VT8235 and VT8237; and other ICH-compatible controllers. All AC'97 codecs are supported.
* Ensoniq AudioPCI. ES1370 with the AK4531 codec, ES1371, ES1373, Creative CT5880 and Ectiva EV1938 with an AC'97 codec. The Sound Blaster Live! (EMU10K1) is also supported. It has not been tried on a real card yet.
* A software mixer with 32 streams for programs and DirectSound hardware buffers, plus the MIDI synthesizer.
* Any PCM format. 8, 16, 24 and 32 bit, 32-bit float, 1 to 8 channels, and 2 to 192 kHz. The sample rate converter is a polyphase sinc design with no drift.
* Recording from the microphone, line in, CD and aux, and stereo mix when the codec has it. The recording level is adjustable.
* A General MIDI synthesizer with 32 voices, two-operator FM, 128 instruments and a drum kit on channel 10. It supports pitch bend, modulation, sustain pedal, RPN bend range and GM, GS and XG reset.
* WINSOUND.DRV for Windows. Wave output and input, MIDI output and aux volume. It works with Windows 3.1 and Windows 3.0 with the Multimedia Extensions, in Standard and 386 Enhanced Mode. On Windows 95, 98 and ME it also provides a mixer driver for Volume Control and the taskbar volume icon. Volumes are kept across restarts.
* Sound Blaster 16 emulation for DOS programs. DSP 4.05 with 8-bit and 16-bit DMA, the CT1745 mixer, an OPL3 FM synthesizer and an MPU-401 UART on the General MIDI synthesizer. The interrupt is on IRQ 5 or 7 and DMA is 1 or 5. It works in the MS-DOS boxes of Windows 3.x, 95, 98 and ME, and in plain DOS for real-mode and protected-mode games. WINAUDIO.SYS brings its own V86 monitor, VCPI server and DPMI host, so nothing else needs to be loaded. QEMM and JEMM with QPIEMU also work.
* WINMIX.EXE for the mixer and AUDTEST.EXE for testing and diagnosis.
* USB Audio 1.0 and 2.0 devices and USB MIDI through the USB stack of WINUSB. Both packages can be used together.

WINAUDIO is one of the four WINDRV packages. Each package is in its own repository and can be used on its own or with the others.

* WINDISK: IDE, SATA AHCI and NVMe disks, IDE and AHCI CD/DVD drives, SD card readers and eMMC.
* WINUSB: USB host controllers and devices, PS/2 mouse and keyboard, touchpads, USB4 and Thunderbolt.
* WINAUDIO: sound.
* WINUDF: the UDF file system, read and write.

## Supported systems

* MS-DOS 5.0, 6.x, 7.x and 8.0, and FreeDOS: sound through AUDTEST, and Sound Blaster 16 emulation for real-mode and protected-mode (DPMI) games. Nothing else has to be loaded. See Sound Blaster 16 emulation.
* Windows 3.0 Real Mode: beeps and PLAY / SOUND music through WINBEEP.DRV.
* Windows 3.0 Standard and 386 Enhanced Mode: everything, with the Multimedia Extensions. Sound Blaster emulation works in MS-DOS boxes in 386 Enhanced Mode.
* Windows 3.1 and Windows for Workgroups 3.11: everything. Sound Blaster emulation works in MS-DOS boxes.
* Windows 95, 98 and ME: everything as Plug and Play devices: WINAUDIO.VXD for the HD Audio bus and codec devices, the Intel DSP and AC'97. Sound Blaster emulation works in MS-DOS boxes. See Windows 95, 98 and ME.

## Requirements

* A Pentium or later processor with a time stamp counter. SoCs that have one but do not report it through CPUID, such as the Vortex86 DX, SX and EX, are supported.
* MS-DOS 5.0 to 8.0 or FreeDOS, and HIMEM.SYS or another XMS driver such as JEMMEX or HIMEMX. EMM386 and JEMM386 are optional. See Sound Blaster 16 emulation for how they affect the emulation. The DOS driver can be loaded high with DEVICEHIGH or with a loader such as DEVLOAD.
* Windows 3.1, Windows for Workgroups 3.11, Windows 3.0 with the Multimedia Extensions (MMSYSTEM.DLL), or Windows 95, 98 or ME.
* An HD Audio or AC'97 controller on PCI, or an Intel audio DSP. See Intel audio DSP.
* A PCI bus.

## Files

* `WINAUDIO.SYS` (DOS): the DOS character device driver `WINAUD$`, loaded from CONFIG.SYS.
* `WINACORE.BIN` (DOS): the 32-bit driver core for DOS and Standard Mode. It must be in the same directory as WINAUDIO.SYS.
* `AUDTEST.EXE` (DOS and WIN9X): the device list, test tone, WAV player and recorder, MIDI test, mixer and log.
* `INSTAUD.EXE` (DOS): the installer.
* `UNINAUD.EXE` (DOS): the uninstaller.
* `WINDRVFW\` (WIN9X): firmware for the Intel audio DSPs and SoundWire codecs. See FIRMWARE.TXT and Intel audio DSP. The installer also copies it next to WINAUDIO.SYS.
* `README.TXT` (DOS): the short README in code page 437.
* `MANUAL.TXT` (DOS): this manual in code page 437.
* `WINAUDIO.386` (WIN3X): the 386 Enhanced Mode VxD. It goes in the directory of WINAUDIO.SYS and is loaded automatically.
* `WINSOUND.DRV` (WIN3X and WIN9X): the MMSYSTEM driver for wave, MIDI, aux and mixer. It goes in `WINDOWS\SYSTEM`.
* `WINMIX.EXE` (WIN3X and WIN9X): the volume levels and recording source.
* `WINBEEP.DRV` (WIN3X): the Windows 3.0 SOUND driver. It plays beeps and the `PLAY` / `SOUND` API in Real Mode too.
* `WIN30FIX.COM` (WIN3X): the XMS fix for Windows 3.0 Standard Mode with more than 16 MB of memory.
* `WINGRP.EXE` (WIN3X and WIN9X): creates the Program Manager or Start menu group at the next start of Windows.
* `WINAUDIO.TXT` (WIN3X and WIN9X): this manual for Notepad, as the Read Me item of the group.
* `WINAUDIO.VXD` (WIN9X): the Windows 95/98/ME Plug and Play VxD. It is a dynamic VxD and the HD Audio bus driver and codec enumerator.
* `WINAUDIO.INF` (WIN9X): HD Audio controllers (bus) and Intel audio DSPs (System devices).
* `WINAUDM.INF` (WIN9X): HD Audio codec functions, DSP endpoints and AC'97 (Sound, video and game controllers).
* `WIN4G.VXD` (WIN9X): moves PCI resources above 4 GB below 4 GB at boot. See PCI resources above 4 GB.

## Repository layout

* `bin\DOS`: the DOS driver, its 32-bit core, the tools, the installer and the uninstaller. All Windows 1.0x and 2.x files are here too.
* `bin\WIN3X`: the Windows 3.0, 3.1 and Windows for Workgroups 3.11 files.
* `bin\WIN9X`: the Windows 95, 98 and ME Plug and Play drivers and their INF files. Give this folder to Have Disk or Update Driver.
* `src`: the source code. See Building.

Files used by several systems are in each folder. Run the installer from `bin\DOS`. It takes the files for the other systems from the folders next to it, so keep the `bin` tree together. Copy the whole tree to a floppy or a directory.

* `bin\DOS`: AUDTEST.EXE, INSTAUD.EXE, MANUAL.TXT, README.TXT, UNINAUD.EXE, WINACORE.BIN, WINAUDIO.SYS.
* `bin\WIN3X`: WIN30FIX.COM, WINAUDIO.386, WINAUDIO.TXT, WINBEEP.DRV, WINGRP.EXE, WINMIX.EXE, WINSOUND.DRV.
* `bin\WIN9X`: AUDTEST.EXE, WIN4G.VXD, WINAUDIO.INF, WINAUDIO.TXT, WINAUDIO.VXD, WINAUDM.INF, WINDRVFW, WINGRP.EXE, WINMIX.EXE, WINSOUND.DRV.

## Installation

Run the installer from DOS, not from inside Windows 3.x, and start in `bin\DOS`.

The default target is the Windows directory. INSTAUD finds it from WIN.COM on the PATH, uses the directory given with `/W:`, or falls back to `C:\WINDOWS`.

```text
INSTAUD                      installs into the Windows directory
INSTAUD D:\DRV\WIN /W:D:\WIN  another directory and Windows directory
INSTAUD /NOWIN               the DOS driver only (WINSOUND.DRV is not set up)
INSTAUD /DOSONLY             the DOS driver only, never in Windows (Windows keeps its own
                             driver for the card; WINAUDIO.SYS gets /NOVXD)
INSTAUD /O /WIN              with driver options (everything after /O goes to WINAUDIO.SYS)
INSTAUD /DOS                 Windows 95/98 installed: adds WINAUDIO.SYS to CONFIG.SYS too
INSTAUD /ALL                 set up every Windows installation without asking
INSTAUD /O /SB:A220,I7,D1,H5 the emulated Sound Blaster on IRQ 7 (and SET BLASTER to match)
```

INSTAUD finds every Windows 3.1, Windows for Workgroups 3.11, Windows 3.0 with the Multimedia Extensions, and Windows 95, 98 or ME installation on the machine. `/W:dir` selects one. It does these things:

* It copies the files. The DSP firmware goes into the `WINDRVFW` subdirectory of the target. It adds `DEVICE=C:\WINDOWS\WINAUDIO.SYS` to CONFIG.SYS after HIMEM.SYS or EMM386, in every block of a menu CONFIG.SYS that loads HIMEM.
* It copies `WINSOUND.DRV` to `WINDOWS\SYSTEM` and sets `wave=winsound.drv`, `midi=winsound.drv` and `aux=winsound.drv` in the `[drivers]` section of SYSTEM.INI. A driver already in one of these keys, such as `sndblst.drv`, moves to the first free `wave1=`, `midi1=` and so on. It keeps working, and the WIN devices become the first, default ones. A description goes into `[drivers.desc]`, which Control Panel shows under Drivers.
* It points the General MIDI setup of the MIDI Mapper in `WINDOWS\SYSTEM\MIDIMAP.CFG` at WIN GM Synth for all 16 channels and makes it current. Media Player and every program that plays MIDI through the MIDI Mapper then use the synthesizer.
* On Windows 3.0 it installs `WINBEEP.DRV` and sets `[boot] sound.drv=winbeep.drv`. The old driver is kept. Windows 3.0's MMSYSTEM.DLL cannot load in Real Mode. WINBEEP plays the beeps and the `PLAY` / `SOUND` API (SetVoiceNote, StartSound and so on) on the synthesizer. In Standard and Enhanced Mode, system sounds go to MMSYSTEM through `sndPlaySound`, as MMSOUND.DRV does it.
* It puts `SET BLASTER=A220 I5 D1 H5 P330 T6` at the top of AUTOEXEC.BAT when there is no BLASTER line. These are the Sound Blaster emulation settings for DOS programs, and the MS-DOS boxes of Windows inherit them. `/O /NOSB` leaves the line out. `/O /SB:...` changes the values.
* It adds the WINAUDIO group to Program Manager on Windows 3.x and to the Start menu on Windows 95/98/ME. The group has Volume Control, Sound Test and Read Me. Installing again does not duplicate lines or group items.
* Windows 3.x also get an Uninstall WINAUDIO item, with the package's icon and a red cross. The uninstaller is a DOS program, so Windows closes, runs `UNINAUD.EXE /REBOOT`, and the computer restarts when it is done. Windows 95/98/ME also get the programs as shortcuts on the desktop in `WINDOWS\DESKTOP`. The uninstaller removes them with the programs.

The original of every changed file is saved once: `CONFIG.WNA`, `SYSTEM.WNA` and `MIDIMAP.WNS`. Running the installer again is safe. The old options are kept unless `/O` is given.

### Uninstall

`C:\WINDOWS\UNINAUD` removes the CONFIG.SYS line, puts the `[drivers]` keys and the moved drivers back, restores MIDIMAP.CFG, removes the group, WINSOUND.DRV and the package files, and removes the desktop shortcuts of Windows 95/98/ME. `/K` keeps the files. `/W:dir` names the Windows directory.

The package files are removed only from the directory that CONFIG.SYS loads the driver from. A Windows 95/98/ME installation without `/DOS` has no such line, so its directory stays and UNINAUD says which one it left. Delete it by hand. If you start UNINAUD in `bin\DOS` without a CONFIG.SYS line, it leaves the package itself alone.

On Windows 95/98/ME the sound device keeps WINAUDIO's driver after the uninstall. Give it the card's own driver back in Device Manager first, as described in Windows 95, 98 and ME. Or select the device and Remove it, restart, and Windows installs the driver it had again. UNINAUD says so when it runs.

With WINUSB installed too, WINSOUND.DRV stays for the USB sound devices, and the MIDI Mapper is pointed at WIN USB GM Synth.

`/REBOOT` waits for a key and restarts the computer at the end. The Uninstall item of Windows 3.x uses it.

### By hand

1. Copy `bin\DOS` to a directory, for example `C:\WINDOWS`. Keep `WINACORE.BIN` next to WINAUDIO.SYS.
2. Add this line to CONFIG.SYS, after HIMEM.SYS:

   ```text
   DEVICE=C:\WINDOWS\WINAUDIO.SYS
   ```

   For Windows 3.x 386 Enhanced Mode, copy `bin\WIN3X\WINAUDIO.386` next to WINAUDIO.SYS. It is loaded automatically.

3. Copy `WINSOUND.DRV` to `C:\WINDOWS\SYSTEM`. Add this to SYSTEM.INI:

   ```ini
   [drivers]
   wave=winsound.drv
   midi=winsound.drv
   aux=winsound.drv
   ```

   You can also use Control Panel, Drivers, Add, Unlisted or Updated Driver.

4. Open Control Panel, MIDI Mapper, Edit, and route the channels to the WIN GM Synth port.

5. For DOS programs that use a Sound Blaster, add `SET BLASTER=A220 I5 D1 H5 P330 T6` to AUTOEXEC.BAT. See Sound Blaster 16 emulation.

Without WINAUDIO.SYS, which means no CONFIG.SYS line or `/NOVXD`, Windows 3.x 386 Enhanced Mode can load the VxD by itself. Copy `WINAUDIO.386` to `WINDOWS\SYSTEM` and add `device=winaudio.386` to the `[386Enh]` section of SYSTEM.INI. Options then go to `Options=` in a `[WINAUDIO]` section of SYSTEM.INI, for example `Options=/SB:A220,I7`. Sound then works in Windows only. There is no DOS sound and no AUDTEST in DOS.

### WINAUDIO.SYS options

General options:

* `/WIN`: run the core only in Windows 386 Enhanced Mode. There is no sound in DOS or Standard Mode.
* `/S`: send the driver log to the screen at boot too.
* `/Q`: quiet. Only error messages appear on the screen at boot.
* `/V`, `/D`, `/COM1`: log options. `/D` sends the debug log to port E9h or COM1.

HD Audio options:

* `/NOHDA`: do not use HD Audio controllers.
* `/IMM`: use the immediate command interface instead of CORB and RIRB.
* `/POSBUF`: use the DMA position buffer instead of LPIB.
* `/GPIO`: use codec GPIO 0 to 3 as outputs, high. This is for notebooks whose external speaker amplifier is switched by GPIO.
* `/DSP`: on a machine with an Intel DSP, drive the HD Audio codec through the DSP too. By default it is driven directly.
* `/NODSP`: never start the Intel audio DSP. Only HD Audio codecs are used.
* `/DMIC`: try the digital microphones with the IPC3 firmware on a machine without an NHLT table, such as a Chromebook.

AC'97 options:

* `/NOAC97`: do not use AC'97 controllers.
* `/NOGEN`: do not try unknown 04/01 devices as ICH-compatible AC'97.
* `/EAPD`: turn the external amplifier on with EAPD = 1. Some notebooks wire it the other way.
* `/NOTPVOL`: on an IBM or Lenovo ThinkPad, leave the volume and mute buttons' embedded controller setting alone. By default it is unmuted and turned up at start.

Synthesizer and VxD options:

* `/NOSYNTH`: no MIDI synthesizer.
* `/NOVXD`: do not load the VxD in Enhanced Mode.

PCI options:

* `/NORELOC`: do not move PCI resources from above 4 GB at boot.
* `/ALL4G`: move every device below 4 GB at boot, not only the WINDRV devices. See PCI resources above 4 GB.

Sound Blaster options:

* `/USBSB`: the Sound Blaster emulation and the synthesizer play on the USB Audio device of WINUSB even when WINAUDIO has a sound device of its own. See Sound Blaster 16 emulation.
* `/NOSB`: no Sound Blaster emulation.
* `/SB:A220,I5,D1,H5,P330`: the Sound Blaster settings. The port is 220h to 280h, the IRQ is 2, 5, 7 or 10, the 8-bit DMA channel is 0, 1 or 3, the 16-bit DMA channel is 5, 6 or 7 (0 means the 8-bit one), and the MPU-401 port is 300h to 330h (P0 means none). Missing letters keep their default.
* `/NOOPL`: no FM synthesizer ports at 388h to 38Bh, base+0 to 3, and base+8 and 9.
* `/NOMPU`: no MPU-401.

V86 monitor options:

* `/NOV86`: in plain DOS, do not start the built-in V86 monitor. There is no emulation in DOS then. See Sound Blaster 16 emulation.
* `/NODPMI`: start the V86 monitor without its DPMI host. VCPI is then available to every program.
* `/NOVCPI`: start the V86 monitor without its VCPI server.
* `/VCPI`: VCPI for every program, not only for WINDRV drivers and Windows Standard Mode.
* `/VME`: use the virtual-8086 mode extensions of the processor. Software interrupts of real-mode programs go straight to their handlers, which is a little faster. It is off by default because some early Ryzen processors without a microcode update have a VME fault.
* `/EMS:n`: the V86 monitor gives at most n MB of expanded memory. It is LIM EMS 4.0 and the default is 16 MB.
* `/NOEMS`: the V86 monitor gives no expanded memory.

## Windows 95, 98 and ME

On Windows 95, 98 and ME, WINAUDIO is a Plug and Play driver. There is no CONFIG.SYS line.

### Installation

Run `INSTAUD` from `bin\DOS` in a DOS box. It copies `WINAUDIO.VXD`, `WIN4G.VXD`, `WINSOUND.DRV`, `WINMIX.EXE` and `AUDTEST.EXE` to `WINDOWS\SYSTEM`, the DSP firmware to `WINDOWS\SYSTEM\WINDRVFW`, and `WINAUDIO.INF` and `WINAUDM.INF` to `WINDOWS\INF`. The INF files are copied without their `CopyFiles` lines because the files are already in place. It writes `wave=winsound.drv`, `midi=winsound.drv` and `mixer=winsound.drv` to the `[drivers]` section of SYSTEM.INI.

That instance of the driver has the USB Audio devices of WINUSB. WINAUDIO's own sound devices bring WINSOUND.DRV with their devnode, as described below.

Then select the sound controller in Device Manager:

* Windows 95: Remove the "PCI Multimedia Audio Device" under Other devices and choose Refresh. Windows finds it as new hardware and installs the default driver.
* Windows 98 and ME: for a sound device with Microsoft's driver, choose Driver, Update Driver, Have Disk, and `C:\WINDOWS\INF` or `bin\WIN9X` of this package.

A controller Windows has not seen before is offered by WINAUDIO at boot.

### Going back to the card's own driver

Choosing `WINAUDM.INF` for the sound device changes only which driver that device uses. The driver the card had stays installed, with its files and settings. You can give it back without a snapshot and without a disk.

1. Open Device Manager and expand "Sound, video and game controllers".
2. Select the sound device. Its name starts with "WINAUDIO" while WINAUDIO drives it.
3. Open Properties, Driver.
4. Choose Change Driver. On Windows 98 and ME, choose Update Driver and "Display a list of all the drivers in a specific location".
5. Select the card's own entry, such as "Creative Sound Blaster AudioPCI" or "Ensoniq AudioPCI".
6. Choose OK and restart Windows.

Its wave, MIDI and mixer devices come back with it because they belong to the same devnode. This is the way back from a driver that plays no sound, and it works while WINAUDIO is installed. Make the switch first, then run UNINAUD.

A card that had no driver of its own was shown as "PCI Multimedia Audio Device" under Other devices. It has nothing to go back to. Select it and Remove, restart, and Windows finds it as a new device. Do this after UNINAUD, which removes WINAUDM.INF from `WINDOWS\INF`. Windows will not offer WINAUDIO's driver again.

VMware's ES1371 in a Windows 95 guest is the card this is for. The AudioPCI driver it had, Creative's, is the entry to choose.

### If the card's own driver is not listed

The list holds the drivers whose INF files Windows has. A driver installed by its own setup program may have left none. On Windows 95 the INFs Windows copied to `WINDOWS\INF` are renamed `OEMxx.INF`. Windows 98 and ME keep the file's name, in `WINDOWS\INF` or `WINDOWS\INF\OTHER`.

Try Have Disk with `C:\WINDOWS\INF` first. Then run the card's own setup program again. It installs the driver and takes the card back from WINAUDIO. You can also use Have Disk with the driver disk.

For VMware's ES1371, the driver is Creative's "Sound Blaster Ensoniq AudioPCI". The Windows 95 and 98 setup is `epw9xup.exe`. Creative's download search finds it under Ensoniq AudioPCI. The Windows 95 Sound Blaster PCI 128 setup, `SBPCI128Setupus_w9x.exe`, uses the same chip. The WDM package `SBPCI_WebDrvsV5_12_01.exe` is for Windows 98 SE and later. Copies of both are on the Internet Archive under "Windows 9x sound drivers for VMware Fusion".

A machine that cannot get the AudioPCI driver back at all can still have sound. Power the virtual machine off, set `sound.virtualDev = "sb16"`, start it, and let Windows install its own Sound Blaster 16 driver. WINAUDIO drives that card too.

### No sound, but the card is listed

WINMIX shows the levels the driver keeps, not the codec's registers. Its Mute box shows the master mute, and Volume Control and the taskbar icon set the same one. A saved mute comes back at every start.

`WINMIX /DUMP C:\WINAUDIO.TXT` writes the devices, every level, `mute: ON` or `off`, and the driver log to a text file. A screen reader can read it. `WINMIX /UNMUTE` clears the mute and sets the master to 100%.

`WINMIX /TONE` plays two seconds of 440 Hz through the driver itself, not through WINSOUND.DRV and MMSYSTEM. If the tone plays but a WAV file does not, the wave path is the problem. If the tone does not play, the card or the driver is not working.

`AUDTEST` in a Windows 95/98/ME DOS box cannot reach the VxD. It says `WINAUDIO.SYS is not loaded` because it talks to the DOS driver. `WINMIX /DUMP` is the way to the VxD's log there.

### HD Audio bus

The HD Audio controller and the Intel audio DSP are a bus controller in the System devices class. Their INF is `WINAUDIO.INF` and their `DevLoader` is `WINAUDIO.VXD`. Device Manager names them "WINAUDIO Intel High Definition Audio Bus Controller" with the vendor name (AMD, NVIDIA, VIA, Zhaoxin and so on), "WINAUDIO Intel Smart Sound Technology Audio DSP (HD Audio Bus)", "WINAUDIO Intel SST Audio DSP (I2S Audio Bus)" and similar names.

The VxD starts the controller, scans the codecs on the link, and creates a child device under the controller for every codec function:

* `WINAUDIO\HDAUDIO&FUNC_01&VEN_10EC&DEV_0892&SUBSYS_...&REV_...` with the compatible ID `WINAUDIO\HDA_AUDIO`: the codec's own name, such as "Realtek ALC892 High Definition Audio".
* `WINAUDIO\HDA_HDMI`: "Intel HDMI / DisplayPort Audio (no WINAUDIO sound device: HDMI)". HDMI and DisplayPort output is not supported, so this child offers no sound device and its name says so.
* `WINAUDIO\HDA_MODEM` and `WINAUDIO\HDA_OTHER`: HD Audio modem and other functions.
* `WINAUDIO\DSP_DMIC`, `WINAUDIO\DSP_I2S`, `WINAUDIO\DSP_SDW` and `WINAUDIO\DSP_HDA`: the Intel DSP's digital microphones, I2S codecs and amplifiers, SoundWire codecs, and HD Audio codecs behind the DSP.

`WINAUDM.INF` installs the children. They have no resources because the sound comes out of the controller's driver. AC'97 controllers are directly in that class as "WINAUDIO AC'97 Audio Controller".

A machine with several HD Audio controllers, such as the mainboard sound and the graphics card's HDMI sound, shows each with its own bus and codecs. Two items called "Intel HDA" and "HDA" where one has Code 2, or a working item that offers no sound device, cannot happen with this structure.

### Sound devices and WINSOUND.DRV

The devnodes of the sound devices (AC'97 controllers, and the codec and DSP children) have `MMDEVLDR.VXD` as their `DevLoader`, with `WINAUDIO.VXD` as its driver. They list `WINSOUND.DRV` for wave, MIDI, mixer and aux under `HKR\Drivers`, as Windows' own sound drivers do.

MMSYSTEM loads WINSOUND.DRV once for every such devnode. That instance has the devices on its devnode. The instance from SYSTEM.INI `[drivers]` has the others. This is how DirectSound learns which wave device belongs to which DirectSound driver.

A sound device installed with WINAUDIO 1.1, where `DevLoader=WINAUDIO.VXD`, keeps working through the SYSTEM.INI instance. Update Driver with Have Disk and `WINDOWS\INF` moves it to the new layout.

### DirectSound

WINAUDIO.VXD registers a DirectSound driver for every sound device with an output. DSOUND.VXD is loaded the first time. Without DirectX, nothing happens.

The primary buffer is 64 KB, 8 or 16 bit, mono or stereo, and it is a hardware buffer. DirectSound's kernel mixer mixes the programs' software buffers into it ahead of the write cursor. The sound engine plays it round and round as a card plays its DMA buffer, through the device's wave level. The write cursor stands 40 ms after what the engine has taken. The engine takes the buffer in steps of its service, and what it has taken becomes silence. A part the mixer has not written again in time is a short gap, never old sound.

Secondary buffers can be in hardware: up to 16 per device, as reported by `dwMaxHwMixingAllBuffers`, and fewer when the engine's streams run short. They are 8 or 16 bit, mono or stereo, 100 Hz to 100 kHz, in locked memory, 8 MB for all of them. The sound engine mixes them like the wave streams. Each has its own rate (`SetFrequency`), volume and pan (`SetVolumePan`), and position (`SetPosition`). A buffer plays once to its end, where its position becomes its size and DirectSound stops it, or it loops. `DuplicateSoundBuffer` gives a buffer on the same memory.

3D buffers and buffers beyond the hardware ones are mixed by DirectSound in software. `DirectSoundEnumerate` lists the driver as "<device> (WINAUDIO)" with module WINAUDIO.VXD. `DirectSoundGetCaps` shows no `DSCAPS_EMULDRIVER`, and the default device (`DirectSoundCreate(NULL)`) uses it. The USB Audio devices of WINUSB have the same driver in WINUSB.VXD and are listed as "<device> (WINUSB)".

Verified in QEMU with Windows 98 and its DirectX 5.2, on AC'97, the Ensoniq ES1370 and USB Audio: a `DSBCAPS_LOCHARDWARE` buffer played once and looping, at another frequency, with volume 6 dB down and pan with the right channel 30 dB down, moved while playing; a duplicate together with its original at another rate, 440 and 660 Hz; an 8-bit stereo buffer. The output had clean tones, and `GetStatus` stopped at the end of a buffer played once.

### Matching IDs and options

The INF files match these IDs:

* `PCI\CC_0403`: HD Audio.
* The device IDs of the Intel DSPs.
* `PCI\VEN_8086&CC_0480`: Skylake to Kaby Lake in DSP mode.
* `ACPI\80860F28`, `ACPI\808622A8` and `ACPI\INT3438`: Bay Trail, Cherry Trail and Broadwell SST.
* `PCI\CC_0401`: AC'97.

The INF copies the firmware files from the package's `WINDRVFW` subdirectory to `WINDOWS\SYSTEM\WINDRVFW`.

Options are the `Options` value of the controller devnode. When it is empty, WINAUDIO reads `Options=` from the `[WINAUDIO]` section of SYSTEM.INI. That is read at boot only.

When Windows ME offers its own Intel AC'97 driver, choose "Specify the location", then "Display a list", then Have Disk, then WINAUDIO.

Verified in QEMU: Windows 95 RTM with two HD Audio controllers, ICH6 and ICH9, with two codecs on a clean installation, while Windows runs and after a restart. Two bus controllers appeared under System devices and two codecs under Sound, video and game controllers, with no error marks, and wave playback worked. A 32-bit program played for 20 seconds from its buffers while another 32-bit process ran in between, with context switches and no glitches. AC'97 wave playback, including the startup sound, worked on Windows 95 RTM, Windows 98 FE and Windows ME.

## Sound Blaster 16 emulation

DOS games and programs that know only a Sound Blaster get one. The emulated card answers on its ports like a Creative Sound Blaster 16. Everything it plays goes through the sound engine to the HD Audio or AC'97 device, or to a USB Audio device of WINUSB, mixed with Windows' own sound.

### What the emulated card has

* DSP 4.05 at 220h to 22Fh (`A`): reset, version, identification, copyright, test register, speaker, direct DAC (10h), 8-bit DMA (14h, 1Ch, 90h, 91h and Cxh), 16-bit DMA (Bxh), high speed, auto-init, pause, continue, exit auto-init, time constant (40h), sample rate (41h and 42h), silence (80h), forced interrupts (F2h and F3h), recording that returns silence, ADPCM transfers that keep their timing and play as silence, and MIDI out (38h, UART modes).
* Mixer CT1745 at 224h and 225h: master, voice, FM, CD, line and mic levels, the SB Pro registers too; IRQ and DMA setting registers 80h and 81h; interrupt status 82h; the SB Pro stereo bit.
* OPL3 FM at 388h to 38Bh, 220h to 223h, and 228h and 229h: a YMF262 synthesizer of our own with 2 and 4 operators, 8 waveforms, rhythm mode, tremolo, vibrato, key scale, feedback and stereo. It is OPL2 compatible and has the timers, so the AdLib detection of games works.
* MPU-401 UART at 330h and 331h (`P`): reset and UART mode with acknowledge. The MIDI bytes play on the General MIDI synthesizer.
* DMA on channel 1 (`D`) and channel 5 (`H`): address, count, page, mode, mask, flip-flop and status as the program programs them. The counters move with the playback.
* IRQ 5 (`I`) or 7: raised at the end of each block and acknowledged by reading 22Eh or 22Fh.

The defaults are `A220 I5 D1 H5 P330`, the usual BLASTER setting of a Sound Blaster 16: `SET BLASTER=A220 I5 D1 H5 P330 T6`. INSTAUD writes it to AUTOEXEC.BAT. `/SB:` changes the settings, for example `/SB:I7` for games that only know IRQ 7. Keep BLASTER in step.

### Windows 3.x, 95, 98 and ME

The VxD traps the card's ports in every virtual machine. It takes over the two DMA channels from VDMAD and the IRQ from VPICD. Every MS-DOS box has the card, and the box that used it last owns it. Nothing has to be loaded in the box. Programs running in protected mode under Windows' DPMI, such as DOS/4GW, DOS32A and PMODE, go through the same traps. This was verified with real-mode test programs only.

### Plain DOS

Nothing else is needed. When DOS runs in real mode with HIMEM.SYS or HIMEMX only and no EMM386, WINAUDIO.SYS starts a small V86 monitor of its own. DOS goes on in virtual 8086 mode, and the ports of the card, the FM synthesizer, the MPU-401 and the two DMA channels are trapped by the processor's I/O permission bitmap.

```text
DEVICE=C:\DOS\HIMEM.SYS
DEVICE=C:\WINDOWS\WINAUDIO.SYS
```

```text
WINAUDIO: Sound Blaster 16 emulation, SET BLASTER=A220 I5 D1 H5 P330 T6
WINAUDIO: DOS programs: ports trapped by the built-in V86 monitor, VCPI and DPMI
WINAUDIO:   available, RTC interrupt 1024 Hz
```

### The V86 monitor

The monitor is there for every kind of DOS program.

* Real-mode programs run in V86 mode with their ports trapped. HIMEM keeps working: the monitor does the INT 15h AH=87h block moves and emulates the A20 gate through the page tables, using port 92h and the keyboard controller. "A20 off" wraps memory like a real PC.
* Protected-mode programs get a DPMI 0.9 host at INT 2Fh AX=1687h. DOS/4GW, DOS/32A, PMODE/W, CauseWay, DJGPP programs and Borland's 16-bit DPMI programs run as its clients in ring 3, with the same ports trapped. Games such as DOOM, Duke Nukem 3D, Descent and Warcraft II have the card too.
* The DPMI host gives memory from XMS (functions 0501h to 0503h), DOS memory (0100h to 0102h), descriptors, real-mode calls and callbacks, raw mode switches and exception handlers. It routes hardware interrupts, the card's too, to the program's protected-mode handlers in either mode. A DPMI program may start another one.
* 16-bit clients get the MS-DOS extensions of Windows' DPMI. INT 21h calls with pointers take protected-mode pointers for file names, reads and writes, find first and next with the DTA, IOCTL, EXEC and long file names. The host copies the data through a buffer in conventional memory. The PSP, the DTA and DOS memory blocks are selectors. INT 21h 25h and 35h set and get the protected-mode vectors. INT 2Fh AX=1600h answers "Windows 3.1 enhanced mode", which Borland's RTM asks for before it uses them. Borland Pascal and C++ programs in protected mode, such as Jazz Jackrabbit and Tyrian 2000, run with the card too.
* VCPI is served at INT 67h AX=DExxh with the device EMMQXXX0, like EMM386 NOEMS. It is for WINDISK.SYS and WINUSB.SYS and for Windows 3.x Standard Mode. DOS extenders that prefer VCPI to DPMI, such as DOS/32A, PMODE/W and CauseWay, would run in ring 0 under it, where ports cannot be trapped. With the DPMI host, VCPI is hidden from other programs. `/VCPI` shows it to all.
* Windows 3.x 386 Enhanced Mode and Windows 95, 98 and ME get the processor back in real mode through the switch routine of INT 2Fh AX=1605h, as with EMM386. After Windows, the monitor runs again. Windows 3.x Standard Mode runs as a VCPI client.

On Windows 95, 98 and ME, IO.SYS loads HIMEM.SYS only after the CONFIG.SYS drivers. `DEVICE=C:\WINDOWS\HIMEM.SYS` must come before WINAUDIO.SYS, and INSTAUD adds it. The monitor then runs in "Restart in MS-DOS mode" sessions too and hands the processor to Windows when it starts.

The card is served between timer ticks from the RTC periodic interrupt, IRQ 8 at 1024 Hz. Its interrupt reaches the program the way the PIC would deliver it: only while the program has interrupts enabled and the IRQ unmasked, with the program's EOI and in-service reads answered for it.

### Expanded memory

Programs that need EMS get it from the monitor itself, so EMM386 is not needed for them. It is LIM EMS 4.0 at INT 67h AH=40h to 5Ah, with the device `EMMXXXX0`. The 64 KB page frame is in a free part of the upper memory area, at D000h, E000h and so on, where no option ROM is and every byte is the same. Logical pages come from XMS and are allocated when a program asks. `/EMS:n` sets at most n MB, with a default of 16. `/NOEMS` turns it off. When Windows 386 Enhanced Mode starts, the pages go back because Windows gives its MS-DOS boxes its own EMS. There are no upper memory blocks.

When the program that last wrote to the emulated card ends at INT 21h AH=00h or 4Ch, the card is reset. A game that exits with its sound still running in auto-init DMA leaves no buzzing.

### Memory managers

A memory manager in V86 mode loaded before WINAUDIO.SYS takes precedence. QEMM 7.03 or later, 386 or 97, or JEMM386 or JEMMEX with its QPIEMU module, trap the ports for WINAUDIO.SYS instead. That is for real-mode programs only. EMM386 and QEMM up to 7.02 have a QPI without port trapping, so DOS has no emulation and WINAUDIO.SYS says so.

QEMM 7.5 has a trap callback of its own. It traps the DMA mode and mask registers, 0Ah and 0Bh, and D4h and D6h, for its DMA translation. WINAUDIO.SYS chains to its callback, leaves those registers to QEMM because floppy transfers need them there, and takes the emulated card's channel as auto-init and unmasked.

With JEMM:

```text
DEVICE=C:\JEMM\JEMMEX.EXE            (or HIMEM.SYS + JEMM386.EXE)
DEVICE=C:\JEMM\JLOAD.EXE C:\JEMM\QPIEMU.DLL
DEVICE=C:\WINDOWS\WINAUDIO.SYS
```

Programs that need real mode itself cannot run in V86 mode. This includes programs that switch to protected mode on their own without VCPI or DPMI, and "unreal mode" programs such as Ultima VII. The monitor stops them with a message. Load WINAUDIO.SYS with `/NOV86` for them; they run without the emulation, as under any memory manager.

`AUDTEST` shows the settings and the state. `AUDTEST SB` tries the card the way a game does: DSP, interrupt, DMA sound, FM detection and MPU-401.

### USB Audio

With WINUSB, the emulation also plays on a USB Audio 1.0 or 2.0 device that WINUSB drives. This works in plain DOS, in the MS-DOS boxes of Windows 3.x, and in Windows 95, 98 and ME. WINAUDIO mixes the card (DSP, FM and MPU-401 on the synthesizer) into a ring in its memory. WINUSB plays that ring on its first USB Audio output like any other sound. The card keeps the USB device's pace. The bridge is used when WINAUDIO has no HD Audio, AC'97 or DSP device of its own, or always with `/USBSB`.

* DOS: load WINUSB.SYS with `/USB` before WINAUDIO.SYS. The installers put the lines in this order. WINAUDIO.SYS waits up to 3 seconds for WINUSB to find a USB Audio output, because devices behind hubs come late. It stays resident even without sound hardware of its own. A USB Audio device plugged in later is taken too. `/USB` belongs to WINUSB.SYS. `/USBSB` is the WINAUDIO.SYS switch that sends the emulation to USB while an HD Audio or AC'97 device is present. When the bridge is not there, the start message says why: WINUSB.SYS was not loaded before it, it was loaded without `/USB`, or no USB Audio output was found.

  ```text
  DEVICE=C:\DOS\HIMEM.SYS
  DEVICE=C:\WINDRV\WINUSB.SYS /USB
  DEVICE=C:\WINDRV\WINAUDIO.SYS
  ```

* Windows 3.x: with both DOS drivers loaded, their VxDs find each other the same way.
* Windows 95, 98 and ME: WINAUDIO.VXD and WINUSB.VXD find each other by themselves. WINAUDIO.VXD is loaded for an HD Audio, AC'97 or DSP device only, so a machine whose only sound is USB has the emulation in plain DOS and Windows 3.x, not in Windows 9x.
* The sound runs about 0.3 to 0.5 seconds behind the program in DOS, because WINUSB schedules the USB transfers 150 ms ahead. It runs about 0.2 seconds behind in Windows. The interrupts come at the right rate.

### Notes

* `AUDTEST SB` measures the RTC interrupt that drives the emulation in DOS, 1024 per second. When it does not arrive, because of HPET legacy replacement mode or a firmware that routes IRQ 8 elsewhere, the emulation runs from the timer tick only and its interrupts come late.
* The sound of the DSP and the FM synthesizer runs about 50 to 100 ms behind the program because the device plays ahead of it. The interrupts come at the right rate.
* The DMA terminal count bit of the 8237 status register is emulated everywhere. In plain DOS the monitor does it. In Windows 95, 98 and ME the VxD answers the status ports 08h and D0h. In Windows 3.x the VxD's fault hooks sit in front of VDMAD's trap of those ports. The other channels' bits come from VDMAD in both.
* With the emulation active under JEMM, the shared 8237 registers of the other DMA channels (mode, mask and flip-flop) are written directly, past the memory manager. Floppy disk DMA into memory that JEMM maps elsewhere, such as UMBs, would go wrong. `/NOSB` turns the emulation off. QEMM 7.5 keeps the mode and mask registers, as described above. The built-in monitor maps memory 1:1 and has no such problem.
* Under QEMM 7.5, a program that leaves the emulation's channel in single-cycle mode and reads its count register after the end sees the count of an auto-init channel because QEMM keeps the mode register. The interrupt comes as usual.
* In QEMU with SeaBIOS, QEMM 7.5 can crash during floppy disk access while the RTC periodic interrupt is on. It is the emulation's 1024 Hz clock under QEMM. A 30-byte program that only turns the RTC interrupt on does the same without WINAUDIO. Real machines were not tried.
* Creative ADPCM transfers keep their timing but play as silence. Recording returns silence.
* The mixer starts at full level, while a real card starts lower. Games that never touch the mixer are not quiet.

## How it works

```text
Windows programs (Media Player, Sound Recorder, games ...)
      | MMSYSTEM
WINSOUND.DRV --- a ring buffer in conventional memory per stream (GlobalDosAlloc)
      |          filled and emptied by a 10 ms multimedia timer
      | requests: protected-mode entry of the VxD (Enhanced), or INT 2Fh D5h through DPMI (Standard)
WINAUDIO.386 (Enhanced Mode) or WINAUDIO.SYS + WINACORE.BIN (DOS / Standard Mode)
      | sound engine: reads the streams, converts the rate, mixes, synthesizes MIDI
HD Audio / AC'97 DMA ring (16-bit stereo at the device rate, 48 kHz)
```

* 386 Enhanced Mode: every 10 ms, in a VMM global event, the VxD fills the hardware ring up to about 70 ms ahead. Latency is low, which is fine for games and MIDI.
* DOS and Standard Mode: the core is polled from the timer interrupt (INT 08h, about 18 Hz) and from DOS idle (INT 28h). No poll can happen while DOS is inside a function or a BIOS disk call. The ring is filled about 400 ms ahead.
* Position: the playback position reported to programs comes from the sample the hardware really plays, with the device's latency subtracted. `waveOutGetPosition` is exact to the sample.
* When Windows starts, the DOS core lets go of the hardware and the VxD takes it over with the same code. When Windows exits, the DOS core takes it back.
* DMA stops when sound is not used for 2 seconds.
* Playback and recording use polling rather than interrupts, so there are no IRQ sharing problems.

## Devices in Windows

* Wave out: `WIN HD Audio vvvv:pppp`, `WIN AC'97 ...` or `WIN <USB product name>`. There is one per sound device.
* Wave in: the same names, for devices that can record.
* MIDI out: `WIN GM Synth`, or `WIN USB GM Synth` with WINUSB, and USB MIDI devices. The synthesizer plays on the first sound device.
* Aux: `WIN Master Volume`, and on AC'97 `WIN CD Audio` and `WIN Line In`. It is set through `auxSetVolume` in Windows 3.x.
* Mixer: the wave out name, on Windows 95, 98 and ME.

`waveOutSetVolume` sets the device's wave level. `midiOutSetVolume` sets the synthesizer level.

### Mixer on Windows 95, 98 and ME

There is one mixer per sound device, for Volume Control (SNDVOL32), the taskbar volume icon (SYSTRAY; Control Panel, Multimedia, "Show volume control on the taskbar") and every program that uses the mixer API.

* Volume Control (speakers): master volume and mute.
* Wave: the wave level, the same value as `waveOutSetVolume`.
* SW Synth: the synthesizer level, the same value as `midiOutSetVolume`.
* CD Audio and Line In (AC'97): the analog input levels.
* Recording Control: the recording source (microphone, line in, CD, aux, stereo mix, as the device has them). Each source shows the recording level; there is one level for all.

Changes made through any of these, or through WINMIX, are told to the open mixers. Volume Control and the taskbar icon follow other programs.

### Volume scale

Windows volumes are linear in amplitude. Half the value is half the amplitude, which is 6 dB down. The hardware amplifiers (HD Audio, AC'97, USB Audio and the codecs of the Intel DSP) work in dB steps. The value is converted to dB, so a slider moves the loudness evenly and the same value gives the same loudness as in the software mix.

### Saved volumes

WINSOUND.DRV writes the volumes of each device to `Volume<core>.<device>=` in the `[winsound.drv]` section of SYSTEM.INI when they change. It sets them again when Windows starts. The values are master, wave, synthesizer, recording, CD, line, recording source and mute. WINMIX writes the same keys.

## Synthesizer

The synthesizer has 32 voices of two-operator FM: a modulator and a carrier, with feedback. The 128 General MIDI instruments have separate ADSR envelopes and modulation depths. Winds and effects use noise. Channel 10 has a 47-note drum kit with bass drum, snares, toms, cymbals, hi-hats and hand drums. When the voices run out, the quietest finishing voice is taken, or the oldest if none is finishing.

To save processor time, it is generated at half the rate, 24 kHz, and converted up. The level follows the pitch: low notes are a little louder, and high notes are 1.5 dB per octave softer, like the key scaling of an OPL. The instruments go through a gentle low pass at about 6 kHz, except the drum kit. The envelopes change sample by sample, so notes start without a tick.

The instruments are built into the driver. There is no SoundFont or sample set and nothing to configure. This is not the OPL3 of the Sound Blaster emulation. That one plays the FM registers DOS games write. This one plays General MIDI, from Windows MIDI and from the MPU-401 of the emulation.

## Tools

```text
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

WINMIX is the Windows mixer. It has a device list, a field for each of Master, Wave, Synthesizer, Recording level, CD and Line, a left-right lock, the master mute, the recording source, and the Tone test and Report buttons. Its settings are kept in SYSTEM.INI like those of the mixer, and Windows sets them again at its start.

The window is a dialog of the standard Windows controls. Tab and Shift+Tab reach every control, and a screen reader reads the label and the value of each field. Up and Down change a level by 5, Page Up and Page Down by 10, Home and End go to 0 and 100. A number typed into a field is taken when the field is left or Enter is pressed.

```text
WINMIX /UNMUTE              master unmuted (and 100%), all devices
WINMIX /MUTE                master muted
WINMIX /MASTER 0..100       master level, all devices
WINMIX /TONE                two seconds of 440 Hz through the driver (not MMSYSTEM)
WINMIX /DUMP [file]         devices, every level, mute state and driver log as text
                            (C:\WINMIX.TXT when no file is given)
WINMIX /HELP                this list
```

The command forms do the same as the window, without it. A batch file or the Run dialog can set the levels, and the report is a plain text file for a screen reader.

## Hardware notes

### HD Audio controller

* Link reset is done with clock gating off on Intel Skylake and later.
* Commands use CORB and RIRB, with a fallback to the immediate command interface when no answer comes.
* The DMA position comes from LPIB, or from the position buffer on VIA.
* Cache coherency is set up as in the Linux driver. Intel PCHs get TCSEL and no-snoop off. ATI and AMD get ENABLE_SNOOP. nVidia gets its snoop bits. On controllers that do not snoop, the DMA areas are written back with CLFLUSH or WBINVD.
* A function in D3 is brought to D0, and MSI and MSI-X are turned off.
* AMD FCH controllers on Ryzen and AM4 or AM5 boards (1022:1457, 1487, 157A, 15E3 and the older ATI SB IDs) get the ATI snoop register, a 1-second codec answer time, and repeated link resets until a codec answers.
* Every HD Audio function of the machine is probed, up to 8.

### Codec

* With several codecs, the first one with an analog output is used. HDMI and DisplayPort codecs are skipped.
* Paths from the line-out, speaker and headphone pins to a DAC are set up through mixers and selectors. The amplifiers on the way are opened, and the volume is applied at the stepped amplifier closest to the DAC.
* When headphones are plugged in, jack detection turns the speaker pins off.
* For recording, the analog ADC that reaches the most input kinds is chosen. On ALC256 and later, the first ADC sees only the digital microphone. Microphone with VREF bias, line in, CD and aux paths are connected to the ADC.
* Stereo mix is offered only when a mixer the ADC reaches also contains a playing DAC. Realtek's 0Bh analog loopback mixer mixes the inputs only, so it is not a stereo mix.
* Playback is 48 kHz, or 44.1 kHz when the codec supports 44.1 but not 48.

### Ensoniq AudioPCI

* ES1370 (1274:5000) with the AK4531 codec: DAC2 and ADC at 44.1 kHz (1411200 / 32), loop mode, and the phantom frame the chip's DMA needs. Master, voice, CD and line go through the AK4531's registers. Recording comes from its input mixer: microphone, line, CD and aux.
* ES1371 and ES1373 (1274:1371), Creative CT5880 (1274:5880) and Ectiva EV1938 (1102:8938): AC'97 warm reset, with the reset bit on the ES1373 revision 8 and CT5880 revisions A, C, D and E. The sample rate converter is set up as in the Linux driver. DAC2 and ADC run at 48 kHz. The AC'97 codec is reached through the codec register, and accesses wait for the converter's safe state.
* VMware's virtual ES1371 (1274:1371 revision 2) answers with a Cirrus Logic CS4297A AC'97 codec that keeps no datasheet defaults and has no power state. Nothing here depends on them. The start waits 10 and 50 ms at most, and the master and PCM volumes are set to 0 dB, not muted, before a mixer sets a level.
* The position is the frame counter of the channel.
* Windows 95, 98 and ME have their own drivers for these cards. WINAUDM.INF matches them by class (`PCI\CC_0401`), so Windows' own driver stays unless WINAUDIO's is chosen with Update Driver.

### Sound Blaster Live!

The Sound Blaster Live! is the EMU10K1 at 1102:0002. Voices 0 and 1 play one 16-bit stereo ring at 48 kHz: a stereo pair through the chip's page table, with the left channel on effects bus 0 and the right on bus 1. A small FX8010 DSP program sends the buses to the AC'97 front channels, the headphone output and the rear outputs, and sends the AC'97 input to the ADC capture channel. The AC'97 codec's mixer gives master, wave, CD and line.

This was written from the Linux and FreeBSD drivers and has not been tried on a real card. The Audigy (EMU10K2) has another register layout and is not driven.

### AC'97

* Cold reset and waiting for codec ready.
* IOSE on ICH4 and later.
* The swapped SR and PICB registers and byte counts of the SiS 7012.
* The CIV and PICB read race of nVidia.
* The AC-link start and SGD channels of VIA: the type register on the 686, and the SDX format and rate register on the 8233 and later.
* With VRA, the DAC and ADC are set to 48 kHz.
* The master volume's 5 or 6-bit step count is measured.
* EAPD is turned on. `/EAPD` is for amplifiers wired the other way.
* A secondary codec at register offset 80h is used when the primary one is a modem.
* Cirrus codecs (CS4297A and CS4299 of the IBM ThinkPad T2x, X2x and A2x) are switched to their normal mode first.
* On IBM and Lenovo ThinkPads, the embedded controller's volume and mute, which the volume buttons set, are set to full and unmuted at start. `/NOTPVOL` leaves them.

### Realtek codecs

The start-up that the Linux `snd-hda-codec-realtek` driver does for each codec is applied:

* Moving EAPD control from COEF to the EAPD verb on ALC2xx, 6xx, 8xx and 1220.
* PLL gating off on the ALC662 and ALC883 families and the ALC269VA.
* The ALC269VB settings.
* Headphone amplifier high power and the 3k pull-down off on the ALC255, 256, 236, 257, 225, 215, 245, 285 and 287 families.
* The amplifier bits of the ALC260, ALC880 to 885, and ALC888.
* The external amplifier GPIO from the codec's assembly id: a valid codec subsystem id, or the checksummed configuration of pin 1Dh.
* Linux's special COEF values for MSI and Clevo ALC1220 notebooks and Gigabyte X570 boards.

The codec parser and these settings were run on Linux against 198 Realtek codec dumps from real machines. They include the ALC255, ALC256, ALC225, ALC287, ALC1220, ALC887, ALC892, ALC888 and 889, ALC1200, the ALC269 family, ALC262, 268, 272 and 282, ALC66x and ALC86x. In every one, the speaker, headphone and line-out paths were set up correctly from the powered-down state.

## Intel audio DSP

Since Skylake, most Intel notebooks have a DSP in the audio controller. The HD Audio codec (Realtek, Conexant and so on) works on the HD Audio link independently of the DSP. The internal digital microphones are on the DSP's PDM interface, and on many machines the codec and the speaker amplifiers are on the DSP's I2S (SSP) or SoundWire links.

For those devices WINAUDIO loads the DSP firmware, sets up the audio pipelines in the DSP over IPC, and programs the codecs over I2C or SoundWire.

* Skylake, Kaby Lake (Amber and Whiskey): 9D70, A170, 9D71, A171, A2F0. `DSPSKL.BIN` (Intel AVS). IPC4.
* Apollo Lake, Gemini Lake: 5A98, 1A98, 3198. `SOFAPL.RI` or `SOFAPLC.RI`. SOF IPC3.
* Cannon Lake, Coffee Lake, Comet Lake, Rocket Lake: 9DC8, A348, 02C8, 06C8, A3F0, F1C8. `SOFCNL.RI` or `SOFCNLC.RI`. SOF IPC3.
* Ice Lake: 34C8, 3DC8. `SOFICL.RI` or `SOFICLC.RI`. SOF IPC3.
* Jasper Lake: 38C8, 4DC8. `SOFJSL.RI` or `SOFJSLC.RI`. SOF IPC3.
* Elkhart Lake: 4B55, 4B58. `SOFEHL.RI` or `SOFEHLC.RI`. SOF IPC3.
* Tiger Lake (Comet Lake-R): A0C8, F0C8. `S4TGL.RI` or `S4TGLC.RI`. SOF IPC4.
* Tiger Lake-H: 43C8. `S4TGLH.RI` or `S4TGLHC.RI`. SOF IPC4.
* Alder Lake-P, Raptor Lake-P: 51C8 to 51CF. `S4ADL.RI` or `S4TGLC.RI`. SOF IPC4.
* Alder Lake-N: 54C8. `S4ADLN.RI` or `S4TGLC.RI`. SOF IPC4.
* Alder Lake-S, Raptor Lake-S: 7AD0, 7A50. `S4ADLS.RI` or `S4TGLHC.RI`. SOF IPC4.
* Meteor Lake, Arrow Lake: 7E28, 7728. `S4MTL.RI` or `S4MTLC.RI`. SOF IPC4.
* Arrow Lake-S: 7F50. `S4ARLS.RI` or `S4MTLC.RI`. SOF IPC4.
* Lunar Lake: A828. `S4LNL.RI` or `S4LNLC.RI`. SOF IPC4.
* Panther Lake, Wildcat Lake: E428, E328, 4D28. `S4PTL.RI`, `S4WCL.RI` and their `C` versions. SOF IPC4.
* Nova Lake: D328, 6E50. `S4NVL.RI`, `S4NVLS.RI` and their `C` versions. SOF IPC4.
* Bay Trail, Cherry Trail and Braswell, Broadwell: ACPI 80860F28, 808622A8, INT3438. `SOFBYT.RI`, `SOFCHT.RI` or `SOFBDW.RI`. SOF IPC3.

### Firmware

The files in `WINDRVFW` are redistributable binaries from Intel and the Sound Open Firmware project. See `FIRMWARE.TXT` and the licences `LICSOF.TXT`, `LICSST.TXT` and `LICCIRR.TXT`.

The DOS driver reads the firmware into XMS at boot. It looks in `WINDRVFW` in the directory of the WINAUDIO.SYS named in CONFIG.SYS, and then in that directory itself. The Windows 95/98/ME driver reads it from `WINDOWS\SYSTEM\WINDRVFW`.

The DSP is started with the Intel-signed file first. When the ROM rejects the signature, as on Chromebooks, the community-signed file is tried. Its name ends in `C`.

### When it starts

The DSP is started when the machine has no HD Audio codec, when the ACPI NHLT table reports a digital microphone or I2S endpoint, or when ACPI has a known I2S codec or SoundWire controller. `/NODSP` turns it off. `/DSP` drives the HD Audio codec through the DSP too.

### Digital microphones

The DSP drives 2 or 4 microphones with the PDM configuration of the NHLT. Four microphones are folded to stereo. They appear as a separate recording device in Windows: "Intel SST Digital Microphones".

### I2S codecs and amplifiers

The codecs are found by their ACPI ids and programmed over I2C. The I2C controller is the Intel LPSS DesignWare. The speaker amplifier enable lines are Intel GPIO pins.

The supported codecs and amplifiers are:

* Everest ES8316, ES8336 and ES8326.
* Realtek RT5682, RT5682S, RT5640, RT5642, RT5645, RT5650, RT5651, RT5663, RT286 and RT288.
* Realtek RT1011, RT1015, RT1015P, RT1019 and RT1308 amplifiers.
* Maxim MAX98357A, MAX98360A, MAX98373, MAX98390, MAX98927 and MAX98090.
* Dialog DA7219 and DA7213.
* Nuvoton NAU8825, NAU8821, NAU8824, NAU8318 and NAU8315.
* Cirrus CS42L42.
* TI TAS2563.
* ADI SSM4567.
* Conexant CX2072X.

The SSP port, clock and format come from the NHLT blob. When there is no blob, Linux's defaults for that platform are used. Output moves to the headphones when they are plugged in, and the headset microphone is detected.

### SoundWire

SoundWire is used on Comet Lake and later, with the Intel cAVS, ACE 1.x and ACE 2+ links. The peripherals are enumerated, data ports and bandwidth are computed, and amplifiers on several links are joined into one stereo stream.

The supported drivers are:

* Realtek RT700, RT711, RT711-SDCA, RT712, RT713, RT714, RT715, RT716, RT717 and RT722.
* Realtek RT1308, RT1316, RT1318, RT1320, RT1321 and RT1017 amplifiers.
* Cirrus CS42L43 with the patch `CS42L43.BIN`, and CS35L56, CS35L57, CS35L62 and CS35L63.
* Cirrus CS42L42.
* Maxim MAX98373.
* Realtek RT5682.

### Bay Trail, Cherry Trail and Broadwell

On these platforms the DSP is an ACPI device. Its addresses are read from the BIOS's ACPI NVS area. The codec's MCLK is enabled from the PMC platform clock at 19.2 MHz.

On tablets the codec is mostly an RT5640, RT5645 or RT5651, an ES8316, NAU8824, DA7213, MAX98090 or CX2072X on SSP2. On Bay Trail CR it is on SSP0.

`AUDTEST LOG` shows the DSP start, the firmware version, the endpoints and the codecs found.

## Programming interface

The interface is INT 2Fh with `AH = D5h`.

* `AL=00h`: installation check. It returns `AL=FFh`, `BX=3357h` and a control block at `ES:DI`.
* `AL=01h`: API call. `ES:BX` points to a `struct winapireq`. See `src/inc/winapi.h`.

In Enhanced Mode, Windows programs can call the protected-mode entry of the VxD, device id 3D35h, with `AX=3` and `CX:DX` set to the real-mode address of the request.

The functions are:

* `WINF_AUD_DEVS` (50h): the device list, as `struct winauddev`.
* `WINF_AUD_OPEN` (51h): open a stream with the device, direction, rate, bits, channels and float flag. The buffer is a `struct winring`.
* `WINF_AUD_CTL` (52h): start, pause, reset, close or set the stream volume.
* `WINF_AUD_MIX` (53h): the mixer and the recording source.
* `WINF_AUD_MIDI` (54h): a MIDI message or byte string, SysEx included.

The ring buffer, `struct winring`, is in conventional memory. It has a 32-byte header followed by the data. The producer advances `wr`, the consumer advances `rd`, and the driver writes the bytes heard or recorded into `pos`.

## PCI resources above 4 GB

With "Above 4G Decoding" enabled in the BIOS setup, the firmware may place the registers of some devices with 64-bit BARs, and bridge windows, above 4 GB. 32-bit DOS and Windows cannot reach them.

At boot, before Windows and its drivers look at the machine, the DOS driver moves those resources of the devices WINDRV drives into free PCI space below 4 GB. WINAUDIO.SYS does it; whichever WINDRV DOS driver loads first does it for all of them. The devices are:

* USB controllers: UHCI, OHCI, EHCI, xHCI and USB4.
* Audio: HD Audio, AC'97 and the Intel audio DSP.
* Disk controllers: IDE, SATA, RAID and NVMe.
* SD controllers.
* Intel LPSS I2C and Thunderbolt NHI.

When such a device sits behind a bridge with a prefetchable window above 4 GB, the whole tree below that bridge is placed again. That includes nested bridges and the BARs behind them, largest first.

Graphics cards and all other devices stay where the firmware put them. The log says:

```text
pci 01:00.0 BAR above 4G left in place (not a WINDRV device; /ALL4G moves it)
```

So "Above 4G" only affects the drivers' own devices. `/ALL4G` moves every device and bridge window. A large BAR that does not fit is shrunk with Resizable BAR. `/NORELOC` turns the pass off. A device's BAR is then moved alone when its driver starts it.

The free space is computed from TOLUD to PCIEXBAR on Intel Core chipsets, from TOP_MEM to FEC00000 on AMD, and from every BAR, window and ACPI MCFG range in the system. Each move is logged:

```text
pci 00:14.0 BAR 4000000000 -> 9f000000 (64K)
```

On Windows 95, 98 and ME, WIN4G.VXD does the same. The INF files and the installer copy it to `SYSTEM\` and add `device=win4g.vxd` to the `[386Enh]` section of SYSTEM.INI. This small static VxD does the move at every boot, before the Plug and Play PCI enumeration and before all device drivers, in `Sys_Critical_Init`. The devices therefore come below 4 GB on Windows ME too, which has no CONFIG.SYS driver. One restart is needed after the first installation.

`All4G=1` in the `[WIN4G]` section of SYSTEM.INI moves all devices, like `/ALL4G`. `Debug=1` (port E9h) or `Debug=COM1` logs what it does.

## Architecture

```text
DOS, Real and Standard Mode

  DOS, programs, Windows (real or standard mode, DOSX)
      |
      | block and character device requests, INT 2Fh API
      v
  WINAUDIO.SYS (16-bit DOS driver)
      - drive letters, INT 2Fh API
      - 8042 injection (keyboard and mouse)
      |
      | switch to protected mode (directly in real mode, VCPI under EMM386)
      v
  WINACORE.BIN (32-bit core in an XMS block, with paging)


386 Enhanced Mode

  Windows VMs (system VM and DOS VMs)
      |
      v
  WINAUDIO.386 (VxD, ring 0)
      - the same 32-bit core code
      - VPICD, VKD, WINMOUSE, VMM
```

* The DOS driver is loaded by CONFIG.SYS. It puts the 32-bit core into XMS memory and switches the processor to protected mode for a moment on every request.
* The VxD is loaded automatically when Windows starts in 386 Enhanced Mode. SYSTEM.INI needs no line. The DOS core lets go of the hardware, the VxD, compiled from the same sources, takes it over, and the DOS core takes it back when Windows exits.
* In DOS, Real Mode and Standard Mode, the core is polled from the timer (INT 08h) and DOS idle (INT 28h) interrupts. It only runs at safe moments: DOS is not inside a function and no BIOS disk call is in progress. In Enhanced Mode the VxD runs every 10 ms.
* Windows 95, 98 and ME use the Plug and Play VxD built from the same core.
* An exception in the core writes `WINAUDIO CORE EXCEPTION ...` on a red background in the first screen line and stops. A photo of that line is enough to locate the fault.
* The API is INT 2Fh, AH=D5h. See `src/inc/winapi.h`.

## Building

You need Open Watcom v2 (wcc, wcc386, wrc and wlink), NASM and Python 3.

On Linux:

```sh
export WATCOM=/opt/watcom
cd src
./build.sh
```

On Windows, in a Command Prompt:

```bat
cd src
set WATCOM=C:\WATCOM
build.bat
```

`build.sh` uses the Linux Open Watcom binaries in `$WATCOM/binl64`; `build.bat` uses the Windows ones in `%WATCOM%\binnt`. `build.bat` looks for Open Watcom in `WATCOM`, then `..\ow`, then `C:\WATCOM`. Both scripts write the same files.

The object files go to `../obj`. The finished files are sorted into `../bin/DOS`, `../bin/WIN3X` and `../bin/WIN9X` with `src/bin-layout.txt`.

`src/buildtools` holds the build helpers:

* `fixvxd.py`: turns the LE file of wlink into the form the Windows 3.1 VMM expects.
* `oslayout.py`: sorts the output by system.
* `genserv.py`: makes the VxD service numbers from the DDK INC files. `src/inc/vxdsvc.inc` comes ready made.
* `genicons.py`: makes the program icons.
* `mkdisk.py`: makes disk images for tests.

`src/firmware` is copied into `bin/WIN9X/WINDRVFW`. The INF files and the installer take it from there.

Source files and documents are stored with CRLF line ends. `.gitattributes` keeps them that way.

### Source tree

* `src/core`: the 32-bit core for all systems. PCI, ACPI, HD Audio (`hda.c`, `afunc.c`), AC'97, Ensoniq AudioPCI (`es137x.c`), Sound Blaster Live! (`emu10k.c`), the sound engine (`aeng.c`), the synthesizer, the Intel DSP (`dsp*.c`, `nhlt.c`), I2S codecs (`cdc*.c`), SoundWire (`sdw*.c`), DesignWare I2C and Intel GPIO.
* `src/inc`: shared headers (`winapi.h` has the API and control block) and the VxD service numbers.
* `src/dos`: DOS. WINAUDIO.SYS (NASM), the DOS platform layer of the core, and WIN30FIX.COM.
* `src/vxd`: Windows 3.x Enhanced Mode and Windows 95/98/ME. The VxD platform layer and entries, Plug and Play (HD Audio bus), the DirectSound driver (`dsvxd.c`) and WIN4G.VXD.
* `src/inf`: the Windows 95/98/ME INF files.
* `src/win`: the 16-bit Windows programs for 3.x and 9x: WINSOUND.DRV, WINBEEP.DRV, WINMIX and WINGRP.
* `src/tools`: the DOS programs, the installer and uninstaller (`winsetup.c`), and AUDTEST.
* `src/firmware`: the DSP and SoundWire firmware (`FIRMWARE.TXT`), copied to `bin/*/WINDRVFW`.
* `src/tests`: the host tests for the sound engine, the synthesizer and the HD Audio codec parser (`hdacodec`).
* `src/buildtools`: the Python build helpers and the sample rate converter and synthesizer table generators.

## Tests

`src/tests/host` runs the sound engine and the synthesizer on Linux with the Open Watcom Linux target. It checks the SNR of the sample rate converter and the float and 8-bit inputs, and plays a MIDI file. Run it with `run.sh [file.mid]`.

`src/tests/host/hdacodec` runs the HD Audio codec parser against Linux `/proc/asound/card*/codec#*` dumps. Run it with `run.sh dump...`. It shows the chosen output pins, DAC paths, volume nodes, ADC and input paths, and the GPIO and COEF writes.

`src/tests/host/es1371` runs `es137x.c` with the sound engine against an emulated Ensoniq ES1371. QEMU has none, so the emulation is part of the test. Run it with `run.sh`. The emulation follows how the Linux driver programs the chip: the sample rate converter RAM with its busy and state bits, the AC'97 codec interface that may be used only in the converter's safe state, the DAC2 and ADC frames in loop mode with their frame counters, the AC'97 reset bit of the ES1373 revision 8 and CT5880, and the extra codec read bit of the EV1938. It takes the DAC2 and ADC rates from what the driver wrote into the converter.

The test checks the start-up of the ES1371, ES1373, CT5880 and EV1938, the master volume, a 440 and 880 Hz stream through DAC2, and recording of a 1 and 2 kHz tone from the ADC, with pitch and continuity measured. The ES1371 run is VMware's virtual card: revision 2, a codec without datasheet defaults, power state or extended id. A codec that answers without a vendor id is refused with its volumes left alone.

`src/buildtools/gensrc.py` makes the sample rate converter filters in `src/core/srctab.h`. `gensynth.py` makes the synthesizer tables in `src/core/synthtab.h`. `mkgpio.py` makes the Intel GPIO pad tables in `src/core/gpiotab.h` from the Linux pinctrl drivers.

QEMU with SeaBIOS was used to verify the drivers. It used `intel-hda` with `hda-duplex` and `AC97`, and wrote the output to a file with `-audiodev wav` for analysis. The checks were:

* HD Audio and AC'97 detection and codec paths.
* The test tone and MIDI in DOS, with pitch and frequency measured.
* Recording in DOS and Windows.
* The startup sound TADA.WAV and CANYON.MID in Media Player, with the synthesizer through the MIDI Mapper, in Windows 3.1 Enhanced Mode.
* The same with AC'97 in Standard Mode (`WIN /S`).
* Looped playback (`WHDR_BEGINLOOP`) and sample-exact position.
* WINMIX.
* INSTAUD and UNINAUD, together with WINUSB too.
* Playback and recording in Windows 3.0 with the Multimedia Extensions, in Standard and Enhanced Mode.
* WINBEEP beeps and `PLAY` / `SOUND` music in Windows 3.0 Real Mode, with frequencies measured.
* The Windows 95, 98 and ME cases described in Windows 95, 98 and ME, DirectSound included.

DOS games with the Sound Blaster emulation had their output recorded and checked for gaps. The games were Epic Pinball in real mode, and Jazz Jackrabbit and Tyrian 2000 through Borland's RTM and the DPMI host, with the built-in V86 monitor. The memory managers tested were MS HIMEM and EMM386, HIMEMX, JEMM386, JEMMEX with QPIEMU for the emulation, and UMBPCI, with all three WINDRV drivers loaded high.

QEMM 7.01 and 7.5 were tested with DOS 6.22 and an ES1370. Both load WINAUDIO and WINUSB and play the test tone. Under QEMM 7.5 the emulation passes `AUDTEST SB`: DSP, interrupt, 8-bit DMA, FM and MPU-401. A DMA test program plays its tone, and a 200 KB floppy copy compares equal. QEMM 7.01 reports that it cannot trap ports.

The Ensoniq ES1370 was tested in QEMU. The test tone ran in DOS. Windows for Workgroups 3.11 had WINAUDIO and WINUSB installed by INSTAUD and INSTUSB, and played the startup and system sounds; exiting to DOS and starting the card again there worked. Windows 98 had the sound device installed by WINAUDM.INF and ran the DirectSound cases above.

The farbrausch music disk fr-028 was tested in Windows 98 with the DirectSound primary buffer. What DirectSound had written ahead of the engine was measured before and after the change of the write cursor.

## Known limitations

* Real hardware: the drivers were verified in QEMU and by code review. The fixes for particular Intel and AMD hardware were written from the Linux drivers and the datasheets.
* Windows/386 2.x switches the processor to V86 mode without offering VCPI. While it runs, the 32-bit core cannot be entered and the driver serves no requests. The real-mode Windows 2.x (`WIN86.COM` and `WIN.COM`) works.
* DOS file systems: MS-DOS 5 and 6 read only FAT12 and FAT16, with 2 GB per partition at most. MS-DOS 7.10, 8.0 and FreeDOS also read FAT32. NTFS and exFAT partitions get no drive letter.
* SeaBIOS in QEMU: when SeaBIOS uses a USB keyboard or mouse at boot, its USB polling in SMM can stop the machine while Windows 2.x starts, and DOS boxes of Windows 95, 98 and ME close. Real BIOSes hand the controllers over properly, with the BIOS and OS semaphore and SMIs off.
* Windows for Workgroups 3.11 was not tried on real hardware.
* Most of the drivers have not been tried on real hardware yet. HD Audio and AC'97 were tested only with QEMU's Intel ICH6 and ICH9 HD Audio and ICH AC'97 emulation. The Intel DSP path (firmware loading, IPC, NHLT, I2S codecs, SoundWire, Bay Trail and Cherry Trail) cannot be emulated in QEMU. It was written from the Linux drivers and the SOF firmware sources; firmware image parsing, the IPC messages and the SoundWire link flow were checked with simulation harnesses on the host. The VIA, SiS and nVidia AC'97 code and the controller-specific HD Audio settings follow the datasheets and the Linux driver.
* Some notebooks need a codec GPIO or a vendor-specific codec setting for the speaker amplifier. Of the hundreds of model-specific fixes in Linux, only the generic ones are applied. With silent speakers, try `/GPIO`.
* The speakers of notebooks with smart amplifiers on I2C next to the HD Audio codec are not supported. This includes some Lenovo, ASUS and HP models with ALC287 or ALC245 and CS35L41 or TAS2781. The microphone of the combo headset jack of HD Audio notebooks is not supported either.
* Intel DSP: only the basic modules of the SOF and AVS firmware are used, such as the copier. There are no audio effects, no speaker protection or calibration (CS35L56 tuning, MAX98390 DSM, TAS2563 RCA files) and no headset buttons. Streams are 48 kHz.
* The Stereo Mix of Realtek's Windows drivers is done in software. WINAUDIO has stereo mix only when the codec hardware offers it.
* Digital outputs (S/PDIF and HDMI) and 5.1 output are not supported. Multichannel sources are not downmixed; the first two channels are played.
* In Windows 3.0 Real Mode, MMSYSTEM does not run, so there is no wave or MIDI. Only WINBEEP's beeps and `PLAY` / `SOUND` music play.
* There is no MIDI input. The synthesizer is an FM approximation, not sample based or wavetable.
* Latency is high in DOS and Standard Mode, about 0.4 seconds. It is 50 to 100 ms while the Sound Blaster emulation runs from the RTC.
* Windows 95, 98 and ME have no hardware 3D in the DirectSound driver. DirectSound mixes 3D buffers in software.
* The Sound Blaster Live! (EMU10K1) has not been tried on a real card. The Audigy (EMU10K2) and the Live! 24-bit (CA0106) are not supported.
* Jazz Jackrabbit 1.0 and other Borland Pascal programs stop with "Runtime error 200" on processors faster than about 200 MHz. It is a bug of Borland's CRT unit, with any DPMI host or none. The published patches for that error fix them.

## Version history

### 1.2 (2 November 2026)

* DirectSound hardware secondary buffers. Every device offers up to 16 secondary buffers in hardware (`DSBCAPS_LOCHARDWARE`): 8 or 16 bit, mono or stereo, 100 Hz to 100 kHz. The sound engine mixes them like the wave streams, each with its own rate (`SetFrequency`), volume and pan (`SetVolumePan`), and position (`SetPosition`). A buffer plays once or loops, and `DuplicateSoundBuffer` shares the memory of the original. DirectSound mixes only 3D buffers and buffers that do not fit in software. Verified in Windows 98 on AC'97, the Ensoniq ES1370 and a USB Audio device.
* DirectSound primary buffer. The write cursor stands 40 ms after what the sound engine has taken, and what it has taken becomes silence. The engine takes the buffer in steps of its service. DirectSound's mixer, and programs that write the primary buffer such as the farbrausch music disk fr-028 "Brullwurfel", wrote only a few ms ahead of the old write cursor. A late step played the sound of one turn of the buffer ago, which was heard as crackling.
* Ensoniq AudioPCI. ES1370 with the AK4531 codec at 44.1 kHz, and ES1371, ES1373, Creative CT5880 and Ectiva EV1938 with an AC'97 codec at 48 kHz through the rate converter. Playback, recording, mixer, Sound Blaster emulation and DirectSound. The ES1370 was tried in QEMU in DOS, Windows for Workgroups 3.11 and Windows 98 with DirectSound. The ES1371 family, which QEMU lacks, was tested against an emulation of the chip written from the Linux driver in `src/tests/host/es1371`.
* VMware's ES1371 interrupt. The DAC2 and ADC period interrupts are enabled through bits 9 and 10 of the serial port register. The DAC2 interrupt is acknowledged by toggling bit 9, as the Linux driver does. The DMA size and period count use the Linux driver's dword values. Playback starts and the position advances; the test tone plays.
* Creative Sound Blaster Live! (EMU10K1). Playback through a stereo voice pair and a small DSP program for the front, headphone and rear outputs. Recording through the ADC channel and the AC'97 mixer. Written from the Linux and FreeBSD drivers and not tried on a real card yet, because QEMU has none. The Audigy (EMU10K2) is not supported.
* `INSTAUD /DOSONLY`: only the DOS driver, with `/NOVXD`. WINAUDIO.SYS lets go of the card when Windows starts and loads nothing into it, so Windows keeps its own driver for the card. The Sound Blaster emulation is there again in DOS and MS-DOS mode.
* Uninstalling WINUSB or WINAUDIO on Windows 95/98/ME leaves the SYSTEM.INI `[drivers]` keys `wave=`, `midi=` and `mixer=winsound.drv` alone while the other package is installed.
* `UNINAUD` deleted the files of the directory it was started from when CONFIG.SYS had no `WINAUDIO.SYS` line. Started from `bin\DOS`, it deleted the package. It now removes files only from the directory CONFIG.SYS names. Any other directory is left in place and the uninstaller says which. A Windows 95/98/ME installation without `/DOS` keeps its directory; delete it by hand.
* WINMIX without its window: `/UNMUTE`, `/MUTE`, `/MASTER n`, `/TONE` and `/DUMP [file]`. They set the mixer, play a test tone through the driver, and write the devices, the levels, the master mute and the driver log to a text file that a screen reader can read. A saved mute from Volume Control or the taskbar icon left WINMIX showing 100% while the card was silent. The window now has a Mute box, and it is a dialog of the standard controls: a label and a field for every level, so a screen reader reads them and Tab reaches every control. The tone test now writes exactly two seconds and waits for the card to play them before it reports.
* Mixer settings are kept across restarts on every system. Windows 95/98/ME keep them in the registry and set them before the sound devices start, for WINMIX and Volume Control alike. Windows 3.x keeps them in SYSTEM.INI as before. DOS keeps them in `WINAUDIO.VOL` next to WINAUDIO.SYS; `AUDTEST MIX ...` writes the file and WINAUDIO.SYS sets the values when it starts.
* The built-in V86 monitor gives expanded memory: LIM EMS 4.0, the device `EMMXXXX0`, a 64 KB page frame in a free part of the upper memory area, and pages from XMS when a program asks. Programs that need EMS no longer need EMM386, which leaves no Sound Blaster emulation in DOS. `/EMS:n` sets the most it gives in MB, with a default of 16. `/NOEMS` turns it off.
* Sound Blaster emulation under QEMM 7.5. QEMM's own trap callback was taken for another emulator (SBEMU), so the emulation stayed off. WINAUDIO.SYS now chains to it. QEMM traps the DMA mode and mask registers for its DMA translation. Trapping them too broke floppy disk transfers, so they stay with QEMM and the card's channel counts as auto-init. QEMM before 7.03, whose QPI cannot trap ports, gets its own message. AUDTEST also names QEMM 7.03 or later, and names AudioPCI and EMU10K1 devices.
* Program Manager on Windows 3.x gets an Uninstall WINAUDIO item with a red cross on the speaker icon. Windows closes, `UNINAUD /REBOOT` runs, and the computer restarts. Windows 95/98/ME also get the programs as desktop shortcuts, and the uninstaller removes them. The banner lists DOS, Windows 3.x and Windows 95/98/ME.
* Sound Blaster emulation: the emulated card is reset when the program that used it ends, in DOS and in Windows MS-DOS boxes. A game that exits with its sound still running leaves no loud buzzing.
* USB Audio through WINUSB: devices such as C-Media 0D8C:0008 whose streaming descriptors were not accepted. See WINUSB 1.2.
* WINSOUND.DRV has 64 buffers per stream and sample-accurate positions for DirectSound in emulation mode and for players that queue many small buffers.
* DirectSound driver for Windows 95/98/ME with DirectX. Every sound device registers a DirectSound driver with DSOUND.VXD. Its primary buffer is played by the sound engine as a card plays its DMA buffer, and DirectSound's kernel mixer mixes the programs' sound buffers into it. DirectSound runs on the hardware abstraction layer instead of emulating through the wave driver, with lower latency and exact positions. The driver is also the one of the default device. WINAUDM.INF makes MMDEVLDR.VXD the DevLoader of the sound devices and lists WINSOUND.DRV in their devnodes, so MMSYSTEM knows which wave device belongs to which devnode. After updating, choose Update Driver for the sound device in Device Manager once, with Have Disk and `WINDOWS\INF`.
* DPMI host: 16-bit clients get the MS-DOS extensions of Windows. INT 21h calls with protected-mode pointers are copied through a buffer in conventional memory. The PSP, DTA, vectors and DOS memory are selectors. Borland's RTM, used by Borland Pascal and C++ programs in protected mode such as Jazz Jackrabbit and Tyrian 2000, refused to start with "Loader error (0025)" or hung.
* DPMI host: segment bases of 16-bit clients have all 32 bits. Their memory is at 40000000h, and the high byte was cut off, so the client wrote over the interrupt vectors. The processor type is given as Windows gives it, at most a 486, because older clients look it up in a table.
* DPMI host: an interrupt that came while a handler on the locked stack had called the host's default handler (PUSHF and CALL FAR) overwrote that handler's return address. This crashed when leaving Tyrian's setup.
* Sound Blaster emulation: the silence transfer (DSP command 80h) ends with its interrupt like other transfers. Tyrian reported "ERROR 253: Sound Effects disabled".
* Sound Blaster emulation: the time between a single-cycle block and the next one the program starts is not played as silence. The next block makes it up. This fixes crackling in Epic Pinball and other games that play block by block.
* The sound engine mixes further ahead when a controller fetches more than it plays. The AC'97 and HD Audio controllers of virtual machines take tens of ms at once. The periodic gaps there are gone.
* The hint about EMM386 in WINAUDIO.SYS and AUDTEST fits the screen and names the EMS of the V86 monitor.
* The installer lists the Windows installations it finds, numbered, and asks which to set up. `1` and `1,3` choose entries, `A` chooses all, and `0` means none, so only the DOS driver is set up. `/ALL` sets up all of them without asking, and `/W:dir` only that one.
* The DOS driver takes its extended memory above 16 MB when there is room there. Windows 3.x Standard Mode and other 286 DOS extenders can use only the memory below 16 MB. They reported "Insufficient extended memory" with all WINDRV drivers loaded.
* `/Q`: only error messages on the screen while the DOS driver loads.
* Fixed "CPU without CPUID or time stamp counter" on Vortex86 DX, MX and EX processors. Their CPUID instruction works, but the ID flag of EFLAGS cannot be changed. The time stamp counter is tried directly.
* WININFO finds the driver when it was loaded with DEVLOAD and a program, such as the command shell, later put back an INT 2Fh vector it had saved before. The driver hooks INT 2Fh again within a few seconds, and WININFO waits for that when the driver's device header is there.
* Fixed a crash at boot with `DEVICEHIGH=` under JEMM386 or JEMMEX: "CORE EXCEPTION 0000000E". The driver used the memory after its image as a buffer while it started, and a small upper memory block ends right there. The buffer now fits in the block DOS gave.
* The texts of the DOS drivers, the VxDs and the documentation name everything the drivers support.
* `src\build.bat`: the build on Windows with the Windows Open Watcom. WSL is no longer needed.

### 1.1 (26 October 2026)

* Fixed "CPU without time stamp counter" on CPUs that have one but do not report it through CPUID, such as the Vortex86 DX, SX and EX. The TSC is tried directly.
* The TSC rate is measured with a time limit and with the PIT counter when the timer interrupt does not come.
* WINAUDIO.SYS loaded with DEVLOAD or another device loader finds WINACORE.BIN in its own directory. It can be loaded high.
* The installer tells Windows 98, 98 Second Edition and ME apart by the version of USER.EXE. Windows 98 was shown as Windows 95.
* Added Sound Blaster 16 emulation: DSP 4.05, mixer, OPL3 FM, MPU-401, 8 and 16-bit DMA, and IRQ 5 or 7. It works for DOS programs in the MS-DOS boxes of Windows 3.x and Windows 95/98/ME, and in plain DOS with QEMM or JEMM386 and JEMMEX plus QPIEMU.
* Added a built-in V86 monitor to WINAUDIO.SYS. In plain DOS with HIMEM only, and no EMM386 or JEMM needed, it traps the Sound Blaster ports itself, emulates A20 and the INT 15h block move for HIMEM, gives Windows 3.x and 9x real mode back at their start through INT 2Fh 1605h, and takes over again when they end.
* Added a DPMI 0.9 host to the V86 monitor, so protected-mode games get the Sound Blaster emulation in plain DOS too. It supports XMS memory, DOS memory, descriptors, real-mode calls and callbacks, raw mode switches, exception handlers, hardware interrupts to protected-mode handlers, and nested DPMI programs.
* Added a VCPI server to the V86 monitor, with the EMMQXXX0 device, for WINDISK.SYS and WINUSB.SYS and for Windows 3.x Standard Mode. It is hidden from other programs so that DOS extenders pick DPMI; `/VCPI` shows it to all.
* Added the WINAUDIO.SYS options `/NOV86`, `/NODPMI`, `/NOVCPI`, `/VCPI` and `/VME`.
* Fixed Windows 3.1 hanging at its start and having no sound after a protected-mode game such as DOS/4GW or Quake had run in DOS. The DPMI host put back the INT 15h and 1Bh vectors that the program had already restored itself, pointing into freed memory.
* HD Audio and AC'97: DMA streams and the position buffer still running from before a warm restart are stopped before the controller is reset.
* Sound Blaster emulation in Windows 3.x and 95/98/ME: the DMA terminal count bit of the 8237 status registers is set for the emulated channels. Ports 08h and D0h are answered by the VxD.
* Added Sound Blaster 16 emulation and the synthesizer on USB Audio 1.0 and 2.0 devices of WINUSB, in DOS, in Windows 3.x MS-DOS boxes and in Windows 95/98/ME. It is used when there is no other sound device, or always with `/USBSB`. INSTAUD puts WINAUDIO.SYS after WINUSB.SYS in CONFIG.SYS. WINAUDIO.SYS waits for WINUSB's USB Audio device, stays resident with `WINUSB.SYS /USB` even without sound hardware of its own, and says at the start why the bridge is not there. AUDTEST SB measures the RTC interrupt that drives the emulation in DOS.
* AC'97: the codec is tried even when the controller never reports it ready. This fixes the Cirrus Logic CS4299 of the ThinkPad T23.
* Added a mixer driver to WINSOUND.DRV for Windows 95/98/ME (`mixer=winsound.drv`). Volume Control (SNDVOL32), the taskbar volume icon and programs that use the mixer API work. It has master volume and mute, wave, synthesizer, CD and line on AC'97, recording level and recording source. Changes from any program are reflected in the open mixers.
* The volumes and the recording source are kept in SYSTEM.INI `[winsound.drv]` and set again when Windows starts. They no longer go back to full after a restart.
* Volume values follow the Windows scale, linear in amplitude, on the hardware amplifiers of HD Audio, AC'97, USB Audio and the Intel DSP codecs. Half volume is 6 dB down, the lower half of a slider is no longer almost silent, and the mixer, programs such as Winamp, and WINMIX give the same loudness for the same value.
* Fixed a short skip at the start of playback in Windows 95/98/ME. The first refill of the controller's buffer came too late.
* Fixed removed USB wave devices accepting format queries. DirectSound or Media Player could choose one and then fail.
* Synthesizer fixes. The instruments with strong modulator feedback, such as organs, accordion, harmonica, overdrive and distortion guitar, strings, brass, synth leads and pads, and bagpipe, were harsh and noisy. Timpani played an octave low. Woodblock, muted guitar and fret noise were very short. Breath noise was not sustained, hi-hats were quiet, and cuica was too short to hear. Basses are louder and fuller, with more upper overtones for small speakers. High notes are softer, with key scaling and a gentle low pass like the OPL's. Notes start without a tick, with smooth envelopes and a longer fade-in. MIDI real-time bytes sent as short messages no longer play notes.
* Added SET BLASTER to the installer and the Sound Blaster settings to the WINAUDIO.SYS start message and AUDTEST. Added AUDTEST SB to test the emulated card.
* WINAUDIO.SYS lists the sound devices it found with their controller, PCI location and IRQ.
* Fixed HD Audio on AMD Ryzen boards, including AM4 and AM5 FCH controllers: snoop setting, codec answer time and link reset retries. Every HD Audio controller is probed.
* Fixed no sound on AC'97 notebooks such as the IBM ThinkPad T23: cold reset, secondary codec, Cirrus CS4299, and the ThinkPad volume and mute.
* Fixed sounds longer than 64 KB, such as the Windows startup sound, looping their beginning.
* Fixed old sound being played again when the driver is serviced late. Loud mixes are limited softly instead of clipping.
* Added the system sounds of WIN.INI `[sounds]`, including start and exit, in Windows 3.0 Real Mode through WINBEEP.DRV.
* Added MS-DOS 7.0, 7.1, 8.0 and FreeDOS support. The installer adds the CONFIG.SYS lines there too.
* The documentation is split into README.md for the overview and installation, and MANUAL.md for all details.

### 1.0 update (14 October 2026)

* Windows 95, 98 and ME: the HD Audio bus driver. Codecs (analog, HDMI and DisplayPort, modem) and the Intel DSP endpoints are separate devices in `WINAUDM.INF`. AC'97 is supported.
* PCI resources above 4 GB: the boot-time move applies only to WINDRV's devices. `/ALL4G` moves all devices. WIN4G.VXD does the move on Windows 9x.
* Installer: Start menu groups on Windows 95/98/ME, Read Me items, and no duplicate entries when installing again. The package is split into `bin/DOS`, `bin/WIN3X` and `bin/WIN9X`.

### 1.0 (5 October 2026)

First release.
