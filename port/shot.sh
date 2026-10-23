#!/bin/bash
# Screenshots for comparing the renderers (GL1 is the reference). Runs one install once with a cfg that takes
# in-game `screenshot`s, in a 1280x720 window with a fresh default config, and collects them: Metal from the metal
# install, GL1 from the GL1 reference install (port/build.sh gl: the last commit that had GL1).
#   port/shot.sh <renderer> <name> <cfg> [engine args...]
#       renderer: metal | opengl1;  cfg: a file in port/shots/ (copied into the run home; prefix shot_: the game itself execs e.g. menu.cfg)
#       -> build.noindex/shots/<name>-<renderer>-<n>.tga (+ .png to look at) and <name>-<renderer>.log
#   port/shot.sh diff <name>   per-pixel difference of every <name>-opengl1-<n> / <name>-metal-<n> pair
#                              (diff images: <name>-diff-<n>.png, differences x16)
#   port/shot.sh pair <name> <cfg> [engine args...]   fixed 3D views (shot_dm6.cfg, shot_dm5.cfg, shot_dm6fx.cfg,
#                              shot_dm4.cfg, shot_obj3.cfg, shot_m2l1.cfg, shot_sh.cfg, shot_bt.cfg, shot_texmodes.cfg,
#                              shot_debug.cfg, shot_cinematic.cfg): both renderers, then the diff of each view; the cfg's
#                              "// args:" line gives the engine args. -> <name>-view<k>-{opengl1,metal,diff}.png
#   port/shot.sh fixed <renderer> <name> <cfg> [engine args...]   one renderer of a pair
#   port/shot.sh views <name>  the per-view diffs again (after re-running one renderer with `fixed`)
#   port/shot.sh savebase [dir]   keep the current Metal captures of the six fixed views (dm6 dm5 dm6fx m2l1 dm4 obj3)
#                              in build.noindex/shots/<dir> (default base) as the baseline for regress
#   port/shot.sh regress [dir] [engine args...]   the six fixed views on Metal only: per-view diffs against GL1, and every capture
#                              against the saved Metal baseline (for changes meant to be pixel-identical; ~8 min)
# Examples:
#   port/shot.sh metal menu shot_menu.cfg && port/shot.sh opengl1 menu shot_menu.cfg && port/shot.sh diff menu
#   port/shot.sh metal console shot_console.cfg +set ui_console 1 +set cl_playintro 0 +set developer 1
#   port/shot.sh metal view shot_view.cfg +set g_gametype 0 +set cg_drawviewmodel 0 +set cheats 1 +devmap dm/mohdm6
#   port/shot.sh pair dm6 shot_dm6.cfg
# OMM_FLAVOR=<flavor> runs another install than build.noindex/metal-install for Metal (e.g. a scratch build with other
# flags), OMM_GL_FLAVOR=<flavor> another one than build.noindex/gl-install for GL1.
# Anything time-based (animated menu items, flickering lights, timing text in the console) differs between runs,
# except in the fixed views: they run with fixedtime, which makes game time advance the same per frame in every run.
# Their cfgs wait in real milliseconds, so the frame rate sets how much game time passes before each capture:
# com_maxfps is pinned at 85 (upstream's default, every baseline's; the game's is 125 since session 66), as
# r_uselod at 1.
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

if [ "${1:-}" = views ]; then
	# Fixed views take three captures per view, one frame apart (see shot_dm6.cfg). Metal's middle capture shows the
	# frame before it; GL1's capture j shows the frame j frames before it, so the GL1 capture that matches best tells
	# GL1's frame age (usually 2) -> <name>-view<k>-{opengl1,metal}.tga and the diff <name>-view<k>-diff.png.
	NAME="$2"
	k=0
	while [ -e "$OUT/$NAME-metal-$((3 * k + 1)).tga" ] && [ -e "$OUT/$NAME-opengl1-$((3 * k + 2)).tga" ]; do
		V="$OUT/$NAME-view$k"
		best=-1; bestn=-1
		for j in 0 1 2; do
			n=$(python3 -I "$OMM_PORT/tgadiff.py" "$OUT/$NAME-opengl1-$((3 * k + j)).tga" "$OUT/$NAME-metal-$((3 * k + 1)).tga" | sed 's/.*>0 \([0-9]*\).*/\1/')
			if [ "$bestn" -lt 0 ] || [ "$n" -lt "$bestn" ]; then best=$j; bestn=$n; fi
		done
		cp "$OUT/$NAME-opengl1-$((3 * k + best)).tga" "$V-opengl1.tga"; cp "$OUT/$NAME-opengl1-$((3 * k + best)).png" "$V-opengl1.png"
		cp "$OUT/$NAME-metal-$((3 * k + 1)).tga" "$V-metal.tga"; cp "$OUT/$NAME-metal-$((3 * k + 1)).png" "$V-metal.png"
		printf '%s view %d (GL1 frame age %d): ' "$NAME" "$k" "$best"
		python3 -I "$OMM_PORT/tgadiff.py" "$V-opengl1.tga" "$V-metal.tga" "$V-diff.tga"
		sips -s format png "$V-diff.tga" --out "$V-diff.png" >/dev/null 2>&1
		k=$((k + 1))
	done
	exit 0
