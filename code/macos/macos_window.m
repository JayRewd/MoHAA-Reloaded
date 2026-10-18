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
// macos_window.m -- the Metal renderer's window, native: an NSWindow whose content view hosts the CAMetalLayer.
// The same MTLimp_* functions, cvars and log lines as sdl/sdl_mtlimp.m, which made the window with SDL until
// Phase 6; mode selection follows upstream's SDL window code of the GL1 renderer.
//
// Low latency (docs/METAL_PORT_PLAN.md 4b): the layer is opaque, has 2 drawables (3 for vsync in fullscreen),
// presents immediately when r_swapInterval is 0 (displaySyncEnabled NO) and never with a Core Animation
// transaction. The backend acquires the drawable at the end of the frame and waits for frame slots only after
// committing.
//
// The display modes:
// - a window: titled (borderless with r_noborder), resizable with r_allowResize, on r_displayIndex, centered (SDL
//   centers windows whether r_centerWindow is set or not), at its old position after a vid_restart;
// - exclusive fullscreen, r_fullscreen 1: a borderless window over the screen at the shielding level, at r_mode's
//   size. The display mode changes only when that size isn't the current mode's (the displays captured as SDL
//   captures them). Losing the focus minimizes the window and restores the desktop's mode, as SDL does;
// - borderless fullscreen, r_fullscreen 1 + r_noborder 1: a native fullscreen Space at the desktop resolution, or
//   with r_borderlessWindow 1 a borderless window over the screen at the normal level, the dock and menu bar
//   hidden while the game is in front (no Space animation when switching away or at vid_restart).
// Both fullscreen modes go direct to display (the compositor is bypassed) and have no window shadow: a shadow needs
// an offscreen pass over the layer, so the layer can't go direct.

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#include <IOKit/graphics/IOGraphicsTypes.h>

#include <math.h>

#include "../renderercommon/tr_common.h"
#include "../renderer_metal/tr_mtl.h"
#include "../sys/sys_local.h"
#include "macos_local.h"

typedef enum
{
	RSERR_OK,

	RSERR_INVALID_FULLSCREEN,
	RSERR_INVALID_MODE,

	RSERR_UNKNOWN
} rserr_t;

typedef enum
{
	MAC_WINDOWED,
	MAC_EXCLUSIVE,			// r_fullscreen 1
	MAC_SPACE,				// r_fullscreen 1 + r_noborder 1
	MAC_DESKTOPWINDOW		// r_fullscreen 1 + r_noborder 1 + r_borderlessWindow 1
} macWindowMode_t;

@interface MacOSWindow : NSWindow
@end

@interface MacOSView : NSView <NSTextInputClient>
@end

@interface MacOSWindowDelegate : NSObject <NSWindowDelegate>
@end

static MacOSWindow			*mac_window;
static MacOSView			*mac_view;
static CAMetalLayer			*mac_layer;
static MacOSWindowDelegate	*mac_windowDelegate;
static macWindowMode_t		mac_windowMode;

static CGDirectDisplayID	mac_display;			// the window's display
static CGDisplayModeRef		mac_desktopMode;		// exclusive fullscreen: the display's own mode, restored
static CGDisplayModeRef		mac_exclusiveMode;		// exclusive fullscreen: r_mode's mode, NULL if it is the desktop's
static qboolean				mac_modeSwitched;		// mac_exclusiveMode is set on the display
static qboolean				mac_exclusiveActive;	// the exclusive window is up (not minimized for a focus loss)

cvar_t *r_allowResize; // make window resizable
cvar_t *r_centerWindow;
cvar_t *r_displayIndex; // display to open the window on (0 = main)
cvar_t *r_metalDrawables; // CAMetalLayer.maximumDrawableCount (2 or 3; 0 = 3 for vsync in fullscreen, else 2)
static cvar_t *r_borderlessWindow; // borderless fullscreen: 1 = a borderless window over the screen, 0 = a fullscreen Space

static void MTLimp_EnterExclusive( void );
static void MTLimp_LeaveExclusive( qboolean minimize );

@implementation MacOSWindow

// borderless windows (exclusive fullscreen, r_noborder) can't take the keyboard otherwise
- (BOOL)canBecomeKeyWindow
{
	return YES;
}

- (BOOL)canBecomeMainWindow
{
	return YES;
}

@end

@implementation MacOSView
{
	NSString	*markedText;	// an input method's text being composed; it never reaches the game
	NSRange		markedRange;
	NSRange		selectedRange;
}

- (BOOL)acceptsFirstResponder
{
	return YES;
}

- (BOOL)isOpaque
{
	return YES;
}

// the event pump handles the keys (macos_input.m); swallowing them here keeps AppKit from beeping
- (void)keyDown:(NSEvent *)event
{
}

- (void)keyUp:(NSEvent *)event
{
}

- (void)flagsChanged:(NSEvent *)event
{
}

// The mouse's buttons, its wheel and the cursor's movement over the window reach the input layer here, through the
// responder chain, as SDL's did (macos_input.m); the captured mouse's movement is read in the pump.
- (void)mouseDown:(NSEvent *)event
{
	MacOS_MouseButton( event, qtrue );
}

- (void)rightMouseDown:(NSEvent *)event
{
	MacOS_MouseButton( event, qtrue );
}

- (void)otherMouseDown:(NSEvent *)event
{
	MacOS_MouseButton( event, qtrue );
}

- (void)mouseUp:(NSEvent *)event
{
	MacOS_MouseButton( event, qfalse );
}

