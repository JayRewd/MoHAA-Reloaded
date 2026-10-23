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

// snd_native_math.h -- the native sound driver's pure functions (docs/METAL_PORT_PLAN.md Phase 9): the original's
// sound math as the PC game played it on Miles' "Fast 2D Positional Audio" (the 3D model, the 2D pan law, the volume
// scales, s_volume's law), the decompiled driver's own curves, and the loader's IMA ADPCM decoder. C and no engine
// header: port/sndtest.c checks every function against the plan's numbers. 9e's open decisions are each one constant
// or one function here; the user's listening kept every default (session 61). Headphone 3D's direction (9f) too.

#pragma once

#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SNM_CEILING  (127.f / 84.f) // A2: Miles' sample volume is 0-127 and the game's 1.0 is 84: the loudest a voice gets
#define SNM_SCALE_2D (84.f / 127.f) // A3: 2D voices against 3D ones, the decompiled scales (kept at 9e)
#define SNM_BEHIND   0.70794578f    // C6: Fast 2D's -3 dB straight behind the listener (kept at 9e)
#define SNM_EDGE     0.05f          // B1: the fade to silence before the max distance, as a part of it
#define SNM_NEAR     1.f            // 9f: closer than this a 3D voice has no direction (at the listener: the centre)

/*
==============
SNM_Volume

A2: a voice's content volume (the alias's, the script's, the server's): negative means 1 (the callers' default),
clamped to Miles' ceiling, so no voice is flattened below it (openal-soft clamped every source at 1.0)
==============
*/
static inline float SNM_Volume(float volume)
{
    if (volume < 0) {
        return 1;
    }
    return volume < SNM_CEILING ? volume : SNM_CEILING;
}

/*
==============
SNM_Master

A4: s_volume's law, applied once on the master gain: s_volume cubed times the fade, the loudness OpenAL played
(its listener gain s_volume squared times each voice's s_volume), so nobody's game gets louder or quieter at the
switch, and the slider and the fades now reach the voices already playing (the law kept at 9e)
==============
*/
static inline float SNM_Master(float volume, float fade)
{
    if (!(volume > 0)) {
        return 0;
    }
    if (volume > 1) {
        volume = 1;
    }
    if (!(fade > 0)) {
        return 0;
    }
    return volume * volume * volume * (fade < 1 ? fade : 1);
}

/*
==============
SNM_Distance

B1: Fast 2D's distance gain for 3D voices: min / d (1 inside the min distance), silent beyond the max distance
(Miles' default since 6.5a: "muted at the maximum distance"), the last SNM_EDGE of the max distance a linear fade
to it so a voice crossing it doesn't step. OpenAL held min / max beyond it, never silent.
==============
*/
static inline float SNM_Distance(float d, float minDist, float maxDist)
{
    float gain, edge;

    if (!(d < maxDist)) {
        return 0;
    }
    gain = d > minDist ? minDist / d : 1;
    edge = maxDist * SNM_EDGE;
    if (d > maxDist - edge && edge > 0) {
        gain *= (maxDist - d) / edge;
    }
    return gain;
}

/*
==============
SNM_PowerPan

The power pan both of Miles' pan laws use: x 0 = hard left, 0.5 = the centre (-3 dB a side), 1 = hard right
==============
*/
static inline void SNM_PowerPan(float x, float *left, float *right)
{
    const float angle = (x < 0 ? 0 : x > 1 ? 1 : x) * 1.5707963f;

    *left  = cosf(angle);
    *right = sinf(angle);
}

/*
==============
SNM_Pan2D

C1: Miles' 2D pan (AIL_set_sample_pan: 0 hard left, 64 the centre, 127 hard right; the decompiled driver's values
from s_separation) as the power pan MSS 6.5a made "compatible with DLS": the General MIDI 2 law, (pan - 1) / 126 of
a quarter turn, so 64 is the exact centre. OpenAL put it on a half circle behind the head, at most +-30 degrees.
==============
*/
static inline void SNM_Pan2D(int pan, float *left, float *right)
{
    SNM_PowerPan((pan > 1 ? pan - 1 : 0) / 126.f, left, right);
}

/*
==============
SNM_Balance2D

C1 for a stereo sound (the MP3 ambiences): the pan as a balance, the same law times sqrt 2 up to 1 a side, so the
centre leaves both channels at their level (OpenAL played stereo sounds straight to the two sides) and a pan to one
side fades the other channel out
==============
*/
static inline void SNM_Balance2D(int pan, float *left, float *right)
{
    float l, r;

    SNM_Pan2D(pan, &l, &r);
    *left  = pan > 64 ? l * 1.41421356f : 1;
    *right = pan < 64 ? r * 1.41421356f : 1;
}

/*
==============
SNM_Pan3D

C6: Fast 2D's pan for 3D voices: the same power pan by the lateral part of the direction (straight ahead -3 dB a
side, fully to one side all on that side), then up to SNM_BEHIND as the source goes behind the listener (Fast 2D
lowered sounds behind by 3 dB; the cut grows with how much it is behind, so nothing steps at the sides).
front, left: the unit direction from the listener to the source in its frame; 0, 0 (at the listener, overhead) is
the centre. OpenAL's stereo decoder put the front centre at -4.5 dB a side.
==============
*/
static inline void SNM_Pan3D(float front, float left, float *gainLeft, float *gainRight)
{
    SNM_PowerPan((1 - left) * 0.5f, gainLeft, gainRight);
    if (front < 0) {
        const float behind = 1 - (1 - SNM_BEHIND) * (front > -1 ? -front : 1);

        *gainLeft *= behind;
        *gainRight *= behind;
    }
}

