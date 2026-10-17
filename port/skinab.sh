#!/bin/bash
# The GPU-skinning A/B build (flavor metalab, build.noindex/metalab-install): the work tree as it is, plus
# port/skinab_patch.py, exported into build.noindex/metalab-src. Every GPU-skinned batch is then drawn the GPU and
# the CPU way and the vertex outputs are compared bit for bit after each frame:
#   port/skinab.sh && port/bench.sh run metalab 1 && grep 'SKINAB' build.noindex/bench/metalab-1.log | tail -1
#   OMM_FLAVOR=metalab port/shot.sh fixed metal abm2l1 shot_m2l1.cfg    (other maps; log in build.noindex/shots)
# Every "differ" count must be 0 ("captured" > 0). The A/B frames are slow (each one waits for the GPU).
set -euo pipefail
source "$(dirname "$0")/env.sh"

SRC_DIR="$OMM_BUILD/metalab-src"
rm -rf "$SRC_DIR"
mkdir -p "$SRC_DIR"
git -C "$OMM_ROOT" archive HEAD | tar -x -C "$SRC_DIR"
git -C "$OMM_ROOT" diff --name-only HEAD | while read -r f; do
	if [ -f "$OMM_ROOT/$f" ]; then cp "$OMM_ROOT/$f" "$SRC_DIR/$f"; else rm -f "$SRC_DIR/$f"; fi
done
python3 -I "$OMM_PORT/skinab_patch.py" "$SRC_DIR"
# the capture makes the vertex functions write to a buffer, which the shader build's -Werror rejects
sed -i '' 's/ -Wall -Werror/ -Wall/' "$SRC_DIR/cmake/utils/metallib.cmake"

export GIT_CEILING_DIRECTORIES="$OMM_BUILD"
omm_mount_metal
"$OMM_CMAKE" -S "$SRC_DIR" -B "$OMM_BUILD/metalab" -G Ninja \
	-DCMAKE_MAKE_PROGRAM="$OMM_NINJA" \
	-DBISON_EXECUTABLE="$OMM_BISON" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$OMM_BUILD/metalab-install" \
	-DCMAKE_DISABLE_FIND_PACKAGE_OpenAL=ON >/dev/null
"$OMM_CMAKE" --build "$OMM_BUILD/metalab" -j"$(sysctl -n hw.ncpu)"
"$OMM_CMAKE" --install "$OMM_BUILD/metalab" >/dev/null
echo "skinab.sh: metalab installed in $OMM_BUILD/metalab-install"
