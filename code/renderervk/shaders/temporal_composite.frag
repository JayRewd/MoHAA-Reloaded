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
// With HDR output (r_hdrOutput, vk_output.c) the scene is expanded toward
// the display's peak (HdrExpand). The ambient occlusion (r_ao) is in the
// scene already (ao_apply.frag). With r_dof the depth of field's result
// (dof.comp) is shown where a blur reaches the tile (Shown), its alpha saying
// which pixels it left as they were: only those are sharpened. With r_motionBlur the scene is blurred
// here (MOTION_BLUR, MotionBlur) along what motion_blur.comp made, after the
// anti-aliasing (its history stays sharp). r_temporalShow shows one of the
// buffers instead, with what temporal_common.glsl gives their readers (the
// camera's motion, the derived normals) from level 0 of the depth pyramid:
// at half the resolution, for a look.

#version 450
#extension GL_GOOGLE_include_directive : require
#include "temporal_common.glsl"

layout(set = 0, binding = 0) uniform sampler2D u_color;		// what is shown: the anti-aliased scene with r_taa, else the scene (and the depth of field's result where it blurs)
layout(set = 0, binding = 1) uniform sampler2D u_velocity;
layout(set = 0, binding = 2) uniform sampler2D u_normal;
layout(set = 0, binding = 3) uniform sampler2D u_hiz;		// this frame's depth pyramid (min, max), level 0 at half size
layout(set = 0, binding = 4) uniform sampler2D u_history;	// the last frame's scene color
layout(set = 0, binding = 5) uniform sampler2D u_prevHiz;	// the last frame's depth pyramid
layout(set = 0, binding = 6) uniform sampler2D u_scene;		// this frame's scene color, unresolved
layout(set = 0, binding = 7) uniform sampler2D u_ao;		// the ambient occlusion's factor (alpha, ao_denoise.comp)
layout(set = 0, binding = 8) uniform sampler2D u_aoRaw;		// and its occlusion before the blending and the blur (r)
layout(set = 0, binding = 9) uniform sampler2D u_mbVelocity;	// the motion blur's: each pixel's half vector in pixels (motion_blur.comp)
layout(set = 0, binding = 10) uniform sampler2D u_mbNeighbor;	// the longest half vector of the 3 x 3 tiles around each tile
layout(set = 0, binding = 11) uniform sampler2D u_dof;			// the depth of field's result (dof.comp), written in the tiles a blur reaches
layout(set = 0, binding = 12) uniform sampler2D u_dofNeighbor;	// its tiles: the largest blur of the 3 x 3 tiles around each (y)
layout(set = 0, binding = 13) uniform sampler2D u_mbSample;		// the motion blur's: each pixel's half vector's length, its view distance (what the samples read)

layout(push_constant) uniform Push {
	TemporalView	view;
	int		mode;		// r_temporalShow
	int		mip;		// r_temporalShowMip
	float	scale;		// r_temporalShowScale
	float	sharpen;	// r_taaSharpen when u_color is anti-aliased, else 0
	ivec4	rect;		// the scene's views: x, y, width, height in framebuffer pixels
	vec4	hdrA;		// HDR output's expansion: knee, b, the sky's b, the peak over paper white
	vec4	hdrB;		// the boost of what is above 1, r_saturation, 1 HDR expansion / 2 the HDR look in SDR / 0 none, r_hdrBlack for the look (VK_OutputExpansion)
	ivec4	mb;			// the motion blur's tile size, its pairs of samples (r_motionBlurQuality), 0, 0
	ivec4	dof;		// the depth of field's tile size (DEPTH_OF_FIELD), 1: r_temporalShow 13 (its result everywhere), 0, 0
	vec4	dofScale;	// 1 / its tile size, 0, 0, 0
} pc;

