/*
===========================================================================
Copyright (C) 2026 the OpenMoHAA Metal port contributors

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
// tr_mtl.m -- Metal backend behind tr_mtl.h (Objective-C, ARC).
//
// Frame model: the scene is rendered into an offscreen color target. mbEndFrame runs one fullscreen
// triangle that copies it into the drawable (gamma ramp applied in the shader), presents and commits. Only
// then does it wait for a free frame slot, so that wait falls before the engine's frame limiter and the next
// frame's input sampling: nothing waits for the GPU between input sampling and present.
//
// Drawables: in a composited window the WindowServer holds the layer's drawables until its next refresh, and
// nextDrawable blocked the frame for 1.3-1.9 ms (2-3 drawables, timedemo). A blocking present would cap the
// engine loop below com_maxfps and change usercmd timing, which GL1 (non-blocking swap) doesn't. So with vsync
// off a helper thread waits in nextDrawable and keeps one drawable ready: the frame takes it without waiting,
// or skips its present when none is ready (a frame the compositor could not have shown anyway). With vsync on
// (r_swapInterval 1) the next drawable is acquired at the end of the frame, blocking like GL1's swap.
//
// Phase 1: device, queue, scene target, clear, final pass and present are real. The state, texture, array,
// draw and readback entry points only keep the bookkeeping the front end relies on (texture names) and
// draw nothing yet (Phase 2).

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <mach/mach_time.h>
#include <os/lock.h>
#include <pthread.h>

#include "../renderercommon/tr_common.h"
#include "tr_mtl.h"
#include "tr_mtl_metallib.h"

qboolean	mbMultitexture;

static id<MTLDevice>				mb_device;
static id<MTLCommandQueue>			mb_queue;
static id<MTLRenderPipelineState>	mb_finalPipeline;
static CAMetalLayer					*mb_layer;
static id<MTLTexture>				mb_sceneColor;		// offscreen RGBA8, what the front end renders into
static id<MTLCommandBuffer>			mb_lastCommitted;	// for mbFinish / shutdown
static dispatch_semaphore_t			mb_frameSemaphore;	// frames in flight

// drawable acquisition (see the top of the file)
static qboolean						mb_vsync;
static id<CAMetalDrawable>			mb_nextDrawable;	// vsync: acquired at the end of the previous frame
static pthread_t					mb_acquireThread;	// no vsync: keeps mb_readyDrawable filled
static qboolean						mb_acquireThreadRunning;
static _Atomic qboolean				mb_acquireQuit;
static dispatch_semaphore_t			mb_acquireRequest;	// signaled when mb_readyDrawable was taken
static os_unfair_lock				mb_readyLock = OS_UNFAIR_LOCK_INIT;
static id<CAMetalDrawable>			mb_readyDrawable;	// guarded by mb_readyLock

static cvar_t	*r_metalFramesInFlight;
static cvar_t	*r_metalPresentStats;

// r_metalPresentStats: drawables actually shown on screen (presentedTime > 0) vs. presented
static _Atomic int	mb_statPresented, mb_statShown;
static int			mb_statFrames, mb_statSkipped;			// frames, frames without a ready drawable
static uint64_t		mb_statDrawableWait, mb_statSlotWait;	// main thread mach ticks in nextDrawable / the frame slot wait

static int		mb_framesInFlight;
static int		mb_width, mb_height;
static char		mb_rendererString[MAX_STRING_CHARS];

// clear state: the scene target is cleared at the start of the next scene pass
static float	mb_clearColor[4];
static qboolean	mb_clearPending;

static uint8_t	mb_gammaRamp[256][4];					// .rgb per input value, see tr_mtl_final.metal

static unsigned int	mb_nextTextureName = 1;			// 0 is "no texture", as in GL

/*
** MB_CreateSceneTargets
*/
static qboolean MB_CreateSceneTargets( int width, int height )
{
	MTLTextureDescriptor *desc;

	desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
		width:width height:height mipmapped:NO];
	desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	desc.storageMode = MTLStorageModePrivate;
	mb_sceneColor = [mb_device newTextureWithDescriptor:desc];
	if ( !mb_sceneColor ) {
		return qfalse;
	}
	mb_sceneColor.label = @"scene color";

	mb_width = width;
	mb_height = height;
	return qtrue;
}

