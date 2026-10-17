// mousemove.c -- synthetic mouse movement for the cl_latencyStats probe (port/latency.sh builds and runs it).
//
// Posts mouse-moved events at the HID event tap (CGEventPost(kCGHIDEventTap)): from there they take the same
// path as a real mouse's events (WindowServer -> the application's event queue -> SDL); only the mouse itself,
// the USB transfer and the HID driver are skipped. Each event carries its creation time as its timestamp,
// which the game sees as NSEvent.timestamp. The posting process needs the Accessibility permission
// (CGPreflightPostEventAccess); without it macOS drops the events silently, so this checks first.
//
//   mousemove <seconds> [rate_hz (1000)] [dx per event (2)] [sweep_ms (400)]
//
// The horizontal direction flips every sweep_ms, so the view swings back and forth around where it started.
// Events are paced on absolute mach deadlines, like a 1000 Hz gaming mouse.

#include <ApplicationServices/ApplicationServices.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

int main( int argc, char **argv )
{
	double seconds = argc > 1 ? atof( argv[1] ) : 10.0;
	double rate = argc > 2 ? atof( argv[2] ) : 1000.0;
	int dx = argc > 3 ? atoi( argv[3] ) : 2;
	double sweepMs = argc > 4 ? atof( argv[4] ) : 400.0;
	mach_timebase_info_data_t tb;
	uint64_t start, interval, deadline, now, late, maxLate = 0, totalLate = 0;
	long count, total, perSweep, i;
	CGEventRef probe;
	CGPoint where;

	if ( seconds <= 0 || rate <= 0 || rate > 8000 ) {
		fprintf( stderr, "usage: mousemove <seconds> [rate_hz] [dx] [sweep_ms]\n" );
		return 2;
	}
	if ( !CGPreflightPostEventAccess() ) {
		fprintf( stderr, "mousemove: this process may not post events (System Settings > Privacy & Security > Accessibility)\n" );
		return 1;
	}

	pthread_set_qos_class_self_np( QOS_CLASS_USER_INTERACTIVE, 0 );
	mach_timebase_info( &tb );

	probe = CGEventCreate( NULL );
	where = CGEventGetLocation( probe );
	CFRelease( probe );

	interval = (uint64_t)( 1e9 / rate * tb.denom / tb.numer );
	total = (long)( seconds * rate );
	perSweep = (long)( sweepMs / 1000.0 * rate );
	if ( perSweep < 1 ) {
		perSweep = 1;
	}

	start = mach_absolute_time();
	for ( i = 0, count = 0; i < total; i++ ) {
		CGEventRef ev;
		int dir = ( ( i / perSweep ) & 1 ) ? -1 : 1;

		deadline = start + (uint64_t)i * interval;
		mach_wait_until( deadline );
		now = mach_absolute_time();
		late = now - deadline;
		totalLate += late;
		if ( late > maxLate ) {
			maxLate = late;
		}

		ev = CGEventCreateMouseEvent( NULL, kCGEventMouseMoved, where, kCGMouseButtonLeft );
		if ( !ev ) {
			continue;
		}
		CGEventSetIntegerValueField( ev, kCGMouseEventDeltaX, dir * dx );
		CGEventSetIntegerValueField( ev, kCGMouseEventDeltaY, 0 );
		CGEventPost( kCGHIDEventTap, ev );
		CFRelease( ev );
		count++;
	}

	printf( "mousemove: %ld events in %.3f s (%.0f Hz asked), dx %d, sweep %.0f ms; wake-up lateness avg %.1f us, max %.1f us\n",
		count, (double)( mach_absolute_time() - start ) * tb.numer / tb.denom / 1e9, rate, dx, sweepMs,
		count ? (double)totalLate * tb.numer / tb.denom / 1e3 / count : 0.0,
		(double)maxLate * tb.numer / tb.denom / 1e3 );
	return 0;
}
