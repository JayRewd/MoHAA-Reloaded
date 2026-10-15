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
# Shots taken from outside the world (view cluster -1, see shots.py) are
# reported: parts of their frame are undefined. So are all-black GL1 frames
# (Mesa now and then hands out a back buffer that was never drawn): run the
# list again.
# A <list>.shader file next to the view list is installed as
# main/scripts/aaa_<list>.shader for the runs (the first shader file in
# alphabetical order overrides the others), to test shaders no game data has.

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
SHADER=${LIST%.txt}.shader
INSTALLED=
if [ -f "$SHADER" ]; then
	INSTALLED=$MOH_HOMEPATH/main/scripts/aaa_$CFG.shader
	mkdir -p "$MOH_HOMEPATH/main/scripts"
	cp "$SHADER" "$INSTALLED"
fi
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

[ -n "$INSTALLED" ] && rm -f "$INSTALLED"

awk '/^cluster:/ { c = $1 } $1 == "shot" && c == "cluster:-1" { print "warning: " $2 ": the camera is outside the world (cluster -1)" }' \
	"$MOH_OUT/${CFG}_vulkan.log"

for v in $VIEWS; do
	f=$SS/$v${TAG}_gl.tga
	[ -f "$f" ] || continue
	# pixels after the 18-byte header, all zero
	if [ "$(tail -c +19 "$f" | tr -d '\000' | head -c 1 | wc -c)" -eq 0 ]; then
		echo "warning: $v: the GL1 frame is all black, run the list again"
	fi
done

SUMMARY=$MOH_OUT/$CFG$TAG.txt
: > "$SUMMARY"
for v in $VIEWS; do
	echo "== $v$TAG" | tee -a "$SUMMARY"
	python3 "$TOOLS/tgadiff.py" "$SS/$v${TAG}_gl.tga" "$SS/$v${TAG}_vk.tga" --diff "$MOH_OUT/$v${TAG}_diff.png" \
		--png-a "$MOH_OUT/$v${TAG}_gl.png" --png-b "$MOH_OUT/$v${TAG}_vk.png" $DIFFARGS | tee -a "$SUMMARY"
done
