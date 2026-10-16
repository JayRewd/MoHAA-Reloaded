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

// skinned.vert -- vertex shader of the skeletal models skinned and lit on
// the GPU (tr_skin.c, vk_draw with drawGeometry_t::isSkinned)
//
// It does what RB_SkelMesh and RB_Light_Real (rgbGen lightingSpherical)
// did on the CPU, with the same floating point operations in the same
// order (precise: no fused multiply-adds the C code doesn't have), so
// positions, normals and colors come out the same to the bit; divisions
// get one correction step, which makes them correctly rounded like the
// CPU's in all but very rare cases. The outputs are generic.vert's.
//
// The vertex comes from the skin data (gl_VertexIndex: the draw's
// vertexOffset is the surface's first vertex record), what changes per
// frame from the frame data, starting at the draw record gl_InstanceIndex
// (the draw's firstInstance). Layouts: see vkSkinDraw_t in vk.h.

#version 450

layout(push_constant) uniform PushConstants {
	mat4	mvp;
	vec4	eyeZ;
	vec4	clipPlane;
} pc;

layout(std430, set = 2, binding = 1) readonly buffer SkinData {
	uint	skin[];
};

layout(std430, set = 2, binding = 2) readonly buffer SkinFrame {
	uint	frame[];
};

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_st0;
layout(location = 2) out vec2 v_st1;
layout(location = 3) out vec2 v_fogClip;
layout(location = 4) out vec3 v_position;

#ifdef CLIP_DISTANCE
out float gl_ClipDistance[1];
#endif

invariant gl_Position;

// lighttype_t
const uint LIGHT_POINT = 0u;
const uint LIGHT_DIRECTIONAL = 1u;
const uint LIGHT_SPOT = 2u;
const uint LIGHT_SPOT_FAST = 3u;

const uint NONE = 0xFFFFFFFFu;

float skinFloat( uint i ) {
	return uintBitsToFloat( skin[i] );
}

float frameFloat( uint i ) {
	return uintBitsToFloat( frame[i] );
}

vec3 frameVec3( uint i ) {
	return vec3( frameFloat( i ), frameFloat( i + 1u ), frameFloat( i + 2u ) );
}

// a / b, rounded like the CPU's division: the reciprocal of the GPU is
// within an ulp, one correction step with the exact residual makes the
// quotient correctly rounded but for very rare operands
float divide( float a, float b ) {
	precise float r = 1.0 / b;
	precise float q = a * r;
	precise float e = fma( -b, q, a );
	precise float c = fma( e, r, q );
	return c;
}

// c / 255 correctly rounded, the value of an R8G8B8A8_UNORM vertex color
// (the CPU path's colors): exact for every byte with one correction step
float unorm8( uint c ) {
	const float r = uintBitsToFloat( 0x3B808081u );	// 1 / 255
	precise float q = float( c ) * r;
	precise float e = fma( -255.0, q, float( c ) );
	precise float d = fma( e, r, q );
	return d;
}

vec4 unpackColor( uint c ) {
	return vec4( unorm8( c & 255u ), unorm8( ( c >> 8 ) & 255u ), unorm8( ( c >> 16 ) & 255u ), unorm8( c >> 24 ) );
}

// DotProduct of q_shared.h
float dot3( vec3 a, vec3 b ) {
	precise float d = a.x * b.x + a.y * b.y + a.z * b.z;
	return d;
}

// the first word of a bone in the frame data (skelBoneCache_t: offset,
// then the rows of the rotation)
uint bone( uint draw, uint index ) {
	uint remap = frame[draw + 1u];

	if ( remap != NONE ) {
		index = skin[remap + index];
	}
	return frame[draw] + index * 16u;
}

// RB_Light_Real: the lights of the entity's sphere, in its space
vec4 lightColor( uint list, vec3 xyz, vec3 normal ) {
	uint numLights = frame[list];
	uint ambient = frame[list + 1u];
	precise vec3 colorout = vec3( 0.0 );

	for ( uint j = 0u; j < numLights; j++ ) {
		uint l = list + 2u + j * 16u;
		uint type = frame[l];
		vec3 color = frameVec3( l + 1u );
		vec3 origin = frameVec3( l + 4u );
		vec3 direction = frameVec3( l + 7u );
		precise vec3 v;
		precise float fDot, fDistSquared, fProjSquared, fMaxIntensity, proj;

		if ( type == LIGHT_DIRECTIONAL ) {
			fDot = dot3( direction, normal );
			if ( fDot > 0.0 ) {
				colorout = colorout + color * fDot;
			}
		} else if ( type == LIGHT_SPOT ) {
			fDot = dot3( direction, normal );
			if ( fDot > 0.0 ) {
				v = origin - xyz;
				proj = dot3( v, direction );
				fProjSquared = proj * proj;
				fDistSquared = dot3( v, v );
				fMaxIntensity = ( frameFloat( l + 10u ) - divide( fDistSquared, fProjSquared ) ) * frameFloat( l + 11u );
				if ( fMaxIntensity > 0.0 ) {
					fDot = divide( fDot, fDistSquared );
					if ( fMaxIntensity < 1.0 ) {
						fDot = fDot * fMaxIntensity;
					}
					colorout = colorout + color * fDot;
				}
			}
		} else if ( type == LIGHT_SPOT_FAST ) {
			fDot = dot3( direction, normal );
			if ( fDot > 0.0 ) {
				v = origin - xyz;
				fDistSquared = dot3( v, v );
				colorout = colorout + color * divide( fDot, fDistSquared );
			}
		} else {
			v = origin - xyz;
			fDot = dot3( v, normal );
			if ( fDot > 0.0 ) {
				fDistSquared = dot3( v, v );
				colorout = colorout + color * divide( fDot, fDistSquared );
			}
		}
	}

	ivec3 level = ivec3( ambient & 255u, ( ambient >> 8 ) & 255u, ( ambient >> 16 ) & 255u );
	uvec3 c = uvec3( clamp( ivec3( colorout ) + level, 0, 255 ) );
	return vec4( unorm8( c.r ), unorm8( c.g ), unorm8( c.b ), 1.0 );
}

