# OpenMoHAA native Metal port: plan

Goal (verbatim, see CLAUDE.md): "Create a FULL macOS Silicon Metal native port of openmohaa, with the ultimate goal of stripping OpenGL entirely from the code by the time we're done, and only have it run Metal from start to finish. Look at all possible ways to improve performance while we do it, increase efficiency, etc. Make it as lean and robust as possible."

Branches: `main` = upstream mirror (openmoh/openmohaa a2f34019 at the start). `metal` = this work.

## Status

| Phase | What | Status |
|---|---|---|
| 0 | Survey, baseline, plan, decisions | **Done** (session 1) |
| 1 | Build plumbing + Metal window layer + `renderer_metal` skeleton | **Done** (session 2; the 4K@2× HiDPI check is open: display not connected) |
| 2 | Core draw path (textures, pipelines, 2D/UI, world, entities) | **In progress:** 2a done (session 3); next 2b. The 4K@2× HiDPI check is still open (display not connected) |
| 3 | Full feature parity + screenshot-compare harness + online smoke test | Not started |
| 4 | Delete OpenGL entirely | Not started |
| 5 | Performance and input latency | Not started |
| 6 | Native AppKit/GameController (replace SDL2) | Not started |
| 7 | Lean and robust (strip platforms, app bundle, openal-soft, dead code) | Not started |

---

## 1. Findings (session 1 survey)

Paths are relative to `code/` unless they start with `cmake/`.

### 1.1 Build and targets
- CMake ≥ 3.25, Ninja. Options in `CMakeLists.txt:14-40`: `BUILD_RENDERER_GL1` ON, `BUILD_RENDERER_GL2` OFF, `USE_RENDERER_DLOPEN` ON, `USE_OPENAL(_DLOPEN)` ON, `BUILD_SERVER` ON, `BUILD_MACOS_APP` ON (`cmake/platforms/macos.cmake:7`).
- Targets: client `openmohaa` (.app bundle), server `omohaaded`, `renderer_opengl1.dylib` (moved into `openmohaa.app/Contents/MacOS` by `finish_macos_app`), `cgame.dylib`, `game.dylib`, plus launchers (broken on macOS: they read `/proc/self/exe`).
- Install layout: everything goes to `<prefix>/lib/openmohaa/`. `fs_basepath` defaults to the folder that holds the .app. Libraries are found in `Contents/MacOS`, then in `fs_basepath` (`sys/sys_main.c:511`).
- `cmake/platforms/macos.cmake:17-18` set **deployment target 11.0 and universal arm64;x86_64** as plain variables, which override `-D` flags. Phase 1 changed them to **26.0 and arm64** (session 2). Every renderer gets `-framework OpenGL` (`:15`); `cmake/renderer_metal.cmake` removes it again for the Metal renderer.
- `find_package(OpenAL)` picks Apple's OpenAL.framework, which has no `alext.h`, and the build fails. `port/build.sh` passes `-DCMAKE_DISABLE_FIND_PACKAGE_OpenAL=ON` (internal openal-soft headers; the library is dlopen'ed as `libopenal.1.dylib`, `client/snd_openal_new.cpp:124`). Nothing builds openal-soft locally, so `port/run.sh` links Homebrew's next to the exe.
- `cmake/libraries/jpeg.cmake:1` and `freetype.cmake:5` only worked when a GL renderer is built (widened for `BUILD_RENDERER_METAL` in session 2). There is also a `JPEG_DEFINTIONS` typo at `jpeg.cmake:22`; left as is, because fixing it adds `USE_INTERNAL_JPEG`/`JPEG_INTERNALS` to GL1's compile.
- Flex/Bison output is written into the source tree (`code/parser/generated`, gitignored). CTest finds no tests.
- Internal SDL is a prebuilt universal `thirdparty/libs/macos/libSDL2-2.0.0.dylib` (2.32.8). zlib, jpeg-9f, ogg, vorbis, opus, libmad are compiled from `thirdparty/`.

### 1.2 Renderer interface
- The real header is `renderercommon/tr_public.h`. `renderergl1/tr_public.h` is dead (`#if 0`). `REF_API_VERSION` is 14.
- `refexport_t` has 85 entries. Groups: frame/scene, 2D (`DrawStretchPic`, `DrawTilePic`, `DrawStretchRaw`, `Set2DWindow`, `Scissor`, `DrawBox`, `DrawLineLoop`, …), TIKI/model (`RegisterModel`, `LerpTag`, `ForceUpdatePose`, `TIKI_Orientation`, swipes, …), fonts (`LoadFont` .RitualFont, `DrawString`), images/shaders, MOHAA world queries (`LightForPoint`, `GetLightingForDecal`, `MarkFragments`, `inPVS`, …). `GetRefAPI` is at `renderergl1/tr_init.c:1847`.
- **Upstream bug:** `SwipePoint` and `DrawBackground` are never assigned and stay NULL, though cgame calls `re.SwipePoint` (`cgame/cg_swipe.cpp:221`). The renderer implements `RE_SwipePoint` (`tr_swipe.cpp:88`).
- `refimport_t` has 87 functions + 4 data pointers, including TIKI/skeletor imports (`TIKI_SetPoseInternal`, `GetFrameInternal`, `SKEL_GetMorphWeightFrame`, …).
- Loading: `CL_InitRef` (`client/cl_main.cpp:3190`). The `cl_renderer` cvar (default `opengl1`, ARCHIVE|LATCH) selects `renderer_<name>.dylib`, falling back to the default. Symbol `GetRefAPI`.
- `glconfig_t` (`renderercommon/tr_types.h:212`) is copied field by field into cgame (`client/cl_cgame.cpp:49-73`), so **its layout is ABI and must not change**. Fields actually read: `vidWidth/vidHeight` (HUD, UI, input), `isFullscreen`, `stereoEnabled`, `renderer_string`, `hardwareType`/`maxTextureSize` (cinematics).

### 1.3 renderergl1 (what gets ported; ~43k lines)
- **The renderer owns the window**: `sdl/sdl_glimp.c` and `sdl/sdl_gamma.c` are compiled into the renderer. `GLimp_SetMode` creates the SDL window + GL context, and `GLimp_Init` calls `ri.IN_Init(SDL_window)` (`sdl_glimp.c:1179`).
- qgl is loaded at runtime (`SDL_GL_GetProcAddress`, `sdl_glimp.c:262`) through X-macros in `renderercommon/qgl.h`. About 73 distinct GL entry points are used. qgl lines per file: tr_backend 142, tr_shade 124, tr_draw 66, tr_shadows 59, tr_sphere_shade 44 (debug lines), tr_init 39, tr_main 28, tr_image 28, tr_model 25, tr_surface 19, tr_sky 17, tr_cmds 14, tr_flares 11, tr_sun_flare 9, sdl_glimp 60. Terrain, BSP, world, font, util, static models and marks have none.
- Single-threaded command queue (no SMP). `GL_State` (`tr_backend.c:220`) maps `GLS_*` bits (blend, depth, alpha test incl. foliage thresholds, fog) to GL state. Other key functions: `GL_Bind`, `GL_SelectTexture`, `GL_TexEnv`.
- **2D is immediate, not queued.** `Draw_*`/`Set2DWindow` (`tr_draw.c`) call `R_IssuePendingRenderCommands()` and then draw at once (Set2DWindow calls `glViewport`/`glOrtho` directly). The UI calls Set2DWindow around the 3D view. A Metal backend must be able to encode at any point in the frame.
- `tr_shade.c`: client-side vertex arrays from `tess` (positions as padded vec4). Per stage, the CPU fills `tess.svars` (`ComputeTexCoords`/`ComputeColors`, deforms/tcMods in `tr_shade_calc.c`), then issues one draw. Two TMUs max (`DrawMultitextured` + `GL_TexEnv`). `MAX_SHADER_STAGES` 8, `SHADER_MAX_VERTEXES` 2048.
- **`R_DrawElements` (`tr_shade.c:163`): `r_primitives 0` picks glDrawElements only if `GL_EXT_compiled_vertex_array` exists. Apple's GL doesn't have it, so the default path is `R_DrawStripElements` with `glArrayElement` inside `glBegin`/`glEnd` (immediate mode). `r_primitives 2` forces glDrawElements, and the user plays with it.**
- `tr_shader.c`: MOHAA keywords (`nextBundle`, `animMapOnce/Phase`, `ifCvar`, `#if` blocks for `r_textureDetails`, `portalsky`, `spritegen`, `force32bit`, `clampTime`, `lightglow` deform; rgbGen `lightingSpherical`; alphaGen `tikiDistFade`, `heightFade`, `dotView`, `skyAlpha`; tcGen `environmentmodel`, `sunreflection`; tcMod `wavetrans`, `parallax`, `macro`; `nofog`).
- `tr_image.c`: its own TGA/JPG/BMP/PCX/DDS/GST loaders. `Upload32` resamples to power of two, applies picmip, generates mipmaps on the CPU (`R_MipMap`) and always uploads RGBA8. DDS uploads as S3TC (off by default). Gamma/overbright: `R_SetColorMappings` (`:2758`) enables overbright only with hardware gamma **and** fullscreen. Otherwise `R_LightScaleTexture` bakes gamma into the texels.
- Sky: Q3 skybox in immediate mode (`tr_sky.c`) and the MOHAA portal sky (`tr_sky_portal.cpp`, re-runs `R_RenderView`). Flares: Q3 flares read depth with `glReadPixels` (off by default); MOHAA sun/lens flares draw orthographic quads (`tr_sun_flare.cpp`).
- Fog: fixed-function **linear distance fog only** (farplane), with `GLS_FOG*` bits for view/black/white fog color. No Q3 fog volumes. Mirrors/portals: `glClipPlane`.
- Dlights: baked on the CPU into copies of the lightmaps and re-uploaded with `glTexSubImage2D` (`tr_light.c`, `R_UploadDlights`). Entities get dlights through sphere lighting (`tr_sphere_shade.cpp`, CPU per-vertex light-grid + light lighting).
- Also: `tr_terrain.c` (ROAM-style LOD terrain with CPU split/merge and geomorph), `tr_staticmodels.cpp` (static TIKIs skinned once at load), `tr_ghost.cpp` (CPU-animated textures re-uploaded each frame), `tr_marks_permanent.c` (`.dcl` decals), `tr_shadows.c` (stencil shadow volumes, immediate mode), `tr_font.cpp` (RitualFont through tess), `tr_util.cpp` (immediate-mode emulation into tess). Screenshots read back with `glReadPixels(GL_RGB)` (`tr_init.c:498`). Cinematics go through `RE_StretchRaw` (`tr_backend.c:1008`), which calls `qglFinish` and uploads each frame.
- `renderergl2` is maintained upstream but off by default. It also skins on the CPU. Not used by this port.

