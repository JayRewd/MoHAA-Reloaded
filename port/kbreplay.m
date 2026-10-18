// kbreplay.m -- SCRATCH ONLY, never in a shipped flavor: port/kbreplay.sh copies it into a scratch tree
// (port/kbreplay_patch.py) built twice, with SDL and with the native AppKit layer (Phase 6c).
//
// The console command `kbreplay <name>` posts a fixed sequence of key events into the game's own event queue
// ([NSApp postEvent:atStart:], nothing leaves the process), one step per few frames, right before the input layer
// pumps (KbReplay_Frame, called from IN_ProcessEvents), and logs every SE_KEY and SE_CHAR the input layer queues
// (KbReplay_LogEvent, called from Com_QueueEvent) into <fs_homepath>/kbreplay-<name>.log. Both input layers must
// log the same stream. The events carry what a real keyboard's would: the device-dependent modifier bits, the
// characters of the current layout (UCKeyTranslate, with dead keys on key-down), isARepeat.

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>

#include "../client/client.h"

void KbReplay_Frame( void );
void KbReplay_LogEvent( int type, int value, int value2 );
void KbReplay_Shutdown( void );

enum { KB_END, KB_STEP, KB_DOWN, KB_UP, KB_REPEAT, KB_MOD, KB_CAPS, KB_FOCUS_AWAY, KB_FOCUS_BACK };

typedef struct
{
	int			op;
	int			vk;
	const char	*note;
} kbAction_t;

#define STEP( note )	{ KB_STEP, 0, note }
#define DOWN( vk )		{ KB_DOWN, vk, NULL }
#define UP( vk )		{ KB_UP, vk, NULL }
#define REP( vk )		{ KB_REPEAT, vk, NULL }
#define TAP( vk )		DOWN( vk ), UP( vk )
#define MOD( vk )		{ KB_MOD, vk, NULL }		// a modifier key changes: down if it was up, up if it was down
#define CAPS			{ KB_CAPS, kVK_CapsLock, NULL }	// caps lock toggles

