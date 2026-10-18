// gcmtest.m -- Phase 6d: GCMouse against the NSEvent path, with the user's own mouse (GCMouse reads the HID
// devices below the WindowServer, so posted or synthetic events never reach it). A small window captures the
// cursor (detached and hidden, like the game) for ~35 s and prompts: move up, move right, click left, click right,
// move freely; Escape ends early. Per phase it prints the sums of both paths' deltas (the y direction, the scale)
// and the timing:
//   - each GCMouse handler call: its lastEventTimestamp against the handler's own CLOCK_UPTIME_RAW time (is it
//     that clock, and how late does the handler run);
//   - each click, paired by order: GCMouse's button handler (time and lastEventTimestamp) against the NSEvent's
//     timestamp and the time the main thread got it.
// Build and run (port/env.sh first):
//   clang -fobjc-arc -O2 -framework AppKit -framework GameController port/gcmtest.m -o build.noindex/gcmtest
//   build.noindex/gcmtest | tee build.noindex/gcmtest.log

#import <AppKit/AppKit.h>
#import <GameController/GameController.h>
#include <os/lock.h>
#include <time.h>

typedef struct
{
	const char	*prompt;
	double		seconds;
} phase_t;

static const phase_t phases[] =
{
	{ "Get ready: the cursor is captured. Keep the mouse still.", 3 },
	{ "Move the mouse UP (away from you), slowly, a few times.", 5 },
	{ "Move the mouse RIGHT, slowly, a few times.", 5 },
	{ "Click the LEFT button 6 times, one click per second.", 7 },
	{ "Click the RIGHT button 6 times, one click per second.", 7 },
	{ "Move the mouse around freely, fast and slow.", 6 },
};
#define NUM_PHASES ( sizeof( phases ) / sizeof( phases[ 0 ] ) )

#define MAX_CLICKS 64

typedef struct
{
	double	nsDX, nsDY;			// NSEvent deltas
	int		nsEvents;
	int		nsSlow;				// events with |delta| < 1 on both axes
	double	gcDX, gcDY;			// GCMouse deltas
	int		gcEvents;
	double	gcLagSum, gcLagMax;	// handler time - lastEventTimestamp, us
	double	gcLagMin;
} phaseStats_t;

typedef struct
{
	double	gcNow, gcStamp;		// GCMouse button handler: its time, lastEventTimestamp (us)
	double	nsStamp, nsNow;		// NSEvent: timestamp, the main thread's time (us)
} click_t;

static phaseStats_t		stats[ NUM_PHASES ];
static int				phase;
static os_unfair_lock	lock = OS_UNFAIR_LOCK_INIT;
static click_t			clicks[ 2 ][ MAX_CLICKS ];	// [0] left, [1] right
static int				gcClicks[ 2 ], nsClicks[ 2 ];
static dispatch_queue_t	queue;
static NSTextField		*label;
static NSWindow			*window;
static NSMutableSet		*watched;

static double NowUsec( void )
{
	return clock_gettime_nsec_np( CLOCK_UPTIME_RAW ) / 1000.0;
}

static void WatchMouse( GCMouse *mouse )
{
	GCMouseInput *input = mouse.mouseInput;

	if( !input || [watched containsObject:mouse] )
		return;
	[watched addObject:mouse];
	printf( "GCMouse: %s\n", mouse.vendorName.UTF8String ?: "(no name)" );

	mouse.handlerQueue = queue;
	input.mouseMovedHandler = ^( GCMouseInput *m, float deltaX, float deltaY )
	{
		double now = NowUsec( ), lag = now - m.lastEventTimestamp * 1e6;
		phaseStats_t *s;

		os_unfair_lock_lock( &lock );
		s = &stats[ phase ];
		s->gcDX += deltaX;
		s->gcDY += deltaY;
		if( !s->gcEvents || lag < s->gcLagMin )
			s->gcLagMin = lag;
		if( lag > s->gcLagMax )
			s->gcLagMax = lag;
		s->gcLagSum += lag;
		s->gcEvents++;
		os_unfair_lock_unlock( &lock );
	};

	void ( ^button )( int, GCControllerButtonInput *, BOOL ) = ^( int which, GCControllerButtonInput *b, BOOL pressed )
	{
		double now = NowUsec( );

		if( !pressed )
			return;
		os_unfair_lock_lock( &lock );
		if( gcClicks[ which ] < MAX_CLICKS )
		{
			clicks[ which ][ gcClicks[ which ] ].gcNow = now;
			clicks[ which ][ gcClicks[ which ] ].gcStamp = mouse.mouseInput.lastEventTimestamp * 1e6;
			gcClicks[ which ]++;
		}
		os_unfair_lock_unlock( &lock );
	};
	input.leftButton.pressedChangedHandler = ^( GCControllerButtonInput *b, float value, BOOL pressed )
	{
		button( 0, b, pressed );
	};
	input.rightButton.pressedChangedHandler = ^( GCControllerButtonInput *b, float value, BOOL pressed )
	{
		button( 1, b, pressed );
	};
}

