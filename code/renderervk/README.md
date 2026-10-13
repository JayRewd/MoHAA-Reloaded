# renderervk — Vulkan renderer (GL-free, backend not implemented yet)

`renderer_vulkan` is a copy of the GL1 renderer (`code/renderergl1`) with every
OpenGL call removed. All GPU work goes through the interface in [`vk.h`](vk.h).
[`vk.c`](vk.c) currently implements it with no-op stubs, so the game runs end to
end (menus, maps, cgame, screenshots) and shows a black window. The next step is
to implement `vk.c` with Vulkan.

Status at handoff (2026-10-13, branch `vulkan-renderer`, based on upstream `a2f3401`):

- builds on Linux with `-DBUILD_RENDERER_VULKAN=ON`
- `renderer_vulkan.so` imports no GL symbol and links only SDL2, libm, libstdc++ and libc
- at runtime no libGL/libGLX/libEGL/Mesa GL driver is loaded (checked with
  `LD_DEBUG=libs`); SDL loads the Vulkan loader because the window is created
  with `SDL_WINDOW_VULKAN`
- loading `dm/mohdm1`, playing ~15 s, `screenshot`, `r_showtris 2`,
  `r_shownormals 1`, `r_showImages 1`, `r_shadows 2`, `r_measureOverdraw 1` and
  `quit` all run without errors
- `renderergl1` is unchanged and still builds and runs; it is the visual
  reference for the Vulkan implementation

## Layout

| File | Role |
| --- | --- |
| `vk.h` | backend interface (the contract below) |
| `vk.c` | backend implementation, currently stubs |
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

```sh
cmake -S . -B build-vk -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_RENDERER_VULKAN=ON
cmake --build build-vk
cd build-vk/RelWithDebInfo
./openmohaa +set cl_renderer vulkan +set fs_basepath ~/Documents/moh \
    +set fs_homepath /tmp/moh-home +set r_fullscreen 0 +set r_mode 3
```

`cl_renderer opengl1` runs the GL1 renderer from the same build for comparison.

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

Screenshots land in `<fs_homepath>/main/screenshots/`. Taking the same
screenshot with both renderers and diffing the TGA files is the quickest parity
check.

## Backend contract

`vk.h` documents each call. The points below are what OpenGL used to provide
implicitly and the Vulkan implementation has to reproduce.

### Coordinate conventions

- Projection matrices are OpenGL ones (column-major, clip z in [-w, w], y up).
  Remap depth with `z' = (z + w) / 2` (pre-multiply the projection) and flip y
  (negative viewport height, core since Vulkan 1.1).
- Viewport, scissor and `vk_read_pixels` use window coordinates with the
  origin at the bottom-left: Vulkan `y = windowHeight - (y + height)`.
- `vk_set_depth_range(n, f)` maps to `VkViewport::minDepth/maxDepth`. The
  renderer uses (0, 1), (0, 0.3) for the weapon, (1, 1) for the sky and (0, 0)
  for debug lines.
- `vk_read_pixels` returns rows bottom row first, tightly packed. Depth is the
  window-space depth (the flare code converts it back with `2 * depth - 1`).
- Texture data is uploaded top row first and t = 0 addresses the first row,
  same as Vulkan, so textures need no flip.
- Front faces are counter-clockwise in window space (GL default). Once y is
  flipped (negative viewport height or a flipped projection, they give the
  same framebuffer coordinates), `VK_FRONT_FACE_COUNTER_CLOCKWISE` matches GL.

### State (`vk_set_state`)

Blend factors: the usual mapping of `GLS_SRCBLEND_*` / `GLS_DSTBLEND_*`; no
blend bits means blending off. `GLS_DEPTHFUNC_EQUAL` selects EQUAL, otherwise
LESS_OR_EQUAL. `GLS_DEPTHTEST_DISABLE`, `GLS_DEPTHMASK_TRUE` and
`GLS_POLYMODE_LINE` (front and back) map directly.

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

### Fragment pipeline

1. Vertex color: per-vertex `colors` or the constant `color` of the geometry.
2. Texture unit 0 (if `st[0]` is set) combines with the vertex color using its
   texture env, unit 1 (if `st[1]` is set) combines with the result of unit 0.
   The formulas are next to `texEnv_t` in `vk.h`. For `TEXFMT_RGB8/RGB5`
   textures alpha samples as 1 (use an `A = ONE` swizzle); with
   `TEXENV_REPLACE` GL then keeps the previous alpha instead of 1.
3. Alpha test.
4. Fog, if enabled: `f = clamp((end - d) / (end - start), 0, 1)` with
   `d = |z_eye|`, `rgb = mix(fogColor.rgb, rgb, f)`; alpha unchanged.
5. User clip plane: discard where `dot(plane, eyePos) < 0` (use
   `gl_ClipDistance`; eye position = modelview * vertex).
