#!/bin/sh
# run.sh dump... : runs the HDA codec parser of hda.c against Linux codec dumps
# (/proc/asound/card*/codec#*, alsa-info output) and checks the routes it sets up
cd "$(dirname "$0")"
./gen.sh && gcc -w -o hdacheck h.c || exit 1
for f in "$@"; do
    python3 conv.py "$f" > dump.txt 2>/dev/null || { echo "== $f: no Realtek codec"; continue; }
    echo "== $f"
    ./hdacheck dump.txt; echo "rc=$?"
done
rm -f dump.txt
