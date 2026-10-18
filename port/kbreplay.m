// kbreplay.m -- SCRATCH ONLY, never in a shipped flavor: port/kbreplay.sh copies it into a scratch tree
// (port/kbreplay_patch.py) built twice, with SDL and with the native AppKit layer (Phase 6c).
//
// The console command `kbreplay <name>` posts a fixed sequence of key events into the game's own event queue
// ([NSApp postEvent:atStart:], nothing leaves the process), one step per few frames, right before the input layer
// pumps (KbReplay_Frame, called from IN_ProcessEvents), and logs every SE_KEY and SE_CHAR the input layer queues
// (KbReplay_LogEvent, called from Com_QueueEvent) into <fs_homepath>/kbreplay-<name>.log. Both input layers must
// log the same stream. The events carry what a real keyboard's would: the device-dependent modifier bits, the
// characters of the current layout (UCKeyTranslate, with dead keys on key-down), isARepeat.
//
// `msreplay <name>` does the same with the mouse (Phase 6d): movement events (whole deltas: posted events can't
// carry fractions), buttons, wheel notches and fine scrolling, built as CGEvents (CGEventCreateMouseEvent and
// friends) on a mouse event AppKit made for the game window (MsPost) and posted as NSEvents ([NSEvent
// eventWithCGEvent:], [NSApp postEvent:atStart:]; never CGEventPost), around the capture changes (the menu, the
// console), and logs SE_MOUSE too, with the UI's cursor position at every step where it is used (released, or a
// captured menu). A capture still takes the real cursor (warped to the window's center and hidden).
//
// `dlgtest <name>` checks Sys_Dialog with the game up (Phase 6e): once the game has the mouse it opens an OK/Cancel
// dialog through Sys_Dialog, logs what the screen shows meanwhile (the dialog's window and every on-screen window in
// front of it that covers it, the cursor, the display mode and capture), dismisses it from a timer (Cancel), then
// logs the game's state for a few frames and quits. With OMM_DLGTEST=<log path> in the environment, a timer installed
// before main does the same for a dialog without a window: the startup's "Abnormal Exit" question (No).
//
// `urltest <name>` (built with PROTOCOL_HANDLER only: the kburl flavor) checks the openmohaa:// handler: it replaces
// the connect command with one that only logs its arguments, sends the game's own process kAEGetURL Apple events
// with good and bad links (nothing leaves the process, nothing connects), logs what reached connect and quits.

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
	else if( type == SE_MOUSE )
		fprintf( kb_log, "  SE_MOUSE %d %d\n", value, value2 );
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
==============================================================================

THE MOUSE REPLAY (msreplay)

==============================================================================
*/

enum { MS_END, MS_STEP, MS_MOVE, MS_AT, MS_DOWN, MS_UP, MS_DRAG, MS_WHEEL, MS_WHEELPX, MS_KEY, MS_MOD, MS_KEYDOWN };

typedef struct
{
	int			op;
	double		a, b, c;
	const char	*note;
} msAction_t;

#define MSTEP( note )		{ MS_STEP, 0, 0, 0, note }
#define MOVE( dx, dy )		{ MS_MOVE, dx, dy, 0, NULL }	// a movement event's deltas, whole (a released cursor moves with them)
#define AT( x, y )			{ MS_AT, x, y, 0, NULL }		// the cursor moves to x, y over the window: points from its top left
#define BDOWN( b )			{ MS_DOWN, b, 0, 0, NULL }		// AppKit's button numbers: 0 left, 1 right, 2 middle
#define BUP( b )			{ MS_UP, b, 0, 0, NULL }
#define CLICK( b )			BDOWN( b ), BUP( b )
#define DRAG( b, dx, dy )	{ MS_DRAG, b, dx, dy, NULL }	// a movement with button b down
#define WHEEL( n )			{ MS_WHEEL, n, 0, 0, NULL }		// wheel notches (lines), up > 0
#define WHEELPX( n )		{ MS_WHEELPX, n, 0, 0, NULL }	// fine scrolling (pixels, a trackpad's)
#define KEY( vk )			{ MS_KEY, vk, 0, 0, NULL }		// a key press and release
#define KMOD( vk )			{ MS_MOD, vk, 0, 0, NULL }		// a modifier key toggles
#define KDOWN( vk )			{ MS_KEYDOWN, vk, 0, 0, NULL }