static const kbAction_t kb_script[] =
{
	// in the game: no key catcher
	STEP( "game: letter" ), TAP( kVK_ANSI_W ),
	STEP( "game: hold" ), DOWN( kVK_ANSI_W ),
	STEP( "game: repeats (dropped without a catcher)" ), REP( kVK_ANSI_W ), REP( kVK_ANSI_W ), REP( kVK_ANSI_W ),
	STEP( "game: release" ), UP( kVK_ANSI_W ),
	STEP( "game: number row" ), TAP( kVK_ANSI_1 ), TAP( kVK_ANSI_5 ), TAP( kVK_ANSI_0 ),
	STEP( "game: arrows" ), TAP( kVK_LeftArrow ), TAP( kVK_RightArrow ), TAP( kVK_UpArrow ), TAP( kVK_DownArrow ),
	STEP( "game: keypad" ), TAP( kVK_ANSI_Keypad8 ), TAP( kVK_ANSI_Keypad5 ), TAP( kVK_ANSI_Keypad0 ),
		TAP( kVK_ANSI_KeypadDecimal ), TAP( kVK_ANSI_KeypadPlus ), TAP( kVK_ANSI_KeypadMultiply ),
		TAP( kVK_ANSI_KeypadEnter ), TAP( kVK_ANSI_KeypadEquals ),
	STEP( "game: left shift down" ), MOD( kVK_Shift ),
	STEP( "game: right shift down (left repeats)" ), MOD( kVK_RightShift ),
	STEP( "game: left shift up" ), MOD( kVK_Shift ),
	STEP( "game: right shift up" ), MOD( kVK_RightShift ),
	STEP( "game: left control" ), MOD( kVK_Control ),
	STEP( "game: left control up" ), MOD( kVK_Control ),
	STEP( "game: right control" ), MOD( kVK_RightControl ),
	STEP( "game: right control up" ), MOD( kVK_RightControl ),
	STEP( "game: left option" ), MOD( kVK_Option ),
	STEP( "game: left option up" ), MOD( kVK_Option ),
	STEP( "game: right option" ), MOD( kVK_RightOption ),
	STEP( "game: right option up" ), MOD( kVK_RightOption ),
	STEP( "game: left command" ), MOD( kVK_Command ),
	STEP( "game: left command up" ), MOD( kVK_Command ),
	STEP( "game: right command" ), MOD( kVK_RightCommand ),
	STEP( "game: right command up" ), MOD( kVK_RightCommand ),
	STEP( "game: caps lock on" ), CAPS,
	STEP( "game: caps lock off" ), CAPS,
	STEP( "game: control down" ), MOD( kVK_Control ),
	STEP( "game: control+a" ), TAP( kVK_ANSI_A ),
	STEP( "game: control up" ), MOD( kVK_Control ),
	STEP( "game: hold w and left shift" ), DOWN( kVK_ANSI_W ), MOD( kVK_Shift ),
	STEP( "focus away (another window of the game takes the keyboard)" ), { KB_FOCUS_AWAY },
	STEP( "focus back" ), { KB_FOCUS_BACK },
	STEP( "game: release w and left shift (no longer down)" ), UP( kVK_ANSI_W ), MOD( kVK_Shift ),

	// the console
	STEP( "console key" ), TAP( kVK_ANSI_Grave ),
	STEP( "console: text" ), TAP( kVK_ANSI_E ), TAP( kVK_ANSI_C ), TAP( kVK_ANSI_H ), TAP( kVK_ANSI_O ),
		TAP( kVK_Space ), TAP( kVK_ANSI_H ), TAP( kVK_ANSI_I ),
	STEP( "console: shift down" ), MOD( kVK_Shift ),
	STEP( "console: shift+x" ), TAP( kVK_ANSI_X ),
	STEP( "console: shift up" ), MOD( kVK_Shift ),
	STEP( "console: caps lock on" ), CAPS,
	STEP( "console: caps+y" ), TAP( kVK_ANSI_Y ),
	STEP( "console: caps lock off" ), CAPS,
	STEP( "console: hold k" ), DOWN( kVK_ANSI_K ),
	STEP( "console: repeats (kept with a catcher)" ), REP( kVK_ANSI_K ), REP( kVK_ANSI_K ), REP( kVK_ANSI_K ),
	STEP( "console: release k" ), UP( kVK_ANSI_K ),
	STEP( "console: backspace" ), TAP( kVK_Delete ), TAP( kVK_Delete ),
	STEP( "console: tab" ), TAP( kVK_Tab ),
	STEP( "console: control down" ), MOD( kVK_Control ),
	STEP( "console: control+e" ), TAP( kVK_ANSI_E ),
	STEP( "console: control up" ), MOD( kVK_Control ),
	STEP( "console: option down" ), MOD( kVK_Option ),
	STEP( "console: option+e (dead key)" ), TAP( kVK_ANSI_E ),
	STEP( "console: option up" ), MOD( kVK_Option ),
	STEP( "console: e after the dead key" ), TAP( kVK_ANSI_E ),
	STEP( "console: digits and punctuation" ), TAP( kVK_ANSI_2 ), TAP( kVK_ANSI_Minus ), TAP( kVK_ANSI_Equal ),
		TAP( kVK_ANSI_LeftBracket ), TAP( kVK_ANSI_RightBracket ), TAP( kVK_ANSI_Quote ), TAP( kVK_ANSI_Comma ),
		TAP( kVK_ANSI_Period ), TAP( kVK_ANSI_Slash ), TAP( kVK_ANSI_Backslash ), TAP( kVK_ANSI_Keypad7 ),
	STEP( "console: function keys" ), TAP( kVK_F1 ), TAP( kVK_F5 ), TAP( kVK_F12 ), TAP( kVK_F13 ), TAP( kVK_F16 ),
	STEP( "console: editing keys" ), TAP( kVK_Home ), TAP( kVK_End ), TAP( kVK_PageUp ), TAP( kVK_PageDown ),
		TAP( kVK_LeftArrow ), TAP( kVK_RightArrow ), TAP( kVK_ForwardDelete ), TAP( kVK_Help ),
	STEP( "console: enter" ), TAP( kVK_Return ),
	STEP( "console: shift down" ), MOD( kVK_Shift ),
	STEP( "console: shift+grave (~) closes the console" ), TAP( kVK_ANSI_Grave ),
	STEP( "console: shift up" ), MOD( kVK_Shift ),

	// the menu
	STEP( "game: escape" ), TAP( kVK_Escape ),
	STEP( "menu: escape" ), TAP( kVK_Escape ),

	// Cmd+Q must reach the menu and quit
	STEP( "command down" ), MOD( kVK_Command ),
	STEP( "command+q" ), DOWN( kVK_ANSI_Q ),

	{ KB_END }
};

@interface KbFocusWindow : NSWindow
@end

@implementation KbFocusWindow
- (BOOL)canBecomeKeyWindow
{
	return YES;
}
@end

static FILE				*kb_log;
static int				kb_pos = -1;			// the next action, -1 = idle
static int				kb_wait;				// frames until the next step
static int				kb_stepNum;
static int				kb_afterEnd;			// frames since the last step
static int				kb_startWait;			// frames waited for the game before the first step
static qboolean			kb_down[ 128 ];			// the modifiers the replayed keyboard holds
static qboolean			kb_capsLock;
static UInt32			kb_deadKeyState;
static NSWindow			*kb_gameWindow;
static KbFocusWindow	*kb_focusWindow;

