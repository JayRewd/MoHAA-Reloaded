// sndtest.c -- the native sound engine's mixer (code/macos/macos_snd.c) on its own (port/sndtest.sh builds and runs
// it). The offline tests render through the IO thread's own code (MacSnd_Render) and check it to the sample; the
// stress test renders on a second thread, as the IO thread does, while this one posts at random (built with
// ThreadSanitizer); the device modes play silence on the real output: start -> output, CPU, overloads.
//
//   sndtest [test ...]                    offline: resample loop offset pause ramp limiter free misc (all by default)
//   sndtest stress [seconds]              the threaded stress test
//   sndtest device [frames ...]           start -> output at random phases, CPU at 0/16/40/96 voices, a restart
//   sndtest load frames seconds [busy] [voices]   96 voices for a while, overloads counted; busy = spinning threads
//   sndtest devices                       the output devices
//
// Every device mode is silent: the sounds are zeros and the master gain is 0. SNDTEST_DEVICE names another output.

#include "../code/macos/macos_snd.h"

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
	int i, v, k, status, pos;
	short pcm[2 * 4096];

	printf( "stress: %.0f s of random commands against a rendering thread\n", seconds );
	offline();
	for ( i = 0; i < 2 * 4096; i++ )
		pcm[i] = (short)( ( i * 7919 ) % 20000 - 10000 );
	atomic_store( &stress_run, 1 );
	pthread_create( &io, NULL, stress_io, NULL );
	srandom( 58 );
	end = mach_absolute_time() + (uint64_t)( seconds * 1e9 * tb.denom / tb.numer );
	while ( mach_absolute_time() < end ) {
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
	for ( v = 0; v < VOICES; v++ )
		MacSnd_Stop( v );
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
			{ "ramp", test_ramp }, { "limiter", test_limiter }, { "free", test_free }, { "misc", test_misc } };
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
