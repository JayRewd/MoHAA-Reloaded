# Source this before any build, run or benchmark:  source "/Volumes/1TB BLUE/openmohaa-metal/port/env.sh"
# Keeps every build product, temp file and compiler cache on the SSD, inside this repo (gitignored *.noindex/ folders).

# The repo root (works when sourced from zsh or bash, from any mount point).
export OMM_PORT="$(cd "$(dirname "${BASH_SOURCE[0]:-${(%):-%x}}")" >/dev/null && pwd)"
export OMM_ROOT="$(dirname "$OMM_PORT")"
export OMM_BUILD="$OMM_ROOT/build.noindex"            # CMake build trees and installs; safe to delete
export OMM_CACHE="$OMM_BUILD/cache"                    # TMPDIR, clang module cache
export OMM_HOME="$OMM_ROOT/home.noindex"               # fs_homepath for every run: configs, logs, demos, screenshots, timedemo logs
# Retail MOHAA data, read-only. Only the retail paks are linked into the run layout, never the user's own dylibs/mods.
export OMM_GAME_DIR="${OMM_GAME_DIR:-$HOME/Documents/Games/moh}"

# Xcode toolchain first; CMake, Ninja and Bison 3 come from Homebrew by full path only (see port/build.sh).
export DEVELOPER_DIR="/Applications/Xcode.app/Contents/Developer"
export PATH="/usr/bin:/bin:/usr/sbin:/sbin:$DEVELOPER_DIR/usr/bin"
export OMM_CMAKE="/opt/homebrew/bin/cmake"
export OMM_NINJA="/opt/homebrew/bin/ninja"
export OMM_BISON="/opt/homebrew/opt/bison/bin/bison"

# Compiler/tool temp files and caches -> SSD instead of /var/folders and ~/Library.
export TMPDIR="$OMM_CACHE/tmp/"
export CLANG_MODULE_CACHE_PATH="$OMM_CACHE/clang-modules"

mkdir -p "$TMPDIR" "$CLANG_MODULE_CACHE_PATH" "$OMM_HOME"

# Apple's Metal Toolchain: `xcrun metal` fails on this machine on purpose. The toolchain is kept as an exported
# disk image in the GTA V port's folder and mounted read-only on demand at /Volumes/MetalToolchainCryptex.
# Never write into /Volumes/1TB BLUE/gtav. NEVER run `xcodebuild -downloadComponent` (installs a copy on the internal disk).
export OMM_METAL_BIN="/Volumes/MetalToolchainCryptex/Metal.xctoolchain/usr/bin"
omm_mount_metal() {
	[ -x "$OMM_METAL_BIN/metal" ] && return 0
	local dmg
	for dmg in "/Volumes/1TB BLUE/gtav/toolchain.noindex"/MetalToolchain-*.exportedBundle/Restore/*.dmg; do :; done
	hdiutil attach -readonly -nobrowse -noautoopen -noverify "$dmg" >/dev/null || { echo "env.sh: failed to mount Metal Toolchain image" >&2; return 1; }
}