// HDR output: the pipeline expands the scene (vk_output.c)
layout(constant_id = 0) const bool HDR_EXPAND = false;
// r_motionBlur: the pipeline blurs the scene (vk_temporal.c, compositeBlurPipeline)
layout(constant_id = 1) const bool MOTION_BLUR = false;
// r_dof: the depth of field's result is shown where it blurs (Shown). A
// constant: as a uniform branch on pc.dof the motion blur's gather took
// 0.022 ms longer without the depth of field (2560x1440, turning)
layout(constant_id = 2) const bool DEPTH_OF_FIELD = false;
// with the motion blur too the depth of field's pass writes every tile: its
// result is shown as it is (a choice for each of the gather's samples cost
// what the pass saved)
const bool DOF_EVERYWHERE = DEPTH_OF_FIELD && MOTION_BLUR;

layout(location = 0) out vec4 out_color;

// the depth of field's tile of a pixel (q + 0.5 is never on a tile's edge:
// the float product floors as the integer division does)
ivec2 DofTile( ivec2 q ) {
	return ivec2( ( vec2( q ) + 0.5 ) * pc.dofScale.x );
}

// whether the depth of field's result is shown in a tile: a blur reaches it
// (the depth of field's pass writes only those tiles: the others hold older
// frames' values), or r_temporalShow 13
bool DofTileShown( ivec2 t ) {
	return DOF_EVERYWHERE || pc.dof.y != 0 || texelFetch( u_dofNeighbor, t, 0 ).y >= 0.5;
}

// what is shown at a pixel: the depth of field's result where it is shown
// (alpha 1 where it left the pixel as it was, 0 where it blurred it), else
// the scene (alpha 1: sharpened)
vec4 ShownIn( ivec2 q, bool dof ) {
	return dof ? texelFetch( u_dof, q, 0 ) : vec4( texelFetch( u_color, q, 0 ).rgb, 1.0 );
}

// the same for a pixel whose tile isn't known
vec4 Shown( ivec2 q ) {
	return ShownIn( q, DEPTH_OF_FIELD && DofTileShown( DofTile( q ) ) );
}

// for a neighbour of a pixel whose tile is t, shown as dof says: its own
// tile looked up only when it is another one
vec4 ShownNear( ivec2 q, ivec2 t, bool dof ) {
	if ( DEPTH_OF_FIELD && !DOF_EVERYWHERE ) {
		ivec2 tq = DofTile( q );

		if ( tq != t ) {
			dof = DofTileShown( tq );
		}
	}
	return ShownIn( q, dof );
}

// the nearest view distance around a pixel, from level 0 of the pyramid
float Distance( ivec2 p ) {
	ivec2 size = textureSize( u_hiz, 0 );

	return texelFetch( u_hiz, clamp( p >> 1, ivec2( 0 ), size - 1 ), 0 ).r;
}

// r_saturation about BT.709 luma, in linear light
vec3 Saturate( vec3 lin ) {
	return max( mix( vec3( dot( lin, vec3( 0.2126, 0.7152, 0.0722 ) ) ), lin, pc.hdrB.y ), vec3( 0.0 ) );
}

// the game's values are gamma 2.2 (an SDR monitor's)
const float GAMMA = 2.2;

// the expansion (VK_OutputExpansion), in linear light of paper white. The
// pixel up to the game's white, each channel clamped at 1 (the color SDR
// shows): its largest channel m unchanged up to the knee k, above it k + (1
// - k)(t + b t^2) with t = (m - k) / (1 - k), the pixel scaled with it (its
// hue kept); what is above 1 (overexposed surfaces, stacked effects) added
// as gray of its BT.709 luma, times the expanded white and the effects'
// boost, so it goes toward white as SDR's clip does instead of showing the
// overexposed color's own hue (which looked more saturated than SDR).
// Clamped at the peak P
vec3 Expand( vec3 lin, bool sky ) {
	float k = pc.hdrA.x;
	float b = sky ? pc.hdrA.z : pc.hdrA.y;
	vec3 base = min( lin, vec3( 1.0 ) );
	float excess = dot( max( lin - 1.0, vec3( 0.0 ) ), vec3( 0.2126, 0.7152, 0.0722 ) );
	float m = max( base.r, max( base.g, base.b ) );

	if ( m > k ) {
		float t = ( m - k ) / ( 1.0 - k );

		base *= ( k + ( 1.0 - k ) * ( t + b * t * t ) ) / m;
	}
	// the expanded white: k + (1 - k)(1 + b)
	return min( base + excess * pc.hdrB.x * ( k + ( 1.0 - k ) * ( 1.0 + b ) ), vec3( pc.hdrA.w ) );
}

