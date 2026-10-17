# OpenMoHAA native Metal port: plan

Goal (verbatim, see CLAUDE.md): "Create a FULL macOS Silicon Metal native port of openmohaa, with the ultimate goal of stripping OpenGL entirely from the code by the time we're done, and only have it run Metal from start to finish. Look at all possible ways to improve performance while we do it, increase efficiency, etc. Make it as lean and robust as possible."

Branches: `main` = upstream mirror (openmoh/openmohaa a2f34019 at the start). `metal` = this work.

## Status

| Phase | What | Status |
|---|---|---|
| 0 | Survey, baseline, plan, decisions | **Done** (session 1) |
| 1 | Build plumbing + Metal window layer + `renderer_metal` skeleton | Not started (next) |
| 2 | Core draw path (textures, pipelines, 2D/UI, world, entities) | Not started |
| 3 | Full feature parity + screenshot-compare harness + online smoke test | Not started |
| 4 | Delete OpenGL entirely | Not started |
| 5 | Performance | Not started |
| 6 | Native AppKit/GameController (replace SDL2) | Not started |
| 7 | Lean and robust (strip platforms, app bundle, openal-soft, dead code) | Not started |

---

## 1. Findings (session 1 survey)

Paths are relative to `code/` unless they start with `cmake/`.

### 1.1 Build and targets
- CMake ≥ 3.25, Ninja. Options in `CMakeLists.txt:14-40`: `BUILD_RENDERER_GL1` ON, `BUILD_RENDERER_GL2` OFF, `USE_RENDERER_DLOPEN` ON, `USE_OPENAL(_DLOPEN)` ON, `BUILD_SERVER` ON, `BUILD_MACOS_APP` ON (`cmake/platforms/macos.cmake:7`).
- Targets: client `openmohaa` (.app bundle), server `omohaaded`, `renderer_opengl1.dylib` (moved into `openmohaa.app/Contents/MacOS` by `finish_macos_app`), `cgame.dylib`, `game.dylib`, plus launchers (broken on macOS: they read `/proc/self/exe`).
- Install layout: everything goes to `<prefix>/lib/openmohaa/`. `fs_basepath` defaults to the folder that holds the .app. Libraries are found in `Contents/MacOS`, then in `fs_basepath` (`sys/sys_main.c:511`).
- `cmake/platforms/macos.cmake:17-18` sets **deployment target 11.0 and universal arm64;x86_64** as plain variables, which override `-D` flags. Every build is universal until Phase 1 changes it. Every renderer links `-framework OpenGL` (`:15`).
- `find_package(OpenAL)` picks Apple's OpenAL.framework, which has no `alext.h`, and the build fails. `port/build.sh` passes `-DCMAKE_DISABLE_FIND_PACKAGE_OpenAL=ON` (internal openal-soft headers; the library is dlopen'ed as `libopenal.1.dylib`, `client/snd_openal_new.cpp:124`). Nothing builds openal-soft locally, so `port/run.sh` links Homebrew's next to the exe.
- `cmake/libraries/jpeg.cmake:1` and `freetype.cmake:5` only work when a GL renderer is built. A Metal-only build must widen these guards. There is also a `JPEG_DEFINTIONS` typo at `jpeg.cmake:22`.
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

**Benchmark** (`port/bench.sh run gl 3`): timedemo of `port/bench/bench_mohdm6.dm_8` (dm/mohdm6 Stalingrad, FFA, 8 bots, spectator following bots, 120 s, 2398 frames; sha256 `c62f7972735b60c15b32392168f41235849407b637c9ac6004d68a36312320a2`), 1280×720 window on the main display, `r_swapInterval 0`, `com_maxfps 0`, sound muted, fresh default config each run.

| Config | fps (3 runs) | wall ms/frame | **work ms/frame** | GPU ms/frame (`trace`) |
|---|---|---|---|---|
| GL1 `r_primitives 2` (**reference**: the user's setting, GL's fastest path) | 923.4 / 925.9 / 926.2 | 1.081 | **0.934** (limiter wait 0.146) | **0.18** (16% GPU utilization in the timedemo) |
| GL1 `r_primitives 0` (upstream default) | 293.2 / 296.3 / 295.9 | 3.39 | 3.39 | ~0.6 (older estimate method) |

