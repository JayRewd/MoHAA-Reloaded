#!/bin/bash
# CPU profile of the game: the reference demo as a looping timedemo, its main thread sampled with `sample`.
#   port/prof.sh [flavor] [name] [seconds] [engine args...]   -> build.noindex/prof/<name>.txt (default: metal, prof, 20)
# Read it with port/sample_by_library.py (self time per library) or port/sample_tree.py (callees / callers of a
# function). The demo reloads between loops (CL_DemoCompleted, CL_PlayDemo_f) and the frame limiter (NET_Sleep and
# the gettimeofday spin in Com_Frame) are in the report too: leave them out. `sample` inflates the frame (~0.24 -> ~0.28 ms).
set -euo pipefail
source "$(dirname "$0")/env.sh"

FLAVOR="${1:-metal}"
NAME="${2:-prof}"
SECS="${3:-20}"
[ $# -ge 3 ] && shift 3 || shift $#
OUT="$OMM_BUILD/prof"
mkdir -p "$OUT" "$OMM_HOME/main/demos"
[ -f "$OMM_HOME/main/demos/bench_mohdm6.dm_8" ] || cp "$OMM_PORT/bench/bench_mohdm6.dm_8" "$OMM_HOME/main/demos/"
printf 'set nextdemo "exec prof_loop.cfg"\ndemo bench_mohdm6\n' > "$OMM_HOME/main/prof_loop.cfg"
rm -rf "$OMM_HOME/main/configs"

"$OMM_PORT/run.sh" "$FLAVOR" +set r_mode -1 +set r_customwidth 1280 +set r_customheight 720 +set r_fullscreen 0 \
	+set r_swapInterval 0 +set com_maxfpsUnfocused 0 +set com_maxfpsMinimized 0 +set s_volume 0 +set s_musicvolume 0 \
	+set r_primitives 2 +set com_maxfps 0 +set timedemo 1 "$@" +exec prof_loop.cfg > "$OUT/$NAME-loop.log" 2>&1 &
RUN=$!

# wait for the first timedemo to finish (the game is warm and looping), then sample
for _ in $(seq 1 90); do
	grep -q '^timedemo work' "$OUT/$NAME-loop.log" 2>/dev/null && break
	sleep 1
done
GAME="$(pgrep -x openmohaa || true)"
[ -n "$GAME" ] || { echo "prof.sh: the game is not running (see $OUT/$NAME-loop.log)" >&2; exit 1; }
sample "$GAME" "$SECS" -file "$OUT/$NAME.txt" > /dev/null 2>&1 || true
kill "$GAME" 2>/dev/null || true
wait "$RUN" 2>/dev/null || true
echo "prof.sh: $OUT/$NAME.txt ($(grep -c '^timedemo work' "$OUT/$NAME-loop.log") timedemo loops in the run)"