6. Blending, color mask, depth and stencil per the state.

Stencil ops are the clamping GL ones (`INCREMENT_AND_CLAMP`,
`DECREMENT_AND_CLAMP`). Stencil shadows (`r_shadows 2`) and
`r_measureOverdraw` need at least 4 stencil bits; set `glConfig.stencilBits`
from the chosen depth format.

Polygon offset (`vk_set_polygon_offset`, from `r_offsetfactor` -1 /
`r_offsetunits` -2) maps to depth bias slope/constant factors.

Line width above 1 needs `wideLines`; stippling needs
`VK_EXT_line_rasterization`. Both only affect debug drawing and can be ignored
when unsupported.

### Clears

`vk_clear` clears the current scissor rectangle. Like `glClear` it honours the
color mask, the depth write bit of the current state and the stencil write
mask. Depth clears to 1.0, stencil to 0. `color` may be NULL when
`CLEAR_COLOR` is not set.

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
  the old contents. Copy through a staging buffer and record the copy in order
  (end the render pass, or upload into a new image and swap).
- Sampler: wrap from `image->wrapClampModeX/Y` (`WRAP_REPEAT`/`WRAP_CLAMP` =
  clamp to edge); textures with one mip level use linear min/mag filtering;
  mipmapped ones use the filter from `vk_set_texture_filter`
  (`r_textureMode`, default `GL_LINEAR_MIPMAP_NEAREST`) and anisotropy from
  `r_ext_texture_filter_anisotropic` / `r_ext_max_anisotropy`.
- `TEXFMT_RGBA4/RGB5` may be stored as 8-bit formats.
- BC formats come from DDS files; the texture env and sampler rules are the
  same.

### Frames and synchronisation

- `vk_begin_frame` is called from `RB_DrawBuffer` at the start of each frame,
  but `RE_StretchRaw` and other front-end paths can draw before the first
  command of a frame runs: start the frame lazily on the first operation.
- `vk_finish` is called at swap for frames with no 3D view (menus) and around
  cinematics, as `glFinish` was. A full device wait there is correct but slow;
  a fence wait on the last submission is enough.
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
| `textureCompression` | `TC_S3TC_ARB` if BC formats are supported | `r_ext_compressed_textures 0` (default); `.dds` files are only loaded when this is not `TC_NONE` |
| `textureFilterAnisotropic`, `maxAnisotropy` | from the device limits | `r_ext_texture_filter_anisotropic 0` |
| all of the above | | `r_allowExtensions 0` |

`vk_initialize` also fills `maxTextureSize` (uploads are downscaled to it)
and the vendor/renderer/version/extensions strings. `VKimp_Init` sets the
requested `colorBits`/`depthBits`/`stencilBits` from `r_colorbits`,
`r_depthbits` and `r_stencilbits` (24/24/8 by default, stencil 0 on Linux
like GL1); update them to the formats actually picked, as GL1 reported the
bits of the visual it got. `deviceSupportsGamma`, `vidWidth`, `vidHeight` and
`isFullscreen` come from `sdl_vkimp.c`. The backend should also honour
`r_swapInterval` (present mode) and `r_ext_multisample`; `r_stereoEnabled`
is not supported.

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

## Suggested order for the Vulkan implementation

Prerequisites: `glslc` and `vulkan-validationlayers` (both installed on the
dev machine, with Vulkan headers 1.4.341). Ship the
compiled SPIR-V as C arrays so builds don't need `glslc`. Load the Vulkan
loader through `SDL_Vulkan_GetVkGetInstanceProcAddr` and use `VK_NO_PROTOTYPES`
so the module does not link against `libvulkan`. Add an `r_vkValidation` cvar
that enables the validation layer and prints its messages.

1. Instance, surface (`SDL_Vulkan_CreateSurface` on `SDL_window`), device,
   swapchain, frame loop, `vk_clear`, `vk_read_pixels` for color. Check: a
   screenshot of a cleared frame has the clear color.
2. Textures, samplers, a dynamic vertex/index ring buffer, `vk_draw` with one
   texture unit, blending, the 2D pipeline. Check: menus match GL1 screenshots.
3. Second texture unit and texture envs, depth, cull, alpha test, polygon
   offset. Check: world and models on `dm/mohdm1` and a single-player map.
4. Fog, clip planes (portals), depth ranges, sky.
5. Stencil shadows, flares (depth readback), lines and debug modes,
   cinematics, dynamic lightmaps, ghost textures, AVI capture.
6. Parity pass against GL1 screenshots on several maps, then performance
   (pipeline cache, frame pacing, present modes from `r_swapInterval`).

Windows: the same code applies; vendor the Vulkan headers (or require the
Vulkan SDK) so CI can build without extra installs.
