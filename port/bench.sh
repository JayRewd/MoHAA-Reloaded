#!/bin/bash
# Repeatable performance baseline: a timedemo of a fixed demo, in a 1280x720 window, vsync off, fps uncapped.
#   port/bench.sh run [flavor] [runs] [engine args...]
#                                        play the demo back as a timedemo <runs> times (default 3), print each result
#   port/bench.sh trace [flavor]         one timedemo under Metal System Trace: GPU busy time and utilization
#   port/bench.sh record [flavor]        re-record the demo (only if the reference demo must change; then re-baseline
#                                        every flavor and replace port/bench/bench_mohdm6.dm_8)
# Flavors: metal (the game) and gl (the GL1 reference, port/build.sh gl: the last commit that had GL1).
# The reference demo is port/bench/bench_mohdm6.dm_8 (dm/mohdm6, 120 s, spectator following 8 bots, 2398 timedemo
# frames); it is copied into home.noindex/main/demos/ when missing.
# Results: one line per run, "<frames> frames <seconds> seconds <fps> fps <min>/<avg>/<max>/<sdev> ms | work: <ms>/frame".
# The engine never runs a frame shorter than 1 ms (Com_Frame pads it), so fps saturates near 1000; "work" is the
# frame time without that padding (wall minus frame-limiter wait), the number to compare once frames are < 1 ms.
# Per-frame durations (integer ms) are written to home.noindex/main/bench/<flavor>-<n>.txt.
set -euo pipefail
source "$(dirname "$0")/env.sh"

MODE="${1:-run}"
FLAVOR="${2:-gl}"
RUNS="${3:-3}"
[ $# -ge 3 ] && shift 3 || shift $#
EXTRA=("$@")        # extra engine args for "run", e.g. +set r_primitives 2
DEMO="bench_mohdm6"
LOGDIR="$OMM_BUILD/bench"
mkdir -p "$LOGDIR" "$OMM_HOME/main"

# Same video/sound settings for every run and every flavor. Sound stays initialised but muted
# (s_initsound 0 crashes on the first footstep upstream). r_primitives 2 (glDrawElements) is the user's setting and
# GL1's fastest path on macOS (the default 0 falls back to glArrayElement immediate mode: ~3x slower); the Metal
# renderer ignores it. Pass "+set r_primitives 0" as extra args to measure upstream's default.
VIDEO=(+set r_mode -1 +set r_customwidth 1280 +set r_customheight 720 +set r_fullscreen 0
	+set r_swapInterval 0 +set com_maxfpsUnfocused 0 +set com_maxfpsMinimized 0
	+set s_volume 0 +set s_musicvolume 0 +set r_primitives 2)

# A watchdog so a hung run never blocks a session: kill the game after N seconds.
watchdog() { local secs="$1"; shift; perl -e 'alarm shift; exec @ARGV' "$secs" "$@"; }

# Fresh default config each time, so nothing from an earlier run leaks in.
reset_config() { rm -rf "$OMM_HOME/main/configs"; }

case "$MODE" in
record)
	cp "$OMM_PORT/bench/record_mohdm6.cfg" "$OMM_HOME/main/"
	reset_config
	watchdog 300 "$OMM_PORT/run.sh" "$FLAVOR" "${VIDEO[@]}" +set com_maxfps 60 \
		+set g_gametype 1 +set sv_maxclients 16 +set sv_maxbots 8 +set sv_numbots 8 \
		+set fraglimit 0 +set timelimit 0 \
		+devmap dm/mohdm6 +exec record_mohdm6.cfg > "$LOGDIR/record.log" 2>&1
	ls -la "$OMM_HOME/main/demos/$DEMO.dm_8"
	shasum -a 256 "$OMM_HOME/main/demos/$DEMO.dm_8"
	;;