/*
** MB_CreatePipelines
*/
static qboolean MB_CreatePipelines( void )
{
	NSError *error = nil;
	dispatch_data_t data;
	id<MTLLibrary> library;
	MTLRenderPipelineDescriptor *desc;

	data = dispatch_data_create( tr_mtl_metallib, tr_mtl_metallib_len, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT );
	library = [mb_device newLibraryWithData:data error:&error];
	if ( !library ) {
		ri.Printf( PRINT_ALL, "mbInit: embedded metallib failed to load: %s\n", error.localizedDescription.UTF8String );
		return qfalse;
	}

	desc = [MTLRenderPipelineDescriptor new];
	desc.label = @"final";
	desc.vertexFunction = [library newFunctionWithName:@"mb_final_vs"];
	desc.fragmentFunction = [library newFunctionWithName:@"mb_final_fs"];
	desc.colorAttachments[0].pixelFormat = mb_layer.pixelFormat;
	mb_finalPipeline = [mb_device newRenderPipelineStateWithDescriptor:desc error:&error];
	if ( !mb_finalPipeline ) {
		ri.Printf( PRINT_ALL, "mbInit: final pipeline failed: %s\n", error.localizedDescription.UTF8String );
		return qfalse;
	}

	return qtrue;
}

/*
** MB_AcquireThread
**
** Waits in nextDrawable so the frame never has to (vsync off).
*/
static void *MB_AcquireThread( void *arg )
{
	CAMetalLayer *layer = (__bridge_transfer CAMetalLayer *)arg;

	pthread_setname_np( "openmohaa drawables" );

	while ( 1 ) {
		dispatch_semaphore_wait( mb_acquireRequest, DISPATCH_TIME_FOREVER );
		if ( mb_acquireQuit ) {
			break;
		}

		@autoreleasepool {
			id<CAMetalDrawable> drawable = [layer nextDrawable];

			if ( !drawable ) {
				// timed out (hidden window): try again
				dispatch_semaphore_signal( mb_acquireRequest );
				continue;
			}

			os_unfair_lock_lock( &mb_readyLock );
			mb_readyDrawable = drawable;
			os_unfair_lock_unlock( &mb_readyLock );
		}
	}

	return NULL;
}

/*
** MB_StartDrawableAcquisition
*/
static qboolean MB_StartDrawableAcquisition( void )
{
	pthread_attr_t attr;
	int err;

	mb_vsync = mb_layer.displaySyncEnabled ? qtrue : qfalse;
	if ( mb_vsync ) {
		return qtrue;
	}

	mb_acquireQuit = qfalse;
	mb_acquireRequest = dispatch_semaphore_create( 0 );

	pthread_attr_init( &attr );
	pthread_attr_set_qos_class_np( &attr, QOS_CLASS_USER_INTERACTIVE, 0 );
	err = pthread_create( &mb_acquireThread, &attr, MB_AcquireThread, (__bridge_retained void *)mb_layer );
	pthread_attr_destroy( &attr );
	if ( err ) {
		ri.Printf( PRINT_ALL, "mbInit: can't start the drawable thread (%d)\n", err );
		mb_acquireRequest = nil;
		return qfalse;
	}
	mb_acquireThreadRunning = qtrue;

	dispatch_semaphore_signal( mb_acquireRequest );	// fetch the first drawable
	return qtrue;
}

/*
** MB_StopDrawableAcquisition
*/
static void MB_StopDrawableAcquisition( void )
{
	if ( mb_acquireThreadRunning ) {
		mb_acquireQuit = qtrue;
		dispatch_semaphore_signal( mb_acquireRequest );
		pthread_join( mb_acquireThread, NULL );
		mb_acquireThreadRunning = qfalse;
	}
	mb_acquireRequest = nil;
	mb_readyDrawable = nil;
	mb_nextDrawable = nil;
}

