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
// So far the event pump, the window's events (focus, minimize, resize, quit), the keyboard and text; the mouse and
// gamepads come in the next slices of Phase 6 (docs/METAL_PORT_PLAN.md).

#import <Carbon/Carbon.h>	// the keyboard layout (TIS, UCKeyTranslate) and the kVK_* virtual key codes

#include "macos_local.h"
#include "../client/client.h"
#include "../sys/sys_local.h"

static cvar_t *in_keyboardDebug     = NULL;

static NSWindow *in_window;
static NSArray *in_windowObservers;

static int vidRestartTime = 0;

static int in_eventTime = 0;

#define CTRL(a) ((a)-'a'+1)

/*
==============================================================================

KEYBOARD

The keys resolve exactly as they did through SDL 2.32 (its Cocoa keyboard, read from its binary) and sdl_input.c,
so every key gives the same Q3 key and saved binds keep working: the virtual key code (a physical position) gives
SDL's scancode (a USB HID usage), the scancode gives the layout's unshifted character (SDL's key code, "sym"), and
IN_TranslateKey makes the Q3 key from both, as IN_TranslateSDLToQ3Key did.

The key events are read in the pump, before AppKit dispatches them (MacOS_PumpEvents), as SDL read them.
==============================================================================
*/

// SDL's scancodes (USB HID usage IDs), the numbering IN_TranslateKey and K_WORLD_0 + scancode use
enum
{
	SC_A = 4, SC_Z = 29, SC_1 = 30, SC_9 = 38, SC_0 = 39, SC_RETURN = 40, SC_SLASH = 56, SC_GRAVE = 53,
	SC_CAPSLOCK = 57, SC_F1 = 58, SC_F2, SC_F3, SC_F4, SC_F5, SC_F6, SC_F7, SC_F8, SC_F9, SC_F10, SC_F11, SC_F12,
	SC_PRINTSCREEN = 70, SC_SCROLLLOCK, SC_PAUSE, SC_INSERT, SC_HOME, SC_PAGEUP, SC_DELETE, SC_END, SC_PAGEDOWN,
	SC_RIGHT, SC_LEFT, SC_DOWN, SC_UP, SC_NUMLOCKCLEAR, SC_KP_DIVIDE, SC_KP_MULTIPLY, SC_KP_MINUS, SC_KP_PLUS,
	SC_KP_ENTER, SC_KP_1, SC_KP_2, SC_KP_3, SC_KP_4, SC_KP_5, SC_KP_6, SC_KP_7, SC_KP_8, SC_KP_9, SC_KP_0,
	SC_KP_PERIOD, SC_NONUSBACKSLASH, SC_APPLICATION, SC_POWER, SC_F13 = 104, SC_F14, SC_F15, SC_HELP = 117,
	SC_MENU, SC_UNDO = 122, SC_INTERNATIONAL1 = 135, SC_INTERNATIONAL3 = 137, SC_LANG1 = 144, SC_LANG2,
	SC_SYSREQ = 154, SC_LCTRL = 224, SC_LSHIFT, SC_LALT, SC_LGUI, SC_RCTRL, SC_RSHIFT, SC_RALT, SC_RGUI,
	SC_MODE = 257,

	SC_NUM = 256	// the scancodes kept; SDL's macOS table yields none above SC_RGUI
};

// SDL's key code of a key without a character (SDL_SCANCODE_TO_KEYCODE)
#define SYM( scancode )		( ( scancode ) | ( 1 << 30 ) )
#define SYM_IS_SCANCODE( sym )	( ( ( sym ) & ( 1 << 30 ) ) != 0 )

// SDL's modifier state (KMOD_*), for in_keyboardDebug and caps lock
#define KMOD_LSHIFT	0x0001
#define KMOD_RSHIFT	0x0002
#define KMOD_LCTRL	0x0040
#define KMOD_RCTRL	0x0080
#define KMOD_LALT	0x0100
#define KMOD_RALT	0x0200
#define KMOD_LGUI	0x0400
#define KMOD_RGUI	0x0800
#define KMOD_NUM	0x1000
#define KMOD_CAPS	0x2000
#define KMOD_MODE	0x4000
#define KMOD_SCROLL	0x8000

