#!/bin/bash
# The keyboard replay check (Phase 6c): the same key events, posted into the game's own event queue (in-process
# NSEvents: nothing system-wide, no synthetic HID input), must give the same SE_KEY/SE_CHAR stream through SDL and
# through the native AppKit layer. Two scratch flavors from the work tree plus port/kbreplay_patch.py (exported
# into build.noindex/kbreplay-src; the shipped flavors never have the replay):
#   port/kbreplay.sh build     kbsdl (SDL, the metal flavor's input) and kbnat (native, -DUSE_SDL=OFF)
#   port/kbreplay.sh run       both in dm/mohdm6 in a 1280x720 window, one after the other, then the diff
# The runs set ui_console 1: without it MOHAA's console key opens nothing. Each run opens the game for ~15 s and
# takes the keyboard focus: don't type meanwhile. The logs and the diff are in build.noindex/kbreplay/
# (kbreplay-<flavor>.log: the steps and the events each one queued).
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

run() {
	local flavor
	cat > "$OMM_HOME/main/kb_replay.cfg" <<'EOF'
wait 3000
finishloadingscreen
wait 500
kbreplay $FLAVOR
EOF
	for flavor in kbsdl kbnat; do
		# the same start for both: no saved config, the replay's name in the cfg
		rm -rf "$OMM_HOME/main/configs"
		sed -i '' "s/^kbreplay .*/kbreplay $flavor/" "$OMM_HOME/main/kb_replay.cfg"
		rm -f "$OMM_HOME/kbreplay-$flavor.log"
		perl -e 'alarm shift; exec @ARGV' 120 "$OMM_PORT/run.sh" "$flavor" \
			+set r_mode -1 +set r_customwidth 1280 +set r_customheight 720 +set r_fullscreen 0 \
			+set com_maxfps 250 +set com_maxfpsUnfocused 0 +set s_volume 0 +set s_musicvolume 0 +set in_mouse 0 \
			+set g_gametype 0 +set thereisnomonkey 1 +set ui_console 1 "$@" +devmap dm/mohdm6 +exec kb_replay.cfg \
			> "$OUT/$flavor-game.log" 2>&1 || echo "kbreplay.sh: $flavor exited with $?"
		cp "$OMM_HOME/kbreplay-$flavor.log" "$OUT/kbreplay-$flavor.log"
		echo "kbreplay.sh: $flavor: $(grep -c '^step' "$OUT/kbreplay-$flavor.log") steps," \
			"$(grep -c 'SE_' "$OUT/kbreplay-$flavor.log") events, quit: $(grep -c 'Closed window' "$OUT/$flavor-game.log")" \
			"'Closed window' line(s)"
	done
	rm -f "$OMM_HOME/main/kb_replay.cfg"
	# The header line names the flavor and the wait for the game depends on the load: the steps must match up to
	# the input shutdown at quit. After it, the native layer queues the release of the keys still down (Cmd and Q),
	# which SDL lost: shown, not compared.
	if diff <(sed -e '1d' -e '/^input shutdown/,$d' "$OUT/kbreplay-kbsdl.log" | grep -v '^waiting') \
		<(sed -e '1d' -e '/^input shutdown/,$d' "$OUT/kbreplay-kbnat.log" | grep -v '^waiting') > "$OUT/diff.txt"; then
		echo "kbreplay.sh: the streams are identical"
	else
		echo "kbreplay.sh: the streams differ ($OUT/diff.txt):"
		head -40 "$OUT/diff.txt"
	fi
	for flavor in kbsdl kbnat; do
		echo "kbreplay.sh: $flavor after the input shutdown:" $(sed -n '/^input shutdown/,$p' "$OUT/kbreplay-$flavor.log" | tail -n +2)
	done
}

case "${1:-}" in
	build) build ;;
	run) shift; run "$@" ;;
	*) echo "usage: port/kbreplay.sh build | run [engine args]" >&2; exit 1 ;;
esac
