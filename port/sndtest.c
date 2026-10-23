// sndtest.c -- the native sound engine's mixer (code/macos/macos_snd.c) on its own (port/sndtest.sh builds and runs
// it). The offline tests render through the IO thread's own code (MacSnd_Render) and check it to the sample; the
// stress test renders on a second thread, as the IO thread does, while this one posts at random (built with
// ThreadSanitizer); the device modes play silence on the real output: start -> output, CPU, overloads.
//
//   sndtest [test ...]                    offline: resample loop offset pause ramp limiter free misc capture stream math ima (all by
//                                         default); math and ima are the native driver's (snd_native_math.h)
//   sndtest stress [seconds]              the threaded stress test
//   sndtest device [frames ...]           start -> output at random phases, CPU at 0/16/40/96 voices, a restart
//   sndtest load frames seconds [busy] [voices]   96 voices for a while, overloads counted; busy = spinning threads
//   sndtest devices                       the output devices
//
// Every device mode is silent: the sounds are zeros and the master gain is 0. SNDTEST_DEVICE names another output.

#include "../code/macos/macos_snd.h"
#include "../code/client/snd_native_math.h"

#include <CoreAudio/CoreAudio.h>
#include <libproc.h>
#include <mach/mach_time.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>


#define RATE		48000.0
#define BLOCK		64
#define CEILING		0.966051f

static int			fails, checks;
static mach_timebase_info_data_t	tb;

static double ms( uint64_t ticks ) { return (double)ticks * tb.numer / tb.denom / 1e6; }

static void check( int ok, const char *fmt, ... ) __attribute__( ( format( printf, 2, 3 ) ) );
static void check( int ok, const char *fmt, ... )
{
	va_list ap;

	checks++;
	if ( !ok )
		fails++;
	printf( "  %s ", ok ? "ok  " : "FAIL" );
	va_start( ap, fmt );
	vprintf( fmt, ap );
	va_end( ap );
	printf( "\n" );
}

static void quiet( const char *msg ) { (void)msg; }
static void say( const char *msg ) { printf( "  %s", msg ); }

static void offline( void )
{
	macSndConfig_t cfg = { 0 };

	cfg.bufferFrames = BLOCK;
	cfg.offlineRate = RATE;
	cfg.print = quiet;
	if ( !MacSnd_Init( &cfg ) ) {
		printf( "MacSnd_Init failed\n" );
		exit( 1 );
	}
}

// renders in callbacks of BLOCK frames (the last one shorter)
static void render( float *out, int frames )
{
	static float scratch[2 * 4096];
	int n;

	while ( frames > 0 ) {
		n = frames < BLOCK ? frames : BLOCK;
		MacSnd_Render( out ? out : scratch, n );
		if ( out )
			out += 2 * n;
		frames -= n;
	}
}

static macSndStart_t play( int sound, float gain )
{
	macSndStart_t s = { 0 };

	s.sound = sound;
	s.gainL = s.gainR = gain;
	s.loopCount = 1;
	return s;
}

static int sound_ramp( int frames, int rate )	// sample k = ( k + 1 ) * 16: every frame tells where it came from
{
	short *pcm = malloc( sizeof( short ) * frames );
	int i, h;

	for ( i = 0; i < frames; i++ )
		pcm[i] = (short)( ( i + 1 ) * 16 % 32000 );
	h = MacSnd_CreateSound( pcm, frames, 1, rate );
	free( pcm );
	return h;
}

static float ramp_value( int k ) { return (float)( ( k + 1 ) * 16 % 32000 ) / 32768.0f; }

static int sound_dc( int frames, int rate, short v )
{
	short *pcm = malloc( sizeof( short ) * frames );
	int i, h;

	for ( i = 0; i < frames; i++ )
		pcm[i] = v;
	h = MacSnd_CreateSound( pcm, frames, 1, rate );
	free( pcm );
	return h;
}

static int sound_sine( int frames, int rate, double hz, double amp, int channels )
{
	short *pcm = malloc( sizeof( short ) * frames * channels );
	int i, c, h;

	for ( i = 0; i < frames; i++ ) {
		for ( c = 0; c < channels; c++ )
			pcm[i * channels + c] = (short)lrint( ( c ? -amp : amp ) * 32767.0 * sin( 2.0 * M_PI * hz * i / rate ) );
	}
	h = MacSnd_CreateSound( pcm, frames, channels, rate );
	free( pcm );
	return h;
}

// a tone's amplitude at hz in one channel of out (4-term Blackman-Harris window)
static double tone( const float *out, int channel, int n, double hz )
{
	double re = 0, im = 0, wsum = 0, t, w, ph;
	int i;

	for ( i = 0; i < n; i++ ) {
		t = 2.0 * M_PI * i / ( n - 1 );
		w = 0.35875 - 0.48829 * cos( t ) + 0.14128 * cos( 2 * t ) - 0.01168 * cos( 3 * t );
		ph = 2.0 * M_PI * hz * i / RATE;
		re += out[2 * i + channel] * w * cos( ph );
		im -= out[2 * i + channel] * w * sin( ph );
		wsum += w;
	}
	return 2.0 * sqrt( re * re + im * im ) / wsum;
}

static double db( double x ) { return 20.0 * log10( x > 1e-12 ? x : 1e-12 ); }

static double fold( double hz )	// a frequency as it lands below the output's Nyquist frequency
{
	hz = fmod( fabs( hz ), RATE );
	return hz > RATE / 2 ? RATE - hz : hz;
}

/*
==============================================================================
resample: passband and aliasing at steps 0.46, 0.92 and 1.2
==============================================================================
*/

// one tone through the mixer: its level and the strongest image or alias, in dB against the source's
static void tone_through( int srcRate, double cycles, int mode, double *pass, double *image, double *imageHz )
{
	static float out[2 * 40000];
	const double hz = cycles * srcRate;
	macSndStart_t s;
	int snd;

	offline();
	MacSnd_SetResampler( mode );
	snd = sound_sine( srcRate * 2, srcRate, hz, 0.5, 1 );
	s = play( snd, 1.0f );
	MacSnd_Start( 0, &s );
	render( out, 40000 );
	*imageHz = fold( srcRate - hz );
	*pass = db( tone( out + 2 * 4096, 0, 32768, fold( hz ) ) / 0.5 );
	*image = db( tone( out + 2 * 4096, 0, 32768, *imageHz ) / 0.5 );
	MacSnd_Shutdown();
}

static void test_resample( void )
{
	static const struct { int rate; const char *name; } cases[] = {
		{ 22050, "22050 Hz, step 0.46" }, { 44100, "44100 Hz, step 0.92" }, { 57600, "57600 Hz, step 1.2" } };
	static const double passTones[] = { 0.05, 0.15, 0.25, 0.30, 0.35, 0.40 };
	double pass, image, imageHz, worst, linPass, linImage;
	int c, i, top;

	printf( "resample: the passband and the images or aliases (dB against the source tone)\n" );
	for ( c = 0; c < 3; c++ ) {
		const int rate = cases[c].rate;

		// upsampling: flat to 0.40 of the source rate; at step 1.2 the cutoff is the output's Nyquist frequency
		top = rate < RATE ? 6 : 4;
		worst = 0;
		for ( i = 0; i < top; i++ ) {
			tone_through( rate, passTones[i], MACSND_SINC, &pass, &image, &imageHz );
			if ( fabs( pass ) > fabs( worst ) )
				worst = pass;
		}
		tone_through( rate, passTones[top - 1], MACSND_LINEAR, &linPass, &linImage, &imageHz );
		check( fabs( worst ) < 0.05, "%s: passband to %.2f of the source rate (%.0f Hz) within %+.4f dB (linear %+.2f dB there)",
			cases[c].name, passTones[top - 1], passTones[top - 1] * rate, worst, linPass );

		if ( rate < RATE ) {
			// upsampling: the image of a tone at 0.30 (and, in the transition band, 0.40) of the source rate
			tone_through( rate, 0.30, MACSND_SINC, &pass, &image, &imageHz );
			tone_through( rate, 0.30, MACSND_LINEAR, &pass, &linImage, &imageHz );
			check( image < -70.0, "%s: image of %.0f Hz at %.0f Hz %.1f dB (linear %.1f dB)", cases[c].name, 0.30 * rate,
				imageHz, image, linImage );
			tone_through( rate, 0.40, MACSND_SINC, &pass, &image, &imageHz );
			tone_through( rate, 0.40, MACSND_LINEAR, &linPass, &linImage, &imageHz );
			check( image < -65.0, "%s: image of %.0f Hz at %.0f Hz %.1f dB (linear %.1f dB)", cases[c].name, 0.40 * rate,
				imageHz, image, linImage );
			tone_through( rate, 0.45, MACSND_SINC, &pass, &image, &imageHz );
			tone_through( rate, 0.45, MACSND_LINEAR, &linPass, &linImage, &imageHz );
			printf( "       %s: in the transition band, %.0f Hz at %+.2f dB (linear %+.2f), its image at %.0f Hz %.1f dB (linear %.1f)\n",
				cases[c].name, 0.45 * rate, pass, linPass, imageHz, image, linImage );
		} else {
			// downsampling: a tone above the output's Nyquist frequency must not alias into the audible band (the
			// kernel's transition is centred on the output's Nyquist frequency, as the upsampling one is on the
			// source's: what aliases from inside it lands above ~20 kHz)
			double alias, lin;

			tone_through( rate, 0.49, MACSND_SINC, &alias, &image, &imageHz );
			tone_through( rate, 0.49, MACSND_LINEAR, &lin, &image, &imageHz );
			check( alias < -60.0, "%s: %.0f Hz (above 24000) aliased to %.0f Hz at %.1f dB (linear %.1f dB)",
				cases[c].name, 0.49 * rate, fold( 0.49 * rate ), alias, lin );
			tone_through( rate, 0.45, MACSND_SINC, &alias, &image, &imageHz );
			tone_through( rate, 0.45, MACSND_LINEAR, &lin, &image, &imageHz );
			tone_through( rate, 0.38, MACSND_SINC, &pass, &image, &imageHz );
			printf( "       %s: in the transition band, %.0f Hz at %+.2f dB; %.0f Hz aliased to %.0f Hz at %.1f dB (linear %.1f dB)\n",
				cases[c].name, 0.38 * rate, pass, 0.45 * rate, fold( 0.45 * rate ), alias, lin );
		}
	}
}

