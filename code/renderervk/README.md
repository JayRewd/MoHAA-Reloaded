# renderervk — Vulkan renderer

`renderer_vulkan` is a copy of the GL1 renderer (`code/renderergl1`) with every
OpenGL call removed. All GPU work goes through the interface in [`vk.h`](vk.h),
which the `vk*.c` files implement with Vulkan. Select it with
`cl_renderer vulkan`.

Status (2026-10-14, branch `vulkan-renderer`): milestones 1 to 6 of the plan
below are done.

- the main menu matches GL1 pixel for pixel, except the animated ceiling fan
  (its frame depends on the wall clock)
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
  samples
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
| `vk_image.c` | texture handles and storage, the memory blocks textures are placed in, samplers, descriptor sets, staging and ordering of uploads |
| `vk_pipeline.c` | pipeline cache keyed on the state that is baked into pipelines, pipeline creation, the `VkPipelineCache` file |
| `tr_staticgeom.c` | the static geometry: world surfaces and static models uploaded at map load, and the back end batches drawn from it (see Implementation notes) |
| `shaders/generic.vert`, `shaders/generic.frag` | the shaders of every pipeline; the vertex shader is built with and without `gl_ClipDistance`, the fragment stage is specialized per pipeline |
| `shaders/compile_shaders.sh` | regenerates `vk_shaders.c` with `glslc` |
| `vk_shaders.c` | generated SPIR-V as C arrays (committed, so builds don't need `glslc`) |
| `tools/` | GL1/Vulkan screenshot comparison and performance scripts, view lists (see Testing) |
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
| `r_ext_multisample` (latched, shared with GL1) | 0 or 1: off; 2 to 16: samples (the largest the device supports up to that), see Implementation notes |
| `r_staticGeometry` (default 1) | 0 tessellates world surfaces and static models on the CPU every frame instead of drawing them from the static geometry (see Implementation notes) |

The console command `vkinfo` prints the backend's counters since its last
call: frames, render passes, submissions, mid-frame flushes, draws,
pipeline and descriptor set binds, bytes copied into vertex and staging
buffers, uploads ordered with draws, textures, pipelines and device memory
allocations created (with the microseconds spent creating them), depth
images replaced to become readable or not (see Implementation notes,
Render target), time spent waiting for frame fences, in `vkQueueSubmit`, `vkAcquireNextImageKHR`
(before a present and right after one) and `vkQueuePresentKHR`, from the
start of a frame's recording to its present, and the GPU time of the frames
from timestamps (main command buffer, copy to the swapchain image, whole
frame); then the device memory objects, memory blocks and pipelines that
exist and whether the depth image is readable. `tools/perf.py` reads it.

The pipeline cache is saved as `vkpipelines.cache` in `fs_homepath/main` at
shutdown and `vid_restart` (when it grew) and given to the driver at
startup when the device and driver match.

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
  uncapped, in interleaved runs (see Performance); `rtimer/` builds the
  `renderer_timed.so` wrapper it uses and `libstacks.so`, a stack sampler
  whose output `stacks.py` sums up.
- `summary.py`: the worst view's numbers of several lists after
  `compare.sh` (`--wide` for the `WIDE=1` runs), in the form of the
  milestone 6 table below, with that table's row next to them
  (`--readme README.md`).

Changes that should not change the picture (the performance work of
milestone 6) are best checked against the previous Vulkan build rather
than GL1: copy the old `renderer_vulkan.so` to `renderer_<name>.so` in the
binary directory, run the list with `run.sh <name> <list>` and compare
its shots with `tgadiff.py`; the frames of a deterministic view are then
identical to the bit, run after run.

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

`m3l1a` is not in the table: the beach artillery shakes and rolls the
camera at random, so the two renderers see slightly different views of the
open sea after the two minutes in the landing craft (GL1's horizon was
tilted in the run made, 97 % of the pixels differ); `obj_team3_deepbluesea`
covers its sea shader. No difference needed a change to the renderer:
besides the causes above, the two views that looked like bugs were taken
from outside the world, and GL1 sometimes saved a black or stale frame
(see Deterministic 3D views).

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
code/renderervk/tools/rtimer/build.sh      # renderer_timed.so and libstacks.so into build-vk
code/renderervk/tools/perf.py --list code/renderervk/tools/views/perf.txt \
    --sizes 640x480,1920x1080 --renderers opengl1,vulkan --runs 3 --json out.json
code/renderervk/tools/perf.py m6l1a 6152 -6528 5500 0 135 0 --runs 1 --stacks out
code/renderervk/tools/stacks.py out_m6l1a_640x480_vulkan_1.txt --tree RB_RenderDrawSurfList --callers CM_BoxTrace
```

Each measurement starts a game, loads the map with `fixedtime 16`, puts the
camera at the view, waits 2 s of game time and times 1000 frames with the
frame rate uncapped (`com_maxfps 0`, `r_swapInterval 0`, no validation).
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

- `com_maxfps 0` still means at most 1000 frames per second: `Com_Frame`
  waits until a whole millisecond has passed since the last frame. On an
  idle machine most views reach that at 640x480 with both renderers, so the
  frame time no longer shows differences; the renderer time does.
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
1080p after that; not done.

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
`m3l3`, but 1 pixel in about 10^5 rounds one step lower, see State).

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
asks for no alpha bits): `DST_ALPHA` and `ONE_MINUS_DST_ALPHA` blend
factors are the constants 1 and 0 and `SRC_ALPHA_SATURATE` gives 0, as in
GL1, whatever the color target's alpha holds (a swapchain image's is
undefined until written; only a test shader, `textures/mohtest/brick`,
uses `GL_DST_ALPHA`). Alpha is not written, except with multisampling:
there pipelines that write all of RGB write alpha too (1 without blending,
the stored value with it), because a write mask without alpha is a
partial write, which made a multisampled frame of the `m6l1a` forest 6 %
slower on the GPU (Radeon 780M, see Performance). Without multisampling it
saves nothing, and RADV then rounds 1 pixel in about 10^5 one step lower
than with the RGB mask (and GL1), so the RGB mask stays.

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
  (MoltenVK, with `VK_KHR_portability_enumeration` on the instance),
  `VK_KHR_create_renderpass2` with `VK_KHR_depth_stencil_resolve`
  (multisampling).
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
  depth/stencil image is always offscreen. Without multisampling it can
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
  semaphores signaled for presentation are per swapchain image (presents
  have no fence). The swapchain is recreated when the drawable size changes
  or on `VK_ERROR_OUT_OF_DATE_KHR`/`VK_SUBOPTIMAL_KHR`; with no area
  (minimized) the frame is submitted without presenting.
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
  before the swapchain is recreated or destroyed.
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
  sampler. Images are placed in 64 MB blocks of device memory, first fit
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
  command buffer in order with the draws. New textures, level loading,
  cinematics and dynamic lightmaps take the first path, so they don't break
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
  mid-frame flush). In the measured views this saves 1 to 4 % of the
  vertex bytes (most surfaces are one multitextured pass). Draws from the
  static geometry (below) copy nothing but, when they need them, their
  indexes and a constant color; vertex and index buffers are only bound
  again when they or their offsets change.
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
  whose shader has no deform, isn't the sky, with no dynamic lightmap and no
  debug drawing, every stage's texture coordinates from `tcGen texture` or
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
  texture envs, the alpha test, fog, the clip plane discard fallback and
  the alpha written without blending (see State);
  pipelines with a clip plane use the vertex shader built with
  `gl_ClipDistance`.
  Pipelines are created on first use and cached for the life of the device,
  with a `VkPipelineCache` saved to `vkpipelines.cache` (fs_homepath) when
  the device is destroyed, if it grew, and loaded at startup when its
  header (vendor, device, pipeline cache UUID) matches; a driver that
  refuses the data gets an empty cache. With a cold driver shader cache
  (`MESA_SHADER_CACHE_DISABLE=true`) a pipeline takes about 1.2 ms to
  create, 18 ms for the 15 pipelines up to the first view of `m6l1a`, and
  0.2 ms with the file; RADV's own disk cache brings it to 0.5 ms without
  the file.
- **Counters** (`vkinfo`, `vkStats_t`; `staticDraws` counts the draws from
  the static geometry). Every `vkAllocateMemory` goes
  through `VK_AllocateMemory`, which counts it; GPU times come from
  timestamps at the start and end of the main command buffer and around the
  copy to the swapchain image (`TOP_OF_PIPE`/`BOTTOM_OF_PIPE`), read without
  waiting when the frame slot is reused; times on the CPU use
  `SDL_GetPerformanceCounter`.
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

Remaining:

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
- Step 3: skinning of skeletal models on the GPU (`RB_SkelMesh` is 5 to 8 %
  of the forest frames); and the LOD terrain, which is re-split every frame
  (`RB_DrawTerrainTris`), still copies its vertexes.

- At 3840x2160 the GPU-bound `dm/mohdm3` overview and `m3l3` path take
  3 to 4 % more GPU time than GL1's frames (2.53 against 2.46-2.48 ms,
  3.05 against 3.00 ms), a fixed ~0.08 ms per frame that doesn't come from
  any one kind of content (the same with `r_drawterrain 0`, `r_fastsky 1`,
  `r_drawstaticmodels 0` or `r_drawentities 0` in both renderers), is not
  the depth clear (a fast clear: `RADV_DEBUG=nofastclears` adds 0.38 ms)
  nor the swapchain image's transition for presentation (0.006 ms), and
  doubles with primitive binning off in both drivers, so it is memory
  traffic again. Writing alpha (1.2 %, but a rounding difference, see
  State), the depth format without stencil (0.5 %) and DCC on textures
  (`RADV_DEBUG=forcecompress`: -2.3 % in one view, +0.8 % in the other)
  account for part of it. At 1080p these views are CPU-bound, where the
  Vulkan renderer is faster.
- Leads left from the main pass investigation (see Performance), none
  worth more than a few percent as far as measured: a depth format
  without the unused stencil plane when `r_stencilbits` is 0 (D32 instead
  of D32S8: 0.5 % less GPU time at 3840x2160, nothing at 1080p or with
  multisampling); RADV's
  primitive binning uses 128x128 bins where radeonsi uses 128x64 for
  depth-tested draws on this GPU (in `radv_gfx10_compute_bin_size`, Mesa
  26.0.8, the final `extent.height = MAX2(extent.width, ...)` and the depth
  bin's height taken from the color bins look like typos; a 128x128 bin
  needs more depth tag space than the 78 KB the formula allows). Neither
  can be changed from the application; testing the bin size needs a
  patched RADV (not done), and if it matters the fix belongs in Mesa.
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

Known risks, not seen on RADV but worth handling before shipping:

- Presentation semaphores (one per swapchain image) are reused across
  swapchain recreation and destroyed at shutdown after `vkDeviceWaitIdle`,
  which formally doesn't cover a queued present's semaphore wait. Drivers
  that defer presentation (NVIDIA, MoltenVK) are the likeliest to object.
  `VK_EXT_swapchain_maintenance1` present fences would make it exact. The
  same holds for the swapchain image acquired ahead (Frame pacing), released
  with an empty submission before the swapchain goes.
- MoltenVK (portability subset): the `A = ONE` view swizzle needs
  `imageViewFormatSwizzle` and the stride-0 vertex bindings need
  `vertexAttributeAccessBeyondStride`; neither is queried. The fallbacks
  are forcing alpha to 1 in the shader for RGB textures and pushing the
  constant color as a push constant.
- Without `VK_KHR/EXT_line_rasterization` (or without `bresenhamLines`)
  lines use the driver's default rule, which may differ from GL's by a pixel
  here and there; only debug drawing has lines.

Windows: the same code applies; vendor the Vulkan headers (or require the
Vulkan SDK) so CI can build without extra installs. `find_package(Vulkan)`
needs the loader library to be found even though it isn't linked.
