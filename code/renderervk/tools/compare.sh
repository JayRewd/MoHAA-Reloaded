#!/bin/sh
#
# compare.sh <view list> [+set args...]
#
# Turns the view list into a cfg (shots.py), runs it with GL1 and with
# Vulkan (run.sh) and compares every screenshot with tgadiff.py. Results:
#   $MOH_HOMEPATH/main/screenshots/<view>[_w]_{gl,vk}.tga
#   $MOH_OUT/<view>[_w]_{gl,vk,diff}.png
#   $MOH_OUT/<list>[_w].txt (the statistics of every view)
# WIDE=1 runs at 4160x240: wider than 4096 pixels both radeonsi and RADV
# rasterize with 1/256 pixel precision, so the frames can be compared to the
# bit (see README.md, Testing). DIFFARGS is passed to tgadiff.py.

. "$(dirname "$0")/env.sh"

LIST=$1; shift
CFG=$(basename "$LIST" .txt)
TAG=
if [ -n "$WIDE" ]; then
	TAG=_w
	set -- +set r_mode -1 +set r_customwidth 4160 +set r_customheight 240 "$@"
fi
SS=$MOH_HOMEPATH/main/screenshots
python3 "$TOOLS/shots.py" "$LIST" "$CFG" >/dev/null || exit 2
VIEWS=$(awk '$1 == "shot" { print $2 }' "$LIST")

for R in opengl1 vulkan; do
	for v in $VIEWS; do rm -f "$SS/$v.tga"; done
	"$TOOLS/run.sh" $R "$CFG" "$@"
	S=gl; [ $R = vulkan ] && S=vk
	for v in $VIEWS; do
		if [ -f "$SS/$v.tga" ]; then
			mv "$SS/$v.tga" "$SS/$v${TAG}_$S.tga"
		else
			echo "missing $v ($R)"
		fi
	done
done

SUMMARY=$MOH_OUT/$CFG$TAG.txt
: > "$SUMMARY"
for v in $VIEWS; do
	echo "== $v$TAG" | tee -a "$SUMMARY"
	python3 "$TOOLS/tgadiff.py" "$SS/$v${TAG}_gl.tga" "$SS/$v${TAG}_vk.tga" --diff "$MOH_OUT/$v${TAG}_diff.png" \
		--png-a "$MOH_OUT/$v${TAG}_gl.png" --png-b "$MOH_OUT/$v${TAG}_vk.png" $DIFFARGS | tee -a "$SUMMARY"
done