// The device-dependent modifier bits of NSEvent.modifierFlags (NX_DEVICE*KEYMASK, IOKit's IOLLEvent.h): which side
// of a modifier is down
#define NX_DEVICELCTLKEYMASK	0x00000001
#define NX_DEVICELSHIFTKEYMASK	0x00000002
#define NX_DEVICERSHIFTKEYMASK	0x00000004
#define NX_DEVICELCMDKEYMASK	0x00000008
#define NX_DEVICERCMDKEYMASK	0x00000010
#define NX_DEVICELALTKEYMASK	0x00000020
#define NX_DEVICERALTKEYMASK	0x00000040
#define NX_DEVICERCTLKEYMASK	0x00002000

// SDL's darwin_scancode_table (copied from its binary): virtual key code -> scancode, 0 = no key
static const byte in_vkToScancode[ 128 ] =
{
	  4,  22,   7,   9,  11,  10,  29,  27,	// 0x00 A S D F H G Z X
	  6,  25, 100,   5,  20,  26,   8,  21,	// 0x08 C V ISO_Section B Q W E R
	 28,  23,  30,  31,  32,  33,  35,  34,	// 0x10 Y T 1 2 3 4 6 5
	 46,  38,  36,  45,  37,  39,  48,  18,	// 0x18 Equal 9 7 Minus 8 0 RightBracket O
	 24,  47,  12,  19,  40,  15,  13,  52,	// 0x20 U LeftBracket I P Return L J Quote
	 14,  51,  49,  54,  56,  17,  16,  55,	// 0x28 K Semicolon Backslash Comma Slash N M Period
	 43,  44,  53,  42,  88,  41, 231, 227,	// 0x30 Tab Space Grave Delete - Escape RightCommand Command
	225,  57, 226, 224, 229, 230, 228, 231,	// 0x38 Shift CapsLock Option Control RightShift RightOption RightControl Function
	108,  99,   0,  85,   0,  87,   0,  83,	// 0x40 F17 KPDecimal - KPMultiply - KPPlus - KPClear
	128, 129, 127,  84,  88,   0,  86, 109,	// 0x48 VolumeUp VolumeDown Mute KPDivide KPEnter - KPMinus F18
	110, 103,  98,  89,  90,  91,  92,  93,	// 0x50 F19 KPEquals KP0 KP1 KP2 KP3 KP4 KP5
	 94,  95,   0,  96,  97, 137, 135, 133,	// 0x58 KP6 KP7 F20 KP8 KP9 JIS_Yen JIS_Underscore JIS_KeypadComma
	 62,  63,  64,  60,  65,  66, 145,  68,	// 0x60 F5 F6 F7 F3 F8 F9 JIS_Eisu F11
	144,  70, 107,  71,   0,  67, 101,  69,	// 0x68 JIS_Kana F13 F16 F14 - F10 ContextualMenu F12
	  0,  72,  73,  74,  75,  76,  61,  77,	// 0x70 - F15 Help Home PageUp ForwardDelete F4 End
	 59,  78,  58,  80,  79,  81,  82, 102,	// 0x78 F2 PageDown F1 LeftArrow RightArrow DownArrow UpArrow -
};

static byte					in_keyState[ SC_NUM ];	// SDL's key state: 1 = down
static int					in_keymap[ SC_NUM ];	// scancode -> sym, for the current layout
static int					in_modState;			// KMOD_*
static TISInputSourceRef	in_keyLayout;			// the layout in_keymap was built from
static keyNum_t				lastKeyDown = 0;

/*
===============
IN_DefaultSym

SDL's default key code of a scancode: a key with a character on a US keyboard has that character (the layout then
replaces it), every other key its SYM().
===============
*/
static int IN_DefaultSym( int scancode )
{
	static const char returnToSlash[] = { '\r', '\x1B', '\b', '\t', ' ', '-', '=', '[', ']', '\\', '#', ';', '\'',
		'`', ',', '.', '/' };

	if( scancode >= SC_A && scancode <= SC_Z )
		return 'a' + scancode - SC_A;
	if( scancode >= SC_1 && scancode <= SC_9 )
		return '1' + scancode - SC_1;
	if( scancode == SC_0 )
		return '0';
	if( scancode >= SC_RETURN && scancode <= SC_SLASH )
		return returnToSlash[ scancode - SC_RETURN ];
	if( scancode == SC_DELETE )
		return '\x7F';

	// no character until the layout gives one
	if( scancode == 0 || scancode == SC_NONUSBACKSLASH || scancode == SC_INTERNATIONAL1 ||
		scancode == SC_INTERNATIONAL3 || scancode == SC_LANG1 || scancode == SC_LANG2 )
		return 0;

	return SYM( scancode );
}