- **"work" is new.** The engine never runs a frame shorter than 1 ms: since Quake 3, `Com_Frame` pads frames to its 1 ms minimum with an integer-ms clock. So fps saturates near 1000, and GL with `r_primitives 2` already sits there. The timedemo now also prints `timedemo work: X ms/frame` (wall time minus Com_Frame's frame-limiter wait, µs clock; `qcommon/common.c` `com_timedemoWaitUsec`, printed in `client/cl_main.cpp CL_DemoCompleted`). **Compare renderers on work ms/frame.** It only measures; it changes nothing in how frames run.
- Run-to-run noise: ±0.3% for `r_primitives 2` and ~3% for `r_primitives 0` (one set gave 286–294 fps). Under `sample` the run is ~4% slower, under xctrace ~5–10% slower.
- **Where the GL time goes** (`sample` profiles; `port/sample_by_library.py`):
  - `r_primitives 2`: the frame is dominated by `RB_SwapBuffers`. It calls `qglFinish()` every frame (`tr_backend.c:1429`), and the CPU blocks in `GLDShareGroupRec::waitUsage` until the GPU is done, then `CGLFlushDrawable` presents. The GPU is busy only 0.18 ms of each ~1 ms frame, so most of the frame is CPU↔GPU **synchronization latency**, not rendering. The engine's own CPU work is small.
  - `r_primitives 0`: ~1.96 ms/frame in `RB_SwapBuffers` (≈half glFinish wait, half present) and ~1.7 ms/frame drawing the 3D view/HUD (`UIWindowManager::UpdateViews`). Of the latter, ~65% is Apple's GL driver (AGX, AppleMetalOpenGLRenderer, GLEngine, malloc churn from immediate mode) and ~13% renderer code (top: `RB_Light_Real` 4.7%, `RB_SkelMesh` 2.4%, `R_DrawStripElements` 1.3%).
  - Apple's GL splits every game frame into ~8–10 GPU submissions (render passes): 23806 trace frames for ~2400 game frames. On a tile-based GPU each extra pass costs tile loads/stores.
- **What this means for Metal:** with frames in flight (no per-frame wait), one render pass per frame where possible, and no driver state validation, the frame cost should approach max(CPU, GPU) instead of their sum. **Phase 2 target: work ≤ 0.45 ms/frame (≥2× GL1 `r_primitives 2`) on this bench. Phase 5 stretch: ≤ 0.25 ms.** Beyond ~1000 fps the visible fps stays capped by the engine's 1 ms frame floor (see Phase 5).

---

## 3. Decisions

### 3.1 User decisions (asked in session 1, 2026-10-16)
1. **Windowing/input: SDL2 now, native later.** Phases 1–5 keep SDL2 and attach a `CAMetalLayer` via `SDL_Metal_CreateView`. Phase 6 replaces SDL with native AppKit + GameController (GCMouse raw deltas), decided after measuring input latency.
2. **Audio: keep OpenAL-soft.** MOHAA's OpenAL sound code stays. Phase 7 bundles openal-soft in the .app, linked directly instead of dlopen'ed. A native CoreAudio/AVAudioEngine backend is only an option if profiling ever shows a reason.
3. **Platforms: macOS-only, keep the server.** Delete Windows/Linux/Emscripten/x86 asm/Launcher platform code and the GL renderers. Keep the dedicated server target (`omohaaded`) building on macOS for local tests. The user's Linux servers keep using upstream builds; `main` stays the upstream mirror.
4. **Shaders: offline .metallib.** CMake compiles the hand-written `.metal` files into one metallib at build time with the read-only mounted Metal Toolchain. At run time an `MTLBinaryArchive` pipeline cache avoids compile hitches.
5. **Push: yes.** Push `metal` to origin at the end of every phase (recorded in CLAUDE.md).

### 3.2 Technical decisions (session 1; revisit only with a reason, tell the user if one changes)
- **renderer_metal starts as a copy of renderergl1** (`code/renderer_metal/`, front end unchanged), with every qgl call replaced by a small GL-free backend API (`tr_mtl.h`, C) that mirrors GL1's state model, as in the Vulkan port. Keeps visual parity, keeps upstream renderer fixes portable (by hand, file for file), and lets GL1 run side by side for A/B until Phase 4. GL-isms are removed later where it pays (Phase 5).
- **Backend language: Objective-C with ARC** (`.m` files) behind the C API. No metal-cpp, no Swift. The front end stays C/C++.
- **Classic Metal API first** (`MTLCommandQueue`, `MTLRenderCommandEncoder`, automatic hazard tracking). Metal 4 (`MTL4CommandQueue`, argument tables, residency sets) is evaluated in Phase 5 where it measurably helps.
- **The metallib is embedded in the renderer binary** (generated C array), so there's no file lookup at run time. Compiled with `-std=metal4.0 -target air64-apple-macos26.0 -O3` (verified working with the mounted toolchain in session 1).
- **Renderer packaging:** a dylib selected with `cl_renderer metal` during Phases 1–3 (A/B against `opengl1`). In Phase 4 it's linked statically into the client (`USE_RENDERER_DLOPEN OFF`: one binary, LTO across client+renderer, no PLT). Hidden visibility from day one.
- **Deployment target macOS 26.0, arm64 only** (Phase 1 edits `cmake/platforms/macos.cmake`, a tiny diff).

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
- `CAMetalLayer`: `BGRA8Unorm` (not sRGB: Q3 math is gamma-space), `framebufferOnly YES`, `maximumDrawableCount 3`, `displaySyncEnabled = r_swapInterval`.
- HiDPI: drawable size in pixels → `glConfig.vidWidth/vidHeight`. Check the HUD/UI scaling on the 4K@2× display.
- Fills `glConfig` (renderer string "Apple M4 (Metal)", maxTextureSize 16384, stencil 8, no hardware gamma: gamma is applied in-shader).

**Frame model:**
- One `MTLCommandQueue`. 3 frames in flight (dispatch_semaphore). One command buffer per frame, begun lazily at the first backend call (2D exports draw outside the queue).
- Rendering goes to an offscreen color target (`RGBA8Unorm`, private) + `Depth32Float_Stencil8`. At end of frame one fullscreen triangle copies it to the drawable and applies **gamma + overbright in the shader**, reproducing GL1's behavior per mode (overbright only where GL1 has it: fullscreen with hw gamma). Then present + commit. Never wait on the GPU except at shutdown and readback.
- Screenshots and video frames: blit from the offscreen target (pre-gamma, as GL1's back-buffer read) into a shared buffer, written out in the completion handler (async, no stall).

**Pipelines and shaders (`renderer_metal/shaders/*.metal`):**
- One generic stage shader pair, specialized with function constants:
  - texture count 0/1/2 and texenv mode (modulate/add/replace/decal, from `GL_TexEnv`);
  - vertex color on/off;
  - alpha test (none / GT0 / LT128 / GE128 / foliage thresholds from cvars);
  - fog mode (none / view color / black / white, from `GLS_FOG*`) as linear eye-distance fog;
  - clip plane (`[[clip_distance]]` for mirrors/portals).
- Separate small shaders: final pass (gamma/overbright), lines/debug, clears.
- PSO cache keyed by (function constants, blend src/dst, color write mask, vertex layout, formats, sample count). Depth/stencil state cache keyed by depth func/test/write + stencil ops. Sampler cache by filter/mip/wrap/aniso. `MTLBinaryArchive` on disk in `fs_homepath` (Phase 3/5).
- GL conventions in the backend: the front end keeps GL projection matrices; the backend remaps clip z from [-1,1] to [0,1] and flips the viewport/scissor origin (bottom-left → top-left). Front-face winding matches GL CCW after the flip. `glDepthRange` (weapon depth hack) → `MTLViewport` znear/zfar. Polygon offset → `setDepthBias`. GL lines → Metal line primitives.

**Resources:**
- Per-frame shared `MTLBuffer` ring (Apple Silicon unified memory) for tess vertices/indices (memcpy + `setVertexBuffer:offset:`). Small uniforms via `setVertexBytes`/`setFragmentBytes`.
- Textures: `image_t.texnum` → index into a backend texture table; handles are never reused while frames are in flight. RGBA8 uploads (CPU mips kept first for parity). DDS → BC1/BC3 (natively supported). Mid-frame re-uploads (cinematics, ghost textures, dlight lightmaps) use a small per-frame ring so recorded draws keep their contents. Deferred destruction after the frame's completion.
- Flares: replace the `glReadPixels` depth test with visibility-result (occlusion) queries or a depth read one frame late.

**Threading:** single-threaded, like GL1 (Metal encoding is cheap, and there's no driver thread to fight). A render thread or parallel encoding only if Phase 5 profiling shows the CPU is the limit.

---

## 5. Phases

Each phase can take several sessions (slices). One session = one phase or slice (see CLAUDE.md).

### Phase 0: Survey, baseline, plan, decisions. **Status: done (session 1, 2026-10-16).**
Done: survey (§1), `port/` scripts, reference demo, GL1 baseline (§2), timedemo work metric, decisions (§3), CLAUDE.md, this plan.

### Phase 1: Build plumbing, Metal window layer, renderer_metal skeleton. **Status: not started.**
- **1a:**
  - `cmake/platforms/macos.cmake`: arm64 only + deployment target 26.0.
  - New `cmake/renderer_metal.cmake` (option `BUILD_RENDERER_METAL`): `renderer_metal` dylib built next to `renderer_opengl1` in the .app; frameworks Metal, QuartzCore; `-fobjc-arc`; hidden visibility; jpeg/freetype guards widened.
  - Offline metallib step (custom command running `$OMM_METAL_BIN/metal`, a clear error if the toolchain isn't mounted, embedded as a C array).
  - `port/build.sh metal` flavor (GL1 + Metal in one install, so `cl_renderer` switches).
  - `sdl/sdl_mtlimp.m` (window, CAMetalLayer, HiDPI, modes, fullscreen, `vid_restart`, `IN_Init`).
  - `renderer_metal/` = copy of renderergl1 with qgl call sites converted to `tr_mtl.h` calls whose implementations are stubs, except device/queue setup and clear+present.
- **1b (if 1a runs long):** finish the call-site conversion; maps load without crashing.
- **Done when:**
  - `port/build.sh metal` builds; `renderer_metal.dylib` has no GL symbols (`nm -u` shows no `gl*`/`CGL*`) and doesn't link OpenGL (`otool -L`).
  - `port/run.sh metal +set cl_renderer metal` opens a window that shows the Metal clear color every frame, at the right size: 1280×720 window, 1920×1080 fullscreen, and full pixel resolution on the 4K@2× display.
  - The menu loop runs (invisible), and `+map dm/mohdm6` loads and runs without crashes. `vid_restart`, fullscreen toggle and quit work.
  - `cl_renderer opengl1` still works, and the GL1 bench is re-recorded for the arm64-only build (expected unchanged).

### Phase 2: Core draw path. **Status: not started.**
- **2a:** frame ring, buffers, texture upload/formats/samplers/deferred free, PSO + depth-stencil caches, the generic stage shader (1–2 textures, texenv, alpha test, blend), the 2D path (`Set2DWindow`/ortho, StretchPic/TilePic/TrianglePic, fonts, DrawBox/LineLoop, Scissor). Menus, console and HUD look right.
- **2b:**
  - 3D: projection/z remap, viewport flip, cull, depth range, polygon offset;
  - BSP surfaces, patches, terrain, lightmaps (multitexture collapse), vertex lighting, CPU tcGen/tcMod/deforms;
  - entities incl. TIKI CPU skinning and sphere lighting, static models, sprites, marks/decals, swipes. GL1 leaves the `SwipePoint`/`DrawBackground` exports NULL: find out how the client copes, then match or fix deliberately and note it here.
- **Done when:** the bench demo plays fully on Metal; 5 fixed views match GL1 by eye and by screenshot diff; first Metal bench numbers are in §2's table (target work ≤ 0.45 ms/frame).

### Phase 3: Full feature parity. **Status: not started.**
- Sky box + cloud layers, portal sky, linear fog (all `GLS_FOG*` modes), mirrors/portals (clip distance).
- Q3 flares + MOHAA sun/lens flares, dlights (lightmap ring uploads), stencil shadows (`r_shadows 2`) and blob shadows, ghost textures.
- Cinematics (`DrawStretchRaw` ring), screenshots (`screenshot`, `screenshotJPEG`, levelshots; async readback), gamma/overbright final pass.
- `r_picmip`, `r_textureMode`/anisotropy, `r_ext_*`-equivalent capability cvars, debug views (`r_showtris`, `r_speeds`, debug lines).
- Spearhead/Breakthrough (`com_target_game 1/2`), the `MTLBinaryArchive` pipeline cache.
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

### Phase 5: Performance. **Status: not started.**
- Measure first (bench + `trace` + `sample`/Time Profiler), one change at a time, each recorded here with before/after numbers. Candidates:
  - Static world geometry in GPU-resident buffers (no per-frame re-tessellation/copy).
  - Batching by shader+lightmap; sort keys to minimize PSO/state changes; fewer render passes.
  - GPU skinning (compute) for TIKI; GPU tcMod/tcGen/rgbGen/deforms for the common cases.
  - Faster `RB_Light_Real` sphere lighting.
  - BC texture compression for memory; async texture loading at map load.
  - Metal 4 (argument tables, residency sets, `MTL4CommandQueue`) where measured.
  - Frame pacing (present timing / CAMetalDisplayLink).
- Engine frame floor: `Com_Frame` pads frames to 1 ms with an integer-ms clock, which caps fps near 1000. Making it µs-precise changes how `com_maxfps` behaves online (frame pacing feeds usercmd timing and movement feel), so **ask the user before touching it**.
- **Done when:** the bench work time is ≤ 0.25 ms/frame or the remaining cost is documented as engine-side, p99 frame time is lower than GL1, and `compare.sh` shows no visual change.

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

**Session 2 = Phase 1, slice 1a.** Start with CLAUDE.md, then this file's §3.2, §4 and Phase 1.

Where things stand:
- `metal` has CLAUDE.md, this plan, `port/` (env/build/run/bench scripts, the profile/trace parsers, the reference demo), the `*.noindex/` gitignore line, and the timedemo work metric (5 small edits in `qcommon/common.c`, `qcommon/qcommon.h`, `client/client.h`, `client/cl_cgame.cpp`, `client/cl_main.cpp`). No renderer code yet.
- The GL1 reference build is in `build.noindex/gl-install`. GL1 numbers are in §2 (compare on **work ms/frame**; reference 0.934 ms with `r_primitives 2`, which `bench.sh` now passes by default).

Do next, in order:
1. `source port/env.sh`, `omm_mount_metal`. Edit `cmake/platforms/macos.cmake` (arm64, 26.0). Rebuild GL (`port/build.sh gl`) and re-run `port/bench.sh run gl 3` for the arm64-only reference.
2. Add `cmake/renderer_metal.cmake` and a `metal` case in `port/build.sh` (`-DBUILD_RENDERER_METAL=ON`, keeping GL1 on for A/B). Model it on `cmake/renderer_gl1.cmake` + `finish_macos_app` in macos.cmake. Widen the jpeg/freetype guards.
3. Add the metallib build + embed step with one trivial shader (clear/fullscreen triangle). Fail with a clear message if `$OMM_METAL_BIN/metal` is missing ("source port/env.sh && omm_mount_metal").
4. Write `sdl/sdl_mtlimp.m` (mirror `sdl_glimp.c`'s `GLimp_SetMode`/`GLimp_Init`/`GLimp_EndFrame`/`GLimp_Shutdown` behavior with `SDL_WINDOW_METAL | SDL_WINDOW_ALLOW_HIGHDPI` + `SDL_Metal_CreateView`/`SDL_Metal_GetLayer`).
5. Copy renderergl1 → `renderer_metal/`, add `tr_mtl.h` (the backend API), and convert qgl call sites file by file (largest first: tr_backend, tr_shade, tr_draw, tr_shadows, tr_init, tr_main, tr_image, tr_model, tr_surface, tr_sky, tr_cmds, flares). The backend implements device/queue/clear/present for real and stubs the rest.
6. Check the Phase 1 done criteria; record what passes. If the session gets long, stop after a coherent subset and hand over 1b.

Gotchas found in session 1:
- OpenMoHAA's `wait N` counts milliseconds; `wait 1` = one frame. Long loading frames use up timed waits at once.
- A killed game leaves `home.noindex/main/*.pid`, and the next launch blocks on a modal "Abnormal Exit" NSAlert. `run.sh` deletes stale PID files when no game runs. Never leave a game process running.
- `xctrace record --launch` often leaves the game suspended at `_dyld_start` (state `T`). `bench.sh trace` therefore records all processes while `run.sh` starts the game normally.
- Don't edit a bash script while it's running (bash reads scripts incrementally).
- Upstream's `.gitignore` has `build*`, which silently ignores any new file whose name starts with "build" (e.g. `port/build.sh` needed a `!/port/build.sh` exception). Check `git status` / `git check-ignore -v` after adding files.
- `s_initsound 0` crashes upstream; mute with `s_volume 0`.
- The upstream launchers don't work on macOS.
- `sv_maxclients 1` makes single-player-style "Continue" prompts block after map loads; use ≥ 2 for scripted runs.
- `glconfig_t` layout is ABI with cgame. `SwipePoint`/`DrawBackground` are NULL in GL1's refexport (upstream bug).
- Overbright in GL1 depends on fullscreen + hardware gamma. Decide parity per mode carefully in Phase 3.

Open bugs: none in port code yet.
