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
// macos_snd.c -- the native sound engine's mixer (docs/METAL_PORT_PLAN.md Phase 9): our own render callback on
// Core Audio's output unit (AUHAL) at the device's rate, stereo float, a small IO buffer (64 frames by default, a
// start reaches the jack ~5 ms later). The driver above it plays MOHAA's sound as the PC original did on Miles;
// this file only mixes what it is told.
//
// The IO thread: no allocation, lock, Objective-C message or system call. It takes the commands from the ring, the
// newest gains and rates from the mailboxes, mixes the active voices only (16-bit sounds, NEON), runs the limiter
// and publishes. The game thread never waits for it: a voice's status is the game's own intent until the IO thread
// has caught up with it (a generation number per start), then what the IO thread published.
//
// Resampling: every voice is resampled to the device's rate from a 32.32 position. The default is a windowed-sinc
// polyphase filter (24 taps, 128 phases with the coefficients interpolated between them, all in 16-bit integers
// with 32-bit sums); a voice read faster than the device rate (a step above 1) gets a stretched kernel, its cutoff
// at the output's Nyquist frequency. Linear interpolation, Miles' resampler, is the A/B.

#include "macos_snd.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <arm_neon.h>
#include <limits.h>
#include <mach/mach_time.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SND_ONE				( 1ull << 32 )		// a frame in 32.32
#define SND_PHASES			128					// the sinc tables' phases
#define SND_PHASE_SHIFT		25					// 32 - log2( SND_PHASES )
#define SND_TAPS			24					// the sinc's taps at steps up to 1
#define SND_KERNELS			4					// steps up to 1, 1.25, 1.5 and 2 (and beyond, aliasing)
#define SND_MAXTAPS			48
#define SND_GUARD			( SND_MAXTAPS / 2 )	// silent frames before and after each sound
#define SND_BETA			7.0					// the Kaiser window's: ~70 dB stopband
#define SND_DC				16384				// the coefficients' sum (DC gain 1)
#define SND_SCALE			( 1.0f / ( 32768.0f * SND_DC ) )	// a 32-bit sum to float
#define SND_RING			1024				// commands (a power of 2)
#define SND_BLOCK			512					// the mix buffer; longer callbacks are mixed in blocks
#define SND_CEILING			0.966051f			// the limiter's: -0.3 dBFS
#define SND_RELEASE			0.050				// the limiter's recovery, seconds
#define SND_GENBITS			30
#define SND_GENMASK			( ( 1u << SND_GENBITS ) - 1 )

enum {
	CMD_START,
	CMD_STOP,
	CMD_PAUSE,
	CMD_RESUME,
	CMD_LOOP,
	CMD_SEEK,
	CMD_FREE
};

enum {
	SOUND_FREE,
	SOUND_LIVE,
	SOUND_FREEING		// the IO thread hasn't let go of it yet
};

typedef struct {
	uint8_t		op;
	uint8_t		paused;
	uint16_t	voice;
	int32_t		sound;			// a slot
	int32_t		frame;
	int32_t		loopStart, loopEnd, loopCount;
	float		gainL, gainR, rate;
	uint32_t	seq, gen;
} sndCmd_t;

// the game's gains and rate for a voice, newest wins: a sequence lock, even when complete
typedef struct {
	_Atomic uint32_t	seq;
	_Atomic uint32_t	gainL, gainR, rate;		// float bits
} sndMailbox_t;

typedef struct {
	int			taps, half;
	short		*rows;			// [SND_PHASES][2 * taps]: a phase's coefficients, then the next phase's minus these
} sndKernel_t;

typedef struct {
	short		*alloc;			// SND_GUARD silent frames, the sound, SND_GUARD silent frames
	int			frames, channels, rate;
	uint32_t	gen;			// 1-65535, in the handle
	int			state;			// SOUND_*, the game's
	int			freePosted;		// the game's: its free reached the ring
	int			ioDying;		// the IO thread's: freed, voices may still play it
	_Atomic uint32_t	released;	// the gen the IO thread let go of

	// a stream: a ring of frames the game writes ahead of the voice playing it, counted from the stream's start
	int			stream;
	uint64_t	written;		// the game's
	_Atomic uint64_t	committed;	// what the voice may read: written, published
	_Atomic uint64_t	played;		// the IO thread's: where its voice is (where the next voice starts)
	_Atomic uint32_t	ended;		// no frame after the committed ones: the voice stops at the last
} sndSound_t;

// the IO thread's voice; the tails (stopped voices fading out) are copies
typedef struct {
	const short	*data;			// the sound's first frame
	int			frames, channels;
	int			sound;
	float		soundRate, rate;
	uint64_t	pos, step;		// 32.32 frames
	int			loopStart, loopEnd;
	int			passes;			// the loop block's passes left: 0 = forever, 1 = the last (no wrap)
	int			wrapped;		// the loop block wrapped: the frames before loopStart are its end
	uint32_t	wraps;			// the loop block's wraps (a stream's ring: its position is wraps * frames + pos)
	int			stream;			// plays a stream's ring
	float		gain[2];		// now, scaled by SND_SCALE
	float		target[2];		// at the end of this callback
	int			fadeIn, fadeLen;	// a fade-in's frames left, its length
	int			fadeOut;		// a tail's frames left
	uint32_t	seen;			// the mailbox's sequence applied
	uint32_t	gen;
	int			state;			// MACSND_*
	int			active;			// its index in snd.active, -1 when stopped
} sndVoice_t;

// the game thread's view of a voice
typedef struct {
	uint32_t	gen;			// of its last start or seek
	uint32_t	seq;			// the mailbox's
	int			stopped;
	int			paused;
	int			stopPending;	// a stop the full ring held back
	int			frame;			// the start or seek offset: the position until the IO thread took it
} sndGameVoice_t;

static struct {
	int				initialized;
	int				offline;
	double			rate;
	int				maxVoices, maxTails, maxSounds;
	int				wantFrames;
	char			wantDevice[256];
	void			( *print )( const char *msg );
	mach_timebase_info_data_t	timebase;
	uint64_t		initTime;

	// shared
	sndCmd_t		*ring;
	_Atomic uint32_t	ringHead, ringTail;
	sndMailbox_t	*mail;
	_Atomic uint64_t	*pub;			// per voice: the generation << 34 | state << 32 | position
	_Atomic uint64_t	*startTime;
	sndSound_t		*sounds;
	_Atomic uint32_t	masterBits, resampler, fadeBits;

	// the game thread's
	sndGameVoice_t	*game;
	int				*freeSlots, numFreeSlots;
	int				*freeing, numFreeing;
	int				numPending;		// stops and frees a full ring held back
	int				numSounds;
	size_t			soundBytes;

	// the IO thread's
	sndVoice_t		*voices;
	int				*active, numActive;
	sndVoice_t		*tails;
	int				numTails;
	int				*dying, numDying;
	float			master;
	float			limiter;
	uint64_t		frameCount;
	uint64_t		renderTicks, maxTicks;
	float			mix[SND_BLOCK * 2];

	// stats
	_Atomic uint32_t	callbacks, overloads, ringDrops, reopens, voicesMixed, underruns;
	_Atomic uint64_t	pubRenderTicks, pubMaxTicks;
	_Atomic uint32_t	cpu[16];

	// the capture of the final mix (tests): the IO thread writes at captureHead, the game thread reads at
	// captureTail (frames, wrapping)
	float			*capture;
	uint32_t		captureFrames;
	_Atomic uint32_t	captureHead, captureTail, captureOn, captureMute, captureDrops;

	// the device
	AudioUnit		unit;
	AudioDeviceID	device;
	int				followDefault;
	int				bufferFrames;
	double			latency;
	char			deviceName[128];
	_Atomic int		reopen;
} snd;

static sndKernel_t	snd_kernels[SND_KERNELS];

static void SND_Printf( const char *fmt, ... ) __attribute__( ( format( printf, 1, 2 ) ) );
static void SND_Printf( const char *fmt, ... )
{
	char	msg[512];
	va_list	ap;

	if( !snd.print )
		return;
	va_start( ap, fmt );
	vsnprintf( msg, sizeof( msg ), fmt, ap );
	va_end( ap );
	snd.print( msg );
}

static inline uint32_t SND_FloatBits( float f )
{
	uint32_t	u;

	memcpy( &u, &f, sizeof( u ) );
	return u;
}

static inline float SND_BitsFloat( uint32_t u )
{
	float	f;

	memcpy( &f, &u, sizeof( f ) );
	return f;
}

/*
==============================================================================

THE SINC TABLES

==============================================================================
*/

static double SND_BesselI0( double x )
{
	double	sum = 1.0, term = 1.0;
	int		k;

	for( k = 1; k < 64; k++ ) {
		term *= ( x / ( 2.0 * k ) ) * ( x / ( 2.0 * k ) );
		sum += term;
		if( term < sum * 1e-15 )
			break;
	}
	return sum;
}

