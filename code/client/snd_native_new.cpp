/*
===========================================================================
Copyright (C) 2024 the OpenMoHAA team
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

// snd_native_new.cpp -- the native sound driver (docs/METAL_PORT_PLAN.md Phase 9): the original game's Miles driver
// as OpenMoHAA reconstructed it in snd_openal_new.cpp (the channels, their picking and stealing, the looping sounds,
// the spatialization), on our own mixer (code/macos/macos_snd.c) instead of OpenAL, with the parity checklist's
// fixes (the plan's Phase 9 "Session 57 result"; the ids in the comments are its items). The original's math is in
// snd_native_math.h: the voices' levels without s_volume, which is the master gain's.
//
// Until 9g both drivers are in the binary: the latched s_native picks this one (snd_restart switches), and at 0 every
// entry point hands its call to OpenAL's (the S_OPENAL_ names, snd_openal_new.h). This is 9c, the sound effects:
// the streamed sounds, the music, the triggered music, the movies' audio and playmp3 are 9d's and don't play yet.
//
// A channel is a mixer voice, the same number. The policy code below keeps snd_openal_new.cpp's shape (the
// channel's Miles-named methods), so the two compare line by line: a start sends everything at once (MacSnd_Start),
// the gains and rates of the playing voices go out after S_Respatialize and in S_Update, only when they changed.

#include "snd_local.h"
#include "snd_openal_new.h"
#include "snd_native_new.h"
#include "snd_native_math.h"
#include "client.h"
#include "../server/server.h"
#include "snd_codec.h"
#include "../macos/macos_snd.h"

#include <mach/mach_time.h>

// F1: the original's 32 3D + 32 2D + 32 2D streamed channels (and the music's), then an overflow pool used before
// any steal; its voices aren't saved (the savegame's layout stays)
#define NATIVE_OVERFLOW_3D     32
#define NATIVE_OVERFLOW_2D     32
#define NATIVE_FIRST_OVERFLOW  MAX_SOUNDSYSTEM_CHANNELS
#define NATIVE_CHANNELS        (MAX_SOUNDSYSTEM_CHANNELS + NATIVE_OVERFLOW_3D + NATIVE_OVERFLOW_2D)
#define NATIVE_FIRST_OVERFLOW_2D (NATIVE_FIRST_OVERFLOW + NATIVE_OVERFLOW_3D)

#define NATIVE_PROBES          4096 // start -> output samples kept
#define NATIVE_CAPTURE_FRAMES  48000

typedef enum {
    NATIVE_KIND_3D,
    NATIVE_KIND_2D,
    NATIVE_KIND_STREAM,
    NATIVE_KIND_MUSIC // songs, mp3, triggered music, the movie: 9d
} native_kind_t;

typedef struct {
    vec3_t vOrigin;
    vec3_t vRelativeOrigin;
    vec3_t vVelocity;

    sfx_t   *pSfx;
    qboolean bPlaying;

    int   iChannel;
    float fVolume;
    float fPitch;

    int      iStartTime;
    qboolean bInUse;
    qboolean bCombine;

    float fBaseVolume;
    float fMinDist;
    float fMaxDist;
    int   iFlags;
} native_loop_sound_t;

struct native_channel {
    // snd_openal_new.cpp's channel (the decompiled one's state)
    sfx_t *pSfx;
    int    iEntNum;
    int    iEntChannel;
    vec3_t vOrigin;

    float fVolume;
    int   iBaseRate;
    float fNewPitchMult;
    float fMinDist;
    float fMaxDist;

    int iStartTime;
    int iTime;
    int iEndTime;
    int iFlags;

    // the voice
    int      iVoice; // the mixer's, the channel's number
    int      iKind;
    int      iSound;  // the mixer's sound set_sfx gave it
    bool     b3D;     // gains from the 3D model (set_3d), or the 2D pan (set_no_3d)
    vec3_t   vPosition; // set_position
    int      iPan;      // set_sample_pan, 0-127
    float    fGain;     // set_gain: the level, without s_volume (the master's)
    float    fRate;     // set_sample_playback_rate, Hz
    int      iOffset;   // set_sample_offset: where the next start begins
    int      iLoopStart, iLoopEnd, iLoopCount;
    float    fPostedL, fPostedR, fPostedRate; // what the mixer was last told
    vec3_t   vSpatial;    // the origin a 2D voice was last panned from (the event log)
    uint64_t iRequestTime; // the probe: the host time of a start not yet seen leaving the device

public:
    void play();
    void stop();
    void pause();

    void set_no_3d();
    void set_3d();

    void set_gain(float gain);
    void set_position(float v0, float v1, float v2);

    bool is_free();
    bool is_paused();
    bool is_playing();

    void force_free();
    bool set_sfx(sfx_t *sfx);

    void start_sample();
    void resume_sample();
    void end_sample();

    void set_sample_pan(int pan);
    void set_sample_playback_rate(int rate);
    int  sample_playback_rate();

    unsigned int sample_offset();
    unsigned int sample_loop_count();
    void         set_sample_offset(unsigned int offset);
    void         set_sample_loop_count(int count);
    void         set_sample_loop_block(int start_offset, int end_offset);

    int sample_status();

    void gains(float *left, float *right);
    void post();
};

static struct {
    native_channel  chan[NATIVE_CHANNELS];
    native_channel *channel[NATIVE_CHANNELS];

    native_loop_sound_t loop_sounds[MAX_SOUNDSYSTEM_LOOP_SOUNDS];

    vec3_t vListenerOrigin;
    vec3_t vListenerAxis[3];
    float  fMaster;

    // the event log, the capture
    fileHandle_t eventLog;
    fileHandle_t capture;
    int          captureFrames;
    float        captureBuf[4096 * 2];

    // the probe and the game thread's time
    float    probeMs[NATIVE_PROBES];
    int      numProbes;
    double   latency; // the device's, seconds
    unsigned reopens;
    uint64_t gameTicks;
    unsigned frames;
} native;

static bool  s_bNative          = false; // the driver S_Init picked
static bool  native_initialized = false;
static int   s_iNextLoopingWarning = 0;
static int   s_iReverbType         = 0;
static float s_fReverbLevel        = 0;
static bool  s_bFading             = false;
static float s_fFadeVolume         = 1.f;
static float s_fFadeStartTime;
static float s_fFadeStopTime;
static char  current_soundtrack[128];
static mach_timebase_info_data_t s_timebase;

// the front end reads it (snd_local_new.h); OpenAL's driver has its own while it is the one
cvar_t        *s_show_sounds;
static cvar_t *s_native;
static cvar_t *s_device;
static cvar_t *s_bufferFrames;
static cvar_t *s_eventLog;
static cvar_t *s_capture;
static cvar_t *s_captureMute;
// the original's, as OpenAL's driver registers them
static cvar_t *s_milesdriver;
static cvar_t *s_reverb;
static cvar_t *s_show_cpu;
static cvar_t *s_show_num_active_sounds;
static cvar_t *s_speaker_type;
static cvar_t *s_obstruction_cal_time;

static int  S_NATIVE_SpatializeStereoSound(const vec3_t listener_origin, const vec3_t listener_left, const vec3_t origin);
static void S_NATIVE_SpatializeChannel(native_channel *pChannel, const vec3_t vListenerOrigin, const vec3_t vTempAxis);
static void S_NATIVE_LogStart(native_channel *pChannel, float fLeft, float fRight);

// the game thread's time in the driver (the shutdown's summary)
struct native_timer {
    uint64_t t0;

    native_timer()
        : t0(mach_absolute_time())
    {}

    ~native_timer() { native.gameTicks += mach_absolute_time() - t0; }
};

static double S_NATIVE_TicksToMs(uint64_t ticks)
{
    return (double)ticks * s_timebase.numer / s_timebase.denom * 1e-6;
}

/*
==============
S_NATIVE_MixerPrint
==============
*/
static void S_NATIVE_MixerPrint(const char *msg)
{
    Com_Printf("%s", msg);
}

/*
==============
S_NATIVE_MasterVolume

The voices' levels leave s_volume and the fade out (OpenAL's S_GetBaseVolume): the master gain has them (A4)
==============
*/
static float S_NATIVE_MasterVolume()
{
    return SNM_Master(s_volume->value, s_fFadeVolume);
}

/*
==============
S_NATIVE_FactFrames

The frame count in a loaded WAV's fact chunk (before its data chunk), -1 without one
==============
*/
static int S_NATIVE_FactFrames(const sfx_t *pSfx)
{
    const byte *chunk = pSfx->data + 12;
    const byte *end   = pSfx->data + pSfx->info.dataofs - 8;

    while (chunk + 8 <= end) {
        const int len = chunk[4] | (chunk[5] << 8) | (chunk[6] << 16) | (chunk[7] << 24);

        if (len < 0) {
            break;
        }
        if (!memcmp(chunk, "fact", 4) && len >= 4 && chunk + 12 <= end) {
            return chunk[8] | (chunk[9] << 8) | (chunk[10] << 16) | (chunk[11] << 24);
        }
        chunk += 8 + ((len + 1) & ~1);
    }

    return -1;
}

/*
==============
S_NATIVE_CreateSfx

A sound's mixer copy, 16-bit PCM: WAV PCM as it was loaded (downsampled to s_khz by the front end, D7), 8-bit
widened, IMA ADPCM decoded here (H2), an MP3 through its codec. Every frame of the data chunk (H1: OpenAL's upload
lost the last ~22 frames of every WAV that wasn't downsampled).
==============
*/
static int S_NATIVE_CreateSfx(sfx_t *pSfx)
{
    int    sound = 0;
    int    frames, i;
    short *pcm;

    if (pSfx->iFlags & SFX_FLAG_STREAMED) {
        // 9d
        return 0;
    }

    if (pSfx->iFlags & SFX_FLAG_MP3) {
        snd_info_t info;
        byte      *data;

        data = (byte *)S_CodecLoad(pSfx->name, &info);
        if (!data) {
            return 0;
        }

        if ((info.width == 1 || info.width == 2) && (info.channels == 1 || info.channels == 2) && info.rate > 0) {
            frames = info.size / ((int)info.width * info.channels);
            if (frames > 0) {
                sound = MacSnd_CreateSound(NULL, frames, info.channels, info.rate);
            }
            pcm = sound ? MacSnd_SoundData(sound) : NULL;
            if (pcm && info.width == 2) {
                memcpy(pcm, data, (size_t)frames * info.channels * sizeof(short));
            } else if (pcm) {
                for (i = 0; i < frames * info.channels; i++) {
                    pcm[i] = (short)((data[i] - 128) << 8);
                }
            }

            // its rate and length as decoded (the front end loads an MP3's file only: no header, its byte count)
            if (pcm) {
                pSfx->info.rate     = info.rate;
                pSfx->info.channels = info.channels;
                pSfx->length        = frames;
                pSfx->time_length   = frames * 1000.f / info.rate;
            }
        }

        Hunk_FreeTempMemory(data);
        return sound;
    }

    if (!pSfx->data || pSfx->info.channels < 1 || pSfx->info.channels > 2 || pSfx->info.rate < 1) {
        return 0;
    }

    if (pSfx->info.format == 1 && (pSfx->info.width == 1 || pSfx->info.width == 2)) {
        const byte *src = pSfx->data + pSfx->info.dataofs;

        frames = pSfx->info.samples / pSfx->info.channels;
        if (frames <= 0) {
            return 0;
        }
        sound = MacSnd_CreateSound(NULL, frames, pSfx->info.channels, (int)pSfx->info.rate);
        pcm   = MacSnd_SoundData(sound);
        if (!pcm) {
            return 0;
        }
        if (pSfx->info.width == 2) {
            memcpy(pcm, src, (size_t)frames * pSfx->info.channels * sizeof(short));
        } else {
            for (i = 0; i < frames * pSfx->info.channels; i++) {
                pcm[i] = (short)((src[i] - 128) << 8);
            }
        }
    } else if (pSfx->info.format == 17 && pSfx->info.dataalign > 1) {
        // The front end's data size drops the last block (the AL extension takes whole blocks): the data chunk is
        // datasize - dataofs plus one block (GetWavinfo)
        const int channels   = pSfx->info.channels;
        const int blockAlign = ((pSfx->info.dataalign - 1) / 8 * 4 + 4) * channels;
        const int bytes      = pSfx->info.datasize - pSfx->info.dataofs + blockAlign;
        const int fact       = S_NATIVE_FactFrames(pSfx);

        frames = SNM_IMAFrames(bytes, blockAlign, channels);
        if (fact >= 0 && fact < frames) {
            frames = fact;
        }
        if (frames <= 0) {
            return 0;
        }
        sound = MacSnd_CreateSound(NULL, frames, channels, (int)pSfx->info.rate);
        pcm   = MacSnd_SoundData(sound);
        if (!pcm) {
            return 0;
        }
        frames = SNM_DecodeIMA(pSfx->data + pSfx->info.dataofs, bytes, blockAlign, channels, pcm, frames);

        // its length as decoded (the front end's counts the bytes)
        pSfx->length      = frames;
        pSfx->time_length = frames / pSfx->info.rate * 1000.f;
    } else {
        Com_Printf(
            "sound: unsupported WAV (format %d, %d channels, %d bits) [%s].\n",
            pSfx->info.format,
            pSfx->info.channels,
            (int)(pSfx->info.width * 8),
            pSfx->name
        );
    }

    return sound;
}

/*
==============
S_NATIVE_SfxLoaded
==============
*/
void S_NATIVE_SfxLoaded(sfx_t *pSfx)
{
    if (!s_bNative || !native_initialized || pSfx->buffer) {
        return;
    }

    pSfx->buffer = S_NATIVE_CreateSfx(pSfx);
}

