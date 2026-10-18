#!/bin/bash
# The keyboard and mouse replay checks (Phase 6c, 6d): the same key or mouse events, posted into the game's own event
# queue (in-process NSEvents: nothing system-wide, no synthetic HID input), must give the same SE_KEY/SE_CHAR/
# SE_MOUSE stream through SDL and through the native AppKit layer. Two scratch flavors from the work tree plus
# port/kbreplay_patch.py (exported into build.noindex/kbreplay-src; the shipped flavors never have the replay):
#   port/kbreplay.sh build             kbsdl (SDL, the metal flavor's input) and kbnat (native, -DUSE_SDL=OFF)
#   port/kbreplay.sh run               the keys: both in dm/mohdm6 in a 1280x720 window, one after the other, then
#                                      the diff
#   port/kbreplay.sh mouse <mode>      the mouse (msreplay), the same way; <mode>: win (a 1280x720 window on the
#                                      main display), full (exclusive fullscreen at 1920x1080), hidpi (a window on
#                                      r_displayIndex 1, a 2x display: the UI's cursor is in pixels on the native
#                                      layer, in points on SDL, so those lines differ)
# The runs set ui_console 1: without it MOHAA's console key opens nothing. Each run opens the game for ~15 s and
# takes the keyboard focus: don't type meanwhile. The mouse runs capture the real cursor (warped to the window's
# center and hidden; fullscreen takes the screen): don't touch the mouse either. The logs and the diff are in
# build.noindex/kbreplay/ (kbreplay-<flavor>[-mouse-<mode>].log: the steps and the events each one queued).
set -euo pipefail
source "$(dirname "$0")/env.sh"

OUT="$OMM_BUILD/kbreplay"
mkdir -p "$OUT"

build() {
	local src="$OMM_BUILD/kbreplay-src"
	rm -rf "$src"
	mkdir -p "$src"
	git -C "$OMM_ROOT" archive HEAD | tar -x -C "$src"
	git -C "$OMM_ROOT" diff --name-only HEAD | while read -r f; do
		if [ -f "$OMM_ROOT/$f" ]; then cp "$OMM_ROOT/$f" "$src/$f"; else rm -f "$src/$f"; fi
	done
	python3 -I "$OMM_PORT/kbreplay_patch.py" "$src"

	export GIT_CEILING_DIRECTORIES="$OMM_BUILD"
	omm_mount_metal
	local flavor sdl
	for flavor in kbsdl kbnat; do
		[ "$flavor" = kbsdl ] && sdl=ON || sdl=OFF
		"$OMM_CMAKE" -S "$src" -B "$OMM_BUILD/$flavor" -G Ninja \
			-DCMAKE_MAKE_PROGRAM="$OMM_NINJA" \
			-DBISON_EXECUTABLE="$OMM_BISON" \
			-DCMAKE_BUILD_TYPE=Release \
			-DCMAKE_INSTALL_PREFIX="$OMM_BUILD/$flavor-install" \
			-DCMAKE_DISABLE_FIND_PACKAGE_OpenAL=ON \
			-DUSE_SDL=$sdl >/dev/null
		"$OMM_CMAKE" --build "$OMM_BUILD/$flavor" -j"$(sysctl -n hw.ncpu)"
		"$OMM_CMAKE" --install "$OMM_BUILD/$flavor" >/dev/null
		echo "kbreplay.sh: $flavor installed in $OMM_BUILD/$flavor-install"
	done
}

# replay <command> <label> [engine args]: runs `<command> <flavor><label>` in both flavors, then the diff
replay() {
	local command=$1 label=$2 flavor log
	shift 2
	for flavor in kbsdl kbnat; do
		# the same start for both: no saved config, the replay's name in the cfg
		log="kbreplay-$flavor$label.log"
		rm -rf "$OMM_HOME/main/configs"
		printf 'wait 3000\nfinishloadingscreen\nwait 500\n%s %s\n' "$command" "$flavor$label" \
			> "$OMM_HOME/main/kb_replay.cfg"
		rm -f "$OMM_HOME/$log"
		perl -e 'alarm shift; exec @ARGV' 120 "$OMM_PORT/run.sh" "$flavor" \
			+set r_mode -1 +set r_customwidth 1280 +set r_customheight 720 +set r_fullscreen 0 \
			+set com_maxfps 250 +set com_maxfpsUnfocused 0 +set s_volume 0 +set s_musicvolume 0 \
			+set g_gametype 0 +set thereisnomonkey 1 +set ui_console 1 "$@" +devmap dm/mohdm6 +exec kb_replay.cfg \
			> "$OUT/$flavor$label-game.log" 2>&1 || echo "kbreplay.sh: $flavor exited with $?"
		cp "$OMM_HOME/$log" "$OUT/$log"
		echo "kbreplay.sh: $flavor$label: $(grep -c '^step' "$OUT/$log") steps," \
			"$(grep -c 'SE_' "$OUT/$log") events, quit: $(grep -c 'Closed window' "$OUT/$flavor$label-game.log")" \
			"'Closed window' line(s)"
	done
	rm -f "$OMM_HOME/main/kb_replay.cfg"
	# The header line names the flavor and the wait for the game depends on the load: the steps must match up to
	# the input shutdown at quit. After it, the native layer queues the release of the keys and buttons still
	# down (Cmd, Q, a mouse button), which SDL lost: shown, not compared.
	if diff <(sed -e '1d' -e '/^input shutdown/,$d' "$OUT/kbreplay-kbsdl$label.log" | grep -v '^waiting') \
		<(sed -e '1d' -e '/^input shutdown/,$d' "$OUT/kbreplay-kbnat$label.log" | grep -v '^waiting') \
		> "$OUT/diff$label.txt"; then
		echo "kbreplay.sh: the streams are identical"
	else
		echo "kbreplay.sh: the streams differ ($OUT/diff$label.txt):"
		head -60 "$OUT/diff$label.txt"
	fi
	for flavor in kbsdl kbnat; do
		echo "kbreplay.sh: $flavor$label after the input shutdown:" \
			$(sed -n '/^input shutdown/,$p' "$OUT/kbreplay-$flavor$label.log" | tail -n +2)
	done
}

run() {
	replay kbreplay "" +set in_mouse 0 "$@"
}

mouse() {
	local mode=${1:-}
	shift || true
	case "$mode" in
		win) replay msreplay -mouse-win +set in_mouse 1 +set in_mouseRaw 0 "$@" ;;
		full) replay msreplay -mouse-full +set in_mouse 1 +set in_mouseRaw 0 +set r_customwidth 1920 \
			+set r_customheight 1080 +set r_fullscreen 1 "$@" ;;
		hidpi) replay msreplay -mouse-hidpi +set in_mouse 1 +set in_mouseRaw 0 +set r_displayIndex 1 "$@" ;;
		*) echo "usage: port/kbreplay.sh mouse win|full|hidpi [engine args]" >&2; exit 1 ;;
	esac
}

case "${1:-}" in
	build) build ;;
	run) shift; run "$@" ;;
	mouse) shift; mouse "$@" ;;
	*) echo "usage: port/kbreplay.sh build | run [engine args] | mouse win|full|hidpi [engine args]" >&2; exit 1 ;;
esac
