# OpenMoHAA for Apple Silicon: a native Metal port

> [!IMPORTANT]
> **This is [pstngh/openmohaa-metal](https://github.com/pstngh/openmohaa-metal), a fork of [OpenMoHAA](https://github.com/openmoh/openmohaa), not the upstream project.** Everything above the horizontal rule is about this fork; below it is upstream's README, unchanged. Problems with this port (the Metal renderer, the macOS layer, the settings below) belong here, not on upstream's tracker or Discord.

This fork runs OpenMoHAA (Medal of Honor: Allied Assault, Spearhead and Breakthrough) natively on Apple Silicon Macs:

- **Metal only.** The renderer is upstream's GL1 renderer with its drawing rewritten for Metal: hand-written Metal shaders, compiled when you build and embedded in the binary. OpenGL is gone from the source and the binary. No translation layers (no MoltenVK, ANGLE or similar).
- **No SDL.** The window, keyboard, mouse and display modes are native AppKit code (`code/macos`), with raw mouse input through GCMouse.
- **arm64, macOS 26 or later only.** Windows, Linux, Intel, the GL renderers, the update checker and curl are removed from this branch. One `openmohaa.app`, ad-hoc signed, with its own [openal-soft](https://github.com/kcat/openal-soft) 1.25.2 and the game libraries inside. No dedicated server.
- **Same game online.** The network protocol, the server-side game code and the data formats are upstream's. It plays on existing MOHAA and OpenMoHAA servers with your retail game data (see [Online](#online)).
- **Built for low input latency and low overhead.** Frame pacing, the command clock, presenting and input were measured and reworked for the shortest time from your mouse to the screen (see [Speed](#speed)).

Developed and measured on one machine: an M4 Mac mini (10-core GPU, 16 GB) on macOS 26 with Xcode 27.1 and a 240 Hz display at 1920×1080. All the numbers below come from that machine.

## Building

You need:

- An Apple Silicon Mac with macOS 26 or later.
- Xcode with the macOS 26 SDK (built with Xcode 27.1 on the developer's Mac and Xcode 26.6 in CI), and its **Metal Toolchain** component. Since Xcode 26 the Metal shader compiler is a separate download: Xcode → Settings → Components, or
  ```bash
  xcodebuild -downloadComponent MetalToolchain
  ```
- CMake (3.25 or later), Ninja and Bison 3 (macOS's own Bison 2.3 is too old), for example from Homebrew:
  ```bash
  brew install cmake ninja bison
  ```

Then configure, build and install. CMake finds the Metal compiler by itself, in the folder of `xcrun -f metallib`. `CMAKE_DISABLE_FIND_PACKAGE_OpenAL` keeps CMake from picking Apple's OpenAL framework, which lacks headers the game needs (it uses the bundled openal-soft). If CMake doesn't find Homebrew's Bison by itself, add `-DBISON_EXECUTABLE="$(brew --prefix bison)/bin/bison"`.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PWD/install" -DCMAKE_DISABLE_FIND_PACKAGE_OpenAL=ON
```

Only if the build stops with "Metal shader compiler not found" (for example because the Metal Toolchain component was installed after the first configure), pass the folder that holds `metal` and `metallib` yourself: add `-DMETAL_TOOLCHAIN_BIN="$(dirname "$(xcrun -f metallib)")"` to the configure command. (CI builds with exactly these commands on every push, without that flag: a clean macOS 26 runner, Xcode 26.6, the component installed with the command above; `.github/workflows/macos-metal.yml`.)

```bash
cmake --build build
```

```bash
cmake --install build
```

The install step signs the app ad hoc and checks its seal. `install/lib/openmohaa/` then holds `openmohaa.app`, with `game.dylib` and `cgame.dylib` inside it (`Contents/MacOS`). Don't add or change files inside `openmohaa.app` afterwards: that breaks its seal. `-DBUILD_SERVER=ON` also builds the dedicated server `omohaaded` (for local tests), with a `game.dylib` beside it.

The scripts in `port/` are the port's development tools (benchmark, screenshot comparison against GL1, the latency probe, input replays). They're written for the developer's machine (`port/env.sh` has its paths and a Metal Toolchain mounted from a disk image), so use the CMake commands above to build.

## Installing and running

You need the retail game data: the folder of an installed MOHAA with `main/` (`Pak0.pk3` to `Pak6`), plus `mainta/` for Spearhead and `maintt/` for Breakthrough. Upstream's [installation guide](docs/markdown/01-intro/01-installation.md#linux-and-macos-users) shows how to get it from the GOG installer with `innoextract`.

**From the release zip** (`openmohaa-metal-<version>-macos-arm64.zip` on the [Releases](https://github.com/pstngh/openmohaa-metal/releases) page): it holds `openmohaa.app` with the game libraries inside (the 1.0.0 zip has `game.dylib`, `cgame.dylib` and `omohaaded` beside the app instead), the licenses and an `INSTALL.txt` with these steps. The app is signed ad hoc and not notarized by Apple, so macOS quarantines the download and refuses to run it. Clear the quarantine once, before the first start: in Terminal, type `xattr -dr com.apple.quarantine ` (with the trailing space), drag the unzipped folder onto the window, and press Return. For example:

```bash
xattr -dr com.apple.quarantine ~/Downloads/openmohaa-metal-1.0.0-macos-arm64
```

"Open Anyway" in System Settings → Privacy & Security is not enough for this game (checked on macOS 26.6): macOS then runs the app from a temporary read-only copy (App Translocation), where it can't find `main/` ("Couldn't load default.cfg").

Then, from the zip's folder or from your own build:

1. Copy `openmohaa.app` (from the 1.0.0 zip, `game.dylib` and `cgame.dylib` too) from the zip's folder or `install/lib/openmohaa/` into that game folder, next to `main/`. The game reads its data from the folder that holds the app. Later builds load the game libraries from inside the app and ignore a `game.dylib` or `cgame.dylib` beside it: you can delete those.
2. Start it from Finder, or from a terminal in the game folder. If the game folder is on an external drive (or in a folder macOS protects, such as Documents or Desktop), the first start asks whether `openmohaa` may access files there: allow it (again after each update: with an ad-hoc signature the permission belongs to that exact build). Allied Assault:
   ```bash
   open openmohaa.app
   ```
   Spearhead (`com_target_game 1`) and Breakthrough (`com_target_game 2`). Upstream's `launch_openmohaa_*` launchers aren't in this fork:
   ```bash
   open openmohaa.app --args +set com_target_game 1
   ```
   ```bash
   open openmohaa.app --args +set com_target_game 2
   ```

Configs, saves, screenshots, demos, logs and downloaded maps go into the game folder, as in the original game (`main/configs/omconfig.cfg`, `main/screenshots/` for the base game), or to `~/Library/Application Support/openmohaa/` (upstream's place, its [running guide](docs/markdown/02-running/01-running.md)) if the game folder isn't writable. The CD key and a server's bans stay in Application Support in every case. A game folder used for the first time reads the configs and saves OpenMoHAA kept in Application Support and saves them into the game folder from then on, unless it already holds a `main/configs/omconfig.cfg` from another OpenMoHAA build: that one wins, so delete it to keep the settings from Application Support. Metal's pipeline cache is in `~/Library/Caches/io.github.pstngh.openmohaa-metal/`. There is no PID file, so no "Abnormal Exit" prompt after a crash or a force-quit. Settings are changed in the console (`seta name value`) or at launch (`--args +set name value`). Display settings take effect after `vid_restart`.

## Online

- **The server side is upstream's.** The protocol, the game code a server runs and the data formats are unchanged, so this client plays on stock MOHAA and OpenMoHAA servers. The port's author played on public servers with it during development, then about 15 minutes in the stock mode and a short session in the decoupled mode, which became the default after it. A full match in the decoupled mode is still to come.
- **What the server receives** in the decoupled mode (below) is what a stock client sends at that command rate: the same command lengths and the same packets. That was checked against a local server that logged every packet and command.
- **No GUID:** unlike upstream's client, this one sends no `cl_guid` (it keeps no `qkey` file), as the original game's clients don't. Servers that track players by GUID (stats, admin rights, GUID bans) see you without one.
- Client-side changes to cgame only affect what you see: how your own movement is shown between commands, and caches that give the same results faster.
- This fork ships no dedicated server (`-DBUILD_SERVER=ON` builds `omohaaded` for local tests). For servers, upstream's builds are the tested ones.

## Settings that matter here

The shortest input-to-screen time on the test Mac, at 240 Hz:

```
seta cl_movefps 100
seta com_maxfps 480
seta r_fullscreen 1
seta r_noborder 1
seta r_swapInterval 0
seta in_mouseRaw 1
```

| Setting | Default | What it does here |
|---|---|---|
| `cl_movefps` | `100` | **The decoupled mode.** The commands the server runs (usercmds) come from their own clock, 85–125 a second (125 is the most `cl_maxpackets` sends, so each command gets a packet of its own), and the game renders at whatever rate `com_maxfps` sets. 100 is close to the rate MOHAA's movement physics were designed for (about 85): shorter steps catch on small bumps more often, and above 250 commands a second terrain breaks on the server. **`0` is stock:** one command per rendered frame, as upstream does. Builds up to 1.0.0 called it `cl_cmdRate`: a config's `seta cl_cmdRate` no longer does anything, so change it to `seta cl_movefps`. A value above 125 plays at 125. |
| `com_maxfps` | `85` (upstream's) | **Set it.** With `cl_movefps` on, the render cap only: any value, `0` = uncapped. Caps are exact now: 240 is 240 frames a second (upstream turns it into 250). On the 240 Hz test display, **480** showed the mouse ~1 ms sooner than 250. 960 wasn't any better, because the display shows only about 500 images a second, so half the frames never appear. `0` was ~0.4 ms better still, but keeps a whole CPU core and the GPU busy. With `cl_cmdRate 0` this is also the command rate: stay at 250 or below (125, 200, 250), because 500 and 1000 change movement on the server. |
| `cl_cmdDisplay` | `1` | How your own movement is shown between two commands. `1` runs the next command's whole step as it will go out with the keys you hold now, and shows the part of it that has passed, to the microsecond: you move on every frame, by steps as long as the server's (short steps catch on small bumps), and a key shows on the next frame that reaches the screen (first motion ~2.3 ms after the press, uncapped in fullscreen; 6 ms in stock at 250). `2` is a smoothed position one command behind. `0` shows the plain command steps. Shots are aimed where your crosshair was at the click, in every setting. Other players and moving things are drawn at the frame's moment too, not at the whole millisecond. |
| `in_mouseRaw` | `1` | `1` reads the mouse through GCMouse: raw deltas, no macOS pointer acceleration, and the buttons while the game holds the mouse. `0` uses AppKit's mouse events. |
| `r_fullscreen`, `r_noborder` | `1`, `0` | The display mode (below). |
| `r_borderlessWindow` | `0` | With `r_fullscreen 1` and `r_noborder 1`: `0` is a native fullscreen Space, `1` a borderless window over the screen (no slide animation when you switch apps or `vid_restart`). |
| `r_swapInterval` | `0` | `0` presents each frame at once (it can tear). `1` is vsync: tear-free, but about two refreshes later (~8 ms at 240 Hz). |
| `com_busyWait` | `0` | `1` spins the CPU through the whole frame wait: ~0.05–0.1 ms sooner, for a full core. |
| `cl_maxpackets` | `125` (upstream's is `30`) | Packets a second, 30–125. At 125 every command of `cl_movefps 100` goes out in its own packet; at 30 they're bundled three or four to a packet. |
| `cl_latencyStats` | `0` | `1` prints input → present times while you play (the port's latency probe). |
| `ui_console` | `1` (upstream's is `0`) | The console key opens the console. The game still starts at the main menu with the console closed (except with `developer 1`). |
| `r_uselod` | `0` (upstream's is `1`) | `0` draws soldiers and weapons at full detail at any distance. `1` lowers their detail with distance, tuned by `r_lodscale`, `r_lodcap` and `r_lodviewmodelcap`. Not saved in configs: set it at launch or in an `autoexec.cfg`. |
| `cl_playintro` | `0` (upstream's is `1`) | The intro videos at startup. `1` plays them: at launch (`--args +set cl_playintro 1`) or with `seta cl_playintro 1` in a config. |

The display modes. Every fullscreen mode goes direct to the display, bypassing macOS's compositor:

- **Fullscreen Space** (`r_fullscreen 1`, `r_noborder 1`): the cleanest on the test Mac. macOS blanks the other displays for it, so nothing else delays the game's frames.
- **Borderless window** (`r_borderlessWindow 1` as well): also direct. Another app drawing on another display can delay a frame by up to ~2 ms.
- **Exclusive fullscreen** (`r_fullscreen 1`, `r_noborder 0`, the default): at `r_mode`'s size, switching the display mode only when the size differs. Cmd+Tab works. With anything animating on another display, about a fifth of the frames at 480 a second are replaced before they're shown (+0.7 ms p50, +2.2 ms p95). Keep other displays still, turn them off, or use the Space.
- **Window** (`r_fullscreen 0`): macOS always composites it, which adds latency.

## Speed

Against upstream's GL1 renderer (built from this branch's last commit that had it), on the same Mac. The benchmark is a 2-minute demo of a free-for-all on Stalingrad (`dm/mohdm6`) with 8 bots, in a 1280×720 window (`port/bench/`). The engine never runs a frame shorter than 1 ms, so frames per second stop near 1000 in both. The fair figure is the CPU's work per frame:

| Per frame | GL1 | Metal |
|---|---|---|
| Work, mean | 0.89–0.95 ms | **0.205 ms** (4.5× less) |
| Work, median | ~0.85 ms | **0.19 ms** |
| Work, 99th percentile | ~2.3 ms | **0.48 ms** (4.8× less) |

Input to screen, from the moment the game reads the mouse to the display showing the frame, at 250 frames a second (median / 95th percentile). OpenGL on macOS can't report when a frame reaches the screen, so there is no GL1 figure. GL1's fullscreen isn't direct to the display either, and it waits for the GPU every frame.

| Mode | First full Metal build | Now |
|---|---|---|
| Window | 10.0 / 12.2 ms | **2.0 / 4.5 ms** |
| Exclusive fullscreen | not measurable (composited) | **1.9 / 3.9 ms** |
| Fullscreen Space | – | **1.8 / 2.0 ms** |

Counted from the mouse's own event, the input on screen is typically **4.6 ms** old at stock 250 frames a second in the Space (the median over time), and **3.55 ms** (p95 4.7) with `cl_movefps 100` and `com_maxfps 480`.

Where the time went, in short:

- **The renderer:** world geometry and static models stay in GPU memory, skeletal models are skinned on the GPU, pipeline states are cached on disk, and nothing in a frame waits for the GPU (screenshots are read back asynchronously).
- **No compile hitches in play:** a map's pipelines are compiled during its loading screen and cached; a warm cache loads them in ~1 ms.
- **Players outside the view** aren't animated or prepared: −10% work on the benchmark, −8 to −16% in busy scenes at 960 frames a second.
- **Foliage after walls:** trees and fences are drawn after the opaque surfaces, so their hidden parts cost nothing on the GPU: up to −27% GPU time and +26% frames a second uncapped on Snowy Park.
- **Cheaper bookkeeping:** the draw sort ~6× cheaper on Snowy Park (~40 → ~7 µs a frame). MOHAA's own dynamic lights (muzzle flashes, explosions) are 2–6× cheaper with identical output: 28 → 12 µs on a frame where you fire. Results that don't change between frames are kept, such as the blob shadows under soldiers: −20 to −65 µs a frame where soldiers stand on terrain.
- **Fewer spikes:** missing sound files (83 per benchmark run in the retail data) are looked up once instead of every time they play.
- **Frame pacing:** a µs frame limiter on a high-precision timer. Frames start within a few µs of their deadline and run on the performance cores, and a dedicated server's frames are exact (upstream's are 0–12 ms late).
- **Presenting:** drawables are acquired off the main thread, a frame that finds none is presented late instead of dropped, and the GPU is woken as the frame starts (~0.35 ms sooner on screen at 250 frames a second).
- **The mouse:** returning to the game from the menu in a window took 1.7–2.6 ms of one frame; now ~0.04 ms.

Ideas from JayRewd's work and from the author's own Vulkan port were measured and re-implemented here, credited in the commits. No code was copied.

## How it looks

On fixed test views across multiplayer, objective and single-player maps, Metal matches GL1 except for single edge and tie pixels (a fraction of a percent, nothing you can see), and its look was reviewed in play. The differences on purpose:

- **Alpha-tested surfaces after the opaque ones** (the foliage item above): a few single pixels differ where a leaf and a wall are at exactly the same depth.
- **32 dynamic lights:** with 32 or more lights in a scene, MOHAA lit no wall at all on arm64; now they all light it. Only cheats get that many lights (`cg_acidtrip`).
- **Models at full detail** (`r_uselod 0` by default): soldiers and weapons don't lose detail with distance.
- **Gamma** (`r_gamma`) is applied in the final shader pass, not through the display's gamma table. It never changes the desktop's colors, and other tools can't override it.

Tried and not taken: per-pixel dynamic lights (as some Vulkan ports do). They save CPU, but on this GPU they cost more when a light covers the screen, which your own muzzle flash does, so your own shots would show ~50 µs later. A depth prepass for foliage gave no steady gain. More drawables or frames in flight add latency.

## Not done, parked and known issues

- **Parked:** a still black window over the other displays in exclusive fullscreen, so that mode would match the Space's latency.
- **Not done:** low-latency vsync (frame pacing for a tear-free mode without vsync's extra refresh), a frame-cap suggestion from frame-time percentiles, measurements of macOS Game Mode on and off and of audio latency, a Developer ID signature and notarization (the release zip is signed ad hoc, hence the quarantine step above).
- **Removed on purpose:** gamepads and joysticks, the `openmohaa://` link handler, the update checker.
- **Upstream behavior, kept:** doors, lifts and other moving brushwork are lit only by the scene's first dynamic light. Custom maps with terrain of 3 or more lightmap texels per step hang when a dynamic light reaches it (no retail map has such terrain).
- **Optional:** smaller CPU savings that were measured but not worth taking yet (2D text batching, the terrain's own per-vertex work). They're listed in [`docs/METAL_PORT_PLAN.md`](docs/METAL_PORT_PLAN.md).

The full record (every decision, measurement and session) is [`docs/METAL_PORT_PLAN.md`](docs/METAL_PORT_PLAN.md). This work is on `main` (named `metal` until 2026-10-20); upstream's own history is at [openmoh/openmohaa](https://github.com/openmoh/openmohaa).

---

> [!NOTE]
> **Upstream's README follows, unchanged.** In this fork, its Windows and Linux content, the release downloads, the `launch_openmohaa_*` launchers and the [building guide](docs/markdown/04-coding/01-compiling.md) don't apply (see [Building](#building) above). SDL and cURL are no longer used.

# OpenMoHAA

[![Build](https://github.com/openmoh/openmohaa/actions/workflows/branches-build.yml/badge.svg?branch=main)](https://github.com/openmoh/openmohaa/actions/workflows/branches-build.yml) [![Release](https://img.shields.io/github/v/release/openmoh/openmohaa)](https://github.com/openmoh/openmohaa/releases) [![Downloads](https://img.shields.io/github/downloads/openmoh/openmohaa/total)](https://github.com/openmoh/openmohaa/releases)

![License](https://img.shields.io/github/license/openmoh/openmohaa) ![Commits](https://img.shields.io/github/commit-activity/t/openmoh/openmohaa)

![Discord](https://img.shields.io/discord/596049712579215361?logo=discord&logoColor=white&color=5865F2)

![logo](misc/openmohaa-text-sm.png)

## What is OpenMoHAA?

OpenMoHAA is an open-source project aimed at preserving and enhancing **Medal of Honor: Allied Assault** (including Spearhead and Breakthrough expansions) by providing more features and bugfixes, across modern platforms and architectures.

Powered by [ioquake3](https://github.com/ioquake/ioq3) and the [F.A.K.K SDK](https://code.idtech.space/ritual/fakk2-sdk), OpenMoHAA provides:
- Full compatibility with the original game: assets, scripts and multiplayer
- Better support for modern systems
- Cross-platform support (Linux, Windows, macOS)
- Support for both single-player and multiplayer modes
- Includes all fixes from Spearhead 2.15 and Breakthrough 2.40b
- More fixes and features, such as bots and a ban system

*OpenMoHAA is an independent project and is not affiliated with or endorsed by Electronic Arts.*

## Getting started

- 📦 [Installing OpenMoHAA](docs/markdown/01-intro/01-installation.md)
- ▶️ [How to play: Launching the game, expansions & file locations](docs/markdown/02-running/01-running.md)
- ❓ [FAQ & Troubleshooting](docs/markdown/02-running/03-faq.md)
- 🌐 [Setting up a game server](docs/markdown/02-running/02-running-server.md)

## Reporting Issues

> [!NOTE]
> OpenMoHAA hasn't hit version 1.0.0 yet. Think of it like a beta build from the golden age of LAN parties. Features are being added, bugs are getting squashed, and more things are being tweaked. Things might change, break, or get even better over time.
> 
> If that sounds like your kind of mission, gear up, frag some bots, and help level up OpenMoHAA!

If you encounter a bug or a problem, you can do one of the following:
- Submit an [issue](https://github.com/openmoh/openmohaa/issues) on GitHub (use the template).
- Join the [OpenMoHAA Discord](https://discord.gg/NYtH58R) for a quick help.

## Additional documentation

- 📖 [Documentation](https://openmoh.github.io/openmohaa)
- ⚙️ [Game settings & configuration](docs/markdown/03-configuration/01-configuration.md)
- 📝 [Code & Scripting reference](docs/markdown/04-coding/02-coding.md)
- 📜 [Contributing guidelines](CONTRIBUTING.md)

## Current state

- 🧰 [List of differences](docs/markdown/01-intro/04-differences.md)

### Single-player

The entire single-player campaign should work (Allied Assault, Spearhead and Breakthrough). If you encounter any bug, please create a new [GitHub issue](https://github.com/openmoh/openmohaa/issues) describing them.

### Multiplayer

- Almost fully stable
- All official game modes are supported, including those from Spearhead and Breakthrough:
  - Free-For-All
  - Team-Deathmatch
  - Round-based match
  - Objective match
  - Tug-of-War (Spearhead)
  - Liberation (Breakthrough)
- Popular mods like **Freeze-Tag** are supported
- Built-in bots for offline practice and for testing
  - 🔧 [Setting up bots](docs/markdown/02-running/01-running.md#Playing-with-bots)

You can host your own [OpenMoHAA server](docs/markdown/02-running/02-running-server.md#) or join others using OpenMoHAA.

## Screenshots

|                                                                                   |                                                                            |
|-----------------------------------------------------------------------------------|----------------------------------------------------------------------------|
| ![](docs/assets/images/v0.60.0-x86_64/mohdm1_1.png)                                      | ![](docs/assets/images/v0.60.0-x86_64/training_1.png)                               |
| ![](docs/assets/images/v0.60.0-x86_64/flughafen_1.png)                                   | ![](docs/assets/images/v0.60.0-x86_64/flughafen_2.png)                            |
| ![](docs/assets/images/v0.60.0-x86_64/mohdm2_1.png "Playing Freeze-Tag mode with bots")  | ![](docs/assets/images/v0.60.0-x86_64/training_3.png "Single-Player training")    |

*More screenshots [here](docs/assets/images)*

## Development & Compiling

- 💻 [Building from source](docs/markdown/04-coding/01-compiling.md)

## Third party librairies

The following third party tools and libraries are used by the project

- [Flex](https://github.com/westes/flex)
- [Bison](https://savannah.gnu.org/projects/bison/)
- [SDL](http://www.libsdl.org/)
- [OpenAL](https://www.openal.org/)
- [LibMAD](http://www.underbit.com/products/mad/)
- [cURL](https://curl.se/)
- [Libogg](https://github.com/gcp/libogg)
- [Libvorbis](https://xiph.org/vorbis/)
- [Libopus](https://opus-codec.org/)

## Resources

- 🔗 [GitHub Repository](https://github.com/openmoh/openmohaa/)
- 🌐 [MOH-DB](https://www.moh-db.com/)
- 🕹️ [333networks](https://333networks.com/)
- 📂 [ModDB](https://www.moddb.com/games/medal-of-honor-allied-assault)
- 📂 [GameBanana](https://gamebanana.com/games/720)
- 💬 [Join us on Discord](https://discord.gg/NYtH58R)
