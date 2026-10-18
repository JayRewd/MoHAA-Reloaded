#!/bin/bash
# Configure, build and install one flavor of OpenMoHAA into build.noindex/<flavor>-install.
#   port/build.sh metal     the game with the Metal renderer (mounts the Metal Toolchain for the offline shader compile)
#   port/build.sh metalref  the Metal game at the last commit (METAL_REF=<ref> for another), for before/after benches
#   port/build.sh gl        the GL1 reference: upstream's GL1 renderer as it was in the last commit that had it
#                           (GL1_REF below), built from a `git archive` export in build.noindex/gl-src, plus the client
#                           hunks of GL1_PATCHES (benchmark logging only). OpenGL is gone from the tree (Phase 4); the
#                           fixed views (port/shot.sh pair) and the bench (port/bench.sh run gl) still compare against
#                           this build.
# Extra arguments are passed to the CMake configure step (e.g. -DCMAKE_BUILD_TYPE=Debug).
set -euo pipefail
source "$(dirname "$0")/env.sh"

FLAVOR="${1:-metal}"
shift || true

BUILD_DIR="$OMM_BUILD/$FLAVOR"
INSTALL_DIR="$OMM_BUILD/$FLAVOR-install"
SRC_DIR="$OMM_ROOT"

# The last commit with the GL1 renderer (Phase 3 complete up to 3c; Phase 4 deleted OpenGL after it).
GL1_REF=c9dddadd22cf775766b34c118e9ce414290efbb6
# Client-only commits applied on top of the GL1 export, so the reference reports what the Metal build reports:
# the timedemo's per-frame work log (Phase 5b). The renderer stays as it was at GL1_REF.
GL1_PATCHES=(ae894bcf)

case "$FLAVOR" in
	gl)
		# Export the reference tree once per GL1_REF (never a second checkout of the repo). The export sits inside
		# this repo's work tree: GIT_CEILING_DIRECTORIES keeps CMake's version step from reading the repo's HEAD.
		SRC_DIR="$OMM_BUILD/gl-src"
		GL1_ID="$GL1_REF ${GL1_PATCHES[*]}"
		if [ "$(cat "$SRC_DIR/.gl1-ref" 2>/dev/null)" != "$GL1_ID" ]; then
			rm -rf "$SRC_DIR"
			mkdir -p "$SRC_DIR"
			git -C "$OMM_ROOT" archive "$GL1_REF" | tar -x -C "$SRC_DIR"
			for c in "${GL1_PATCHES[@]}"; do
				git -C "$OMM_ROOT" diff "$c^" "$c" -- code/client code/qcommon | patch -p1 -s -N --no-backup-if-mismatch -d "$SRC_DIR"
			done
			echo "$GL1_ID" > "$SRC_DIR/.gl1-ref"
		fi
		export GIT_CEILING_DIRECTORIES="$OMM_BUILD"
		FLAVOR_ARGS=() ;;
	metal) omm_mount_metal; FLAVOR_ARGS=() ;;
	metalref)
		# The Metal game at a commit (METAL_REF, default HEAD: the last commit without the work tree's changes),
		# from a `git archive` export like gl, for before/after benchmarks in one sitting (bench.sh run metalref).
		REF="$(git -C "$OMM_ROOT" rev-parse "${METAL_REF:-HEAD}")"
		SRC_DIR="$OMM_BUILD/metalref-src"
		if [ "$(cat "$SRC_DIR/.metal-ref" 2>/dev/null)" != "$REF" ]; then
			rm -rf "$SRC_DIR"
			mkdir -p "$SRC_DIR"
			git -C "$OMM_ROOT" archive "$REF" | tar -x -C "$SRC_DIR"
			echo "$REF" > "$SRC_DIR/.metal-ref"
		fi
		export GIT_CEILING_DIRECTORIES="$OMM_BUILD"
		omm_mount_metal; FLAVOR_ARGS=() ;;
	*) echo "build.sh: unknown flavor '$FLAVOR'" >&2; exit 1 ;;
esac

# A build tree configured from another source tree (the gl flavor before the reference export) can't be reused.
if [ -f "$BUILD_DIR/CMakeCache.txt" ] && ! grep -qxF "CMAKE_HOME_DIRECTORY:INTERNAL=$SRC_DIR" "$BUILD_DIR/CMakeCache.txt"; then
	rm -rf "$BUILD_DIR"
fi

# CMAKE_DISABLE_FIND_PACKAGE_OpenAL: CMake otherwise picks Apple's OpenAL.framework headers, which lack alext.h.
# The internal openal-soft is used instead: since Phase 7c it is built from code/thirdparty and linked directly
# (inside the .app). Older trees (gl, metalref before 7c) take only its headers and dlopen libopenal.1.dylib.
"$OMM_CMAKE" -S "$SRC_DIR" -B "$BUILD_DIR" -G Ninja \
	-DCMAKE_MAKE_PROGRAM="$OMM_NINJA" \
	-DBISON_EXECUTABLE="$OMM_BISON" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
	-DCMAKE_DISABLE_FIND_PACKAGE_OpenAL=ON \
	${FLAVOR_ARGS[@]+"${FLAVOR_ARGS[@]}"} ${1+"$@"}

"$OMM_CMAKE" --build "$BUILD_DIR" -j"$(sysctl -n hw.ncpu)"
"$OMM_CMAKE" --install "$BUILD_DIR" >/dev/null
echo "build.sh: $FLAVOR installed in $INSTALL_DIR"