/*
==============================================================================
loop: loop blocks and counts to the frame
==============================================================================
*/

static int expect_frames( const float *out, int n, const int *idx, const char *what )
{
	int f, bad = -1;

	for ( f = 0; f < n; f++ ) {
		float want = idx[f] < 0 ? 0.0f : ramp_value( idx[f] );
		if ( out[2 * f] != want || out[2 * f + 1] != want ) {
			bad = f;
			break;
		}
	}
	if ( bad >= 0 )
		check( 0, "%s: frame %d is %.6f, not %.6f (sample %d)", what, bad, out[2 * bad],
			idx[bad] < 0 ? 0.0f : ramp_value( idx[bad] ), idx[bad] );
	return bad < 0;
}

static void test_loop( void )
{
	static float out[2 * 20000];
	static int idx[20000];
	macSndStart_t s;
	int snd, f, n, k, status, ok;
	double first, step, endHz;

	printf( "loop: loop blocks and counts, to the frame\n" );
	offline();
	MacSnd_SetStopFade( 0 );
	snd = sound_ramp( 1000, 48000 );

	// count 3: the start to the block's end, the block again, then the block and on to the sound's end
	s = play( snd, 1.0f );
	s.loopStart = 200; s.loopEnd = 700; s.loopCount = 3;
	MacSnd_Start( 0, &s );
	render( out, 1984 );
	status = MacSnd_Status( 0 );
	render( out + 2 * 1984, 2400 - 1984 );
	for ( f = 0, n = 0; f < 700; f++ ) idx[n++] = f;
	for ( f = 200; f < 700; f++ ) idx[n++] = f;
	for ( f = 200; f < 1000; f++ ) idx[n++] = f;
	while ( n < 2400 ) idx[n++] = -1;
	ok = expect_frames( out, 2400, idx, "count 3" );
	check( ok && status == MACSND_PLAYING && MacSnd_Status( 0 ) == MACSND_STOPPED,
		"block 200-700, count 3: 2000 frames, every one the right sample; playing at 1984, stopped after 2000" );

	// count 0: forever, from inside the block
	s.loopCount = 0; s.startFrame = 0;
	MacSnd_Start( 0, &s );
	render( out, 20000 );
	for ( f = 0; f < 20000; f++ ) idx[f] = f < 700 ? f : 200 + ( f - 700 ) % 500;
	ok = expect_frames( out, 20000, idx, "count 0" );
	check( ok && MacSnd_Status( 0 ) == MACSND_PLAYING && MacSnd_Position( 0 ) == idx[19999] + 1,
		"count 0: 20000 frames, 39 passes, still playing at %d", MacSnd_Position( 0 ) );

	// the count changed while it plays: the pass under way is the last, then the sound's end
	MacSnd_SetLoop( 0, 200, 700, 1 );
	k = MacSnd_Position( 0 );	// 200 + ( 20000 - 700 ) % 500 = 500
	render( out, 1000 );
	for ( f = 0; f < 1000; f++ ) idx[f] = k + f < 1000 ? k + f : -1;
	ok = expect_frames( out, 1000, idx, "count set to 1" );
	check( ok && MacSnd_Status( 0 ) == MACSND_STOPPED, "count set to 1 at %d: plays on to the end, no wrap", k );

	// the whole sound as the block (loopEnd 0), twice
	s = play( snd, 1.0f );
	s.loopCount = 2;
	MacSnd_Start( 0, &s );
	render( out, 2100 );
	for ( f = 0; f < 2100; f++ ) idx[f] = f < 2000 ? f % 1000 : -1;
	ok = expect_frames( out, 2100, idx, "whole sound twice" );
	check( ok, "no block (loopEnd 0), count 2: the sound twice, 2000 frames" );

	// a start beyond the block on a looping voice lands inside it
	s = play( snd, 1.0f );
	s.loopStart = 200; s.loopEnd = 700; s.loopCount = 0; s.startFrame = 900;
	MacSnd_Start( 0, &s );
	render( out, 64 );
	for ( f = 0; f < 64; f++ ) idx[f] = 400 + f;
	check( expect_frames( out, 64, idx, "start beyond the block" ), "start 900 with the block 200-700: plays from 400" );
	MacSnd_Shutdown();

	// at step 0.459 the voice ends where the source frames say: 2000 frames of the source = 4353.7 output frames
	offline();
	MacSnd_SetStopFade( 0 );
	snd = sound_ramp( 1000, 22050 );
	s = play( snd, 1.0f );
	s.loopStart = 200; s.loopEnd = 700; s.loopCount = 3;
	MacSnd_Start( 0, &s );
	render( NULL, 4352 );
	k = MacSnd_Status( 0 );
	for ( f = 4352; f < 4360 && MacSnd_Status( 0 ) == MACSND_PLAYING; f++ )
		MacSnd_Render( out, 1 );
	step = 22050.0 / 48000.0;
	check( k == MACSND_PLAYING && f == (int)ceil( 2000.0 / step ), "step 0.459, count 3: ends after %d output frames (%.1f expected)",
		f, 2000.0 / step );
	MacSnd_Shutdown();

	// across the wraps at step 0.459 the windows are gathered: a sine of exactly 10 periods in the block must come
	// out whole, pass after pass
	offline();
	snd = sound_sine( 1000, 22050, 22050.0 / 50.0, 0.5, 1 );
	s = play( snd, 1.0f );
	s.loopStart = 200; s.loopEnd = 700; s.loopCount = 0;
	MacSnd_Start( 0, &s );
	render( out, 20000 );
	endHz = 22050.0 / 50.0;
	{
		double maxWrap = 0, maxMid = 0, sq = 0;

		for ( f = 2000; f < 20000; f++ ) {
			double src = f * step, want = 0.5 * sin( 2.0 * M_PI * endHz * src / 22050.0 ), e = fabs( out[2 * f] - want );
			double inBlock = fmod( src - 200.0, 500.0 );	// the source frame's place in the block

			sq += e * e;
			if ( inBlock < 12.0 || inBlock > 488.0 ) {
				if ( e > maxWrap )
					maxWrap = e;
			} else if ( e > maxMid )
				maxMid = e;
		}
		first = sqrt( sq / 18000 );
		check( db( first / 0.5 ) < -80.0 && db( maxWrap / 0.5 ) < -66.0 && maxWrap < 2.0 * maxMid,
			"step 0.459 across 40 wraps: a 441 Hz sine %.1f dB rms from the ideal; largest error %.1f dB at the wraps, %.1f dB between",
			db( first / 0.5 ), db( maxWrap / 0.5 ), db( maxMid / 0.5 ) );
	}
	MacSnd_Shutdown();
}

/*
==============================================================================
offset: start offsets
==============================================================================
*/

static void test_offset( void )
{
	static float out[2 * 4096];
	static int idx[4096];
	macSndStart_t s;
	int snd, f, ok;
	float maxStep, prev;

	printf( "offset: start offsets\n" );
	offline();
	MacSnd_SetStopFade( 0 );
	snd = sound_ramp( 1000, 48000 );
	s = play( snd, 1.0f );
	s.startFrame = 333;
	MacSnd_Start( 0, &s );
	check( MacSnd_Position( 0 ) == 333 && MacSnd_Status( 0 ) == MACSND_PLAYING, "before the IO thread took it: position 333, playing" );
	render( out, 1000 );
	for ( f = 0; f < 1000; f++ ) idx[f] = 333 + f < 1000 ? 333 + f : -1;
	ok = expect_frames( out, 1000, idx, "start 333" );
	check( ok && MacSnd_Status( 0 ) == MACSND_STOPPED, "start 333 (no fade): the first frame is sample 333, 667 frames" );

	s.startFrame = 1000;
	MacSnd_Start( 0, &s );
	render( out, 64 );
	check( MacSnd_Status( 0 ) == MACSND_STOPPED && out[0] == 0.0f, "start at the end: stopped after one callback, silent" );
	MacSnd_Shutdown();

	// with the fade (1.5 ms = 72 frames) an offset start fades in, linearly
	offline();
	snd = sound_dc( 4000, 48000, 16384 );
	s = play( snd, 1.0f );
	s.startFrame = 100;
	MacSnd_Start( 0, &s );
	render( out, 256 );
	maxStep = 0; prev = 0;
	ok = 1;
	for ( f = 0; f < 256; f++ ) {
		float want = 0.5f * ( f < 72 ? f / 72.0f : 1.0f );
		if ( fabsf( out[2 * f] - want ) > 1e-5f )
			ok = 0;
		if ( fabsf( out[2 * f] - prev ) > maxStep )
			maxStep = fabsf( out[2 * f] - prev );
		prev = out[2 * f];
	}
	check( ok, "start 100 with the fade: 0 -> 0.5 over 72 frames, largest step %.5f (%.5f a frame)", maxStep, 0.5f / 72 );
	MacSnd_SetParams( 0, 0.5f, 0.5f, 0 );
	render( out, 128 );
	s.startFrame = 0;
	s.gainL = s.gainR = 0.5f;
	MacSnd_Start( 1, &s );
	MacSnd_Stop( 0 );
	render( out, 256 );
	check( out[0] == 0.5f, "a start at 0 doesn't fade in (the attack stays): first frame %.4f (0.25 new + 0.25 fading)", out[0] );
	MacSnd_Shutdown();
}

/*
==============================================================================
pause
==============================================================================
*/