void main() {
	uint draw = uint( gl_InstanceIndex ) * 8u;
	uint renderCount = frame[draw + 6u];
	uint firstVertex = frame[draw + 7u];
	uint v = uint( gl_VertexIndex ) - firstVertex;

	// RB_SkelMesh's collapse map at a lower level of detail
	while ( v >= renderCount ) {
		v = skin[( firstVertex + v ) * 8u + 7u];
	}
	uint vert = ( firstVertex + v ) * 8u;
	uint morphs = frame[draw + 2u];
	uint lights = frame[draw + 3u];
	float scale = frameFloat( draw + 4u );
	uint counts = skin[vert + 5u];
	uint numWeights = counts & 0xFFFFu;
	uint numMorphs = counts >> 16;
	uint p = vert + skin[vert + 6u];
	vec3 normalIn = vec3( skinFloat( vert ), skinFloat( vert + 1u ), skinFloat( vert + 2u ) );

	// SkelMorphGetXyz
	precise vec3 totalmorph = vec3( 0.0 );
	for ( uint m = 0u; m < numMorphs; m++, p += 4u ) {
		int weight = morphs != NONE ? int( frame[morphs + skin[p]] ) : 0;

		if ( weight != 0 ) {
			totalmorph = totalmorph + vec3( skinFloat( p + 1u ), skinFloat( p + 2u ), skinFloat( p + 3u ) ) * float( weight );
		}
	}

	// SkelVertGetNormal with the first weight's bone, SkelWeightGetXyz and
	// SkelWeightMorphGetXyz (the first weight of an entity with morphs)
	precise vec3 normal = vec3( 0.0 );
	precise vec3 xyz = vec3( 0.0 );
	for ( uint w = 0u; w < numWeights; w++, p += 5u ) {
		uint b = bone( draw, skin[p] );
		vec3 offset = frameVec3( b );
		vec3 row0 = frameVec3( b + 4u );
		vec3 row1 = frameVec3( b + 8u );
		vec3 row2 = frameVec3( b + 12u );
		precise vec3 point = vec3( skinFloat( p + 2u ), skinFloat( p + 3u ), skinFloat( p + 4u ) );
		precise vec3 t;

		if ( w == 0u ) {
			normal = normalIn.x * row0 + normalIn.y * row1 + normalIn.z * row2;
			if ( morphs != NONE ) {
				point = totalmorph + point;
			}
		}
		t = ( point.x * row0 + point.y * row1 + point.z * row2 ) + offset;
		xyz = xyz + t * skinFloat( p + 1u );
	}
	xyz = xyz * scale;

	// generic.vert
	vec4 pos = pc.mvp[0] * xyz.x;
	pos = fma( pc.mvp[1], vec4( xyz.y ), pos );
	pos = fma( pc.mvp[2], vec4( xyz.z ), pos );
	gl_Position = pos + pc.mvp[3];

	float eyeZ = pc.eyeZ.x * xyz.x;
	eyeZ = fma( pc.eyeZ.y, xyz.y, eyeZ );
	eyeZ = fma( pc.eyeZ.z, xyz.z, eyeZ );
	v_fogClip.x = abs( eyeZ + pc.eyeZ.w );

	v_fogClip.y = dot( pc.clipPlane, vec4( xyz, 1.0 ) );
#ifdef CLIP_DISTANCE
	gl_ClipDistance[0] = v_fogClip.y;
#endif

	v_color = lights != NONE ? lightColor( lights, xyz, normal ) : unpackColor( frame[draw + 5u] );
	v_st0 = vec2( skinFloat( vert + 3u ), skinFloat( vert + 4u ) );
	v_st1 = vec2( 0.0 );
	v_position = xyz;
}