run)
	# The committed copy (port/bench/) is the reference demo; restore it if the run home was wiped.
	mkdir -p "$OMM_HOME/main/demos"
	[ -f "$OMM_HOME/main/demos/$DEMO.dm_8" ] || cp "$OMM_PORT/bench/$DEMO.dm_8" "$OMM_HOME/main/demos/"
	for n in $(seq 1 "$RUNS"); do
		reset_config
		watchdog 300 "$OMM_PORT/run.sh" "$FLAVOR" "${VIDEO[@]}" +set com_maxfps 0 \
			+set cl_timedemoLog "bench/$FLAVOR-$n.txt" +set nextdemo quit \
			${EXTRA[@]+"${EXTRA[@]}"} +set timedemo 1 +demo "$DEMO" > "$LOGDIR/$FLAVOR-$n.log" 2>&1 || true
		result="$(grep -E '^[0-9]+ frames ' "$LOGDIR/$FLAVOR-$n.log" | tail -1)"
		work="$(grep -E '^timedemo work: ' "$LOGDIR/$FLAVOR-$n.log" | tail -1 | sed 's/^timedemo //')"
		echo "$FLAVOR run $n: ${result:-FAILED (see $LOGDIR/$FLAVOR-$n.log)}${work:+ | $work}"
	done
	;;
trace)
	# One timedemo under Instruments' Metal System Trace (all processes), then GPU busy time per game frame.
	# xctrace --launch is unreliable here (the game often stays suspended), so the trace records every process
	# while the game is started normally. The trace (~0.5 GB) is deleted unless KEEP_TRACE=1.
	mkdir -p "$OMM_HOME/main/demos"
	[ -f "$OMM_HOME/main/demos/$DEMO.dm_8" ] || cp "$OMM_PORT/bench/$DEMO.dm_8" "$OMM_HOME/main/demos/"
	reset_config
	TRACE="$LOGDIR/$FLAVOR.trace"
	rm -rf "$TRACE" "$LOGDIR/$FLAVOR-gpu.xml"
	xcrun xctrace record --template 'Metal System Trace' --output "$TRACE" --time-limit 30s --all-processes \
		> "$LOGDIR/$FLAVOR-xctrace.log" 2>&1 &
	XCTRACE_PID=$!
	sleep 4                       # let the recording start
	watchdog 60 "$OMM_PORT/run.sh" "$FLAVOR" "${VIDEO[@]}" +set com_maxfps 0 +set nextdemo quit \
		+set timedemo 1 +demo "$DEMO" > "$LOGDIR/$FLAVOR-trace.log" 2>&1 || true
	traced="$(grep -E '^[0-9]+ frames ' "$LOGDIR/$FLAVOR-trace.log" | tail -1)"
	echo "$FLAVOR traced run: ${traced:-FAILED (see $LOGDIR/$FLAVOR-trace.log)}"
	# the traced run's duration and frame count locate the timedemo inside the trace
	demo_secs="$(echo "$traced" | sed -E 's|^[0-9]+ frames ([0-9.]+) seconds.*|\1|')"
	demo_frames="$(echo "$traced" | sed -E 's|^([0-9]+) frames.*|\1|')"
	wait "$XCTRACE_PID" || true   # xctrace exits non-zero when it reaches the time limit
	xcrun xctrace export --input "$TRACE" \
		--xpath '/trace-toc/run[@number="1"]/data/table[@schema="metal-gpu-intervals"]' > "$LOGDIR/$FLAVOR-gpu.xml"
	/usr/bin/python3 -I "$OMM_PORT/gpu_frame_times.py" "$LOGDIR/$FLAVOR-gpu.xml" openmohaa ${traced:+"$demo_secs" "$demo_frames"}
	[ "${KEEP_TRACE:-0}" = 1 ] || rm -rf "$TRACE" "$LOGDIR/$FLAVOR-gpu.xml"
	;;
*)
	echo "usage: port/bench.sh record|run|trace [flavor] [runs]" >&2
	exit 1
	;;
esac
