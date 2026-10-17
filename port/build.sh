#!/bin/bash
# Configure, build and install one flavor of OpenMoHAA into build.noindex/<flavor>-install.
#   port/build.sh gl        upstream GL1 renderer (the baseline / reference build)
#   port/build.sh metal     GL1 + the Metal renderer in one install (switch with +set cl_renderer metal|opengl1);
#                           mounts the Metal Toolchain for the offline shader compile
# Extra arguments are passed to the CMake configure step (e.g. -DCMAKE_BUILD_TYPE=Debug).
set -euo pipefail
source "$(dirname "$0")/env.sh"

FLAVOR="${1:-gl}"
shift || true

BUILD_DIR="$OMM_BUILD/$FLAVOR"
INSTALL_DIR="$OMM_BUILD/$FLAVOR-install"

case "$FLAVOR" in
	gl) FLAVOR_ARGS=() ;;
	metal) omm_mount_metal; FLAVOR_ARGS=(-DBUILD_RENDERER_METAL=ON) ;;
	*) echo "build.sh: unknown flavor '$FLAVOR'" >&2; exit 1 ;;
esac

# CMAKE_DISABLE_FIND_PACKAGE_OpenAL: CMake otherwise picks Apple's OpenAL.framework headers, which lack alext.h.
# The internal openal-soft headers are used instead; the library itself is dlopen'ed at run time (libopenal.1.dylib).
"$OMM_CMAKE" -S "$OMM_ROOT" -B "$BUILD_DIR" -G Ninja \
	-DCMAKE_MAKE_PROGRAM="$OMM_NINJA" \
	-DBISON_EXECUTABLE="$OMM_BISON" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
	-DCMAKE_DISABLE_FIND_PACKAGE_OpenAL=ON \
	${FLAVOR_ARGS[@]+"${FLAVOR_ARGS[@]}"} ${1+"$@"}

"$OMM_CMAKE" --build "$BUILD_DIR" -j"$(sysctl -n hw.ncpu)"
"$OMM_CMAKE" --install "$BUILD_DIR" >/dev/null
echo "build.sh: $FLAVOR installed in $INSTALL_DIR"
