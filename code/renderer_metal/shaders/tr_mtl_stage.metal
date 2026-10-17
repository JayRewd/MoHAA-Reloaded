// tr_mtl_stage.metal -- the generic stage shader: GL1's fixed-function pipeline as the front end drives it
// (two texture units with texenv, vertex or constant color, alpha test, linear fog, one user clip plane),
// plus the quad that clears part of the scene target in the middle of a render pass.
//
// Vertices are fetched by hand from the frame's buffer ring: the backend copies the front end's client
// arrays (or the immediate-mode vertices) into the ring as they are, and passes per-stream offsets and
// strides. So no array is reformatted on the CPU and the pipelines don't depend on vertex layouts.
//
// Specialization (function constants) is kept to what changes the shader's structure: which texture units
// are sampled, and whether the shader can discard (alpha test; a discard in every pipeline would cost hidden
// surface removal on opaque surfaces). Texenv modes, the alpha function and fog are uniform branches, so the
// number of pipelines (and first-use compiles) stays small.
// Measured side effect: hidden surface removal breaks exact depth ties between triangles of one draw
// differently from GL1 on Apple's GL (which behaves like a pipeline that can discard). So models with
// coincident faces (static models, the view weapon, stitched terrain) differ from GL1 by a few levels in up
// to ~2500 edge pixels of a 1280x720 view. With a discard in every pipeline the fixed views (port/shots)
// matched GL1 to 2-24 pixels; not worth the GPU cost.
//
// Keep the structs in sync with tr_mtl.m.

#include <metal_stdlib>
using namespace metal;

constant uint mb_texMask	[[function_constant(0)]];	// bit 0: unit 0 sampled, bit 1: unit 1
constant bool mb_alphaTest	[[function_constant(1)]];

constant bool mb_tex0 = ( mb_texMask & 1u ) != 0u;
constant bool mb_tex1 = ( mb_texMask & 2u ) != 0u;

struct MbVSUniforms
{
	float4x4	mvp;			// projection * modelview (GL clip space)
	float4		clipPlane;		// object space; (0,0,0,1) when the clip plane is off
	float4		eyeZ;			// modelview row 2: eye-space z = dot(eyeZ, position)
	float4		color;			// constant color when the color array is off
	float4		texCoord;		// constant texcoords when the arrays are off: unit 0 xy, unit 1 zw
	uint4		offset;			// ring byte offsets: position, color, texcoord 0, texcoord 1
	uint4		stride;			// byte strides; 0 = not streamed (use the constant)
	uint		posSize;		// 2, 3 or 4 floats
	uint		colorFloat;		// the color stream is float4 (immediate mode), else unorm uchar4
	uint		skin;			// mb_skin_vs: ring byte offset of the draw's MbSkinUniforms
	uint		pad;
};

// texenv modes (MB_TexEnvIndex in tr_mtl.m)
#define MB_ENV_MODULATE		0u
#define MB_ENV_REPLACE		1u
#define MB_ENV_DECAL		2u
#define MB_ENV_ADD			3u
#define MB_ENV_COMBINE		4u

// alpha functions: MB_ compare func - MB_NEVER
#define MB_AF_NEVER			0u
#define MB_AF_LESS			1u
#define MB_AF_EQUAL			2u
#define MB_AF_LEQUAL		3u
#define MB_AF_GREATER		4u
#define MB_AF_NOTEQUAL		5u
#define MB_AF_GEQUAL		6u

#define MB_FS_FOG			1u
#define MB_FS_RGB0			2u		// unit 0's texture has no alpha channel (MB_RGB): replace keeps the incoming alpha
#define MB_FS_RGB1			4u

struct MbFSUniforms
{
	float4		fogColor;
	float		fogEnd;
	float		fogScale;		// 1 / (end - start)
	float		alphaRef;
	uint		alphaFunc;
	uint		env0;
	uint		env1;
	uint		flags;
	uint		pad;
};

struct MbStageOut
{
	float4	position [[position, invariant]];
	float4	color;
	float2	tc0;
	float2	tc1;
	float	eyeZ;
	float	clip [[clip_distance]] [1];
};

// what the fragment stage reads of MbStageOut (a clip distance array can't be a stage_in member)
struct MbStageIn
{
	float4	position [[position]];
	float4	color;
	float2	tc0;
	float2	tc1;
	float	eyeZ;
};