/*
===============
SND_BuildKernel

A windowed sinc for steps up to maxStep: its cutoff at the source's Nyquist frequency when upsampling, at the
output's when downsampling (stretched to taps = SND_TAPS * maxStep). The window is a Kaiser window without its
pedestal, so a frame enters and leaves the kernel at zero weight. Each phase sums to SND_DC exactly; at step 1
the table at phase 0 is the identity, so a sound at the device's rate plays bit for bit.
===============
*/
static int SND_BuildKernel( sndKernel_t *k, int taps, double maxStep )
{
	const double	fc = 0.5 / maxStep;
	const double	i0beta = SND_BesselI0( SND_BETA );
	int				p, t;

	short			coef[( SND_PHASES + 1 ) * SND_MAXTAPS];

	k->taps = taps;
	k->half = taps / 2;
	k->rows = malloc( sizeof( short ) * SND_PHASES * 2 * taps );
	if( !k->rows )
		return 0;

	for( p = 0; p <= SND_PHASES; p++ ) {
		double	row[SND_MAXTAPS], sum = 0.0;
		short	*c = coef + p * taps;
		int		isum = 0, big = 0;

		for( t = 0; t < taps; t++ ) {
			// the distance from the output point to this tap's frame
			const double	x = (double)p / SND_PHASES + k->half - 1 - t;
			const double	r = x / k->half;
			double			w, s;

			w = fabs( r ) < 1.0 ? ( SND_BesselI0( SND_BETA * sqrt( 1.0 - r * r ) ) - 1.0 ) / ( i0beta - 1.0 ) : 0.0;
			if( x == 0.0 )
				s = 2.0 * fc;
			else if( fc == 0.5 && x == floor( x ) )
				s = 0.0;
			else
				s = sin( 2.0 * M_PI * fc * x ) / ( M_PI * x );
			row[t] = s * w;
			sum += row[t];
		}
		for( t = 0; t < taps; t++ ) {
			c[t] = (short)lrint( row[t] / sum * SND_DC );
			isum += c[t];
			if( fabs( row[t] ) > fabs( row[big] ) )
				big = t;
		}
		c[big] += SND_DC - isum;
	}
	for( p = 0; p < SND_PHASES; p++ ) {
		for( t = 0; t < taps; t++ ) {
			k->rows[p * 2 * taps + t] = coef[p * taps + t];
			k->rows[p * 2 * taps + taps + t] = coef[( p + 1 ) * taps + t] - coef[p * taps + t];
		}
	}
	return 1;
}

static void SND_FreeKernels( void )
{
	int		i;

	for( i = 0; i < SND_KERNELS; i++ ) {
		free( snd_kernels[i].rows );
		snd_kernels[i].rows = NULL;
	}
}

static int SND_BuildKernels( void )
{
	if( snd_kernels[0].rows )
		return 1;
	if( SND_BuildKernel( &snd_kernels[0], SND_TAPS, 1.0 )
		&& SND_BuildKernel( &snd_kernels[1], 32, 1.25 )
		&& SND_BuildKernel( &snd_kernels[2], 40, 1.5 )
		&& SND_BuildKernel( &snd_kernels[3], SND_MAXTAPS, 2.0 ) )
		return 1;
	SND_FreeKernels();
	return 0;
}

static inline const sndKernel_t *SND_Kernel( uint64_t step )
{
	if( step <= SND_ONE )
		return &snd_kernels[0];
	if( step <= SND_ONE + SND_ONE / 4 )
		return &snd_kernels[1];
	if( step <= SND_ONE + SND_ONE / 2 )
		return &snd_kernels[2];
	return &snd_kernels[3];
}

/*
==============================================================================

THE RESAMPLERS

A frame's value is a 32-bit sum, the sample times SND_DC; the gains carry SND_SCALE. The window of a frame at
position i + f is the frames i - half + 1 .. i + half.

==============================================================================
*/

// a frame's partial sums (4 lanes); the coefficients between two phases: c + round( d * q / 2^15 ), one SQRDMLAH
static inline __attribute__( ( always_inline ) ) int32x4_t SND_DotMono( const short *s, const short *row, int16x8_t q,
	const int taps )
{
	int32x4_t	a0 = vdupq_n_s32( 0 ), a1 = vdupq_n_s32( 0 );
	int			j;

	for( j = 0; j < taps; j += 8 ) {
		const int16x8_t	x = vld1q_s16( s + j );
		const int16x8_t	w = vqrdmlahq_s16( vld1q_s16( row + j ), vld1q_s16( row + taps + j ), q );

		a0 = vmlal_s16( a0, vget_low_s16( x ), vget_low_s16( w ) );
		a1 = vmlal_high_s16( a1, x, w );
	}
	return vaddq_s32( a0, a1 );
}

// the same for a stereo frame: { left's partial sums, right's }
static inline __attribute__( ( always_inline ) ) int32x4x2_t SND_DotStereo( const short *s, const short *row,
	int16x8_t q, const int taps )
{
	int32x4_t	l0 = vdupq_n_s32( 0 ), l1 = vdupq_n_s32( 0 ), r0 = vdupq_n_s32( 0 ), r1 = vdupq_n_s32( 0 );
	int32x4x2_t	a;
	int			j;

	for( j = 0; j < taps; j += 8 ) {
		const int16x8x2_t	x = vld2q_s16( s + 2 * j );
		const int16x8_t		w = vqrdmlahq_s16( vld1q_s16( row + j ), vld1q_s16( row + taps + j ), q );

		l0 = vmlal_s16( l0, vget_low_s16( x.val[0] ), vget_low_s16( w ) );
		l1 = vmlal_high_s16( l1, x.val[0], w );
		r0 = vmlal_s16( r0, vget_low_s16( x.val[1] ), vget_low_s16( w ) );
		r1 = vmlal_high_s16( r1, x.val[1], w );
	}
	a.val[0] = vaddq_s32( l0, l1 );
	a.val[1] = vaddq_s32( r0, r1 );
	return a;
}

static inline const short *SND_Row( const sndKernel_t *k, uint32_t frac, const int taps )
{
	return k->rows + ( frac >> SND_PHASE_SHIFT ) * 2 * taps;
}

static inline int16x8_t SND_Frac( uint32_t frac )
{
	return vdupq_n_s16( (short)( ( frac >> 10 ) & 0x7fff ) );
}

// one frame from a window (win = the window's first frame)
static inline __attribute__( ( always_inline ) ) float32x2_t SND_SincFrame( const short *win, int channels,
	const sndKernel_t *k, uint32_t frac, const int taps )
{
	const short	*row = SND_Row( k, frac, taps );

	if( channels == 1 )
		return vdup_n_f32( (float)vaddvq_s32( SND_DotMono( win, row, SND_Frac( frac ), taps ) ) );
	{
		const int32x4x2_t	a = SND_DotStereo( win, row, SND_Frac( frac ), taps );

		return vcvt_f32_s32( vset_lane_s32( vaddvq_s32( a.val[1] ), vdup_n_s32( vaddvq_s32( a.val[0] ) ), 1 ) );
	}
}

static inline float32x2_t SND_LinearFrame( const short *win, int channels, uint32_t frac )
{
	const int	f = frac >> 18;		// 14 bits: the sum's scale is SND_DC

	if( channels == 1 )
		return vdup_n_f32( (float)( win[0] * SND_DC + ( win[1] - win[0] ) * f ) );
	return vcvt_f32_s32( vset_lane_s32( win[1] * SND_DC + ( win[3] - win[1] ) * f,
		vdup_n_s32( win[0] * SND_DC + ( win[2] - win[0] ) * f ), 1 ) );
}

// n frames: mono four at a time and stereo two at a time, their sums reduced together and mixed as vectors
static inline __attribute__( ( always_inline ) ) void SND_SincRun( const short *data, int channels,
	const sndKernel_t *k, uint64_t *ppos, uint64_t step, int n, float *mix, float32x2_t *pg, float32x2_t dg,
	const int taps )
{
	const int	off = taps / 2 - 1;
	uint64_t	pos = *ppos;
	float32x2_t	g = *pg;
	int			f = 0;

	if( channels == 1 ) {
		float32x4_t			g01 = vcombine_f32( g, vadd_f32( g, dg ) );
		float32x4_t			g23 = vaddq_f32( g01, vcombine_f32( vadd_f32( dg, dg ), vadd_f32( dg, dg ) ) );
		const float32x4_t	dg4 = vmulq_n_f32( vcombine_f32( dg, dg ), 4.0f );

		for( ; f + 4 <= n; f += 4, mix += 8 ) {
			int32x4_t	a[4];
			float32x4_t	v;
			int			j;

			for( j = 0; j < 4; j++, pos += step )
				a[j] = SND_DotMono( data + (int)( pos >> 32 ) - off, SND_Row( k, (uint32_t)pos, taps ),
					SND_Frac( (uint32_t)pos ), taps );
			v = vcvtq_f32_s32( vpaddq_s32( vpaddq_s32( a[0], a[1] ), vpaddq_s32( a[2], a[3] ) ) );
			vst1q_f32( mix, vfmaq_f32( vld1q_f32( mix ), vzip1q_f32( v, v ), g01 ) );
			vst1q_f32( mix + 4, vfmaq_f32( vld1q_f32( mix + 4 ), vzip2q_f32( v, v ), g23 ) );
			g01 = vaddq_f32( g01, dg4 );
			g23 = vaddq_f32( g23, dg4 );
		}
		g = vget_low_f32( g01 );
		for( ; f < n; f++, mix += 2, pos += step ) {
			const float32x2_t	a = SND_SincFrame( data + (int)( pos >> 32 ) - off, 1, k, (uint32_t)pos, taps );

			vst1_f32( mix, vfma_f32( vld1_f32( mix ), a, g ) );
			g = vadd_f32( g, dg );
		}
	} else {
		float32x4_t			g01 = vcombine_f32( g, vadd_f32( g, dg ) );
		const float32x4_t	dg2 = vcombine_f32( vadd_f32( dg, dg ), vadd_f32( dg, dg ) );

		for( ; f + 2 <= n; f += 2, mix += 4 ) {
			const int32x4x2_t	a0 = SND_DotStereo( data + 2 * ( (int)( pos >> 32 ) - off ), SND_Row( k, (uint32_t)pos, taps ),
				SND_Frac( (uint32_t)pos ), taps );
			const uint64_t		pos1 = pos + step;
			const int32x4x2_t	a1 = SND_DotStereo( data + 2 * ( (int)( pos1 >> 32 ) - off ), SND_Row( k, (uint32_t)pos1, taps ),
				SND_Frac( (uint32_t)pos1 ), taps );
			const float32x4_t	v = vcvtq_f32_s32( vpaddq_s32( vpaddq_s32( a0.val[0], a0.val[1] ),
				vpaddq_s32( a1.val[0], a1.val[1] ) ) );		// L0 R0 L1 R1

			vst1q_f32( mix, vfmaq_f32( vld1q_f32( mix ), v, g01 ) );
			g01 = vaddq_f32( g01, dg2 );
			pos = pos1 + step;
		}
		g = vget_low_f32( g01 );
		for( ; f < n; f++, mix += 2, pos += step ) {
			const float32x2_t	a = SND_SincFrame( data + 2 * ( (int)( pos >> 32 ) - off ), 2, k, (uint32_t)pos, taps );

			vst1_f32( mix, vfma_f32( vld1_f32( mix ), a, g ) );
			g = vadd_f32( g, dg );
		}
	}
	*ppos = pos;
	*pg = g;
}

