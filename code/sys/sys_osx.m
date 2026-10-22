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

#ifndef __APPLE__
#error This file is for Mac OS X only. You probably should not compile it.
#endif

// Please note that this file is just some Mac-specific bits. Most of the
// Mac OS X code is shared with other Unix platforms in sys_unix.c ...

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "sys_local.h"

#import <Cocoa/Cocoa.h>
#include <dlfcn.h>

/*
==============
Sys_Dialog

Display an OS X dialog box
==============
*/
dialogResult_t Sys_Dialog( dialogType_t type, const char *message, const char *title )
{
	dialogResult_t result = DR_OK;
	NSAlert *alert = [NSAlert new];

#ifndef DEDICATED
	// the native AppKit layer: neither exclusive fullscreen nor a captured cursor may hide the alert (macos_window.m)
	MacOS_DialogBegin( );
#endif

	[alert setMessageText: [NSString stringWithUTF8String: title]];
	[alert setInformativeText: [NSString stringWithUTF8String: message]];

	if( type == DT_ERROR )
		[alert setAlertStyle: NSAlertStyleCritical];
	else
		[alert setAlertStyle: NSAlertStyleWarning];

	switch( type )
	{
		default:
			[alert runModal];
			result = DR_OK;
			break;

		case DT_YES_NO:
			[alert addButtonWithTitle: @"Yes"];
			[alert addButtonWithTitle: @"No"];
			switch( [alert runModal] )
			{
				default:
				case NSAlertFirstButtonReturn: result = DR_YES; break;
				case NSAlertSecondButtonReturn: result = DR_NO; break;
			}
			break;

		case DT_OK_CANCEL:
			[alert addButtonWithTitle: @"OK"];
			[alert addButtonWithTitle: @"Cancel"];

			switch( [alert runModal] )
			{
				default:
				case NSAlertFirstButtonReturn: result = DR_OK; break;
				case NSAlertSecondButtonReturn: result = DR_CANCEL; break;
			}
			break;
	}

	[alert release];

#ifndef DEDICATED
	MacOS_DialogEnd( );
#endif

	return result;
}

/*
=================
Sys_StripAppBundle

Discovers if passed dir is suffixed with the directory structure of a Mac OS X
.app bundle. If it is, the .app directory structure is stripped off the end and
the result is returned. If not, dir is returned untouched.
=================
*/
char *Sys_StripAppBundle( char *dir )
{
	static char cwd[MAX_OSPATH];

	Q_strncpyz(cwd, dir, sizeof(cwd));
	if(strcmp(Sys_Basename(cwd), "MacOS"))
		return dir;
	Q_strncpyz(cwd, Sys_Dirname(cwd), sizeof(cwd));
	if(strcmp(Sys_Basename(cwd), "Contents"))
		return dir;
	Q_strncpyz(cwd, Sys_Dirname(cwd), sizeof(cwd));
	if(!strstr(Sys_Basename(cwd), ".app"))
		return dir;
	Q_strncpyz(cwd, Sys_Dirname(cwd), sizeof(cwd));
	return cwd;
}

#ifndef DEDICATED
/*
=================
Sys_CheckTranslocation

Metal port: "Open Anyway" in System Settings starts a download that is still quarantined from a temporary
read-only copy (App Translocation: /AppTranslocation/ in the app's path). The game folder isn't there and macOS
refuses the quarantined game libraries, so instead of "Couldn't load default.cfg" this says how to clear the
quarantine (INSTALL.txt's xattr line, with the folder that holds the app when Security finds it) and quits.
Before the filesystem starts.
=================
*/
void Sys_CheckTranslocation( void )
{
	@autoreleasepool
	{
		NSString *app = [NSBundle mainBundle].bundlePath;
		NSString *folder = nil;
		NSString *line, *how;
		CFURLRef (*originalPath)( CFURLRef, CFErrorRef * );
		NSAlert *alert;

		if( ![app containsString:@"/AppTranslocation/"] )
			return;

		// Security's SPI since macOS 10.12, not in the SDK's headers: looked up by name
		originalPath = (CFURLRef (*)( CFURLRef, CFErrorRef * ))dlsym( RTLD_DEFAULT, "SecTranslocateCreateOriginalPathForURL" );
		if( originalPath )
		{
			CFURLRef original = originalPath( (CFURLRef)[NSURL fileURLWithPath:app], NULL );

			if( original )
			{
				NSString *path = ((NSURL *)original).path;

				if( path.length && ![path containsString:@"/AppTranslocation/"] )
					folder = path.stringByDeletingLastPathComponent;
				CFRelease( original );
			}
		}

		if( folder )
		{
			// in single quotes for the shell, a quote in the path as '\''
			line = [NSString stringWithFormat:@"xattr -dr com.apple.quarantine '%@'",
				[folder stringByReplacingOccurrencesOfString:@"'" withString:@"'\\''"]];
			how = @"Clear the quarantine once: open Terminal, run the line below, then start openmohaa.app again.";
		}
		else
		{
			line = @"xattr -dr com.apple.quarantine ";
			how = @"Clear the quarantine once: open Terminal, type the line below followed by a space, drag the folder "
				@"that holds openmohaa.app onto the Terminal window, press Return, then start openmohaa.app again.";
		}

		fprintf( stderr, "Started from App Translocation's copy (%s): the download is still quarantined.\n%s\n",
			app.UTF8String, line.UTF8String );

		[NSApplication sharedApplication];
		[NSApp activateIgnoringOtherApps:YES];

		alert = [[NSAlert alloc] init];
		alert.alertStyle = NSAlertStyleCritical;
		alert.messageText = @"The download is still quarantined";
		alert.informativeText = [NSString stringWithFormat:@"macOS started openmohaa.app from a temporary read-only copy, "
			@"because the downloaded files are still quarantined (\"Open Anyway\" isn't enough for this game). From there "
			@"the game can't find your game data, and macOS refuses its game libraries.\n\n%@\n\n%@", how, line];
		[alert addButtonWithTitle:@"Copy Line and Quit"];
		[alert addButtonWithTitle:@"Quit"];

		if( [alert runModal] == NSAlertFirstButtonReturn )
		{
			[[NSPasteboard generalPasteboard] clearContents];
			[[NSPasteboard generalPasteboard] setString:line forType:NSPasteboardTypeString];
		}

		[alert release];
	}

	exit( 1 );
}
#endif
