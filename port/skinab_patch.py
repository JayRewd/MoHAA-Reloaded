#!/usr/bin/env python3
# The GPU-skinning A/B harness (port/skinab.sh applies it to a scratch copy of the tree, never to the tree itself).
# Every GPU-skinned batch is drawn twice, the GPU way then the CPU way; both vertex shaders capture their outputs per
# vertex into the ring, and mbEndFrame waits for the frame and compares them bit for bit: clip position, color,
# texcoords, eye z, clip distance, plus the GPU's skinned positions, lit colors and unquantized light sums against
# the CPU's (RB_SkelMeshVertexes, RB_Light_Real). The patch is anchored to exact source text: when the code it
# touches changes, an assert names the stale anchor. Usage: python3 port/skinab_patch.py <tree>
import sys
root = sys.argv[1]
def patch(path, pairs):
    p = root + '/' + path
    s = open(p).read()
    for old, new in pairs:
        assert old in s, (path, old[:80])
        s = s.replace(old, new, 1)
    open(p, 'w').write(s)

# --- shaders
cap_struct = """
struct MbCapture
{
	float4	position;
	float4	color;
	float4	tc;
	float	eyeZ;
	float	clip;
	float2	pad;
	float4	skinXyz;
	uint	skinColor;
	uint	pad2[3];
};
static_assert( sizeof( MbCapture ) == 96, "MbCapture" );
"""
patch('code/renderer_metal/shaders/tr_mtl_stage.metal', [
    ("""	uint		skin;			// mb_skin_vs: ring byte offset of the draw's MbSkinUniforms
	uint		pad;
};""", """	uint		skin;			// mb_skin_vs: ring byte offset of the draw's MbSkinUniforms
	uint		capture;
};
""" + cap_struct),
    ("""vertex MbStageOut mb_stage_vs( uint vid [[vertex_id]],
	device const uchar *ring [[buffer(0)]],
	constant MbVSUniforms &u [[buffer(1)]] )""", """vertex MbStageOut mb_stage_vs( uint vid [[vertex_id]],
	device const uchar *ring [[buffer(0)]],
	constant MbVSUniforms &u [[buffer(1)]],
	device uchar *cap [[buffer(3)]] )"""),
    ("""	out.eyeZ = dot( u.eyeZ, pos );
	out.clip[0] = dot( u.clipPlane, pos );
	return out;
}

//
// GPU skinning""", """	out.eyeZ = dot( u.eyeZ, pos );
	out.clip[0] = dot( u.clipPlane, pos );
	if ( u.capture ) {
		device MbCapture *c = (device MbCapture *)( cap + u.capture ) + vid;
		c->position = out.position; c->color = out.color; c->tc = float4( out.tc0, out.tc1 ); c->eyeZ = out.eyeZ; c->clip = out.clip[0];
	}
	return out;
}

//
// GPU skinning"""),
    ("""	constant MbVSUniforms &u [[buffer(1)]],
	device const uint *skin [[buffer(2)]] )""", """	constant MbVSUniforms &u [[buffer(1)]],
	device const uint *skin [[buffer(2)]],
	device uchar *cap [[buffer(3)]] )"""),
    ("""	out.eyeZ = dot( u.eyeZ, pos );
	out.clip[0] = dot( u.clipPlane, pos );
	return out;
}

// One fixed-function""", """	out.eyeZ = dot( u.eyeZ, pos );
	out.clip[0] = dot( u.clipPlane, pos );
	if ( u.capture ) {
		device MbCapture *c = (device MbCapture *)( cap + u.capture ) + vid;
		c->position = out.position; c->color = out.color; c->tc = float4( out.tc0, out.tc1 ); c->eyeZ = out.eyeZ; c->clip = out.clip[0];
		c->skinXyz = float4( v.xyz, 0.0f ); c->skinColor = as_type<uint>( v.color );
	}
	return out;
}

// One fixed-function"""),
])