static const msAction_t ms_script[] =
{
	// in the game: captured
	MSTEP( "game: the first movement after the capture (dropped)" ), MOVE( 5, 3 ),
	MSTEP( "game: whole moves" ), MOVE( 5, 3 ), MOVE( -7, 2 ), MOVE( 0, -4 ),
	MSTEP( "game: small moves" ), MOVE( 1, 0 ), MOVE( 0, -1 ), MOVE( -1, 1 ),
	MSTEP( "game: no movement" ), MOVE( 0, 0 ),
	MSTEP( "game: fast moves" ), MOVE( 120, -80 ), MOVE( 250, 33 ),
	MSTEP( "game: left button" ), CLICK( 0 ),
	MSTEP( "game: right button" ), CLICK( 1 ),
	MSTEP( "game: middle button" ), CLICK( 2 ),
	MSTEP( "game: buttons 4, 5, 6 and 8" ), CLICK( 3 ), CLICK( 4 ), CLICK( 5 ), CLICK( 7 ),
	MSTEP( "game: left down twice, up twice" ), BDOWN( 0 ), BDOWN( 0 ), BUP( 0 ), BUP( 0 ),
	MSTEP( "game: a drag with the left button" ), BDOWN( 0 ), DRAG( 0, 3, 4 ), DRAG( 0, -2, 1 ), BUP( 0 ),
	MSTEP( "game: a drag with the right button" ), BDOWN( 1 ), DRAG( 1, -6, 1 ), BUP( 1 ),
	MSTEP( "game: moves between button events" ), BDOWN( 2 ), MOVE( 2, 2 ), BUP( 2 ), MOVE( -2, -2 ),
	MSTEP( "game: wheel notches" ), WHEEL( 1 ), WHEEL( -1 ), WHEEL( 3 ), WHEEL( -2 ),
	MSTEP( "game: fine scrolling adds up" ), WHEELPX( 4 ), WHEELPX( 4 ), WHEELPX( 4 ), WHEELPX( 12 ),
	MSTEP( "game: fine scrolling changes direction" ), WHEELPX( 6 ), WHEELPX( -6 ), WHEELPX( -6 ),

	// the menu: released in a window (the cursor at the window's center), captured in fullscreen
	MSTEP( "game: escape opens the menu" ), KEY( kVK_Escape ),
	MSTEP( "menu: movement events" ), MOVE( 10, 5 ), MOVE( -3, 2 ),
	MSTEP( "menu: the cursor over the window" ), AT( 100, 100 ), AT( 101.7, 50.2 ),
	MSTEP( "menu: near the bottom right corner" ), AT( 1279.5, 719.5 ),
	MSTEP( "menu: leaving the window on the right" ), AT( 1290, 700 ), AT( 1300, 600 ),
	MSTEP( "menu: back over the window" ), AT( 1200, 650 ),
	MSTEP( "menu: leaving at the top left" ), AT( -0.5, -0.5 ), AT( -10, 20 ),
	MSTEP( "menu: back near the corner" ), AT( 20, 20 ),
	MSTEP( "menu: a click in the corner (none in a captured menu)" ), CLICK( 0 ),
	MSTEP( "menu: a right click" ), CLICK( 1 ),
	MSTEP( "menu: a drag out of the window and back" ), BDOWN( 0 ), DRAG( 0, -30, 2 ), DRAG( 0, 20, 3 ), BUP( 0 ),
	MSTEP( "menu: wheel" ), WHEEL( 1 ), WHEEL( -1 ),
	MSTEP( "menu: escape back to the game" ), KEY( kVK_Escape ),
	MSTEP( "game: captured again: the first movement is dropped" ), MOVE( 3, 3 ), MOVE( 2, -2 ),

	// the console: released in a window
	MSTEP( "game: the console key" ), KEY( kVK_ANSI_Grave ),
	MSTEP( "console: movement" ), MOVE( 5, 5 ), AT( 300, 200 ),
	MSTEP( "console: wheel" ), WHEEL( -1 ),
	MSTEP( "console: shift down" ), KMOD( kVK_Shift ),
	MSTEP( "console: shift+grave (~) closes it" ), KEY( kVK_ANSI_Grave ),
	MSTEP( "console: shift up" ), KMOD( kVK_Shift ),
	MSTEP( "game: captured again" ), MOVE( 1, 1 ), MOVE( -4, 4 ),

	// Cmd+Q with the left button held
	MSTEP( "game: left button down" ), BDOWN( 0 ),
	MSTEP( "command down" ), KMOD( kVK_Command ),
	MSTEP( "command+q" ), KDOWN( kVK_ANSI_Q ),

	{ MS_END }
};