/*
==============
S_NATIVE_FreeSfx

H4: OpenAL's buffers leaked when registration dropped a sound. The mixer keeps the memory until no voice plays it.
==============
*/
void S_NATIVE_FreeSfx(sfx_t *pSfx)
{
    if (!s_bNative || !native_initialized || !pSfx->buffer) {
        return;
    }

    MacSnd_FreeSound(pSfx->buffer);
    pSfx->buffer = 0;
}

/*
==============
S_NATIVE_InitChannel
==============
*/
static void S_NATIVE_InitChannel(int idx, int iKind)
{
    native_channel *chan = &native.chan[idx];

    native.channel[idx] = chan;
    *chan               = {};
    chan->fVolume       = 1.0;
    chan->iEntNum       = ENTITYNUM_NONE;
    chan->iVoice        = idx;
    chan->iKind         = iKind;
    chan->iPan          = 64;
    chan->fGain         = 1;
    chan->iLoopCount    = 1;
}

/*
==============
S_NATIVE_NotYet

The music's, the mp3's and the triggered music's commands: 9d
==============
*/
static void S_NATIVE_NotYet()
{
    Com_DPrintf("sound: '%s' doesn't play on the native driver yet\n", Cmd_Argv(0));
}

/*
==============
S_NATIVE_Init
==============
*/
qboolean S_NATIVE_Init()
{
    macSndConfig_t mixer;
    int            i;

    s_native       = Cvar_Get("s_native", "0", CVAR_SOUND_LATCH);
    s_device       = Cvar_Get("s_device", "", CVAR_ARCHIVE | CVAR_SOUND_LATCH);
    s_bufferFrames = Cvar_Get("s_bufferFrames", "64", CVAR_ARCHIVE | CVAR_SOUND_LATCH);
    s_show_sounds  = Cvar_Get("s_show_sounds", "0", 0);

    s_bNative = s_native->integer != 0;
    if (!s_bNative) {
        return S_OPENAL_Init();
    }

    if (native_initialized) {
        Com_DPrintf("S_NATIVE_Init(): Called when sound is already initialized!\n");
        return qtrue;
    }

    s_milesdriver            = Cvar_Get("s_milesdriver", "auto", CVAR_SOUND_LATCH | CVAR_ARCHIVE);
    s_reverb                 = Cvar_Get("s_reverb", "0", CVAR_SOUND_LATCH | CVAR_ARCHIVE);
    s_show_cpu               = Cvar_Get("s_show_cpu", "0", 0);
    s_show_num_active_sounds = Cvar_Get("s_show_num_active_sounds", "0", 0);
    s_speaker_type           = Cvar_Get("s_speaker_type", "0", CVAR_ARCHIVE);
    s_obstruction_cal_time   = Cvar_Get("s_obstruction_cal_time", "500", CVAR_ARCHIVE);
    s_eventLog               = Cvar_Get("s_eventLog", "0", 0);
    s_capture                = Cvar_Get("s_capture", "", 0);
    s_captureMute            = Cvar_Get("s_captureMute", "0", 0);
    s_capture->modified      = qtrue;

    mach_timebase_info(&s_timebase);

    memset(&mixer, 0, sizeof(mixer));
    mixer.device       = s_device->string;
    mixer.bufferFrames = s_bufferFrames->integer;
    mixer.maxVoices    = NATIVE_CHANNELS;
    mixer.maxSounds    = MAX_SFX + 64;
    mixer.print        = S_NATIVE_MixerPrint;
    if (!MacSnd_Init(&mixer)) {
        Com_Printf("sound: the native mixer couldn't start. No audio will play.\n");
        return qfalse;
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_CHANNELS_3D; i++) {
        S_NATIVE_InitChannel(i, NATIVE_KIND_3D);
    }
    for (i = 0; i < MAX_SOUNDSYSTEM_CHANNELS_2D; i++) {
        S_NATIVE_InitChannel(MAX_SOUNDSYSTEM_CHANNELS_3D + i, NATIVE_KIND_2D);
    }
    for (i = 0; i < MAX_SOUNDSYSTEM_CHANNELS_2D_STREAM; i++) {
        S_NATIVE_InitChannel(MAX_SOUNDSYSTEM_CHANNELS_3D + MAX_SOUNDSYSTEM_CHANNELS_2D + i, NATIVE_KIND_STREAM);
    }
    for (i = MAX_SOUNDSYSTEM_POSITION_CHANNELS; i < MAX_SOUNDSYSTEM_CHANNELS; i++) {
        S_NATIVE_InitChannel(i, NATIVE_KIND_MUSIC);
    }
    for (i = 0; i < NATIVE_OVERFLOW_3D; i++) {
        S_NATIVE_InitChannel(NATIVE_FIRST_OVERFLOW + i, NATIVE_KIND_3D);
    }
    for (i = 0; i < NATIVE_OVERFLOW_2D; i++) {
        S_NATIVE_InitChannel(NATIVE_FIRST_OVERFLOW_2D + i, NATIVE_KIND_2D);
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_LOOP_SOUNDS; i++) {
        native.loop_sounds[i] = {};
    }

    VectorClear(native.vListenerOrigin);
    AxisClear(native.vListenerAxis);
    native.fMaster = S_NATIVE_MasterVolume();
    MacSnd_SetMaster(native.fMaster);
    native.numProbes = 0;
    native.gameTicks = 0;
    native.frames    = 0;
    native.reopens   = 0;
    native.latency   = 0;

    if (s_speaker_type->integer) {
        // 9f: headphone 3D; the original's Fast 2D was stereo (no 5.1 or quad)
        Com_Printf("sound: s_speaker_type %d plays stereo on the native driver\n", s_speaker_type->integer);
    }
    s_speaker_type->modified = qfalse;
    s_reverb->modified       = qfalse;

    Cmd_AddCommand("playmp3", S_NATIVE_NotYet);
    Cmd_AddCommand("stopmp3", S_NATIVE_NotYet);
    Cmd_AddCommand("loadsoundtrack", S_NATIVE_NotYet);
    Cmd_AddCommand("playsong", S_NATIVE_NotYet);
    Cmd_AddCommand("pitch", S_NATIVE_NotYet);
    Cmd_AddCommand("tmstart", S_NATIVE_NotYet);
    Cmd_AddCommand("tmstartloop", S_NATIVE_NotYet);
    Cmd_AddCommand("tmstop", S_NATIVE_NotYet);
    Cmd_AddCommand("tmvolume", S_NATIVE_NotYet);

    S_NATIVE_ClearLoopingSounds();
    load_sfx_info();
    native_initialized = true;

    S_CodecInit();

    // the sounds already loaded (a snd_restart): their mixer copies
    for (i = 0; i < s_numSfx; i++) {
        if (s_knownSfx[i].name[0] && s_knownSfx[i].data) {
            s_knownSfx[i].buffer = 0;
            S_NATIVE_SfxLoaded(&s_knownSfx[i]);
        }
    }

    return qtrue;
}

/*
==============
S_NATIVE_Summary

The shutdown's account of the session: start -> output (the probe), the device, the game thread's time
==============
*/
static int S_NATIVE_CompareFloat(const void *a, const void *b)
{
    const float x = *(const float *)a, y = *(const float *)b;

    return x < y ? -1 : x > y;
}

static void S_NATIVE_Summary()
{
    macSndStats_t stats;
    float         sorted[NATIVE_PROBES];
    int           n, i, perf;
    double        sum;

    MacSnd_GetStats(&stats);

    n = native.numProbes < NATIVE_PROBES ? native.numProbes : NATIVE_PROBES;
    if (n) {
        memcpy(sorted, native.probeMs, n * sizeof(float));
        qsort(sorted, n, sizeof(float), S_NATIVE_CompareFloat);
        for (i = 0, sum = 0; i < n; i++) {
            sum += sorted[i];
        }
        Com_Printf(
            "sound: start -> output %d starts (the last %d): mean %.2f, p50 %.2f, p95 %.2f, max %.2f ms\n",
            native.numProbes,
            n,
            sum / n,
            sorted[n / 2],
            sorted[n * 95 / 100],
            sorted[n - 1]
        );
    }

    for (i = 6, perf = 0; i < 16; i++) {
        perf += stats.cpuCallbacks[i];
    }
    Com_Printf(
        "sound: %s, %.0f Hz, %d frames; %u callbacks (%u on cores 6+), %u overloads, %u ring drops, %u reopens; "
        "render %.2f us per ms of audio, longest %.0f us\n",
        stats.device,
        stats.rate,
        stats.bufferFrames,
        stats.callbacks,
        perf,
        stats.overloads,
        stats.ringDrops,
        stats.reopens,
        stats.wallSeconds > 0 ? stats.renderSeconds * 1e3 / stats.wallSeconds : 0,
        stats.maxRender * 1e6
    );
    Com_Printf(
        "sound: game thread %.2f us a frame over %u frames; %d sounds, %.1f MB\n",
        native.frames ? S_NATIVE_TicksToMs(native.gameTicks) * 1e3 / native.frames : 0,
        native.frames,
        stats.sounds,
        stats.soundBytes / (1024.0 * 1024.0)
    );
}

/*
==============
S_NATIVE_StopCapture
==============
*/
static void S_NATIVE_StopCapture()
{
    int header[2];

    MacSnd_Capture(0, 0);
    if (!native.capture) {
        return;
    }

    // the RIFF and data chunks' sizes
    header[0] = 36 + native.captureFrames * 8;
    FS_Seek(native.capture, 4, FS_SEEK_SET);
    FS_Write(&header[0], 4, native.capture);
    header[1] = native.captureFrames * 8;
    FS_Seek(native.capture, 40, FS_SEEK_SET);
    FS_Write(&header[1], 4, native.capture);
    FS_FCloseFile(native.capture);
    native.capture = 0;

    Com_Printf("sound: captured %d frames\n", native.captureFrames);
}

/*
==============
S_NATIVE_Shutdown
==============
*/
void S_NATIVE_Shutdown()
{
    int i;

    if (!s_bNative) {
        S_OPENAL_Shutdown();
        return;
    }

    if (!native_initialized) {
        Com_DPrintf("S_NATIVE_Shutdown(): Called when sound is NOT initialized!\n");
        return;
    }

    S_NATIVE_StopAllSounds(true);

    Cmd_RemoveCommand("playmp3");
    Cmd_RemoveCommand("stopmp3");
    Cmd_RemoveCommand("loadsoundtrack");
    Cmd_RemoveCommand("playsong");
    Cmd_RemoveCommand("pitch");
    Cmd_RemoveCommand("tmstart");
    Cmd_RemoveCommand("tmstartloop");
    Cmd_RemoveCommand("tmstop");
    Cmd_RemoveCommand("tmvolume");

    S_NATIVE_StopCapture();
    if (native.eventLog) {
        FS_FCloseFile(native.eventLog);
        native.eventLog = 0;
    }

    S_NATIVE_Summary();
    MacSnd_Shutdown();

    for (i = 0; i < s_numSfx; i++) {
        s_knownSfx[i].buffer = 0;
    }

    native_initialized = false;
}

/*
==============
S_FadeSound
==============
*/
void S_FadeSound(float fTime)
{
    if (!s_bNative) {
        S_OPENAL_FadeSound(fTime);
        return;
    }

    Com_Printf("Called FadeSound with: %f\n", fTime);

    if (fTime > 0) {
        s_bFading        = true;
        s_fFadeStartTime = cls.realtime;
        s_fFadeVolume    = 1;
        s_fFadeStopTime  = cls.realtime + fTime;
    } else {
        s_fFadeVolume = 1;
        s_bFading     = false;
    }
}

/*
==============
S_NeedFullRestart
==============
*/
qboolean S_NeedFullRestart()
{
    if (!s_bNative) {
        return S_OPENAL_NeedFullRestart();
    }

    return Cvar_Get("s_initsound", "1", 0)->integer != s_bLastInitSound;
}

/*
==============
S_PrintInfo
==============
*/
void S_PrintInfo()
{
    macSndStats_t stats;

    if (!s_bNative) {
        S_OPENAL_PrintInfo();
        return;
    }

    Com_Printf("----- Sound Info -----\n");

    if (s_bSoundStarted) {
        MacSnd_GetStats(&stats);
        Com_Printf("driver - native\n");
        Com_Printf("device - %s\n", stats.device[0] ? stats.device : "{none}");
        Com_Printf("samplebits - 16\n");
        Com_Printf("speed - %.0f\n", stats.rate);
        Com_Printf("buffer - %d frames (%.2f ms), latency %.2f ms\n",
            stats.bufferFrames, stats.rate > 0 ? stats.bufferFrames * 1000.0 / stats.rate : 0, stats.latency * 1000.0);
        Com_Printf("voices - %d playing, %d sounds (%.1f MB)\n", stats.voices, stats.sounds,
            stats.soundBytes / (1024.0 * 1024.0));
        Com_Printf("overloads - %u, ring drops - %u\n", stats.overloads, stats.ringDrops);
    } else {
        Com_Printf("sound system not started\n");
    }

    Com_Printf("----------------------\n");
}

/*
==============
S_DumpStatus
==============
*/
static void S_DumpStatus(const char *pszChanName, int iChanNum, native_channel *channel)
{
    sfx_t *sfx;
    int    status;

    sfx    = channel->pSfx;
    status = channel->sample_status();

    if (status == MACSND_PLAYING || status == MACSND_PAUSED) {
        const char *pszMode = status == MACSND_PLAYING ? "playing" : "paused";

        if (sfx) {
            if (sfx->name[0]) {
                Com_Printf("%s channel %i - %s sfx %s\n", pszChanName, iChanNum, pszMode, sfx->name);
            } else {
                Com_Printf("%s channel %i - %s with nameless sfx\n", pszChanName, iChanNum, pszMode);
            }
        } else {
            Com_Printf("%s channel %i - %s with NULL sfx\n", pszChanName, iChanNum, pszMode);
        }
    }
}

