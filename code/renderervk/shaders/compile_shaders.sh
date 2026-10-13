#!/bin/sh
#
# Compiles the GLSL shaders in this directory to SPIR-V and writes them as
# C arrays to ../vk_shaders.c, so that normal builds don't need glslc.
# Run it after changing a shader and commit the regenerated file.
#
# Usage: code/renderervk/shaders/compile_shaders.sh   (GLSLC=/path/to/glslc to override)

set -eu

cd "$(dirname "$0")"

GLSLC=${GLSLC:-glslc}
SHADERS="generic.vert generic.frag"
OUT=../vk_shaders.c
TMP=$OUT.tmp

{
	cat <<'EOF'
/*
===========================================================================
Copyright (C) 2026 the OpenMoHAA team

This file is part of OpenMoHAA source code.

OpenMoHAA source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

OpenMoHAA source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenMoHAA source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

// vk_shaders.c -- SPIR-V of the shaders in shaders/
//
// GENERATED FILE, DO NOT EDIT: run shaders/compile_shaders.sh instead.

#include <stddef.h>
#include <stdint.h>

EOF
	for src in $SHADERS; do
		name=vk_spirv_$(echo "$src" | tr . _)
		echo "// $src"
		echo "const uint32_t ${name}[] ="
		"$GLSLC" --target-env=vulkan1.1 -O -mfmt=c "$src" -o -
		echo ";"
		echo "const size_t ${name}_size = sizeof( ${name} );"
		echo
	done
} > "$TMP"

mv "$TMP" "$OUT"
echo "wrote $(cd .. && pwd)/vk_shaders.c"
