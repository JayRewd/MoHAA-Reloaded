/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.
Copyright (C) 2026 the OpenMoHAA Metal port contributors

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// sdl_mtlimp.m -- the Metal renderer's window: an SDL window with a CAMetalLayer (SDL_Metal_CreateView).
// Mode selection and fullscreen handling follow sdl_glimp.c; there is no GL context.
//
// Low latency (docs/METAL_PORT_PLAN.md 4b): the layer is opaque, has 2 drawables (3 for vsync in fullscreen),
// presents immediately when r_swapInterval is 0 (displaySyncEnabled NO) and never with a Core Animation
// transaction. The backend acquires the drawable at the end of the frame and waits for frame slots only after
// committing.
//
// Fullscreen goes direct to display (the compositor is bypassed): r_fullscreen 1 is SDL's exclusive fullscreen
// (a borderless window above everything, at the mode's size), r_fullscreen 1 + r_noborder 1 is borderless
// fullscreen (a native macOS fullscreen Space at the desktop resolution, no display mode change). Fullscreen
// windows have no shadow: a window shadow needs an offscreen pass over the layer, so the layer can't go direct.

#ifdef USE_INTERNAL_SDL_HEADERS
#	include "SDL.h"
#else
#	include <SDL.h>
#endif

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <math.h>

#include "../renderercommon/tr_common.h"
#include "../renderer_metal/tr_mtl.h"
#include "../sys/sys_local.h"
#include "sdl_icon.h"

typedef enum
{
	RSERR_OK,

	RSERR_INVALID_FULLSCREEN,
	RSERR_INVALID_MODE,

	RSERR_UNKNOWN
} rserr_t;

SDL_Window *SDL_window = NULL;
static SDL_MetalView SDL_metalView = NULL;

cvar_t *r_allowResize; // make window resizable
cvar_t *r_centerWindow;
cvar_t *r_sdlDriver;
cvar_t *r_displayIndex; // display to open the window on (0 = main)
cvar_t *r_metalDrawables; // CAMetalLayer.maximumDrawableCount (2 or 3; 0 = 3 for vsync in fullscreen, else 2)

/*
===============
GLimp_DestroyWindow
===============
*/
static void GLimp_DestroyWindow( void )
{
	mbShutdown();

	if( SDL_metalView != NULL )
	{
		SDL_Metal_DestroyView( SDL_metalView );
		SDL_metalView = NULL;
	}

	if( SDL_window != NULL )
	{
		SDL_DestroyWindow( SDL_window );
		SDL_window = NULL;
	}
}

/*
===============
GLimp_Shutdown
===============
*/
void GLimp_Shutdown( void )
{
	ri.IN_Shutdown();

	GLimp_DestroyWindow();

	SDL_QuitSubSystem( SDL_INIT_VIDEO );
}

/*
===============
GLimp_Minimize

Minimize the game so that user is back at the desktop
===============
*/
void GLimp_Minimize( void )
{
	SDL_MinimizeWindow( SDL_window );
}


/*
===============
GLimp_LogComment
===============
*/
void GLimp_LogComment( char *comment )
{
}

/*
=================
GLimp_SetGamma

There is no hardware gamma: the ramp is applied in the backend's final pass.
=================
*/
void GLimp_SetGamma( unsigned char red[256], unsigned char green[256], unsigned char blue[256] )
{
	mbSetGamma( red, green, blue );
}

/*
===============
GLimp_CompareModes
===============
*/
static int GLimp_CompareModes( const void *a, const void *b )
{
	const float ASPECT_EPSILON = 0.001f;
	SDL_Rect *modeA = (SDL_Rect *)a;
	SDL_Rect *modeB = (SDL_Rect *)b;
	float aspectA = (float)modeA->w / (float)modeA->h;
	float aspectB = (float)modeB->w / (float)modeB->h;
	int areaA = modeA->w * modeA->h;
	int areaB = modeB->w * modeB->h;
	float aspectDiffA = fabs( aspectA - displayAspect );
	float aspectDiffB = fabs( aspectB - displayAspect );
	float aspectDiffsDiff = aspectDiffA - aspectDiffB;

	if( aspectDiffsDiff > ASPECT_EPSILON )
		return 1;
	else if( aspectDiffsDiff < -ASPECT_EPSILON )
		return -1;
	else
		return areaA - areaB;
}