/*
==============
S_DumpInfo
==============
*/
void S_DumpInfo()
{
    int i;

    if (!s_bNative) {
        S_OPENAL_DumpInfo();
        return;
    }

    if (!native_initialized) {
        return;
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_CHANNELS_3D; i++) {
        S_DumpStatus("3D", i, native.channel[i]);
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_CHANNELS_2D; i++) {
        S_DumpStatus("2D", i, native.channel[MAX_SOUNDSYSTEM_CHANNELS_3D + i]);
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_CHANNELS_2D_STREAM; i++) {
        S_DumpStatus("2D stream", i, native.channel[MAX_SOUNDSYSTEM_CHANNELS_3D + MAX_SOUNDSYSTEM_CHANNELS_2D + i]);
    }

    for (i = 0; i < NATIVE_OVERFLOW_3D; i++) {
        S_DumpStatus("3D overflow", i, native.channel[NATIVE_FIRST_OVERFLOW + i]);
    }

    for (i = 0; i < NATIVE_OVERFLOW_2D; i++) {
        S_DumpStatus("2D overflow", i, native.channel[NATIVE_FIRST_OVERFLOW_2D + i]);
    }
}

/*
==============
S_NATIVE_IsPositionChannel

The effects' channels: every one but the music's (MAX_SOUNDSYSTEM_POSITION_CHANNELS and the overflow pool)
==============
*/
static inline bool S_NATIVE_IsPositionChannel(int i)
{
    return i < MAX_SOUNDSYSTEM_POSITION_CHANNELS || i >= NATIVE_FIRST_OVERFLOW;
}

/*
==============
S_NATIVE_PauseSound
==============
*/
static void S_NATIVE_PauseSound()
{
    int i;

    if (!s_bSoundStarted) {
        return;
    }

    s_bSoundPaused = true;

    for (i = 0; i < NATIVE_CHANNELS; i++) {
        native_channel *pChannel = native.channel[i];
        if (!S_NATIVE_IsPositionChannel(i)) {
            continue;
        }

        if (pChannel->iEntChannel == CHAN_MENU) {
            continue;
        }

        if (!pChannel->is_playing()) {
            continue;
        }

        pChannel->pause();
    }
}

/*
==============
S_NATIVE_UnpauseSound
==============
*/
static void S_NATIVE_UnpauseSound()
{
    int i;

    if (!s_bSoundStarted) {
        return;
    }

    for (i = 0; i < NATIVE_CHANNELS; i++) {
        native_channel *pChannel = native.channel[i];
        if (!S_NATIVE_IsPositionChannel(i)) {
            continue;
        }

        if (pChannel->is_paused() || (pChannel->iFlags & CHANNEL_FLAG_PLAY_DEFERRED)) {
            pChannel->iFlags &= ~CHANNEL_FLAG_PLAY_DEFERRED;
            pChannel->play();
        }
    }

    s_bSoundPaused = false;
}

/*
==============
S_NATIVE_ShouldPlay

F2: at most max_number_playing voices of a sound (10 unless global/sound*.txt says otherwise)
==============
*/
static qboolean S_NATIVE_ShouldPlay(sfx_t *pSfx)
{
    if (sfx_infos[pSfx->sfx_info_index].max_number_playing <= 0) {
        return qtrue;
    }

    int iRemainingTimesToPlay;
    int i;

    iRemainingTimesToPlay = sfx_infos[pSfx->sfx_info_index].max_number_playing;

    for (i = 0; i < NATIVE_CHANNELS; i++) {
        native_channel *pChannel = native.channel[i];
        if (!S_NATIVE_IsPositionChannel(i)) {
            continue;
        }

        if (pChannel->pSfx == pSfx && pChannel->is_playing()) {
            iRemainingTimesToPlay--;
            if (!iRemainingTimesToPlay) {
                return qfalse;
            }
        }
    }

    return qtrue;
}

/*
==============
S_NATIVE_ShouldStart

B3: a 3D one-shot starts only inside its max distance
==============
*/
static qboolean S_NATIVE_ShouldStart(const vec3_t vOrigin, float fMinDist, float fMaxDist)
{
    vec3_t vDir;

    if (!native_initialized) {
        return false;
    }

    VectorSubtract(vOrigin, native.vListenerOrigin, vDir);

    return Square(fMaxDist) > VectorLengthSquared(vDir);
}

/*
==============
S_NATIVE_PickChannelBase

The decompiled picking: the voice already playing the same entity and channel, else a free one, else (F1) a free
voice of the overflow pool, else steal the oldest of the lowest channel priority not above the new sound's (never
the listener's own for another entity's sound). OpenAL's version also scanned the other ranges, to no effect.
==============
*/
static int S_NATIVE_PickChannelBase(
    int iEntNum, int iEntChannel, int iFirstChannel, int iLastChannel, int iFirstOverflow, int iLastOverflow
)
{
    int             i;
    int             iBestChannel;
    native_channel *pChannel;

    iBestChannel = -1;
    if (iEntNum != ENTITYNUM_NONE && iEntChannel) {
        for (i = iFirstChannel; i <= iLastChannel; i++) {
            pChannel = native.channel[i];

            if (pChannel->is_free()) {
                iBestChannel = i;
                continue;
            }

            if (pChannel->iEntNum == iEntNum && pChannel->iEntChannel == iEntChannel) {
                pChannel->end_sample();
                return i;
            }
        }

        for (i = iFirstOverflow; i <= iLastOverflow; i++) {
            pChannel = native.channel[i];

            if (!pChannel->is_free() && pChannel->iEntNum == iEntNum && pChannel->iEntChannel == iEntChannel) {
                pChannel->end_sample();
                return i;
            }
        }
    }

    if (iBestChannel >= 0) {
        return iBestChannel;
    }

    for (i = iFirstChannel; i <= iLastChannel; i++) {
        if (native.channel[i]->is_free()) {
            return i;
        }
    }

    // Added in the Metal port (F1)
    //  More voices before any steal
    for (i = iFirstOverflow; i <= iLastOverflow; i++) {
        if (native.channel[i]->is_free()) {
            return i;
        }
    }

    {
        int iBestTime = 0x7FFFFFFF;

        for (i = iFirstChannel; i <= iLastChannel; i++) {
            pChannel = native.channel[i];

            if (pChannel->iEntNum == s_iListenerNumber && iEntNum != pChannel->iEntNum) {
                continue;
            }

            if (pChannel->iEntChannel < iEntChannel
                || (pChannel->iEntChannel == iEntChannel && pChannel->iStartTime < iBestTime)) {
                iBestChannel = i;
                iEntChannel  = pChannel->iEntChannel;
                iBestTime    = pChannel->iStartTime;
            }
        }
    }

    return iBestChannel;
}

/*
==============
S_NATIVE_PickChannel3D
==============
*/
static int S_NATIVE_PickChannel3D(int iEntNum, int iEntChannel)
{
    return S_NATIVE_PickChannelBase(
        iEntNum,
        iEntChannel,
        0,
        MAX_SOUNDSYSTEM_CHANNELS_3D - 1,
        NATIVE_FIRST_OVERFLOW,
        NATIVE_FIRST_OVERFLOW + NATIVE_OVERFLOW_3D - 1
    );
}

/*
==============
S_NATIVE_PickChannel2D
==============
*/
static int S_NATIVE_PickChannel2D(int iEntNum, int iEntChannel)
{
    return S_NATIVE_PickChannelBase(
        iEntNum,
        iEntChannel,
        MAX_SOUNDSYSTEM_CHANNELS_3D,
        MAX_SOUNDSYSTEM_CHANNELS_3D + MAX_SOUNDSYSTEM_CHANNELS_2D - 1,
        NATIVE_FIRST_OVERFLOW_2D,
        NATIVE_FIRST_OVERFLOW_2D + NATIVE_OVERFLOW_2D - 1
    );
}

/*
==============
S_NATIVE_PickChannel2DStreamed
==============
*/
static int S_NATIVE_PickChannel2DStreamed(int iEntNum, int iEntChannel)
{
    return S_NATIVE_PickChannelBase(
        iEntNum,
        iEntChannel,
        MAX_SOUNDSYSTEM_CHANNELS_3D + MAX_SOUNDSYSTEM_CHANNELS_2D,
        MAX_SOUNDSYSTEM_CHANNELS_3D + MAX_SOUNDSYSTEM_CHANNELS_2D + MAX_SOUNDSYSTEM_CHANNELS_2D_STREAM - 1,
        0,
        -1
    );
}

/*
==============
S_NATIVE_CallbackServer
==============
*/
static void S_NATIVE_CallbackServer(int entnum, int channel_number, const char *name)
{
    if (!com_sv_running->integer) {
        return;
    }

    SV_SoundCallback(entnum, channel_number, name);
}

/*
==============
S_NATIVE_Start2DSound
==============
*/
static void S_NATIVE_Start2DSound(
    const vec3_t vOrigin,
    int          iEntNum,
    int          iEntChannel,
    sfx_t       *pSfx,
    float        fVolume,
    float        fMinDistance,
    float        fPitch,
    float        fMaxDistance
)
{
    int             iRealEntNum;
    int             iFreeChannel;
    native_channel *pChannel;
    bool            bSupportWaitTillSoundDone;

    iRealEntNum = iEntNum & ~S_FLAG_DO_CALLBACK;

    if (pSfx->iFlags & SFX_FLAG_STREAMED) {
        iFreeChannel = S_NATIVE_PickChannel2DStreamed(iRealEntNum, iEntChannel);
    } else {
        iFreeChannel = S_NATIVE_PickChannel2D(iRealEntNum, iEntChannel);
    }

    if (iFreeChannel < 0) {
        Com_DPrintf(
            "sound: Couldn't play %s sound '%s' for entity %i on channel %s\n",
            (pSfx->iFlags & SFX_FLAG_STREAMED) ? "2Dstreamed" : "2D",
            pSfx->name,
            iRealEntNum,
            S_ChannelNumToName(iEntChannel)
        );
        return;
    }

    if (iEntNum & S_FLAG_DO_CALLBACK) {
        S_NATIVE_CallbackServer(iRealEntNum, iFreeChannel, pSfx->name);
    }

    pChannel = native.channel[iFreeChannel];
    pChannel->force_free();

    // Changed in the Metal port (A2)
    //  The content volume up to Miles' ceiling; negative = 1
    fVolume           = SNM_Volume(fVolume);
    pChannel->fVolume = fVolume;

    // Fixed in the Metal port (C4)
    //  The new sound's channel decides, not the voice's previous one
    if (iEntChannel == CHAN_LOCAL) {
        pChannel->iFlags |= CHANNEL_FLAG_LOCAL_LISTENER;
    } else {
        pChannel->iFlags &= ~CHANNEL_FLAG_LOCAL_LISTENER;
    }
    pChannel->iFlags &= ~CHANNEL_FLAG_PAUSED;

    if (fMinDistance < 0.0) {
        fMinDistance = 200.0;
    }
    pChannel->fMinDist = fMinDistance;

    if (fMaxDistance < 0.0) {
        fMaxDistance = pChannel->fMinDist * 64;
    }
    pChannel->fMaxDist = fMaxDistance;

    pChannel->fNewPitchMult = fPitch;
    pChannel->pSfx          = pSfx;
    pChannel->iEntNum       = iRealEntNum;
    pChannel->iEntChannel   = iEntChannel;
    if (iRealEntNum == ENTITYNUM_NONE) {
        VectorClear(pChannel->vOrigin);
        pChannel->iFlags |= CHANNEL_FLAG_NO_ENTITY;

        if (vOrigin) {
            VectorCopy(vOrigin, pChannel->vOrigin);
        }
    } else {
        pChannel->iFlags &= ~CHANNEL_FLAG_NO_ENTITY;
        if (vOrigin) {
            VectorCopy(vOrigin, pChannel->vOrigin);

            bSupportWaitTillSoundDone = cl.serverTime - 1 < 0;
            pChannel->iTime           = cl.serverTime - 1;
            if (bSupportWaitTillSoundDone) {
                pChannel->iTime = 0;
            }

            // Fixed in the Metal port (F5)
            //  The entity, not the entity with the callback flag (past the end of s_entity)
            if (s_entity[iRealEntNum].time < pChannel->iTime) {
                // Fixed in OPM
                //  Not sure if it's the real solution, but script_origin entities are usually
                //  never sent to client so the sound will immediately get stopped
                pChannel->iFlags |= CHANNEL_FLAG_MISSING_ENT;
            }
        } else {
            VectorClear(pChannel->vOrigin);
            pChannel->iTime = 0;
        }
    }

    pChannel->iStartTime = cl.serverTime;
    pChannel->iEndTime   = (int)(cl.serverTime + pSfx->time_length + 250.f);

    // Fixed in the Metal port (F6)
    //  The first streamed channel took the other branch
    if (pChannel->iKind == NATIVE_KIND_STREAM) {
        if (!pChannel->set_sfx(pSfx)) {
            Com_DPrintf("sound: Set stream error - %s\n", pSfx->name);
            return;
        }

        pChannel->iBaseRate = pChannel->sample_playback_rate();
        pChannel->set_no_3d();
    } else {
        pChannel->stop();
        pChannel->set_no_3d();
        if (!pChannel->set_sfx(pSfx)) {
            Com_DPrintf("sound: Set sample error - %s\n", pSfx->name);
            return;
        }
    }

    pChannel->set_gain(fVolume);

    // Fixed in the Metal port (D1)
    //  The pitch before the start, not a frame later; a pitch <= 0 keeps the sound's (D4)
    if (pChannel->fNewPitchMult > 0) {
        pChannel->set_sample_playback_rate(pChannel->iBaseRate * pChannel->fNewPitchMult);
    }
    pChannel->fNewPitchMult = 0;

    // Changed in the Metal port
    //  Panned and faded by distance before it starts (OpenAL's started centred at full level until the next
    //  S_Respatialize)
    S_NATIVE_SpatializeChannel(pChannel, native.vListenerOrigin, native.vListenerAxis[1]);
    pChannel->play();

    if (s_show_sounds->integer > 0) {
        Com_DPrintf(
            "sound: 2D - %d (#%i) - %s (vol %f, mindist %f, maxdist %f)\n",
            cl.serverTime,
            iFreeChannel,
            pSfx->name,
            fVolume,
            fMinDistance,
            fMaxDistance
        );
    }
}

