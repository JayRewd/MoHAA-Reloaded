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

// snd_native_new.h -- the native sound driver (snd_native_new.cpp, docs/METAL_PORT_PLAN.md Phase 9)

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

qboolean S_NATIVE_Init();
void     S_NATIVE_Shutdown();
void     S_NATIVE_StartSound(
        const vec3_t vOrigin,
        int          iEntNum,
        int          iEntChannel,
        sfxHandle_t  sfxHandle,
        float        fVolume,
        float        fMinDist,
        float        fPitch,
        float        fMaxDist,
        qboolean     bStreamed
    );
void S_NATIVE_AddLoopingSound(
    const vec3_t vOrigin,
    const vec3_t vVelocity,
    sfxHandle_t  sfxHandle,
    float        fVolume,
    float        fMinDist,
    float        fMaxDist,
    float        fPitch,
    int          iFlags
);
void S_NATIVE_ClearLoopingSounds();
void S_NATIVE_StopSound(int iEntNum, int iEntChannel);
void S_NATIVE_StopAllSounds(qboolean bStopMusic);
void S_NATIVE_Respatialize(int iEntNum, const vec3_t vHeadPos, const vec3_t vAxis[3]);
void S_NATIVE_SetReverb(int iType, float fLevel);
void S_NATIVE_Update();

const char  *S_NATIVE_GetMusicFilename();
int          S_NATIVE_GetMusicLoopCount();
unsigned int S_NATIVE_GetMusicOffset();

// The front end's (snd_dma_new.cpp): a sound's data was loaded, and is about to be freed
void S_NATIVE_SfxLoaded(sfx_t *pSfx);
void S_NATIVE_FreeSfx(sfx_t *pSfx);

#ifdef __cplusplus
}
#endif
