@echo off
rem build.bat - builds WINAUDIO on Windows with the Windows Open Watcom v2, NASM and Python 3
rem
rem   WINAUDIO.SYS  WINACORE.BIN  WINAUDIO.386  WINAUDIO.VXD  WINSOUND.DRV
rem   WINBEEP.DRV  WINMIX.EXE  WINGRP.EXE  WIN4G.VXD  AUDTEST.EXE
rem   INSTAUD.EXE  UNINAUD.EXE  WIN30FIX.COM  the INF files and the firmware
rem
rem The files go to obj\out and then to ..\bin\DOS, ..\bin\WIN3X and ..\bin\WIN9X
rem with bin-layout.txt, the same as build.sh.
rem Set WATCOM to the Open Watcom folder if it is not ..\ow or C:\WATCOM.
setlocal EnableExtensions
pushd "%~dp0" || exit /b 1

rem ---- tools ----
if defined WATCOM if exist "%WATCOM%\binnt64\wcc386.exe" goto have_watcom
if defined WATCOM if exist "%WATCOM%\binnt\wcc386.exe" goto have_watcom
set "WATCOM=..\ow"
if exist "%WATCOM%\binnt64\wcc386.exe" goto have_watcom
if exist "%WATCOM%\binnt\wcc386.exe" goto have_watcom
set "WATCOM=C:\WATCOM"
if exist "%WATCOM%\binnt64\wcc386.exe" goto have_watcom
if exist "%WATCOM%\binnt\wcc386.exe" goto have_watcom
echo build: Open Watcom v2 was not found.
echo Set WATCOM to its folder, for example: set WATCOM=C:\WATCOM
goto fail
:have_watcom
set "OWBIN=%WATCOM%\binnt"
if exist "%WATCOM%\binnt64\wcc386.exe" set "OWBIN=%WATCOM%\binnt64"
set "PATH=%OWBIN%;%PATH%"
set "INCLUDE=%WATCOM%\h"
rem Open Watcom v2 says "The Open Watcom Contributors"; the 2002 Sybase beta does not
set "OW2="
for /f "delims=" %%v in ('wcc386 2^>^&1 ^| findstr /c:"The Open Watcom Contributors"') do set "OW2=1"
if not defined OW2 goto not_v2
if not exist "%WATCOM%\lib286\win\mmsystem.lib" goto old_watcom

set "PY=python"
if defined PYTHON set "PY=%PYTHON%"
%PY% -c "import sys" >nul 2>nul
if errorlevel 1 set "PY=py -3"
%PY% -c "import sys" >nul 2>nul
if errorlevel 1 goto no_python
where nasm >nul 2>nul
if errorlevel 1 goto no_nasm

rem ---- sources and output ----
if not exist core\hda.c goto no_audio
if not exist bin-layout.txt goto no_layout
if not defined OBJ set "OBJ=..\obj"
set "OUT=%OBJ%\out"
set "BINDIR=..\bin"
if not exist "%OBJ%\audio\dos" mkdir "%OBJ%\audio\dos"
if not exist "%OBJ%\audio\vxd" mkdir "%OBJ%\audio\vxd"
if not exist "%OBJ%\win" mkdir "%OBJ%\win"
if not exist "%OBJ%\tools" mkdir "%OBJ%\tools"
if not exist "%OBJ%\win4g" mkdir "%OBJ%\win4g"
if not exist "%OUT%" mkdir "%OUT%"

set "COMMON=winlib pci acpi blk corent"
set "AUDSRC=aeng synth sbemu opl3 hda afunc ac97 es137x emu10k dsp dspipc4 dspipc3 dspdev dspcdc dspsst nhlt dwi2c sdw sdwlnk sdwstm sdwadv sdwrt71 sdwrt7l sdwamp sdwcs sdwmisc gpio cdces16 cdces26 cdcrt82 cdcrt10 cdcrt56 cdcrt63 cdcmax cdcda72 cdcnau cdccs42 cdctas cdcssm cdccx72 cdcm90 cdcrt86"
set "CF=-q -bt=windows -mf -s -zl -ox -wx -we -zp4 -ei -Icore -Iinc -DWIN_AUDIO"

