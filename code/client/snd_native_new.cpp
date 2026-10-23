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
// as OpenMoHAA reconstructed it in upstream's snd_openal_new.cpp (the channels, their picking and stealing, the looping
// sounds, the spatialization), on our own mixer (code/macos/macos_snd.c) instead of OpenAL, with the parity checklist's
// fixes (the plan's Phase 9 "Session 57 result"; the ids in the comments are its items). The original's math is in
// snd_native_math.h: the voices' levels without s_volume, which is the master gain's.
//
// The only driver since 9g, which removed OpenAL's with openal-soft (the sndref flavor keeps both). The sound effects
// are 9c's; the streamed sounds, the music (moods, crossfades), the triggered music, the movies' audio and playmp3
// 9d's: a stream is read ahead into a mixer ring a little each frame, a short one decoded whole and looped by the
// mixer. Headphone 3D (9f, s_speaker_type 1): the 3D voices go to the mixer with a direction, for Apple's HRTF, in
// place of the pan.
//
// A channel is a mixer voice, the same number. The policy code below keeps snd_openal_new.cpp's shape (the
// channel's Miles-named methods), so the two compare line by line: a start sends everything at once (MacSnd_Start),
// the gains and rates of the playing voices go out after S_Respatialize and in S_Update, only when they changed.

#include "snd_local.h"
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

// G1: a stream is read ahead into a mixer ring a little each frame on the main thread (the codecs use the zone and
// the filesystem); a short one is decoded whole and looped by the mixer
#define NATIVE_STREAM_SECONDS  0.5f // a ring's length
#define NATIVE_STREAM_FIRST    4096 // frames read at the start; a stream this short is decoded whole
#define NATIVE_STREAM_CHUNK    1024 // frames: a refill waits for this much room
#define NATIVE_STREAM_STEP     4096 // frames: the most a refill reads in one frame
#define NATIVE_STREAM_SEGMENTS 8
#define NATIVE_PARKED          3    // G8: songs kept open where they stopped
#define MAX_MUSIC_SONGS        16

typedef enum {
    NATIVE_KIND_3D,
    NATIVE_KIND_2D,
    NATIVE_KIND_STREAM,
    NATIVE_KIND_MUSIC // songs, mp3, triggered music, the movie
} native_kind_t;

typedef enum {
    SONG_FADE_NONE,
    SONG_FADE_IN,
    SONG_FADE_OUT
} native_fade_t;

// a stream: the codec read ahead into the mixer's ring, or decoded whole (bWhole)
typedef struct {
    snd_stream_t *pCodec; // open while there is more to read
    char          szFile[MAX_QPATH];
    int           iRate;
    int           iChannels;
    int           iWidth;      // bytes a sample; 0 = IMA ADPCM
    int           iBlockAlign; // IMA ADPCM's, bytes
    int           iFrames;     // a pass's, as the codec counts them
    int           iSound;      // the mixer's ring, or the whole stream
    int           iRingFrames;
    bool          bWhole;
    bool          bStarted; // a voice was started on it
    bool          bParked;  // stopped where it was, its voice to start again there (G8)
    bool          bEnded;   // its last frame is in the ring
    int           iPass;    // the pass being read (1 = the first)
    int           iFilePos; // the codec's next frame
    uint64_t      iWritten; // frames committed to the ring
    // ring frame iSegRing[i] onward is file frame iSegFile[i] onward (the start offset, each loop)
    uint64_t iSegRing[NATIVE_STREAM_SEGMENTS];
    int      iSegFile[NATIVE_STREAM_SEGMENTS];
    int      iNumSeg;
} native_stream_t;

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
    float    fPostedAzimuth, fPostedElevation; // ... with a direction (headphone 3D: fPostedL is the level)
    bool     bPostedDirection;
    vec3_t   vSpatial;    // the origin a 2D voice was last panned from (the event log)
    uint64_t iRequestTime; // the probe: the host time of a start not yet seen leaving the device

    // the streamed and music channels' (9d)
    native_stream_t stream;
    bool            bPlayedOut; // its stream ended by itself (G7)
    int             song_number;
    native_fade_t   fading;
    float           fade_time; // ms
    int             fade_start_time;

public:
    void play();
    void stop();
    void pause();

    bool         queue_stream(const char *fileName);
    void         stream_start(bool bPaused);
    void         update();
    unsigned int sample_ms_offset();
    void         set_sample_ms_offset(unsigned int offset);
    int          channels();

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
    bool direction(float *gain, float *azimuth, float *elevation);
    bool post_direction();
    void post();
};

static struct {
    native_channel  chan[NATIVE_CHANNELS];
    native_channel *channel[NATIVE_CHANNELS];

    native_loop_sound_t loop_sounds[MAX_SOUNDSYSTEM_LOOP_SOUNDS];

    vec3_t vListenerOrigin;
    vec3_t vListenerAxis[3];
    float  fMaster;
    bool   bHeadphones; // 9f: headphone 3D in effect (s_speaker_type 1)

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

    // G8: songs stopped where they were, their streams kept (the oldest goes first)
    native_stream_t parked[NATIVE_PARKED];
    int             parkedSong[NATIVE_PARKED];
    int             parkedTime[NATIVE_PARKED];

    // the streams' account (the shutdown's summary)
    unsigned streamOpens, streamPasses, streamWhole, streamResumes;
    uint64_t streamFrames;
    short    scratch[8192]; // frames skipped
} native;

// the music: snd_openal_new.cpp's (the decompiled driver's), this driver's own
static qboolean music_active            = qfalse;
static int      music_current_mood      = 0;
static int      music_fallback_mood     = 0;
static float    old_music_volume        = 1.f;
static float    music_volume            = 1.f;
static float    new_music_volume        = 1.f;
static float    music_volume_fade_time  = 0;
static long int music_volume_start_time = 0;
static int      music_loaded            = 0;
static int      music_numsongs          = 0;
static int      music_currentsong       = 0;
static song_t   music_songs[MAX_MUSIC_SONGS];
static char     tm_filename[MAX_RES_NAME];
static int      tm_loopcount;

#define chan_song       (&native.chan[MAX_SOUNDSYSTEM_POSITION_CHANNELS])
#define chan_mp3        native.chan[SOUNDSYSTEM_CHANNEL_MP3_ID]
#define chan_trig_music native.chan[SOUNDSYSTEM_CHANNEL_TRIGGER_MUSIC_ID]
#define chan_movie      native.chan[SOUNDSYSTEM_CHANNEL_MOVIE_ID]

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

// the front end reads it (snd_local_new.h)
cvar_t        *s_show_sounds;
static cvar_t *s_device;
static cvar_t *s_bufferFrames;
static cvar_t *s_eventLog;
static cvar_t *s_capture;
static cvar_t *s_captureMute;
// the original's, as upstream's OpenAL driver registers them (without s_milesdriver, s_show_cpu and
// s_obstruction_cal_time, which nothing read: gone with it in 9g)
static cvar_t *s_reverb;
static cvar_t *s_show_num_active_sounds;
static cvar_t *s_speaker_type;