static int			ms_pos = -1;		// the next action, -1 = idle
static double		ms_x, ms_y;			// the replayed cursor over the window: points from its top left
static int			ms_captured = -1;	// the capture at the last step
static qboolean		ms_skipButtons;		// a captured menu (fullscreen): a click could hit anything

/*
===============
MsGlobal

A point of the window's content (points from its top left) in global display coordinates.
===============
*/
static CGPoint MsGlobal( double x, double y )
{
	NSRect frame = [kb_gameWindow convertRectToScreen:kb_gameWindow.contentView.frame];

	return CGPointMake( NSMinX( frame ) + x, NSMaxY( NSScreen.screens.firstObject.frame ) - NSMaxY( frame ) + y );
}

/*
===============
MsPost

The CGEvent's fields go onto a mouse event made by AppKit for the game window at the replayed cursor's point, which
becomes the NSEvent posted into the game's queue. (A CGEvent alone gets no window, or with the window number's field
a wrong point, and AppKit drops it; the fields that hold the window and its point are left alone. Its delta fields
are integers, so posted movement has whole deltas only.) What AppKit made of it goes to the log.
===============
*/
static void MsPost( CGEventRef cg )
{
	static const char *types[] = { [NSEventTypeLeftMouseDown] = "left down", [NSEventTypeLeftMouseUp] = "left up",
		[NSEventTypeRightMouseDown] = "right down", [NSEventTypeRightMouseUp] = "right up",
		[NSEventTypeMouseMoved] = "moved", [NSEventTypeLeftMouseDragged] = "left dragged",
		[NSEventTypeRightMouseDragged] = "right dragged", [NSEventTypeScrollWheel] = "scroll",
		[NSEventTypeOtherMouseDown] = "other down", [NSEventTypeOtherMouseUp] = "other up",
		[NSEventTypeOtherMouseDragged] = "other dragged" };
	NSInteger windowNumber = kb_gameWindow.windowNumber;
	NSEvent *base = [NSEvent mouseEventWithType:NSEventTypeMouseMoved location:NSMakePoint( ms_x,
		kb_gameWindow.contentView.bounds.size.height - ms_y ) modifierFlags:0
		timestamp:[NSProcessInfo processInfo].systemUptime windowNumber:windowNumber context:nil eventNumber:0
		clickCount:1 pressure:1];
	CGEventRef copy = CGEventCreateCopy( base.CGEvent );
	NSEvent *event;
	int field;

	CGEventSetType( copy, CGEventGetType( cg ) );
	for( field = 0; field < 256; field++ )
	{
		// the type, the window and its point (found by trying)
		if( field == 41 || field == 43 || field == 44 || field == 50 || field == 51 || field == 52 || field == 55 ||
			field == 102 )
			continue;
		CGEventSetIntegerValueField( copy, field, CGEventGetIntegerValueField( cg, field ) );
	}
	if( CGEventGetType( cg ) == kCGEventScrollWheel )
	{
		CGEventSetDoubleValueField( copy, kCGScrollWheelEventFixedPtDeltaAxis1,
			CGEventGetDoubleValueField( cg, kCGScrollWheelEventFixedPtDeltaAxis1 ) );
	}
	event = [NSEvent eventWithCGEvent:copy];
	CFRelease( copy );
	CFRelease( cg );

	if( event.type == NSEventTypeScrollWheel )
	{
		fprintf( kb_log, "    posted %s: game window %d, at %.2f,%.2f, deltaY %.6f, precise %d\n", types[ event.type ],
			event.windowNumber == windowNumber, event.locationInWindow.x, event.locationInWindow.y, event.deltaY,
			event.hasPreciseScrollingDeltas );
	}
	else
	{
		fprintf( kb_log, "    posted %s: game window %d, at %.2f,%.2f, delta %.9f,%.9f, button %ld\n",
			types[ event.type ] ?: "?", event.windowNumber == windowNumber, event.locationInWindow.x,
			event.locationInWindow.y, event.deltaX, event.deltaY, (long)event.buttonNumber );
	}
	fflush( kb_log );

	[NSApp postEvent:event atStart:NO];
}

