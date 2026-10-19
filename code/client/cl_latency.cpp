/*
===========================================================================
Copyright (C) 2026 the OpenMoHAA Metal port contributors

This file is part of OpenMoHAA source code.

OpenMoHAA source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

OpenMoHAA source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenMoHAA source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// cl_latency.cpp -- cl_latencyStats: how long mouse input takes to reach the screen.
//
// Every frame whose usercmd took in new mouse movement (CL_MouseMove) is followed through these times:
//   event    the OS timestamps of the oldest and newest mouse event delivered by that frame's input pump
//            (NSEvent.timestamp: when the event entered the system; a real mouse's polling interval and USB
//            transfer come before it);
//   pump     when IN_ProcessEvents started pumping the events that delivered it: the frame's input sample;
//   commit   when the renderer committed the frame to the GPU;
//   GPU done when the GPU finished the frame (MTLCommandBuffer.GPUEndTime);
//   present  when the first frame at or after it was shown on screen (Core Animation's presentedTime: the
//            display swap that starts showing it). A frame that skipped its present (no drawable ready) or was
//            replaced before the compositor took it shows its input only with a later frame, so that frame's
//            present time counts: the moment the input really became visible.
// And by time on screen: the age of the newest mouse event in the image on screen (since its OS timestamp), sampled
// every 10 us from one present to the next while newer input exists (the mouse moves): what a continuous movement
// looks like on the display. Unlike the per-frame times it includes how long each image stays up, so it shows what
// presenting more often gains.
// GL1 can't report these times; there the probe ends when EndFrame returns (after glFinish and SwapBuffers),
// a lower bound. All times are microseconds on the mach_absolute_time clock (Core Animation's host time).
// Percentiles come from 10 us histograms: printed every cl_latencyStatsFrames input frames, and for the whole
// run when the probe is turned off or the client shuts down. cl_latencyStats 2 also prints every frame the
// renderer reports, with its raw times (to check them against Instruments' display trace). The probe only reads
// clocks: frame pacing and usercmd timing don't change.

#include "client.h"
#include <time.h>

#define LAT_BIN_USEC	10
#define LAT_BINS		4000		// 0..40 ms; the last bin also takes everything longer
#define LAT_STALE_USEC	1000000		// longer waits are loads or restarts, not latency: not sampled
#define LAT_RING		256			// power of two

enum {
	LAT_EVENT_PUMP,		// newest event -> pump
	LAT_OLDEST_PUMP,	// oldest event -> pump
	LAT_PUMP_COMMIT,	// pump -> commit
	LAT_PUMP_GPU,		// pump -> GPU done
	LAT_PUMP_PRESENT,	// pump -> present (GL1: EndFrame return)
	LAT_EVENT_PRESENT,	// newest event -> present (GL1: EndFrame return)
	LAT_EVENT_SCREEN,	// the newest event's age on screen, by time (10 us samples; not GL1)
	LAT_NUM
};

typedef struct {
	unsigned int	count;
	unsigned int	bins[LAT_BINS];
} latHistogram_t;

typedef struct {
	latHistogram_t	hist[LAT_NUM];
	int				frames;			// frames with mouse input
	int				shownOwn;		// ... whose input became visible with their own frame
	int				shownLater;		// ... with a later frame
	int				lost;			// the renderer never reported the frame
	int				stale;			// their input waited too long to be shown (loads, restarts)
	int				presented;		// frames (with or without input) that were presented
	int				neverShown;		// ... that Core Animation reported as never shown
} latStats_t;

typedef struct {
	int			tag;
	long long	pump;						// 0 = no input
	long long	eventOldest, eventNewest;	// 0 = unknown
} latInput_t;

static cvar_t		*cl_latencyStats;
static cvar_t		*cl_latencyStatsFrames;

static qboolean		lat_on;
static qboolean		lat_rendererTimes;		// the renderer reports commit, GPU and present times
static long long	lat_pump;				// when the current event pump started
static latInput_t	lat_pending;			// input in cl.mouseDx, not yet in a usercmd
static latInput_t	lat_frame;				// input in this frame's usercmd
static int			lat_endFrameTag;		// tag of the frame in EndFrame, 0 = no input
static int			lat_nextTag;
static latInput_t	lat_tagged[LAT_RING];	// tagged frames waiting for the renderer's report, by tag
static latInput_t	lat_waiting[LAT_RING];	// reported frames waiting for a frame that is shown
static int			lat_numWaiting;
static latStats_t	lat_window, lat_total;
static long long	lat_screenSince;		// the last present, 0 = none yet
static long long	lat_screenEvent;		// the newest event in the image on screen since then
static long long	lat_newestEvent;		// the newest event pumped

/*
==================
CL_LatencyClock
==================
*/
static long long CL_LatencyClock( void ) {
#ifdef __APPLE__
	// the clock of NSEvent timestamps and Core Animation's presentedTime
	return (long long)( clock_gettime_nsec_np( CLOCK_UPTIME_RAW ) / 1000 );
#else
	struct timespec ts;

	clock_gettime( CLOCK_MONOTONIC, &ts );
	return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
#endif
}

