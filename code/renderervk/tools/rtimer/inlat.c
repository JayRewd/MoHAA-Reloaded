/*
 * libinlat.so -- the time from input read to present of every frame
 *
 * Preloaded (LD_PRELOAD) into the game, with either renderer and any
 * engine build. A frame reads input at its start (Com_Frame: the frame
 * limiter's wait, then IN_Frame, whose SDL_PollEvent loop takes the window
 * system's events) and reaches the display at its present: GL1's
 * SDL_GL_SwapWindow, or the Vulkan renderer's vkQueuePresentKHR (its entry
 * point comes from SDL_Vulkan_GetVkGetInstanceProcAddr, then
 * vkGetDeviceProcAddr, both wrapped here). For every present it records
 * the time of the first SDL_PollEvent call since the previous present (the
 * frame's input read; 0 when there was none, as in loading screens) and of
 * the present call, with CLOCK_MONOTONIC (the clock of renderer_timed.so's
 * frame files), and writes them at exit to $INLAT_FRAMES:
 *
 *   # inlat frames 1: <n> frames (code/renderervk/tools/rtimer/inlat.c)
 *   # columns, CLOCK_MONOTONIC microseconds: frame read present
 *
 * Every frame since startup is in the file; tools/framelog.py --batch keeps
 * those renderer_timed.so measured and prints the input to present times
 * and the intervals between input reads and between presents. tools/perf.py
 * --framelog sets INLAT_FRAMES. Built by build.sh.
 */

#define _GNU_SOURCE		// RTLD_NEXT
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_FRAMES	1000000

typedef void ( *voidFunc_t )( void );
typedef voidFunc_t ( *getProcAddr_t )( void *handle, const char *name );	// vkGet{Instance,Device}ProcAddr
typedef int ( *present_t )( void *queue, const void *presentInfo );		// vkQueuePresentKHR

static long long		( *in_frames )[2];	// read, present
static int				in_numFrames;
static long long		in_read;			// first SDL_PollEvent since the last present, 0: none
static getProcAddr_t	in_getInstanceProcAddr, in_getDeviceProcAddr;
static present_t		in_queuePresent;

static long long IN_Now( void ) {
	struct timespec ts;

	clock_gettime( CLOCK_MONOTONIC, &ts );
	return ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static void IN_Present( void ) {
	long long now = IN_Now();

	if ( !in_frames ) {
		in_frames = malloc( sizeof( *in_frames ) * MAX_FRAMES );
	}
	if ( in_frames && in_numFrames < MAX_FRAMES ) {
		in_frames[in_numFrames][0] = in_read;
		in_frames[in_numFrames][1] = now;
		in_numFrames++;
	}
	in_read = 0;
}

__attribute__(( destructor )) static void IN_Write( void ) {
	const char *path = getenv( "INLAT_FRAMES" );
	FILE *f;
	int i;

	if ( !path || !*path || !in_numFrames || !( f = fopen( path, "w" ) ) ) {
		return;
	}
	fprintf( f, "# inlat frames 1: %d frames (code/renderervk/tools/rtimer/inlat.c)\n", in_numFrames );
	fprintf( f, "# columns, CLOCK_MONOTONIC microseconds: frame read present\n" );
	for ( i = 0; i < in_numFrames; i++ ) {
		fprintf( f, "%d %lld %lld\n", i, in_frames[i][0], in_frames[i][1] );
	}
	fclose( f );
}

int SDL_PollEvent( void *event ) {
	static int ( *real )( void * );

	if ( !real ) {
		real = (int ( * )( void * ))dlsym( RTLD_NEXT, "SDL_PollEvent" );
	}
	if ( !in_read ) {
		in_read = IN_Now();
	}
	return real( event );
}

void SDL_GL_SwapWindow( void *window ) {
	static void ( *real )( void * );

	if ( !real ) {
		real = (void ( * )( void * ))dlsym( RTLD_NEXT, "SDL_GL_SwapWindow" );
	}
	IN_Present();
	real( window );
}

static int IN_QueuePresent( void *queue, const void *presentInfo ) {
	IN_Present();
	return in_queuePresent( queue, presentInfo );
}

static voidFunc_t IN_GetDeviceProcAddr( void *device, const char *name ) {
	voidFunc_t f = in_getDeviceProcAddr( device, name );

	if ( f && !strcmp( name, "vkQueuePresentKHR" ) ) {
		in_queuePresent = (present_t)f;
		return (voidFunc_t)IN_QueuePresent;
	}
	return f;
}

static voidFunc_t IN_GetInstanceProcAddr( void *instance, const char *name ) {
	voidFunc_t f = in_getInstanceProcAddr( instance, name );

	if ( f && !strcmp( name, "vkGetDeviceProcAddr" ) ) {
		in_getDeviceProcAddr = (getProcAddr_t)f;
		return (voidFunc_t)IN_GetDeviceProcAddr;
	}
	if ( f && !strcmp( name, "vkQueuePresentKHR" ) ) {
		in_queuePresent = (present_t)f;
		return (voidFunc_t)IN_QueuePresent;
	}
	return f;
}

void *SDL_Vulkan_GetVkGetInstanceProcAddr( void ) {
	static void *( *real )( void );

	if ( !real ) {
		real = (void *( * )( void ))dlsym( RTLD_NEXT, "SDL_Vulkan_GetVkGetInstanceProcAddr" );
	}
	in_getInstanceProcAddr = (getProcAddr_t)real();
	return in_getInstanceProcAddr ? (void *)IN_GetInstanceProcAddr : NULL;
}