static void test_pause( void )
{
	static float out[2 * 4096];
	static int idx[4096];
	macSndStart_t s;
	int snd, f, ok, silent;
	float maxStep, prev;

	printf( "pause\n" );
	offline();
	MacSnd_SetStopFade( 0 );
	snd = sound_ramp( 4000, 48000 );
	s = play( snd, 1.0f );
	MacSnd_Start( 0, &s );
	render( out, 640 );
	MacSnd_Pause( 0, 1 );
	check( MacSnd_Status( 0 ) == MACSND_PAUSED, "paused at once for the game (before the IO thread took it)" );
	render( out, 640 );
	silent = 1;
	for ( f = 0; f < 640; f++ )
		silent &= out[2 * f] == 0.0f;
	check( silent && MacSnd_Position( 0 ) == 640 && MacSnd_Status( 0 ) == MACSND_PAUSED, "no fade: silent at once, held at 640" );
	MacSnd_Pause( 0, 0 );
	render( out, 640 );
	for ( f = 0; f < 640; f++ ) idx[f] = 640 + f;
	ok = expect_frames( out, 640, idx, "resume" );
	check( ok, "resumed: the next frame is sample 640, to the sample" );

	// started paused: nothing until resumed, from its offset
	s.startFrame = 50; s.paused = 1;
	MacSnd_Start( 1, &s );
	MacSnd_Stop( 0 );
	render( out, 256 );
	silent = 1;
	for ( f = 0; f < 256; f++ )
		silent &= out[2 * f] == 0.0f;
	check( silent && MacSnd_Status( 1 ) == MACSND_PAUSED && MacSnd_Position( 1 ) == 50, "started paused: silent, held at 50" );
	MacSnd_Pause( 1, 0 );
	render( out, 64 );
	for ( f = 0; f < 64; f++ ) idx[f] = 50 + f;
	check( expect_frames( out, 64, idx, "started paused" ), "resumed from 50" );
	MacSnd_Shutdown();

	// with the fade: out and back in without a step, and the position held exactly
	offline();
	snd = sound_dc( 8000, 48000, 16384 );
	s = play( snd, 1.0f );
	MacSnd_Start( 0, &s );
	render( out, 640 );
	MacSnd_Pause( 0, 1 );
	render( out, 640 );
	maxStep = 0; prev = 0.5f;
	for ( f = 0; f < 640; f++ ) {
		if ( fabsf( out[2 * f] - prev ) > maxStep )
			maxStep = fabsf( out[2 * f] - prev );
		prev = out[2 * f];
	}
	ok = out[2 * 71] > 0.0f && out[2 * 72] == 0.0f && MacSnd_Position( 0 ) == 640;
	MacSnd_Pause( 0, 0 );
	render( out, 640 );
	for ( f = 0; f < 640; f++ ) {
		if ( fabsf( out[2 * f] - prev ) > maxStep )
			maxStep = fabsf( out[2 * f] - prev );
		prev = out[2 * f];
	}
	check( ok && out[2 * 72] == 0.5f && maxStep <= 0.5f / 72 + 1e-5f,
		"with the fade: out over 72 frames, held at 640, back in over 72; largest step %.5f", maxStep );
	MacSnd_Shutdown();
}

/*
==============================================================================
ramp: gains ramped across a callback, stops faded
==============================================================================
*/

static float largest_step( const float *out, int channel, int n, float prev )
{
	float m = 0;
	int f;

	for ( f = 0; f < n; f++ ) {
		if ( fabsf( out[2 * f + channel] - prev ) > m )
			m = fabsf( out[2 * f + channel] - prev );
		prev = out[2 * f + channel];
	}
	return m;
}

static void test_ramp( void )
{
	static float out[2 * 4096];
	macSndStart_t s;
	int snd, silent, f;
	float step, stepR;

	printf( "ramp: no clicks\n" );
	offline();
	snd = sound_dc( 48000, 48000, 16384 );
	s = play( snd, 1.0f );
	MacSnd_Start( 0, &s );
	render( out, 256 );
	MacSnd_SetParams( 0, 0.2f, 0.7f, 0 );
	render( out, 256 );
	step = largest_step( out, 0, 256, 0.5f );
	stepR = largest_step( out, 1, 256, 0.5f );
	check( fabsf( out[2 * 64] - 0.1f ) < 1e-6f && fabsf( out[2 * 64 + 1] - 0.35f ) < 1e-6f && step <= 0.4f / 64 + 1e-6f
		&& stepR <= 0.15f / 64 + 1e-6f, "gains 1/1 -> 0.2/0.7 across one callback: largest step %.5f / %.5f (%.5f / %.5f a frame)",
		step, stepR, 0.4f / 64, 0.15f / 64 );

	MacSnd_SetParams( 0, 1.0f, 1.0f, 0 );
	render( out, 64 );
	MacSnd_Stop( 0 );
	check( MacSnd_Status( 0 ) == MACSND_STOPPED, "stopped at once for the game" );
	render( out, 256 );
	step = largest_step( out, 0, 256, 0.5f );
	silent = 1;
	for ( f = 72; f < 256; f++ )
		silent &= out[2 * f] == 0.0f;
	check( step <= 0.5f / 72 + 1e-5f && silent, "stop: 0.5 -> 0 over 72 frames (1.5 ms), largest step %.5f", step );

	// a voice restarted while it plays (a gun's next shot on the same channel): the old one fades under the new
	MacSnd_Start( 0, &s );
	render( out, 256 );
	s.sound = sound_dc( 1000, 48000, 0 );
	MacSnd_Start( 0, &s );
	render( out, 256 );
	step = largest_step( out, 0, 256, 0.5f );
	check( step <= 0.5f / 72 + 1e-5f && out[2 * 72] == 0.0f, "restart on a playing voice: the old sound fades over 72 frames, step %.5f",
		step );

	MacSnd_SetStopFade( 0 );
	s.sound = snd;
	MacSnd_Start( 0, &s );
	render( out, 64 );
	MacSnd_Stop( 0 );
	render( out, 64 );
	printf( "       with the stop fade off (Miles' cut): a step of %.3f at the stop\n", fabsf( out[0] - 0.5f ) );
	MacSnd_Shutdown();
}

/*
==============================================================================
limiter: the ceiling, the recovery, the clip
==============================================================================
*/

static void test_limiter( void )
{
	static float out[2 * 48000];
	macSndStart_t s;
	int snd, quietSnd, i, f, clipped;
	float peak, level10, level300;
	short sq[4800];

	printf( "limiter\n" );
	offline();
	for ( f = 0; f < 4800; f++ )
		sq[f] = ( f / 24 ) & 1 ? 30000 : -30000;	// a 1 kHz square wave, -0.8 dBFS
	snd = MacSnd_CreateSound( sq, 4800, 1, 48000 );
	quietSnd = sound_dc( 48000, 48000, 16384 );
	for ( i = 0; i < 8; i++ ) {
		s = play( snd, 1.0f );
		s.loopCount = 0;
		MacSnd_Start( i, &s );
	}
	render( out, 24000 );
	peak = 0; clipped = 0;
	for ( f = 0; f < 2 * 24000; f++ ) {
		if ( fabsf( out[f] ) > peak )
			peak = fabsf( out[f] );
		clipped += fabsf( out[f] ) >= 1.0f;
	}
	check( peak <= CEILING + 1e-6f && peak > CEILING - 0.01f && !clipped,
		"8 square waves at once (+17.3 dB over full scale): peak %.6f (ceiling %.6f), %d frames at 1.0", peak, CEILING, clipped );

	for ( i = 0; i < 8; i++ )
		MacSnd_Stop( i );
	s = play( quietSnd, 1.0f );
	s.loopCount = 0;
	MacSnd_Start( 0, &s );
	render( out, 48000 );
	level10 = out[2 * 480];
	level300 = out[2 * 14400];
	check( level10 < 0.5f * 0.9f && level300 > 0.5f * 0.99f && out[2 * 47999] == 0.5f,
		"recovery after it: 10 ms %.4f, 300 ms %.4f, 1 s %.4f (0.5 is unlimited)", level10, level300, out[2 * 47999] );

	MacSnd_SetMaster( 2.0f );
	render( out, 4800 );
	peak = 0;
	for ( f = 0; f < 2 * 4800; f++ )
		if ( fabsf( out[f] ) > peak )
			peak = fabsf( out[f] );
	check( peak <= CEILING + 1e-6f && peak > CEILING - 0.001f, "master 2 on a 0.5 sound: peak %.6f", peak );
	MacSnd_SetMaster( 1.0f );

	MacSnd_SetParams( 0, NAN, INFINITY, NAN );
	render( out, 4800 );
	check( out[2 * 4799] == 0.0f && out[2 * 4799 + 1] == 0.0f && MacSnd_Status( 0 ) == MACSND_PLAYING,
		"NaN and infinite gains are taken as 0: silent, nothing broken" );
	MacSnd_Shutdown();
}

/*
==============================================================================
free: a sound freed while voices play it
==============================================================================
*/

static void test_free( void )
{
	static float out[2 * 8192];
	macSndStats_t st;
	macSndStart_t s;
	int snd, other, f, silent, before, after1, after2;

	printf( "free: a sound freed while voices play it (built with AddressSanitizer: a late read would stop here)\n" );
	offline();
	snd = sound_dc( 10000, 48000, 8000 );
	other = sound_dc( 100, 48000, 0 );
	s = play( snd, 1.0f );
	MacSnd_Start( 0, &s );
	s.loopCount = 0;
	MacSnd_Start( 1, &s );
	s.paused = 1;
	MacSnd_Start( 2, &s );
	render( out, 640 );
	MacSnd_FreeSound( snd );
	MacSnd_GetStats( &st );
	before = st.sounds;
	s.paused = 0;
	MacSnd_Start( 3, &s );
	check( MacSnd_Status( 3 ) == MACSND_STOPPED && MacSnd_SoundData( snd ) == NULL,
		"a start on a freed sound is refused, its handle is dead" );
	MacSnd_Update();
	MacSnd_GetStats( &st );
	check( before == 2 && st.sounds == 2, "kept until the IO thread lets go (2 sounds after the free and an update)" );
	render( out, 64 );
	MacSnd_Update();
	MacSnd_GetStats( &st );
	after1 = st.sounds;
	render( out + 128, 64 );
	MacSnd_Update();
	MacSnd_GetStats( &st );
	after2 = st.sounds;
	check( after1 == 2 && after2 == 1 && MacSnd_Status( 0 ) == MACSND_STOPPED && MacSnd_Status( 1 ) == MACSND_STOPPED
		&& MacSnd_Status( 2 ) == MACSND_STOPPED, "its voices stop; the memory goes once their fades are done (after 2 callbacks)" );
	render( out, 8192 );
	silent = 1;
	for ( f = 0; f < 8192; f++ )
		silent &= out[2 * f] == 0.0f;
	check( silent, "then silence" );

	// no voice on it, or the fade off: one callback
	MacSnd_FreeSound( other );
	render( out, 64 );
	MacSnd_Update();
	MacSnd_GetStats( &st );
	check( st.sounds == 0 && st.soundBytes == 0, "a sound nothing plays: freed after one callback, 0 bytes left" );
	MacSnd_SetStopFade( 0 );
	snd = sound_dc( 10000, 48000, 8000 );
	check( snd != other && ( snd & 0xffff ) == ( other & 0xffff ) && !MacSnd_SoundData( other ),
		"a new sound in the freed slot gets a new handle, the old one stays dead" );
	s = play( snd, 1.0f );
	MacSnd_Start( 0, &s );
	render( out, 64 );
	MacSnd_FreeSound( snd );
	render( out, 64 );
	MacSnd_Update();
	MacSnd_GetStats( &st );
	check( st.sounds == 0 && out[0] == 0.0f, "with the stop fade off: cut and freed in the same callback" );
	MacSnd_Shutdown();
}