/*
===============
MsMouse
===============
*/
static void MsMouse( CGEventType type, int button, double dx, double dy )
{
	CGEventRef cg = CGEventCreateMouseEvent( NULL, type, MsGlobal( ms_x, ms_y ), (CGMouseButton)button );

	CGEventSetIntegerValueField( cg, kCGMouseEventDeltaX, (int64_t)dx );
	CGEventSetIntegerValueField( cg, kCGMouseEventDeltaY, (int64_t)dy );
	CGEventSetIntegerValueField( cg, kCGMouseEventClickState, 1 );
	MsPost( cg );
}

/*
===============
MsButton
===============
*/
static void MsButton( int button, qboolean down )
{
	CGEventType type;

	if( ms_skipButtons )
	{
		fprintf( kb_log, "    button %d %s: skipped (a captured menu)\n", button, down ? "down" : "up" );
		return;
	}

	if( button == 0 )
		type = down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp;
	else if( button == 1 )
		type = down ? kCGEventRightMouseDown : kCGEventRightMouseUp;
	else
		type = down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp;
	MsMouse( type, button, 0, 0 );
}

/*
===============
MsWheel
===============
*/
static void MsWheel( CGScrollEventUnit unit, int amount )
{
	CGEventRef cg = CGEventCreateScrollWheelEvent( NULL, unit, 1, amount );

	CGEventSetLocation( cg, MsGlobal( ms_x, ms_y ) );
	MsPost( cg );
}

/*
===============
MsReplay_f
===============
*/
static void MsReplay_f( void )
{
	char path[ MAX_OSPATH ];

	if( Cmd_Argc( ) != 2 )
	{
		Com_Printf( "msreplay <name>: posts the mouse replay, logs to <fs_homepath>/kbreplay-<name>.log\n" );
		return;
	}

	Com_sprintf( path, sizeof( path ), "%s/kbreplay-%s.log", Cvar_VariableString( "fs_homepath" ), Cmd_Argv( 1 ) );
	kb_log = fopen( path, "w" );
	if( !kb_log )
	{
		Com_Printf( "msreplay: can't write %s\n", path );
		return;
	}

	kb_gameWindow = NSApp.keyWindow ?: NSApp.mainWindow;
	fprintf( kb_log, "msreplay %s: game window key %d\n", Cmd_Argv( 1 ), kb_gameWindow.keyWindow );
	fflush( kb_log );

	ms_pos = 0;
	kb_wait = 0;
	kb_stepNum = 0;
}

/*
===============
MsLog_f

`mslog <name>`: no replay, only the log of the queued events (a real mouse or keyboard, e.g. GCMouse's path with
in_mouseRaw 1), with a line every second.
===============
*/
static int ms_logFrames = -1;

static void MsLog_f( void )
{
	char path[ MAX_OSPATH ];

	if( Cmd_Argc( ) != 2 )
	{
		Com_Printf( "mslog <name>: logs the queued events to <fs_homepath>/kbreplay-<name>.log\n" );
		return;
	}

	Com_sprintf( path, sizeof( path ), "%s/kbreplay-%s.log", Cvar_VariableString( "fs_homepath" ), Cmd_Argv( 1 ) );
	kb_log = fopen( path, "w" );
	if( kb_log )
	{
		fprintf( kb_log, "mslog %s: in_mouseRaw %d\n", Cmd_Argv( 1 ), Cvar_VariableIntegerValue( "in_mouseRaw" ) );
		fflush( kb_log );
		ms_logFrames = 0;
	}
}

