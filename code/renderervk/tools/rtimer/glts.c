/*
 * libglts.so -- GPU time of the GL1 renderer's frames, from GL timestamps
 *
 *   GLTS_OUT=file LD_PRELOAD=libglts.so ./openmohaa +set cl_renderer opengl1 ...
 *
 * The Vulkan renderer reports its GPU time (vkinfo); GL1 has nothing like
 * it, and its frame time is only its GPU time when the GPU limits. This
 * shim wraps glClear (through SDL_GL_GetProcAddress, which GL1 loads its
 * functions with) and SDL_GL_SwapWindow, and writes GL_TIMESTAMP queries
 * (bottom of the pipe, when everything before them is done) right after
 * each swap, before and after the first depth clear of a frame (the 3D
 * view's) and right before the swap. Results are read 4 frames later. At
 * exit it appends one line to GLTS_OUT with the average milliseconds per
 * frame over the frames [-1006, -6) (the 1000 frames tools/perf.py
 * measures, which end 6 frames before the game quits):
 *
 *   glts frames=<n> pre=<swap to clear> clear=<the clear> draw=<clear to
 *     swap> swap=<before the swap to after it> period=<swap to swap> ms
 *
 * "period" is the GPU's time per frame (the frame time when the GPU
 * limits). Run perf.py with it in LD_PRELOAD and GLTS_OUT set: the Vulkan
 * renderer doesn't call these functions, and the lines are in the order of
 * the GL1 runs. Build with tools/rtimer/build.sh.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef unsigned int GLbitfield;
typedef int GLsizei;
typedef uint64_t GLuint64;

#define GL_TIMESTAMP			0x8E28
#define GL_QUERY_RESULT			0x8866
#define GL_DEPTH_BUFFER_BIT		0x00000100

typedef void ( *pfnClear )( GLbitfield );
typedef void ( *pfnGenQueries )( GLsizei, GLuint * );
typedef void ( *pfnQueryCounter )( GLuint, GLenum );
typedef void ( *pfnGetQueryObjectui64v )( GLuint, GLenum, GLuint64 * );
typedef void ( *pfnSwap )( void * );

static pfnClear real_glClear;
static pfnGenQueries pGenQueries;
static pfnQueryCounter pQueryCounter;
static pfnGetQueryObjectui64v pGetQueryObjectui64v;

// queries of the frames in flight: 0 after the previous swap, 1 before the
// first depth clear, 2 after it, 3 before the swap
#define RING	8
#define NQ		4
static GLuint queries[RING][NQ];
static int written[RING];
static long frameNum;
static int inited;

#define MAX_FRAMES	200000
static double seg[MAX_FRAMES][5];	// pre, clear, draw, swap, period in microseconds
static long numFrames;
static uint64_t lastEnd;

static void init( void ) {
	if ( inited || !pGenQueries ) {
		return;
	}
	pGenQueries( RING * NQ, &queries[0][0] );
	inited = 1;
}

static void stamp( int q ) {
	int slot = frameNum % RING;

	if ( !inited || ( written[slot] & ( 1 << q ) ) ) {
		return;
	}
	pQueryCounter( queries[slot][q], GL_TIMESTAMP );
	written[slot] |= 1 << q;
}

// frames without a 3D view (no depth clear) are skipped
static void collect( long f ) {
	int slot = f % RING;
	GLuint64 t[NQ];
	int i;

	if ( f < 0 || written[slot] != ( 1 << NQ ) - 1 ) {
		written[slot] = 0;
		lastEnd = 0;
		return;
	}
	for ( i = 0; i < NQ; i++ ) {
		pGetQueryObjectui64v( queries[slot][i], GL_QUERY_RESULT, &t[i] );
	}
	written[slot] = 0;
	if ( lastEnd && numFrames < MAX_FRAMES ) {
		seg[numFrames][0] = ( t[1] - t[0] ) / 1e3;
		seg[numFrames][1] = ( t[2] - t[1] ) / 1e3;
		seg[numFrames][2] = ( t[3] - t[2] ) / 1e3;
		seg[numFrames][3] = ( t[0] - lastEnd ) / 1e3;
		seg[numFrames][4] = ( t[3] - lastEnd ) / 1e3;
		numFrames++;
	}
	lastEnd = t[3];
}

static void w_glClear( GLbitfield mask ) {
	int first = ( mask & GL_DEPTH_BUFFER_BIT ) && !( written[frameNum % RING] & 2 );

	if ( first ) {
		stamp( 1 );
	}
	real_glClear( mask );
	if ( first ) {
		stamp( 2 );
	}
}

void SDL_GL_SwapWindow( void *window ) {
	static pfnSwap real;

	if ( !real ) {
		real = (pfnSwap)dlsym( RTLD_NEXT, "SDL_GL_SwapWindow" );
	}
	init();
	stamp( 3 );
	real( window );
	collect( frameNum - RING / 2 );
	frameNum++;
	written[frameNum % RING] = 0;
	stamp( 0 );
}

void *SDL_GL_GetProcAddress( const char *name ) {
	static void *( *real )( const char * );
	void *p;

	if ( !real ) {
		real = dlsym( RTLD_NEXT, "SDL_GL_GetProcAddress" );
	}
	p = real( name );
	if ( !pGenQueries ) {
		pGenQueries = (pfnGenQueries)real( "glGenQueries" );
		pQueryCounter = (pfnQueryCounter)real( "glQueryCounter" );
		pGetQueryObjectui64v = (pfnGetQueryObjectui64v)real( "glGetQueryObjectui64v" );
	}
	if ( !strcmp( name, "glClear" ) ) {
		real_glClear = (pfnClear)p;
		return p ? (void *)w_glClear : NULL;
	}
	return p;
}

__attribute__((destructor)) static void report( void ) {
	const char *out = getenv( "GLTS_OUT" );
	double sum[5] = { 0 };
	long a, b, i, n;
	int k;
	FILE *f;

	if ( !out || numFrames < 20 ) {
		return;
	}
	b = numFrames - 6;
	a = b - 1000 > 0 ? b - 1000 : 0;
	n = b - a;
	for ( i = a; i < b; i++ ) {
		for ( k = 0; k < 5; k++ ) {
			sum[k] += seg[i][k];
		}
	}
	f = fopen( out, "a" );
	if ( !f ) {
		return;
	}
	fprintf( f, "glts frames=%ld pre=%.4f clear=%.4f draw=%.4f swap=%.4f period=%.4f ms\n", n,
		sum[0] / n / 1e3, sum[1] / n / 1e3, sum[2] / n / 1e3, sum[3] / n / 1e3, sum[4] / n / 1e3 );
	fclose( f );
}