/*
===============
GLimp_DetectAvailableModes
===============
*/
static void GLimp_DetectAvailableModes(void)
{
	int i, j;
	char buf[ MAX_STRING_CHARS ] = { 0 };
	int numSDLModes;
	SDL_Rect *modes;
	int numModes = 0;

	SDL_DisplayMode windowMode;
	int display = SDL_GetWindowDisplayIndex( SDL_window );
	if( display < 0 )
	{
		ri.Printf( PRINT_WARNING, "Couldn't get window display index, no resolutions detected: %s\n", SDL_GetError() );
		return;
	}
	numSDLModes = SDL_GetNumDisplayModes( display );

	if( SDL_GetWindowDisplayMode( SDL_window, &windowMode ) < 0 || numSDLModes <= 0 )
	{
		ri.Printf( PRINT_WARNING, "Couldn't get window display mode, no resolutions detected: %s\n", SDL_GetError() );
		return;
	}

	modes = SDL_calloc( (size_t)numSDLModes, sizeof( SDL_Rect ) );
	if ( !modes )
	{
		ri.Error( ERR_FATAL, "Out of memory" );
	}

	for( i = 0; i < numSDLModes; i++ )
	{
		SDL_DisplayMode mode;

		if( SDL_GetDisplayMode( display, i, &mode ) < 0 )
			continue;

		if( !mode.w || !mode.h )
		{
			ri.Printf( PRINT_ALL, "Display supports any resolution\n" );
			SDL_free( modes );
			return;
		}

		if( windowMode.format != mode.format )
			continue;

		// SDL can give the same resolution with different refresh rates.
		// Only list resolution once.
		for( j = 0; j < numModes; j++ )
		{
			if( mode.w == modes[ j ].w && mode.h == modes[ j ].h )
				break;
		}

		if( j != numModes )
			continue;

		modes[ numModes ].w = mode.w;
		modes[ numModes ].h = mode.h;
		numModes++;
	}

	if( numModes > 1 )
		qsort( modes, numModes, sizeof( SDL_Rect ), GLimp_CompareModes );

	for( i = 0; i < numModes; i++ )
	{
		const char *newModeString = va( "%ux%u ", modes[ i ].w, modes[ i ].h );

		if( strlen( newModeString ) < (int)sizeof( buf ) - strlen( buf ) )
			Q_strcat( buf, sizeof( buf ), newModeString );
		else
			ri.Printf( PRINT_WARNING, "Skipping mode %ux%u, buffer too small\n", modes[ i ].w, modes[ i ].h );
	}

	if( *buf )
	{
		buf[ strlen( buf ) - 1 ] = 0;
		ri.Printf( PRINT_ALL, "Available modes: '%s'\n", buf );
		ri.Cvar_Set( "r_availableModes", buf );
	}
	SDL_free( modes );
}