# --- backend
patch('code/renderer_metal/tr_mtl.m', [
    ("#define MB_RING_SIZE		( 8 * 1024 * 1024 )", "#define MB_RING_SIZE		( 96 * 1024 * 1024 )"),
    ("""	uint32_t	skin;
	uint32_t	pad;
} mbVSUniforms_t;""", """	uint32_t	skin;
	uint32_t	capture;
} mbVSUniforms_t;

// A/B
typedef struct { float position[4], color[4], tc[4], eyeZ, clip, pad[2], skinXyz[4]; uint32_t skinColor, pad2[3]; } mbCapture_t;
typedef struct { void *ring; uint32_t gpuOff, cpuOff; int n, lights; float *xyz; byte *colors; } mbABPair_t;
#define MB_AB_MAX_PAIRS 512
static mbABPair_t mb_abPairs[MB_AB_MAX_PAIRS];
static int mb_abNumPairs, mb_abWant = -1;
static uint32_t mb_abOff[2];
static unsigned mb_abGen[2];
static uint64_t mb_abFrames, mb_abPairsDone, mb_abVerts, mb_abCaptured, mb_abMissing, mb_abPos, mb_abCol, mb_abTc, mb_abEye, mb_abClip,
	mb_abXyzBits, mb_abXyzVal, mb_abSkinCol, mb_abDropped, mb_abLitVerts, mb_abPrinted;
void mbABCapture( int which ) { mb_abWant = which; }
"""),
    ("""	if ( mb_skinDraw && mb_encSkin != mb_skinBuffer ) {""", """	[mb_enc setVertexBuffer:mb_ring offset:0 atIndex:3];
	if ( mb_skinDraw && mb_encSkin != mb_skinBuffer ) {"""),
    ("""	memcpy( indexDst, indices, (size_t)count * 4 );

	MB_BindVertices( mb_ring );""", """	memcpy( indexDst, indices, (size_t)count * 4 );

	mb_vsu.capture = 0;
	if ( mb_abWant >= 0 ) {
		uint32_t off;
		byte *c = MB_RingAlloc( (NSUInteger)numVertexes * sizeof( mbCapture_t ), &off );

		memset( c, 0xff, (size_t)numVertexes * sizeof( mbCapture_t ) );
		mb_vsu.capture = off;
		mb_abOff[mb_abWant] = off;
		mb_abGen[mb_abWant] = mb_ringGeneration;
		mb_abWant = -1;
	}

	MB_BindVertices( mb_ring );"""),
    ("""		MB_CommitUploads();
		[mb_cmd commit];
		if ( latency ) {
			MB_LatencyCommitted( latency, drawable != nil );
		}""", """		MB_CommitUploads();
		[mb_cmd commit];
		if ( latency ) {
			MB_LatencyCommitted( latency, drawable != nil );
		}
		if ( mb_abNumPairs ) {
			[mb_cmd waitUntilCompleted];
			MB_ABCompare();
		}
		mb_abFrames++;
		if ( mb_abFrames % 500 == 0 ) {
			MB_ABReport();
		}"""),
    ("""	MB_SavePipelineCache();
	MB_StopDrawableAcquisition();""", """	MB_ABReport();
	MB_SavePipelineCache();
	MB_StopDrawableAcquisition();"""),
    ("""static void MB_SkinUniforms( void );
""", """static void MB_SkinUniforms( void );
static void MB_ABCompare( void );
static void MB_ABReport( void );
"""),
])