vertex MbStageOut mb_stage_vs( uint vid [[vertex_id]],
	device const uchar *ring [[buffer(0)]],
	constant MbVSUniforms &u [[buffer(1)]] )
{
	MbStageOut out;
	device const float *p = (device const float *)( ring + u.offset.x + vid * u.stride.x );
	float4 pos = float4( p[0], p[1], 0.0f, 1.0f );

	if ( u.posSize > 2u ) {
		pos.z = p[2];
		if ( u.posSize > 3u ) {
			pos.w = p[3];
		}
	}

	if ( u.stride.y == 0u ) {
		out.color = u.color;
	} else if ( u.colorFloat ) {
		out.color = float4( *(device const packed_float4 *)( ring + u.offset.y + vid * u.stride.y ) );
	} else {
		out.color = float4( *(device const uchar4 *)( ring + u.offset.y + vid * u.stride.y ) ) * ( 1.0f / 255.0f );
	}

	out.tc0 = u.stride.z ? float2( *(device const packed_float2 *)( ring + u.offset.z + vid * u.stride.z ) ) : u.texCoord.xy;
	out.tc1 = u.stride.w ? float2( *(device const packed_float2 *)( ring + u.offset.w + vid * u.stride.w ) ) : u.texCoord.zw;

	out.position = u.mvp * pos;
	out.position.z = ( out.position.z + out.position.w ) * 0.5f;	// GL clip z [-w, w] -> Metal [0, w]
	out.eyeZ = dot( u.eyeZ, pos );
	out.clip[0] = dot( u.clipPlane, pos );
	return out;
}

//
// GPU skinning (tr_mtl.h mbSkin*): mb_skin_vs computes what RB_SkelMesh (tr_model.cpp) and RB_Light_Real
// (tr_sphere_shade.cpp) compute on the CPU for a vertex-lit skeletal surface, then continues as mb_stage_vs.
// MB_Skin repeats the CPU's arithmetic operation for operation, in safe math (no reassociation): clang fuses
// a*b + c within one C expression into an fma (-ffp-contract=on; in a*b + c*d the left product is fused), so
// those are written as fma() here in the same order, the divisions are precise (IEEE, like the CPU's), and the
// colors get the CPU's float -> int conversion (arm64 fcvtzs: toward zero, NaN -> 0, saturating) and its int
// clamp. Keep the structs in sync with tr_mtl.m.
//
struct MbSkinUniforms
{
	uint		numJobs;
	int			numLights;		// -1: colors from the color stream
	uint		ambient;		// rgba bytes
	uint		pad;
	// then numJobs MbSkinJob, then numLights MbSkinLight
};

struct MbSkinJob
{
	uint		firstVertex;
	uint		numVertexes;
	uint		surface;		// skin buffer word offset: per vertex the word offset of its record, then the records
	uint		boneMap;		// skin buffer word offset of the weight bone -> bone table, MB_SKIN_NONE: identity
	uint		bones;			// ring byte offset of the MbSkinBones
	uint		morphs;			// ring byte offset of the morph weights (int), MB_SKIN_NONE: no morphs
	float		scale;
	uint		pad;
};

struct MbSkinLight				// reallightinfo_t
{
	packed_float3	color;
	int				type;
	float			intensity;
	float			dist;
	float			spotSlope;
	float			spotConst;
	float			spotScale;
	packed_float3	origin;
	packed_float3	direction;
};

struct MbSkinBone				// skelBoneCache_t
{
	float4		offset;
	float4		m[3];
};

#define MB_SKIN_NONE			0xffffffffu

// lighttype_t
#define MB_LIGHT_POINT			0
#define MB_LIGHT_DIRECTIONAL	1
#define MB_LIGHT_SPOT			2
#define MB_LIGHT_SPOT_FAST		3

struct MbSkinVertex
{
	float3		xyz;
	float2		tc;
	uchar4		color;
};

// DotProduct (and VectorLengthSquared with a == b): fma( x2, y2, fma( x0, y0, x1 * y1 ) )
static float MB_SkinDot( float3 a, float3 b )
{
#pragma METAL fp math_mode(safe)
	return fma( a.z, b.z, fma( a.x, b.x, a.y * b.y ) );
}

// Q_clamp_int( (int)x + ambient, 0, 255 )
static uchar MB_SkinClamp( float x, uint ambient )
{
#pragma METAL fp math_mode(safe)
	int i;

	if ( isnan( x ) ) {
		i = 0;
	} else if ( x >= 2147483648.0f ) {
		i = 0x7fffffff;
	} else if ( x <= -2147483648.0f ) {
		i = (int)0x80000000u;
	} else {
		i = (int)x;
	}
	i = as_type<int>( as_type<uint>( i ) + ambient );
	return (uchar)clamp( i, 0, 255 );
}

