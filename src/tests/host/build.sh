#!/bin/bash
# build.sh: builds the host test of the sound engine (Open Watcom, Linux target)
T=$(cd "$(dirname "$0")" && pwd)
S=$(cd "$T/../.." && pwd)
export WATCOM=${WATCOM:-/opt/watcom}
export PATH=$WATCOM/binl64:$PATH INCLUDE=$WATCOM/lh
set -e
cd "$T"
CF="-q -bt=linux -ox -zp4 -I$S/core -I$S/inc -wx"
wcc386 $CF -fo=aeng.o $S/core/aeng.c
wcc386 $CF -fo=synth.o $S/core/synth.c
wcc386 $CF -fo=harness.o harness.c
wlink system linux name harness file harness.o,aeng.o,synth.o option quiet