fi

if [ "${1:-}" = savebase ]; then
	# Keep the current Metal captures of the six fixed views as a baseline for regress.
	BASE="$OUT/${2:-base}"
	mkdir -p "$BASE"
	for n in dm6 dm5 dm6fx m2l1 dm4 obj3; do cp "$OUT/$n"-metal-*.tga "$BASE/"; done
	echo "savebase: $(ls "$BASE" | wc -l | tr -d ' ') captures in $BASE"
	exit 0
fi

if [ "${1:-}" = regress ]; then
	# The six fixed views on Metal only (GL1's side never changes), diffed against GL1 per view and, capture by
	# capture, against a saved Metal baseline: after a change meant to be pixel-identical, only the views that vary
	# from run to run differ (dm6 view 8, dm5's snow, dm6fx views 4-7, m2l1 view 0 by up to ~17 pixels).
	# Engine args after the baseline dir go to every Metal run (e.g. +set r_metalStaticWorld 2).
	BASE="$OUT/${2:-base}"
	EXTRA=("${@:3}")
	for p in "dm6 shot_dm6.cfg" "dm5 shot_dm5.cfg" "dm6fx shot_dm6fx.cfg" "m2l1 shot_m2l1.cfg" "dm4 shot_dm4.cfg" "obj3 shot_obj3.cfg"; do
		set -- $p
		"$0" fixed metal "$1" "$2" ${EXTRA[@]+"${EXTRA[@]}"}
		"$0" views "$1"
		for f in "$OUT/$1"-metal-*.tga; do
			printf 'vs %s %s: ' "${BASE##*/}" "$(basename "$f" .tga)"
			python3 -I "$OMM_PORT/tgadiff.py" "$BASE/$(basename "$f")" "$f"
		done
	done
	exit 0
fi

if [ "${1:-}" = fixed ]; then
	R="$2"; NAME="$3"; CFG="$4"; shift 4
	ARGS=$(sed -n 's|^// args: ||p' "$OMM_PORT/shots/$CFG" | head -1)
	# shellcheck disable=SC2086
	exec "$0" "$R" "$NAME" "$CFG" $ARGS "$@"
fi

if [ "${1:-}" = pair ]; then
	NAME="$2"; CFG="$3"; shift 3
	"$0" fixed opengl1 "$NAME" "$CFG" "$@" && "$0" fixed metal "$NAME" "$CFG" "$@" && exec "$0" views "$NAME"
	exit 1
fi

R="$1"; NAME="$2"; CFG="$3"; shift 3
[ -f "$OMM_PORT/shots/$CFG" ] && cp "$OMM_PORT/shots/$CFG" "$OMM_HOME/main/"
# screenshots land in the game dir: main/, or mainta/ / maintt/ with com_target_game 1/2 (Spearhead/Breakthrough)
rm -rf "$OMM_HOME"/main*/screenshots "$OMM_HOME/main/configs" "$OUT/$NAME-$R"-* "$OUT/$NAME-view"*-"$R".*
if [ "$R" = opengl1 ]; then FLAVOR="${OMM_GL_FLAVOR:-gl}"; else FLAVOR="${OMM_FLAVOR:-metal}"; fi
perl -e 'alarm shift; exec @ARGV' 120 "$OMM_PORT/run.sh" "$FLAVOR" +set cl_renderer "$R" \
	+set r_mode -1 +set r_customwidth 1280 +set r_customheight 720 +set r_fullscreen 0 \
	+set s_volume 0 +set s_musicvolume 0 +set r_primitives 2 +set r_uselod 1 +set com_maxfps 85 "$@" +exec "$CFG" > "$OUT/$NAME-$R.log" 2>&1
n=0
for f in "$OMM_HOME"/main*/screenshots/*.tga; do
	[ -e "$f" ] || continue
	cp "$f" "$OUT/$NAME-$R-$n.tga"
	sips -s format png "$f" --out "$OUT/$NAME-$R-$n.png" >/dev/null 2>&1
	n=$((n + 1))
done
echo "$NAME $R: $n screenshot(s) in $OUT"