/*
===============
IN_IsISOKeyboard

On ISO keyboards the key left of 1 and the key right of left shift come with each other's virtual key codes
(SDL swaps them back, and so do we).
===============
*/
static qboolean IN_IsISOKeyboard( void )
{
	return KBGetLayoutType( LMGetKbdType( ) ) == kKeyboardISO;
}

/*
===============
IN_UpdateKeymap

SDL's UpdateKeymap: when the keyboard layout changed (checked at every key press that isn't a repeat; it costs a
pointer compare), each key with a character gets the layout's unshifted character, without dead keys.
===============
*/
static void IN_UpdateKeymap( void )
{
	TISInputSourceRef layout = TISCopyCurrentKeyboardLayoutInputSource( );
	CFDataRef uchr;
	const UCKeyboardLayout *keyboardLayout;
	UInt32 keyboardType;
	int i;

	if( !in_keymap[ SC_A ] )
	{
		// the first call: the default map until a layout replaces it
		for( i = 0; i < SC_NUM; i++ )
			in_keymap[ i ] = IN_DefaultSym( i );
	}

	if( layout == in_keyLayout )
	{
		if( layout )
			CFRelease( layout );
		return;
	}

	if( in_keyLayout )
		CFRelease( in_keyLayout );
	in_keyLayout = layout;

	// a layout without Unicode data leaves the old map (SDL)
	uchr = layout ? TISGetInputSourceProperty( layout, kTISPropertyUnicodeKeyLayoutData ) : NULL;
	keyboardLayout = uchr ? (const UCKeyboardLayout *)CFDataGetBytePtr( uchr ) : NULL;
	if( !keyboardLayout )
		return;

	for( i = 0; i < SC_NUM; i++ )
		in_keymap[ i ] = IN_DefaultSym( i );

	keyboardType = LMGetKbdType( );

	for( i = 0; i < ARRAY_LEN( in_vkToScancode ); i++ )
	{
		int scancode = in_vkToScancode[ i ];
		UniChar chars[ 8 ];
		UniCharCount length;
		UInt32 deadKeyState = 0;

		if( scancode == 0 || SYM_IS_SCANCODE( in_keymap[ scancode ] ) )
			continue;

		if( ( scancode == SC_NONUSBACKSLASH || scancode == SC_GRAVE ) && IN_IsISOKeyboard( ) )
			scancode = SC_NONUSBACKSLASH + SC_GRAVE - scancode;

		if( UCKeyTranslate( keyboardLayout, i, kUCKeyActionDown, 0, keyboardType, kUCKeyTranslateNoDeadKeysMask,
			&deadKeyState, ARRAY_LEN( chars ), &length, chars ) != noErr )
			continue;

		// 0x10 is the "function key" character
		if( length > 0 && chars[ 0 ] != 0x10 )
			in_keymap[ scancode ] = chars[ 0 ];
	}

	// (SDL also gives the number row of AZERTY layouts digits as key codes; IN_TranslateKey takes the number row
	// by scancode, so only in_keyboardDebug would show the difference)
}

/*
===============
IN_PrintKey
===============
*/
static void IN_PrintKey( int virtualKey, int scancode, int sym, keyNum_t key, qboolean down )
{
	if( down )
		Com_Printf( "+ " );
	else
		Com_Printf( "  " );

	Com_Printf( "Scancode: 0x%02x Sym: 0x%02x", scancode, sym );
	if( sym > ' ' && sym < 0x7F )
		Com_Printf( "(%c)", sym );
	if( virtualKey >= 0 )
		Com_Printf( " kVK: 0x%02x", virtualKey );

	if( in_modState & KMOD_LSHIFT )   Com_Printf( " KMOD_LSHIFT" );
	if( in_modState & KMOD_RSHIFT )   Com_Printf( " KMOD_RSHIFT" );
	if( in_modState & KMOD_LCTRL )    Com_Printf( " KMOD_LCTRL" );
	if( in_modState & KMOD_RCTRL )    Com_Printf( " KMOD_RCTRL" );
	if( in_modState & KMOD_LALT )     Com_Printf( " KMOD_LALT" );
	if( in_modState & KMOD_RALT )     Com_Printf( " KMOD_RALT" );
	if( in_modState & KMOD_LGUI )     Com_Printf( " KMOD_LGUI" );
	if( in_modState & KMOD_RGUI )     Com_Printf( " KMOD_RGUI" );
	if( in_modState & KMOD_NUM )      Com_Printf( " KMOD_NUM" );
	if( in_modState & KMOD_CAPS )     Com_Printf( " KMOD_CAPS" );
	if( in_modState & KMOD_MODE )     Com_Printf( " KMOD_MODE" );
	if( in_modState & KMOD_SCROLL )   Com_Printf( " KMOD_SCROLL" );

	Com_Printf( " Q:0x%02x(%s)\n", key, Key_KeynumToString( key ) );
}

