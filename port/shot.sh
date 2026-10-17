#!/bin/bash
# Screenshots for comparing the renderers (GL1 is the reference). Runs the metal install (GL1 + Metal) once with
# a cfg that takes in-game `screenshot`s, in a 1280x720 window with a fresh default config, and collects them.
#   port/shot.sh <renderer> <name> <cfg> [engine args...]
#       renderer: metal | opengl1;  cfg: a file in port/shots/ (copied into the run home; prefix shot_: the game itself execs e.g. menu.cfg)
#       -> build.noindex/shots/<name>-<renderer>-<n>.tga (+ .png to look at) and <name>-<renderer>.log
#   port/shot.sh diff <name>   per-pixel difference of every <name>-opengl1-<n> / <name>-metal-<n> pair
#                              (diff images: <name>-diff-<n>.png, differences x16)
# Examples:
#   port/shot.sh metal menu shot_menu.cfg && port/shot.sh opengl1 menu shot_menu.cfg && port/shot.sh diff menu
#   port/shot.sh metal console shot_console.cfg +set ui_console 1 +set cl_playintro 0
#   port/shot.sh metal view shot_view.cfg +set g_gametype 0 +set cg_drawviewmodel 0 +set cheats 1 +devmap dm/mohdm6
# Anything time-based (animated menu items, flickering lights, timing text in the console) differs between runs.
set -uo pipefail
source "$(dirname "$0")/env.sh"

OUT="$OMM_BUILD/shots"
mkdir -p "$OUT" "$OMM_HOME/main"

if [ "${1:-}" = diff ]; then
	NAME="$2"
	for ref in "$OUT/$NAME"-opengl1-*.tga; do
		n="${ref##*-}"; n="${n%.tga}"
		[ -e "$OUT/$NAME-metal-$n.tga" ] || continue
		printf '%s %s: ' "$NAME" "$n"
		python3 -I "$OMM_PORT/tgadiff.py" "$ref" "$OUT/$NAME-metal-$n.tga" "$OUT/$NAME-diff-$n.tga"
		sips -s format png "$OUT/$NAME-diff-$n.tga" --out "$OUT/$NAME-diff-$n.png" >/dev/null 2>&1
	done
	exit 0
fi

R="$1"; NAME="$2"; CFG="$3"; shift 3
[ -f "$OMM_PORT/shots/$CFG" ] && cp "$OMM_PORT/shots/$CFG" "$OMM_HOME/main/"
rm -rf "$OMM_HOME/main/screenshots" "$OMM_HOME/main/configs" "$OUT/$NAME-$R"-*
perl -e 'alarm shift; exec @ARGV' 120 "$OMM_PORT/run.sh" metal +set cl_renderer "$R" \
	+set r_mode -1 +set r_customwidth 1280 +set r_customheight 720 +set r_fullscreen 0 \
	+set s_volume 0 +set s_musicvolume 0 +set r_primitives 2 "$@" +exec "$CFG" > "$OUT/$NAME-$R.log" 2>&1
n=0
for f in "$OMM_HOME"/main/screenshots/*.tga; do
	[ -e "$f" ] || continue
	cp "$f" "$OUT/$NAME-$R-$n.tga"
	sips -s format png "$f" --out "$OUT/$NAME-$R-$n.png" >/dev/null 2>&1
	n=$((n + 1))
done
echo "$NAME $R: $n screenshot(s) in $OUT"