# compare functions appended
p = root + '/code/renderer_metal/tr_mtl.m'
s = open(p).read()
s += r'''

/*
** A/B (temporary): GPU vs CPU skinning
*/
void mbABPair( const float *xyz, const byte *colors, int n, int lights )
{
	mbABPair_t *p;

	if ( mb_abGen[0] != mb_ringGeneration || mb_abGen[1] != mb_ringGeneration || mb_abNumPairs == MB_AB_MAX_PAIRS ) {
		mb_abDropped++;
		return;
	}
	p = &mb_abPairs[mb_abNumPairs++];
	p->ring = (__bridge_retained void *)mb_ring;
	p->gpuOff = mb_abOff[0];
	p->cpuOff = mb_abOff[1];
	p->n = n;
	p->lights = lights;
	p->xyz = malloc( (size_t)n * 16 );
	memcpy( p->xyz, xyz, (size_t)n * 16 );
	p->colors = malloc( (size_t)n * 4 );
	memcpy( p->colors, colors, (size_t)n * 4 );
}

static void MB_ABReport( void )
{
	ri.Printf( PRINT_ALL, "SKINAB %llu frames, %llu batches, %llu verts, %llu captured (%llu lit), %llu dropped | differ: position %llu color %llu tc %llu eyeZ %llu clip %llu missing %llu | skinned xyz bits %llu (values %llu), lit color %llu | ring growths %d\n",
		mb_abFrames, mb_abPairsDone, mb_abVerts, mb_abCaptured, mb_abLitVerts, mb_abDropped, mb_abPos, mb_abCol, mb_abTc, mb_abEye, mb_abClip, mb_abMissing,
		mb_abXyzBits, mb_abXyzVal, mb_abSkinCol, mb_statRingGrowths );
}

static void MB_ABCompare( void )
{
	int i, v;

	for ( i = 0; i < mb_abNumPairs; i++ ) {
		mbABPair_t *p = &mb_abPairs[i];
		id<MTLBuffer> ring = (__bridge_transfer id<MTLBuffer>)p->ring;
		const mbCapture_t *g = (const mbCapture_t *)( (const byte *)ring.contents + p->gpuOff );
		const mbCapture_t *c = (const mbCapture_t *)( (const byte *)ring.contents + p->cpuOff );

		mb_abPairsDone++;
		for ( v = 0; v < p->n; v++ ) {
			uint32_t gb, cb;
			qboolean bad = qfalse;
			int k;

			mb_abVerts++;
			memcpy( &gb, g[v].position, 4 );
			memcpy( &cb, c[v].position, 4 );
			if ( ( gb == 0xffffffffu ) != ( cb == 0xffffffffu ) ) {
				mb_abMissing++;
				continue;
			}
			if ( gb == 0xffffffffu ) {
				continue;
			}
			mb_abCaptured++;
			if ( memcmp( g[v].position, c[v].position, 16 ) ) { mb_abPos++; bad = qtrue; }
			if ( memcmp( g[v].color, c[v].color, 16 ) ) { mb_abCol++; bad = qtrue; }
			if ( memcmp( g[v].tc, c[v].tc, 16 ) ) { mb_abTc++; bad = qtrue; }
			if ( memcmp( &g[v].eyeZ, &c[v].eyeZ, 4 ) ) { mb_abEye++; bad = qtrue; }
			if ( memcmp( &g[v].clip, &c[v].clip, 4 ) ) { mb_abClip++; bad = qtrue; }
			if ( memcmp( g[v].skinXyz, &p->xyz[v * 4], 12 ) ) {
				mb_abXyzBits++;
				for ( k = 0; k < 3; k++ ) {
					if ( g[v].skinXyz[k] != p->xyz[v * 4 + k] ) {
						mb_abXyzVal++;
						bad = qtrue;
						break;
					}
				}
			}
			if ( p->lights ) {
				mb_abLitVerts++;
				if ( memcmp( &g[v].skinColor, &p->colors[v * 4], 4 ) ) { mb_abSkinCol++; bad = qtrue; }
			}
			if ( bad && mb_abPrinted < 20 ) {
				const byte *gc = (const byte *)&g[v].skinColor;
				mb_abPrinted++;
				ri.Printf( PRINT_ALL, "SKINAB diff v%d/%d: pos %.9g %.9g %.9g %.9g | %.9g %.9g %.9g %.9g; col %g %g %g %g | %g %g %g %g; eyeZ %.9g | %.9g; xyz %.9g %.9g %.9g | %.9g %.9g %.9g; lit %d %d %d %d | %d %d %d %d\n",
					v, p->n, g[v].position[0], g[v].position[1], g[v].position[2], g[v].position[3], c[v].position[0], c[v].position[1], c[v].position[2], c[v].position[3],
					g[v].color[0], g[v].color[1], g[v].color[2], g[v].color[3], c[v].color[0], c[v].color[1], c[v].color[2], c[v].color[3],
					g[v].eyeZ, c[v].eyeZ, g[v].skinXyz[0], g[v].skinXyz[1], g[v].skinXyz[2], p->xyz[v * 4], p->xyz[v * 4 + 1], p->xyz[v * 4 + 2],
					gc[0], gc[1], gc[2], gc[3], p->colors[v * 4], p->colors[v * 4 + 1], p->colors[v * 4 + 2], p->colors[v * 4 + 3] );
			}
		}
		free( p->xyz );
		free( p->colors );
	}
	mb_abNumPairs = 0;
}
'''
open(p, 'w').write(s)