### 1.4 GL outside the renderer
- None in client/cgame/fgame/uilib. They draw only through `re.*`/`cgi.*`/`uii.*`. The leaks are `renderercommon/qgl.h` (includes `SDL_opengl.h`), `tr_common.h` (`#include "qgl.h"`, `image_t.texnum` is a `GLuint`, GLimp declarations), `new/tr_types_new.h` (`OPENGL_DRIVER_NAME`), and `sdl_glimp.c`/`sdl_gamma.c`. `tools/` and the gamespy samples have GL but aren't built.
- `cl_avi.cpp` video capture is effectively disabled. `CL_RefCIN_UploadCinematic` is an empty stub, so in-world videoMaps are unsupported upstream.

### 1.5 TIKI / skeletor
- `skeletor/*` and `tiki/*` are compiled into the client and server only. The renderer compiles just `tiki/tiki_mesh.cpp` (bone/morph caches) and reaches the rest through `ri.TIKI_*`/`ri.SKEL_*`.
- Skinning is all on the CPU: `R_AddSkelSurfaces` (`tr_model.cpp:720`) → `ri.GetFrameInternal` → `skeletor_c::GetFrame` → bone matrices in `skelBoneCache_t` → `RB_SkelMesh` (`tr_model.cpp:1022`) skins into `tess.xyz/normal` in the backend. Vertices are variable-length (`skeletorVertex_t` + morphs + weights, no fixed cap on weights). Normals use the first bone only. `TIKI_MAX_BONES` 100, `TIKI_MAX_VERTEXES` 1000. LOD uses progressive-mesh collapse maps.