/*
==================
CL_LatencySample
==================
*/
static void CL_LatencySample( int metric, long long usec ) {
	int bin;

	bin = usec > 0 ? (int)( usec / LAT_BIN_USEC ) : 0;
	if ( bin >= LAT_BINS ) {
		bin = LAT_BINS - 1;
	}
	lat_window.hist[metric].bins[bin]++;
	lat_window.hist[metric].count++;
	lat_total.hist[metric].bins[bin]++;
	lat_total.hist[metric].count++;
}

#define LAT_COUNT( field )	( lat_window.field++, lat_total.field++ )

/*
==================
CL_LatencyInput

An input frame's own times are known (GL1: 'done' = EndFrame returned).
==================
*/
static void CL_LatencyInput( const latInput_t *in, long long commit, long long gpuDone, long long done ) {
	if ( in->eventNewest ) {
		CL_LatencySample( LAT_EVENT_PUMP, in->pump - in->eventNewest );
		CL_LatencySample( LAT_OLDEST_PUMP, in->pump - in->eventOldest );
	}
	if ( commit ) {
		CL_LatencySample( LAT_PUMP_COMMIT, commit - in->pump );
	}
	if ( gpuDone ) {
		CL_LatencySample( LAT_PUMP_GPU, gpuDone - in->pump );
	}
	if ( done ) {
		CL_LatencySample( LAT_PUMP_PRESENT, done - in->pump );
		if ( in->eventNewest ) {
			CL_LatencySample( LAT_EVENT_PRESENT, done - in->eventNewest );
		}
	}
	LAT_COUNT( frames );
}

/*
==================
CL_LatencyShown

An input frame's input became visible at 'shown'.
==================
*/
static void CL_LatencyShown( const latInput_t *in, long long shown, qboolean own ) {
	if ( shown - in->pump > LAT_STALE_USEC ) {
		LAT_COUNT( stale );
		return;
	}

	CL_LatencySample( LAT_PUMP_PRESENT, shown - in->pump );
	if ( in->eventNewest ) {
		CL_LatencySample( LAT_EVENT_PRESENT, shown - in->eventNewest );
	}
	if ( own ) {
		LAT_COUNT( shownOwn );
	} else {
		LAT_COUNT( shownLater );
	}
}

/*
==================
CL_LatencyPercentile

In ms (the middle of the 10 us bin); -1 when it falls into the last bin.
==================
*/
static float CL_LatencyPercentile( const latHistogram_t *h, float p ) {
	unsigned int rank, sum;
	int i;

	rank = (unsigned int)ceil( p * h->count );
	if ( rank < 1 ) {
		rank = 1;
	}

	sum = 0;
	for ( i = 0; i < LAT_BINS - 1; i++ ) {
		sum += h->bins[i];
		if ( sum >= rank ) {
			return ( i + 0.5f ) * LAT_BIN_USEC / 1000.0f;
		}
	}
	return -1;
}