#define MAX_CONSOLE_KEYS 16

/*
===============
IN_IsConsoleKey

TODO: If the SDL_Scancode situation improves, use it instead of
      both of these methods
===============
*/
static qboolean IN_IsConsoleKey( keyNum_t key, int character )
{
	typedef struct consoleKey_s
	{
		enum
		{
			QUAKE_KEY,
			CHARACTER
		} type;

		union
		{
			keyNum_t key;
			int character;
		} u;
	} consoleKey_t;

	static consoleKey_t consoleKeys[ MAX_CONSOLE_KEYS ];
	static int numConsoleKeys = 0;
	int i;

	// Only parse the variable when it changes
	if( cl_consoleKeys->modified )
	{
		char *text_p;
		const char *token;

		cl_consoleKeys->modified = qfalse;
		text_p = cl_consoleKeys->string;
		numConsoleKeys = 0;

		while( numConsoleKeys < MAX_CONSOLE_KEYS )
		{
			consoleKey_t *c = &consoleKeys[ numConsoleKeys ];
			int charCode = 0;

			token = COM_Parse( &text_p );
			if( !token[ 0 ] )
				break;

			charCode = Com_HexStrToInt( token );

			if( charCode > 0 )
			{
				c->type = CHARACTER;
				c->u.character = charCode;
			}
			else
			{
				c->type = QUAKE_KEY;
				c->u.key = Key_StringToKeynum( token );

				// 0 isn't a key
				if( c->u.key <= 0 )
					continue;
			}

			numConsoleKeys++;
		}
	}

	// If the character is the same as the key, prefer the character
	if( key == character )
		key = 0;

	for( i = 0; i < numConsoleKeys; i++ )
	{
		consoleKey_t *c = &consoleKeys[ i ];

		switch( c->type )
		{
			case QUAKE_KEY:
				if( key && c->u.key == key )
					return qtrue;
				break;

			case CHARACTER:
				if( c->u.character == character )
					return qtrue;
				break;
		}
	}

	return qfalse;
}