/*
** MB_TakeDrawable
**
** The drawable for this frame's final pass, or nil (vsync off and none ready: skip the present).
*/
static id<CAMetalDrawable> MB_TakeDrawable( void )
{
	id<CAMetalDrawable> drawable;
	uint64_t t0;

	if ( mb_vsync ) {
		drawable = mb_nextDrawable;
		mb_nextDrawable = nil;
		if ( !drawable ) {
			t0 = mach_absolute_time();
			drawable = [mb_layer nextDrawable];
			mb_statDrawableWait += mach_absolute_time() - t0;
		}
		return drawable;
	}

	os_unfair_lock_lock( &mb_readyLock );
	drawable = mb_readyDrawable;
	mb_readyDrawable = nil;
	os_unfair_lock_unlock( &mb_readyLock );

	if ( drawable ) {
		dispatch_semaphore_signal( mb_acquireRequest );	// fetch the next one
	} else {
		mb_statSkipped++;
	}
	return drawable;
}

/*
** mbInit
**
** Called by GLimp_Init once the window and its CAMetalLayer exist and glConfig.vidWidth/vidHeight hold
** the drawable size in pixels. The device and queue live for the whole process (vid_restart keeps them).
*/
qboolean mbInit( void *metalLayer )
{
	int i;

	r_metalFramesInFlight = ri.Cvar_Get( "r_metalFramesInFlight", "2", CVAR_ARCHIVE | CVAR_LATCH );
	r_metalPresentStats = ri.Cvar_Get( "r_metalPresentStats", "0", 0 );

	if ( !mb_device ) {
		mb_device = MTLCreateSystemDefaultDevice();
		if ( !mb_device ) {
			ri.Printf( PRINT_ALL, "mbInit: no Metal device\n" );
			return qfalse;
		}
		mb_queue = [mb_device newCommandQueue];
		mb_queue.label = @"openmohaa";
		Com_sprintf( mb_rendererString, sizeof( mb_rendererString ), "%s (Metal)", mb_device.name.UTF8String );
	}

	mb_layer = (__bridge CAMetalLayer *)metalLayer;
	mb_layer.device = mb_device;
	mb_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;	// not sRGB: Q3 math is gamma-space
	mb_layer.framebufferOnly = YES;

	if ( !MB_CreatePipelines() || !MB_CreateSceneTargets( glConfig.vidWidth, glConfig.vidHeight ) ) {
		mbShutdown();
		return qfalse;
	}

	for ( i = 0; i < 256; i++ ) {
		mb_gammaRamp[i][0] = mb_gammaRamp[i][1] = mb_gammaRamp[i][2] = i;
		mb_gammaRamp[i][3] = 255;
	}

	// a recognizable color until the front end clears or draws
	mb_clearColor[0] = 0.0f;
	mb_clearColor[1] = 0.25f;
	mb_clearColor[2] = 0.35f;
	mb_clearColor[3] = 1.0f;
	mb_clearPending = qtrue;

	// Frames in flight. One slot is taken for the first frame right away; afterwards mbEndFrame takes the
	// next frame's slot right after committing (never between input sampling and encoding).
	mb_framesInFlight = Com_Clamp( 1, 3, r_metalFramesInFlight->integer );
	mb_frameSemaphore = dispatch_semaphore_create( mb_framesInFlight );
	dispatch_semaphore_wait( mb_frameSemaphore, DISPATCH_TIME_FOREVER );

	if ( !MB_StartDrawableAcquisition() ) {
		mbShutdown();
		return qfalse;
	}

	ri.Printf( PRINT_ALL, "Metal: %s, %dx%d, %d frames in flight, %d drawables, vsync %s\n",
		mb_rendererString, mb_width, mb_height, mb_framesInFlight, (int)mb_layer.maximumDrawableCount,
		mb_layer.displaySyncEnabled ? "on" : "off" );

	return qtrue;
}