static MbSkinVertex MB_Skin( uint vid, device const uchar *ring, device const uint *skin, uint uniforms )
{
#pragma METAL fp math_mode(safe)
	device const MbSkinUniforms *su = (device const MbSkinUniforms *)( ring + uniforms );
	device const MbSkinJob *jobs = (device const MbSkinJob *)( su + 1 );
	device const MbSkinLight *lights = (device const MbSkinLight *)( jobs + su->numJobs );
	device const MbSkinBone *bones;
	device const uint *rec;
	device const uint *w;
	MbSkinVertex v;
	MbSkinJob job;
	MbSkinBone b;
	float3 normal, out, totalMorph;
	uint j, numWeights, numMorphs, bone, k;
	uchar4 ambient = as_type<uchar4>( su->ambient );
	int numLights = su->numLights;

	// the job (surface) whose vertexes vid is in
	for ( j = 0; j + 1 < su->numJobs && vid >= jobs[j + 1].firstVertex; j++ ) {
	}
	job = jobs[j];
	bones = (device const MbSkinBone *)( ring + job.bones );
	rec = skin + job.surface + skin[job.surface + vid - job.firstVertex];

	numWeights = rec[5];
	numMorphs = rec[6];
	w = rec + 7 + numMorphs * 4;	// the first weight

	// RB_SkelMesh: the normal goes with the first weight's bone (SkelVertGetNormal)
	bone = job.boneMap == MB_SKIN_NONE ? w[0] : skin[job.boneMap + w[0]];
	b = bones[bone];
	{
		float3 n = float3( as_type<float>( rec[0] ), as_type<float>( rec[1] ), as_type<float>( rec[2] ) );

		normal = fma( float3( n.z ), b.m[2].xyz, fma( float3( n.x ), b.m[0].xyz, n.y * b.m[1].xyz ) );
	}

	// morphs (SkelMorphGetXyz), then the weights (SkelWeightMorphGetXyz for the first, SkelWeightGetXyz)
	totalMorph = float3( 0.0f );
	if ( job.morphs != MB_SKIN_NONE ) {
		device const int *morphs = (device const int *)( ring + job.morphs );

		for ( k = 0; k < numMorphs; k++ ) {
			device const uint *m = rec + 7 + k * 4;
			int weight = morphs[m[0]];

			if ( weight ) {
				float3 offset = float3( as_type<float>( m[1] ), as_type<float>( m[2] ), as_type<float>( m[3] ) );

				totalMorph = fma( offset, float3( (float)weight ), totalMorph );
			}
		}
	}

	out = float3( 0.0f );
	for ( k = 0; k < numWeights; k++, w += 5 ) {
		float3 p = float3( as_type<float>( w[2] ), as_type<float>( w[3] ), as_type<float>( w[4] ) );
		float3 s;

		if ( k ) {
			bone = job.boneMap == MB_SKIN_NONE ? w[0] : skin[job.boneMap + w[0]];
			b = bones[bone];
		}
		if ( !k && job.morphs != MB_SKIN_NONE ) {
			p = totalMorph + p;
		}
		s = fma( float3( p.z ), b.m[2].xyz, fma( float3( p.x ), b.m[0].xyz, p.y * b.m[1].xyz ) );
		out = fma( s + b.offset.xyz, float3( as_type<float>( w[1] ) ), out );
	}

	v.xyz = out * job.scale;
	v.tc = float2( as_type<float>( rec[3] ), as_type<float>( rec[4] ) );

	// RB_Light_Real (not the cube map variant: the CPU keeps it)
	v.color = ambient;
	if ( numLights > 1 ) {
		float3 lc = float3( 0.0f );

		for ( int i = 0; i < numLights; i++ ) {
			MbSkinLight light = lights[i];
			float3 color = light.color, dir = light.direction, d;
			float fDot, distSquared;

			switch ( light.type ) {
			case MB_LIGHT_DIRECTIONAL:
				fDot = MB_SkinDot( dir, normal );
				fDot = fDot > 0.0f ? fDot : 0.0f;
				lc = fma( color, float3( fDot ), lc );
				break;
			case MB_LIGHT_SPOT:
				fDot = MB_SkinDot( dir, normal );
				if ( fDot > 0.0f ) {
					float proj, maxIntensity;

					d = float3( light.origin ) - v.xyz;
					proj = MB_SkinDot( d, dir );
					distSquared = MB_SkinDot( d, d );
					maxIntensity = ( light.spotConst - precise::divide( distSquared, proj * proj ) ) * light.spotScale;
					if ( maxIntensity > 0.0f ) {
						fDot = precise::divide( fDot, distSquared );
						if ( maxIntensity < 1.0f ) {
							fDot = fDot * maxIntensity;
						}
						lc = fma( color, float3( fDot ), lc );
					}
				}
				break;
			case MB_LIGHT_SPOT_FAST:
				fDot = MB_SkinDot( dir, normal );
				if ( fDot > 0.0f ) {
					d = float3( light.origin ) - v.xyz;
					distSquared = MB_SkinDot( d, d );
					lc = fma( color, float3( precise::divide( fDot, distSquared ) ), lc );
				}
				break;
			case MB_LIGHT_POINT:
				d = float3( light.origin ) - v.xyz;
				fDot = MB_SkinDot( d, normal );
				if ( fDot > 0.0f ) {
					distSquared = MB_SkinDot( d, d );
					lc = fma( color, float3( precise::divide( fDot, distSquared ) ), lc );
				}
				break;
			}
		}
		v.color = uchar4( MB_SkinClamp( lc.x, ambient.x ), MB_SkinClamp( lc.y, ambient.y ), MB_SkinClamp( lc.z, ambient.z ), 255 );
	} else if ( numLights == 1 ) {
		MbSkinLight light = lights[0];
		float3 color = light.color, dir = light.direction, d;
		float fDot, distSquared;
		bool lit = false;

		switch ( light.type ) {
		case MB_LIGHT_DIRECTIONAL:
			fDot = MB_SkinDot( dir, normal );
			lit = fDot > 0.0f;
			break;
		case MB_LIGHT_SPOT:
			fDot = MB_SkinDot( dir, normal );
			if ( fDot > 0.0f ) {
				float proj, maxIntensity;

				d = float3( light.origin ) - v.xyz;
				proj = MB_SkinDot( d, dir );
				distSquared = MB_SkinDot( d, d );
				maxIntensity = ( light.spotConst - precise::divide( distSquared, proj * proj ) ) * light.spotScale;
				if ( maxIntensity > 0.0f ) {
					fDot = precise::divide( fDot, distSquared );
					if ( maxIntensity < 1.0f ) {
						fDot = fDot * maxIntensity;
					}
					lit = true;
				}
			}
			break;
		case MB_LIGHT_SPOT_FAST:
			fDot = MB_SkinDot( dir, normal );
			if ( fDot > 0.0f ) {
				d = float3( light.origin ) - v.xyz;
				distSquared = MB_SkinDot( d, d );
				fDot = precise::divide( fDot, distSquared );
				lit = true;
			}
			break;
		case MB_LIGHT_POINT:
			d = float3( light.origin ) - v.xyz;
			fDot = MB_SkinDot( d, normal );
			if ( fDot > 0.0f ) {
				distSquared = MB_SkinDot( d, d );
				fDot = precise::divide( fDot, distSquared );
				lit = true;
			}
			break;
		}
		if ( lit ) {
			float3 c = color * fDot;

			v.color = uchar4( MB_SkinClamp( c.x, ambient.x ), MB_SkinClamp( c.y, ambient.y ), MB_SkinClamp( c.z, ambient.z ), 255 );
		}
	}
	return v;
}