/*
===============
IN_TranslateKey

sdl_input.c's IN_TranslateSDLToQ3Key, case for case.
===============
*/
static keyNum_t IN_TranslateKey( int virtualKey, int scancode, int sym, qboolean down )
{
	keyNum_t key = 0;

	if( scancode >= SC_1 && scancode <= SC_0 )
	{
		// Always map the number keys as such even if they actually map
		// to other characters (eg, "1" is "&" on an AZERTY keyboard).
		if( scancode == SC_0 )
			key = '0';
		else
			key = '1' + scancode - SC_1;
	}
	else if( sym >= ' ' && sym < '\x7F' )
	{
		// These happen to match the ASCII chars
		key = sym;
	}
	else if( scancode == SC_GRAVE )
	{
		// Added in OPM
		// so the console can be toggled properly
		key = '`';
	}
	else
	{
		switch( sym )
		{
			case SYM( SC_PAGEUP ):       key = K_PGUP;          break;
			case SYM( SC_KP_9 ):         key = K_KP_PGUP;       break;
			case SYM( SC_PAGEDOWN ):     key = K_PGDN;          break;
			case SYM( SC_KP_3 ):         key = K_KP_PGDN;       break;
			case SYM( SC_KP_7 ):         key = K_KP_HOME;       break;
			case SYM( SC_HOME ):         key = K_HOME;          break;
			case SYM( SC_KP_1 ):         key = K_KP_END;        break;
			case SYM( SC_END ):          key = K_END;           break;
			case SYM( SC_KP_4 ):         key = K_KP_LEFTARROW;  break;
			case SYM( SC_LEFT ):         key = K_LEFTARROW;     break;
			case SYM( SC_KP_6 ):         key = K_KP_RIGHTARROW; break;
			case SYM( SC_RIGHT ):        key = K_RIGHTARROW;    break;
			case SYM( SC_KP_2 ):         key = K_KP_DOWNARROW;  break;
			case SYM( SC_DOWN ):         key = K_DOWNARROW;     break;
			case SYM( SC_KP_8 ):         key = K_KP_UPARROW;    break;
			case SYM( SC_UP ):           key = K_UPARROW;       break;
			case '\x1B':                 key = K_ESCAPE;        break;
			case SYM( SC_KP_ENTER ):     key = K_KP_ENTER;      break;
			case '\r':                   key = K_ENTER;         break;
			case '\t':                   key = K_TAB;           break;
			case SYM( SC_F1 ):           key = K_F1;            break;
			case SYM( SC_F2 ):           key = K_F2;            break;
			case SYM( SC_F3 ):           key = K_F3;            break;
			case SYM( SC_F4 ):           key = K_F4;            break;
			case SYM( SC_F5 ):           key = K_F5;            break;
			case SYM( SC_F6 ):           key = K_F6;            break;
			case SYM( SC_F7 ):           key = K_F7;            break;
			case SYM( SC_F8 ):           key = K_F8;            break;
			case SYM( SC_F9 ):           key = K_F9;            break;
			case SYM( SC_F10 ):          key = K_F10;           break;
			case SYM( SC_F11 ):          key = K_F11;           break;
			case SYM( SC_F12 ):          key = K_F12;           break;
			case SYM( SC_F13 ):          key = K_F13;           break;
			case SYM( SC_F14 ):          key = K_F14;           break;
			case SYM( SC_F15 ):          key = K_F15;           break;

			case '\b':                   key = K_BACKSPACE;     break;
			case SYM( SC_KP_PERIOD ):    key = K_KP_DEL;        break;
			case '\x7F':                 key = K_DEL;           break;
			case SYM( SC_PAUSE ):        key = K_PAUSE;         break;

			case SYM( SC_LSHIFT ):
			case SYM( SC_RSHIFT ):       key = K_SHIFT;         break;

			case SYM( SC_LCTRL ):
			case SYM( SC_RCTRL ):        key = K_CTRL;          break;

			case SYM( SC_RGUI ):
			case SYM( SC_LGUI ):         key = K_COMMAND;       break;

			case SYM( SC_RALT ):
			case SYM( SC_LALT ):         key = K_ALT;           break;

			case SYM( SC_KP_5 ):         key = K_KP_5;          break;
			case SYM( SC_INSERT ):       key = K_INS;           break;
			case SYM( SC_KP_0 ):         key = K_KP_INS;        break;
			case SYM( SC_KP_MULTIPLY ):  key = K_KP_STAR;       break;
			case SYM( SC_KP_PLUS ):      key = K_KP_PLUS;       break;
			case SYM( SC_KP_MINUS ):     key = K_KP_MINUS;      break;
			case SYM( SC_KP_DIVIDE ):    key = K_KP_SLASH;      break;

			case SYM( SC_MODE ):         key = K_MODE;          break;
			case SYM( SC_HELP ):         key = K_HELP;          break;
			case SYM( SC_PRINTSCREEN ):  key = K_PRINT;         break;
			case SYM( SC_SYSREQ ):       key = K_SYSREQ;        break;
			case SYM( SC_MENU ):         key = K_MENU;          break;
			case SYM( SC_APPLICATION ):  key = K_MENU;          break;
			case SYM( SC_POWER ):        key = K_POWER;         break;
			case SYM( SC_UNDO ):         key = K_UNDO;          break;
			case SYM( SC_SCROLLLOCK ):   key = K_SCROLLOCK;     break;
			case SYM( SC_NUMLOCKCLEAR ): key = K_KP_NUMLOCK;    break;
			case SYM( SC_CAPSLOCK ):     key = K_CAPSLOCK;      break;

			default:
				if( !SYM_IS_SCANCODE( sym ) && scancode <= 95 )
				{
					// Map Unicode characters to 95 world keys using the key's scan code.
					// FIXME: There aren't enough world keys to cover all the scancodes.
					// Maybe create a map of scancode to quake key at start up and on
					// key map change; allocate world key numbers as needed similar
					// to SDL 1.2.
					key = K_WORLD_0 + scancode;
				}
				break;
		}
	}

	if( in_keyboardDebug->integer )
		IN_PrintKey( virtualKey, scancode, sym, key, down );

	if( IN_IsConsoleKey( key, 0 ) )
	{
		// Console keys can't be bound or generate characters
		key = K_CONSOLE;
	}

	return key;
}

