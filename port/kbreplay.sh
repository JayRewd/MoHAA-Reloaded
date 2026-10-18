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
#   port/kbreplay.sh dialog <mode>     Sys_Dialog (dlgtest, Phase 6e) with the game up and the mouse captured: what
#                                      covers the dialog, the cursor, the display; the logs side by side, no verdict
#                                      (the layers differ on purpose). <mode>: win (a 1280x720 window), full
#                                      (exclusive fullscreen at the desktop's 1920x1080), full720 (exclusive at
#                                      1280x720: the display mode switched and the displays captured), space
#                                      (borderless fullscreen, a Space), start (no window: the startup's "Abnormal
#                                      Exit" question, from an empty PID file)
#   port/kbreplay.sh url               the openmohaa:// handler (urltest, Phase 6e): builds kburl (native, compiled
#                                      with PROTOCOL_HANDLER="openmohaa", which the shipped flavors leave out as
#                                      upstream does) from build's tree, then sends the game's own process link
#                                      Apple events; connect is replaced by a logger, nothing connects
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

	build_flavor kbsdl -DUSE_SDL=ON
	build_flavor kbnat -DUSE_SDL=OFF
}

# build_flavor <flavor> <cmake args>: one scratch flavor from build's tree
build_flavor() {
	local flavor=$1
	shift
	[ -d "$OMM_BUILD/kbreplay-src" ] || { echo "kbreplay.sh: run port/kbreplay.sh build first" >&2; exit 1; }
	export GIT_CEILING_DIRECTORIES="$OMM_BUILD"
	omm_mount_metal
	"$OMM_CMAKE" -S "$OMM_BUILD/kbreplay-src" -B "$OMM_BUILD/$flavor" -G Ninja \
		-DCMAKE_MAKE_PROGRAM="$OMM_NINJA" \
		-DBISON_EXECUTABLE="$OMM_BISON" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$OMM_BUILD/$flavor-install" \
		-DCMAKE_DISABLE_FIND_PACKAGE_OpenAL=ON \
		"$@" >/dev/null
	"$OMM_CMAKE" --build "$OMM_BUILD/$flavor" -j"$(sysctl -n hw.ncpu)"
	"$OMM_CMAKE" --install "$OMM_BUILD/$flavor" >/dev/null
	echo "kbreplay.sh: $flavor installed in $OMM_BUILD/$flavor-install"
}

# url: the openmohaa:// handler, compiled in (the C and Objective-C sources see the define; ninja's shell takes the
# backslashes off the quotes)
url() {
	build_flavor kburl -DUSE_SDL=OFF -DCMAKE_C_FLAGS='-DPROTOCOL_HANDLER=\"openmohaa\"' \
		-DCMAKE_OBJC_FLAGS='-DPROTOCOL_HANDLER=\"openmohaa\"'
	game kburl urltest kburl-url +set in_mouse 0 "$@"
	echo "kbreplay.sh: kburl-url: $(grep -c 'Sys_ParseProtocolUri' "$OUT/kburl-url-game.log")" \
		"Sys_ParseProtocolUri line(s) in the game's log:"
	grep 'Sys_ParseProtocolUri' "$OUT/kburl-url-game.log" || true
	cat "$OUT/kbreplay-kburl-url.log"
}

# game <flavor> <command> <name> [engine args]: one run of `<command> <name>` in dm/mohdm6 in a 1280x720 window
# (unless the engine args say otherwise); its log goes to $OUT/kbreplay-<name>.log
game() {
	local flavor=$1 command=$2 name=$3 log="kbreplay-$3.log"
	shift 3
	# the same start for every run: no saved config, the run's name in the cfg
	rm -rf "$OMM_HOME/main/configs"
	printf 'wait 3000\nfinishloadingscreen\nwait 500\n%s %s\n' "$command" "$name" > "$OMM_HOME/main/kb_replay.cfg"
	rm -f "$OMM_HOME/$log"
	perl -e 'alarm shift; exec @ARGV' 120 "$OMM_PORT/run.sh" "$flavor" \
		+set r_mode -1 +set r_customwidth 1280 +set r_customheight 720 +set r_fullscreen 0 \
		+set com_maxfps 250 +set com_maxfpsUnfocused 0 +set s_volume 0 +set s_musicvolume 0 \
		+set g_gametype 0 +set thereisnomonkey 1 +set ui_console 1 "$@" +devmap dm/mohdm6 +exec kb_replay.cfg \
		> "$OUT/$name-game.log" 2>&1 || echo "kbreplay.sh: $flavor exited with $?"
	rm -f "$OMM_HOME/main/kb_replay.cfg"
	cp "$OMM_HOME/$log" "$OUT/$log"
}