/*
===============
KbFlags

NSEvent.modifierFlags as a real keyboard sets them: the device-dependent side bits and the independent ones.
===============
*/
static NSUInteger KbFlags( void )
{
	NSUInteger flags = 0;

	if( kb_down[ kVK_Shift ] )			flags |= 0x0002 | NSEventModifierFlagShift;
	if( kb_down[ kVK_RightShift ] )		flags |= 0x0004 | NSEventModifierFlagShift;
	if( kb_down[ kVK_Control ] )		flags |= 0x0001 | NSEventModifierFlagControl;
	if( kb_down[ kVK_RightControl ] )	flags |= 0x2000 | NSEventModifierFlagControl;
	if( kb_down[ kVK_Option ] )			flags |= 0x0020 | NSEventModifierFlagOption;
	if( kb_down[ kVK_RightOption ] )	flags |= 0x0040 | NSEventModifierFlagOption;
	if( kb_down[ kVK_Command ] )		flags |= 0x0008 | NSEventModifierFlagCommand;
	if( kb_down[ kVK_RightCommand ] )	flags |= 0x0010 | NSEventModifierFlagCommand;
	if( kb_capsLock )					flags |= NSEventModifierFlagCapsLock;

	return flags;
}

/*
===============
KbCarbonModifiers

The modifier state UCKeyTranslate takes.
===============
*/
static UInt32 KbCarbonModifiers( NSUInteger flags )
{
	UInt32 modifiers = 0;

	if( flags & NSEventModifierFlagShift )		modifiers |= shiftKey;
	if( flags & NSEventModifierFlagControl )	modifiers |= controlKey;
	if( flags & NSEventModifierFlagOption )		modifiers |= optionKey;
	if( flags & NSEventModifierFlagCommand )	modifiers |= cmdKey;
	if( flags & NSEventModifierFlagCapsLock )	modifiers |= alphaLock;

	return ( modifiers >> 8 ) & 0xFF;
}

/*
===============
KbCharacters
===============
*/
static NSString *KbCharacters( int vk, UInt16 action, UInt32 modifiers, UInt32 *deadKeyState )
{
	TISInputSourceRef source = TISCopyCurrentKeyboardLayoutInputSource( );
	CFDataRef uchr = TISGetInputSourceProperty( source, kTISPropertyUnicodeKeyLayoutData );
	UniChar chars[ 8 ];
	UniCharCount length = 0;
	UInt32 noDeadKeys = 0;

	UCKeyTranslate( (const UCKeyboardLayout *)CFDataGetBytePtr( uchr ), vk, action, modifiers, LMGetKbdType( ),
		deadKeyState ? 0 : kUCKeyTranslateNoDeadKeysMask, deadKeyState ? deadKeyState : &noDeadKeys,
		ARRAY_LEN( chars ), &length, chars );
	CFRelease( source );

	return [NSString stringWithCharacters:chars length:length];
}

/*
===============
KbPostKey
===============
*/
static void KbPostKey( NSEventType type, int vk, BOOL repeat )
{
	NSUInteger flags = KbFlags( );
	UInt32 modifiers = KbCarbonModifiers( flags );
	NSString *characters, *ignoringModifiers;
	NSEvent *event;

	if( type == NSEventTypeKeyDown )
		characters = KbCharacters( vk, repeat ? kUCKeyActionAutoKey : kUCKeyActionDown, modifiers, &kb_deadKeyState );
	else
		characters = KbCharacters( vk, kUCKeyActionUp, modifiers, NULL );
	ignoringModifiers = KbCharacters( vk, kUCKeyActionDown, KbCarbonModifiers( flags & NSEventModifierFlagShift ), NULL );

	event = [NSEvent keyEventWithType:type location:NSZeroPoint modifierFlags:flags
		timestamp:[NSProcessInfo processInfo].systemUptime windowNumber:kb_gameWindow.windowNumber context:nil
		characters:characters charactersIgnoringModifiers:ignoringModifiers isARepeat:repeat keyCode:vk];
	[NSApp postEvent:event atStart:NO];
}

/*
===============
KbPostFlags
===============
*/
static void KbPostFlags( int vk )
{
	NSEvent *event = [NSEvent keyEventWithType:NSEventTypeFlagsChanged location:NSZeroPoint modifierFlags:KbFlags( )
		timestamp:[NSProcessInfo processInfo].systemUptime windowNumber:kb_gameWindow.windowNumber context:nil
		characters:@"" charactersIgnoringModifiers:@"" isARepeat:NO keyCode:vk];

	[NSApp postEvent:event atStart:NO];
}

/*
===============
KbReplay_LogEvent
===============
*/
void KbReplay_LogEvent( int type, int value, int value2 )
{
	if( !kb_log )
		return;

	if( type == SE_KEY )
		fprintf( kb_log, "  SE_KEY %s %s\n", Key_KeynumToString( value ), value2 ? "down" : "up" );
	else if( type == SE_CHAR )
		fprintf( kb_log, "  SE_CHAR 0x%x\n", value );
	else
		return;

	fflush( kb_log );
}