/*
==============================================================================
misc
==============================================================================
*/

static void test_misc( void )
{
	static float out[2 * 8192];
	static short st[2 * 1000];
	static int idx[8192];
	macSndStats_t stats;
	macSndStart_t s;
	int snd, f, ok, i;
	float maxDiff;

	printf( "misc\n" );
	offline();
	MacSnd_SetStopFade( 0 );

	// a silent voice costs nothing but keeps time with a mixed one
	snd = sound_ramp( 10000, 22050 );
	s = play( snd, 1.0f );
	s.loopStart = 300; s.loopEnd = 4000; s.loopCount = 3;
	MacSnd_Start( 0, &s );
	s.gainL = s.gainR = 0;
	MacSnd_Start( 1, &s );
	ok = 1;
	for ( f = 0; f < 600 && ok; f++ ) {
		render( out, 64 );
		ok = MacSnd_Position( 0 ) == MacSnd_Position( 1 ) && MacSnd_Status( 0 ) == MacSnd_Status( 1 );
	}
	check( ok && MacSnd_Status( 0 ) == MACSND_STOPPED, "a voice at gain 0 only moves on, frame for frame with a mixed one" );

	MacSnd_Shutdown();

	// stereo, both channels to the sample at step 1
	offline();
	MacSnd_SetStopFade( 0 );
	for ( f = 0; f < 1000; f++ ) {
		st[2 * f] = (short)( ( f + 1 ) * 16 );
		st[2 * f + 1] = (short)( -( f + 1 ) * 16 );
	}
	snd = MacSnd_CreateSound( st, 1000, 2, 48000 );
	s = play( snd, 1.0f );
	MacSnd_Start( 2, &s );
	render( out, 1000 );
	ok = 1;
	for ( f = 0; f < 1000; f++ )
		ok &= out[2 * f] == ramp_value( f ) && out[2 * f + 1] == -ramp_value( f );
	check( ok, "a stereo sound at the device rate: both channels bit for bit" );
	MacSnd_Shutdown();

	// a stereo sine at 22050: left and right mirror each other exactly
	offline();
	snd = sound_sine( 22050, 22050, 1000.0, 0.5, 2 );
	s = play( snd, 1.0f );
	MacSnd_Start( 0, &s );
	render( out, 8192 );
	maxDiff = 0;
	for ( f = 0; f < 8192; f++ )
		if ( fabsf( out[2 * f] + out[2 * f + 1] ) > maxDiff )
			maxDiff = fabsf( out[2 * f] + out[2 * f + 1] );
	check( maxDiff < 1e-6f, "a stereo sine at step 0.46: left = -right within %.2g", maxDiff );

	// a stereo frame through the one-frame path (a run's odd last frame, a loop's wrap) at its level (9d found it
	// doubled since 9b): callbacks of 37 frames and a looped block give what 64-frame callbacks of the sound do
	{
		static float ref[2 * 8192];
		int n;

		memcpy( ref, out, sizeof( ref ) );
		MacSnd_SetStopFade( 0 );	// the restart without the old sound's tail
		MacSnd_Start( 0, &s );
		for ( f = 0; f < 8192; f += n ) {
			n = 8192 - f < 37 ? 8192 - f : 37;
			MacSnd_Render( out + 2 * f, n );
		}
		for ( f = 0, maxDiff = 0; f < 2 * 8192; f++ )
			maxDiff = fmaxf( maxDiff, fabsf( out[f] - ref[f] ) );
		check( maxDiff == 0, "a stereo sine at step 0.46 in 37-frame callbacks: the 64-frame output within %.2g",
			maxDiff );
		s.loopStart = 1000; s.loopEnd = 1441; s.loopCount = 0;	// 20 whole cycles
		MacSnd_Start( 0, &s );
		render( out, 8192 );
		for ( f = 0, maxDiff = 0; f < 8192; f++ )
			maxDiff = fmaxf( maxDiff, fabsf( out[2 * f] ) );
		check( maxDiff < 0.501f, "the same sine looped over 20 whole cycles (5 wraps): its peak %.4f, no doubled "
			"frame at the wraps (0.5)", maxDiff );
	}
	MacSnd_Shutdown();

	// the linear A/B at step 1 is bit for bit too; a rate twice the sound's moves twice as fast
	offline();
	MacSnd_SetResampler( MACSND_LINEAR );
	snd = sound_ramp( 4000, 48000 );
	s = play( snd, 1.0f );
	MacSnd_Start( 0, &s );
	render( out, 256 );
	for ( f = 0; f < 256; f++ ) idx[f] = f;
	ok = expect_frames( out, 256, idx, "linear" );
	MacSnd_SetParams( 0, 1, 1, 96000 );
	render( out, 256 );
	check( ok && MacSnd_Position( 0 ) == 256 + 512, "linear at step 1: bit for bit; at rate 96000 twice as fast (%d)",
		MacSnd_Position( 0 ) );
	MacSnd_Shutdown();

	// a full ring: a start it loses stops the voice instead (held back, posted first next time), nothing else breaks
	offline();
	snd = sound_dc( 1000, 48000, 100 );
	s = play( snd, 0.1f );
	s.loopCount = 0;
	for ( i = 0; i < 1100; i++ )
		MacSnd_Start( i & 63, &s );
	MacSnd_GetStats( &stats );
	ok = stats.ringDrops > 0;
	for ( i = 0; i < 64; i++ )
		ok &= MacSnd_Status( i ) == MACSND_STOPPED;	// every voice's last start came after the ring was full
	render( out, 64 );
	MacSnd_Update();
	render( out, 256 );
	MacSnd_GetStats( &stats );
	check( ok && stats.voices == 0, "1100 starts before a callback (the ring holds 1024): the lost ones stop their voices, "
		"%d voices left playing", stats.voices );
	MacSnd_Shutdown();
}

/*
==============================================================================
stress: a second thread renders while this one posts at random
==============================================================================
*/

// the capture of the final mix (the 9c WAV files): what went out, then silence for the device when muted
static void test_capture( void )
{
	static float out[2 * 4096], got[2 * 8192];
	macSndStats_t stats;
	int snd, n, i, ok;

	printf( "capture\n" );
	offline();
	snd = sound_sine( 48000, 48000, 1000, 0.5, 1 );
	MacSnd_Start( 0, &( macSndStart_t ){ .sound = snd, .gainL = 1, .gainR = 0.5f } );
	render( out, 256 );
	check( MacSnd_Capture( 1000, 0 ) && MacSnd_ReadCapture( got, 8192 ) == 0, "nothing captured before it starts" );
	render( out, 512 );
	n = MacSnd_ReadCapture( got, 8192 );
	for ( ok = n == 512, i = 0; i < 2 * 512 && ok; i++ )
		ok = got[i] == out[i];
	check( ok, "512 frames out, the same 512 captured" );
	render( out, 600 );
	render( out + 2 * 600, 600 );
	MacSnd_GetStats( &stats );
	n = MacSnd_ReadCapture( got, 8192 );
	check( n <= 1000 && n > 900 && n + stats.captureDrops == 1200, "a full ring (1000 frames): the callbacks that "
		"didn't fit are lost and counted (%d read, %u dropped)", n, stats.captureDrops );
	MacSnd_Capture( 1000, 1 );
	render( out, 300 );
	n = MacSnd_ReadCapture( got, 8192 );
	for ( ok = n == 300, i = 0; i < 2 * 300 && ok; i++ )
		ok = out[i] == 0;
	check( ok && got[2 * 100] != 0, "muted: the device gets silence, the capture the mix" );
	MacSnd_Capture( 0, 0 );
	render( out, 300 );
	check( MacSnd_ReadCapture( got, 8192 ) == 0 && out[2 * 100] != 0, "stopped: nothing captured, the device hears it "
		"again" );
	MacSnd_Shutdown();
}

/*
==============================================================================
streams (Phase 9d): a ring written ahead of its voice
==============================================================================
*/

static short stream_value( long k ) { return (short)( ( k * 13 + 1 ) % 30011 ); }	// under the limiter's ceiling

// commits up to want frames of stream_value from *next, in pieces of at most piece frames; returns the frames written
static int stream_write( int snd, long *next, int want, int piece )
{
	int done = 0, space, n, i;
	short *p;

	while ( done < want && ( p = MacSnd_StreamBuffer( snd, &space ) ) ) {
		n = space < piece ? space : piece;
		if ( n > want - done )
			n = want - done;
		for ( i = 0; i < n; i++ )
			p[i] = stream_value( *next + i );
		MacSnd_StreamCommit( snd, n );
		*next += n;
		done += n;
	}
	return done;
}