- (void)rightMouseUp:(NSEvent *)event
{
	MacOS_MouseButton( event, qfalse );
}

- (void)otherMouseUp:(NSEvent *)event
{
	MacOS_MouseButton( event, qfalse );
}

- (void)mouseMoved:(NSEvent *)event
{
	MacOS_MouseMoved( event );
}

- (void)mouseDragged:(NSEvent *)event
{
	MacOS_MouseMoved( event );
}

- (void)rightMouseDragged:(NSEvent *)event
{
	MacOS_MouseMoved( event );
}

- (void)otherMouseDragged:(NSEvent *)event
{
	MacOS_MouseMoved( event );
}

- (void)scrollWheel:(NSEvent *)event
{
	MacOS_MouseWheel( event );
}

// the UI's cursor over the window, none while the game has the mouse
- (void)resetCursorRects
{
	[self addCursorRect:self.bounds cursor:MacOS_Cursor()];
}

// The text input client: the pump passes each key-down through interpretKeyEvents: (macos_input.m), so dead keys
// and input methods compose text as in any text field. It is SDL's (SDLTranslatorResponder), without a subview: the
// view's only layer stays the CAMetalLayer, which fullscreen needs to go direct to display.
- (void)insertText:(id)string replacementRange:(NSRange)replacementRange
{
	if( [string isKindOfClass:[NSAttributedString class]] )
		string = [string string];

	// the composed text replaces the marked text
	if( [self hasMarkedText] )
		[self unmarkText];

	MacOS_InsertText( string );
}

// the keys reach the game as key events: the commands they map to (cancelOperation:, insertNewline:, ...) are
// ignored, and nothing beeps
- (void)doCommandBySelector:(SEL)selector
{
}

- (void)setMarkedText:(id)string selectedRange:(NSRange)selected replacementRange:(NSRange)replacementRange
{
	if( [string isKindOfClass:[NSAttributedString class]] )
		string = [string string];

	if( [string length] == 0 )
	{
		[self unmarkText];
		return;
	}

	markedText = [string copy];
	selectedRange = selected;
	markedRange = NSMakeRange( 0, markedText.length );
}

- (void)unmarkText
{
	markedText = nil;
}

- (BOOL)hasMarkedText
{
	return markedText != nil;
}

- (NSRange)markedRange
{
	return markedRange;
}

- (NSRange)selectedRange
{
	return selectedRange;
}

- (NSArray<NSAttributedStringKey> *)validAttributesForMarkedText
{
	return @[];
}

- (NSAttributedString *)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actualRange
{
	return nil;
}

- (NSUInteger)characterIndexForPoint:(NSPoint)point
{
	return 0;
}

// an input method's candidate window opens at the game's top left corner, as with SDL (the game has no text
// position to give)
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange
{
	NSRect rect = NSZeroRect;

	if( actualRange )
		*actualRange = range;

	rect.origin = [self.window convertPointToScreen:NSMakePoint( 0, self.bounds.size.height )];
	return rect;
}

@end

@implementation MacOSWindowDelegate

// the close button quits like Cmd+Q ("quit Closed window", as SDL's quit event)
- (BOOL)windowShouldClose:(NSWindow *)sender
{
	[NSApp terminate:nil];
	return NO;
}

- (void)windowDidBecomeKey:(NSNotification *)notification
{
	if( mac_windowMode == MAC_EXCLUSIVE )
		MTLimp_EnterExclusive();
}

- (void)windowDidResignKey:(NSNotification *)notification
{
	if( mac_windowMode == MAC_EXCLUSIVE )
		MTLimp_LeaveExclusive( qtrue );
}

- (void)windowDidDeminiaturize:(NSNotification *)notification
{
	if( mac_windowMode == MAC_EXCLUSIVE )
		MTLimp_EnterExclusive();
}

// borderless fullscreen's Space covers the menu bar too, even when the user's settings keep it in fullscreen apps
// ("display rejected" otherwise); SDL asks for the same
- (NSApplicationPresentationOptions)window:(NSWindow *)window
	willUseFullScreenPresentationOptions:(NSApplicationPresentationOptions)proposedOptions
{
	if( mac_windowMode != MAC_SPACE )
		return proposedOptions;

	return NSApplicationPresentationFullScreen | NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar;
}

- (void)windowDidChangeBackingProperties:(NSNotification *)notification
{
	// the drawable keeps its size until the vid_restart that the input side queues (macos_input.m)
	mac_layer.contentsScale = mac_window.backingScaleFactor;
}

// the application became active again (Cmd+Tab, the Dock): bring the window back, as SDL does
- (void)applicationDidBecomeActive:(NSNotification *)notification
{
	if( mac_window.miniaturized )
		[mac_window deminiaturize:nil];
}

@end

/*
===============
MTLimp_GetDisplays

The displays in SDL's order (r_displayIndex): the main display, then the others, without mirrors.
===============
*/
static int MTLimp_GetDisplays( CGDirectDisplayID *displays, int max )
{
	CGDirectDisplayID online[ 32 ];
	uint32_t count = 0, i;
	int pass, num = 0;

	if( CGGetOnlineDisplayList( ARRAY_LEN( online ), online, &count ) != kCGErrorSuccess )
		return 0;

	for( pass = 0; pass < 2; pass++ )
	{
		for( i = 0; i < count && num < max; i++ )
		{
			CGDisplayModeRef mode;

			if( !!CGDisplayIsMain( online[ i ] ) != ( pass == 0 ) )
				continue;
			if( CGDisplayMirrorsDisplay( online[ i ] ) != kCGNullDirectDisplay )
				continue;

			mode = CGDisplayCopyDisplayMode( online[ i ] );
			if( !mode )
				continue;
			CGDisplayModeRelease( mode );

			displays[ num++ ] = online[ i ];
		}
	}

	return num;
}