/*
===============
IN_UpdateModState

SDL's modifier state after a key event.
===============
*/
static void IN_UpdateModState( int sym, qboolean down )
{
	int modifier;

	switch( sym )
	{
		case SYM( SC_LCTRL ):  modifier = KMOD_LCTRL;  break;
		case SYM( SC_RCTRL ):  modifier = KMOD_RCTRL;  break;
		case SYM( SC_LSHIFT ): modifier = KMOD_LSHIFT; break;
		case SYM( SC_RSHIFT ): modifier = KMOD_RSHIFT; break;
		case SYM( SC_LALT ):   modifier = KMOD_LALT;   break;
		case SYM( SC_RALT ):   modifier = KMOD_RALT;   break;
		case SYM( SC_LGUI ):   modifier = KMOD_LGUI;   break;
		case SYM( SC_RGUI ):   modifier = KMOD_RGUI;   break;
		case SYM( SC_MODE ):   modifier = KMOD_MODE;   break;
		default:               modifier = 0;           break;
	}

	if( !down )
		in_modState &= ~modifier;
	else if( sym == SYM( SC_NUMLOCKCLEAR ) )
		in_modState ^= KMOD_NUM;
	else if( sym == SYM( SC_CAPSLOCK ) )
		in_modState ^= KMOD_CAPS;
	else if( sym == SYM( SC_SCROLLLOCK ) )
		in_modState ^= KMOD_SCROLL;
	else
		in_modState |= modifier;
}

/*
===============
IN_SendKey

A key went down or up: SDL_SendKeyboardKey, then sdl_input.c's SDL_KEYDOWN or SDL_KEYUP. An unknown key sends
nothing; a key that is already down sends a repeat, dropped unless a key catcher is up; a key going up that isn't
down sends nothing.
===============
*/
static void IN_SendKey( int virtualKey, int scancode, qboolean down )
{
	qboolean repeat = qfalse;
	keyNum_t key;
	int sym;

	if( scancode <= 0 || scancode >= SC_NUM )
		return;

	if( down )
		repeat = in_keyState[ scancode ] != 0;
	else if( !in_keyState[ scancode ] )
		return;

	in_keyState[ scancode ] = down;
	sym = in_keymap[ scancode ];
	IN_UpdateModState( sym, down );

	if( down )
	{
		if( repeat && Key_GetCatcher( ) == 0 )
			return;

		if( ( key = IN_TranslateKey( virtualKey, scancode, sym, qtrue ) ) )
			Com_QueueEvent( in_eventTime, SE_KEY, key, qtrue, 0, NULL );

		if( key == K_BACKSPACE )
			Com_QueueEvent( in_eventTime, SE_CHAR, CTRL('h'), 0, 0, NULL );
		else if (key == K_ENTER || key == K_TAB || key == K_ESCAPE) // Added in OPM: mostly used by UINotepad
			Com_QueueEvent( in_eventTime, SE_CHAR, key, 0, 0, NULL);
		else if( keys[K_CTRL].down && key >= 'a' && key <= 'z' )
			Com_QueueEvent( in_eventTime, SE_CHAR, CTRL(key), 0, 0, NULL );

		lastKeyDown = key;
	}
	else
	{
		if( ( key = IN_TranslateKey( virtualKey, scancode, sym, qfalse ) ) )
			Com_QueueEvent( in_eventTime, SE_KEY, key, qfalse, 0, NULL );

		lastKeyDown = 0;
	}
}

/*
===============
IN_IsModifierPressed

SDL's IsModifierKeyPressed: one side of a modifier from the device-dependent bits, trusting the device-independent
bit when the two disagree (events without device bits).
===============
*/
static qboolean IN_IsModifierPressed( unsigned int flags, unsigned int side, unsigned int otherSide,
	unsigned int either )
{
	qboolean sidePressed = ( flags & side ) != 0;
	qboolean otherPressed = ( flags & otherSide ) != 0;
	qboolean eitherPressed = ( flags & either ) != 0;

	if( eitherPressed != ( sidePressed || otherPressed ) )
		return eitherPressed;

	return sidePressed;
}

