# OpenMoHAA native Metal port: plan

Goal (verbatim, see CLAUDE.md): "Create a FULL macOS Silicon Metal native port of openmohaa, with the ultimate goal of stripping OpenGL entirely from the code by the time we're done, and only have it run Metal from start to finish. Look at all possible ways to improve performance while we do it, increase efficiency, etc. Make it as lean and robust as possible."

Branches: `main` = upstream mirror (openmoh/openmohaa a2f34019 at the start). `metal` = this work.

## Status

| Phase | What | Status |
|---|---|---|
| 0 | Survey, baseline, plan, decisions | **Done** (session 1) |
| 1 | Build plumbing + Metal window layer + `renderer_metal` skeleton | **Done** (session 2; the 4K@2× HiDPI check done in session 4) |
| 2 | Core draw path (textures, pipelines, 2D/UI, world, entities) | **Done** (2a session 3, 2b session 4, 2c session 5: the latency probe) |
| 3 | Full feature parity + screenshot-compare harness + online smoke test | **In progress:** 3a done (session 6: display modes, direct to display); next 3b (session 7) |
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
- **Metal, Phase 2b** (session 4, 2026-10-17, **screen unlocked**: `CGSSessionScreenIsLocked` absent before and after). Same sitting, `port/bench.sh run gl 3` and `port/bench.sh run metal 3 +set cl_renderer metal +set r_metalPresentStats 1`, then one run each with `+set r_swapInterval 1`:

  | Config | fps | **work ms/frame** | notes |
  |---|---|---|---|
  | GL1 `r_primitives 2` | 902.2 / 938.2 / 943.4 | **0.955 / 0.887 / 0.884** | |
  | Metal (2 frames in flight, 2 drawables) | 999.2 / 997.9 / 997.5 | **0.393 / 0.396 / 0.389** | GPU 0.12 ms/frame; 58 draws, 1 pass, 143 KB ring per frame; 12 pipelines; ~40% of frames get a drawable (the compositor shows ~240/s), the rest skip their present without waiting; every presented frame was shown |
  | GL1, `r_swapInterval 1` | 239.5 | (4.173: includes the vsync wait) | |
  | Metal, `r_swapInterval 1` | 237.0 | (4.218: includes the vsync wait) | 3.47 ms/frame waiting in `nextDrawable`, at the end of the frame (§4b rule 1); every frame presented and shown |

  - **2.3× less work per frame than GL1 with the compositor running**, under the Phase 2 target (≤ 0.45 ms). Metal's 0.39 ms vs 0.36 ms on a locked screen (session 3) is the cost of really getting and presenting drawables.
  - 4K@2× (U4323QE, `+set r_displayIndex 1`, 1280×720-point window): Metal draws 2560×1440 pixels, GPU 1.2 ms/frame (not benchmarked; the display runs at 60 Hz). GL1 renders the same window at 1280×720 and lets macOS upscale it.