/*
==============
S_NATIVE_StartSound
==============
*/
void S_NATIVE_StartSound(
    const vec3_t vOrigin,
    int          iEntNum,
    int          iEntChannel,
    sfxHandle_t  sfxHandle,
    float        fVolume,
    float        fMinDist,
    float        fPitch,
    float        fMaxDist,
    qboolean     bStreamed
)
{
    int             iChannel;
    native_channel *pChannel;
    sfx_info_t     *pSfxInfo;
    sfx_t          *pSfx;
    int             state;
    bool            bOnlyUpdate;
    bool            bSupportWaitTillSoundDone;

    if (!s_bNative) {
        S_OPENAL_StartSound(vOrigin, iEntNum, iEntChannel, sfxHandle, fVolume, fMinDist, fPitch, fMaxDist, bStreamed);
        return;
    }

    native_timer timer;

    bOnlyUpdate = false;
    pSfx        = &s_knownSfx[sfxHandle];
    if (bStreamed) {
        pSfx->iFlags |= SFX_FLAG_STREAMED;
    }

    if (!S_NATIVE_ShouldPlay(pSfx)) {
        Com_DPrintf("sound: ^~^~^ Not playing sound '%s'\n", pSfx->name);
        return;
    }

    if ((pSfx->iFlags & (SFX_FLAG_NO_OFFSET) || pSfx->iFlags & (SFX_FLAG_STREAMED | SFX_FLAG_MP3))
        || iEntChannel == CHAN_MENU || iEntChannel == CHAN_LOCAL) {
        S_NATIVE_Start2DSound(vOrigin, iEntNum, iEntChannel, pSfx, fVolume, fMinDist, fPitch, fMaxDist);
        return;
    }

    bSupportWaitTillSoundDone = (iEntNum & S_FLAG_DO_CALLBACK) != 0;
    iEntNum &= ~S_FLAG_DO_CALLBACK;

    pSfxInfo = &sfx_infos[pSfx->sfx_info_index];
    if (pSfx->iFlags & SFX_FLAG_STREAMED) {
        Com_DPrintf("sound: 3D sounds not supported - couldn't play '%s'\n", pSfx->name);
        return;
    }

    iChannel = S_NATIVE_PickChannel3D(iEntNum, iEntChannel);
    if (iChannel < 0) {
        Com_DPrintf(
            "sound: Couldn't play %s sound '%s' for entity %i on channel %s\n",
            (pSfx->iFlags & SFX_FLAG_STREAMED) ? "3Dstreamed" : "3D",
            pSfx->name,
            iEntNum,
            S_ChannelNumToName(iEntChannel)
        );
        return;
    }

    if (bSupportWaitTillSoundDone) {
        S_NATIVE_CallbackServer(iEntNum, iChannel, pSfx->name);
    }

    pChannel                = native.channel[iChannel];
    pChannel->fNewPitchMult = fPitch;
    pChannel->iEntChannel   = iEntChannel;
    pChannel->iFlags &= ~(CHANNEL_FLAG_PAUSED | CHANNEL_FLAG_LOCAL_LISTENER);
    state = pChannel->sample_status();

    // F3: the same sound of the same entity still playing is only updated, as the original (OpenAL's loop count
    // call then stopped it, E3); an entity-less one always starts (its voices are ENTITYNUM_NONE's since F4, and
    // OpenAL's entity 0 never matched)
    if (iEntNum != ENTITYNUM_NONE && pChannel->iEntNum == iEntNum && state != MACSND_STOPPED && pChannel->pSfx == pSfx) {
        bOnlyUpdate = true;
    } else {
        pChannel->stop();
        pChannel->iFlags &= ~(CHANNEL_FLAG_LOOPING | CHANNEL_FLAG_PAUSED | CHANNEL_FLAG_LOCAL_LISTENER);
        if (!pChannel->set_sfx(pSfx)) {
            Com_DPrintf("sound: Set sample error - %s\n", pSfx->name);
            return;
        }
    }

    if (fMinDist < 0.0) {
        fMinDist = 200.0;
        fMaxDist = 12800.0;
    }
    // Fixed in the Metal port (B2)
    //  A max distance < 0 with a min distance >= 0 never started
    if (fMaxDist < 0.0) {
        fMaxDist = fMinDist * 64;
    }
    pChannel->fMinDist = fMinDist;
    pChannel->fMaxDist = fMaxDist;

    // Fixed in the Metal port (A1, A2)
    //  The 3D one-shot plays at its own volume (OpenAL's set_3d overwrote it with s_volume, so every one played at
    //  one level), up to Miles' ceiling
    fVolume           = SNM_Volume(fVolume);
    pChannel->fVolume = fVolume;
    pChannel->set_gain(pChannel->fVolume);
    pChannel->set_3d();

    if (s_show_sounds->integer > 0) {
        Com_DPrintf(
            "sound: %d (#%i) - %s (vol %f, mindist %f, maxdist %f)\n",
            cl.serverTime,
            iChannel,
            pSfx->name,
            pChannel->fVolume,
            fMinDist,
            fMaxDist
        );
    }

    if (iEntNum == ENTITYNUM_NONE) {
        if (vOrigin) {
            VectorCopy(vOrigin, pChannel->vOrigin);
        } else {
            VectorClear(pChannel->vOrigin);
        }

        pChannel->set_position(pChannel->vOrigin[0], pChannel->vOrigin[1], pChannel->vOrigin[2]);
        pChannel->iFlags |= CHANNEL_FLAG_NO_ENTITY;
        // Fixed in the Metal port (F4)
        //  An entity-less voice was entity 0's for the stops and the picking
        pChannel->iEntNum = ENTITYNUM_NONE;
    } else {
        pChannel->iFlags &= ~CHANNEL_FLAG_NO_ENTITY;
        pChannel->iEntNum = iEntNum;
        if (vOrigin) {
            VectorCopy(vOrigin, pChannel->vOrigin);

            bSupportWaitTillSoundDone = cl.serverTime - 1 < 0;
            pChannel->iTime           = cl.serverTime - 1;
            if (bSupportWaitTillSoundDone) {
                pChannel->iTime = 0;
            }

            if (s_entity[iEntNum].time < pChannel->iTime) {
                // Fixed in OPM
                //  Not sure if it's the real solution, but script_origin entities are usually
                //  never sent to client so the sound will immediately get stopped
                pChannel->iFlags |= CHANNEL_FLAG_MISSING_ENT;
            }
        } else {
            VectorClear(pChannel->vOrigin);
            pChannel->iTime = 0;
        }
        pChannel->set_position(pChannel->vOrigin[0], pChannel->vOrigin[1], pChannel->vOrigin[2]);
    }

    if (pSfxInfo->loop_start != -1) {
        // E2: Miles' loop block: the start, then the block forever
        pChannel->set_sample_loop_block(pSfxInfo->loop_start, pSfxInfo->loop_end);
        pChannel->set_sample_loop_count(0);

        pChannel->iFlags |= CHANNEL_FLAG_LOOPING;
        if (s_show_sounds->integer) {
            Com_DPrintf("sound: loopblock - %d to %d\n", pSfxInfo->loop_start, pSfxInfo->loop_end);
        }
    } else {
        pChannel->set_sample_loop_count(1);
    }

    if (!bOnlyUpdate && S_NATIVE_ShouldStart(pChannel->vOrigin, pChannel->fMinDist, pChannel->fMaxDist)) {
        // Fixed in OPM
        //  Make sure to set the pitch before playing the sound
        // Fixed in the Metal port (D4)
        //  A pitch <= 0 keeps the sound's
        pChannel->iBaseRate = pSfx->info.rate;
        if (pChannel->fNewPitchMult > 0) {
            pChannel->set_sample_playback_rate(pChannel->iBaseRate * pChannel->fNewPitchMult);
        }
        pChannel->fNewPitchMult = 0;

        pChannel->play();
        pChannel->iEndTime   = cl.serverTime + (int)pChannel->pSfx->time_length + 250;
        pChannel->iStartTime = cl.serverTime;
    }
}

/*
==============
S_NATIVE_AddLoopingSound
==============
*/
void S_NATIVE_AddLoopingSound(
    const vec3_t vOrigin,
    const vec3_t vVelocity,
    sfxHandle_t  sfxHandle,
    float        fVolume,
    float        fMinDist,
    float        fMaxDist,
    float        fPitch,
    int          iFlags
)
{
    int                  i;
    int                  iFreeLoopSound;
    sfx_t               *pSfx;
    native_loop_sound_t *pLoopSound;

    if (!s_bNative) {
        S_OPENAL_AddLoopingSound(vOrigin, vVelocity, sfxHandle, fVolume, fMinDist, fMaxDist, fPitch, iFlags);
        return;
    }

    native_timer timer;

    iFreeLoopSound = -1;
    pSfx           = &s_knownSfx[sfxHandle];
    if (!pSfx || !pSfx->length) {
        return;
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_LOOP_SOUNDS; i++) {
        pLoopSound = &native.loop_sounds[i];
        if (pLoopSound->pSfx == pSfx && !pLoopSound->bInUse) {
            iFreeLoopSound = i;
            break;
        }
    }

    if (iFreeLoopSound < 0) {
        for (i = 0; i < MAX_SOUNDSYSTEM_LOOP_SOUNDS; i++) {
            pLoopSound = &native.loop_sounds[i];
            if (!pLoopSound->pSfx && !pLoopSound->bInUse) {
                iFreeLoopSound       = i;
                pLoopSound->bPlaying = false;
                break;
            }
        }
    }

    if (iFreeLoopSound < 0) {
        if (cls.realtime >= s_iNextLoopingWarning) {
            Com_DPrintf("sound: Too many looping sounds\n");
            s_iNextLoopingWarning = cls.realtime + 1000;
        }
        return;
    }

    pLoopSound = &native.loop_sounds[iFreeLoopSound];
    VectorCopy(vOrigin, pLoopSound->vOrigin);
    VectorCopy(vVelocity, pLoopSound->vVelocity);
    pLoopSound->pSfx        = pSfx;
    pLoopSound->bInUse      = true;
    pLoopSound->iStartTime  = cls.realtime;
    pLoopSound->fBaseVolume = fVolume;
    pLoopSound->fMinDist    = fMinDist;
    pLoopSound->fMaxDist    = fMaxDist;
    pLoopSound->fPitch      = fPitch;
    pLoopSound->iFlags      = iFlags;
    pLoopSound->bCombine    = !VectorCompare(vVelocity, vec3_origin);

    if (pLoopSound->bCombine) {
        for (i = 0; i < MAX_SOUNDSYSTEM_LOOP_SOUNDS; i++) {
            if (native.loop_sounds[i].pSfx == pSfx && native.loop_sounds[i].bInUse) {
                pLoopSound->iStartTime = native.loop_sounds[i].iStartTime;
                if (native.loop_sounds[i].bPlaying) {
                    pLoopSound->bPlaying = true;
                    pLoopSound->iChannel = native.loop_sounds[i].iChannel;
                }
            }
        }
    }
}

/*
==============
S_NATIVE_LoopOwns

E5: whether a looping sound's voice is still its own (a start may have taken it): a loop's voice (the decompiled
CHANNEL_FLAG_PAUSED, which every other start clears) playing the loop's sound (combined loops share one)
==============
*/
static bool S_NATIVE_LoopOwns(const native_loop_sound_t *pLoopSound)
{
    const native_channel *pChannel = native.channel[pLoopSound->iChannel];

    return pLoopSound->bPlaying && (pChannel->iFlags & CHANNEL_FLAG_PAUSED) && pChannel->pSfx == pLoopSound->pSfx;
}

/*
==============
S_NATIVE_StopLoopingSound
==============
*/
static void S_NATIVE_StopLoopingSound(native_loop_sound_t *pLoopSound)
{
    bool            bMayStop;
    int             i;
    native_channel *pChannel;

    // Fixed in the Metal port (E5)
    //  The slot is cleared even when it never played (OpenAL's kept its sound, so it could start again for a
    //  frame), and only its own voice is stopped
    if (pLoopSound->bPlaying) {
        bMayStop = true;

        if (pLoopSound->bCombine) {
            for (i = 0; i < MAX_SOUNDSYSTEM_LOOP_SOUNDS; i++) {
                if (native.loop_sounds[i].pSfx == pLoopSound->pSfx && native.loop_sounds[i].bInUse) {
                    bMayStop = false;
                    break;
                }
            }
        }

        if (bMayStop && S_NATIVE_LoopOwns(pLoopSound)) {
            pChannel = native.channel[pLoopSound->iChannel];
            if (s_show_sounds->integer > 0) {
                Com_DPrintf(
                    "sound: %d (#%i) - stopped loop - %s\n", cl.serverTime, pLoopSound->iChannel, pChannel->pSfx->name
                );
            }
            pChannel->force_free();
        }
    }

    pLoopSound->pSfx     = NULL;
    pLoopSound->bPlaying = false;
}