/*
===============
MsReplay_Frame
===============
*/
static void MsReplay_Frame( void )
{
	const msAction_t *a;
	NSSize size;
	qboolean captured;

	if( kb_wait > 0 )
	{
		kb_wait--;
		return;
	}

	// the first step waits for the game: in the map, no menu or loading screen up, the mouse captured (~10 s)
	if( ms_pos == 0 && ( clc.state != CA_ACTIVE || Key_GetCatcher( ) != 0 || !IN_IsCursorActive( ) ) &&
		kb_startWait < 2500 )
	{
		if( kb_startWait++ % 250 == 0 )
		{
			fprintf( kb_log, "waiting for the game: state %d, catcher 0x%x, captured %d\n", clc.state,
				Key_GetCatcher( ), IN_IsCursorActive( ) );
			fflush( kb_log );
		}
		return;
	}

	a = &ms_script[ ms_pos ];
	if( a->op == MS_END )
	{
		if( ++kb_afterEnd == 100 )
		{
			fprintf( kb_log, "still running 100 frames after Cmd+Q: quitting\n" );
			fflush( kb_log );
			Cbuf_AddText( "quit\n" );
		}
		return;
	}

	// a capture parks the cursor at the window's center, and so does the release
	size = kb_gameWindow.contentView.bounds.size;
	captured = IN_IsCursorActive( );
	if( captured != ms_captured )
	{
		ms_captured = captured;
		ms_x = (int)size.width / 2;
		ms_y = (int)size.height / 2;
	}
	ms_skipButtons = captured && ( Key_GetCatcher( ) & KEYCATCH_UI );

	// the UI's cursor where it is used: released, or a captured menu
	if( captured && !in_guimouse )
	{
		fprintf( kb_log, "step %d: %s (catcher 0x%x, captured %d, guimouse %d)\n", ++kb_stepNum, a->note,
			Key_GetCatcher( ), captured, in_guimouse );
	}
	else
	{
		fprintf( kb_log, "step %d: %s (catcher 0x%x, captured %d, guimouse %d, ui cursor %d,%d)\n", ++kb_stepNum,
			a->note, Key_GetCatcher( ), captured, in_guimouse, cl.mousex, cl.mousey );
	}
	fflush( kb_log );
	kb_wait = 3;

	for( a++, ms_pos++; a->op != MS_STEP && a->op != MS_END; a++, ms_pos++ )
	{
		switch( a->op )
		{
			case MS_MOVE:
				if( !captured )
				{
					ms_x += a->a;
					ms_y += a->b;
				}
				MsMouse( kCGEventMouseMoved, 0, a->a, a->b );
				break;

			case MS_AT:
				ms_x = a->a;
				ms_y = a->b;
				MsMouse( kCGEventMouseMoved, 0, 0, 0 );
				break;

			case MS_DOWN:
				MsButton( (int)a->a, qtrue );
				break;

			case MS_UP:
				MsButton( (int)a->a, qfalse );
				break;

			case MS_DRAG:
				if( !captured )
				{
					ms_x += a->b;
					ms_y += a->c;
				}
				if( ms_skipButtons )
					MsMouse( kCGEventMouseMoved, 0, a->b, a->c );
				else if( a->a == 0 )
					MsMouse( kCGEventLeftMouseDragged, 0, a->b, a->c );
				else if( a->a == 1 )
					MsMouse( kCGEventRightMouseDragged, 1, a->b, a->c );
				else
					MsMouse( kCGEventOtherMouseDragged, (int)a->a, a->b, a->c );
				break;

			case MS_WHEEL:
				MsWheel( kCGScrollEventUnitLine, (int)a->a );
				break;

			case MS_WHEELPX:
				MsWheel( kCGScrollEventUnitPixel, (int)a->a );
				break;

			case MS_KEY:
				KbPostKey( NSEventTypeKeyDown, (int)a->a, NO );
				KbPostKey( NSEventTypeKeyUp, (int)a->a, NO );
				break;

			case MS_MOD:
				kb_down[ (int)a->a ] = !kb_down[ (int)a->a ];
				KbPostFlags( (int)a->a );
				break;

			case MS_KEYDOWN:
				KbPostKey( NSEventTypeKeyDown, (int)a->a, NO );
				break;
		}
	}
}

/*
==============================================================================

THE DIALOG CHECK (dlgtest)

==============================================================================
*/

static int		dlg_state;		// 1 = waiting for the game, 2 = the dialog is done: frames counted, 0 = idle
static int		dlg_frames;

static const char *DlgRect( CGRect r )
{
	return va( "%.0f,%.0f %.0fx%.0f", r.origin.x, r.origin.y, r.size.width, r.size.height );
}