/*
===============
IN_SendModifiers

A modifier changed: as SDL, every modifier side is set from the flags in this order (a side that is still down
sends a repeat), then caps lock sends a press and a release when it toggled.
===============
*/
static void IN_SendModifiers( unsigned int flags )
{
	static const struct
	{
		int				scancode;
		unsigned int	side, otherSide, either;
	} modifiers[] =
	{
		{ SC_LSHIFT, NX_DEVICELSHIFTKEYMASK, NX_DEVICERSHIFTKEYMASK, NSEventModifierFlagShift },
		{ SC_LCTRL,  NX_DEVICELCTLKEYMASK,   NX_DEVICERCTLKEYMASK,   NSEventModifierFlagControl },
		{ SC_LALT,   NX_DEVICELALTKEYMASK,   NX_DEVICERALTKEYMASK,   NSEventModifierFlagOption },
		{ SC_LGUI,   NX_DEVICELCMDKEYMASK,   NX_DEVICERCMDKEYMASK,   NSEventModifierFlagCommand },
		{ SC_RSHIFT, NX_DEVICERSHIFTKEYMASK, NX_DEVICELSHIFTKEYMASK, NSEventModifierFlagShift },
		{ SC_RCTRL,  NX_DEVICERCTLKEYMASK,   NX_DEVICELCTLKEYMASK,   NSEventModifierFlagControl },
		{ SC_RALT,   NX_DEVICERALTKEYMASK,   NX_DEVICELALTKEYMASK,   NSEventModifierFlagOption },
		{ SC_RGUI,   NX_DEVICERCMDKEYMASK,   NX_DEVICELCMDKEYMASK,   NSEventModifierFlagCommand },
	};
	int i;

	for( i = 0; i < ARRAY_LEN( modifiers ); i++ )
	{
		IN_SendKey( -1, modifiers[ i ].scancode, IN_IsModifierPressed( flags, modifiers[ i ].side,
			modifiers[ i ].otherSide, modifiers[ i ].either ) );
	}

	// caps lock is a toggle: AppKit tells its state, not the key's
	if( ( ( flags & NSEventModifierFlagCapsLock ) != 0 ) != ( ( in_modState & KMOD_CAPS ) != 0 ) )
	{
		IN_SendKey( -1, SC_CAPSLOCK, qtrue );
		IN_SendKey( -1, SC_CAPSLOCK, qfalse );
	}
}

/*
===============
IN_ResetKeyboard

Every key that is down goes up (SDL_ResetKeyboard): the keyboard is gone with the focus or the window.
===============
*/
static void IN_ResetKeyboard( void )
{
	int scancode;

	for( scancode = 0; scancode < SC_NUM; scancode++ )
	{
		if( in_keyState[ scancode ] )
			IN_SendKey( -1, scancode, qfalse );
	}
}

/*
===============
IN_SyncCapsLock

The window got the keyboard: caps lock may have changed meanwhile (SDL sets its state without an event).
===============
*/
static void IN_SyncCapsLock( void )
{
	if( [NSEvent modifierFlags] & NSEventModifierFlagCapsLock )
		in_modState |= KMOD_CAPS;
	else
		in_modState &= ~KMOD_CAPS;
}

/*
===============
IN_TextClient

The window's text input client (the content view, macos_window.m).
===============
*/
static NSView<NSTextInputClient> *IN_TextClient( void )
{
	NSView *view = in_window.contentView;

	return [view conformsToProtocol:@protocol(NSTextInputClient)] ? (NSView<NSTextInputClient> *)view : nil;
}

/*
===============
MacOS_KeyEvent

A key or modifier event from the pump, before AppKit dispatches it (SDL's Cocoa_HandleKeyEvent). A key going down
then also goes through the text input client, which turns it into text (MacOS_InsertText).
===============
*/
void MacOS_KeyEvent( NSEvent *event )
{
	int virtualKey = event.keyCode;
	int scancode;

	if( !in_window )
		return;

	if( ( virtualKey == kVK_ISO_Section || virtualKey == kVK_ANSI_Grave ) && IN_IsISOKeyboard( ) )
		virtualKey = kVK_ISO_Section + kVK_ANSI_Grave - virtualKey;

	scancode = virtualKey < ARRAY_LEN( in_vkToScancode ) ? in_vkToScancode[ virtualKey ] : 0;

	switch( event.type )
	{
		case NSEventTypeKeyDown:
			if( !event.ARepeat )
				IN_UpdateKeymap( );

			IN_SendKey( virtualKey, scancode, qtrue );

			[IN_TextClient( ) interpretKeyEvents:@[ event ]];
			break;

		case NSEventTypeKeyUp:
			IN_SendKey( virtualKey, scancode, qfalse );
			break;

		case NSEventTypeFlagsChanged:
			IN_SendModifiers( (unsigned int)event.modifierFlags );
			break;

		default:
			break;
	}
}

