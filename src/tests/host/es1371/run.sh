#!/bin/bash
# run.sh: builds and runs the ES1371 emulation test, then checks the sound it played and recorded
T=$(cd "$(dirname "$0")" && pwd)
cd "$T"
./build.sh || exit 1
./es1371 | grep -v "^es137x:\|^audio " || exit 1
python3 - <<'PY'
import numpy as np, sys
def tone(x, sr):
    f = np.abs(np.fft.rfft(x * np.hanning(len(x))))
    return np.fft.rfftfreq(len(x), 1 / sr)[np.argmax(f)]
d = np.fromfile('dac2.raw', '<i2').reshape(-1, 2).astype(float)
loud = np.where(np.abs(d[:, 0]) > 100)[0]
s = d[loud[0]:loud[-1]]
fl, fr = tone(s[:, 0], 48000), tone(s[:, 1], 48000)
gaps = np.sum(np.abs(np.diff(s[:, 0], 2)) > 2000)
print('DAC2: %.2f s of tone, left %.1f Hz, right %.1f Hz, discontinuities %d' % (len(s) / 48000, fl, fr, gaps))
r = np.fromfile('rec.raw', '<i2').reshape(-1, 2).astype(float)[4800:]
rl, rr = tone(r[:, 0], 48000), tone(r[:, 1], 48000)
print('ADC: %.2f s recorded, left %.1f Hz, right %.1f Hz, level %d' % (len(r) / 48000, rl, rr, np.abs(r).max()))
ok = abs(fl - 440) < 2 and abs(fr - 880) < 2 and gaps == 0 and abs(rl - 1000) < 3 and abs(rr - 2000) < 3
print('sound checks passed' if ok else 'sound checks FAILED')
sys.exit(0 if ok else 1)
PY
