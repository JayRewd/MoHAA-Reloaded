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
	float4x4	mvp;			// clip z already remapped to Metal's [0, w]
	float4		clipPlane;		// object space; (0,0,0,1) when the clip plane is off
	float4		eyeZ;			// modelview row 2: eye-space z = dot(eyeZ, position)
	float4		color;			// constant color when the color array is off
	float4		texCoord;		// constant texcoords when the arrays are off: unit 0 xy, unit 1 zw
	uint4		offset;			// ring byte offsets: position, color, texcoord 0, texcoord 1
	uint4		stride;			// byte strides; 0 = not streamed (use the constant)
	uint		posSize;		// 2, 3 or 4 floats
	uint		colorFloat;		// the color stream is float4 (immediate mode), else unorm uchar4
	uint2		pad;
};

// texenv modes (MB_TexEnvIndex in tr_mtl.m)
#define MB_ENV_MODULATE		0u
#define MB_ENV_REPLACE		1u
#define MB_ENV_DECAL		2u
#define MB_ENV_ADD			3u
#define MB_ENV_COMBINE		4u

// alpha functions: GL compare func - GL_NEVER
#define MB_AF_NEVER			0u
#define MB_AF_LESS			1u
#define MB_AF_EQUAL			2u
#define MB_AF_LEQUAL		3u
#define MB_AF_GREATER		4u
#define MB_AF_NOTEQUAL		5u
#define MB_AF_GEQUAL		6u

#define MB_FS_FOG			1u
#define MB_FS_RGB0			2u		// unit 0's texture has no alpha channel (GL_RGB): replace keeps the incoming alpha
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
		// GL1's GL_COMBINE setup (GLS_MULTITEXTURE): rgb = interpolate(primary, texture, primary alpha),
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