/*
===============
DlgProbe

What the screen shows: the game window, the dialog, and the on-screen windows in front of the dialog that overlap
it (CGWindowList, front to back, global top-left coordinates). Window names need a permission the tools don't have;
the owner, level and bounds don't.
===============
*/
static void DlgProbe( FILE *log, const char *label, NSWindow *game )
{
	NSWindow *modal = NSApp.modalWindow;
	CGDirectDisplayID display = CGMainDisplayID( );
	CGDisplayModeRef mode = CGDisplayCopyDisplayMode( display );

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	fprintf( log, "%s: app active %d, presentation 0x%lx, main display %zux%zu captured %d, cursor visible %d, "
		"IN_IsCursorActive %d, catcher 0x%x\n", label, NSApp.active, (unsigned long)NSApp.presentationOptions,
		CGDisplayModeGetWidth( mode ), CGDisplayModeGetHeight( mode ), CGDisplayIsCaptured( display ),
		CGCursorIsVisible( ), IN_IsCursorActive( ), Key_GetCatcher( ) );
#pragma clang diagnostic pop
	CGDisplayModeRelease( mode );

	if( game )
		fprintf( log, "  game window %ld: level %ld, visible %d, minimized %d, key %d, on the active space %d\n",
			(long)game.windowNumber, (long)game.level, game.visible, game.miniaturized, game.keyWindow,
			game.onActiveSpace );

	if( modal )
	{
		CFArrayRef list = CGWindowListCopyWindowInfo( kCGWindowListOptionOnScreenOnly, kCGNullWindowID );
		NSArray *windows = (__bridge_transfer NSArray *)list;
		CGRect dialogRect = CGRectNull;
		int over = 0;

		fprintf( log, "  dialog %ld: level %ld, visible %d, key %d, on the active space %d\n",
			(long)modal.windowNumber, (long)modal.level, modal.visible, modal.keyWindow, modal.onActiveSpace );

		for( NSDictionary *w in windows )
		{
			if( [w[ (id)kCGWindowNumber ] longValue] == modal.windowNumber )
				CGRectMakeWithDictionaryRepresentation( (__bridge CFDictionaryRef)w[ (id)kCGWindowBounds ],
					&dialogRect );
		}
		if( CGRectIsNull( dialogRect ) )
		{
			fprintf( log, "  dialog not in the on-screen window list\n" );
			return;
		}

		fprintf( log, "  dialog on screen at %s\n", DlgRect( dialogRect ) );
		for( NSDictionary *w in windows )
		{
			CGRect r;

			if( [w[ (id)kCGWindowNumber ] longValue] == modal.windowNumber )
				break;
			CGRectMakeWithDictionaryRepresentation( (__bridge CFDictionaryRef)w[ (id)kCGWindowBounds ], &r );
			if( !CGRectIntersectsRect( r, dialogRect ) )
				continue;
			over++;
			fprintf( log, "  in front of it: %s (pid %d) window %ld, layer %ld, alpha %.2f, %s\n",
				[w[ (id)kCGWindowOwnerName ] UTF8String], [w[ (id)kCGWindowOwnerPID ] intValue],
				[w[ (id)kCGWindowNumber ] longValue], [w[ (id)kCGWindowLayer ] longValue],
				[w[ (id)kCGWindowAlpha ] doubleValue], DlgRect( r ) );
		}
		fprintf( log, "  %d window(s) in front of the dialog over it\n", over );
	}
	fflush( log );
}

/*
===============
DlgButton

The dialog's button with that title (NSAlert's own views).
===============
*/
static NSButton *DlgButton( NSView *view, NSString *title )
{
	if( [view isKindOfClass:[NSButton class]] && [((NSButton *)view).title isEqualToString:title] )
		return (NSButton *)view;

	for( NSView *subview in view.subviews )
	{
		NSButton *button = DlgButton( subview, title );

		if( button )
			return button;
	}
	return nil;
}

/*
===============
DlgAnswer

Answers the dialog with its second button, pressed as a click presses it (performClick: runs the button's action,
NSAlert's own stop). Without the button, the modal loop is stopped directly; a timer isn't an event, so an empty
event then wakes the loop to see the stop.
===============
*/
static void DlgAnswer( FILE *log )
{
	NSWindow *modal = NSApp.modalWindow;
	NSButton *button = DlgButton( modal.contentView, @"Cancel" ) ?: DlgButton( modal.contentView, @"No" );

	fprintf( log, "  answered: %s\n", button ? button.title.UTF8String : "no button found, the loop stopped" );
	fflush( log );
	if( button )
	{
		[button performClick:nil];
		return;
	}
	[NSApp stopModalWithCode:NSAlertSecondButtonReturn];
	[NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint
		modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0] atStart:YES];
}