static void test_stream( void )
{
	enum { RING = 4096, TOTAL = 30000 };
	static float out[2 * 40000], ref[2 * 40000];
	macSndStats_t st;
	long next = 0, f;
	int snd, snd2, ok, bad, status, i, piece;
	unsigned long long played;
	float diff;

	printf( "stream: a ring written ahead of its voice\n" );
	offline();
	MacSnd_SetStopFade( 0 );
	check( !MacSnd_CreateStream( 96, 1, 48000 ) && MacSnd_CreateStream( 97, 1, 48000 ), "a ring needs more than "
		"4 x 24 frames (the voice's window behind it, the end's silence ahead)" );

	// written in uneven pieces while it plays: every frame once, in order, across 7 wraps; stopped with the last
	snd = MacSnd_CreateStream( RING, 1, 48000 );
	stream_write( snd, &next, 2000, 2000 );
	MacSnd_Start( 0, &( macSndStart_t ){ .sound = snd, .gainL = 1, .gainR = 1, .startFrame = 500, .loopCount = 3 } );
	for ( f = 0, piece = 1, status = -1; f < TOTAL + 64; f += BLOCK, piece++ ) {
		stream_write( snd, &next, TOTAL - (int)next, 37 + piece * 101 % 777 );
		if ( next == TOTAL )
			MacSnd_StreamEnd( snd );
		render( out + 2 * f, BLOCK );
		if ( f + BLOCK == TOTAL - TOTAL % BLOCK )
			status = MacSnd_Status( 0 );	// before the callback that plays the last frame
	}
	for ( f = 0, bad = -1; f < TOTAL + 64 && bad < 0; f++ ) {
		const float want = f < TOTAL ? stream_value( f ) / 32768.0f : 0.0f;
		if ( out[2 * f] != want || out[2 * f + 1] != want )
			bad = (int)f;
	}
	MacSnd_GetStats( &st );
	check( bad < 0 && status == MACSND_PLAYING && MacSnd_Status( 0 ) == MACSND_STOPPED
		&& MacSnd_StreamPlayed( snd ) == TOTAL && st.underruns == 0, "30000 frames through a 4096-frame ring in "
		"pieces of 37-814: each frame once and in order (first wrong frame %d; its offset and loops ignored), playing "
		"before its last callback, stopped in it; played %llu; %u underruns", bad, MacSnd_StreamPlayed( snd ),
		st.underruns );
	MacSnd_FreeSound( snd );
	MacSnd_Update();

	// short of frames: it holds (silent) and goes on with the next frame, none lost or repeated
	snd = MacSnd_CreateStream( RING, 1, 48000 );
	next = 0;
	stream_write( snd, &next, 1000, 1000 );
	MacSnd_Start( 0, &( macSndStart_t ){ .sound = snd, .gainL = 1, .gainR = 1 } );
	render( out, 3008 );
	MacSnd_GetStats( &st );
	status = MacSnd_Status( 0 );
	played = MacSnd_StreamPlayed( snd );
	for ( f = 0, ok = 1; f < 3008; f++ )
		ok &= out[2 * f] == ( f < 1000 - 24 ? stream_value( f ) / 32768.0f : 0.0f );
	check( ok && status == MACSND_PLAYING && played == 1000 - 24 && st.underruns == 32, "1000 frames committed: the "
		"first 976 play (the last 24 are its window's), then silence; still playing at %llu, %u underrun callbacks",
		played, st.underruns );
	stream_write( snd, &next, 2000, 2000 );
	MacSnd_StreamEnd( snd );
	render( out, 2048 );
	for ( f = 0, ok = 1; f < 2048; f++ )
		ok &= out[2 * f] == ( f < 3000 - 976 ? stream_value( 976 + f ) / 32768.0f : 0.0f );
	check( ok && MacSnd_Status( 0 ) == MACSND_STOPPED, "the rest committed and the end: it goes on at frame 976 to "
		"the last (2999), then stops" );

	// a stopped voice: the stream resumes where it stopped, on another voice (with the fade: in over 72 frames)
	snd2 = MacSnd_CreateStream( RING, 1, 48000 );
	next = 0;
	stream_write( snd2, &next, RING, RING );
	MacSnd_Start( 1, &( macSndStart_t ){ .sound = snd2, .gainL = 1, .gainR = 1 } );
	render( out, 1280 );
	MacSnd_Stop( 1 );
	render( out, 64 );
	played = MacSnd_StreamPlayed( snd2 );
	stream_write( snd2, &next, RING, RING );
	MacSnd_Start( 2, &( macSndStart_t ){ .sound = snd2, .gainL = 1, .gainR = 1 } );
	render( out, 512 );
	for ( f = 0, ok = 1; f < 512; f++ )
		ok &= out[2 * f] == stream_value( 1280 + f ) / 32768.0f;
	check( ok && played == 1280 && next == 1280 + RING - 48, "stopped at %llu: the ring filled up to the voice's "
		"window and the end's silence (%ld written), a new voice goes on at frame 1280", played, next );
	MacSnd_SetStopFade( 1.5f );
	MacSnd_Stop( 2 );
	render( out, 128 );
	MacSnd_Start( 3, &( macSndStart_t ){ .sound = snd2, .gainL = 1, .gainR = 1 } );
	render( out, 128 );
	for ( f = 0, ok = 1; f < 128; f++ )
		ok &= fabsf( out[2 * f] - stream_value( 1792 + f ) / 32768.0f * ( f < 72 ? f / 72.0f : 1.0f ) ) < 1e-5f;
	check( ok, "with the stop fade a resumed stream fades in over 72 frames" );
	MacSnd_Shutdown();

	// resampled (44.1 kHz, step 0.92, stereo): the same output as the whole sound played from memory, wraps and all
	offline();
	snd = MacSnd_CreateStream( RING, 2, 44100 );
	snd2 = MacSnd_CreateSound( NULL, 20000, 2, 44100 );
	{
		short *pcm = MacSnd_SoundData( snd2 ), *p;
		int space;

		for ( i = 0; i < 20000; i++ ) {
			pcm[2 * i] = (short)lrint( 16000.0 * sin( 2.0 * M_PI * 3000.0 * i / 44100 ) );
			pcm[2 * i + 1] = (short)lrint( 12000.0 * sin( 2.0 * M_PI * 700.0 * i / 44100 ) );
		}
		MacSnd_Start( 0, &( macSndStart_t ){ .sound = snd2, .gainL = 1, .gainR = 1, .loopCount = 1 } );
		render( ref, 22080 );
		MacSnd_Stop( 0 );
		next = 0;
		for ( f = 0; f < 22080; f += BLOCK ) {
			while ( next < 20000 && ( p = MacSnd_StreamBuffer( snd, &space ) ) ) {
				if ( space > 20000 - next )
					space = 20000 - (int)next;
				memcpy( p, pcm + 2 * next, sizeof( short ) * 2 * space );
				MacSnd_StreamCommit( snd, space );
				next += space;
			}
			if ( next == 20000 )
				MacSnd_StreamEnd( snd );
			if ( !f )
				MacSnd_Start( 1, &( macSndStart_t ){ .sound = snd, .gainL = 1, .gainR = 1 } );
			render( out + 2 * f, BLOCK );
		}
	}
	for ( f = 0, diff = 0, bad = -1; f < 2 * 22080; f++ ) {
		if ( bad < 0 && fabsf( out[f] - ref[f] ) > 2e-7f )
			bad = (int)f;
		diff = fmaxf( diff, fabsf( out[f] - ref[f] ) );
	}
	MacSnd_GetStats( &st );
	if ( bad >= 0 )
		printf( "  first difference at sample %d (frame %d): %.6f, from memory %.6f; %u underruns\n", bad, bad / 2,
			out[bad], ref[bad], st.underruns );
	check( diff < 2e-7f && MacSnd_Status( 1 ) == MACSND_STOPPED, "44.1 kHz stereo through the ring (4 wraps): "
		"the sound from memory's output within %.1e (the gathered windows at the wraps), stopped at its end", diff );
	MacSnd_Shutdown();
}

/*
The native driver's math (code/client/snd_native_math.h) against the plan's numbers (Phase 9, the parity checklist):
the original's Fast 2D model, its pan laws, the volume scales and s_volume's law. OpenAL's levels for the same cases
(openal-soft 1.25.2's stereo decoder, alc/panning.cpp: 0.5 +- 0.5 x left + 0.0957 x front a side) are printed beside
them: the expected differences of the 9c comparison.
*/
static int near( double a, double b, double eps ) { return fabs( a - b ) <= eps; }

static void openal_pan( double front, double left, double *l, double *r )
{
	*l = 0.5 + 0.288675135 * sqrt( 3.0 ) * left + 0.0552305643 * sqrt( 3.0 ) * front;
	*r = 0.5 - 0.288675135 * sqrt( 3.0 ) * left + 0.0552305643 * sqrt( 3.0 ) * front;
}