// mixes n frames whose windows stay inside the sound's data
static void SND_FastRun( sndVoice_t *v, const sndKernel_t *k, int n, float *mix, float32x2_t *g, float32x2_t dg )
{
	int		f;

	if( !k ) {
		uint64_t	pos = v->pos;
		float32x2_t	gg = *g;

		for( f = 0; f < n; f++, mix += 2, pos += v->step ) {
			const float32x2_t	a = SND_LinearFrame( v->data + (int)( pos >> 32 ) * v->channels, v->channels,
				(uint32_t)pos );

			vst1_f32( mix, vfma_f32( vld1_f32( mix ), a, gg ) );
			gg = vadd_f32( gg, dg );
		}
		v->pos = pos;
		*g = gg;
		return;
	}
	switch( k->taps ) {
	case 24: SND_SincRun( v->data, v->channels, k, &v->pos, v->step, n, mix, g, dg, 24 ); break;
	case 32: SND_SincRun( v->data, v->channels, k, &v->pos, v->step, n, mix, g, dg, 32 ); break;
	case 40: SND_SincRun( v->data, v->channels, k, &v->pos, v->step, n, mix, g, dg, 40 ); break;
	default: SND_SincRun( v->data, v->channels, k, &v->pos, v->step, n, mix, g, dg, 48 ); break;
	}
}

// one frame whose window crosses the loop block's end (or, after a wrap, its start): gathered, then filtered
static void SND_SlowFrame( sndVoice_t *v, const sndKernel_t *k, int half, int wrapping, float *mix, float32x2_t g )
{
	short		win[SND_MAXTAPS * 2];
	const int	i = (int)( v->pos >> 32 );
	const int	len = v->loopEnd - v->loopStart;
	float32x2_t	a;
	int			t, j;

	for( t = 0; t < 2 * half; t++ ) {
		j = i - half + 1 + t;
		if( wrapping && j >= v->loopEnd )
			j = v->loopStart + ( j - v->loopEnd ) % len;
		else if( v->wrapped && j < v->loopStart )
			j = v->loopEnd - 1 - ( v->loopStart - 1 - j ) % len;
		if( v->channels == 1 )
			win[t] = v->data[j];
		else {
			win[2 * t] = v->data[2 * j];
			win[2 * t + 1] = v->data[2 * j + 1];
		}
	}
	if( !k )
		a = SND_LinearFrame( win, v->channels, (uint32_t)v->pos );
	else if( k->taps == 24 )
		a = SND_SincFrame( win, v->channels, k, (uint32_t)v->pos, 24 );
	else if( k->taps == 32 )
		a = SND_SincFrame( win, v->channels, k, (uint32_t)v->pos, 32 );
	else if( k->taps == 40 )
		a = SND_SincFrame( win, v->channels, k, (uint32_t)v->pos, 40 );
	else
		a = SND_SincFrame( win, v->channels, k, (uint32_t)v->pos, 48 );
	vst1_f32( mix, vfma_f32( vld1_f32( mix ), a, g ) );
	v->pos += v->step;
}

/*
===============
SND_Resample

Mixes up to n frames of v from its position, the gains starting at g and changing by dg a frame (k NULL =
linear). Wraps the loop block while passes are left; returns the frames mixed, fewer when the sound ended
(v->state is then MACSND_STOPPED). With k == NULL and mix == NULL it only advances (a silent voice).
===============
*/
static int SND_Resample( sndVoice_t *v, const sndKernel_t *k, int n, float *mix, float32x2_t g, float32x2_t dg )
{
	const int	half = k ? k->half : 1;
	int			done = 0;

	while( done < n ) {
		const int	i = (int)( v->pos >> 32 );
		const int	wrapping = v->passes != 1;
		int			lo, hi, m;

		if( wrapping && i >= v->loopEnd ) {
			v->pos -= (uint64_t)( v->loopEnd - v->loopStart ) << 32;
			v->wrapped = 1;
			v->wraps++;
			if( v->passes > 1 )
				v->passes--;
			continue;
		}
		if( !wrapping && i >= v->frames ) {
			v->state = MACSND_STOPPED;
			break;
		}

		if( !mix ) {
			// silent: only the position moves, to the next wrap or the end
			const uint64_t	left = ( (uint64_t)( wrapping ? v->loopEnd : v->frames ) << 32 ) - v->pos;
			const uint64_t	count = ( left + v->step - 1 ) / v->step;

			m = count < (uint64_t)( n - done ) ? (int)count : n - done;
			v->pos += m * v->step;
			done += m;
			continue;
		}

		// the window stays in the data from lo to hi (before a wrap the guard frames hold the silence around it)
		lo = v->wrapped ? v->loopStart + half - 1 : INT_MIN;
		hi = wrapping ? v->loopEnd - half : v->frames;
		if( i >= lo && i < hi ) {
			const uint64_t	left = ( (uint64_t)hi << 32 ) - v->pos;
			const uint64_t	count = ( left + v->step - 1 ) / v->step;

			m = count < (uint64_t)( n - done ) ? (int)count : n - done;
			SND_FastRun( v, k, m, mix, &g, dg );
		} else {
			m = 1;
			SND_SlowFrame( v, k, half, wrapping, mix, g );
			g = vadd_f32( g, dg );
		}
		mix += 2 * m;
		done += m;
	}
	// it ended with this frame: stopped in this callback, not the next
	if( v->passes == 1 && (int)( v->pos >> 32 ) >= v->frames )
		v->state = MACSND_STOPPED;
	return done;
}

/*
==============================================================================

THE IO THREAD

==============================================================================
*/

static inline uint64_t SND_Step( float rate )
{
	double	step = (double)rate / snd.rate * (double)SND_ONE;

	if( !( step >= 1.0 ) )
		step = 1.0;
	if( step > 16.0 * SND_ONE )
		step = 16.0 * SND_ONE;
	return (uint64_t)step;
}

static inline void SND_Publish( const sndVoice_t *v, int voice )
{
	atomic_store_explicit( &snd.pub[voice], ( (uint64_t)v->gen << 34 ) | ( (uint64_t)v->state << 32 )
		| (uint32_t)( v->pos >> 32 ), memory_order_release );
}

static void SND_Activate( sndVoice_t *v )
{
	if( v->active < 0 ) {
		v->active = snd.numActive;
		snd.active[snd.numActive++] = (int)( v - snd.voices );
	}
}

static void SND_Deactivate( sndVoice_t *v )
{
	const int	voice = (int)( v - snd.voices );

	v->state = MACSND_STOPPED;
	if( v->active >= 0 ) {
		const int	last = snd.active[--snd.numActive];

		snd.active[v->active] = last;
		snd.voices[last].active = v->active;
		v->active = -1;
	}
	SND_Publish( v, voice );
}

// what v plays fades out from a copy over fade frames (nothing when cut at once or no tail is free)
static void SND_Tail( const sndVoice_t *v, int fade )
{
	sndVoice_t	*t;
	float		f;

	if( v->state != MACSND_PLAYING || fade <= 0 || snd.numTails == snd.maxTails )
		return;
	t = &snd.tails[snd.numTails++];
	*t = *v;
	f = v->fadeIn > 0 ? 1.0f - (float)v->fadeIn / v->fadeLen : 1.0f;
	t->gain[0] *= f;
	t->gain[1] *= f;
	t->fadeIn = 0;
	t->fadeOut = fade;
	t->active = -1;
}

static void SND_SetLoop( sndVoice_t *v, int loopStart, int loopEnd, int loopCount )
{
	if( loopEnd <= 0 || loopEnd > v->frames )
		loopEnd = v->frames;
	if( loopStart < 0 || loopStart >= loopEnd )
		loopStart = 0;
	v->loopStart = loopStart;
	v->loopEnd = loopEnd;
	v->passes = loopCount > 0 ? loopCount : 0;
}

static void SND_SetPos( sndVoice_t *v, int frame )
{
	if( frame < 0 )
		frame = 0;
	if( frame > v->frames )
		frame = v->frames;
	if( v->passes != 1 && frame >= v->loopEnd )
		frame = v->loopStart + ( frame - v->loopStart ) % ( v->loopEnd - v->loopStart );
	v->pos = (uint64_t)frame << 32;
	v->wrapped = 0;
}

static void SND_StartVoice( sndVoice_t *v, const sndCmd_t *c, uint64_t now, int fade )
{
	const sndSound_t	*s = &snd.sounds[c->sound];
	const int			voice = (int)( v - snd.voices );

	SND_Tail( v, fade );
	v->data = s->alloc + SND_GUARD * s->channels;
	v->frames = s->frames;
	v->channels = s->channels;
	v->sound = c->sound;
	v->soundRate = (float)s->rate;
	v->rate = c->rate > 0.0f ? c->rate : v->soundRate;
	v->step = SND_Step( v->rate );
	v->stream = s->stream;
	if( s->stream ) {
		// the whole ring forever, from where the stream's last voice stopped (its start for a new one)
		const uint64_t	at = atomic_load_explicit( &s->played, memory_order_relaxed );

		SND_SetLoop( v, 0, 0, 0 );
		v->wraps = (uint32_t)( at / (uint64_t)s->frames );
		v->pos = ( at % (uint64_t)s->frames ) << 32;
		v->wrapped = v->wraps > 0;
		v->fadeIn = at > 0 ? fade : 0;
	} else {
		SND_SetLoop( v, c->loopStart, c->loopEnd, c->loopCount );
		SND_SetPos( v, c->frame );
		v->wraps = 0;
		v->fadeIn = c->frame > 0 ? fade : 0;
	}
	v->gain[0] = v->target[0] = c->gainL * SND_SCALE;
	v->gain[1] = v->target[1] = c->gainR * SND_SCALE;
	v->fadeLen = fade;
	v->seen = c->seq;
	v->gen = c->gen;
	v->state = c->paused ? MACSND_PAUSED : MACSND_PLAYING;
	SND_Activate( v );
	atomic_store_explicit( &snd.startTime[voice], now, memory_order_relaxed );
}