/*
===============
DlgDismissLater

A timer that runs inside the dialog's modal loop: probes, then answers.
===============
*/
static void DlgDismissLater( FILE *log, NSWindow *game )
{
	NSTimer *timer = [NSTimer timerWithTimeInterval:1.5 repeats:NO block:^( NSTimer *t )
	{
		DlgProbe( log, "during the dialog", game );
		if( NSApp.modalWindow )
			DlgAnswer( log );
	}];

	[[NSRunLoop mainRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];
}

/*
===============
DlgTest_f
===============
*/
static void DlgTest_f( void )
{
	char path[ MAX_OSPATH ];

	if( Cmd_Argc( ) != 2 )
	{
		Com_Printf( "dlgtest <name>: Sys_Dialog in the game, logs to <fs_homepath>/kbreplay-<name>.log\n" );
		return;
	}

	Com_sprintf( path, sizeof( path ), "%s/kbreplay-%s.log", Cvar_VariableString( "fs_homepath" ), Cmd_Argv( 1 ) );
	kb_log = fopen( path, "w" );
	if( !kb_log )
	{
		Com_Printf( "dlgtest: can't write %s\n", path );
		return;
	}

	kb_gameWindow = NSApp.keyWindow ?: NSApp.mainWindow;
	fprintf( kb_log, "dlgtest %s: r_fullscreen %d, r_noborder %d, r_borderlessWindow %d\n", Cmd_Argv( 1 ),
		Cvar_VariableIntegerValue( "r_fullscreen" ), Cvar_VariableIntegerValue( "r_noborder" ),
		Cvar_VariableIntegerValue( "r_borderlessWindow" ) );
	fflush( kb_log );

	dlg_state = 1;
	dlg_frames = 0;
}

/*
===============
DlgTest_Frame
===============
*/
static void DlgTest_Frame( void )
{
	dialogResult_t result;

	if( dlg_state == 2 )
	{
		dlg_frames++;
		if( dlg_frames == 1 || dlg_frames == 50 || dlg_frames == 250 )
			DlgProbe( kb_log, va( "%d frame(s) after the dialog", dlg_frames ), kb_gameWindow );
		if( dlg_frames == 260 )
		{
			dlg_state = 0;
			Cbuf_AddText( "quit\n" );
		}
		return;
	}

	// the game has the mouse (in the map, no catcher, captured) for 100 frames, or 10 s went by
	if( clc.state == CA_ACTIVE && Key_GetCatcher( ) == 0 && IN_IsCursorActive( ) )
		dlg_frames++;
	if( dlg_frames < 100 && ++kb_startWait < 2500 )
		return;

	DlgProbe( kb_log, "before the dialog", kb_gameWindow );
	DlgDismissLater( kb_log, kb_gameWindow );
	result = Sys_Dialog( DT_OK_CANCEL, "dlgtest: Sys_Dialog with the game up. It closes by itself.", "dlgtest" );
	fprintf( kb_log, "Sys_Dialog returned %s\n", result == DR_CANCEL ? "DR_CANCEL (the second button)" :
		result == DR_OK ? "DR_OK" : "something else" );
	DlgProbe( kb_log, "right after Sys_Dialog", kb_gameWindow );

	dlg_state = 2;
	dlg_frames = 0;
}

/*
===============
DlgStartupWatch

OMM_DLGTEST=<log path>: before main, a timer that answers the first dialog (the startup's "Abnormal Exit"
question, before any window) with its second button, No, after probing it.
===============
*/
__attribute__((constructor)) static void DlgStartupWatch( void )
{
	const char *path = getenv( "OMM_DLGTEST" );
	__block FILE *log;
	__block int fires = 0;
	CFRunLoopTimerRef timer;

	if( !path || !( log = fopen( path, "w" ) ) )
		return;

	timer = CFRunLoopTimerCreateWithHandler( NULL, CFAbsoluteTimeGetCurrent( ) + 1.5, 0.5, 0, 0,
		^( CFRunLoopTimerRef t )
	{
		if( NSApp.modalWindow )
		{
			DlgProbe( log, "the startup dialog", nil );
			DlgAnswer( log );
			CFRunLoopTimerInvalidate( t );
		}
		else if( ++fires == 60 )
		{
			fprintf( log, "no dialog in 30 s\n" );
			fflush( log );
			CFRunLoopTimerInvalidate( t );
		}
	} );
	CFRunLoopAddTimer( CFRunLoopGetMain( ), timer, kCFRunLoopCommonModes );
	CFRelease( timer );
}

#ifdef PROTOCOL_HANDLER
/*
==============================================================================

THE LINK CHECK (urltest)

==============================================================================
*/

/*
===============
UrlConnect_f

Stands in for connect: logs, connects nowhere.
===============
*/
static void UrlConnect_f( void )
{
	fprintf( kb_log, "  connect: %s\n", Cmd_Args( ) );
	fflush( kb_log );
}

/*
===============
UrlTest_f
===============
*/
static void UrlTest_f( void )
{
	static const char *links[] =
	{
		"openmohaa://connect/203.0.113.7:12203",			// documentation addresses (RFC 5737, RFC 3849)
		"openmohaa:connect/[2001:db8::1]:12203",
		"openmohaa://connect/example.test?password=abc",	// the query is dropped
		"openmohaa://connect/bad;host",						// refused: a character outside a-zA-Z0-9.:-[]
		"openmohaa://connect/",								// refused: no argument
		"openmohaa://status",								// refused: only connect
		"quake3://connect/203.0.113.7",						// another scheme: ignored
	};
	char path[ MAX_OSPATH ];
	int i;

	if( Cmd_Argc( ) != 2 )
	{
		Com_Printf( "urltest <name>: the openmohaa:// handler, logs to <fs_homepath>/kbreplay-<name>.log\n" );
		return;
	}

	Com_sprintf( path, sizeof( path ), "%s/kbreplay-%s.log", Cvar_VariableString( "fs_homepath" ), Cmd_Argv( 1 ) );
	kb_log = fopen( path, "w" );
	if( !kb_log )
	{
		Com_Printf( "urltest: can't write %s\n", path );
		return;
	}
	fprintf( kb_log, "urltest %s: PROTOCOL_HANDLER \"%s\"\n", Cmd_Argv( 1 ), PROTOCOL_HANDLER );

	Cmd_RemoveCommand( "connect" );
	Cmd_AddCommand( "connect", UrlConnect_f );

	for( i = 0; i < ARRAY_LEN( links ); i++ )
	{
		NSAppleEventDescriptor *event = [NSAppleEventDescriptor appleEventWithEventClass:kInternetEventClass
			eventID:kAEGetURL targetDescriptor:[NSAppleEventDescriptor currentProcessDescriptor]
			returnID:kAutoGenerateReturnID transactionID:kAnyTransactionID];
		NSError *error = nil;

		[event setParamDescriptor:[NSAppleEventDescriptor descriptorWithString:@( links[ i ] )]
			forKeyword:keyDirectObject];
		fprintf( kb_log, "link %s\n", links[ i ] );
		fflush( kb_log );
		if( ![event sendEventWithOptions:NSAppleEventSendNoReply timeout:5 error:&error] )
			fprintf( kb_log, "  not sent: %s\n", error.description.UTF8String );
		// the handler appends connect to the command buffer: run it now, under this link
		Cbuf_Execute( 0 );
	}

	fprintf( kb_log, "done\n" );
	fflush( kb_log );
	Cbuf_AddText( "quit\n" );
}
#endif

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
		Cmd_AddCommand( "msreplay", MsReplay_f );
		Cmd_AddCommand( "mslog", MsLog_f );
		Cmd_AddCommand( "dlgtest", DlgTest_f );
#ifdef PROTOCOL_HANDLER
		Cmd_AddCommand( "urltest", UrlTest_f );
#endif
		registered = qtrue;
	}

	if( dlg_state )
	{
		DlgTest_Frame( );
		return;
	}

	if( ms_logFrames >= 0 )
	{
		if( ms_logFrames++ % 250 == 0 )
		{
			fprintf( kb_log, "second %d (catcher 0x%x, captured %d)\n", ms_logFrames / 250, Key_GetCatcher( ),
				IN_IsCursorActive( ) );
			fflush( kb_log );
		}
		return;
	}

	if( ms_pos >= 0 )
	{
		MsReplay_Frame( );
		return;
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
