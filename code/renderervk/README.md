# renderervk — Vulkan renderer

`renderer_vulkan` is a copy of the GL1 renderer (`code/renderergl1`) with every
OpenGL call removed. All GPU work goes through the interface in [`vk.h`](vk.h),
which the `vk*.c` files implement with Vulkan. Select it with
`cl_renderer vulkan`.

Status (2026-10-13, branch `vulkan-renderer`): milestones 1 and 2 of the plan
below are done, with parts of 3 and 5.

- the main menu matches GL1 pixel for pixel, except the animated ceiling fan
  (its frame depends on the wall clock)
- `dm/mohdm1` renders like GL1 (world, lightmaps through the second texture
  unit, models, alpha-tested foliage, HUD). What differs is
  timing-dependent (swaying foliage, HUD flashes, sub-pixel camera
  differences) plus a few pixels along polygon edges. There is no fog and no
  portal clipping yet
- no validation or synchronization-validation message (`r_vkValidation 2`)
  during startup, the menu, a map load, `vid_restart` (also with a mode
  change and inside a map), window resizes (`r_allowResize 1`), `r_shadows 2`,
  `r_measureOverdraw 1`, `r_showImages 1`, texture mode changes and shutdown
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
| `shaders/generic.vert`, `shaders/generic.frag` | the one shader pair; the fragment stage is specialized per pipeline |
| `shaders/compile_shaders.sh` | regenerates `vk_shaders.c` with `glslc` |
| `vk_shaders.c` | generated SPIR-V as C arrays (committed, so builds don't need `glslc`) |
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
`wait` counts frames, so cap the frame rate with `+set com_maxfps 60` or the
client is still connecting when the script ends. Use `devmap` so cheat cvars
(`r_showtris`, ...) work.

```
devmap dm/mohdm1
wait 900
screenshot vktest_base
wait 5
quit
```

Do not use `s_initsound 0`: the game then crashes in
`ClientGameCommandManager::PlaySound` on the first footstep, with either
renderer (upstream bug). Mute with `s_volume 0`, `s_musicvolume 0`,
`s_ambientvolume 0` instead. `+set in_nograb 1` keeps the mouse free.

Screenshots land in `<fs_homepath>/main/screenshots/` as uncompressed 24-bit
TGA. Taking the same screenshot with both renderers and diffing the files is
the quickest parity check. Things learned while doing that:

- `com_maxfps` does not cap the menus: there `wait 1000` lasts about a second.
- Commands appended while a script waits run after the whole script. The
  `vid_restart` that a window resize schedules is one of them, so a script
  with a long `wait` never sees it. The client also reads console commands
  from stdin (a FIFO works), which is a way to drive resize tests with
  `xdotool windowsize`.
- Depending on timing, a map can stop at its loading screen with a CONTINUE
  button (with both renderers); `finishloadingscreen` dismisses it.
- Time-dependent elements differ between any two runs: the menu fan, swaying
  foliage, HUD flashes (the compass), and the camera can be off by a
  sub-pixel amount, which shows up as ±1 differences on mipmapped surfaces.
  Compare the menu, or mask those areas.
- The game archives cvars and autosaves into `fs_homepath`; delete
  `main/configs` and `main/save` between test runs to start from the same
  state.
- `r_anaglyphMode 1` is a fatal error in OpenMoHAA with either renderer
  (`RE_BeginFrame: Stereo is enabled, but stereoFrame was 0`).
- `PRINT_WARNING` and `PRINT_DEVELOPER` messages only show with
  `developer 1` in OpenMoHAA (`CL_RefPrintf`), so the backend prints
  validation messages and its own warnings with `PRINT_ALL`.

## Backend contract

`vk.h` documents each call. The points below are what OpenGL used to provide
implicitly and the Vulkan implementation reproduces.

### Coordinate conventions

- Projection matrices are OpenGL ones (column-major, clip z in [-w, w], y up).
  The backend remaps depth with `z' = (z + w) / 2` (pre-multiplied into the
  projection, then multiplied with the modelview on the CPU and pushed as one
  matrix) and flips y with a negative viewport height (core since Vulkan 1.1).
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
   `d = |z_eye|`, `rgb = mix(fogColor.rgb, rgb, f)`; alpha unchanged.
5. User clip plane: discard where `dot(plane, eyePos) < 0` (use
   `gl_ClipDistance`; eye position = modelview * vertex).
6. Blending, color mask, depth and stencil per the state.

Steps 4 and 5 are not implemented yet (milestone 4).

Stencil ops are the clamping GL ones (`INCREMENT_AND_CLAMP`,
`DECREMENT_AND_CLAMP`). Stencil shadows (`r_shadows 2`) and
`r_measureOverdraw` need at least 4 stencil bits.

Polygon offset (`vk_set_polygon_offset`, from `r_offsetfactor` -1 /
`r_offsetunits` -2) maps to depth bias slope/constant factors, for filled
polygons only (`GL_POLYGON_OFFSET_FILL`).

Line width above 1 needs `wideLines`; stippling needs
`VK_EXT_line_rasterization`. Both only affect debug drawing and can be ignored
when unsupported.

### Clears

`vk_clear` clears the current scissor rectangle. Like `glClear` it honours the
color mask, the depth write bit of the current state and the stencil write
mask. Depth clears to 1.0, stencil to 0. `color` may be NULL when
`CLEAR_COLOR` is not set. The backend uses `vkCmdClearAttachments`, which
ignores masks, and draws a quad for a color clear with a partial color mask; a
partial stencil write mask clears all bits (the renderer never sets one).

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
  `r_measureOverdraw`, and one depth read per visible flare per frame in
  `RB_TestFlare`). The flare reads are the expensive case: consider reading
  depth from the previous frame or replacing the test with a query.

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
  `samplerAnisotropy`, `textureCompressionBC`, `shaderClipDistance` (for
  milestone 4). `VK_KHR_portability_enumeration`/`_subset` are enabled when
  present (MoltenVK).
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
  semaphore, a vertex/index buffer (4 MB, doubled when full) and a 32 MB
  staging buffer. The rendering is submitted first; then the swapchain image
  is acquired and the present command buffer (blit and layout transitions)
  is submitted with the frame fence, which covers every earlier submission.
  The semaphores signaled for presentation are per swapchain image (presents
  have no fence). The swapchain is recreated when the drawable size changes
  or on `VK_ERROR_OUT_OF_DATE_KHR`/`VK_SUBOPTIMAL_KHR`; with no area
  (minimized) the frame is submitted without presenting.