rem ---- DOS core ----
echo build: DOS core
for %%f in (%COMMON% %AUDSRC%) do (
  wcc386 %CF% -fo=%OBJ%\audio\dos\%%f.obj core\%%f.c || goto fail
)
wcc386 %CF% -fo=%OBJ%\audio\dos\platdos.obj dos\platdos.c || goto fail
nasm -f obj -o %OBJ%\audio\dos\dosent.obj dos\dosent.asm || goto fail
for %%f in (vmon dpmi ems) do (
  wcc386 %CF% -Idos -fo=%OBJ%\audio\dos\%%f.obj dos\%%f.c || goto fail
)
nasm -f obj -o %OBJ%\audio\dos\vmonent.obj dos\vmonent.asm || goto fail
(
echo format raw bin
echo name %OUT%\WINACORE.BIN
echo option offset=0x80004000
echo option nodefaultlibs, quiet, map=%OBJ%\audio\dos\core.map
echo disable 1023
echo library %WATCOM%\lib386\dos\clib3r.lib
echo order clname CODE segment _HDR clname DATA clname BSS segment _BSSHEAD clname STACK clname ZZEND
echo file %OBJ%\audio\dos\dosent.obj
for %%f in (%COMMON% %AUDSRC%) do echo file %OBJ%\audio\dos\%%f.obj
for %%f in (platdos vmon dpmi ems vmonent) do echo file %OBJ%\audio\dos\%%f.obj
) > "%OBJ%\audio\dos\core.lnk"
wlink @%OBJ%\audio\dos\core.lnk || goto fail

rem ---- VxD ----
echo build: VxD
for %%f in (%COMMON% %AUDSRC%) do (
  wcc386 %CF% -nt=_LTEXT -nc=CODE -fo=%OBJ%\audio\vxd\%%f.obj core\%%f.c || goto fail
)
wcc386 %CF% -nt=_LTEXT -nc=CODE -fo=%OBJ%\audio\vxd\platvxd.obj vxd\platvxd.c || goto fail
for %%f in (pnp pnpaud dsvxd) do (
  wcc386 %CF% -nt=_LTEXT -nc=CODE -Ivxd -fo=%OBJ%\audio\vxd\%%f.obj vxd\%%f.c || goto fail
)
nasm -f obj -Iinc/ -DPKG_AUDIO -o %OBJ%\audio\vxd\vxdent.obj vxd\vxdent.asm || goto fail
nasm -f obj -Iinc/ -DPKG_AUDIO -DWIN9X -o %OBJ%\audio\vxd\vxdent9x.obj vxd\vxdent.asm || goto fail
call :vxdlink "%OUT%\WINAUDIO.386" "%OBJ%\audio\vxd\vxdent.obj" ""
%PY% buildtools\fixvxd.py "%OUT%\WINAUDIO.386" || goto fail
call :vxdlink "%OUT%\WINAUDIO.VXD" "%OBJ%\audio\vxd\vxdent9x.obj" "dynamic"
%PY% buildtools\fixvxd.py "%OUT%\WINAUDIO.VXD" || goto fail

rem ---- DOS driver ----
echo build: WINAUDIO.SYS
nasm -f bin -Idos/ -DPKG_AUDIO -o "%OUT%\WINAUDIO.SYS" dos\winstor.asm || goto fail
nasm -f bin -Idos/ -o "%OUT%\WIN30FIX.COM" dos\win30fix.asm || goto fail
copy /y inf\WINAUDIO.INF "%OUT%" >nul || goto fail
copy /y inf\WINAUDM.INF "%OUT%" >nul || goto fail
if exist "%OUT%\WINDRVFW" rd /s /q "%OUT%\WINDRVFW"
mkdir "%OUT%\WINDRVFW"
copy /y firmware\* "%OUT%\WINDRVFW" >nul || goto fail

rem ---- WIN4G.VXD ----
echo build: WIN4G.VXD
set "G=%OBJ%\win4g"
set "CF4=-q -bt=windows -mf -s -zl -ox -wx -we -zp4 -ei -nt=_LTEXT -nc=CODE -Icore -Iinc"
for %%f in (winlib pci acpi) do (
  wcc386 %CF4% -fo=%G%\%%f.obj core\%%f.c || goto fail
)
wcc386 %CF4% -fo=%G%\w4gplat.obj vxd\w4gplat.c || goto fail
nasm -f obj -Iinc/ -o %G%\win4g.obj vxd\win4g.asm || goto fail
(
echo format windows vxd
echo name %G%\WIN4G.VXD
echo option nodefaultlibs, quiet, offset=0, map=%G%\win4g.map
echo option description 'WIN4G moves PCI resources above 4G below 4G'
echo segment _LTEXT preload nondiscardable
echo segment _LDATA preload nondiscardable
echo export WIN4G_DDB.1
echo file %G%\win4g.obj, %G%\w4gplat.obj, %G%\winlib.obj, %G%\pci.obj, %G%\acpi.obj
) > "%G%\win4g.lnk"
wlink @%G%\win4g.lnk || goto fail
%PY% buildtools\fixvxd.py "%G%\WIN4G.VXD" || goto fail
copy /y "%G%\WIN4G.VXD" "%OUT%" >nul || goto fail

