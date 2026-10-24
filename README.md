# MoHAA Reloaded: native Vulkan

**MoHAA Reloaded** brings [OpenMoHAA](https://github.com/openmoh/openmohaa) natively to modern hardware. This is its `vulkan` branch: OpenMoHAA with a Vulkan renderer of its own, for 64-bit Linux and Windows (Windows not played yet). It plays Medal of Honor: Allied Assault, Spearhead and Breakthrough with your retail game data, on existing MOHAA and OpenMoHAA servers.

| Branch | Renderer | Status |
|---|---|---|
| `metal` | Metal, macOS on Apple Silicon | [Released](https://github.com/pstngh/MoHAA-Reloaded/releases) |
| **`vulkan`** (this one) | Vulkan, Linux and Windows | [Released](https://github.com/pstngh/MoHAA-Reloaded/releases/tag/v1.0.0) (Windows untested) |

> [!IMPORTANT]
> **This is a fork, not the upstream project.** Problems with this port belong on [this repository's tracker](https://github.com/pstngh/MoHAA-Reloaded/issues) (say which branch: `metal` or `vulkan`), not upstream's tracker or Discord.

- **Vulkan, drawn its own way.** `renderer_vulkan` starts from upstream's GL1 renderer with every OpenGL call replaced, then redesigned: the world and the static models are uploaded once per map and drawn from GPU memory, soldiers are skinned and lit in the vertex shader, dynamic lights are per pixel, and a map's pipelines are built while it loads, so nothing stalls the first time it is drawn. Vulkan is loaded at run time, and no OpenGL is left: upstream's GL1 and GL2 renderers are gone from the code, the build and the downloads.
- **Low input latency.** Frames are drawn straight into the swapchain image, with at most two frames in flight and nothing drawn from older input: from reading the input to the present takes 0.47 and 0.83 ms (medians) in two real matches at 1920x1080 uncapped, against 0.66 and 1.31 with upstream's GL1 renderer.
- **Movement apart from the frame rate.** The commands the server runs come from their own clock (`cl_cmdfps`, below), each exactly as long at any frame rate, and the view turns with every frame.
- **Smooth matches.** What used to load in the middle of a match (models announced late, a sound's first play, the soundtrack at a round restart) now happens while the map loads.
- **Fast.** In two real matches played back at 1920x1080 on an AMD Radeon 780M (an integrated GPU), the renderer's CPU time per frame is about half of upstream's GL1 renderer's (0.35 ms against 0.65, 0.68 against 1.26), frames take 35 to 40 % less time, and the 99th percentile is 1.2 ms against 1.9 and 1.4 against 2.4.
- **The Omaha sound mixer** (JayRewd's) instead of OpenAL: miniaudio for the device, gains per kind of sound, ducking, sounds behind walls muffled, and HRTF for headphones.
- **Same game online.** The network protocol, the server-side game code and the data formats are upstream's.

Needs a GPU with a Vulkan 1.1 driver (on Linux Mesa's RADV, ANV or NVK, or NVIDIA's driver; on Windows any current AMD, NVIDIA or Intel driver). The Linux build needs glibc 2.34 or later (Ubuntu 22.04, Debian 12, Fedora 35 or later), the Windows build the [Microsoft Visual C++ 2015–2022 Redistributable (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe), which most PCs with games already have; SDL2 and libcurl come with both. Developed and measured on an AMD Radeon 780M with Mesa's RADV driver under KDE Plasma (Wayland); also played on an NVIDIA RTX 4070 Ti.

## Install

1. Download `openmohaa-vulkan-1.0.0-linux-amd64.tar.gz` (64-bit Linux) or `openmohaa-vulkan-1.0.0-windows-x64.zip` (64-bit Windows) from [Releases](https://github.com/pstngh/MoHAA-Reloaded/releases/tag/v1.0.0) and extract it (`tar xf openmohaa-vulkan-1.0.0-linux-amd64.tar.gz`; on Windows, right-click and Extract All). Nobody has played the Windows build on Windows yet: please report how it runs.
2. Find your MOHAA game folder: the folder with `main/` (`Pak0.pk3` to `Pak6.pk3`; `mainta/` and `maintt/` for the expansions). Upstream's [installation guide](docs/markdown/01-intro/01-installation.md#linux-and-macos-users) shows how to get it from the GOG installer.
3. Copy everything in the extracted folder into the game folder, next to `main/` (its own `main/` only holds the headphone sound set: let it merge), and start `launch_openmohaa_base` (or `launch_openmohaa_spearhead`, `launch_openmohaa_breakthrough`; `.exe` on Windows).

On Linux you can also leave the game folder as it is and start the game from the extracted folder: `./openmohaa +set fs_basepath /path/to/mohaa`. `INSTALL.txt` in the download has the same steps. Configs go to `~/.config/openmohaa/` and saves and screenshots to `~/.local/share/openmohaa/` on Linux, all of them to `%APPDATA%\openmohaa\` on Windows, as with upstream's OpenMoHAA. Without a Vulkan driver the game stops and says so.

Builds of newer commits come from [CI on this branch](https://github.com/pstngh/MoHAA-Reloaded/actions/workflows/branches-build.yml?query=branch%3Avulkan) (`out-linux-amd64`, `out-windows-x64`; needs a GitHub login, kept for a few days), or build it yourself (below).

## Settings that matter here

Low input-to-screen time with even frames, as played on a 240 Hz display (tearing allowed):

```
seta com_maxfps 250
seta r_swapInterval 0
seta r_fullscreen 1
```

Change settings in the console (`seta name value`) or at launch (`+set name value`). Latched settings take effect after `vid_restart` (sound settings after `snd_restart`). Both ports' settings side by side: [the settings guide](https://github.com/pstngh/MoHAA-Reloaded#settings-guide).

**Added by this port:**

| Setting | Default | What it does |
|---|---|---|
| `cl_cmdfps` | `100` | **Movement apart from the frame rate.** The client makes the server's commands at this rate, 85 to 125 a second, each exactly 1000 / `cl_cmdfps` whole ms of movement (10 at 100, 8 at 125), at any `com_maxfps`; the view turns with every frame's mouse look. Buttons and keys go with the next command (5 ms after the frame on average at 100). |
| `r_maxDlights` | `4` | Dynamic lights a scene keeps, the nearest the view, 0 to 64 (latched). Real matches have 4 at most at once. |
| `r_vkSubmitThread` | `0` | `1`: a thread of its own submits and presents the frames, so the main thread never waits in the driver's present or acquire (which stalled up to 1.8 ms here, 1.3 ms every 341 frames on NVIDIA). Uncapped, 3 to 21 % more frames a second; at 250 the same frames, input to present 0.01 to 0.02 ms longer. 5 swapchain images (3 with vsync); latched. |

**Upstream's, with new behavior or defaults:**

| Setting | Default | What it does here |
|---|---|---|
| `com_maxfps` | `125` (upstream `85`) | The render cap, exact now (144, 165 and 240 ran at 167, 167 and 250 before); above 1000 and `0` (uncapped) work too. Movement doesn't depend on it (`cl_cmdfps`). |
| `r_swapInterval` | `0` | `0` shows each frame at once (it can tear; mailbox where immediate is missing). `1` is vsync (FIFO), `-1` vsync that tears when a frame is late (FIFO relaxed). Latched. |
| `r_ext_multisample` | `0` | `2` to `16`: multisampling (latched). |
| `s_speaker_type` | `0` | `1` is **headphones** (HRTF, mixed at 48 kHz): sounds behind, above or below you sound there. Set it at launch. |
| `cl_maxpackets` | `125` (upstream `30`) | Every command in its own packet. |
| `m_filter` | `0` (upstream `1` on macOS) | `1` averages each frame's mouse movement with the frame before's: half of every movement shows a frame later. |
| `r_uselod` | `0` (upstream `1`) | `0`: soldiers and weapons at full detail at any distance. Not saved in configs: set it at launch or in `autoexec.cfg`. |
| `ui_console` | `1` (upstream `0`) | The console key works; the game still starts at the main menu. |
| `cl_playintro` | `0` (upstream `1`) | `1` plays the intro videos at startup. |

Configs saved by earlier builds keep the values they hold: a saved `com_maxfps 85`, `cl_maxpackets 30` or `ui_console 0` stays until you change it.

## Online

- The server side is upstream's: this client plays on stock MOHAA and OpenMoHAA servers.
- A server receives `cl_cmdfps` commands a second (100 by default), each exactly one step long, as from a stock client at that frame rate.

## Building

Linux needs CMake 3.25 or later, Ninja, Clang or GCC, Flex, Bison, SDL2, libcurl and the Vulkan headers 1.4 (Khronos' Vulkan-Headers; distributions' packages may be too old: Ubuntu 22.04's are):

```bash
sudo apt-get install -y cmake ninja-build clang lld flex bison libsdl2-dev libcurl4-openssl-dev
```

```bash
git clone --depth 1 -b v1.4.341 https://github.com/KhronosGroup/Vulkan-Headers.git
cmake -S Vulkan-Headers -B Vulkan-Headers/build -DCMAKE_INSTALL_PREFIX="$PWD/vulkan-headers"
cmake --install Vulkan-Headers/build
```

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DVulkanHeaders_DIR="$PWD/vulkan-headers/share/cmake/VulkanHeaders"
cmake --build build
```

Run it from `build/Release` with your game data: `./openmohaa +set fs_basepath /path/to/mohaa`, or install it into the game folder (`-DCMAKE_INSTALL_PREFIX=/path/to/mohaa`, then `cmake --install build`). `glslc` is only needed to change the shaders, the Vulkan validation layers only for `r_vkValidation`. CI builds Linux and Windows on every push (`.github/workflows/branches-build.yml`); macOS isn't built for this branch.

## Looks and limits

- **Looks like upstream's GL1 renderer**, except where a change bought speed: dynamic lights per pixel (a smoother falloff than GL1's lightmap texels), up to 64 of them and on doors and lifts too, and a few pixels where the drivers rasterize differently.
- **Tested on Linux only**, on AMD (Mesa) and NVIDIA. Windows builds but hasn't been played; macOS (MoltenVK) is untested. Reports are welcome.

## More

- The full record of the port (the plan, every measurement, how each part works, the tests): [`code/renderervk/README.md`](code/renderervk/README.md).
- Upstream, its documentation and community: [openmoh/openmohaa](https://github.com/openmoh/openmohaa). Its guides in [`docs/markdown`](docs/markdown) apply here except for the renderer, the sound system and building.
- Thanks to the OpenMoHAA team for the engine (built on [ioquake3](https://github.com/ioquake/ioq3) and the F.A.K.K SDK), to JayRewd for the Omaha sound mixer and the renderer work merged here and the ideas re-implemented, and to notuern for testing on NVIDIA.
- OpenMoHAA is GPL-2.0 ([`COPYING.txt`](COPYING.txt)). It is an independent project, not affiliated with or endorsed by Electronic Arts.
