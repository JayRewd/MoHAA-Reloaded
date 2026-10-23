#!/bin/bash
# The native sound engine's mixer alone (code/macos/macos_snd.c, docs/METAL_PORT_PLAN.md Phase 9b): port/sndtest.c
# built three ways into build.noindex/sndtest/: sndtest (-O3, the device modes), sndtest-asan (the offline tests
# under AddressSanitizer and UBSan), sndtest-tsan (the stress test under ThreadSanitizer). The native driver's math
# (code/client/snd_native_math.h, Phase 9c) is checked here too: test math ima.
#   port/sndtest.sh                     the offline tests and 10 s of stress
#   port/sndtest.sh test [name ...]     the offline tests (resample loop offset pause ramp limiter free misc capture math ima)
#   port/sndtest.sh stress [seconds]
#   port/sndtest.sh device [frames ...] start -> output, CPU at 0/16/40/96 voices, a restart (silent; 64 and 32)
#   port/sndtest.sh load frames seconds [busy threads]   overloads under load (run port/bench.sh beside it)
#   port/sndtest.sh devices
set -euo pipefail
source "$(dirname "$0")/env.sh"

OUT="$OMM_BUILD/sndtest"
mkdir -p "$OUT"
SRC=("$OMM_PORT/sndtest.c" "$OMM_ROOT/code/macos/macos_snd.c")
FLAGS=(-std=gnu11 -Wall -Wextra -Wno-unused-parameter -framework AudioToolbox -framework CoreAudio -framework CoreFoundation)

build() {	# name, flags
	local name=$1
	shift
	if [ ! -x "$OUT/$name" ] || [ -n "$(find "${SRC[@]}" "$OMM_ROOT/code/macos/macos_snd.h" \
		"$OMM_ROOT/code/client/snd_native_math.h" "$0" -newer "$OUT/$name")" ]; then
		clang "${FLAGS[@]}" "$@" "${SRC[@]}" -o "$OUT/$name"
	fi
}

mode=${1:-all}
[ $# -gt 0 ] && shift
case "$mode" in
	all)
		build sndtest-asan -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
		build sndtest-tsan -O1 -g -fsanitize=thread
		"$OUT/sndtest-asan"
		"$OUT/sndtest-tsan" stress 10
		;;
	test)
		build sndtest-asan -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
		"$OUT/sndtest-asan" "$@"
		;;
	stress)
		build sndtest-tsan -O1 -g -fsanitize=thread
		"$OUT/sndtest-tsan" stress "$@"
		;;
	device|load|devices)
		build sndtest -O3
		"$OUT/sndtest" "$mode" "$@"
		;;
	*)
		echo "usage: port/sndtest.sh [test [name ...] | stress [s] | device [frames ...] | load frames s [busy] | devices]" >&2
		exit 2
		;;
esac