/*
==============
S_NATIVE_ClearLoopingSounds
==============
*/
void S_NATIVE_ClearLoopingSounds()
{
    int i;

    if (!s_bNative) {
        S_OPENAL_ClearLoopingSounds();
        return;
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_LOOP_SOUNDS; i++) {
        native.loop_sounds[i].bInUse = false;
    }
}

/*
==============
S_NATIVE_StopLoopingSounds
==============
*/
static void S_NATIVE_StopLoopingSounds()
{
    int i;

    for (i = 0; i < MAX_SOUNDSYSTEM_LOOP_SOUNDS; i++) {
        native.loop_sounds[i].bInUse = false;
        S_NATIVE_StopLoopingSound(&native.loop_sounds[i]);
    }
}

/*
==============
S_NATIVE_StopSound
==============
*/
void S_NATIVE_StopSound(int iEntNum, int iEntChannel)
{
    int i;

    if (!s_bNative) {
        S_OPENAL_StopSound(iEntNum, iEntChannel);
        return;
    }

    for (i = 0; i < NATIVE_CHANNELS; i++) {
        native_channel *pChannel = native.channel[i];
        if (!S_NATIVE_IsPositionChannel(i)) {
            continue;
        }

        if (!pChannel->is_free() && pChannel->iEntNum == iEntNum && pChannel->iEntChannel == iEntChannel) {
            pChannel->end_sample();
            break;
        }
    }
}

/*
==============
S_NATIVE_StopAllSounds
==============
*/
void S_NATIVE_StopAllSounds(qboolean bStopMusic)
{
    int i;

    if (!s_bNative) {
        S_OPENAL_StopAllSounds(bStopMusic);
        return;
    }

    if (!s_bSoundStarted || !native_initialized) {
        return;
    }

    S_NATIVE_StopLoopingSounds();

    for (i = 0; i < NATIVE_CHANNELS; i++) {
        if (S_NATIVE_IsPositionChannel(i)) {
            native.channel[i]->force_free();
        }
    }

    // the music (bStopMusic): 9d
}

/*
==============
S_NATIVE_LoopSpatialize2D

A 2D looping sound's level and pan (OpenAL's S_OPENAL_UpdateLoopSound, also before its start): centred for NO_PAN,
else panned from s_separation and min / d on top of the inverse square (B5)
==============
*/
static void S_NATIVE_LoopSpatialize2D(
    native_channel            *pChannel,
    const native_loop_sound_t *pLoopSound,
    float                      fVolumeToPlay,
    const vec3_t               vListenerOrigin,
    const vec3_t               vTempAxis,
    const vec3_t               vLoopOrigin
)
{
    vec3_t vOrigin;
    vec3_t vDir;
    float  fVolume;
    float  fMaxVolume;
    float  fDistance;
    int    iPan;

    pChannel->fVolume = fVolumeToPlay;
    fVolume           = pChannel->fVolume;
    fMaxVolume        = fVolume;

    if (pLoopSound->iFlags & LOOPSOUND_FLAG_NO_PAN) {
        // Center the sound
        iPan = 64;
        VectorCopy(vListenerOrigin, pChannel->vSpatial);
    } else {
        VectorCopy(vLoopOrigin, vOrigin);
        iPan = S_NATIVE_SpatializeStereoSound(vListenerOrigin, vTempAxis, vOrigin);
        VectorCopy(vOrigin, pChannel->vSpatial);

        VectorSubtract(vListenerOrigin, vOrigin, vDir);
        // Clamp the volume by distance
        fDistance = VectorLength(vDir);
        if (pChannel->fMinDist >= fDistance) {
            fVolume = fMaxVolume;
        } else {
            fVolume = pChannel->fMinDist / fDistance * fMaxVolume;
        }
    }

    pChannel->set_gain(fVolume);
    pChannel->set_sample_pan(iPan);
}

/*
==============
S_NATIVE_LoopOffset

E6: where a looping sound's voice starts: in phase with when the game first added it, in frames (OpenAL's
offset counted bytes, then its loop count call dropped it)
==============
*/
static int S_NATIVE_LoopOffset(const native_loop_sound_t *pLoopSound)
{
    const sfx_t *pSfx = pLoopSound->pSfx;
    const double frame = (double)pSfx->info.rate * (cls.realtime - pLoopSound->iStartTime) / 1000.0;

    if (pSfx->length <= 0 || !(frame > 0)) {
        return 0;
    }
    return (int)fmod(frame, pSfx->length);
}

/*
==============
S_NATIVE_Start2DLoopSound
==============
*/
static int S_NATIVE_Start2DLoopSound(
    native_loop_sound_t *pLoopSound,
    float                fVolume,
    float                fVolumeToPlay,
    float                fMinDistance,
    const vec3_t         vLoopOrigin,
    const vec3_t         vListenerOrigin,
    const vec3_t         vTempAxis
)
{
    int             iChannel;
    native_channel *pChannel;

    if (pLoopSound->pSfx->iFlags & SFX_FLAG_STREAMED) {
        iChannel = S_NATIVE_PickChannel2DStreamed(ENTITYNUM_NONE, 0);
    } else {
        iChannel = S_NATIVE_PickChannel2D(ENTITYNUM_NONE, 0);
    }

    if (iChannel < 0) {
        Com_DPrintf("sound: Could not find a free 2D sound channel\n");
        return iChannel;
    }

    pChannel = native.channel[iChannel];
    pChannel->force_free();
    pChannel->fVolume = fVolume;

    if (pLoopSound->iFlags & LOOPSOUND_FLAG_NO_PAN) {
        pChannel->iFlags |= CHANNEL_FLAG_LOCAL_LISTENER;
    } else {
        pChannel->iFlags &= ~CHANNEL_FLAG_LOCAL_LISTENER;
    }
    pChannel->fMinDist = fMinDistance;

    if (!pChannel->set_sfx(pLoopSound->pSfx)) {
        Com_DPrintf("sound: Set sample error (loopsound) - %s\n", pLoopSound->pSfx->name);
        pChannel->iFlags &= ~CHANNEL_FLAG_PLAY_DEFERRED;
        return -1;
    }

    pChannel->set_no_3d();
    pChannel->iEntNum     = ENTITYNUM_NONE;
    pChannel->iEntChannel = 0;
    pChannel->pSfx        = pLoopSound->pSfx;
    pChannel->iFlags |= CHANNEL_FLAG_PAUSED | CHANNEL_FLAG_NO_ENTITY;
    pChannel->iBaseRate = pChannel->sample_playback_rate();
    VectorCopy(vLoopOrigin, pChannel->vOrigin);

    pChannel->set_sample_offset(S_NATIVE_LoopOffset(pLoopSound));
    pChannel->set_sample_loop_count(0);
    // Changed in the Metal port
    //  The pitch and the pan before the start (OpenAL's came a frame later)
    if (pLoopSound->fPitch > 0) {
        pChannel->set_sample_playback_rate(pChannel->iBaseRate * pLoopSound->fPitch);
    }
    S_NATIVE_LoopSpatialize2D(pChannel, pLoopSound, fVolumeToPlay, vListenerOrigin, vTempAxis, vLoopOrigin);
    pChannel->start_sample();
    if (s_show_sounds->integer > 0) {
        Com_DPrintf("sound: %d (#%i) - %s (vol %f)\n", cl.serverTime, iChannel, pLoopSound->pSfx->name, fVolume);
    }

    return iChannel;
}

/*
==============
S_NATIVE_Start3DLoopSound
==============
*/
static int S_NATIVE_Start3DLoopSound(
    native_loop_sound_t *pLoopSound,
    float                fVolumeToPlay,
    float                fMinDistance,
    float                fMaxDistance,
    const vec3_t         vLoopOrigin,
    const vec3_t         vListenerOrigin
)
{
    int             iChannel;
    native_channel *pChan3D;

    if (pLoopSound->pSfx->iFlags & SFX_FLAG_STREAMED) {
        return -1;
    }

    if (!pLoopSound->pSfx->data || !pLoopSound->pSfx->length) {
        return -1;
    }

    iChannel = S_NATIVE_PickChannel3D(ENTITYNUM_NONE, 0);
    if (iChannel < 0) {
        Com_DPrintf("sound: Could not find a free channel\n");
        return iChannel;
    }

    pChan3D = native.channel[iChannel];
    pChan3D->force_free();
    pChan3D->iEntNum     = ENTITYNUM_NONE;
    pChan3D->iEntChannel = 0;
    pChan3D->fMinDist    = fMinDistance;
    pChan3D->fMaxDist    = fMaxDistance;
    pChan3D->set_3d();

    if (!pChan3D->set_sfx(pLoopSound->pSfx)) {
        Com_DPrintf("sound: Set sample error - %s\n", pLoopSound->pSfx->name);
        return -1;
    }

    pChan3D->set_position(vLoopOrigin[0], vLoopOrigin[1], vLoopOrigin[2]);
    VectorCopy(vLoopOrigin, pChan3D->vOrigin);
    pChan3D->pSfx = pLoopSound->pSfx;
    pChan3D->iFlags |= CHANNEL_FLAG_PAUSED | CHANNEL_FLAG_NO_ENTITY;
    pChan3D->iBaseRate = pChan3D->sample_playback_rate();

    pChan3D->set_sample_offset(S_NATIVE_LoopOffset(pLoopSound));
    pChan3D->set_sample_loop_count(0);
    if (pLoopSound->fPitch > 0) {
        pChan3D->set_sample_playback_rate(pChan3D->iBaseRate * pLoopSound->fPitch);
    }
    pChan3D->fVolume = fVolumeToPlay;
    pChan3D->set_gain(fVolumeToPlay);
    pChan3D->play();

    return iChannel;
}

/*
==============
S_NATIVE_UpdateLoopSound
==============
*/
static bool S_NATIVE_UpdateLoopSound(
    native_loop_sound_t *pLoopSound,
    float                fVolumeToPlay,
    float                fMinDistance,
    float                fMaxDistance,
    const vec3_t         vListenerOrigin,
    const vec3_t         vTempAxis,
    const vec3_t         vLoopOrigin
)
{
    native_channel *pChannel;

    // Fixed in the Metal port (E5)
    //  Not a voice another sound took
    if (!S_NATIVE_LoopOwns(pLoopSound)) {
        pLoopSound->bPlaying = false;
        return false;
    }

    pChannel             = native.channel[pLoopSound->iChannel];
    pChannel->iStartTime = cl.serverTime;

    if (pLoopSound->pSfx->iFlags & (SFX_FLAG_NO_OFFSET) || pLoopSound->pSfx->iFlags & (SFX_FLAG_STREAMED | SFX_FLAG_MP3)
        || (pLoopSound->iFlags & LOOPSOUND_FLAG_NO_PAN)) {
        S_NATIVE_LoopSpatialize2D(pChannel, pLoopSound, fVolumeToPlay, vListenerOrigin, vTempAxis, vLoopOrigin);
    } else {
        pChannel->set_position(vLoopOrigin[0], vLoopOrigin[1], vLoopOrigin[2]);
        pChannel->fVolume = fVolumeToPlay;
        pChannel->set_gain(fVolumeToPlay);
    }

    return true;
}