rem ---- WINGRP ----
echo build: WINGRP.EXE
wcc -q -ml -zw -bt=windows -ox -s -wx -I%WATCOM%\h\win -fo=%OBJ%\win\wingrp.obj win\wingrp.c || goto fail
wrc -q -r -bt=windows -i=win -i=%WATCOM%\h -i=%WATCOM%\h\win -fo=%OBJ%\win\wingrp.res win\wingrp.rc || goto fail
(
echo system windows
echo name %OUT%\WINGRP.EXE
echo option quiet, heapsize=1024, stack=4096
echo option resource=%OBJ%\win\wingrp.res
echo option description 'WINGRP Program Manager groups for WINDRV'
echo file %OBJ%\win\wingrp.obj
) > "%OBJ%\win\wingrp.lnk"
wlink @%OBJ%\win\wingrp.lnk || goto fail

rem ---- installer and uninstaller ----
echo build: INSTAUD.EXE UNINAUD.EXE
set "DF=-q -ml -bt=dos -ox -s -wx -Iinc -Itools"
wcc %DF% -DWIN_AUDIO -fo=%OBJ%\tools\instaud.obj tools\winsetup.c || goto fail
wcc %DF% -DWIN_AUDIO -DUNINST -fo=%OBJ%\tools\uninaud.obj tools\winsetup.c || goto fail
wlink system dos name %OUT%\INSTAUD.EXE file %OBJ%\tools\instaud.obj option quiet, stack=12288 || goto fail
wlink system dos name %OUT%\UNINAUD.EXE file %OBJ%\tools\uninaud.obj option quiet, stack=12288 || goto fail

rem ---- sound programs ----
echo build: WINSOUND.DRV WINBEEP.DRV WINMIX.EXE AUDTEST.EXE
set "WS=-q -ml -zu -s -bt=windows -ox -wx -zl -I%WATCOM%\h\win -Iinc"
wcc %WS% -fo=%OBJ%\win\winsound.obj win\winsound.c || goto fail
nasm -f obj -o %OBJ%\win\winsndlb.obj win\winsndlb.asm || goto fail
(
echo format windows dll
echo name %OUT%\WINSOUND.DRV
echo option modname=WINSOUND
echo option description 'wave,midi,aux,mixer:WINSOUND HD Audio, AC97, Intel DSP and USB Audio'
echo option quiet, map=%OBJ%\win\winsound.map
echo segment type CODE preload fixed
echo segment type DATA preload fixed
echo file %OBJ%\win\winsndlb.obj, %OBJ%\win\winsound.obj
echo library %WATCOM%\lib286\win\windows.lib, %WATCOM%\lib286\win\mmsystem.lib, %WATCOM%\lib286\win\clibl.lib
echo export WEP, DRIVERPROC.1, WODMESSAGE.2, WIDMESSAGE.3, MODMESSAGE.4, MIDMESSAGE.5, AUXMESSAGE.6, MXDMESSAGE.7, TIMERPROC
) > "%OBJ%\win\winsound.lnk"
wlink @%OBJ%\win\winsound.lnk || goto fail