- **Mid-frame flush.** `vk_read_pixels`, and uploads that don't fit in the
  staging buffer, submit what the frame recorded so far, wait for it and
  continue in the same command buffers (a new main-buffer "segment").
  Uploads larger than the staging buffer get a temporary buffer.
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
  separate bindings. Projection × modelview is one push constant.
- **Pipelines** (`vk_pipeline.c`). The key packs blend factors, depth
  test/write/func, polygon mode, cull mode, topology, RGB color mask, alpha
  function (and its reference value), polygon offset on/off, color array or
  constant color, texture env per unit (0 for an unused unit), "RGB texture
  with TEXENV_REPLACE" per unit, and the stencil function and ops. Viewport,
  scissor, depth bias, line width and stencil reference/masks are dynamic.
  The fragment shader is specialized for texture envs and the alpha test.
  Pipelines are created on first use and cached for the life of the device,
  with a `VkPipelineCache` that is not saved to disk yet.
- **Shutdown and `vid_restart`.** `vk_shutdown` waits for the device and
  destroys everything, including the instance; `vid_restart` recreates the
  window, so the backend starts from scratch.

## GL1 quirks kept on purpose

- `Set2DWindow`, the stencil shadow code and `RE_DrawBackground_TexSubImage`
  change cull state behind `GL_Cull`'s cache, like GL1 did.
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
  surfaces can resolve differently from GL1's 24-bit buffer.

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

Also done ahead of plan, but only checked through the `dm/mohdm1` comparison
and validation, not on dedicated test views: the second texture unit, the
alpha test, polygon offset, and the stencil state (`r_shadows 2` and
`r_measureOverdraw` run without validation messages; a temporary self-test
read back the expected stencil and depth values).

Remaining:

3. Parity check of what is already implemented: decals (polygon offset),
   alpha-tested and blended surfaces, models with `TEXENV_REPLACE`/`DECAL`,
   a single-player map. Needs a deterministic camera: the runs above differ
   by timing.
4. Fog (`vk_set_fog*`: eye-space distance per vertex, which needs the
   modelview's z row in the push constants), user clip planes for portals
   and mirrors (`gl_ClipDistance`, plane in object space), depth ranges
   and the sky in 3D views.
5. Flares (depth readback per flare, see above), lines and debug modes
   (`r_showtris` shows nothing in screenshots with either renderer; check
   it on screen), line stipple, cinematics, dynamic lightmaps, ghost
   textures, AVI capture, `GLS_CLAMP_EDGE`.
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
- `RB_TestFlare` resets `glState.finishCalled`, so with a flare in view
  `vk_finish` runs at every swap (one frame in flight), on top of the
  readback per flare.

Windows: the same code applies; vendor the Vulkan headers (or require the
Vulkan SDK) so CI can build without extra installs. `find_package(Vulkan)`
needs the loader library to be found even though it isn't linked.