// a freed sound: the voices on it fade out; acknowledged once no tail plays it either (SND_CheckFrees)
static void SND_KillSound( int slot, int fade )
{
	int		i;

	snd.sounds[slot].ioDying = 1;
	snd.dying[snd.numDying++] = slot;
	for( i = 0; i < snd.numActive; ) {
		sndVoice_t	*v = &snd.voices[snd.active[i]];

		if( v->sound == slot ) {
			SND_Tail( v, fade );
			SND_Deactivate( v );	// moves the last active voice to i
		} else
			i++;
	}
}

static void SND_CheckFrees( void )
{
	int		i, j;

	for( i = 0; i < snd.numDying; ) {
		const int	slot = snd.dying[i];

		for( j = 0; j < snd.numTails; j++ ) {
			if( snd.tails[j].sound == slot )
				break;
		}
		if( j < snd.numTails ) {
			i++;
			continue;
		}
		snd.sounds[slot].ioDying = 0;
		atomic_store_explicit( &snd.sounds[slot].released, snd.sounds[slot].gen, memory_order_release );
		snd.dying[i] = snd.dying[--snd.numDying];
	}
}

static void SND_TakeCommands( uint64_t now, int fade )
{
	uint32_t		tail = atomic_load_explicit( &snd.ringTail, memory_order_relaxed );
	const uint32_t	head = atomic_load_explicit( &snd.ringHead, memory_order_acquire );

	for( ; tail != head; tail++ ) {
		const sndCmd_t	*c = &snd.ring[tail & ( SND_RING - 1 )];
		sndVoice_t		*v = &snd.voices[c->voice];

		switch( c->op ) {
		case CMD_START:
			SND_StartVoice( v, c, now, fade );
			break;
		case CMD_STOP:
			SND_Tail( v, fade );
			SND_Deactivate( v );
			break;
		case CMD_PAUSE:
			if( v->state == MACSND_PLAYING ) {
				SND_Tail( v, fade );
				v->state = MACSND_PAUSED;
				v->fadeIn = 0;
			}
			break;
		case CMD_RESUME:
			if( v->state == MACSND_PAUSED ) {
				v->state = MACSND_PLAYING;
				v->fadeIn = v->fadeLen = fade;
			}
			break;
		case CMD_LOOP:
			if( v->state != MACSND_STOPPED && !v->stream )
				SND_SetLoop( v, c->loopStart, c->loopEnd, c->loopCount );
			break;
		case CMD_SEEK:
			v->gen = c->gen;
			if( v->state == MACSND_STOPPED || v->stream ) {
				SND_Publish( v, c->voice );		// a stream doesn't seek: the game opens it again
				break;
			}
			if( v->state == MACSND_PLAYING ) {
				SND_Tail( v, fade );
				v->fadeIn = v->fadeLen = fade;
			}
			SND_SetPos( v, c->frame );
			break;
		case CMD_FREE:
			SND_KillSound( c->sound, fade );
			break;
		}
	}
	atomic_store_explicit( &snd.ringTail, tail, memory_order_release );
}

// the game's newest gains and rate (skipped this callback while the game is writing them)
static void SND_TakeParams( sndVoice_t *v, int voice )
{
	sndMailbox_t	*m = &snd.mail[voice];
	const uint32_t	s = atomic_load_explicit( &m->seq, memory_order_acquire );
	float			gl, gr, rate;

	if( s == v->seen || ( s & 1 ) )
		return;
	gl = SND_BitsFloat( atomic_load_explicit( &m->gainL, memory_order_relaxed ) );
	gr = SND_BitsFloat( atomic_load_explicit( &m->gainR, memory_order_relaxed ) );
	rate = SND_BitsFloat( atomic_load_explicit( &m->rate, memory_order_relaxed ) );
	atomic_thread_fence( memory_order_acquire );
	if( atomic_load_explicit( &m->seq, memory_order_relaxed ) != s )
		return;
	v->seen = s;
	v->target[0] = gl * SND_SCALE;
	v->target[1] = gr * SND_SCALE;
	v->rate = rate > 0.0f ? rate : v->soundRate;
	v->step = SND_Step( v->rate );
}

static void SND_MixVoice( sndVoice_t *v, const sndKernel_t *k, int n, float *mix )
{
	const float32x2_t	g0 = vld1_f32( v->gain );
	const float32x2_t	g1 = vld1_f32( v->target );
	const float32x2_t	dg = vmul_n_f32( vsub_f32( g1, g0 ), 1.0f / n );
	int					done = 0;

	if( k )
		k = SND_Kernel( v->step );

	if( v->fadeIn > 0 ) {
		// the fade-in's frames, its factor ramped with the gains
		const int			m = v->fadeIn < n ? v->fadeIn : n;
		const float			f0 = 1.0f - (float)v->fadeIn / v->fadeLen;
		const float			f1 = 1.0f - (float)( v->fadeIn - m ) / v->fadeLen;
		const float32x2_t	a = vmul_n_f32( g0, f0 );
		const float32x2_t	b = vmul_n_f32( vfma_n_f32( g0, dg, (float)m ), f1 );

		done = SND_Resample( v, k, m, mix, a, vmul_n_f32( vsub_f32( b, a ), 1.0f / m ) );
		v->fadeIn -= m;
		if( done < m )
			n = done;
	}
	if( done < n ) {
		const float32x2_t	g = vfma_n_f32( g0, dg, (float)done );

		if( vmaxv_f32( vabs_f32( g0 ) ) == 0.0f && vmaxv_f32( vabs_f32( g1 ) ) == 0.0f )
			SND_Resample( v, NULL, n - done, NULL, g, dg );
		else
			SND_Resample( v, k, n - done, mix + 2 * done, g, dg );
	}
	vst1_f32( v->gain, g1 );
}

/*
===============
SND_StreamFrames

How many of the next n output frames a stream's voice (or its tail) can play: its window stays in the frames the
game committed (short of them the voice holds where it is, silent: an underrun). After the stream's end the frames
past it are silence (MacSnd_StreamEnd) and it plays up to its last frame: *end is set when that is within the n.
===============
*/
static int SND_StreamFrames( const sndVoice_t *v, int n, int *end )
{
	const sndSound_t	*s = &snd.sounds[v->sound];
	const uint32_t		ended = atomic_load_explicit( &s->ended, memory_order_acquire );	// before committed
	const uint64_t		committed = atomic_load_explicit( &s->committed, memory_order_acquire );
	const uint64_t		at = (uint64_t)v->wraps * (uint64_t)v->frames + ( v->pos >> 32 );
	const uint64_t		limit = ended ? committed : committed > SND_GUARD ? committed - SND_GUARD : 0;
	uint64_t			count;

	*end = 0;
	if( at >= limit ) {
		*end = ended != 0;
		return 0;
	}
	count = ( ( ( limit - at ) << 32 ) - (uint32_t)v->pos + v->step - 1 ) / v->step;
	if( count > (uint64_t)n )
		return n;
	*end = ended != 0;
	return (int)count;
}

// a stream's voice: what it can play of the n frames, then its position published for the game
static void SND_MixStream( sndVoice_t *v, const sndKernel_t *k, int n, float *mix )
{
	sndSound_t	*s = &snd.sounds[v->sound];
	int			end;
	const int	m = SND_StreamFrames( v, n, &end );

	if( m > 0 )
		SND_MixVoice( v, k, m, mix );
	else
		vst1_f32( v->gain, vld1_f32( v->target ) );
	if( end )
		v->state = MACSND_STOPPED;
	else if( m < n )
		atomic_fetch_add_explicit( &snd.underruns, 1, memory_order_relaxed );
	atomic_store_explicit( &s->played, (uint64_t)v->wraps * (uint64_t)v->frames + ( v->pos >> 32 ),
		memory_order_release );
}

// a tail: its gains fall to 0 over its fade's frames left; returns 0 when it's done
static int SND_MixTail( sndVoice_t *t, const sndKernel_t *k, int n, float *mix )
{
	const float32x2_t	g0 = vld1_f32( t->gain );
	const float32x2_t	dg = vmul_n_f32( g0, -1.0f / t->fadeOut );
	int					m = t->fadeOut < n ? t->fadeOut : n;
	int					cut = 0, end;

	if( t->stream ) {
		const int	can = SND_StreamFrames( t, m, &end );

		cut = can < m;
		m = can;
	}
	if( k )
		k = SND_Kernel( t->step );
	if( SND_Resample( t, k, m, mix, g0, dg ) < m || cut )
		return 0;
	vst1_f32( t->gain, vfma_n_f32( g0, dg, (float)m ) );
	t->fadeOut -= m;
	return t->fadeOut > 0;
}

