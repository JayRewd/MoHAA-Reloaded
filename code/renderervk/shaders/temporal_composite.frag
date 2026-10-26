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

// temporal_composite.frag -- the temporal path's scene (r_temporal,
// vk_temporal.c) into the frame's color target, before the 2D drawing.
// What resolves the scene goes before it, at VK_TemporalConsumers: with
// r_taa u_color is the anti-aliased result (taa_resolve.comp), which is
// sharpened here (r_taaSharpen: it stays unsharpened as the history).
// Ambient occlusion is to come. r_temporalShow shows one of the
// buffers instead, with what temporal_common.glsl gives their readers (the
// camera's motion, the derived normals) from level 0 of the depth pyramid:
// at half the resolution, for a look.

#version 450
#extension GL_GOOGLE_include_directive : require
#include "temporal_common.glsl"

layout(set = 0, binding = 0) uniform sampler2D u_color;		// what is shown: the anti-aliased scene with r_taa, else the scene
layout(set = 0, binding = 1) uniform sampler2D u_velocity;
layout(set = 0, binding = 2) uniform sampler2D u_normal;
layout(set = 0, binding = 3) uniform sampler2D u_hiz;		// this frame's depth pyramid (min, max), level 0 at half size
layout(set = 0, binding = 4) uniform sampler2D u_history;	// the last frame's scene color
layout(set = 0, binding = 5) uniform sampler2D u_prevHiz;	// the last frame's depth pyramid
layout(set = 0, binding = 6) uniform sampler2D u_scene;		// this frame's scene color, unresolved

layout(push_constant) uniform Push {
	TemporalView	view;
	int		mode;		// r_temporalShow
	int		mip;		// r_temporalShowMip
	float	scale;		// r_temporalShowScale
	float	sharpen;	// r_taaSharpen when u_color is anti-aliased, else 0
	ivec4	rect;		// the scene's views: x, y, width, height in framebuffer pixels
} pc;

layout(location = 0) out vec4 out_color;

// the nearest view distance around a pixel, from level 0 of the pyramid
float Distance( ivec2 p ) {
	ivec2 size = textureSize( u_hiz, 0 );

	return texelFetch( u_hiz, clamp( p >> 1, ivec2( 0 ), size - 1 ), 0 ).r;
}

void main() {
	ivec2 p = ivec2( gl_FragCoord.xy );
	vec2 pixel = gl_FragCoord.xy;
	float far = pc.view.depth.w;
	vec3 c;

	if ( pc.mode == 1 ) {
		// motion: red right, green down, blue left or up, in units of the
		// viewport times r_temporalShowScale
		vec4 n = texelFetch( u_normal, p, 0 );
		vec2 v = ( TemporalFlags( n ) & TEMPORAL_FLAG_MOTION ) != 0u ? texelFetch( u_velocity, p, 0 ).rg
			: TemporalCameraMotion( pc.view, pixel, Distance( p ) );

		v *= pc.scale;
		c = vec3( max( v.x, 0.0 ), max( v.y, 0.0 ), max( -v.x, 0.0 ) + max( -v.y, 0.0 ) );
	} else if ( pc.mode == 2 ) {
		// black: no surface, or the view weapon without a normal
		vec4 n = texelFetch( u_normal, p, 0 );
		int cls = TemporalClass( n );
		float d = Distance( p );

		if ( ( TemporalFlags( n ) & TEMPORAL_FLAG_NORMAL ) != 0u ) {
			c = TemporalOctDecode( n.rg ) * 0.5 + 0.5;
		} else if ( cls == TEMPORAL_CLASS_WEAPON || d >= far * 0.999 ) {
			c = vec3( 0.0 );
		} else {
			c = TemporalDerivedNormal( pc.view, pixel, 2.0, d, Distance( p - ivec2( 2, 0 ) ), Distance( p + ivec2( 2, 0 ) ),
				Distance( p - ivec2( 0, 2 ) ), Distance( p + ivec2( 0, 2 ) ) ) * 0.5 + 0.5;
		}
	} else if ( pc.mode == 3 ) {
		// class: gray surface (light blue when it moves), green a surface
		// without a normal, red the view weapon, blue the sky, black nothing
		vec4 n = texelFetch( u_normal, p, 0 );
		uint flags = TemporalFlags( n );
		int cls = TemporalClass( n );

		c = cls == TEMPORAL_CLASS_WEAPON ? vec3( 1.0, 0.0, 0.0 ) : cls == TEMPORAL_CLASS_SKY ? vec3( 0.0, 0.0, 1.0 )
			: cls == TEMPORAL_CLASS_NONE ? vec3( 0.0 ) : ( flags & TEMPORAL_FLAG_NORMAL ) == 0u ? vec3( 0.0, 0.8, 0.0 )
			: ( flags & TEMPORAL_FLAG_MOTION ) != 0u ? vec3( 0.4, 0.6, 1.0 ) : vec3( 0.5 );
	} else if ( pc.mode == 4 ) {
		// the depth pyramid's level: min in red, max in green, as a fraction
		// of the far distance (level 0 covers 2 x 2 pixels)
		int mip = clamp( pc.mip, 0, textureQueryLevels( u_hiz ) - 1 );
		ivec2 size = textureSize( u_hiz, mip );
		vec2 d = texelFetch( u_hiz, min( p >> ( mip + 1 ), size - 1 ), mip ).rg;

		c = vec3( sqrt( clamp( d / max( far, 1.0 ), 0.0, 1.0 ) ), 0.0 );
	} else if ( pc.mode == 6 || pc.mode == 7 ) {
		// the nearest view distance of level 0 as a fraction of the far
		// distance, this frame's or the last one's
		ivec2 size = textureSize( u_hiz, 0 );
		ivec2 q = clamp( p >> 1, ivec2( 0 ), size - 1 );
		float d = pc.mode == 6 ? texelFetch( u_hiz, q, 0 ).r : texelFetch( u_prevHiz, q, 0 ).r;

		c = vec3( sqrt( clamp( d / max( far, 1.0 ), 0.0, 1.0 ) ) );
	} else if ( pc.mode == 5 ) {
		c = texelFetch( u_history, p, 0 ).rgb;
	} else if ( pc.mode == 8 ) {
		c = texelFetch( u_scene, p, 0 ).rgb;
	} else {
		c = texelFetch( u_color, p, 0 ).rgb;
		if ( pc.sharpen > 0.0 ) {
			// the pixel away from the mean of its 4 neighbours, no farther
			// than the darkest and the brightest of them (no halos)
			ivec2 lo = pc.rect.xy, hi = pc.rect.xy + pc.rect.zw - 1;
			vec3 l = texelFetch( u_color, clamp( p - ivec2( 1, 0 ), lo, hi ), 0 ).rgb;
			vec3 r = texelFetch( u_color, clamp( p + ivec2( 1, 0 ), lo, hi ), 0 ).rgb;
			vec3 u = texelFetch( u_color, clamp( p - ivec2( 0, 1 ), lo, hi ), 0 ).rgb;
			vec3 d = texelFetch( u_color, clamp( p + ivec2( 0, 1 ), lo, hi ), 0 ).rgb;
			vec3 nmin = min( min( min( l, r ), min( u, d ) ), c );
			vec3 nmax = max( max( max( l, r ), max( u, d ) ), c );

			c = clamp( c + ( c - 0.25 * ( l + r + u + d ) ) * pc.sharpen, nmin, nmax );
		}
	}
	out_color = vec4( clamp( c, 0.0, 1.0 ), 1.0 );
}