/*
===============
MacOS_InsertText

Text from the text input client: sdl_input.c's SDL_TEXTINPUT. A console character opens the console instead, and
the text of the key that toggled the console is dropped.
===============
*/
void MacOS_InsertText( NSString *text )
{
	const char *c = text.UTF8String;

	// no text event for an unprintable character (SDL_SendKeyboardText)
	if( !c || (unsigned char)*c < ' ' || *c == 127 )
		return;

	if( lastKeyDown == K_CONSOLE )
		return;

	// Quick and dirty UTF-8 to UTF-32 conversion
	while( *c )
	{
		int utf32 = 0;

		if( ( *c & 0x80 ) == 0 )
			utf32 = *c++;
		else if( ( *c & 0xE0 ) == 0xC0 ) // 110x xxxx
		{
			utf32 |= ( *c++ & 0x1F ) << 6;
			utf32 |= ( *c++ & 0x3F );
		}
		else if( ( *c & 0xF0 ) == 0xE0 ) // 1110 xxxx
		{
			utf32 |= ( *c++ & 0x0F ) << 12;
			utf32 |= ( *c++ & 0x3F ) << 6;
			utf32 |= ( *c++ & 0x3F );
		}
		else if( ( *c & 0xF8 ) == 0xF0 ) // 1111 0xxx
		{
			utf32 |= ( *c++ & 0x07 ) << 18;
			utf32 |= ( *c++ & 0x3F ) << 12;
			utf32 |= ( *c++ & 0x3F ) << 6;
			utf32 |= ( *c++ & 0x3F );
		}
		else
		{
			Com_DPrintf( "Unrecognised UTF-8 lead byte: 0x%x\n", (unsigned int)*c );
			c++;
		}

		if( utf32 != 0 )
		{
			if( IN_IsConsoleKey( 0, utf32 ) )
			{
				Com_QueueEvent( in_eventTime, SE_KEY, K_CONSOLE, qtrue, 0, NULL );
				Com_QueueEvent( in_eventTime, SE_KEY, K_CONSOLE, qfalse, 0, NULL );
			}
			else
				Com_QueueEvent( in_eventTime, SE_CHAR, utf32, 0, 0, NULL );
		}
	}
}

/*
==============================================================================

THE WINDOW

==============================================================================
*/

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
IN_WindowFocused

The window got or lost the keyboard. Losing it releases every key that is down and drops any text being composed
(SDL resets its keyboard and stops text input).
===============
*/
static void IN_WindowFocused( qboolean focused )
{
	if( focused )
		IN_SyncCapsLock( );
	else
	{
		NSView<NSTextInputClient> *client = IN_TextClient( );

		IN_ResetKeyboard( );
		[client.inputContext discardMarkedText];
		[client unmarkText];
	}

	Cvar_SetValue( "com_unfocused", !focused );
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
			usingBlock:^( NSNotification *n ) { IN_WindowFocused( qtrue ); }],
		[center addObserverForName:NSWindowDidResignKeyNotification object:in_window queue:queue
			usingBlock:^( NSNotification *n ) { IN_WindowFocused( qfalse ); }],
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
IN_StopObservingWindow
===============
*/
static void IN_StopObservingWindow( void )
{
	for( id observer in in_windowObservers )
		[[NSNotificationCenter defaultCenter] removeObserver:observer];
	in_windowObservers = nil;
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

	// Set event time for next frame to earliest possible time an event could happen
	in_eventTime = Sys_Milliseconds( );

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

	in_keyboardDebug = Cvar_Get( "in_keyboardDebug", "0", CVAR_ARCHIVE );

	// mouse variables (registered as SDL registers them, so configs keep them; used from slice 6d on)
	Cvar_Get( "in_mouse", "1", CVAR_ARCHIVE );
	Cvar_Get( "in_nograb", "0", CVAR_ARCHIVE );

	Cvar_Get( "in_joystick", "0", CVAR_ARCHIVE|CVAR_LATCH );
	Cvar_Get( "joy_threshold", "0.15", CVAR_ARCHIVE );

	IN_UpdateKeymap( );
	IN_SyncCapsLock( );

	IN_ObserveWindow( );

	Cvar_SetValue( "com_unfocused",	!in_window.keyWindow );
	Cvar_SetValue( "com_minimized", in_window.miniaturized );

	Com_DPrintf( "------------------------------------\n" );
}

/*
===============
IN_Shutdown

The window goes away (vid_restart or quit): the keys that are down go up. SDL queued the same key releases when it
destroyed its window, but lost them with its event queue at vid_restart, which left them down in the client.
===============
*/
void IN_Shutdown( void )
{
	IN_ResetKeyboard( );

	IN_StopObservingWindow( );

	in_window = nil;
}

/*
===============
IN_Restart

in_restart keeps the window and the keyboard's state.
===============
*/
void IN_Restart( void )
{
	IN_StopObservingWindow( );
	IN_Init( (__bridge void *)in_window );
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