vertex MbStageOut mb_skin_vs( uint vid [[vertex_id]],
	device const uchar *ring [[buffer(0)]],
	constant MbVSUniforms &u [[buffer(1)]],
	device const uint *skin [[buffer(2)]] )
{
	MbStageOut out;
	MbSkinVertex v = MB_Skin( vid, ring, skin, u.skin );
	float4 pos = float4( v.xyz, 1.0f );

	if ( u.stride.y == 0u ) {
		out.color = float4( v.color ) * ( 1.0f / 255.0f );
	} else if ( u.colorFloat ) {
		out.color = float4( *(device const packed_float4 *)( ring + u.offset.y + vid * u.stride.y ) );
	} else {
		out.color = float4( *(device const uchar4 *)( ring + u.offset.y + vid * u.stride.y ) ) * ( 1.0f / 255.0f );
	}

	out.tc0 = v.tc;
	out.tc1 = u.stride.w ? float2( *(device const packed_float2 *)( ring + u.offset.w + vid * u.stride.w ) ) : u.texCoord.zw;

	out.position = u.mvp * pos;
	out.position.z = ( out.position.z + out.position.w ) * 0.5f;	// GL clip z [-w, w] -> Metal [0, w]
	out.eyeZ = dot( u.eyeZ, pos );
	out.clip[0] = dot( u.clipPlane, pos );
	return out;
}

