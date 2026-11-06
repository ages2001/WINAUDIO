#!/bin/bash
# build.sh: the host test of es137x.c against the emulated ES1371 (Open Watcom, Linux target).
# The core's sources are compiled from a copy whose core.h has no inline port / interrupt
# instructions: the test program supplies inb / outl / irq_save ... as functions.
T=$(cd "$(dirname "$0")" && pwd)
S=$(cd "$T/../../.." && pwd)
export WATCOM=${WATCOM:-/opt/watcom}
export PATH=$WATCOM/binl64:$PATH INCLUDE=$WATCOM/lh
set -e
cd "$T"
rm -rf gen && mkdir gen
cp $S/core/*.h gen/
for f in es137x ac97 aeng synth; do cp $S/core/$f.c gen/; done
python3 - gen/core.h <<'PY'
import re, sys
p = sys.argv[1]
s = open(p, newline='').read()
s = re.sub(r'#pragma aux (inb|inw|inl|outb|outw|outl|irq_save|irq_restore) =[^\n]*\n', '', s)
open(p, 'w', newline='').write(s)
PY
CF="-q -bt=linux -ox -zp4 -I$T/gen -I$S/inc -wx"
for f in es137x ac97 aeng synth; do wcc386 $CF -fo=$f.o gen/$f.c; done
wcc386 $CF -fo=es1371.o es1371.c
wlink system linux name es1371 file es1371.o,es137x.o,ac97.o,aeng.o,synth.o option quiet