/*
===============
MTLimp_ScreenForDisplay
===============
*/
static NSScreen *MTLimp_ScreenForDisplay( CGDirectDisplayID display )
{
	for( NSScreen *screen in [NSScreen screens] )
	{
		if( [screen.deviceDescription[ @"NSScreenNumber" ] unsignedIntValue] == display )
			return screen;
	}

	return [NSScreen mainScreen];
}

/*
===============
MTLimp_DisplayFrame

The display's bounds in AppKit's coordinates (CGDisplayBounds is current right after a mode change, NSScreen only
once AppKit has seen the change).
===============
*/
static NSRect MTLimp_DisplayFrame( CGDirectDisplayID display )
{
	CGRect bounds = CGDisplayBounds( display );
	CGRect main = CGDisplayBounds( CGMainDisplayID() );

	return NSMakeRect( bounds.origin.x, main.size.height - bounds.origin.y - bounds.size.height,
		bounds.size.width, bounds.size.height );
}

/*
===============
MTLimp_ModeFormat

SDL's GetDisplayModePixelFormat: 32, 30 or 16 bits per pixel, 0 for the formats SDL leaves out.
===============
*/
static int MTLimp_ModeFormat( CGDisplayModeRef mode )
{
	CFStringRef encoding;
	int format = 0;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	encoding = CGDisplayModeCopyPixelEncoding( mode );
#pragma clang diagnostic pop

	if( encoding )
	{
		if( CFStringCompare( encoding, CFSTR( IO32BitDirectPixels ), kCFCompareCaseInsensitive ) == kCFCompareEqualTo )
			format = 32;
		else if( CFStringCompare( encoding, CFSTR( kIO30BitDirectPixels ), kCFCompareCaseInsensitive ) == kCFCompareEqualTo )
			format = 30;
		else if( CFStringCompare( encoding, CFSTR( IO16BitDirectPixels ), kCFCompareCaseInsensitive ) == kCFCompareEqualTo )
			format = 16;
		CFRelease( encoding );
	}

	return format;
}

/*
===============
MTLimp_ModeUsable

SDL's HasValidDisplayModeFlags: valid and safe, neither hidden nor below graphics quality.
===============
*/
static qboolean MTLimp_ModeUsable( CGDisplayModeRef mode )
{
	uint32_t flags = CGDisplayModeGetIOFlags( mode );

	if( flags & ( kDisplayModeNeverShowFlag | kDisplayModeNotGraphicsQualityFlag ) )
		return qfalse;

	return ( flags & kDisplayModeValidFlag ) && ( flags & kDisplayModeSafeFlag );
}

/*
===============
MTLimp_ModeRefresh
===============
*/
static int MTLimp_ModeRefresh( CGDisplayModeRef mode )
{
	return (int)( CGDisplayModeGetRefreshRate( mode ) + 0.5 );
}

/*
===============
MTLimp_CopyDisplayModes

Every mode of the display, with the HiDPI ones (SDL's kCGDisplayShowDuplicateLowResolutionModes).
===============
*/
static CFArrayRef MTLimp_CopyDisplayModes( CGDirectDisplayID display )
{
	NSDictionary *options = @{ (__bridge NSString *)kCGDisplayShowDuplicateLowResolutionModes: @YES };

	return CGDisplayCopyAllDisplayModes( display, (__bridge CFDictionaryRef)options );
}

/*
===============
MTLimp_FindDisplayMode

The mode exclusive fullscreen uses for a size in points, chosen as SDL_GetClosestDisplayMode chooses: the smallest
mode at least that large in the desktop's format, at the requested refresh rate (0: the desktop's) or the lowest
above it, else the highest; a HiDPI mode before a 1x mode of the same size. NULL if there is none.
===============
*/
static CGDisplayModeRef MTLimp_FindDisplayMode( CGDirectDisplayID display, CGDisplayModeRef desktop,
	int width, int height, int refresh )
{
	CFArrayRef modes = MTLimp_CopyDisplayModes( display );
	CGDisplayModeRef best = NULL;
	int format = MTLimp_ModeFormat( desktop );
	CFIndex i, count;

	if( !refresh )
		refresh = MTLimp_ModeRefresh( desktop );

	count = modes ? CFArrayGetCount( modes ) : 0;
	for( i = -1; i < count; i++ )
	{
		CGDisplayModeRef mode = i < 0 ? desktop : (CGDisplayModeRef)CFArrayGetValueAtIndex( modes, i );
		int w = (int)CGDisplayModeGetWidth( mode ), h = (int)CGDisplayModeGetHeight( mode );
		int bw, bh, r, br;
		qboolean hidpi, bestHidpi;

		if( w < width || h < height || MTLimp_ModeFormat( mode ) != format )
			continue;
		if( i >= 0 && !MTLimp_ModeUsable( mode ) )
			continue;

		if( !best )
		{
			best = mode;
			continue;
		}

		bw = (int)CGDisplayModeGetWidth( best );
		bh = (int)CGDisplayModeGetHeight( best );
		if( w != bw || h != bh )
		{
			if( w < bw || ( w == bw && h < bh ) )
				best = mode;
			continue;
		}

		r = MTLimp_ModeRefresh( mode );
		br = MTLimp_ModeRefresh( best );
		if( r != br )
		{
			if( ( r >= refresh && ( br < refresh || r < br ) ) || ( br < refresh && r > br ) )
				best = mode;
			continue;
		}

		hidpi = CGDisplayModeGetPixelWidth( mode ) != (size_t)w;
		bestHidpi = CGDisplayModeGetPixelWidth( best ) != (size_t)bw;
		if( hidpi && !bestHidpi )
			best = mode;
	}

	if( best )
		CGDisplayModeRetain( best );
	if( modes )
		CFRelease( modes );

	return best;
}