/*
==================
CL_LatencyPrint
==================
*/
static void CL_LatencyPrint( const latStats_t *s, const char *what ) {
	static const char *labels[2][LAT_NUM] = {
		{ "newest event -> pump", "oldest event -> pump", NULL, NULL,
		  "pump -> EndFrame return", "newest event -> EndFrame return", NULL },
		{ "newest event -> pump", "oldest event -> pump", "pump -> commit", "pump -> GPU done",
		  "pump -> present", "newest event -> present", "input age on screen (by time)" }
	};
	static const float p[3] = { 0.50f, 0.95f, 0.99f };
	qboolean presentTimes = qtrue;
	int i, j;

	if ( lat_rendererTimes && s->neverShown * 2 > s->presented ) {
		// seen in fullscreen with vsync off, where Instruments shows those frames on the display
		Com_Printf( "cl_latencyStats, %s: %d frames with mouse input; %d lost. Core Animation reported %d of %d presented "
			"frames as never shown: its present times are not usable here, so the present lines are left out\n",
			what, s->frames, s->lost, s->neverShown, s->presented );
		presentTimes = qfalse;
	} else if ( lat_rendererTimes ) {
		Com_Printf( "cl_latencyStats, %s: %d frames with mouse input, %d shown by their own frame, %d by a later one; %d lost, %d stale\n",
			what, s->frames, s->shownOwn, s->shownLater, s->lost, s->stale );
	} else {
		Com_Printf( "cl_latencyStats, %s: %d frames with mouse input. The renderer reports no GPU or present times: "
			"lower bounds, ending when EndFrame returned (GL1: after glFinish and SwapBuffers)\n",
			what, s->frames );
	}
	Com_Printf( "  %-32s %7s %7s %7s\n", "ms", "p50", "p95", "p99" );

	for ( i = 0; i < LAT_NUM; i++ ) {
		const char *label = labels[lat_rendererTimes ? 1 : 0][i];

		if ( !label || !s->hist[i].count ) {
			continue;
		}
		if ( !presentTimes && ( i == LAT_PUMP_PRESENT || i == LAT_EVENT_PRESENT || i == LAT_EVENT_SCREEN ) ) {
			continue;
		}

		Com_Printf( "  %-32s", label );
		for ( j = 0; j < 3; j++ ) {
			float ms = CL_LatencyPercentile( &s->hist[i], p[j] );

			if ( ms < 0 ) {
				Com_Printf( " %7s", ">40" );
			} else {
				Com_Printf( " %7.2f", ms );
			}
		}
		Com_Printf( "\n" );
	}
}

/*
==================
CL_LatencyReset
==================
*/
static void CL_LatencyReset( void ) {
	lat_pump = 0;
	lat_pending.pump = 0;
	lat_frame.pump = 0;
	lat_endFrameTag = 0;
	Com_Memset( lat_tagged, 0, sizeof( lat_tagged ) );
	lat_numWaiting = 0;
	lat_screenSince = lat_screenEvent = lat_newestEvent = 0;
	Com_Memset( &lat_window, 0, sizeof( lat_window ) );
	Com_Memset( &lat_total, 0, sizeof( lat_total ) );
}

/*
==================
CL_LatencyStop

Prints the whole run.
==================
*/
static void CL_LatencyStop( void ) {
	if ( lat_window.frames && lat_window.frames != lat_total.frames ) {
		CL_LatencyPrint( &lat_window, va( "last %d", lat_window.frames ) );
	}
	if ( lat_total.frames ) {
		CL_LatencyPrint( &lat_total, "whole run" );
	} else {
		Com_Printf( "cl_latencyStats: no frames with mouse input\n" );
	}

#ifdef __APPLE__
	Sys_MouseEventMonitor( qfalse );
#endif
	CL_LatencyReset();
	lat_on = qfalse;
}

/*
==================
CL_LatencyInit
==================
*/
void CL_LatencyInit( void ) {
	cl_latencyStats = Cvar_Get( "cl_latencyStats", "0", CVAR_TEMP );
	cl_latencyStatsFrames = Cvar_Get( "cl_latencyStatsFrames", "1000", CVAR_TEMP );
}

/*
==================
CL_LatencyShutdown
==================
*/
void CL_LatencyShutdown( void ) {
	if ( lat_on ) {
		CL_LatencyStop();
	}
}

/*
==================
CL_LatencyPump

IN_ProcessEvents is about to pump the events: this frame's input sample.
==================
*/
void CL_LatencyPump( void ) {
	if ( !lat_on ) {
		return;
	}

	lat_pump = CL_LatencyClock();
#ifdef __APPLE__
	// drop events dispatched outside the input pump
	Sys_TakeMouseEventTimes( NULL, NULL );
#endif
}

/*
==================
CL_LatencyMouseEvent

A mouse delta went into cl.mouseDx.
==================
*/
void CL_LatencyMouseEvent( void ) {
	long long oldest = 0, newest = 0;

	if ( !lat_on || !lat_pump || lat_pending.pump == lat_pump ) {
		return;		// the probe is off, or this pump was seen already
	}

#ifdef __APPLE__
	Sys_TakeMouseEventTimes( &oldest, &newest );
#endif
	if ( !lat_pending.pump || !lat_pending.eventOldest ) {
		lat_pending.eventOldest = oldest;
	}
	lat_pending.eventNewest = newest;
	if ( newest > lat_newestEvent ) {
		lat_newestEvent = newest;
	}
	lat_pending.pump = lat_pump;
}

/*
==================
CL_LatencyMouseMove

The mouse movement in cl.mouseDx went into the usercmd and the view angles.
==================
*/
void CL_LatencyMouseMove( void ) {
	if ( !lat_pending.pump ) {
		return;
	}

	if ( lat_frame.pump && lat_frame.eventOldest ) {
		lat_pending.eventOldest = lat_frame.eventOldest;
	}
	lat_frame = lat_pending;
	lat_pending.pump = 0;
}

