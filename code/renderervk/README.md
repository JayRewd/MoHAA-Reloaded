# renderervk — Vulkan renderer

`renderer_vulkan` is a copy of the GL1 renderer (`code/renderergl1`) with every
OpenGL call removed. All GPU work goes through the interface in [`vk.h`](vk.h),
which the `vk*.c` files implement with Vulkan. Select it with
`cl_renderer vulkan`.

Status (2026-10-13, branch `vulkan-renderer`): milestones 1 to 5 of the plan
below are done.

- the main menu matches GL1 pixel for pixel, except the animated ceiling fan
  (its frame depends on the wall clock)
- with the deterministic setup described in [Testing](#testing), 3D views
  match GL1: world surfaces and lightmaps, static and animated models,
  alpha-tested and blended surfaces, decals, the view weapon, the sky, the
  sky portal and farplane fog, on `dm/mohdm1`, `dm/mohdm2`, `dm/mohdm4`,
  `dm/mohdm5`, `m2l1`, `m2l3` and `m4l1`. When both drivers rasterize with
  the same subpixel precision (a window wider than 4096 pixels) they match to
  the bit except for at most about 0.3 % of the pixels, nearly all off by 1
  (large triangles clipped against different guard bands, transient HUD
  text), a few isolated alpha-tested pixels, and what the game randomizes
  (snow particles, AI poses). At 640x480, radeonsi uses a finer subpixel
  precision than RADV and 1 to 8 % of the pixels differ by 1
- fog and the user clip plane (portals and mirrors) are implemented
- the sun lens flare and its full-screen fade, dynamic lights (dynamic
  lightmaps and `r_fastdlights 1`), cinematics, `TEXENV_ADD` (with a test
  shader: no shipped shader produces it) and `r_showtris`/`r_shownormals`
  match GL1 the same way (see [Testing](#testing) for the numbers)
- the light flare depth test (`RB_TestFlare`, unreachable in OpenMoHAA, see
  [Flares](#flares)) reads the depth without waiting for the GPU; line
  stipple and GL's line rasterization rule come from
  `VK_KHR/EXT_line_rasterization`
- no validation or synchronization-validation message (`r_vkValidation 2`)
  during startup, the menu, a map load, `vid_restart` (also with a mode
  change and inside a map), window resizes (`r_allowResize 1`), `r_shadows 2`,
  `r_measureOverdraw 1`, `r_showImages 1`, texture mode changes, fogged maps,
  single-player maps, the sky portal, sun flares, dynamic lights, cinematics,
  debug drawing (with and without stipple) and shutdown
- `renderer_vulkan.so` imports no GL symbol and no Vulkan symbol; it needs
  only SDL2, libm, libstdc++, libgcc_s and libc (same as `renderer_opengl1.so`).
  Vulkan is loaded at runtime through SDL
- `renderergl1` is unchanged; it is the visual reference

## Layout

| File | Role |
| --- | --- |
| `vk.h` | backend interface (the contract below) |
| `vk_local.h` | definitions shared by the backend files: `qvk*` function pointers, frame, texture and pipeline-key types |
| `vk_instance.c` | Vulkan loading through SDL, instance, validation messenger, device selection, swapchain |
| `vk.c` | frames and command buffers, render targets, presentation, state recording, `vk_draw`, `vk_clear`, `vk_read_pixels` |
| `vk_image.c` | texture handles and storage, samplers, descriptor sets, staging and ordering of uploads |
| `vk_pipeline.c` | pipeline cache keyed on the state that is baked into pipelines, pipeline creation |
| `shaders/generic.vert`, `shaders/generic.frag` | the shaders of every pipeline; the vertex shader is built with and without `gl_ClipDistance`, the fragment stage is specialized per pipeline |
| `shaders/compile_shaders.sh` | regenerates `vk_shaders.c` with `glslc` |
| `vk_shaders.c` | generated SPIR-V as C arrays (committed, so builds don't need `glslc`) |
| `tools/` | GL1/Vulkan screenshot comparison scripts and view lists (see Testing) |
| `tr_immediate.c` | `glBegin`/`glEnd` emulation for debug and 2D drawing, emits indexed triangles or lines |
| `tr_backend.c` | renderer-side state tracking (`GL_State`, `GL_Bind`, `GL_Cull`, `GL_TexEnv`, matrices, vertex arrays, current color) |
| `tr_*.c`, `tr_*.cpp` | the GL1 renderer with GL calls replaced, otherwise unchanged |
| `../sdl/sdl_vkimp.c` | SDL window, video modes and fullscreen without a GL context (`VKimp_*`) |
| `../../cmake/renderer_vulkan.cmake` | build target `renderer_vulkan` |

The `GL_` prefix of the state helpers and `glConfig`/`glState` are historical
names kept to make it easy to port fixes from `renderergl1`; they do not touch
OpenGL. `GLimp_SetGamma` comes from the shared `sdl/sdl_gamma.c` and only uses
SDL.

Shared files touched outside this directory: `CMakeLists.txt` (option),
`cmake/libraries/{freetype,jpeg}.cmake` and `cmake/platforms/macos.cmake`
(accept the new renderer), `code/renderercommon/tr_common.h` (does not include
`qgl.h` when `USE_VULKAN` is defined; `texnum` is `unsigned int`, same type as
before).

## Build and run

Requirements: the Vulkan headers (1.4.341 on the dev machine) and, for CMake's
`FindVulkan` to report Vulkan as found, the loader development library, even
though the renderer doesn't link it (only `Vulkan::Headers` is used). `glslc`
is only needed to change shaders. `vulkan-validationlayers` is needed for
`r_vkValidation`.

```sh
cmake -S . -B build-vk -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_RENDERER_VULKAN=ON
cmake --build build-vk
cd build-vk/RelWithDebInfo
./openmohaa +set cl_renderer vulkan +set fs_basepath ~/Documents/moh \
    +set fs_homepath /tmp/moh-home +set r_fullscreen 0 +set r_mode 3
```

`cl_renderer opengl1` runs the GL1 renderer from the same build for comparison.

Cvars of the backend:

| cvar | |
| --- | --- |
| `r_vkValidation` (latched, default 0) | 1 enables `VK_LAYER_KHRONOS_validation` and prints its warnings and errors to the console; 2 also enables synchronization validation (slow) |
| `r_swapInterval` (latched) | 0: immediate present mode (mailbox if unavailable), < 0: FIFO relaxed, > 0: FIFO; FIFO when the wanted mode is missing |

Shaders: edit `shaders/*.vert|frag`, run `code/renderervk/shaders/compile_shaders.sh`
and commit the regenerated `vk_shaders.c`.

## Testing

Automated runs: put a cfg in `<fs_homepath>/main/` and pass `+exec name.cfg`.
Don't give it the name of a cfg the game runs itself: `default.cfg` runs
`menu.cfg` at startup, so a test script called `menu.cfg` runs then. Use
`devmap` so cheats work.

Do not use `s_initsound 0`: the game then crashes in
`ClientGameCommandManager::PlaySound` on the first footstep, with either
renderer (upstream bug). Mute with `s_volume 0`, `s_musicvolume 0`,
`s_ambientvolume 0` instead. `+set in_nograb 1` keeps the mouse free and
`+set in_mouse 0` keeps it from turning the view.

Screenshots land in `<fs_homepath>/main/screenshots/` as uncompressed 24-bit
TGA (18-byte header, BGR pixels, bottom row first).

### Deterministic 3D views

Two runs of the same map normally give different frames: the game time at the
screenshot depends on how long the loading and every frame took (swaying
foliage, animated and scrolling shaders, HUD), the player spawns at a random
point and the camera drifts. This script gives the same frame in every run
and with both renderers:

```
set thereisnomonkey 1
set fixedtime 16
devmap dm/mohdm5
wait 3000
finishloadingscreen
wait 1000
god
notarget
noclip
ui_hud 0
cg_drawviewmodel 0
tele 4896 -4640 172
face 0 180 0
wait 2000
pause
wait 300
screenshot f5_a
wait 100
centerprint " "
pause
wait 100
quit
```

Repeat the block from `tele` to the second `pause` for more views of the same
map; `tools/shots.py` writes such a script from a view list (see Comparing).
What each part is for:

- In OpenMoHAA `wait` counts milliseconds of game time
  (`Cbuf_Execute( msec )` in `Com_Frame`), not frames. `fixedtime 16` (a
  cheat cvar, settable before `devmap`) makes every frame advance the game by
  exactly 16 ms whatever the real frame time, so the script reaches each
  command after the same number of frames and at the same game time. `+set
  com_maxfps 60` keeps the real speed close to the game speed.
- `thereisnomonkey 1`: without it the game turns `cheats` off at the first
  cheat command (`Listener::ProcessEvent`), and `tele`, `noclip`, ... fail
  with "You must run the server with '+set cheats 1'".
- `tele x y z` and `face pitch yaw roll` (`fgame/player_util.cpp`) put the
  eye at an exact position; `noclip` keeps the player from falling or
  colliding, `god` and `notarget` keep the AI out of it. `coord` prints the
  current position and angles. Spawn points are in the entity lump of the
  BSP (`info_player_*`).
- `pause` before the screenshot: the `screenshot` command reads the frame
  before anything of the next frame is drawn. The Vulkan backend then
  returns the last presented frame, but GL1 reads a back buffer that Mesa
  hands out from two or three swaps earlier, so without the pause everything
  that moves (trees and bushes sway) differs. While paused every frame is the
  same. `finishloadingscreen` unpauses too.
- The `screenshot` command centerprints "Wrote ..." in the view; the blank
  `centerprint` replaces it before the next screenshot.
- `ui_hud 0` and `cg_drawviewmodel 0` hide the HUD and the view weapon (set
  `cg_drawviewmodel 2` to test the weapon's depth range). Messages such as
  "Game Saved" and "An objective has been added!" still show for a few
  seconds after the map starts.

Still random between runs: what the game seeds with the wall clock
(`G_InitGame` seeds `rand()` with `Com_Milliseconds()`), such as the snow
particles of `dm/mohdm5` and AI movement in single-player maps.

### Comparing

The scripts in `tools/` (Python 3 standard library and POSIX sh) automate it.
They read `MOH_BASEPATH` (game data, default `~/Documents/moh`),
`MOH_HOMEPATH` (throwaway homepath, default `/tmp/moh-vktest`), `MOH_BINDIR`
(default `build-vk/RelWithDebInfo`) and `MOH_OUT` (results, default
`$MOH_HOMEPATH/compare`).

```sh
code/renderervk/tools/compare.sh code/renderervk/tools/views/mohdm5_fog.txt
WIDE=1 code/renderervk/tools/compare.sh code/renderervk/tools/views/mohdm5_fog.txt
```

- `views/*.txt`: view lists (`map <name>`, or `spmap <name>` for a single
  player game, `set <command>`, `shot <name> x y z pitch yaw roll [wait]`),
  one per map or feature tested so far. `shot` waits 2 s of game time after
  moving the camera, or `wait` milliseconds (to catch a fade).
- `compare.sh`: writes the cfg (`shots.py`), runs it with GL1 and Vulkan
  (`run.sh`), then prints per channel the max and mean absolute difference
  and the number of differing pixels for every view (`tgadiff.py`), and
  writes PNG copies and an amplified difference image (|a - b| x 8, equal
  pixels as a dark copy) to `$MOH_OUT`. `WIDE=1` compares at 4160x240.
  `VAL=2` runs with synchronization validation; validation messages are
  printed. A `views/<list>.shader` next to the list is installed as
  `main/scripts/aaa_<list>.shader` during the runs: the shader file listed
  first wins, so it replaces shaders of the game data (see
  `mohdm1_texenv`).
- `hotspots.py`: zoomed side-by-side crops of the areas that differ most.
- `montage.py`: a grid of images, for example `gl | vk | diff` rows.
- `bspinfo.py`: worldspawn keys (fog), spawn points and the surfaces of the
  shaders that contain a keyword (`polygonoffset`, `alphafunc`, ...), to
  pick views.

With RADV and GL1 on radeonsi (same GPU) expect:

- At 640x480 (`r_mode 3`) 1 to 8 % of the pixels differ by 1 and a few
  hundred alpha-tested pixels (foliage edges) flip. radeonsi rasterizes
  viewports of up to 1024 pixels with 1/4096 pixel vertex precision, up to
  4096 with 1/1024, and 1/256 beyond (`si_state_viewport.c`); RADV always
  uses 1/256. Whole triangles then interpolate their texture coordinates very
  slightly differently, which bilinear filtering turns into ±1.
- With a window wider than 4096 pixels both drivers use 1/256:
  `+set r_mode -1 +set r_customwidth 4160 +set r_customheight 240`. The
  frames are then the same to the bit except at most a few thousand pixels
  off by 1 on large triangles next to the camera (the drivers clip against
  different guard bands) and on transient HUD text, a few isolated
  alpha-tested pixels, plus random particles and AI poses. That is the
  setting to check a change for exactness.
- `r_farplane_nofog 1` turns farplane fog off in both renderers.

Test content:

- `dm/mohdm1`: world, lightmaps, alpha-tested railings, trees and bushes
  (alpha-tested static models with vertex deforms), furniture, sky.
- Decals: a map's `.dcl` file only loads when its map time matches the
  BSP's. In the shipped data that is the case for `dm/mohdm4` (3 posters),
  `m2l3` (39 decals) and `obj/obj_team4`; the other maps print "BSP file is
  newer than DCL file" and have no decals with either renderer.
- Fog: `dm/mohdm2` (farplane 5000), `dm/mohdm4` (5500), `dm/mohdm5` (1800,
  dense), `m4l1` (3100), `m2l3` (set by its script). Sky portal: `m2l1`.
- Sun flare: `mohdm1_sunflare` (the "sun" lens flare of
  `global/lensflaredefs.txt`: a sprite at the sun and a full-screen white
  blend that fades over 600 ms once the sun leaves the view; the fade is
  caught at 160 and 400 ms). Every map with a `suncolor`/`sundirection` key
  and no `sunflarename none` has it (`dm/mohdm1`, `dm/mohdm3`, `dm/mohdm6`,
  `m2l2*`, `m3l2`, `m4l0`, `m5l*`, ...), but the flare only shows where the
  trace towards the sun hits a sky surface (not in the fog of `m2l3`).
- Dynamic lights: `mohdm1_dlight` spawns invisible entities
  (`spawn fx/dummy.tik targetname dl1`) and gives them a light (`script dl1
  origin "x y z"`, `script dl1 light r g b radius`); cgame adds them with
  `R_AddLightToScene` every frame, so they are as deterministic as the rest
  of the view. `script` (and `cam`) are refused when the game has more than
  one client slot, which is the case for `devmap` of a dm map: the list uses
  `spmap` (`spdevmap`). Run it again with `+set r_fastdlights 1` (latched)
  for the projected dlight pass. `cg_acidtrip N` adds random dlights for a
  quick look.
- Cinematics: the game data has no RoQ file. `ffmpeg -loop 1 -i still.png
  -t 2 -r 30 -c:v roqvideo -f roq <homepath>/main/video/test.roq` makes one
  (the encoder is slow, about 10 frames a second); `cinematic test.roq 1`
  plays it full screen and holds the last frame, so a screenshot taken after
  the end is deterministic. Sizes must be powers of 2 (both renderers drop
  to the menu otherwise). RoQ refines a still image over several frames, so
  only the held last frame compares exactly.
- `TEXENV_ADD`: no shader of the game data produces it. `nextBundle add`
  is never used, and the collapse of a stage with an additive stage
  (`r_ext_texture_env_add 1`) needs `rgbGen identity` on both, which none of
  the three candidates has (`shaderlist` shows `MT(a)` for such stages; 12
  maps showed none). `mohdm1_texenv` replaces the ground shader with an
  opaque and an additive texture stage and a lightmap stage (one
  `TEXENV_ADD` stage plus the lightmap with `r_ext_texture_env_add 1`, three
  passes with 0; the two give the same ground within ±2) and a wall shader
  with `nextBundle add`. The pair to collapse must not contain the
  lightmap: see GL1 quirks.
- Debug drawing: `mohdm1_debug`. `r_showtris` and `r_shownormals` only draw
  with `developer 1` (`r_developer`), which is why they seemed to show
  nothing. The game's debug lines (`sv_showbboxes`, camera paths shown with
  `cam show`, AI paths) are drawn by `R_DrawDebugLines` from the front end,
  before the back end draws the view, which then covers them: they are
  invisible with both renderers. That is also the only user of line
  stipple (`DrawLineLoop` has no caller), so stipple was checked by forcing
  a pattern on `r_showtris` temporarily.
- No shipped map has a portal or mirror surface (`portal` shader keyword), so
  the user clip plane has no GL1 reference in game content. It was checked
  with a temporary plane forced on the main view: x_eye >= 0 cut every
  frame exactly at the middle column, the kept half was identical to the
  unclipped frame with the fragment shader fallback and identical except ±1
  on the triangles crossing the plane with `gl_ClipDistance`, and a plane at
  400 units removed the near geometry.

Results of the milestone 5 tests with `WIDE=1` (GL1 vs Vulkan, worst view
of each list; per channel max R/G/B and the share of differing pixels):

| test | max R/G/B | differing | nearly all |
| --- | --- | --- | --- |
| `mohdm1_sunflare` (6 views, fade included) | 132/129/101 | 0.375 % | ±1, 14 isolated foliage pixels |
| `mohdm1_dlight` | 61/42/23 | 0.024 % | ±1 |
| `mohdm1_dlight +set r_fastdlights 1` | 61/42/23 | 0.025 % | ±1 |
| `mohdm1_texenv` (`r_ext_texture_env_add` 1 and 0) | 66/54/20 | 0.024 % | ±1 |
| `mohdm1_debug` (`r_showtris 2`) | 252/253/253 | 0.015 % | a few nearly horizontal line segments |
| cinematics (held last frame, 256x256 and 512x256, 640x480) | 0/0/0 | 0 % | |
| main menu (fan masked, 640x480) | 0/0/0 | 0 % | |

At 640x480 the same lists show the usual 1 to 10 % of pixels off by 1 and a
few dozen to a few hundred isolated pixels off by more (alpha-tested foliage,
and broken `r_showtris` edges where the line and its polygon tie in depth),
all gone or nearly gone at 4160x240.

Other things learned:

- `com_maxfps` does not cap the menus.
- Commands appended while a script waits run after the whole script. The
  `vid_restart` that a window resize schedules is one of them, so a script
  with a long `wait` never sees it. The client also reads console commands
  from stdin (a FIFO works), which is a way to drive resize tests with
  `xdotool windowsize`; a `devmap` sent that way stops at the loading screen.
- Depending on timing, a map can stop at its loading screen with a CONTINUE
  button (with both renderers); `finishloadingscreen` dismisses it.
- The menu fan and 2D shader animations use the wall clock
  (`backEnd.refdef.time = ri.Milliseconds()` in `Set2DWindow`), so they
  differ between any two runs.
- The game archives cvars and autosaves into `fs_homepath`; delete
  `main/configs` and `main/save` between test runs to start from the same
  state.
- `r_anaglyphMode 1` is a fatal error in OpenMoHAA with either renderer
  (`RE_BeginFrame: Stereo is enabled, but stereoFrame was 0`).
- `PRINT_WARNING` and `PRINT_DEVELOPER` messages only show with
  `developer 1` in OpenMoHAA (`CL_RefPrintf`), so the backend prints
  validation messages and its own warnings with `PRINT_ALL`. `+set logfile 2`
  writes the console to `<fs_homepath>/main/qconsole.log`.
- AVI capture is compiled out of OpenMoHAA (`CL_OpenAVIForWriting` returns
  `qfalse`), and the `video` command only runs during demo playback, so
  `RB_TakeVideoFrameCmd` never runs.
- No `.gst` file (ghost texture) exists in the game data, so
  `R_UpdateGhostTextures` never has a texture to update.
- `AMD_DEBUG=vs,ps,nir` (with `MESA_SHADER_CACHE_DISABLE=true`) dumps what
  radeonsi compiles for GL1's fixed-function pipeline and `RADV_DEBUG=shaders`
  what RADV compiles for ours, which is how the arithmetic of the vertex
  transform and of fog were matched.

## Backend contract

`vk.h` documents each call. The points below are what OpenGL used to provide
implicitly and the Vulkan implementation reproduces.

### Coordinate conventions

- Projection matrices are OpenGL ones (column-major, clip z in [-w, w], y up).
  With `VK_EXT_depth_clip_control` (enabled when the device has it) the
  pipelines use GL's clip space depth (`negativeOneToOne`) and the projection
  is used as is, so clip space z, near/far clipping and depth values are the
  same as GL's; otherwise the backend remaps depth with `z' = (z + w) / 2`,
  pre-multiplied into the projection. Projection × modelview is computed on
  the CPU in the order of Mesa's `_ModelProjectMatrix` and pushed as one
  matrix, and the vertex shader transforms with the operations of Mesa's
  fixed-function vertex program (a multiply, then fused multiply-adds), so
  clip coordinates come from the same operations on the same values as GL1's
  on Mesa. y is flipped with
  a negative viewport height (core since Vulkan 1.1).
- Viewport, scissor and `vk_read_pixels` use window coordinates with the
  origin at the bottom-left: Vulkan `y = windowHeight - (y + height)`.
- `vk_set_depth_range(n, f)` maps to `VkViewport::minDepth/maxDepth`. The
  renderer uses (0, 1), (0, 0.3) for the weapon, (1, 1) for the sky and (0, 0)
  for debug lines.
- `vk_read_pixels` returns rows bottom row first, tightly packed. Depth is the
  window-space depth (the flare code converts it back with `2 * depth - 1`).
  Pixels outside the framebuffer read as 0.
- Texture data is uploaded top row first and t = 0 addresses the first row,
  same as Vulkan, so textures need no flip.
- Front faces are counter-clockwise in window space (GL default). Once y is
  flipped (negative viewport height or a flipped projection, they give the
  same framebuffer coordinates), `VK_FRONT_FACE_COUNTER_CLOCKWISE` matches GL.

### State (`vk_set_state`)

Blend factors: the usual mapping of `GLS_SRCBLEND_*` / `GLS_DSTBLEND_*`; no
blend bits means blending off. `GLS_DEPTHFUNC_EQUAL` selects EQUAL, otherwise
LESS_OR_EQUAL. `GLS_DEPTHTEST_DISABLE`, `GLS_DEPTHMASK_TRUE` and
`GLS_POLYMODE_LINE` (front and back) map directly. Like GL, there are no depth
writes while the depth test is disabled.

Alpha test (fragment discard), from the GL1 code:

| bits | keep fragment when |
| --- | --- |
| `GLS_ATEST_GT_0` | alpha > 0.05 |
| `GLS_ATEST_LT_80` | alpha < 0.5 |
| `GLS_ATEST_GE_80` | alpha >= 0.5 |
| `GLS_ATEST_LT_FOLIAGE1` / `GE_FOLIAGE1` | alpha < / >= `r_alpha_foliage1` (default 0.75) |
| `GLS_ATEST_LT_FOLIAGE2` / `GE_FOLIAGE2` | alpha < / >= `r_alpha_foliage2` (default 0.75) |

The test applies to the final fragment alpha (after texture env, before fog).

`GLS_CLAMP_EDGE`, `GLS_MULTITEXTURE` and `GLS_MULTITEXTURE_ENV` are never set
by the current shader parser. `GLS_COLOR_NOMASK` (shader keyword
`nocolorwrite`) is set but GL1 ignores it. Fog bits are handled by `GL_State`
through `vk_set_fog*`.

The framebuffer has no destination alpha, like the GL1 visual on Linux (SDL
asks for no alpha bits): alpha is never written and stays 1, so `DST_ALPHA`
blend factors read 1 and `SRC_ALPHA_SATURATE` gives 0, as in GL1.

### Fragment pipeline

1. Vertex color: per-vertex `colors` or the constant `color` of the geometry.
2. Texture unit 0 (if `st[0]` is set) combines with the vertex color using its
   texture env, unit 1 (if `st[1]` is set) combines with the result of unit 0.
   The formulas are next to `texEnv_t` in `vk.h`; `TEXENV_ADD` clamps the sum
   to 1 like GL. For `TEXFMT_RGB8/RGB5` (and `BC1_RGB`) textures alpha samples
   as 1 (`A = ONE` swizzle); with `TEXENV_REPLACE` GL then keeps the previous
   alpha instead of 1. `TEXENV_COMBINE4` is drawn as modulate (no shader
   produces it).
3. Alpha test.
4. Fog, if enabled: `f = clamp((end - d) / (end - start), 0, 1)` with
   `d = |z_eye|`, `rgb = mix(fogColor.rgb, rgb, f)`; alpha unchanged. The
   backend computes it like Mesa's fixed-function fog, which makes it match
   GL1 to the bit on Mesa: the vertex shader writes `|z_eye|` (row 2 of the
   modelview, same operations as the position) as the fog coordinate, the
   fragment shader computes `f = sat(fma(d, -1 / (end - start), end / (end -
   start)))` with both constants computed on the CPU in float, then
   `rgb = fma(rgb, f, fogColor * (1 - f))`. The fog color is clamped to
   [0, 1]. Fog on/off is part of the pipeline key; color and range are push
   constants.
5. User clip plane: discard where `dot(plane, eyePos) < 0`, eye position =
   modelview * vertex. The backend transforms the plane to object space
   (`plane * modelview`) and clips with `gl_ClipDistance`; on devices without
   `shaderClipDistance` the fragment shader discards on the interpolated
   distance instead (same pixels, except that triangles crossing the plane
   aren't split).
6. Blending, color mask, depth and stencil per the state.

Stencil ops are the clamping GL ones (`INCREMENT_AND_CLAMP`,
`DECREMENT_AND_CLAMP`). Stencil shadows (`r_shadows 2`) and
`r_measureOverdraw` need at least 4 stencil bits.

Polygon offset (`vk_set_polygon_offset`, from `r_offsetfactor` -1 /
`r_offsetunits` -2) maps to depth bias slope/constant factors, for filled
polygons only (`GL_POLYGON_OFFSET_FILL`).

Lines (line primitives and polygons in line mode) use GL's rasterization
rule for aliased lines, the diamond-exit rule, which is Vulkan's Bresenham
mode (`VK_KHR_line_rasterization` or `VK_EXT_line_rasterization` with
`bresenhamLines`; RADV's default line mode rasterizes the same pixels).
`vk_set_line_stipple(factor, pattern)` stipples them like `glLineStipple`
(factor clamped to [1, 256], 0 disables it; needs `stippledBresenhamLines`);
the stipple counter restarts at each segment of a line list, as GL does for
`GL_LINES`. Line width above 1 needs `wideLines`. All of this only affects
debug drawing and is ignored when unsupported.

### Clears

`vk_clear` clears the current scissor rectangle. Like `glClear` it honours the
color mask, the depth write bit of the current state and the stencil write
mask. Depth clears to 1.0, stencil to 0. `color` may be NULL when
`CLEAR_COLOR` is not set. The backend uses `vkCmdClearAttachments`, which
ignores masks, and draws a quad for a color clear with a partial color mask; a
partial stencil write mask clears all bits (the renderer never sets one). The
clear color is first rounded to the nearest 8-bit value on the CPU: drivers
convert clear colors with float arithmetic that can round the other way near
half a step (RADV gives 179 for 0.7, GL1 the correctly rounded 178; it shows
in the fog-colored background of `m2l3`).

### Textures

- `vk_create_texture` gives the image a non-zero handle in `image->texnum`.
  Handles must never be reused: the renderer caches bound handles per unit
  (`glState.currenttextures`).
- Calling it again on an image replaces its storage (scratch images for
  cinematics, ghost textures); draws already recorded keep the old storage, so
  release old storage only once the GPU is done with it. Same for
  `vk_destroy_texture`.
- `vk_bind_texture` binds the image, not a particular storage: a draw uses the
  storage the image has when the draw is recorded.
- Uploads happen mid-frame (cinematics every frame, dynamic lightmaps in
  `R_UploadDlights`, ghost textures): draws recorded before an upload must see
  the old contents.
- Sampler: wrap from `image->wrapClampModeX/Y` (`WRAP_REPEAT` repeats,
  `WRAP_CLAMP` clamps to the edge); textures with one mip level use linear
  min/mag filtering; mipmapped ones use the filter from
  `vk_set_texture_filter` (`r_textureMode`, default
  `GL_LINEAR_MIPMAP_LINEAR`) and anisotropy from
  `r_ext_texture_filter_anisotropic` / `r_ext_max_anisotropy`.
- `TEXFMT_RGBA4/RGB5` may be stored as 8-bit formats (they are).
- BC formats come from DDS files; the texture env and sampler rules are the
  same.

### Frames and synchronisation

- `vk_begin_frame` is called from `RB_DrawBuffer` at the start of each frame,
  but `RE_StretchRaw`, texture uploads and other front-end paths can run
  before the first command of a frame: the frame starts lazily on the first
  operation.
- Screenshot commands are queued when the console command runs, before the
  frame's `RC_DRAW_BUFFER`, so `vk_read_pixels` usually runs before anything
  is drawn: it must then return the last presented frame, like a GL back
  buffer that kept its contents.
- `vk_finish` is called at swap for frames with no 3D view (menus) and around
  cinematics, as `glFinish` was. It waits for the fence of the last
  submission; what the current frame recorded is submitted at
  `vk_end_frame`.
- `vk_read_pixels` must see everything recorded so far in the frame and the
  frame must continue afterwards (screenshots, AVI capture,
  `r_measureOverdraw`). It waits for the GPU, which is acceptable for these
  occasional reads.
- `vk_queue_depth_reads` records reads of single depth values in order with
  the draws without waiting; `vk_get_depth_read` returns a result once the
  frame that recorded it has completed. The backend collects the results
  when it reuses the frame slot, so a read made in frame N is available
  from frame N + 2 on (2 frames in flight), whatever the GPU timing, and
  stays available for 2 frames. Ids from before a `vid_restart` are lost.

### Flares

There are two flare systems in the renderer:

- The lens flares of `tr_sun_flare.cpp` (`R_DrawLensFlares`): the sun flare
  of maps with a sun (`sunflarename`, default "sun"), entities with
  `RF_LENSFLARE`/`RF_VIEWLENSFLARE` and dlights of type `lensflare` or
  `additive`. They test visibility with collision traces (`CM_BoxTrace`),
  not the depth buffer, are always on and draw through immediate mode; they
  match GL1 (`views/mohdm1_sunflare.txt`). `r_drawSun` is unused (the
  `RB_DrawSun` call is `#if 0` in both renderers).
- The Quake 3 light flares of `tr_flares.c` (`r_flares`, `r_flareSize`,
  `r_flareFade`): `RB_AddFlare` has no caller in OpenMoHAA. `MST_FLARE`
  surfaces load as `SF_FLARE`, whose tesselation function is empty in both
  renderers, no shipped map has one, and `RB_AddDlightFlares` is commented
  out. `RB_TestFlare` therefore never runs in the game.

GL1's `RB_TestFlare` read the depth under each flare with `glReadPixels`
(a GPU sync per flare and per frame) and reset `glState.finishCalled`, so
`glFinish` ran at every swap. The Vulkan version queues the reads of all the
flares of a view with one `vk_queue_depth_reads` call at the same point of
the frame, and tests each flare against the newest read that has completed,
with the eye depth and projection of the frame that made it: no flush, no
wait, no reset of `finishCalled`. Until a read completes, a flare keeps its
visibility; the fade (`r_flareFade`, `fadeTime`) is GL1's code. The effect is
that visibility changes reach the fade 2 frames later than in GL1: a flare
that appears starts fading in 2 frames late (32 ms with `fixedtime 16`, at
most 0.22 of intensity behind with `r_flareFade 7`), then both are equal. A
flare that faded out stays in the active list while it is added (its next
read is pending) instead of being dropped and re-added, which gives the
same result. This was checked with a temporary hack that adds a flare for
every dlight (`RB_AddDlightFlares`) in the old (synchronous) and new code:
frames are identical once the flares have faded in, and the logged
intensities of the new code follow the old ones exactly 2 frames later.
Like GL1, `RB_RenderFlare` doesn't set `tess.vertexColorValid`, so
`rgbGen vertex` skips the flare color and the quad draws with stale colors;
the test set it to see the fade.

### Capabilities (`vk_initialize`)

`vk_initialize` reports what the device supports; `R_ApplyExtensionCvars`
(`tr_init.c`) then turns features off from the same cvars the GL1 renderer
used, so shader collapsing and image loading follow the user's config:

| glConfig field | device value | turned off by |
| --- | --- | --- |
| `numTextureUnits` | 2 (shaders using `nextBundle` fail with fewer) | `r_ext_multitexture 0` |
| `textureEnvAddAvailable` | true | `r_ext_texture_env_add 0` (the Linux default, so additive second stages draw as two passes like GL1) |
| `textureCompression` | `TC_S3TC_ARB` if `textureCompressionBC` is supported | `r_ext_compressed_textures 0` (default); `.dds` files are only loaded when this is not `TC_NONE` |
| `textureFilterAnisotropic`, `maxAnisotropy` | `samplerAnisotropy`, `maxSamplerAnisotropy` | `r_ext_texture_filter_anisotropic 0` (default) |
| all of the above | | `r_allowExtensions 0` |

`vk_initialize` also fills `maxTextureSize` (`maxImageDimension2D`; uploads
are downscaled to it), the vendor string (from the PCI vendor id), the
renderer string (device name), the version string (Vulkan version, driver
name and info) and the extensions string (enabled device extensions).
`VKimp_Init` sets the requested `colorBits`/`depthBits`/`stencilBits` from
`r_colorbits`, `r_depthbits` and `r_stencilbits`; the backend replaces them
with what it picked: 24 color bits, the depth bits of the depth format
(`D24_UNORM_S8_UINT` when available, `D32_SFLOAT_S8_UINT` on AMD, which has no
D24), and 8 stencil bits only when `r_stencilbits` asked for a stencil buffer
(0 by default on Linux, like GL1, which disables `r_shadows 2` and
`r_measureOverdraw`); the depth format always has a stencil aspect.
`deviceSupportsGamma`, `vidWidth`, `vidHeight` and `isFullscreen` come from
`sdl_vkimp.c`. `r_stereoEnabled` is not supported; `r_ext_multisample` is
not implemented yet.

## Implementation notes

- **Loading.** `SDL_Vulkan_GetVkGetInstanceProcAddr` gives
  `vkGetInstanceProcAddr`; every other function is loaded into a `qvk*`
  pointer (lists in `vk_local.h`), device functions through
  `vkGetDeviceProcAddr`. Vulkan 1.1 is required (negative viewport height).
  The device is the first discrete, then integrated, then virtual, then CPU
  device with a graphics queue that can present to the window. Enabled
  features, when supported: `fillModeNonSolid`, `wideLines`,
  `samplerAnisotropy`, `textureCompressionBC`, `shaderClipDistance` (user
  clip plane). Extensions when present: `VK_EXT_depth_clip_control` (GL clip
  space depth, see Coordinate conventions), `VK_KHR_line_rasterization` or
  else `VK_EXT_line_rasterization` (with `bresenhamLines`, and
  `stippledBresenhamLines` for line stipple; `vkCmdSetLineStipple*` is
  loaded under the enabled extension's name), `VK_KHR_portability_subset`
  (MoltenVK, with `VK_KHR_portability_enumeration` on the instance).
- **Render target.** The frame is drawn into an offscreen `R8G8B8A8_UNORM`
  color image and a depth/stencil image of `glConfig.vidWidth x vidHeight`,
  with one render pass that loads and stores both. `vk_end_frame` blits the
  color image to the swapchain image (scaled when the window size differs,
  e.g. during a resize before the `vid_restart`). The target keeps its
  contents between frames, readbacks don't depend on the swapchain format,
  and the swapchain only needs `TRANSFER_DST` usage. The render pass ends for
  every readback and every upload that must be ordered with draws, and
  restarts on the next draw or clear.
- **Frames.** Two frames in flight. Each frame slot has a command pool with
  three command buffers (upload, main, present), a fence, an acquire
  semaphore, a vertex/index buffer (4 MB, doubled when full), a 32 MB
  staging buffer and a buffer for 256 depth reads. The rendering is
  submitted first; then the swapchain image is acquired and the present
  command buffer (blit and layout transitions) is submitted with the frame
  fence, which covers every earlier submission. The semaphores signaled for
  presentation are per swapchain image (presents have no fence). The
  swapchain is recreated when the drawable size changes or on
  `VK_ERROR_OUT_OF_DATE_KHR`/`VK_SUBOPTIMAL_KHR`; with no area (minimized)
  the frame is submitted without presenting.
- **Mid-frame flush.** `vk_read_pixels`, and uploads that don't fit in the
  staging buffer, submit what the frame recorded so far, wait for it and
  continue in the same command buffers (a new main-buffer "segment").
  Uploads larger than the staging buffer get a temporary buffer.
- **Depth reads** (`vk_queue_depth_reads`). One `vkCmdCopyImageToBuffer`
  with a 1x1 region per read copies the depth aspect into the frame slot's
  depth read buffer (4 bytes per read, the layout of the depth format's
  depth aspect), between the same barriers as `vk_read_pixels` and a
  transfer-to-host barrier on the buffer. An id is the frame serial (low 24
  bits) and the index of the read. When the slot is reused, after waiting
  for its fence, the values are converted to floats and kept with the
  serial of their frame (`VK_CollectDepthReads`); reads outside the
  framebuffer give 0.
- **Deferred destruction.** Released objects go to a list of the frame that
  may still use them and are destroyed when that frame slot's fence has been
  waited for (or after a mid-frame flush, when the GPU is idle).
- **Textures** (`vk_image.c`). `texnum` encodes a slot of a 4096-entry table
  plus a per-slot generation, so a handle is only reused after 2^20 images
  went through the same slot. Each storage is a `VkImage` with its own
  memory (no sub-allocator yet), a view (with the `A = ONE` swizzle for RGB
  formats) and a descriptor set holding view and sampler. Samplers are
  cached by filter and wrap mode. When `r_textureMode` changes, mipmapped
  textures get new descriptor sets (sets used by recorded commands can't be
  updated). Uploads go to the upload command buffer, submitted before the
  main one, unless a draw recorded in the current segment of the main
  command buffer uses the texture. Then they are recorded in the main
  command buffer in order with the draws. New textures, level loading,
  cinematics and dynamic lightmaps take the first path, so they don't break
  the render pass. Between command buffers every texture is in
  `SHADER_READ_ONLY_OPTIMAL`.
- **Draws.** Each draw copies positions (repacked to 12 bytes), colors (RGBA8
  per vertex, or one float RGBA color read with a binding stride of 0),
  texture coordinates and 32-bit indexes into the frame's vertex buffer, in
  separate bindings.
- **Push constants** (`vkPushConstants_t`, 120 bytes of the guaranteed 128).
  Vertex stage: projection × modelview, row 2 of the modelview (fog
  distance) and the clip plane in object space (zero without a clip plane),
  pushed again when a matrix or the plane changes. Fragment stage: fog color
  and the two linear fog constants, pushed when they change. Both ranges are
  pushed at the start of every command buffer.
- **Pipelines** (`vk_pipeline.c`). The key packs blend factors, depth
  test/write/func, polygon mode, cull mode, topology, RGB color mask, alpha
  function (and its reference value), polygon offset on/off, color array or
  constant color, texture env per unit (0 for an unused unit), "RGB texture
  with TEXENV_REPLACE" per unit, the stencil function and ops, fog on/off
  clip plane on/off and line stipple on/off. Viewport, scissor, depth bias,
  line width and stencil reference/masks are dynamic, and the stipple factor
  and pattern for stippled pipelines (set when such a pipeline is bound and
  they changed). Line pipelines and polygon-line pipelines chain a
  `VkPipelineRasterizationLineStateCreateInfo` with the Bresenham mode. The fragment shader is specialized for
  texture envs, the alpha test, fog and the clip plane discard fallback;
  pipelines with a clip plane use the vertex shader built with
  `gl_ClipDistance`.
  Pipelines are created on first use and cached for the life of the device,
  with a `VkPipelineCache` that is not saved to disk yet.
- **Shutdown and `vid_restart`.** `vk_shutdown` waits for the device and
  destroys everything, including the instance; `vid_restart` recreates the
  window, so the backend starts from scratch.

## GL1 quirks kept on purpose

- `Set2DWindow`, the stencil shadow code and `RE_DrawBackground_TexSubImage`
  change cull state behind `GL_Cull`'s cache, like GL1 did.
- `R_DrawDebugLines` and `R_DebugGraphics` draw from the front end
  (`R_RenderView`), before the back end draws the view over them.
- `RB_RenderFlare` doesn't set `tess.vertexColorValid` (see Flares).
- A one-pass shader whose stage has a lightmap in the second bundle gets
  `RB_StageIteratorLightmappedMultitextureUnfogged` whatever the stage's
  texture env (ioq3 requires `GL_MODULATE`), and that iterator always
  modulates: a lightmap stage and an additive stage collapsed with
  `r_ext_texture_env_add 1` draw as lightmap x texture, not lightmap +
  texture as with two passes.
- `RE_StretchRaw` (cinematics) waits for the GPU before each frame
  (`vk_finish`), as GL1 called `glFinish`.
- `GL_State` enables fog on any transition of the external `GLS_FOG` bit,
  whatever the stage's `GLS_FOG_ENABLED` bit.
- The `nofog` shader keyword sets `GLS_FOG` in the stage bits.

Differences from GL1 (all in paths that were dead, broken or debug only):

- Strip/discrete drawing (`r_primitives 1/3`) draws triangles like mode 2.
- `RE_DrawBackground_DrawPixels` draws a textured quad (no `glDrawPixels`);
  neither background function is exported to the client.
- The portal multitexture `glPolygonMode` hack for old GeForce drivers is
  gone.
- `RB_ShowImages` clears to black instead of the last clear color.
- `GL_CLAMP` textures (lightmaps, dlights, scratch) clamp to the edge; GL
  blended with the border color, which texel-centred coordinates hide.
- Ghost textures are respecified as single-level RGB every frame, as
  `glTexImage2D(..., 3, ...)` did; GL1 left mipmapped ghost textures
  incomplete. A ghost texture without an image is skipped instead of
  overwriting the default image.
- The cinematic scratch image is recreated when the frame size differs from
  its storage size (GL1 compared the source size and its sub-upload failed
  with `r_picmip` set).
- Mipmapped DDS textures get anisotropic filtering, GL1 never set it on them.
- Immediate mode keeps at most 4092 vertexes per triangle strip, polygon or
  line loop (all current users are far below).
- Immediate mode vertexes with different colors pass them as 8-bit values;
  a single color for the whole primitive is passed as an exact float.
- A texture unit whose image has no storage samples opaque black, as an
  incomplete GL texture does.
- The depth buffer is 32-bit float on AMD (no D24 there), so z-fighting
  surfaces can resolve differently from GL1's 24-bit buffer (not seen in the
  comparisons: quantizing the depth to 24 bits in the shader changed no
  pixel of the test views).
- A screenshot taken before the frame is drawn (the `screenshot` command)
  shows the last presented frame; GL1 shows the back buffer the driver hands
  out, an older frame on Mesa.
- Light flares (`RB_TestFlare`, unreachable) see depth changes 2 frames
  later than GL1 and don't force a `glFinish`-like wait (see Flares).
- `GLS_CLAMP_EDGE` is ignored. Nothing sets it; GL1 handled it by changing
  the wrap mode of the textures bound at that moment, permanently.

## Plan

Done:

1. Instance, surface, device, swapchain (present mode from `r_swapInterval`,
   recreation on resize and out-of-date), frames and synchronisation,
   `vk_clear`, `vk_read_pixels` (color, depth, stencil), capabilities,
   `r_vkValidation`.
2. Textures (storage, uploads in order, samplers, swizzle, filter changes),
   the vertex/index buffer, `vk_draw`, texture envs, blending, depth state,
   cull, scissor/viewport, the pipeline cache. Check: the main menu matches
   GL1.

3. Deterministic 3D comparisons (see Testing) and the parity check of the
   second texture unit, `TEXENV_REPLACE` (`r_lightmap 1`), the alpha test,
   blended surfaces, decals (polygon offset), static and animated models, the
   view weapon, and the single-player maps `m2l3` and `m4l1`.
   `TEXENV_DECAL` is never produced (its collapse entry is `#if 0` in
   `tr_shader.c`). Found and fixed on the way: the vertex transform and depth
   convention now match GL's arithmetic, and clear colors are rounded like
   GL's. Not covered by the test views: the stencil state (`r_shadows 2`,
   `r_measureOverdraw`), checked by validation and a readback self-test
   only. (`TEXENV_ADD` was covered in milestone 5 with a test shader: the
   candidates listed here, `snowfield1`, `afrika_ocean`, `deepbluesea`,
   `subpen_water` and `cliffsky`, all use `nextBundle` without `add` or lack
   `rgbGen identity`, so none produces it.)
4. Fog (Mesa's arithmetic, see Fragment pipeline), the user clip plane
   (`gl_ClipDistance`, object space plane, fragment shader fallback), depth
   ranges (sky at 1, view weapon at 0..0.3) and the sky and sky portal in 3D
   views, checked on `dm/mohdm2`, `dm/mohdm4`, `dm/mohdm5`, `m2l1`, `m2l3`
   and `m4l1`.

5. Flares: the sun lens flare and its fade compared with GL1; the light
   flare depth test, unreachable in the game, reads depth without waiting
   (see Flares). Dynamic lights (dynamic lightmaps uploaded mid-frame and
   `r_fastdlights 1`), cinematics (`RE_StretchRaw`, scratch image),
   `TEXENV_ADD`, `r_showtris` 1 and 2, `r_shownormals`, GL's line rule and
   line stipple (`VK_KHR/EXT_line_rasterization`). Not reachable with
   OpenMoHAA and its data, so not compared: ghost textures (no `.gst`
   file), AVI capture (compiled out of the client), the game's debug lines
   (covered by the view, see GL1 quirks), `GLS_CLAMP_EDGE` (never set;
   ignored) and `RE_UploadCinematic` (the shader parser has no videoMap).

Remaining:

6. Parity pass against GL1 screenshots on several maps, then performance:
   a memory sub-allocator for textures (one allocation per image now),
   saving the pipeline cache to disk, fewer render pass breaks, avoiding
   repeated copies of the same vertexes for multi-stage shaders, frame
   pacing, `r_ext_multisample` (a multisampled target resolved into the
   current one).

Known risks, not seen on RADV but worth handling before shipping:

- Presentation semaphores (one per swapchain image) are reused across
  swapchain recreation and destroyed at shutdown after `vkDeviceWaitIdle`,
  which formally doesn't cover a queued present's semaphore wait. Drivers
  that defer presentation (NVIDIA, MoltenVK) are the likeliest to object.
  `VK_EXT_swapchain_maintenance1` present fences would make it exact.
- MoltenVK (portability subset): the `A = ONE` view swizzle needs
  `imageViewFormatSwizzle` and the stride-0 vertex bindings need
  `vertexAttributeAccessBeyondStride`; neither is queried. The fallbacks
  are forcing alpha to 1 in the shader for RGB textures and pushing the
  constant color as a push constant.
- One `vkAllocateMemory` per texture storage: Windows drivers report
  `maxMemoryAllocationCount` 4096, which `MAX_DRAWIMAGES` (2048) plus
  replaced storages stays under, but not by much.
- Without `VK_KHR/EXT_line_rasterization` (or without `bresenhamLines`)
  lines use the driver's default rule, which may differ from GL's by a pixel
  here and there; only debug drawing has lines.

Windows: the same code applies; vendor the Vulkan headers (or require the
Vulkan SDK) so CI can build without extra installs. `find_package(Vulkan)`
needs the loader library to be found even though it isn't linked.
