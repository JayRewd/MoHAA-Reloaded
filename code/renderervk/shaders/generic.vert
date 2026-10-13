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

// generic.vert -- vertex shader for every draw of the fixed-function
// emulation (see generic.frag)
//
// Every pipeline provides all four attributes: draws without a color array
// read one RGBA float color with a binding stride of 0, and texture units
// without coordinates read a dummy value the fragment shader ignores.

#version 450

layout(push_constant) uniform PushConstants {
	// projection * modelview, with the projection already remapped to
	// Vulkan clip space (z in [0, w]); y is flipped by the viewport
	mat4	mvp;
} pc;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec2 in_st0;
layout(location = 3) in vec2 in_st1;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_st0;
layout(location = 2) out vec2 v_st1;

// multi-pass shaders use GLS_DEPTHFUNC_EQUAL: every pipeline must produce
// bit-identical depth for the same vertex
invariant gl_Position;

void main() {
	gl_Position = pc.mvp * vec4( in_position, 1.0 );
	v_color = in_color;
	v_st0 = in_st0;
	v_st1 = in_st1;
}