/*
===============
MTLimp_SameMode
===============
*/
static qboolean MTLimp_SameMode( CGDisplayModeRef a, CGDisplayModeRef b )
{
	return CGDisplayModeGetWidth( a ) == CGDisplayModeGetWidth( b ) &&
		CGDisplayModeGetHeight( a ) == CGDisplayModeGetHeight( b ) &&
		CGDisplayModeGetPixelWidth( a ) == CGDisplayModeGetPixelWidth( b ) &&
		CGDisplayModeGetPixelHeight( a ) == CGDisplayModeGetPixelHeight( b ) &&
		MTLimp_ModeRefresh( a ) == MTLimp_ModeRefresh( b );
}

/*
===============
MTLimp_SetDisplayMode

Switches the display to exclusive fullscreen's mode (on) or back to the desktop's (off), faded to black to hide
the flicker. Like SDL: the displays are captured while the mode is switched, all of them when it is the main
display (otherwise macOS rearranges the windows of other applications).
===============
*/
static void MTLimp_SetDisplayMode( qboolean on )
{
	CGDisplayFadeReservationToken fade = kCGDisplayFadeReservationInvalidToken;
	CGError err;

	if( !mac_exclusiveMode || !!on == !!mac_modeSwitched )
		return;

	if( CGAcquireDisplayFadeReservation( 5, &fade ) == kCGErrorSuccess )
		CGDisplayFade( fade, 0.3f, kCGDisplayBlendNormal, kCGDisplayBlendSolidColor, 0, 0, 0, TRUE );

	if( on )
	{
		err = CGDisplayIsMain( mac_display ) ? CGCaptureAllDisplays() : CGDisplayCapture( mac_display );
		if( err == kCGErrorSuccess )
		{
			err = CGDisplaySetDisplayMode( mac_display, mac_exclusiveMode, NULL );
			if( err == kCGErrorSuccess )
				mac_modeSwitched = qtrue;
			else if( CGDisplayIsMain( mac_display ) )
				CGReleaseAllDisplays();
			else
				CGDisplayRelease( mac_display );
		}
		if( err != kCGErrorSuccess )
			ri.Printf( PRINT_WARNING, "Couldn't change the display mode (%d)\n", err );
	}
	else
	{
		CGDisplaySetDisplayMode( mac_display, mac_desktopMode, NULL );
		if( CGDisplayIsMain( mac_display ) )
			CGReleaseAllDisplays();
		else
			CGDisplayRelease( mac_display );
		mac_modeSwitched = qfalse;
	}

	if( fade != kCGDisplayFadeReservationInvalidToken )
	{
		CGDisplayFade( fade, 0.5f, kCGDisplayBlendSolidColor, kCGDisplayBlendNormal, 0, 0, 0, FALSE );
		CGReleaseDisplayFadeReservation( fade );
	}
}

/*
===============
MTLimp_EnterExclusive

Exclusive fullscreen up: the mode, the window over the screen above everything, the dock and menu bar hidden.
===============
*/
static void MTLimp_EnterExclusive( void )
{
	if( mac_exclusiveActive )
		return;

	mac_exclusiveActive = qtrue;
	MTLimp_SetDisplayMode( qtrue );
	[mac_window setFrame:MTLimp_DisplayFrame( mac_display ) display:NO];
	mac_window.level = CGShieldingWindowLevel();
	NSApp.presentationOptions = NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar;
	[mac_window makeKeyAndOrderFront:nil];
}

/*
===============
MTLimp_LeaveExclusive

The focus went elsewhere: like SDL, give the desktop its mode back and minimize the window, which would otherwise
cover everything (applicationDidBecomeActive brings it back). For a dialog the window stays, at the normal level
under the dialog (MacOS_DialogBegin).
===============
*/
static void MTLimp_LeaveExclusive( qboolean minimize )
{
	if( !mac_exclusiveActive )
		return;

	mac_exclusiveActive = qfalse;
	NSApp.presentationOptions = NSApplicationPresentationDefault;
	MTLimp_SetDisplayMode( qfalse );
	mac_window.level = NSNormalWindowLevel;
	if( minimize )
		[mac_window miniaturize:nil];
}

/*
===============
MTLimp_DestroyWindow
===============
*/
static void MTLimp_DestroyWindow( void )
{
	mbShutdown();

	if( mac_window != nil )
	{
		mac_window.delegate = nil;
		if( mac_windowMode == MAC_EXCLUSIVE || mac_windowMode == MAC_DESKTOPWINDOW )
			NSApp.presentationOptions = NSApplicationPresentationDefault;
		[mac_window orderOut:nil];
		[mac_window close];
		mac_window = nil;
		mac_view = nil;
		mac_layer = nil;
	}

	MTLimp_SetDisplayMode( qfalse );
	if( mac_exclusiveMode )
	{
		CGDisplayModeRelease( mac_exclusiveMode );
		mac_exclusiveMode = NULL;
	}
	if( mac_desktopMode )
	{
		CGDisplayModeRelease( mac_desktopMode );
		mac_desktopMode = NULL;
	}
	mac_exclusiveActive = qfalse;
	mac_windowMode = MAC_WINDOWED;
}