static void test_math( void )
{
	float l, r, l3, r3;
	double ol, or_;
	int p, ok;

	printf( "math\n" );

	// A2: the content volume up to Miles' ceiling (127/84), negative = 1
	check( SNM_Volume( 1.5f ) == 1.5f && SNM_Volume( 0.25f ) == 0.25f && SNM_Volume( -1 ) == 1.0f
		&& near( SNM_Volume( 2.0f ), 127.0 / 84.0, 1e-6 ), "A2: volume 1.5 and 0.25 kept, 2.0 -> %.4f (127/84), -1 -> 1",
		SNM_Volume( 2.0f ) );

	// A4: s_volume^3 x fade on the master (OpenAL: listener s^2 x each voice's s)
	check( near( SNM_Master( 0.9f, 1 ), 0.729, 1e-6 ) && near( SNM_Master( 0.4f, 1 ), 0.064, 1e-7 )
		&& near( SNM_Master( 1, 0.5f ), 0.5, 1e-7 ) && SNM_Master( 1.2f, 1 ) == 1.0f && SNM_Master( 0, 1 ) == 0
		&& SNM_Master( 0.9f, -1 ) == 0, "A4: master 0.9 -> 0.729, 0.4 -> 0.064 (the user's: %.1f dB), fade 0.5 -> 0.5, "
		"clamped to 0..1", 20 * log10( 0.064 ) );

	// B1: min / d, silent beyond max, the last 5%% a fade (160/1600: footsteps; 1000/8000: weapons)
	check( SNM_Distance( 80, 160, 1600 ) == 1 && SNM_Distance( 160, 160, 1600 ) == 1
		&& near( SNM_Distance( 320, 160, 1600 ), 0.5, 1e-7 ) && near( SNM_Distance( 1520, 160, 1600 ), 160.0 / 1520, 1e-6 )
		&& near( SNM_Distance( 1560, 160, 1600 ), 160.0 / 1560 * 0.5, 1e-6 ) && SNM_Distance( 1600, 160, 1600 ) == 0
		&& SNM_Distance( 2000, 160, 1600 ) == 0, "B1: 160/1600 at 80, 160, 320, 1520, 1560, 1600, 2000: 1, 1, 0.5, "
		"%.4f (%.1f dB), %.4f, 0, 0 (OpenAL held %.1f dB beyond)", SNM_Distance( 1520, 160, 1600 ),
		db( SNM_Distance( 1520, 160, 1600 ) ), SNM_Distance( 1560, 160, 1600 ), db( 0.1 ) );
	check( near( SNM_Distance( 4000, 1000, 8000 ), 0.25, 1e-7 ) && near( SNM_Distance( 7600, 1000, 8000 ), 1000.0 / 7600, 1e-6 )
		&& near( SNM_Distance( 7800, 1000, 8000 ), 1000.0 / 7800 * 0.5, 1e-6 ), "B1: 1000/8000 at 4000, 7600, 7800: "
		"-12.04 dB, %.2f dB, %.2f dB", db( SNM_Distance( 7600, 1000, 8000 ) ), db( SNM_Distance( 7800, 1000, 8000 ) ) );
	for ( ok = 1, p = 0; p <= 1700; p++ ) {	// never rising with the distance
		ok &= SNM_Distance( (float)p + 1, 160, 1600 ) <= SNM_Distance( (float)p, 160, 1600 );
	}
	check( ok, "B1: never rising with the distance, 0..1700" );

	// C1: Miles' 2D pan, GM2's power law: 64 the exact centre, 0 and 1 hard left, 127 hard right, power constant
	SNM_Pan2D( 64, &l, &r );
	check( near( l, M_SQRT1_2, 1e-6 ) && near( r, M_SQRT1_2, 1e-6 ), "C1: pan 64 -> %.5f / %.5f (-3.01 dB a side)", l, r );
	SNM_Pan2D( 0, &l, &r );
	ok = l == 1 && r == 0;
	SNM_Pan2D( 1, &l, &r );
	ok &= l == 1 && r == 0;
	SNM_Pan2D( 127, &l, &r );
	check( ok && fabs( l ) < 1e-7 && r == 1, "C1: pan 0 and 1 hard left, 127 hard right" );
	for ( ok = 1, p = 0; p <= 127; p++ ) {
		float pl, pr;

		SNM_Pan2D( p, &l, &r );
		SNM_Pan2D( p + 1, &pl, &pr );
		ok &= near( l * l + r * r, 1, 1e-6 ) && ( p >= 127 || ( pl <= l && pr >= r ) );
	}
	SNM_Pan2D( 96, &l, &r );
	openal_pan( -sqrt( 1 - ( 32 / 127.0 ) * ( 32 / 127.0 ) ), -32 / 127.0, &ol, &or_ );
	check( ok, "C1: power constant and monotonic over 0..127; pan 96 -> %.4f / %.4f (OpenAL: %.4f / %.4f, behind)",
		l, r, ol, or_ );

	// C1 for a stereo sound: the pan a balance, both channels at their level at the centre
	SNM_Balance2D( 64, &l, &r );
	ok = l == 1 && r == 1;
	SNM_Balance2D( 0, &l, &r );
	ok &= l == 1 && r == 0;
	SNM_Balance2D( 127, &l, &r );
	ok &= fabs( l ) < 1e-6 && r == 1;
	SNM_Balance2D( 96, &l, &r );
	check( ok && near( l, sqrt( 2.0 ) * cos( 95 / 126.0 * M_PI_2 ), 1e-6 ) && r == 1, "C1 stereo: a balance, 64 both "
		"at 1, 0 and 127 one side, 96 -> %.4f / %.4f", l, r );

	// C6: Fast 2D's 3D pan: front -3 dB a side, a side all on it, straight behind -3 dB more, nothing steps
	SNM_Pan3D( 1, 0, &l, &r );
	openal_pan( 1, 0, &ol, &or_ );
	check( near( l, M_SQRT1_2, 1e-6 ) && near( r, M_SQRT1_2, 1e-6 ), "C6: in front %.5f / %.5f (OpenAL %.4f, %.2f dB)",
		l, r, ol, db( ol ) );
	SNM_Pan3D( 0, 1, &l, &r );
	ok = l == 1 && fabs( r ) < 1e-7;
	SNM_Pan3D( 0, -1, &l, &r );
	check( ok && fabs( l ) < 1e-7 && r == 1, "C6: fully left 1 / 0, fully right 0 / 1" );
	SNM_Pan3D( -1, 0, &l, &r );
	openal_pan( -1, 0, &ol, &or_ );
	check( near( l, M_SQRT1_2 * SNM_BEHIND, 1e-6 ) && near( r, l, 1e-7 ), "C6: straight behind %.5f a side (%.2f dB; "
		"OpenAL %.2f dB)", l, db( l ), db( ol ) );
	SNM_Pan3D( -M_SQRT1_2, M_SQRT1_2, &l, &r );
	check( near( l, cos( ( 1 - M_SQRT1_2 ) * 0.5 * M_PI_2 ) * ( 1 - ( 1 - SNM_BEHIND ) * M_SQRT1_2 ), 1e-6 )
		&& near( r, sin( ( 1 - M_SQRT1_2 ) * 0.5 * M_PI_2 ) * ( 1 - ( 1 - SNM_BEHIND ) * M_SQRT1_2 ), 1e-6 ),
		"C6: behind left at 45 degrees %.4f / %.4f", l, r );
	for ( ok = 1, p = 0; p < 3600; p++ ) {	// around the listener: no step anywhere
		const double a = p * M_PI / 1800, b = ( p + 1 ) * M_PI / 1800;
		float bl, br;

		SNM_Pan3D( (float)cos( a ), (float)sin( a ), &l, &r );
		SNM_Pan3D( (float)cos( b ), (float)sin( b ), &bl, &br );
		ok &= fabs( bl - l ) < 0.002 && fabs( br - r ) < 0.002;
	}
	SNM_Pan3D( 0, 0, &l, &r );
	check( ok && near( l, M_SQRT1_2, 1e-6 ) && near( r, M_SQRT1_2, 1e-6 ), "C6: no step around the listener (0.1 "
		"degree steps); overhead or on the listener the centre" );

	// A3: 2D voices x 84/127 against 3D ones: at volume 1, the 2D centre against the 3D front
	SNM_Pan2D( 64, &l, &r );
	SNM_Pan3D( 1, 0, &l3, &r3 );
	check( near( db( l * SNM_SCALE_2D ) - db( l3 ), 20 * log10( 84.0 / 127 ), 1e-4 ), "A3: a 2D voice %.2f dB, a 3D one "
		"%.2f dB a side: %.2f dB apart (OpenAL's 2D at pan 64 sat behind: %.2f dB)", db( l * SNM_SCALE_2D ), db( l3 ),
		db( l * SNM_SCALE_2D ) - db( l3 ), db( 0.5 - 0.0552305643 * sqrt( 3.0 ) ) );

	// B4, B5: the decompiled curves
	check( SNM_Linear2D( 160, 160, 1600 ) == 1 && near( SNM_Linear2D( 880, 160, 1600 ), 0.5, 1e-6 )
		&& SNM_Linear2D( 1600, 160, 1600 ) == 0, "B4: 2D linear falloff 160/1600 at 160, 880, 1600: 1, 0.5, 0" );
	check( SNM_LoopLevel( 100, 160, 1600 ) == 1 && near( SNM_LoopLevel( 320, 160, 1600 ), 0.25, 1e-7 )
		&& SNM_LoopLevel( 1600, 160, 1600 ) == 0, "B5: loops' inverse square 160/1600 at 100, 320, 1600: 1, 0.25, 0" );
}