static void NSEventSeen( NSEvent *event )
{
	double now = NowUsec( );
	int which = -1;

	switch( event.type )
	{
		case NSEventTypeMouseMoved:
		case NSEventTypeLeftMouseDragged:
		case NSEventTypeRightMouseDragged:
		case NSEventTypeOtherMouseDragged:
			os_unfair_lock_lock( &lock );
			stats[ phase ].nsDX += event.deltaX;
			stats[ phase ].nsDY += event.deltaY;
			stats[ phase ].nsEvents++;
			if( fabs( event.deltaX ) < 1 && fabs( event.deltaY ) < 1 )
				stats[ phase ].nsSlow++;
			os_unfair_lock_unlock( &lock );
			break;

		case NSEventTypeLeftMouseDown:
			which = 0;
			break;

		case NSEventTypeRightMouseDown:
			which = 1;
			break;

		case NSEventTypeKeyDown:
			if( event.keyCode == 53 )	// escape
				phase = NUM_PHASES - 1;
			break;

		default:
			break;
	}

	if( which >= 0 )
	{
		os_unfair_lock_lock( &lock );
		if( nsClicks[ which ] < MAX_CLICKS )
		{
			clicks[ which ][ nsClicks[ which ] ].nsStamp = event.timestamp * 1e6;
			clicks[ which ][ nsClicks[ which ] ].nsNow = now;
			nsClicks[ which ]++;
		}
		os_unfair_lock_unlock( &lock );
	}
}

static void Report( void )
{
	static const char *buttons[] = { "left", "right" };
	int i, b;

	printf( "\nphase: NSEvent events (below 1 point), sum dx dy | GCMouse events, sum dx dy, handler - lastEventTimestamp us (min/avg/max)\n" );
	for( i = 1; i < NUM_PHASES; i++ )
	{
		phaseStats_t *s = &stats[ i ];

		printf( "%d %-55s ns %4d (%4d) %9.2f %9.2f | gc %4d %9.2f %9.2f | %.0f/%.0f/%.0f\n", i, phases[ i ].prompt,
			s->nsEvents, s->nsSlow, s->nsDX, s->nsDY, s->gcEvents, s->gcDX, s->gcDY, s->gcLagMin,
			s->gcEvents ? s->gcLagSum / s->gcEvents : 0, s->gcLagMax );
	}

	for( b = 0; b < 2; b++ )
	{
		int n = gcClicks[ b ] < nsClicks[ b ] ? gcClicks[ b ] : nsClicks[ b ];

		printf( "\n%s clicks: GCMouse %d, NSEvent %d (paired by order)\n", buttons[ b ], gcClicks[ b ], nsClicks[ b ] );
		printf( "  gc handler - ns timestamp | gc lastEventTimestamp - ns timestamp | ns main thread - ns timestamp | gc handler - ns main thread (us)\n" );
		for( i = 0; i < n; i++ )
		{
			click_t *c = &clicks[ b ][ i ];

			printf( "  %8.0f %8.0f %8.0f %8.0f\n", c->gcNow - c->nsStamp, c->gcStamp - c->nsStamp, c->nsNow - c->nsStamp,
				c->gcNow - c->nsNow );
		}
	}
	fflush( stdout );
}

@interface TestDelegate : NSObject <NSApplicationDelegate>
@end

@implementation TestDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)n
{
	NSRect frame = NSMakeRect( 0, 0, 640, 200 );
	NSRect screen = NSScreen.mainScreen.frame;
	CGPoint center;

	window = [[NSWindow alloc] initWithContentRect:frame styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered
		defer:NO];
	window.title = @"GCMouse test (Escape ends)";
	window.acceptsMouseMovedEvents = YES;
	[window center];
	label = [NSTextField labelWithString:@""];
	label.font = [NSFont systemFontOfSize:20];
	label.frame = NSInsetRect( frame, 20, 20 );
	label.lineBreakMode = NSLineBreakByWordWrapping;
	[window.contentView addSubview:label];
	[window makeKeyAndOrderFront:nil];
	[NSApp activateIgnoringOtherApps:YES];

	// capture the cursor as the game does: at the window's center, detached from the mouse, hidden
	center = CGPointMake( NSMidX( window.frame ), NSMaxY( screen ) - NSMidY( window.frame ) );
	CGWarpMouseCursorPosition( center );
	CGAssociateMouseAndMouseCursorPosition( false );
	[NSCursor hide];

	queue = dispatch_queue_create( "gcmtest", dispatch_queue_attr_make_with_qos_class( DISPATCH_QUEUE_SERIAL,
		QOS_CLASS_USER_INTERACTIVE, 0 ) );
	watched = [NSMutableSet set];
	[[NSNotificationCenter defaultCenter] addObserverForName:GCMouseDidConnectNotification object:nil
		queue:[NSOperationQueue mainQueue] usingBlock:^( NSNotification *note ) { WatchMouse( note.object ); }];
	for( GCMouse *mouse in GCMouse.mice )
		WatchMouse( mouse );

	[NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskAny handler:^NSEvent *( NSEvent *event )
	{
		NSEventSeen( event );
		return event;
	}];

	[self runPhase:@0];
}

- (void)runPhase:(NSNumber *)number
{
	int next = number.intValue;

	if( next < phase )	// Escape jumped to the end
		next = phase + 1;
	if( next >= (int)NUM_PHASES )
	{
		CGAssociateMouseAndMouseCursorPosition( true );
		[NSCursor unhide];
		Report( );
		[NSApp terminate:nil];
		return;
	}

	os_unfair_lock_lock( &lock );
	phase = next;
	os_unfair_lock_unlock( &lock );
	label.stringValue = [NSString stringWithFormat:@"%d/%d: %s", next, (int)NUM_PHASES - 1, phases[ next ].prompt];
	printf( "%s\n", phases[ next ].prompt );
	fflush( stdout );
	[self performSelector:@selector(runPhase:) withObject:@( next + 1 ) afterDelay:phases[ next ].seconds];
}

@end

int main( void )
{
	@autoreleasepool
	{
		TestDelegate *delegate = [[TestDelegate alloc] init];

		[NSApplication sharedApplication];
		[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
		NSApp.delegate = delegate;
		[NSApp run];
	}
	return 0;
}