// One fixed-function texture unit. prev = the previous unit's result, primary = the vertex color.
static float4 MB_TexEnv( uint env, float4 tex, float4 prev, float4 primary, bool rgbOnly )
{
	switch ( env ) {
	case MB_ENV_REPLACE:
		return float4( tex.rgb, rgbOnly ? prev.a : tex.a );
	case MB_ENV_DECAL:
		return float4( mix( prev.rgb, tex.rgb, tex.a ), prev.a );
	case MB_ENV_ADD:
		return float4( min( prev.rgb + tex.rgb, 1.0f ), prev.a * tex.a );
	case MB_ENV_COMBINE:
		// GL1's combiner setup (GLS_MULTITEXTURE): rgb = interpolate(primary, texture, primary alpha),
		// alpha = texture * previous
		return float4( mix( tex.rgb, primary.rgb, primary.a ), tex.a * prev.a );
	default:
		return prev * tex;
	}
}

static bool MB_AlphaPasses( uint func, float a, float ref )
{
	switch ( func ) {
	case MB_AF_NEVER:		return false;
	case MB_AF_LESS:		return a < ref;
	case MB_AF_EQUAL:		return a == ref;
	case MB_AF_LEQUAL:		return a <= ref;
	case MB_AF_GREATER:		return a > ref;
	case MB_AF_NOTEQUAL:	return a != ref;
	case MB_AF_GEQUAL:		return a >= ref;
	default:				return true;
	}
}

fragment float4 mb_stage_fs( MbStageIn in [[stage_in]],
	constant MbFSUniforms &u [[buffer(0)]],
	texture2d<float> tex0 [[texture(0), function_constant(mb_tex0)]],
	texture2d<float> tex1 [[texture(1), function_constant(mb_tex1)]],
	sampler smp0 [[sampler(0), function_constant(mb_tex0)]],
	sampler smp1 [[sampler(1), function_constant(mb_tex1)]] )
{
	float4 primary = saturate( in.color );
	float4 c = primary;

	if ( mb_tex0 ) {
		c = MB_TexEnv( u.env0, tex0.sample( smp0, in.tc0 ), c, primary, ( u.flags & MB_FS_RGB0 ) != 0u );
	}
	if ( mb_tex1 ) {
		c = MB_TexEnv( u.env1, tex1.sample( smp1, in.tc1 ), c, primary, ( u.flags & MB_FS_RGB1 ) != 0u );
	}

	if ( u.flags & MB_FS_FOG ) {
		// GL linear fog on the eye-space depth, applied before the alpha test and blending
		float f = saturate( ( u.fogEnd - abs( in.eyeZ ) ) * u.fogScale );
		c.rgb = mix( u.fogColor.rgb, c.rgb, f );
	}

	if ( mb_alphaTest ) {
		if ( !MB_AlphaPasses( u.alphaFunc, c.a, u.alphaRef ) ) {
			discard_fragment();
		}
	}

	return c;
}

//
// partial clears inside a render pass (scissored, color-masked or after something was drawn)
//
struct MbClearOut
{
	float4	position [[position]];
};

// One triangle covering the viewport, at the clear depth.
vertex MbClearOut mb_clear_vs( uint vid [[vertex_id]], constant float &depth [[buffer(1)]] )
{
	float2 uv = float2( ( vid << 1 ) & 2, vid & 2 );
	MbClearOut out;
	out.position = float4( uv * 2.0f - 1.0f, depth, 1.0f );
	return out;
}

fragment float4 mb_clear_fs( constant float4 &color [[buffer(0)]] )
{
	return color;
}

//
// depth queries (flares): one point at a pixel center and a window depth, depth-tested, counted by the
// pass's visibility result buffer; no fragment function, nothing written
//
struct MbQueryOut
{
	float4	position [[position]];
	float	size [[point_size]];
};

vertex MbQueryOut mb_query_vs( constant float4 &position [[buffer(1)]] )
{
	MbQueryOut out;
	out.position = position;
	out.size = 1.0f;
	return out;
}