/*
===============
SND_Output

The master gain and the limiter, then a hard clip, into out. The limiter looks ahead by the block itself: when the
block's peak would pass the ceiling the gain falls at once, for the whole block, to keep it under; otherwise it
recovers towards 1 (SND_RELEASE), never past what the block allows.
===============
*/
static void SND_Output( float *out, const float *mix, int n, float master )
{
	const float32x2_t	one = vdup_n_f32( 1.0f ), minus = vdup_n_f32( -1.0f );
	float32x2_t			peak2 = vdup_n_f32( 0.0f ), g, dg;
	float				peak, mmax, need, l0, l1;
	int					i;

	for( i = 0; i < n; i++ )
		peak2 = vmax_f32( peak2, vabs_f32( vld1_f32( mix + 2 * i ) ) );
	peak = vmaxv_f32( peak2 );
	mmax = master > snd.master ? master : snd.master;
	need = peak * mmax > SND_CEILING ? SND_CEILING / ( peak * mmax ) : 1.0f;
	if( need < snd.limiter )
		l0 = l1 = need;
	else {
		l0 = snd.limiter;
		l1 = 1.0f - ( 1.0f - l0 ) * expf( -(float)n / (float)( snd.rate * SND_RELEASE ) );
		if( l1 > 0.99999f )
			l1 = 1.0f;		// not stuck a float below 1
		if( l1 > need )
			l1 = need;
	}
	g = vdup_n_f32( snd.master * l0 );
	dg = vdup_n_f32( ( master * l1 - snd.master * l0 ) / n );
	for( i = 0; i < n; i++ ) {
		vst1_f32( out + 2 * i, vmaxnm_f32( vminnm_f32( vmul_f32( vld1_f32( mix + 2 * i ), g ), one ), minus ) );
		g = vadd_f32( g, dg );
	}
	snd.limiter = l1;
	snd.master = master;
}

// the capture: the block as it goes out, then silence for the device when muted
static void SND_Capture( float *out, int n )
{
	const uint32_t	head = atomic_load_explicit( &snd.captureHead, memory_order_relaxed );
	const uint32_t	tail = atomic_load_explicit( &snd.captureTail, memory_order_acquire );
	uint32_t		i;

	if( snd.captureFrames - ( head - tail ) < (uint32_t)n )
		atomic_fetch_add_explicit( &snd.captureDrops, n, memory_order_relaxed );
	else {
		for( i = 0; i < (uint32_t)n; i++ ) {
			const uint32_t	k = ( head + i ) % snd.captureFrames;

			snd.capture[2 * k] = out[2 * i];
			snd.capture[2 * k + 1] = out[2 * i + 1];
		}
		atomic_store_explicit( &snd.captureHead, head + n, memory_order_release );
	}
	if( atomic_load_explicit( &snd.captureMute, memory_order_relaxed ) )
		memset( out, 0, sizeof( float ) * 2 * n );
}

/*
===============
SND_Render

The IO thread's callback (offline, MacSnd_Render's): the commands, the voices, the output, the publishing. out
NULL takes the commands only (no device: the game thread drains the ring). now = the host time of the first frame's
output (offline, the frame number).
===============
*/
static void SND_Render( float *out, int frames, uint64_t now )
{
	const uint64_t	t0 = mach_absolute_time();
	const float		master = SND_BitsFloat( atomic_load_explicit( &snd.masterBits, memory_order_relaxed ) );
	const int		linear = atomic_load_explicit( &snd.resampler, memory_order_relaxed ) == MACSND_LINEAR;
	const float		fadeMs = SND_BitsFloat( atomic_load_explicit( &snd.fadeBits, memory_order_relaxed ) );
	const int		fade = (int)lrintf( fadeMs * (float)snd.rate * 0.001f );
	const sndKernel_t	*k = linear ? NULL : &snd_kernels[0];
	uint64_t		ticks;
	size_t			cpu = 0;
	int				i, mixed;

	SND_TakeCommands( now, fade );
	if( !out ) {
		SND_CheckFrees();
		return;
	}
	for( i = 0; i < snd.numActive; i++ )
		SND_TakeParams( &snd.voices[snd.active[i]], snd.active[i] );

	while( frames > 0 ) {
		const int	n = frames < SND_BLOCK ? frames : SND_BLOCK;

		memset( snd.mix, 0, sizeof( float ) * 2 * n );
		for( i = 0; i < snd.numActive; ) {
			sndVoice_t	*v = &snd.voices[snd.active[i]];

			if( v->state == MACSND_PLAYING ) {
				if( v->stream )
					SND_MixStream( v, k, n, snd.mix );
				else
					SND_MixVoice( v, k, n, snd.mix );
				if( v->state == MACSND_STOPPED ) {
					SND_Deactivate( v );	// ended: moves the last active voice to i
					continue;
				}
			}
			i++;
		}
		for( i = 0; i < snd.numTails; ) {
			if( !SND_MixTail( &snd.tails[i], k, n, snd.mix ) )
				snd.tails[i] = snd.tails[--snd.numTails];
			else
				i++;
		}
		SND_Output( out, snd.mix, n, master );
		if( atomic_load_explicit( &snd.captureOn, memory_order_relaxed ) )
			SND_Capture( out, n );
		out += 2 * n;
		frames -= n;
		snd.frameCount += n;
	}
	SND_CheckFrees();
	mixed = snd.numTails;
	for( i = 0; i < snd.numActive; i++ ) {
		SND_Publish( &snd.voices[snd.active[i]], snd.active[i] );
		mixed += snd.voices[snd.active[i]].state == MACSND_PLAYING;
	}

	ticks = mach_absolute_time() - t0;
	snd.renderTicks += ticks;
	if( ticks > snd.maxTicks )
		snd.maxTicks = ticks;
	atomic_store_explicit( &snd.pubRenderTicks, snd.renderTicks, memory_order_relaxed );
	atomic_store_explicit( &snd.pubMaxTicks, snd.maxTicks, memory_order_relaxed );
	atomic_store_explicit( &snd.voicesMixed, mixed, memory_order_relaxed );
	atomic_fetch_add_explicit( &snd.callbacks, 1, memory_order_relaxed );
	pthread_cpu_number_np( &cpu );		// no system call: the thread's register
	atomic_fetch_add_explicit( &snd.cpu[cpu & 15], 1, memory_order_relaxed );
}

/*
==============================================================================

THE DEVICE

==============================================================================
*/

static OSStatus SND_Callback( void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus,
	UInt32 frames, AudioBufferList *io )
{
	SND_Render( io->mBuffers[0].mData, (int)frames, ( ts->mFlags & kAudioTimeStampHostTimeValid ) ? ts->mHostTime : 0 );
	return noErr;
}

// on Core Audio's notification thread
static OSStatus SND_Listener( AudioObjectID object, UInt32 count, const AudioObjectPropertyAddress *addresses,
	void *ref )
{
	UInt32	i;

	for( i = 0; i < count; i++ ) {
		if( addresses[i].mSelector == kAudioDeviceProcessorOverload )
			atomic_fetch_add_explicit( &snd.overloads, 1, memory_order_relaxed );
		else
			atomic_store_explicit( &snd.reopen, 1, memory_order_relaxed );
	}
	return noErr;
}

