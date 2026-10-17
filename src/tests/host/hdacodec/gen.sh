#!/bin/sh
# extract the codec parser of hda.c for the host harness
cd "$(dirname "$0")"
S=../../../core/hda.c
sed -n '/^\/\* widget types/,/^static struct hda \*ctrls/p' $S | sed '$d' > hdadefs.c
sed -n '/^static u32 param/,/^\/\* -* streams/p' $S | sed '$d' > hdapart.c