// HDR output: the expansion and the saturation, encoded again: the frame
// holds the game's values above 1 too (output.frag decodes them)
vec3 HdrExpand( vec3 c, bool sky ) {
	vec3 lin = Saturate( Expand( pow( max( c, vec3( 0.0 ) ), vec3( GAMMA ) ), sky ) );

	return pow( min( lin, vec3( pc.hdrA.w ) ), vec3( 1.0 / GAMMA ) );
}

// r_hdrOutput 2, the HDR look on an SDR display: the expansion up to P, the
// expanded white (VK_OutputExpansion), then a shoulder that puts [0.5, P]
// into [0.5, 1] on the
// largest channel (0.5 + 0.5 u (1 + u / w^2) / (1 + u), u = (m - 0.5) / 0.5,
// w = (P - 0.5) / 0.5: continuous in value and slope at 0.5, P lands at 1),
// the saturation and r_hdrBlack's black level (output.frag's), clamped at 1
vec3 SdrLook( vec3 c, bool sky ) {
	const float s = 0.5;
	vec3 lin = Expand( pow( max( c, vec3( 0.0 ) ), vec3( GAMMA ) ), sky );
	float m = max( lin.r, max( lin.g, lin.b ) );
	float black = pc.hdrB.w;

	if ( m > s ) {
		float u = ( m - s ) / ( 1.0 - s );
		float w = ( pc.hdrA.w - s ) / ( 1.0 - s );

		lin *= ( s + ( 1.0 - s ) * u * ( 1.0 + u / ( w * w ) ) / ( 1.0 + u ) ) / m;
	}
	lin = Saturate( lin );
	m = max( lin.r, max( lin.g, lin.b ) );
	if ( black != 0.0 && m > 0.0 ) {
		lin *= pow( m / ( m + 0.005 ), black );
	}
	return pow( clamp( lin, 0.0, 1.0 ), vec3( 1.0 / GAMMA ) );
}

// the motion blur's gather: McGuire et al., "A Reconstruction Filter for
// Plausible Motion Blur" (2012), as Jimenez refined it for Call of Duty:
// Advanced Warfare ("Next Generation Post Processing in Call of Duty", 2014).
// Where the longest blur around the pixel's tile is half a pixel or more,
// pairs of samples on both sides of the pixel along it, at offsets moved by
// a noise fixed per pixel (nothing blends it over frames after this), every
// other pair along the pixel's own blur when it goes another way. A sample
// adds its color where it is behind the pixel and the pixel's blur reaches
// it (what the pixel's motion uncovers), or in front of the pixel and its
// own blur reaches the pixel (what moves over it), the depths compared
// softly; a pair whose nearer sample hides what the farther one shows takes
// the farther one's weight for both (the mirror: the background behind what
// moves is estimated from the other side). What the samples don't cover is
// the pixel's own color. Summed in linear light

const float MB_SOFT_DEPTH = 0.05;	// the depths a comparison fades over: this share of the nearer one, plus 1 unit

float MbNoise( vec2 pixel ) {
	return fract( 52.9829189 * fract( dot( pixel, vec2( 0.06711056, 0.00583715 ) ) ) );
}

