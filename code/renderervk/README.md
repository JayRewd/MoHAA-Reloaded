# renderervk — Vulkan renderer

`renderer_vulkan` is a copy of the GL1 renderer (`code/renderergl1`) with every
OpenGL call removed. All GPU work goes through the interface in [`vk.h`](vk.h),
which the `vk*.c` files implement with Vulkan. It is the default renderer
(`cl_renderer vulkan`) of builds that include it (`BUILD_RENDERER_VULKAN`,
with `USE_RENDERER_DLOPEN`); `cl_renderer opengl1` selects the GL1 renderer.
On a system without Vulkan (no loader, driver or device) it stops with an
error that says so and suggests `+set cl_renderer opengl1`. `cl_renderer`
is archived, so a config written before keeps its renderer.

Status (2026-10-14, branch `vulkan-renderer`): milestones 1 to 6 of the plan
below are done.

- the main menu matches GL1 pixel for pixel, except the animated ceiling fan
  (its frame depends on the wall clock), and so do the options pages, the
  weapon and team selection and the main menu of a game (`views/menus.txt`,
  but for a few pixels off by 1)
- with the deterministic setup described in [Testing](#testing), 3D views
  match GL1: world surfaces and lightmaps, static and animated models,
  alpha-tested and blended surfaces, decals, the view weapon, the sky, the
  sky portal and farplane fog, on `dm/mohdm1`, `dm/mohdm2`, `dm/mohdm4`,
  `dm/mohdm5`, `m2l1`, `m2l3` and `m4l1`, and in the milestone 6 parity
  pass on `dm/mohdm3`, `dm/mohdm6`, `dm/mohdm7`, `m1l1`, `m1l2a`, `m1l3a`,
  `m3l2`, `m3l3`, `m5l1a`, `m5l2a`, `m5l3`, `m6l1a`, `m6l2a`, `m6l3a` and
  `obj/obj_team1` to `obj/obj_team4`: LOD terrain, the ocean and other
  water shaders, snow, night and fog maps, six more sky boxes (one with a
  cloud layer), sun flares, environment mapping, decals (91 views; the
  parity pass needed no renderer change). When both drivers rasterize with
  the same subpixel precision (a window wider than 4096 pixels) they match
  to the bit except for pixels off by 1 (usually under 0.3 % of the frame,
  up to 5 % when large polygons pass right next to the camera: the drivers
  clip against different guard bands; transient HUD text), a few isolated
  alpha-tested pixels, and what the game randomizes (snow and smoke
  particles, AI poses, swinging shutters). At 640x480, radeonsi uses a finer
  subpixel precision than RADV and 1 to 8 % of the pixels differ by 1
- fog and the user clip plane (portals and mirrors) are implemented
- the sun lens flare and its full-screen fade, the projected dynamic
  lights of `r_fastdlights 1`, cinematics, `TEXENV_ADD` (with a test
  shader: no shipped shader produces it) and `r_showtris`/`r_shownormals`
  match GL1 the same way (see [Testing](#testing) for the numbers); the
  dynamic lights of `r_fastdlights 0` are drawn per pixel, with a smoother
  falloff than GL1's texel-filtered dynamic lightmaps (an accepted
  difference, see GL1 quirks)
- the light flare depth test (`RB_TestFlare`, unreachable in OpenMoHAA, see
  [Flares](#flares)) reads the depth without waiting for the GPU; line
  stipple and GL's line rasterization rule come from
  `VK_KHR/EXT_line_rasterization`
- no validation or synchronization-validation message (`r_vkValidation 2`)
  during startup, the menu, a map load, `vid_restart` (also with a mode
  change and inside a map), window resizes (`r_allowResize 1`), `r_shadows 2`,
  `r_measureOverdraw 1`, `r_showImages 1`, texture mode changes, fogged maps,
  single-player maps, the sky portal, sun flares, dynamic lights, cinematics,
  debug drawing (with and without stipple) and shutdown; no validation
  message in any milestone 6 comparison run (`r_vkValidation 1`) and none
  with synchronization validation on `m3l2`, `m5l3`, `m6l2a`, `dm/mohdm7`,
  `obj_team3_deepbluesea` and `obj/obj_team4`; after the performance work,
  none either with synchronization validation for `mohdm1_dlight`, the
  main menu, `dm/mohdm1` with `r_ext_multisample 4` and `r_measureOverdraw
  1` (stencil readbacks every frame), live window resizes followed by
  their `vid_restart`, mode changes and a multisampling `vid_restart`;
  after the depth image change, none with synchronization validation for
  `mohdm1_dlight`, `m6l1a` and `r_measureOverdraw` turned on and off on
  `dm/mohdm1` (the depth image becomes readable, then not 300 frames
  later; overdraw counts equal to GL1's from the first frame), with 1 and 4
  samples; after the known risks' fixes (2026-10-18), none with
  synchronization validation through startup, map loads, `vid_restart`
  with mode changes, live resizes (a burst of 11 swapchains replaced),
  minimize and restore (also a simulated window without area, the Windows
  path), fullscreen toggles and shutdown, with present fences and with
  every fallback forced (`r_vkFallbacks 15`), and none in the 41 lists'
  runs of the build-against-build check (`r_vkValidation 1`); with the
  pipelines and skin data at load (2026-10-18), none with synchronization
  validation through startup, map loads and a map change, `vid_restart` in
  a map and with a mode change, a demo and shutdown
- performance (milestone 6, see [Performance](#performance)): on an idle
  machine Vulkan spends 13 to 28 % less main-thread time in the renderer
  than GL1 in the measured views at 1920x1080 (fence waits left out; after
  the second round, which took 6 to 24 % off: hidden symbol visibility,
  cheaper static model culling, cached sun traces, a faster draw surface
  sort), and in the `m6l1a` forest, where the GPU limits, its frames take 7
  % less time than GL1's at 3840x2160 and 5 % less at 1920x1080 (`m3l3`:
  13 % less at 1080p, CPU-bound; but 3 to 4 % more at 3840x2160 in
  `dm/mohdm3` and `m3l3`). Frames are drawn straight into the swapchain image (no copy
  at present, 8 to 9 % more frames per second where the GPU limits), the
  depth image is only readable while depth or stencil readbacks are wanted
  (a readable one has TC-compatible HTILE on RADV, which keeps fewer depth
  tiles compressed: 12 to 13 % more GPU time in that forest), textures
  share memory blocks, the pipeline cache is saved to disk, and with vsync
  the wait for the display happens after the present, like GL's swap.
  `r_ext_multisample` works (2 to 16 samples), 3 % slower than GL1's with 4
  samples (9 % before alpha was written with it)
- static geometry (2026-10-15, step 1 of a redesign of the back end): the
  world's faces, triangle surfaces and grids and the static models are
  uploaded once per map and drawn from device memory when their shader
  allows it, instead of being tessellated and copied every frame;
  consecutive static models with the same one-pass shader share a batch.
  The frames are the same to the bit as before in every view without
  random content; the renderer's CPU time goes down 15 to 20 % in the
  `obj_team2` yard and the `dm/mohdm3` overview (640x480 to 3840x2160, see
  Performance, Static geometry), with little change in the forests, whose
  static models mostly still need the CPU (steps 2 and 3, see Plan); the
  GPU time is unchanged
- dynamic lights per pixel (2026-10-16): the fragment shader adds the
  frame's lights to the lightmap instead of the CPU rebuilding dynamic
  lightmaps every frame; with 16 random lights the renderer's CPU time
  goes down 57 to 66 % in the `obj_team2` yard and the `dm/mohdm3`
  overview (both now at the 1000 fps limit at 1920x1080, from 559 and 487
  fps; GL1 479 and 447), with no measurable GPU cost at 3840x2160 (see
  Performance, Dynamic lights); views without dynamic lights are the same
  to the bit as before
- skeletal models on the GPU (2026-10-16, step 3, measured on new
  multiplayer views with 20 player models, see Testing): the vertex
  shader skins them and lights them (`rgbGen lightingSpherical`) with the
  CPU's floating point operations, so the frames are the same to the bit,
  and also collapses their vertexes at a lower level of detail; with a
  fix of the sun visibility trace cache, whose hash sent every entity on
  whole-unit coordinates to the same slot, the renderer's CPU time goes
  down 57 to 64 % in these views (1.00 to 1.41 ms to 0.39 to 0.56 ms at
  640x480; at 1920x1080 three of them now reach the 1000 fps limit, from
  652 to 838 fps, GL1 506 to 757, and the fourth runs at 772 fps instead of
  614, GL1 500), with the GPU time unchanged within 1 % at 3840x2160 (see
  Performance, Skeletal models); views without players don't change
- GPU time at 3840x2160 (2026-10-16 evening): where the GPU limits, the
  Vulkan renderer took 1 to 4 % more GPU time than GL1; the pipelines
  without blending now write alpha too (an RGB-only write to the BGRA
  swapchain image is a partial write), the depth image has no stencil
  plane when no stencil buffer is wanted, and RADV's clears to the fog
  color are drawn: 2.7 to 5.4 % less GPU time per frame in six of seven
  views (0.7 % in the seventh), faster than GL1 in all but the
  `dm/mohdm3` overview (0.6 % slower), with the same frames (see
  Performance, GPU time at 3840x2160). What is left is RADV not
  compressing textures
- frame spikes (2026-10-16 night): a frame log (`r_vkFrameLog`,
  `vkframelog`, off by default and free when off) records every frame's
  time, its parts and what the backend did, for a tester to send; demos
  of real matches played frame by frame compare both renderers on moving
  content. In two `obj` matches Vulkan's 99th and 99.9th percentiles are
  lower than GL1's at every size (1920x1080: 1.18 and 1.56 ms against
  1.92 and 2.65, 1.95 and 2.69 against 2.71 and 3.52), and the causes of
  the slow frames are ranked in Plan, Remaining (see Performance, Frame
  spikes)
- frame limiter (2026-10-17, an engine change outside this directory, for
  both renderers): `com_maxfps` is a deadline in microseconds that advances
  by one period instead of whole milliseconds since the last frame, so caps
  that don't divide 1000 are exact (144, 165 and 240 ran at 167, 167 and
  250 frames per second) and capped frames start evenly (the 99th
  percentile of the interval between frame starts 2 to 10 µs above the
  period, the old limiter's up to 83 µs above its whole milliseconds); the
  median time from a frame's input read to its present is the same or
  shorter at every cap with both renderers, the polled last millisecond of
  the wait costs 0.4 to 0.7 ms of a core per frame. Caps that divide 1000
  (125, 250, 500) start frames on the tick of the millisecond clock, as the
  old limiter did, so every frame there is the same whole milliseconds of
  game time (one usercmd): with the deadline in microseconds anywhere in
  the millisecond, up to half of the frames at 250 were 3 and 5 ms (see
  Performance, Frame limiter)
- present timing (2026-10-17, measurement only): when frames reach the
  display after their present, under KWin with Xwayland and as a native
  Wayland client, from the X11 completion events, Wayland's presentation
  feedback, the kernel's vblank timestamps and the framebuffer the primary
  plane scans out (`libinlat.so` with `INLAT_DISPLAY=1`,
  `framelog.py --display`). From input read to display at 1920x1080 on
  the 60 Hz display: fullscreen immediate (tearing) 1.5 ms uncapped and
  2.1 at 250, fullscreen mailbox (direct scanout, KWin bypassed) 4.6 to
  5.7 and 6.7 to 7.5, a window (composited) 6.6 to 7.3 and 8.6 to 8.9,
  FIFO 50.5 (three frames queued) and 71 in a window; Xwayland costs
  nothing measurable. The pacing changes that could lower it without
  adding lag, with their measured upside, are in Plan, Remaining (see
  Performance, Present timing)
- world culling (2026-10-17): starting from the Metal port hand-over's
  review of JayRewd's exact world culls, world faces wholly outside the
  view are left out (half of those GL1 draws in the `obj_team2` yard and
  `o2match`; not where dynamic lights reach), the BSP walk is replaced by the potentially visible leaves
  in its order, culled 8 at a time, the PVS is listed from a compact copy
  of the leaves (GL1's pass over every node took 0.23 ms at each cluster
  change in `obj/obj_team4`, 2 % of a match's frames) and culling a face
  reads only its `msurface_t`. The same leaves and surfaces as GL1's code
  (checked beside it, view by view, with a temporary build) and the same
  frames to the bit; the renderer's CPU time 2 to 10 % lower in the yard,
  4 to 5 % in `o2match`, 1 to 2 % in `o4match`, the same within the noise
  in the `dm/mohdm3` overview (see Performance, World culling)
- known risks before shipping (2026-10-18): presentation semaphores
  belong to their swapchain and go only once the presentation engine is
  done with them (present fences with `VK_KHR/EXT_swapchain_maintenance1`,
  otherwise the device wait that comes before), MoltenVK's portability
  subset has fallbacks for the view swizzle and the bindings of stride 0,
  lines without `VK_KHR/EXT_line_rasterization` were checked; every
  fallback can be forced (`r_vkFallbacks`) and gives the same frames, the
  input read to present time is unchanged, and Windows has a test
  checklist ([WINDOWS_TESTING.md](WINDOWS_TESTING.md); see Plan 15)
- pipelines and skin data at load (2026-10-18): the pipelines of a map's
  draws are created at the end of its load, worked out from the registered
  shaders as the stage iterators draw them, and a skeletal model's skin
  data (GPU skinning) when the model is registered, instead of at their
  first draw: with cold caches (the first play after an install) the 39
  and 49 pipelines that two matches created while they were played (60 and
  80 ms, up to 58 ms in one frame) now come at load (the map load 0.18 to
  0.20 s longer, 2 pipelines left in the match), the 99.9th percentile of
  the frame time goes from 2.08 to 1.56 ms and from 3.66 to 2.76 ms, the
  maximum from 34 and 58 ms to 21 and 29 ms (what registration in the
  match costs both renderers); after a driver update (the keys of what
  still came late are kept in `vkpipelines.keys`) and with warm caches
  none is created in the match and the load takes as long as before; the
  same frames, the same input to present time (see Performance, Pipelines
  and skin data at load)
- skeletal culling (2026-10-19): skeletal models wholly outside the view
  are no longer prepared and drawn (GL1 did, and the GPU clipped them): the
  sphere of the animation frame with a margin and the entity's axes,
  against the view's side planes, measured over 20 real matches and the
  41 view lists with a verification build that computed every posed
  vertex (no culled model had a vertex in view; JayRewd's rule, which this
  started from, hid splash drops in view and hasn't the margin for players
  aiming up or down). The renderer's CPU time goes down 6.5 and 8.5 % in
  `o2match` and `o4match` at 640x480 (-22 and -61 µs), 4 to 13 % in the
  soldier views (34 % in one that looks away from them), their 99th and
  99.9th percentiles too (`o4match` at 1920x1080: 1.631 and 2.134 ms to
  1.555 and 2.019), the input to present time a little; the same frames to
  the bit (see Performance, Skeletal culling)
- command rate (2026-10-19, an engine, client and cgame change, for both
  renderers): the client makes its usercmds, the steps of the player's
  movement, at a fixed rate (`cl_cmdfps`, 85 to 125 a second, default 100) instead of
  one per frame, the same steps at every frame rate, and the frame rate is
  no longer held at 1000 a second (`com_maxfps` above 1000 or 0); the view
  turns with every frame's mouse look, the own position moves once per
  command. From JayRewd's `cl_cmdfps` (omaha 1620f310) and his limiter
  change (b26d8a67), with commands on a fixed grid of server time (his steps
  depend on the frame rate), the look applied to the view only and no
  smoothing of the own position. Short steps get stuck on small bumps that
  longer ones get over: on `dm/mohdm2` 1 ms steps (1000 frames a second
  before) stopped 3 to 4 times as many walks as 8 to 12 ms, 4 ms (250) 1.6
  to 2 times as many, and no step got over every bump; why is in Performance,
  Command rate. The commands' steps are exact at 60 to 3300 frames a
  second, a walk makes the same moves to the bit at 144, 250, 1000 and
  uncapped, nothing is lost between commands, the input to display time
  at 250 and 1000 is the same as before; uncapped it is 0.32 to 0.41 ms
  lower where the GPU keeps up (640x480: 1800 to 2100 frames a second),
  higher where it limits (1920x1080 on this machine). Buttons and movement
  keys reach the server with the next command: 5 ms after the frame on
  average at the default, 2 at 250 frames a second before. The `fps`
  counter counts in microseconds
- `renderer_vulkan.so` imports no GL symbol and no Vulkan symbol; it needs
  only SDL2, libm, libstdc++, libgcc_s and libc (same as `renderer_opengl1.so`).
  Vulkan is loaded at runtime through SDL
- `renderergl1` is unchanged. It was the visual reference until
  2026-10-19; since then the look may change where that buys performance,
  and comparisons with GL1 (and with the build before) check for bugs and
  show what changed instead of requiring the same pixels

## Layout

| File | Role |
| --- | --- |
| `vk.h` | backend interface (the contract below) |
| `vk_local.h` | definitions shared by the backend files: `qvk*` function pointers, frame, texture and pipeline-key types |
| `vk_instance.c` | Vulkan loading through SDL, instance, validation messenger, device selection, swapchain, present fences and retired swapchains |
| `vk.c` | frames and command buffers, render targets, presentation, state recording, `vk_draw`, `vk_clear`, `vk_read_pixels` |
| `vk_image.c` | texture handles and storage, the memory blocks textures are placed in, samplers, descriptor sets, staging and ordering of uploads |
| `vk_pipeline.c` | pipeline cache keyed on the state that is baked into pipelines, pipeline creation, the `VkPipelineCache` file, the pipelines created before their first draw and the file of keys created after a load (`vkpipelines.keys`) |
| `vk_framelog.c` | the frame log (`r_vkFrameLog`, `vkframelog`): the per-frame exports timed, the backend's counters and GPU time of every frame, the dump (see Performance, Frame spikes) |
| `tr_staticgeom.c` | the static geometry: world surfaces and static models uploaded at map load, and the back end batches drawn from it (see Implementation notes) |
| `tr_skin.c` | skeletal models skinned and lit on the GPU: the skin data of their surfaces (added when a model is registered), the frame data of each entity, the skinned batches (see Implementation notes) |
| `tr_pipelines.c` | the pipelines of a registration's shaders, worked out from their stages as the stage iterators draw them and created at its end (see Implementation notes, Pipelines) |
| `shaders/generic.vert`, `shaders/generic.frag` | the shaders of every pipeline; the vertex shader is built with and without `gl_ClipDistance`, the fragment stage is specialized per pipeline |
| `shaders/skinned.vert` | the vertex shader of skinned draws (skinning and `rgbGen lightingSpherical`), built with and without `gl_ClipDistance`; its outputs are `generic.vert`'s |
| `shaders/compile_shaders.sh` | regenerates `vk_shaders.c` with `glslc` |
| `vk_shaders.c` | generated SPIR-V as C arrays (committed, so builds don't need `glslc`) |
| `tools/` | GL1/Vulkan screenshot comparison and performance scripts, view lists (see Testing) |
| `WINDOWS_TESTING.md` | a tester's checklist for Windows: commands, cvars, what to send back |
| `tr_immediate.c` | `glBegin`/`glEnd` emulation for debug and 2D drawing, emits indexed triangles or lines |
| `tr_backend.c` | renderer-side state tracking (`GL_State`, `GL_Bind`, `GL_Cull`, `GL_TexEnv`, matrices, vertex arrays, current color) |
| `tr_*.c`, `tr_*.cpp` | the GL1 renderer with GL calls replaced, otherwise unchanged but for the performance work (see Performance) and the static geometry hooks of the surface functions and stage iterators |
| `../sdl/sdl_vkimp.c` | SDL window, video modes and fullscreen without a GL context (`VKimp_*`) |
| `../../cmake/renderer_vulkan.cmake` | build target `renderer_vulkan` (only `GetRefAPI` is exported: hidden symbol visibility) |

The `GL_` prefix of the state helpers and `glConfig`/`glState` are historical
names kept to make it easy to port fixes from `renderergl1`; they do not touch
OpenGL. `GLimp_SetGamma` comes from the shared `sdl/sdl_gamma.c` and only uses
SDL.

Shared files touched outside this directory: `CMakeLists.txt` (option),
`cmake/libraries/{freetype,jpeg}.cmake` and `cmake/platforms/macos.cmake`
(accept the new renderer), `code/renderercommon/tr_common.h` (does not include
`qgl.h` when `USE_VULKAN` is defined; `texnum` is `unsigned int`, same type as
before), and the CI workflows (`.github/workflows`: Linux x64 and Windows x64
only, with `USE_RENDERER_DLOPEN=ON` and `BUILD_RENDERER_VULKAN=ON`, the
renderers then shipping as `renderer_opengl1` and `renderer_vulkan` libraries
next to the executable; artifacts kept 3 days) and the Windows installer
(`misc/setup/windows/app/Package.wxs` lists both renderer DLLs), and the
client's default `cl_renderer` (`vulkan` when `BUILD_RENDERER_VULKAN` is on:
`cmake/client.cmake`, `code/client/cl_main.cpp`), and the frame limiter
(`Com_Frame` and `Com_FrameWait` in `code/qcommon/common.c`,
`Sys_Microseconds` in `code/sys/sys_unix.c` and `code/sys/sys_win32.c`,
the timer slack set in `sys_unix.c`'s `Sys_PlatformInit`, `NET_SleepUs`
in `code/qcommon/net_ip.c`, their declarations in `code/qcommon/qcommon.h`:
`com_maxfps` caps in microseconds, those that divide 1000 on the
millisecond tick, see Performance, Frame limiter), and the command rate
(`code/client/cl_input.cpp`: `cl_cmdfps`, `CL_CreateNewCommands`,
`CL_ApplyLook`, `CL_GetViewUserCmd`, `CL_SendCmd`; `code/client/client.h`;
`code/client/cl_cgame.cpp` and `code/cgame/cg_public.h`: the cgame import
`GetViewUserCmd`; `code/cgame/cg_predict.c`; `code/cgame/cg_specialfx.cpp`;
`Com_Frame` and `Com_ModifyMsec` in `code/qcommon/common.c`: caps above
1000 and uncapped, frames of 0 ms, the `fps` counter in microseconds; see
Performance, Command rate).

## Build and run

Requirements: the Vulkan headers (1.4.341 on the dev machine; Ubuntu 22.04's
1.3.204 are too old), either Khronos' Vulkan-Headers installed as a CMake
package (`VulkanHeaders`, found with `-DVulkanHeaders_DIR=<prefix>/share/cmake/VulkanHeaders`
or `CMAKE_PREFIX_PATH`; the CI builds fetch and install the `v1.4.341`
tag) or, through CMake's `FindVulkan`, the system's headers plus the
loader development library, which `FindVulkan` needs to report Vulkan as
found even though the renderer doesn't link it (only `Vulkan::Headers` is
used). `glslc` is only needed to change shaders. `vulkan-validationlayers`
is needed for `r_vkValidation`.

```sh
cmake -S . -B build-vk -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_RENDERER_VULKAN=ON
cmake --build build-vk
cd build-vk/RelWithDebInfo
./openmohaa +set cl_renderer vulkan +set fs_basepath ~/Documents/moh \
    +set fs_homepath /tmp/moh-home +set r_fullscreen 0 +set r_mode 3
```

`cl_renderer opengl1` runs the GL1 renderer from the same build for comparison
(`cl_renderer` defaults to `vulkan` when the Vulkan renderer is built,
`cmake/client.cmake`, `code/client/cl_main.cpp`).

Cvars of the backend:

| cvar | |
| --- | --- |
| `r_vkValidation` (latched, default 0) | 1 enables `VK_LAYER_KHRONOS_validation` and prints its warnings and errors to the console; 2 also enables synchronization validation (slow) |
| `r_swapInterval` (latched) | 0: immediate present mode (mailbox if unavailable), < 0: FIFO relaxed, > 0: FIFO; FIFO when the wanted mode is missing |
| `r_ext_multisample` (latched, shared with GL1) | 0 or 1: off; 2 to 16: samples (the largest the device supports up to that), see Implementation notes |
| `r_staticGeometry` (default 1) | 0 tessellates world surfaces and static models on the CPU every frame instead of drawing them from the static geometry (see Implementation notes) |
| `r_gpuSkinning` (default 1) | 0 skins and lights skeletal models on the CPU every frame instead of in the vertex shader (see Implementation notes, Skeletal models) |
| `r_skelCull` (default 1) | 0 prepares and draws skeletal models outside the view too, as GL1 does (see Implementation notes, Skeletal culling) |
| `r_vkFrameLog` (latched, default 0) | the frame log: 1 records the last 131072 frames (larger values: that many), see `vkframelog` below and Performance, Frame spikes; 0 costs nothing |
| `r_vkFallbacks` (latched, default 0) | for tests: ignores what the device has and uses the fallback, bits added: 1 no `imageViewFormatSwizzle` (the alpha of RGB textures set in the shader), 2 no binding stride of 0 (those bindings read per instance, as on every portability subset device), 4 no `VK_KHR/EXT_line_rasterization`, 8 no `VK_KHR/EXT_swapchain_maintenance1` (no present fences); see Implementation notes. The startup line `Vulkan present fences ..., view swizzle ..., stride 0 bindings ..., Bresenham lines ..., line stipple ...` says what is used |

The console command `vkinfo` prints the backend's counters since its last
call: frames, render passes, submissions, mid-frame flushes, draws,
pipeline and descriptor set binds, bytes copied into vertex and staging
buffers, uploads ordered with draws, textures, pipelines and device memory
allocations created (with the microseconds spent creating them; of the
pipelines, those created before a draw needed them, `prebuiltPipelines`
and `prebuiltUs`, see Implementation notes, Pipelines), skin data added
(`skinUploads`, `skinBytes`), depth
images replaced to become readable or not (see Implementation notes,
Render target), swapchains created, bytes of frame vertex buffers created
because one was full, time spent waiting for frame fences (`waitUs`, with
the waits of mid-frame flushes, which `flushUs` also counts), in `vkQueueSubmit`, `vkAcquireNextImageKHR`
(before a present and right after one) and `vkQueuePresentKHR`, from the
start of a frame's recording to its present, and the GPU time of the frames
from timestamps (main command buffer, copy to the swapchain image, whole
frame); then the device memory objects, memory blocks and pipelines that
exist (with how many were created before their first draw and how many of
those a draw has used) and whether the depth image is readable.
`tools/perf.py` reads it.

With `r_vkFrameLog 1` (latched: on the command line, or set it and
`vid_restart`), `vkframelog [file]` writes every frame recorded since the
last `vkframelog` (at most the last 131072) to `fs_homepath/main/<file>`
(default `vkframes.txt`) and prints the frame time percentiles and the
slowest frames with what happened in them; `vkframelog clear` drops them.
See Performance, Frame spikes, for what is in the file and how to send one.

The pipeline cache is saved as `vkpipelines.cache` in `fs_homepath/main` at
shutdown and `vid_restart` (when it grew) and given to the driver at
startup when the device and driver match. Next to it, `vkpipelines.keys`
lists the pipelines that were created after a load (a draw no shader of
the load gave, a shader registered during the match), at most 256: they are
created at the end of every load of the next runs, also after a driver
update, which makes `vkpipelines.cache` useless (see Implementation notes,
Pipelines). Deleting both files gives what a first play after an install
does.

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
- `pause` before the screenshot: GL1's `screenshot` command reads the back
  buffer before anything of the frame is drawn, a buffer that Mesa hands out
  from two or three swaps earlier, while the Vulkan renderer takes it at the
  end of the frame in which the command runs (see Frames and
  synchronisation), so without the pause everything that moves (trees and
  bushes sway) differs. While paused every frame is the same.
  `finishloadingscreen` unpauses too.
- The `screenshot` command centerprints "Wrote ..." in the view; the blank
  `centerprint` replaces it before the next screenshot.
- `ui_hud 0` and `cg_drawviewmodel 0` hide the HUD and the view weapon (set
  `cg_drawviewmodel 2` to test the weapon's depth range). Messages such as
  "Game Saved" and "An objective has been added!" go to the game message
  box, which `+set ui_gmbox 0` (on the command line: the box is created at
  startup) removes; `run.sh` sets it.

Still random between runs: what the game seeds with the wall clock
(`G_InitGame` seeds `rand()` with `Com_Milliseconds()`), such as the snow
particles of `dm/mohdm5` and `m6l1a`, AI movement in single-player maps and
the vehicles AI drives (the truck convoy of `m1l1`).

Single-player maps that start in a vehicle ignore `tele` while the player
is attached to it: `m1l1` (a truck of the convoy for about 90 s of game
time; `views/m1l1.txt` waits, run it with `TIMEOUT=300`), `m1l3a`
(halftrack gun, it starts driving about 15 s in), `m5l2a` (tank, the whole
level), `m3l1a` (landing craft for about two minutes; `views/m3l1a.txt`
waits, run it with `TIMEOUT=400`). `face` still turns the view.

Views must be taken from inside the world. From outside it (the void
around the map, above the sky brushes, inside a solid; the view cluster is
-1) part of the frame can be covered by no surface at all: it keeps what an
earlier frame drew into that buffer, an older swapchain image with Vulkan
(the offscreen render target before milestone 6's performance work) and an
older back buffer with GL1, so the two differ there without meaning
anything. Two such views of this parity pass
looked like bugs: `obj_team2` from 1177 289 -120 and `dm/mohdm7` from
-1000 1000 800 above the sky, where the sky box (drawn from the sky polygons
seen from the view point) covers less than the frame. `shots.py` turns on
`r_showcluster` and `compare.sh` warns about shots whose cluster is -1.

GL1 now and then saves an all-black frame (about one shot in 40 in the
milestone 6 runs, a different one every run): the back buffer Mesa hands
out was never drawn, it seems. Once it saved the frame of the previous shot
instead. Run the list again; `compare.sh` warns about all-black GL1
frames. With several games running at once it happened more often and
also gave frames with black areas, so run one game at a time. The Vulkan
frames are the same either way.

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
  player game, `set <command>`, `shot <name> x y z pitch yaw roll [wait]`,
  `menu <name> [<menu> [wait]]`, `mask <name> x,y,w,h`), one per map or
  feature tested so far. `shot` waits 2 s of game time after moving the
  camera, or `wait` milliseconds (to catch a fade). `menu` takes a
  screenshot of a UI menu (`pushmenu <menu>`, then `popmenu`): over the
  main menu before the first `map`, over the paused view of the previous
  `shot` after it. `mask` leaves a rectangle of the 640x480 UI screen out
  of a view's statistics, scaled to the frame size (`tgadiff.py
  --mask-res`), for what the UI animates with the wall clock.
- `compare.sh`: writes the cfg (`shots.py`), runs it with GL1 and Vulkan
  (`run.sh`), then prints per channel the max and mean absolute difference
  and the number of differing pixels for every view (`tgadiff.py`), and
  writes PNG copies and an amplified difference image (|a - b| x 8, equal
  pixels as a dark copy) to `$MOH_OUT`. `WIDE=1` compares at 4160x240.
  `VAL=2` runs with synchronization validation; validation messages are
  printed. A `views/<list>.shader` next to the list is installed as
  `main/scripts/aaa_<list>.shader` during the runs: the shader file listed
  first wins, so it replaces shaders of the game data (see
  `mohdm1_texenv`, `obj_team3_deepbluesea`). It warns about shots taken
  from outside the world and about all-black GL1 frames (see
  Deterministic 3D views); `TIMEOUT` (seconds, default 180) is passed to
  `run.sh` for the lists that wait long (`m1l1`, `m3l1a`).
- `hotspots.py`: zoomed side-by-side crops of the areas that differ most.
- `montage.py`: a grid of images, for example `gl | vk | diff` rows.
- `bspinfo.py`: worldspawn keys (fog, sun), spawn points, the number of LOD
  terrain patches and static models (the models whose path contains a
  keyword are listed with their origins), whether the `.dcl` decals load
  (keyword `decal` lists them) and the surfaces of the shaders whose name or
  script contains a keyword (`polygonoffset`, `alphafunc`, `nextbundle`,
  ...; terrain patches included), to pick views.
- `perf.py`: frame time and renderer time of views with both renderers,
  uncapped, in interleaved runs (see Performance), also of demos played
  frame by frame (`demo` lines, `views/perf_demo.txt`) and with every
  frame kept (`--framelog`), at frame rate caps (`--caps`), and of other
  engine builds (`--binaries`); `rtimer/` builds the
  `renderer_timed.so` wrapper it uses, `libstacks.so`, a stack sampler
  whose output `stacks.py` sums up, `libglts.so`, GPU timestamps of
  GL1's frames (see Performance, GPU time at 3840x2160), and
  `libinlat.so`, the time from each frame's input read to its present
  (see Performance, Frame limiter) and, with `INLAT_DISPLAY=1`, what the
  presentation engine, the compositor and the display did with it (see
  Performance, Present timing).
- `framelog.py`: frame time percentiles and the causes of the slow frames
  from the Vulkan renderer's frame log (`r_vkFrameLog`, `vkframelog`) or
  `renderer_timed.so`'s frame files, a `perf.py --framelog` directory as a
  whole (GL1 against Vulkan frame by frame, an SVG of the distributions,
  the input to present times of `libinlat.so`), the stack samples of
  the slow frames (see Performance, Frame spikes), and when each frame
  reached the display (`--display`, see Performance, Present timing).
- `soldiers.py`: writes a view list of a multiplayer scene, soldier models
  around the camera and muzzle flash lights (see Test content).
- `summary.py`: the worst view's numbers of several lists after
  `compare.sh` (`--wide` for the `WIDE=1` runs), in the form of the
  milestone 6 table below, with that table's row next to them
  (`--readme README.md`).

Changes that should not change the picture (the performance work of
milestone 6) are best checked against the previous Vulkan build rather
than GL1: copy the old `renderer_vulkan.so` to `renderer_<name>.so` in the
binary directory, run the list with `run.sh <name> <list>` and compare
its shots with `tgadiff.py`; the frames of a deterministic view are then
identical to the bit, run after run. For an engine change, copy the old
`openmohaa` under another name and run the list with `MOH_BIN=<name>
run.sh vulkan <list>`.

With RADV and GL1 on radeonsi (same GPU) expect:

- At 640x480 (`r_mode 3`) 1 to 8 % of the pixels differ by 1 and a few
  hundred alpha-tested pixels (foliage edges) flip. radeonsi rasterizes
  viewports of up to 1024 pixels with 1/4096 pixel vertex precision, up to
  4096 with 1/1024, and 1/256 beyond (`si_state_viewport.c`); RADV always
  uses 1/256. Whole triangles then interpolate their texture coordinates very
  slightly differently, which bilinear filtering turns into ±1.
- With a window wider than 4096 pixels both drivers use 1/256:
  `+set r_mode -1 +set r_customwidth 4160 +set r_customheight 240`. The
  frames are then the same to the bit except pixels off by 1 on large
  polygons right next to the camera (the drivers clip against different
  guard bands; usually a few thousand pixels, up to 5 % of the frame when
  tree trunks, terrain or roofs pass the camera, as in `m61_slope` and
  `d6_roofs`) and on transient HUD text, a few isolated alpha-tested
  pixels, plus random particles and AI poses. One view differs much more:
  in `o1_sky` (`obj_team1`, looking up between hedges) the hedge polygons
  that pass nearly edge-on within a few units of the eye fill both ends of
  the 4160-pixel frame, and there their textures come out visibly shifted
  between the renderers (55 % of the pixels differ). The same view matches
  like the others at 640x480 and the middle of the wide frame matches, so
  this is attributed to the drivers clipping those polygons differently;
  it was not proven. That is the setting to check a change for exactness.
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
  for the projected dlight pass. Its views show world faces, static models
  (the bushes, lit per vertex) and a small entity (the lamp);
  `mohdm1_dlight2` adds a brush model lit per pixel by light 0 and by
  `ProjectDlightTexture` from another light (the door `*15`: GL1 lights
  brush models' lightmaps with light 0 only, see Implementation notes,
  Dynamic lights), curved patches and a soldier (`script_model`), and
  `mohdm3_dlight` LOD terrain. `cg_acidtrip N` adds N random dlights a
  frame (radius 128 to 639) for a quick look and for timing (`perf.py
  --exec`), not for comparisons.
- Multiplayer views with players (2026-10-16): `mp_mohdm1`, `mp_mohdm2`,
  `mp_mohdm7`, `mp_obj_team1` and `mp_obj_team4`, written by
  `tools/soldiers.py` (the command is in each list's first lines). Each
  spawns 20 `script_model`s with the player TIKIs of multiplayer
  (`models/player/*.tik`, allied and axis), 16 in a fan in front of the
  camera from 160 to 1060 units and 4 beside it, outside the view; they
  are dropped to the floor (`physics_on`, so that `droptofloor` traces
  against the world, then `physics_off`, and `physics_velocity "0 0 -1"`
  to cancel the upward velocity `physics_on` gives, which `physics_off`
  keeps: without it the soldiers rise by a unit a second), turned,
  animated with player animations (run, walk, idle, crouch) and most get
  a weapon in the right hand (`attachmodel models/weapons/<weapon>.tik
  tag_weapon_right`). Shots: `<tag>_front`; `<tag>_flash`, the same view
  with three dynamic lights next to three soldiers (muzzle flashes), 1.6 s
  of game time later; and `<tag>_side`, the view turned towards the
  soldiers beside the camera. The first shot comes more than 33 s after
  the map is loaded: the engine's skeletor keeps the time of a head's next
  blink in a `short` (`skeletor_c::m_timeNextBlink`), so heads blink at
  random during the first 32.8 s of `Sys_Milliseconds` and never after,
  and earlier shots differed between two runs of the same build in a few
  dozen pixels of one soldier's face. Now two runs of the same build give
  the same frames, but for 2 or 3 pixels of `pm7_front`.
  `views/perf_mp.txt` times them (`perf.py` takes the shots of a view
  list with `views <list> [shot ...]`).
- Menus: `menus.txt` takes the main menu (fan masked), the options pages
  (options, controls, video, audio, advanced, multiplayer), the weapon
  selection, then in a game of `dm/mohdm1`, over a paused view, the weapon
  selection with the sign of a hovered weapon (`ui_weaponsign`), the main
  menu of a game (BACK TO GAME, QUIT; fan masked) and the team selection.
  Menu names are the first line of each `ui/*.urc` (`listmenus` lists the
  loaded ones). The panels and buttons of most of these menus (the
  weapon selection's wood panel, the controls, video and options pages,
  BACK TO GAME, QUIT, APPLY, DEFAULT, BACK) are in `Pak6EnUk.pk3`, the
  English localization pak; an install without it draws them, with
  either renderer, with the default image (`*default`: 16x16, grey 32
  with a white border, stretched and filtered: dark grey panels with
  soft white frames), and with `developer 1` the log shows `could not
  find 'textures/mohmenu/...'`. That was a tester's report on 2026-10-16
  (NVIDIA, Linux; GL1 the same); adding the pak fixed it. Running
  `menus.txt` with a basepath without that pak gives the same frames with
  both renderers.
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
- Milestone 6 parity pass (one list per map, the comment at the top of each
  says what it covers):
  - sky boxes: `mohday1` (`mohdm3`, `mohdm6`, `obj_team2` at dusk),
    `africanight` with its scrolling additive cloud layer (`mohdm7`,
    `m1l1`, `m1l2a`), `mohnightfog` (`m1l3a`), `d-day2` (`m3l2`, `m3l3`,
    `obj_team3`), `m5l2` (`m5l1a`, `m5l2a`, `obj_team1`, `obj_team4`),
    `mohday2` (`m5l3`); the `m6` maps use `caulksky` under fog;
  - sun lens flares: `d3_sun`, `m32_sun`, `m53_sun`, `o2_sun` (and
    `m53_sunedge` with the sun just outside the view);
  - water: `deepbluesea_runup` and `deepbluesea_shoreline` (`obj_team3`),
    `deepbluesea` itself through `obj_team3_deepbluesea.shader` (its only
    map, `m3l1a`, shakes the camera), `subpen_water4` under `canal_sludge`
    (`m3l2`), `frozenriver` (`m6l2a`), `riverbed` (`obj_team4`);
  - snow and night: `snowfield1` with `tcMod parallax` (`m6l2a`), the
    snowy forest of `m6l1a` (2763 static models, fog 1500, random snow),
    the script fog of `m6l3a`, night maps `mohdm7`, `m1l1`, `m1l2a`,
    `m1l3a`, `m6l*`;
  - LOD terrain: `mohdm3`, `m1l1`, `m1l3a`, `m3l3`, `m5l1a`, `m6l1a`,
    `m6l2a`, `obj_team1`, `obj_team3`, `obj_team4`;
  - farplane fog: 1500 to 15000 (`m6l1a`, `m5l1a`, `obj_team1`, `m1l3a`,
    `obj_team4`, `m5l2a`, `m3l2`, `m5l3`);
  - environment mapping (`tcGen environment`): the windows of `mohdm3`,
    the wet pipes of `obj_team2`;
  - decals: the posters of `obj_team4`;
  - map-script effects: the burning debris of `m1l1`, the fog of `m6l3a`,
    the convoy, landing craft and halftrack rides (see Deterministic 3D
    views for what they mean for the views).
- No shipped map has a portal or mirror surface (`portal` shader keyword), so
  the user clip plane has no GL1 reference in game content. It was checked
  with a temporary plane forced on the main view: x_eye >= 0 cut every
  frame exactly at the middle column, the kept half was identical to the
  unclipped frame with the fragment shader fallback and identical except ±1
  on the triangles crossing the plane with `gl_ClipDistance`, and a plane at
  400 units removed the near geometry.
- Fallbacks (2026-10-18; RADV has every feature they replace, so they are
  forced with `r_vkFallbacks`, see Build and run): run a list with `+set
  r_vkFallbacks 3` (no view swizzle, no stride 0 bindings) against the
  default build; the frames must be the same to the bit. The game's RGB
  textures have alpha 255 in their data anyway (GL1 makes a texture RGB
  when every alpha is 255), so that alone doesn't show the shader's alpha
  at work: a temporary build that wrote alpha 0 into the uploads of RGB
  textures gave the same frames as before with the swizzle and with the
  shader setting it (`mohdm1`, `obj_team2`), and without either 0.3 to 100
  % of the pixels differed in every view (the sky boxes are RGB). `4`
  (lines without `VK_KHR/EXT_line_rasterization`) gives the same frames on
  RADV; on llvmpipe (`VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json`,
  a device with `strictLines`) it moves line pixels (see Fragment pipeline).
  `8` (no present fences) changes no pixel. The presentation paths were
  run with synchronization validation (`r_vkValidation 2`) and every
  combination of the bits: startup, map loads, `vid_restart` (with mode
  changes), live resizes with `r_allowResize 1` (`xdotool windowsize`, a
  burst of 11 sizes 50 ms apart: 11 swapchains replaced before the
  `vid_restart` the resize schedules), minimize and restore (Xwayland keeps
  the window's size: a temporary build reported no area for 60 frames out
  of every 200, the path of a minimized window on Windows, where the
  swapchain is destroyed and created again), fullscreen toggles (`set
  r_fullscreen 1`, `0`, with `in_nograb 0`; KWin under Xwayland) and quit:
  no message. The validation layer (1.4.341) doesn't check present fences
  against the destruction of swapchains and semaphores, though: a
  temporary build that left the fences out with the extension enabled gave
  no message either, so this rests on reading the specification (see
  Implementation notes, Presentation semaphores and swapchains), not on
  validation. Build against build: the 41 view lists (177 views,
  640x480, `r_vkValidation 1`) with this build and with `r_vkFallbacks 15`
  against the build before: 143 views the same to the bit with both; the
  other 34 are in the lists with random content (`m1l1`, `m1l2a`, `m1l3a`,
  `m2l1_skyportal`, `m3l1a`, `m4l1_fog`, `m5l1a`, `m5l2a`, `m5l3`,
  `m6l1a`, `mohdm5_fog`, `mohdm7`, `mp_mohdm7`: AI, vehicles, the landing
  craft's camera shake, snow, the known 1-pixel noise near the camera), and
  each of them also differs between two runs of the build before (a
  second pass with it twice); no validation message. GL1 against this
  build with `compare.sh` (`mohdm1`, `obj_team2`, `menus`,
  `mohdm1_debug`, `mohdm1_dlight`, at 640x480 and `WIDE=1`) gives the
  numbers of before. Windows: see [WINDOWS_TESTING.md](WINDOWS_TESTING.md),
  the checklist for a tester.

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

Menus (`menus.txt`, 2026-10-16, 12 views): the 8 views over the main
menu give 0 differing pixels but 17 pixels off by 1 in `mn_video` at
640x480, and at 4160x240 198 in `mn_controls` and 14 in `mn_video`, all
off by 1. The 4 views in `dm/mohdm1` are the 3D view `m1_spawn` with the
menus over it: at 640x480 the differences of that view (5 to 7 % of the
pixels, nearly all ±1, 140 to 220 foliage pixels by more than 3), none
in the menus, and the main menu of the game, which covers the view, 0; at
4160x240 28 to 36 pixels, max 4 (0 for the main menu). The same numbers
at 640x480 with the game data without `Pak6EnUk.pk3`. No validation message, also with
synchronization validation (`VAL=2`).

At 640x480 the same lists show the usual 1 to 10 % of pixels off by 1 and a
few dozen to a few hundred isolated pixels off by more (alpha-tested foliage,
and broken `r_showtris` edges where the line and its polygon tie in depth),
all gone or nearly gone at 4160x240.

Results of the milestone 6 parity pass (sequential runs, worst view of each
list; per channel max and mean, share of differing pixels, and the number
of pixels off by more than 3 in the worst view for that count):

| list (views) | 640x480 max R/G/B | differing | > 3 | 4160x240 max R/G/B | differing | > 3 |
| --- | --- | --- | --- | --- | --- | --- |
| `mohdm3` (6) | 235/211/187 | 4.38 % | 199 | 63/58/25 | 0.245 % | 21 |
| `mohdm6` (6) | 116/141/185 | 1.62 % | 121 | 2/2/1 | 1.87 % | 0 |
| `mohdm7` (5) | 247/213/125 | 1.28 % | 112 | 28/25/23 | 0.114 % | 5 |
| `m1l1` (3) | 161/134/121 | 4.49 % | 6132 | 156/129/115 | 7.61 % | 41818 |
| `m1l2a` (5) | 66/57/70 | 1.24 % | 34 | 12/16/18 | 0.260 % | 1 |
| `m1l3a` (4) | 70/57/69 | 1.90 % | 39 | 138/94/92 | 0.243 % | 292 |
| `m3l2` (6) | 136/132/133 | 4.29 % | 144 | 161/157/155 | 1.08 % | 363 |
| `m3l3` (4) | 150/146/143 | 5.50 % | 125 | 157/153/152 | 0.303 % | 16 |
| `m5l1a` (6) | 136/126/116 | 4.53 % | 527 | 127/118/107 | 0.745 % | 3088 |
| `m5l2a` (2) | 112/107/110 | 3.05 % | 195 | 47/38/26 | 0.007 % | 19 |
| `m5l3` (6) | 165/155/128 | 3.34 % | 21 | 50/50/44 | 1.02 % | 32 |
| `m6l1a` (4) | 122/122/126 | 8.26 % | 625 | 126/126/126 | 4.92 % | 1030 |
| `m6l2a` (6) | 90/87/103 | 5.90 % | 281 | 30/27/28 | 3.09 % | 2 |
| `m6l3a` (3) | 53/58/91 | 1.48 % | 91 | 37/37/65 | 0.281 % | 885 |
| `obj_team1` (4) | 110/108/119 | 4.41 % | 171 | 118/118/119 | 54.7 % | 442501 |
| `obj_team2` (6) | 60/59/50 | 2.10 % | 48 | 214/189/204 | 2.56 % | 103 |
| `obj_team3` (6) | 50/56/63 | 3.79 % | 8 | 12/12/12 | 0.355 % | 48 |
| `obj_team3_deepbluesea` (3) | 49/56/63 | 3.55 % | 8 | 12/12/12 | 0.289 % | 48 |
| `obj_team4` (6) | 215/216/179 | 4.08 % | 310 | 83/83/88 | 0.782 % | 21 |

The mean differences stay below 0.15 per channel everywhere except `m1l1`
(up to 0.38: smoke) and the `obj_team1` WIDE row. Nearly every differing
pixel is off by 1 at both sizes; what is off by more was traced, view by
view, to:

- random game content: AI soldiers (`m3l2`, `m1l3a`, `m12_yard`), window
  shutters that swing (`m5l1a`: they are open in one run and closed in the
  other), snow particles (`m6l1a`, `m6l3a`), the smoke and fire light of
  `m1l1`;
- the WIDE clipping cases above (the larger off-by-1 areas of `m6l1a`,
  `m6l2a`, `obj_team2`, `mohdm6`, `m5l3`, and `o1_sky`, attributed to the
  drivers' clipping);
- at 640x480, alpha-tested foliage and one-pixel shifts of polygon edges
  (`o4_poster1`: a vertical edge one column over).

After the performance work (drawing into the swapchain image, screenshots
at the end of the frame, the memory sub-allocator, vertex reuse) every list
was run again at both sizes (`tools/summary.py` gives the rows): 13 lists
at 640x480 and 7 at 4160x240 give exactly the numbers of the table, and the
others differ where the game randomizes (the AI fire of `m1l3a`, which put
the red damage flash into one GL1 shot, its halftrack; the shutters of
`m5l1a`, the tank of `m5l2a`, the snow of `m6l1a` and `m6l3a`; the smoke of
`m1l1`, now 3.7 % and 1.1 % differing), or are better (`obj_team1` at
4160x240: 0.15 % instead of 54.7 %, the `o1_sky` clipping case did not
recur). The milestone 5 lists give their table's numbers too. The sharper
check is the previous Vulkan build: run on all 19 lists at 640x480 (and
`mohdm1_debug` at 4160x240), it gives the same frames to the bit in every
view without random content, with one exception: `d7_window` (`dm/mohdm7`)
has a two-pixel-high crack between world surfaces at the bottom of the
frame (`r_fastsky 1` shows the clear color there), which shows whatever
the buffer held: an earlier frame with GL1 and the old offscreen target,
black in a freshly acquired swapchain image (196 of the 202 pixels off by
more than 3 in that list). With `r_fastsky 1` the old and new frames are
identical there too. No validation message in any of these runs
(`r_vkValidation 1`).

The depth image that is only readable while readbacks are wanted (see
Implementation notes, Render target) was checked against the build before
it in the same way, on 29 lists at 640x480 (131 views: every list but
`m1l1` and `m3l1a`): 20 lists give the same frames to the bit in every
view (among them `mohdm1`, `mohdm3`, `obj_team1` to `obj_team4`, `m3l2`,
`m3l3`, `m5l3`, `m6l2a`, `m6l3a`, the decal, texture env, dynamic light,
sun flare and fog lists). The others differ in 2 to 3970 pixels of some
views: snowflakes (`m6l1a`, `mohdm5_fog`), the random content named above
(`m1l2a`, `m1l3a`, `m5l1a`, `m5l2a`), and off-by-1 noise on surfaces near
the camera in `m6l1a`, `mohdm7`, `m2l1_skyportal` and `m4l1_fog`. Running
the build before the change twice (two copies of the same `.so`) gives the
same kind of differences in the same views (`m61_slope` 2341 pixels,
`d7_arch` 798, `fm4_a`/`fm4_b` 4/2, `m5l1a` 7 to 297), so they vary from
run to run and are not the change (the comparison above found `mohdm7`
identical but for `d7_window`: these near-camera differences don't occur
in every run). No validation message (`r_vkValidation 1`). Against GL1
(`compare.sh`, `summary.py`): `m6l1a` 116/116/126, 8.26 %, 379 pixels off
by more than 3 at 640x480 and 125/125/125, 5.39 %, 272 at 4160x240;
`mohdm7` 247/213/125, 1.27 %, 202 and 48/36/35, 0.091 %, 56, where the
pixels off by more than 3 beyond the table's are the `d7_window` crack
(black in Vulkan, an older frame in GL1; at 4160x240 all 56 on one row).

The second round of the performance work (see Performance) was checked
against the build before it in the same way, on the 31 view lists at
640x480 plus `mohdm1_dlight` with `r_fastdlights 1` and `mohdm1_texenv`
with `r_ext_texture_env_add 1` (145 views): 118 views give the same frames
to the bit; the others are the random content (AI in `m1l2a` and `m1l3a`,
where AI fire put the red damage flash into one run's `m13_c`, the
shutters of `m5l1a`, the tank of `m5l2a`, snow in `m6l1a` and
`mohdm5_fog`) and the near-camera noise of `m2l1_skyportal`, `m4l1_fog`,
`m61_slope` and `mohdm7`, plus one pixel off by 1 in `m33_grass`
(`m3l3`), which two runs of the same old build also gave. With 4 samples
the `mohdm1` views are the same to the bit too. Against GL1
(`compare.sh`): `dm/mohdm3`, `m3l3` and `obj_team2` give the table's
numbers at 640x480, `m6l1a` 120/120/126, 8.28 %, 469 (snow); at 4160x240
`m6l1a`'s `m61_slope` differed in 5.8 % of the pixels, but two runs of the
same old build differ by 5.2 % there. No validation message with
synchronization validation (`r_vkValidation 2`) on `dm/mohdm1` (also with
`r_ext_multisample 4`, and with `r_stencilbits 8` and `r_measureOverdraw
1`), `m6l1a`, `mohdm1_dlight` and `m2l1_skyportal`; the main menu matches
GL1 (fan masked).

The static geometry (2026-10-15, see Implementation notes) was checked
against the build before it in the same way, on the 31 view lists at
640x480 plus `mohdm1_dlight` with `r_fastdlights 1` and `mohdm1_texenv`
with `r_ext_texture_env_add 1` (151 views): 119 views give the same
frames to the bit, among them every view of `dm/mohdm1` to `dm/mohdm4`,
`dm/mohdm6`, `m2l3`, `m3l2`, `m3l3`, `m6l2a`, `m6l3a`, `obj/obj_team1` to
`obj/obj_team4`, the decal, dynamic light (both modes), texture env, sun
flare, fog and debug drawing lists. The other 32 are the random content
and run-to-run noise named above: the smoke and convoy of `m1l1`, AI in
`m1l2a` and `m1l3a` (`m13_sky` caught the red damage flash of AI fire),
the camera shake of `m3l1a`, the shutters of `m5l1a`, the tank of
`m5l2a`, snow in `m6l1a` and `mohdm5_fog`, a particle in `m53_street`
(`m5l3`, which two runs of the old build show too), and the near-camera
noise of `m2l1_skyportal`, `m4l1_fog`, `m61_slope` and `d7_arch` (±1). Also
the same to the bit with `r_vertexLight 1` and with `r_lightmap 1` on
`dm/mohdm1` and `obj/obj_team2`. A bug found that way: a static model
whose surfaces draw nothing at their level of detail still joined a merged
batch and changed the modelview, so the next one-model batch was drawn
with the wrong matrix (29 pixels of `d3_top`); each model's batch now sets
its own matrix. Against GL1 (`compare.sh`, `summary.py`) every list gives
the numbers of the milestone 6 table at 640x480 and at 4160x240 except
where the game randomizes (`m1l1`, `m3l1a`, AI in `m1l3a` and `m3l2`, the
shutters of `m5l1a`, snow in `m6l1a` and `m6l3a`), after running again
the lists where GL1 saved a black frame (`m51_sky`, `d3_terrain` at
4160x240) or the previous shot's frame (`m53_river`); `obj_team1` at
4160x240 gives 0.15 % like the second round, `mohdm7` the `d7_window`
crack noted above. No validation message in any of these runs
(`r_vkValidation 1`), nor with synchronization validation
(`r_vkValidation 2`) on `dm/mohdm1`, `mohdm1_dlight` (both modes),
`m2l1_skyportal`, `m6l1a`, `obj/obj_team2` and a script that loads
`dm/mohdm1`, turns `r_staticGeometry` off and on, runs `vid_restart`,
then loads `dm/mohdm3` and `m6l1a`; the main menu matches GL1 (fan
masked).

The per-pixel dynamic lights (2026-10-15, see Implementation notes,
Dynamic lights), against GL1 (`compare.sh`, `summary.py`: worst view of
each list; per channel max, highest mean, share of differing pixels and the
pixels off by more than 3):

| list (views) | `r_fastdlights` | 640x480 max R/G/B | mean | differing | > 3 | 4160x240 max R/G/B | differing | > 3 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `mohdm1_dlight` (4) | 0 | 254/228/190 | 0.330 | 49.0 % | 2338 | 61/42/23 | 26.2 % | 1123 |
| | 1 | 254/228/190 | 0.074 | 8.65 % | 376 | 61/42/23 | 0.025 % | 3 |
| `mohdm1_dlight2` (4) | 0 | 228/218/165 | 0.518 | 48.2 % | 10316 | 21/15/7 | 44.4 % | 11354 |
| | 1 | 228/218/165 | 0.071 | 9.32 % | 162 | 2/2/0 | 3 pixels | 0 |
| `mohdm3_dlight` (2) | 0 | 220/193/140 | 0.147 | 20.3 % | 831 | 87/95/114 | 2.47 % | 210 |
| | 1 | 221/194/140 | 0.031 | 3.43 % | 84 | 87/95/114 | 0.31 % | 74 |

With `r_fastdlights 1` the frames are the previous build's to the bit (the
projected pass is unchanged) and GL1's at 4160x240 but for a few pixels;
the largest values at 640x480 and in `mohdm3` at 4160x240 are foliage
edges and the soldier's outline, which differ without dynamic lights too.
With `r_fastdlights 0` the differences are in the light pools (diff images
in the 2026-10-15 report): nearly all ±1 to ±3 where the falloff has no
texel steps any more, and bands a few pixels wide next to the lights where
GL1 normalized a saturated texel before filtering (up to 46 levels at
640x480 on the wall next to the light in `dl2_vault`, 21 at 4160x240), see
Differences from GL1. Against the previous Vulkan build (the same dynamic
lightmaps as GL1) `mohdm1_dlight` differs in 16 to 47 % of the pixels,
max 22 to 24, all in the light pools; views without dynamic lights are
the same to the bit (see below).

Against the previous build (`run.sh`, `tgadiff.py`, 640x480) on the 35
view lists plus `mohdm1_dlight`, `mohdm1_dlight2` and `mohdm3_dlight` with
`r_fastdlights 1` and `mohdm1_texenv` with `r_ext_texture_env_add 1` (163
views): 121 views give the same frames to the bit, all three lists with
`r_fastdlights 1` among them. Of the other 42, 10 are the views with
dynamic lights in `r_fastdlights 0`; the rest are the random content and
run-to-run noise named above: the smoke, fire and convoy of `m1l1`, the
camera shake of `m3l1a`, AI in `m1l2a` and `m1l3a`, the shutters of
`m5l1a`, the tank of `m5l2a`, snow in `m6l1a` and `mohdm5_fog`, and ±1
near the camera in `m2l1_skyportal`, `m33_grass`, `fm4_a`, `m61_slope`,
`d7_alley` and `d7_arch`; `m12_court`, `m12_lamp` and `m12_yard` (`m1l2a`,
±1 over a few hundred pixels) differ the same way between two runs of the
previous build. No validation message in any of these runs
(`r_vkValidation 1`), nor with synchronization validation (`r_vkValidation
2`) on the three dynamic light lists (both modes for `mohdm1_dlight`),
`mohdm1_dlight` with `r_ext_multisample 4`, `dm/mohdm1`, and a script that
loads `dm/mohdm1` with `cg_acidtrip 16`, turns `r_staticGeometry` and
`r_dlightBacks` off and on, runs `vid_restart` and loads `dm/mohdm3` and
`m3l3` with the same lights; the main menu matches GL1 (fan masked).

The skeletal models on the GPU (2026-10-16, see Implementation notes,
Skeletal models), against GL1 (`compare.sh`, `summary.py`): the 19
multiplayer and dm/obj lists of the earlier tables give their numbers
exactly at 640x480, and at 4160x240 too (`mohdm7`: 48/36/35, 0.057 %, 56,
the `d7_window` crack; `obj_team1` 0.149 % as in the second round); the
dynamic light lists those of their table. The new multiplayer lists (worst
view of each; the `_flash` views have the accepted per-pixel dynamic
light falloff, the others don't):

| list (views) | 640x480 max R/G/B | differing | > 3 | 4160x240 max R/G/B | differing | > 3 |
| --- | --- | --- | --- | --- | --- | --- |
| `mp_mohdm1` (3) | 253/228/189 | 16.0 % | 2514 | 25/18/9 | 27.4 % | 994 |
| `mp_mohdm2` (3) | 96/82/91 | 15.2 % | 2593 | 64/64/72 | 14.6 % | 2115 |
| `mp_mohdm7` (3) | 117/86/110 | 2.23 % | 102 | 2/1/1 | 0.054 % | 0 |
| `mp_obj_team1` (3) | 105/95/106 | 10.2 % | 1058 | 66/93/77 | 0.528 % | 179 |
| `mp_obj_team4` (3) | 93/91/103 | 22.7 % | 4066 | 54/44/31 | 17.0 % | 1004 |

Without dynamic lights (`_front`, `_side`) the 4160x240 frames differ
from GL1's in 2 to 243 pixels (nearly all by 1) where the soldiers
face the camera, and in 2324 to 8912 pixels (nearly all by 1) in three side
views that face a nearby wall (the clipping case of Comparing): the 20
skinned, lit soldiers of each view come out as GL1's.
Against the previous build (`run.sh`, `tgadiff.py`, 640x480) on the 41
view lists plus `mohdm1_dlight`, `mohdm1_dlight2` and `mohdm3_dlight` with
`r_fastdlights 1` and `mohdm1_texenv` with `r_ext_texture_env_add 1` (178
views): 142 views give the same frames to the bit, among them every view
of the multiplayer lists (but 1 pixel of `pm7_front`, which two runs of
one build show too), of `dm/mohdm1` to `dm/mohdm4`, `dm/mohdm6`,
`obj/obj_team1` to `obj/obj_team4`, the dynamic light lists (both modes),
`m2l3`, `m3l2`, `m5l3` but a particle, `m6l2a` and `m6l3a`. The other 36
are the random content and run-to-run noise named above: the smoke, fire
and convoy of `m1l1`, the camera shake of `m3l1a`, AI in `m1l3a`, the
shutters of `m5l1a`, the tank of `m5l2a`, snow in `m6l1a` and
`mohdm5_fog`, and the near-camera ±1 of `m2l1_skyportal`, `m33_grass`,
`fm4_a`/`fm4_b`, `m61_slope`, `d7_alley`, `d7_arch` and `m1l2a` (whose
`m12_*` views differ exactly the same way between two runs of the
previous build). The view weapon (`m2l3` with `cg_drawviewmodel 1`,
skinned and lit in the vertex shader) gives the same frames too, and so
does `mp_mohdm1` with `r_gpuSkinning 0` and 1. Coverage of these views (counted with
a temporary build, `mp_mohdm1` with the view weapon): 139 skinned
batches a frame and 2 left on the CPU (other shaders), surfaces at a
lower level of detail and at full detail, entities with head morphs,
meshes after the first (bone remap tables), light lists and constant
colors. No validation message in any of these runs (`r_vkValidation 1`),
nor with synchronization validation (`r_vkValidation 2`) on `mp_mohdm1`,
`mp_mohdm7`, `mohdm1_dlight2`, the view weapon test and a script that
spawns the soldiers of `mp_mohdm1`, turns `r_gpuSkinning` off and on,
runs `vid_restart` and loads `obj/obj_team4` with its soldiers (the skin
data cleared and filled again); the main menu matches GL1 (fan masked).

The GPU time changes of 2026-10-16 evening (alpha written by the
pipelines without blending, the depth image without a stencil plane when
`r_stencilbits` is 0, the fog color and `r_fastsky` clears drawn on
RADV; see Performance, GPU time at 3840x2160), against the build before
them (`run.sh`, `tgadiff.py`, 640x480) on the 41 view lists plus the
dynamic light lists with `r_fastdlights 1`, `mohdm1_texenv` with
`r_ext_texture_env_add 1`, `mohdm1` with `r_ext_multisample 4` and with
`r_stencilbits 8` (the stencil plane kept), `mohdm1` and `dm/mohdm3`
with `r_fastsky 1` (drawn clears) and `menus.txt` (208 views): 176 views
give the same frames to the bit, and the other 32 are the random content
and run-to-run noise named above (the smoke and convoy of `m1l1`, AI in
`m1l2a` and `m1l3a`, the camera shake of `m3l1a`, the shutters of
`m5l1a`, the tank of `m5l2a`, a particle in `m53_street`, snow in
`m6l1a` and `mohdm5_fog`, and ±1 near the camera in `s2_a`/`s2_b`,
`fm4_a`/`fm4_b`, `m61_slope`, `d7_alley` and `d7_arch`), among them the
`r_fastsky 1` views, whose color clears are all drawn now, and the fog
maps. No validation message in any of these runs (`r_vkValidation 1`),
nor with synchronization validation (`r_vkValidation 2`) on `mohdm1`
with `r_fastsky 1` (also with `r_ext_multisample 4`), `mohdm2_fog`,
`mohdm1_dlight`, `mohdm1` with `r_stencilbits 8` and `r_measureOverdraw
1` and with `r_measureOverdraw 1` alone (no stencil buffer: the warning,
like GL1), and a script that switches `r_stencilbits` 8 and 0,
`r_measureOverdraw` and `r_shadows 2`, and multisampling on and off with
`vid_restart` in `dm/mohdm1`, then loads `dm/mohdm2`. Against GL1
(`compare.sh`, `summary.py`), `dm/mohdm3`, `obj_team2`, `obj_team4`,
`mp_mohdm1` and `mp_obj_team4` give the numbers of the tables above at
640x480 and at 4160x240, `dm/mohdm1`, `mohdm2_fog` and `mohdm4_fog` the
same kind (at most 0.17 % of the pixels differ at 4160x240), and the
main menu matches GL1 (fan masked; `menus.txt` gives the numbers of its
paragraph above).

The world culling (2026-10-17, see Performance, World culling), against
the build before it (`run.sh`, `tgadiff.py`, 640x480) on the 14 `dm`, 5
`obj` and 5 multiplayer lists, `m2l1_skyportal`, `m2l3`, `m2l3_fog`,
`m3l3`, `m4l1_fog`, `m5l3`, `m6l1a` and `m6l2a`, the dynamic light lists
with `r_fastdlights 1`, `mohdm1_texenv` with `r_ext_texture_env_add 1`,
`obj_team2` and `dm/mohdm3` with `r_novis 1`, and `obj_team2` with
`r_nocull 1` and with `r_facePlaneCull 0` (170 views): 154 views give the
same frames to the bit, among them every view of `obj/obj_team1` to
`obj/obj_team4`, of the dynamic light lists in both modes, of the
multiplayer lists but 2 pixels of `pm7_front`, and of `m2l3`, `m3l3`,
`m6l2a`; the other 16 are the random content and run-to-run noise named
above (snow in `mohdm5_fog` and `m6l1a`, a particle in `m53_street`, and
near the camera `d7_alley`, `d7_arch`, `fm4_a`/`fm4_b`, `m61_slope`,
`pm7_front` and `s2_a`/`s2_b`, whose few larger pixels at the bottom edge
also differ between two runs of one build). Two views differed at first,
`dl_a` (`r_fastdlights 1`) and `dbg_normals` (see Performance, World
culling, change 1); they no longer cull faces there. Against GL1
(`compare.sh`, `summary.py`): `dm/mohdm3`, `dm/mohdm6`, `dm/mohdm7`,
`obj_team1` to `obj_team4`, `mp_mohdm1`, `mp_obj_team4` and
`mohdm1_dlight` give the numbers of the tables above at 640x480, and
`dm/mohdm3`, `dm/mohdm7`, `obj_team1`, `obj_team2`, `obj_team4`,
`mp_mohdm1` and `mp_obj_team4` at 4160x240 (`dm/mohdm7` 48/36/35,
0.070 %, 56: the `d7_window` crack; `obj_team1` 0.149 %); the main menu
matches GL1 (fan masked; `menus.txt` gives the numbers of its paragraph
above, 17 pixels off by 1 in `mn_video`, the views over `m1_spawn` that
view's differences, 5 to 7.4 %).
No validation message in these runs (`r_vkValidation 1`), nor with
synchronization validation (`r_vkValidation 2`) on `mohdm1_dlight` in
both modes, `m2l1_skyportal`, `obj_team2`, `mp_mohdm1` and a script that
loads `dm/mohdm1`, runs `vid_restart`, loads `obj/obj_team2`, turns
`r_novis`, `r_lockpvs` and `r_nocull` on and off, and loads `dm/mohdm3`;
`renderer_vulkan.so` imports no GL or Vulkan symbol and exports only
`GetRefAPI`.

Pipelines and skin data at load (2026-10-18, see Performance, Pipelines
and skin data at load), against the build before it (`run.sh`,
`tgadiff.py`, 640x480) on the 41 view lists (177 views): 146 give the
same frames to the bit, among them every view of the `dm`, `obj` and
multiplayer lists but the snow of `mohdm5_fog` and the near-camera ±1 of
`d7_alley` and `d7_arch`, of the menus, the dynamic light lists, the debug
drawing list, `m2l3`, `m3l2`, `m3l3`, `m5l3`, `m6l2a` and `m6l3a`. The
other 31 are in the 11 lists with random content or run-to-run noise named
above (`m1l1`, `m1l2a`, `m1l3a`, `m2l1_skyportal`, `m3l1a`, `m4l1_fog`,
`m5l1a`, `m5l2a`, `m6l1a`, `mohdm5_fog`, `mohdm7`): two runs of the build
before differ in 29 of the 43 views of those lists in the same way, and
the one view that differed only between the builds, `m13_sky`, differs by
a tracer the AI fired in one run (the rest of its frame is the same).
With `r_ext_multisample 4` (`mohdm1`, `mp_mohdm1`), `r_fastdlights 1` and
`r_dlightBacks 0` (the dynamic light lists), `r_vkFallbacks 3` (`mohdm1`,
`obj_team2`, `mp_mohdm1`: keys with the RGB bit of every unit, bindings
read per instance), `r_gpuSkinning 0` (`mp_mohdm1`, `mp_obj_team4`) and
`r_lightmap 1` (`mohdm1`: `TEXENV_REPLACE` with the RGB lightmaps) the 46
views are the same to the bit. No validation message in these runs
(`r_vkValidation 1`), nor with synchronization validation (`r_vkValidation
2`, and 1) in a script that starts the game, loads `obj/obj_team2`, runs
`vid_restart` in it, changes the mode to 800x600 with `vid_restart`,
loads `dm/mohdm1`, goes back to 640x480, disconnects, plays the `o2match`
demo and quits (the pipelines of each load built at its end, the unused
ones of the previous load destroyed, the keys file written at each
`vid_restart` and at shutdown and read again). Against GL1 (`compare.sh`,
`summary.py`): `obj_team2`, `mp_mohdm1`, `dm/mohdm3`, `obj_team4` and
`mp_obj_team4` give the numbers of the tables above at 640x480 and at
4160x240, `mohdm1_dlight` its known per-pixel light differences, and the
main menu matches GL1 (fan masked; `menus.txt`: 0 pixels in every menu
but the 17 of `mn_video`, the views over `m1_spawn` 5.2 to 7.4 %).
`renderer_vulkan.so` imports no GL or Vulkan symbol and exports only
`GetRefAPI`.

Skeletal culling (2026-10-19, see Performance, Skeletal culling), against
the verification build of that step (`run.sh`, `tgadiff.py`, 640x480, the
41 view lists, 177 views): that build draws every model as the build
before does and only adds the checks (the same frames as the build before
to the bit in `m3l3`, `mohdm1_debug`, `obj_team2` and `mp_mohdm1`); 144
views give the same frames to the bit, among them every view of the `dm`,
`obj` and multiplayer lists but the snow of `mohdm5_fog`, the near-camera
±1 of `d7_alley` and `d7_arch` and 2 pixels of `pm7_front`, of the
menus, the dynamic light, decal, fog and sun flare lists, `m2l3`, `m3l2`,
`m6l2a` and `m6l3a`. The others are in the lists with random content or
run-to-run noise named above (`m1l1`, `m1l2a`, `m1l3a`, `m2l1_skyportal`,
`m3l1a`, `m4l1_fog`, `m5l1a`, `m5l2a`, `m6l1a`, `mohdm5_fog`, `mohdm7`,
`pm7_front`, a particle in `m53_street`, one pixel of `m33_grass`, which
two runs of the build before also differ in), and `dbg_normals`
(`mohdm1_debug`, `r_shownormals`): it draws the normals of batches that
don't compute them, what earlier batches left in `tess.normal`, and a
culled model no longer leaves its own, so the cull is off with
`r_shownormals` (as world culling's is); with that the view is the same
to the bit. The final build against the build before on `m3l3`,
`mohdm1_debug`, `obj_team2` and `mp_mohdm1`: the same frames to the bit
(the four soldiers beside the camera of the `mp_*` front views are
culled). No validation message in these runs (`r_vkValidation 1`).

`m3l1a` is not in the table: the beach artillery shakes and rolls the
camera at random, so the two renderers see slightly different views of the
open sea after the two minutes in the landing craft (GL1's horizon was
tilted in the run made, 97 % of the pixels differ); `obj_team3_deepbluesea`
covers its sea shader. No difference needed a change to the renderer:
besides the causes above, the two views that looked like bugs were taken
from outside the world, and GL1 sometimes saved a black or stale frame
(see Deterministic 3D views).

Other things learned:

- `com_maxfps` caps the menus too (this note used to say the opposite;
  measured on 2026-10-17 with `libinlat.so`: the main menu presented 167
  frames a second at `com_maxfps 144` with the old frame limiter, 144 with
  the new one).
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
- `r_clear 1` clears only the scissor rectangle the last 2D draw left (in
  both renderers: `glClear` honours the scissor), a strip of the screen, so
  it doesn't show which pixels a frame leaves undrawn. `r_fastsky 1` does:
  it clears every view to (0.5, 0.5, 1) and skips the sky.
  `bspinfo.py <map> --view x y z` tells whether a view is inside the world
  before running it.
- `AMD_DEBUG=vs,ps,nir` (with `MESA_SHADER_CACHE_DISABLE=true`) dumps what
  radeonsi compiles for GL1's fixed-function pipeline and `RADV_DEBUG=shaders`
  what RADV compiles for ours, which is how the arithmetic of the vertex
  transform and of fog were matched. With Mesa 26 radeonsi prints the ISA
  only with `asm` added (`AMD_DEBUG=vs,ps,asm`). `RADV_DEBUG=img` and
  `AMD_DEBUG=tex` print the layout of every image (swizzle mode, DCC,
  HTILE, TC-compatible HTILE), which is how the depth buffer's compression
  mode was found (see Performance).

### Performance

`tools/perf.py` measures views with both renderers (see its docstring):

```sh
code/renderervk/tools/rtimer/build.sh      # renderer_timed.so, libstacks.so and libglts.so into build-vk
code/renderervk/tools/perf.py --list code/renderervk/tools/views/perf.txt \
    --sizes 640x480,1920x1080 --renderers opengl1,vulkan --runs 3 --json out.json
code/renderervk/tools/perf.py m6l1a 6152 -6528 5500 0 135 0 --runs 1 --stacks out
code/renderervk/tools/stacks.py out_m6l1a_640x480_vulkan_1.txt --tree RB_RenderDrawSurfList --callers CM_BoxTrace
```

Each measurement starts a game, loads the map with `fixedtime 16`, puts the
camera at the view, waits 2 s of game time and times 1000 frames with the
frame rate uncapped (`com_maxfps 0`, `r_swapInterval 0`, no validation).
A list line `views <view list> [shot ...]` takes shots of a view list,
with the commands that come before them in it (`views/perf_mp.txt`: the
soldiers of the multiplayer views, see Comparing).
The runs interleave the renderers (run i starts with renderer i), and the
summary gives medians with the lowest and highest. `--renderers` takes any
`renderer_<name>.so` of the binary directory: a copy of an older
`renderer_vulkan.so` under another name compares two builds in the same
batch, which is how every change below was measured. What it reports:

- frame time: wall clock time of the 1000 frames;
- renderer time, with `renderer_timed.so` (`tools/rtimer/rtimer.c`, built
  by `tools/rtimer/build.sh`; perf.py runs `cl_renderer timed` with
  `MOH_TIMED_RENDERER=<renderer>`): the main thread's time in the
  renderer's exports per frame, with a microsecond clock, for GL1 and
  Vulkan alike: `scene` (`RenderScene`, the front end), `2d` (`Set2DWindow`,
  `DrawStretchPic`, `DrawString`, ...: in OpenMoHAA they run the queued
  render commands, so this is where the 3D view's back end runs), `end`
  (`EndFrame`: the rest of the back end, swap or present), and `load`
  (time in the registration exports since startup);
- GPU busy (amdgpu's `gpu_busy_percent`) and the clock of the core running
  the main thread, sampled every 5 ms;
- for Vulkan, `vkinfo` after loading and around the measured frames: GPU
  time per frame from timestamps (and the copy to the swapchain image,
  when there is one), draws, render passes, vertex bytes, waits; pipelines,
  textures and allocations created while loading;
- how many CPUs other processes kept busy (from `/proc/stat` minus the
  game's CPU time); about one is the compositor showing a thousand frames
  a second, more is flagged. `--pause PID,...` stops other processes
  (SIGSTOP/SIGCONT) during each measurement, only for processes whose
  owner agreed.

`--stacks` preloads `libstacks.so` (`tools/rtimer/stacks.c`), which
interrupts the main thread every millisecond of wall clock time on average
and records its backtrace (the intervals are random between 0.5 and 1.5 ms:
at a fixed 1 ms the samples locked to the phase of frames capped at 1000
per second and always landed in the same part of the frame, which made
640x480 profiles show only the world traversal); `stacks.py` symbolizes
them (addr2line, with the library paths recorded in the file: keep a copy
of a `.so` that is rebuilt to read its old profiles) and prints self and
inclusive time per function, the call paths below one (`--focus`), its
callers (`--callers`) and the tree of what it calls (`--tree`). It needs
no privileges (perf is refused with the default
`kernel.perf_event_paranoid` of Ubuntu) and shows time spent waiting
(fences, the swap) where perf's CPU samples show nothing.

What makes simpler measurements misleading:

- `com_maxfps 0` meant at most 1000 frames per second until 2026-10-19:
  `Com_Frame` waited until a whole millisecond had passed since the last
  frame. On an idle machine most views reached that at 640x480 with both
  renderers, so the frame time showed no differences; the renderer time
  did. The tables below with 0.999 or 1.000 ms are from then. Since the
  command rate change (see Command rate) uncapped means uncapped, and
  `perf.py`'s uncapped frame times are the views' own.
- `com_speeds`' `rf` and `bk` are whole milliseconds, read right after the
  frame limiter's millisecond tick, and `bk` only counts the last batch of
  render commands of a frame; most of the back end runs before it, inside
  the UI's `Set2DWindow`. Both renderers were affected alike.
- The first numbers of this section (2026-10-14 morning) were taken while
  15 unrelated CPU-bound processes ran on the 16-core machine; they are
  replaced by the ones below. That run read a Vulkan front end of 1.15 ms
  against GL1's 0.84 ms in the `m6l1a` forest at 1080p: it was
  preemption and the `com_speeds` artifacts above; on the idle machine the
  front ends take 0.34 and 0.32 ms.

Results (2026-10-14 evening, idle machine: load average 0.4 to 1.8 with
the game itself, other processes 0.4 to 1.8 CPUs; AMD Radeon 780M, radeonsi
for GL1 and RADV, Mesa 26.0.8, windowed under Xwayland; medians of 3
interleaved runs, ms per frame). "before" is the renderer at the start of
the performance work (commit 41274ef3), "after" the current one; the GPU
columns are the main pass before and after the last change (the depth
image only readable while readbacks are wanted, see below):

| view | size | frame GL1 | frame before | frame after | renderer GL1 | renderer before | renderer after | GPU before last change | GPU after |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `m6l1a` forest `6152 -6528 5500 0 135 0` | 640x480 | 1.043 | 1.048 | 1.024 | 0.874 | 0.881 | 0.865 | 0.45 | 0.44 |
| | 1920x1080 | 1.333 | 1.578 | 1.298 | 1.139 | 1.359 | 1.097 | 1.41 | 1.23 |
| | 3840x2160 | 4.398 | | 4.084 | 3.969 | | 3.760 | 4.64 | 4.05 |
| `obj/obj_team2` yard `1744 2832 68 0 315 0` | 640x480 | 0.999 | 0.999 | 0.999 | 0.543 | 0.549 | 0.533 | 0.22 | 0.22 |
| | 1920x1080 | 0.999 | 1.000 | 0.999 | 0.638 | 0.623 | 0.556 | 0.62 | 0.62 |
| `dm/mohdm3` overview `3000 -2400 1500 50 0 0` | 640x480 | 0.999 | 0.999 | 0.999 | 0.635 | 0.610 | 0.566 | 0.29 | 0.27 |
| | 1920x1080 | 0.999 | 0.999 | 0.999 | 0.748 | 0.729 | 0.699 | 0.78 | 0.73 |
| `m3l3` forest path `-6800 3248 76 0 270 0` | 640x480 | 1.119 | 1.080 | 1.077 | 0.901 | 0.858 | 0.860 | 0.36 | 0.35 |
| | 1920x1080 | 1.306 | 1.273 | 1.213 | 1.006 | 0.987 | 0.936 | 1.02 | 0.93 |

(0.999 is the 1000 frames per second limit; GPU: from the start of the
frame's main command buffer to its end, timestamps; the 3840x2160 row is a
separate batch of 3 runs with GL1, the build before the last change and
the current one.)

The renderer time splits, at 1920x1080 after: `m6l1a` 0.343 front end,
0.674 2D calls with the 3D back end, 0.081 end of frame (GL1 0.319, 0.653,
0.162); `m3l3` 0.126, 0.737, 0.075 (GL1 0.129, 0.710, 0.165). The end of
frame is cheaper with Vulkan everywhere (no driver flush and swap), the
front end is the same code (equal within 3 %, except 7 to 8 % more in the
`m6l1a` forest and 7 % less in the `obj_team2` yard at 1080p, with the
same CPU clock: not explained; in the afternoon runs it was 8 % less in
the `dm/mohdm3` overview instead) and the back end is mostly the
renderer's own tessellation shared with GL1.

Profile of the `m6l1a` forest at 1920x1080, where Vulkan tied GL1 in the
first numbers and was 18 % slower on the idle machine (1.575 against 1.335
ms): Vulkan was GPU-bound. The GPU timestamps gave 1.545 ms of GPU work
per frame (1.41 main pass, 0.13 copy of the offscreen image to the
swapchain image), more than the frame's CPU work, and the CPU waited 0.22
ms per frame for the frame fence (inside `Set2DWindow`, where the back end
starts the frame). GL1 was CPU-bound. The stack samples (1 kHz, main
thread) put 0.33 to 0.35 ms per frame in the front end with both, 0.65 to
0.66 ms in the 3D back end (`RB_RenderDrawSurfList`) with both, of which
GL1's driver takes 0.27 ms on the main thread and Vulkan's `vk_draw` with
RADV 0.07 ms for 430 draws; the rest is the renderer's tessellation
(skeletal and static models, `ComputeColors`, terrain). The CPU clock was
the same (3.6 GHz). So the backend's own CPU cost was small and the plan
items that reduce it (vertex copies, render pass breaks) could not pay off
much; the GPU time could:

- the copy at present: `vkCmdCopyImage` costs the same as the blit; drawing
  into the swapchain image removed it (0.13 ms of GPU at 1080p, 0.47 ms at
  3840x2160): 1.575 to 1.451 ms per frame (635 to 689 fps), 5.15 to 4.69 ms
  at 3840x2160;
- the main pass: it took 6 to 7 % more GPU time than GL1's whole frame
  where both were GPU-bound (3840x2160: 4.63 ms against GL1's 4.39 ms
  frame; at 1080p with HiZ off, `RADV_DEBUG=nohiz`/`AMD_DEBUG=nohyperz`:
  2.08 against 1.94 ms), growing with the pixel count (0.23 ms at 320x240,
  0.41 at 640x480, 0.74 at 1280x720, 1.41 at 1080p, 4.63 at 3840x2160:
  about 0.25 ms plus 0.52 ms per megapixel). The cause was the depth
  buffer's compression mode; see below.

How the main pass was explained (2026-10-14 evening, same machine, idle;
`m6l1a` forest, medians of 2 or 3 runs):

- The shaders are not it. RADV's fragment shaders of the dominant
  pipelines (`MESA_SHADER_CACHE_DISABLE=true RADV_DEBUG=shaders`) are as
  short as radeonsi's fixed-function ones (`AMD_DEBUG=ps,vs,asm`: with Mesa
  26 nothing is printed without `asm`) or shorter. Both are ACO, wave64, in
  whole-quad mode for the derivatives and export FP16 color. The
  lightmapped fogged two-texture shader is 69 instructions; radeonsi's
  is a 19-instruction prolog (color interpolation), 80 in the main part
  (it divides the texture coordinates by q) and an 8-instruction epilog
  (its alpha test). The alpha-tested foliage shader demotes and leaves
  early like radeonsi's kill.
- Turning hardware features off in both drivers shows where the difference
  is (3840x2160, ms per frame, GL1 / Vulkan):

  | | GL1 | Vulkan |
  | --- | --- | --- |
  | as is | 4.40 | 4.69 |
  | primitive binning off (`AMD_DEBUG=nodpbb`, `RADV_DEBUG=nobinning`) | 4.86 | 5.85 |
  | DCC off (`nodcc`) | 4.80 | 5.85 |
  | binning and DCC off | 5.13 | 6.82 |
  | binning and HiZ off (`nohyperz`, `nohiz`) | 9.57 | 10.94 |

  Every feature that saves memory traffic hides part of the gap: Vulkan
  moved more data per pixel. (RADV's binning differs from radeonsi's too,
  128x128 bins where radeonsi uses 128x64 for depth-tested draws, see
  Plan, Remaining; with binning off in both the gap is larger, not
  smaller.)
- Not the color writes: writing alpha too (alpha 1, so the RGB write mask
  no longer makes partial writes) changed nothing, with binning off too
  (5.84 against 5.86 ms). Not the textures: RADV gives DCC only to color
  attachments larger than 512x512, radeonsi to textures too, but forcing
  it (`RADV_DEBUG=forcecompress`) saved only 0.02 to 0.03 ms. Not the
  memory: everything is in the 2 GB carve-out (`DEVICE_LOCAL`).
- By content (`r_drawstaticmodels 0`, `r_drawterrain 0`,
  `r_farplane_nofog 1`): the 2763 alpha-tested static models are about 80 %
  of the GPU time (0.81 ms left without them), the terrain 0.32 (GL1) and
  0.39 ms (Vulkan); without the farplane fog GL1 saves 0.44 ms and Vulkan
  0.09 ms (that includes the clear of the color buffer to the fog color,
  which RADV does as a comp-to-single DCC fast clear and which seems to
  cost GL1 more), so without it the gap was 15 %.
- The surfaces (`RADV_DEBUG=img`, `AMD_DEBUG=tex`): the color buffers are
  alike (64KB_R_X with DCC, BGRA8 against BGRX8), but GL1's depth buffer is
  Z24X8 with HTILE that isn't TC-compatible and ours was D32S8 with
  TC-compatible HTILE. RADV makes HTILE TC-compatible (the texture unit can
  read it, so copies and sampling need no decompression) for depth images
  with `SAMPLED`, `INPUT_ATTACHMENT` or `TRANSFER_SRC` usage, and ours had
  `TRANSFER_SRC` for depth and stencil readbacks. With TC-compatible HTILE,
  GFX9 and later keep an 8x8 depth tile compressed only while it holds at
  most 4 planes (`DECOMPRESS_ON_N_ZPLANES` 5 instead of 15,
  `ac_get_decompress_on_z_planes` in Mesa); the forest's small alpha-tested
  triangles put more planes than that in most tiles, which are then
  written and read uncompressed.

Without `TRANSFER_SRC` (the depth image is now only readable while
readbacks are wanted, see Implementation notes, Render target) the main
pass takes 12 to 13 % less GPU time and Vulkan is faster than GL1 where the
GPU limits:

Medians of 3 interleaved runs (`perf.py`, idle machine: load average 1.1
to 1.6 with the game, other processes 1.0 to 1.4 CPUs), ms per frame:

| `m6l1a` forest | GL1 | Vulkan before this change | after | GPU main pass before, after |
| --- | --- | --- | --- | --- |
| 1920x1080 | 1.370 (730 fps) | 1.455 (687 fps) | 1.328 (753 fps) | 1.412, 1.239 (-12 %) |
| 3840x2160 | 4.398 (227 fps) | 4.686 (213 fps) | 4.084 (245 fps) | 4.643, 4.046 (-13 %) |

At 1080p Vulkan is CPU-bound again (renderer time 1.115 ms against GL1's
1.152 and 1.236 before). In the other views of the results table the main
pass at 1080p went from 1.02 to 0.93 ms (`m3l3` path, -9 %), 0.78 to 0.73
(`dm/mohdm3` overview, -7 %) and stayed at 0.62 (`obj_team2` yard); their
frames are CPU-limited.

A depth format without the unused stencil plane (D32 instead of D32S8,
GL1 has no stencil either) saves 0.5 % more at 3840x2160 and nothing at
1080p after that; not done then, done since (see GPU time at 3840x2160).

Second round (2026-10-14 night): the renderer's CPU time. With the
depth image change the `m6l1a` forest is GPU-bound at 1080p, so CPU gains
there only turn into fence waits; the numbers below are the renderer time
minus `vkinfo`'s fence waits (`waitUs` per frame), at 1080p for the first
four changes and at 640x480 (no waits) for the last two, medians of 3
interleaved runs on the idle machine, each against the build before it, ms
of main-thread CPU time per frame in the `m6l1a` forest / `obj_team2` yard
/ `dm/mohdm3` overview / `m3l3` path. The stack samples put most of the
CPU time in code shared with GL1; every change keeps the frames
bit-identical (see Testing):

| change | why (stack samples) | ms per frame |
| --- | --- | --- |
| hidden symbol visibility: the library exports only `GetRefAPI` (`cmake/renderer_vulkan.cmake`) | with default visibility, calls between the renderer's own functions (1621 exported symbols) went through the PLT and could not be inlined, LTO or not (4034 PLT call sites in the library, 868 after; code 960 KB, 715 KB after, `size`) | -0.092 / -0.024 / -0.039 / -0.037 |
| static model culling without the model matrix: `R_AddStaticModelSurfaces` computed `R_RotateForStaticModel` (a 4x4 matrix product) for each of the 2763 static models of `m6l1a` every frame, though culling only needs the origin and axes; the matrix of the last one is still computed so `tr.ori` ends up the same | 9 % of the `m6l1a` frame | -0.060 / -0.016 / +0.006 / -0.004 |
| sun visibility traces kept by origin (`RB_Sphere_TraceSun`): the trace from an entity's lighting origin towards the sun only depends on the world, so its result is kept (1024 entries, keyed on the exact origin and sun direction, cleared at map load) for the entities that don't move | `CM_BoxTrace` for every lit entity and frame: 20 % of the `m3l3` frame, 6.5 % in `m6l1a`; most entities there move, the attached weapons share their owner's origin | -0.028 / -0.012 / -0.018 / -0.025 |
| radix sort of the draw surfaces: the four byte histograms counted in one pass, bytes that are the same in every key skipped (the same stable order) | `R_Radix` 9.7 % of the `obj_team2` frame | -0.030 / -0.012 / -0.007 / -0.014 |
| static model distance fade (`AGEN_DIST_FADE` in `ComputeColors`): the eye position in the model's axes computed once per batch instead of per vertex (the same operations) | 4.5 % of the `m6l1a` frame | -0.030 / -0.005 / -0.013 / -0.006 |
| static models culled from a compact array: their world-space sphere is computed at load with the same arithmetic, and the per-frame loop reads 24 bytes per model instead of the 1.3 KB `cStaticModelUnpacked_t` entries (one cache miss each) | `R_AddStaticModelSurfaces` 5.5 % of the `m6l1a` frame after the second change | -0.024 / -0.025 / -0.005 / -0.012 |

The result, against GL1 and the build before the second round (medians
of 3 interleaved runs, idle machine: load 1.0 to 1.4 with the game, other
processes 1.6 CPUs at most; frame time with the fps of the median, the
renderer's main-thread time without the fence waits, its front end, and
the main pass's GPU time; ms per frame):

| view | size | frame GL1 | frame before | frame after | renderer GL1 | before | after | front end GL1 / before / after | GPU before / after |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `m6l1a` forest | 1920x1080 | 1.323 (756 fps) | 1.301 (769) | 1.259 (794) | 1.129 | 1.071 | 0.811 | 0.320 / 0.340 / 0.189 | 1.235 / 1.235 |
| | 3840x2160 | 4.386 (228) | 4.081 (245) | 4.089 (245) | 3.990 | 1.374 | 1.079 | 0.429 / 0.466 / 0.283 | 4.037 / 4.045 |
| `obj_team2` yard | 1920x1080 | 0.999 | 0.999 | 0.999 | 0.618 | 0.546 | 0.482 | 0.143 / 0.137 / 0.109 | 0.622 / 0.622 |
| | 3840x2160 | 1.765 (567) | 1.763 (567) | 1.762 (568) | 1.608 | 0.704 | 0.652 | 0.172 / 0.163 / 0.137 | 1.742 / 1.740 |
| `dm/mohdm3` overview | 1920x1080 | 0.999 | 0.999 | 0.999 | 0.744 | 0.682 | 0.644 | 0.162 / 0.151 / 0.142 | 0.728 / 0.726 |
| | 3840x2160 | 2.460 (407) | 2.558 (391) | 2.563 (390) | 2.324 | 0.888 | 0.876 | 0.222 / 0.201 / 0.196 | 2.527 / 2.532 |
| `m3l3` path | 1920x1080 | 1.294 (773) | 1.246 (803) | 1.122 (891) | 0.995 | 0.935 | 0.849 | 0.128 / 0.129 / 0.105 | 0.933 / 0.936 |
| | 3840x2160 | 2.992 (334) | 3.099 (323) | 3.090 (324) | 2.495 | 1.183 | 1.068 | 0.166 / 0.167 / 0.143 | 3.060 / 3.051 |

(GL1's renderer time includes its waits for the GPU, in its swap; 0.999
is the 1000 frames per second limit. Where the GPU limits, at 3840x2160
and in the `m6l1a` forest at 1080p, the CPU savings become fence waits.)

Tried and not kept (no gain beyond the ±0.01 ms noise, or a cost): a
prefetch of the next draw surfaces in `RB_RenderDrawSurfList` (the world
surface data is mostly cache misses in `obj_team2`), the per-frame vertex
buffer in host-cached memory instead of write-combined device-local memory,
the depth format without the stencil plane (0.5 % less GPU time at
3840x2160, nothing at 1080p or with multisampling) and writing alpha
without multisampling (1.2 % less GPU time at 3840x2160 in `dm/mohdm3` and
`m3l3`, but 1 pixel in about 10^5 rounds one step lower, see State). Both
were taken up again with the GPU time at 3840x2160 (below): alpha is now
written by the pipelines without blending only, which keeps the frames.

What the profile shows after it (640x480, `m6l1a`, % of the 1 ms frame):
`vk_draw` 10 % for 340 draws (vertex and index copies about 4, RADV's
command recording 2, the backend's own bookkeeping 3), the renderer's
tessellation (static and skeletal models, terrain, `ComputeColors`) about
20, the sun traces of moving entities 3, `vk_end_frame` 7.5 (RADV's
submission ioctl and the sync objects of the X11 present and acquire), the
front end 14. Keeping static geometry in GPU buffers would save part of
the copies (about 0.03 ms), not worth its complexity on its own (it was
done since as the first step of a redesign, see Static geometry below:
what it saves is mostly the tessellation, not the copies).

Multisampling: the gap to GL1 was the color write mask. The pipelines
wrote RGB only (no destination alpha), and on the multisampled
DCC-compressed color image such a partial write costs: with 4 samples the
`m6l1a` forest took 2.887 ms of GPU time per frame, 2.729 ms when the
pipelines that write all of RGB write alpha too (1 without blending,
unchanged with it, see State), and 2.651 ms without the resolve at all
(the resolve into the swapchain image costs 0.26 ms; `vkCmdResolveImage`
or resolving into the offscreen image cost the same). Frame times, medians
of 3: GL1 2.699 ms, before 2.934, after 2.774 ms (3 % slower than GL1
instead of 9 %), the same frames as before with multisampling (the
`mohdm1` views). The depth
format without stencil changed nothing there (2.889 against 2.884 ms).

Static geometry (2026-10-15, step 1 of the redesign, see Implementation
notes): the world's faces, triangle surfaces and grids and the static
models are drawn from device memory instead of being tessellated into
`tess` and copied every frame. What it covers (`vkinfo` per frame at
640x480, `m6l1a` / `obj_team2` / `dm/mohdm3` / `m3l3`): 107 / 93 / 221 / 25
of 322 / 201 / 408 / 262 draws come from the static geometry, and the
vertex bytes copied go from 584 / 819 / 690 / 503 KB to 529 / 256 / 252 /
436 KB. There are as many draws as before (a merged batch still draws each
of its static models); there are fewer batches, the stage iterator runs:
88 static models in 3 batches in the `m6l1a` forest, 118 in 35 in the
`dm/mohdm3` overview. In the forests most static models still take the CPU
path (deforms, `alphaGen distFade`, see Plan, Remaining), and the terrain
and skeletal models are not part of this step, so `m6l1a` and `m3l3`
change little. Stack samples (640x480): `RB_SurfaceFace` went from 8.7 % to
1.0 % of the `obj_team2` frame (4.1 to 1.0 % in `dm/mohdm3`), `vk_draw`
below the lightmapped iterator from 6.8 to 4.0 %.

Against GL1 and the build before (2026-10-15 evening, idle machine: load
1.0 to 1.8 with the game, other processes 1.6 CPUs at most, 2.2 in one GL1
run at 3840x2160; medians of 3 interleaved runs; frame time with the fps
of the median, the renderer's main-thread time without the fence waits
(GL1's includes its waits in the swap), the main pass's GPU time; ms per
frame):

| view | size | frame GL1 | frame before | frame after | renderer GL1 | before | after | GPU before / after |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `m6l1a` forest | 640x480 | 1.049 (953 fps) | 1.000 | 1.000 | 0.881 | 0.648 | 0.640 | 0.447 / 0.440 |
| | 1920x1080 | 1.374 (728) | 1.255 (797) | 1.256 (796) | 1.153 | 0.827 | 0.824 | 1.229 / 1.228 |
| | 3840x2160 | 4.403 (227) | 4.091 (244) | 4.093 (244) | 3.994 | 1.139 | 1.118 | 4.043 / 4.044 |
| `obj_team2` yard | 640x480 | 1.000 | 1.000 | 1.000 | 0.562 | 0.487 | 0.405 | 0.216 / 0.207 |
| | 1920x1080 | 1.000 | 1.000 | 1.000 | 0.638 | 0.499 | 0.402 | 0.620 / 0.621 |
| | 3840x2160 | 1.766 (566) | 1.765 (567) | 1.755 (570) | 1.605 | 0.664 | 0.535 | 1.741 / 1.732 |
| `dm/mohdm3` overview | 640x480 | 1.000 | 1.000 | 1.000 | 0.627 | 0.546 | 0.443 | 0.270 / 0.283 |
| | 1920x1080 | 1.000 | 1.000 | 1.000 | 0.748 | 0.652 | 0.539 | 0.726 / 0.718 |
| | 3840x2160 | 2.469 (405) | 2.566 (390) | 2.556 (391) | 2.322 | 0.886 | 0.750 | 2.533 / 2.523 |
| `m3l3` path | 640x480 | 1.168 (856) | 1.007 (993) | 1.012 (988) | 0.929 | 0.763 | 0.769 | 0.339 / 0.361 |
| | 1920x1080 | 1.347 (742) | 1.132 (884) | 1.128 (887) | 1.018 | 0.863 | 0.847 | 0.930 / 0.929 |
| | 3840x2160 | 3.020 (331) | 3.094 (323) | 3.091 (323) | 2.472 | 1.092 | 1.119 | 3.053 / 3.050 |

(1.000 is the 1000 frames per second limit.) The renderer's CPU time goes
down 17 to 20 % in the `obj_team2` yard and 15 to 19 % in the `dm/mohdm3`
overview; in the forests the changes are within the noise (±0.03 ms). The
GPU time doesn't change: equal at 1920x1080 and 3840x2160, where it is
stable; at 640x480 the GPU is a fifth busy and its time varies by ±10 %
from run to run with either build (`m3l3`: 0.315 to 0.367 ms before, 0.361
to 0.366 after). Where the GPU limits (3840x2160, the forest at 1080p) the
frames take the same time as before.

Merged static model batches and grids, each measured against the same
build without it (640x480, 5 interleaved runs of 5000 frames, medians):
merging 0.643 against 0.657 ms in the `m6l1a` forest and 0.453 against
0.463 in the `dm/mohdm3` overview; grids 0.767 against 0.782 in the `m3l3`
path (2 % each; 3 runs of 1000 frames could not tell them from the noise).

Dynamic lights (2026-10-16, see Implementation notes, Dynamic lights):
`perf.py --exec 'cg_acidtrip N'` adds N random lights of radius 128 to
639 every frame (for timing only: they differ from run to run), on the
idle machine (load 0.5 to 2 with the game, other processes 1.7 CPUs at
most, but for bursts of up to 9.5 CPUs in a few 3840x2160 runs: the
medians below come from clean runs, and GL1's yard with 16 lights there
was measured again); medians of 3 interleaved runs, the renderer's
main-thread time without the fence waits, ms per frame. Each step against
the one before it, at 640x480 (no waits there):

| view | lights | GL1 | before | part 1: texel culling | part 2: per pixel | + box test of the texels | + lights culled per node (final) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `obj_team2` yard | 0 | 0.561 | 0.392 | | 0.398 | | |
| | 4 | 1.387 | 1.166 | 0.875 | 0.659 | 0.551 | 0.499 |
| | 16 | 2.209 | 2.008 | 1.952 | 1.234 | 0.833 | 0.684 |
| `dm/mohdm3` overview | 0 | 0.626 | 0.459 | | 0.441 | | |
| | 4 | 1.300 | 1.128 | 0.826 | 0.662 | 0.613 | 0.581 |
| | 16 | 2.072 | 1.927 | 1.559 | 1.031 | 0.836 | 0.796 |
| `m3l3` path | 0 | 0.912 | 0.763 | | 0.754 | | |
| | 4 | 1.046 | 0.887 | 0.867 | 0.787 | 0.788 | 0.764 |
| | 16 | 1.222 | 1.093 | 1.039 | 0.837 | 0.817 | 0.814 |

(GL1, before, part 1 and part 2 in one batch per light count; the box
test against part 2, the node culling against the box test, each in a
batch of its own. Without lights the last two do nothing.)

- Part 1 (exact: texels outside every light's reach skipped, faces whose
  texels no light reaches not allocated, see Testing) took 25 to 27 % off
  with 4 lights in the yard and the overview, 19 % in the overview with 16, but
  only 3 % in the yard with 16 lights of up to 639 units, where most
  texels are in reach; part 2 replaced it (its face test stayed).
- Part 2 removed the rest of the texel work, the uploads (94 KB per frame
  in `mohdm1_dlight`) and the lit surfaces' batches of their own: 41 to 43
  % less than before with 4 lights, 39 to 46 % with 16. The stack samples then put
  41 % of the yard's frame with 16 lights in `R_DlightReachesFace` and 11 %
  in the plane tests of `R_RealDlightFace`: every visible face tested every
  light, as GL1 did.
- The box of the texel centers, computed at load, rejects the faces far
  from a light before the exact test (the same masks): -16 % and -32 % in
  the yard with 4 and 16 lights, -9 and -19 % in the overview.
- The lights culled per BSP node (the same masks again): -9 % and -18 % in
  the yard, -5 % in the overview, -3 % in the `m3l3` path with 4.
- Dropped: letting the static models no light reaches keep their merged
  batches when lights are in view (`RB_StaticMerge`): no difference beyond
  the noise in these views (0.661 against 0.659, 0.659 against 0.662 ms...).

Against GL1 and the build before, at 1920x1080 and 3840x2160 (frame time
with the fps of the median, renderer time without fence waits, the main
pass's GPU time; ms per frame; final runs at 1080p and 4K with lights in
a batch with the build before, GL1 from the earlier batches):

| view | size | lights | frame GL1 | before | final | renderer GL1 | before | final | GPU before | final |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `obj_team2` yard | 1920x1080 | 0 | 0.999 | 0.999 | 0.999 | 0.625 | 0.391 | 0.401 | 0.617 | 0.620 |
| | | 4 | 1.454 (688 fps) | 1.191 (840) | 0.999 | 1.319 | 1.078 | 0.495 | 0.702 | 0.625 |
| | | 16 | 2.088 (479) | 1.788 (559) | 0.999 | 1.951 | 1.676 | 0.685 | 0.798 | 0.630 |
| | 3840x2160 | 0 | 1.843 (543) | 1.770 (565) | 1.753 (570) | 1.637 | 0.588 | 0.558 | 1.747 | 1.732 |
| | | 16 | 2.300 (435) | 1.959 (511) | 1.774 (564) | 2.128 | 1.809 | 0.771 | 1.753 | 1.751 |
| `dm/mohdm3` overview | 1920x1080 | 0 | 0.999 | 0.999 | 0.999 | 0.750 | 0.537 | 0.533 | 0.718 | 0.719 |
| | | 4 | 1.514 (661) | 1.291 (774) | 0.999 | 1.405 | 1.205 | 0.678 | 0.810 | 0.722 |
| | | 16 | 2.239 (447) | 2.054 (487) | 1.000 | 2.121 | 1.952 | 0.840 | 1.050 | 0.725 |
| | 3840x2160 | 0 | 2.469 (405) | 2.558 (391) | 2.569 (389) | 2.327 | 0.757 | 0.834 | 2.528 | 2.538 |
| | | 16 | 2.713 (369) | 2.645 (378) | 2.555 (391) | 2.552 | 2.192 | 1.028 | 2.568 | 2.524 |
| `m3l3` path | 1920x1080 | 0 | 1.367 (732) | 1.137 (879) | 1.169 (855) | 1.019 | 0.847 | 0.840 | 0.936 | 0.933 |
| | | 4 | 1.506 (664) | 1.220 (820) | 1.121 (892) | 1.162 | 0.941 | 0.859 | 0.935 | 0.931 |
| | | 16 | 1.616 (619) | 1.487 (672) | 1.198 (835) | 1.292 | 1.139 | 0.896 | 0.966 | 0.932 |

(0.999 is the 1000 frames per second limit. The CPU times of the
3840x2160 rows without lights come from the runs with bursts of load:
only their GPU times count. The no-light frame times of the `m3l3` path
include the game's own time, which varies more than the renderer's.)

The per-pixel lights cost no measurable GPU time: with 16 lights the main
pass takes 0.630 and 0.725 ms at 1080p against 0.620 and 0.719 without,
1.751 and 2.524 ms at 3840x2160 against 1.732 and 2.538. The dynamic
lightmaps cost 0.18 and 0.33 ms more at 1080p than no lights (the lit
surfaces in batches of their own, drawn from the CPU path, and the
uploads); the GPU time with lights now equals the GPU time without. Where
the CPU limited the frame, 16 lights now leave the yard and the overview at
the 1000 fps limit at 1080p (559 and 487 fps before, GL1 479 and 447), and
at 3840x2160 the yard runs at 564 fps against 511 (GL1 435).

What is left with 16 lights (stack samples, 640x480): the per-face tests
(`R_RealDlightFace`, its box and exact tests: about 19 % of the yard's
0.74 ms frame, the lights there being large and many faces in their
reach), and in the overview the per-vertex lighting of the static models
within twice a light's radius (`ComputeColors` and `RB_StaticMesh`, 13 %:
those models leave the static geometry), see Plan, Remaining. The
entities' lighting (`RB_Light_Real`) takes 0.3 %.

Skeletal models (2026-10-16, step 3, see Implementation notes, Skeletal
models). The views measured until then had few characters; in
multiplayer the players are most of the frame. On the multiplayer views
with 20 player models (`views/perf_mp.txt`: `pm1_front`, `po1_front`,
`po4_front` and `pm7_flash`, the last with three muzzle flash lights, see
Testing), the stack samples of the build before (640x480, % of the
frame; one run each) put the skinning (`RB_SkelMesh` and the functions
inlined into it) at 27 / 34 / 31 / 29 %, the per-vertex lighting
(`RB_Light_Real`) at 7 / 3 / 7 / 14 %, the copies of the skinned
vertexes into the frame's vertex buffer at 5 to 7 % (1.05 MB per frame
against 0.25 MB without the players), the sun visibility traces of the
entities (`RB_Sphere_TraceSun`, `CM_BoxTrace`) at 8 to 10 % and the
skeletons (`R_AddSkelSurfaces`, the engine's skeletor) at 6 to 9 %; the
renderer's time per frame was 1.0 to 1.4 ms there, against 0.40 to 0.47
in the `obj_team2` yard and the `dm/mohdm3` overview. Each change against
the build before it, renderer time (ms per frame, 640x480, where there
are no fence waits; medians of 3 interleaved runs, idle machine, other
processes 1.8 CPUs at most):

| change | `pm1_front` | `po1_front` | `po4_front` | `pm7_flash` | `obj_team2` yard | `dm/mohdm3` overview |
| --- | --- | --- | --- | --- | --- | --- |
| before | 1.336 | 1.042 | 1.208 | 1.421 | 0.398 | 0.474 |
| skinning and lighting in the vertex shader | 0.810 | 0.605 | 0.660 | 0.735 | 0.413 | 0.454 |
| sun traces kept for real (hash fix) | 0.642 | 0.476 | 0.548 | 0.596 | 0.401 | 0.453 |
| level of detail collapsed in the vertex shader | 0.650 → 0.571 | 0.471 → 0.397 | 0.554 → 0.467 | 0.597 → 0.511 | 0.402 → 0.385 | 0.450 → 0.448 |

(The first three rows in one batch, the last in a batch of its own with
the build of the row above.)

- The vertex shader does `RB_SkelMesh`'s and `RB_Light_Real`'s work with
  their floating point operations: 39 to 48 % less time in these views.
  The vertexes are no longer copied (497 against 1095 KB per frame in
  `pm1_front`); there are more draws (one per surface instead of one per
  batch: 556 against 497).
- The sun trace cache of the second round (`RB_Sphere_TraceSun`) hashed
  the bits of the origin with products by odd constants, which keep the
  low bits clear: an origin on whole or eighth units (map entities,
  network snapped positions) has many clear low mantissa bits, so every
  such entity went to the first of the 1024 slots and evicted the one
  before it, and all 36 traces of a `pm1_front` frame missed (counted
  with a temporary build). With the high bits mixed into the slot, the
  entities that don't move keep their trace: 17 to 21 % less. The
  soldiers of these views drift by tenths of a unit in the uncapped
  frames of `perf.py` (client interpolation), so about 15 of the 36 still
  miss there, as moving players do in a game: the trace of a moving
  entity is the engine's collision code (`CM_BoxTrace`, 5 to 8 % of these
  frames now), which only an approximation could avoid.
- The collapse of the vertexes at a lower level of detail (most of the
  surfaces of these views: `RB_SkelMesh` recomputed it every frame, 6 to 8
  % of the frame after the first change) is done by the vertex shader,
  which follows each vertex's collapse chain, and the draw takes a prefix
  of the surface's triangles found with a binary search (see
  Implementation notes, Skeletal models): 12 to 16 % less, and no indexes
  are copied any more.
- Tried and dropped: keeping the results of the visibility traces of
  `needs_trace` sphere lights (`RB_Sphere_AddLight`) by origin and light,
  like the sun's: no such light in these maps (no difference beyond the
  noise).

The result against GL1 and the build before (medians of 3 interleaved
runs, idle machine: others 2.0 CPUs at most, the one run at 2.0 being the
slowest of its median; frame time with the fps of the median, the
renderer's main-thread time without the fence waits (GL1's includes its
waits in the swap), the main pass's GPU time; ms per frame):

| view | size | frame GL1 | before | after | renderer GL1 | before | after | GPU before | after |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `pm1_front` | 640x480 | | 1.480 (676 fps) | 0.999 | | 1.320 | 0.564 | 0.480 | 0.548 |
| | 1920x1080 | 2.001 (500) | 1.630 (614) | 1.295 (772) | 1.768 | 1.432 | 0.701 | 1.239 | 1.268 |
| | 3840x2160 | 4.050 (247) | 4.093 (244) | 4.096 (244) | 3.709 | 1.946 | 0.977 | 4.045 | 4.056 |
| `po1_front` | 640x480 | | 1.140 (877) | 0.999 | | 1.004 | 0.385 | 0.301 | 0.408 |
| | 1920x1080 | 1.321 (757) | 1.193 (838) | 0.999 | 1.135 | 1.035 | 0.409 | 0.809 | 0.835 |
| | 3840x2160 | 2.736 (365) | 2.771 (361) | 2.783 (359) | 2.459 | 1.254 | 0.570 | 2.743 | 2.754 |
| `po4_front` | 640x480 | | 1.364 (733) | 0.999 | | 1.188 | 0.470 | 0.272 | 0.383 |
| | 1920x1080 | 1.529 (654) | 1.426 (702) | 0.999 | 1.315 | 1.206 | 0.521 | 0.891 | 0.812 |
| | 3840x2160 | 2.526 (396) | 2.600 (385) | 2.610 (383) | 2.154 | 1.407 | 0.673 | 2.574 | 2.586 |
| `pm7_flash` | 640x480 | | 1.577 (634) | 0.999 | | 1.413 | 0.504 | 0.275 | 0.391 |
| | 1920x1080 | 1.976 (506) | 1.534 (652) | 0.999 | 1.795 | 1.372 | 0.548 | 0.929 | 0.662 |
| | 3840x2160 | 2.289 (437) | 2.004 (499) | 2.000 (500) | 2.058 | 1.547 | 0.640 | 1.967 | 1.982 |
| `obj_team2` yard | 1920x1080 | 0.999 | 0.999 | 0.999 | 0.618 | 0.404 | 0.396 | 0.621 | 0.622 |
| | 3840x2160 | 1.765 (566) | 1.752 (571) | 1.750 (571) | 1.605 | 0.538 | 0.522 | 1.737 | 1.735 |
| `dm/mohdm3` overview | 1920x1080 | 0.999 | 0.999 | 0.999 | 0.756 | 0.540 | 0.545 | 0.719 | 0.718 |
| | 3840x2160 | 2.468 (405) | 2.554 (392) | 2.551 (392) | 2.322 | 0.729 | 0.738 | 2.530 | 2.527 |

(0.999 is the 1000 frames per second limit.) Where the GPU limits
(3840x2160) the frames take the same time as before and the GPU time is
within 1 % (+0.3 to +0.8 % in the soldier views, the same in the
others): the skinning costs the GPU next to nothing. At 640x480 and
1920x1080 the GPU is a quarter to two thirds busy and its timestamps vary
with its clock from run to run (up and down by up to 30 % here: they
rose at 640x480 while the frames came faster, fell in `pm7_flash` at
1080p), which a fixed cost per frame wouldn't do: at 3840x2160 it would
show as much. At 1920x1080 `pm1_front` is now GPU-bound (93 % busy).

With four random dynamic lights (`cg_acidtrip 4`, 1920x1080, same
batches): `pm1_front` 2.685 ms (373 fps) with GL1, 1.731 (578) before,
1.293 (773) after, renderer 2.476 / 1.529 / 0.779; `po4_front` 1.805
(554), 1.455 (687), 0.999 (1000 fps limit), renderer 1.582 / 1.238 /
0.549. The lights now cost the entities nothing but the sphere setup:
their lighting is in the vertex shader.

What is left in these views (stack samples, 640x480): the skeletons in
the engine (`R_AddEntitySurfaces`, 7 to 8 %), the sun traces of the
entities that move (5 to 8 %), the skinned draws (`RB_SkinDraw`, 3 %:
`vk_draw` per surface), the index prefixes (`RB_SkinSurface`, 1.3 %),
the world and the static models (`RB_StaticMesh` 4 % in `pm1_front`),
cgame (about 20 % of the wall clock time, outside the renderer) and the
end of the frame.

GPU time at 3840x2160 (2026-10-16 evening). Where the GPU limits, the
Vulkan renderer's main pass took 1 to 4 % more GPU time than GL1's whole
frame (`dm/mohdm3` overview 2.53 ms against 2.47, `po4_front` 2.59
against 2.53, see the tables above), about 0.08 ms per frame, which
turning content off didn't change (see Plan, Remaining, before this
work). GL1 has no GPU timestamps, so `tools/rtimer/glts.c`
(`libglts.so`, preloaded: `LD_PRELOAD=<bindir>/libglts.so
GLTS_OUT=<file> perf.py ... --renderers opengl1,...`) writes GL
timestamp queries right after each swap, before and after the first
depth clear of a frame (the 3D view's) and before the swap, and a
temporary build of the Vulkan renderer wrote timestamps at the same
points (bottom of the pipe, like GL's). Per frame, ms (medians of 2 or 3
runs; the timestamps themselves cost about 0.02 ms):

| view | | before the clear | the clear | clear to end of frame | end to next frame (swap, present) | per frame |
| --- | --- | --- | --- | --- | --- | --- |
| `dm/mohdm3` overview | GL1 | 0.000 | 0.266 | 2.190 | 0.043 | 2.499 |
| | Vulkan before | 0.004 | 0.280 | 2.262 | 0.039 | 2.585 |
| `obj_team2` yard | GL1 | 0.000 | 0.002 | 1.750 | 0.028 | 1.780 |
| | Vulkan before | 0.004 | 0.010 | 1.722 | 0.027 | 1.762 |
| `obj_team2` yard, `r_fastsky 1` | GL1 | 0.000 | 0.264 | 1.503 | 0.034 | 1.801 |
| | Vulkan, alpha written | 0.004 | 0.279 | 1.560 | 0.028 | 1.871 |

So the start and the end of the frame (the swapchain image's layout
transitions, the submission, the present) cost what GL1's swap costs,
and the yard was already faster than GL1. The difference is in the
drawing, and it is largest in the views that clear the color buffer (to
the fog color in `dm/mohdm3`, where the view holds no sky, and with
`r_fastsky 1`); GL1 draws its sky more slowly than Vulkan (`r_fastsky 1`
takes 0.25 ms of drawing off GL1's yard and about 0.13 off Vulkan's),
which hid the rest there. What it was, each measured on its own against
the build before it (medians of 3 interleaved runs, ms per frame, frame
time):

- Partial color writes. GL1's window buffer is BGRX8 (no alpha), so its
  RGB writes are writes of whole pixels; the swapchain image is BGRA8 and
  the pipelines wrote RGB only, partial writes, which the DCC-compressed
  target has to merge with the alpha it holds. Writing alpha, as 1, in
  the pipelines without blending: 2.560 to 2.533 in the overview (-1.1 %),
  1.757 to 1.727 in the yard (-1.7 %), 1.952 to 1.872 in the yard with
  `r_fastsky 1` (-4.1 %). Alpha written by the blending pipelines too
  (unchanged, factors 0 and 1) saves nothing more (2.532, 1.724) and
  changes the rounding of a few blended pixels by 1 (6 in `d3_street`, 15
  in `o2_pipe2`, both directions: the 1 in 10^5 of the second round), so
  they keep the RGB mask; with only the pipelines without blending the
  frames are the same to the bit.
- The stencil plane. D32S8 has one; GL1's depth buffer (Z24X8) has none.
  Without a stencil buffer (`r_stencilbits` 0, the default on Linux) the
  depth image is D32, whose HTILE holds depth only and whose depth clear
  is a fill instead of a compute pass that keeps the stencil bits (the
  clear took 0.010 ms in the yard against GL1's 0.002): with alpha written,
  2.531 to 2.516 in the overview, 1.727 to 1.719 in the yard (-0.5 %).
- The color clear. To a color with components other than 0 and 1, both
  drivers clear with DCC comp-to-single, a compute pass that writes the
  color into every 256-byte block: 0.266 ms with GL1, 0.280 with Vulkan
  (with the depth clear). A quad drawn over the view (writing alpha, so
  whole pixels) takes 0.247 ms: with the two changes above, 2.517 to
  2.500 in the overview, 1.855 to 1.837 in the yard with `r_fastsky 1`,
  3.019 to 2.984 in the `m3l3` path (fog color too), the yard without a
  color clear unchanged (1.715, 1.716). Done on RADV only (other drivers
  clear any color without such a pass, untested here) and without
  multisampling.
- Not tried, from RADV's source: the load and store operations of the
  render pass (`DONT_CARE` for depth at the end of the frame, for stencil
  throughout). `radv_cmd_buffer.c` never reads `storeOp` and
  `radv_meta_clear.c` only looks for `loadOp` `CLEAR` (Mesa 26.0.8), and
  the measured views have one render pass per frame.

What is left is the textures. radeonsi gives DCC to textures, RADV only
to images that can be color attachments and are larger than 512x512
(`radv_image_use_fast_clear_for_image_early`): after loading
`dm/mohdm3`, 711 of GL1's 940 tiled textures have DCC (`AMD_DEBUG=tex`;
128x128 ones, the size of the lightmaps, among them) and none of
Vulkan's 533 (`RADV_DEBUG=img`), which are all 512x512 or smaller. With
`RADV_DEBUG=forcecompress` (DCC for every image) the final build takes
2.438 ms in the overview (GL1 2.473), 1.687 in the yard (1.773) and
2.984 in the `m3l3` path (3.022): faster than GL1 in every view, 0.03 to
0.06 ms less than without it. The renderer can't get that: its textures
are below RADV's threshold whatever their usage flags. It would be a
change to RADV (compressing sampled images that copies write, as
radeonsi does); not done.

The result against GL1 and the build before (idle machine; medians of 3
interleaved runs; frame time with the fps of the median and the main
pass's GPU time; ms per frame; the frames are the same to the bit, see
Testing):

| view | size | frame GL1 | before | after | GPU before | after |
| --- | --- | --- | --- | --- | --- | --- |
| `dm/mohdm3` overview | 3840x2160 | 2.483 (403 fps) | 2.566 (390) | 2.497 (400) | 2.535 | 2.466 |
| `obj_team2` yard | 3840x2160 | 1.775 (563) | 1.777 (563) | 1.721 (581) | 1.749 | 1.699 |
| `obj_team2` yard, `r_fastsky 1` | 3840x2160 | 1.779 (562) | 1.952 (512) | 1.848 (541) | 1.929 | 1.825 |
| `m3l3` path | 3840x2160 | 3.062 (327) | 3.129 (320) | 3.017 (331) | 3.090 | 2.980 |
| `pm1_front` | 3840x2160 | 4.062 (246) | 4.121 (243) | 3.943 (254) | 4.073 | 3.897 |
| `po1_front` | 3840x2160 | 2.761 (362) | 2.803 (357) | 2.727 (367) | 2.768 | 2.693 |
| `po4_front` | 3840x2160 | 2.551 (392) | 2.626 (381) | 2.491 (401) | 2.595 | 2.461 |
| `pm7_flash` | 3840x2160 | 2.455 (407) | 2.016 (496) | 2.003 (499) | 1.991 | 1.978 |
| `dm/mohdm3` overview | 1920x1080 | | 0.999 | 0.999 | 0.722 | 0.700 |
| `obj_team2` yard | 1920x1080 | | 0.999 | 0.999 | 0.620 | 0.622 |
| `m3l3` path | 1920x1080 | | 0.999 | 0.998 | 0.943 | 0.921 |
| `pm1_front` | 1920x1080 | | 1.299 (770) | 1.271 (787) | 1.271 | 1.241 |
| `po1_front` | 1920x1080 | | 0.999 | 0.999 | 0.834 | 0.823 |
| `po4_front` | 1920x1080 | | 0.999 | 0.999 | 0.812 | 0.792 |
| `pm7_flash` | 1920x1080 | | 0.999 | 0.999 | 0.663 | 0.661 |

(0.999 is the 1000 frames per second limit; others 2.3 CPUs at most, in
one run of the build before in the yard, its slowest. GL1 varies more
from run to run in the `m3l3` path, 3.005 to 3.119 ms, and in
`pm7_flash`, where it is CPU-bound with the three lights.) At 3840x2160
the GPU time per frame goes down 2.7 to 5.4 % (0.7 % in `pm7_flash`),
and Vulkan's frames are faster than GL1's in every view but the
`dm/mohdm3` overview (0.6 % slower) and the yard with `r_fastsky 1` (3.9
% slower: GL1's slow sky is gone there); at 1920x1080, where these views
are CPU-bound but `pm1_front`, the GPU time goes down or stays within
±0.3 %, and `pm1_front` runs at 787 fps instead of 770.

The plan items, each measured before and after with interleaved builds:

| item | measured |
| --- | --- |
| depth image readable only while depth or stencil reads are wanted (new) | main pass -12/-13 % in the `m6l1a` forest at 1080p/3840x2160 (7 % less frame time than GL1 at 3840x2160, 3 % less at 1080p), -9 % `m3l3`, -7 % `dm/mohdm3`, unchanged in `obj_team2`; the same frames as the build before it (see Testing); multisampling unchanged |
| drawing into the swapchain image (new) | no copy: 8 to 9 % more fps where the GPU limits (above), 0.02 ms less CPU at the end of each frame; screenshots unchanged |
| frame pacing (acquire right after present) | with vsync (60 Hz, `m6l1a`): start of a frame's recording to its present 16.3 ms with the image acquired at the frame's first render pass (the earlier code waited in the acquire before its copy, the same), 1.7 ms after; frame interval p99/sd 18.4/0.60 ms at 1080p, GL1 18.3/0.61, the copy-based code 18.6/0.69 |
| texture memory sub-allocator | per map load, `m6l1a`/`obj_team2`/`dm/mohdm3`/`m3l3`: 317/524/557/526 device allocations become 10/11/11/11, time in `vkAllocateMemory` 7.3/12.2/13.5/11.9 ms becomes 0.3 ms, creating texture storages 8.6/14.5/16.3/14.6 ms becomes 1.5/2.5/3.0/2.7 ms, renderer load time 339/688/616/667 becomes 327/673/595/666 ms; per frame: no change (submissions 41 to 51 µs per frame either way) |
| pipeline cache on disk | 15 pipelines from startup to the first `m6l1a` view: 17.5 to 19.4 ms with no file and `MESA_SHADER_CACHE_DISABLE=true`, 0.2 ms with the file; with RADV's own cache 0.5 ms without the file. No pipeline was created during the measured frames |
| vertex copies of multi-pass shaders | vertex bytes per frame 588/826/716/509 KB become 584/819/690/503 KB; no measurable time |
| fewer render pass breaks | nothing to remove: 1.00 render pass and 1 submission per frame in the 4 views, in `mohdm1_dlight` (94 KB of dynamic lightmaps uploaded per frame, all before the first draw that uses them) and in the `m2l1` sky portal |
| `r_ext_multisample` | 4 samples, `m6l1a` at 1080p: 2.94 ms per frame against GL1's 2.71 ms (4.23 ms while the depth/stencil buffer was resolved with every render pass; 2.94 against 2.70 again after the depth image change; 2.77 against 2.70 with alpha written, second round) |

Multisampling matches GL1's (`r_ext_multisample 4` with both): in the
`mohdm1` views 9.1 % of the pixels differ, nearly all by 1, against 7.3 %
without multisampling; each renderer changes the same ~4 % of the pixels
(edges, max 112) when it is turned on.

Frame spikes (2026-10-16 night): what makes the slowest frames (the
99th and 99.9th percentiles of the frame time, the maximum), measured
before changing anything. A tester (NVIDIA RTX 4070 Ti, Linux/Wayland)
judges by these percentiles; the medians and means of the tables above
don't show them, and fixed views (`perf.py`'s usual views) hide what only
happens when things move: effects, players, sounds, models and textures
loaded on first use, pipelines created on first use.

The frame log of the Vulkan renderer records every frame with what
happened in it. Off by default; `r_vkFrameLog` is latched, so:

```sh
./openmohaa +set r_vkFrameLog 1 ...        # or in the console: r_vkFrameLog 1; vid_restart
```

then, in the console, `vkframelog clear` before what should be measured
(a match, a demo) and `vkframelog name.txt` after it (or only
`vkframelog`, which writes `vkframes.txt`): the file is written to
`<fs_homepath>/main/` (the game's writable directory: the console's
`path` command lists it first) and holds at most the last 131072 frames (`r_vkFrameLog 500000` keeps
more; 128 bytes per frame). The console shows the percentiles and the
slowest frames; the file is what to send. Its header has the device,
driver, present mode, size and these percentiles; then one line per
frame, in microseconds:

| columns | |
| --- | --- |
| `frame start interval` | index, start (since the log started), frame time: from this `BeginFrame` to the next |
| `begin scene 2d end load` | main thread time in `BeginFrame`, `RenderScene` (the front end), the 2D calls (in OpenMoHAA the queued render commands run there: the 3D view's back end, and the fence wait at the start of the frame), `EndFrame` (submission, present, the acquire of the next image) and the registration calls (models, shaders, skins, fonts loaded during the frame) |
| `cgame engine` | the rest of the frame: `cgame` from `BeginFrame` to `EndFrame` outside of the renderer's calls (the UI and cgame building the scene), `engine` from `EndFrame`'s return to the next `BeginFrame` (sound mixing, the frame limiter's sleep, input and network events, the local server's game frame, the client's packets) |
| `present_at` | when `vkQueuePresentKHR` was called (differences: present to present) |
| `fence flush submit acquire acquire_ahead present` | waits for the frame fence and for mid-frame flushes, `vkQueueSubmit`, `vkAcquireNextImageKHR` before the present (normally 0) and right after it, `vkQueuePresentKHR` |
| `gpu gpu_start gpu_end` | GPU time of the main command buffer, and when it started and ended on the GPU (since the first timestamp; 0: none): `gpu_end` differences are the GPU's frame to frame time, `gpu_start` minus the previous `gpu_end` the GPU's idle time |
| `draws render_passes submits vertex_bytes staging_bytes` | per frame |
| `pipelines pipeline_us textures texture_us allocations allocation_us flushes vertex_growth depth_targets swapchains` | what was created in the frame and the time it took: pipelines, texture storages, device memory, mid-frame flushes, a frame vertex buffer replaced by a larger one (its size), the depth image replaced, a swapchain created |
| `skin_uploads skin_bytes` | skin data of skeletal models added in the frame (`vk_add_skin_data` calls and bytes; GPU skinning, see Implementation notes, Skeletal models) |

`tools/framelog.py file` prints the distributions and the causes of the
slow frames (below). The log changes nothing of what the frames do or
draw (the same submissions, presents and acquires, in the same order) and
costs nothing when off: `GetRefAPI` returns the exports themselves then.
While it is on, its timing costs about 15 µs per frame on this machine
(two clock reads around each of the HUD's ~300 2D calls).

The same split of the frame for GL1: `renderer_timed.so` (`rtimer.c`)
writes every frame it measures to `$RTIMER_FRAMES` with the first columns
of the log, measured the same way, and `libglts.so` (`glts.c`) writes GL1's
GPU timestamps of every frame to `$GLTS_FRAMES`. `perf.py --framelog DIR`
keeps them for every run (and the Vulkan log for the renderers named by
`--vklog`), `framelog.py --batch DIR` compares them.

Engine time: the renderer only sees its own calls, so what isn't in them
is split by where it happens, `cgame` inside the frame and `engine`
between frames, without touching the engine. Within a frame below the
1000 frames per second limit, `engine` also holds the frame limiter's
sleep (`Com_Frame` waits for the next whole millisecond). For the details
of slow frames, `framelog.py --stacks samples.txt frames.rt.txt` puts the
stack samples of `libstacks.so` (`perf.py --stacks` in the same run;
both use `CLOCK_MONOTONIC`) into the frames of `renderer_timed.so`'s file
and lists the functions by their share of the slow frames' samples.

Moving content that both renderers can replay: demos. The client plays
`.dm_8` demos (`demo name`), and the original game's `.dm3` demos (protocol
8, the same format) through a `.dm_8` link. With `fixedtime 16` every
frame advances the demo by 16 ms whatever the frame took, so frame k shows
the same moment in every run and with both renderers (the same frames to
compare one by one), like a timedemo that interpolates between snapshots.
`views/perf_demo.txt` lists two matches recorded by original game clients
(files of the dev machine): `o2match` (`obj/obj_team2`, 256 s, the
recorder's own view, 52 kills) and `o4match` (`obj/obj_team4`, 246 s, 23
kills), 15000 frames each (240 s of the match) from the first frame after
loading. Bots (`sv_maxbots`, `addbot`) move and shoot but are random from
run to run (the game seeds `rand()` with the clock); they are a way to
record a demo, not to measure.

Results (2026-10-16 night, idle machine: others 1.6 CPUs at most but in
one GL1 run of the static views, 2.1, my analysis running beside it;
medians of 3 interleaved runs; frame time from `renderer_timed.so`, ms;
Vulkan without the frame log, whose runs gave the same within the run to
run spread). The demos, 15000 frames each, warm caches (the pipeline cache
file and Mesa's shader cache filled by earlier runs, as on a second
play):

| demo | size | renderer | mean (fps) | median | p99 (fps) | p99.9 (fps) | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `o2match` | 640x480 | GL1 | 1.050 (952) | 1.000 | 1.736 (576) | 2.258 (443) | 25.7 |
| | | Vulkan | 1.003 (997) | 1.000 | 1.039 (963) | 1.442 (693) | 20.9 |
| | 1920x1080 | GL1 | 1.075 (930) | 1.000 | 1.920 (521) | 2.646 (378) | 25.8 |
| | | Vulkan | 1.003 (997) | 1.000 | 1.175 (851) | 1.560 (641) | 21.3 |
| | 3840x2160 | GL1 | 1.416 (706) | 1.243 | 3.122 (320) | 3.810 (262) | 26.3 |
| | | Vulkan | 1.329 (753) | 1.130 | 3.068 (326) | 3.690 (271) | 23.3 |
| `o4match` | 640x480 | GL1 | 1.385 (722) | 1.348 | 2.336 (428) | 3.289 (304) | 31.1 |
| | | Vulkan | 1.037 (965) | 1.002 | 1.415 (707) | 1.821 (549) | 27.6 |
| | 1920x1080 | GL1 | 1.532 (653) | 1.513 | 2.706 (370) | 3.518 (284) | 31.6 |
| | | Vulkan | 1.168 (856) | 1.081 | 1.953 (512) | 2.693 (371) | 28.1 |
| | 3840x2160 | GL1 | 2.368 (422) | 2.273 | 3.999 (250) | 5.091 (196) | 32.3 |
| | | Vulkan | 2.241 (446) | 2.167 | 3.671 (272) | 4.950 (202) | 30.5 |

(1.000 is the 1000 frames per second limit of `Com_Frame`; the tester's
client has none, ours puts the median of the lighter frames there.) In the
matches Vulkan's 99th and 99.9th percentiles are lower than GL1's at every
size, by 0.6 to 0.9 ms at 640x480 and 1920x1080 (where GL1's CPU time per
frame limits) and 0.1 to 0.3 ms at 3840x2160 (where the GPU does), and so
is the maximum.

What makes the slow frames slow. The demo's frames are the same moments
in every run, so each frame's median over the 3 runs is what the content
costs, and a run's frame minus that median is noise of that run (the
system, the driver, the clock): `framelog.py --batch` splits them. The
noise is lower with Vulkan in every demo and size (99.9th percentile of a
frame's excess over its median: 0.13 against 0.65 ms at 640x480 in
`o2match`, 0.90 to 1.20 against 1.44 ms at 1920x1080 in `o4match`, 1.70 to
1.86 against 2.26 at 3840x2160). The content's slow frames (the medians
above their 99th percentile), split into parts that don't overlap, each
part's excess over its median summed (Vulkan's frame log; at 1920x1080 from
a run of the fixed log, see below):

| demo, size | front end (`scene`) | back end | cgame | registration (`load`) | fence wait | engine | pipelines |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `o2match` 1920x1080 | 19 % | 20 % | 21 % | 28 % | 6 % | 4 % | 1 % |
| `o4match` 1920x1080 | 38 % | 27 % | 15 % | 13 % | 0 | 5 % | 0 |
| `o2match` 3840x2160 | 10 % | 10 % | 10 % | 15 % | 53 % | 0 | 0 |
| `o4match` 3840x2160 | 19 % | 14 % | 10 % | 15 % | 37 % | 5 % | 0 |

(The logger of these batch runs timed nested registration calls twice;
fixed, the 1920x1080 rows come from the run with the stack samples below,
the 3840x2160 ones are as measured, their `load` share a little high.)

- The heavier moments of the match (more players, effects, sprites and
  terrain in view: the front end, the back end and cgame) make most of the
  99th percentile with both renderers; Vulkan's are smaller (GL1's front end
  and back end cost more per frame). At 3840x2160 it is the GPU: the fence
  wait of the frames whose drawing takes the GPU longer (the main pass
  takes 1.12 ms in the median `o2match` frame and 2.96 ms at its 99th
  percentile).
- Registration in the middle of the match makes the largest frames (21 to
  31 ms, the maximum of both renderers, GL1's larger). The stack samples
  of `libstacks.so`, put into the frames (`framelog.py --stacks`, one run
  per renderer at 1920x1080), show what: a configstring announces a model
  (a player's model or weapon, `CG_ProcessConfigString`,
  `RegisterServerModel`), cgame caches its resources (`CacheResource`)
  and its shaders load their images from the pk3s (`R_FindShader`,
  `LoadTGA`/`LoadJPG`, the zlib inflate, `R_MipMap`), 22 textures and 6.7
  MB in the worst frame of `o2match`; the file system's case correction
  (`FS_CorrectCase`, `opendir`/`readdir` of the directories) is part of
  it. Code shared by both renderers.
- The skin data of GPU skinning (Vulkan only): a skeletal model's first
  draw builds and uploads its skin data (`R_SkinAddSurface`,
  `R_SkinThresholds`, 429 KB for a player model), 0.6 ms more than GL1 in
  that frame (`o2match` frame 142, every run), once per model and map
  (since 2026-10-18 when the model is registered, see Pipelines and skin
  data at load).
- `engine` in `o4match`'s round changes (frames 4451, 7055, 11008, 13483:
  +1.2 ms, with +1.4 ms of cgame): a new soundtrack is loaded and decoded
  on the main thread (`MUSIC_NewSoundtrack`, `S_CodecLoad`, MP3 decoding,
  even with the music volume at 0) along with large configstrings.
- Pipelines: with warm caches the same ones are created in every run (a
  process creates its pipelines on first use, the caches make that cheap):
  19 or 38 in the first frames after the load and 11 or 20 in 10 or 11
  frames of the match, 0.6 to 1.5 ms in all.

Cold caches (the first play after an install or a driver update: no
`vkpipelines.cache`, `MESA_SHADER_CACHE_DISABLE=true` for both renderers,
`perf.py --cold`), 1920x1080, medians of 3:

| demo | renderer | mean (fps) | p99 (fps) | p99.9 (fps) | max | warm p99.9, max |
| --- | --- | --- | --- | --- | --- | --- |
| `o2match` | GL1 | 1.073 (932) | 1.941 (515) | 2.684 (373) | 25.3 | 2.646, 25.8 |
| | Vulkan | 1.007 (993) | 1.191 (840) | 2.088 (479) | 33.3 | 1.560, 21.3 |
| `o4match` | GL1 | 1.548 (646) | 2.737 (365) | 3.756 (266) | 31.8 | 3.518, 31.6 |
| | Vulkan | 1.157 (864) | 1.893 (528) | 3.516 (284) | 58.1 | 2.693, 28.1 |

Besides the 7 pipelines of the loading (11 ms), Vulkan creates 17 to 38
pipelines in the first frames after the load (25 to 52 ms: the 58 ms
frame of `o4match`) and 11 to 22 more during the match, in 10 to 12
frames, 1.6 to 2.4 ms each and up to 7 ms in one frame; the pipelines are
then 30 to 34 % of the content's slow frames, and Vulkan's 99.9th
percentile comes near GL1's. GL1's changes by 0.04 to 0.24 ms. (Since
2026-10-18 these pipelines are created at load: see Pipelines and skin data
at load below.)

Fixed views (no movement, 5000 frames, medians of 3; the GPU's frame to
frame time: Vulkan's `gpu_end` differences from the frame log, GL1's from
`libglts.so` in 2 runs of their own):

| view | size | renderer | median | p99 | p99.9 | max | GPU frame to frame p99, p99.9 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `obj_team2` yard | 1920x1080 | GL1 | 0.999 | 1.195 | 1.627 | 1.925 | |
| | | Vulkan | 0.999 | 1.117 | 1.183 | 1.221 | |
| | 3840x2160 | GL1 | 1.757 | 2.107 | 2.455 | 2.807 | 1.942 to 1.958, 2.306 to 2.703 |
| | | Vulkan | 1.701 | 2.500 | 2.885 | 3.579 | 1.862, 1.928 |
| `dm/mohdm3` overview | 1920x1080 | GL1 | 0.999 | 1.395 | 1.845 | 3.385 | |
| | | Vulkan | 0.999 | 1.069 | 1.146 | 1.269 | |
| | 3840x2160 | GL1 | 2.457 | 2.798 | 3.275 | 3.682 | 2.707, 2.881 to 3.407 |
| | | Vulkan | 2.476 | 3.019 | 3.484 | 3.667 | 2.701, 2.872 |
| `pm1_front` | 1920x1080 | GL1 | 1.848 | 2.876 | 3.345 | 3.778 | |
| | | Vulkan | 1.250 | 1.737 | 2.059 | 2.619 | |
| | 3840x2160 | GL1 | 4.020 | 4.690 | 5.333 | 5.897 | 5.325, 5.967 (1 run) |
| | | Vulkan | 3.931 | 5.039 | 5.628 | 6.126 | 4.192, 4.345 |

The build with the frame log and the build before it, logger off, give
the same frame times (0.002 ms at most between their medians, renderer
time within 0.013 ms); with the log on the renderer time is up to 0.02 ms
higher at 640x480 and 1920x1080 and the same within the noise at
3840x2160. Without `com_speeds 1` (perf.py no longer prints it when
`renderer_timed.so` measures), the spread of the 3840x2160 views is about
what the earlier runs with it gave (the overview's 99th percentile 3.02 to
3.20 ms against 3.24 then, GL1 2.80 against 2.86).

At 3840x2160, where the GPU limits, Vulkan's frame times spread more than
GL1's (99th percentile 0.2 to 0.4 ms higher) but the GPU finishes its
frames as evenly as GL1's or more (the GPU frame to frame times above):
what reaches the screen is as even, the spread is in when the main thread
starts its frames. Two causes, from the frame log:

- Where the wait is. Vulkan waits for the fence of frame N-2 at the start
  of frame N's back end, which in OpenMoHAA is the first UI widget's
  `set2D()` (`UIWidget::Display` runs the queued `RB_DrawBuffer`), before
  `View3D::Draw` runs cgame and `RenderScene`. Time those take beyond
  their median lengthens that frame and shortens the next frame's wait (a
  long interval, then a short one). GL1 waits in its swap, after the
  frame's work, which absorbs it in the same frame. The slow frames of the
  fixed views keep a fence wait near its median (1.6 ms in the overview)
  and add front end, back end and engine time above their medians.
- `vkAcquireNextImageKHR` right after the present waits 0.6 to 1 ms in 4 %
  of the yard's frames (215 of 4998; 12 in the overview), when the X
  server (Xwayland) has not released an image yet; the fence wait is that
  much shorter in those frames.

In the moving demos the GPU's load changes from frame to frame and
Vulkan's 99th percentile is still below GL1's at 3840x2160.

Checks of the log itself: against the build before it (`run.sh`,
`tgadiff.py`, 640x480), `dm/mohdm1`, `obj/obj_team2`, `mohdm1_dlight` and
`mp_mohdm1` (17 views) give the same frames to the bit with
`r_vkFrameLog` 0 and 1; no validation message in these runs
(`r_vkValidation 1`), nor with synchronization validation on
`mohdm1_dlight` with the log on, nor in a demo played with the log on
through `vkframelog`, `vid_restart` (the log starts again), `vkframelog`
twice (the second has no frames), `vkframelog clear` and `vkframelog`;
`renderer_vulkan.so` still imports no GL or Vulkan symbol and exports
only `GetRefAPI`.

Frame limiter (2026-10-17; an engine change, `Com_Frame` in
`code/qcommon/common.c`, shared by both renderers): `com_maxfps` was
applied in whole milliseconds. `Com_Frame` waited until
`Sys_Milliseconds` had passed the last frame's start by `1000 /
com_maxfps`, an integer, less a `bias` that took the previous frame's
lateness off, so caps that don't divide 1000 were wrong (144 and 165 ran
at 167 frames per second, 240 at 250, 334 to 499 at 500: players who cap
just below their display's refresh rate for VRR got more), and frames
started on the millisecond tick.

Now a cap is a deadline in microseconds (`Sys_Microseconds`:
`CLOCK_MONOTONIC`, `QueryPerformanceCounter` on Windows) that advances by
exactly one period (1000000 / cap, rounded up so that the rate never
exceeds the cap; a cap of 1000 or more had no deadline, only the
millisecond below, like uncapped, until the command rate change of
2026-10-19 (see Command rate: now 1000 waits for the millisecond, a cap
above it for its deadline alone, uncapped for nothing); a cap that divides 1000 has its deadline
in whole milliseconds since the change described at the end of this part,
Caps that divide 1000). A frame late by less than a period is
made up by the next one; after a later one the deadline starts again from
now (no burst of frames to catch up). As before, a frame also
waits until `Sys_Milliseconds` has passed the previous frame's start
(`com_frameTime`) by `minMsec`: 1 ms for a client, so that no frame lasts
0 ms of game time (`Com_ModifyMsec` would make it 1 ms and the game would
run ahead of real time) and uncapped still means at most 1000 frames per
second (both changed on 2026-10-19, see Command rate); a dedicated server's frame (`SV_FrameMsec`). The game time of a
frame alternates where the period isn't whole milliseconds (6 and 7 ms at
144). `com_timedemo`, `com_busyWait`, `com_maxfpsUnfocused`,
`com_maxfpsMinimized` and the local server's queued packets
(`SV_SendQueuedPackets`) work as before. The order of a frame is the same:
the wait, then the input (`IN_Frame`), the events, the server and client
frames; nothing waits after the input is read, and the renderer has no
part in it (no frame is held before its present).

The wait (`Com_FrameWait`) sleeps in `select` on the network sockets
(`NET_SleepUs`: packets are still read as they come) until 1 ms before the
deadline and polls the network for the rest, as the old code did for the
last millisecond before the tick. The millisecond (`Sys_Milliseconds`)
condition is waited for as before. Sleeps end late, by more when the core
had the time to reach a deep idle state; on this machine (Ryzen 7 PRO
8700GE, `amd-pstate-epp`, the menu idle governor; C1 1 µs exit latency, C2
18 µs, C3 350 µs with 700 µs target residency) a `select` on a UDP socket,
idle machine, 2000 sleeps each, woke up this late (µs):

| sleep asked | timer slack 50 µs (default): median, p99, p99.9, max | timer slack 1 µs |
| --- | --- | --- |
| 200 µs | 55, 80, 90, 251 | 6, 31, 38, 45 |
| 1 ms | 56, 174, 517, 579 | 7, 62, 429, 466 |
| 3 ms | 81, 667, 854, 907 | 35, 576, 741, 773 |
| 6 ms | 83, 625, 814, 850 | 45, 641, 798, 813 |

The timer slack is now 1 µs on Linux (`PR_SET_TIMERSLACK` in
`Sys_PlatformInit`; threads created after inherit it). The long sleeps'
late wake-ups (C3) are why the last millisecond is polled. Shorter sleeps
in that millisecond instead were tried, in a simulation (a loop with 1 ms
of work per frame, 10000 frames at 144 frames per second: sleeps of 100 µs
and polling the last 150 µs started the frames as late as polling at the
99th percentile, 4 µs, but 0.16 ms at the 99.9th against 0.02, for 2.8 %
of a core instead of 13 %) and in the game, where they cost latency:
the core clocks down when it sleeps more, and the frame's work, from its
input read to its present, takes longer. `o2match` (see below) at
1920x1080 with Vulkan, medians of 3 runs, in three batches (`libinlat.so`,
below; CPU: the game's, in CPUs):

| the wait's last millisecond | cap 144: input to present median, CPU, core clock | cap 240 |
| --- | --- | --- |
| old limiter (167 and 250 frames per second) | 0.685 to 0.696 ms, 0.20, 3191 to 3203 MHz | 0.616 to 0.631 ms, 0.27, 3271 to 3280 MHz |
| polled (kept) | 0.666 to 0.671 ms, 0.27, 3247 to 3267 MHz | 0.578 to 0.586 ms, 0.42, 3345 to 3354 MHz |
| sleeps of 100 µs, the last 150 µs polled | 0.713 to 0.723 ms, 0.16, 3185 to 3193 MHz | 0.619 to 0.631 ms, 0.24, 3211 to 3217 MHz |
| sleeps of 30 µs (no C2) | 0.702 ms, 0.18, 3227 MHz | 0.621 ms, 0.26, 3262 MHz |
| sleeps, the last 500 µs polled | 0.720 ms, 0.21, 3217 MHz | 0.619 ms, 0.31, 3270 MHz |
| sleeps, the last 300 µs polled | 0.729 ms, 0.18, 3198 MHz | 0.621 ms, 0.26, 3226 MHz |

Only polling the whole millisecond made the input to present time shorter
than the old limiter's (by 0.02 to 0.05 ms); the sleeps saved CPU but
made it up to 0.03 ms longer at 144. The polling costs no measurable GPU
time on this APU, whose CPU and GPU share a power budget: at 3840x2160 and cap 144 the
GPU's main pass took 2.17 to 2.20 ms per frame with it, 2.16 to 2.18 with
the old limiter and 2.00 to 2.23 with 30 µs sleeps (input to present
0.652, 0.676 and 0.686 ms). On Windows (untested: the CI builds it, this
machine can't run it) a sleep lasts whole timer ticks, 1 ms at best
(`Sys_PlatformInit` asks for 1 ms with `timeBeginPeriod`), so the wait
sleeps whole milliseconds while more than 2 ms remain and polls the rest;
`Sys_Microseconds` takes the whole seconds and the rest of
`QueryPerformanceCounter` apart (the counter times a million overflows
after a few days of uptime).

Results (2026-10-17, idle machine: other processes 1.5 CPUs at most, load
average under 2 with the game; 1920x1080 under Xwayland, the display at
59.96 Hz; medians of 3 interleaved runs of 10 s with each renderer and the
engine before ("old", left of the arrow) and after this change: `perf.py
--binaries openmohaa_before,openmohaa --caps 60,125,144,165,240,333,500,1000,0
--seconds 10 --preload libinlat.so --framelog DIR`, then `framelog.py
--batch DIR`). Frames per second: from the mean interval between input
reads, the frame starts the limiter sets; frame time: `renderer_timed.so`'s
`BeginFrame` to `BeginFrame`; frame starts: the interval between input
reads (`libinlat.so`); CPU: the game process in CPUs, its main thread in
parentheses. `o2match` (`views/perf_demo.txt` from its start, 600 frames at
60 to 10000 uncapped; the rows of 1000 and 0 come from a second batch,
since a cap of 1000 now counts as uncapped):

| `com_maxfps` | renderer | frames per second | frame time (ms): median, p99, p99.9, max, sd | frame starts (ms): p99, sd | input to present (ms): median, p99 | CPU (main thread) |
| --- | --- | --- | --- | --- | --- | --- |
| 60 | GL1 | 62.6 → 60.0 | 15.999, 16.089, 16.532, 16.53, 0.509 → 16.667, 16.731, 16.923, 16.92, 0.041 | 16.057, 0.480 → 16.671, 0.002 | 1.311, 2.821 → 1.277, 2.966 | 0.18 (0.12) → 0.20 (0.15) |
| 60 | Vulkan | 62.6 → 60.0 | 15.999, 16.169, 16.368, 16.37, 0.502 → 16.667, 16.743, 16.861, 16.86, 0.048 | 16.070, 0.481 → 16.675, 0.003 | 1.017, 2.069 → 1.008, 2.093 | 0.11 (0.10) → 0.14 (0.13) |
| 125 | GL1 | 125.1 → 125.0 | 8.000, 8.078, 8.356, 8.79, 0.193 → 8.000, 8.049, 8.151, 8.83, 0.055 | 8.056, 0.192 → 8.003, 0.063 | 0.899, 2.068 → 0.885, 2.097 | 0.27 (0.20) → 0.34 (0.26) |
| 125 | Vulkan | 125.1 → 125.0 | 8.000, 8.074, 8.247, 8.34, 0.198 → 8.000, 8.047, 8.169, 8.32, 0.181 | 8.047, 0.198 → 8.003, 0.074 | 0.718, 1.559 → 0.682, 1.463 | 0.16 (0.15) → 0.24 (0.22) |
| 144 | GL1 | 166.7 → 144.0 | 6.000, 6.072, 6.166, 9.00, 0.157 → 6.945, 6.996, 7.128, 8.98, 0.092 | 6.054, 0.157 → 6.950, 0.099 | 0.837, 1.917 → 0.843, 1.987 | 0.34 (0.24) → 0.38 (0.29) |
| 144 | Vulkan | 166.7 → 144.0 | 6.000, 6.070, 6.178, 8.35, 0.150 → 6.945, 6.996, 7.121, 8.43, 0.083 | 6.047, 0.157 → 6.951, 0.094 | 0.695, 1.317 → 0.658, 1.322 | 0.20 (0.19) → 0.27 (0.25) |
| 165 | GL1 | 166.7 → 165.1 | 6.000, 6.070, 6.157, 8.67, 0.143 → 6.061, 6.113, 6.293, 8.60, 0.153 | 6.053, 0.145 → 6.066, 0.110 | 0.852, 1.844 → 0.830, 1.849 | 0.35 (0.25) → 0.43 (0.33) |
| 165 | Vulkan | 166.7 → 165.0 | 6.000, 6.063, 6.201, 8.26, 0.147 → 6.061, 6.104, 6.229, 8.31, 0.096 | 6.048, 0.144 → 6.064, 0.106 | 0.684, 1.326 → 0.643, 1.232 | 0.20 (0.18) → 0.30 (0.29) |
| 240 | GL1 | 250.0 → 240.0 | 4.000, 4.066, 4.284, 8.93, 0.129 → 4.167, 4.205, 4.529, 8.87, 0.107 | 4.053, 0.137 → 4.170, 0.007 | 0.774, 1.650 → 0.744, 1.618 | 0.46 (0.33) → 0.58 (0.45) |
| 240 | Vulkan | 250.0 → 239.9 | 4.000, 4.045, 4.401, 8.16, 0.111 → 4.167, 4.203, 4.498, 8.28, 0.098 | 4.036, 0.121 → 4.170, 0.098 | 0.620, 1.112 → 0.571, 1.033 | 0.27 (0.25) → 0.42 (0.40) |
| 333 | GL1 | 333.2 → 332.9 | 3.000, 3.059, 3.259, 8.86, 0.115 → 3.004, 3.039, 3.271, 8.98, 0.107 | 3.051, 0.120 → 3.006, 0.019 | 0.748, 1.712 → 0.717, 1.653 | 0.59 (0.42) → 0.78 (0.62) |
| 333 | Vulkan | 333.3 → 332.9 | 3.000, 3.042, 3.243, 8.23, 0.103 → 3.004, 3.038, 3.254, 8.23, 0.096 | 3.036, 0.112 → 3.006, 0.010 | 0.594, 1.150 → 0.545, 1.094 | 0.36 (0.34) → 0.57 (0.55) |
| 500 | GL1 | 500.1 → 499.8 | 2.000, 2.060, 2.585, 8.65, 0.108 → 2.000, 2.048, 2.488, 8.61, 0.100 | 2.051, 0.037 → 2.002, 0.042 | 0.676, 1.600 → 0.655, 1.643 | 0.80 (0.57) → 1.09 (0.87) |
| 500 | Vulkan | 499.8 → 499.6 | 2.000, 2.034, 2.198, 8.20, 0.092 → 2.000, 2.031, 2.190, 8.25, 0.092 | 2.017, 0.101 → 2.001, 0.101 | 0.508, 0.994 → 0.490, 1.018 | 0.52 (0.49) → 0.81 (0.79) |
| 1000 | GL1 | 912.8 → 903.9 | 1.007, 1.967, 2.453, 11.27, 0.255 → 1.008, 1.987, 2.526, 11.47, 0.257 | 1.976, 0.243 → 1.999, 0.259 | 0.720, 1.738 → 0.724, 1.754 | 1.34 (0.92) → 1.33 (0.92) |
| 1000 | Vulkan | 997.3 → 996.7 | 1.000, 1.215, 1.634, 8.62, 0.120 → 1.000, 1.215, 1.567, 8.40, 0.116 | 1.223, 0.124 → 1.216, 0.090 | 0.514, 1.127 → 0.515, 1.140 | 1.04 (0.99) → 1.04 (1.00) |
| 0 | GL1 | 910.8 → 912.2 | 1.007, 1.973, 2.401, 11.38, 0.259 → 1.007, 1.913, 2.391, 11.24, 0.248 | 1.979, 0.263 → 1.927, 0.241 | 0.723, 1.750 → 0.708, 1.717 | 1.33 (0.92) → 1.33 (0.92) |
| 0 | Vulkan | 997.3 → 996.5 | 1.000, 1.215, 1.597, 8.59, 0.117 → 1.000, 1.184, 1.553, 8.49, 0.114 | 1.233, 0.122 → 1.181, 0.119 | 0.516, 1.145 → 0.504, 1.106 | 1.04 (0.99) → 1.04 (1.00) |

The `obj_team2` yard (a fixed view of `views/perf.txt`, the local server
running):

| `com_maxfps` | renderer | frames per second | frame time (ms): median, p99, p99.9, max, sd | frame starts (ms): p99, sd | input to present (ms): median, p99 | CPU (main thread) |
| --- | --- | --- | --- | --- | --- | --- |
| 60 | GL1 | 62.5 → 60.0 | 15.999, 16.224, 16.549, 16.55, 0.107 → 16.667, 16.893, 16.957, 16.96, 0.104 | 16.056, 0.013 → 16.671, 0.002 | 1.097, 2.128 → 1.114, 2.376 | 0.15 (0.12) → 0.18 (0.14) |
| 60 | Vulkan | 62.5 → 60.0 | 16.001, 16.257, 16.682, 16.68, 0.119 → 16.667, 16.889, 17.137, 17.14, 0.105 | 16.083, 0.052 → 16.677, 0.004 | 0.960, 1.894 → 0.961, 2.063 | 0.11 (0.10) → 0.13 (0.13) |
| 125 | GL1 | 125.0 → 125.0 | 8.000, 8.200, 8.402, 8.59, 0.102 → 8.000, 8.174, 8.227, 8.47, 0.091 | 8.061, 0.021 → 8.003, 0.001 | 0.877, 1.596 → 0.851, 1.533 | 0.25 (0.19) → 0.31 (0.25) |
| 125 | Vulkan | 125.0 → 125.0 | 8.000, 8.198, 8.271, 8.47, 0.096 → 8.000, 8.172, 8.217, 8.42, 0.088 | 8.051, 0.013 → 8.003, 0.001 | 0.743, 1.320 → 0.682, 1.280 | 0.15 (0.14) → 0.23 (0.22) |
| 144 | GL1 | 166.7 → 144.0 | 6.000, 6.184, 6.429, 6.48, 0.094 → 6.945, 7.115, 7.352, 7.39, 0.091 | 6.058, 0.022 → 6.949, 0.002 | 0.783, 1.382 → 0.821, 1.423 | 0.29 (0.22) → 0.34 (0.28) |
| 144 | Vulkan | 166.7 → 144.0 | 5.999, 6.186, 6.234, 6.47, 0.092 → 6.945, 7.107, 7.174, 7.32, 0.088 | 6.042, 0.009 → 6.949, 0.002 | 0.677, 1.207 → 0.649, 1.240 | 0.19 (0.17) → 0.26 (0.25) |
| 165 | GL1 | 166.7 → 165.0 | 6.000, 6.190, 6.400, 6.56, 0.097 → 6.060, 6.223, 6.410, 6.49, 0.087 | 6.065, 0.030 → 6.065, 0.002 | 0.791, 1.487 → 0.747, 1.334 | 0.29 (0.22) → 0.38 (0.31) |
| 165 | Vulkan | 166.7 → 165.0 | 6.000, 6.171, 6.398, 6.51, 0.092 → 6.061, 6.216, 6.446, 6.47, 0.087 | 6.036, 0.008 → 6.065, 0.002 | 0.673, 1.223 → 0.625, 1.146 | 0.19 (0.18) → 0.30 (0.28) |
| 240 | GL1 | 250.0 → 240.0 | 3.999, 4.133, 4.172, 4.23, 0.072 → 4.166, 4.296, 4.461, 4.71, 0.075 | 4.054, 0.019 → 4.170, 0.016 | 0.692, 1.110 → 0.644, 1.158 | 0.36 (0.26) → 0.51 (0.42) |
| 240 | Vulkan | 250.0 → 240.0 | 4.000, 4.119, 4.155, 4.26, 0.070 → 4.166, 4.302, 4.458, 4.61, 0.075 | 4.007, 0.005 → 4.171, 0.002 | 0.587, 0.992 → 0.555, 1.014 | 0.26 (0.25) → 0.41 (0.39) |
| 333 | GL1 | 333.3 → 332.9 | 3.000, 3.114, 3.147, 3.17, 0.065 → 3.004, 3.118, 3.169, 3.26, 0.066 | 3.041, 0.010 → 3.007, 0.012 | 0.634, 1.001 → 0.602, 1.023 | 0.44 (0.32) → 0.69 (0.57) |
| 333 | Vulkan | 333.3 → 332.9 | 3.000, 3.111, 3.144, 3.16, 0.065 → 3.004, 3.114, 3.146, 3.19, 0.063 | 3.003, 0.004 → 3.007, 0.002 | 0.556, 0.902 → 0.520, 0.864 | 0.34 (0.32) → 0.55 (0.53) |
| 500 | GL1 | 500.0 → 500.0 | 2.000, 2.098, 2.161, 2.73, 0.054 → 2.000, 2.096, 2.147, 2.30, 0.056 | 2.014, 0.015 → 2.002, 0.007 | 0.543, 0.890 → 0.533, 0.847 | 0.62 (0.46) → 0.97 (0.82) |
| 500 | Vulkan | 500.0 → 500.0 | 2.000, 2.091, 2.121, 2.14, 0.050 → 2.000, 2.094, 2.122, 2.14, 0.051 | 2.002, 0.003 → 2.002, 0.001 | 0.457, 0.721 → 0.447, 0.706 | 0.50 (0.48) → 0.78 (0.76) |
| 1000 | GL1 | 999.9 → 998.5 | 0.999, 1.189, 1.453, 2.00, 0.092 → 0.999, 1.201, 1.623, 2.89, 0.085 | 1.126, 0.038 → 1.164, 0.049 | 0.565, 0.877 → 0.563, 0.895 | 1.25 (0.94) → 1.24 (0.94) |
| 1000 | Vulkan | 1000.0 → 1000.0 | 0.999, 1.105, 1.186, 1.29, 0.047 → 0.998, 1.102, 1.201, 1.29, 0.049 | 1.001, 0.004 → 1.002, 0.006 | 0.431, 0.616 → 0.433, 0.665 | 1.03 (1.00) → 1.03 (1.00) |
| 0 | GL1 | 999.8 → 998.7 | 0.999, 1.180, 1.449, 1.71, 0.085 → 0.999, 1.184, 1.500, 1.96, 0.083 | 1.120, 0.037 → 1.116, 0.040 | 0.567, 0.878 → 0.559, 0.869 | 1.25 (0.94) → 1.25 (0.94) |
| 0 | Vulkan | 1000.0 → 1000.0 | 0.999, 1.098, 1.177, 1.35, 0.047 → 0.999, 1.097, 1.178, 1.56, 0.047 | 1.002, 0.007 → 1.001, 0.012 | 0.438, 0.652 → 0.432, 0.656 | 1.03 (1.00) → 1.03 (1.00) |

- The caps are exact: the mean interval between frame starts is the
  period to the microsecond (6944.997 µs at 144 for a period of 6945,
  16666.98 at 60), where the old limiter gave 62.5 frames per second at
  60, 166.7 at 144 and 165 and 250 at 240. Caps that divide 1000 (125,
  500) ran at their rate before too.
- Frame starts are even: the 99th percentile of the interval between them
  is 2 to 10 µs above the period at every cap up to 500 (the old limiter's
  frames started on the millisecond tick, 2 to 83 µs above its whole
  milliseconds), with a standard deviation of 0.001 to 0.016 ms in the
  yard. In the demo what is left are the frames that take longer than a
  period (models loaded in the middle of the match, 8 to 9 ms), followed
  by one short interval that makes up for it when they were late by less
  than a period.
- The frame time and the presents still vary with each frame's own work
  (the local server's frame, cgame, the renderer), relative to the period
  about as before: the limiter starts frames evenly and holds nothing
  after the input is read.
- Input to present: the median is the same or shorter at every cap with
  both renderers (in the yard Vulkan 0.649 against 0.677 ms at 144, 0.555
  against 0.587 at 240; GL1 at 144 in a batch of 5 runs 0.789 against
  0.788, at 60 1.103 against 1.105). At 60 and 144 the tail is longer, the
  90th percentile up to 0.16 ms and the 99th up to 0.25 (GL1 in the yard,
  5 runs: 1.638 against 1.450 ms at 144), all of it after `BeginFrame` (the
  renderer, cgame and the driver; from the input read to `BeginFrame` the
  same). It comes from the frame rate, not the wait: the new limiter at
  the frame rates the old one gave (caps 62 and 167: 62.0 and 166.97 frames
  per second, 4 runs) has the old tails within the run to run spread (99th
  percentile, GL1 at 167: 1.425 against 1.438 ms, Vulkan 1.229 against
  1.173, its runs 1.162 to 1.264 against 1.146 to 1.225; at 62: 2.346
  against 2.237 and 1.991 against 1.925, the runs 2.081 to 2.474 against
  2.135 to 2.337 and 1.871 to 2.028 against 1.756 to 1.960) and lower
  medians, and the same binary at 144 the longer GL1 tail (1.589). How a
  frame rate near a multiple of the display's (59.96 Hz here, frames shown
  by the compositor) lands against its refreshes is the likely cause; a VRR
  display, where caps are used most, wasn't available to test.
- CPU: the polled last millisecond costs CPU at every cap below 1000, 0.4
  to 0.7 ms of a core more per frame: Vulkan in the yard 0.11 to 0.13 CPUs at
  60, 0.19 to 0.26 at 144, 0.26 to 0.41 at 240, 0.34 to 0.55 at 333 and
  0.50 to 0.78 at 500; uncapped and at 1000, the same as before.
- Uncapped and at 1000 the frame rates are the old ones within the run to
  run spread (GL1 in the demo 903.9 and 912.2 frames per second with the
  new binary at 1000 and 0, the same code, the old one 912.8 and 910.8).
  The new limiter doesn't start a frame before `Sys_Milliseconds` has
  ticked since the last one, where the old `bias` let a frame that
  followed a late one start at once, with 0 ms of game time when the
  millisecond hadn't changed (`Com_ModifyMsec` made it 1 ms).
- The deterministic views (`run.sh`: `com_maxfps 60`, `fixedtime 16`) give
  the same frames to the bit with both binaries: `dm/mohdm1`,
  `obj/obj_team2`, `mohdm1_dlight`, `menus.txt` and `mp_mohdm1` with Vulkan
  and `dm/mohdm1` with GL1 (33 views; `MOH_BIN=openmohaa_before run.sh`).
  A dedicated server (`omohaaded`, `dm/mohdm1`) runs 20.00 and 40.00 frames
  a second at `sv_fps` 20 and 40 with both binaries, its intervals 49.82 to
  50.16 ms instead of 49.36 to 50.64.

Caps that divide 1000 (2026-10-17, a second change): the deadline in
microseconds started frames wherever in the millisecond it happened to be
(set at startup and again after every frame late by more than a period),
but the game time of a frame is whole milliseconds: `com_frameTime -
lastTime`, `com_frameTime` read from `Sys_Milliseconds` in `Com_EventLoop`
right after `IN_Frame`. From the wait's end to that read takes 7 to 14 µs
(median; 99th percentile 11 to 47 µs, a tail of up to 0.9 ms). When the
deadline sat that close before a tick, some reads fell in the next
millisecond and frames alternated 3 and 5 ms at a cap of 250 instead of
4. Every frame is a usercmd, a step of the player's movement (pmove:
`cmd.serverTime` minus the previous one). The old limiter started frames
on the tick. Measured with a temporary build (not committed) that logged
every frame's msec before `Com_ModifyMsec`, without `fixedtime`, 1920x1080,
Vulkan: a local game (`dm/mohdm1`, free for all with 8 bots, the view
turning) and the `o2match` demo, 5 runs of 60 s per cap and binary, every
run a new deadline phase. The frames that started on time but whose
`com_frameTime` fell in another millisecond than one period after the
previous frame's, and in parentheses the frames late by more than half a
millisecond and their catch-up (bursts of 6 to 8 ms frames, one per
binary in the demo at different moments: the machine, not the limiter):

| | frames per binary | old limiter | 413c50d1 | now |
| --- | --- | --- | --- | --- |
| local game, cap 125 | 37490 | 0 (0) | 0 (4) | 0 (0) |
| local game, cap 250 | 74990 | 0 (6) | 0 (18) | 0 (2) |
| local game, cap 500 | 149990 | 0 (9) | 36, in 4 of 5 runs (32) | 0 (6) |
| `o2match`, cap 125 | 37490 | 0 (0) | 4, in 1 run (28) | 0 (0) |
| `o2match`, cap 250 | 74990 | 0 (0) | 0 (0) | 0 (43) |
| `o2match`, cap 500 | 149990 | 0 (58) | 18, in 1 run (0) | 0 (0) |

The runs of 413c50d1 with such frames had their deadline 794 to 963 µs
after the tick. None of its ten runs at 250 drew a phase close enough;
with the deadline moved to a chosen phase (the same build, the local game,
cap 250, 20 s each, frames of 3 or 5 ms out of 5000):

| deadline after the tick (µs) | 500 | 900 | 950 | 970 | 980 | 984 | 986 | 988 | 990 | 992 | 994 | 997 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| frames of 3 or 5 ms | 0 | 18 | 18 | 4 | 26 | 130 | 242 | 554 | 2533 | 442 | 0 | 0 |

With the phase drawn by chance, about one start (or frame late by more
than a period) in a hundred got a phase where 3 to 51 % of the frames
were 3 and 5 ms, and about one in ten one with a pair every few seconds.
The old limiter also let a frame after a late one last 0 ms (`bias`; 7
such frames in its runs at 500).

Now a cap that divides 1000 (`Com_Frame`: 125, 200, 250, 500 and the
lower ones, also for `com_maxfpsUnfocused` and `com_maxfpsMinimized`)
keeps the deadline in whole milliseconds of `Sys_Milliseconds`: it
advances by 1000 / cap, the same rule starts it again after a frame late
by more than a period, and the frame starts when that clock ticks
(`minMsec` up to the deadline).
Caps that don't divide 1000 keep the deadline in microseconds; their 6 and
7 ms frames at 144 are what the cap asks for. The wait (`Com_FrameWait`)
still sleeps until 1 ms before the frame and polls the rest: it knows when
`Sys_Milliseconds` (`gettimeofday`) will tick from the last tick it saw
while polling, since `Sys_Microseconds` (`CLOCK_MONOTONIC`) runs at the
same rate and the tick is whole milliseconds later. If the millisecond
clock disagrees by more than 50 µs (the clock was set, a suspend), it
waits once like the old limiter, asleep for the whole milliseconds but one
and polling the last, and sees a tick again. Not on Windows, where
`timeGetTime` follows the timer interrupts: there it always waits the old
way. That old way alone, the first version measured, starts frames on the
tick too but polls 0 to 1 ms instead of 1 ms, and like the old limiter it
lengthened the input to present time by 0.02 to 0.05 ms at every cap with
both renderers (the core clocks down, see above).

Results (idle machine, other processes 1.4 CPUs at most; `perf.py
--binaries <old>,<413c50d1>,<the old way alone>,<this build> --caps
125,250,500 --seconds 10 --preload libinlat.so --framelog DIR`, 1920x1080,
medians of 3 interleaved runs; input to present and frame starts as in the
first results, in ms; CPU of the game process):

| view, cap | renderer | input to present, median: old, 413c50d1, old way alone, now | input to present, p99: 413c50d1, now | frame starts p99: old, 413c50d1, now | CPU: old, 413c50d1, old way alone, now |
| --- | --- | --- | --- | --- | --- |
| `o2match`, 125 | GL1 | 0.904, 0.862, 0.884, 0.883 | 2.076, 2.035 | 8.059, 8.003, 8.003 | 0.27, 0.33, 0.27, 0.33 |
| | Vulkan | 0.742, 0.680, 0.724, 0.670 | 1.425, 1.346 | 8.053, 8.003, 8.003 | 0.16, 0.24, 0.17, 0.24 |
| `o2match`, 250 | GL1 | 0.777, 0.726, 0.788, 0.751 | 1.447, 1.584 | 4.054, 4.002, 4.002 | 0.46, 0.59, 0.47, 0.60 |
| | Vulkan | 0.634, 0.575, 0.615, 0.589 | 1.161, 1.088 | 4.040, 4.002, 4.003 | 0.27, 0.44, 0.28, 0.44 |
| `o2match`, 500 | GL1 | 0.670, 0.666, 0.662, 0.657 | 1.599, 1.619 | 2.053, 2.002, 2.002 | 0.79, 1.09, 0.81, 1.09 |
| | Vulkan | 0.511, 0.488, 0.509, 0.497 | 0.963, 1.006 | 2.013, 2.001, 2.001 | 0.52, 0.81, 0.54, 0.82 |
| `obj_team2` yard, 125 | GL1 | 0.860, 0.829, 0.854, 0.831 | 1.723, 1.557 | 8.063, 8.003, 8.003 | 0.25, 0.31, 0.25, 0.30 |
| | Vulkan | 0.720, 0.692, 0.726, 0.682 | 1.390, 1.292 | 8.050, 8.003, 8.003 | 0.15, 0.24, 0.16, 0.24 |
| `obj_team2` yard, 250 | GL1 | 0.686, 0.647, 0.694, 0.643 | 1.116, 1.035 | 4.053, 4.002, 4.002 | 0.36, 0.54, 0.37, 0.53 |
| | Vulkan | 0.591, 0.555, 0.594, 0.547 | 0.950, 0.947 | 4.019, 4.002, 4.002 | 0.26, 0.43, 0.27, 0.42 |
| `obj_team2` yard, 500 | GL1 | 0.552, 0.522, 0.548, 0.537 | 0.867, 0.903 | 2.020, 2.002, 2.002 | 0.62, 0.97, 0.64, 0.98 |
| | Vulkan | 0.465, 0.454, 0.457, 0.435 | 0.723, 0.775 | 2.002, 2.002, 2.001 | 0.51, 0.78, 0.52, 0.78 |

- Every frame at a cap that divides 1000 is now 1000 / cap ms of game
  time but for the frames late by more than a period and their catch-up,
  as with the old limiter (the table above); the frames start on the tick
  plus 7 to 11 µs (median) to the read.
- Input to present: the medians are within 0.025 ms of 413c50d1's either
  way and shorter than the old limiter's everywhere (by 0.013 to 0.072
  ms); a second batch of 5 runs of `o2match` gave 0.880 against 0.891 ms
  (GL1, 125), 0.744 against 0.736 (GL1, 250), 0.694 against 0.683
  (Vulkan, 125) and 0.578 against 0.568 (Vulkan, 250), the runs
  overlapping. In `o2match` at 250 the medians were 0.008 to 0.025 ms
  higher in both batches, in the yard with Vulkan 0.008 to 0.019 lower.
  The 99th percentile: in the yard the same or lower; in `o2match` with
  Vulkan about the same (1.088 against 1.066 ms at 250, the
  median of 8 runs each, 1.042 to 1.526 against 1.008 to 1.225; 0.971 to
  1.044 against 0.955 to 0.972 at 500), with GL1 about 0.08 ms higher at
  250 in both batches (1.513 to 1.636 in 8 runs against 1.412 to 1.544),
  where the old limiter and the old way alone, which also start frames on
  the tick, gave 1.591 to 1.903 and 1.562 to 1.659. Not explained: the
  phase of the frames against the kernel's 1000 Hz timer tick is fixed
  when they start on the wall clock's millisecond and drawn by chance with
  a deadline in microseconds (not tested).
- Frame starts and CPU: those of 413c50d1 (the polled millisecond is
  kept).
- The usercmd's step also moves with the client's clock adjustment, which
  this doesn't change: on every snapshot `CL_AdjustTimeDelta` moves
  `cl.serverTimeDelta` by 1 ms, or -2 ms after extrapolated frames
  (Quake 3's code), so in the local game 6000 of the 74990 usercmds at 250
  are 5 or 2 ms with every binary (none in demos, which don't adjust).
- The deterministic views give the same frames to the bit with
  413c50d1 and with this build, at `run.sh`'s cap of 60 and at 50, which
  divides 1000: `dm/mohdm1`, `obj/obj_team2`, `mohdm1_dlight`,
  `menus.txt` and `mp_mohdm1` with Vulkan and `dm/mohdm1` with GL1 (66
  comparisons). A dedicated server runs 20.00 and 40.00 frames a second at
  `sv_fps` 20 and 40 as with 413c50d1 (its wait doesn't use the tick).

Present timing (2026-10-17): everything measured above ends at the
present call. What happens after it, until the frame is on the screen,
was measured on this machine's path: KWin 6.6.6 (Plasma Wayland session,
atomic modesetting, `allowTearing` on, its default), Xwayland 24.1.10,
RADV (Mesa 26.0.8), SDL 2.32.10, the display 1920x1080 on HDMI at 59.975
Hz (the mean interval of the kernel's vblank timestamps, 16.6735 ms), no
VRR. The game runs under Xwayland (SDL's default video driver, x11) and,
with `SDL_VIDEODRIVER=wayland`, as a native Wayland client: this build
works both ways. A 240 Hz display isn't available here, so these numbers
show the mechanisms (what the compositor costs, what each present mode
queues, latency against the cap), not a 240 Hz player's numbers.

What a client is told (`vulkaninfo`; read in the sources: Mesa's
`src/vulkan/wsi/wsi_common_x11.c`, `wsi_common_wayland.c`, `wsi_common.c`
and `src/loader/loader_wayland_helper.c` at `mesa-26.0.8`, Xwayland's
`hw/xwayland/xwayland-present.c` at `xwayland-24.1.10`, KWin's
`src/compositor.cpp`, `src/core/renderloop.cpp`,
`src/backends/drm/drm_commit_thread.cpp`, `drm_commit.cpp` and
`src/wayland/presentationtime.cpp` at `v6.6.6`):

- RADV has `VK_KHR_present_id` and `VK_KHR_present_wait` (and their `2`
  versions) on X11 (Xwayland) and Wayland surfaces; not
  `VK_EXT_present_timing` or `VK_GOOGLE_display_timing`.
- X11: `vkWaitForPresentKHR` returns once the frame's GPU work is done (a
  timeline semaphore of the WSI) and Xwayland's `PresentCompleteNotify`
  for it has arrived. Xwayland hands the game's buffer to KWin (mode
  "flip", in a window too) and completes an asynchronous present
  (immediate; mailbox, which Mesa sends to Xwayland as asynchronous too;
  FIFO relaxed) once a `wl_display.sync` round trip shows KWin has the
  commit, with the UST of the last frame callback; a FIFO present at the
  next frame callback, which KWin sends at the end of the composite that
  latched the frame, before its page flip. Neither is the time the frame
  is shown.
- Wayland: Mesa asks for presentation feedback (`wp_presentation`) for the
  presents that carry a present id; `vkWaitForPresentKHR` returns when it
  says "presented" or "discarded". KWin gives the kernel's page flip
  timestamp (flags hardware clock, hardware completion, vsync), never
  "zero copy" (direct scanout isn't reported), and for a tearing flip the
  timestamp of the vblank before it.
- So only a native Wayland client is told when its frame was shown, and
  not with tearing. What an unprivileged process can see besides: the
  kernel's vblank timestamps (`DRM_IOCTL_WAIT_VBLANK` on `/dev/dri/card1`
  needs no DRM master), which framebuffer the primary plane scans out
  (`GETPLANE`, `GETFB2`: KWin composites into AR30 buffers, the game's
  swapchain images are XR24, so one of them there is direct scanout, the
  compositor bypassed) and when that changes (KWin's commit; with tearing,
  the flip), and when the GPU finished each frame.
- Calling `vkWaitForPresentKHR` on a second thread while the game acquires
  and presents is invalid (the swapchain is externally synchronized for
  all three): a first version of the tool did, and with the Wayland WSI
  the heap got corrupted.

The tool: `libinlat.so` with `INLAT_DISPLAY=1` (nothing of it runs
otherwise; no renderer change) records, besides each frame's input read
and present call, the X11 completion events (`xcb_wait_for_special_event`
wrapped in Mesa's event thread), the Wayland presentation feedback (it
enables `VK_KHR_present_id`, numbers every present and wraps the
feedback's listener), when the GPU finished each submission (a timeline
semaphore of its own that every `vkQueueSubmit` also signals, waited for
by a thread), every vblank with the primary plane's framebuffer and its
format, the framebuffer changes (polled every 250 µs with
`INLAT_FBPOLL_US=250`, for the tearing variants), and whether the SDL
window is fullscreen. `INLAT_PRESENT_MODE=mailbox` forces mailbox (the
renderer asks for immediate with `r_swapInterval 0`, FIFO with `1`).
`framelog.py --display DIR` (`--table`, `--json`) works out when each
frame was shown, the start of the scanout that shows it or the tearing
flip:

- Wayland without tearing: the feedback's time, exact.
- X11 FIFO: the vblank after the completion (the frame callback).
- Tearing (Wayland: no vsync flag; X11: more frame callbacks than
  refreshes, or KWin's rule, a fullscreen window presenting with
  `PresentOptionAsyncMayTear`, which Mesa's immediate mode sets): each
  framebuffer change shows the newest frame ready before it (KWin
  composites as soon as a frame is ready and its commit thread flips at
  once).
- X11 asynchronous without tearing: KWin latches, when a composite starts,
  the newest commit whose buffer is ready, and sends frame callbacks at
  its end; a present completed with a UST older than a frame callback
  reached KWin before that composite. When the composite started isn't
  reported: taking its frame callback gives a lower bound, 1.5 ms before
  it an upper one ("to" in the table).

Checks: with direct scanout, the frames the primary plane showed (for the
best of the 24 ways to assign the 4 framebuffers to the swapchain images)
are the ones the rules put there at 95 to 100 % of the vblanks, for
Xwayland (mailbox: the lower bound; FIFO) and Wayland alike (78 % in one
Wayland FIFO run at 60 that was composited at 16 % of its vblanks); with
Wayland's tearing, the frames found are those KWin reported presented;
in a window, Wayland's exact times and Xwayland's lower bound agree within
0.35 ms uncapped and at 250 (at 59 the lower bound is 1.1 to 1.8 ms
under). The probes cost nothing measurable: the input to present medians
with them and without (`INLAT_DISPLAY=0`) within 0.025 ms.

Results (idle machine: other processes 1.9 CPUs at most; Vulkan,
1920x1080; `perf.py --list ... --binaries <variant wrappers> --caps
0,59,250 --seconds 10 --runs 3 --preload libinlat.so --framelog DIR` with
`INLAT_DISPLAY=1`, FIFO with `--frames 600`, cap 60 one run of 40 s: a cap
of 60 drifts against the 59.975 Hz display by one refresh in 40 s, so a
shorter run sees one phase; then `framelog.py --display DIR --table`). A
variant is a wrapper script in the binary directory, for example
`SDL_VIDEODRIVER=wayland INLAT_FBPOLL_US=250 exec ./openmohaa "$@" +set
r_fullscreen 1 +set in_nograb 0` (fullscreen needs `in_nograb 0`: with
`in_nograb 1`, perf.py's default, the first frame turns it off again).
Input to present: the median of the runs' medians; input to display: the
frames shown in all runs pooled (about 600 per 10 s run at 60 Hz, 2400 in
the 40 s run), ms; shown: the share of the presented frames that reached
the display; slack: the median time a finished frame waited for the
compositor's latch (its ready-to-display time above its run's 1st
percentile), the most a later frame start could take off:

| `com_maxfps` | variant | `o2match`: input to present, median | input to display: median, p90, p99, p99.9 | shown | slack | yard: input to present, median | input to display: median, p90, p99, p99.9 | slack |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 0 | Xwayland, window, immediate | 0.514 | 7.29, 7.81, 8.22, 10.01 (to 8.41) | 6 % | 1.36 | 0.431 | 6.61, 7.48, 7.77, 7.86 (to 7.71) | 0.97 |
| 0 | Wayland, window, immediate | 0.516 | 7.33, 7.90, 8.89, 9.36 | 6 % | 1.35 | 0.430 | 6.94, 7.58, 7.78, 8.12 | 1.10 |
| 0 | Xwayland, fullscreen, mailbox | 0.516 | 4.63, 5.67, 6.21, 13.85 (to 6.33) | 6 % | 0.92 | 0.440 | 5.44, 6.73, 7.66, 7.76 (to 7.25) | 1.01 |
| 0 | Wayland, fullscreen, mailbox | 0.520 | 4.77, 5.71, 6.61, 12.32 | 6 % | 1.14 | 0.438 | 5.69, 7.06, 7.68, 7.90 | 0.99 |
| 0 | Xwayland, fullscreen, immediate | 0.532 | 1.57, 2.19, 3.09, 3.46 | 96 % | 0.40 | 0.465 | 1.52, 1.76, 2.01, 2.42 | 0.45 |
| 0 | Wayland, fullscreen, immediate | 0.541 | 1.56, 2.19, 3.12, 3.57 | 96 % | 0.39 | 0.473 | 1.52, 1.76, 2.01, 2.85 | 0.45 |
| 0 | Xwayland, window, FIFO | 1.008 | 70.97, 71.16, 71.79, 72.13 | 100 % | 2.41 | 0.941 | 71.00, 71.23, 72.11, 72.25 | 1.75 |
| 0 | Wayland, window, FIFO | 1.070 | 71.27, 72.23, 72.38, 72.44 | 100 % | 2.15 | 1.010 | 71.22, 72.09, 72.35, 72.43 | 1.54 |
| 0 | Xwayland, fullscreen, FIFO | 1.013 | 50.50, 50.53, 50.55, 50.56 | 100 % | 1.58 | 0.951 | 50.49, 50.52, 50.54, 50.56 | 1.41 |
| 0 | Wayland, fullscreen, FIFO | 1.011 | 50.49, 50.53, 50.55, 50.56 | 100 % | 1.63 | 0.947 | 50.49, 50.52, 50.55, 50.56 | 1.52 |
| 250 | Xwayland, window, immediate | 0.589 | 8.60, 10.34, 10.94, 11.17 (to 9.54) | 24 % | 2.25 | 0.561 | 8.59, 10.36, 10.90, 11.54 (to 9.57) | 2.35 |
| 250 | Wayland, window, immediate | 0.579 | 8.94, 10.61, 11.61, 12.03 | 24 % | 2.55 | 0.558 | 8.66, 10.43, 11.07, 11.82 | 2.52 |
| 250 | Xwayland, fullscreen, mailbox | 0.576 | 6.80, 8.50, 9.04, 10.11 (to 8.36) | 24 % | 2.66 | 0.551 | 7.49, 9.34, 10.74, 11.40 (to 9.24) | 2.48 |
| 250 | Wayland, fullscreen, mailbox | 0.575 | 6.74, 8.48, 9.38, 10.17 | 24 % | 2.60 | 0.562 | 7.45, 9.26, 10.52, 11.14 | 2.50 |
| 250 | Xwayland, fullscreen, immediate | 0.580 | 2.04, 2.53, 3.03, 3.42 | 100 % | 0.16 | 0.563 | 2.10, 2.35, 2.65, 2.90 | 0.17 |
| 250 | Wayland, fullscreen, immediate | 0.580 | 2.10, 2.42, 2.89, 3.36 | 100 % | 0.17 | 0.565 | 2.17, 2.42, 2.69, 3.17 | 0.16 |
| 59 | Xwayland, window, immediate | 0.996 | 11.95, 18.60, 21.07, 22.35 (to 16.02) | 100 % | 8.31 | 0.955 | 11.30, 18.09, 19.71, 20.68 (to 16.27) | 7.94 |
| 59 | Wayland, window, immediate | 1.023 | 13.08, 19.61, 21.34, 21.93 | 100 % | 8.15 | 0.964 | 13.13, 19.64, 21.14, 21.65 | 8.21 |
| 59 | Xwayland, fullscreen, mailbox | 0.977 | 11.30, 17.99, 20.00, 21.74 (to 15.27) | 100 % | 8.32 | 0.916 | 11.25, 17.97, 20.28, 21.75 (to 15.47) | 7.89 |
| 59 | Wayland, fullscreen, mailbox | 0.985 | 11.36, 17.97, 19.82, 20.88 | 100 % | 8.09 | 0.925 | 11.36, 18.01, 19.75, 21.43 | 7.99 |
| 59 | Xwayland, fullscreen, immediate | 0.991 | 2.83, 3.50, 4.40, 10.49 | 100 % | 0.19 | 0.923 | 2.67, 3.34, 4.13, 4.84 | 0.18 |
| 59 | Wayland, fullscreen, immediate | 0.984 | 2.80, 3.48, 4.40, 10.49 | 100 % | 0.21 | 0.929 | 2.69, 3.47, 4.26, 4.92 | 0.19 |
| 59 | Xwayland, window, FIFO | 0.993 | 14.41, 22.78, 39.92, 44.34 | 100 % | 9.85 | 0.929 | 12.53, 19.22, 22.43, 23.71 | 8.66 |
| 59 | Wayland, window, FIFO | 1.013 | 14.02, 21.36, 35.93, 38.13 | 100 % | 8.95 | 0.946 | 13.11, 19.72, 21.28, 21.65 | 8.12 |
| 59 | Xwayland, fullscreen, FIFO | 0.974 | 12.53, 20.45, 36.37, 40.79 | 100 % | 9.41 | 0.941 | 11.05, 17.75, 19.31, 20.55 | 8.17 |
| 59 | Wayland, fullscreen, FIFO | 0.991 | 12.91, 22.26, 39.04, 43.02 | 100 % | 9.46 | 0.939 | 11.27, 17.92, 19.53, 19.90 | 8.03 |
| 60 | Xwayland, window, immediate | 0.856 | 15.68, 21.84, 23.32, 23.61 (to 17.02) | 98 % | 8.47 | 0.958 | 15.29, 21.52, 23.02, 23.16 (to 16.01) | 7.53 |
| 60 | Wayland, window, immediate | 0.885 | 13.84, 21.09, 22.50, 22.69 | 100 % | 7.65 | 0.951 | 14.89, 22.13, 23.54, 23.70 | 7.45 |
| 60 | Xwayland, fullscreen, mailbox | 0.822 | 11.87, 19.02, 20.53, 20.77 (to 15.60) | 98 % | 7.72 | 0.921 | 12.87, 19.12, 20.53, 20.79 (to 15.35) | 8.58 |
| 60 | Wayland, fullscreen, mailbox | 0.837 | 11.97, 19.21, 20.64, 20.77 | 100 % | 7.47 | 0.905 | 13.65, 19.91, 21.54, 21.68 | 9.13 |
| 60 | Xwayland, fullscreen, immediate | 0.836 | 2.56, 3.21, 3.99, 4.54 | 100 % | 0.18 | 0.915 | 2.68, 3.38, 4.16, 4.61 | 0.17 |
| 60 | Wayland, fullscreen, immediate | 0.834 | 2.54, 3.18, 3.93, 4.62 | 100 % | 0.20 | 0.917 | 2.61, 3.32, 4.14, 4.93 | 0.17 |
| 60 | Xwayland, window, FIFO | 0.870 | 43.66, 49.94, 51.35, 51.48 | 100 % | 7.76 | 0.942 | 43.03, 49.32, 50.73, 50.88 | 7.84 |
| 60 | Wayland, window, FIFO | 0.873 | 51.58, 57.86, 59.28, 59.42 | 100 % | 8.00 | 0.947 | 30.70, 36.97, 38.39, 38.53 | 7.59 |
| 60 | Xwayland, fullscreen, FIFO | 0.832 | 44.20, 50.36, 50.53, 50.55 | 100 % | 7.92 | 0.907 | 41.11, 47.38, 48.80, 48.94 | 7.77 |
| 60 | Wayland, fullscreen, FIFO | 0.829 | 32.31, 38.60, 40.01, 40.15 | 100 % | 7.88 | 0.986 | 41.18, 47.52, 48.92, 49.06 | 7.53 |

What the compositor costs. From the frame's GPU work done to the display
(the `obj_team2` yard, uncapped, medians; first percentile in
parentheses): 5.4 to 5.8 ms (4.6) composited in a window, 4.2 to 4.5 ms
(2.6 to 3.5) with direct scanout, 0.5 ms (0.06) tearing; input to present
is 0.43 to 0.47 ms and the GPU is done 0.6 to 0.8 ms later. KWin's frame
callbacks, sent at the end of its composite, reach Xwayland 3.9 ms (in a
window) and 3.3 ms (direct scanout) before the vblank on the median (it
starts a composite its render time estimate, a safety margin and 1 ms
before the vblank), and the scanout starts 0.6 ms after the vblank
does; a frame waits for the latch about half a frame period (the slack
column). So composition itself (a window against direct scanout) costs
about 1 to 1.5 ms; the vsync latch at least 2.6 to 3.5 ms more than
tearing, plus the wait for it.

- Direct scanout: KWin scans the game's swapchain image out in fullscreen
  with mailbox and FIFO, under Xwayland and Wayland alike (a game buffer
  on the primary plane at every vblank); never in a window, and never with
  tearing: it then composites every frame into its own buffer and flips
  that at once (its AR30 buffers on the primary plane).
- Xwayland costs nothing measurable: fullscreen it gives the native
  Wayland numbers within 0.4 ms in every mode at 0, 59 and 250 (the single
  runs at 60 differ by up to 0.8 ms, and with FIFO by how full the queue
  got); its completion comes 0.1 ms after the present call.
- The lowest input to display is fullscreen immediate (`r_fullscreen 1`,
  `r_swapInterval 0`, the default), with tearing: 1.5 ms uncapped, 2.0 to
  2.2 at 250, 2.5 to 2.8 at 59 and 60 (the frame's own CPU and GPU time,
  and KWin's 0.45 ms). Without tearing the lowest is fullscreen mailbox
  (direct scanout), 4.6 to 5.7 ms uncapped, 6.7 to 7.5 at 250, 11.3 to
  13.7 at 59 and 60, rendering frames that are never shown (6 % are
  uncapped, 24 % at 250). A window adds 1.1 to 2.7 ms to that at the high
  caps. In a window or with mailbox, a higher cap shortens the wait for
  the latch (half a frame period on average): 250 is 1.3 to 2.2 ms worse
  than uncapped there, 0.5 to 0.6 ms with tearing.
- FIFO queues: uncapped, three frames wait (four swapchain images) and
  input to display is 50.5 ms fullscreen, 71 ms in a window (a refresh
  more: the composite). Capped at 59, just below the refresh, the queue
  stays empty, 11 to 14 ms on the median; but frames queued once drain
  by only 0.28 ms a frame (59 against 59.975 Hz): in `o2match` the first
  35 frames measured came 25 to 35 ms after their input (frames queued at
  the demo's start), the 99th percentile 36 to 40 ms (19 to 22 in the
  yard). A slow frame that the limiter makes up would queue one the same
  way. At 60, above the refresh, the queue fills: 31 to 52 ms within 40 s.
- At caps near the refresh (59, 60) every vsync mode shows 100 % of the
  frames but each waits a random part of a refresh for the latch (slack
  7.5 to 9.9 ms).

Pacing changes that could lower input to display without adding lag
(none implemented: the frame limiter is left as it is in this step):

1. FIFO: keep at most one frame queued. Wait, before the next input read
   (after the present, where the acquire of the next image already
   blocks, or in `Com_Frame`'s wait), until the previous frame's present
   has completed (`vkWaitForPresentKHR` on the
   main thread is valid; on X11 it returns at KWin's composite, early
   enough). Measured upside: FIFO uncapped 50.5 and 71 ms against 11 to 14
   ms at a cap below the refresh, so 38 to 59 ms; it also removes what
   stays queued at 59. Latency effect: lower, never higher (it waits
   before the input is read; nothing is held after it). Fewer swapchain
   images with FIFO (3, the minimum) should take one refresh off without
   it (not measured).
2. Vsync modes (mailbox, a window, FIFO): start the frame just in time, so
   that its image is ready right before the compositor's latch. Measured
   upside: the slack, 0.9 to 1.4 ms uncapped, 2.3 to 2.7 ms at 250, 7.5
   to 9.9 ms at 59 and 60 (with FIFO on top of the queue). Latency effect:
   lower by up to the slack when the latch is predicted right, a whole
   refresh higher for a frame that misses it. It needs the latch time,
   which KWin doesn't report (3.3 to 3.9 ms before the vblank here, and it
   adapts: its safety margin grows after a late commit); the vblank grid
   is known (the kernel's timestamps, or Wayland's feedback). An engine
   change (when `Com_Frame` starts a frame), not a renderer one.
3. With tearing nothing is left to pace: the slack is 0.16 to 0.45 ms, and
   input to display is the frame's own CPU and GPU time plus KWin's 0.45
   ms; only less CPU or GPU time per frame lowers it. For the user's setup
   (250 on a 240 Hz display, no VRR) fullscreen immediate is the lowest,
   with tearing; on that display vsync through KWin should cost about what
   it costs here at 250, 4.6 to 5.4 ms over tearing (KWin's margins are
   times, not refreshes, and the frames come as often), and a full FIFO
   queue 12.5 ms (not measured).

World culling (2026-10-17): the world's traversal (`R_RecursiveWorldNode`,
`R_AddWorldSurfaces`) led the CPU causes of `o4match`'s slow frames (Plan,
Remaining, item 3 before this step). The starting point was the review, in
the Metal port's hand-over, of the "exact world culls" of JayRewd's omaha
commit 1c2c8754 (a flat list of the potentially visible leaves, culls of
groups of 8 leaves, a bounding sphere per face, early rejects in the mark
fragments), read in the commit itself.

What the traversal did per view (a temporary counting build; all frames of
the demos):

| per view | `obj_team2` yard | `dm/mohdm3` overview | `o2match` | `o4match` |
| --- | --- | --- | --- | --- |
| nodes visited | 324 | 1937 | 218 | 2817 |
| leaves added | 80 | 562 | 20 | 1124 |
| leaf surfaces (marks) | 4899 | 5631 | 1742 | 5313 |
| faces reached | 3988 | 3160 | 1413 | 2471 |
| culled by their plane (GL1) | 685 | 1458 | 184 | 1109 |
| drawn by GL1 | 3246 | 1702 | 1219 | 1290 |
| of those, wholly outside the view | 1622 (50 %) | 269 (16 %) | 677 (56 %) | 259 (20 %) |

In the stack samples of `o4match` (1920x1080) the traversal took 10.9 to
11.6 % of the main thread's time: 5.5 to 5.7 % the walk itself (the nodes'
`visframe` read, the recursion, the leaves' surface lists), 2.8 to 3.2 %
`R_CullSurface` (reading each face's plane, a cache miss per face: every
face is an allocation of its own) and 1.3 to 1.4 % `BoxOnPlaneSide`.
Whenever the view's cluster changed, `R_MarkLeaves` also marked the leaves
of the new PVS and their ancestors in a pass over every node: 0.23 ms each
time in `obj/obj_team4` (a temporary timing build; 278 times in the 15000
frames of `o4match`, 2 % of the frames, so right at their 99th percentile).

JayRewd's culls as written, against GL1 (a script also checked the 66 BSP
files of the game data):

- The flat leaf list takes every node that `R_MarkLeaves` marked and that
  is a leaf, in the order of the nodes array. That includes the leaves of
  the brush models, which the BSP tree doesn't reach but which have a
  cluster, so `R_MarkLeaves` marks them when it is in the PVS (14 to 94
  such leaves in the `dm` and `obj` maps; no leaf the tree reaches lists a
  brush model's surface): their surfaces are in the models' own
  coordinates, and drawn as world surfaces they put copies near the
  origin, which the commit hides by skipping surfaces past the world
  model's ("Fixed in Omaha"). The array's order is the walk's in every
  shipped map, but each leaf is tested on its own box, which gives GL1's
  result only when the leaf's box lies within the boxes of all its
  ancestors: in 43 of the 66 maps some leaves stick out of an ancestor's
  box, by one unit (the stored integer bounds are rounded outwards; up to
  134 leaves in a map, 68 in `dm/mohdm3`, 5 in `obj/obj_team2`), and such
  a leaf can be culled by GL1 at an ancestor and kept on its own box, or
  the reverse.
- The groups of 8 consecutive leaves are exact by themselves: a box that
  contains the leaves' boxes is wholly behind (or in front of) a plane
  only if each of them is (`BoxOnPlaneSide` takes the farthest corners,
  and rounding keeps their order).
- The sphere per face is centred on the face's bounds with half their
  diagonal as radius, computed in floats (not certain to enclose every
  point), and culls a face whose sphere is behind a frustum plane, with no
  margin. It leaves out faces with deforms but not the sky (the sky's
  faces give the extents of the sky box, `RB_ClipSkyPolygons`) nor the
  sky portal's (they decide whether the sky portal's view is rendered).
- The early mark rejects (`R_MarkFragments`: cgame's bullet holes and
  feet shadows) are exact, since a fragment whose points are all less
  than 0.25 in front of a plane is dropped by `R_ChopPolyBehindPlane`
  (0.5) anyway, but they aren't the world's traversal: `R_MarkFragments`
  is 0.8 % of the `o4match` samples in all. Not taken.
- The commit's replay of the last walk when the view repeats exactly
  (frames that share a client millisecond) isn't in the review's list: it
  only helps above 1000 frames per second, or in fixed benchmark views.

What was done (see Implementation notes, World culling), each change
measured against the build before it:

1. Faces culled against the view: a face whose bounds lie wholly behind
   one of the frustum's side planes, by more than a unit, is left out (the
   GPU would clip all of it away). Only the planes its leaf isn't wholly
   in front of; not for faces whose shader deforms vertexes, the sky,
   portals and the sky portal's faces, nor in the leaves that dynamic
   lights reach, nor with `r_shownormals`, where a face outside the view
   still changes the frame: the lights of a batch are the union of its
   surfaces' (with `r_fastdlights 1` every light that reaches one face is
   projected onto all the batch's triangles, `ProjectDlightTexture`, and
   in this renderer's per-pixel lights the side a light must be on is
   tested for the whole batch when it has a lit face,
   `tess.lightmapDlightFaces`), and `r_shownormals` draws `tess.normal`,
   which faces write only when their shader needs normals, so the others
   show what earlier batches left there. The comparison with the build
   before found the `r_fastdlights 1` case in `dl_a` (`mohdm1_dlight`: a
   wall top lit by a light of an off-screen face of its batch, 730 pixels)
   and the `r_shownormals` one in `dbg_normals` (384 pixels of normals);
   the per-pixel case came from reading the code (it needs a lit face and
   a lit grid or terrain patch in one batch). With the conditions both
   views give the frames of the build before.
2. The BSP walk replaced by the potentially visible leaves listed in its
   order: the same leaves in the same order, each tested on its own box
   after the box of 8 consecutive ones, and the few that stick out of an
   ancestor's box along their path from the root; `BoxOnPlaneSide`'s
   arithmetic inline (the same products in the same order).
3. What culling a face reads copied into its `msurface_t` (64 bytes,
   aligned): its plane and its bounds, rounded outwards to whole units in
   shorts, so that the faces culled by their plane or by the view are not
   read at all; `srfSurfaceFace_t` keeps its size (with the bounds in it,
   change 1 had moved the dynamic light fields, which the front end
   writes for every drawn face, onto another cache line for more faces).
4. `R_MarkLeaves` lists the leaves of the PVS in the walk's order, with
   GL1's test, from a compact copy of each leaf's cluster and area (8
   bytes; the boxes in another array in the same order), and groups them:
   no pass over the nodes, no marks for the ancestors (nothing walks the
   nodes any more). In the same frames of `o4match`: 67 µs per cluster
   change on the median (mean 111, 90th percentile 227) against 238 for
   GL1's pass (mean 227, 90th percentile 359); it varies with the GPU's
   memory traffic.

Exactness. A temporary verification build ran GL1's code beside the new
one in every view: `R_MarkLeaves` and `R_RecursiveWorldNode` as in
`renderergl1` (with this renderer's culling of dynamic lights per node),
marking a private copy of the nodes' `visframe`, and `R_CullSurface` on
each face's own data. It compared the leaves added (in order, each with
the frustum planes left and its dynamic lights) and the faces culled by
their plane, and projected every face culled against the view with the
view's modelview and projection matrices (in double precision; zFar
doesn't change x, y and w) to check that all its points lie outside one
clip plane x = ±w or y = ±w by more than float32's rounding (1e-5 of |x|
+ |w|). Results:

| what was run (with the final build's verification copy) | lists | views | leaves added | faces culled by their plane | faces culled against the view | differences |
| --- | --- | --- | --- | --- | --- | --- |
| `dm` lists (`dm/mohdm1` to `dm/mohdm7`; fog, decals, dynamic lights, sun flare, debug drawing) | 14 | 11375 | 3.58 M | 7.31 M | 2.28 M | 0 |
| `obj` lists (`obj/obj_team1` to `obj/obj_team4`, the sea shader) | 5 | 4165 | 0.72 M | 1.37 M | 1.26 M | 0 |
| multiplayer lists (`mp_*`: 20 soldiers, muzzle flash lights) | 5 | 11240 | 3.18 M | 7.17 M | 1.71 M | 0 |
| single-player lists (the sky portal of `m2l1`, `m2l3` and its fog, `m3l3`, the fog of `m4l1`, `m5l3`, `m6l1a`, `m6l2a`) | 8 | 5586 | 1.57 M | 2.71 M | 0.84 M | 0 |
| variants: the dynamic light lists with `r_fastdlights 1`, `mohdm1_texenv` with `r_ext_texture_env_add 1`, `obj_team2` and `dm/mohdm3` with `r_novis 1`, `obj_team2` with `r_nocull 1` and with `r_facePlaneCull 0` | 8 | 7234 | 2.08 M | 4.04 M | 2.97 M | 0 |
| demos: `o2match`, `o4match`, an `obj/obj_team1` match (240 s) and a `dm/mohdm6` one (123 s), every frame | 4 | 52654 | 21.4 M | 36.1 M | 18.2 M | 0 |

(Differences: leaves that differ from GL1's walk in order, frustum planes
left or dynamic lights, faces whose plane cull differs from GL1's, faces
culled against the view with a point not outside the clip volume. The
nearest point of a culled face was 0.005 pixel beyond the frame's edge,
on a face whose points are all beyond it. The `r_novis` lists first
showed different leaves: the verification copy of GL1's `R_MarkLeaves`
had left out its re-mark when `r_showcluster` changes, which the lists
turn on; with it, none.)

Results (the final build; idle machine: other processes 1.6 CPUs at
most; the main thread's time in the renderer per frame, with its front end
(`scene`) and the 2D calls with the 3D back end; GPU time from timestamps;
medians of 5 interleaved runs for the fixed views and `o4match` at
1920x1080, of 3 for the other demo rows, 15000 frames each, whose
"content" percentiles are those of each frame's median over the runs, see
Frame spikes; ms):

| view | size | renderer before | after | scene before / after | 2D and back end before / after | GPU before / after | content p99 before / after | p99.9 before / after |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `obj_team2` yard | 640x480 | 0.398 | 0.360 (-10 %) | 0.115 / 0.105 | 0.208 / 0.186 | 0.224 / 0.220 | | |
| | 1920x1080 | 0.402 | 0.393 (-2 %) | 0.113 / 0.113 | 0.217 / 0.209 | 0.623 / 0.622 | | |
| `dm/mohdm3` overview | 640x480 | 0.446 | 0.451 (+1 %) | 0.108 / 0.118 | 0.264 / 0.259 | 0.295 / 0.294 | | |
| | 1920x1080 | 0.524 | 0.529 (+1 %) | 0.133 / 0.138 | 0.321 / 0.319 | 0.700 / 0.702 | | |
| `o2match` | 640x480 | 0.359 | 0.344 (-4 %) | 0.067 / 0.063 | 0.221 / 0.211 | 0.277 / 0.278 | 1.037 / 1.035 | 1.446 / 1.381 |
| | 1920x1080 | 0.389 | 0.371 (-5 %) | 0.074 / 0.068 | 0.242 / 0.231 | 0.554 / 0.550 | 1.149 / 1.109 | 1.548 / 1.536 |
| `o4match` | 640x480 | 0.748 | 0.736 (-2 %) | 0.219 / 0.215 | 0.455 / 0.448 | 0.365 / 0.363 | 1.368 / 1.373 | 1.677 / 1.733 |
| | 1920x1080 | 0.887 | 0.876 (-1 %) | 0.289 / 0.282 | 0.523 / 0.517 | 0.749 / 0.745 | 1.671 / 1.648 | 2.105 / 2.018 |

(The frame times stay at the 1000 frames per second limit in the fixed
views and at 1.003 to 1.006 ms in `o2match`; `o4match`'s go from 1.051 to
1.045 and from 1.159 to 1.153.) The `dm/mohdm3` overview's 1 % is within
its run-to-run spread (0.440 to 0.471 before, 0.449 to 0.455 after at
640x480; earlier batches gave from -0.5 to +3 %). The slow frames of the
demos compared frame by frame (the 150 frames where the two builds'
medians add up highest; selecting by one build's frames alone favours the
other by regression to the mean): 0.046 ms shorter on average in
`o4match` at 1920x1080 (5 runs; 3 runs gave 0.070 longer, and
`o4match`'s content percentiles move by as much from one batch to the
next), 0.023 longer at 640x480, 0.051 and 0.004 shorter in `o2match`.
Between the build without the last condition of change 1 (no view
culling where dynamic lights reach) and the final one, in the same
batch: `o2match` 0.339 and 0.374 against 0.344 and 0.371, `o4match`
0.736 and 0.861 against 0.736 and 0.869; `o2match`, whose recorder fires
often, culls 9 % fewer faces against the view with it (614 per view
against 676).

Each change against the build before it (separate interleaved batches,
the same columns; renderer time, ms):

| view | size | before | 1. faces against the view | 2. leaf list | 3. face data in `msurface_t` | 4. PVS list | lit leaves kept |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `obj_team2` yard | 640x480 | 0.391 | 0.362 | 0.362 | 0.367 / 0.368 * | 0.361 / 0.356 | |
| | 1920x1080 | 0.398 | 0.390 | 0.380 | | 0.387 / 0.383 | |
| `dm/mohdm3` overview | 640x480 | 0.439 | 0.452 | 0.444 | 0.448 / 0.449 * | 0.443 / 0.454 | |
| | 1920x1080 | 0.534 | 0.535 | 0.531 | | 0.536 / 0.534 | |
| `o2match` | 640x480 | 0.362 | 0.349 | 0.342 | 0.347 | 0.342 / 0.337 | 0.339 / 0.344 |
| | 1920x1080 | 0.385 | 0.379 | 0.372 | 0.367 | 0.369 / 0.363 | 0.374 / 0.371 |
| `o4match` | 640x480 | 0.738 | 0.747 | 0.731 | 0.731 | 0.731 / 0.731 | 0.736 / 0.736 |
| | 1920x1080 | 0.866 | 0.877 | 0.876 | 0.852 | 0.855 / 0.848 | 0.861 / 0.869 |

(Changes 1 to 3 in one batch for each kind of view; * a batch of 3 runs
with the build before and after change 3; change 4 against the build with
changes 1 to 3 and the inline box test, which was worth 3 to 5 µs of the
front end in the overview and `o4match` on its own; the last column is
the condition of change 1 that keeps the faces where dynamic lights reach
and with `r_shownormals`, which changes nothing in the fixed views, which
have no dynamic lights.)

- Culling faces against the view saves back-end time (the static geometry
  had already made a world face cheap there, about 11 ns each in the
  yard) and a little GPU time at 640x480, and costs a test per face in the
  front end: it pays where many faces are outside the view (the yard and
  `o2match`, half the faces GL1 drew) and cost a little in the overview
  and `o4match` (16 and 20 %) until change 3 took the reads of the faces
  out of the culling. Static views keep the world's data in the caches,
  so change 3 shows in the demos only.
- The leaf list makes more box tests than the recursion where the PVS is
  large and the frustum culls most of it (`o4match`: `BoxOnPlaneSide` and
  the leaf tests 3.1 % of the samples against 1.3 %) and saves the node
  visits: about even, a little better with the box test inline. Groups
  of 4 or 16 leaves gave the same within the noise.
- What is left of the traversal in `o4match`: the leaves' surface lists
  (`R_AddWorldLeaf`, 3.6 % of the samples), the first read of each drawn
  face (now in `R_CheckDlightSurface`, which writes its dynamic light
  fields, 1.1 %; the back end reads the face anyway), the culling (1.4 %)
  and the box tests (3.1 %).

Present fences (2026-10-18, Plan 15; see Implementation notes,
Presentation semaphores and swapchains): what the known risks' fixes cost
on the main path is the present fence in `VkPresentInfoKHR` (Mesa adds it
to the submission it makes for every present anyway) and, after the
present and the acquire of the next image, one `vkGetFenceStatus` and one
`vkResetFences` for the next present's fence (two slots in use on RADV);
nothing between a frame's input read and its present, the acquire where it
was, no frame more in flight. The fallbacks are off on RADV. Measured on
the idle machine (other processes 0.7 to 1.9 CPUs, no Plex transcoding),
1920x1080 under Xwayland, the build before (`base`) and this one
(`new`) interleaved, 5 runs of 10 s each (`perf.py --list ... --binaries
openmohaa,openmohaa_fs --caps 0,250 --seconds 10 --runs 5 --preload
libinlat.so --framelog DIR`, `openmohaa_fs` a wrapper adding `+set
r_fullscreen 1 +set in_nograb 0`; fullscreen is immediate, with tearing).
Input read to present (`libinlat.so`), ms: the median of the runs'
medians (their range), the median of their 99th percentiles:

| view | `com_maxfps` | variant | base: median (runs), p99 | new: median (runs), p99 |
| --- | --- | --- | --- | --- |
| `obj_team2` yard | 0 | window | 0.410 (0.399 to 0.416), 0.631 | 0.411 (0.402 to 0.425), 0.618 |
| | 0 | fullscreen | 0.445 (0.438 to 0.462), 0.769 | 0.444 (0.433 to 0.457), 0.735 |
| | 250 | window | 0.540 (0.535 to 0.542), 0.929 | 0.529 (0.524 to 0.570), 0.944 |
| | 250 | fullscreen | 0.535 (0.530 to 0.549), 0.921 | 0.541 (0.526 to 0.545), 0.917 |
| `o2match` | 0 | window | 0.499 (0.489 to 0.517), 1.124 | 0.498 (0.484 to 0.506), 1.164 |
| | 0 | fullscreen | 0.524 (0.487 to 0.534), 1.299 | 0.533 (0.516 to 0.577), 1.326 |
| | 250 | window | 0.566 (0.555 to 0.576), 1.062 | 0.551 (0.547 to 0.565), 1.025 |
| | 250 | fullscreen | 0.568 (0.549 to 0.576), 1.046 | 0.562 (0.546 to 0.571), 1.026 |

The medians differ by -0.015 to +0.009 ms, inside the spread of the runs
either way: no change to measure. The build timed here had the stride 0
bindings follow `vertexAttributeAccessBeyondStride` on portability
devices; the final one differs from it only in `VK_CreateLogicalDevice`
(and the function it is inlined into): every other function is the same
code, instruction for instruction (`objdump -d`, addresses left out).

Frame time and the renderer's time uncapped (`perf.py --list ...
--sizes 1920x1080,3840x2160 --renderers base,final --runs 3`, medians of
3 interleaved runs with their range, ms; GPU: the main command buffer,
timestamps; end: `EndFrame`, which submits, presents, acquires the next
image and now chooses the next present's fence; other processes 0.9 to
2.0 CPUs):

| view | size | frame base | frame new | renderer base | renderer new | end base / new | GPU base / new |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `obj_team2` yard | 1920x1080 | 0.999 | 0.999 | 0.380 (0.372 to 0.403) | 0.394 (0.381 to 0.410) | 0.069 / 0.075 | 0.621 / 0.624 |
| | 3840x2160 | 1.724 (1.719 to 1.732) | 1.713 (1.710 to 1.714) | 1.567 | 1.569 | 0.125 / 0.122 | 1.708 / 1.697 |
| `o2match` | 1920x1080 | 1.005 | 1.005 | 0.376 (0.370 to 0.379) | 0.373 (0.373 to 0.376) | 0.073 / 0.076 | 0.551 / 0.552 |
| | 3840x2160 | 1.326 | 1.330 | 0.907 | 0.906 | 0.085 / 0.091 | 1.261 / 1.265 |
| `o4match` | 1920x1080 | 1.121 (1.121 to 1.135) | 1.139 (1.123 to 1.162) | 0.843 (0.843 to 0.856) | 0.860 (0.844 to 0.883) | 0.075 / 0.080 | 0.729 / 0.738 |
| | 3840x2160 | 2.240 | 2.236 | 1.959 | 1.963 | 0.102 / 0.105 | 2.203 / 2.200 |

The yard again with 5 runs of 5000 frames: renderer 0.355 (0.351 to
0.362) against 0.362 (0.360 to 0.367) at 640x480, 0.391 (0.387 to 0.394)
against 0.388 (0.380 to 0.395) at 1920x1080. In every case the present
call (`vkinfo`'s `presentUs`) takes 1.1 µs more, 11.0 to 13.4 µs against
12.1 to 14.6 (Mesa signals one fence more in the submission it makes
there), and `EndFrame` 3 to 6 µs more in all (the fence's status and its
reset after the present); the rest moves both ways within the noise.
That is the cost: about 4 µs a frame after the input's present, of which
1 µs inside the present call, after the moment `libinlat.so` takes as
the present.

Pipelines and skin data at load (2026-10-18, Plan 16; see Implementation
notes, Pipelines at load and Skeletal models). Three cases: the first play
after an install (cold caches: no `vkpipelines.cache`, no
`vkpipelines.keys`, `MESA_SHADER_CACHE_DISABLE=true`, `perf.py --cold`),
the first play after a driver update (the same with `vkpipelines.keys`
kept, `--keep-keys`), and every later play (warm caches). Idle machine
(other processes at most 1.1 CPUs, no Plex transcoding), 1920x1080 under
Xwayland, the build before (`base`) and this one (`new`) interleaved with
frame log copies of both (`--vklog`), 5 runs of the two match demos (3 for
the driver update), medians of the runs. Map load: `CL_InitCGame`'s time
(the map, cgame's registration, `EndRegistration`); the first frames after
it: the sum of the first 10, the slowest of the first 30; pipelines and
skin data added during the 15000 measured frames (`vkinfo`; the base build
added the skin data at first draws, which it doesn't count); ms:

| demo | caches | build | map load (s) | pipelines at load (ms) | first 10 frames | slowest of the first 30 | p99 | p99.9 | max | pipelines in the match (ms) | skin data in the match |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `o2match` | cold | base | 1.33 | 7 (11) | 43.7 | 33.6 | 1.161 | 2.080 | 33.6 | 39 (60) | at first draws |
| | | new | 1.51 | 157 (179) | 20.8 | 10.5 | 1.167 | 1.556 | 21.2 | 2 (3.6) | 12 (214 KB), when registered |
| | driver update | new | 1.51 | 159 (183) | | | 1.157 | 1.544 | 21.0 | 0 | 12 (214 KB) |
| | warm | base | 1.33 | 7 (0.2) | 18.2 | 8.2 | 1.166 | 1.558 | 21.0 | 39 (0.9) | at first draws |
| | | new | 1.33 | 159 (1.0) | 16.5 | 7.0 | 1.150 | 1.554 | 21.2 | 0 | 12 (214 KB) |
| `o4match` | cold | base | 1.23 | 7 (14) | 80.2 | 58.4 | 1.832 | 3.658 | 58.4 | 49 (80) | at first draws |
| | | new | 1.43 | 177 (196) | 24.4 | 9.4 | 1.878 | 2.761 | 28.5 | 2 (3.6) | 7 (106 KB), when registered |
| | driver update | new | 1.44 | 179 (199) | | | 1.764 | 2.276 | 28.1 | 0 | 7 (106 KB) |
| | warm | base | 1.24 | 7 (0.2) | 24.9 | 8.5 | 1.821 | 2.452 | 28.0 | 49 (0.9) | at first draws |
| | | new | 1.24 | 179 (1.0) | 21.2 | 6.0 | 1.805 | 2.460 | 28.7 | 0 | 7 (106 KB) |

(The ranges over the runs: the cold maxima 33.2 to 34.5 and 58.1 to 59.2
ms before, 21.0 to 22.0 and 28.3 to 29.2 after; the warm 20.9 to 21.1 and
27.9 to 28.3 before, 20.9 to 21.7 and 28.0 to 31.1 after.)

- With cold caches the 39 and 49 pipelines that the matches created on
  first use (60 and 80 ms: 21 and 38 of them in the first 30 frames, 29
  and 53 ms, the others in 10 and 11 frames of the match, up to 7.6 ms in
  one) are created at load instead: 157 and 177 pipelines in 179 and 196
  ms, the map load 0.18 and 0.20 s longer (+14 and +16 %). The first
  frames after the load take 21 and 24 ms in all instead of 44 and 80, the
  slowest of them 10 and 9 ms instead of 34 and 58; the 99.9th percentile goes from 2.08 to 1.56
  ms and from 3.66 to 2.76 ms, the warm plays' values within the run to run
  spread (o4match's 99.9th percentile spreads more, 2.28 to 2.76 ms, from
  batch to batch); the maximum from 33.6 to 21.2 and from 58.4 to 28.5 ms,
  which is registration in the match (shared with GL1, see below and
  Remaining), the same as with warm caches. In `framelog.py --batch`'s
  causes of the slow frames (those above each run's 99th percentile),
  pipelines were 27 to 30 % of the excess in both matches and are 1.5 to
  2.4 % (the head icons).
- What the first play still creates in the match: 2 pipelines (3.6 ms), in
  the first frame, for the head icon sprites (`textures/hud/*_headicon`, a
  sprite model cgame registers when it first draws one; their alpha test
  and blend combination isn't any load shader's). After a driver update,
  and in every later play, they are recorded keys and built at load: 0.
- With warm caches the pipelines of the load come from the pipeline cache:
  152 and 172 more in 1 ms, the map load unchanged; the 0.8 to 0.9 ms they
  cost in the match is gone, and the first frames are shorter by 1.7 and
  3.7 ms (pipelines and the skin data of the first draws).
- The skin data is added when a model is registered: at load for the
  models of the gamestate (234 additions, 3.8 MB in `o2match`, where the
  base build added 1.6 MB over the whole match at first draws: the load
  now also adds the models that the match never draws), and during the
  match the 12 and 7 additions (214 and 106 KB) of the models announced
  then, all in frames that register (their frame log records have
  registration time): no first draw adds any. The skin data buffer is 32
  MB; 4.3 MB was the most a load used in the six demos below.
- The frame time otherwise: the same within the noise (the median of the
  runs' means 1.005 and 1.004 ms, 1.141 and 1.142 ms warm; renderer time
  0.377 and 0.380, 0.862 and 0.868 ms, ranges 0.375 to 0.384 and 0.371 to
  0.389, 0.839 to 0.899 and 0.860 to 0.894).

How complete the load's pipelines are, checked with a temporary build that
printed every pipeline a draw created and every shader registered after the
load, on six matches of four maps (`o2match`, `o4match`, a second match of
each, `obj/obj_team1`, `dm/mohdm6`), a fresh homepath for each: the base
build created 33 to 49 pipelines after the load, the new one creates none
at a draw; 2 come in the first frame for the head icons and 1 when the
menu opens at the end of a demo (`menu_button_trans`, which a real session
has created in the main menu); with the keys file of the six runs, none.
With `r_vkFallbacks 3` (keys with the RGB bit of every texture),
`r_dlightBacks 0`, `r_fastdlights 1`, `r_gpuSkinning 0` and
`r_ext_multisample 4` (the final build, `o2match` and `o4match`, no keys
file) the same: the 2 head icon pipelines after the load, none at a draw
(201 to 225 pipelines at load with the fallbacks, 172 to 198 with
`r_dlightBacks 0`, 146 to 166 without GPU skinning, which then adds no
skin data). The loads create 122 to 173 pipelines (fogged maps more: their fog isn't
known at load, so both), of which 31 to 50 (20 to 29 %) are used in the
match; unused ones are destroyed at the next registration. The first
version of the prediction missed the immediate mode clear to the fog color
and the UI boxes (now built, see Implementation notes) and built shaders
registered later with both fogs and static geometry variants (4 pipelines
for a head icon instead of 1). Memory on RADV: 178 more pipelines left the
game's resident memory 50 KB larger and its device memory (amdgpu's
`fdinfo`) unchanged: pipelines that differ only in fixed-function state
share their shaders.

The `obj_team2` yard (`views/perf.txt`, a fixed view; 5 runs each): with
cold caches the map load takes 1.07 s instead of 0.90 (157 pipelines at
load, 166 to 167 ms of them created before their draw), against the base
build's 20 pipelines by the time the view is set up (the loading screen's
7 and 13 in the first frames, 23 to 25 ms); none after it in either. Warm,
the map load is 0.90 and 0.90 to 0.91 s and the renderer's time per frame
the same: 0.359 and 0.358 ms at 640x480, 0.385 and 0.386 ms at 1920x1080.

Input read to present (`libinlat.so`, `perf.py --caps 0,250 --seconds 10
--runs 5 --preload libinlat.so --framelog`, 1920x1080 in a window, the
yard and `o2match`; ms, the median of the runs' medians with their range,
the median of their 99th percentiles):

| view | `com_maxfps` | base | new |
| --- | --- | --- | --- |
| `obj_team2` yard | 0 | 0.408 (0.406 to 0.416), 0.602 | 0.409 (0.405 to 0.411), 0.612 |
| | 250 | 0.530 (0.518 to 0.562), 0.917 | 0.530 (0.520 to 0.545), 0.910 |
| `o2match` | 0 | 0.498 (0.495 to 0.506), 1.141 | 0.508 (0.494 to 0.512), 1.110 |
| | 250 | 0.560 (0.557 to 0.564), 1.040 | 0.558 (0.557 to 0.570), 1.047 |

Nothing was added between a frame's input read and its present: the
pipelines are created at the end of the load (between frames), or for a
shader registered in a frame when it is registered, which is no later
than its first draw would have created them; the skin data when a model is
registered; per frame only the pipeline's used flag is stored at each
pipeline lookup that isn't the previous draw's pipeline; no frame more in
flight, the acquire where it was. The medians differ by -0.002 to +0.010
ms, inside the spread of the runs.

What is left of the slowest frames: registration in the match (Remaining,
item 1), shared with GL1, measured in the same demos with warm caches (the
frame logs of the 5 runs above; stack samples of `libstacks.so` in 2 more
runs per renderer, `perf.py --stacks`). `o2match` has 6 frames whose
registration (`load`) takes 0.3 ms or more, 39 ms of registration in all,
the slowest of them 21 ms with 19 ms of registration; `o4match` 3 frames,
26 ms, the slowest 28.6 ms with 24 ms of registration (GL1: 7 frames and
49 to 50 ms, the slowest 26 ms; 4 frames and 31 ms, the slowest 32 to 33
ms); 26 to 36 textures are created after the load (8 to 11 MB). Where the
main thread is in those frames' registration exports (134 samples of
Vulkan's 4 runs, 155 of GL1's, each attributed to the first of these
functions from its leaf up):

| | Vulkan | GL1 |
| --- | --- | --- |
| zlib decompressing the images from the pk3s (`inflate`, `unzReadCurrentFile`) | 48 % | 47 % |
| the file system's lookups (`FS_FOpenFileRead`, `FS_ReadFile`, the pk3 directories) | 17 % | 13 % |
| its case correction (`FS_CorrectCase`: `opendir`/`readdir` of each search directory that lacks the path) | 11 % | 10 % |
| mipmaps and scaling on the CPU (`R_MipMap`, `ResampleTexture`, `R_LightScaleTexture`; GL1's uploads) | 10 % | 21 % |
| TIKI files (the engine's skeletal models) | 7.5 % | 3 % |
| shader parsing, the sound | 2 % | 4 % |
| the skin data, now built when the model is registered (`R_SkinTiki`) | under 1 % | |
| TGA and JPEG decoding itself | about 1 % | 3 % |

The registration takes as long as before (0.3 to 0.7 ms more in all per
match: the skin data of the models announced in the match, 214 and 106 KB,
which their first draws added before), and the frames that used to add
skin data and create pipelines at a first draw don't any more.

Skeletal culling (2026-10-19, Plan 17; see Implementation notes, Skeletal
culling). GL1 prepares every skeletal model of the scene, in view or not:
its bones, morphs and surfaces, then in the back end its lighting (the sun
trace of a moving entity, the sphere's lights) and its draws, which the GPU
clips away. JayRewd's fork (b26d8a67) returns from `R_AddSkelSurfaces`
before the bones when the sphere of the model's animation frame, with its
radius plus 16 times the entity's scale, is outside the view, and reported
-22 and -61 µs per frame in `o2match` and `o4match`. The frame radius comes
from the animation files and isn't a bound of the mesh, so before taking it
a temporary verification build (not kept) measured how far the vertexes
really reach. It drew every model as before and, for every model the cull
could reject (not the player's own body, the view weapon, mirror views or
`lod_tool`), computed each vertex of each drawn surface as `RB_SkelMesh`
does (bones, weights, morphs; every vertex, so every level of detail,
whose vertexes are a subset) in world space with the entity's axes; it
recorded per TIKI how far the vertexes reach from the sphere's center, and
for a set of candidate margins checked each model the sphere would cull:
all its vertexes must lie outside one plane of the clip volume (x = ±w or y
= ±w, in double precision, by more than float32's rounding; the world
culling's check), else no triangle of it can be in view. It ran over 20
matches of real players played frame by frame (`fixedtime 16`, 1920x1080, up
to 40000 frames each: 5 of `obj/obj_team2` with `o2match`, 5 of
`obj/obj_team4` with `o4match`, 4 of `obj/obj_team1`, 4 of `dm/mohdm6`, one
of `dm/mohdm4` and of `obj/obj_team3`; a 21st switched to a map the game
data doesn't have) and over the 41 view lists (single-player AI, vehicles,
the landing craft, the convoy, the sky portal, the soldier views):

| content | models checked | culled by the final rule | with a vertex in view | closest culled model | radius alone: with a vertex in view | JayRewd's rule: with a vertex in view |
| --- | --- | --- | --- | --- | --- | --- |
| 20 matches | 11.68 M | 6.12 M (52.4 %) | 0 | 1.16 px outside the edge | 151 | 0 |
| 41 view lists | 1.22 M | 0.53 M (43.6 %) | 0 | 5.09 px | 0 | 23 |

What the radius misses, the largest reaches found (from the sphere's
center, against the frame's radius; TIKI units are world units divided by
the TIKI's `load_scale` times the entity's scale):

- players, while the bone controllers turn the torso and the head (aiming
  up or down, throwing a grenade): up to 1.20 times the radius, 35 TIKI
  units past it (`german_winter_1` in a `dm/mohdm6` match: 110.4 units
  for a radius of 92.1; `allied_airborne` 109.2, `american_ranger` 108.0);
  the bones alone reach 11 units past it, so the controllers, not the
  skin, make most of it; the morphs (faces) move vertexes by 0.5 units at
  most. Without the controllers 1.09 at most (the soldier views and the
  single-player AI 1.07);
- the stick grenade: its frame radius is 4.7 units, its vertexes reach
  14.0 (2.96 times);
- muzzle flashes are drawn with axes 2.4 times longer than unit vectors,
  splash drops (`fx/water_splash_drop.tik`) with axes up to 69 times longer
  and with a negative scale (mirrored): measured in world space their
  vertexes reach 2.3 and 44 times the radius, and a negative scale makes
  the radius negative, which turns the sphere test around (it culls what is
  inside). JayRewd's rule culled such drops in view in `m3l1a` (23 cases,
  the drops of the beach) and hasn't the margin for the players either:
  the worst one above is 2.3 units past his sphere (it was out of view
  then);
- vehicles, weapons, items, debris: within their radius.

The final rule: the radius (absolute) times 1.25 plus 32 TIKI units
(absolute), times how much the entity's axes can lengthen a vector, and
only the four side planes. With it the farthest vertex reaches 0.84 of the
sphere (the `german_winter_1` above: 110.4 of 131.8 units), the grenade
0.56; 1.25 + 32 culls 52.4 % of the matches' models against 54.6 % for the
radius alone and 53.2 % for JayRewd's rule (51.1 % for 1.5 + 48, 49.3 %
for 2 + 64): most culled models are far outside the view, so the margin
costs few culls. Models whose shaders deform vertexes are kept (0.2 % of
the culls: the bullet hole puffs).

Results (idle machine: other processes 1.0 CPU at most, no Plex
transcoding; the build before (`base`) and this one interleaved, medians
of 5 runs; the renderer's main-thread time per frame, its front end
(`scene`) and the 2D calls with the 3D back end, the GPU time of the main
command buffer and the draws per frame; the soldier views 1000 frames, the
demos 15000; ms):

| view | size | renderer base | new | scene base / new | 2D and back end base / new | GPU base / new | draws base / new |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `pm1_front` | 640x480 | 0.565 | 0.540 (-4 %) | 0.145 / 0.136 | 0.345 / 0.332 | 0.516 / 0.516 | 553 / 531 |
| `pm1_side` | 640x480 | 0.359 | 0.236 (-34 %) | 0.107 / 0.052 | 0.179 / 0.115 | 0.393 / 0.299 | 315 / 174 |
| `po1_front` | 640x480 | 0.381 | 0.351 (-8 %) | 0.119 / 0.103 | 0.187 / 0.175 | 0.422 / 0.413 | 343 / 299 |
| `po4_front` | 640x480 | 0.447 | 0.390 (-13 %) | 0.152 / 0.135 | 0.219 / 0.182 | 0.388 / 0.373 | 333 / 285 |
| `pm7_flash` | 640x480 | 0.494 | 0.457 (-8 %) | 0.147 / 0.129 | 0.274 / 0.248 | 0.383 / 0.358 | 386 / 334 |
| `o2match` | 640x480 | 0.341 | 0.319 (-6.5 %) | 0.061 / 0.057 | 0.206 / 0.190 | 0.273 / 0.264 | |
| `o4match` | 640x480 | 0.720 | 0.659 (-8.5 %) | 0.207 / 0.193 | 0.436 / 0.391 | 0.363 / 0.341 | |
| `pm1_front` | 1920x1080 | 1.085 | 1.071 | 0.168 / 0.163 | 0.836 / 0.825 | 1.225 / 1.222 | |
| `po4_front` | 1920x1080 | 0.482 | 0.428 (-11 %) | 0.171 / 0.153 | 0.239 / 0.200 | 0.780 / 0.774 | |
| `o2match` | 1920x1080 | 0.365 | 0.345 (-5.5 %) | 0.067 / 0.062 | 0.225 / 0.210 | 0.537 / 0.526 | |
| `o4match` | 1920x1080 | 0.833 | 0.779 (-6.5 %) | 0.265 / 0.247 | 0.493 / 0.452 | 0.721 / 0.703 | |

The frame times of the demos (medians of the 5 runs' percentiles, and
"content", each frame's median over the runs, see Frame spikes), ms:

| demo | size | mean base / new | p99 base / new | p99.9 base / new | content p99 base / new | content p99.9 base / new | max base / new |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `o2match` | 640x480 | 1.003 / 1.003 | 1.033 / 1.030 | 1.291 / 1.253 | 1.031 / 1.030 | 1.271 / 1.227 | 21.3 / 20.9 |
| `o4match` | 640x480 | 1.032 / 1.017 | 1.323 / 1.261 | 1.669 / 1.615 | 1.295 / 1.238 | 1.599 / 1.558 | 27.7 / 28.8 |
| `o2match` | 1920x1080 | 1.004 / 1.003 | 1.112 / 1.059 | 1.532 / 1.457 | 1.073 / 1.044 | 1.446 / 1.314 | 21.5 / 21.2 |
| `o4match` | 1920x1080 | 1.111 / 1.081 | 1.631 / 1.555 | 2.134 / 2.019 | 1.553 / 1.500 | 1.892 / 1.864 | 28.8 / 30.2 |

(1.000 is the 1000 frames per second limit, which most frames of
`o2match` reach; the maxima are registration in the middle of the match,
see Remaining: 27.6 to 30.8 ms from run to run in both builds.)

- What is saved per culled model: its bones (`R_GetFrame`, the engine's
  skeletor), morphs and surfaces in the front end, and in the back end its
  lighting sphere (with the sun trace of a model that moves) and one draw
  per surface (`vkinfo`: 553 to 531 draws in `pm1_front`, 315 to 174 in
  `pm1_side`, which looks away from the 16 soldiers of the front view). The
  GPU time goes down a little too (the culled triangles were set up and
  clipped). In the matches 6.3 and 10.1 models a view are culled (counted
  with a temporary timing build), 52 % of the skeletal models in all the
  matches above.
- The pose isn't skipped: the frame radius comes from it (`SetPose` takes
  the largest radius of the animation frames it blends), so culling before
  it would need a bound of every animation the model can play, or the
  engine giving the radius without posing. It costs the culled models 1.0
  and 1.9 µs a view in `o2match` and `o4match` (the temporary timing build:
  pose and radius of 6.1 and 8.5 models a view that cgame hadn't posed
  already for their tags), against 1.6 µs a model for the bones, morphs
  and surfaces that are left out in the front end alone.
- Input read to present (`libinlat.so`, `perf.py --caps 0,250 --seconds 10
  --preload libinlat.so`, 1920x1080 in a window, 5 runs; ms, base / new):
  `o2match` uncapped median 0.481 / 0.465, p99 1.067 / 1.031, p99.9 1.429
  / 1.369, at 250 0.534 / 0.523, 0.942 / 0.941, 1.324 / 1.323; `po4_front`
  uncapped 0.550 / 0.515, 0.810 / 0.741, 1.105 / 0.935, at 250 0.647 /
  0.589, 0.943 / 0.894, 1.114 / 1.110. Lower, never higher: the cull only
  removes work between the input read and the present; nothing else
  changes (no frame more in flight, the acquire where it was).

Command rate (2026-10-19, Plan 18; an engine, client and cgame change,
for both renderers). The client made one usercmd per frame, and a
usercmd is a step of the player's movement (pmove, run by the server and
predicted by cgame) as long as the frame: 1 ms at 1000 frames a second, 4
ms at 250 (and 2 and 5 ms when the clock drifts, `CL_AdjustTimeDelta`), 7
ms at 144 (4 to 8), 14 to 18 ms at 60. The frame rate was held at 1000 a
second (`Com_Frame` waited for a new millisecond of `Sys_Milliseconds`,
since `Com_ModifyMsec` counted a frame of 0 ms as 1 ms and the game would
have run ahead of real time). Players report that the short steps of high
frame rates get stuck on small bumps of `dm/mohdm2` that longer steps get
over; JayRewd's omaha commit 1620f310 (`cl_cmdfps`) separated the command
rate from the frame rate, and his b26d8a67 let the frame rate pass 1000.
What was taken, and how it differs:

- Usercmds at a fixed rate whatever the frame rate (`CL_CreateNewCommands`):
  `cl_cmdfps` (archived, 85 to 125 a second, default 100), a command every
  1000 / `cl_cmdfps` whole milliseconds of server time (8 to 11 ms, 10 by
  default, the user's choice from the bump test below; 12 ms, 83 a second,
  is outside the range). JayRewd's waits until that much
  server time has passed since the last command and stamps it with the
  frame's time, so its step is the first multiple of the frame time at or
  above it (by its code about 14 ms at 144 frames a second, 8 at 250).
  Here each command is stamped one step after the one before, so every
  step has the same length at every frame rate, also when the clock
  drifts: a frame that passes no step makes no command, a frame longer
  than a step makes several with the same input (the weapon command, which
  cgame sends in three commands, is asked for each), up to 100 ms of them:
  below 10 frames a second, after a hitch or when the server time jumps,
  one command of all of it as before, so that a packet with the last
  packet's commands (`cl_packetdup`) stays within `MAX_PACKET_USERCMDS`.
  Until the first snapshot (`CA_PRIMED`) one per frame as before.
- The view turns every frame: `CL_ApplyLook` applies the mouse, the keys
  that turn and the joystick every frame, and cgame turns the view with
  this frame's angles through a new import, `GetViewUserCmd` (the newest
  command with this frame's view angles), after it has predicted the
  commands with the angles they were sent with (`CG_PredictPlayerState`,
  and `CG_InterpolatePlayerState` for noclip and spectators). No command
  changes once it is made; JayRewd's writes each frame's angles into the
  newest command, sent or not, and predicts it with them, so cgame's
  prediction of that command differs from the server's. What the mouse
  adds to the movement (strafe, `BUTTON_ANY`) waits for the next command.
  Buttons, taps and weapon commands are read only when a command is made,
  so none is lost between commands; a key held for part of a command's
  time moves it by that part, as before (JayRewd's counts a tap as the
  whole command and resamples the keys into the newest command for
  prediction, input the server never gets: not taken).
- No packet without a new command unless reliable commands wait
  (`CL_SendCmd`): a frame between commands sends nothing.
- cgame: in the frames between commands the snapshot can have run every
  command already, and its playerState lacks what the network doesn't
  carry, which cgame then took from the snapshot (`walking` false in a
  `nextSnap`: the view bob, the view model and the landing would flicker).
  It now takes `walking`, `groundPlane` and `groundTrace` from its last
  pmove of the same command and the rest from the snapshot (JayRewd's keeps
  the whole predicted state, the server's position included).
- Not taken: JayRewd's display lerp of the own position between the last
  two commands (it shows the player up to a step behind), extrapolation, a
  partial step ahead of the last command. The own position moves once per
  command, every 10 ms at the default (every second or third frame at 250
  frames a second, every tenth at 1000), and lags the world drawn at the
  frame's time by 0 to 10 ms, as at 100 frames a second before; the view
  turns every frame, the other players are interpolated every frame.
- What the fixed rate costs: a button (fire, use, jump) or a movement key
  reaches the server with the next command, 0 to 10 ms after the frame
  that read it at the default (5 on average), where one command per frame
  sent it within 4 ms at 250 frames a second (2 on average) and within 1 ms
  at 1000: 3 ms more on average than at 250. The view's turning is in
  every frame and goes to the server with the next command, as before
  with the frame's.
- The limiter (`Com_Frame`): at a timescale of 1, a client cap above 1000
  waits for its deadline in microseconds alone, an uncapped client
  (`com_maxfps 0`) for nothing, and `Com_ModifyMsec` no longer counts a
  frame of 0 ms as 1 ms (JayRewd's b26d8a67 and 1620f310; his also drop
  the millisecond at other timescales, where a frame of 0 ms still counts
  as 1 ms, so scaled time, a demo in slow motion for one, would run fast:
  they keep it here). Caps up to 1000 (the default is 85) and dedicated
  servers are as before. The `fps` counter takes the frame times in
  microseconds (`Sys_Microseconds`; whole milliseconds over 16 frames
  showed 1455 or 1600 at 1515 frames a second, 2667 or 3200 at 3000).
  The world is still drawn at whole milliseconds of game time (`cg.time`):
  a frame in the same millisecond as the one before shows the same moment
  of the world with the newest view. One division by the frame time in
  cgame (the water ripples) is guarded against 0.

The bump. A temporary cgame command (not kept) walked the player's pmove
the way the server runs it (`Player::ClientMove`: no ground entity before
the move, the velocity clipped to the ground plane after it, a move
shorter than 0.005 units undone; cgame's prediction does none of these)
from every floor of a grid over `dm/mohdm2` in 8 directions for 1.5 s at
each step and recorded where each walk ended. Walks that stopped short
where another step from the same start went on (under half of the longest
of them), of 29,315 (a 96-unit grid; the 64-unit grid at 4, 8 and 12 ms
gave the same order):

| step (ms) | 1 | 4 | 8 | 9 | 10 | 11 | 12 | 16 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| walks stopped | 193 | 103 | 66 | 58 | 48 | 57 | 59 | 47 |
| of them only at this step | 81 | 6 | 2 | 1 | 0 | 3 | 4 | 6 |

Then in the game, the server and the client's real commands (a test build
that set the step, `fixedtime 16`), three bumps where the shorter steps
stopped, each walked into from 29 positions (64 to 256 units before it, 8
units to either side): the walks that got more than 32 units past it in
2.5 s:

| bump (where, direction) | 1 ms | 4 | 8 | 9 | 10 | 11 | 12 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| C1 `-2624 331 38`, south: the edge where the dirt terrain meets the cobbles, at a house corner | 0 | 1 | 12 | 13 | 15 | 14 | 18 |
| C3 `-2817 -1485 18`, north | 27 | 21 | 25 | 25 | 27 | 27 | 26 |
| C7 `-1330 -3587 -6`, north-west | 19 | 17 | 17 | 18 | 20 | 19 | 24 |

Why, from pmove with debug prints at C1: the bump is a lip 1.5 units high
whose edge is a bevel of 70° about 0.55 units wide. When a move is
blocked, `PM_StepSlideMove` tries it again 18 units higher and traces back
down; when that trace lands on a surface too steep to walk on
(`MIN_WALK_NORMAL`: the bevel) the step is refused and the blocked move is
kept, whose slide along the bevel has taken nearly all the forward speed.
The raised move covers one command's distance from where the command
started, so the step is refused when the command started too close to the
lip for its distance to carry the box past the bevel, about the bevel's
width out of each step's length: at a running speed of 264 units a second,
0.55 out of 1.1 units at 4 ms, of 2.1 at 8 ms, of 3.2 at 12 ms. Once the
speed is gone a command from a standstill moves 0.17 units at 8 ms (0.38
at 12, 0.68 at 16): never past the bevel again, and the player stays there until
turning or jumping. So a shorter step stops more often at such an edge, a
longer one less, none never; other bumps stop longer steps more (C7 at 4
to 11 ms the same, the sweep's 16 ms as many as 10 ms). A fix would be in
pmove (the step-up refused on a short bevel), which the server runs, not
in the client.

Checks, with a test build (not kept) that logged every command, packet and
server move and cgame's view and prediction, in a local game of
`dm/mohdm2` (the player running in circles for 8.5 s with two button taps,
a weapon change and a use; `cl_cmdfps` 125, 8 ms, the default then, also in
the first latency batch below):

| `com_maxfps` | frames a second | usercmds a second | steps (ms) | usercmds per frame | packets without a new usercmd | server moves of 8 ms | before: steps (ms) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 60 | 60 | 125.3 | 8 (1045 of 1045) | 1 to 3 | 0 | all but the first | 14 to 18 |
| 144 | 144 | 125.2 | 8 | 0 or 1 | 0 | all but the first | 4 to 8 (993 of 1216 7 ms) |
| 250 | 250 | 125.0 | 8 | 0 or 1 | 0 | all but the first | 2, 4, 5 (169 of 2116 not 4) |
| 1000 | 1000 | 125.0 | 8 | 0 or 1 | 0 | all but the first | 0, 1, 2 (231 of 8483 not 1; the server runs no 0 ms one) |
| 0 | 3272 | 125.1 | 8 | 0 or 1 | 0 | all but the first | as at 1000 |

- Nothing lost between commands: each attack tap is in one command (two at
  60 frames a second, whose frames make two commands), the use tap too, the
  weapon change in three commands, at every cap, as before.
- The view turns in the frames without a command: while turning with the
  keys, the view's yaw changed in 88 of 120 such frames at 144, 579 of 802
  at 250, 4108 of 5675 at 1000 (keys turn by whole milliseconds, so frames
  within one millisecond don't turn; the mouse turns the view in every
  frame that has its input).
- The same walk at 144, 250, 1000 frames a second and uncapped (`+forward`
  from a standstill, into C1 and from the Allied spawn): the server's moves
  are the same to the bit at every cap (311 and 374 moves of 8 ms).
- Prediction: the server's result for each predicted command, at every
  cap, before and after: the same errors (median 0.02 to 0.04 units, 90th
  percentile 0.05 to 0.12, maximum 0.5 to 3.4) in both builds, from what
  the server does that cgame doesn't (above).

Latency (the build before against this one, interleaved, 5 runs of 10 s,
1920x1080 fullscreen with tearing, `r_swapInterval 0`, `libinlat.so` with
`INLAT_DISPLAY=1` and `INLAT_FBPOLL_US=250`, `framelog.py --display`; ms,
medians of the runs (their range) and of their percentiles; uncapped is
1000 frames a second before and 1500 now):

| view | `com_maxfps` | build | input to present: median, p99 | input to GPU done | input to display: median (runs), p99, p99.9 |
| --- | --- | --- | --- | --- | --- |
| `obj_team2` yard | 250 | before | 0.501, 0.810 | 1.398 | 1.925 (1.848 to 2.062), 2.562, 2.845 |
| | | now | 0.499, 0.823 | 1.381 | 2.010 (1.937 to 2.081), 2.509, 2.761 |
| | 1000 | before | 0.427, 0.620 | 0.982 | 1.502 (1.407 to 1.593), 1.753, 1.900 |
| | | now | 0.430, 0.640 | 0.985 | 1.602 (1.473 to 1.637), 1.852, 2.112 |
| | 0 | before | 0.428, 0.610 | 0.981 | 1.542 (1.402 to 1.674), 1.820, 2.041 |
| | | now | 0.627, 0.837 | 1.466 | 1.777 (1.732 to 1.809), 2.083, 2.192 |
| `o2match` | 250 | before | 0.518, 0.956 | 1.305 | 1.901 (1.849 to 1.948), 2.671, 3.140 |
| | | now | 0.523, 0.964 | 1.315 | 2.046 (1.927 to 2.084), 2.827, 3.136 |
| | 1000 | before | 0.501, 1.216 | 1.065 | 1.497 (1.456 to 1.613), 3.126, 3.428 |
| | | now | 0.495, 1.201 | 1.051 | 1.570 (1.458 to 1.583), 3.151, 3.507 |
| | 0 | before | 0.502, 1.212 | 1.056 | 1.531 (1.507 to 1.593), 3.143, 3.469 |
| | | now | 0.563, 1.221 | 1.194 | 1.545 (1.495 to 1.547), 3.161, 3.554 |

(One run of this build at 250 in `o2match` didn't go fullscreen and is
left out.) At 250 and 1000 the input to present and to GPU done are the
same; input to display was 0.07 to 0.15 ms higher in all four cells of
this batch, inside the runs' range, so a second batch with the final build
(`cl_cmdfps` 100) and 8 runs each: `o2match` 1.466 (1.404 to 1.598)
against 1.498 (1.413 to 1.628) ms at 1000 and 1.978 against 1.965 at 250,
the yard 1.551 against 1.579 and 1.995 against 2.039: the same within the
spread, both ways.

Frame times and CPU (the same runs, `renderer_timed.so`): at 250 and 1000
the same as before (the yard 4.000 and 0.999 ms on the median, 99th
percentile 4.099 and 1.091 against 4.098 and 1.090; `o2match` 1.266
against 1.283 at 1000; the game 0.45 and 1.08 CPUs in both builds).
Uncapped, 1498 frames a second in the yard and 1516 in `o2match` (0.666
and 0.626 ms on the median, 99th percentile 0.975 and 1.284, 99.9th 1.276
and 1.726), the GPU 87 and 81 % busy, the game's main thread a whole core.
On this machine (Radeon 780M) uncapped at 1920x1080 is where the GPU
limits, and a frame then waits for the one before it on the GPU (input to
GPU done 1.47 against 0.98 ms in the yard): uncapped beyond what the GPU
can draw shows the newest input later, not earlier. Where the GPU keeps up
(640x480, fullscreen with tearing, 5 runs), uncapped is what the change
was for: 2083 frames a second in the yard and 1816 in `o2match` against
the 1000 before, input to display 0.936 (0.930 to 1.043) against 1.257
(1.163 to 1.335) ms and 1.062 (1.048 to 1.072) against 1.470 (1.316 to
1.504), p99 1.155 against 1.510 and 1.915 against 1.973, input to GPU
done 0.606 against 0.739 and 0.724 against 0.865; at a cap of 1000 the
same as before (1.216 against 1.247, 1.403 against 1.453). The game then
takes 1.12 CPUs (its main thread never waits).

View lists against the build before (`run.sh`, `tgadiff.py`, 640x480,
`r_vkValidation 1`, the 41 lists, 177 views, at `cl_cmdfps` 125: two
commands a frame under `fixedtime 16`): 143 views give the same frames to
the bit, among them every view of the `dm`, `obj` and multiplayer lists
but the snow of `mohdm5_fog` and the near-camera ±1 of `d7_alley` and
`d7_arch`, the menus, the dynamic light, decal, fog, sun flare and debug
drawing lists. The others are the random content and run-to-run noise
named in Testing (`m1l1`, `m1l2a`, `m1l3a`, `m2l1_skyportal`, `m3l1a`,
`m5l1a`, `m5l2a`, `m6l1a`, `mohdm5_fog`, a particle in `m53_street`, a
pixel of `m33_grass`) and the first view of two single-player lists,
`m32_house` (`m3l2`) and `m62_square` (`m6l2a`, 38 and 26 % of the pixels,
nearly all at edges): the eye is at the same place and the view is turned
by one unit of the 16-bit angle (0.0055°: the frames match shifted by 0.05
pixel; `viewpos` prints -59 against -60, the server's `coord` -60.00 in
both). `face` sets the view from the angles of the last command the server
got, and after the spawn that is another command now. No validation
message. With the final default (100 a second, 10 ms: one or
two commands a frame) on 9 of the lists again (`mohdm1`, `obj_team2`,
`mp_mohdm1`, `mp_obj_team4`, `menus`, `m3l2`, `m6l2a`, `mohdm1_dlight`,
`m2l3`, 48 views): the same, those two first views and the rest to the
bit. The final engine differs from the one these lists ran with in
`Com_Frame` alone (`objdump -d`, addresses left out: the timescale
condition and the `fps` counter) and in `cl_cmdfps`'s default.


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
asks for no alpha bits): `DST_ALPHA` and `ONE_MINUS_DST_ALPHA` blend
factors are the constants 1 and 0 and `SRC_ALPHA_SATURATE` gives 0, as in
GL1, whatever the color target's alpha holds (a swapchain image's is
undefined until written; only a test shader, `textures/mohtest/brick`,
uses `GL_DST_ALPHA`). Pipelines that write all of RGB without blending
write alpha too, as 1, because a write mask without alpha is a partial
write, which costs DCC-compressed color targets: 1 to 2 % of the GPU time
at 3840x2160, 4 % in views that clear the color buffer first, 6 % in the
`m6l1a` forest with 4 samples (Radeon 780M, see Performance). Blending
pipelines write alpha (unchanged) only with multisampling: without it RADV
then rounds about 1 blended pixel in 10^5 one step lower than with the RGB
mask (and GL1), in both directions (15 pixels of `o2_pipe2`, the wet pipes
of `obj_team2`), and blending reads the target anyway.

### Fragment pipeline

1. Vertex color: per-vertex `colors` or the constant `color` of the geometry.
2. Texture unit 0 (if `st[0]` is set) combines with the vertex color using its
   texture env, unit 1 (if `st[1]` is set) combines with the result of unit 0.
   The formulas are next to `texEnv_t` in `vk.h`; `TEXENV_ADD` clamps the sum
   to 1 like GL. For `TEXFMT_RGB8/RGB5` (and `BC1_RGB`) textures alpha samples
   as 1 (`A = ONE` swizzle, or, on a portability subset device without
   `imageViewFormatSwizzle`, the shader sets it: see Implementation notes,
   Textures); with `TEXENV_REPLACE` GL then keeps the previous
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
debug drawing and is ignored when unsupported: without the extension (or
`bresenhamLines`) lines have the device's default rule, the same pixels on
RADV (`strictLines` false, its lines follow the diamond-exit rule anyway;
`r_vkFallbacks 4` gives the same frames as before in `mohdm1_debug`), and on a
device with `strictLines` (rectangular lines: llvmpipe) a pixel here and
there on nearly diagonal lines moves by one (`r_showtris` 1 and 2 and
`r_shownormals` in `mohdm1_debug` on llvmpipe: 0.5 to 1.4 % of the pixels,
isolated pixels along the lines; the triangles under them are the same);
without `stippledBresenhamLines` lines aren't stippled (only the game's
debug lines are, and the view covers them, see Testing).

### Clears

`vk_clear` clears the current scissor rectangle. Like `glClear` it honours the
color mask, the depth write bit of the current state and the stencil write
mask. Depth clears to 1.0, stencil to 0. `color` may be NULL when
`CLEAR_COLOR` is not set. The backend uses `vkCmdClearAttachments`, which
ignores masks, and draws a quad for a color clear with a partial color mask,
and on RADV (without multisampling) for a color clear to a color with a
component other than 0 or 1, which RADV clears with comp-to-single, slower
than the quad at 3840x2160 (see Performance, GPU time at 3840x2160); a
partial stencil write mask clears all bits (the renderer never sets one).
Without a stencil buffer (`r_stencilbits` 0, see Implementation notes,
Render target) stencil clears do nothing and stencil reads give 0. The
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
- Uploads happen mid-frame (cinematics every frame, ghost textures; the
  dynamic lightmaps of `R_UploadDlights` before the dynamic lights were
  drawn per pixel): draws recorded before an upload must see the old
  contents.
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
  operation. In OpenMoHAA the back end mostly runs inside the 2D exports:
  `Set2DWindow`, `DrawStretchPic`, `DrawString`, ... call
  `R_IssuePendingRenderCommands` and draw at once, and the UI calls
  `Set2DWindow` before and after the 3D view, so the 3D view's draw
  surfaces are executed there, not in `RE_EndFrame`.
- The color contents of the frame are undefined until it draws or clears
  them, like a GL back buffer (frames are drawn into the swapchain image,
  see Implementation notes). Screenshots, levelshots and resampled
  screenshots are therefore taken at the end of the frame, in
  `RB_SwapBuffers` (`RB_TakePendingScreenshots`), where GL1 read the back
  buffer when the command ran, before the frame was drawn (an older frame on
  Mesa); the "Wrote" centerprint of the `screenshot` command is run once
  the file is written, so it isn't in the picture. With `pause` (see
  Deterministic 3D views) both show the same frame.
- `vk_finish` is called at swap for frames with no 3D view (menus) and around
  cinematics, as `glFinish` was. It waits for the fence of the last
  submission; what the current frame recorded is submitted at
  `vk_end_frame`.
- `vk_read_pixels` must see everything recorded so far in the frame and the
  frame must continue afterwards (screenshots, AVI capture,
  `r_measureOverdraw`). It waits for the GPU, which is acceptable for these
  occasional reads. Depth and stencil read as 0 until the depth image is
  readable: without multisampling it only is from the start of the frame
  after a depth or stencil read, or after `vk_want_depth_reads` (called
  before the frame starts, it makes that frame's reads work;
  `RE_BeginFrame` calls it for `r_measureOverdraw`), until 300 frames
  after the last one (see Implementation notes, Render target).
- `vk_queue_depth_reads` records reads of single depth values in order with
  the draws without waiting; `vk_get_depth_read` returns a result once the
  frame that recorded it has completed. The backend collects the results
  when it reuses the frame slot, so a read made in frame N is available
  from frame N + 2 on (2 frames in flight), whatever the GPU timing, and
  stays available for 2 frames. Ids from before a `vid_restart` are lost,
  and the reads of a frame in which the depth image isn't readable get id
  0 (lost); they make it readable from the next frame.

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
The first reads (and the first after 300 frames without any) are lost
while the depth image becomes readable (see Frames and synchronisation),
which delays the first test by one more frame. Like GL1, `RB_RenderFlare` doesn't set `tess.vertexColorValid`, so
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
`sdl_vkimp.c`. `r_stereoEnabled` is not supported. `r_ext_multisample`
selects the sample count (see Implementation notes).

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
  (MoltenVK, with `VK_KHR_portability_enumeration` on the instance; its
  `imageViewFormatSwizzle` is enabled when it has it, with a fallback
  otherwise, and the bindings of stride 0, which it never allows, have one:
  see Textures and Draws), `VK_KHR_create_renderpass2` with `VK_KHR_depth_stencil_resolve`
  (multisampling), `VK_KHR_swapchain_maintenance1` or else
  `VK_EXT_swapchain_maintenance1` (`swapchainMaintenance1`: present fences,
  see Frames; on the instance `VK_KHR_get_surface_capabilities2` and
  `VK_KHR_surface_maintenance1` or `VK_EXT_surface_maintenance1`, which they
  need). `r_vkFallbacks` turns these off for tests (see Build and run).
- **Render target.** The color target of a frame is the swapchain image it
  presents (`VK_SelectTarget`, at the frame's first render pass, readback or
  present), when the swapchain has the size of the render target
  (`glConfig.vidWidth x vidHeight`) and images that can be color
  attachments and transfer sources: the color format is the swapchain's
  (`B8G8R8A8_UNORM` on RADV, `R8G8B8A8_UNORM` when the swapchain has neither
  of the two), each swapchain image has a view and a framebuffer, and
  readbacks swap red and blue for BGRA. The image comes from
  `UNDEFINED`, so its contents are undefined until the frame draws, like a
  GL back buffer (see Frames and synchronisation). Otherwise (a window
  resized before its `vid_restart`, no usable swapchain) the frame is drawn
  into an offscreen color image of that format, blitted (scaled) to the
  swapchain image at present; frames that drew nothing present nothing. The
  depth/stencil image is always offscreen. Its format is the first of
  `D24_UNORM_S8_UINT`, `D32_SFLOAT_S8_UINT` and `D16_UNORM_S8_UINT` the
  device supports, or without a stencil buffer (`r_stencilbits` 0, the
  default on Linux, where GL1's depth buffer has no stencil either) the same
  depth without the stencil plane (`X8_D24_UNORM_PACK32`, `D32_SFLOAT`,
  `D16_UNORM`; `D32_SFLOAT` on RADV): HTILE then holds depth only, 0.5 % less
  GPU time at 3840x2160 on a Radeon 780M, and the depth clear is a plain fill
  of HTILE instead of a compute pass that keeps its stencil bits. The
  stencil buffer is reported (`glConfig.stencilBits`) only when there is one,
  like GL1's. Without multisampling it can
  only be copied from (`TRANSFER_SRC` usage) while depth or stencil reads
  are wanted (`VK_UpdateDepthTarget`, at the start of a frame, replaces it
  with a readable image after a read or `vk_want_depth_reads`, and with a
  non-readable one 300 frames after the last; the new image starts cleared
  and the old one and its framebuffers go through the deferred destruction
  lists): RADV gives depth images that can be copied from or sampled
  TC-compatible HTILE, which keeps an 8x8 depth tile compressed only while
  it holds at most 4 planes (15 otherwise), and that cost 12 to 13 % of the
  GPU time in the `m6l1a` forest (see Performance). A frame's depth and
  stencil contents at its start are never needed (every 3D view clears
  depth first, 2D drawing doesn't test it), so the switch loses nothing.
  With multisampling the draws use the multisampled depth image, which is
  never copied from, and the single-sampled one is always readable. One
  render pass loads and stores
  both attachments; it ends for every readback and every upload that must
  be ordered with draws, and restarts on the next draw or clear. Drawing
  into the swapchain image saves the copy of every frame: 0.13 ms of GPU
  time at 1920x1080 on a Radeon 780M, 0.47 ms at 3840x2160 (see
  Performance).
- **Multisampling** (`r_ext_multisample` 2 to 16, latched). The draws go to
  multisampled color and depth/stencil images of the sample count asked for
  (the largest the device supports up to it, for color, depth and stencil
  attachments); every render pass resolves the color (average of the
  samples) into the frame's color target, so presenting and color readbacks
  work as without it, and the pipelines rasterize with that many samples
  (no sample shading, no alpha to coverage, like GL1's multisampled visual).
  The depth/stencil buffer is resolved (sample 0, `VK_KHR_depth_stencil_
  resolve`, render pass 2) into the single-sampled depth image only before
  a depth or stencil readback (`VK_ResolveDepth`, an empty render pass of
  its own): resolving it with every render pass cost 1.3 ms per frame at
  1920x1080 with 4 samples. Without the extensions multisampling is off
  (with a warning). Barriers on the depth image include the resolve's color
  attachment stage and access.
- **Frames.** Two frames in flight. Each frame slot has a command pool with
  three command buffers (upload, main, and present for frames drawn
  offscreen), a fence, a vertex/index buffer (4 MB, doubled when full), a
  32 MB staging buffer, a buffer for 256 depth reads and a query pool of 4
  GPU timestamps (`vkinfo`). A frame drawn into the swapchain image is
  submitted once, waiting for the image's acquire semaphore at the color
  attachment stage, signaling the image's present semaphore and the frame
  fence; the image goes to `PRESENT_SRC` at the end of the main command
  buffer. A frame drawn offscreen submits the rendering, then the present
  command buffer (blit and layout transitions) with the frame fence. The
  swapchain is recreated when the drawable size changes
  or on `VK_ERROR_OUT_OF_DATE_KHR`/`VK_SUBOPTIMAL_KHR`; with no area
  (minimized) the frame is submitted without presenting.
- **Presentation semaphores and swapchains** (`vk_instance.c`, present
  fences and retired swapchains). The semaphores that presents wait for
  belong to the swapchain, one per image, created with it: the semaphore of
  an image is only signaled again once the image has been acquired again
  and its acquire semaphore waited for, which guarantees that the image's
  previous present has completed, its semaphore wait included (Khronos'
  Vulkan Guide, Swapchain Semaphore Reuse). A swapchain that is replaced
  (resize, out of date), or destroyed (minimized, shutdown), is retired with
  its semaphores: nothing is presented to it any more, but the presentation
  engine may still hold them. With `swapchain_maintenance1` every present
  signals a fence (`VkSwapchainPresentFenceInfoKHR`) once the presentation
  engine is done with its wait semaphore; a present's semaphores can be
  destroyed once its fence has signaled, the swapchain once the fences of
  all its presents have, and those of one swapchain signal in present order,
  so a retired swapchain and its semaphores are destroyed once the fence of
  its last present has signaled: checked after each frame's present, waited
  for (at most 1 s each) at shutdown and `vid_restart`, and when 8 retired
  swapchains are pending (not seen). The fences come from up to 16 slots,
  taken in turn; the next present's is chosen and reset right after the
  present before it (`VK_PreparePresentFence`, a slot whose present is done,
  or a new one): nothing is waited for or reset between a frame's input
  and its present, which only adds the fence to its `VkPresentInfoKHR`. On
  RADV two slots are used: Mesa signals the fence in the submission it makes
  for the present anyway (`wsi_common_queue_present`), with the frame's GPU
  work. Without the extension a retired swapchain and its semaphores are
  destroyed when it is retired, which always follows a `vkDeviceWaitIdle`
  after its last present: per the Vulkan specification `vkQueueWaitIdle`
  waits for the queue operations of presents too (`vkQueuePresentKHR`
  "behaves as if it signals a fence payload" for it; the semaphore waits are
  such operations). Before, the 16 semaphores were created with the frames,
  reused by every swapchain and destroyed at shutdown before the swapchain,
  which relied on that wait for both. `vkinfo` prints the number of present
  fence slots and of retired swapchains still pending.
- **Frame pacing.** Right after presenting a frame drawn into the swapchain
  image, `vk_end_frame` acquires the image of the next frame
  (`VK_AcquireNextImage`, the image is "held" until a frame uses it). With
  a vsync present mode (`r_swapInterval` non-zero) `vkAcquireNextImageKHR`
  is where the CPU waits for the display, so the wait now happens before
  the game runs the next frame, as GL's swap waits after queuing the
  present, instead of between the next frame's game logic and its present:
  the time from the start of a frame's recording to its present went from
  16.3 to 1.7 ms at 60 Hz (one refresh less of input latency). Acquire
  semaphores come from a ring of 3 (frames in flight + 1), not from the
  frame slot, since an image is acquired before its frame starts; a
  semaphore is reused once the submission that waited for it has
  completed. A held image's semaphore is waited for by an empty submission
  before the swapchain is recreated or destroyed, and with
  `swapchain_maintenance1` the image is then released
  (`vkReleaseSwapchainImagesKHR`), so a retired swapchain can free it. The
  acquire stays right after the present, and present fences add no wait
  (see Presentation semaphores and swapchains).
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
  went through the same slot. Each storage is a `VkImage`, a view (with the
  `A = ONE` swizzle for RGB formats) and a descriptor set holding view and
  sampler. A portability subset device without `imageViewFormatSwizzle`
  (MoltenVK can lack it) gets identity views, and the pipelines of draws
  with an RGB texture have `VK_PIPE_TEX*_RGB` set with every texture env (not
  only `TEXENV_REPLACE`), whose fragment shader then sets the texel's alpha
  to 1: the same 1.0 (`r_vkFallbacks 1`, see Testing). Images are placed in 64 MB blocks of device memory, first fit
  in a sorted list of free ranges per block with merging on release (one
  block per memory type to start; an image larger than 16 MB gets memory of
  its own); a released range goes through the deferred destruction lists
  like the image, and an empty block is freed unless it is the last one of
  its memory type. A map load now makes 10 or 11 device memory allocations
  instead of 300 to 560 (one per texture), 7 to 13 ms less in
  `vkAllocateMemory`, and no driver limit on the number of allocations
  (`maxMemoryAllocationCount`, 4096 on some) is in sight. Samplers are
  cached by filter and wrap mode. When `r_textureMode` changes, mipmapped
  textures get new descriptor sets (sets used by recorded commands can't be
  updated). Uploads go to the upload command buffer, submitted before the
  main one, unless a draw recorded in the current segment of the main
  command buffer uses the texture. Then they are recorded in the main
  command buffer in order with the draws. New textures, level loading and
  cinematics take the first path, so they don't break
  the render pass. Between command buffers every texture is in
  `SHADER_READ_ONLY_OPTIMAL`.
- **Draws.** Each draw copies positions (repacked to 12 bytes), colors (RGBA8
  per vertex, or one float RGBA color read with a binding stride of 0),
  texture coordinates and 32-bit indexes into the frame's vertex buffer, in
  separate bindings. The passes of a multi-pass shader draw the same
  positions: the stage iterators set them with `RB_SetTessVertexArray`,
  which gives the draws a version (`drawGeometry_t::version`), and a draw
  with the same version, positions pointer, stride and vertex count as the
  previous one binds the copy made for it (the indexes too when they are
  the same array and count; the dlight pass has its own). The copy is valid
  until the frame's vertex buffer is reused from its start (new frame,
  mid-frame flush). A constant color, and the texture coordinates of a
  unit that isn't used (the positions, which the shader ignores), are read
  with a binding stride of 0. The portability subset allows no stride of 0:
  a stride must be at least `minVertexInputBindingStrideAlignment` (4 on
  MoltenVK; VUID-VkVertexInputBindingDescription-stride-04456), whatever
  `vertexAttributeAccessBeyondStride` says, so on such a device those
  bindings are read per instance (`VK_VERTEX_INPUT_RATE_INSTANCE`, the
  attribute's size as the stride, so no attribute reaches beyond it either):
  every draw has one instance, the first, so they read the same bytes
  (`r_vkFallbacks 2` forces it; the other strides are 4, 8 and 12 bytes, and
  a larger alignment is reported with a warning). A push constant for the
  color, the first idea, would need 16 bytes more than the 128 that are
  guaranteed and all used, and wouldn't cover the texture coordinates. In the measured views this saves 1 to 4 % of the
  vertex bytes (most surfaces are one multitextured pass). Draws from the
  static geometry (below) copy nothing but, when they need them, their
  indexes and a constant color, skinned draws (Skeletal models) nothing
  (but the indexes of a lower level of detail of the rare surfaces whose
  collapse map the vertex shader can't follow); vertex and index buffers are
  only bound again when they or their offsets change.
- **Static geometry** (`tr_staticgeom.c`, `vk_set_static_geometry`). At the
  end of a map load, `R_BuildStaticGeometry` puts into one device-local
  buffer (through the staging buffer and the upload command buffer, with a
  barrier to vertex input): positions, texture and lightmap coordinates of
  the world's faces, triangle surfaces and grids (every vertex of a grid's
  subdivision), then of each TIKI that static models use (its surfaces in
  the order of `R_AddStaticModelSurfaces`, from `pStaticXyz` and
  `pStaticTexCoords` like `RB_StaticMesh`); vertex colors (the world's, the
  same with white RGB, the static models' `staticModelData`, so vertex v of
  a model has color `staticColor + v`); and indexes: those of the faces and
  triangle surfaces, grouped by shader in the order a traversal of the BSP
  tree first reaches the surfaces (what the front end adds in a frame is a
  subsequence), then those of the TIKI surfaces. The back end decides at
  the first surface of a batch whether it draws it from there
  (`RB_StaticBatch`): faces, triangle surfaces, grids and static models
  whose shader has no deform, isn't the sky, with no dynamic light pass of
  `r_fastdlights 1` (`ProjectDlightTexture`) and no debug drawing, every stage's texture coordinates from `tcGen texture` or
  `lightmap` without `tcMod`, and colors that `ComputeColors` would make a
  constant (evaluated on the CPU for one vertex, `RB_StaticStage`), the
  vertex colors as they are, with alpha 255, or white with the vertex alpha
  (`RB_StaticStageColor` follows `ComputeColors` case by case, including
  where it leaves colors of an earlier batch, which stays on the CPU). The
  stage iterators run as before; their arrays (`tess.xyz`,
  `tess.svars.colors`, ...) stand for the streams and `RB_DrawElements` draws
  those (`RB_StaticDraw`). Such a batch counts the vertexes and indexes it
  would have put into `tess`, so it is split at the same surfaces as on the
  CPU; a surface it can't take (one that needs a dynamic light pass, vertex
  alphas below 255 where a stage needs 255, another surface type) turns it
  into a CPU batch: its surfaces are added again (`RB_StaticBatchToCPU`).
  Grids and static models with a lower level of detail compute their
  indexes per frame (into `tess.indexes`); a batch whose surfaces are not
  one range of the index stream copies them there too, and the backend
  copies them once for all passes. Consecutive static models with the same
  one-pass shader whose colors don't depend on the model share one batch
  (`RB_StaticMerge`): one draw per model with its modelview, colors and
  indexes, one stage setup for all, the same triangles in the same order.
  `r_staticGeometry 0` draws everything through `tess` again.
- **World culling** (`tr_world.c`). GL1's marking of the PVS and walk of
  the world's BSP tree (`R_MarkLeaves`, `R_RecursiveWorldNode`) are
  replaced by a list of the potentially visible leaves that gives the same
  leaves in the same order (see Performance, World culling, for why and
  how it was checked). At map load `R_SetupWorldLeaves` lists the leaves
  the tree reaches in the walk's order (children[0] first; the brush
  models' leaves aren't reached) with their boxes, their cluster and area
  and whether the box lies within the boxes of all their ancestors
  (`worldWalkLeaf_t`). When the view's cluster or the area mask changes,
  `R_MarkLeaves` tests them as GL1 did (`R_GroupVisibleLeaves`) and keeps
  the visible ones in groups of up to 8 consecutive leaves that lie within
  their ancestors' boxes, with the box of their boxes, and the others in a
  group of their own. Per view, `R_AddWorldLeaves` tests a group's box
  against the frustum planes (`R_CullBoxToFrustum`: `BoxOnPlaneSide`'s
  arithmetic inline, the far plane only where GL1 culls with it), then
  each leaf's box against the planes the group isn't wholly in front of,
  or a leaf of its own against the boxes of its path from the root
  (`R_CullLeafAlongPath`), and `R_AddWorldLeaf` runs GL1's leaf body, the
  dynamic lights first culled to the leaf's bounds (`R_CullDlightsToNode`;
  its ancestors' bounds contain them). A face's `msurface_t` holds what
  culling it reads (`isFace`, the plane, `viewCull` and the bounds of its
  points rounded outwards to whole units, in shorts; 64 bytes, the world's
  array aligned to them; `ParseFace`): `R_CullWorldFace` makes GL1's plane
  cull on the copy, then `R_CullFaceToView` leaves out a face whose bounds
  lie wholly behind one of the side planes its leaf isn't wholly in front
  of, by more than a unit (not for faces whose shader deforms vertexes,
  the sky, portals and the sky portal's faces: `viewCull` 0; not in the
  leaves dynamic lights reach, nor with `r_shownormals`:
  `R_FaceViewPlanes`). Brush models keep GL1's culling.
- **Dynamic lights** (`r_fastdlights 0`, the default; `tr_light.c`,
  `vk_add_dlights`, `vk_set_lightmap_dlights`). GL1 relit the lightmap of
  every surface a light reached, texel by texel (`R_RealDlightFace`,
  `R_RealDlightPatch`, `R_RealDlightTerrain`, `R_DlightSample`), into 15
  atlas textures uploaded every frame (`R_UploadDlights`), and drew the lit
  surfaces with them, in batches of their own (the atlas number was in the
  sort key) and with offset lightmap coordinates, so they left the static
  geometry. The fragment shader now adds the lights to the lightmap texel
  per pixel, with the light model of `R_DlightSample`: for each light,
  `color * 375 * (1 - d / radius)^2` truncated to a lightmap byte, times
  the overbright scale (`tr.overbrightMult / 255`), added to the filtered
  lightmap texel, the sum divided by its largest component when that is
  above 1 and truncated to a byte (GL1 did that per texel; see Differences
  from GL1). The front end only decides which lights a surface gets, with
  GL1's tests (the face's plane and `r_dlightBacks`, the bounds of patches
  and terrain) and a test that the light reaches a sample point of its
  lightmap texels (`R_DlightReachesFace`: the parallelogram `lmOrigin + s *
  lmVecs[0] + t * lmVecs[1]` of the texel centers, after a test of its
  bounding box computed at load, `lmBounds`; the bounds of patches and
  terrain), and keeps them as a mask in the surface's `dlightMap`. GL1 gave
  every visible surface every light to test (`R_RecursiveWorldNode` passes
  them all down), which with 16 lights cost most of the frame once the
  texels were gone (in the `obj_team2` yard 41 % in the parallelogram test
  and 11 % in the plane tests); now each node of the BSP tree has the
  bounds of what its subtree can light (`dlightBounds`: the texel boxes of
  its lightmapped faces and the bounds of its lightmapped patches, from
  `R_SetupDlightBounds` at load) and the recursion drops the lights whose
  reach misses them (`R_CullDlightsToNode`, `r_fastdlights 0` only: the
  projected pass reaches farther), which changes no surface's mask. The
  back end ORs the masks of a batch's surfaces (`tess.lightmapDlights`);
  where GL1 bound the dynamic lightmap (a stage's lightmap bundle), the
  stage iterators call `RB_LightmapDlights`, which stores the lights in the
  space of the current entity once per view for the world, once per run of
  batches for a brush model (`vk_add_dlights`), and sets the unit, the list
  and the mask for the draw. The backend keeps a 128 KB host-visible
  storage buffer per frame slot for the lists (4096 lights,
  `vkDlightGPU_t`: origin, 1 / radius, color in lightmap bytes, scale; set
  2 of the shaders, bound once per command buffer); the list's first light
  and the mask are push constants, the unit and the `r_dlightBacks 0` test
  pipeline key fields (specialization constants `DLIGHT_UNIT`,
  `DLIGHT_BACKS`). The vertex shader passes the object space position on;
  pipelines without lights don't read it (RADV removes the unused varying
  when it links the stages). With `r_dlightBacks 0` the shader takes the
  plane of the triangle from the derivatives of the position (their cross
  product, oriented with `gl_FrontFacing`: a BSP face's front is the
  clockwise side) and leaves out the lights behind it, in batches with a
  lit face (GL1 tested each face's plane against the light's center;
  patches and terrain have no such test, and a batch that mixes faces and
  patches tests both). Lit world surfaces keep their batches (their sort
  keys have no dlight bits any more) and their static geometry. Brush
  models keep GL1's behaviour, odd as it is: `R_AddBrushModelSurfaces`
  passes `needDlights` (1) as the lights to test, so only light 0 lights
  their lightmaps, and the surfaces it doesn't reach get
  `ProjectDlightTexture` from every light that reaches the model
  (`R_DlightBmodel`); the surfaces lit per pixel still sort apart from the
  others (`R_DlightSortKey`) so the projected pass leaves them out, and
  their batches stay on the static geometry (`RB_StaticDlights`). GL1
  also gave dynamic lightmaps to lightmapped faces with `SURF_NODLIGHT`,
  which only stops the projected pass; so do the per-pixel lights.
  `r_fastdlights 1` (`ProjectDlightTexture`, a pass per light with
  per-vertex texture coordinates and colors) is unchanged, and so is the
  per-vertex lighting of entities (`RB_Light_Real`) and static models
  (`useSpecialLighting`), see Performance.
- **Skeletal models** (`tr_skin.c`, `shaders/skinned.vert`, `vk_add_skin_data`,
  `vk_alloc_skin_frame`). `RB_SkelMesh` computed every vertex of a
  skeletal model's surfaces from its bones on the CPU (each weight's offset
  rotated by its bone, plus the bone's position, times the weight; morph
  offsets on the first weight; the normal rotated by the first weight's
  bone), and the vertex-lit iterator lit them per vertex
  (`RB_Light_Real`, `rgbGen lightingSpherical`), every frame. A batch of
  skeletal model surfaces whose shader takes
  `RB_StageIteratorVertexLitTextureUnfogged` (one stage, `rgbGen
  lightingSpherical` or `lightingGrid`, `tcGen texture` without `tcMod`,
  no deform: the characters, the weapons and most other skeletal models)
  is now drawn with `shaders/skinned.vert`, which does both with the same
  floating point operations in the same order (`precise`, so no fused
  multiply-add the C code doesn't have; the build has no FMA and no
  fast-math): positions, normals and colors are the CPU's to the bit, so
  the frames are too. Divisions (`fDot / fDistSquared`) get one correction
  step with the exact residual (`fma`), which makes the GPU's reciprocal
  product correctly rounded but for very rare operands, and the color
  bytes are turned into floats as `c / 255` correctly rounded (one
  correction step, exact for all 256 values), the value an `R8G8B8A8_UNORM`
  vertex color has. The vertexes of each surface go once into the skin
  data, a 32 MB device local buffer (`vk_add_skin_data`, through the
  staging buffer and the upload command buffer): 8-word vertex records
  (normal, texture coordinates, counts, where its morphs and weights
  start), the morphs and weights, the indexes of all the triangles, and
  the bone remap tables of the meshes after a TIKI's first (heads, hands,
  helmets: `TIKI_GetLocalChannel` of each bone's channel, as `RB_SkelMesh`
  maps them), when the TIKI is registered (`R_SkinRegisterModel` from
  `R_RegisterModelInternal`; cgame registers the models of the
  configstrings, the players' and their weapons, as server models), if one
  of its surfaces has a shader that the vertex-lit iterator draws, and
  those registered before the map load at the end of the registration
  (`R_SkinRegisteredModels`): its first draw used to add it, 0.6 ms more
  than GL1 in that frame for a player model; the first draw still adds
  what that missed (a custom shader). Everything is forgotten at each map
  load (`R_ClearSkinData`, after `vk_finish`), which comes before the
  models the map's cgame registers. What changes every
  frame goes into the frame data, a 2 MB host-visible buffer per frame
  slot (`vk_alloc_skin_frame`), once per entity and frame: its bones (the
  `skelBoneCache_t` that `R_AddSkelSurfaces` filled), its morph weights,
  and its sphere's lights (`reallightinfo_t`: point, directional, spot),
  or the constant color the other lighting functions give
  (`RB_CalcLightGridColor`, `RB_Light_Fullbright`, `RB_Light_Real` without
  lights), evaluated for one vertex. The skin data and the frame data are
  bindings 1 and 2 of set 2, next to the dynamic lights. A skinned batch
  counts the vertexes and indexes `RB_SkelMesh` would have added, so it is
  split at the same surfaces, and draws each surface in turn (one draw per
  surface instead of one per batch): `vertexOffset` is its first vertex
  record, `firstInstance` its draw record (bones, remap table, morphs,
  lights or color, scale, level of detail), and the indexes are those of
  the skin data. At a lower level of detail (`render_count` below the
  surface's vertex count) `RB_SkelMesh` collapsed each vertex from
  `render_count` on along its collapse chain and kept the triangles up to
  the first degenerate one; the vertex shader follows the chain itself
  (the target is the eighth word of the vertex record), and the draw takes
  the same prefix of the surface's triangles: a triangle that is
  degenerate at a level of detail is at every lower one (the chains only
  go down and stay together once they meet), so each has a threshold,
  computed at upload with a binary search, and the prefix is found with a
  binary search in their running maximum (`R_SkinThresholds`). Surfaces
  whose collapse map has a target that isn't below its vertex (the colt45:
  `RB_SkelMesh` then reads entries it hasn't filled) get their collapsed
  indexes from the CPU as `RB_SkelMesh` computes them, copied into the
  frame's vertex buffer. The pipelines of skinned
  draws have no vertex input (`VK_PIPE_SKINNED`); the fragment stage is
  the same. Still skinned on the CPU: the other shaders (`alphaGen
  distFade`, `tcGen environment`, deforms...), debug drawing (`r_showtris`,
  `r_shownormals`), `lod_tool`, stencil shadows, a TIKI whose mesh maps a
  bone to no channel or reads more morph weights than the entity has, and
  what doesn't fit in the skin or frame data. `r_gpuSkinning 0` skins
  everything on the CPU.
- **Skeletal culling** (`R_SkelOutsideView` in `tr_model.cpp`, `r_skelCull`;
  see Performance, Skeletal culling). GL1 prepared every skeletal model of
  the scene whether it was in view or not (`R_AddSkelSurfaces` let
  `CULL_OUT` through and `R_CullSkelModel` returns `CULL_IN`, "FIXME: not
  working properly"): its bones, morphs and surfaces, and in the back end
  its lighting (sun traces, the sphere's lights) and draws, which the GPU
  then clipped away. Now, once the skeleton is posed (the pose gives the
  radius of the animation frames), a model whose sphere lies wholly behind
  one of the view's four side planes returns before its bones: the sphere
  around the TIKI's origin with the frame's radius times 1.25 plus 32 units
  of the TIKI (`load_scale` times the entity's scale), both taken absolute
  (splash drops have a negative scale), times how much the entity's axes can
  lengthen a vector (the square root of the largest row sum of their
  absolute Gram matrix: 1 for a rotation; muzzle flashes and splash drops
  are drawn with scaled axes). The frame radius alone doesn't hold every
  vertex: the bone controllers turn the torso and the head past it (players
  reach up to 1.2 times it), and a stick grenade's vertexes reach three
  times its radius; the margin comes from a verification build that computed
  every posed vertex of every model the cull would reject (see Performance).
  Not culled: models drawn with a shader that deforms vertexes (any skin, or
  the custom shader), `RF_THIRD_PERSON`, `RF_FIRST_PERSON` and
  `RF_DEPTHHACK` models (the player's own body, the view weapon), mirror and
  portal views, `lod_tool`, `r_nocull`, and `r_shownormals` (it draws the
  normals of batches that don't compute them, what earlier batches left in
  `tess.normal`, which a culled model would change). The farplane's plane
  isn't used: a model beyond it is drawn in the fog color, which shows where
  the sky is behind it (`obj/obj_team4` has both). What a culled model
  leaves out isn't read by anything else: the bone and morph caches
  (`bonestart`, `morphstart`), its level of detail, skin frame data and
  lighting sphere are only used by its surfaces in the back end; cgame poses
  the skeleton itself for tags and shadows (`RE_TIKI_Orientation`), the lens
  flares of `RF_LENSFLARE` entities go through the entity list on their own.
  The one side effect left out is the skeletor's blink: its morph weights
  (`SKEL_GetMorphWeightFrame`) draw a `rand()` for a head whose blink is due
  (every frame once the blink timer, a `short`, has wrapped after 33 s), so
  the client's random sequence (seeded from the clock at every start anyway)
  moves differently.
- **Push constants** (`vkPushConstants_t`, the guaranteed 128 bytes).
  Vertex stage: projection × modelview, row 2 of the modelview (fog
  distance) and the clip plane in object space (zero without a clip plane),
  pushed again when a matrix or the plane changes. Fragment stage: fog color
  and the two linear fog constants, pushed when they change, and the
  dynamic lights of the draw (first light of its list in the frame's
  dlight buffer and the mask of those it adds, see Dynamic lights), pushed
  when they change. Both ranges are pushed at the start of every command
  buffer.
- **Pipelines** (`vk_pipeline.c`). The key packs blend factors, depth
  test/write/func, polygon mode, cull mode, topology, RGB color mask, alpha
  function (and its reference value), polygon offset on/off, color array or
  constant color, texture env per unit (0 for an unused unit), "RGB texture
  with TEXENV_REPLACE" per unit (with any env without the view swizzle), the stencil function and ops, fog on/off
  clip plane on/off, line stipple on/off, the unit whose texture gets the
  dynamic lights (0 for none), `r_dlightBacks 0`'s test and whether the
  draw is skinned (no vertex input, `shaders/skinned.vert`). Viewport, scissor, depth bias,
  line width and stencil reference/masks are dynamic, and the stipple factor
  and pattern for stippled pipelines (set when such a pipeline is bound and
  they changed). Line pipelines and polygon-line pipelines chain a
  `VkPipelineRasterizationLineStateCreateInfo` with the Bresenham mode. The fragment shader is specialized for
  texture envs, the alpha test, fog, the clip plane discard fallback, the
  alpha written without blending (see State) and the dynamic lights;
  pipelines with a clip plane use the vertex shader built with
  `gl_ClipDistance`.
  Pipelines are cached for the life of the device, with a
  `VkPipelineCache` saved to `vkpipelines.cache` (fs_homepath) when the
  device is destroyed, if it grew, and loaded at startup when its header
  (vendor, device, pipeline cache UUID) matches; a driver that refuses the
  data gets an empty cache. With a cold driver shader cache
  (`MESA_SHADER_CACHE_DISABLE=true`) a pipeline takes about 1 to 1.5 ms to
  create, 18 ms for the 15 pipelines up to the first view of `m6l1a`, and
  0.2 ms with the file; RADV's own disk cache brings it to 0.5 ms without
  the file.
- **Pipelines at load** (`tr_pipelines.c`, `vk_prebuild_pipeline`; see
  Performance, Pipelines and skin data at load). A pipeline used to be
  created by the first draw that needed it, inside that frame: with cold
  caches 1 to 2 ms each, 33 to 49 of them in the first frames after a load
  and during a match. At the end of a registration (`RE_EndRegistration`,
  after cgame has registered the map's models, effects and HUD)
  `R_PrebuildPipelines` goes through every registered shader and works
  out the draws the stage iterators make with it, from its stages as they
  pass them to `GL_State`, `GL_Cull`, `GL_TexEnv` and `GL_Bind` (the
  generic iterator stage by stage, the lightmapped multitexture and
  vertex-lit ones, the sky box, the projected dynamic light pass), and
  `vk_prebuild_pipeline` creates their pipelines. It builds the key with
  `vk_draw`'s own `VK_BuildPipelineKey`, which now reads the state from a
  `vkState_t` it is given (`vk_draw` gives it the recorded state), from a
  state with those fields set and a geometry that says only whether it is
  skinned, has colors per vertex and which units have texture coordinates:
  the same key, so the pipeline the draw finds is the one it would have
  created (`VK_CreatePipeline` depends on the key and the device only). What
  only the frame decides, every value it can take: where the shader is
  drawn (2D, where depth test is off for a stage without a second texture
  and the cull is the shader's or the none `Set2DWindow` sets behind
  `GL_Cull`'s back, which a draw keeps until `GL_Cull` changes it; a 3D
  view on the CPU, with colors per vertex; the static geometry, with a
  constant color when `RB_StaticStageColor` can give one; skinned), the
  fog (off, or the stage's `GLS_FOG_ENABLED`, since `GL_State` keeps the
  previous fog when that bit doesn't change and `Set2DWindow` turns it
  off: both, the map's fog isn't known before its first frame), dynamic
  lights on a lightmap unit (and `r_dlightBacks 0`'s test when set), the
  images an animated bundle binds (RGB textures with `TEXENV_REPLACE`, or
  any env without the view swizzle, change the key), the cull left before
  the sky box, and the immediate mode draws of every frame (the color
  clear drawn as a quad, the UI's boxes). A shader registered after that
  (an effect or a sprite announced in the match, in
  `GeneratePermanentShader`) gets its pipelines in that frame, with the fog
  of the last 3D view and none from the static geometry, which only the
  load registers. Not built: mirror and portal views (cull swapped, clip
  plane), debug drawing, stencil shadows, `r_measureOverdraw`, other
  immediate mode draws. The keys created after a load (by a draw, or for
  a shader registered later) are written to `vkpipelines.keys` at
  shutdown when new ones came (keys only: a header with the layout
  version of the `VK_PIPE_*` fields, at most 256, the new ones first),
  and created at the end of every registration of the next runs (those
  this device can make: no stipple without the extension, no polygon line
  mode without `fillModeNonSolid`), so they are compiled at load after a
  driver update too. Bounded: at most `VK_MAX_PREBUILT` (1024) pipelines
  per registration, and at the start of each registration
  (`vk_begin_prebuild`) the pipelines created before their draw that no
  draw used are destroyed (no command buffer refers to them), so what
  exists is what the draws used plus one registration's: 130 to 190 after
  loading the measured maps, of which 30 to 50 are used. On RADV they cost
  next to nothing in memory (pipelines that differ only in fixed-function
  state share their shaders): 178 more pipelines left the game's resident
  memory 50 KB larger and its device memory (amdgpu's fdinfo) the same.
  `vkinfo` prints how many exist, were created before their draw and of
  those were used.
- **Counters** (`vkinfo`, `vkStats_t`; `staticDraws` counts the draws from
  the static geometry). Every `vkAllocateMemory` goes
  through `VK_AllocateMemory`, which counts it; GPU times come from
  timestamps at the start and end of the main command buffer and around the
  copy to the swapchain image (`TOP_OF_PIPE`/`BOTTOM_OF_PIPE`), read without
  waiting when the frame slot is reused; times on the CPU use
  `SDL_GetPerformanceCounter`.
- **Frame log** (`vk_framelog.c`, `r_vkFrameLog`). When the cvar is set as
  the renderer starts, `GetRefAPI` returns wrappers of the exports that run
  every frame (the ones `tools/rtimer/rtimer.c` wraps, timed the same way)
  instead of the exports; otherwise it returns them unchanged, and the only
  trace of the log is one test of `vk_frameLog` per frame in
  `VK_CollectTimestamps`. Each frame's record is filled at `EndFrame` from
  the difference of `vkStats_t` since its `BeginFrame` (so the fence wait
  at the start of the frame, in the first 2D call, and the acquire after
  the present are in it; what the backend does between frames, a map load
  with the pipelines created at its end, is no frame's, like the
  registration time between frames, which `load` leaves out too; until
  2026-10-18 it went to the first frame after it) and completed at the next `BeginFrame` (its
  interval, the engine's time after it); the GPU timestamps come two frames
  later, when the slot is reused (`VK_FrameLogGpu`, matched by frame
  serial). Nothing of the frames, submissions, presents or acquires
  changes; the wrappers cost about 15 µs per frame (two clock reads around
  each of the ~300 2D calls of the HUD) while the log is on.
- **Shutdown and `vid_restart`.** `vk_shutdown` waits for the device,
  releases the held image and retires the swapchain, destroys the retired
  swapchains and their semaphores once their presents are done (present
  fences, see Presentation semaphores and swapchains), then the present
  fences and everything else, including the instance; `vid_restart`
  recreates the window, so the backend starts from scratch.

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

Accepted difference from GL1 (decided on 2026-10-15 for the speed): the
dynamic lights of `r_fastdlights 0` are added to the lightmap per pixel
(see Implementation notes, Dynamic lights) instead of being sampled at the
lightmap texels' centers (16 units apart on most maps) and filtered with
the lightmap, so:

- the falloff is smooth: no texel steps (GL1's contour lines in the light
  pools of `r_lightmap 1` frames); away from saturation the two agree on
  average (`mohdm1_dlight` lightmaps, lit pixels below 200: within ±0.3 of
  GL1 on average, 89 to 97 % of them within ±2);
- the division by the largest component happens per pixel: GL1 divided
  each texel, then filtered, so where a saturated texel neighbours one
  that isn't, its filtered result is darker. Pixels between 200 and 245
  are 1 to 3 levels brighter on average (all lit unsaturated pixels: +1.4
  to +2.5, never more than 4 levels darker), and next to a light, where
  the lightmap drops sharply (the foot of walls, steps), up to about 20
  levels brighter in a band a few pixels wide (the largest differences of
  the lit views);
- GL1 added the light to the raw lightmap data, before its overbright
  shift and normalization; the shader adds it to the lightmap texture,
  normalized already, which only differs where the lightmap itself
  saturates (its hue then shifts a little less towards the light's);
- in the unlit texels of a lit surface GL1's dynamic lightmap normalized
  the lightmap with float arithmetic, the static one with integers (±1);
  the shader keeps the static lightmap's value there.

Everything else is GL1's: the light model and colors (the 375 scale and
the truncation to lightmap bytes per light), which surfaces get which
lights (lightmapped world faces, patches and terrain; brush models with
light 0 only and `ProjectDlightTexture` for the rest; `r_dlightBacks 0`;
`SURF_NODLIGHT`, which doesn't stop them; nothing on the sky), the
overbright shift and normalization, and additive lights, which GL1's
dynamic lightmaps treated like the others (only `ProjectDlightTexture`
adds them with a different blend).

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
- Screenshots, levelshots and resampled screenshots are taken at the end of
  the frame in which the command runs, once it is drawn; GL1 read the back
  buffer the driver handed out when the command ran, an older frame on
  Mesa. The "Wrote" centerprint comes after the file is written, so it is
  not in the picture (GL1's never was either).
- Light flares (`RB_TestFlare`, unreachable) see depth changes 2 frames
  later than GL1 and don't force a `glFinish`-like wait (see Flares).
- `GLS_CLAMP_EDGE` is ignored. Nothing sets it; GL1 handled it by changing
  the wrap mode of the textures bound at that moment, permanently.
- Depth and stencil reads return 0 (`vk_queue_depth_reads`: lost) in a
  frame whose depth buffer isn't readable yet: the first frame with reads,
  unless `vk_want_depth_reads` announced them (`r_measureOverdraw` does, so
  its counts match GL1 from the first frame), and again after 300 frames
  without reads. The depth and stencil contents don't carry over into a
  frame where the depth buffer switches (no code needs them to).

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

6. (first half) Parity pass against GL1 on the maps not covered before:
   `dm/mohdm3`, `dm/mohdm6`, `dm/mohdm7`, `m1l1`, `m1l2a`, `m1l3a`, `m3l1a`,
   `m3l2`, `m3l3`, `m5l1a`, `m5l2a`, `m5l3`, `m6l1a`, `m6l2a`, `m6l3a`,
   `obj/obj_team1` to `obj/obj_team4` (see Testing for the content and the
   numbers). Every difference beyond the driver ones was traced to random
   game content, views from outside the world or GL1 saving a black or
   stale frame; no renderer change was needed. The tools gained the
   outside-world and black-frame warnings, `bspinfo.py` terrain, static
   model, decal and `--view` queries, `ui_gmbox 0` in `run.sh`, and
   `perf.py` with first frame times.

6. (second half) Performance, on an idle machine (see Performance for the
   method and numbers). The tools now measure the renderer's own time with
   a microsecond clock on both renderers (`renderer_timed.so`), GPU time
   from timestamps and backend counters (`vkinfo`), and sample the main
   thread's stack (`libstacks.so`, `stacks.py`). In order of measured
   benefit:
   - the depth image is only readable (`TRANSFER_SRC`) while depth or
     stencil readbacks are wanted (not in the original plan; it explains
     the main pass that took 6 to 7 % more GPU time than GL1's whole
     frame): RADV gives readable depth images TC-compatible HTILE, which
     keeps a depth tile compressed only up to 4 planes; 12 to 13 % less GPU
     time in the `m6l1a` forest, whose frames now take 7 % less time than
     GL1's at 3840x2160 and 3 % less at 1920x1080;
   - frames are drawn into the swapchain image instead of an offscreen
     image copied at present (not in the original plan; the profile of the
     `m6l1a` forest showed Vulkan was GPU-bound there and the copy took 8 %
     of its GPU time): 8 to 9 % more frames per second where the GPU
     limits;
   - frame pacing: the next swapchain image is acquired right after the
     present, so with vsync the wait for the display comes before the game
     runs the next frame (one refresh less of latency, jitter like GL1's);
   - the texture memory sub-allocator: 10 or 11 device allocations per map
     instead of 300 to 560, 10 to 25 ms less per map load, no per-frame
     change;
   - the pipeline cache saved to disk: 18 ms of pipeline compilation up to
     the first view of `m6l1a` when the driver's shader cache is cold, 0.4
     ms on RADV with its own cache;
   - reusing the vertex copies of multi-pass shaders: 1 to 4 % fewer vertex
     bytes, no measurable time;
   - fewer render pass breaks: not done, there are none to remove (one
     render pass per frame in every measured view, with dynamic lights and
     the sky portal too);
   - `r_ext_multisample`: multisampled images resolved into the frame's
     color target, the depth/stencil buffer only for readbacks (1.3 ms per
     frame saved at 1920x1080), matching GL1's multisampling about as
     closely as without it.

6. (second round) The renderer's CPU time and multisampling (see
   Performance, Second round), each change measured against the build
   before it and checked bit-identical on the 31 view lists: hidden symbol
   visibility (code 960 to 715 KB, the largest single CPU gain), static
   model culling without the model matrix and from a compact array, sun
   visibility traces kept by origin, a one-pass radix sort, the distance
   fade loop; alpha written with multisampling (the multisampled frame 6 %
   faster on the GPU, 3 % behind GL1 instead of 9 %). The stack sampler
   now samples at random intervals (it locked onto the 1000 frames per
   second cap), `stacks.py` has `--tree` and `--callers`, and the
   backend's duplicated framebuffer, present and readback barrier code is
   shared.

7. Step 1 of the redesign: static geometry (see Implementation notes,
   Static geometry, and Performance, Static geometry). The world's faces,
   triangle surfaces and grids and the static models are uploaded once at
   map load and drawn from device memory when their shader allows it,
   instead of being tessellated into `tess` and copied into the frame's
   vertex buffer every frame; consecutive static models with the same
   one-pass shader share one batch. Everything the GPU path doesn't
   support falls back to the CPU path, batch by batch, with GL1's batches
   and draw order. The frames are the same to the bit as the build before
   it in every view without random content (see Testing).

8. Dynamic lights per pixel (2026-10-15/16, see Implementation notes,
   Dynamic lights, Performance, Dynamic lights, and the accepted
   difference in GL1 quirks). First the exact CPU fix (each light tested
   against the face's lightmap texels, patch quads and terrain bounds
   before the texel loops; the same lightmaps, proven texel for texel
   against GL1's code over 2.1 million surface evaluations with random
   lights), then the lights evaluated in the fragment shader on the
   lightmap, which replaces the dynamic lightmaps (no atlas, no uploads,
   no batches of their own, lit surfaces stay on the static geometry); the
   per-surface masks are then made cheap with a box test of the texel
   centers and lights culled per BSP node. With 16 random lights the
   renderer's CPU time drops by 66 % in the `obj_team2` yard, 59 % in the
   `dm/mohdm3` overview and 26 % in the `m3l3` path (640x480), with no
   measurable GPU cost even at 3840x2160 (less GPU time than the dynamic
   lightmaps took). `r_fastdlights 1`, entities and static
   models are unchanged (see Remaining).

9. Step 3 of the redesign: skeletal models on the GPU (2026-10-16, see
   Implementation notes, Skeletal models, and Performance, Skeletal
   models), chosen from the profiles of new multiplayer views with 20
   player models (`tools/soldiers.py`, `views/mp_*.txt`,
   `views/perf_mp.txt`), where skinning, per-vertex lighting and the
   copies of the skinned vertexes took 35 to 50 % of the frame. The
   vertex shader skins the surfaces of the vertex-lit shaders (characters,
   weapons), lights them like `RB_Light_Real` and collapses their
   vertexes at a lower level of detail, with the CPU's floating point
   operations: the frames are the same to the bit. A bug of the sun trace
   cache of the second round was fixed on the way (every entity on
   whole-unit coordinates went to the same slot). The renderer's CPU time
   goes down 57 to 64 % in these views, with no measurable GPU cost.

10. The GPU time at 3840x2160 (2026-10-16 evening, see Performance, GPU
    time at 3840x2160): GL timestamps for GL1 (`tools/rtimer/glts.c`)
    and temporary ones in the Vulkan renderer put the 1 to 4 % that
    Vulkan took more than GL1 in the drawing, mostly in views that clear
    the color buffer, and A/B builds found three causes: partial color
    writes (the pipelines wrote RGB only to the BGRA swapchain image,
    GL1's BGRX buffer takes whole pixels), the unused stencil plane of
    the depth image, and RADV's comp-to-single clears of the fog color.
    The pipelines without blending write alpha, the depth image has no
    stencil plane when `r_stencilbits` is 0, and on RADV those clears
    are drawn: 2.7 to 5.4 % less GPU time per frame in six of the seven
    views measured at 3840x2160 (0.7 % in the seventh); Vulkan's frames
    are now faster than GL1's there but in the `dm/mohdm3` overview (0.6
    % slower). The frames are the same to the bit. What is left is
    RADV's choice not to compress textures (see Remaining).

11. Frame spikes, measurement only (2026-10-16 night, see Performance,
    Frame spikes): the frame log (`vk_framelog.c`, `r_vkFrameLog`,
    `vkframelog`), per-frame files of `renderer_timed.so` and `libglts.so`
    for GL1, demos in `perf.py` (`views/perf_demo.txt`) and
    `tools/framelog.py`. The causes it found are the first entries of
    Remaining.

12. Frame limiter (2026-10-17, an engine change, see Performance, Frame
    limiter): `Com_Frame` waits for a deadline in microseconds that
    advances by one period (`Sys_Microseconds`, `NET_SleepUs`, a timer
    slack of 1 µs on Linux), sleeping until 1 ms before it and polling the
    rest, instead of whole milliseconds since the last frame; the `bias`
    is gone, the 1 ms floor (no frame of 0 ms, uncapped at most 1000 a
    second) stays. Measured with both renderers in a match demo and a
    fixed view at nine caps: exact caps, even frame starts, the median
    input to present time the same or shorter, more CPU; shorter sleeps
    instead of the polling saved CPU but cost latency (the core clocks
    down). The deterministic views give the same frames to the bit. The
    tools gained `perf.py --binaries`, `--caps`, `--seconds`, `--preload`
    and the game's CPU use and core, `libinlat.so` (input read to present,
    either renderer), its files in `framelog.py`, and `MOH_BIN` in
    `run.sh`. Then caps that divide 1000 went back to starting frames on
    the tick of `Sys_Milliseconds` (a deadline in whole milliseconds; the
    wait knows the tick from the last one it saw and still polls the last
    millisecond): with the deadline in microseconds anywhere in the
    millisecond, the read of `com_frameTime` after the input sometimes fell
    in the next one, up to half of the frames 3 and 5 ms at 250 depending
    on the phase. Measured per frame with a temporary logging build, in a
    local game and a demo without `fixedtime`: every frame 1000 / cap ms
    again but for late frames, input to present and CPU those of the
    deadline in microseconds; the old way alone of waiting for the tick
    cost 0.02 to 0.05 ms of input to present.

13. Present timing, measurement only (2026-10-17, see Performance,
    Present timing): `libinlat.so` with `INLAT_DISPLAY=1` records what
    happens to each present after the call (Xwayland's completion events,
    Wayland's presentation feedback with present ids it enables, the GPU
    completion of every submission through a timeline semaphore of its
    own, the kernel's vblanks and the framebuffer on the primary plane,
    the window's fullscreen state; `INLAT_PRESENT_MODE` forces a present
    mode) and `framelog.py --display` works out each frame's time to the
    display; no renderer change. Measured at 1920x1080 with Xwayland and
    native Wayland, in a window and fullscreen, immediate, mailbox and
    FIFO, at caps 0, 59, 60 and 250, in the `obj_team2` yard and the
    `o2match` demo: vsync through KWin costs 3 to 7 ms over tearing
    uncapped and at 250 (8.5 to 11 ms at 59 and 60), composition 1 to 1.5
    ms of it; KWin bypasses itself (direct
    scanout) for fullscreen mailbox and FIFO but composites with tearing;
    FIFO queues three frames; Xwayland costs nothing measurable.

14. World culling (2026-10-17, see Performance, World culling, and
    Implementation notes, World culling), starting from the Metal port
    hand-over's review of JayRewd's exact world culls (omaha 1c2c8754),
    each taken only in a form that gives GL1's leaves and surfaces: world
    faces wholly outside the view culled (the bounds of each face, a unit
    of margin; not for deforms, the sky, portals and the sky portal, nor
    where dynamic lights reach, whose batches take the lights of all
    their faces), the
    BSP walk replaced by the potentially visible leaves in its order with
    culls of 8 leaves at a time (exact by box containment; the leaves
    that stick out of an ancestor's box tested along their path; the
    commit's list also took the brush models' leaves), what culling a face
    reads kept in its `msurface_t`, and the PVS listed from a compact copy
    of the leaves instead of GL1's pass over every node (0.23 ms at each
    cluster change in `obj/obj_team4`, 67 µs now on the median). A
    temporary verification build ran GL1's code beside it in 92254 views
    of 40 view lists and 4 demos: the same leaves in the same order, the
    same plane culls, every face culled against the view outside the clip
    volume; the frames are the same to the bit. The renderer's CPU time
    goes down 2 to 10 % in the `obj_team2` yard, 4 to 5 % in `o2match` and
    1 to 2 % in `o4match`, and stays within the noise in the `dm/mohdm3`
    overview; the slowest 1 % of the demos' frames, compared frame by
    frame, change by -0.05 to +0.02 ms.

15. The known risks before shipping (2026-10-18, see Remaining, Known
    risks, Implementation notes, Presentation semaphores and swapchains,
    Textures and Draws, and Performance, Present fences). Presentation:
    the semaphores presents wait for belong to their swapchain (one per
    image, reused once the image is acquired again), and a replaced or
    destroyed swapchain is retired with them and destroyed once the
    presentation engine is done with them: with
    `VK_KHR/EXT_swapchain_maintenance1` when the fence of its last present
    has signaled (present fences, chosen and reset after the present before:
    nothing between a frame's input and its present), otherwise after the
    `vkDeviceWaitIdle` that comes before (which, per the specification,
    waits for the presents' queue operations); the image acquired ahead is
    also handed back (`vkReleaseSwapchainImagesKHR`), and shutdown destroys
    the swapchains before their semaphores, not after. MoltenVK:
    `imageViewFormatSwizzle` is queried and enabled; without it the shader
    sets the alpha of RGB textures; the bindings of stride 0, which the
    portability subset never allows, are read per instance there. Lines
    without `VK_KHR/EXT_line_rasterization`: checked. `r_vkFallbacks`
    forces each fallback: the same frames to the bit as the build before
    (a temporary build with alpha 0 in RGB textures showed that the
    shader's alpha is what keeps them so), checked on RADV and llvmpipe,
    and every presentation path ran clean under synchronization
    validation. Input read to present the same within the noise, uncapped
    and at 250, in a window and fullscreen. A checklist for testing on
    Windows: [WINDOWS_TESTING.md](WINDOWS_TESTING.md).

16. Pipelines and skin data at load (2026-10-18, see Performance,
    Pipelines and skin data at load, and Implementation notes, Pipelines
    at load and Skeletal models). At the end of a registration
    (`RE_EndRegistration`) the pipelines of the draws the registered
    shaders will make are created (`tr_pipelines.c`: the stage iterators'
    state, with each value of what only the frame decides: where the
    shader is drawn, the fog, dynamic lights, the images of animations, the
    sky box, the clear to the fog color and the UI's boxes), through
    `vk_draw`'s own key function, so a draw finds the pipeline it would have
    created; a shader registered later gets its own when it is created; the
    keys that still come after a load are kept in `vkpipelines.keys` and
    created at the loads of the next runs (after a driver update too);
    unused ones are destroyed at the next registration. A skeletal model's
    skin data is added when the model is registered instead of at its
    first draw. With cold caches the 39 and 49 pipelines that two matches
    created on first use (60 and 80 ms, up to 58 ms in one frame) are
    created at load (the map load 0.18 to 0.20 s longer): 2 left in the
    match for the first play after an install, 0 after a driver update and
    with warm caches, where the load is as long as before; the 99.9th
    percentile with cold caches 2.08 to 1.56 ms and 3.66 to 2.76 ms, the
    maximum 33.6 to 21.2 and 58.4 to 28.5 ms (registration, shared with
    GL1). No skin data at first draws any more. The same frames, the same
    input to present time. The frame log no longer charges what happens
    between frames (a load) to the first frame after it.

17. Skeletal culling (2026-10-19, see Performance, Skeletal culling, and
    Implementation notes, Skeletal culling), from JayRewd's b26d8a67: a
    skeletal model whose sphere lies behind one of the view's side planes
    returns from `R_AddSkelSurfaces` before its bones, morphs and surfaces
    (and its lighting and draws in the back end). A temporary verification
    build computed every posed vertex of every model the cull could reject
    in 20 matches and the 41 view lists (12.9 million models): the
    animation frame's radius is exceeded by up to 1.2 times for players
    whose bone controllers turn the torso, 3 times by the stick grenade,
    and by the scaled axes of muzzle flashes and splash drops, whose
    negative scale also made the radius negative (JayRewd's rule culled
    such drops in view); radius times 1.25 plus 32 TIKI units, times the
    axes' norm, never culled a model with a vertex in view. The renderer's
    CPU time 6.5 to 8.5 % lower in the matches, 4 to 13 % in the soldier
    views, the slow frames and the input to present time lower; the same
    frames to the bit.

18. Command rate (2026-10-19, see Performance, Command rate), from
    JayRewd's `cl_cmdfps` (omaha 1620f310) and limiter change (b26d8a67):
    usercmds at a fixed rate (`cl_cmdfps`, 85 to 125 a second) on a grid of
    server time, every step the same length at every frame rate (his
    depend on it), several per frame below the rate; the view turned every
    frame through a new cgame import (`GetViewUserCmd`), no command
    rewritten after it is made, no smoothing of the own position; no packet
    without a new command; cgame keeps what the network doesn't carry when
    the snapshot has run every command; client caps above 1000 and uncapped
    on the microsecond deadline, frames of 0 ms counted as 0 ms, the `fps`
    counter in microseconds. Default 100 commands a second. Why short
    steps get stuck on bumps (pmove's step-up refused on a steep bevel,
    then the speed lost) was found with a pmove sweep of `dm/mohdm2` and
    walks in the game: 1 ms steps stop 3 to 4 times as many walks as 8 to
    12 ms, 4 ms 1.6 to 2 times, no step none. Steps exact from 60 to 3300 frames a
    second, the same moves to the bit at 144, 250, 1000 and uncapped,
    nothing lost between commands, prediction errors and input to present
    as before.

Remaining:

- What makes the slow frames, in order (see Performance, Frame spikes;
  frame times in two matches played back, 1920x1080 unless named):
  1. Registration in the middle of a match: a model announced by a
     configstring (`CG_ProcessConfigString`, cgame's `CacheResource`)
     loads its shaders' images from the pk3s: the largest frames of both
     renderers (21 to 29 ms in the two matches, GL1 26 to 33 ms) and 13 to
     28 % of the slow frames' excess; 6 and 3 frames, 39 and 26 ms of
     registration per match, 26 to 36 textures (8 to 11 MB). Measured
     (2026-10-18, see Performance, Pipelines and skin data at load): zlib
     decompressing the images from the pk3s takes 48 % of it, the file
     system's lookups 17 % and its case correction (`FS_CorrectCase`) 11
     %, mipmaps and scaling on the CPU 10 %, TIKI files 7.5 %, TGA and JPEG
     decoding about 1 %; shared with GL1, which spends about the same. Not
     done, each keeping the frames: (a) the players' models and weapons
     registered at map load (cgame or the engine: the stock lists are
     known; more load time and memory) would take nearly all of it out of
     the match; (b) a faster inflate for the pk3 reads (zlib-ng's or
     libdeflate's decompression, the same bytes, reported about twice as
     fast as stock zlib; not measured here) would take up to a quarter of
     it off, in the load too; (c) the file system's directory scans: `FS_CorrectCase` lists
     every search directory that lacks a path (`opendir`/`readdir`) on
     each lookup of a file that only a pk3 has; listing each directory
     once per file system start, or skipping the correction where a pk3
     has the file, would remove most of its 11 % and part of the 17 %;
     (d) the mipmaps on the GPU, with a compute shader doing `R_MipMap`'s
     integer box filter (the same bytes; `vkCmdBlitImage`'s filtering
     would not be), 10 %; (e) a cache of decoded images (like the pipeline
     cache) would remove the decompression and the decoding together, at
     the cost of disk space. (a) to (c) are engine or cgame changes, (d)
     the renderer's. The skin data of GPU skinning is no longer built at a
     model's first draw (0.6 ms more than GL1 in that frame): it is built
     when the model is registered, in these frames (under 1 % of them).
  2. Pipelines on first use, with cold caches: done (2026-10-18, Plan 16):
     the map's pipelines are created at load (0.18 to 0.20 s more map load
     with cold caches, the same with warm ones), 2 are left in the match
     for the first play after an install (the head icon sprite, registered
     in the first frame), none after a driver update or with warm caches
     (`vkpipelines.keys`). What a first play can still create: mirror and
     portal views (cull swapped, clip plane), debug drawing, stencil
     shadows, `r_measureOverdraw`, immediate mode draws other than the UI's
     boxes and the clear, shaders whose state combination no load shader
     has. Untested where pipelines compile more slowly than on RADV
     (NVIDIA, MoltenVK), where the load would grow more and the gain too.
  3. The busier moments of a match on the CPU: front end, back end and
     cgame are 60 to 80 % of the slow frames' excess at 1920x1080,
     Vulkan's smaller than GL1's. In the stack samples of
     `o4match`'s slow frames the world traversal (`R_RecursiveWorldNode`,
     `R_AddWorldSurfaces`, 15.7 % of their samples against 11.3 % of all
     frames' samples) led, then the terrain re-split every frame
     (`RB_DrawTerrainTris`, `R_DoTriSplitting`, 8.6 % against 6.2 %),
     `vk_draw` (7.5 %) and cgame's entities (`CG_AddPacketEntities`, 7 %);
     `RB_SkelMesh`'s mesh search and the sky clipping are about 1 % each.
     The world culling (Plan 14) took GL1's pass over the nodes off the
     frames where the cluster changes and the faces outside the view off
     the back end; what is left of the traversal (about 9 % of `o4match`'s
     samples: the leaves' surface lists, the box tests, the first read of
     each drawn face, the culling) has no exact shortcut left that this
     step found worth it. Next candidates: the terrain re-split; the sky's
     faces outside the view (their clipping, `ClipSkyPolygon` and
     `AddSkyPolygon`, is 1.2 % of `o4match`), if a sky box drawn from fewer
     of them gives the same pixels where no world face covers the frame;
     JayRewd's exact early rejects in cgame's mark fragments
     (`R_MarkFragments`, 0.8 %).
  4. CPU-side frame pacing where the GPU limits (3840x2160 fixed views):
     Vulkan's frame times spread more than GL1's (99th percentile 0.2 to
     0.4 ms higher) while the GPU finishes its frames as evenly as GL1's:
     the fence wait comes before cgame and the front end in the frame
     (the first UI widget's `set2D()` runs `RB_DrawBuffer`), so their
     noise isn't absorbed in the same frame as with GL1's wait in the
     swap; and the acquire after the present waits 0.6 to 1 ms in up to 4
     % of the frames under Xwayland. Starting the backend's frame (and its
     wait) at the frame's first recorded command instead of in
     `RB_DrawBuffer` would put the wait after the CPU work like GL1's,
     with the same latency (the frame is still submitted when the GPU
     frees its slot): to be tried and measured, with the input to present
     time checked. Tools that report frame times from the CPU's side
     (present to present) see this spread; what is displayed is even.
  5. The engine: at round changes a soundtrack is loaded and decoded on
     the main thread (`MUSIC_NewSoundtrack`, `S_CodecLoad`, MP3 decoding,
     with the music volume at 0 too) with large configstrings: 1.2 ms
     more engine and 1.4 ms more cgame time in each of 4 frames of
     `o4match`; outside the renderer.
  6. Where the GPU limits, the GPU-heavy moments (the fence wait is 37 to
     53 % of the slow frames at 3840x2160): GPU time, see the texture
     compression item below.

- Present timing (see Performance, Present timing), pacing changes not
  made, each lowering input to display without adding lag (the waits come
  before the input read): (1) with FIFO, wait before the input read until
  the previous present has completed (`vkWaitForPresentKHR` on the main
  thread), so at most one frame is queued: 38 to 59 ms less uncapped at
  60 Hz, and no queue left at a cap below the refresh; (2) with vsync
  (mailbox, a window, FIFO), start each frame just in time for the
  compositor's latch: up to the measured slack, 0.9 to 1.4 ms uncapped,
  2.3 to 2.7 ms at 250 and 7.5 to 9.9 ms at 59 and 60, but KWin doesn't
  report its latch (3.3 to 3.9 ms before the vblank here, adaptive) and a
  frame that misses it waits a whole refresh; an engine change (when
  `Com_Frame` starts a frame). With tearing (fullscreen immediate, the
  lowest: 1.5 ms uncapped, 2.1 at 250) the slack is under 0.5 ms: only
  less CPU and GPU time per frame lowers it. Not measured: a VRR or 240 Hz
  display, other compositors (GNOME's, wlroots), NVIDIA.
- The engine's error dialog hung games, with either renderer (fixed in
  `code/sys/sys_unix.c`; upstream code, the same in ioq3 and openmohaa
  today): `Sys_Exec` forked and called `exit(-1)` in the child when
  `execvp` failed (no `zenity`: KDE without it), so the child ran the
  parent's exit handlers, and OpenAL Soft's waits forever to join
  PipeWire's event thread, which the child doesn't have (backtrace of the
  child under gdb; not Mesa); the parent waits for the child. The child
  now calls only `execvp` and `_exit(127)`, and the parent waits for that
  pid and counts 127 or a killed program as no dialog. That made
  `Sys_Dialog`'s fallback work for the first time (the failed child's 255
  counted as an answer, so kdialog and xmessage were never tried), and the
  fallback then showed `--title=Error` in place of the message: the zenity
  command's two `va()` calls overwrote the caller's `va()` message (two
  buffers); the command builders no longer use `va()`. The hang needs the
  sound still up when the dialog starts: a plain fatal error shuts the
  client down first, a recursive error (one raised while an error is
  handled) goes straight to `Sys_Error`. Tested with a temporary
  `error sys` command calling `Sys_Error` with the renderer and sound up,
  `cl_renderer vulkan` and `opengl1`, no zenity: the old build hangs
  (parent in `do_wait`, child in `futex_do_wait`), the new one shows
  kdialog with the message, then xmessage when kdialog is killed, and
  exits (status 3, crash log written) when the dialog closes or when no
  dialog program is on the `PATH`; the plain `error` command the same.
- The frame limiter (see Performance, Frame limiter): at caps of 60 and
  144 the tail of the input to present time is up to 0.25 ms longer than
  with the old limiter's 62.5 and 166.7 frames per second, after
  `BeginFrame` (renderer and driver), and the new limiter at those old
  rates has the old tails: likely the frame rate against the 59.96 Hz
  display, untested with a VRR display. Untested on Windows (whole
  millisecond sleeps, the last 2 ms polled). The polled millisecond costs
  0.4 to 0.7 ms of a core per frame; on this CPU sleeping through it cost
  up to 0.03 ms of input to present at 144 (lower clocks). Caps that
  divide 1000 start on the millisecond tick again: GL1's 99th percentile
  of input to present in `o2match` at 250 is then about 0.08 ms higher
  than with a deadline anywhere in the millisecond (the old limiter's
  level), Vulkan's about the same; not explained. The usercmd's step
  moved by 1 or -2 ms on every snapshot (`CL_AdjustTimeDelta`, Quake 3's
  clock drift: 8 % of the usercmds at 250 in a local game), whatever the
  limiter, until the command rate change (steps on a fixed grid since).
- The command rate (see Performance, Command rate): no step gets over
  every bump of `dm/mohdm2` (the default, 100 a second, 10 ms, stopped the
  fewest walks of the sweep). The step-up that pmove refuses when
  its trace back down lands on a short steep bevel (the cause at the bump
  measured) could be fixed in pmove itself, for the servers that run this
  game code and cgame's prediction alike; not done (a gameplay change).
  The world is drawn at whole milliseconds of game time (`cg.time` is an
  integer): above 1000 frames a second consecutive frames in the same
  millisecond show the same moment of the world (the view turns); a render
  time in microseconds for interpolation and animation was proposed, not
  done. The own position moves once per command (no smoothing, by the no
  added lag rule). A button or movement key waits for the next command (5
  ms on average at 100 a second, 2 at 250 frames a second before): an
  early command when a button is pressed between commands would take most
  of that off (a shorter step now and then); proposed, not done. Uncapped beyond what the GPU can draw (1500 frames
  a second at 1920x1080 on this machine) shows the input later than a cap
  just below it.
- Step 2 of the redesign: the per-vertex shader effects on the GPU. In the
  forests most batches still go through the CPU because of them: in
  `m6l1a`, `m3l3`, `dm/mohdm3` and `obj/obj_team2` a surface of the tree
  models uses `rgbGen vertex` with `alphaGen distFade` (a per-vertex
  distance fade) and the others deform (wind sway, autosprites); 69 to
  144 static model batches per frame in the measured views, against 8 to
  118 static models that the static path draws. Then `tcMod` (scroll, rotate, ...), `tcGen environment` (the
  windows of `dm/mohdm3`) and the other `rgbGen`/`alphaGen` cases of
  `RB_StaticStageColor` that need the CPU, and the dynamic light pass
  (`ProjectDlightTexture`). Once the forests' static models are on the
  GPU path, drawing a merged batch's models with one instanced or indirect
  draw (per-model matrix and color offset in a buffer, colors read with
  the vertex index) would take most of the remaining per-model cost of the
  backend (`vk_draw` per model: pipeline key, push constants, binds); with
  step 1's eligible models (8 to 118 per frame) it isn't worth a second
  vertex shader yet.
- Dynamic lights, what is left on the CPU (see Performance, Dynamic
  lights): the per-vertex lighting of static models within twice a light's
  radius (`useSpecialLighting` in `ComputeColors`, which also takes them
  off the static geometry: about 0.05 ms with 4 random lights and 0.14 ms
  with 16 in the `dm/mohdm3` overview, nothing measurable in the
  `obj_team2` yard) belongs with step 2: it needs normals in the static
  geometry and the light loop in the vertex stage (GL1 lights per vertex,
  with N·L and an inverse square falloff). The entities' lighting
  (`RB_Light_Real`) is done by the vertex shader since step 3.
  `r_fastdlights 1` (`ProjectDlightTexture`: a pass per
  light with per-vertex colors and texture coordinates) is GL1's look of
  that mode and is still needed in `r_fastdlights 0` for the surfaces of
  brush models that light 0 doesn't reach (and their triangle surfaces),
  so it stays.
- After step 3, skeletal models: the shaders that don't take the
  vertex-lit iterator (`alphaGen distFade`, `tcGen environment`, deforms,
  about 150 of the 870 `lightingSpherical` shaders of the game data) are
  still skinned on the CPU. Skeletal models outside the view are culled
  since Plan 17; their pose is still set (the radius comes from it: 1 to 2
  µs a frame in the matches), and models drawn with vertex deforms are
  never culled (0.2 % of the culls). The sun visibility traces of the
  entities that move (5 to 8 % of those views) are the engine's collision
  code. The `m3l3` path, whose sun traces took 23 % of the frame, wasn't
  measured again after the trace cache fix. The LOD terrain, re-split
  every frame (`RB_DrawTerrainTris`), still copies its vertexes.

- At 3840x2160, where the GPU limits, the views measured since the GPU
  time work (see Performance, GPU time at 3840x2160) take 1 to 3 % less
  time than GL1's frames, but the `dm/mohdm3` overview, 0.6 % more (2.497
  against 2.483 ms), and the `obj_team2` yard with `r_fastsky 1`, 3.9 %
  more. What is left of the difference is the textures: radeonsi gives
  textures DCC, RADV only images that can be color attachments and are
  larger than 512x512, and with `RADV_DEBUG=forcecompress` (DCC for every
  image) Vulkan's frames take 0.03 to 0.06 ms less and are faster than
  GL1's in the `dm/mohdm3` overview, the `obj_team2` yard and the `m3l3`
  path. The textures (all 512x512 or smaller in `dm/mohdm3`, the lightmaps
  128x128) are below RADV's threshold whatever the renderer asks for; the fix
  would be in RADV (compressing sampled images that copies write, as
  radeonsi does), not in the renderer; not tried.
- A lead left from the main pass investigation (see Performance): RADV's
  primitive binning uses 128x128 bins where radeonsi uses 128x64 for
  depth-tested draws on this GPU (in `radv_gfx10_compute_bin_size`, Mesa
  26.0.8, the final `extent.height = MAX2(extent.width, ...)` and the depth
  bin's height taken from the color bins look like typos; a 128x128 bin
  needs more depth tag space than the 78 KB the formula allows). It can't
  be changed from the application; testing it needs a patched RADV (not
  done), and if it matters the fix belongs in Mesa.
- `r_ext_multisample 4` is 3 % slower than GL1's (2.77 against 2.70 ms per
  frame in the `m6l1a` forest at 1080p); the resolve into the swapchain
  image costs 0.26 ms of GPU time (RADV resolves into a DCC-compressed
  image with a shader; `vkCmdResolveImage` costs the same, and no image
  without DCC could be had to try the hardware resolve).
- The renderer's own CPU cost per draw (`vk_draw`, about 0.3 µs, 10 % of
  a 640x480 frame in the `m6l1a` forest before the static geometry) is
  RADV's command recording, the backend's bookkeeping and, for the CPU
  path, the vertex copies. The end of the frame (0.07 ms) is RADV's
  submission and the sync objects of the X11 present and acquire.
- The front end used to take up to 8 % longer than GL1's in the `m6l1a`
  forest: explained, the calls through the PLT and the static model loop
  (see Performance, Second round); it now takes 0.19 against GL1's 0.32
  ms per frame there at 1080p.

Known risks before shipping, handled (2026-10-18, Plan 15), and what is
left of them:

- Presentation semaphores: they belong to their swapchain now, and a
  replaced or destroyed swapchain and its semaphores are destroyed once
  the presentation engine is done with them, known from present fences
  (`VK_KHR/EXT_swapchain_maintenance1`) or, without them, from the
  `vkDeviceWaitIdle` before the swapchain is replaced, which the
  specification makes wait for presents too (see Implementation notes,
  Presentation semaphores and swapchains). The image acquired ahead is
  still released with an empty submission (that is correct: the
  submission waits for its semaphore and completes before the swapchain
  goes) and now also handed back with `vkReleaseSwapchainImagesKHR`. Left:
  not run on a driver that defers presentation (NVIDIA, MoltenVK) or on
  Windows (see [WINDOWS_TESTING.md](WINDOWS_TESTING.md)); the validation
  layer doesn't check this part (see Testing, Fallbacks).
- MoltenVK (portability subset): `imageViewFormatSwizzle` is queried and
  enabled; without it the fragment shader sets the alpha of RGB textures.
  The bindings of stride 0 aren't allowed by the portability subset at
  all (not only without `vertexAttributeAccessBeyondStride`: a stride must
  be at least `minVertexInputBindingStrideAlignment`), so on such a device
  they are read per instance with a real stride (see Implementation
  notes, Draws). Both forced on RADV and llvmpipe (`r_vkFallbacks 3`): the
  same frames. Left: not run on MoltenVK.
- Lines without `VK_KHR/EXT_line_rasterization` or `bresenhamLines`:
  forced with `r_vkFallbacks 4`, the same frames on RADV, line pixels
  moved here and there on llvmpipe (strict lines), no stipple (see
  Fragment pipeline). Debug drawing only.

Windows: the same code applies; the CI build fetches Khronos' Vulkan-Headers
(see Build and run), so no Vulkan SDK is needed. The Vulkan-specific files
had never been compiled with MSVC before that CI build (they have no
variable-length array, checked with clang's `-Wvla`).