/*
** mbShutdown
*/
void mbShutdown( void )
{
	int i;

	if ( mb_frameSemaphore ) {
		// We hold one slot (the next frame's). Taking the others means every frame in flight has completed;
		// then give them all back: libdispatch aborts if a semaphore is freed below its initial value.
		for ( i = 1; i < mb_framesInFlight; i++ ) {
			dispatch_semaphore_wait( mb_frameSemaphore, DISPATCH_TIME_FOREVER );
		}
		for ( i = 0; i < mb_framesInFlight; i++ ) {
			dispatch_semaphore_signal( mb_frameSemaphore );
		}
	}
	mb_lastCommitted = nil;

	MB_StopDrawableAcquisition();

	if ( mb_statFrames && r_metalPresentStats->integer ) {
		mach_timebase_info_data_t tb;

		mach_timebase_info( &tb );
		ri.Printf( PRINT_ALL, "Metal: %d frames, %d presented (%d skipped: no drawable ready), %d shown; "
			"per frame: %.3f ms waiting for a drawable, %.3f ms waiting for a frame slot\n",
			mb_statFrames, (int)mb_statPresented, mb_statSkipped, (int)mb_statShown,
			(double)mb_statDrawableWait * tb.numer / tb.denom / 1e6 / mb_statFrames,
			(double)mb_statSlotWait * tb.numer / tb.denom / 1e6 / mb_statFrames );
	}
	mb_statFrames = mb_statSkipped = 0;
	mb_statPresented = mb_statShown = 0;
	mb_statDrawableWait = mb_statSlotWait = 0;

	mb_sceneColor = nil;
	mb_finalPipeline = nil;
	mb_layer = nil;
	mb_frameSemaphore = nil;
	mb_width = mb_height = 0;
}

const char *mbRendererString( void )
{
	return mb_rendererString;
}

int mbMaxTextureSize( void )
{
	return 16384;	// every Apple GPU family supported by macOS 26
}

/*
** mbSetGamma
*/
void mbSetGamma( const unsigned char red[256], const unsigned char green[256], const unsigned char blue[256] )
{
	int i;

	for ( i = 0; i < 256; i++ ) {
		mb_gammaRamp[i][0] = red[i];
		mb_gammaRamp[i][1] = green[i];
		mb_gammaRamp[i][2] = blue[i];
	}
}