/*
IMA ADPCM, Microsoft's block layout (WAVE format 17): a reference encoder here, the driver's decoder there
*/
static const int ima_index[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
static const int ima_step[89] = { 7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50,
	55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494,
	544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
	3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
	20350, 22385, 24623, 27086, 29794, 32767 };

static int ima_code( int sample, int *pred, int *index, short *decoded )
{
	const int step = ima_step[*index];
	int diff = sample - *pred, code = 0, d;

	if ( diff < 0 ) {
		code = 8;
		diff = -diff;
	}
	if ( diff >= step ) { code |= 4; diff -= step; }
	if ( diff >= step >> 1 ) { code |= 2; diff -= step >> 1; }
	if ( diff >= step >> 2 ) { code |= 1; }
	d = step >> 3;
	if ( code & 1 ) d += step >> 2;
	if ( code & 2 ) d += step >> 1;
	if ( code & 4 ) d += step;
	*pred += code & 8 ? -d : d;
	*pred = *pred < -32768 ? -32768 : *pred > 32767 ? 32767 : *pred;
	*index += ima_index[code];
	*index = *index < 0 ? 0 : *index > 88 ? 88 : *index;
	*decoded = (short)*pred;
	return code;
}

// frames of channels into blocks of blockAlign bytes; ref gets what a decoder must give back
static int ima_encode( const short *in, int frames, int channels, int blockAlign, unsigned char *out, short *ref )
{
	const int per = ( blockAlign - 4 * channels ) / ( 4 * channels ) * 8 + 1;
	int pred[2] = { 0, 0 }, index[2] = { 0, 0 }, f = 0, bytes = 0, c, g, k;

	while ( f < frames ) {
		unsigned char *b = out + bytes;
		const int n = frames - f < per ? frames - f : per;

		memset( b, 0, blockAlign );
		for ( c = 0; c < channels; c++ ) {
			pred[c] = in[f * channels + c];
			b[4 * c] = (unsigned char)( pred[c] & 255 );
			b[4 * c + 1] = (unsigned char)( ( pred[c] >> 8 ) & 255 );
			b[4 * c + 2] = (unsigned char)index[c];
			ref[f * channels + c] = (short)pred[c];
		}
		for ( g = 0; 1 + g * 8 < n; g++ ) {
			for ( c = 0; c < channels; c++ ) {
				for ( k = 0; k < 8; k++ ) {
					const int fr = f + 1 + g * 8 + k;
					const int code = fr < frames ? ima_code( in[fr * channels + c], &pred[c], &index[c], &ref[fr * channels + c] ) : 0;

					b[4 * channels + ( g * channels + c ) * 4 + ( k >> 1 )] |= (unsigned char)( code << ( ( k & 1 ) * 4 ) );
				}
			}
		}
		// a cut last block: the header and the whole groups its frames need
		bytes += n == per ? blockAlign : 4 * channels + ( n - 1 + 7 ) / 8 * 4 * channels;
		f += n;
	}
	return bytes;
}

static void test_ima( void )
{
	enum { FRAMES = 5000, ALIGN = 1024 };
	static short in[2 * FRAMES], ref[2 * FRAMES + 16], dec[2 * FRAMES + 16];
	static unsigned char data[4 * FRAMES];
	const unsigned char hand[12] = { 0, 0, 0, 0, 0x07, 0x00, 0, 0, 0, 0, 0, 0 };
	double err, sig;
	int ch, i, bytes, frames, ok;

	printf( "ima\n" );

	// by hand: first sample 0, index 0; codes 7 then 0: 0 + (0 + 1 + 3 + 7) = 11, then step 16: 11 + 2 = 13 (16 codes)
	frames = SNM_DecodeIMA( hand, 12, 12, 1, dec, 100 );
	check( frames == 17 && dec[0] == 0 && dec[1] == 11 && dec[2] == 13, "a block by hand: %d frames, %d %d %d (0 11 13)",
		frames, dec[0], dec[1], dec[2] );

	for ( ch = 1; ch <= 2; ch++ ) {
		for ( i = 0; i < FRAMES * ch; i++ )
			in[i] = (short)( 12000 * sin( 2 * M_PI * 441.0 * ( i / ch ) / 44100 + ( i % ch ) ) );
		bytes = ima_encode( in, FRAMES, ch, ALIGN, data, ref );
		memset( dec, 0x55, sizeof( dec ) );
		frames = SNM_DecodeIMA( data, bytes, ALIGN, ch, dec, FRAMES );
		for ( ok = 1, i = 0; i < FRAMES * ch; i++ )
			ok &= dec[i] == ref[i];
		for ( err = sig = 0, i = 0; i < FRAMES * ch; i++ ) {
			err += ( dec[i] - in[i] ) * (double)( dec[i] - in[i] );
			sig += in[i] * (double)in[i];
		}
		{
			// without a fact chunk the cut block's frames count in whole groups of 8
			const int per = ( ALIGN - 4 * ch ) / ( 4 * ch ) * 8 + 1, rest = FRAMES % per;
			const int whole = FRAMES / per * per + ( rest ? 1 + ( rest - 1 + 7 ) / 8 * 8 : 0 );

			check( ok && frames == FRAMES && SNM_IMAFrames( bytes, ALIGN, ch ) == whole && dec[FRAMES * ch] == 0x5555,
				"%s: %d frames in %d bytes (blocks of %d frames and a cut one; %d without the fact chunk's count), the "
				"reference encoder's samples exactly, %.1f dB SNR against the input, nothing written past them",
				ch == 1 ? "mono" : "stereo", frames, bytes, per, whole, 10 * log10( sig / err ) );
		}
		memset( dec, 0x55, sizeof( dec ) );
		frames = SNM_DecodeIMA( data, bytes, ALIGN, ch, dec, 1234 );
		for ( ok = 1, i = 0; i < 1234 * ch; i++ )
			ok &= dec[i] == ref[i];
		check( ok && frames == 1234 && dec[1234 * ch] == 0x5555, "%s: stops at the fact chunk's count (1234)",
			ch == 1 ? "mono" : "stereo" );
	}
	check( SNM_DecodeIMA( data, 3, ALIGN, 1, dec, 10 ) == 0 && SNM_IMAFrames( 3, ALIGN, 1 ) == 0
		&& SNM_DecodeIMA( data, 100, 2, 1, dec, 10 ) == 0, "a block shorter than its header, a block size too small: "
		"nothing" );
}

static _Atomic int stress_run;

static void *stress_io( void *arg )
{
	static float out[2 * 512];
	unsigned r = 1;

	(void)arg;
	while ( atomic_load( &stress_run ) ) {
		r = r * 1103515245 + 12345;
		MacSnd_Render( out, 16 + ( r >> 16 ) % 497 );
		if ( ( r >> 8 ) % 4 == 0 )
			usleep( 200 );
	}
	return NULL;
}

static void test_stress( double seconds )
{
	enum { SOUNDS = 48, VOICES = 64 };
	int sounds[SOUNDS] = { 0 }, frames[SOUNDS] = { 0 }, voiceFrames[VOICES] = { 0 };
	macSndStats_t st;
	macSndStart_t s;
	pthread_t io;
	uint64_t end;
	long ops = 0, bad = 0;
	int i, v, k, status, pos, stream = 0, streamChannels = 1, space, n;
	short pcm[2 * 4096], *p;

	printf( "stress: %.0f s of random commands against a rendering thread\n", seconds );
	offline();
	for ( i = 0; i < 2 * 4096; i++ )
		pcm[i] = (short)( ( i * 7919 ) % 20000 - 10000 );
	atomic_store( &stress_run, 1 );
	pthread_create( &io, NULL, stress_io, NULL );
	srandom( 58 );
	end = mach_absolute_time() + (uint64_t)( seconds * 1e9 * tb.denom / tb.numer );
	while ( mach_absolute_time() < end ) {
		// a stream on voice 100, written ahead at random, ended, stopped and resumed, made again
		if ( !stream || random() % 20000 == 0 ) {
			MacSnd_FreeSound( stream );
			streamChannels = 1 + random() % 2;
			stream = MacSnd_CreateStream( 128 + random() % 8192, streamChannels, 8000 + random() % 88000 );
			MacSnd_Start( 100, &( macSndStart_t ){ .sound = stream, .gainL = 0.5f, .gainR = 0.5f } );
		}
		if ( ( p = MacSnd_StreamBuffer( stream, &space ) ) ) {
			n = random() % ( space + 1 );
			memcpy( p, pcm, sizeof( short ) * streamChannels * ( n < 4096 / streamChannels ? n : 4096 / streamChannels ) );
			MacSnd_StreamCommit( stream, n );
		}
		switch ( random() % 4000 ) {
		case 0: MacSnd_StreamEnd( stream ); break;
		case 1: MacSnd_Stop( 100 ); break;
		case 2: MacSnd_Start( 100, &( macSndStart_t ){ .sound = stream, .gainL = 0.5f, .gainR = 0.5f } ); break;
		case 3: MacSnd_Pause( 100, random() % 2 ); break;
		}
		if ( MacSnd_StreamPlayed( stream ) > 1ull << 40 )
			bad++;

		k = random() % SOUNDS;
		v = random() % VOICES;
		switch ( random() % 12 ) {
		case 0:
			if ( sounds[k] ) {
				MacSnd_FreeSound( sounds[k] );
				sounds[k] = 0;
			} else {
				frames[k] = 1 + random() % 4096;
				sounds[k] = MacSnd_CreateSound( pcm, frames[k], 1 + random() % 2, 8000 + random() % 88000 );
			}
			break;
		case 1: case 2: case 3:
			if ( !sounds[k] )
				break;
			memset( &s, 0, sizeof( s ) );
			s.sound = sounds[k];
			s.gainL = (float)( random() % 100 ) / 100.0f;
			s.gainR = (float)( random() % 100 ) / 100.0f;
			s.rate = random() % 3 ? 0.0f : (float)( 4000 + random() % 120000 );
			s.startFrame = random() % ( frames[k] + 10 );
			s.loopStart = random() % ( frames[k] + 1 );
			s.loopEnd = random() % ( frames[k] + 1 );
			s.loopCount = random() % 4;
			s.paused = random() % 8 == 0;
			MacSnd_Start( v, &s );
			voiceFrames[v] = frames[k];
			break;
		case 4: MacSnd_Stop( v ); break;
		case 5: MacSnd_Pause( v, random() % 2 ); break;
		case 6: MacSnd_SetParams( v, (float)( random() % 150 ) / 100.0f, 0.5f, random() % 2 ? 0.0f : 30000.0f ); break;
		case 7: MacSnd_SetLoop( v, random() % 2000, random() % 4000, random() % 3 ); break;
		case 8: MacSnd_Seek( v, random() % 5000 ); break;
		case 9: MacSnd_SetStopFade( (float)( random() % 4 ) ); break;
		case 10: MacSnd_SetResampler( random() % 2 ); MacSnd_SetMaster( (float)( random() % 200 ) / 100.0f ); break;
		default: MacSnd_Update(); break;
		}
		status = MacSnd_Status( v );
		pos = MacSnd_Position( v );
		if ( status < MACSND_STOPPED || status > MACSND_PAUSED || pos < 0 || ( status != MACSND_STOPPED && pos > 5000 + voiceFrames[v] ) )
			bad++;
		ops++;
	}
	for ( k = 0; k < SOUNDS; k++ )
		if ( sounds[k] )
			MacSnd_FreeSound( sounds[k] );
	MacSnd_FreeSound( stream );
	for ( v = 0; v < VOICES; v++ )
		MacSnd_Stop( v );
	MacSnd_Stop( 100 );
	for ( i = 0; i < 200; i++ ) {
		usleep( 1000 );
		MacSnd_Update();
	}
	atomic_store( &stress_run, 0 );
	pthread_join( io, NULL );
	MacSnd_Update();
	MacSnd_GetStats( &st );
	check( !bad && st.sounds == 0 && st.soundBytes == 0,
		"%ld commands, %u callbacks, %u met a full ring, %ld bad states, %d sounds left, %zu bytes", ops, st.callbacks,
		st.ringDrops, bad, st.sounds, st.soundBytes );
	MacSnd_Shutdown();
}

/*
==============================================================================
device modes
==============================================================================
*/

static int cmpd( const void *a, const void *b )
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

// the process's CPU time (us), cycles, instructions and energy (nJ), as the kernel counts them
typedef struct { double cpu, cycles, instructions, energy; } usage_t;

static usage_t usage( void )
{
	struct rusage_info_v6 r;
	usage_t u = { 0 };

	if ( proc_pid_rusage( getpid(), RUSAGE_INFO_V6, (rusage_info_t *)&r ) == 0 ) {
		u.cpu = ( r.ri_user_time + r.ri_system_time ) * (double)tb.numer / tb.denom / 1000.0;
		u.cycles = (double)r.ri_cycles;
		u.instructions = (double)r.ri_instructions;
		u.energy = (double)r.ri_energy_nj;
	}
	return u;
}

static void print_cores( const macSndStats_t *a, const macSndStats_t *b )
{
	unsigned e = 0, p = 0;
	int i;

	for ( i = 0; i < 16; i++ ) {
		unsigned n = b->cpuCallbacks[i] - ( a ? a->cpuCallbacks[i] : 0 );
		if ( i < 6 )
			e += n;
		else
			p += n;
	}
	printf( "callbacks on E cores %u, P cores %u", e, p );
}

static int device_init( int frames )
{
	macSndConfig_t cfg = { 0 };

	cfg.bufferFrames = frames;
	cfg.print = say;
	cfg.device = getenv( "SNDTEST_DEVICE" );	// a device's name or UID (the default output otherwise)
	if ( !MacSnd_Init( &cfg ) )
		return 0;
	MacSnd_SetMaster( 0.0f );
	return 1;
}

static void device_run( int frames )
{
	enum { TRIALS = 300 };
	static const int counts[] = { 0, 16, 40, 96 };
	static double d[TRIALS];
	macSndStats_t st, a, b;
	macSndStart_t s;
	double lat, sum = 0, wms;
	usage_t u0, u1;
	uint64_t t0, w0, w1, deadline;
	int snd, t, c, i, timeouts = 0;
	short *zeros;

	printf( "device, %d frames:\n", frames );
	if ( !device_init( frames ) )
		return;
	MacSnd_GetStats( &st );
	if ( !st.device[0] ) {
		printf( "  no device\n" );
		MacSnd_Shutdown();
		return;
	}
	lat = st.latency;
	zeros = calloc( 22050 * 2, sizeof( short ) );
	snd = MacSnd_CreateSound( zeros, 22050 * 2, 1, 22050 );
	free( zeros );
	usleep( 500000 );

	// start -> output: the start at a random phase, the output time of the voice's first frame + the latency
	srandom( 58 );
	for ( t = 0; t < TRIALS; t++ ) {
		usleep( 20000 + random() % 13000 );
		s = play( snd, 1.0f );
		t0 = mach_absolute_time();
		MacSnd_Start( 0, &s );
		deadline = t0 + (uint64_t)( 100e6 * tb.denom / tb.numer );
		while ( !MacSnd_StartTime( 0 ) && mach_absolute_time() < deadline )
			;
		if ( !MacSnd_StartTime( 0 ) ) {
			timeouts++;
			d[t] = 100.0;
		} else
			d[t] = ms( MacSnd_StartTime( 0 ) - t0 ) + lat * 1000.0;
		sum += d[t];
		MacSnd_Stop( 0 );
	}
	qsort( d, TRIALS, sizeof( d[0] ), cmpd );
	printf( "  start -> output (%d starts at random phases): mean %.2f p50 %.2f p95 %.2f max %.2f ms (min %.2f; latency %.2f ms in it)%s\n",
		TRIALS, sum / TRIALS, d[TRIALS / 2], d[TRIALS * 95 / 100], d[TRIALS - 1], d[0], lat * 1000.0,
		timeouts ? " TIMEOUTS" : "" );

	// the CPU with N voices: looping, rates 0.9-1.1 of 22050, panned
	for ( c = 0; c < 4; c++ ) {
		for ( i = 0; i < counts[c]; i++ ) {
			s = play( snd, 1.0f );
			s.gainL = 0.2f + ( i % 7 ) * 0.1f;
			s.gainR = 0.9f - ( i % 7 ) * 0.1f;
			s.rate = 22050.0f * ( 0.9f + ( i % 5 ) * 0.05f );
			s.loopCount = 0;
			MacSnd_Start( i, &s );
		}
		usleep( 300000 );
		MacSnd_GetStats( &a );
		u0 = usage();
		w0 = mach_absolute_time();
		sleep( 4 );
		u1 = usage();
		w1 = mach_absolute_time();
		MacSnd_GetStats( &b );
		wms = ms( w1 - w0 );
		printf( "  %2d voices: process CPU %.1f us per ms, %.0f cycles and %.0f instructions per ms (%.2f GHz), %.1f uJ per s; "
			"the render %.1f us per ms, longest %.1f us, %d mixed; ", counts[c], ( u1.cpu - u0.cpu ) / wms,
			( u1.cycles - u0.cycles ) / wms, ( u1.instructions - u0.instructions ) / wms,
			( u1.cycles - u0.cycles ) / ( ( u1.cpu - u0.cpu ) * 1000.0 ), ( u1.energy - u0.energy ) / wms,
			( b.renderSeconds - a.renderSeconds ) * 1e6 / ( b.wallSeconds - a.wallSeconds ) / 1000.0, b.maxRender * 1e6, b.voices );
		print_cores( &a, &b );
		printf( "\n" );
		for ( i = 0; i < counts[c]; i++ )
			MacSnd_Stop( i );
	}

	// a re-created output unit (a device change): a looping voice goes on
	s = play( snd, 1.0f );
	s.loopCount = 0;
	MacSnd_Start( 0, &s );
	usleep( 200000 );
	i = MacSnd_Position( 0 );
	MacSnd_Restart();
	usleep( 200000 );
	MacSnd_GetStats( &st );
	check( st.reopens == 1 && st.device[0] && MacSnd_Status( 0 ) == MACSND_PLAYING && MacSnd_Position( 0 ) != i,
		"restart: the unit re-created, the looping voice goes on (%d -> %d)", i, MacSnd_Position( 0 ) );
	printf( "  callbacks %u, overloads %u, ring drops %u, longest render %.1f us\n", st.callbacks, st.overloads,
		st.ringDrops, st.maxRender * 1e6 );
	MacSnd_Shutdown();
}

static _Atomic int busy_run;

static void *busy( void *arg )
{
	volatile double x = 1.0;

	(void)arg;
	while ( atomic_load_explicit( &busy_run, memory_order_relaxed ) )
		x = x * 1.0000001 + 1e-9;
	return NULL;
}

static void device_load( int frames, int seconds, int threads, int voices )
{
	macSndStats_t a, b;
	macSndStart_t s;
	pthread_t th[32];
	int snd, i, t;
	short *zeros;

	printf( "load: %d frames, %d voices, %d s, %d spinning threads\n", frames, voices, seconds, threads );
	if ( !device_init( frames ) )
		return;
	zeros = calloc( 22050 * 2, sizeof( short ) );
	snd = MacSnd_CreateSound( zeros, 22050 * 2, 1, 22050 );
	free( zeros );
	for ( i = 0; i < voices; i++ ) {
		s = play( snd, 0.5f );
		s.rate = 22050.0f * ( 0.9f + ( i % 5 ) * 0.05f );
		s.loopCount = 0;
		MacSnd_Start( i, &s );
	}
	atomic_store( &busy_run, 1 );
	for ( i = 0; i < threads && i < 32; i++ )
		pthread_create( &th[i], NULL, busy, NULL );
	MacSnd_GetStats( &a );
	for ( t = 0; t < seconds; t += 5 ) {
		sleep( 5 );
		MacSnd_GetStats( &b );
		printf( "  %3d s: callbacks %u, overloads %u, render %.1f us per ms, longest %.1f us; ", t + 5, b.callbacks - a.callbacks,
			b.overloads - a.overloads, ( b.renderSeconds - a.renderSeconds ) * 1e6 / ( b.wallSeconds - a.wallSeconds ) / 1000.0,
			b.maxRender * 1e6 );
		print_cores( &a, &b );
		printf( "\n" );
		fflush( stdout );
	}
	atomic_store( &busy_run, 0 );
	for ( i = 0; i < threads && i < 32; i++ )
		pthread_join( th[i], NULL );
	MacSnd_GetStats( &b );
	printf( "  total: %u callbacks, %u overloads, longest render %.1f us\n", b.callbacks, b.overloads, b.maxRender * 1e6 );
	MacSnd_Shutdown();
}

static void list_devices( void )
{
	AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
	AudioDeviceID list[64], def = 0;
	UInt32 size = sizeof( list ), i, sz;

	AudioObjectGetPropertyData( kAudioObjectSystemObject, &a, 0, NULL, &size, list );
	a.mSelector = kAudioHardwarePropertyDefaultOutputDevice;
	sz = sizeof( def );
	AudioObjectGetPropertyData( kAudioObjectSystemObject, &a, 0, NULL, &sz, &def );
	for ( i = 0; i < size / sizeof( list[0] ); i++ ) {
		AudioObjectPropertyAddress p = { kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
		CFStringRef name = NULL;
		char cname[256] = "";
		Float64 rate = 0;
		UInt32 running = 0;

		sz = 0;
		if ( AudioObjectGetPropertyDataSize( list[i], &p, 0, NULL, &sz ) || !sz )
			continue;
		p.mScope = kAudioObjectPropertyScopeGlobal;
		p.mSelector = kAudioObjectPropertyName;
		sz = sizeof( name );
		if ( !AudioObjectGetPropertyData( list[i], &p, 0, NULL, &sz, &name ) && name ) {
			CFStringGetCString( name, cname, sizeof( cname ), kCFStringEncodingUTF8 );
			CFRelease( name );
		}
		p.mSelector = kAudioDevicePropertyNominalSampleRate;
		sz = sizeof( rate );
		AudioObjectGetPropertyData( list[i], &p, 0, NULL, &sz, &rate );
		p.mSelector = kAudioDevicePropertyDeviceIsRunningSomewhere;
		sz = sizeof( running );
		AudioObjectGetPropertyData( list[i], &p, 0, NULL, &sz, &running );
		printf( "%s %u \"%s\" %.0f Hz%s\n", list[i] == def ? "*" : " ", list[i], cname, rate, running ? " (in use)" : "" );
	}
}

int main( int argc, char **argv )
{
	int i, ran = 0;

	mach_timebase_info( &tb );
	setvbuf( stdout, NULL, _IOLBF, 0 );
	if ( argc > 1 && !strcmp( argv[1], "stress" ) ) {
		test_stress( argc > 2 ? atof( argv[2] ) : 5.0 );
	} else if ( argc > 1 && !strcmp( argv[1], "device" ) ) {
		if ( argc == 2 ) {
			device_run( 64 );
			device_run( 32 );
		}
		for ( i = 2; i < argc; i++ )
			device_run( atoi( argv[i] ) );
	} else if ( argc > 1 && !strcmp( argv[1], "load" ) ) {
		device_load( argc > 2 ? atoi( argv[2] ) : 64, argc > 3 ? atoi( argv[3] ) : 60, argc > 4 ? atoi( argv[4] ) : 0,
			argc > 5 ? atoi( argv[5] ) : 96 );
	} else if ( argc > 1 && !strcmp( argv[1], "devices" ) ) {
		list_devices();
		return 0;
	} else {
		static const struct { const char *name; void ( *fn )( void ); } tests[] = {
			{ "resample", test_resample }, { "loop", test_loop }, { "offset", test_offset }, { "pause", test_pause },
			{ "ramp", test_ramp }, { "limiter", test_limiter }, { "free", test_free }, { "misc", test_misc },
			{ "capture", test_capture }, { "stream", test_stream }, { "math", test_math }, { "ima", test_ima } };
		int t;

		for ( t = 0; t < (int)( sizeof( tests ) / sizeof( tests[0] ) ); t++ ) {
			int want = argc == 1;
			for ( i = 1; i < argc; i++ )
				want |= !strcmp( argv[i], tests[t].name );
			if ( want ) {
				tests[t].fn();
				ran++;
			}
		}
		if ( !ran ) {
			fprintf( stderr, "usage: see the top of port/sndtest.c\n" );
			return 2;
		}
	}
	if ( checks )
		printf( "%d checks, %d failed\n", checks, fails );
	return fails ? 1 : 0;
}