/*
===============
MTLimp_Shutdown
===============
*/
void MTLimp_Shutdown( void )
{
	ri.IN_Shutdown();

	@autoreleasepool
	{
		MTLimp_DestroyWindow();
	}
}

/*
===============
MTLimp_Minimize

Minimize the game so that user is back at the desktop
===============
*/
void MTLimp_Minimize( void )
{
	[mac_window miniaturize:nil];
}


/*
===============
MTLimp_LogComment
===============
*/
void MTLimp_LogComment( char *comment )
{
}

/*
=================
MTLimp_SetGamma

There is no hardware gamma: the ramp is applied in the backend's final pass.
=================
*/
void MTLimp_SetGamma( unsigned char red[256], unsigned char green[256], unsigned char blue[256] )
{
	mbSetGamma( red, green, blue );
}

/*
===============
MTLimp_CompareModes
===============
*/
static int MTLimp_CompareModes( const void *a, const void *b )
{
	const float ASPECT_EPSILON = 0.001f;
	const int *modeA = (const int *)a;
	const int *modeB = (const int *)b;
	float aspectA = (float)modeA[ 0 ] / (float)modeA[ 1 ];
	float aspectB = (float)modeB[ 0 ] / (float)modeB[ 1 ];
	int areaA = modeA[ 0 ] * modeA[ 1 ];
	int areaB = modeB[ 0 ] * modeB[ 1 ];
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
MTLimp_DetectAvailableModes

r_availableModes as SDL lists it: the sizes in points of the window display's modes in the desktop's format, each
once, the closest aspect to the display's first.
===============
*/
static void MTLimp_DetectAvailableModes(void)
{
	int i, j;
	char buf[ MAX_STRING_CHARS ] = { 0 };
	int (*modes)[ 2 ];
	int numModes = 0;
	CGDisplayModeRef desktop;
	CFArrayRef all;
	CFIndex count;
	int format;

	desktop = CGDisplayCopyDisplayMode( mac_display );
	if( !desktop )
	{
		ri.Printf( PRINT_WARNING, "Couldn't get window display mode, no resolutions detected\n" );
		return;
	}
	format = MTLimp_ModeFormat( desktop );

	all = MTLimp_CopyDisplayModes( mac_display );
	count = all ? CFArrayGetCount( all ) : 0;

	modes = calloc( (size_t)count + 1, sizeof( *modes ) );
	if ( !modes )
	{
		ri.Error( ERR_FATAL, "Out of memory" );
	}

	// the desktop's mode first (CGDisplayCopyAllDisplayModes may leave it out), then the others
	for( i = -1; i < count; i++ )
	{
		CGDisplayModeRef mode = i < 0 ? desktop : (CGDisplayModeRef)CFArrayGetValueAtIndex( all, i );
		int w = (int)CGDisplayModeGetWidth( mode ), h = (int)CGDisplayModeGetHeight( mode );

		if( !format || MTLimp_ModeFormat( mode ) != format )
			continue;
		if( i >= 0 && !MTLimp_ModeUsable( mode ) )
			continue;

		// Only list resolution once.
		for( j = 0; j < numModes; j++ )
		{
			if( w == modes[ j ][ 0 ] && h == modes[ j ][ 1 ] )
				break;
		}

		if( j != numModes )
			continue;

		modes[ numModes ][ 0 ] = w;
		modes[ numModes ][ 1 ] = h;
		numModes++;
	}

	if( all )
		CFRelease( all );
	CGDisplayModeRelease( desktop );

	if( numModes > 1 )
		qsort( modes, numModes, sizeof( *modes ), MTLimp_CompareModes );

	for( i = 0; i < numModes; i++ )
	{
		const char *newModeString = va( "%ux%u ", modes[ i ][ 0 ], modes[ i ][ 1 ] );

		if( strlen( newModeString ) < (int)sizeof( buf ) - strlen( buf ) )
			Q_strcat( buf, sizeof( buf ), newModeString );
		else
			ri.Printf( PRINT_WARNING, "Skipping mode %ux%u, buffer too small\n", modes[ i ][ 0 ], modes[ i ][ 1 ] );
	}

	if( *buf )
	{
		buf[ strlen( buf ) - 1 ] = 0;
		ri.Printf( PRINT_ALL, "Available modes: '%s'\n", buf );
		ri.Cvar_Set( "r_availableModes", buf );
	}
	free( modes );
}

/*
===============
MTLimp_SetMode
===============
*/
static int MTLimp_SetMode(int mode, qboolean fullscreen, qboolean noborder)
{
	CGDirectDisplayID displays[ 16 ];
	int numDisplays = MTLimp_GetDisplays( displays, ARRAY_LEN( displays ) );
	CGDisplayModeRef desktopMode;
	NSWindowStyleMask style;
	NSScreen *screen;
	NSRect screenFrame, contentRect;
	qboolean keepPosition = qfalse;
	NSPoint topLeft = NSZeroPoint;
	int windowWidth, windowHeight;
	int drawableWidth, drawableHeight;
	CGFloat scale;

	ri.Printf( PRINT_ALL, "Initializing Metal display\n");

	mac_display = numDisplays > 0 ? displays[ 0 ] : CGMainDisplayID();

	// If a window exists, note its display, and its place if it is a plain window
	if( mac_window != nil )
	{
		NSNumber *number = mac_window.screen.deviceDescription[ @"NSScreenNumber" ];

		if( number )
			mac_display = [number unsignedIntValue];
		if( mac_windowMode == MAC_WINDOWED )
		{
			NSRect content = [mac_window contentRectForFrameRect:mac_window.frame];

			keepPosition = qtrue;
			topLeft = NSMakePoint( NSMinX( content ), NSMaxY( content ) );
		}
	}
	else if( r_displayIndex->integer > 0 && r_displayIndex->integer < numDisplays )
	{
		mac_display = displays[ r_displayIndex->integer ];
	}

	// Destroy existing state if it exists (exclusive fullscreen gives the display its own mode back)
	if( mac_window != nil )
	{
		ri.Printf( PRINT_DEVELOPER, "Existing window at %.0fx%.0f before being destroyed\n", topLeft.x, topLeft.y );
		MTLimp_DestroyWindow();
	}

	desktopMode = CGDisplayCopyDisplayMode( mac_display );
	if( desktopMode )
	{
		displayAspect = (float)CGDisplayModeGetWidth( desktopMode ) / (float)CGDisplayModeGetHeight( desktopMode );

		ri.Printf( PRINT_ALL, "Display aspect: %.3f\n", displayAspect );
	}
	else
	{
		ri.Printf( PRINT_ALL,
				"Cannot determine display aspect, assuming 1.333\n" );
	}

	ri.Printf (PRINT_ALL, "...setting mode %d:", mode );

	// the mode is the window size in points; the drawable (glConfig) gets the pixel size below
	if (mode == -2 || ( fullscreen && noborder ))
	{
		// use desktop video resolution
		if( desktopMode )
		{
			windowWidth = (int)CGDisplayModeGetWidth( desktopMode );
			windowHeight = (int)CGDisplayModeGetHeight( desktopMode );
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
		if( desktopMode )
			CGDisplayModeRelease( desktopMode );
		return RSERR_INVALID_MODE;
	}
	glConfig.windowAspect = (float)windowWidth / (float)windowHeight;
	ri.Printf( PRINT_ALL, " %d %d\n", windowWidth, windowHeight);

	if( fullscreen )
	{
		mac_windowMode = !noborder ? MAC_EXCLUSIVE : r_borderlessWindow->integer ? MAC_DESKTOPWINDOW : MAC_SPACE;
		glConfig.isFullscreen = qtrue;
	}
	else
	{
		mac_windowMode = MAC_WINDOWED;
		glConfig.isFullscreen = qfalse;
	}

	glConfig.stereoEnabled = qfalse;

	screen = MTLimp_ScreenForDisplay( mac_display );
	screenFrame = screen.frame;
	scale = screen.backingScaleFactor;

	switch( mac_windowMode )
	{
	case MAC_EXCLUSIVE:
		glConfig.displayFrequency = ri.Cvar_VariableIntegerValue( "r_displayRefresh" );
		mac_desktopMode = desktopMode;
		desktopMode = NULL;
		mac_exclusiveMode = mac_desktopMode ? MTLimp_FindDisplayMode( mac_display, mac_desktopMode,
			windowWidth, windowHeight, glConfig.displayFrequency ) : NULL;
		if( !mac_exclusiveMode )
		{
			ri.Printf( PRINT_DEVELOPER, "No display mode for %dx%d\n", windowWidth, windowHeight );
			MTLimp_DestroyWindow();
			return RSERR_INVALID_FULLSCREEN;
		}

		// the mode changes only when it isn't the current one
		if( MTLimp_SameMode( mac_exclusiveMode, mac_desktopMode ) )
		{
			CGDisplayModeRelease( mac_exclusiveMode );
			mac_exclusiveMode = NULL;
		}
		else
		{
			ri.Printf( PRINT_ALL, "Display mode %dx%d (%dx%d pixels) at %d Hz\n",
				(int)CGDisplayModeGetWidth( mac_exclusiveMode ), (int)CGDisplayModeGetHeight( mac_exclusiveMode ),
				(int)CGDisplayModeGetPixelWidth( mac_exclusiveMode ), (int)CGDisplayModeGetPixelHeight( mac_exclusiveMode ),
				MTLimp_ModeRefresh( mac_exclusiveMode ) );
			MTLimp_SetDisplayMode( qtrue );
			scale = (CGFloat)CGDisplayModeGetPixelWidth( mac_exclusiveMode ) / CGDisplayModeGetWidth( mac_exclusiveMode );
		}

		contentRect = MTLimp_DisplayFrame( mac_display );
		style = NSWindowStyleMaskBorderless | NSWindowStyleMaskMiniaturizable;
		break;

	case MAC_DESKTOPWINDOW:
		contentRect = screenFrame;
		style = NSWindowStyleMaskBorderless | NSWindowStyleMaskMiniaturizable;
		break;

	default:
		// centered on the display (SDL), or where the old window was
		if( keepPosition )
			contentRect = NSMakeRect( topLeft.x, topLeft.y - windowHeight, windowWidth, windowHeight );
		else
			contentRect = NSMakeRect( NSMinX( screenFrame ) + (int)( NSWidth( screenFrame ) - windowWidth ) / 2,
				NSMaxY( screenFrame ) - (int)( NSHeight( screenFrame ) - windowHeight ) / 2 - windowHeight,
				windowWidth, windowHeight );

		if( mac_windowMode == MAC_SPACE )
			style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
				NSWindowStyleMaskResizable;
		else if( noborder )
			style = NSWindowStyleMaskBorderless | NSWindowStyleMaskMiniaturizable;
		else
			style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
		if( mac_windowMode == MAC_WINDOWED && r_allowResize->integer )
			style |= NSWindowStyleMaskResizable;
		break;
	}

	if( desktopMode )
		CGDisplayModeRelease( desktopMode );

	if( !mac_windowDelegate )
	{
		mac_windowDelegate = [[MacOSWindowDelegate alloc] init];
		[[NSNotificationCenter defaultCenter] addObserver:mac_windowDelegate
			selector:@selector(applicationDidBecomeActive:) name:NSApplicationDidBecomeActiveNotification object:nil];
	}

	// placed relative to its screen, as SDL places it: with global coordinates and no screen, AppKit opens the
	// window on the main display
	mac_window = [[MacOSWindow alloc] initWithContentRect:NSOffsetRect( contentRect, -NSMinX( screenFrame ),
		-NSMinY( screenFrame ) ) styleMask:style backing:NSBackingStoreBuffered defer:NO screen:screen];
	if( mac_window == nil )
	{
		ri.Printf( PRINT_ALL, "Couldn't create the window\n" );
		MTLimp_DestroyWindow();
		return RSERR_INVALID_MODE;
	}
	if( mac_windowMode == MAC_EXCLUSIVE )
	{
		// after a mode change NSScreen's frame may still be the old one
		[mac_window setFrame:contentRect display:NO];
	}

	mac_window.title = @CLIENT_WINDOW_TITLE;
	mac_window.releasedWhenClosed = NO;
	mac_window.restorable = NO;
	mac_window.tabbingMode = NSWindowTabbingModeDisallowed;
	mac_window.backgroundColor = [NSColor blackColor];
	mac_window.opaque = YES;
	mac_window.acceptsMouseMovedEvents = YES;
	mac_window.delegate = mac_windowDelegate;
	if( mac_windowMode == MAC_SPACE || ( mac_windowMode == MAC_WINDOWED && r_allowResize->integer ) )
	{
		// resizable windows get the green button's fullscreen Space, as SDL's do
		mac_window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
	}
	if( mac_windowMode != MAC_WINDOWED )
	{
		mac_window.hasShadow = NO;
	}
	if( mac_windowMode == MAC_EXCLUSIVE )
	{
		mac_window.level = CGShieldingWindowLevel();
	}

	// a layer-hosting view: the layer is set before wantsLayer, so AppKit leaves its contents alone
	mac_layer = [CAMetalLayer layer];
	mac_view = [[MacOSView alloc] initWithFrame:NSMakeRect( 0, 0, NSWidth( contentRect ), NSHeight( contentRect ) )];
	mac_view.layer = mac_layer;
	mac_view.wantsLayer = YES;
	mac_window.contentView = mac_view;
	[mac_window makeFirstResponder:mac_view];

	if( mac_windowMode != MAC_EXCLUSIVE || !mac_exclusiveMode )
		scale = mac_window.backingScaleFactor;
	mac_layer.contentsScale = scale;

	// render at the drawable's full pixel size (Retina: 2x the window size in points)
	drawableWidth = (int)lround( NSWidth( contentRect ) * scale );
	drawableHeight = (int)lround( NSHeight( contentRect ) * scale );
	glConfig.vidWidth = drawableWidth;
	glConfig.vidHeight = drawableHeight;

	mac_layer.opaque = YES;
	// Fullscreen goes direct to display, and there vsync with 2 drawables ran the game at half the refresh rate
	// (125 fps at 240 Hz: every frame waited ~7 ms for a drawable). 3 keep the full rate at the same latency.
	// Without vsync 2 are better: with 3, more presented frames are never shown and latency goes up.
	if( r_metalDrawables->integer == 2 || r_metalDrawables->integer == 3 )
		mac_layer.maximumDrawableCount = r_metalDrawables->integer;
	else
		mac_layer.maximumDrawableCount = fullscreen && r_swapInterval->integer ? 3 : 2;
	mac_layer.displaySyncEnabled = r_swapInterval->integer ? YES : NO;
	mac_layer.presentsWithTransaction = NO;
	mac_layer.drawableSize = CGSizeMake( drawableWidth, drawableHeight );

	if( !mbInit( (__bridge void *)mac_layer ) )
	{
		MTLimp_DestroyWindow();
		return RSERR_UNKNOWN;
	}

	// the backend renders into its own target; these describe what the front end gets
	glConfig.colorBits = 24;
	glConfig.depthBits = 32;
	glConfig.stencilBits = 8;

	ri.Printf( PRINT_ALL, "Using %d color bits, %d depth, %d stencil display, %dx%d pixels.\n",
			glConfig.colorBits, glConfig.depthBits, glConfig.stencilBits, glConfig.vidWidth, glConfig.vidHeight );

	switch( mac_windowMode )
	{
	case MAC_EXCLUSIVE:
		mac_exclusiveActive = qtrue;
		NSApp.presentationOptions = NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar;
		[mac_window makeKeyAndOrderFront:nil];
		break;

	case MAC_DESKTOPWINDOW:
		NSApp.presentationOptions = NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar;
		[mac_window makeKeyAndOrderFront:nil];
		break;

	case MAC_SPACE:
		[mac_window makeKeyAndOrderFront:nil];
		[mac_window toggleFullScreen:nil];
		break;

	default:
		[mac_window makeKeyAndOrderFront:nil];
		break;
	}

	MTLimp_DetectAvailableModes();

	ri.Printf( PRINT_ALL, "RENDERER: %s\n", mbRendererString() );

	return RSERR_OK;
}

/*
===============
MTLimp_StartDriverAndSetMode
===============
*/
static qboolean MTLimp_StartDriverAndSetMode(int mode, qboolean fullscreen, qboolean noborder)
{
	rserr_t err;

	MacOS_InitApp();

	if (fullscreen && ri.Cvar_VariableIntegerValue( "in_nograb" ) )
	{
		ri.Printf( PRINT_ALL, "Fullscreen not allowed with in_nograb 1\n");
		ri.Cvar_Set( "r_fullscreen", "0" );
		r_fullscreen->modified = qfalse;
		fullscreen = qfalse;
	}

	@autoreleasepool
	{
		err = MTLimp_SetMode(mode, fullscreen, noborder);
	}

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
MTLimp_InitCapabilities

What the GL1 front end reads from extensions, set the way GL1 sets them on Apple's GL driver so shader
collapsing and image uploads take the same paths.
===============
*/
static void MTLimp_InitCapabilities( void )
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
MTLimp_Init

This routine is responsible for initializing the OS specific portions
of the renderer (window, Metal layer)
===============
*/
void MTLimp_Init( qboolean fixedFunction )
{
	ri.Printf( PRINT_DEVELOPER, "MTLimp_Init( )\n" );

	r_allowResize = ri.Cvar_Get( "r_allowResize", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_centerWindow = ri.Cvar_Get( "r_centerWindow", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_displayIndex = ri.Cvar_Get( "r_displayIndex", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_metalDrawables = ri.Cvar_Get( "r_metalDrawables", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_borderlessWindow = ri.Cvar_Get( "r_borderlessWindow", "0", CVAR_ARCHIVE | CVAR_LATCH );

	if( ri.Cvar_VariableIntegerValue( "com_abnormalExit" ) )
	{
		ri.Cvar_Set( "r_mode", va( "%d", R_MODE_FALLBACK ) );
		ri.Cvar_Set( "r_fullscreen", "0" );
		ri.Cvar_Set( "r_centerWindow", "0" );
		ri.Cvar_Set( "com_abnormalExit", "0" );
	}

	// Create the window and its Metal layer
	if(MTLimp_StartDriverAndSetMode(r_mode->integer, r_fullscreen->integer, r_noborder->integer))
		goto success;

	// Try again, this time without the borderless window
	if(MTLimp_StartDriverAndSetMode(r_mode->integer, r_fullscreen->integer, qfalse))
		goto success;

	// Finally, try the default screen resolution
	if( r_mode->integer != R_MODE_FALLBACK )
	{
		ri.Printf( PRINT_ALL, "Setting r_mode %d failed, falling back on r_mode %d\n",
				r_mode->integer, R_MODE_FALLBACK );

		if(MTLimp_StartDriverAndSetMode(R_MODE_FALLBACK, qfalse, qfalse))
			goto success;
	}

	// Nothing worked, give up
	ri.Error( ERR_FATAL, "MTLimp_Init() - could not initialize Metal" );

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

	MTLimp_InitCapabilities();

	ri.Cvar_Get( "r_availableModes", "", CVAR_ROM );

	ri.IN_Init( (__bridge void *)mac_window );
}


/*
===============
MTLimp_EndFrame

Responsible for doing a swapbuffers
===============
*/
void MTLimp_EndFrame( void )
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

/*
===============
MacOS_DialogBegin

A dialog is coming (Sys_Dialog, sys_osx.m: a modal NSAlert, mostly Sys_Error's last words). It must be seen and
clicked: exclusive fullscreen's window at the shielding level and the displays captured for a mode switch would
cover it, and a captured cursor couldn't reach it. So the cursor goes back to the system (where it was parked, the
window's center) and exclusive fullscreen is left: the desktop's mode back, the displays released, the window at the
normal level under the dialog, not minimized. The alert takes the keyboard (windowDidResignKey then finds exclusive
fullscreen left already).
===============
*/
void MacOS_DialogBegin( void )
{
	if( mac_window == nil )
		return;

	@autoreleasepool
	{
		MacOS_ReleaseMouse();

		if( mac_windowMode == MAC_EXCLUSIVE )
			MTLimp_LeaveExclusive( qfalse );
	}
}

/*
===============
MacOS_DialogEnd

The dialog closed. AppKit gives the keyboard back to no window (SDL's window stayed without it too), so the game
window takes it, which brings exclusive fullscreen back (windowDidBecomeKey); IN_Frame then captures the cursor
again. Not while the game is in the background or minimized.
===============
*/
void MacOS_DialogEnd( void )
{
	if( mac_window == nil || mac_window.miniaturized || !NSApp.active )
		return;

	@autoreleasepool
	{
		[mac_window makeKeyAndOrderFront:nil];
	}
}

/*
===============
MacOS_WindowExists

in_restart needs a window (sys_main.c).
===============
*/
qboolean MacOS_WindowExists( void )
{
	return mac_window != nil;
}