static int  S_NATIVE_SpatializeStereoSound(const vec3_t listener_origin, const vec3_t listener_left, const vec3_t origin);
static void S_NATIVE_SpatializeChannel(native_channel *pChannel, const vec3_t vListenerOrigin, const vec3_t vTempAxis);
static void S_NATIVE_LogStart(native_channel *pChannel, float fLeft, float fRight);
static void S_NATIVE_PlayMP3();
static void S_NATIVE_StopMP3();
static void S_NATIVE_Pitch();
static void MUSIC_Pause();
static void MUSIC_Unpause();
static void S_NATIVE_UpdateStreams();
static void S_NATIVE_DropParked();

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
        // read from its file when it plays (native_channel::stream_start)
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
    if (!native_initialized || pSfx->buffer) {
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
    if (!native_initialized || !pSfx->buffer) {
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
S_NATIVE_SpeakerType

9f: s_speaker_type 1 is headphone 3D (Apple's spatial mixer, made the first time; the 3D voices posted with a
direction from the next frame on); 0 stereo, the default; 2 and 3 (the original's 4 speakers and 5.1) play stereo:
its Fast 2D was stereo, and this port has no surround output
==============
*/
static void S_NATIVE_SpeakerType()
{
    const bool bWant = s_speaker_type->integer == 1;

    s_speaker_type->modified = qfalse;
    native.bHeadphones       = MacSnd_SetSpatial(bWant ? MACSND_HEADPHONES : MACSND_STEREO) == MACSND_HEADPHONES;
    if (bWant && !native.bHeadphones) {
        Com_Printf("sound: headphone 3D isn't available, stereo\n");
    } else if (s_speaker_type->integer > 1) {
        Com_Printf("sound: s_speaker_type %d plays stereo on the native driver\n", s_speaker_type->integer);
    } else if (native_initialized || native.bHeadphones) {
        Com_Printf("sound: %s\n", native.bHeadphones ? "headphone 3D" : "stereo");
    }
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

    s_device       = Cvar_Get("s_device", "", CVAR_ARCHIVE | CVAR_SOUND_LATCH);
    s_bufferFrames = Cvar_Get("s_bufferFrames", "64", CVAR_ARCHIVE | CVAR_SOUND_LATCH);
    s_show_sounds  = Cvar_Get("s_show_sounds", "0", 0);

    if (native_initialized) {
        Com_DPrintf("S_NATIVE_Init(): Called when sound is already initialized!\n");
        return qtrue;
    }

    s_reverb                 = Cvar_Get("s_reverb", "0", CVAR_SOUND_LATCH | CVAR_ARCHIVE);
    s_show_num_active_sounds = Cvar_Get("s_show_num_active_sounds", "0", 0);
    s_speaker_type           = Cvar_Get("s_speaker_type", "0", CVAR_ARCHIVE);
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
    mixer.maxSpatial   = 2 * (MAX_SOUNDSYSTEM_CHANNELS_3D + NATIVE_OVERFLOW_3D); // and as many playing out
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
    native.numProbes     = 0;
    native.gameTicks     = 0;
    native.frames        = 0;
    native.reopens       = 0;
    native.latency       = 0;
    native.streamOpens   = 0;
    native.streamWhole   = 0;
    native.streamPasses  = 0;
    native.streamResumes = 0;
    native.streamFrames  = 0;
    for (i = 0; i < NATIVE_PARKED; i++) {
        native.parked[i] = {};
    }

    native.bHeadphones = false;
    S_NATIVE_SpeakerType();
    s_reverb->modified = qfalse;

    Cmd_AddCommand("playmp3", S_NATIVE_PlayMP3);
    Cmd_AddCommand("stopmp3", S_NATIVE_StopMP3);
    Cmd_AddCommand("loadsoundtrack", S_loadsoundtrack);
    Cmd_AddCommand("playsong", S_PlaySong);
    Cmd_AddCommand("pitch", S_NATIVE_Pitch);
    Cmd_AddCommand("tmstart", S_TriggeredMusic_Start);
    Cmd_AddCommand("tmstartloop", S_TriggeredMusic_StartLoop);
    Cmd_AddCommand("tmstop", S_TriggeredMusic_Stop);
    Cmd_AddCommand("tmvolume", S_TriggeredMusic_Volume);

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
    if (stats.spatialMade) {
        Com_Printf(
            "sound: headphone 3D %s, %s HRTF, its latency %.2f ms, %u render errors\n",
            stats.spatial == MACSND_HEADPHONES ? "on" : "off",
            stats.personalized ? "the personalized" : "Apple's",
            stats.spatialLatency * 1e3,
            stats.spatialErrors
        );
    }
    Com_Printf(
        "sound: game thread %.2f us a frame over %u frames; %d sounds, %.1f MB\n",
        native.frames ? S_NATIVE_TicksToMs(native.gameTicks) * 1e3 / native.frames : 0,
        native.frames,
        stats.sounds,
        stats.soundBytes / (1024.0 * 1024.0)
    );
    Com_Printf(
        "sound: streams %u opened, %u decoded whole, %u passes looped, %u songs resumed, %llu frames read; %u "
        "underruns\n",
        native.streamOpens,
        native.streamWhole,
        native.streamPasses,
        native.streamResumes,
        (unsigned long long)native.streamFrames,
        stats.underruns
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
        Com_Printf("speakers - %s\n", stats.spatial == MACSND_HEADPHONES ? (stats.personalized
            ? "headphone 3D (the personalized HRTF)" : "headphone 3D (Apple's HRTF)") : "stereo");
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

    if (chan_mp3.is_playing()) {
        chan_mp3.pause();
    }

    MUSIC_Pause();
    S_TriggeredMusic_Pause();
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

    if (chan_mp3.is_paused()) {
        chan_mp3.play();
    }

    MUSIC_Unpause();
    S_TriggeredMusic_Unpause();

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

    if (!s_bSoundStarted || !native_initialized) {
        return;
    }

    S_NATIVE_StopLoopingSounds();

    for (i = 0; i < NATIVE_CHANNELS; i++) {
        if (S_NATIVE_IsPositionChannel(i)) {
            native.channel[i]->force_free();
        }
    }

    if (bStopMusic) {
        MUSIC_FreeAllSongs();
        S_TriggeredMusic_Stop();
        // Fixed in the Metal port (G11)
        //  The mp3 and the movie's audio stop too (OpenAL's went on)
        chan_mp3.stop();
        chan_movie.stop();
    }
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

The playing voices' gains and rates to the mixer, those that changed (the music's too)
==============
*/
static void S_NATIVE_PostAll()
{
    int i;

    for (i = 0; i < NATIVE_CHANNELS; i++) {
        native.channel[i]->post();
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
        S_NATIVE_SpeakerType();
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

    Music_Update();

    for (i = 0; i < MAX_SOUNDSYSTEM_LOOP_SOUNDS; i++) {
        if (!native.loop_sounds[i].bInUse) {
            S_NATIVE_StopLoopingSound(&native.loop_sounds[i]);
        }
    }

    S_NATIVE_UpdateStreams();
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

The triggered music's (the savegame and snd_restart keep it)
==============
*/
const char *S_NATIVE_GetMusicFilename()
{
    if (!native_initialized || (!chan_trig_music.is_playing() && !chan_trig_music.is_paused())) {
        return "";
    }

    return tm_filename;
}

/*
==============
S_NATIVE_GetMusicLoopCount
==============
*/
int S_NATIVE_GetMusicLoopCount()
{
    return tm_loopcount;
}

/*
==============
S_NATIVE_GetMusicOffset
==============
*/
unsigned int S_NATIVE_GetMusicOffset()
{
    if (!native_initialized) {
        return 0;
    }

    return chan_trig_music.sample_offset();
}

/*
==============
S_IsSoundPlaying
==============
*/
qboolean S_IsSoundPlaying(int channel_number, const char *sfxName)
{
    native_channel *pChannel;

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

    for (i = 0; i < MAX_SOUNDSYSTEM_POSITION_CHANNELS; i++) {
        S_InitBase(&pSave->Channels[i]);
    }
}

/*
==============================================================================

The streams (9d): the 2D streamed channels' (dialogue, the streamed aliases) and the music's (the songs, playmp3,
the triggered music, the movies' audio). OpenAL's driver read 16384 frames a frame into AL buffers, up to 16 of them
(~6 s ahead, one big read on the main thread each frame until then); here a stream is read ahead into a mixer ring of
NATIVE_STREAM_SECONDS, at most NATIVE_STREAM_STEP frames a frame and only once NATIVE_STREAM_CHUNK frames fit (G1), and
a stream no longer than NATIVE_STREAM_FIRST frames is decoded whole at its start and looped by the mixer (no file
opened again at every loop of a short song).

==============================================================================
*/

/*
==============
S_NATIVE_CloseStream
==============
*/
static void S_NATIVE_CloseStream(native_stream_t *s)
{
    if (s->pCodec) {
        S_CodecCloseStream(s->pCodec);
    }
    if (s->iSound) {
        MacSnd_FreeSound(s->iSound);
    }
    *s = {};
}

/*
==============
S_NATIVE_OpenStream

The codec on a file: 16-bit or 8-bit PCM or IMA ADPCM (H2), mono or stereo
==============
*/
static bool S_NATIVE_OpenStream(native_stream_t *s, const char *name)
{
    snd_stream_t *codec;

    S_NATIVE_CloseStream(s);

    codec = S_CodecOpenStream(name);
    if (!codec) {
        return false;
    }

    if (codec->info.channels < 1 || codec->info.channels > 2 || codec->info.rate < 1
        || !(codec->info.width == 1 || codec->info.width == 2 || (codec->info.width == 0.5f && codec->info.dataalign > 1)
        )) {
        Com_Printf(
            "sound: unsupported stream (%d channels, %d bits) [%s].\n",
            codec->info.channels,
            (int)(codec->info.width * 8),
            name
        );
        S_CodecCloseStream(codec);
        return false;
    }

    Q_strncpyz(s->szFile, name, sizeof(s->szFile));
    s->pCodec    = codec;
    s->iRate     = codec->info.rate;
    s->iChannels = codec->info.channels;
    s->iWidth    = codec->info.width >= 1 ? (int)codec->info.width : 0;
    if (s->iWidth) {
        s->iFrames = codec->info.samples;
    } else {
        s->iBlockAlign = ((codec->info.dataalign - 1) / 8 * 4 + 4) * s->iChannels;
        s->iFrames     = SNM_IMAFrames(codec->info.size, s->iBlockAlign, s->iChannels);
    }
    s->iPass = 1;
    native.streamOpens++;

    return true;
}

/*
==============
S_NATIVE_ReadStream

Up to frames from the codec as 16-bit PCM (8-bit widened in place: each sample's byte is read before its short is
written); 0 at the end of the pass
==============
*/
static int S_NATIVE_ReadStream(native_stream_t *s, short *out, int frames)
{
    int bytes, i;

    if (!s->pCodec || !s->iWidth || frames <= 0) {
        return 0;
    }

    if (s->iWidth == 2) {
        bytes  = S_CodecReadStream(s->pCodec, frames * s->iChannels * 2, out);
        frames = bytes > 0 ? bytes / (s->iChannels * 2) : 0;
    } else {
        byte *raw = (byte *)out + frames * s->iChannels;

        bytes = S_CodecReadStream(s->pCodec, frames * s->iChannels, raw);
        for (i = 0; i < bytes; i++) {
            out[i] = (short)((raw[i] - 128) << 8);
        }
        frames = bytes > 0 ? bytes / s->iChannels : 0;
    }

    s->iFilePos += frames;
    native.streamFrames += frames;
    return frames;
}

/*
==============
S_NATIVE_SkipStream
==============
*/
static void S_NATIVE_SkipStream(native_stream_t *s, int frames)
{
    const int max = ARRAY_LEN(native.scratch) / 2;
    int       n;

    while (frames > 0 && (n = S_NATIVE_ReadStream(s, native.scratch, frames < max ? frames : max)) > 0) {
        frames -= n;
    }
}

/*
==============
S_NATIVE_StreamWhole

A short stream (or IMA ADPCM) decoded whole into a mixer sound, the codec closed
==============
*/
static bool S_NATIVE_StreamWhole(native_stream_t *s)
{
    short *pcm;
    int    got = 0, n;

    if (s->iFrames <= 0) {
        return false;
    }
    s->iSound = MacSnd_CreateSound(NULL, s->iFrames, s->iChannels, s->iRate);
    pcm       = MacSnd_SoundData(s->iSound);
    if (!pcm) {
        s->iSound = 0;
        return false;
    }

    if (s->iWidth) {
        while (got < s->iFrames && (n = S_NATIVE_ReadStream(s, pcm + got * s->iChannels, s->iFrames - got)) > 0) {
            got += n;
        }
    } else {
        // the data chunk at once (not the hunk's temp memory: a long stream would drop the game)
        const int bytes = s->pCodec->info.size;
        byte     *raw   = bytes > 0 ? (byte *)malloc(bytes) : NULL;

        if (raw) {
            n   = S_CodecReadStream(s->pCodec, bytes, raw);
            got = SNM_DecodeIMA(raw, n > 0 ? n : 0, s->iBlockAlign, s->iChannels, pcm, s->iFrames);
            free(raw);
        }
    }

    S_CodecCloseStream(s->pCodec);
    s->pCodec = NULL;
    s->bWhole = true;
    s->bEnded = true;
    if (got <= 0) {
        return false;
    }
    // the frames the codec gave (its count was the header's)
    s->iFrames = got;
    native.streamWhole++;

    return true;
}

/*
==============
S_NATIVE_AddSegment
==============
*/
static void S_NATIVE_AddSegment(native_stream_t *s, uint64_t ring, int file)
{
    if (s->iNumSeg == NATIVE_STREAM_SEGMENTS) {
        // the oldest is behind the voice (a looped stream's passes are longer than NATIVE_STREAM_FIRST frames)
        memmove(s->iSegRing, s->iSegRing + 1, sizeof(s->iSegRing[0]) * (NATIVE_STREAM_SEGMENTS - 1));
        memmove(s->iSegFile, s->iSegFile + 1, sizeof(s->iSegFile[0]) * (NATIVE_STREAM_SEGMENTS - 1));
        s->iNumSeg--;
    }
    s->iSegRing[s->iNumSeg] = ring;
    s->iSegFile[s->iNumSeg] = file;
    s->iNumSeg++;
}

/*
==============
S_NATIVE_LoopStream

The end of a pass: the next one from the loop block's start (G3: a looped song goes on 63 ms in; OpenAL's started
over at 0), the codec opened again; a pass that gave nothing ends it
==============
*/
static bool S_NATIVE_LoopStream(native_channel *pChannel)
{
    native_stream_t *s = &pChannel->stream;

    if (pChannel->iLoopCount > 0 && s->iPass >= pChannel->iLoopCount) {
        return false;
    }
    if (!s->pCodec || s->iWritten == s->iSegRing[s->iNumSeg - 1]) {
        return false;
    }

    S_CodecCloseStream(s->pCodec);
    s->pCodec = S_CodecOpenStream(s->szFile);
    if (!s->pCodec) {
        return false;
    }
    s->iFilePos = 0;
    S_NATIVE_SkipStream(s, pChannel->iLoopStart < s->iFrames ? pChannel->iLoopStart : 0);
    S_NATIVE_AddSegment(s, s->iWritten, s->iFilePos);
    s->iPass++;
    native.streamPasses++;

    return true;
}

/*
==============
S_NATIVE_FillStream

The ring topped up from the codec, at most budget frames
==============
*/
static void S_NATIVE_FillStream(native_channel *pChannel, int budget)
{
    native_stream_t *s = &pChannel->stream;
    short           *p;
    int              space, n;

    while (budget > 0 && !s->bEnded && (p = MacSnd_StreamBuffer(s->iSound, &space))) {
        n = S_NATIVE_ReadStream(s, p, space < budget ? space : budget);
        if (n > 0) {
            MacSnd_StreamCommit(s->iSound, n);
            s->iWritten += n;
            budget -= n;
        } else if (!S_NATIVE_LoopStream(pChannel)) {
            MacSnd_StreamEnd(s->iSound);
            s->bEnded = true;
            if (s->pCodec) {
                S_CodecCloseStream(s->pCodec);
                s->pCodec = NULL;
            }
        }
    }
}

/*
==============
S_NATIVE_StreamPosition

The file's frame its voice is at
==============
*/
static unsigned int S_NATIVE_StreamPosition(const native_channel *pChannel)
{
    const native_stream_t *s = &pChannel->stream;
    uint64_t               played;
    int                    i;

    if (s->bWhole) {
        return MacSnd_Position(pChannel->iVoice);
    }
    if (!s->iSound || !s->iNumSeg) {
        return 0;
    }

    played = MacSnd_StreamPlayed(s->iSound);
    for (i = s->iNumSeg - 1; i > 0 && s->iSegRing[i] > played; i--) {}

    return s->iSegFile[i] + (unsigned int)(played - s->iSegRing[i]);
}

/*
==============
S_NATIVE_IsStreamKind
==============
*/
static inline bool S_NATIVE_IsStreamKind(int iKind)
{
    return iKind == NATIVE_KIND_STREAM || iKind == NATIVE_KIND_MUSIC;
}

/*
==============
S_NATIVE_UpdateStreams
==============
*/
static void S_NATIVE_UpdateStreams()
{
    int i;

    for (i = MAX_SOUNDSYSTEM_CHANNELS_3D + MAX_SOUNDSYSTEM_CHANNELS_2D; i < MAX_SOUNDSYSTEM_CHANNELS; i++) {
        native.chan[i].update();
    }
}

/*
==============
S_NATIVE_DropParked
==============
*/
static void S_NATIVE_DropParked()
{
    int i;

    for (i = 0; i < NATIVE_PARKED; i++) {
        S_NATIVE_CloseStream(&native.parked[i]);
    }
}

/*
==============
S_NATIVE_ParkSong

Added in the Metal port (G8): a song stopped where it was keeps its stream (the codec and the ring, the voice
stopped), so playing it again goes on from there without reading the file up to its position again (OpenAL's
songs started over); the oldest kept song goes when there is no room
==============
*/
static void S_NATIVE_ParkSong(native_channel *pChannel)
{
    native_stream_t *s = &pChannel->stream;
    int              i, slot = 0;

    if (!s->bStarted || s->bWhole || !s->iSound) {
        return;
    }

    MacSnd_Stop(pChannel->iVoice);
    for (i = 0; i < NATIVE_PARKED; i++) {
        if (!native.parked[i].bParked) {
            slot = i;
            break;
        }
        if (native.parkedTime[i] < native.parkedTime[slot]) {
            slot = i;
        }
    }

    S_NATIVE_CloseStream(&native.parked[slot]);
    native.parked[slot]         = *s;
    native.parked[slot].bParked = true;
    native.parkedSong[slot]     = pChannel->song_number;
    native.parkedTime[slot]     = cls.realtime;
    *s                          = {};
}

/*
==============
S_NATIVE_UnparkSong
==============
*/
static bool S_NATIVE_UnparkSong(native_channel *pChannel, int songnum)
{
    int i;

    for (i = 0; i < NATIVE_PARKED; i++) {
        if (native.parked[i].bParked && native.parkedSong[i] == songnum
            && !Q_stricmp(native.parked[i].szFile, music_songs[songnum].path)) {
            pChannel->stop();
            pChannel->stream     = native.parked[i];
            native.parked[i]     = {};
            pChannel->iBaseRate  = pChannel->stream.iRate;
            pChannel->fRate      = pChannel->iBaseRate;
            pChannel->iOffset    = 0;
            pChannel->iLoopStart = 0;
            pChannel->iLoopEnd   = 0;
            pChannel->iLoopCount = 1;
            return true;
        }
    }

    return false;
}

/*
==============
MUSIC_LoadSoundtrackFile

Fixed in the Metal port (G9): a one-argument line names the file and the alias is the name without its 4-character
extension (OpenAL's took the second argument, nothing, and dropped a character more); "path" and "!" lines need
their arguments; a line's tokens past MAX_MUSIC_SONGS are skipped (they overran the array)
==============
*/
qboolean MUSIC_LoadSoundtrackFile(const char *filename)
{
    song_t *psong = NULL;
    char    args[MAX_MUSIC_SONGS][MAX_RES_NAME];
    int     numargs;
    char    com_token[MAX_STRING_CHARS];
    char    alias[128];
    char    file[128];
    char    load_path[128];
    char   *buffer;
    char    path[MAX_RES_NAME];
    int     i;
    byte   *data;

    if (strrchr(filename, '.')) {
        Com_sprintf(path, sizeof(path), "%s", filename);
    } else {
        Com_sprintf(path, sizeof(path), "%s.mus", filename);
    }

    music_numsongs = 0;

    FS_ReadFile(path, (void **)&data);
    if (!data) {
        Com_DPrintf("sound: Couldn't load %s\n", path);
        return false;
    }
    Com_DPrintf("SOUNDTRACK: Loading %s\n", path);

    MUSIC_StopAllSongs();
    S_NATIVE_DropParked();

    buffer       = (char *)data;
    load_path[0] = 0;

    while (1) {
        Q_strncpyz(com_token, COM_GetToken(&buffer, true), sizeof(com_token));
        if (!com_token[0]) {
            break;
        }

        if (strlen(com_token) >= MAX_RES_NAME) {
            Com_Printf("MUSIC_LoadSoundtrackFile: argument too long, truncating in %s\n", path);
            com_token[MAX_RES_NAME - 1] = 0;
        }

        numargs = 1;
        Q_strncpyz(args[0], com_token, sizeof(args[0]));

        while (1) {
            Q_strncpyz(com_token, COM_GetToken(&buffer, false), sizeof(com_token));
            if (!com_token[0]) {
                break;
            }

            if (numargs >= MAX_MUSIC_SONGS) {
                continue;
            }

            if (strlen(com_token) >= MAX_RES_NAME) {
                Com_Printf("MUSIC_LoadSoundtrackFile: argument too long, truncating in %s\n", path);
                com_token[MAX_RES_NAME - 1] = 0;
            }

            Q_strncpyz(args[numargs], com_token, sizeof(args[numargs]));
            numargs++;
        }

        if (!Q_stricmp(args[0], "path")) {
            if (numargs < 2) {
                Com_Printf("MUSIC_LoadSoundtrackFile: path without a folder in %s\n", path);
                continue;
            }
            Q_strncpyz(load_path, args[1], sizeof(load_path));
            if (load_path[strlen(load_path) - 1] != '/' && load_path[strlen(load_path) - 1] != '\\') {
                Q_strcat(load_path, sizeof(load_path), "/");
            }
        } else if (args[0][0] == '!') {
            for (i = 0; i < music_numsongs; i++) {
                psong = &music_songs[i];
                if (!Q_stricmp(psong->alias, &args[0][1])) {
                    break;
                }
            }

            if (i == music_numsongs) {
                Com_Printf("MUSIC_LoadSoundtrackFile: song %s not found, command skipped in %s.\n", &args[0][1], path);
                continue;
            }

            if (numargs < 2) {
                Com_Printf("MUSIC_LoadSoundtrackFile: no command for song %s in %s.\n", &args[0][1], path);
            } else if (!Q_stricmp(args[1], "volume") && numargs > 2) {
                psong->volume = atof(args[2]);
            } else if (!Q_stricmp(args[1], "fadetime") && numargs > 2) {
                psong->fadetime = atof(args[2]);
            } else if (!Q_stricmp(args[1], "loop")) {
                psong->flags |= 1;
            } else if (!Q_stricmp(args[1], "restart")) {
                psong->flags |= 2;
            } else if (!Q_stricmp(args[1], "interrupt")) {
                psong->fadetime = 0;
                psong->flags |= 4 | 2;
            } else {
                Com_Printf(
                    "MUSIC_LoadSoundtrackFile: unknown command %s for song %s in %s.\n", args[1], &args[0][1], path
                );
            }
        } else {
            if (numargs > 1) {
                Q_strncpyz(alias, args[0], sizeof(alias));
                Q_strncpyz(file, load_path, sizeof(file));
                Q_strcat(file, sizeof(file), args[1]);
            } else {
                size_t len;

                Q_strncpyz(file, load_path, sizeof(file));
                Q_strcat(file, sizeof(file), args[0]);

                len = strlen(args[0]);
                if (len > 4) {
                    Q_strncpyz(alias, args[0], len - 3);
                } else {
                    alias[0] = 0;
                }
            }

            if (music_numsongs >= MAX_MUSIC_SONGS) {
                Com_Printf("MUSIC_LoadSoundtrackFile: Too many songs in %s, skipping %s.\n", path, alias);
                continue;
            }

            psong = &music_songs[music_numsongs];
            Q_strncpyz(psong->alias, alias, sizeof(psong->alias));
            Q_strncpyz(psong->path, file, sizeof(psong->path));
            music_songs[music_numsongs].fadetime    = 1.0;
            music_songs[music_numsongs].volume      = 1.0;
            music_songs[music_numsongs].flags       = 0;
            music_songs[music_numsongs].current_pos = 0;
            music_songs[music_numsongs].mood_num    = MusicMood_NameToNum(alias);
            music_numsongs++;
        }
    }

    if (!music_numsongs) {
        Com_Printf("MUSIC_LoadSoundtrackFile: could not load %s, no songs defined.\n", path);
        FS_FreeFile(data);
        return false;
    }

    music_currentsong = -1;
    FS_FreeFile(data);

    if (music_current_mood == mood_none) {
        MUSIC_UpdateMood(mood_normal, mood_normal);
    }

    return true;
}

/*
==============
MUSIC_SongValid
==============
*/
qboolean MUSIC_SongValid(const char *mood)
{
    return MUSIC_FindSong(mood) != -1;
}

/*
==============
MUSIC_Loaded
==============
*/
qboolean MUSIC_Loaded()
{
    return music_loaded;
}

/*
==============
S_NATIVE_SongGain

A song's level (A5: the song's volume x s_ambientvolume x the script's music volume; s_volume and the fade are
the master's) at a crossfade's point
==============
*/
static float S_NATIVE_SongGain(int song_number, float fade)
{
    return music_songs[song_number].volume * s_ambientVolume->value * music_volume * fade;
}

/*
==============
S_NATIVE_AmbientOff

Added in the Metal port (9g, the user's): s_ambientvolume 0 plays no song of the soundtrack (the mood music and the
amb_stereo ambiences): none is opened, decoded or mixed, and those playing, fading or kept where they stopped (G8)
stop and free their streams. Raised again, Music_Update starts the mood's song (music_currentsong -1: not playing).
==============
*/
static bool S_NATIVE_AmbientOff()
{
    int i;

    if (s_ambientVolume->value > 0) {
        return false;
    }

    for (i = 0; i < MAX_SOUNDSYSTEM_SONGS; i++) {
        if (chan_song[i].is_playing() || chan_song[i].is_paused()) {
            MUSIC_StopChannel(i);
        }
    }
    for (i = 0; i < NATIVE_PARKED; i++) {
        if (native.parked[i].bParked) {
            S_NATIVE_CloseStream(&native.parked[i]);
        }
    }
    music_currentsong = -1;

    return true;
}

/*
==============
S_NATIVE_TriggeredGain

Fixed in the Metal port (G10): the triggered music follows s_musicvolume and tmvolume without a soundtrack too
(the menu's theme: OpenAL's Music_Update returned before it)
==============
*/
static void S_NATIVE_TriggeredGain()
{
    if (chan_trig_music.is_playing() || chan_trig_music.is_paused()) {
        chan_trig_music.set_gain(chan_trig_music.fVolume * s_musicVolume->value);
    }
}

/*
==============
Music_Update
==============
*/
void Music_Update()
{
    int currentsong;

    if (!s_bSoundStarted) {
        return;
    }

    S_NATIVE_TriggeredGain();

    if (!music_active) {
        return;
    }

    if (s_bSoundPaused) {
        return;
    }

    MUSIC_CheckForStoppedSongs();

    if (!MUSIC_Loaded() && music_active && strlen(current_soundtrack)) {
        MUSIC_LoadSoundtrackFile(current_soundtrack);
        music_loaded = true;
    }

    if (S_NATIVE_AmbientOff()) {
        return;
    }

    if (music_currentsong >= 0) {
        currentsong = music_songs[music_currentsong].mood_num;
    } else {
        currentsong = -1;
    }

    if (music_current_mood == mood_none) {
        if (MUSIC_Playing()) {
            MUSIC_StopAllSongs();
        }
    } else if (music_current_mood != currentsong) {
        const char *mood = MusicMood_NumToName(music_current_mood);
        if (MUSIC_SongValid(mood) && MUSIC_Loaded() && strlen(current_soundtrack)) {
            Com_DebugPrintf("Playing %s.\n", mood);
            MUSIC_PlaySong(mood);
        }
    }

    // Fixed in the Metal port (G6)
    //  From the old volume to the new over the fade time (going down, OpenAL's started from 1)
    if (new_music_volume != music_volume) {
        if (music_volume_fade_time > 0) {
            const float f = (cls.realtime - music_volume_start_time) / (music_volume_fade_time * 1000.f);

            if (f >= 1) {
                music_volume = new_music_volume;
            } else {
                music_volume = old_music_volume + (new_music_volume - old_music_volume) * (f > 0 ? f : 0);
            }
        } else {
            music_volume = new_music_volume;
        }
    }

    MUSIC_UpdateMusicVolumes();
}

/*
==============
MUSIC_SongEnded
==============
*/
void MUSIC_SongEnded()
{
    Com_DPrintf(
        "MUSIC: Song ended, changing from [ %s ] to [ %s ]\n",
        MusicMood_NumToName(music_current_mood),
        MusicMood_NumToName(music_fallback_mood)
    );
    music_current_mood = music_fallback_mood;
}

/*
==============
MUSIC_NewSoundtrack
==============
*/
void MUSIC_NewSoundtrack(const char *name)
{
    if (!s_bSoundStarted) {
        return;
    }

    if (!Q_stricmp(name, current_soundtrack)) {
        return;
    }

    Q_strncpyz(current_soundtrack, name, sizeof(current_soundtrack));

    if (!Q_stricmp(current_soundtrack, "none") || !Q_stricmp(current_soundtrack, "")) {
        music_active = qfalse;
        if (MUSIC_Playing()) {
            MUSIC_StopAllSongs();
        }
    } else {
        music_active = qtrue;
        MUSIC_LoadSoundtrackFile(name);
        music_loaded = qtrue;
    }
}

/*
==============
MUSIC_UpdateMood
==============
*/
void MUSIC_UpdateMood(int current, int fallback)
{
    static int last_current_mood  = -1;
    static int last_fallback_mood = -1;
    static int current_mood       = -1;
    static int fallback_mood      = -1;
    qboolean   was_action;

    if (current == current_mood && fallback == fallback_mood) {
        return;
    }

    was_action          = current == last_current_mood && fallback == last_fallback_mood && current_mood == mood_action;
    last_current_mood   = current_mood;
    last_fallback_mood  = fallback_mood;
    current_mood        = current;
    music_current_mood  = current;
    fallback_mood       = fallback;
    music_fallback_mood = fallback;
    music_active        = qtrue;

    Com_DPrintf(
        "MUSIC: changing from [ %s | %s ] to [ %s | %s ]\n",
        MusicMood_NumToName(last_current_mood),
        MusicMood_NumToName(last_fallback_mood),
        MusicMood_NumToName(current_mood),
        MusicMood_NumToName(fallback)
    );

    if (was_action) {
        int songnum = MUSIC_FindSong(MusicMood_NumToName(current_mood));

        if (songnum != -1 && (music_songs[songnum].flags & 4)) {
            Com_DPrintf(
                "MUSIC: restoring music from action state, skipping [ %s ] for [ %s ]\n",
                MusicMood_NumToName(current_mood),
                MusicMood_NumToName(fallback_mood)
            );
            music_current_mood = music_fallback_mood;
        }
    }
}

/*
==============
MUSIC_UpdateVolume
==============
*/
void MUSIC_UpdateVolume(float volume, float fade_time)
{
    if (new_music_volume == volume && music_volume_fade_time == fade_time) {
        return;
    }

    old_music_volume        = music_volume;
    new_music_volume        = volume;
    music_volume_fade_time  = fade_time;
    music_volume_start_time = cls.realtime;
}

/*
==============
MUSIC_StopAllSongs
==============
*/
void MUSIC_StopAllSongs()
{
    for (int i = 0; i < MAX_SOUNDSYSTEM_SONGS; i++) {
        MUSIC_StopChannel(i);
    }

    music_currentsong = -1;
}

/*
==============
MUSIC_FreeAllSongs
==============
*/
void MUSIC_FreeAllSongs()
{
    MUSIC_StopAllSongs();
    S_NATIVE_DropParked();
    MUSIC_UpdateMood(mood_none, mood_none);
    current_soundtrack[0] = 0;
    music_loaded          = false;

    // Added in OPM
    //  Clear the list of songs so the songs from previous maps
    //  won't get played
    music_numsongs = 0;
}

/*
==============
MUSIC_Playing
==============
*/
qboolean MUSIC_Playing()
{
    return MUSIC_CurrentSongChannel() != -1;
}

/*
==============
MUSIC_FindSong
==============
*/
int MUSIC_FindSong(const char *name)
{
    int i;

    for (i = 0; i < music_numsongs; i++) {
        if (!Q_stricmp(music_songs[i].alias, name)) {
            return i;
        }
    }

    return -1;
}

/*
==============
S_loadsoundtrack
==============
*/
void S_loadsoundtrack()
{
    if (Cmd_Argc() != 2) {
        Com_Printf("loadsoundtrack <sound track file>\n");
        return;
    }

    MUSIC_LoadSoundtrackFile(Cmd_Argv(1));
    music_loaded = true;
    Q_strncpyz(current_soundtrack, Cmd_Argv(1), sizeof(current_soundtrack));
}

/*
==============
S_CurrentSoundtrack
==============
*/
const char *S_CurrentSoundtrack()
{
    return current_soundtrack;
}

/*
==============
S_PlaySong
==============
*/
void S_PlaySong()
{
    if (Cmd_Argc() != 2) {
        Com_Printf("playsong <song alias>\n");
        return;
    }

    MUSIC_PlaySong(Cmd_Argv(1));
    music_active = true;
}

/*
==============
MUSIC_CurrentSongChannel
==============
*/
int MUSIC_CurrentSongChannel()
{
    int channel_number = -1;
    int ch_idx         = 0;

    for (ch_idx = 0; ch_idx < MAX_SOUNDSYSTEM_SONGS; ch_idx++) {
        if (chan_song[ch_idx].is_playing() && chan_song[ch_idx].song_number == music_currentsong) {
            channel_number = ch_idx;
        }
    }

    return channel_number;
}

/*
==============
MUSIC_StopChannel

Fixed in the Metal port (G8): the position read while the song still plays (OpenAL paused it first, and a paused
channel's position read 0: songs never went on where they were), the song then kept where it stopped
==============
*/
void MUSIC_StopChannel(int channel_number)
{
    native_channel *channel = &chan_song[channel_number];
    song_t         *song    = &music_songs[channel->song_number];

    if (channel->is_playing() || channel->is_paused()) {
        if (song->flags & 2) {
            song->current_pos = 0;
        } else {
            song->current_pos = channel->sample_ms_offset();
            S_NATIVE_ParkSong(channel);
        }
    }

    channel->stop();
    channel->fading = SONG_FADE_NONE;
}

/*
==============
MUSIC_PlaySong
==============
*/
qboolean MUSIC_PlaySong(const char *alias)
{
    int             channel_number;
    song_t         *song;
    int             songnum;
    int             channel_to_play_on;
    int             fading_song;
    native_channel *song_channel;
    int             rate;

    fading_song = 0;
    songnum     = MUSIC_FindSong(alias);

    if (songnum == -1) {
        return true;
    }

    song = &music_songs[songnum];

    // Added in the Metal port (9g)
    //  s_ambientvolume 0: no song, and not taken as the current one (S_NATIVE_AmbientOff)
    if (s_ambientVolume->value <= 0) {
        return true;
    }

    if (MUSIC_Playing() && songnum == music_currentsong) {
        return true;
    }

    channel_number    = MUSIC_CurrentSongChannel();
    music_currentsong = songnum;

    if (channel_number != -1) {
        if (song->flags & 4) {
            MUSIC_StopChannel(channel_number);
        } else {
            // Fixed in the Metal port (G5)
            //  The fade time in ms (OpenAL's in whole seconds: 0.5 was 0, a division by zero)
            song_channel                  = &chan_song[channel_number];
            song_channel->fading          = SONG_FADE_OUT;
            song_channel->fade_time       = song->fadetime * 1000.f;
            song_channel->fade_start_time = cls.realtime;
            fading_song                   = true;
        }
    }
    channel_to_play_on = (fading_song != 0) && (channel_number == 0);
    song_channel       = &chan_song[channel_to_play_on];

    if (song_channel->is_playing() || song_channel->is_paused()) {
        MUSIC_StopChannel(channel_to_play_on);
    }

    // Added in the Metal port (G8)
    //  A song kept where it stopped goes on from there
    if (!S_NATIVE_UnparkSong(song_channel, songnum)) {
        if (!song_channel->queue_stream(song->path)) {
            Com_DPrintf("Could not start music file '%s'!", song->path);
            return false;
        }

        rate = song_channel->sample_playback_rate();
        if (song->current_pos) {
            song_channel->set_sample_ms_offset(song->current_pos);
        } else {
            song_channel->set_sample_offset(rate * 0.063f);
        }
    }

    rate                      = song_channel->sample_playback_rate();
    song_channel->song_number = songnum;

    if (song->flags & 1) {
        song_channel->set_sample_loop_count(0);
        song_channel->set_sample_loop_block(rate * 0.063f, -1);
    } else {
        song_channel->set_sample_loop_count(1);
    }

    if (fading_song) {
        song_channel->fading          = SONG_FADE_IN;
        song_channel->fade_time       = song->fadetime * 1000.f;
        song_channel->fade_start_time = cls.realtime;
        song_channel->set_gain(0.0);
    } else {
        song_channel->fading = SONG_FADE_NONE;
        song_channel->set_gain(S_NATIVE_SongGain(songnum, 1));
    }

    song_channel->play();

    return true;
}

/*
==============
MUSIC_UpdateMusicVolumes

Fixed in the Metal port (G5): a crossfade's fraction is the time over the fade time, 0 to 1 (an instant change
without one), the new song in on it and the old one out on 1 minus it, then stopped (OpenAL's fade-out grew from 0,
so the old song got louder and never stopped). Every song's level each frame (it only reaches the mixer when it
changes).
==============
*/
void MUSIC_UpdateMusicVolumes()
{
    int   i;
    float fade;

    for (i = 0; i < MAX_SOUNDSYSTEM_SONGS; i++) {
        native_channel *chan = &chan_song[i];

        if (!chan->is_playing() && !chan->is_paused()) {
            continue;
        }

        if (chan->fading == SONG_FADE_NONE) {
            chan->set_gain(S_NATIVE_SongGain(chan->song_number, 1));
            continue;
        }

        if (chan->fade_time > 0) {
            fade = (cls.realtime - chan->fade_start_time) / chan->fade_time;
            fade = fade < 0 ? 0 : fade > 1 ? 1 : fade;
        } else {
            fade = 1;
        }

        if (chan->fading == SONG_FADE_IN) {
            chan->set_gain(S_NATIVE_SongGain(chan->song_number, fade));
            if (fade >= 1) {
                chan->fading = SONG_FADE_NONE;
            }
        } else if (fade < 1) {
            chan->set_gain(S_NATIVE_SongGain(chan->song_number, 1 - fade));
        } else {
            MUSIC_StopChannel(i);
        }
    }
}

/*
==============
MUSIC_CheckForStoppedSongs

Fixed in the Metal port (G7): when the current song has played out the mood goes to the fallback (OpenAL's test
needed a channel playing and stopped at once: never); a song that played out starts over the next time
==============
*/
void MUSIC_CheckForStoppedSongs()
{
    int i;

    for (i = 0; i < MAX_SOUNDSYSTEM_SONGS; i++) {
        if (!chan_song[i].bPlayedOut) {
            continue;
        }

        chan_song[i].bPlayedOut                            = false;
        music_songs[chan_song[i].song_number].current_pos = 0;
        if (chan_song[i].song_number == music_currentsong) {
            MUSIC_SongEnded();
        }
    }
}

/*
==============
S_TriggeredMusic_SetupHandle

Fixed in the Metal port (G10): a failed start leaves nothing open; without autostart the music is set up paused
(OpenAL's played, then paused it)
==============
*/
void S_TriggeredMusic_SetupHandle(const char *pszName, int iLoopCount, int iOffset, qboolean autostart)
{
    const char      *pszRealName;
    float            fVolume     = 1.0;
    AliasListNode_t *pSoundAlias = NULL;

    if (!s_bSoundStarted) {
        return;
    }

    if (chan_trig_music.is_playing() || chan_trig_music.is_paused()) {
        Com_DPrintf("Didn't start new triggered music because some was already playing\n");
        return;
    }

    chan_trig_music.stop();
    Q_strncpyz(tm_filename, pszName, sizeof(tm_filename));

    tm_loopcount = iLoopCount;
    pszRealName  = Alias_FindRandom(pszName, &pSoundAlias);

    if (!pszRealName) {
        pszRealName = pszName;
    } else if (pSoundAlias) {
        fVolume = random() * pSoundAlias->volumeMod + pSoundAlias->volume;
    }

    if (!chan_trig_music.queue_stream(pszRealName)) {
        Com_DPrintf("Could not start triggered music '%s'\n", pszName);
        return;
    }

    chan_trig_music.fVolume = fVolume;
    chan_trig_music.set_gain(fVolume * s_musicVolume->value);
    chan_trig_music.set_sample_loop_count(iLoopCount);
    chan_trig_music.set_sample_offset(iOffset);
    chan_trig_music.stream_start(!autostart);
}

/*
==============
S_TriggeredMusic_Start
==============
*/
void S_TriggeredMusic_Start()
{
    if (Cmd_Argc() != 2) {
        Com_Printf("tmstart <sound file>\n");
        return;
    }

    S_TriggeredMusic_SetupHandle(Cmd_Argv(1), 1, 0, true);
}

/*
==============
S_TriggeredMusic_StartLoop
==============
*/
void S_TriggeredMusic_StartLoop()
{
    if (Cmd_Argc() != 2) {
        Com_Printf("tmstartloop <sound file>\n");
        return;
    }

    S_TriggeredMusic_SetupHandle(Cmd_Argv(1), 0, 0, true);
}

/*
==============
S_TriggeredMusic_Stop
==============
*/
void S_TriggeredMusic_Stop()
{
    if (!s_bSoundStarted) {
        return;
    }

    chan_trig_music.stop();
    // Fixed in OPM
    //  Make sure to clear the triggered music in case snd_restart is called
    tm_filename[0] = 0;
    tm_loopcount   = 0;
}

/*
==============
S_TriggeredMusic_Volume
==============
*/
void S_TriggeredMusic_Volume()
{
    if (!s_bSoundStarted) {
        return;
    }

    if (Cmd_Argc() != 2) {
        Com_Printf("tmvolume <0-1>\n\n");
        return;
    }

    chan_trig_music.fVolume = atof(Cmd_Argv(1));
}

/*
==============
S_TriggeredMusic_Pause
==============
*/
void S_TriggeredMusic_Pause()
{
    if (!s_bSoundStarted) {
        return;
    }

    if (chan_trig_music.is_playing()) {
        chan_trig_music.pause();
    }
}

/*
==============
S_TriggeredMusic_Unpause
==============
*/
void S_TriggeredMusic_Unpause()
{
    if (!s_bSoundStarted) {
        return;
    }

    if (chan_trig_music.is_paused()) {
        chan_trig_music.play();
    }

    S_NATIVE_TriggeredGain();
}

/*
==============
S_TriggeredMusic_PlayIntroMusic
==============
*/
void S_TriggeredMusic_PlayIntroMusic()
{
    S_TriggeredMusic_SetupHandle("sound/music/mus_MainTheme.mp3", 0, 0, true);
}

/*
==============
S_StopMovieAudio
==============
*/
void S_StopMovieAudio()
{
    chan_movie.stop();
}

/*
==============
S_SetupMovieAudio

G15: the movie's .mp3, else its .wav, at 1.5 (A5); the cinematic's clock follows its position
==============
*/
void S_SetupMovieAudio(const char *pszMovieName)
{
    char filename[MAX_QPATH];

    S_StopMovieAudio();

    COM_StripExtension(pszMovieName, filename, sizeof(filename));
    Q_strcat(filename, sizeof(filename), ".mp3");

    if (!chan_movie.queue_stream(filename)) {
        COM_StripExtension(pszMovieName, filename, sizeof(filename));
        Q_strcat(filename, sizeof(filename), ".wav");

        if (!chan_movie.queue_stream(filename)) {
            Com_DPrintf("Can't find any matching audio file for movie: %s\n", pszMovieName);
            return;
        }
    }

    chan_movie.set_gain(1.5f);
    chan_movie.play();

    Com_DPrintf("Movie Audio setup: %s\n", filename);
}

/*
==============
S_CurrentMoviePosition
==============
*/
int S_CurrentMoviePosition()
{
    return chan_movie.sample_ms_offset();
}

/*
==============
S_NATIVE_PlayMP3

The whole file decoded at once on the main thread in OpenAL's; streamed here
==============
*/
static void S_NATIVE_PlayMP3()
{
    char path[MAX_QPATH];

    if (Cmd_Argc() != 2) {
        Com_Printf("playmp3 <mp3 file>\n");
        return;
    }

    Q_strncpyz(path, Cmd_Argv(1), sizeof(path));
    FS_CorrectCase(path);

    if (!chan_mp3.queue_stream(path)) {
        Com_Printf("Failed to play mp3 - %s\n", Cmd_Argv(1));
        return;
    }

    chan_mp3.set_no_3d();
    chan_mp3.play();
    Com_Printf("Playing mp3 - %s\n", Cmd_Argv(1));
}

/*
==============
S_NATIVE_StopMP3
==============
*/
static void S_NATIVE_StopMP3()
{
    chan_mp3.stop();
}

/*
==============
S_NATIVE_Pitch
==============
*/
static void S_NATIVE_Pitch()
{
    Com_Printf("S_NATIVE_Pitch() needs to be implemented!\n");
}

/*
==============
MUSIC_Pause
==============
*/
static void MUSIC_Pause()
{
    int i;

    for (i = 0; i < MAX_SOUNDSYSTEM_SONGS; i++) {
        chan_song[i].pause();
    }
}

/*
==============
MUSIC_Unpause
==============
*/
static void MUSIC_Unpause()
{
    int i;

    for (i = 0; i < MAX_SOUNDSYSTEM_SONGS; i++) {
        if (chan_song[i].is_paused()) {
            chan_song[i].play();
        }
    }
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
    const char *kind = pChannel->iKind == NATIVE_KIND_3D       ? "3d"
                     : pChannel->iKind == NATIVE_KIND_STREAM ? "stream"
                     : pChannel->iKind == NATIVE_KIND_MUSIC  ? "music"
                                                             : "2d";
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
        "L %.9g R %.9g master %.9g 3d %d stereo %d hrtf %d az %.9g el %.9g\n",
        cls.realtime,
        cl.serverTime,
        pChannel->iVoice,
        kind,
        (pChannel->iFlags & CHANNEL_FLAG_PAUSED) != 0,
        pChannel->iEntNum,
        pChannel->iEntChannel,
        pChannel->pSfx ? pChannel->pSfx->name : pChannel->stream.szFile[0] ? pChannel->stream.szFile : "?",
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
        pChannel->channels() == 2,
        pChannel->bPostedDirection,
        pChannel->bPostedDirection ? pChannel->fPostedAzimuth : 0.f,
        pChannel->bPostedDirection ? pChannel->fPostedElevation : 0.f
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
    } else if (channels() == 2) {
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
native_channel::direction

9f, headphone 3D: a 3D voice's level by the Fast 2D distance model (B1) and its direction for Apple's HRTF, which
replaces the pan (C6); false for a 2D voice, without headphone 3D, or at the listener (no direction: the pan's centre)
==============
*/
bool native_channel::direction(float *gain, float *azimuth, float *elevation)
{
    vec3_t dir;
    float  d;

    if (!b3D || !native.bHeadphones) {
        return false;
    }

    VectorSubtract(vPosition, native.vListenerOrigin, dir);
    d = VectorNormalize(dir);
    if (d < SNM_NEAR) {
        return false;
    }

    *gain = fGain * SNM_Distance(d, fMinDist, fMaxDist);
    SNM_Direction(
        DotProduct(dir, native.vListenerAxis[0]),
        DotProduct(dir, native.vListenerAxis[1]),
        DotProduct(dir, native.vListenerAxis[2]),
        azimuth,
        elevation
    );
    return true;
}

/*
==============
native_channel::post_direction

Headphone 3D's post: false when the voice has no direction. Out of line, so post() stays small in the loop over every
channel, twice a frame
==============
*/
__attribute__((noinline)) bool native_channel::post_direction()
{
    float g, az, el;

    if (!direction(&g, &az, &el)) {
        return false;
    }

    if (!bPostedDirection || g != fPostedL || az != fPostedAzimuth || el != fPostedElevation || fRate != fPostedRate) {
        MacSnd_SetParams3D(iVoice, g, az, el, fRate);
        fPostedL         = g;
        fPostedR         = g;
        fPostedAzimuth   = az;
        fPostedElevation = el;
        fPostedRate      = fRate;
        bPostedDirection = true;
    }
    return true;
}

/*
==============
native_channel::post

Always inlined in its one caller, the loop over every channel twice a frame (S_NATIVE_PostAll): a call per channel
there costs the driver ~0.85 us a frame on the bench (session 62)
==============
*/
__attribute__((always_inline)) void native_channel::post()
{
    float l, r;

    if (MacSnd_Status(iVoice) == MACSND_STOPPED) {
        return;
    }

    if (b3D && native.bHeadphones && post_direction()) {
        return;
    }

    gains(&l, &r);
    if (bPostedDirection || l != fPostedL || r != fPostedR || fRate != fPostedRate) {
        MacSnd_SetParams(iVoice, l, r, fRate);
        fPostedL         = l;
        fPostedR         = r;
        fPostedRate      = fRate;
        bPostedDirection = false;
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

    if (S_NATIVE_IsStreamKind(iKind)) {
        stream_start(false);
        return;
    }

    if (!iSound) {
        return;
    }

    memset(&start, 0, sizeof(start));
    bPostedDirection = direction(&l, &start.azimuth, &start.elevation);
    if (bPostedDirection) {
        r                = l;
        start.spatial    = 1;
        fPostedAzimuth   = start.azimuth;
        fPostedElevation = start.elevation;
    } else {
        gains(&l, &r);
    }
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
    if (S_NATIVE_IsStreamKind(iKind)) {
        S_NATIVE_CloseStream(&stream);
    }
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

    if (S_NATIVE_IsStreamKind(iKind)) {
        iSound = 0;

        // Added in the Metal port
        //  A missing stream isn't searched for again at every start (as OpenAL's)
        if (pSfx->iFlags & SFX_FLAG_STREAM_FAILED) {
            return false;
        }
        if (!S_NATIVE_OpenStream(&stream, pSfx->name)) {
            pSfx->iFlags |= SFX_FLAG_STREAM_FAILED;
            return false;
        }

        // the sfx is the stream's (OpenAL's)
        pSfx->info.channels = stream.iChannels;
        pSfx->info.dataofs  = stream.pCodec->info.dataofs;
        pSfx->info.datasize = stream.pCodec->info.size;
        pSfx->info.rate     = stream.iRate;
        pSfx->info.samples  = stream.iFrames;
        pSfx->info.width    = stream.pCodec->info.width;

        iBaseRate = stream.iRate;
        fRate     = iBaseRate;
        return true;
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

    if (S_NATIVE_IsStreamKind(iKind)) {
        return S_NATIVE_StreamPosition(this);
    }

    return MacSnd_Position(iVoice);
}

/*
==============
native_channel::sample_ms_offset

0 when it isn't playing or paused (OpenAL's: also when paused); G13: the product in 64 bits
==============
*/
unsigned int native_channel::sample_ms_offset()
{
    if (MacSnd_Status(iVoice) == MACSND_STOPPED || fRate <= 0) {
        return 0;
    }

    return (unsigned int)((uint64_t)sample_offset() * 1000 / (uint64_t)fRate);
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
    const int status = MacSnd_Status(iVoice);

    if (status == MACSND_STOPPED) {
        iOffset = offset;
    } else if (!S_NATIVE_IsStreamKind(iKind)) {
        MacSnd_Seek(iVoice, offset);
    } else if (stream.bWhole) {
        MacSnd_Seek(iVoice, offset);
    } else {
        // a stream reads its file again up to the offset (OpenAL's)
        char name[MAX_QPATH];

        Q_strncpyz(name, stream.szFile, sizeof(name));
        MacSnd_Stop(iVoice);
        if (S_NATIVE_OpenStream(&stream, name)) {
            iOffset = offset;
            stream_start(status == MACSND_PAUSED);
        }
    }
}

/*
==============
native_channel::set_sample_ms_offset

G13: the product in 64 bits (OpenAL's overflowed after ~97 s at 44.1 kHz)
==============
*/
void native_channel::set_sample_ms_offset(unsigned int offset)
{
    set_sample_offset((unsigned int)((uint64_t)offset * (uint64_t)sample_playback_rate() / 1000));
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
    if (S_NATIVE_IsStreamKind(iKind)) {
        // a stream counts its passes again (OpenAL's); a whole one is looped by the mixer
        stream.iPass = 1;
        if (!stream.bWhole) {
            return;
        }
    }
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
    if (S_NATIVE_IsStreamKind(iKind) && !stream.bWhole) {
        // a stream's next pass starts there (G3); its end is the file's
        return;
    }
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

/*
==============
native_channel::channels

A stream's (a music channel has no sfx), else its sound's
==============
*/
int native_channel::channels()
{
    if (S_NATIVE_IsStreamKind(iKind) && stream.iChannels) {
        return stream.iChannels;
    }

    return pSfx ? pSfx->info.channels : 1;
}

/*
==============
native_channel::queue_stream

Fixed in the Metal port (G12): the old stream cleared before the name is kept (OpenAL's kept the name, then cleared
it with the stream still playing: the open failed); the end time in float seconds
==============
*/
bool native_channel::queue_stream(const char *fileName)
{
    stop();

    iOffset    = 0;
    iLoopStart = 0;
    iLoopEnd   = 0;
    iLoopCount = 1;

    if (!S_NATIVE_OpenStream(&stream, fileName)) {
        return false;
    }

    iBaseRate  = stream.iRate;
    fRate      = iBaseRate;
    iStartTime = cl.serverTime;
    iEndTime   = (int)(cl.serverTime + stream.iFrames * 1000.f / stream.iRate);

    return true;
}

/*
==============
native_channel::stream_start

Starts what set_sfx or queue_stream opened, from iOffset: decoded whole when short (or IMA ADPCM) and looped by the
mixer as Miles' loop block, else a ring with its first frames; a song kept where it stopped (G8) goes on there
==============
*/
void native_channel::stream_start(bool bPaused)
{
    native_stream_t *s = &stream;
    macSndStart_t    start;
    int              offset;
    float            l, r;

    memset(&start, 0, sizeof(start));

    if (s->bParked) {
        s->bParked = false;
        native.streamResumes++;
    } else {
        if (!s->pCodec || s->bStarted) {
            return;
        }

        offset  = iOffset;
        iOffset = 0;
        if (offset >= s->iFrames) {
            // OpenAL's: an offset past the end (a save of another file)
            offset = s->iFrames > 0 ? offset % s->iFrames : 0;
        }

        if (!s->iWidth || s->iFrames <= NATIVE_STREAM_FIRST) {
            if (!S_NATIVE_StreamWhole(s)) {
                S_NATIVE_CloseStream(s);
                return;
            }
            start.startFrame = offset;
            start.loopStart  = iLoopStart < s->iFrames ? iLoopStart : 0;
            start.loopEnd    = iLoopEnd > 0 && iLoopEnd < s->iFrames ? iLoopEnd : s->iFrames;
            start.loopCount  = iLoopCount;
        } else {
            s->iRingFrames = (int)(s->iRate * NATIVE_STREAM_SECONDS);
            if (s->iRingFrames < 8192) {
                s->iRingFrames = 8192;
            }
            s->iSound = MacSnd_CreateStream(s->iRingFrames, s->iChannels, s->iRate);
            if (!s->iSound) {
                S_NATIVE_CloseStream(s);
                return;
            }
            S_NATIVE_SkipStream(s, offset);
            S_NATIVE_AddSegment(s, 0, s->iFilePos);
            S_NATIVE_FillStream(this, NATIVE_STREAM_FIRST);
        }
    }

    s->bStarted = true;
    bPlayedOut  = false;

    gains(&l, &r);
    start.sound  = s->iSound;
    start.gainL  = l;
    start.gainR  = r;
    start.rate   = fRate;
    start.paused = bPaused;
    MacSnd_Start(iVoice, &start);

    fPostedL         = l;
    fPostedR         = r;
    fPostedRate      = fRate;
    bPostedDirection = false;
    iRequestTime     = mach_absolute_time();

    if (s_eventLog->integer) {
        S_NATIVE_LogStart(this, l, r);
    }
}

/*
==============
native_channel::update

A stream's frame: its ring topped up (G1), its end seen
==============
*/
void native_channel::update()
{
    native_stream_t *s = &stream;

    if (!s->bStarted) {
        return;
    }

    if (MacSnd_Status(iVoice) == MACSND_STOPPED) {
        // it played out (a stop closes it at once)
        bPlayedOut = true;
        S_NATIVE_CloseStream(s);
        return;
    }

    if (s->bWhole || s->bEnded) {
        return;
    }

    // a chunk's room in the ring (the mixer keeps 48 frames of it)
    if (s->iWritten - MacSnd_StreamPlayed(s->iSound) + NATIVE_STREAM_CHUNK + 48 > (uint64_t)s->iRingFrames) {
        return;
    }

    S_NATIVE_FillStream(this, NATIVE_STREAM_STEP);
}