/*
==================
CL_LatencyBeforeEndFrame
==================
*/
void CL_LatencyBeforeEndFrame( void ) {
	qboolean rendererTimes;

	if ( !cl_latencyStats ) {
		return;
	}

	if ( !!cl_latencyStats->integer != lat_on ) {
		if ( lat_on ) {
			CL_LatencyStop();
		} else {
			CL_LatencyReset();
#ifdef __APPLE__
			Sys_MouseEventMonitor( qtrue );
#endif
			lat_rendererTimes = re.LatencyTag && re.LatencyPoll;
			lat_on = qtrue;
		}
	}

	if ( !lat_on ) {
		return;
	}

	rendererTimes = re.LatencyTag && re.LatencyPoll;
	if ( rendererTimes != lat_rendererTimes ) {
		// another renderer: its numbers mean something else
		if ( lat_total.frames ) {
			CL_LatencyPrint( &lat_total, "until the renderer changed" );
		}
		CL_LatencyReset();
		lat_rendererTimes = rendererTimes;
	}

	lat_endFrameTag = 0;
	if ( lat_frame.pump ) {
		latInput_t *in;

		if ( ++lat_nextTag <= 0 ) {
			lat_nextTag = 1;
		}
		in = &lat_tagged[lat_nextTag & ( LAT_RING - 1 )];
		if ( in->tag ) {
			LAT_COUNT( lost );	// the renderer never reported it
		}
		*in = lat_frame;
		in->tag = lat_nextTag;
		lat_endFrameTag = lat_nextTag;
		lat_frame.pump = 0;
	}

	if ( lat_rendererTimes ) {
		re.LatencyTag( lat_endFrameTag );
	}
}

/*
==================
CL_LatencyAfterEndFrame
==================
*/
void CL_LatencyAfterEndFrame( void ) {
	int tag, i;
	long long commit, gpuDone, present;

	if ( !lat_on ) {
		return;
	}

	if ( !lat_rendererTimes ) {
		if ( lat_endFrameTag ) {
			latInput_t *in = &lat_tagged[lat_endFrameTag & ( LAT_RING - 1 )];

			CL_LatencyInput( in, 0, 0, CL_LatencyClock() );
			in->tag = 0;
		}
	} else {
		// finished frames, in frame order
		while ( re.LatencyPoll( &tag, &commit, &gpuDone, &present ) ) {
			if ( cl_latencyStats->integer == 2 ) {
				Com_Printf( "cl_latencyStats frame: tag %d commit %lld gpu %lld present %lld\n", tag, commit, gpuDone, present );
			}
			if ( tag ) {
				latInput_t *in = &lat_tagged[tag & ( LAT_RING - 1 )];

				if ( in->tag == tag ) {
					CL_LatencyInput( in, commit, gpuDone, 0 );
					if ( lat_numWaiting < LAT_RING ) {
						lat_waiting[lat_numWaiting++] = *in;
					} else {
						LAT_COUNT( stale );	// nothing was shown for LAT_RING frames
					}
					in->tag = 0;
				}
			}

			if ( present ) {
				LAT_COUNT( presented );
				if ( present < 0 ) {
					LAT_COUNT( neverShown );
				}
			}
			if ( present > 0 ) {
				// the image before this one was on screen until now, with its newest event's age growing
				// (not after the movement stopped: then nothing newer is missing from it)
				if ( lat_screenSince && lat_screenEvent && lat_newestEvent > lat_screenEvent
					&& present - lat_screenEvent <= LAT_STALE_USEC ) {
					long long t;

					for ( t = lat_screenSince; t < present; t += LAT_BIN_USEC ) {
						CL_LatencySample( LAT_EVENT_SCREEN, t - lat_screenEvent );
					}
				}
				lat_screenSince = present;

				// this frame shows the input of every frame still waiting
				for ( i = 0; i < lat_numWaiting; i++ ) {
					CL_LatencyShown( &lat_waiting[i], present, tag && lat_waiting[i].tag == tag );
					if ( lat_waiting[i].eventNewest > lat_screenEvent ) {
						lat_screenEvent = lat_waiting[i].eventNewest;
					}
				}
				lat_numWaiting = 0;
			}
		}
	}
	lat_endFrameTag = 0;

	if ( lat_window.frames >= cl_latencyStatsFrames->integer && cl_latencyStatsFrames->integer > 0 ) {
		CL_LatencyPrint( &lat_window, va( "last %d", lat_window.frames ) );
		Com_Memset( &lat_window, 0, sizeof( lat_window ) );
	}
}
