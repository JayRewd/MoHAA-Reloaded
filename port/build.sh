#!/bin/bash
# Configure, build and install one flavor of OpenMoHAA into build.noindex/<flavor>-install.
#   port/build.sh metal     the game: the Metal renderer and the native AppKit layer (mounts the Metal Toolchain for
#                           the offline shader compile)
#   port/build.sh metalref  the Metal game at the last commit (METAL_REF=<ref> for another), for before/after benches
#   port/build.sh sdlref    the Metal game with SDL2 as it was in the last commit that had it (SDL_REF below), from a
#                           `git archive` export in build.noindex/sdlref-src: the SDL baseline of Phase 6f (the
#                           user's feel test and their mouse against SDL), kept apart from metalref so a before/after
#                           bench never overwrites it.
#   port/build.sh sndref    the Metal game with openal-soft and its sound driver as it was in the last commit that had
#                           them (SND_REF below), from an export in build.noindex/sndref-src like sdlref: the sound
#                           baseline of Phase 9 (`+set s_native 0` plays OpenAL's driver, 1 the native one).
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
# The last commit with SDL2 (Phase 6g removed it after this one): its code is db88239d's, the 6f baseline.
SDL_REF=fd94160b617a8afa34e4acac8faa3ca8fc306eed
# The last commit with openal-soft (Phase 9g removed it after this one): 9f's tree, both sound drivers (846287f5
# until session 72's rewrite folded headphone 3D's EQ into 9f; the same tree otherwise).
SND_REF=a661b7b29ba3df91faed0688a70dd6c8cae7cff0

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
	metalref|sdlref|sndref)
		# The Metal game at a commit, from a `git archive` export like gl: metalref at METAL_REF (default HEAD: the
		# last commit without the work tree's changes), for before/after benchmarks in one sitting (bench.sh run
		# metalref); sdlref at SDL_REF and sndref at SND_REF, always.
		case "$FLAVOR" in
			sdlref) REF="$SDL_REF" ;;
			sndref) REF="$SND_REF" ;;
			*) REF="$(git -C "$OMM_ROOT" rev-parse "${METAL_REF:-HEAD}")" ;;
		esac
		SRC_DIR="$OMM_BUILD/$FLAVOR-src"
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

# CMAKE_DISABLE_FIND_PACKAGE_OpenAL, for the trees that still have OpenAL (sndref, sdlref, gl, a metalref before
# Phase 9g): CMake otherwise picks Apple's OpenAL.framework headers, which lack alext.h, over the internal openal-soft
# (built from code/thirdparty and linked inside the .app since Phase 7c; older trees take only its headers and dlopen
# libopenal.1.dylib). Trees since 9g have no OpenAL: nothing reads it.
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
