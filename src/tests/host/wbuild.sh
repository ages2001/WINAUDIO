#!/bin/bash
# wbuild.sh: WSNDT.EXE, the Win16 test of WINSOUND.DRV through MMSYSTEM
cd "$(dirname "$0")"
export WATCOM=${WATCOM:-$(pwd)/../../../ow}
export PATH=$WATCOM/binl64:$WATCOM/binl:$PATH INCLUDE=$WATCOM/h:$WATCOM/h/win
set -e
wcc -q -ml -zw -bt=windows -ox -s -I$WATCOM/h/win -fo=wsndt.obj wsndt.c
wlink system windows name WSNDT.EXE file wsndt.obj library mmsystem.lib option quiet, heapsize=4096, stack=16384
wcc -q -ml -zw -bt=windows -ox -s -I$WATCOM/h/win -fo=beept.obj beept.c
wlink system windows name BEEPT.EXE file beept.obj option quiet, heapsize=2048, stack=8192
