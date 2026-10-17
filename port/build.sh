#!/bin/bash
# Configure, build and install one flavor of OpenMoHAA into build.noindex/<flavor>-install.
#   port/build.sh metal     the game with the Metal renderer (mounts the Metal Toolchain for the offline shader compile)
#   port/build.sh gl        the GL1 reference: upstream's GL1 renderer as it was in the last commit that had it
#                           (GL1_REF below), built from a `git archive` export in build.noindex/gl-src. OpenGL is gone
#                           from the tree (Phase 4); the fixed views (port/shot.sh pair) and the bench (port/bench.sh
#                           run gl) still compare against this build.
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

case "$FLAVOR" in
	gl)
		# Export the reference tree once per GL1_REF (never a second checkout of the repo). The export sits inside
		# this repo's work tree: GIT_CEILING_DIRECTORIES keeps CMake's version step from reading the repo's HEAD.
		SRC_DIR="$OMM_BUILD/gl-src"
		if [ "$(cat "$SRC_DIR/.gl1-ref" 2>/dev/null)" != "$GL1_REF" ]; then
			rm -rf "$SRC_DIR"
			mkdir -p "$SRC_DIR"
			git -C "$OMM_ROOT" archive "$GL1_REF" | tar -x -C "$SRC_DIR"
			echo "$GL1_REF" > "$SRC_DIR/.gl1-ref"
		fi
		export GIT_CEILING_DIRECTORIES="$OMM_BUILD"
		FLAVOR_ARGS=() ;;
	metal) omm_mount_metal; FLAVOR_ARGS=() ;;
	*) echo "build.sh: unknown flavor '$FLAVOR'" >&2; exit 1 ;;
esac

# A build tree configured from another source tree (the gl flavor before the reference export) can't be reused.
if [ -f "$BUILD_DIR/CMakeCache.txt" ] && ! grep -qxF "CMAKE_HOME_DIRECTORY:INTERNAL=$SRC_DIR" "$BUILD_DIR/CMakeCache.txt"; then
	rm -rf "$BUILD_DIR"
fi

# CMAKE_DISABLE_FIND_PACKAGE_OpenAL: CMake otherwise picks Apple's OpenAL.framework headers, which lack alext.h.
# The internal openal-soft headers are used instead; the library itself is dlopen'ed at run time (libopenal.1.dylib).
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
