#!/bin/bash
# Phase 4 check: no OpenGL left in the tree or in the installed binary (docs/METAL_PORT_PLAN.md, Phase 4).
#   port/glcheck.sh [flavor]     (default metal)
# 1. The plan's grep over code/ and cmake/. Each hit has the documented exceptions removed from its line first
#    (cvar values and shader keywords kept for old configs and retail data, false positives, upstream code kept
#    for small diffs, vendored SDL until Phase 6); whatever still matches is printed.
# 2. otool -L and nm -u of the installed game binary: no OpenGL framework, no GL/CGL/NSOpenGL imports.
# Exit status 0 = clean. Run it after upstream merges too: a merge can bring GL code back.
set -uo pipefail
source "$(dirname "$0")/env.sh"

FLAVOR="${1:-metal}"
PATTERN='qgl|opengl|SDL_GL_|GL_[A-Z0-9_]+|glimp'
status=0

cd "$OMM_ROOT" || exit 1
hits="$(grep -rniE "$PATTERN" code cmake | /usr/bin/python3 -I -c '
import re, sys
pattern = re.compile(sys.argv[1], re.I)
# (file regex, allowed text regex): the allowed text is removed from the line before testing it again
allowed = [
	# vendored SDL2 headers (the SDL_GL_* API, build configs of other platforms): SDL goes in Phase 6
	(r"^code/thirdparty/SDL2-[^/]*/", r".*"),
	# vendored CI files of recastnavigation (they installed GL for the RecastDemo app, deleted in Phase 4)
	(r"^code/thirdparty/recastnavigation/\.github/", r".*"),
	# false positives in vendored code: the Qt qglobal.h guard; in openal-soft, a comment comparing coordinate
	# systems (al.h, al.cppm, al.xml), the OpenGL registry and tooling in its registry notes, and a glGetString
	# example in the old changelog of its copy of fmt
	(r"^code/thirdparty/jpeg-[^/]*/jmorecfg\.h$", r"QGLOBAL_H|qglobal\.h"),
	(r"^code/thirdparty/openal-soft-[^/]*/", r"like OpenGL|The OpenGL registry|OpenCL/OpenGL tooling|glGetString\(GL_VERSION\)"),
	# shader script keywords (blendFunc GL_ONE GL_ZERO ... in the retail .shader files)
	(r"^code/renderer_metal/tr_shader\.c$", r"\"GL_[A-Z_]+\"|substituting GL_ONE"),
	# cvar values kept for old configs: r_textureMode GL_LINEAR_MIPMAP_NEAREST ..., r_drawBuffer GL_BACK/GL_FRONT
	(r"^code/renderer_metal/tr_image\.c$", r"\{\"GL_[A-Z_]+\", MB_"),
	(r"^code/renderer_metal/tr_init\.c$", r"\"gl_linear_mipmap_linear\"|\"GL_BACK\""),
	(r"^code/renderer_metal/tr_cmds\.c$", r"\"GL_FRONT\""),
	# false positives: the "sgl" font header type, the glue flag of the game, a UI event of the LAN game list
	(r"^code/", r"fontheader_sgl_[st]"),
	(r"^code/fgame/entity\.(h|cpp)$", r"GL_USEANGLES"),
	(r"^code/uilib/uilangamelist\.cpp$", r"EV_UILGL_"),
	# upstream client/sys code kept for small diffs: the refimport hooks (no-ops on macOS, no longer called),
	# the inactive USE_RENDERER_DLOPEN branch of CL_InitRef, a comment on matrix order
	(r"^code/(renderercommon/tr_public\.h|client/cl_main\.cpp|sys/sys_local\.h|sys/sys_unix\.c)$", r"Sys_GLimp(Safe)?Init"),
	(r"^code/client/cl_main\.cpp$", r"Cvar_Get\(\"cl_renderer\", \"opengl1\""),
	(r"^code/qcommon/q_math\.c$", r"the OpenGL order"),
]
allowed = [(re.compile(f), re.compile(t)) for f, t in allowed]
for line in sys.stdin:
	path, _, rest = line.partition(":")
	num, _, text = rest.partition(":")
	for f, t in allowed:
		if f.search(path):
			text = t.sub("", text)
	if pattern.search(text):
		sys.stdout.write(line)
' "$PATTERN")"
if [ -n "$hits" ]; then
	echo "glcheck: GL names outside the documented exceptions:"
	echo "$hits"
	status=1
else
	echo "glcheck: source clean (only documented exceptions)"
fi

BIN="$OMM_BUILD/$FLAVOR-install/lib/openmohaa/openmohaa.app/Contents/MacOS/openmohaa"
if [ ! -x "$BIN" ]; then
	echo "glcheck: no $FLAVOR install (port/build.sh $FLAVOR)" >&2
	exit 1
fi
if otool -L "$BIN" | grep -i opengl; then
	echo "glcheck: the binary links OpenGL"
	status=1
fi
if nm -u "$BIN" | grep -E '^_(gl[A-Z]|CGL|OBJC_CLASS_\$_NSOpenGL)'; then
	echo "glcheck: the binary imports GL symbols"
	status=1
fi
ls "$(dirname "$BIN")" | grep -i 'renderer_' && { echo "glcheck: a renderer dylib is installed"; status=1; }
[ $status = 0 ] && echo "glcheck: binary clean (otool -L: no OpenGL; nm -u: no GL imports; no renderer dylibs)"
exit $status