static const AudioObjectPropertyAddress	snd_defaultDevice = { kAudioHardwarePropertyDefaultOutputDevice,
	kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
static const AudioObjectPropertyAddress	snd_deviceEvents[] = {
	{ kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
	{ kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
	{ kAudioDeviceProcessorOverload, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain }
};

static UInt32 SND_DeviceU32( AudioObjectID object, AudioObjectPropertySelector selector,
	AudioObjectPropertyScope scope )
{
	AudioObjectPropertyAddress	a = { selector, scope, kAudioObjectPropertyElementMain };
	UInt32						value = 0, size = sizeof( value );

	if( AudioObjectGetPropertyData( object, &a, 0, NULL, &size, &value ) != noErr )
		return 0;
	return value;
}

static void SND_DeviceString( AudioObjectID object, AudioObjectPropertySelector selector, char *buf, int size )
{
	AudioObjectPropertyAddress	a = { selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
	CFStringRef					str = NULL;
	UInt32						sz = sizeof( str );

	buf[0] = 0;
	if( AudioObjectGetPropertyData( object, &a, 0, NULL, &sz, &str ) == noErr && str ) {
		CFStringGetCString( str, buf, size, kCFStringEncodingUTF8 );
		CFRelease( str );
	}
}

static int SND_HasOutput( AudioDeviceID device )
{
	AudioObjectPropertyAddress	a = { kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput,
		kAudioObjectPropertyElementMain };
	UInt32						size = 0;

	return AudioObjectGetPropertyDataSize( device, &a, 0, NULL, &size ) == noErr && size > 0;
}

// the device named by its name or UID; the default output when none is named or it's gone
static AudioDeviceID SND_FindDevice( void )
{
	AudioObjectPropertyAddress	a = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain };
	AudioDeviceID				device = 0, list[64];
	UInt32						size = sizeof( device ), i;

	snd.followDefault = 1;
	if( snd.wantDevice[0] ) {
		size = sizeof( list );
		if( AudioObjectGetPropertyData( kAudioObjectSystemObject, &a, 0, NULL, &size, list ) == noErr ) {
			for( i = 0; i < size / sizeof( list[0] ); i++ ) {
				char	name[256], uid[256];

				if( !SND_HasOutput( list[i] ) )
					continue;
				SND_DeviceString( list[i], kAudioObjectPropertyName, name, sizeof( name ) );
				SND_DeviceString( list[i], kAudioDevicePropertyDeviceUID, uid, sizeof( uid ) );
				if( !strcasecmp( name, snd.wantDevice ) || !strcmp( uid, snd.wantDevice ) ) {
					snd.followDefault = 0;
					return list[i];
				}
			}
		}
		SND_Printf( "sound: no output device \"%s\", using the default\n", snd.wantDevice );
	}
	size = sizeof( device );
	if( AudioObjectGetPropertyData( kAudioObjectSystemObject, &snd_defaultDevice, 0, NULL, &size, &device ) != noErr )
		return 0;
	return device;
}

static void SND_CloseDevice( void )
{
	int		i;

	if( !snd.unit )
		return;
	AudioOutputUnitStop( snd.unit );
	AudioUnitUninitialize( snd.unit );
	AudioComponentInstanceDispose( snd.unit );
	snd.unit = NULL;
	for( i = 0; i < (int)( sizeof( snd_deviceEvents ) / sizeof( snd_deviceEvents[0] ) ); i++ )
		AudioObjectRemovePropertyListener( snd.device, &snd_deviceEvents[i], SND_Listener, NULL );
	if( snd.followDefault )
		AudioObjectRemovePropertyListener( kAudioObjectSystemObject, &snd_defaultDevice, SND_Listener, NULL );
	snd.device = 0;
	snd.deviceName[0] = 0;
}

// a new mix rate: every voice's step again (the IO thread isn't running)
static void SND_SetRate( double rate )
{
	int		i;

	if( rate == snd.rate )
		return;
	snd.rate = rate;
	for( i = 0; i < snd.maxVoices; i++ )
		snd.voices[i].step = SND_Step( snd.voices[i].rate );
	for( i = 0; i < snd.numTails; i++ )
		snd.tails[i].step = SND_Step( snd.tails[i].rate );
}

static int SND_OpenDevice( void )
{
	AudioComponentDescription	desc = { kAudioUnitType_Output, kAudioUnitSubType_HALOutput,
		kAudioUnitManufacturer_Apple, 0, 0 };
	AudioObjectPropertyAddress	a = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain };
	AudioStreamBasicDescription	format;
	AURenderCallbackStruct		callback = { SND_Callback, NULL };
	AudioValueRange				range = { 0, 0 };
	AudioComponent				component;
	AudioStreamID				streams[16];
	AudioDeviceID				device;
	Float64						rate = 0;
	UInt32						size, frames, maxSlice = 4096;
	OSStatus					err;
	int							i;

	device = SND_FindDevice();
	if( !device ) {
		SND_Printf( "sound: no output device\n" );
		return 0;
	}
	size = sizeof( rate );
	if( AudioObjectGetPropertyData( device, &a, 0, NULL, &size, &rate ) != noErr || rate < 8000.0 ) {
		SND_Printf( "sound: the output device has no rate\n" );
		return 0;
	}

	component = AudioComponentFindNext( NULL, &desc );
	if( !component || AudioComponentInstanceNew( component, &snd.unit ) != noErr ) {
		snd.unit = NULL;
		SND_Printf( "sound: no output unit\n" );
		return 0;
	}
	err = AudioUnitSetProperty( snd.unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0,
		&device, sizeof( device ) );

	frames = snd.wantFrames;
	a.mSelector = kAudioDevicePropertyBufferFrameSizeRange;
	size = sizeof( range );
	if( AudioObjectGetPropertyData( device, &a, 0, NULL, &size, &range ) == noErr && range.mMaximum > 0 ) {
		if( frames < range.mMinimum )
			frames = (UInt32)range.mMinimum;
		if( frames > range.mMaximum )
			frames = (UInt32)range.mMaximum;
	}
	if( !err )
		err = AudioUnitSetProperty( snd.unit, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0,
			&frames, sizeof( frames ) );

	memset( &format, 0, sizeof( format ) );
	format.mSampleRate = rate;
	format.mFormatID = kAudioFormatLinearPCM;
	format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
	format.mChannelsPerFrame = 2;
	format.mBitsPerChannel = 32;
	format.mBytesPerFrame = 8;
	format.mFramesPerPacket = 1;
	format.mBytesPerPacket = 8;
	if( !err )
		err = AudioUnitSetProperty( snd.unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format,
			sizeof( format ) );
	if( !err )
		err = AudioUnitSetProperty( snd.unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0,
			&maxSlice, sizeof( maxSlice ) );
	if( !err )
		err = AudioUnitSetProperty( snd.unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
			&callback, sizeof( callback ) );
	if( !err )
		err = AudioUnitInitialize( snd.unit );
	if( err ) {
		SND_Printf( "sound: the output unit failed (%d)\n", (int)err );
		AudioComponentInstanceDispose( snd.unit );
		snd.unit = NULL;
		return 0;
	}

	snd.device = device;
	SND_DeviceString( device, kAudioObjectPropertyName, snd.deviceName, sizeof( snd.deviceName ) );
	snd.bufferFrames = (int)SND_DeviceU32( device, kAudioDevicePropertyBufferFrameSize,
		kAudioObjectPropertyScopeGlobal );
	snd.latency = SND_DeviceU32( device, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput );
	a.mSelector = kAudioDevicePropertyStreams;
	a.mScope = kAudioObjectPropertyScopeOutput;
	size = sizeof( streams );
	if( AudioObjectGetPropertyData( device, &a, 0, NULL, &size, streams ) == noErr && size >= sizeof( streams[0] ) )
		snd.latency += SND_DeviceU32( streams[0], kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal );
	snd.latency /= rate;

	for( i = 0; i < (int)( sizeof( snd_deviceEvents ) / sizeof( snd_deviceEvents[0] ) ); i++ )
		AudioObjectAddPropertyListener( device, &snd_deviceEvents[i], SND_Listener, NULL );
	if( snd.followDefault )
		AudioObjectAddPropertyListener( kAudioObjectSystemObject, &snd_defaultDevice, SND_Listener, NULL );

	SND_SetRate( rate );
	err = AudioOutputUnitStart( snd.unit );
	if( err ) {
		SND_Printf( "sound: the output unit didn't start (%d)\n", (int)err );
		SND_CloseDevice();
		return 0;
	}
	SND_Printf( "sound: %s, %.0f Hz, %d frames (%.2f ms), latency %.2f ms\n", snd.deviceName, rate,
		snd.bufferFrames, snd.bufferFrames * 1000.0 / rate, snd.latency * 1000.0 );
	return 1;
}

/*
==============================================================================

THE GAME THREAD

==============================================================================
*/

static void SND_Flush( void );

/*
===============
MacSnd_Init
===============
*/
int MacSnd_Init( const macSndConfig_t *config )
{
	int		i;

	if( snd.initialized )
		MacSnd_Shutdown();
	memset( &snd, 0, sizeof( snd ) );
	snd.print = config->print;
	snd.wantFrames = config->bufferFrames > 0 ? config->bufferFrames : 64;
	if( snd.wantFrames < 32 )
		snd.wantFrames = 32;
	if( snd.wantFrames > 512 )
		snd.wantFrames = 512;
	snd.maxVoices = config->maxVoices > 0 && config->maxVoices <= 65535 ? config->maxVoices : 128;
	snd.maxTails = config->maxTails > 0 ? config->maxTails : 32;
	snd.maxSounds = config->maxSounds > 0 && config->maxSounds <= 65535 ? config->maxSounds : 2048;
	if( config->device )
		snprintf( snd.wantDevice, sizeof( snd.wantDevice ), "%s", config->device );

	if( !SND_BuildKernels() )
		return 0;
	snd.ring = calloc( SND_RING, sizeof( *snd.ring ) );
	snd.mail = calloc( snd.maxVoices, sizeof( *snd.mail ) );
	snd.pub = calloc( snd.maxVoices, sizeof( *snd.pub ) );
	snd.startTime = calloc( snd.maxVoices, sizeof( *snd.startTime ) );
	snd.sounds = calloc( snd.maxSounds, sizeof( *snd.sounds ) );
	snd.game = calloc( snd.maxVoices, sizeof( *snd.game ) );
	snd.freeSlots = calloc( snd.maxSounds, sizeof( int ) );
	snd.freeing = calloc( snd.maxSounds, sizeof( int ) );
	snd.voices = calloc( snd.maxVoices, sizeof( *snd.voices ) );
	snd.active = calloc( snd.maxVoices, sizeof( int ) );
	snd.tails = calloc( snd.maxTails, sizeof( *snd.tails ) );
	snd.dying = calloc( snd.maxSounds, sizeof( int ) );
	if( !snd.ring || !snd.mail || !snd.pub || !snd.startTime || !snd.sounds || !snd.game || !snd.freeSlots
		|| !snd.freeing || !snd.voices || !snd.active || !snd.tails || !snd.dying ) {
		snd.initialized = 1;
		MacSnd_Shutdown();
		return 0;
	}
	for( i = 0; i < snd.maxVoices; i++ ) {
		snd.voices[i].active = -1;
		snd.game[i].stopped = 1;
	}
	for( i = 0; i < snd.maxSounds; i++ ) {
		snd.freeSlots[i] = snd.maxSounds - 1 - i;
		snd.sounds[i].gen = 1;
	}
	snd.numFreeSlots = snd.maxSounds;
	snd.master = 1.0f;
	snd.limiter = 1.0f;
	atomic_store( &snd.masterBits, SND_FloatBits( 1.0f ) );
	atomic_store( &snd.fadeBits, SND_FloatBits( 1.5f ) );
	mach_timebase_info( &snd.timebase );
	snd.initTime = mach_absolute_time();
	snd.initialized = 1;

	if( config->offlineRate > 0.0 ) {
		snd.offline = 1;
		snd.rate = config->offlineRate;
		snd.bufferFrames = snd.wantFrames;
		snprintf( snd.deviceName, sizeof( snd.deviceName ), "offline" );
		return 1;
	}
	snd.rate = 48000.0;		// until a device says otherwise
	SND_OpenDevice();
	return 1;
}

/*
===============
MacSnd_Shutdown
===============
*/
void MacSnd_Shutdown( void )
{
	int		i;

	if( !snd.initialized )
		return;
	SND_CloseDevice();
	if( snd.sounds ) {
		for( i = 0; i < snd.maxSounds; i++ )
			free( snd.sounds[i].alloc );
	}
	free( snd.ring );
	free( snd.mail );
	free( (void *)snd.pub );
	free( (void *)snd.startTime );
	free( snd.sounds );
	free( snd.game );
	free( snd.freeSlots );
	free( snd.freeing );
	free( snd.voices );
	free( snd.active );
	free( snd.tails );
	free( snd.dying );
	free( snd.capture );
	memset( &snd, 0, sizeof( snd ) );
}

/*
===============
MacSnd_Update

Re-creates the output unit after a device or rate change (the voices go on where they were), frees the sounds the
IO thread let go of, and with no device takes the commands itself.
===============
*/
void MacSnd_Update( void )
{
	int		i;

	if( !snd.initialized )
		return;
	if( !snd.offline && atomic_exchange_explicit( &snd.reopen, 0, memory_order_relaxed ) ) {
		SND_CloseDevice();
		SND_OpenDevice();
		atomic_fetch_add_explicit( &snd.reopens, 1, memory_order_relaxed );
	}
	if( !snd.offline && !snd.unit )
		SND_Render( NULL, 0, 0 );
	SND_Flush();

	for( i = 0; i < snd.numFreeing; ) {
		sndSound_t	*s = &snd.sounds[snd.freeing[i]];

		if( atomic_load_explicit( &s->released, memory_order_acquire ) != s->gen ) {
			i++;
			continue;
		}
		snd.soundBytes -= (size_t)( s->frames + 2 * SND_GUARD ) * s->channels * sizeof( short );
		free( s->alloc );
		s->alloc = NULL;
		s->state = SOUND_FREE;
		s->freePosted = 0;
		s->gen = s->gen == 0xffff ? 1 : s->gen + 1;
		snd.numSounds--;
		snd.freeSlots[snd.numFreeSlots++] = snd.freeing[i];
		snd.freeing[i] = snd.freeing[--snd.numFreeing];
	}
}

void MacSnd_Restart( void )
{
	if( !snd.initialized || snd.offline )
		return;
	atomic_store( &snd.reopen, 1 );
	MacSnd_Update();
}

static int SND_Post( const sndCmd_t *c )
{
	const uint32_t	head = atomic_load_explicit( &snd.ringHead, memory_order_relaxed );

	if( head - atomic_load_explicit( &snd.ringTail, memory_order_acquire ) >= SND_RING ) {
		atomic_fetch_add_explicit( &snd.ringDrops, 1, memory_order_relaxed );
		return 0;
	}
	snd.ring[head & ( SND_RING - 1 )] = *c;
	atomic_store_explicit( &snd.ringHead, head + 1, memory_order_release );
	return 1;
}

// the stops and frees a full ring held back go first (a start on the voice since then replaced its stop)
static void SND_Flush( void )
{
	sndCmd_t	c;
	int			i;

	if( !snd.numPending )
		return;
	memset( &c, 0, sizeof( c ) );
	c.op = CMD_STOP;
	for( i = 0; i < snd.maxVoices && snd.numPending; i++ ) {
		if( !snd.game[i].stopPending )
			continue;
		c.voice = (uint16_t)i;
		if( !SND_Post( &c ) )
			return;
		snd.game[i].stopPending = 0;
		snd.numPending--;
	}
	c.op = CMD_FREE;
	c.voice = 0;
	for( i = 0; i < snd.numFreeing && snd.numPending; i++ ) {
		sndSound_t	*s = &snd.sounds[snd.freeing[i]];

		if( s->freePosted )
			continue;
		c.sound = snd.freeing[i];
		if( !SND_Post( &c ) )
			return;
		s->freePosted = 1;
		snd.numPending--;
	}
}

// a stop that must not be lost: held back when the ring is full
static void SND_PostStop( int voice )
{
	sndGameVoice_t	*g = &snd.game[voice];
	sndCmd_t		c;

	memset( &c, 0, sizeof( c ) );
	c.op = CMD_STOP;
	c.voice = (uint16_t)voice;
	if( !g->stopPending && !SND_Post( &c ) ) {
		g->stopPending = 1;
		snd.numPending++;
	}
}

static sndSound_t *SND_Sound( int sound )
{
	const int	slot = ( sound & 0xffff ) - 1;
	sndSound_t	*s;

	if( !snd.initialized || slot < 0 || slot >= snd.maxSounds )
		return NULL;
	s = &snd.sounds[slot];
	if( s->state != SOUND_LIVE || s->gen != (uint32_t)( sound >> 16 ) )
		return NULL;
	return s;
}

static sndGameVoice_t *SND_GameVoice( int voice )
{
	if( !snd.initialized || voice < 0 || voice >= snd.maxVoices )
		return NULL;
	return &snd.game[voice];
}

static inline float SND_Sane( float f )
{
	return isfinite( f ) ? f : 0.0f;
}

/*
===============
MacSnd_CreateSound
===============
*/
int MacSnd_CreateSound( const short *pcm, int frames, int channels, int rate )
{
	sndSound_t	*s;
	size_t		bytes;
	int			slot;

	if( !snd.initialized || frames <= 0 || frames > INT_MAX / 4 - 2 * SND_GUARD || ( channels != 1 && channels != 2 )
		|| rate <= 0 || rate > 768000 || !snd.numFreeSlots )
		return 0;
	bytes = (size_t)( frames + 2 * SND_GUARD ) * channels * sizeof( short );
	slot = snd.freeSlots[snd.numFreeSlots - 1];
	s = &snd.sounds[slot];
	s->alloc = malloc( bytes );
	if( !s->alloc )
		return 0;
	snd.numFreeSlots--;
	memset( s->alloc, 0, bytes );
	if( pcm )
		memcpy( s->alloc + SND_GUARD * channels, pcm, (size_t)frames * channels * sizeof( short ) );
	s->frames = frames;
	s->channels = channels;
	s->rate = rate;
	s->state = SOUND_LIVE;
	s->stream = 0;
	s->written = 0;
	atomic_store_explicit( &s->committed, 0, memory_order_relaxed );
	atomic_store_explicit( &s->played, 0, memory_order_relaxed );
	atomic_store_explicit( &s->ended, 0, memory_order_relaxed );
	snd.numSounds++;
	snd.soundBytes += bytes;
	return (int)( s->gen << 16 ) | ( slot + 1 );
}

short *MacSnd_SoundData( int sound )
{
	sndSound_t	*s = SND_Sound( sound );

	return s ? s->alloc + SND_GUARD * s->channels : NULL;
}

void MacSnd_FreeSound( int sound )
{
	sndSound_t	*s = SND_Sound( sound );
	sndCmd_t	c;

	if( !s )
		return;
	SND_Flush();
	memset( &c, 0, sizeof( c ) );
	c.op = CMD_FREE;
	c.sound = ( sound & 0xffff ) - 1;
	s->state = SOUND_FREEING;
	s->freePosted = SND_Post( &c );
	snd.numPending += !s->freePosted;
	snd.freeing[snd.numFreeing++] = c.sound;
}

/*
===============
MacSnd_CreateStream

A ring the game writes ahead of the voice playing it. The voice reads only what was committed (short of it, it
holds, silent) and starts where the stream's last voice stopped (its first frame for a new stream). Behind the voice
SND_GUARD frames stay for its window, and SND_GUARD more for the silence MacSnd_StreamEnd writes after the last frame.
===============
*/
int MacSnd_CreateStream( int frames, int channels, int rate )
{
	sndSound_t	*s;
	int			sound;

	if( frames <= 4 * SND_GUARD )
		return 0;
	sound = MacSnd_CreateSound( NULL, frames, channels, rate );
	s = SND_Sound( sound );
	if( !s )
		return 0;
	s->stream = 1;
	return sound;
}

short *MacSnd_StreamBuffer( int sound, int *frames )
{
	sndSound_t	*s = SND_Sound( sound );
	int64_t		space;
	int			at;

	*frames = 0;
	if( !s || !s->stream || atomic_load_explicit( &s->ended, memory_order_relaxed ) )
		return NULL;
	space = (int64_t)s->frames - 2 * SND_GUARD
		- (int64_t)( s->written - atomic_load_explicit( &s->played, memory_order_acquire ) );
	if( space <= 0 )
		return NULL;
	at = (int)( s->written % (uint64_t)s->frames );
	if( space > s->frames - at )
		space = s->frames - at;
	*frames = (int)space;
	return s->alloc + (size_t)( SND_GUARD + at ) * s->channels;
}

void MacSnd_StreamCommit( int sound, int frames )
{
	sndSound_t	*s = SND_Sound( sound );
	int			space;

	if( !s || !s->stream || frames <= 0 || !MacSnd_StreamBuffer( sound, &space ) )
		return;
	s->written += frames < space ? frames : space;
	atomic_store_explicit( &s->committed, s->written, memory_order_release );
}

void MacSnd_StreamEnd( int sound )
{
	sndSound_t	*s = SND_Sound( sound );
	int			i;

	if( !s || !s->stream || atomic_load_explicit( &s->ended, memory_order_relaxed ) )
		return;
	// the voice's window past the last frame reads silence (in the space MacSnd_StreamBuffer kept for it)
	for( i = 0; i < SND_GUARD; i++ ) {
		const int	at = (int)( ( s->written + i ) % (uint64_t)s->frames );

		memset( s->alloc + (size_t)( SND_GUARD + at ) * s->channels, 0, sizeof( short ) * s->channels );
	}
	atomic_store_explicit( &s->ended, 1, memory_order_release );
}

unsigned long long MacSnd_StreamPlayed( int sound )
{
	sndSound_t	*s = SND_Sound( sound );

	return s && s->stream ? atomic_load_explicit( &s->played, memory_order_acquire ) : 0;
}

static void SND_PostParams( int voice, float gainL, float gainR, float rate )
{
	sndMailbox_t	*m = &snd.mail[voice];
	const uint32_t	s = snd.game[voice].seq;

	atomic_store_explicit( &m->seq, s + 1, memory_order_relaxed );
	atomic_thread_fence( memory_order_release );
	atomic_store_explicit( &m->gainL, SND_FloatBits( gainL ), memory_order_relaxed );
	atomic_store_explicit( &m->gainR, SND_FloatBits( gainR ), memory_order_relaxed );
	atomic_store_explicit( &m->rate, SND_FloatBits( rate ), memory_order_relaxed );
	atomic_store_explicit( &m->seq, s + 2, memory_order_release );
	snd.game[voice].seq = s + 2;
}

/*
===============
MacSnd_Start
===============
*/
void MacSnd_Start( int voice, const macSndStart_t *start )
{
	sndGameVoice_t	*g = SND_GameVoice( voice );
	sndSound_t		*s = SND_Sound( start->sound );
	sndCmd_t		c;

	if( !g )
		return;
	if( !s ) {
		MacSnd_Stop( voice );
		return;
	}
	SND_Flush();
	memset( &c, 0, sizeof( c ) );
	c.op = CMD_START;
	c.paused = start->paused != 0;
	c.voice = (uint16_t)voice;
	c.sound = ( start->sound & 0xffff ) - 1;
	c.frame = start->startFrame;
	c.loopStart = start->loopStart;
	c.loopEnd = start->loopEnd;
	c.loopCount = start->loopCount;
	c.gainL = SND_Sane( start->gainL );
	c.gainR = SND_Sane( start->gainR );
	c.rate = SND_Sane( start->rate );
	SND_PostParams( voice, c.gainL, c.gainR, c.rate );
	c.seq = g->seq;
	c.gen = ( g->gen + 1 ) & SND_GENMASK;
	atomic_store_explicit( &snd.startTime[voice], 0, memory_order_relaxed );
	if( !SND_Post( &c ) ) {
		// lost to a full ring: the voice stops instead (what it played mustn't go on)
		g->stopped = 1;
		g->paused = 0;
		SND_PostStop( voice );
		return;
	}
	if( g->stopPending ) {
		g->stopPending = 0;		// the start replaces it
		snd.numPending--;
	}
	g->gen = c.gen;
	g->stopped = 0;
	g->paused = c.paused;
	g->frame = c.frame;
}

void MacSnd_SetParams( int voice, float gainL, float gainR, float rate )
{
	if( SND_GameVoice( voice ) )
		SND_PostParams( voice, SND_Sane( gainL ), SND_Sane( gainR ), SND_Sane( rate ) );
}

void MacSnd_Stop( int voice )
{
	sndGameVoice_t	*g = SND_GameVoice( voice );

	if( !g || g->stopped )
		return;
	SND_Flush();
	SND_PostStop( voice );
	g->stopped = 1;
	g->paused = 0;
}

void MacSnd_Pause( int voice, int paused )
{
	sndGameVoice_t	*g = SND_GameVoice( voice );
	sndCmd_t		c;

	paused = paused != 0;
	if( !g || g->stopped || g->paused == paused )
		return;
	SND_Flush();
	memset( &c, 0, sizeof( c ) );
	c.op = paused ? CMD_PAUSE : CMD_RESUME;
	c.voice = (uint16_t)voice;
	if( SND_Post( &c ) )
		g->paused = paused;
}

void MacSnd_SetLoop( int voice, int loopStart, int loopEnd, int loopCount )
{
	sndGameVoice_t	*g = SND_GameVoice( voice );
	sndCmd_t		c;

	if( !g || g->stopped )
		return;
	SND_Flush();
	memset( &c, 0, sizeof( c ) );
	c.op = CMD_LOOP;
	c.voice = (uint16_t)voice;
	c.loopStart = loopStart;
	c.loopEnd = loopEnd;
	c.loopCount = loopCount;
	SND_Post( &c );
}

void MacSnd_Seek( int voice, int frame )
{
	sndGameVoice_t	*g = SND_GameVoice( voice );
	sndCmd_t		c;

	if( !g || MacSnd_Status( voice ) == MACSND_STOPPED )
		return;
	SND_Flush();
	memset( &c, 0, sizeof( c ) );
	c.op = CMD_SEEK;
	c.voice = (uint16_t)voice;
	c.frame = frame < 0 ? 0 : frame;
	c.gen = ( g->gen + 1 ) & SND_GENMASK;
	if( SND_Post( &c ) ) {
		g->gen = c.gen;
		g->frame = c.frame;
	}
}

/*
===============
MacSnd_Status

The game's own intent until the IO thread has caught up with the voice's last start or seek; then whether it
ended.
===============
*/
int MacSnd_Status( int voice )
{
	sndGameVoice_t	*g = SND_GameVoice( voice );
	uint64_t		p;

	if( !g || g->stopped )
		return MACSND_STOPPED;
	p = atomic_load_explicit( &snd.pub[voice], memory_order_acquire );
	if( (uint32_t)( p >> 34 ) == g->gen && ( ( p >> 32 ) & 3 ) == MACSND_STOPPED ) {
		g->stopped = 1;
		g->paused = 0;
		return MACSND_STOPPED;
	}
	return g->paused ? MACSND_PAUSED : MACSND_PLAYING;
}

int MacSnd_Position( int voice )
{
	sndGameVoice_t	*g = SND_GameVoice( voice );
	uint64_t		p;

	if( !g )
		return 0;
	p = atomic_load_explicit( &snd.pub[voice], memory_order_acquire );
	if( (uint32_t)( p >> 34 ) != g->gen )
		return g->frame;
	return (int)(uint32_t)p;
}

unsigned long long MacSnd_StartTime( int voice )
{
	if( !SND_GameVoice( voice ) )
		return 0;
	return atomic_load_explicit( &snd.startTime[voice], memory_order_relaxed );
}

void MacSnd_SetMaster( float gain )
{
	gain = SND_Sane( gain );
	atomic_store_explicit( &snd.masterBits, SND_FloatBits( gain < 0.0f ? 0.0f : gain ), memory_order_relaxed );
}

void MacSnd_SetResampler( int mode )
{
	atomic_store_explicit( &snd.resampler, mode == MACSND_LINEAR ? MACSND_LINEAR : MACSND_SINC,
		memory_order_relaxed );
}

void MacSnd_SetStopFade( float ms )
{
	ms = SND_Sane( ms );
	atomic_store_explicit( &snd.fadeBits, SND_FloatBits( ms < 0.0f ? 0.0f : ms > 50.0f ? 50.0f : ms ),
		memory_order_relaxed );
}

void MacSnd_GetStats( macSndStats_t *stats )
{
	const double	tick = snd.initialized ? (double)snd.timebase.numer / snd.timebase.denom * 1e-9 : 0.0;
	int				i;

	memset( stats, 0, sizeof( *stats ) );
	if( !snd.initialized )
		return;
	snprintf( stats->device, sizeof( stats->device ), "%s", snd.deviceName );
	stats->rate = snd.rate;
	stats->bufferFrames = snd.bufferFrames;
	stats->latency = snd.latency;
	stats->callbacks = atomic_load_explicit( &snd.callbacks, memory_order_relaxed );
	stats->overloads = atomic_load_explicit( &snd.overloads, memory_order_relaxed );
	stats->ringDrops = atomic_load_explicit( &snd.ringDrops, memory_order_relaxed );
	stats->reopens = atomic_load_explicit( &snd.reopens, memory_order_relaxed );
	stats->voices = (int)atomic_load_explicit( &snd.voicesMixed, memory_order_relaxed );
	stats->sounds = snd.numSounds;
	stats->soundBytes = snd.soundBytes;
	stats->renderSeconds = atomic_load_explicit( &snd.pubRenderTicks, memory_order_relaxed ) * tick;
	stats->maxRender = atomic_load_explicit( &snd.pubMaxTicks, memory_order_relaxed ) * tick;
	stats->wallSeconds = ( mach_absolute_time() - snd.initTime ) * tick;
	for( i = 0; i < 16; i++ )
		stats->cpuCallbacks[i] = atomic_load_explicit( &snd.cpu[i], memory_order_relaxed );
	stats->captureDrops = atomic_load_explicit( &snd.captureDrops, memory_order_relaxed );
	stats->underruns = atomic_load_explicit( &snd.underruns, memory_order_relaxed );
}

/*
===============
MacSnd_Capture

The ring is allocated once (the IO thread may still be writing a block into it when a capture stops); a new capture
starts after what is in it.
===============
*/
int MacSnd_Capture( int frames, int mute )
{
	if( !snd.initialized )
		return 0;
	atomic_store_explicit( &snd.captureOn, 0, memory_order_relaxed );
	atomic_store_explicit( &snd.captureMute, 0, memory_order_relaxed );
	if( frames <= 0 )
		return 1;
	if( !snd.capture ) {
		snd.capture = calloc( (size_t)frames * 2, sizeof( float ) );
		if( !snd.capture )
			return 0;
		snd.captureFrames = (uint32_t)frames;
	}
	atomic_store_explicit( &snd.captureTail, atomic_load_explicit( &snd.captureHead, memory_order_acquire ),
		memory_order_release );
	atomic_store_explicit( &snd.captureMute, mute != 0, memory_order_relaxed );
	atomic_store_explicit( &snd.captureOn, 1, memory_order_relaxed );
	return 1;
}

int MacSnd_ReadCapture( float *out, int maxFrames )
{
	const uint32_t	head = snd.capture ? atomic_load_explicit( &snd.captureHead, memory_order_acquire ) : 0;
	uint32_t		tail, i, n;

	if( !snd.capture || maxFrames <= 0 )
		return 0;
	tail = atomic_load_explicit( &snd.captureTail, memory_order_relaxed );
	n = head - tail < (uint32_t)maxFrames ? head - tail : (uint32_t)maxFrames;
	for( i = 0; i < n; i++ ) {
		const uint32_t	k = ( tail + i ) % snd.captureFrames;

		out[2 * i] = snd.capture[2 * k];
		out[2 * i + 1] = snd.capture[2 * k + 1];
	}
	atomic_store_explicit( &snd.captureTail, tail + n, memory_order_release );
	return (int)n;
}

void MacSnd_Render( float *out, int frames )
{
	if( snd.initialized && snd.offline && out && frames > 0 )
		SND_Render( out, frames, snd.frameCount );
}