/*
===============
GLimp_SetMode
===============
*/
static int GLimp_SetMode(int mode, qboolean fullscreen, qboolean noborder)
{
	SDL_Surface *icon = NULL;
	Uint32 flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_METAL | SDL_WINDOW_ALLOW_HIGHDPI;
	SDL_DisplayMode desktopMode;
	int display = 0;
	int x = SDL_WINDOWPOS_UNDEFINED, y = SDL_WINDOWPOS_UNDEFINED;
	int windowWidth, windowHeight;
	int drawableWidth, drawableHeight;
	CAMetalLayer *layer;

	ri.Printf( PRINT_ALL, "Initializing Metal display\n");

	if ( r_allowResize->integer )
		flags |= SDL_WINDOW_RESIZABLE;

#ifdef USE_ICON
	icon = SDL_CreateRGBSurfaceFrom(
			(void *)CLIENT_WINDOW_ICON.pixel_data,
			CLIENT_WINDOW_ICON.width,
			CLIENT_WINDOW_ICON.height,
			CLIENT_WINDOW_ICON.bytes_per_pixel * 8,
			CLIENT_WINDOW_ICON.bytes_per_pixel * CLIENT_WINDOW_ICON.width,
#ifdef Q3_LITTLE_ENDIAN
			0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000
#else
			0xFF000000, 0x00FF0000, 0x0000FF00, 0x000000FF
#endif
			);
#endif

	// If a window exists, note its display index
	if( SDL_window != NULL )
	{
		display = SDL_GetWindowDisplayIndex( SDL_window );
		if( display < 0 )
		{
			ri.Printf( PRINT_DEVELOPER, "SDL_GetWindowDisplayIndex() failed: %s\n", SDL_GetError() );
			display = 0;
		}
	}
	else if( r_displayIndex->integer > 0 && r_displayIndex->integer < SDL_GetNumVideoDisplays() )
	{
		display = r_displayIndex->integer;
		x = y = SDL_WINDOWPOS_UNDEFINED_DISPLAY( display );
	}

	if( SDL_GetDesktopDisplayMode( display, &desktopMode ) == 0 )
	{
		displayAspect = (float)desktopMode.w / (float)desktopMode.h;

		ri.Printf( PRINT_ALL, "Display aspect: %.3f\n", displayAspect );
	}
	else
	{
		Com_Memset( &desktopMode, 0, sizeof( SDL_DisplayMode ) );

		ri.Printf( PRINT_ALL,
				"Cannot determine display aspect, assuming 1.333\n" );
	}

	ri.Printf (PRINT_ALL, "...setting mode %d:", mode );

	// the mode is the window size in points; the drawable (glConfig) gets the pixel size below
	if (mode == -2 || ( fullscreen && noborder ))
	{
		// use desktop video resolution
		if( desktopMode.h > 0 )
		{
			windowWidth = desktopMode.w;
			windowHeight = desktopMode.h;
		}
		else
		{
			windowWidth = 640;
			windowHeight = 480;
			ri.Printf( PRINT_ALL,
					"Cannot determine display resolution, assuming 640x480\n" );
		}
	}
	else if ( !R_GetModeInfo( &windowWidth, &windowHeight, &glConfig.windowAspect, mode ) )
	{
		ri.Printf( PRINT_ALL, " invalid mode\n" );
		return RSERR_INVALID_MODE;
	}
	glConfig.windowAspect = (float)windowWidth / (float)windowHeight;
	ri.Printf( PRINT_ALL, " %d %d\n", windowWidth, windowHeight);

	// Center window
	if( r_centerWindow->integer && !fullscreen )
	{
		x = SDL_WINDOWPOS_CENTERED_DISPLAY( display );
		y = SDL_WINDOWPOS_CENTERED_DISPLAY( display );
	}

	// Destroy existing state if it exists
	if( SDL_window != NULL )
	{
		SDL_GetWindowPosition( SDL_window, &x, &y );
		ri.Printf( PRINT_DEVELOPER, "Existing window at %dx%d before being destroyed\n", x, y );
		GLimp_DestroyWindow();
	}

	if( fullscreen )
	{
		flags |= noborder ? SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_FULLSCREEN;
		glConfig.isFullscreen = qtrue;
	}
	else
	{
		if( noborder )
			flags |= SDL_WINDOW_BORDERLESS;

		glConfig.isFullscreen = qfalse;
	}

	glConfig.stereoEnabled = qfalse;

	if( ( SDL_window = SDL_CreateWindow( CLIENT_WINDOW_TITLE, x, y,
			windowWidth, windowHeight, flags ) ) == NULL )
	{
		ri.Printf( PRINT_ALL, "SDL_CreateWindow failed: %s\n", SDL_GetError( ) );
		SDL_FreeSurface( icon );
		return RSERR_INVALID_MODE;
	}

	if( fullscreen && !noborder )
	{
		SDL_DisplayMode desiredMode;

		Com_Memset( &desiredMode, 0, sizeof( desiredMode ) );
		desiredMode.w = windowWidth;
		desiredMode.h = windowHeight;
		desiredMode.refresh_rate = glConfig.displayFrequency = ri.Cvar_VariableIntegerValue( "r_displayRefresh" );

		if( SDL_SetWindowDisplayMode( SDL_window, &desiredMode ) < 0 )
		{
			ri.Printf( PRINT_DEVELOPER, "SDL_SetWindowDisplayMode failed: %s\n", SDL_GetError( ) );
			GLimp_DestroyWindow();
			SDL_FreeSurface( icon );
			return RSERR_INVALID_FULLSCREEN;
		}
	}

	SDL_SetWindowIcon( SDL_window, icon );
	SDL_FreeSurface( icon );

	SDL_metalView = SDL_Metal_CreateView( SDL_window );
	if( !SDL_metalView )
	{
		ri.Printf( PRINT_ALL, "SDL_Metal_CreateView failed: %s\n", SDL_GetError( ) );
		GLimp_DestroyWindow();
		return RSERR_INVALID_MODE;
	}
	layer = (__bridge CAMetalLayer *)SDL_Metal_GetLayer( SDL_metalView );

	// render at the drawable's full pixel size (Retina: 2x the window size in points)
	SDL_Metal_GetDrawableSize( SDL_window, &drawableWidth, &drawableHeight );
	glConfig.vidWidth = drawableWidth;
	glConfig.vidHeight = drawableHeight;

	layer.opaque = YES;
	// Fullscreen goes direct to display, and there vsync with 2 drawables ran the game at half the refresh rate
	// (125 fps at 240 Hz: every frame waited ~7 ms for a drawable). 3 keep the full rate at the same latency.
	// Without vsync 2 are better: with 3, more presented frames are never shown and latency goes up.
	if( r_metalDrawables->integer == 2 || r_metalDrawables->integer == 3 )
		layer.maximumDrawableCount = r_metalDrawables->integer;
	else
		layer.maximumDrawableCount = fullscreen && r_swapInterval->integer ? 3 : 2;
	layer.displaySyncEnabled = r_swapInterval->integer ? YES : NO;
	layer.presentsWithTransaction = NO;
	layer.drawableSize = CGSizeMake( drawableWidth, drawableHeight );

	if( !mbInit( (__bridge void *)layer ) )
	{
		GLimp_DestroyWindow();
		return RSERR_UNKNOWN;
	}

	// the backend renders into its own target; these describe what the front end gets
	glConfig.colorBits = 24;
	glConfig.depthBits = 32;
	glConfig.stencilBits = 8;

	ri.Printf( PRINT_ALL, "Using %d color bits, %d depth, %d stencil display, %dx%d pixels.\n",
			glConfig.colorBits, glConfig.depthBits, glConfig.stencilBits, glConfig.vidWidth, glConfig.vidHeight );

	SDL_ShowWindow( SDL_window );

	// after showing: that is when SDL makes the window fullscreen (and sets its style)
	if( fullscreen )
	{
		( (__bridge NSView *)SDL_metalView ).window.hasShadow = NO;
	}

	GLimp_DetectAvailableModes();

	ri.Printf( PRINT_ALL, "GL_RENDERER: %s\n", mbRendererString() );

	return RSERR_OK;
}

