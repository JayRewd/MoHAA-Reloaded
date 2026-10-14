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

// generic.frag -- the GL1 fixed-function fragment pipeline
//
// The variant is selected with specialization constants, so each pipeline
// only runs the stages its state needs (see vk_pipeline.c).

#version 450

// texture env of each unit, texEnv_t values from vk.h; 0 disables the unit
layout(constant_id = 0) const int TEX_ENV0 = 0;
layout(constant_id = 1) const int TEX_ENV1 = 0;
// the texture of the unit has no alpha channel: it samples as 1 (the image
// view swizzle does that) and TEXENV_REPLACE keeps the previous alpha
layout(constant_id = 2) const bool TEX0_RGB = false;
layout(constant_id = 3) const bool TEX1_RGB = false;
// alpha test: 0 off, 1 keep alpha > ref, 2 keep alpha < ref, 3 keep alpha >= ref
layout(constant_id = 4) const int ALPHA_FUNC = 0;
layout(constant_id = 5) const float ALPHA_REF = 0.0;
// linear fog with the parameters of the push constants
layout(constant_id = 6) const bool FOG = false;
// discard on the clip plane side (when the vertex shader can't use
// gl_ClipDistance)
layout(constant_id = 7) const bool CLIP_DISCARD = false;

// the fragment stage range of the push constants (vkPushConstants_t in vk.c)
layout(push_constant) uniform PushConstants {
	layout(offset = 96) vec4	fogColor;
	// -1 / (end - start), end / (end - start), as Mesa's
	// gl_MesaFogParamsOptimized
	vec2	fogParams;
} pc;

// texEnv_t
const int TEXENV_MODULATE = 1;
const int TEXENV_REPLACE = 2;
const int TEXENV_DECAL = 3;
const int TEXENV_ADD = 4;
const int TEXENV_COMBINE_INTERPOLATE = 5;

layout(set = 0, binding = 0) uniform sampler2D u_texture0;
layout(set = 1, binding = 0) uniform sampler2D u_texture1;

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_st0;
layout(location = 2) in vec2 v_st1;
layout(location = 3) in vec2 v_fogClip;

layout(location = 0) out vec4 out_color;

vec4 TexEnv( int env, bool rgbTexture, vec4 previous, vec4 texel ) {
	if ( env == TEXENV_REPLACE ) {
		return rgbTexture ? vec4( texel.rgb, previous.a ) : texel;
	}
	if ( env == TEXENV_DECAL ) {
		return vec4( mix( previous.rgb, texel.rgb, texel.a ), previous.a );
	}
	if ( env == TEXENV_ADD ) {
		return vec4( min( previous.rgb + texel.rgb, 1.0 ), previous.a * texel.a );
	}
	if ( env == TEXENV_COMBINE_INTERPOLATE ) {
		return vec4( v_color.rgb * v_color.a + texel.rgb * ( 1.0 - v_color.a ), previous.a * texel.a );
	}
	// TEXENV_MODULATE, and TEXENV_COMBINE4 which no shader produces
	return previous * texel;
}

void main() {
	vec4 color = v_color;

	if ( CLIP_DISCARD && v_fogClip.y < 0.0 ) {
		discard;
	}

	if ( TEX_ENV0 != 0 ) {
		color = TexEnv( TEX_ENV0, TEX0_RGB, color, texture( u_texture0, v_st0 ) );
	}
	if ( TEX_ENV1 != 0 ) {
		color = TexEnv( TEX_ENV1, TEX1_RGB, color, texture( u_texture1, v_st1 ) );
	}

	if ( ALPHA_FUNC == 1 ) {
		if ( !( color.a > ALPHA_REF ) ) {
			discard;
		}
	} else if ( ALPHA_FUNC == 2 ) {
		if ( !( color.a < ALPHA_REF ) ) {
			discard;
		}
	} else if ( ALPHA_FUNC == 3 ) {
		if ( !( color.a >= ALPHA_REF ) ) {
			discard;
		}
	}

	// Mesa's fixed-function fog (st_nir_lower_fog): a single multiply-add
	// for the factor, then fogColor * ( 1 - f ) + color * f; alpha unchanged
	if ( FOG ) {
		float f = clamp( fma( v_fogClip.x, pc.fogParams.x, pc.fogParams.y ), 0.0, 1.0 );

		color.rgb = fma( color.rgb, vec3( f ), pc.fogColor.rgb * ( 1.0 - f ) );
	}

	out_color = color;
}
