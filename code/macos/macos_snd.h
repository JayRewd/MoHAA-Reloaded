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
// macos_snd.h -- the native sound engine's mixer (docs/METAL_PORT_PLAN.md Phase 9): Core Audio's output unit, the
// voices and the render. C, Core Audio and AudioToolbox only, no engine header, so port/sndtest.c builds it alone.
//
// One producer, the game thread, calls everything here. The output unit's IO thread is the only consumer: it takes
// the game's commands from a lock-free ring at the start of each callback and the voices' gains and rates from a
// mailbox per voice (the newest wins), mixes, and publishes each voice's state and position with atomics. Nothing
// here waits for the IO thread.

#ifndef __MACOS_SND_H__
#define __MACOS_SND_H__

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// MacSnd_Status
#define MACSND_STOPPED		0
#define MACSND_PLAYING		1
#define MACSND_PAUSED		2

// MacSnd_SetResampler
#define MACSND_SINC			0	// windowed-sinc polyphase (the default)
#define MACSND_LINEAR		1	// linear interpolation, as Miles resampled (the A/B)

typedef struct {
	const char	*device;		// an output device's name or UID; NULL or "" = the system's default output, followed
	int			bufferFrames;	// the IO buffer in frames, 32-512 (s_bufferFrames; 0 = 64)
	int			maxVoices;		// voice numbers 0 .. maxVoices-1 (0 = 128)
	int			maxTails;		// stopped voices still fading out (0 = 32)
	int			maxSounds;		// (0 = 2048)
	double		offlineRate;	// > 0: no device; MacSnd_Render mixes at this rate (port/sndtest.c)
	void		( *print )( const char *msg );	// init, device changes and errors (NULL = quiet)
} macSndConfig_t;

typedef struct {
	int			sound;			// from MacSnd_CreateSound
	float		gainL, gainR;	// linear, 1 = the sound's own level
	float		rate;			// the playback rate in Hz (Miles' set_sample_playback_rate); 0 = the sound's own
	int			startFrame;		// the start offset
	int			loopStart;		// the loop block in frames; it also covers the end of the sound when loopEnd is 0
	int			loopEnd;
	int			loopCount;		// Miles' loop count: 0 = forever, 1 = once, n = the block played n times
	int			paused;			// start paused (MacSnd_Pause resumes it)
} macSndStart_t;

typedef struct {
	char		device[128];	// the open device's name, "" when none
	double		rate;			// the device's rate, the mix rate
	int			bufferFrames;
	double		latency;		// seconds from a buffer's output time to the jack (device + stream latency)
	unsigned	callbacks;		// render callbacks so far
	unsigned	overloads;		// the device's overload notifications
	unsigned	ringDrops;		// commands lost to a full ring
	unsigned	reopens;		// the output unit re-created (device or rate changes)
	int			voices;			// voices mixed in the last callback (tails included)
	int			sounds;			// live sounds
	size_t		soundBytes;		// their memory
	double		renderSeconds;	// the render's time over all callbacks
	double		maxRender;		// the longest callback's render (seconds)
	double		wallSeconds;	// since MacSnd_Init
	unsigned	cpuCallbacks[16];	// callbacks per CPU number (here 0-5 efficiency, 6-9 performance cores)
	unsigned	captureDrops;	// captured frames lost to a full capture ring
} macSndStats_t;

int			MacSnd_Init( const macSndConfig_t *config );	// 0 on failure
void		MacSnd_Shutdown( void );
void		MacSnd_Update( void );		// once a frame: device changes, sounds the IO thread let go of

// 16-bit PCM, interleaved when stereo; pcm NULL = silence (a stream's ring, filled through MacSnd_SoundData).
// Returns a sound, 0 on failure.
int			MacSnd_CreateSound( const short *pcm, int frames, int channels, int rate );
short		*MacSnd_SoundData( int sound );
void		MacSnd_FreeSound( int sound );	// voices on it fade out; its memory goes once the IO thread let go

void		MacSnd_Start( int voice, const macSndStart_t *start );	// replaces what the voice played
void		MacSnd_SetParams( int voice, float gainL, float gainR, float rate );	// ramped across the next callback
void		MacSnd_Stop( int voice );
void		MacSnd_Pause( int voice, int paused );
void		MacSnd_SetLoop( int voice, int loopStart, int loopEnd, int loopCount );	// the loop count restarts
void		MacSnd_Seek( int voice, int frame );		// a playing or paused voice
int			MacSnd_Status( int voice );
int			MacSnd_Position( int voice );			// the frame of the sound it plays
unsigned long long	MacSnd_StartTime( int voice );	// the host time at which its first frame leaves the device
													// (the jack: + latency), 0 until the IO thread started it;
													// offline, the frame number

void		MacSnd_SetMaster( float gain );
void		MacSnd_SetResampler( int mode );
void		MacSnd_SetStopFade( float ms );		// stops, pauses, seeks; 0 = cut at once, as Miles did
void		MacSnd_Restart( void );				// re-creates the output unit (what a device change does)
void		MacSnd_GetStats( macSndStats_t *stats );

// A capture of the final mix (after the master gain, the limiter and the clip), for tests: a ring of frames (0 stops
// it; the first capture's size is kept until MacSnd_Shutdown); mute: the device plays silence meanwhile. The game
// thread drains it often enough (the ring's length of audio), or the frames are lost (stats: captureDrops).
int			MacSnd_Capture( int frames, int mute );		// 0 on failure
int			MacSnd_ReadCapture( float *out, int maxFrames );	// interleaved stereo: the frames read

void		MacSnd_Render( float *out, int frames );	// offline only: interleaved stereo, the IO thread's code

#ifdef __cplusplus
}
#endif

#endif