- **Metal, Phase 2c** (session 5, 2026-10-17, screen unlocked). Bench in the same sitting (`port/bench.sh run gl 3`, `port/bench.sh run metal 3 +set cl_renderer metal`): GL1 **1.082 / 1.087 / 1.091 ms** work (GL1 ran ~15% slower than in session 4 this sitting), Metal **0.412 / 0.403 / 0.397** and 0.397 / 0.398 ms. With the probe on (`+set cl_latencyStats 1`; a timedemo has no mouse input, but every frame is still tagged and recorded with both handlers): 0.409 / 0.400 / 0.401 / 0.400 ms (one run 0.462). **The probe costs nothing measurable.**
- **Input → present latency** (session 5, `port/latency.sh`): dm/mohdm6 in single-player mode on a local server; 20 s of synthetic 1000 Hz mouse movement posted at the HID event tap (§3.2 "Added in session 5"); `com_maxfps 250` (the user's cap) unless noted; vsync off unless noted; 2 frames in flight, 2 drawables; the 240 Hz G2524H. Values in ms, p50 / p95 / p99.
  - *pump* = when the frame's SDL event pump started (its input sample).
  - *event* = the OS timestamp of the newest mouse event in that pump.
  - *present* = the display swap that first shows the input (checked against Instruments, below).

  | Mode | Frames with input | event → pump | pump → commit | pump → GPU done | **pump → present** | **event → present** |
  |---|---|---|---|---|---|---|
  | Metal, 1280×720 window, 250 fps | 4979 (96% shown by their own frame) | 0.98 / 1.92 / 2.05 | 0.31 / 0.46 / 0.55 | 0.75 / 1.24 / 1.38 | **10.02 / 12.16 / 12.35** | **10.90 / 13.19 / 13.40** |
  | Metal, window, ~1000 fps (`com_maxfps 0`) | 13965 (32% own; 68% by a later frame, no drawable ready) | 0.57 / 1.51 / 1.67 | 0.29 / 0.43 / 0.50 | 0.62 / 0.94 / 1.16 | **5.07 / 8.91 / 10.98** | **5.72 / 9.60 / 11.65** |
  | Metal, window, 250 fps, vsync on | 4741 (100% own) | 0.80 / 1.22 / 1.70 | 0.34 / 0.44 / 0.50 | 1.21 / 1.35 / 1.39 | **7.72 / 11.88 / 11.89** | **8.72 / 13.06 / 13.10** |
  | Metal, 1920×1080 fullscreen, 250 fps | 4992 | 0.76 / 0.88 / 0.92 | 0.25 / 0.31 / 0.35 | 1.02 / 1.49 / 1.70 | not measurable (see below) | |
  | Metal, fullscreen, ~1000 fps | 15846 (33% own) | 0.57 / 1.54 / 1.72 | 0.28 / 0.38 / 0.41 | 0.64 / 1.28 / 1.51 | 5.05 / 9.78 / 12.19 (upper bound, see below) | 5.93 / 10.38 / 12.73 |
  | Metal, fullscreen, 250 fps, vsync on | 4739 (100% own) | 0.79 / 1.34 / 1.68 | 0.28 / 0.37 / 0.44 | 2.12 / 2.51 / 2.60 | **11.93 / 11.94 / 11.94** | **12.66 / 13.24 / 13.52** |
  | GL1, window, 250 fps | 4988 | 1.07 / 1.37 / 2.07 | | | pump → EndFrame return (after glFinish + SwapBuffers; no present time, a lower bound): 0.68 / 1.35 / 1.60 | event → EndFrame return: 1.68 / 2.49 / 2.85 |
  | GL1, fullscreen, 250 fps | 4945 | 0.50 / 1.51 / 1.67 | | | 0.56 / 0.87 / 1.29 (same lower bound) | 1.09 / 2.10 / 2.41 |

  - **The probe's present times are real.** `TRACE=1 LATSTATS=2 port/latency.sh …` records Instruments' Metal System Trace during the run, and `port/display_trace.py` matches every present time the probe reported against the display's own swap log. Window: 1557 of 1561 present times fall within ±1 µs on a display swap that shows that very frame. Fullscreen at ~1000 fps: 2392 of 2396.
  - **A composited window costs about two refreshes, in two modes.** At 250 fps, commit → present sits at either ~5 ms or ~9.7 ms (one 240 Hz refresh apart) for stretches of 0.5–5 s, then switches (the probe's time series; Instruments agrees). At ~1000 fps the median falls to 5.1 ms: only frames that find a ready drawable are presented, and they arrive just after the compositor released the previous drawable. Phase 5: find what selects the mode (present timing, drawable count, `CAMetalDisplayLink`).
  - **Fullscreen is not direct-to-display, in either renderer.** Instruments: Metal's fullscreen layer "cannot go direct to display due to layer requires a potential offscreen render"; GL1's fullscreen surface isn't direct either. So fullscreen goes through the compositor like a window (Phase 3).
  - **Fullscreen with vsync off: Core Animation's present feedback is wrong.** Share of presented drawables reported as never shown (presented-handler status 3; `presentedTime` 0):
    - 98–99% at 250 fps;
    - ~33% at ~1000 fps;
    - almost all at 200 and 125 fps.

    Yet Instruments shows the display swapping game frames 220–240 times a second. Where Core Animation does give a time, it's right. So the probe leaves its present lines out when more than half of the presented frames are reported as never shown (fullscreen 250 fps above). In fullscreen at ~1000 fps, pump → present is an upper bound: the input of a frame reported as never shown is credited to the next frame that has a time. Re-check once fullscreen goes direct.
  - **Vsync on queues a frame.** Fullscreen at 250 fps with vsync: 11.9 ms, constant (2 drawables: each frame waits a refresh behind the one on screen). Window with vsync: 7.7 ms p50. With vsync on, the GPU also finishes 1.8 ms after the commit, against ~0.45 ms without vsync (not explained yet). Phase 5's low-latency vsync (1 frame in flight, `CAMetalDisplayLink`, just-in-time frame start) targets both.
  - **Commit → GPU done takes 0.35–0.45 ms** for ~0.12 ms of GPU work: the GPU starts ~0.3 ms after the commit (session 3 measured the same). So Metal's GPU done (0.75 ms p50 in a window) lands where GL1's EndFrame returns (0.68 ms, after its glFinish), although Metal commits at 0.31 ms. The submission gap is a Phase 5 target.
  - **event → pump** is the OS-side wait plus SDL's pump: ~0–2 ms for 1000 Hz events at 250 fps. The oldest and newest event of a pump are almost always the same: macOS coalesces mouse moves into one event per pump.
  - Caveat: on the local server, `SV_Frame` (the 20 Hz game frame) runs between the input pump and the usercmd on about 1 frame in 12 at 250 fps. Online clients don't do that work, so it can only inflate pump → commit's p95/p99.
- **Metal, Phase 3a** (session 6, 2026-10-17, screen unlocked). `port/bench.sh run gl 3` and `port/bench.sh run metal 3 +set cl_renderer metal`, same sitting: GL1 **0.928 / 0.937 / 0.917 ms** work, Metal **0.395 / 0.397 / 0.391 ms**. The bench runs in a window, so the display-mode changes don't touch it.
- **Input → present latency per display mode** (session 6, 2026-10-17, after the direct-to-display fix, §3.2 "Added in session 6"). Same setup as session 5 (`port/latency.sh`, 20 s of synthetic mouse, 2 frames in flight, screen unlocked), all three modes in one sitting:
  - *window*: 1280×720, composited;
  - *fullscreen*: exclusive, `r_fullscreen 1`, 1920×1080;
  - *borderless*: `r_fullscreen 1` + `r_noborder 1`, a native fullscreen Space at the desktop's 1920×1080.

  Drawables: 2, except fullscreen/borderless with vsync, where the new automatic choice uses 3. Values in ms, p50 / p95 / p99.

  | Mode | Frames with input | pump → commit | pump → GPU done | **pump → present** | **event → present** |
  |---|---|---|---|---|---|
  | window, 250 fps | 4950 (98% own) | 0.31 / 0.44 / 0.56 | 0.89 / 1.30 / 1.45 | **2.86 / 11.77 / 12.27** (traced run: 2.38 / 9.85 / 11.85) | 3.94 / 12.85 / 13.34 |
  | window, ~1000 fps | 16359 (20% own) | 0.26 / 0.35 / 0.43 | 0.58 / 0.75 / 0.85 | **12.91 / 14.78 / 15.10** (traced run: 4.91 / 8.36 / 10.06) | 13.64 / 15.34 / 15.48 |
  | window, 250 fps, vsync | 4793 (100%) | 0.34 / 0.52 / 0.75 | 1.02 / 1.29 / 1.37 | **11.86 / 11.89 / 11.90** | 12.77 / 13.41 / 13.90 |
  | window, ~1000 fps, vsync | 4793 (100%) | 0.34 / 0.47 / 0.56 | 1.18 / 1.38 / 1.46 | **11.86 / 11.89 / 11.90** | 12.78 / 13.34 / 13.84 |
  | fullscreen, 250 fps | 4990 (100%) | 0.22 / 0.28 / 0.32 | 0.96 / 1.05 / 1.10 | **2.07 / 2.26 / 2.41** | 3.31 / 3.52 / 3.68 |
  | fullscreen, ~1000 fps | 19809 (49% own) | 0.28 / 0.38 / 0.41 | 0.74 / 0.87 / 0.93 | **2.35 / 3.10 / 3.87** (traced run: 2.68 / 3.77 / 5.01) | 3.44 / 4.18 / 4.95 |
  | fullscreen, 250 fps, vsync, 3 drawables | 4787 (100%) | 0.25 / 0.35 / 0.41 | 1.37 / 1.50 / 1.57 | **8.06 / 8.10 / 8.11** | 9.02 / 9.38 / 9.89 |
  | fullscreen, ~1000 fps, vsync, 3 drawables | 4791 (100%) | 0.26 / 0.35 / 0.43 | 1.38 / 1.50 / 1.57 | **8.06 / 8.09 / 8.10** | 8.98 / 9.49 / 9.99 |
  | borderless, 250 fps | 4936 (100%) | 0.22 / 0.26 / 0.31 | 0.95 / 1.04 / 1.12 | **2.06 / 2.27 / 2.36** | 2.96 / 3.24 / 3.99 |
  | borderless, ~1000 fps | 16971 (51% own) | 0.29 / 0.38 / 0.44 | 0.75 / 0.88 / 0.94 | **2.20 / 3.15 / 3.86** | 3.19 / 4.24 / 5.03 |
  | borderless, 250 fps, vsync, 3 drawables | 4745 (100%) | 0.28 / 0.37 / 0.44 | 1.38 / 1.51 / 1.60 | **8.06 / 8.09 / 8.10** | 9.05 / 9.38 / 9.88 |
  | borderless, ~1000 fps, vsync, 3 drawables | 4792 (100%) | 0.25 / 0.32 / 0.40 | 1.36 / 1.48 / 1.54 | **8.06 / 8.09 / 8.10** | 8.98 / 9.36 / 9.85 |

  - **Both fullscreen modes go direct to display, on every frame** (Instruments: "direct to display: Yes" for every swap with a game frame). With vsync off the display flips as soon as a frame arrives (tearing): 250 flips/s at 250 fps, ~480/s at ~1000 fps. **pump → present is 2.1 ms at 250 fps** (p99 2.4), against 10.0 ms in session 5's window, and fullscreen couldn't be measured at all before. Exclusive and borderless are the same.
  - **Core Animation's present feedback is right once the layer is direct.** Every presented handler reports a time (status 2), and the probe's present times land exactly on the display's swaps:
    - fullscreen 250 fps: 1661 of 1667 within 2 µs, 1657 showing that very frame;
    - borderless 250 fps: 1632 of 1635;
    - fullscreen ~1000 fps: 3082 of 3087;
    - fullscreen with vsync: 829 of 831.

    Session 5's "never shown" reports came from the composited fullscreen. The probe's fullscreen numbers are now real, at every rate.
  - **Where the 2.1 ms goes (250 fps, vsync off):** commit at 0.22 ms; the GPU done 0.74 ms later, for ~0.12 ms of GPU work (session 5's submission gap); the flip ~1.1 ms after that. Both gaps are Phase 5 targets.
  - **~1000 fps, vsync off:** only ~half the frames find a free drawable (the drawable that was flipped away comes back ~2 ms later). A frame without one skips its present, and its input waits for the next presented frame: p50 2.2–2.35 ms.
    - **3 drawables don't help:** more frames are presented (24348 vs 19520 in 41 s), but 26% of them are never shown, and latency goes up (3.42 / 5.74 / 6.95).
    - At 250 fps, 3 drawables also cost a little: 2.41 / 2.81 / 4.66.

    So 2 stay the default without vsync.
  - **Vsync direct to display needed a third drawable.** With 2, fullscreen and borderless ran at **125 fps** instead of 240:
    - every frame waited 7.25 ms for a drawable;
    - Instruments: 125 flips/s;
    - pump → present was already 8.05 ms.

    That would also have halved the command rate (§4b rule 7). With 3 drawables they run at 240 fps with the same 8.06 ms, constant; `r_metalDrawables` now defaults to 0 = automatic (§3.2 "Added in session 6"). This is two refreshes minus the frame's own work: the vsync queue is the Phase 5 low-latency-vsync target (part 4). Session 5's composited fullscreen with vsync took 11.9 ms.
  - **The window is never direct** ("display rejected"), and today's compositor behaved differently from session 5's: it swapped the display at the game's present rate (249 swaps/s at 250 fps, 436/s at ~1000 fps) instead of 240 Hz. So the window's numbers swing between runs: at 250 fps p50 2.86 and 2.38 ms (p95 ~10–12); at ~1000 fps 12.91 and 4.91 ms. Session 5 measured 10.0 and 5.1. With vsync the window holds 11.86 ms (session 5: 7.7). **Fullscreen is the low-latency mode:** stable ~2 ms vs anything from 2.4 to 13 ms windowed.
  - **event → pump depends on each run's phase** between the synthetic 1000 Hz events and the 4 ms frame (p50 0.44–1.28 ms, sometimes with p50 ≈ p95), not on the display mode. Compare modes on pump → present.
  - **Game Mode turns on in both fullscreen modes** (gamepolicyd log), not in the window. It wasn't measured on vs off (Phase 5).
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
7. **Client timing redesign: both parts approved** (2026-10-16): precise frame pacing as the default, and an optional mode with separate command and render clocks that the user tests online before it can become the default. Planned for Phase 5 (§4b). No VRR work (the user isn't interested). Refined 2026-10-17 with the user and JayRewd (see Phase 5): exact µs caps, a selectable fallback for the movement display, a suggest-only fps cap from frame-time percentiles. The user will never use VRR and plans to cap fps (2026-10-17): the present guard is dropped, low-latency vsync moves up, and the recommended setup is recorded in Phase 5.

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
- **Defaults: 2 drawables, 2 frames in flight** (`r_metalDrawables` 2/3, `r_metalFramesInFlight` 1–3, both LATCH). With 3 drawables more frames get presented (8600 vs 6069 in the bench run) but 1700 of them are never shown; with 2, 6067 of 6069 presented frames were shown. *Session 6: `r_metalDrawables` defaults to 0 = automatic, 3 for vsync in fullscreen (direct to display), see "Added in session 6".*
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
- GL conventions as planned: clip z remapped (session 4: in the vertex shader after mvp = P · MV, see below), viewport/scissor flipped, CCW front faces, depth range → viewport znear/zfar, polygon offset → `setDepthBias(units, factor)`, the clip plane converted to object space per draw and written to `[[clip_distance]]`, `[[position, invariant]]` (`-fpreserve-invariance`) for multi-pass `GLS_DEPTHFUNC_EQUAL`. Fog: GL linear fog on the per-fragment |eye z| (Phase 3 checks it against GL1). Not emulated: line width, line stipple, points, flat shading (only debug/editor views and a disabled GL1 path use them).

Added in session 4 (Phase 2b):
- **Clip z is remapped in the vertex shader** (`z' = (z + w) / 2` after `mvp · v`, with mvp = P · MV on the CPU), not folded into the matrix: that rounded depth differently from GL1 on Apple's GL and changed z-fighting where models touch the world (a spawned soldier on a floor: 281 → 1 pixels different). Computing `P · (MV · v)` in the shader instead matched GL1 worse everywhere, so Apple's GL premultiplies like we do.
- **Hidden surface removal stays on.** Opaque pipelines have no `discard`, so the GPU's HSR resolves exact depth ties between triangles of one draw in its own way, while GL1 on Apple's GL behaves like a pipeline that can discard (in-order `LEQUAL`). Models with coincident faces (static models, the view weapon, stitched terrain) therefore differ from GL1 by a few levels in up to ~2500 edge pixels of a 1280×720 view. Proven: drawing every triangle as its own draw, or a `discard` in every pipeline, matched GL1 to 2–24 pixels per view. Not worth the GPU cost (and HSR is part of why Metal's GPU time is low); invisible by eye.
- **`screenshot` reads the previous frame on Metal** (the scene target before this frame's first draw). GL1 on macOS reads the back buffer after the swap, which holds the frame from two frames back (measured with `r_znear` toggled per frame). Metal is not changed to imitate that; the fixed-view harness compensates (three captures per view, best GL1 match).
- **`SwipePoint` and `DrawBackground` stay NULL, as in GL1.** `re.DrawBackground` has no caller anywhere. `cgi.R_SwipePoint` is called unguarded by cgame's swipe rendering (`cg_swipe.cpp`), which tempmodels with a `trail` reach: the only retail one is the grenade explosion's debris (`models/fx/fx_grenade_explosion.tik`, `trail "fireswipe2" tag_p1 tag_p2 0.8`). Upstream's `RE_SwipePoint` is also broken (it writes `swipes[numswipes - 1]` without ever incrementing `numswipes`: index -1), so exporting it would trade a NULL call for memory corruption. Not verified at run time yet (a grenade couldn't be thrown from a cfg); Phase 3 throws one, and if GL1 crashes there too, it's an upstream bug to fix in both renderers' front ends with the user's OK.

Added in session 5 (Phase 2c, the latency probe):
- **The client owns the statistics, the renderer only reports times.** The statistics live in `client/cl_latency.cpp`, renderer-independent: tagging, attribution, histograms (10 µs bins up to 40 ms), printing. The hooks outside it are one line each:
  - `IN_ProcessEvents`: the pump starts;
  - `CL_MouseEvent`: a delta from this pump went into `cl.mouseDx`;
  - `CL_MouseMove`: the delta went into the usercmd and the view angles;
  - around `re.EndFrame` in `SCR_SimpleUpdateScreen`;
  - `CL_Init` / `CL_Shutdown`.

  `cmake/client.cmake` lists the new file. Frame pacing and usercmd timing are untouched: nothing waits, and every hook is one branch while the probe is off.
- **refexport: two entries appended,** `LatencyTag(tag)` before EndFrame and `LatencyPoll(&tag, &commit, &gpuDone, &present)`. `REF_API_VERSION` is unchanged: both renderers are built from this tree, and GL1's `GetRefAPI` memset leaves the entries NULL, so the client falls back to timing EndFrame's return (after glFinish and SwapBuffers).
- **OS event times come from an `NSEvent` local monitor** (`sys/sys_osx.m`: `Sys_MouseEventMonitor`, `Sys_TakeMouseEventTimes`). SDL2 doesn't expose `NSEvent.timestamp`. The monitor runs inside SDL's pump on the main thread and is installed only while the probe is on.
- **One clock everywhere:** microseconds on `mach_absolute_time` (`clock_gettime_nsec_np(CLOCK_UPTIME_RAW)`). NSEvent timestamps, `presentedTime` and `GPUEndTime` are on the same host clock.
- **Attribution:** an input becomes visible with the first frame at or after its own that is shown. The input of a frame that skipped its present (no drawable ready) or was dropped by the compositor waits for the next shown frame. Commit and GPU-done times come from the frame's own record.
- **Renderer records** (`tr_mtl.m`):
  - a ring of 256 records under an `os_unfair_lock`;
  - the completed handler (`GPUEndTime`) and the presented handler (`presentedTime`) write only the record whose sequence number they hold;
  - records are dropped at `mbShutdown`, and a handler that hasn't run after 1 s counts as "no time";
  - work happens only for tagged frames, i.e. while the probe is on.

  `presentUsec` is 0 when the frame wasn't presented (no drawable) and -1 when it was presented but Core Animation reports it as never shown.
- **Synthetic input:** `port/mousemove.c` posts 1000 Hz `kCGEventMouseMoved` events (delta x ±2, direction flipped every 400 ms) at the HID event tap with `CGEventPost`. That needs the Accessibility permission, which this machine's session has (`CGPreflightPostEventAccess`). The events take the same WindowServer → app → SDL path as a real mouse; only the device, USB and HID driver are skipped. Their OS timestamp is their creation time; a real mouse adds its polling interval (1 ms at 1000 Hz) and the USB/HID path before that timestamp.
- **Cvars:**
  - `cl_latencyStats` is `CVAR_TEMP` (never archived);
  - `cl_latencyStatsFrames` (1000) sets how often it prints;
  - `cl_latencyStats 2` also logs every frame's raw times, which `port/display_trace.py` checks against the display.

Added in session 6 (Phase 3a, display modes):
- **Why fullscreen wasn't direct to display: the window shadow.** SDL's exclusive fullscreen is a borderless window at the shielding level that keeps `hasShadow = YES`. A shadow needs an offscreen pass over the window's layers, and Instruments' detachment columns say so: "layer requires a potential offscreen render", suggestion "edit the layer tree to prevent the extra rendering passes over the candidate detached". Found with a standalone SDL2 + `CAMetalLayer` test program (SDL's exact window, presenting clears) under Metal System Trace, one property at a time:
  - exclusive fullscreen as SDL makes it: not direct;
  - the `CAMetalLayer` as the content view's own layer instead of SDL's subview: not direct;
  - no black background on SDL's content view: not direct;
  - normal window level instead of shielding: not direct;
  - **`hasShadow = NO`: direct on every frame;**
  - SDL's desktop fullscreen (a native macOS fullscreen Space): direct on every frame, with or without the shadow;
  - a borderless desktop-size window at the normal level, no shadow: "display rejected" (the menu bar sits on top).

  The layer tree itself (SDL's content view with its black background, the Metal view as a subview, the zero-size `SDLTranslatorResponder` layer above it) was never the problem. `sdl_mtlimp.m` now clears `hasShadow` on fullscreen windows after `SDL_ShowWindow` (that is when SDL makes the window fullscreen and sets its style). GL1's fullscreen isn't direct for the same reason; GL1 is left alone (it goes away in Phase 4).
- **Borderless fullscreen = `r_fullscreen 1` + `r_noborder 1`:** `SDL_WINDOW_FULLSCREEN_DESKTOP`, a native macOS fullscreen Space at the desktop resolution (`r_mode` ignored, no display mode change). It's the only borderless mode that can go direct on macOS: a borderless window at the normal level always sits under the menu bar. Alt+Enter (which toggles `r_fullscreen`) keeps `r_noborder`, so it switches between borderless fullscreen and a borderless window. `r_fullscreen 1` alone stays SDL's exclusive fullscreen at `r_mode`'s size. `glConfig.isFullscreen` is set in both, so the client ignores resize events and the front end allows overbright in both, as in GL1's fullscreen.
- **Game Mode:** the Info.plist already had `LSApplicationCategoryType public.app-category.games` (upstream); `GCSupportsGameMode` is added (`cmake/Info.plist.in`). gamepolicyd logs show Game Mode turning on for both fullscreen modes ("Found game … labelReason=appStoreCategory", "Full screen gaming session is now active", "Game mode status is now on") and pausing when the game leaves fullscreen. The category alone triggers it, so session 5's fullscreen runs most likely had Game Mode on too.
- **Exclusive fullscreen at the desktop's own mode still reconfigures the display when it exits** (one `kCGSDisplayWillReconfigure` at exit in gamepolicyd's log, none at entry): SDL restores the desktop mode. Harmless here; borderless fullscreen doesn't do it.
- **Drawables: `r_metalDrawables` 0 (new default) = automatic: 3 for vsync in fullscreen or borderless, 2 otherwise;** 2 or 3 force a count. Direct to display with vsync and 2 drawables, the game ran at 125 fps on the 240 Hz display (each frame waited ~7 ms in `nextDrawable`), which would also halve the command rate. 3 keep 240 fps at the same latency. Without vsync, 3 make things worse (more presented frames never shown, higher latency), and a vsync'd window already runs at 240 with 2. Numbers in §2 "per display mode".
- **Gamma and overbright per mode match GL1.** Both renderers run the same `R_SetColorMappings`, and `glConfig.deviceSupportsGamma` is true in both (GL1: SDL's gamma call succeeds on macOS 26; Metal: `!r_ignorehwgamma`). Checked with `+set r_gamma 1.5 +set r_overBrightBits 1 +gfxinfo`:
  - GL1: 1 overbright bit in fullscreen, 0 in a window;
  - Metal: 1 in fullscreen, 1 in borderless, 0 in a window.

  MOHAA's default `r_overBrightBits` is 0, so by default nothing is overbright in any mode. The "GAMMA:" line in the startup log always says 0 bits: `GfxInfo_f` runs before `R_SetColorMappings` (upstream order). `gfxinfo` shows the real value.
  - **Where the ramp goes:** GL1 loads it into the display's hardware table. Read back with `CGGetDisplayTransferByTable`, it holds gamma 1.5, plus the doubling in fullscreen. On this Mac it's back to identity within 0.05–0.45 s, most likely because of the user's display-tint daemon (`~/.local/bin/dusk tint`, left running), so here GL1 with overbright shows the half-bright scene. Metal applies the same table in its final pass: unaffected by other gamma tools, never touching the rest of the desktop, and working in a window too.
  - **Color space:** the `CAMetalLayer` is tagged sRGB (its default; SDL also tags its windows sRGB, measured in the test program), and the display pipeline converts to the display's profile (DELL G2524H) without leaving direct-to-display. GL1's surface lives in the same sRGB-tagged SDL window (not checked separately).

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
- `CAMetalLayer`: `BGRA8Unorm` (not sRGB: Q3 math is gamma-space), `framebufferOnly YES`, opaque, `maximumDrawableCount` 2, or 3 for vsync in fullscreen (`r_metalDrawables`, session 6), `displaySyncEnabled = r_swapInterval`, `presentsWithTransaction NO`, `drawableSize` = window pixels.
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
4. **Fullscreen must hit direct-to-display** (compositor bypass): opaque layer, drawable size = native display pixels, no overlays or transforms. Verify with the Metal Performance HUD ("Direct") and Metal System Trace. Offer a borderless-fullscreen mode that keeps direct-to-display, so the user doesn't pay compositor latency for "windowed fullscreen". Tell the user that a plain window is always composited (+1 refresh). *Session 6: and no window shadow (a shadow forces an offscreen pass); measured with `display_trace.py`, which prints Instruments' reason when a frame isn't direct.*
5. **Game Mode and scheduling.** Info.plist `LSApplicationCategoryType = public.app-category.games` + `GCSupportsGameMode` (macOS Game Mode in fullscreen: CPU/GPU priority, doubled Bluetooth sampling for controllers and AirPods). While in a game: `NSProcessInfo beginActivityWithOptions: NSActivityLatencyCritical | NSActivityUserInitiated` (no App Nap, minimal timer coalescing), and the main thread at `QOS_CLASS_USER_INTERACTIVE` (P-cores).
6. **Low overhead in the hot path:** no per-frame heap allocations in the renderer or backend; one `@autoreleasepool` per frame; cached PSOs, depth/stencil and sampler states (no lookups by string); state changes filtered on the CPU; hidden visibility, LTO, and the renderer linked statically in Phase 4; no Objective-C in the front end; no GPU→CPU readbacks in the frame (flares/screenshots are async).
7. **Gameplay feel is not ours to change silently.** Anything that alters frame pacing or usercmd timing (the 1 ms frame floor, `com_maxfps` precision, `Sys_Milliseconds` resolution) affects movement and online feel: measure, propose, and get the user's OK first. The facts players rely on, which must stay exact:
   - The engine clock is whole milliseconds. `Com_Frame` uses `minMsec = 1000 / com_maxfps` (integer division, rounds down), and `bias` only catches up after a frame overran. So every value runs at even whole-ms frames, but values that don't divide 1000 are silently rounded *up* to the next rate that does: 240 → 4 ms frames = 250 fps, 144 → 6 ms = ~167 fps, 120 → 8 ms = 125 fps. Vanilla already wakes ~1 ms early and polls `select(0)` until the ms tick flips, so divisor rates are close to tick-exact today. The cost is ~1 ms of polling CPU per frame.
   - `com_maxfps` ≥ 1001 gives `minMsec 0`: no floor, and 0 ms frames are bumped to 1 ms by `Com_ModifyMsec` (`common.c:2188`), so the game clock runs fast (existing upstream bug). `com_maxfps 0` and timedemo use the 1 ms floor.
   - `cl_maxpackets` (clamped 30–125, default 30) sends a packet only when ≥ `1000 / cl_maxpackets` ms have passed since the last one (`client/cl_input.cpp:895-906`). Commands in between are bundled into the next packet, so none are lost. The real packet rate is fps / n: 250 fps + 125 packets = every 2nd frame (even, 125/s); 125 fps + 100 = every 2nd frame, only 62.5/s.
   - The server runs each usercmd as soon as its packet arrives (`server/sv_client.c:1963`); `sv_fps` is 20. There is no lag compensation anywhere, so packet timing directly affects hit registration.
   - MOHAA's pmove has no per-frame velocity snapping (unlike Q3's 125/333 fps jump physics) and chops moves into ≤ 66 ms steps (`fgame/bg_pmove.cpp:1530`), so fps doesn't change jump height (from a code read, not measured).
   - **The command rate sets movement physics** (JayRewd, 2026-10-17). pmove works out speed and direction over each command's time window, and shorter windows change step heights and sliding along surfaces: as designed up to ~85 commands/s, okay-ish at 125, noticeable at 250, a real problem at 500+ ("can't even walk on the terrain"). The server simulates the same commands, so this can't be fixed client-side. The user plays at 250 (normal mode) and walks on terrain fine. In normal mode fps = command rate, so **normal mode must stay at ≤ 250 fps** (125 / 200 / 250); high render rates only come through the decoupled mode. Above 1000 fps normal mode also runs the game clock fast (turbo); the fix comes with the command clock (82cb483d idea).
   - Any limiter change must keep divisor values bit-exact (250 → every frame exactly 4 ms) and should be checked with frame-time and packet-spacing histograms.

**Planned latency work, by phase:**
- **Phase 1:** CAMetalLayer configured per rules 2–4 (drawable count, `displaySyncEnabled` from `r_swapInterval`, opaque layer, `presentsWithTransaction NO`); the wait-placement rule built into the frame loop from day one. *Done in session 2; drawables come from a helper thread with vsync off, see §3.2 "Drawable acquisition".*
- **Phase 2:** **latency probe** `cl_latencyStats 1`. For each frame, the time from the newest mouse event folded into the usercmd (mach time stamped at pump time, µs) to the frame's real present time (`MTLDrawable addPresentedHandler` → `presentedTime`). Log p50/p95/p99 over N frames, plus input→commit. For GL1, only input→`SwapBuffers` return is available (a lower bound; documented). Record SDL+Metal numbers windowed and fullscreen in §2. *Done in session 5: §2 "Input → present latency" (numbers and the Instruments cross-check), §3.2 "Added in session 5" (design). It also reports GPU-done times and the OS event timestamps.*
- **Phase 3:** direct-to-display verified in fullscreen and borderless fullscreen; Info.plist Game Mode keys; latency numbers per display mode recorded. *Done in session 6: the window shadow kept fullscreen from going direct. Both fullscreen modes now go direct on every frame: pump → present 2.1 ms at 250 fps, and Core Animation's present feedback is right. Borderless fullscreen = `r_fullscreen 1` + `r_noborder 1`. Game Mode turns on in both. See §2 "per display mode" and §3.2 "Added in session 6". Tell the user: play in fullscreen or borderless, not in a window.*
- **Phase 5 (performance and latency):**
  - targets the session 5 probe found (§2):
    - the compositor's two latency modes in a window (~5 vs ~9.7 ms at 250 fps);
    - the ~0.3 ms from commit to GPU start;
    - the vsync queue (11.9 ms in fullscreen) and the late GPU finish with vsync on;
  - tune frames in flight / drawable count with the probe;
  - **raw mouse input via GCMouse** (GameController framework: unaccelerated HID deltas from a high-frequency handler, accumulated off the main thread and read at `IN_Frame`), usable while SDL still owns the window. Pulled ahead of Phase 6 because it's the biggest input-path win; verify first whether SDL's relative mode on macOS applies pointer acceleration;
  - **Client timing redesign (approved by the user 2026-10-16, both parts):**
    1. **Precise pacing, the default.** A µs-precise frame limiter on `mach_absolute_time` (sleep with `mach_wait_until`, or `kevent` EVFILT_TIMER with `NOTE_CRITICAL|NOTE_MACHTIME`, minus a learned oversleep estimate; at least one network poll per frame; a final spin of ~50–100 µs with no system calls). Caps are honored exactly in µs (JayRewd's approach). Divisor caps stay tick-exact as in stock (250 → every frame 4 ms). Non-divisor caps run at the requested rate instead of stock's round-up: **stock's integer-ms limiter (`minMsec = 1000/com_maxfps`) turns 240 into 250 fps; JayRewd's µs limiter, and ours, honor 240 exactly.** In classic mode (one command per frame) their command lengths then alternate (4/5 ms at 240), which is what any client at such a frame rate sends; in the decoupled mode commands keep their own exact clock. Vanilla is already near tick-exact for divisors, so the expected gains are less polling CPU (~1 ms/frame today), exact non-divisor caps, correct behavior above 1000 fps (with the `Com_ModifyMsec` 0 ms fix), and an fps counter in µs. JayRewd's present guard (never present two frames closer than the cap) is **not used**: it waits after input sampling (§4b rule 1) and mainly serves VRR, which the user will never use. Commands, `serverTime` and `msec` stay whole milliseconds. Measure vanilla's frame-time and packet-spacing histograms first, then compare.
    2. **Decoupled command and render clocks, an optional mode** (cvar, off by default until the user has tested it online). Usercmds are created on their own exact integer-ms schedule, independent of rendering. **Command rate 85–250/s, never higher** (physics, see rule 7). Development starts at 125 (JayRewd's tested rate), then 250: at 250 the mode must send exactly the stream normal mode sends at 250 fps today (same command lengths, key/button sampling, bundling, a packet every 2nd command = 125/s), which is known-good on terrain. Check terrain walking on terrain-heavy maps; reproduce and explain JayRewd's terrain breakage so we don't repeat it. **Default 125/s, with 250 optional** for the familiar feel (decided 2026-10-17). Packets are capped at 125/s (`cl_maxpackets`), so 250 commands reach the server in the same packets at the same moments as 125. 250 only splits each packet into two 4 ms physics steps, which bend step heights and slides. Rendering runs at any rate (uncapped or any cap, not just divisors of 1000), and every rendered frame uses the newest mouse input for the view. Touches `client/cl_input.cpp` (`CL_CreateCmd`/`CL_SendCmd`/`CL_ReadyToSendPacket`), `cl_cgame.cpp` timing, and cgame prediction of the in-progress command. **Must prove** with packet/command histograms that the server receives a stream identical to a steady 250 fps / 125 packet client (same command lengths, same packet spacing, same bundling) before the user tests it online.
       **Movement display between commands, selectable by cvar:**
       - (a) **predict to now:** a never-sent virtual in-progress command with `serverTime` = now, the held keys and live angles. No added delay. Prediction-error detection/smoothing in cgame's `CG_PredictPlayerState` must ignore it. Key changes or latched taps inside a command window can cause small corrections.
       - (b) **JayRewd's lerp:** shows the state at `cg.time − interval`, i.e. 0..1 interval older than the newest prediction (~2 ms average at 250/s). The safe fallback.
       Default: whichever tests cleaner. Compare them with the latency probe and `cg_showmiss`/prediction-error logs on ladders, movers/elevators, teleports, jump/crouch/lean/prone, weapon switches, high ping and packet loss. JayRewd reports CPMA (Q3) did predict-to-now; not checked.
       **Shot aim taken at the moment of the click** (at 125/s a shot would otherwise use aim from up to 8 ms after the click, which costs flick shots):
       - **Click-time aim:** when the fire button goes down between commands, latch `cl.viewangles` at that instant (render-frame resolution, ~1 ms at 960 fps). The next scheduled command carries the fire button with those angles; later commands use the live angles again. Commands stay perfectly regular.
       - **Dropped (2026-10-17): an extra off-schedule shot command** ("shooting on framerate"). Every command the server receives is a physics step, so an extra short command bends movement; JayRewd's own test build came to the same conclusion. Extrapolated inputs fail too; anything finer than the command needs a new protocol (sub-tick timestamps), which existing servers don't have.
       Click-time aim sends no extra commands and no made-up data: the same single 125/s command, whose angle is the one the player really had at the click. It reaches the server in the same packet, so it doesn't get the shot there sooner; it fixes aim drift during flicks. Side effect: that one command's movement direction uses the click-time angle (negligible: a few degrees for one 8 ms step during a flick). View angles are already decoupled (updated every render frame, sent with each command).
       **Check:** local firing feedback (muzzle flash, recoil, view-model animation, sound) shows on the frame of the click via predict-to-now, if MOHAA's cgame predicts it locally; otherwise note what waits for the server.
    3. **Suggested fps cap from frame-time percentiles** (after part 1; idea from JayRewd's fpsbench). A real-time bench (actual pacing, not a timedemo) reports p95/p99/p99.9 frame times and *suggests* a cap of floor(p99 fps / 100)·100 − 100. That closes the gap between p99 and p99.9 for consistent frame times. The trade-off: slightly higher average latency than uncapped; the user chooses. It also answers which render cap the Mac sustains (e.g. 480 / 720 / 960 at 1080p in a busy match). In the decoupled mode any render cap is fine; in classic mode it never suggests above 250 (physics) and only divisors of 1000 (125 / 200 / 250), since non-divisors make uneven commands. It prints the value / a ready command and never writes archived cvars itself; temporary cvars are never saved into the config, even on a crash.
    4. **Low-latency vsync, the tear-free option** (moved up 2026-10-17: with a cap at the refresh rate and vsync off, the tear line parks in one spot). `displaySyncEnabled YES`, 2 drawables, 1 frame in flight, paced by `CAMetalDisplayLink` (`preferredFrameLatency` 1). **Just-in-time frame start:** sleep *before* input sampling so the frame finishes just before the vsync deadline (a moving estimate of frame work time + margin; §4b rule 1 stays intact). Compare with the latency probe against vsync off at the same cap.
    **Recommended setup for the user** (240 Hz display, no VRR, recorded 2026-10-17):
    - Decoupled mode, commands at 125/s (250 optional for the familiar feel), click-time shot aim, vsync off, fullscreen direct-to-display.
    - Render cap 960 (or 1000) if the part 3 bench shows the Mac holds it at p99 at 1080p in a busy match, otherwise 480. Multiples of 240 park tear lines in fixed spots (four tiny ones per refresh at 960); 1000 makes them roam. The user picks by eye.
    - Tear-free alternative: 240 + low-latency vsync (part 4).
    - In classic mode: stay at ≤ 250 fps (125 / 200 / 250). 500 / 1000 break movement physics (rule 7), and above 1000 is turbo. Classic mode warns above 250 and hard-caps at 1000. (Corrects session 1's earlier "500 / 1000" advice.)
    The user will never use VRR: no VRR work, no present guard.
  - **Prior art: the user's friend's commits (JayRewd, reviewed 2026-10-16, ideas only; the user wants leaner/better re-implementations, not copies; credit JayRewd in commit messages when an idea is used):**
    - c3294f8c: µs frame deadline, `com_maxfps` above 1000.
    - 82cb483d: `cl_cmdfps` separate command clock. Our version adds a never-sent "virtual in-progress command" predicted to *now*, and keeps its cgame display lerp (state up to one command interval in the past) as the selectable fallback; 250 or 125 cmd/s.
    - 92810e4c: learned-oversleep µs sleeping (adopt); the present-anchored PresentGuard spin (skip: it waits between input sampling and present); `cl_lowLatencyInput` late mouse (redo as a view-only GCMouse read in part 2); the fpsbench percentiles + cap suggestion (adopt as part 3, suggest-only).
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

### Phase 1: Build plumbing, Metal window layer, renderer_metal skeleton. **Status: done (session 2, 2026-10-16); the 4K@2× HiDPI check was carried over and done in session 4 (rendering passes; HiDPI input not exercised).**
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

### Phase 2: Core draw path. **Status: done (2a session 3, 2b session 4, 2c session 5, 2026-10-17).**
Session 5 result (2c):
- **`cl_latencyStats 1`**: design in §3.2 "Added in session 5", numbers in §2 "Input → present latency". Output example: `cl_latencyStats, whole run: 4979 frames with mouse input, 4779 shown by their own frame, 200 by a later one; 0 lost, 0 stale`, then p50/p95/p99 lines for:
  - newest event → pump;
  - oldest event → pump;
  - pump → commit;
  - pump → GPU done;
  - pump → present;
  - newest event → present.

  GL1 prints the EndFrame-return lower bound instead.
- **New tools:**
  - `port/latency.sh <renderer> <name> [seconds] [engine args]`: map, probe, synthetic mouse, results; `MOUSE=0`, `LATSTATS=2`, `TRACE=1`;
  - `port/mousemove.c`;
  - `port/display_trace.py`: what the display showed (swaps per display, direct-to-display or why not, presented-handler statuses), and the probe's present times matched against the display swaps.
- **Done criteria:**
  - **Build: pass,** no new warnings.
  - **Bench: pass.** Metal 0.40 ms vs GL1 1.08 ms work (this sitting); the probe on changes nothing measurable.
  - **Fixed views: unchanged.** `pair dm6`: 1454 / 135 / 496 / 2533 / 1404 / 152 / 117 / 1 / 1979 pixels; only view 8 differs from session 4 (2411), the HUD weapon bar's real-time highlight. `pair dm5`: 1732 / 1337 / 749 / 558, identical.
  - **Latency numbers windowed and fullscreen on the 240 Hz display: recorded,** with the fullscreen limits above.
- Phase 2 is complete: the bench demo plays fully on Metal; the fixed views match GL1 by eye and by diff; Metal's work per frame is 0.39–0.41 ms against the 0.45 ms target; input → present numbers are recorded.

Session 4 result (2b):
- **Fixed 3D views** (`port/shots/shot_dm6.cfg`: 9 views; `shot_dm5.cfg`: 4 views; run with `port/shot.sh pair <name> <cfg>`, which runs both renderers and diffs every view; `fixed` runs one renderer, `views` re-diffs). How they work:
  - `+set g_gametype 0 +set thereisnomonkey 1 +devmap <map> +set fixedtime 10` (from the cfg's `// args:` line). `thereisnomonkey 1` is what unlocks the cheat commands (without it the game resets `cheats` to 0 at the first cheat command: that's why session 3's `tele` did nothing). `fixedtime 10` makes every frame advance game time by exactly 10 ms, so the frames are identical in every run: **both renderers are bit-for-bit deterministic run to run.**
  - `noclip`, `tele x y z`, `face pitch yaw roll` (3 floats; pitch < 0 looks up); `spawn models/human/allied_airborne_soldier.tik` puts a skinned model 40 units in front of the player; the view model needs noclip **off** plus `wuss` and `use models/weapons/thompsonsmg.tik`.
  - Three captures per view one frame apart; Metal's middle capture is compared with the best of GL1's three (GL1's frame age is printed: 2 in every run so far).
- **Result** (pixels of 921600 differing; >8 levels in brackets): mohdm6 views: courtyard + sky 1454 (12), courtyard 90° 135 (2), roofs + sky 496 (4), sandbags 2533 (111), courtyard 270° 1404 (16), indoor light + lockers 152 (9), ceiling bulb 117 (41), spawned soldier 1 (1), view weapon (Thompson + arms) indoors 2411 (3, of which ~1980 are the weapon bar's highlight, which also differs between two Metal runs: real-time clock). mohdm5 (fog farplane 1800, terrain, snow): 1732 (9), 1337 (49), 749 (8), 558 (3). By eye: identical.
  - Where the rest comes from (measured): almost all of it is HSR tie-breaking on coincident faces (§3.2 "Added in session 4"): with `r_drawstaticmodels 0` mohdm6 drops to 0–135 pixels per view and the sandbag view to exactly 0; with a `discard` in every pipeline mohdm6 drops to 2–24 pixels except the ceiling bulb (116: thin cage wires; not alpha-test rounding, not explained, 41 pixels > 8). mohdm5 without static models keeps 550–880 pixels off by 1–2 (not the fog blend formula; probably terrain LoD stitching, i.e. ties again; not chased further).
  - Ruled out along the way, each by a measurement: texture filtering (every `r_textureMode` gives the same), mouse input (`in_mouse 0`), vertex submission and lock snapshots (copying every draw, per-triangle index rotation), `P · (MV · v)`.
- **Fixed:** the clip-z remap moved into the vertex shader (§3.2).
- **Exports:** `SwipePoint`/`DrawBackground` stay NULL like GL1 (§3.2).
- **Bench, unlocked screen:** §2 "Metal, Phase 2b": Metal 0.39 ms vs GL1 0.88–0.96 ms work; `r_swapInterval 1` runs at 237–240 fps on both.
- **HiDPI (4K@2×, `r_displayIndex 1`): pass for rendering:** drawable and screenshots 2560×1440, HUD scaled like at 720p (`port/shots/shot_hidpi.cfg`). Input on a 2× display (the HiDPI gotchas in §6) was not exercised: needs a person at the mouse.
- **Found, not ours: most maps crash the game when loaded locally.** `devmap`/`map` of dm/mohdm1–4 and mohdm7, obj/obj_team1–3 and m2l1 (also in FFA with `sv_maxclients 8`, and from a cfg after the menu) crash in `SimpleEntity::SetOrigin` (NULL `this`, from a map script setter at `Level::ServerSpawned`), with both renderers and with the upstream GL build (`port/build.sh gl`). Only mohdm6 and mohdm5 load. So the views for portal sky (m2l1), objective maps and more terrain (`shot_dm4.cfg`, `shot_obj3.cfg`, `shot_m2l1.cfg`) are written but can't run yet. Game code: investigate separately; any fix needs the user's OK (hard rules).
- **Not covered by the views yet:** portal sky, mirrors, marks/decals (mohdm6 has no `.dcl`; bullet marks need shooting), swipes, sprites/flares, dlights, polygon-offset decals, mirrored cull. Phase 3's fixed view list takes them once maps load.

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

### Phase 3: Full feature parity. **Status: in progress (3a done in session 6, 2026-10-17; next 3b).**
Session 6 result (3a, display modes):
- **Direct to display in fullscreen: fixed.** The cause was the window shadow on SDL's exclusive-fullscreen window (§3.2 "Added in session 6"); `sdl_mtlimp.m` clears it. **Borderless fullscreen** is new: `r_fullscreen 1` + `r_noborder 1`, a native fullscreen Space at the desktop resolution. Both go direct on every frame (Instruments).
- **`r_metalDrawables` 0 = automatic** (3 for vsync in fullscreen): direct-to-display vsync with 2 drawables ran at 125 fps.
- **Game Mode:** `GCSupportsGameMode` added to `cmake/Info.plist.in` (the games category was already there). gamepolicyd turns Game Mode on in both fullscreen modes, not in a window.
- **Latency matrix** (window / fullscreen / borderless × 250 / ~1000 fps × vsync off / on) in §2 "per display mode". **Fullscreen and borderless: pump → present 2.1 ms p50 at 250 fps** (event → present 3.0–3.3 ms), 8.06 ms with vsync. The window ranges from 2.4 to 13 ms depending on the compositor's mode that run. Core Animation's present feedback is right when direct, at every rate.
- **Gamma/overbright per mode match GL1** (fullscreen 1 bit, window 0, with `r_overBrightBits 1`). GL1's hardware ramp gets overridden on this Mac by the user's display-tint daemon; Metal's in-shader ramp doesn't (§3.2).
- **Tools:**
  - `port/display_trace.py` takes `PROCESS=<name>` to look at another process's frames;
  - a standalone SDL2 + Metal test program for direct-to-display experiments in `build.noindex/experiments/dtd/` (not committed: build output, it may be deleted).
- **Done criteria for 3a:**
  - **Build: pass,** no new warnings.
  - **Fixed views: unchanged.** `pair dm6`: 1454 / 135 / 496 / 2533 / 1404 / 152 / 117 / 1 / 2437 pixels (view 8 = the weapon bar's real-time highlight, 1979–2437 between sittings). `pair dm5`: 1732 / 1337 / 749 / 558, identical to sessions 4 and 5.
  - **Bench: pass** (window, unchanged path): Metal 0.395 / 0.397 / 0.391 ms work vs GL1 0.928 / 0.937 / 0.917 in the same sitting (§2).
- **Not done in 3a:**
  - exclusive fullscreen at a non-native mode (a display mode change) is untested;
  - the HiDPI 4K display in fullscreen is untested;
  - GL1's fullscreen isn't direct either (same shadow), left as is;
  - Game Mode and `NSActivityLatencyCritical` haven't been measured on vs off (Phase 5).

- Sky box + cloud layers, portal sky, linear fog (all `GLS_FOG*` modes), mirrors/portals (clip distance).
- Q3 flares + MOHAA sun/lens flares, dlights (lightmap ring uploads), stencil shadows (`r_shadows 2`) and blob shadows, ghost textures.
- Cinematics (`DrawStretchRaw` ring), screenshots (`screenshot`, `screenshotJPEG`, levelshots; async readback), gamma/overbright final pass.
- `r_picmip`, `r_textureMode`/anisotropy, `r_ext_*`-equivalent capability cvars, debug views (`r_showtris`, `r_speeds`, debug lines).
- Spearhead/Breakthrough (`com_target_game 1/2`), the `MTLBinaryArchive` pipeline cache.
- Fullscreen and borderless fullscreen hitting direct-to-display, Info.plist Game Mode keys (§4b rules 4–5).
  - Session 5 found that neither renderer's fullscreen goes direct today. Instruments' reason for Metal's layer: "layer requires a potential offscreen render". Find the cause (SDL's view and layer setup, the window style and fullscreen path, `CAMetalLayer` properties) and fix it.
  - Verify with `TRACE=1 LATSTATS=2 port/latency.sh metal <name> 12 +set r_fullscreen 1 +set r_customwidth 1920 +set r_customheight 1080`: `display_trace.py` prints "direct to display" per swap.
  - Then re-run session 5's latency matrix (§2) in window, fullscreen and borderless modes, and check whether Core Animation's present feedback works in fullscreen with vsync off once the layer goes direct.
- **Done when:**
  - The fixed-view harness (`port/shot.sh pair`, session 4: devmap + fixedtime + tele/face, GL1 vs Metal screenshots, per-pixel diff) shows no visible differences on a fixed view list (stock DM + objective + SP maps with sky portals, fog, mirrors, terrain), and the user confirms the look.
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
- Latency work from §4b (frames-in-flight tuning, GCMouse raw input, the client timing redesign: precise pacing by default + the optional decoupled command/render mode + the percentile cap suggestion + low-latency vsync, activity/QoS/Game Mode, audio latency), measured with the latency probe.
- Measure first (bench + `trace` + `sample`/Time Profiler), one change at a time, each recorded here with before/after numbers. Candidates:
  - Static world geometry in GPU-resident buffers (no per-frame re-tessellation/copy).
  - Batching by shader+lightmap; sort keys to minimize PSO/state changes; fewer render passes.
  - GPU skinning (compute) for TIKI; GPU tcMod/tcGen/rgbGen/deforms for the common cases.
  - Faster `RB_Light_Real` sphere lighting.
  - BC texture compression for memory; async texture loading at map load.
  - Metal 4 (argument tables, residency sets, `MTL4CommandQueue`) where measured.
  - Frame pacing (present timing / CAMetalDisplayLink).
- Engine frame floor: `Com_Frame` pads frames to 1 ms with an integer-ms clock, which caps fps near 1000. Making it µs-precise changes how `com_maxfps` behaves online (frame pacing feeds usercmd timing and movement feel), so **ask the user before touching it**.
- **Done when:** the bench work time is ≤ 0.25 ms/frame or the remaining cost is documented as engine-side, p99 frame time is lower than GL1, input→present latency is measured lower than the Phase 2 numbers (and as low as the display mode allows), and the fixed views (`port/shot.sh pair`) show no visual change.

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

**Session 7 = Phase 3, slice 3b (the world features with fixed views).** Start with CLAUDE.md, then Phase 3 (bullets and the session 6 result), §3.2 "Added in session 4" (the fixed-view harness, HSR ties, the swipe/`SwipePoint` question, the map-load crash) and "Added in session 6", and Phase 2's session 4 result (what the views cover so far).

Where things stand (end of session 6):
- **Phase 3a is done** (display modes): committed on `metal`, not pushed. Pushes happen at the end of a phase (CLAUDE.md), so 3a goes out with the rest of Phase 3.
- **Fullscreen and borderless fullscreen go direct to display** (the window shadow was the cause). Borderless = `r_fullscreen 1` + `r_noborder 1`.
- **Latency, 250 fps, vsync off:** pump → present **2.1 ms** in both fullscreen modes, against 2.4–13 ms in a window, depending on the compositor's mode that run. With vsync: 8.06 ms at 240 fps (3 drawables, automatic). Core Animation's present feedback is right when direct, so the probe's fullscreen numbers are real at every rate.
- **Game Mode** turns on in both fullscreen modes.
- **Gamma/overbright per mode match GL1.**
- **Bench:** Metal 0.39–0.40 ms work vs GL1 0.92–0.94 ms. The fixed views are unchanged (dm6 and dm5).
- **For the user** (not yet told in a session report): play in fullscreen or borderless fullscreen. Borderless at the desktop resolution has the same latency as exclusive, plus Spaces/Cmd-Tab and no display reconfigure at exit. The user currently plays in a 1920×1080 window (§4b).
- Code: the backend is `code/renderer_metal/tr_mtl.m` + `shaders/`; the window layer is `code/sdl/sdl_mtlimp.m`.

Phase 3's remaining slices (adjust if something turns out bigger):
- **3b (session 7): the world features with fixed views.**
  - First the local map-load crash (`SimpleEntity::SetOrigin` with a NULL `this` from a map script setter at `Level::ServerSpawned`, both renderers and the upstream GL build; only mohdm5/6 load). It's game code: investigate, then propose any fix to the user before changing it (hard rules). Phase 3's fixed view list needs maps beyond mohdm5/6.
  - Then sky box, portal sky (m2l1), all fog modes, mirrors/portals, Q3 flares + sun/lens flares, dlights, stencil and blob shadows, ghost textures, marks, and the swipe check (throw a grenade, §3.2 "Added in session 4"). Each gets fixed views in `port/shots/` and a `pair` diff against GL1.
- **3c:**
  - cinematics (`RE_StretchRaw`), screenshot variants and levelshots (async readback);
  - `r_picmip`, `r_textureMode`/anisotropy, capability cvars, debug views (`r_showtris`, `r_speeds`, debug lines);
  - Spearhead/Breakthrough;
  - the `MTLBinaryArchive` pipeline cache.
- **3d:** the user's look at the result, and an online smoke test on a real server. The session report should then also recommend fullscreen/borderless to the user (see above). Push at the end of Phase 3.

Gotchas found in session 6:
- **In zsh, `log` is a builtin:** use `/usr/bin/log show …` for the unified log (gamepolicyd: `--predicate 'process == "gamepolicyd"'`, look for "gaming session" and "Game mode status").
- **Instruments' displayed-surfaces table has `detachment-reason` and `detachment-suggestion` columns** next to `direct-to-display`. `display_trace.py` prints the reason; `PROCESS=<name>` points it at another process.
- **Window latency changes with the compositor's mode, from run to run** (2.4–13 ms p50 this session). Measure windows several times, or trace them; compare display modes in one sitting.
- **The user's display-tint daemon (`~/.local/bin/dusk tint`) is running:** it overrides display gamma tables (GL1's hardware ramp), so don't judge GL1's gamma/overbright by eye on this Mac. Never stop it; it's the user's.
- **`GAMMA:` in the startup log always says 0 overbright bits** (printed before `R_SetColorMappings`); run `gfxinfo` later in the run for the real value.
- **Fullscreen runs reach Game Mode**, which may shift scheduling. Compare modes in one sitting (Phase 5 measures Game Mode on/off).

Gotchas found in session 5:
- **`port/latency.sh` takes over the mouse:** the game window grabs it, and synthetic moves are posted for the run's length, about 30 s per run. Every run in session 5 got the keyboard focus by itself. If the user clicks elsewhere, SDL ignores the mouse and the run has no input frames. Check the screen lock first (the script does).
- **Instruments' display labels are misleading:**
  - the 240 Hz G2524H shows up as "external-0" with resolution (3840,2160), and the 60 Hz 4K as "external-1" (1920,1080): tell displays apart by their swap rate;
  - `Frame N` in surface labels starts at 1 with the first present inside the trace and wraps at 1000;
  - the presented-handler table's "Requested Time" is not on the host clock.

  `port/display_trace.py` handles all of this.
- Traces are ~0.3 GB each in `build.noindex/latency/`; `latency.sh` keeps them, so delete them after use.
- ~~Core Animation's present feedback in fullscreen with vsync off is wrong~~: fixed by going direct to display (session 6); still true for a composited fullscreen (GL1).
- macOS `sed` has no `\|` in basic regexes (use `grep -E`/awk). In zsh, `${=var}` word-splits a string variable.
- Session 4's gotchas still apply:
  - the Bash tool runs zsh (use arrays or `${=A}` for several args);
  - `wait N` with `fixedtime` continues after N/msec − 1 frames;
  - several `screenshot`s in one frame overwrite each other;
  - `r_drawworld 0` does nothing;
  - cheats need `+set thereisnomonkey 1`;
  - the weapon bar highlight runs on a real-time clock;
  - most maps crash when loaded locally.
- Sessions 1–3's gotchas still apply:
  - check `CGSSessionScreenIsLocked` before measuring;
  - never pace frames on completed handlers;
  - test cfgs get a prefix (`shot_`, `lat_`);
  - `g_gametype 0 +devmap` needs `finishloadingscreen`;
  - the renderer's `vid_restart` is queued after pending cfg commands;
  - `screencapture` and ScreenCaptureKit have no permission (`CGPreflightScreenCaptureAccess` 0), but posting events does;
  - stale PID files block on the "Abnormal Exit" dialog;
  - `s_initsound 0` crashes;
  - use `sv_maxclients` ≥ 2 for scripted map loads;
  - `glconfig_t` is ABI;
  - HiDPI input on 2× displays is untested (§6 of session 4: `SDL_WINDOWEVENT_RESIZED` compares points with pixels, relative mouse deltas are in points, `SDL_WarpMouseInWindow` expects points).

Open bugs:
- **In the renderer:** none known.
- **Display path:** GL1's fullscreen still isn't direct (same window shadow; left alone, GL1 goes away in Phase 4). The window's latency depends on the compositor's mode (§2).
- **Outside it:** the local map-load crash (game code, upstream).
- **Untested:**
  - HiDPI input, DDS/BC uploads, cinematics, swipes at run time;
  - exclusive fullscreen at a non-native resolution (a display mode change), and fullscreen on the 4K@2× display.