/*
==============
S_NATIVE_AddLoopSounds
==============
*/
static void S_NATIVE_AddLoopSounds(const vec3_t vTempAxis)
{
    int                  i, j;
    float                fDistance;
    int                  iChannel;
    vec3_t               vListenerOrigin;
    vec3_t               vLoopOrigin;
    native_loop_sound_t *pLoopSound;
    float                fTotalVolume;
    float                fVolumeToPlay;
    float                fMinDistance, fMaxDistance;
    float                fVolume;
    float                fMaxVolume, fMaxFactor;
    bool                 bAlreadyAdded[MAX_SOUNDSYSTEM_LOOP_SOUNDS] = {false};

    VectorCopy(native.vListenerOrigin, vListenerOrigin);

    for (i = 0; i < MAX_SOUNDSYSTEM_LOOP_SOUNDS; i++) {
        vec3_t vDir;

        if (bAlreadyAdded[i]) {
            continue;
        }

        pLoopSound = &native.loop_sounds[i];
        // Fixed in the Metal port (E5)
        //  Only the looping sounds the game added this frame (OpenAL's could start one it no longer asked for, for
        //  a frame); S_Update stops the others
        if (!pLoopSound->pSfx || !pLoopSound->bInUse) {
            continue;
        }

        fMinDistance = pLoopSound->fMinDist;
        if (fMinDistance < 0) {
            fMinDistance = 200;
        }

        fMaxDistance = pLoopSound->fMaxDist;
        if (fMaxDistance < 0) {
            fMaxDistance = fMinDistance * 64;
        }

        // Changed in the Metal port (A2)
        //  Negative = 1, up to Miles' ceiling
        fVolume = SNM_Volume(pLoopSound->fBaseVolume);
        fVolume = fVolume * s_fAmbientVolume;

        fTotalVolume = 0.0;
        fMaxVolume   = 0.0;

        if (S_NATIVE_LoopOwns(pLoopSound)) {
            native.channel[pLoopSound->iChannel]->fNewPitchMult = pLoopSound->fPitch;
        }

        if (pLoopSound->bCombine) {
            for (j = 0; j < MAX_SOUNDSYSTEM_LOOP_SOUNDS; j++) {
                native_loop_sound_t *pLoopSound2 = &native.loop_sounds[j];

                if (pLoopSound2->pSfx == pLoopSound->pSfx && pLoopSound2->bInUse) {
                    VectorSubtract(pLoopSound2->vOrigin, vListenerOrigin, vDir);
                    VectorCopy(vDir, pLoopSound2->vRelativeOrigin);
                    fDistance = VectorLength(pLoopSound2->vRelativeOrigin);

                    fVolumeToPlay = SNM_LoopLevel(fDistance, fMinDistance, fMaxDistance) * fVolume;
                    // Fixed in the Metal port (E8)
                    //  Each one's weight in the combined direction below: OpenAL's never set it (0, every
                    //  combined loop sat on the listener, without a direction)
                    pLoopSound2->fVolume = fVolumeToPlay;

                    if (fMaxVolume < fVolumeToPlay) {
                        fMaxVolume = fVolumeToPlay;
                    }
                    fTotalVolume += fVolumeToPlay;
                    bAlreadyAdded[j] = true;
                }
            }
        } else {
            VectorSubtract(pLoopSound->vOrigin, vListenerOrigin, vDir);
            VectorCopy(vDir, pLoopSound->vRelativeOrigin);
            fDistance = VectorLength(pLoopSound->vRelativeOrigin);

            fTotalVolume        = SNM_LoopLevel(fDistance, fMinDistance, fMaxDistance) * fVolume;
            pLoopSound->fVolume = fTotalVolume;
            fMaxVolume          = fTotalVolume;
        }

        fMaxFactor = sfx_infos[pLoopSound->pSfx->sfx_info_index].max_factor;
        if (fMaxFactor >= 1 && fMaxVolume * fMaxFactor < fTotalVolume) {
            fTotalVolume = fMaxVolume * fMaxFactor;
        }

        if (fTotalVolume <= 0 && !(pLoopSound->iFlags & LOOPSOUND_FLAG_NO_PAN)) {
            if (pLoopSound->bPlaying) {
                if (S_NATIVE_LoopOwns(pLoopSound)) {
                    if (s_show_sounds->integer > 0) {
                        Com_DPrintf(
                            "sound: %d (#%i) - stopped loop - %s\n",
                            cl.serverTime,
                            pLoopSound->iChannel,
                            native.channel[pLoopSound->iChannel]->pSfx->name
                        );
                    }

                    native.channel[pLoopSound->iChannel]->stop();
                }
                pLoopSound->bPlaying = false;
                if (pLoopSound->bCombine) {
                    for (j = 0; j < MAX_SOUNDSYSTEM_LOOP_SOUNDS; j++) {
                        native_loop_sound_t *pLoopSound2 = &native.loop_sounds[j];

                        if (pLoopSound2->pSfx == pLoopSound->pSfx) {
                            pLoopSound2->bPlaying = false;
                        }
                    }
                }
            }

            continue;
        }

        VectorClear(vLoopOrigin);

        if (pLoopSound->bCombine) {
            for (j = 0; j < MAX_SOUNDSYSTEM_LOOP_SOUNDS; j++) {
                native_loop_sound_t *pLoopSound2 = &native.loop_sounds[j];

                if (pLoopSound2->pSfx == pLoopSound->pSfx && pLoopSound2->bInUse) {
                    VectorNormalize(pLoopSound2->vRelativeOrigin);

                    VectorScale(
                        pLoopSound2->vRelativeOrigin, pLoopSound2->fVolume / fTotalVolume, pLoopSound2->vRelativeOrigin
                    );
                    VectorAdd(pLoopSound2->vRelativeOrigin, vLoopOrigin, vLoopOrigin);
                }
            }

            VectorNormalize(vLoopOrigin);
            VectorMA(vListenerOrigin, fMinDistance * 0.5f, vLoopOrigin, vLoopOrigin);
        } else {
            VectorCopy(pLoopSound->vOrigin, vLoopOrigin);
        }

        // Changed in the Metal port (E5)
        //  A loop whose voice another sound took starts again in the same frame, not the next
        if (pLoopSound->bPlaying
            && S_NATIVE_UpdateLoopSound(
                pLoopSound, fTotalVolume, fMinDistance, fMaxDistance, vListenerOrigin, vTempAxis, vLoopOrigin
            )) {
            continue;
        }

        if (s_show_sounds->integer > 0) {
            Com_DPrintf("sound: %d (#%i) - started loop - %s\n", cl.serverTime, i, pLoopSound->pSfx->name);
        }

        if (pLoopSound->pSfx->iFlags & (SFX_FLAG_NO_OFFSET)
            || pLoopSound->pSfx->iFlags & (SFX_FLAG_STREAMED | SFX_FLAG_MP3)
            || (pLoopSound->iFlags & LOOPSOUND_FLAG_NO_PAN)) {
            iChannel = S_NATIVE_Start2DLoopSound(
                pLoopSound, fVolume, fTotalVolume, fMinDistance, vLoopOrigin, vListenerOrigin, vTempAxis
            );
        } else {
            iChannel = S_NATIVE_Start3DLoopSound(
                pLoopSound, fTotalVolume, fMinDistance, fMaxDistance, vLoopOrigin, vListenerOrigin
            );
        }

        if (iChannel < 0) {
            continue;
        }

        pLoopSound->bPlaying = true;
        pLoopSound->iChannel = iChannel;

        if (pLoopSound->bCombine) {
            for (j = 0; j < MAX_SOUNDSYSTEM_LOOP_SOUNDS; j++) {
                native_loop_sound_t *pLoopSound2 = &native.loop_sounds[j];

                if (pLoopSound2->pSfx == pLoopSound->pSfx && pLoopSound2->bInUse) {
                    pLoopSound2->bPlaying = true;
                    pLoopSound2->iChannel = iChannel;
                }
            }
        }
    }
}

/*
==============
S_NATIVE_SpatializeChannel

A playing voice's place for this frame (OpenAL's S_OPENAL_Respatialize loop body): a 3D voice's position; a 2D
voice's pan from s_separation and its linear falloff (B4). Doppler is gone (D3: the original's velocity scale left
it under 3% of the pitch).
==============
*/
static void S_NATIVE_SpatializeChannel(native_channel *pChannel, const vec3_t vListenerOrigin, const vec3_t vTempAxis)
{
    vec3_t vOrigin;
    vec3_t vDir;
    int    iPan;
    float  fMaxVolume;
    float  fVolume;
    float  fDist;
    bool   b2D;

    // Fixed in the Metal port (F6)
    //  By the voice's kind, not its number (the overflow pool)
    b2D        = pChannel->iKind != NATIVE_KIND_3D;
    fMaxVolume = pChannel->fVolume;
    fVolume    = 1;
    iPan       = 64;

    if (pChannel->iFlags & CHANNEL_FLAG_NO_ENTITY) {
        VectorCopy(pChannel->vOrigin, vOrigin);

        if (pChannel->iFlags & CHANNEL_FLAG_LOCAL_LISTENER) {
            VectorCopy(vListenerOrigin, vOrigin);
            if (b2D) {
                fVolume = fMaxVolume;
                iPan    = 64;
            } else {
                pChannel->set_position(vOrigin[0], vOrigin[1], vOrigin[2]);
            }
        } else {
            if (b2D) {
                iPan = S_NATIVE_SpatializeStereoSound(vListenerOrigin, vTempAxis, vOrigin);
                VectorSubtract(vListenerOrigin, vOrigin, vDir);

                fDist   = VectorLength(vDir);
                fVolume = SNM_Linear2D(fDist, pChannel->fMinDist, pChannel->fMaxDist) * fMaxVolume;
            } else {
                pChannel->set_position(vOrigin[0], vOrigin[1], vOrigin[2]);
            }
        }
    } else if (pChannel->iFlags & CHANNEL_FLAG_LOCAL_LISTENER) {
        VectorCopy(vListenerOrigin, vOrigin);
        if (b2D) {
            fVolume = fMaxVolume;
            iPan    = 64;
        } else {
            pChannel->set_position(vOrigin[0], vOrigin[1], vOrigin[2]);
        }
    } else {
        // Fixed in the Metal port (M)
        //  Within s_entity
        if (pChannel->iEntNum < 0 || pChannel->iEntNum >= MAX_GENTITIES) {
            return;
        }

        if (s_entity[pChannel->iEntNum].time < pChannel->iTime) {
            VectorCopy(pChannel->vOrigin, vOrigin);
            if (!(pChannel->iFlags & CHANNEL_FLAG_LOOPING) && !(pChannel->iFlags & (CHANNEL_FLAG_MISSING_ENT))) {
                pChannel->end_sample();
                return;
            }
        } else {
            pChannel->iFlags &= ~CHANNEL_FLAG_MISSING_ENT;

            VectorCopy(s_entity[pChannel->iEntNum].position, vOrigin);
            VectorCopy(vOrigin, pChannel->vOrigin);
            pChannel->iTime = s_entity[pChannel->iEntNum].time;
        }

        if (s_entity[pChannel->iEntNum].use_listener) {
            VectorCopy(vListenerOrigin, vOrigin);
        }

        if (pChannel->iEntNum == s_iListenerNumber) {
            if (vListenerOrigin[0] == vOrigin[0] && vListenerOrigin[2] == vOrigin[2]) {
                float fDelta = vListenerOrigin[1] - vOrigin[1];

                if (fDelta > 89.9f && fDelta < 90.09f) {
                    VectorCopy(vListenerOrigin, vOrigin);
                }
            }
        }

        if (b2D) {
            iPan = S_NATIVE_SpatializeStereoSound(vListenerOrigin, vTempAxis, vOrigin);
            VectorSubtract(vListenerOrigin, vOrigin, vDir);
            fDist   = VectorLength(vDir);
            fVolume = SNM_Linear2D(fDist, pChannel->fMinDist, pChannel->fMaxDist) * fMaxVolume;
        } else {
            pChannel->set_position(vOrigin[0], vOrigin[1], vOrigin[2]);
        }
    }

    if (b2D) {
        VectorCopy(vOrigin, pChannel->vSpatial);
        pChannel->set_gain(fVolume);
        pChannel->set_sample_pan(iPan);
    }
}

/*
==============
S_NATIVE_PostAll

The playing voices' gains and rates to the mixer, those that changed
==============
*/
static void S_NATIVE_PostAll()
{
    int i;

    for (i = 0; i < NATIVE_CHANNELS; i++) {
        if (S_NATIVE_IsPositionChannel(i)) {
            native.channel[i]->post();
        }
    }
}

/*
==============
S_NATIVE_Respatialize
==============
*/
void S_NATIVE_Respatialize(int iEntNum, const vec3_t vHeadPos, const vec3_t vAxis[3])
{
    int             i;
    native_channel *pChannel;

    if (!s_bNative) {
        S_OPENAL_Respatialize(iEntNum, vHeadPos, vAxis);
        return;
    }

    if (cls.no_menus) {
        return;
    }

    native_timer timer;

    s_iListenerNumber = iEntNum;

    VectorCopy(vHeadPos, native.vListenerOrigin);
    AxisCopy(vAxis, native.vListenerAxis);

    for (i = 0; i < NATIVE_CHANNELS; i++) {
        if (!S_NATIVE_IsPositionChannel(i)) {
            continue;
        }
        pChannel = native.channel[i];

        if (!pChannel->is_playing()) {
            continue;
        }

        if (pChannel->iFlags & CHANNEL_FLAG_PAUSED) {
            continue;
        }

        S_NATIVE_SpatializeChannel(pChannel, native.vListenerOrigin, native.vListenerAxis[1]);
    }

    S_NATIVE_AddLoopSounds(native.vListenerAxis[1]);

    // Added in the Metal port
    //  Out now rather than at S_Update, after the frame is drawn
    S_NATIVE_PostAll();
}

/*
==============
S_NATIVE_SpatializeStereoSound
==============
*/
static int S_NATIVE_SpatializeStereoSound(const vec3_t listener_origin, const vec3_t listener_left, const vec3_t origin)
{
    vec3_t source_vec;
    float  pan;

    VectorSubtract(origin, listener_origin, source_vec);
    VectorNormalize(source_vec);

    pan = s_separation->value + (1.f - s_separation->value) * -DotProduct(listener_left, source_vec);

    if (pan < 0) {
        pan = 0;
    }

    return ceilf(pan * 127);
}

/*
==============
S_NATIVE_SetReverb

K: the original's EAX rooms are parked
==============
*/
void S_NATIVE_SetReverb(int iType, float fLevel)
{
    if (!s_bNative) {
        S_OPENAL_SetReverb(iType, fLevel);
        return;
    }

    s_fReverbLevel = fLevel;
    s_iReverbType  = iType;
}

/*
==============
S_NATIVE_Probe

Start -> output: from the game's start to the host time the voice's first frame leaves the device, plus the
device's latency to the jack
==============
*/
static void S_NATIVE_Probe()
{
    macSndStats_t stats;
    int           i;
    bool          bStats = false;

    for (i = 0; i < NATIVE_CHANNELS; i++) {
        native_channel *pChannel = native.channel[i];
        uint64_t        t;

        if (!pChannel->iRequestTime) {
            continue;
        }
        t = MacSnd_StartTime(pChannel->iVoice);
        if (!t) {
            continue;
        }
        if (!bStats) {
            MacSnd_GetStats(&stats);
            native.latency = stats.latency;
            bStats         = true;
        }
        if (t > pChannel->iRequestTime) {
            native.probeMs[native.numProbes % NATIVE_PROBES] =
                S_NATIVE_TicksToMs(t - pChannel->iRequestTime) + native.latency * 1000.0;
            native.numProbes++;
        }
        pChannel->iRequestTime = 0;
    }
}