vec3 MbLinear( vec3 c ) {
	return pow( max( c, vec3( 0.0 ) ), vec3( GAMMA ) );
}

// x: the sample is behind the center, y: in front of it
vec2 MbDepthCompare( float center, float s ) {
	float extent = MB_SOFT_DEPTH * min( center, s ) + 1.0;

	return clamp( 0.5 + vec2( 1.0, -1.0 ) * ( s - center ) / extent, 0.0, 1.0 );
}

// whether blurs of these half lengths (the center's, the sample's) reach the
// offset, over half a pixel: a still pixel (under half a pixel) reaches only
// itself, so in a tile where something moves what doesn't stays as it is
// (with a pixel, as Jimenez has it, the neighbours of still pixels there
// blended in a little: the tiles showed)
vec2 MbSpread( float offset, vec2 spread ) {
	return clamp( spread - offset + 0.5, 0.0, 1.0 );
}

// tile and dof: the pixel's depth of field's tile and whether its result is
// shown there (ShownNear: a sample in the same tile looks nothing up)
vec3 MotionBlur( ivec2 p, vec3 center, ivec2 tile, bool dof ) {
	ivec2 lo = pc.rect.xy, hi = pc.rect.xy + pc.rect.zw - 1;
	vec2 vn = texelFetch( u_mbNeighbor, p / pc.mb.x, 0 ).xy;
	float nLen = length( vn );

	if ( nLen < 0.5 ) {
		return center;
	}

	vec2 vc = texelFetch( u_mbVelocity, p, 0 ).xy;
	vec2 cs = texelFetch( u_mbSample, p, 0 ).xy;
	float cLen = cs.x;
	float cd = cs.y;
	int pairs = pc.mb.y;
	float jitter = MbNoise( gl_FragCoord.xy ) - 0.5;
	// its own blur differs from the tile's by more than about 25 degrees
	bool own = cLen >= 0.5 && dot( vc, vn ) < 0.9 * cLen * nLen;
	vec4 sum = vec4( 0.0 );

	for ( int i = 0; i < pairs; i++ ) {
		vec2 v = own && ( i & 1 ) != 0 ? vc : vn;
		vec2 offset = v * ( ( float( i ) + 0.5 + jitter ) / float( pairs ) );
		float offsetLen = length( offset );
		ivec2 p1 = clamp( ivec2( gl_FragCoord.xy + offset ), lo, hi );
		ivec2 p2 = clamp( ivec2( gl_FragCoord.xy - offset ), lo, hi );
		vec2 s1 = texelFetch( u_mbSample, p1, 0 ).xy;
		vec2 s2 = texelFetch( u_mbSample, p2, 0 ).xy;
		float l1 = s1.x, l2 = s2.x;
		float w1 = dot( MbDepthCompare( cd, s1.y ), MbSpread( offsetLen, vec2( cLen, l1 ) ) );
		float w2 = dot( MbDepthCompare( cd, s2.y ), MbSpread( offsetLen, vec2( cLen, l2 ) ) );
		bvec2 mirror = bvec2( s1.y > s2.y, l2 > l1 );

		w1 = all( mirror ) ? w2 : w1;
		w2 = any( mirror ) ? w2 : w1;
		sum += w1 * vec4( MbLinear( ShownNear( p1, tile, dof ).rgb ), 1.0 );
		sum += w2 * vec4( MbLinear( ShownNear( p2, tile, dof ).rgb ), 1.0 );
	}
	if ( sum.a <= 0.0 ) {
		return center;
	}
	sum /= float( 2 * pairs );
	return pow( max( sum.rgb + ( 1.0 - sum.a ) * MbLinear( center ), vec3( 0.0 ) ), vec3( 1.0 / GAMMA ) );
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
	} else if ( pc.mode == 14 ) {
		// the effects' reactive share (the scene's alpha, VK_PIPE_REACTIVE)
		// in red over the scene's brightness
		vec4 scene = texelFetch( u_scene, p, 0 );

		c = mix( vec3( dot( clamp( scene.rgb, 0.0, 1.0 ), vec3( 0.2126, 0.7152, 0.0722 ) ) * 0.5 ), vec3( 1.0, 0.0, 0.0 ), clamp( scene.a, 0.0, 1.0 ) );
	} else if ( pc.mode == 9 ) {
		// the factor the scene was multiplied with (gamma-encoded)
		c = vec3( texelFetch( u_ao, p, 0 ).a );
	} else if ( pc.mode == 10 ) {
		// the visibility of one frame, before the blending and the blur
		c = vec3( texelFetch( u_aoRaw, p >> 1, 0 ).r );
	} else if ( pc.mode == 11 || pc.mode == 12 ) {
		// the motion blur's half vector of the pixel, or the longest around
		// its tile, in mode 1's colors: 1 the longest (a tile's side)
		float k = float( max( pc.mb.x, 1 ) );
		vec2 v = ( pc.mode == 11 ? texelFetch( u_mbVelocity, p, 0 ).xy : texelFetch( u_mbNeighbor, p / max( pc.mb.x, 1 ), 0 ).xy ) / k;

		c = vec3( max( v.x, 0.0 ), max( v.y, 0.0 ), max( -v.x, 0.0 ) + max( -v.y, 0.0 ) );
	} else {
		ivec2 tile = DEPTH_OF_FIELD ? DofTile( p ) : ivec2( 0 );
		bool dof = DEPTH_OF_FIELD && DofTileShown( tile );
		vec4 cs = ShownIn( p, dof );
		// the depth of field's blur isn't sharpened
		float sharpen = pc.sharpen * cs.a;

		c = cs.rgb;
		if ( sharpen > 0.0 ) {
			// the pixel away from the mean of its 4 neighbours, no farther
			// than the darkest and the brightest of them (no halos)
			ivec2 lo = pc.rect.xy, hi = pc.rect.xy + pc.rect.zw - 1;
			vec3 l = ShownNear( clamp( p - ivec2( 1, 0 ), lo, hi ), tile, dof ).rgb;
			vec3 r = ShownNear( clamp( p + ivec2( 1, 0 ), lo, hi ), tile, dof ).rgb;
			vec3 u = ShownNear( clamp( p - ivec2( 0, 1 ), lo, hi ), tile, dof ).rgb;
			vec3 d = ShownNear( clamp( p + ivec2( 0, 1 ), lo, hi ), tile, dof ).rgb;
			vec3 nmin = min( min( min( l, r ), min( u, d ) ), c );
			vec3 nmax = max( max( max( l, r ), max( u, d ) ), c );

			c = clamp( c + ( c - 0.25 * ( l + r + u + d ) ) * sharpen, nmin, nmax );
		}
		if ( MOTION_BLUR ) {
			c = MotionBlur( p, c, tile, dof );
		}
		if ( HDR_EXPAND && pc.hdrB.z > 0.5 && pc.hdrB.z < 1.5 ) {
			out_color = vec4( HdrExpand( c, TemporalClass( texelFetch( u_normal, p, 0 ) ) == TEMPORAL_CLASS_SKY ), 1.0 );
			return;
		}
		if ( pc.hdrB.z > 1.5 ) {
			out_color = vec4( SdrLook( c, TemporalClass( texelFetch( u_normal, p, 0 ) ) == TEMPORAL_CLASS_SKY ), 1.0 );
			return;
		}
		if ( pc.hdrB.y != 1.0 ) {
			// r_saturation without the expansion (SDR), in linear light
			c = pow( Saturate( pow( clamp( c, 0.0, 1.0 ), vec3( 2.2 ) ) ), vec3( 1.0 / 2.2 ) );
		}
	}
	out_color = vec4( clamp( c, 0.0, 1.0 ), 1.0 );
}