patch('code/renderer_metal/tr_mtl.h', [
    ("""void		mbSkinEnd( void );""", """void		mbSkinEnd( void );
void		mbABCapture( int which );	// A/B: capture the next draw (0 GPU, 1 CPU)
void		mbABPair( const float *xyz, const byte *colors, int n, int lights );"""),
])

# --- front end: draw the CPU version after the GPU one
patch('code/renderer_metal/tr_shade.c', [
    ("""	R_DrawElements( input->numIndexes, input->indexes );
	if ( skin ) {
		mbSkinEnd();
	}""", """	if ( skin ) {
		mbABCapture( 0 );
	}
	R_DrawElements( input->numIndexes, input->indexes );
	if ( skin ) {
		mbSkinEnd();
		// A/B: the same batch the CPU way
		RB_SkinMaterialize();
		if ( skin == 2 ) {
			backEnd.currentSphere->TessFunction((unsigned char*)tess.svars.colors);
		}
		mbABCapture( 1 );
		R_DrawElements( input->numIndexes, input->indexes );
		mbABPair( (const float *)tess.xyz, (const byte *)tess.svars.colors, tess.numVertexes, skin == 2 );
	}"""),
])

# --- stronger lighting check: the unquantized light sums (multi-light case) against RB_Light_Real's accumulator
patch('code/renderer_metal/tr_sphere_shade.cpp', [
    ("""void RB_Light_Real(unsigned char *colors)
{""", """vec3_t rb_abLightColors[SHADER_MAX_VERTEXES];
int    rb_abLightMulti;

void RB_Light_Real(unsigned char *colors)
{
    rb_abLightMulti = 0;"""),
    ("""        static vec3_t   lightColors[SHADER_MAX_VERTEXES];""", """        vec3_t         *lightColors = rb_abLightColors;"""),
    ("""        memset(lightColors, 0, sizeof(lightColors[0]) * numVertexes);""", """        memset(lightColors, 0, sizeof(lightColors[0]) * numVertexes);
        rb_abLightMulti = 1;"""),
])
patch('code/renderer_metal/shaders/tr_mtl_stage.metal', [
    ("""struct MbSkinVertex
{
	float3		xyz;
	float2		tc;
	uchar4		color;
};""", """struct MbSkinVertex
{
	float3		xyz;
	float2		tc;
	uchar4		color;
	float3		lc;
};"""),
    ("""	v.color = ambient;
	if ( numLights > 1 ) {""", """	v.color = ambient;
	v.lc = float3( 0.0f );
	if ( numLights > 1 ) {"""),
    ("""		v.color = uchar4( MB_SkinClamp( lc.x, ambient.x ), MB_SkinClamp( lc.y, ambient.y ), MB_SkinClamp( lc.z, ambient.z ), 255 );
	} else if ( numLights == 1 ) {""", """		v.color = uchar4( MB_SkinClamp( lc.x, ambient.x ), MB_SkinClamp( lc.y, ambient.y ), MB_SkinClamp( lc.z, ambient.z ), 255 );
		v.lc = lc;
	} else if ( numLights == 1 ) {"""),
    ("""		c->skinXyz = float4( v.xyz, 0.0f ); c->skinColor = as_type<uint>( v.color );""", """		c->skinXyz = float4( v.xyz, 0.0f ); c->skinColor = as_type<uint>( v.color );
		c->pad2[0] = as_type<uint>( v.lc.x ); c->pad2[1] = as_type<uint>( v.lc.y ); c->pad2[2] = as_type<uint>( v.lc.z );"""),
])
patch('code/renderer_metal/tr_mtl.h', [
    ("""void		mbABPair( const float *xyz, const byte *colors, int n, int lights );""", """void		mbABPair( const float *xyz, const byte *colors, int n, int lights, const float *lightColors );"""),
])
patch('code/renderer_metal/tr_mtl.m', [
    ("typedef struct { void *ring; uint32_t gpuOff, cpuOff; int n, lights; float *xyz; byte *colors; } mbABPair_t;",
     "typedef struct { void *ring; uint32_t gpuOff, cpuOff; int n, lights; float *xyz; byte *colors; float *lc; } mbABPair_t;"),
    ("	mb_abXyzBits, mb_abXyzVal, mb_abSkinCol, mb_abDropped, mb_abLitVerts, mb_abPrinted;",
     "	mb_abXyzBits, mb_abXyzVal, mb_abSkinCol, mb_abDropped, mb_abLitVerts, mb_abPrinted, mb_abLcVerts, mb_abLc;"),
    ("void mbABPair( const float *xyz, const byte *colors, int n, int lights )", "void mbABPair( const float *xyz, const byte *colors, int n, int lights, const float *lightColors )"),
    ("""	p->colors = malloc( (size_t)n * 4 );
	memcpy( p->colors, colors, (size_t)n * 4 );""", """	p->colors = malloc( (size_t)n * 4 );
	memcpy( p->colors, colors, (size_t)n * 4 );
	p->lc = NULL;
	if ( lightColors ) {
		p->lc = malloc( (size_t)n * 12 );
		memcpy( p->lc, lightColors, (size_t)n * 12 );
	}"""),
    ("""			if ( p->lights ) {
				mb_abLitVerts++;""", """			if ( p->lc ) {
				mb_abLcVerts++;
				if ( memcmp( g[v].pad2, &p->lc[v * 3], 12 ) ) { mb_abLc++; bad = qtrue; }
			}
			if ( p->lights ) {
				mb_abLitVerts++;"""),
    ("""		free( p->xyz );
		free( p->colors );""", """		free( p->xyz );
		free( p->colors );
		free( p->lc );"""),
    ("""lit color %llu | ring growths %d\\n",""", """lit color %llu | light sums %llu of %llu | ring growths %d\\n","""),
    ("""		mb_abXyzBits, mb_abXyzVal, mb_abSkinCol, mb_statRingGrowths );""", """		mb_abXyzBits, mb_abXyzVal, mb_abSkinCol, mb_abLc, mb_abLcVerts, mb_statRingGrowths );"""),
])
patch('code/renderer_metal/tr_shade.c', [
    ("""		mbABPair( (const float *)tess.xyz, (const byte *)tess.svars.colors, tess.numVertexes, skin == 2 );""",
     """		{
			extern vec3_t rb_abLightColors[];
			extern int rb_abLightMulti;
			mbABPair( (const float *)tess.xyz, (const byte *)tess.svars.colors, tess.numVertexes, skin == 2,
				skin == 2 && rb_abLightMulti ? (const float *)rb_abLightColors : NULL );
		}"""),
])
print("skinab: A/B patch applied to", root)