/*
===============
KbReplay_Shutdown

The input layer shuts down (vid_restart or quit): what it queues from here on is the window going away.
===============
*/
void KbReplay_Shutdown( void )
{
	if( !kb_log )
		return;

	fprintf( kb_log, "input shutdown\n" );
	fflush( kb_log );
}

/*
===============
KbReplay_f
===============
*/
static void KbReplay_f( void )
{
	char path[ MAX_OSPATH ];

	if( Cmd_Argc( ) != 2 )
	{
		Com_Printf( "kbreplay <name>: posts the key replay, logs to <fs_homepath>/kbreplay-<name>.log\n" );
		return;
	}

	Com_sprintf( path, sizeof( path ), "%s/kbreplay-%s.log", Cvar_VariableString( "fs_homepath" ), Cmd_Argv( 1 ) );
	kb_log = fopen( path, "w" );
	if( !kb_log )
	{
		Com_Printf( "kbreplay: can't write %s\n", path );
		return;
	}

	kb_gameWindow = NSApp.keyWindow ?: NSApp.mainWindow;
	fprintf( kb_log, "kbreplay %s: layout %s, keyboard type %d, game window key %d\n", Cmd_Argv( 1 ),
		[(__bridge NSString *)TISGetInputSourceProperty( TISCopyCurrentKeyboardLayoutInputSource( ),
		kTISPropertyInputSourceID ) UTF8String], LMGetKbdType( ), kb_gameWindow.keyWindow );
	fflush( kb_log );

	kb_pos = 0;
	kb_wait = 0;
	kb_stepNum = 0;
}

/*
===============
KbReplay_Frame

Called right before the input layer pumps its events.
===============
*/
void KbReplay_Frame( void )
{
	static qboolean registered;
	const kbAction_t *a;

	if( !registered )
	{
		Cmd_AddCommand( "kbreplay", KbReplay_f );
		registered = qtrue;
	}

	if( kb_pos < 0 )
		return;

	if( kb_wait > 0 )
	{
		kb_wait--;
		return;
	}

	// the first step waits for the game: in the map, no menu or loading screen up (at most ~10 s)
	if( kb_pos == 0 && ( clc.state != CA_ACTIVE || Key_GetCatcher( ) != 0 ) && kb_startWait < 2500 )
	{
		if( kb_startWait++ % 250 == 0 )
		{
			fprintf( kb_log, "waiting for the game: state %d, catcher 0x%x\n", clc.state, Key_GetCatcher( ) );
			fflush( kb_log );
		}
		return;
	}

	a = &kb_script[ kb_pos ];
	if( a->op == KB_END )
	{
		// Cmd+Q should have quit by now
		if( ++kb_afterEnd == 100 )
		{
			fprintf( kb_log, "still running 100 frames after Cmd+Q: quitting\n" );
			fflush( kb_log );
			Cbuf_AddText( "quit\n" );
		}
		return;
	}

	// a step: its note, the game's state, then its events
	fprintf( kb_log, "step %d: %s (catcher 0x%x, ctrl %d, window key %d)\n", ++kb_stepNum, a->note,
		Key_GetCatcher( ), keys[ K_CTRL ].down, kb_gameWindow.keyWindow );
	fflush( kb_log );
	kb_wait = 3;

	for( a++, kb_pos++; a->op != KB_STEP && a->op != KB_END; a++, kb_pos++ )
	{
		switch( a->op )
		{
			case KB_DOWN:
				KbPostKey( NSEventTypeKeyDown, a->vk, NO );
				break;

			case KB_REPEAT:
				KbPostKey( NSEventTypeKeyDown, a->vk, YES );
				break;

			case KB_UP:
				KbPostKey( NSEventTypeKeyUp, a->vk, NO );
				break;

			case KB_MOD:
				kb_down[ a->vk ] = !kb_down[ a->vk ];
				KbPostFlags( a->vk );
				break;

			case KB_CAPS:
				kb_capsLock = !kb_capsLock;
				KbPostFlags( a->vk );
				break;

			case KB_FOCUS_AWAY:
				kb_focusWindow = [[KbFocusWindow alloc] initWithContentRect:NSMakeRect( 0, 0, 16, 16 )
					styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
				kb_focusWindow.releasedWhenClosed = NO;
				[kb_focusWindow makeKeyAndOrderFront:nil];
				kb_wait = 30;
				break;

			case KB_FOCUS_BACK:
				[kb_focusWindow orderOut:nil];
				[kb_gameWindow makeKeyAndOrderFront:nil];
				kb_focusWindow = nil;
				kb_wait = 30;
				break;
		}
	}
}