/*
===============
GLimp_StartDriverAndSetMode
===============
*/
static qboolean GLimp_StartDriverAndSetMode(int mode, qboolean fullscreen, qboolean noborder)
{
	rserr_t err;

	if (!SDL_WasInit(SDL_INIT_VIDEO))
	{
		const char *driverName;

		if (SDL_Init(SDL_INIT_VIDEO) != 0)
		{
			ri.Printf( PRINT_ALL, "SDL_Init( SDL_INIT_VIDEO ) FAILED (%s)\n", SDL_GetError());
			return qfalse;
		}

		driverName = SDL_GetCurrentVideoDriver( );
		ri.Printf( PRINT_ALL, "SDL using driver \"%s\"\n", driverName );
		ri.Cvar_Set( "r_sdlDriver", driverName );
	}

	if (fullscreen && ri.Cvar_VariableIntegerValue( "in_nograb" ) )
	{
		ri.Printf( PRINT_ALL, "Fullscreen not allowed with in_nograb 1\n");
		ri.Cvar_Set( "r_fullscreen", "0" );
		r_fullscreen->modified = qfalse;
		fullscreen = qfalse;
	}

	err = GLimp_SetMode(mode, fullscreen, noborder);

	switch ( err )
	{
		case RSERR_INVALID_FULLSCREEN:
			ri.Printf( PRINT_ALL, "...WARNING: fullscreen unavailable in this mode\n" );
			return qfalse;
		case RSERR_INVALID_MODE:
			ri.Printf( PRINT_ALL, "...WARNING: could not set the given mode (%d)\n", mode );
			return qfalse;
		case RSERR_UNKNOWN:
			ri.Printf( PRINT_ALL, "...WARNING: Metal initialization failed\n" );
			return qfalse;
		default:
			break;
	}

	return qtrue;
}


/*
===============
GLimp_InitCapabilities

What the GL1 front end reads from extensions, set the way GL1 sets them on Apple's GL driver so shader
collapsing and image uploads take the same paths.
===============
*/
static void GLimp_InitCapabilities( void )
{
	glConfig.textureCompression = TC_NONE;
	if ( r_ext_compressed_textures->value )
	{
		glConfig.textureCompression = TC_S3TC_ARB;
		ri.Printf( PRINT_ALL, "...using BC1-3 (S3TC) textures\n" );
	}

	glConfig.textureEnvAddAvailable = r_ext_texture_env_add->integer ? qtrue : qfalse;

	mbMultitexture = r_ext_multitexture->value ? qtrue : qfalse;
	glConfig.numTextureUnits = mbMultitexture ? 8 : 1;

	textureFilterAnisotropic = qfalse;
	maxAnisotropy = 0;
	if ( r_ext_texture_filter_anisotropic->integer )
	{
		maxAnisotropy = 16;
		textureFilterAnisotropic = qtrue;
		ri.Printf( PRINT_ALL, "...using anisotropic filtering (max: %i)\n", maxAnisotropy );
	}

	haveClampToEdge = qtrue;
}