/*
==============
S_NATIVE_UpdateCapture

s_capture <name>: the final mix to <name>.wav in the home folder (32-bit float stereo at the device's rate),
s_captureMute 1: the device silent meanwhile; "" ends it
==============
*/
static void S_NATIVE_UpdateCapture()
{
    int frames;

    if (s_capture->modified || s_captureMute->modified) {
        s_capture->modified     = qfalse;
        s_captureMute->modified = qfalse;
        S_NATIVE_StopCapture();

        if (s_capture->string[0]) {
            macSndStats_t stats;
            char          name[MAX_QPATH];
            int           header[11];

            MacSnd_GetStats(&stats);
            Com_sprintf(name, sizeof(name), "%s.wav", s_capture->string);
            native.capture = FS_FOpenFileWrite_HomeData(name);
            if (native.capture) {
                memcpy(&header[0], "RIFF", 4);
                header[1] = 36;
                memcpy(&header[2], "WAVE", 4);
                memcpy(&header[3], "fmt ", 4);
                header[4]  = 16;
                header[5]  = 3 | (2 << 16); // IEEE float, 2 channels
                header[6]  = (int)stats.rate;
                header[7]  = (int)stats.rate * 8;
                header[8]  = 8 | (32 << 16); // 8 bytes a frame, 32 bits
                memcpy(&header[9], "data", 4);
                header[10] = 0;
                FS_Write(header, sizeof(header), native.capture);
                native.captureFrames = 0;
                MacSnd_Capture(NATIVE_CAPTURE_FRAMES, s_captureMute->integer);
                Com_Printf("sound: capturing the mix to %s\n", name);
            }
        }
    }

    if (!native.capture) {
        return;
    }

    while ((frames = MacSnd_ReadCapture(native.captureBuf, 4096)) > 0) {
        FS_Write(native.captureBuf, frames * 8, native.capture);
        native.captureFrames += frames;
    }
}

/*
==============
S_NATIVE_Update
==============
*/
void S_NATIVE_Update()
{
    int             i;
    native_channel *pChannel;
    float           fMaster;

    if (!s_bNative) {
        S_OPENAL_Update();
        return;
    }

    native_timer timer;

    if (cl.snap.ps.stats[STAT_CINEMATIC]) {
        S_SetGlobalAmbientVolumeLevel(0.5f);
    } else {
        S_SetGlobalAmbientVolumeLevel(1.f);
    }

    if (paused->integer && !s_bSoundPaused) {
        S_NATIVE_PauseSound();
    } else if (!paused->integer && s_bSoundPaused) {
        S_NATIVE_UnpauseSound();
    }

    if (s_bFading) {
        s_fFadeVolume = 1.f - (cls.realtime - s_fFadeStartTime) / (s_fFadeStopTime - s_fFadeStartTime);
        if (s_fFadeVolume < 0) {
            s_fFadeVolume = 0;
        }
    }

    if (s_volume->modified) {
        if (s_volume->value > 1) {
            Cvar_Set("s_volume", "1.0");
        } else if (s_volume->value < 0) {
            Cvar_Set("s_volume", "0.0");
        }

        s_volume->modified = 0;
    }

    // Changed in the Metal port (A4)
    //  s_volume and the fade on the master: they reach the voices already playing
    fMaster = S_NATIVE_MasterVolume();
    if (fMaster != native.fMaster) {
        native.fMaster = fMaster;
        MacSnd_SetMaster(fMaster);
    }

    for (i = 0; i < NATIVE_CHANNELS; i++) {
        pChannel = native.channel[i];
        if (!S_NATIVE_IsPositionChannel(i)) {
            continue;
        }

        if (!pChannel->is_playing()) {
            continue;
        }

        if (pChannel->fNewPitchMult <= 0) {
            continue;
        }

        pChannel->set_sample_playback_rate(pChannel->iBaseRate * pChannel->fNewPitchMult);
        pChannel->fNewPitchMult = 0;
    }

    if (s_speaker_type->modified) {
        s_speaker_type->modified = qfalse;
        Com_Printf("sound: s_speaker_type plays stereo on the native driver\n");
    }

    if (s_show_num_active_sounds->integer == 1) {
        int num = 0;

        for (i = 0; i < NATIVE_CHANNELS; i++) {
            pChannel = native.channel[i];
            if (S_NATIVE_IsPositionChannel(i) && pChannel->is_playing()) {
                ++num;
            }
        }
        Com_DPrintf("sound: Number of active sounds = %d\n", num);
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_LOOP_SOUNDS; i++) {
        if (!native.loop_sounds[i].bInUse) {
            S_NATIVE_StopLoopingSound(&native.loop_sounds[i]);
        }
    }

    S_NATIVE_PostAll();
    MacSnd_Update();

    S_NATIVE_Probe();
    S_NATIVE_UpdateCapture();
    if (!s_eventLog->integer && native.eventLog) {
        FS_FCloseFile(native.eventLog);
        native.eventLog = 0;
    }

    native.frames++;
}

/*
==============
S_NATIVE_GetMusicFilename

The triggered music: 9d
==============
*/
const char *S_NATIVE_GetMusicFilename()
{
    if (!s_bNative) {
        return S_OPENAL_GetMusicFilename();
    }

    return "";
}

/*
==============
S_NATIVE_GetMusicLoopCount
==============
*/
int S_NATIVE_GetMusicLoopCount()
{
    if (!s_bNative) {
        return S_OPENAL_GetMusicLoopCount();
    }

    return 0;
}

/*
==============
S_NATIVE_GetMusicOffset
==============
*/
unsigned int S_NATIVE_GetMusicOffset()
{
    if (!s_bNative) {
        return S_OPENAL_GetMusicOffset();
    }

    return 0;
}

/*
==============
S_IsSoundPlaying
==============
*/
qboolean S_IsSoundPlaying(int channel_number, const char *sfxName)
{
    native_channel *pChannel;

    if (!s_bNative) {
        // the overflow pool's numbers aren't OpenAL's
        if (channel_number < 0 || channel_number >= MAX_SOUNDSYSTEM_CHANNELS) {
            return false;
        }
        return S_OPENAL_IsSoundPlaying(channel_number, sfxName);
    }

    if (!native_initialized || channel_number < 0 || channel_number >= NATIVE_CHANNELS) {
        return false;
    }

    pChannel = native.channel[channel_number];

    if (s_bSoundPaused) {
        return false;
    }

    if (!pChannel->is_playing()) {
        return false;
    }

    if (!pChannel->pSfx) {
        return false;
    }

    if (strcmp(sfxName, pChannel->pSfx->name)) {
        return false;
    }

    return true;
}

/*
==============
S_StoreBase
==============
*/
static void S_StoreBase(channelbasesavegame_t *pBase, native_channel *pChannel)
{
    if (!pChannel) {
        return;
    }

    // Changed in the Metal port (I)
    //  A looping sound's voice isn't saved: the game adds the loop again and it starts again
    if (pChannel->iEntChannel == CHAN_MENU || pChannel->is_free() || (pChannel->iFlags & CHANNEL_FLAG_PAUSED)) {
        pBase->bPlaying      = false;
        pBase->iOffset       = 0;
        pBase->iLoopCount    = 0;
        pBase->sfx.szName[0] = 0;
        pBase->sfx.iFlags    = 0;
        pBase->fNewPitchMult = 1.f;
        pBase->iBaseRate     = pChannel->iBaseRate;
        pBase->iStatus       = 0;
    } else {
        pBase->bPlaying   = true;
        pBase->iOffset    = pChannel->sample_offset();
        pBase->iLoopCount = pChannel->sample_loop_count();
        memcpy(pBase->sfx.szName, pChannel->pSfx->name, sizeof(pBase->sfx.szName));
        pBase->sfx.iFlags = pChannel->pSfx->iFlags;
        pBase->iBaseRate  = pChannel->iBaseRate;
        pBase->iStatus    = 0;
        // Fixed in the Metal port (D5)
        //  The played rate over the base rate (OpenAL's was inverted)
        pBase->fNewPitchMult = pChannel->iBaseRate ? (float)pChannel->sample_playback_rate() / pChannel->iBaseRate : 1;
    }

    pBase->iStartTime                = pChannel->iStartTime - cl.serverTime;
    pBase->iEndTime                  = pChannel->iEndTime - cl.serverTime;
    pBase->iEntChannel               = pChannel->iEntChannel;
    pBase->iEntNum                   = pChannel->iEntNum;
    pBase->iFlags                    = pChannel->iFlags;
    pBase->fMaxDist                  = pChannel->fMaxDist;
    pBase->fMinDist                  = pChannel->fMinDist;
    pBase->iNextCheckObstructionTime = 0;
    VectorCopy(pChannel->vOrigin, pBase->vOrigin);
    pBase->iTime   = pChannel->iTime - cl.serverTime;
    pBase->fVolume = pChannel->fVolume;
}

/*
==============
S_StartSoundFromBase
==============
*/
static void S_StartSoundFromBase(channelbasesavegame_t *pBase, native_channel *pChannel, sfx_t *pSfx, bool bStartUnpaused)
{
    if (!pChannel->set_sfx(pSfx)) {
        Com_DPrintf("sound: Set sample error - %s\n", pSfx->name);
        pChannel->iFlags &= ~CHANNEL_FLAG_PLAY_DEFERRED;
        return;
    }

    pChannel->iBaseRate = pChannel->sample_playback_rate();

    // Fixed in the Metal port (I)
    //  The voice's mode again: 3D, or 2D
    if (pChannel->iKind == NATIVE_KIND_3D) {
        pChannel->set_3d();
        pChannel->set_position(pChannel->vOrigin[0], pChannel->vOrigin[1], pChannel->vOrigin[2]);
    } else {
        pChannel->set_no_3d();
    }

    if (sfx_infos[pSfx->sfx_info_index].loop_start != -1) {
        pChannel->set_sample_loop_block(
            sfx_infos[pSfx->sfx_info_index].loop_start, sfx_infos[pSfx->sfx_info_index].loop_end
        );
        // Fixed in the Metal port (E7)
        //  A looping sound loops again (OpenAL's played once)
        pChannel->set_sample_loop_count(0);
        pChannel->iFlags |= CHANNEL_FLAG_LOOPING;
        if (s_show_sounds->integer > 0) {
            Com_DPrintf(
                "sound: loopblock - %d to %d\n",
                sfx_infos[pSfx->sfx_info_index].loop_start,
                sfx_infos[pSfx->sfx_info_index].loop_end
            );
        }
    } else {
        pChannel->set_sample_loop_count(1);
    }

    pChannel->set_gain(pChannel->fVolume);
    pChannel->set_sample_offset(pBase->iOffset);
    if (pBase->fNewPitchMult > 0) {
        pChannel->set_sample_playback_rate(pChannel->iBaseRate * pBase->fNewPitchMult);
    }

    if (bStartUnpaused) {
        pChannel->resume_sample();
    } else {
        pChannel->iFlags |= CHANNEL_FLAG_PLAY_DEFERRED;
    }

    // Fixed in the Metal port (I)
    //  An entity-less voice doesn't move entity 0
    if (pChannel->iEntNum >= 0 && pChannel->iEntNum < MAX_GENTITIES && !(pChannel->iFlags & CHANNEL_FLAG_NO_ENTITY)) {
        s_entity[pChannel->iEntNum].time = pChannel->iTime;
        VectorCopy(pChannel->vOrigin, s_entity[pChannel->iEntNum].position);
    }
}

/*
==============
S_LoadBase
==============
*/
static void S_LoadBase(channelbasesavegame_t *pBase, native_channel *pChannel, bool bStartUnpaused)
{
    sfxHandle_t handle;

    if (!pChannel) {
        return;
    }

    if (!pBase->bPlaying) {
        return;
    }

    if (strstr(pBase->sfx.szName, "null.wav")) {
        return;
    }

    handle = S_RegisterSound(pBase->sfx.szName, (pBase->sfx.iFlags & SFX_FLAG_STREAMED), false);
    // Fixed in the Metal port (I)
    //  A sound that couldn't be registered is skipped
    if (!handle) {
        return;
    }

    pChannel->iBaseRate     = pBase->iBaseRate;
    pChannel->iStartTime    = pBase->iStartTime;
    pChannel->iEndTime      = pBase->iEndTime;
    pChannel->iEntChannel   = pBase->iEntChannel;
    pChannel->iEntNum       = pBase->iEntNum;
    pChannel->iFlags        = pBase->iFlags;
    pChannel->fMaxDist      = pBase->fMaxDist;
    pChannel->fMinDist      = pBase->fMinDist;
    pChannel->fNewPitchMult = pBase->fNewPitchMult;
    VectorCopy(pBase->vOrigin, pChannel->vOrigin);
    pChannel->iTime   = pBase->iTime;
    pChannel->fVolume = pBase->fVolume;
    pChannel->pSfx    = &s_knownSfx[handle];

    S_StartSoundFromBase(pBase, pChannel, &s_knownSfx[handle], bStartUnpaused);
}

/*
==============
S_SaveData
==============
*/
void S_SaveData(soundsystemsavegame_t *pSave)
{
    int  i;
    bool bSoundWasUnpaused;

    if (!s_bNative) {
        S_OPENAL_SaveData(pSave);
        return;
    }

    if (!native_initialized) {
        return;
    }

    bSoundWasUnpaused = !s_bSoundPaused;
    if (!s_bSoundPaused) {
        S_NATIVE_PauseSound();
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_POSITION_CHANNELS; i++) {
        S_StoreBase(&pSave->Channels[i], native.channel[i]);
    }

    if (bSoundWasUnpaused) {
        S_NATIVE_UnpauseSound();
    }
}