/*
==============
SNM_Direction

9f, headphone 3D: where a 3D voice is for Apple's spatial mixer, which renders it in place of SNM_Pan3D (its HRTF has
its own front and back, up and down; SNM_Distance stays). front, left, up: the unit direction from the listener to
the source in its frame. Azimuth in degrees, 0 ahead, positive to the right; elevation in degrees, positive up.
==============
*/
static inline void SNM_Direction(float front, float left, float up, float *azimuth, float *elevation)
{
    *azimuth   = atan2f(-left, front) * 57.2957795f;
    *elevation = asinf(up < -1 ? -1 : up > 1 ? 1 : up) * 57.2957795f;
}

/*
==============
SNM_Linear2D

B4: the decompiled linear falloff of 2D voices with an origin (S_*_Respatialize)
==============
*/
static inline float SNM_Linear2D(float d, float minDist, float maxDist)
{
    if (d <= minDist + 0.001f) {
        return 1;
    }
    if (d >= maxDist - 0.001f) {
        return 0;
    }
    return 1.f - (d - minDist) / (maxDist - minDist);
}

/*
==============
SNM_LoopLevel

B5: the decompiled level of a looping sound (S_*_AddLoopSounds): inverse square between the min and max distances,
silent beyond (on top of it a 3D loop gets SNM_Distance, a panned 2D loop min / d: the original's (min / d)^3)
==============
*/
static inline float SNM_LoopLevel(float d, float minDist, float maxDist)
{
    if (d <= minDist) {
        return 1;
    }
    if (d >= maxDist) {
        return 0;
    }
    return minDist * minDist / (d * d);
}

/*
==============
SNM_DecodeIMA

H2: IMA ADPCM (WAVE format 17, Microsoft's block layout) to 16-bit PCM: each block starts with a header per
channel (the first sample, the step index), then 4-bit codes, low nibble first, interleaved by 4 bytes a channel
when stereo. The last block may be cut short. Returns the frames written, at most maxFrames (the fact chunk's
count when the file has one).
==============
*/
static inline int SNM_DecodeIMA(
    const unsigned char *data, int bytes, int blockAlign, int channels, short *out, int maxFrames
)
{
    static const signed char indexTable[16]
        = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
    static const short stepTable[89] = {
        7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
        31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
        130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
        544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
        2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
        9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
    };
    int frames = 0;

    if (channels < 1 || channels > 2 || blockAlign < 4 * channels || bytes <= 0 || maxFrames <= 0) {
        return 0;
    }

    while (bytes >= 4 * channels && frames < maxFrames) {
        const int            size = bytes < blockAlign ? bytes : blockAlign;
        const unsigned char *code = data + 4 * channels;
        // the codes the block holds, whole groups of 4 bytes a channel
        const int groups = (size - 4 * channels) / (4 * channels);
        int       predictor[2], index[2];
        int       c, g, k;

        for (c = 0; c < channels; c++) {
            predictor[c] = (short)(data[4 * c] | (data[4 * c + 1] << 8));
            index[c]     = data[4 * c + 2] > 88 ? 88 : data[4 * c + 2];
            out[frames * channels + c] = (short)predictor[c];
        }
        frames++;

        for (g = 0; g < groups && frames < maxFrames; g++) {
            const int n = maxFrames - frames < 8 ? maxFrames - frames : 8;

            for (c = 0; c < channels; c++) {
                const unsigned char *group = code + (g * channels + c) * 4;

                for (k = 0; k < n; k++) {
                    const int nibble = (group[k >> 1] >> ((k & 1) * 4)) & 15;
                    const int step   = stepTable[index[c]];
                    int       diff   = step >> 3;

                    if (nibble & 1) {
                        diff += step >> 2;
                    }
                    if (nibble & 2) {
                        diff += step >> 1;
                    }
                    if (nibble & 4) {
                        diff += step;
                    }
                    if (nibble & 8) {
                        diff = -diff;
                    }
                    predictor[c] += diff;
                    predictor[c] = predictor[c] < -32768 ? -32768 : predictor[c] > 32767 ? 32767 : predictor[c];
                    index[c] += indexTable[nibble];
                    index[c] = index[c] < 0 ? 0 : index[c] > 88 ? 88 : index[c];
                    out[(frames + k) * channels + c] = (short)predictor[c];
                }
            }
            frames += n;
        }

        data += size;
        bytes -= size;
    }

    return frames;
}

/*
==============
SNM_IMAFrames

The frames SNM_DecodeIMA writes for bytes of data (a cut last block included)
==============
*/
static inline int SNM_IMAFrames(int bytes, int blockAlign, int channels)
{
    int frames, rest;

    if (channels < 1 || channels > 2 || blockAlign < 4 * channels || bytes <= 0) {
        return 0;
    }
    frames = bytes / blockAlign * ((blockAlign - 4 * channels) / (4 * channels) * 8 + 1);
    rest   = bytes % blockAlign;
    if (rest >= 4 * channels) {
        frames += (rest - 4 * channels) / (4 * channels) * 8 + 1;
    }
    return frames;
}

#ifdef __cplusplus
}
#endif