### 1.6 SDL, input, audio
- SDL2 only (2.32.8). Video and window in the renderer. Input in the client (`sdl/sdl_input.c`: keyboard, text input, relative mouse + grab, joystick/gamecontroller, window events, DROPFILE for the `openmohaa://` URL). Colour cursor in `sdl_mouse.c`. Clipboard. `sdl_snd.c` is not compiled (audio doesn't use SDL). No HiDPI support (`SDL_WINDOW_ALLOW_HIGHDPI` unused, no `NSHighResolutionCapable`).
- Audio: the active system is the MOHAA `*_new.cpp` one on OpenAL only (`NO_MODERN_DMA` undefined); there is no fallback. 3D positional sources, 16-buffer streaming for music/dialog/movies (polled, no thread), ALC_SOFT HRTF/limiter/resampler extensions. EFX reverb is a FIXME no-op. About 7.1k lines of ioq3 sound code are present but not compiled.

### 1.7 Other platforms
- Windows only: `sys/sys_win32.c`, `con_win32.c`, `sys/new/sys_win32_new.c`, .rc/manifest. `win_bounds.cpp` and `win_localization.cpp` are built everywhere despite their names. `asm/` is x86 only and not built on clang. `null/` is used by the dedicated server. `Launcher/` doesn't work on macOS. `sys_osx.m` has only `Sys_Dialog` (NSAlert) and `Sys_StripAppBundle`; it imports Carbon without using it, and IOKit is linked but unused. gamespy is 178k lines (only common/gcdkey/natneg/qr2 are built). Platform `#ifdef` density is low.

### 1.8 Ideas from the user's Vulkan port (pstngh/openmohaa-vulkan, branch `vulkan-renderer`; ideas only, no code copied)
- `code/renderervk` is a copy of renderergl1 with every GL call replaced by a GL-free backend API that mirrors GL1's fixed-function state (`GLS_*` bits, texenv, cull, depth range, polygon offset, fog, clip plane, stencil, matrices, viewport/scissor, draw/clear/readback, texture create/upload/destroy). With that, the `tr_*` code barely changes and upstream renderer fixes keep porting. They added an immediate-mode emulator (`tr_immediate.c`), GPU static geometry and GPU skinning.
- Outside the renderer they changed only CMake (new renderer target, jpeg/freetype guards, macos.cmake bundle placement), `tr_common.h` (skip qgl.h, `texnum` → `unsigned int`), the `cl_renderer` default, and a new `sdl_vkimp.c` window file. The refexport/refimport ABI is unchanged.
- Gotchas they recorded:
  - Hidden visibility (only `GetRefAPI` exported) was their biggest single CPU win.
  - Keep GL projection matrices and remap clip z; mind the bottom-left window origin for viewport/scissor/readback; keep CCW front faces.
  - Reproduce the exact alpha-test thresholds, "no destination alpha" (RGB textures sample alpha = 1), Mesa-style fog, clamping stencil ops.
  - Never reuse texture handles while frames are in flight. Uploads mid-frame (cinematics, ghosts) must not change already recorded draws. Free GPU objects only after the frame's fence.
  - Frames start lazily (2D exports flush render commands). Take screenshots at end of frame (GL1 read a stale back buffer).
  - Capability flags come from the same `r_ext_*` cvars so shader collapsing matches. A default stencil of 0 bits disables `r_shadows 2`.
  - Deterministic screenshot testing: `devmap`, `fixedtime 16`, `thereisnomonkey 1`, `tele`/`face`, `pause` before `screenshot`; delete `main/configs` between runs; GL1 sometimes saves an all-black frame.

---

## 2. Baseline (upstream GL1, a2f34019 + the timedemo work metric)

Machine: Mac mini M4 (4P+6E, 10-core GPU, 16 GB), macOS 26.6.2, Xcode 27.1. Display: DELL G2524H 1920×1080 @ 240 Hz (main) + DELL U4323QE 4K at 2× (second).

**Build** (`port/build.sh gl`): Release, ThinLTO, universal arm64+x86_64 (forced by macos.cmake), deployment target 11.0. 85 s on 10 cores. GL context reports "2.1 Metal - 90.5": Apple's GL runs on Metal internally.

**arm64-only rebuild** (session 2, after Phase 1 set arm64 + macOS 26.0): build 36 s. Same bench, same binary, two sets: 937.1 / 941.1 / 941.1 fps = **0.896 / 0.895 / 0.893 ms work**, and later 909.4 / 925.5 / 912.5 fps = 0.954 / 0.922 / 0.941 ms. GL1 from the combined `metal` install (`cl_renderer opengl1`): 0.901 / 0.943 / 0.940 ms. So the arm64-only GL1 reference is **~0.90–0.95 ms work/frame**; between sets the noise is about ±3% (within a set ±0.3%), larger than session 1 measured. Compare against GL1 runs made in the same sitting.

**Benchmark** (`port/bench.sh run gl 3`): timedemo of `port/bench/bench_mohdm6.dm_8` (dm/mohdm6 Stalingrad, FFA, 8 bots, spectator following bots, 120 s, 2398 frames; sha256 `c62f7972735b60c15b32392168f41235849407b637c9ac6004d68a36312320a2`), 1280×720 window on the main display, `r_swapInterval 0`, `com_maxfps 0`, sound muted, fresh default config each run.

| Config | fps (3 runs) | wall ms/frame | **work ms/frame** | GPU ms/frame (`trace`) |
|---|---|---|---|---|
| GL1 `r_primitives 2` (**reference**: the user's setting, GL's fastest path) | 923.4 / 925.9 / 926.2 | 1.081 | **0.934** (limiter wait 0.146) | **0.18** (16% GPU utilization in the timedemo) |
| GL1 `r_primitives 0` (upstream default) | 293.2 / 296.3 / 295.9 | 3.39 | 3.39 | ~0.6 (older estimate method) |

- **"work" is new.** The engine never runs a frame shorter than 1 ms: since Quake 3, `Com_Frame` pads frames to its 1 ms minimum with an integer-ms clock. So fps saturates near 1000 (with `com_maxfps 0` or in a timedemo; `com_maxfps` above 1000 removes the floor but makes the game clock run fast, see §4b rule 7), and GL with `r_primitives 2` already sits there. The timedemo now also prints `timedemo work: X ms/frame` (wall time minus Com_Frame's frame-limiter wait, µs clock; `qcommon/common.c` `com_timedemoWaitUsec`, printed in `client/cl_main.cpp CL_DemoCompleted`). **Compare renderers on work ms/frame.** It only measures; it changes nothing in how frames run.
- Run-to-run noise: ±0.3% for `r_primitives 2` and ~3% for `r_primitives 0` (one set gave 286–294 fps). Under `sample` the run is ~4% slower, under xctrace ~5–10% slower.
- **Where the GL time goes** (`sample` profiles; `port/sample_by_library.py`):
  - `r_primitives 2`: the frame is dominated by `RB_SwapBuffers`. It calls `qglFinish()` every frame (`tr_backend.c:1429`), and the CPU blocks in `GLDShareGroupRec::waitUsage` until the GPU is done, then `CGLFlushDrawable` presents. The GPU is busy only 0.18 ms of each ~1 ms frame, so most of the frame is CPU↔GPU **synchronization latency**, not rendering. The engine's own CPU work is small.
  - `r_primitives 0`: ~1.96 ms/frame in `RB_SwapBuffers` (≈half glFinish wait, half present) and ~1.7 ms/frame drawing the 3D view/HUD (`UIWindowManager::UpdateViews`). Of the latter, ~65% is Apple's GL driver (AGX, AppleMetalOpenGLRenderer, GLEngine, malloc churn from immediate mode) and ~13% renderer code (top: `RB_Light_Real` 4.7%, `RB_SkelMesh` 2.4%, `R_DrawStripElements` 1.3%).
  - Apple's GL splits every game frame into ~8–10 GPU submissions (render passes): 23806 trace frames for ~2400 game frames. On a tile-based GPU each extra pass costs tile loads/stores.
- **Metal, Phase 1** (session 2; draws are still stubs, so this is the floor of front end + backend frame overhead, not comparable with GL1): `port/bench.sh run metal 1 +set cl_renderer metal`: 998.8 fps, **0.318 ms work/frame**, no wait for drawables or GPU. With `r_swapInterval 1`: 239.1 fps, the ~3.5 ms vsync wait sits at the end of the frame (before the next input sample).
- **Metal, Phase 2a** (session 3, 2026-10-17; the whole renderer now draws, see Phase 2's result). Same sitting, `port/bench.sh run gl 3` and `port/bench.sh run metal 3 +set cl_renderer metal`, **with the screen locked** (`CGSSessionScreenIsLocked`: nothing is composited, so GL1's swap is cheaper than in §2's table and Metal gets a drawable for only ~1 frame in 15; re-measure both unlocked):

  | Config | fps (3 runs) | **work ms/frame** | GPU ms/frame |
  |---|---|---|---|
  | GL1 `r_primitives 2` | 975.2 / 982.0 / 981.2 | **0.766 / 0.759 / 0.765** | (no trace) |
  | Metal, 2 frames in flight (default) | 1000.0 / 1000.0 / 1000.0 | **0.359 / 0.364 / 0.361** | 0.11 (frame command buffer GPU time, `r_metalPresentStats 1`) |
  | Metal, `r_metalFramesInFlight 1` | 998.3 | 0.707 (0.31 ms/frame waiting for the GPU, after the commit) | |
  | Metal, `r_metalFramesInFlight 3` | 1000.0 | 0.361 | |

  - **2.1× less work per frame than GL1, under the Phase 2 target (≤ 0.45 ms).** fps is pinned at the engine's 1 ms floor.
  - CPU profile (`sample`, 8 s of real-time demo playback): 74% `select` + 7.6% `mach_absolute_time` = the engine's frame limiter polling. Of the busy time: renderer front end ≈ 9% of all samples (`RB_SkelMesh` 2.7%, `RB_Light_Real` 1.9%, `RB_SurfaceFace` 0.8%), Metal driver (AGX + IOGPU) ≈ 2%, ring copies (`memmove`) 0.9%, the backend's own functions ≈ 0.2%. GL1 spent ~65% of its busy time in Apple's GL driver.
  - Two measured traps, both fixed (see §3.2 "Added in session 3"): with the present in the frame's command buffer, completion came only at the display's next refresh while the screen was locked (runs 1 and 3 ran at 239 fps, 3.2 ms/frame waiting for a frame slot); and completed handlers arrived ~1.3 ms after the GPU had finished (0.97 ms work). Frame completion now comes from an `MTLSharedEvent`.
- **What this means for Metal:** with frames in flight (no per-frame wait), one render pass per frame where possible, and no driver state validation, the frame cost should approach max(CPU, GPU) instead of their sum. **Phase 2 target: work ≤ 0.45 ms/frame (≥2× GL1 `r_primitives 2`) on this bench. Phase 5 stretch: ≤ 0.25 ms.** Beyond ~1000 fps the visible fps stays capped by the engine's 1 ms frame floor (see Phase 5).

---

## 3. Decisions

### 3.1 User decisions (asked in session 1, 2026-10-16)
1. **Windowing/input: SDL2 now, native later.** Phases 1–5 keep SDL2 and attach a `CAMetalLayer` via `SDL_Metal_CreateView`. Phase 6 replaces SDL with native AppKit + GameController (GCMouse raw deltas), decided after measuring input latency.
2. **Audio: keep OpenAL-soft.** MOHAA's OpenAL sound code stays. Phase 7 bundles openal-soft in the .app, linked directly instead of dlopen'ed. A native CoreAudio/AVAudioEngine backend is only an option if profiling ever shows a reason.
3. **Platforms: macOS-only, keep the server.** Delete Windows/Linux/Emscripten/x86 asm/Launcher platform code and the GL renderers. Keep the dedicated server target (`omohaaded`) building on macOS for local tests. The user's Linux servers keep using upstream builds; `main` stays the upstream mirror.
4. **Shaders: offline .metallib.** CMake compiles the hand-written `.metal` files into one metallib at build time with the read-only mounted Metal Toolchain. At run time an `MTLBinaryArchive` pipeline cache avoids compile hitches.
5. **Push: yes.** Push `metal` to origin at the end of every phase (recorded in CLAUDE.md).
6. **Lowest input latency and overhead** (asked by the user after the plan was written): a standing requirement next to the Goal. See §4b.
7. **Client timing redesign: both parts approved** (2026-10-16): precise frame pacing as the default, and an optional mode with separate command and render clocks that the user tests online before it can become the default. Planned for Phase 5 (§4b). No VRR work (the user isn't interested).

### 3.2 Technical decisions (session 1; revisit only with a reason, tell the user if one changes)
- **renderer_metal starts as a copy of renderergl1** (`code/renderer_metal/`, front end unchanged), with every qgl call replaced by a small GL-free backend API (`tr_mtl.h`, C) that mirrors GL1's state model, as in the Vulkan port. Keeps visual parity, keeps upstream renderer fixes portable (by hand, file for file), and lets GL1 run side by side for A/B until Phase 4. GL-isms are removed later where it pays (Phase 5).
- **Backend language: Objective-C with ARC** (`.m` files) behind the C API. No metal-cpp, no Swift. The front end stays C/C++.
- **Classic Metal API first** (`MTLCommandQueue`, `MTLRenderCommandEncoder`, automatic hazard tracking). Metal 4 (`MTL4CommandQueue`, argument tables, residency sets) is evaluated in Phase 5 where it measurably helps.
- **The metallib is embedded in the renderer binary** (generated C array), so there's no file lookup at run time. Compiled with `-std=metal4.0 -target air64-apple-macos26.0 -O3` (verified working with the mounted toolchain in session 1).
- **Renderer packaging:** a dylib selected with `cl_renderer metal` during Phases 1–3 (A/B against `opengl1`). In Phase 4 it's linked statically into the client (`USE_RENDERER_DLOPEN OFF`: one binary, LTO across client+renderer, no PLT). Hidden visibility from day one.
- **Deployment target macOS 26.0, arm64 only** (Phase 1 edits `cmake/platforms/macos.cmake`, a tiny diff).

Added in session 2 (Phase 1):
- **Backend API shape (`renderer_metal/tr_mtl.h`):** the GL1 fixed-function state model with GL-free names: every `qglFoo` call became `mbFoo` (same arguments), every `GL_FOO` constant `MB_FOO`, and GL types plain C types (a perl pass outside string literals, then hand fixes). `MB_*` values equal the GL enums, so shader/texture data, decompiled literals and printouts stay identical; they are just numbers. Front-end diffs against renderergl1 stay mechanical (upstream fixes port by reversing the rename). The texenv combiner setups collapsed into modes: `mbTexEnv(MB_MODULATE | MB_REPLACE | MB_DECAL | MB_ADD | MB_COMBINE | MB_COMBINE4_NV)`. GL1's function-pointer checks became `mbMultitexture` (from `r_ext_multitexture`); compiled vertex arrays always exist (`mbLockArraysEXT` = one vertex copy for all stages), so `r_primitives 0` means indexed triangles. GL queries became `glConfig` fields filled by the window layer. `mbGetError` always returns `MB_NO_ERROR`. `glCallList` (display lists, unused) is gone.
- **The pristine copy is its own commit** (`5d6b2dd6`): `git diff 5d6b2dd6 -- code/renderer_metal` shows exactly what the Metal port changed.
- **Drawable acquisition (low latency without throttling the game loop).** Measured: acquiring the drawable inside the frame blocked 1.3 ms per frame in a 1280×720 window (the WindowServer holds drawables until its next 240 Hz composite), and acquiring it at the end of the previous frame blocked 1.9 ms (2 drawables) / 1.25 ms (3). A blocking present caps the engine loop below `com_maxfps` (350–680 fps instead of GL1's ~1000), which changes usercmd timing. So with `r_swapInterval 0` a helper thread (`QOS_CLASS_USER_INTERACTIVE`) waits in `nextDrawable` and keeps one drawable ready. The frame takes it without waiting, or skips its present when none is ready: the compositor couldn't have shown that frame, and GL1's non-blocking swap drops it too. With `r_swapInterval 1` the next drawable is acquired at the end of the frame, after commit, and blocks like GL1's swap. Either way nothing waits between input sampling and present. To revisit with the Phase 2c latency probe.
- **Defaults: 2 drawables, 2 frames in flight** (`r_metalDrawables` 2/3, `r_metalFramesInFlight` 1–3, both LATCH). With 3 drawables more frames get presented (8600 vs 6069 in the bench run) but 1700 of them are never shown; with 2, 6067 of 6069 presented frames were shown.
- **Window size and HiDPI:** `r_mode`/`r_customwidth` give the window size in points; `glConfig.vidWidth/vidHeight` = the drawable in pixels (`SDL_WINDOW_ALLOW_HIGHDPI` + `SDL_Metal_GetDrawableSize`). Identical on the 1080p main display. New `r_displayIndex` picks the display the window opens on.
- **Fullscreen toggle = `vid_restart`.** `r_fullscreen` is LATCH, so Alt+Enter only stores the new value and GL1's in-place toggle in `GLimp_EndFrame` never fires (`->integer` doesn't change before a restart). The Metal window layer issues `vid_restart` when the latched value differs from the current mode.
- **Gamma:** `glConfig.deviceSupportsGamma = !r_ignorehwgamma`; `GLimp_SetGamma` hands the same 256-entry table GL1 loads into the display's hardware ramp to the backend, which applies it in the final pass (also in a window, and without touching the rest of the desktop). Overbright parity per mode is still Phase 3.
- **Capabilities mirror Apple's GL driver** (from GL1's log): texenv add on, multitexture on (8 units reported), S3TC only with `r_ext_compressed_textures`, anisotropy only with `r_ext_texture_filter_anisotropic` (max 16), clamp-to-edge on, max texture size 16384, 24/32/8 color/depth/stencil bits.

Added in session 3 (Phase 2a):
- **Vertices are fetched by hand.** Client arrays are copied as they are (one `memcpy` per array) into the frame slot's shared, untracked `MTLBuffer` ring; the vertex shader reads them by offset and stride from one bound buffer. One `setVertexBytes` (176 bytes: matrices, constant color/texcoords, stream offsets/strides) and one draw call per draw, and pipelines don't depend on vertex layouts. `mbLockArraysEXT` keeps the compiled-vertex-array contract: arrays enabled at the lock are copied once, at the first draw of the lock (Q3 fills colors/texcoords after the lock in single-pass shaders), arrays enabled later are copied per draw. Immediate mode collects vertices (float colors) and turns quads/polygons/line loops into indexed draws.
- **Pipeline specialization is minimal:** function constants only for the sampled texture units (0–2) and alpha test on/off (a `discard` in every pipeline would cost hidden-surface removal on opaque surfaces). Texenv per unit, the alpha function/reference and fog are uniform branches. Pipelines are cached by a 32-bit key (blend factors, RGB write mask, units, alpha test): **12 pipelines for the whole bench**, so first-use compiles are rare (an `MTLBinaryArchive` can still come in Phase 3/5). Depth/stencil states are cached by a 34-bit key, samplers by their GL parameters (stored in the texture, so a draw only compares pointers). Setters only record state and set dirty bits; nothing is looked up by string.
- **Frame completion comes from an `MTLSharedEvent`** the GPU signals with the frame's serial at the end of its command buffer, not from completed handlers (measured ~1.3 ms late, and late until the next refresh when the present is in the same command buffer and the screen is locked). The frames-in-flight wait reads `signaledValue` and sleeps (`waitUntilSignaledValue`) only when the GPU really is behind; it stays at the end of the frame (§4b rule 1).
- **`mbFinish` doesn't wait** (GL1 calls glFinish on every 2D-only frame, with `r_finish 1`, and before cinematic uploads). Queueing is bounded by `r_metalFramesInFlight`, and textures in use are replaced, not overwritten. `r_finish` has no effect on Metal; `r_metalFramesInFlight 1` is the equivalent.
- **Textures:** `MTLStorageModeShared` + `replaceRegion` (no staging copy) when no frame in flight or in this frame uses the texture. Otherwise the name gets a replacement texture (from a 16-entry pool of retired ones): the old contents are copied on the GPU and the new data blitted, in an upload command buffer committed right before the frame's, so draws recorded earlier keep the old image (GL semantics) and nothing waits. Uploads go to RGBA8 (GL1's 16-bit `r_texturebits 16` formats are stored at 32 bits), RGB-like internal formats sample alpha 1 through a texture swizzle (and texenv replace keeps the incoming alpha, as GL does for GL_RGB), DDS to BC1/BC2/BC3. A texture starts with one mip level and gets the full chain at the first mip upload. Deleted textures and names: objects are released after their frames complete; names wait in a FIFO (reused after 1024 frees), and units keep a deleted name bound so a reused name resolves to its new image.
- **No destination alpha:** the scene target's alpha is never written and stays 1 (GL1's 24-bit color buffer: `GL_DST_ALPHA` = 1).
- **Clears:** a full-target clear before the pass begins becomes the pass's load action; any other clear (scissored, color-masked, after drawing) is a triangle through the clear pipeline, honoring the write masks like glClear. Depth/stencil start cleared each frame and are stored only when the pass is interrupted (readback); `MTLStoreActionUnknown` decides at the end.
- **`MB_COMBINE4_NV` keeps the unit's previous texenv**, as on Apple's GL (no `GL_NV_texture_env_combine4`: GL1's call fails with GL_INVALID_ENUM there).
- GL conventions as planned: clip z remapped on the CPU (mvp = remap · P · MV), viewport/scissor flipped, CCW front faces, depth range → viewport znear/zfar, polygon offset → `setDepthBias(units, factor)`, the clip plane converted to object space per draw and written to `[[clip_distance]]`, `[[position, invariant]]` (`-fpreserve-invariance`) for multi-pass `GLS_DEPTHFUNC_EQUAL`. Fog: GL linear fog on the per-fragment |eye z| (Phase 3 checks it against GL1). Not emulated: line width, line stipple, points, flat shading (only debug/editor views and a disabled GL1 path use them).

---

## 4. Architecture of renderer_metal

```
client (cl_main, cgame, uilib)  --refexport_t/refimport_t (unchanged ABI)-->  renderer_metal
  front end (copied from GL1): BSP/world, shaders, images (CPU decode), TIKI/skeletor glue, terrain,
  marks, sky portal, fonts, tess/svars stage computation
        |  tr_mtl.h: GL-free C backend API (state bits, binds, draws, uploads, readback)
  backend (Objective-C, ARC): device/queue, frame ring, encoders, PSO/depth-stencil/sampler caches,
  textures, ring buffers, final pass, screenshots
        |
  sdl/sdl_mtlimp.m: SDL window (SDL_WINDOW_METAL | ALLOW_HIGHDPI) + SDL_Metal_CreateView -> CAMetalLayer
```

**Window layer (`sdl/sdl_mtlimp.m`, compiled into the renderer like sdl_glimp.c):**
- Same mode/fullscreen logic as `sdl_glimp.c` (`r_mode`, `r_customwidth/height`, `r_fullscreen`, `r_noborder`, desktop mode `-2`, live fullscreen toggle, resize → `vid_restart`). Calls `ri.IN_Init(window)`.
- `CAMetalLayer`: `BGRA8Unorm` (not sRGB: Q3 math is gamma-space), `framebufferOnly YES`, opaque, `maximumDrawableCount 2` (`r_metalDrawables`), `displaySyncEnabled = r_swapInterval`, `presentsWithTransaction NO`, `drawableSize` = window pixels.
- HiDPI: drawable size in pixels → `glConfig.vidWidth/vidHeight`. Check the HUD/UI scaling on the 4K@2× display.
- Fills `glConfig` (renderer string "Apple M4 (Metal)", maxTextureSize 16384, stencil 8, no hardware gamma: gamma is applied in-shader).

**Frame model:**
- One `MTLCommandQueue`. 2 frames in flight by default (`r_metalFramesInFlight`; an `MTLSharedEvent` signaled by the GPU per frame, waited on right after commit). One command buffer per frame (`commandBufferWithUnretainedReferences`), begun lazily at the first draw or clear (2D exports draw outside the queue), plus an upload command buffer committed before it when textures in use are rewritten. Drawables: see §3.2 "Drawable acquisition".
- Rendering goes to an offscreen color target (`RGBA8Unorm`, private) + `Depth32Float_Stencil8`. At end of frame one fullscreen triangle copies it to the drawable and applies **gamma + overbright in the shader**, reproducing GL1's behavior per mode (overbright only where GL1 has it: fullscreen with hw gamma). Then present + commit. Never wait on the GPU except at shutdown and readback.
- Screenshots and video frames: blit from the offscreen target (pre-gamma, as GL1's back-buffer read) into a shared buffer, written out in the completion handler (async, no stall).

**Pipelines and shaders (`renderer_metal/shaders/*.metal`):**
- One generic stage shader pair (`shaders/tr_mtl_stage.metal`, session 3): function constants for the sampled texture units and alpha test on/off only; texenv per unit (modulate/replace/decal/add/combine), vertex vs constant color, the alpha function and reference, linear fog and the clip plane (`[[clip_distance]]`) are uniforms. Vertices fetched by hand from the frame ring (see §3.2 "Added in session 3").
- Separate small shaders: final pass (gamma/overbright), partial clears.
- PSO cache keyed by blend factors, RGB write mask, texture units and alpha test (formats are fixed). Depth/stencil state cache keyed by depth func/test/write + stencil func/ops/masks. Sampler cache by filter/mip/wrap/aniso/max LOD. `MTLBinaryArchive` on disk in `fs_homepath` (Phase 3/5).
- GL conventions in the backend: the front end keeps GL projection matrices; the backend remaps clip z from [-1,1] to [0,1] and flips the viewport/scissor origin (bottom-left → top-left). Front-face winding matches GL CCW after the flip. `glDepthRange` (weapon depth hack) → `MTLViewport` znear/zfar. Polygon offset → `setDepthBias`. GL lines → Metal line primitives.

**Resources:**
- Per-frame shared `MTLBuffer` ring (Apple Silicon unified memory) for tess vertices/indices (memcpy + `setVertexBuffer:offset:`). Small uniforms via `setVertexBytes`/`setFragmentBytes`.
- Textures: `image_t.texnum` → index into a backend texture table; handles are never reused while frames are in flight. RGBA8 uploads (CPU mips kept first for parity). DDS → BC1/BC3 (natively supported). Mid-frame re-uploads (cinematics, ghost textures, dlight lightmaps) use a small per-frame ring so recorded draws keep their contents. Deferred destruction after the frame's completion.
- Flares: replace the `glReadPixels` depth test with visibility-result (occlusion) queries or a depth read one frame late.

**Threading:** single-threaded, like GL1 (Metal encoding is cheap, and there's no driver thread to fight). A render thread or parallel encoding only if Phase 5 profiling shows the CPU is the limit. A render thread would add a frame of latency, so it needs a clear win to justify it (see §4b).

## 4b. Lowest input latency and overhead (user requirement, 2026-10-16)

The user's words: "can we also optimize for lowest input lag/delay possible? like lowest overhead". This sits alongside the Goal: every design choice is judged on input-to-photon latency and overhead as well as fps. The user plays competitively online on a 240 Hz display, currently in a 1920×1080 *window* on a 1920×1080 screen.

**The latency chain and what we control:**
mouse HID → macOS → SDL event pump (`IN_Frame`, once per frame) → `CL_SendCmd` builds the usercmd/view angles → cgame + renderer build the frame → GPU → present → WindowServer compositor (windowed) or direct-to-display (fullscreen) → scanout.
- The engine's frame order is already latency-friendly: Com_Frame sleeps in its frame limiter **before** pumping input, then samples input, then renders (`qcommon/common.c` Com_Frame → `client/cl_main.cpp:2830` `CL_SendCmd` → `:2852` `SCR_UpdateScreen`). Keep it that way: **no waiting between input sampling and rendering**.
- GL1 today: `glFinish` every frame (`tr_backend.c:1429`) plus a composited window (at least one extra refresh, ~4.2 ms at 240 Hz).

**Design rules (all phases):**
1. **Never block between input sampling and encoding.** Any wait for the GPU (frames-in-flight semaphore) or for a drawable happens at the *end* of the previous frame or right before the limiter sleep, never after `CL_SendCmd`. Acquire the drawable as late as possible (only for the final pass); present right after commit; `presentsWithTransaction NO`.
2. **Minimal queueing.** Frames in flight is a cvar (`r_metalFramesInFlight`, 1–3), and `CAMetalLayer.maximumDrawableCount` is 2 by default (3 only if measured to help). With ~0.2 ms of GPU work per frame, 1–2 frames in flight cost almost no throughput; pick the default by measuring latency *and* work time.
3. **`r_swapInterval 0` = immediate present** (`displaySyncEnabled NO`, tearing allowed in fullscreen) for the lowest latency. `1` = vsync, optionally paced with `CAMetalDisplayLink` (`preferredFrameLatency` 1) instead of blocking in `nextDrawable`.
4. **Fullscreen must hit direct-to-display** (compositor bypass): opaque layer, drawable size = native display pixels, no overlays or transforms. Verify with the Metal Performance HUD ("Direct") and Metal System Trace. Offer a borderless-fullscreen mode that keeps direct-to-display, so the user doesn't pay compositor latency for "windowed fullscreen". Tell the user that a plain window is always composited (+1 refresh).
5. **Game Mode and scheduling.** Info.plist `LSApplicationCategoryType = public.app-category.games` + `GCSupportsGameMode` (macOS Game Mode in fullscreen: CPU/GPU priority, doubled Bluetooth sampling for controllers and AirPods). While in a game: `NSProcessInfo beginActivityWithOptions: NSActivityLatencyCritical | NSActivityUserInitiated` (no App Nap, minimal timer coalescing), and the main thread at `QOS_CLASS_USER_INTERACTIVE` (P-cores).
6. **Low overhead in the hot path:** no per-frame heap allocations in the renderer or backend; one `@autoreleasepool` per frame; cached PSOs, depth/stencil and sampler states (no lookups by string); state changes filtered on the CPU; hidden visibility, LTO, and the renderer linked statically in Phase 4; no Objective-C in the front end; no GPU→CPU readbacks in the frame (flares/screenshots are async).
7. **Gameplay feel is not ours to change silently.** Anything that alters frame pacing or usercmd timing (the 1 ms frame floor, `com_maxfps` precision, `Sys_Milliseconds` resolution) affects movement and online feel: measure, propose, and get the user's OK first. The facts players rely on, which must stay exact:
   - The engine clock is whole milliseconds. `Com_Frame` uses `minMsec = 1000 / com_maxfps` (integer division, rounds down), and `bias` only catches up after a frame overran. So every value runs at even whole-ms frames, but values that don't divide 1000 are silently rounded *up* to the next rate that does: 240 → 4 ms frames = 250 fps, 144 → 6 ms = ~167 fps, 120 → 8 ms = 125 fps. Vanilla already wakes ~1 ms early and polls `select(0)` until the ms tick flips, so divisor rates are close to tick-exact today. The cost is ~1 ms of polling CPU per frame.
   - `com_maxfps` ≥ 1001 gives `minMsec 0`: no floor, and 0 ms frames are bumped to 1 ms by `Com_ModifyMsec` (`common.c:2188`), so the game clock runs fast (existing upstream bug). `com_maxfps 0` and timedemo use the 1 ms floor.
   - `cl_maxpackets` (clamped 30–125, default 30) sends a packet only when ≥ `1000 / cl_maxpackets` ms have passed since the last one (`client/cl_input.cpp:895-906`). Commands in between are bundled into the next packet, so none are lost. The real packet rate is fps / n: 250 fps + 125 packets = every 2nd frame (even, 125/s); 125 fps + 100 = every 2nd frame, only 62.5/s.
   - The server runs each usercmd as soon as its packet arrives (`server/sv_client.c:1963`); `sv_fps` is 20. There is no lag compensation anywhere, so packet timing directly affects hit registration.
   - MOHAA's pmove has no per-frame velocity snapping (unlike Q3's 125/333 fps jump physics) and chops moves into ≤ 66 ms steps (`fgame/bg_pmove.cpp:1530`), so fps mainly affects command timing, not jump height (from a code read, not measured).
   - Any limiter change must keep divisor values bit-exact (250 → every frame exactly 4 ms) and should be checked with frame-time and packet-spacing histograms.

**Planned latency work, by phase:**
- **Phase 1:** CAMetalLayer configured per rules 2–4 (drawable count, `displaySyncEnabled` from `r_swapInterval`, opaque layer, `presentsWithTransaction NO`); the wait-placement rule built into the frame loop from day one. *Done in session 2; drawables come from a helper thread with vsync off, see §3.2 "Drawable acquisition".*
- **Phase 2:** **latency probe** `cl_latencyStats 1`. For each frame, the time from the newest mouse event folded into the usercmd (mach time stamped at pump time, µs) to the frame's real present time (`MTLDrawable addPresentedHandler` → `presentedTime`). Log p50/p95/p99 over N frames, plus input→commit. For GL1, only input→`SwapBuffers` return is available (a lower bound; documented). Record SDL+Metal numbers windowed and fullscreen in §2.
- **Phase 3:** direct-to-display verified in fullscreen and borderless fullscreen; Info.plist Game Mode keys; latency numbers per display mode recorded.
- **Phase 5 (performance and latency):**
  - tune frames in flight / drawable count with the probe;
  - **raw mouse input via GCMouse** (GameController framework: unaccelerated HID deltas from a high-frequency handler, accumulated off the main thread and read at `IN_Frame`), usable while SDL still owns the window. Pulled ahead of Phase 6 because it's the biggest input-path win; verify first whether SDL's relative mode on macOS applies pointer acceleration;
  - **Client timing redesign (approved by the user 2026-10-16, both parts):**
    1. **Precise pacing, the default.** A µs-precise frame limiter on `mach_absolute_time` (sleep with `mach_wait_until`, or `kevent` EVFILT_TIMER with `NOTE_CRITICAL|NOTE_MACHTIME`, minus a learned oversleep estimate; at least one network poll per frame; a final spin of ~50–100 µs with no system calls). Deadlines stay locked to integer-ms ticks so divisor rates stay exact (250 → every frame 4 ms). Vanilla is already near tick-exact for divisors, so the expected gains are less polling CPU (~1 ms/frame today), correct behavior above 1000 fps (with the `Com_ModifyMsec` 0 ms fix), and an fps counter in µs. Commands, `serverTime` and `msec` stay whole milliseconds. Measure vanilla's frame-time and packet-spacing histograms first, then compare.
    2. **Decoupled command and render clocks, an optional mode** (cvar, off by default until the user has tested it online). Usercmds are created on their own exact integer-ms schedule (e.g. every 4 ms, a packet every 2nd command = 125/s), independent of rendering. Rendering runs at any rate (uncapped or any cap, not just divisors of 1000), and every rendered frame uses the newest mouse input for the view. Touches `client/cl_input.cpp` (`CL_CreateCmd`/`CL_SendCmd`/`CL_ReadyToSendPacket`), `cl_cgame.cpp` timing, and cgame prediction of the in-progress command. **Must prove** with packet/command histograms that the server receives a stream identical to a steady 250 fps / 125 packet client (same command lengths, same packet spacing, same bundling) before the user tests it online.
    The user isn't interested in VRR, so no VRR-specific work.
  - **Prior art: the user's friend's commits (JayRewd, reviewed 2026-10-16, ideas only; the user wants leaner/better re-implementations, not copies; credit JayRewd in commit messages when an idea is used):**
    - c3294f8c: µs frame deadline, `com_maxfps` above 1000.
    - 82cb483d: `cl_cmdfps` separate command clock. Our version adds a never-sent "virtual in-progress command" predicted to *now* instead of its cgame display lerp (which shows position up to one command interval in the past), needs no cgame changes, and allows 250 or 125 cmd/s.
    - 92810e4c: learned-oversleep µs sleeping (adopt); the present-anchored PresentGuard spin (skip: it waits between input sampling and present); `cl_lowLatencyInput` late mouse (redo as a view-only GCMouse read in part 2); `fpsbench set` (skip).
    - omaha 1c2c8754 (CPU cost cuts in cgame + GL1 front end; hand-port only, it depends on fork-only code). Adopt in Phase 5, each with a screenshot diff:
      - the exact world culls: flat PVS leaf list, 8-leaf group culls, per-face bounding spheres, early mark rejects;
      - the lightmap-atlas idea, done as an `MTLTexture` 2D array (no bleed, unlike its atlas without gutters);
      - a skeletal mesh-index table built at model load and the exact morph-weight check;
      - the two exact dlight micro-optimisations;
      - the exact cgame caches: tag indices (with an `iTagNum >= 0` guard), event interning, the temporary-mark clip cache, `CG_FootstepByTag`.

      Skip:
      - cross-entity world-space batching: wrong for env-map/`dot`/heightFade/specular stages, and wrong grid lighting per batch;
      - its skeletal caches: crash risk with nested views, and the normal skip ignores deforms;
      - in-place dlights;
      - the GL VBO path;
      - the smoke lighting cache: ignores dlights;
      - tempmodel sleep: freezes animated tempmodels;
      - the savegame-format change and the partial memset.

      Phase 2b keeps GL1's code verbatim.
    - Full notes: `/Users/pstn/Library/Application Support/Claude/side-session-notes/local_7a338994-81bf-4051-a8d3-93d0ff81e9c9/friend-commits-review.md`. Copy the conclusions into this plan when Phase 5 starts.
  - `NSActivityLatencyCritical` / QoS / Game Mode measured on and off;
  - **audio latency:** openal-soft period/refresh (`ALC_REFRESH`, period size) tuned and measured (footsteps matter online).
- **Phase 6:** native AppKit input replaces SDL entirely (keyboard events, text input, window); compare with the probe.
- Optional external check (user, any phase): iPhone 240 fps slow-motion video of mouse click → muzzle flash, GL1 vs Metal, same settings.

---

## 5. Phases

Each phase can take several sessions (slices). One session = one phase or slice (see CLAUDE.md).

### Phase 0: Survey, baseline, plan, decisions. **Status: done (session 1, 2026-10-16).**
Done: survey (§1), `port/` scripts, reference demo, GL1 baseline (§2), timedemo work metric, decisions (§3), CLAUDE.md, this plan.

### Phase 1: Build plumbing, Metal window layer, renderer_metal skeleton. **Status: done (session 2, 2026-10-16), except the 4K@2× HiDPI check (display not connected; carried into Phase 2a).**
Session 2 result:
- `cmake/platforms/macos.cmake` arm64 + 26.0. `cmake/renderer_metal.cmake` (`BUILD_RENDERER_METAL`, default OFF): `renderer_metal.dylib` in `openmohaa.app/Contents/MacOS` next to `renderer_opengl1.dylib`; Metal + QuartzCore, no OpenGL; `-fobjc-arc` for the two `.m` files; hidden visibility (only `GetRefAPI` exported). jpeg/freetype/renderer_common guards widened.
- `cmake/utils/metallib.cmake`: the custom command compiles `code/renderer_metal/shaders/*.metal` with `$OMM_METAL_BIN/metal -std=metal4.0 -target air64-apple-macos26.0 -O3 -Wall -Werror` (module cache inside the build dir), links with `metallib`, and embeds the result as a C array (`xxd -i` → `build.noindex/metal/renderer_metal/tr_mtl_metallib.h`). It fails with "Mount the Metal Toolchain first: source port/env.sh && omm_mount_metal" when the toolchain isn't mounted. `port/build.sh metal` mounts it itself. Shader so far: `tr_mtl_final.metal` (fullscreen triangle, texel read of the scene target, gamma ramp).
- `code/sdl/sdl_mtlimp.m`: the window layer, mirroring `sdl_glimp.c` (modes, `-2` desktop mode, fullscreen with display mode, `r_noborder`, `r_centerWindow`, fallbacks, `IN_Init`), plus the layer setup, `r_displayIndex` and the fullscreen-toggle `vid_restart`.
- `code/renderer_metal/`: renderergl1 copy, fully converted to `tr_mtl.h` (no qgl call site left). `tr_mtl.m`: device/queue (kept across `vid_restart`), embedded library + final pipeline, offscreen RGBA8 scene target, clear, final pass, present, frames-in-flight semaphore, drawable thread, texture names, synchronous scene readback for `screenshot`. Everything else is an empty stub. `r_metalPresentStats 1` prints frames / presented / skipped / shown and the waits at shutdown.
- Outside the renderer: `renderercommon/tr_common.h` (`qgl.h` only without `RENDERER_METAL`, `texnum` is `unsigned int` = GLuint) and the CMake lines above. No client, cgame or protocol change.
- Done criteria:
  - **Build: pass.** `port/build.sh metal` builds (36 s). `otool -L renderer_metal.dylib`: Metal, QuartzCore, Cocoa, SDL2, libc++, libobjc, no OpenGL. `nm -u`: no `gl*`/`CGL*`.
  - **Window: pass at 1280×720 windowed and 1920×1080 fullscreen** (log and drawable size; every presented drawable has a `presentedTime`; `screenshot` reads back the clear color (0, 64, 89)). **4K@2×: not tested,** only the DELL G2524H was connected (`system_profiler`).
  - **Menu, `+map dm/mohdm6` (`map` from a cfg), in-map `vid_restart`, fullscreen toggle, quit: pass.** A live switch Metal → `cl_renderer opengl1` + `vid_restart` → Metal works too. The bench demo plays through on Metal (draws stubbed).
  - **GL1: pass.** Works from the combined install; arm64-only bench in §2.
- **1a:**
  - `cmake/platforms/macos.cmake`: arm64 only + deployment target 26.0.
  - New `cmake/renderer_metal.cmake` (option `BUILD_RENDERER_METAL`): `renderer_metal` dylib built next to `renderer_opengl1` in the .app; frameworks Metal, QuartzCore; `-fobjc-arc`; hidden visibility; jpeg/freetype guards widened.
  - Offline metallib step (custom command running `$OMM_METAL_BIN/metal`, a clear error if the toolchain isn't mounted, embedded as a C array).
  - `port/build.sh metal` flavor (GL1 + Metal in one install, so `cl_renderer` switches).
  - `sdl/sdl_mtlimp.m` (window, CAMetalLayer, HiDPI, modes, fullscreen, `vid_restart`, `IN_Init`). The layer is set up for low latency per §4b rules 1–4.
  - `renderer_metal/` = copy of renderergl1 with qgl call sites converted to `tr_mtl.h` calls whose implementations are stubs, except device/queue setup and clear+present.
- **1b (if 1a runs long):** finish the call-site conversion; maps load without crashing. *(Not needed: done in session 2.)*
- **Done when:**
  - `port/build.sh metal` builds; `renderer_metal.dylib` has no GL symbols (`nm -u` shows no `gl*`/`CGL*`) and doesn't link OpenGL (`otool -L`).
  - `port/run.sh metal +set cl_renderer metal` opens a window that shows the Metal clear color every frame, at the right size: 1280×720 window, 1920×1080 fullscreen, and full pixel resolution on the 4K@2× display.
  - The menu loop runs (invisible), and `+map dm/mohdm6` loads and runs without crashes. `vid_restart`, fullscreen toggle and quit work.
  - `cl_renderer opengl1` still works, and the GL1 bench is re-recorded for the arm64-only build (expected unchanged).

### Phase 2: Core draw path. **Status: in progress. 2a done (session 3, 2026-10-17); next 2b.**
Session 3 result (2a):
- `tr_mtl.m` is a real backend: GL state tracking with dirty bits; pipeline/depth-stencil/sampler caches; the frame ring (8 MB per slot to start, grows if a frame needs more); lazy command buffer and scene pass, so drawing and clearing work at any point of the frame; textures (uploads, mips, BC formats, samplers per texture, in-use replacement, deferred destruction); client-array and immediate-mode draws; partial clears; readback after the frame's work so far. New `shaders/tr_mtl_stage.metal`. `r_metalPresentStats 1` also prints draws, passes, clear quads and ring KB per frame, pipelines created, in-use texture copies, and the frame command buffer's GPU time and commit-to-GPU/completion latency. Decisions: §3.2 "Added in session 3". Numbers: §2 "Metal, Phase 2a".
- `cmake/utils/metallib.cmake` now splits the shader list (it only ever saw one file) and compiles with `-fpreserve-invariance`.
- New `port/shot.sh` (screenshots of either renderer from a cfg in `port/shots/`, and `diff`) and `port/tgadiff.py` (per-pixel TGA difference: mean, max, pixel counts over thresholds, a ×16 diff image).
- Done criteria for 2a:
  - **Build: pass**, no warnings in the new code. GL1 from the same install still works and is unchanged.
  - **2D vs GL1, `port/shot.sh` + `diff`:** main menu identical except the animated ceiling fan (one run hit the same fan angle: 0 of 921600 pixels differed); console identical except the "init … ms" timing text (16–51 pixels); single-player loading screen with levelshot: 39 pixels differ by 1. Fonts, HUD (compass, health bar, chat/notify text) look the same in-map.
  - **3D came along:** the single-player spawn view of dm/mohdm6 (BSP, lightmaps, static models, HUD) differs from GL1 in 0.02–0.04% of pixels: the flickering light bulb's glow and a few edge pixels of a chair. `r_primitives 1` (immediate mode via `mbArrayElement`) matches too (11 pixels).
  - **Robustness:** menu → `vid_restart` → map → restart and quit (session 2's smoke cfg) pass; `r_metalFramesInFlight 1/2/3` work; the bench runs all three Metal runs.
  - **Bench:** §2, Metal 0.36 ms vs GL1 0.76 ms work in the same sitting (screen locked).
  - **Not done:** HiDPI on the 4K@2× display (not connected); DDS/BC uploads are implemented but no map tested so far loads a .dds; `r_swapInterval 1` not run (a locked screen makes `nextDrawable` time out); cinematics (`RE_StretchRaw`) not exercised (upstream has the intro disabled).
- **2a:** frame ring, buffers, texture upload/formats/samplers/deferred free, PSO + depth-stencil caches, the generic stage shader (1–2 textures, texenv, alpha test, blend), the 2D path (`Set2DWindow`/ortho, StretchPic/TilePic/TrianglePic, fonts, DrawBox/LineLoop, Scissor). Menus, console and HUD look right.
- **2b:**
  - 3D: projection/z remap, viewport flip, cull, depth range, polygon offset;
  - BSP surfaces, patches, terrain, lightmaps (multitexture collapse), vertex lighting, CPU tcGen/tcMod/deforms;
  - entities incl. TIKI CPU skinning and sphere lighting, static models, sprites, marks/decals, swipes. GL1 leaves the `SwipePoint`/`DrawBackground` exports NULL: find out how the client copes, then match or fix deliberately and note it here.
- **2c:** the `cl_latencyStats` latency probe (§4b).
- **Done when:** the bench demo plays fully on Metal; 5 fixed views match GL1 by eye and by screenshot diff; first Metal bench numbers are in §2's table (target work ≤ 0.45 ms/frame); first input→present latency numbers (windowed + fullscreen) recorded.

### Phase 3: Full feature parity. **Status: not started.**
- Sky box + cloud layers, portal sky, linear fog (all `GLS_FOG*` modes), mirrors/portals (clip distance).
- Q3 flares + MOHAA sun/lens flares, dlights (lightmap ring uploads), stencil shadows (`r_shadows 2`) and blob shadows, ghost textures.
- Cinematics (`DrawStretchRaw` ring), screenshots (`screenshot`, `screenshotJPEG`, levelshots; async readback), gamma/overbright final pass.
- `r_picmip`, `r_textureMode`/anisotropy, `r_ext_*`-equivalent capability cvars, debug views (`r_showtris`, `r_speeds`, debug lines).
- Spearhead/Breakthrough (`com_target_game 1/2`), the `MTLBinaryArchive` pipeline cache.
- Fullscreen and borderless fullscreen hitting direct-to-display, Info.plist Game Mode keys (§4b rules 4–5).
- **Done when:**
  - `port/compare.sh` (new: deterministic views via devmap + fixedtime + tele/face + pause, GL1 vs Metal screenshots, per-pixel diff) shows no visible differences on a fixed view list (stock DM + objective + SP maps with sky portals, fog, mirrors, terrain), and the user confirms the look.
  - Online smoke test on a real server (connect, play a few minutes, no errors or desync).
  - Bench recorded.

### Phase 4: Delete OpenGL entirely. **Status: not started.**
- Remove:
  - `renderergl1/`, `renderergl2/`, `renderercommon/qgl.h`, GL parts of `tr_common.h` (`texnum` → plain integer);
  - `sdl_glimp.c`, `sdl_gamma.c`;
  - `cmake/renderer_gl1.cmake`/`gl2.cmake` and their options, `-framework OpenGL`;
  - `OPENGL_DRIVER_NAME`/`r_glDriver`, unused SDL GL headers.
- Keep the `glconfig_t` layout (ABI), and keep accepting GL-named cvar values in old configs (e.g. `r_textureMode GL_LINEAR_MIPMAP_NEAREST`).
- Link the renderer statically (`USE_RENDERER_DLOPEN OFF`); `cl_renderer` becomes ignored.
- **Done when:**
  - `grep -rniE "qgl|opengl|SDL_GL_|GL_[A-Z0-9_]+|glimp" code cmake` finds nothing except documented exceptions (struct/cvar names kept for compatibility).
  - The build passes, the bench is unchanged, and `otool -L`/`nm` show no OpenGL.

### Phase 5: Performance and input latency. **Status: not started.**
- Latency work from §4b (frames-in-flight tuning, GCMouse raw input, the client timing redesign: precise pacing by default + the optional decoupled command/render mode, activity/QoS/Game Mode, audio latency), measured with the latency probe.
- Measure first (bench + `trace` + `sample`/Time Profiler), one change at a time, each recorded here with before/after numbers. Candidates:
  - Static world geometry in GPU-resident buffers (no per-frame re-tessellation/copy).
  - Batching by shader+lightmap; sort keys to minimize PSO/state changes; fewer render passes.
  - GPU skinning (compute) for TIKI; GPU tcMod/tcGen/rgbGen/deforms for the common cases.
  - Faster `RB_Light_Real` sphere lighting.
  - BC texture compression for memory; async texture loading at map load.
  - Metal 4 (argument tables, residency sets, `MTL4CommandQueue`) where measured.
  - Frame pacing (present timing / CAMetalDisplayLink).
- Engine frame floor: `Com_Frame` pads frames to 1 ms with an integer-ms clock, which caps fps near 1000. Making it µs-precise changes how `com_maxfps` behaves online (frame pacing feeds usercmd timing and movement feel), so **ask the user before touching it**.
- **Done when:** the bench work time is ≤ 0.25 ms/frame or the remaining cost is documented as engine-side, p99 frame time is lower than GL1, input→present latency is measured lower than the Phase 2 numbers (and as low as the display mode allows), and `compare.sh` shows no visual change.

### Phase 6: Native AppKit/GameController (replace SDL2). **Status: not started.**
- Measure input latency SDL vs native first, and confirm with the user before starting.
- Native `NSWindow` + `CAMetalLayer` view, keyboard (NSEvent → Q3 keys, text input), raw mouse (GCMouse deltas, cursor capture), gamepad (GCController), clipboard, window modes, Retina, the `openmohaa://` URL handler, the color cursor.
- **Done when:** no SDL in `otool -L`, input matches or beats SDL (user test, no mouse acceleration), menus/console/text input work.

### Phase 7: Lean and robust. **Status: not started.**
- Delete non-macOS platform code: Windows sys/con files and resources, Launcher, `asm/`, Emscripten/Linux cmake, other-platform CI, docker `container/`. Keep `null/` (server) and the `omohaaded` target.
- Arm64-only `.app` with a proper Info.plist (`NSHighResolutionCapable`, `LSMinimumSystemVersion` 26).
- openal-soft built from pinned vendored source and linked directly; ad-hoc codesign.
- Dead code out: the uncompiled ioq3 sound files, unused renderercommon loaders, unused gamespy parts if safe. Warnings cleaned in touched code.
- README section on building/running the Metal port.
- **Done when:** one `port/build.sh` produces `OpenMoHAA.app` (arm64) + `omohaaded`, the bench and an online test pass, and the tree has no dead platform code.

---

## 6. Next session

**Session 4 = Phase 2, slice 2b** (the 3D path verified against GL1, then the gaps). Start with CLAUDE.md, then this file's §3.2 (including "Added in session 3"), §4, §4b, Phase 2 (session 3's result) and §2 "Metal, Phase 2a".

Where things stand (end of session 3):
- 2a is done and committed on `metal` (not pushed: Phase 2 isn't finished). The Metal backend draws everything the front end issues: menus, console, HUD and the 3D world already render and match GL1 closely in the views tested. Bench: Metal 0.36 ms work vs GL1 0.76 ms in the same sitting (screen locked).
- Run Metal with `port/run.sh metal +set cl_renderer metal`. Compare with GL1 using `port/shot.sh <metal|opengl1> <name> <cfg> [args]` and `port/shot.sh diff <name>`; cfgs live in `port/shots/` (`shot_menu.cfg`, `shot_console.cfg`, `shot_view.cfg`; usage in the script header). `r_metalPresentStats 1` prints the backend's per-frame counters at shutdown.
- All backend code is `code/renderer_metal/tr_mtl.m` (+ `shaders/tr_mtl_stage.metal`); the front end is still verbatim GL1 apart from the qgl → mb rename.

Do next, in order (Phase 2b):
1. **Deterministic 3D views.** Extend `port/shots/` so several fixed views per map can be compared: the single-player spawn works (`shot_view.cfg`, needs `finishloadingscreen`), but `tele x y z` / `face p y r` typed in a cfg did not move the player (find the right syntax or command: vector arguments, `cmd tele`, or a spawn-point/`setviewpos`-like route; `coord` prints the position). Add `fixedtime`-style determinism if MOHAA has it, `pause` before `screenshot`. Views: dm/mohdm6 outdoors with sky, terrain, an objective map, a map with fog, entities in view (bots or a script-spawned model).
2. Fix what the views show. Known so far: a few edge pixels on a static model differ (CPU-computed mvp vs GL's P·(MV·v) rounding, or the z remap; check whether it matters); the light-bulb glow differs between runs (time-based: confirm with two GL1 runs). Also check: weapon view model (depth range hack), sky box and portal sky, sprites, marks/decals, swipes, terrain, dlights (bench reports in-use texture copies: the dlight lightmap path), entity sphere lighting, polygon offset decals, cull on mirrored views.
3. GL1 leaves the `SwipePoint`/`DrawBackground` exports NULL: find out how the client copes, then match or fix deliberately and note it here.
4. 2c: the `cl_latencyStats` latency probe (§4b).
5. Bench Metal and GL1 in the same sitting **with the screen unlocked** (session 3's numbers are from a locked screen), and with `r_swapInterval 1` once. Record in §2. Then push `metal` at the end of Phase 2.
6. The 4K@2× HiDPI check when the U4323QE is connected (`+set r_displayIndex 1`), including the HiDPI input gotchas below.

Gotchas found in session 3:
- **The screen was locked for the whole session** (`ioreg -n Root -d1 -a | grep -A1 CGSSessionScreenIsLocked`). Then nothing is composited: `presentedTime` is 0 (r_metalPresentStats shows 0 shown), `nextDrawable` rarely returns (most frames skip their present), and GL1's swap gets cheaper (0.76 instead of ~0.93 ms work). Check the lock state before benchmarking or measuring latency.
- **Never pace frames on completed handlers:** they arrived ~1.3 ms after the GPU had finished; with `presentDrawable:` in the same command buffer and a locked screen, only at the next refresh (the game loop fell to 239 fps). Frame completion is read from `mb_frameEvent` (`MTLSharedEvent`).
- The game itself runs `exec menu.cfg` at startup: never name a test cfg like a game cfg (`port/shots/` uses a `shot_` prefix).
- `wait N` counts real milliseconds, also in a timedemo, so a timedemo can't be screenshotted at a fixed frame. Use devmap views.
- `g_gametype 0 +devmap <map>` stops at a "Continue" loading screen: `finishloadingscreen` dismisses it. The console opens at startup with `+set ui_console 1 +set cl_playintro 0` (there is no `toggleconsole`; `devcon` toggles the developer console).
- The main menu's ceiling fan is animated, so menu screenshots differ there between any two runs.
- `cmake/utils/metallib.cmake` gets the shader list joined with `|` (a `;` would split the `-D` argument).
- Session 2's gotchas still apply: the renderer's `vid_restart` is queued after pending cfg commands; `screencapture` has no permission (use the in-game `screenshot`); a fullscreen `vid_restart` takes seconds; HiDPI (2× displays, untested): with `r_allowResize 1`, `sdl_input.c`'s `SDL_WINDOWEVENT_RESIZED` compares points with pixels and would `vid_restart` repeatedly, relative mouse deltas are in points while the UI cursor is in pixels, and `SDL_WarpMouseInWindow(vidWidth / 2, ...)` expects points; `R_ArrayElementDiscrete` passes units 0/1 (handled); maps with a sky never clear color (handled: the scene target loads).
- Session 1 gotchas still apply: stale PID files block on an "Abnormal Exit" dialog (`run.sh` cleans them); `xctrace --launch` is unreliable; don't edit a running bash script; upstream's `.gitignore` `build*` rule; `s_initsound 0` crashes; the upstream launchers don't work on macOS; use `sv_maxclients` ≥ 2 for scripted map loads; `glconfig_t` is ABI; overbright depends on fullscreen + hardware gamma in GL1.

Open bugs: none known. Untested: HiDPI on a 2× display, DDS/BC texture uploads (no tested map loads a .dds), `r_swapInterval 1` on Metal since 2a, cinematics (`RE_StretchRaw`).