#define R_MODE_FALLBACK 3 // 640 * 480

/*
===============
GLimp_Init

This routine is responsible for initializing the OS specific portions
of the renderer (window, Metal layer)
===============
*/
void GLimp_Init( qboolean fixedFunction )
{
	ri.Printf( PRINT_DEVELOPER, "Glimp_Init( )\n" );

	r_sdlDriver = ri.Cvar_Get( "r_sdlDriver", "", CVAR_ROM );
	r_allowResize = ri.Cvar_Get( "r_allowResize", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_centerWindow = ri.Cvar_Get( "r_centerWindow", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_displayIndex = ri.Cvar_Get( "r_displayIndex", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_metalDrawables = ri.Cvar_Get( "r_metalDrawables", "0", CVAR_ARCHIVE | CVAR_LATCH );

	if( ri.Cvar_VariableIntegerValue( "com_abnormalExit" ) )
	{
		ri.Cvar_Set( "r_mode", va( "%d", R_MODE_FALLBACK ) );
		ri.Cvar_Set( "r_fullscreen", "0" );
		ri.Cvar_Set( "r_centerWindow", "0" );
		ri.Cvar_Set( "com_abnormalExit", "0" );
	}

	ri.Sys_GLimpInit( );

	// Create the window and its Metal layer
	if(GLimp_StartDriverAndSetMode(r_mode->integer, r_fullscreen->integer, r_noborder->integer))
		goto success;

	// Try again, this time in a platform specific "safe mode"
	ri.Sys_GLimpSafeInit( );

	if(GLimp_StartDriverAndSetMode(r_mode->integer, r_fullscreen->integer, qfalse))
		goto success;

	// Finally, try the default screen resolution
	if( r_mode->integer != R_MODE_FALLBACK )
	{
		ri.Printf( PRINT_ALL, "Setting r_mode %d failed, falling back on r_mode %d\n",
				r_mode->integer, R_MODE_FALLBACK );

		if(GLimp_StartDriverAndSetMode(R_MODE_FALLBACK, qfalse, qfalse))
			goto success;
	}

	// Nothing worked, give up
	ri.Error( ERR_FATAL, "GLimp_Init() - could not initialize Metal" );

success:
	// These values force the UI to disable driver selection
	glConfig.driverType = GLDRV_ICD;
	glConfig.hardwareType = GLHW_GENERIC;

	// gamma and overbright are applied in the backend's final pass
	glConfig.deviceSupportsGamma = !r_ignorehwgamma->integer;

	glConfig.maxTextureSize = mbMaxTextureSize();

	// get our config strings
	Q_strncpyz( glConfig.vendor_string, "Apple", sizeof( glConfig.vendor_string ) );
	Q_strncpyz( glConfig.renderer_string, mbRendererString(), sizeof( glConfig.renderer_string ) );
	Q_strncpyz( glConfig.version_string, "Metal 4", sizeof( glConfig.version_string ) );
	glConfig.extensions_string[0] = 0;

	GLimp_InitCapabilities();

	ri.Cvar_Get( "r_availableModes", "", CVAR_ROM );

	// This depends on SDL_INIT_VIDEO, hence having it here
	ri.IN_Init( SDL_window );
}


/*
===============
GLimp_EndFrame

Responsible for doing a swapbuffers
===============
*/
void GLimp_EndFrame( void )
{
	mbEndFrame();

	if( r_fullscreen->modified )
	{
		int wantFullscreen;

		// r_fullscreen is latched (Alt+Enter only stores the new value), so rebuild the window, the backend
		// targets and glConfig at once with vid_restart instead of toggling the window in place.
		wantFullscreen = r_fullscreen->latchedString ? atoi( r_fullscreen->latchedString ) : r_fullscreen->integer;

		if( wantFullscreen && ri.Cvar_VariableIntegerValue( "in_nograb" ) )
		{
			ri.Printf( PRINT_ALL, "Fullscreen not allowed with in_nograb 1\n");
			ri.Cvar_Set( "r_fullscreen", "0" );
			wantFullscreen = 0;
		}

		if( !!wantFullscreen != !!glConfig.isFullscreen )
		{
			ri.Cmd_ExecuteText( EXEC_APPEND, "vid_restart\n" );
		}

		r_fullscreen->modified = qfalse;
	}
}