/*
** mbEndFrame
*/
void mbEndFrame( void )
{
	if ( !mb_layer ) {
		return;
	}

	@autoreleasepool {
		id<MTLCommandBuffer> cmd;
		MTLRenderPassDescriptor *pass;
		id<MTLRenderCommandEncoder> enc;
		id<CAMetalDrawable> drawable;
		dispatch_semaphore_t semaphore = mb_frameSemaphore;

		cmd = [mb_queue commandBufferWithUnretainedReferences];

		// scene pass (Phase 1: the clear only)
		if ( mb_clearPending ) {
			pass = [MTLRenderPassDescriptor renderPassDescriptor];
			pass.colorAttachments[0].texture = mb_sceneColor;
			pass.colorAttachments[0].loadAction = MTLLoadActionClear;
			pass.colorAttachments[0].storeAction = MTLStoreActionStore;
			pass.colorAttachments[0].clearColor = MTLClearColorMake( mb_clearColor[0], mb_clearColor[1], mb_clearColor[2], mb_clearColor[3] );
			enc = [cmd renderCommandEncoderWithDescriptor:pass];
			[enc endEncoding];
			mb_clearPending = qfalse;
		}

		// final pass into the drawable, if one is ready
		mb_statFrames++;
		drawable = MB_TakeDrawable();
		if ( drawable ) {
			pass = [MTLRenderPassDescriptor renderPassDescriptor];
			pass.colorAttachments[0].texture = drawable.texture;
			pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
			pass.colorAttachments[0].storeAction = MTLStoreActionStore;
			enc = [cmd renderCommandEncoderWithDescriptor:pass];
			[enc setRenderPipelineState:mb_finalPipeline];
			[enc setFragmentTexture:mb_sceneColor atIndex:0];
			[enc setFragmentBytes:mb_gammaRamp length:sizeof( mb_gammaRamp ) atIndex:0];
			[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			[enc endEncoding];
			if ( r_metalPresentStats->integer ) {
				mb_statPresented++;
				[drawable addPresentedHandler:^( id<MTLDrawable> shown ) {
					if ( shown.presentedTime > 0.0 ) {
						mb_statShown++;
					}
				}];
			}
			[cmd presentDrawable:drawable];
		}

		[cmd addCompletedHandler:^( id<MTLCommandBuffer> buffer ) {
			dispatch_semaphore_signal( semaphore );
		}];
		[cmd commit];
		mb_lastCommitted = cmd;
	}

	// Take the next frame's slot (and with vsync its drawable) now, at the end of this frame: the waits happen
	// before the engine's frame limiter and input sampling, never between input sampling and present.
	{
		uint64_t t0 = mach_absolute_time();
		uint64_t t1;

		dispatch_semaphore_wait( mb_frameSemaphore, DISPATCH_TIME_FOREVER );
		t1 = mach_absolute_time();
		mb_statSlotWait += t1 - t0;

		if ( mb_vsync ) {
			@autoreleasepool {
				mb_nextDrawable = [mb_layer nextDrawable];
			}
			mb_statDrawableWait += mach_absolute_time() - t1;
		}
	}
}

/*
** mbFinish
*/
void mbFinish( void )
{
	if ( mb_lastCommitted ) {
		[mb_lastCommitted waitUntilCompleted];
	}
}

/*
=============================================================

CLEARS

=============================================================
*/

static float	mb_pendingClearColor[4];

void mbClearColor( float red, float green, float blue, float alpha )
{
	mb_pendingClearColor[0] = red;
	mb_pendingClearColor[1] = green;
	mb_pendingClearColor[2] = blue;
	mb_pendingClearColor[3] = alpha;
}

void mbClearDepth( double depth ) {}
void mbClearStencil( int s ) {}

void mbClear( unsigned int mask )
{
	if ( mask & MB_COLOR_BUFFER_BIT ) {
		mb_clearColor[0] = mb_pendingClearColor[0];
		mb_clearColor[1] = mb_pendingClearColor[1];
		mb_clearColor[2] = mb_pendingClearColor[2];
		mb_clearColor[3] = mb_pendingClearColor[3];
		mb_clearPending = qtrue;
	}
}

/*
=============================================================

TEXTURES

=============================================================
*/

void mbGenTextures( int n, unsigned int *textures )
{
	int i;

	for ( i = 0; i < n; i++ ) {
		textures[i] = mb_nextTextureName++;
	}
}

void mbDeleteTextures( int n, const unsigned int *textures ) {}
void mbActiveTextureARB( int texture ) {}
void mbClientActiveTextureARB( int texture ) {}
void mbBindTexture( int target, unsigned int texture ) {}
void mbTexEnv( int mode ) {}
void mbTexParameterf( int target, int pname, float param ) {}
void mbTexParameteri( int target, int pname, int param ) {}
void mbTexImage2D( int target, int level, int internalformat, int width, int height, int border, int format, int type, const void *pixels ) {}
void mbTexSubImage2D( int target, int level, int xoffset, int yoffset, int width, int height, int format, int type, const void *pixels ) {}
void mbCompressedTexImage2D( int target, int level, int internalformat, int width, int height, int border, int imageSize, const void *data ) {}

/*
=============================================================

READBACK

=============================================================
*/

void mbReadPixels( int x, int y, int width, int height, int format, int type, void *pixels )
{
	int components;
	int row, col;

	switch ( format ) {
	case MB_RGBA:
		components = 4;
		break;
	case MB_RGB:
		components = 3;
		break;
	default:
		components = 1;
		break;
	}
	if ( type == MB_FLOAT ) {
		components *= sizeof( float );
	}

	if ( ( format != MB_RGB && format != MB_RGBA ) || !mb_sceneColor
		|| x < 0 || y < 0 || x + width > mb_width || y + height > mb_height ) {
		// depth/stencil readback: Phase 3
		memset( pixels, 0, (size_t)width * height * components );
		return;
	}

	// Synchronous copy of the scene target (screenshots and video frames only, never in the frame path;
	// Phase 3 makes it asynchronous). GL rows are bottom-up, Metal rows top-down.
	@autoreleasepool {
		id<MTLBuffer> buffer;
		id<MTLCommandBuffer> cmd;
		id<MTLBlitCommandEncoder> blit;
		const byte *src;
		byte *dst;

		buffer = [mb_device newBufferWithLength:(NSUInteger)width * height * 4 options:MTLResourceStorageModeShared];
		cmd = [mb_queue commandBuffer];
		blit = [cmd blitCommandEncoder];
		[blit copyFromTexture:mb_sceneColor sourceSlice:0 sourceLevel:0
			sourceOrigin:MTLOriginMake( x, mb_height - y - height, 0 ) sourceSize:MTLSizeMake( width, height, 1 )
			toBuffer:buffer destinationOffset:0 destinationBytesPerRow:width * 4 destinationBytesPerImage:width * height * 4];
		[blit endEncoding];
		[cmd commit];
		[cmd waitUntilCompleted];

		dst = pixels;
		for ( row = 0; row < height; row++ ) {
			src = (const byte *)buffer.contents + (size_t)( height - 1 - row ) * width * 4;
			for ( col = 0; col < width; col++, src += 4, dst += components ) {
				dst[0] = src[0];
				dst[1] = src[1];
				dst[2] = src[2];
				if ( components == 4 ) {
					dst[3] = src[3];
				}
			}
		}
	}
}

void mbDrawPixels( int width, int height, int format, int type, const void *pixels ) {}
void mbPixelZoom( float xfactor, float yfactor ) {}

/*
=============================================================

STATE (Phase 2)

=============================================================
*/

void mbEnable( int cap ) {}
void mbDisable( int cap ) {}
void mbBlendFunc( int sfactor, int dfactor ) {}
void mbDepthFunc( int func ) {}
void mbDepthMask( qboolean flag ) {}
void mbDepthRange( double zNear, double zFar ) {}
void mbAlphaFunc( int func, float ref ) {}
void mbCullFace( int mode ) {}
void mbPolygonMode( int face, int mode ) {}
void mbPolygonOffset( float factor, float units ) {}
void mbColorMask( qboolean red, qboolean green, qboolean blue, qboolean alpha ) {}
void mbStencilFunc( int func, int ref, unsigned int mask ) {}
void mbStencilOp( int fail, int zfail, int zpass ) {}
void mbStencilMask( unsigned int mask ) {}
void mbShadeModel( int mode ) {}
void mbLineWidth( float width ) {}
void mbLineStipple( int factor, unsigned short pattern ) {}
void mbFogf( int pname, float param ) {}
void mbFogfv( int pname, const float *params ) {}
void mbFogi( int pname, int param ) {}
void mbClipPlane( int plane, const double *equation ) {}
void mbViewport( int x, int y, int width, int height ) {}
void mbScissor( int x, int y, int width, int height ) {}
void mbDrawBuffer( int mode ) {}

void mbMatrixMode( int mode ) {}
void mbLoadMatrixf( const float *m ) {}
void mbLoadIdentity( void ) {}
void mbOrtho( double left, double right, double bottom, double top, double zNear, double zFar ) {}
void mbPushMatrix( void ) {}
void mbPopMatrix( void ) {}
void mbTranslatef( float x, float y, float z ) {}

void mbEnableClientState( int array ) {}
void mbDisableClientState( int array ) {}
void mbVertexPointer( int size, int type, int stride, const void *pointer ) {}
void mbColorPointer( int size, int type, int stride, const void *pointer ) {}
void mbTexCoordPointer( int size, int type, int stride, const void *pointer ) {}
void mbLockArraysEXT( int first, int count ) {}
void mbUnlockArraysEXT( void ) {}
void mbDrawElements( int mode, int count, int type, const void *indices ) {}
void mbArrayElement( int i ) {}

void mbBegin( int mode ) {}
void mbEnd( void ) {}
void mbVertex2f( float x, float y ) {}
void mbVertex3f( float x, float y, float z ) {}
void mbVertex3fv( const float *v ) {}
void mbTexCoord2f( float s, float t ) {}
void mbTexCoord2fv( const float *v ) {}
void mbMultiTexCoord2fARB( int target, float s, float t ) {}
void mbColor3f( float red, float green, float blue ) {}
void mbColor4f( float red, float green, float blue, float alpha ) {}
void mbColor4ubv( const byte *v ) {}
