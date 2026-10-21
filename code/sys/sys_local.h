/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

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

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DEDICATED
// The native AppKit layer (code/macos)
qboolean MacOS_WindowExists( void );
char *MacOS_GetClipboardText( void );	// UTF-8, free() it; NULL if the clipboard holds no text
void MacOS_SetClipboardText( const char *text );
void MacOS_DialogBegin( void );			// Sys_Dialog: the mouse released, exclusive fullscreen left
void MacOS_DialogEnd( void );			// the game window gets the keyboard back
#endif

// Console
void CON_Shutdown( void );
void CON_Init( void );
char *CON_Input( void );
void CON_Print( const char *message );

unsigned int CON_LogSize( void );
unsigned int CON_LogWrite( const char *in );
unsigned int CON_LogRead( char *out, unsigned int outSize );

char *Sys_BinaryPath( void );
char *Sys_BinaryPathRelative( const char *relative );

#ifdef __APPLE__
char *Sys_StripAppBundle( char *pwd );
#endif

void Sys_GLimpSafeInit( void );
void Sys_GLimpInit( void );
void Sys_PlatformInit( void );
void Sys_PlatformExit( void );
void Sys_SigHandler( int signal ) Q_NO_RETURN;
void Sys_ErrorDialog( const char *error );
void Sys_AnsiColorPrint( const char *msg );

qboolean Sys_OpenFolderInPlatformFileManager( const char *path );

qboolean Sys_SetMaxFileLimit( void );

#ifdef USE_AUTOUPDATER
void Sys_LaunchAutoupdater(int argc, char **argv);
#endif

#include "new/sys_local_new.h"

#ifdef __cplusplus
}
#endif