# replay <command> <label> [engine args]: runs `<command> <flavor><label>` in both flavors, then the diff
replay() {
	local command=$1 label=$2 flavor log
	shift 2
	for flavor in kbsdl kbnat; do
		log="kbreplay-$flavor$label.log"
		game "$flavor" "$command" "$flavor$label" "$@"
		echo "kbreplay.sh: $flavor$label: $(grep -c '^step' "$OUT/$log") steps," \
			"$(grep -c 'SE_' "$OUT/$log") events, quit: $(grep -c 'Closed window' "$OUT/$flavor$label-game.log")" \
			"'Closed window' line(s)"
	done
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

# dialog_start: the startup's "Abnormal Exit" question (an empty PID file is a stale one), answered No by the
# OMM_DLGTEST timer; run.sh would remove the PID file, so the app is started the way run.sh starts it (its pak links
# are there from the other runs)
dialog_start() {
	local flavor name log app
	for flavor in kbsdl kbnat; do
		name="$flavor-dialog-start"
		log="$OUT/kbreplay-$name.log"
		app="$OMM_BUILD/$flavor-install/lib/openmohaa/openmohaa.app/Contents/MacOS/openmohaa"
		rm -rf "$OMM_HOME/main/configs"
		printf 'wait 3000\nquit\n' > "$OMM_HOME/main/kb_replay.cfg"
		: > "$OMM_HOME/main/OpenMoHAA.pid"
		OMM_DLGTEST="$log" perl -e 'alarm shift; exec @ARGV' 120 "$app" +set fs_homepath "$OMM_HOME" \
			+set r_mode -1 +set r_customwidth 1280 +set r_customheight 720 +set r_fullscreen 0 \
			+set s_volume 0 +set s_musicvolume 0 +exec kb_replay.cfg \
			> "$OUT/$name-game.log" 2>&1 || echo "kbreplay.sh: $flavor exited with $?"
		rm -f "$OMM_HOME/main/kb_replay.cfg"
		echo "kbreplay.sh: $name: $(grep -c 'Abnormal\|abnormal' "$OUT/$name-game.log") abnormal-exit line(s)," \
			"PID file left: $(ls "$OMM_HOME"/main/*.pid 2>/dev/null | wc -l | tr -d ' ')"
		cat "$log"
	done
}

dialog() {
	local mode=${1:-} flavor args
	shift || true
	case "$mode" in
		win) args=() ;;
		full) args=(+set r_customwidth 1920 +set r_customheight 1080 +set r_fullscreen 1) ;;
		full720) args=(+set r_fullscreen 1) ;;
		space) args=(+set r_customwidth 1920 +set r_customheight 1080 +set r_fullscreen 1 +set r_noborder 1) ;;
		start) dialog_start; return ;;
		*) echo "usage: port/kbreplay.sh dialog win|full|full720|space|start [engine args]" >&2; exit 1 ;;
	esac
	for flavor in kbsdl kbnat; do
		game "$flavor" dlgtest "$flavor-dialog-$mode" +set in_mouse 1 +set in_mouseRaw 0 ${args[@]+"${args[@]}"} "$@"
		echo "kbreplay.sh: $flavor-dialog-$mode:"
		cat "$OUT/kbreplay-$flavor-dialog-$mode.log"
	done
}

case "${1:-}" in
	build) build ;;
	run) shift; run "$@" ;;
	mouse) shift; mouse "$@" ;;
	dialog) shift; dialog "$@" ;;
	url) shift; url "$@" ;;
	*) echo "usage: port/kbreplay.sh build | run [engine args] | mouse win|full|hidpi [engine args] |" \
		"dialog win|full|full720|space|start [engine args] | url [engine args]" >&2; exit 1 ;;
esac
