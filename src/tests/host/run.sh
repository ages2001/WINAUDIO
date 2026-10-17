#!/bin/bash
# run.sh [file.mid]: sample rate conversion and format tests of the sound engine, and a
# MIDI file through the synthesizer (output: out-*.raw, 16-bit stereo 48 kHz)
T=$(cd "$(dirname "$0")" && pwd)
cd "$T"
./build.sh || exit 1
python3 - <<'PY'
import numpy as np
sr=44100; t=np.arange(sr*2)/sr
np.stack([np.sin(2*np.pi*1000*t)*20000, np.sin(2*np.pi*440*t)*20000],1).astype('<i2').tofile('in-s44.raw')
sr=22050; t=np.arange(sr*2)/sr
(np.sin(2*np.pi*1000*t)*100+128).astype('u1').tofile('in-m22.raw')
sr=96000; t=np.arange(sr*2)/sr
np.stack([np.sin(2*np.pi*3000*t)*0.5]*2,1).astype('<f4').tofile('in-f96.raw')
PY
./harness out-s44.raw 48000 pcm in-s44.raw 44100 16 2 | tail -1
./harness out-m22.raw 48000 pcm in-m22.raw 22050 8 1 | tail -1
./harness out-f96.raw 48000 pcm in-f96.raw 96000 32 2 f | tail -1
python3 ana.py
if [ -n "$1" ]; then
  python3 mid2ev.py "$1" > midi.ev && ./harness out-midi.raw 48000 midi midi.ev > /dev/null && \
  python3 -c "
import numpy as np
x=np.fromfile('out-midi.raw','<i2').reshape(-1,2).astype(float)
print('MIDI: %.1f s, peak %d, rms %d' % (len(x)/48000, abs(x).max(), (x**2).mean()**0.5))"
fi