wcc %WS% -fo=%OBJ%\win\winbeep.obj win\winbeep.c || goto fail
(
echo format windows dll
echo name %OUT%\WINBEEP.DRV
echo option modname=SOUND
echo option description 'WINBEEP sound driver for Windows 3.0'
echo option quiet, map=%OBJ%\win\winbeep.map
echo segment type CODE preload fixed
echo segment type DATA preload fixed
echo file %OBJ%\win\winsndlb.obj, %OBJ%\win\winbeep.obj
echo library %WATCOM%\lib286\win\windows.lib, %WATCOM%\lib286\win\clibl.lib
echo import CREATESYSTEMTIMER SYSTEM.2, KILLSYSTEMTIMER SYSTEM.3, GETSYSTEMMSECCOUNT SYSTEM.6
echo export OPENSOUND.1, CLOSESOUND.2, SETVOICEQUEUESIZE.3, SETVOICENOTE.4, SETVOICEACCENT.5
echo export SETVOICEENVELOPE.6, SETSOUNDNOISE.7, SETVOICESOUND.8, STARTSOUND.9, STOPSOUND.10
echo export WAITSOUNDSTATE.11, SYNCALLVOICES.12, COUNTVOICENOTES.13, GETTHRESHOLDEVENT.14
echo export GETTHRESHOLDSTATUS.15, SETVOICETHRESHOLD.16, DOBEEP.17, MYOPENSOUND.18, WEP.19
echo export ENABLE.20, DISABLE.21
) > "%OBJ%\win\winbeep.lnk"
wlink @%OBJ%\win\winbeep.lnk || goto fail

wcc -q -ml -zw -bt=windows -ox -s -wx -I%WATCOM%\h\win -Iinc -fo=%OBJ%\win\winmix.obj win\winmix.c || goto fail
wrc -q -r -bt=windows -i=win -i=%WATCOM%\h -i=%WATCOM%\h\win -fo=%OBJ%\win\winmix.res win\winmix.rc || goto fail
(
echo system windows
echo name %OUT%\WINMIX.EXE
echo option quiet, heapsize=1024, stack=8192
echo option description 'WINMIX volume control for WINAUDIO and WINUSB sound devices'
echo file %OBJ%\win\winmix.obj
echo option resource=%OBJ%\win\winmix.res
) > "%OBJ%\win\winmix.lnk"
wlink @%OBJ%\win\winmix.lnk || goto fail

wcc %DF% -fo=%OBJ%\tools\audtest.obj tools\audtest.c || goto fail
wlink system dos name %OUT%\AUDTEST.EXE file %OBJ%\tools\audtest.obj option quiet, stack=8192 || goto fail

rem ---- bin\DOS, bin\WIN3X, bin\WIN9X ----
echo build: sorting the files
%PY% buildtools\oslayout.py bin-layout.txt "%OUT%" "%BINDIR%" || goto fail
echo build: done
popd
exit /b 0

rem ---- helpers ----
:vxdlink
(
echo format windows vxd %~3
echo name %~1
echo option nodefaultlibs, quiet, offset=0, map=%OBJ%\audio\vxd\vxd.map
echo library %WATCOM%\lib386\dos\clib3r.lib
echo option description 'WINAUDIO HD Audio, AC97 and Intel DSP sound driver for Windows 3.x and 95/98/ME'
echo segment _LTEXT preload nondiscardable
echo segment _LDATA preload nondiscardable
echo export WINCORE_DDB.1
echo file %~2
for %%f in (%COMMON% %AUDSRC%) do echo file %OBJ%\audio\vxd\%%f.obj
echo file %OBJ%\audio\vxd\platvxd.obj
for %%f in (pnp pnpaud dsvxd) do echo file %OBJ%\audio\vxd\%%f.obj
) > "%OBJ%\audio\vxd\vxd.lnk"
wlink @%OBJ%\audio\vxd\vxd.lnk || goto fail
goto :eof

:not_v2
echo build: %WATCOM% is not Open Watcom v2.
for /f "delims=" %%v in ('wcc386 2^>^&1 ^| findstr /c:"Version"') do echo build: found %%v
echo Install Open Watcom v2 from https://github.com/open-watcom/open-watcom-v2/releases
echo and set WATCOM to its folder, for example: set WATCOM=C:\WATCOM
goto fail

:old_watcom
echo build: this Open Watcom has no lib286\win\mmsystem.lib.
echo Install Open Watcom v2 from https://github.com/open-watcom/open-watcom-v2/releases.
goto fail

:no_python
echo build: Python 3 was not found.
echo Install Python 3 and put python on PATH, or set PYTHON to python.exe.
goto fail

:no_nasm
echo build: NASM was not found on PATH.
echo Install NASM from https://www.nasm.us/.
goto fail

:no_audio
echo build: the WINAUDIO sources are not in this folder.
goto fail

:no_layout
echo build: bin-layout.txt is missing.
goto fail

:fail
echo build: failed.
popd
exit /b 1
