#!/bin/bash
# Input-to-present latency in a live map: the cl_latencyStats probe with synthetic mouse movement.
#   port/latency.sh <renderer> <name> [seconds] [engine args...]
#       renderer: metal | opengl1 (Metal from the metal install, or OMM_FLAVOR's, e.g. OMM_FLAVOR=native; GL1 from
#       the GL1 reference install, port/build.sh gl);
#       seconds of mouse movement (default 20)
#       -> build.noindex/latency/<name>.log; prints the probe's whole-run numbers and the mouse tool's line
# Spawns in dm/mohdm6 (single-player mode on a local server, like the fixed views), turns the probe on, then
# port/mousemove.c posts 1000 Hz mouse moves at the HID event tap (the same WindowServer -> application path
# as a real mouse; see that file) while the probe runs, and the game quits. Window: 1280x720, vsync off,
# com_maxfps 250 (the user's setting) unless the engine args say otherwise, e.g.
#   port/latency.sh metal metal-fs-250 20 +set r_fullscreen 1 +set r_customwidth 1920 +set r_customheight 1080
#   port/latency.sh metal metal-bfs-250 20 +set r_fullscreen 1 +set r_noborder 1     (borderless fullscreen)
# The screen must be unlocked: while it is locked the WindowServer composites nothing (checked first).
# The game window must have the keyboard focus, or SDL ignores the mouse: don't click elsewhere during a run.
# Environment:
#   MOUSE=0     no mouse movement (to look at presents alone; the probe then has no input frames)
#   LATSTATS=2  cl_latencyStats 2: also log every frame's raw times (needed by TRACE's per-frame check);
#               LATSTATS=0 leaves the probe off (with MOUSE=0 and TRACE=1: only what the display showed)
#   TRACE=1     record 6 s of Instruments' Metal System Trace during the movement (-> <name>.trace, ~0.3 GB,
#               kept) and summarize what the display showed with port/display_trace.py: swaps per display,
#               direct to display or why not, presented-handler statuses, and the probe's present times
#               matched against the display swaps (with LATSTATS=2)
set -uo pipefail
source "$(dirname "$0")/env.sh"

R="${1:?renderer}"; NAME="${2:?name}"; SECS="${3:-20}"
[ $# -ge 3 ] && shift 3 || shift $#
OUT="$OMM_BUILD/latency"
TOOL="$OMM_BUILD/tools/mousemove"
mkdir -p "$OUT" "$OMM_BUILD/tools" "$OMM_HOME/main"

if ioreg -n Root -d1 -a | grep -A1 CGSSessionScreenIsLocked | grep -q true; then
	echo "latency.sh: the screen is locked; unlock it first" >&2
	exit 1
fi

if [ ! -x "$TOOL" ] || [ "$OMM_PORT/mousemove.c" -nt "$TOOL" ]; then
	clang -O2 -Wall -framework ApplicationServices -o "$TOOL" "$OMM_PORT/mousemove.c" || exit 1
fi

# Spawn, turn the probe on, tell the script, then keep running a little longer than the mouse moves.
cat > "$OMM_HOME/main/lat_probe.cfg" <<EOF
// written by port/latency.sh
wait 6000
finishloadingscreen
wait 2000
set cl_latencyStats ${LATSTATS:-1}
echo latency-probe-start
wait $(( (SECS + 4) * 1000 ))
quit
EOF

LOG="$OUT/$NAME.log"
rm -rf "$OMM_HOME/main/configs"
if [ "$R" = opengl1 ]; then FLAVOR=gl; else FLAVOR="${OMM_FLAVOR:-metal}"; fi
perl -e 'alarm shift; exec @ARGV' $(( SECS + 90 )) "$OMM_PORT/run.sh" "$FLAVOR" +set cl_renderer "$R" \
	+set r_mode -1 +set r_customwidth 1280 +set r_customheight 720 +set r_fullscreen 0 \
	+set r_swapInterval 0 +set com_maxfps 250 +set com_maxfpsUnfocused 0 +set com_maxfpsMinimized 0 \
	+set s_volume 0 +set s_musicvolume 0 +set r_primitives 2 \
	+set g_gametype 0 +set thereisnomonkey 1 "$@" +devmap dm/mohdm6 +exec lat_probe.cfg > "$LOG" 2>&1 &
GAME=$!

# wait for the probe to start (or the game to end)
for _ in $(seq 1 600); do
	grep -q '^latency-probe-start' "$LOG" 2>/dev/null && break
	kill -0 "$GAME" 2>/dev/null || break
	sleep 0.1
done
TRACE_FILE="$OUT/$NAME.trace"
if grep -q '^latency-probe-start' "$LOG"; then
	if [ "${TRACE:-0}" = 1 ]; then
		rm -rf "$TRACE_FILE"
		( sleep 2; xcrun xctrace record --template 'Metal System Trace' --output "$TRACE_FILE" --time-limit 6s \
			--all-processes > "$OUT/$NAME-xctrace.log" 2>&1 ) &
	fi
	if [ "${MOUSE:-1}" = 1 ]; then
		"$TOOL" "$SECS" 1000 2 400 > "$OUT/$NAME-mouse.txt" 2>&1
	else
		rm -f "$OUT/$NAME-mouse.txt"
		sleep "$SECS"
	fi
fi
wait

cat "$OUT/$NAME-mouse.txt" 2>/dev/null
grep -E '^Metal: [0-9]+ frames' "$LOG"
grep -A7 '^cl_latencyStats, whole run' "$LOG" | grep -v '^RE_Shutdown'
if [ "${TRACE:-0}" = 1 ]; then
	/usr/bin/python3 -I "$OMM_PORT/display_trace.py" "$TRACE_FILE" "$LOG"
fi
grep -q '^cl_latencyStats, whole run' "$LOG" || { echo "latency.sh: no whole-run numbers (see $LOG)" >&2; exit 1; }
