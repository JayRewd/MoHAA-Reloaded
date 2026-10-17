#!/bin/bash
# Run one installed flavor against the retail data. Everything the game writes goes to home.noindex/ (fs_homepath).
#   port/run.sh gl [+set r_mode -1 +map dm/mohdm6 ...]
set -euo pipefail
source "$(dirname "$0")/env.sh"

FLAVOR="${1:-gl}"
shift || true

BASE="$OMM_BUILD/$FLAVOR-install/lib/openmohaa"   # fs_basepath (the folder holding the .app)
APP="$BASE/openmohaa.app/Contents/MacOS"
[ -x "$APP/openmohaa" ] || { echo "run.sh: build $FLAVOR first (port/build.sh $FLAVOR)" >&2; exit 1; }

# Link only the retail paks (read-only) into the install's main/, never the user's own dylibs, mods or configs.
mkdir -p "$BASE/main"
for pak in "$OMM_GAME_DIR"/main/[Pp]ak[0-9]*.pk3; do ln -sfn "$pak" "$BASE/main/"; done
[ -e "$BASE/main/Pak0.pk3" ] || { echo "run.sh: no retail paks in $OMM_GAME_DIR/main (set OMM_GAME_DIR)" >&2; exit 1; }

# A PID file left by a killed run makes the game block on a modal "Abnormal Exit" dialog at startup.
# Remove it when no game is running (pgrep matches the installed app binaries only).
pgrep -f "build.noindex/.*/openmohaa.app/Contents/MacOS/openmohaa" >/dev/null || rm -f "$OMM_HOME"/main/*.pid

# openal-soft from Homebrew, dlopen'ed as libopenal.1.dylib from the executable's folder.
ln -sfn /opt/homebrew/opt/openal-soft/lib/libopenal.1.dylib "$APP/libopenal.1.dylib"

exec "$APP/openmohaa" +set fs_homepath "$OMM_HOME" ${1+"$@"}
