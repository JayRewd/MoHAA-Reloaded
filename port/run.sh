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

# Link only the retail paks (read-only) into the install's main/ (and mainta/, maintt/ for Spearhead and
# Breakthrough, com_target_game 1/2), never the user's own dylibs, mods or configs.
for dir in main mainta maintt; do
	[ -d "$OMM_GAME_DIR/$dir" ] || continue
	mkdir -p "$BASE/$dir"
	for pak in "$OMM_GAME_DIR/$dir"/[Pp]ak[0-9]*.pk3; do [ -e "$pak" ] && ln -sfn "$pak" "$BASE/$dir/"; done
done
[ -e "$BASE/main/Pak0.pk3" ] || { echo "run.sh: no retail paks in $OMM_GAME_DIR/main (set OMM_GAME_DIR)" >&2; exit 1; }

# Builds of commits before session 46 (gl, sdlref, ci, an older metalref) keep a PID file: one left by a killed run
# makes them block on a modal "Abnormal Exit" dialog at startup. Remove it when no game is running (pgrep matches the
# installed app binaries only).
pgrep -f "build.noindex/.*/openmohaa.app/Contents/MacOS/openmohaa" >/dev/null || rm -f "$OMM_HOME"/main*/*.pid

# Builds since Phase 7c never load this link (until 9g they link the app's own openal-soft, in Contents/Frameworks;
# since 9g there is no OpenAL). It stays for the flavors that still dlopen libopenal.1.dylib: the gl reference and
# metalref of commits before 7c.
# openal-soft from Homebrew: Sys_LoadDll tries the executable's folder, then fs_basepath. The link goes into
# fs_basepath, so the bundle's ad-hoc seal (Phase 7b) stays intact: a file added to the signed bundle breaks it
# (and `codesign` refuses to sign a link pointing out of the bundle).
ln -sfn /opt/homebrew/opt/openal-soft/lib/libopenal.1.dylib "$BASE/libopenal.1.dylib"

exec "$APP/openmohaa" +set fs_homepath "$OMM_HOME" ${1+"$@"}