/*
==============
S_ReLoad
==============
*/
void S_ReLoad(soundsystemsavegame_t *pSave)
{
    int  i;
    bool bSoundWasUnpaused;

    if (!s_bNative) {
        S_OPENAL_ReLoad(pSave);
        return;
    }

    if (!native_initialized) {
        return;
    }

    bSoundWasUnpaused = !s_bSoundPaused;
    if (!s_bSoundPaused) {
        S_NATIVE_PauseSound();
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_POSITION_CHANNELS; i++) {
        S_LoadBase(&pSave->Channels[i], native.channel[i], bSoundWasUnpaused);
    }

    if (bSoundWasUnpaused) {
        S_NATIVE_UnpauseSound();
    }
}

/*
==============
S_InitBase
==============
*/
static void S_InitBase(channelbasesavegame_t *pBase)
{
    if (!pBase->bPlaying) {
        return;
    }

    if (strstr(pBase->sfx.szName, "null.wav")) {
        return;
    }

    SV_AddSvsTimeFixup(&pBase->iStartTime);
    SV_AddSvsTimeFixup(&pBase->iEndTime);
    SV_AddSvsTimeFixup(&pBase->iTime);
}

/*
==============
S_LoadData
==============
*/
void S_LoadData(soundsystemsavegame_t *pSave)
{
    int i;

    if (!s_bNative) {
        S_OPENAL_LoadData(pSave);
        return;
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_POSITION_CHANNELS; i++) {
        S_InitBase(&pSave->Channels[i]);
    }
}

/*
==============
The music, the triggered music, the movies' audio: 9d. Until then the state the client saves and restores
around snd_restart (cl_main.cpp) is kept, and nothing plays.
==============
*/
void MUSIC_NewSoundtrack(const char *name)
{
    if (!s_bNative) {
        S_OPENAL_MUSIC_NewSoundtrack(name);
        return;
    }

    Q_strncpyz(current_soundtrack, name, sizeof(current_soundtrack));
}

void MUSIC_UpdateMood(int current, int fallback)
{
    if (!s_bNative) {
        S_OPENAL_MUSIC_UpdateMood(current, fallback);
        return;
    }
}

void MUSIC_UpdateVolume(float volume, float fade_time)
{
    if (!s_bNative) {
        S_OPENAL_MUSIC_UpdateVolume(volume, fade_time);
        return;
    }
}

const char *S_CurrentSoundtrack()
{
    if (!s_bNative) {
        return S_OPENAL_CurrentSoundtrack();
    }

    return current_soundtrack;
}

void S_TriggeredMusic_SetupHandle(const char *pszName, int iLoopCount, int iOffset, qboolean autostart)
{
    if (!s_bNative) {
        S_OPENAL_TriggeredMusic_SetupHandle(pszName, iLoopCount, iOffset, autostart);
        return;
    }
}

void S_TriggeredMusic_Stop()
{
    if (!s_bNative) {
        S_OPENAL_TriggeredMusic_Stop();
        return;
    }
}

void S_TriggeredMusic_PlayIntroMusic()
{
    if (!s_bNative) {
        S_OPENAL_TriggeredMusic_PlayIntroMusic();
        return;
    }
}

void S_StopMovieAudio()
{
    if (!s_bNative) {
        S_OPENAL_StopMovieAudio();
        return;
    }
}

void S_SetupMovieAudio(const char *pszMovieName)
{
    if (!s_bNative) {
        S_OPENAL_SetupMovieAudio(pszMovieName);
        return;
    }
}

int S_CurrentMoviePosition()
{
    if (!s_bNative) {
        return S_OPENAL_CurrentMoviePosition();
    }

    return 0;
}

/*
==============
S_NATIVE_LogStart

s_eventLog 1: every start to sound_events.log in the home folder, with what its gains come from (the 9c check:
scripts recompute them from the formulas, and compare them with a scratch-instrumented OpenAL build's)
==============
*/
static void S_NATIVE_LogStart(native_channel *pChannel, float fLeft, float fRight)
{
    const char *kind = pChannel->iKind == NATIVE_KIND_3D ? "3d" : pChannel->iKind == NATIVE_KIND_STREAM ? "stream" : "2d";
    const float *pos = pChannel->b3D ? pChannel->vPosition : pChannel->vSpatial;

    if (!native.eventLog) {
        native.eventLog = FS_FOpenFileWrite_HomeData("sound_events.log");
        if (!native.eventLog) {
            Cvar_Set("s_eventLog", "0");
            return;
        }
        FS_Printf(native.eventLog, "driver native sep %.9g\n", s_separation->value);
    }

    FS_Printf(
        native.eventLog,
        "start t %d st %d v %d k %s loop %d ent %d ch %d sfx %s vol %.9g min %.9g max %.9g rate %.9g base %d "
        "pos %.9g %.9g %.9g lis %.9g %.9g %.9g fwd %.9g %.9g %.9g left %.9g %.9g %.9g pan %d gain %.9g "
        "L %.9g R %.9g master %.9g 3d %d stereo %d\n",
        cls.realtime,
        cl.serverTime,
        pChannel->iVoice,
        kind,
        (pChannel->iFlags & CHANNEL_FLAG_PAUSED) != 0,
        pChannel->iEntNum,
        pChannel->iEntChannel,
        pChannel->pSfx ? pChannel->pSfx->name : "?",
        pChannel->fVolume,
        pChannel->fMinDist,
        pChannel->fMaxDist,
        pChannel->fRate,
        pChannel->iBaseRate,
        pos[0],
        pos[1],
        pos[2],
        native.vListenerOrigin[0],
        native.vListenerOrigin[1],
        native.vListenerOrigin[2],
        native.vListenerAxis[0][0],
        native.vListenerAxis[0][1],
        native.vListenerAxis[0][2],
        native.vListenerAxis[1][0],
        native.vListenerAxis[1][1],
        native.vListenerAxis[1][2],
        pChannel->iPan,
        pChannel->fGain,
        fLeft,
        fRight,
        native.fMaster,
        pChannel->b3D,
        pChannel->pSfx && pChannel->pSfx->info.channels == 2
    );
}

/*
==============================================================================

native_channel: snd_openal_new.cpp's channel methods (Miles' sample API) on a mixer voice

==============================================================================
*/

/*
==============
native_channel::gains

The voice's left and right gains now (snd_native_math.h): a 3D voice by the Fast 2D model from the listener
(B1, C6), a 2D voice by its pan (C1; a stereo sound's pan a balance) and the 2D scale (A3)
==============
*/
void native_channel::gains(float *left, float *right)
{
    float l, r, g;

    if (b3D) {
        vec3_t dir;
        float  d;

        VectorSubtract(vPosition, native.vListenerOrigin, dir);
        d = VectorNormalize(dir);
        g = fGain * SNM_Distance(d, fMinDist, fMaxDist);
        SNM_Pan3D(DotProduct(dir, native.vListenerAxis[0]), DotProduct(dir, native.vListenerAxis[1]), &l, &r);
    } else if (pSfx && pSfx->info.channels == 2) {
        g = fGain * SNM_SCALE_2D;
        SNM_Balance2D(iPan, &l, &r);
    } else {
        g = fGain * SNM_SCALE_2D;
        SNM_Pan2D(iPan, &l, &r);
    }

    *left  = g * l;
    *right = g * r;
}

/*
==============
native_channel::post
==============
*/
void native_channel::post()
{
    float l, r;

    if (MacSnd_Status(iVoice) == MACSND_STOPPED) {
        return;
    }

    gains(&l, &r);
    if (l != fPostedL || r != fPostedR || fRate != fPostedRate) {
        MacSnd_SetParams(iVoice, l, r, fRate);
        fPostedL    = l;
        fPostedR    = r;
        fPostedRate = fRate;
    }
}

/*
==============
native_channel::play

Resumes a paused voice, else starts it (a playing one again from the start, as alSourcePlay)
==============
*/
void native_channel::play()
{
    macSndStart_t start;
    float         l, r;

    if (MacSnd_Status(iVoice) == MACSND_PAUSED) {
        MacSnd_Pause(iVoice, 0);
        return;
    }

    if (!iSound) {
        return;
    }

    gains(&l, &r);
    memset(&start, 0, sizeof(start));
    start.sound      = iSound;
    start.gainL      = l;
    start.gainR      = r;
    start.rate       = fRate;
    start.startFrame = iOffset;
    start.loopStart  = iLoopStart;
    start.loopEnd    = iLoopEnd;
    start.loopCount  = iLoopCount;
    MacSnd_Start(iVoice, &start);

    fPostedL     = l;
    fPostedR     = r;
    fPostedRate  = fRate;
    iOffset      = 0;
    iRequestTime = mach_absolute_time();

    if (s_eventLog->integer) {
        S_NATIVE_LogStart(this, l, r);
    }
}

/*
==============
native_channel::stop
==============
*/
void native_channel::stop()
{
    MacSnd_Stop(iVoice);
}

/*
==============
native_channel::pause
==============
*/
void native_channel::pause()
{
    if (MacSnd_Status(iVoice) == MACSND_PLAYING) {
        MacSnd_Pause(iVoice, 1);
    }
}

/*
==============
native_channel::set_no_3d
==============
*/
void native_channel::set_no_3d()
{
    b3D        = false;
    iPan       = 64;
    iLoopCount = 1;
    fGain      = 1;
}

/*
==============
native_channel::set_3d

Fixed in the Metal port (A1): the gain is the caller's (OpenAL's reset it to s_volume)
==============
*/
void native_channel::set_3d()
{
    b3D        = true;
    iLoopCount = 1;
}

/*
==============
native_channel::set_gain
==============
*/
void native_channel::set_gain(float gain)
{
    fGain = gain;
}

/*
==============
native_channel::set_position
==============
*/
void native_channel::set_position(float v0, float v1, float v2)
{
    VectorSet(vPosition, v0, v1, v2);
}

/*
==============
native_channel::is_free
==============
*/
bool native_channel::is_free()
{
    return MacSnd_Status(iVoice) == MACSND_STOPPED;
}

/*
==============
native_channel::is_paused
==============
*/
bool native_channel::is_paused()
{
    return MacSnd_Status(iVoice) == MACSND_PAUSED;
}

/*
==============
native_channel::is_playing
==============
*/
bool native_channel::is_playing()
{
    return MacSnd_Status(iVoice) == MACSND_PLAYING;
}

/*
==============
native_channel::force_free
==============
*/
void native_channel::force_free()
{
    stop();
    iFlags = 0;
}

/*
==============
native_channel::set_sfx
==============
*/
bool native_channel::set_sfx(sfx_t *sfx)
{
    stop();

    pSfx       = sfx;
    iOffset    = 0;
    iLoopStart = 0;
    iLoopEnd   = 0;
    iLoopCount = 1;

    if (iKind != NATIVE_KIND_3D && iKind != NATIVE_KIND_2D) {
        // the streams: 9d
        iSound = 0;
        return false;
    }

    // made when it was loaded (S_NATIVE_SfxLoaded): none means it couldn't be
    iSound = pSfx->buffer;
    if (!iSound) {
        return false;
    }

    iBaseRate = (int)pSfx->info.rate;
    fRate     = iBaseRate;

    return true;
}

/*
==============
native_channel::start_sample
==============
*/
void native_channel::start_sample()
{
    play();
}

/*
==============
native_channel::resume_sample
==============
*/
void native_channel::resume_sample()
{
    play();
}

/*
==============
native_channel::end_sample
==============
*/
void native_channel::end_sample()
{
    stop();
}

/*
==============
native_channel::set_sample_pan
==============
*/
void native_channel::set_sample_pan(int pan)
{
    iPan = pan;
}

/*
==============
native_channel::set_sample_playback_rate

D4: a rate <= 0 keeps the one it has
==============
*/
void native_channel::set_sample_playback_rate(int rate)
{
    if (rate > 0) {
        fRate = rate;
    }
}

/*
==============
native_channel::sample_playback_rate
==============
*/
int native_channel::sample_playback_rate()
{
    return (int)fRate;
}

/*
==============
native_channel::sample_offset
==============
*/
unsigned int native_channel::sample_offset()
{
    if (MacSnd_Status(iVoice) == MACSND_STOPPED) {
        return iOffset;
    }

    return MacSnd_Position(iVoice);
}

/*
==============
native_channel::sample_loop_count
==============
*/
unsigned int native_channel::sample_loop_count()
{
    return iLoopCount;
}

/*
==============
native_channel::set_sample_offset
==============
*/
void native_channel::set_sample_offset(unsigned int offset)
{
    if (MacSnd_Status(iVoice) == MACSND_STOPPED) {
        iOffset = offset;
    } else {
        MacSnd_Seek(iVoice, offset);
    }
}

/*
==============
native_channel::set_sample_loop_count

Fixed in the Metal port (E3): only the count changes (OpenAL's stopped the voice)
==============
*/
void native_channel::set_sample_loop_count(int count)
{
    iLoopCount = count;
    if (MacSnd_Status(iVoice) != MACSND_STOPPED) {
        MacSnd_SetLoop(iVoice, iLoopStart, iLoopEnd, iLoopCount);
    }
}

/*
==============
native_channel::set_sample_loop_block

E2: Miles' loop block in frames (the songs' is 63 ms; global/sound*.txt's values as they are, the unit open); an end
<= 0 is the sound's end
==============
*/
void native_channel::set_sample_loop_block(int start_offset, int end_offset)
{
    iLoopStart = start_offset > 0 ? start_offset : 0;
    iLoopEnd   = end_offset > 0 ? end_offset : 0;
    if (MacSnd_Status(iVoice) != MACSND_STOPPED) {
        MacSnd_SetLoop(iVoice, iLoopStart, iLoopEnd, iLoopCount);
    }
}

/*
==============
native_channel::sample_status
==============
*/
int native_channel::sample_status()
{
    return MacSnd_Status(iVoice);
}
