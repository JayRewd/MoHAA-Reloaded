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
// macos_input.m -- input, native (USE_SDL off): the IN_* functions of sdl/sdl_input.c and sdl/sdl_mouse.c.
// So far the event pump and the window's events (focus, minimize, resize, quit); the keyboard, text, the mouse and
// gamepads come in the next slices of Phase 6 (docs/METAL_PORT_PLAN.md).

#include "macos_local.h"
#include "../client/client.h"
#include "../sys/sys_local.h"

static NSWindow *in_window;
static NSArray *in_windowObservers;

static int vidRestartTime = 0;

/*
===============
IN_WindowResized

A window's size or backing scale changed: rebuild at the new size once the user stops dragging. glConfig holds
the drawable's pixels, r_customwidth/height the window's points (as with SDL).
===============
*/
static void IN_WindowResized( void )
{
	NSSize size = in_window.contentView.bounds.size;
	CGFloat scale = in_window.backingScaleFactor;
	int width = (int)size.width, height = (int)size.height;

	// ignore this event on fullscreen
	if( cls.glconfig.isFullscreen )
	{
		return;
	}

	// check if size actually changed
	if( cls.glconfig.vidWidth == (int)lround( width * scale ) && cls.glconfig.vidHeight == (int)lround( height * scale ) )
	{
		return;
	}

	Cvar_SetValue( "r_customwidth", width );
	Cvar_SetValue( "r_customheight", height );
	Cvar_Set( "r_mode", "-1" );

	// Wait until user stops dragging for 1 second, so
	// we aren't constantly recreating the GL context while
	// he tries to drag...
	vidRestartTime = Sys_Milliseconds( ) + 1000;
}

/*
===============
IN_ObserveWindow

The window's events reach the input layer as SDL's window events did: during the pump, on the main thread.
===============
*/
static void IN_ObserveWindow( void )
{
	NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
	NSOperationQueue *queue = nil;	// run on the posting thread, inside the pump

	in_windowObservers = @[
		[center addObserverForName:NSWindowDidBecomeKeyNotification object:in_window queue:queue
			usingBlock:^( NSNotification *n ) { Cvar_SetValue( "com_unfocused", 0 ); }],
		[center addObserverForName:NSWindowDidResignKeyNotification object:in_window queue:queue
			usingBlock:^( NSNotification *n ) { Cvar_SetValue( "com_unfocused", 1 ); }],
		[center addObserverForName:NSWindowDidMiniaturizeNotification object:in_window queue:queue
			usingBlock:^( NSNotification *n ) { Cvar_SetValue( "com_minimized", 1 ); }],
		[center addObserverForName:NSWindowDidDeminiaturizeNotification object:in_window queue:queue
			usingBlock:^( NSNotification *n ) { Cvar_SetValue( "com_minimized", 0 ); }],
		[center addObserverForName:NSWindowDidResizeNotification object:in_window queue:queue
			usingBlock:^( NSNotification *n ) { IN_WindowResized(); }],
		[center addObserverForName:NSWindowDidChangeBackingPropertiesNotification object:in_window queue:queue
			usingBlock:^( NSNotification *n ) { IN_WindowResized(); }],
	];
}

/*
===============
IN_ProcessEvents
===============
*/
static void IN_ProcessEvents( void )
{
	if( !MacOS_AppStarted( ) )
		return;

	CL_LatencyPump( );

	MacOS_PumpEvents( );

	if( MacOS_TakeQuitRequest( ) )
		Cbuf_ExecuteText( EXEC_NOW, "quit Closed window\n" );
}

/*
===============
IN_Frame
===============
*/
void IN_Frame( void )
{
	// update isFullscreen since it might of changed since the last vid_restart
	cls.glconfig.isFullscreen = Cvar_VariableIntegerValue( "r_fullscreen" ) != 0;

	IN_ProcessEvents( );

	// In case we had to delay actual restart of video system
	if( ( vidRestartTime != 0 ) && ( vidRestartTime < Sys_Milliseconds( ) ) )
	{
		vidRestartTime = 0;
		Cbuf_AddText( "vid_restart\n" );
	}
}

/*
===============
IN_Init
===============
*/
void IN_Init( void *windowData )
{
	if( !MacOS_AppStarted( ) )
	{
		Com_Error( ERR_FATAL, "IN_Init called before the window exists" );
		return;
	}

	in_window = (__bridge NSWindow *)windowData;

	Com_DPrintf( "\n------- Input Initialization -------\n" );

	// registered as SDL registers them, so configs keep them (used from slice 6c on)
	Cvar_Get( "in_keyboardDebug", "0", CVAR_ARCHIVE );

	// mouse variables
	Cvar_Get( "in_mouse", "1", CVAR_ARCHIVE );
	Cvar_Get( "in_nograb", "0", CVAR_ARCHIVE );

	Cvar_Get( "in_joystick", "0", CVAR_ARCHIVE|CVAR_LATCH );
	Cvar_Get( "joy_threshold", "0.15", CVAR_ARCHIVE );

	IN_ObserveWindow( );

	Cvar_SetValue( "com_unfocused",	!in_window.keyWindow );
	Cvar_SetValue( "com_minimized", in_window.miniaturized );

	Com_DPrintf( "------------------------------------\n" );
}

/*
===============
IN_Shutdown
===============
*/
void IN_Shutdown( void )
{
	for( id observer in in_windowObservers )
		[[NSNotificationCenter defaultCenter] removeObserver:observer];
	in_windowObservers = nil;

	in_window = nil;
}

/*
===============
IN_Restart
===============
*/
void IN_Restart( void )
{
	NSWindow *window = in_window;

	IN_Shutdown( );
	IN_Init( (__bridge void *)window );
}

/*
===============
IN_GetMousePosition (the mouse: slice 6d)
===============
*/
void IN_GetMousePosition( int *x, int *y )
{
	*x = *y = 0;
}

/*
===============
IN_SetCursorFromImage (the color cursor: slice 6d)
===============
*/
qboolean IN_SetCursorFromImage( const byte *pic, int width, int height, pCursorFree cursorFreeFn )
{
	cursorFreeFn( (byte *)pic );
	return qfalse;
}

/*
===============
IN_FreeCursor
===============
*/
void IN_FreeCursor( void )
{
}

/*
===============
IN_IsCursorActive
===============
*/
qboolean IN_IsCursorActive( void )
{
	return qfalse;
}
