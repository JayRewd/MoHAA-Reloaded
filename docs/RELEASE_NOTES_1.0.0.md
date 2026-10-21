# OpenMoHAA for Apple Silicon 1.0.0: a native Metal port

> Published on 2026-10-20 as [v1.0.0](https://github.com/pstngh/openmohaa-metal/releases/tag/v1.0.0); the release's text is this file without its title and this note.

The first release of [pstngh/openmohaa-metal](https://github.com/pstngh/openmohaa-metal), a fork of [OpenMoHAA](https://github.com/openmoh/openmohaa) that runs Medal of Honor: Allied Assault, Spearhead and Breakthrough natively on Apple Silicon Macs. It is not an upstream release: problems with this port belong on this repository's tracker, not upstream's.

**You need** an Apple Silicon Mac (M1 or later) with macOS 26 or later, and your own retail game data (`main/` with `Pak0.pk3` to `Pak6.pk3`, plus `mainta/` and `maintt/` for the expansions). The game data is not included.

## Install

1. Download `openmohaa-metal-1.0.0-macos-arm64.zip` and unzip it.
2. **Clear the download's quarantine, once.** This build is signed ad hoc, not notarized by Apple, so macOS refuses to run the downloaded files until the quarantine is cleared. In Terminal, type `xattr -dr com.apple.quarantine ` (with the trailing space), drag the unzipped folder onto the window, and press Return:
   ```bash
   xattr -dr com.apple.quarantine ~/Downloads/openmohaa-metal-1.0.0-macos-arm64
   ```
   "Open Anyway" in System Settings → Privacy & Security is not enough for this game: macOS then runs the app from a temporary read-only copy where it can't find your game data, and it still refuses to load `game.dylib` and `cgame.dylib`.
3. Copy `openmohaa.app`, `game.dylib` and `cgame.dylib` into your MOHAA folder, next to `main/` (`omohaaded`, a dedicated server, is optional).
4. Start `openmohaa.app`. If the game folder is on an external drive (or in Documents or Desktop), macOS asks whether it may access files there: allow it (it asks again after each update, as does step 2). Spearhead and Breakthrough: `open openmohaa.app --args +set com_target_game 1` (or `2`) in Terminal, in the game folder.

`INSTALL.txt` in the zip has the same steps. Configs, saves, screenshots, demos and logs go into the game folder (`main/configs/`, `main/screenshots/` …), as in the original game, or to `~/Library/Application Support/openmohaa/` if the game folder isn't writable. Settings an earlier OpenMoHAA kept in Application Support are read on the first start. If your game folder already has a `main/configs/omconfig.cfg` from another OpenMoHAA build, that one is used: delete it to keep the settings from Application Support.

## What's in it

- **Metal from start to finish.** The renderer is upstream's GL1 renderer with its drawing rewritten for Metal: hand-written Metal shaders compiled into the binary, a pipeline cache with each map's pipelines compiled during its loading screen, world and static-model geometry kept in GPU memory, skeletal models skinned on the GPU, and no GPU wait inside a frame. OpenGL is gone from the source and the binary. No translation layers.
- **No SDL.** The window, display modes, keyboard and mouse are native AppKit code, with raw mouse input through GCMouse (`in_mouseRaw 1`, the default).
- **Like the original, where it counts:** files go into the game folder, and the intro videos are off (`+set cl_playintro 1` brings them back). No PID file, so no "Abnormal Exit" prompt after a crash or a force-quit.
- **Lean.** arm64 and macOS 26 only: one sealed `openmohaa.app` with openal-soft 1.25.2 inside, plus `game.dylib`, `cgame.dylib` and `omohaaded`.
- **Faster than GL1** on the test Mac (the benchmark demo: a 2-minute 8-bot free-for-all on Stalingrad): the CPU's work per frame 0.89–0.95 → **0.205 ms** (4.5× less), the 99th percentile ~2.3 → **0.48 ms** (4.8× less).
- **Lower input latency.** From reading the mouse to the display showing the frame, at 250 frames a second: **2.0 ms** median in a window, **1.9** in exclusive fullscreen, **1.8** in the fullscreen Space (the first Metal build: 10 ms in a window). Counted from the mouse's own event, the input on screen is typically 3.55 ms old in the Space with `cl_cmdRate 100` and `com_maxfps 480`, against 4.6 ms in stock mode at 250.
- **A decoupled command clock** (`cl_cmdRate`, default 100): the commands the server runs come from their own clock, and the game renders at whatever `com_maxfps` sets. What the server receives is what a stock client sends at that command rate. `cl_cmdRate 0` is stock behavior.

The settings that gave the shortest input-to-screen time on the test Mac (240 Hz):

```
seta cl_cmdRate 100
seta com_maxfps 480
seta r_fullscreen 1
seta r_noborder 1
seta r_swapInterval 0
seta in_mouseRaw 1
```

The README's [Settings that matter here](https://github.com/pstngh/openmohaa-metal#settings-that-matter-here) explains each one and the display modes.

## Online

The network protocol, the server-side game code and the data formats are upstream's, so this client plays on existing MOHAA and OpenMoHAA servers. The game reports upstream's version (0.83.0, with this build's commit). It sends no `cl_guid` (it keeps no `qkey` file), as the original game's clients don't: servers that track players by GUID see you without one. It was played on public servers during development, in both command modes. `omohaaded` is built for local tests; for servers, upstream's builds are the tested ones.

## Looks

On fixed test views across multiplayer, objective and single-player maps, it matches GL1 except for single edge and tie pixels. On purpose: foliage and fences are drawn after the opaque surfaces (a few tie pixels differ), with 32 dynamic lights in a scene the world is lit (MOHAA lit no wall at all on arm64), and `r_gamma` is applied in the final shader pass instead of the display's gamma table.

## Known issues and limits

- **Tested on one Mac only:** an M4 Mac mini (10-core GPU) on macOS 26.6 with a 240 Hz and a 60 Hz 4K display. Reports from M1/M2/M3 Macs, laptops and ProMotion displays are welcome.
- **Not notarized:** signed ad hoc, hence the quarantine step.
- **Exclusive fullscreen with other displays:** when another app animates on another display, about a fifth of the frames at 480 a second are replaced before they're shown. Keep other displays still, or use the fullscreen Space (`r_noborder 1`).
- **Not done:** low-latency vsync, Game Mode and audio latency measurements.
- **Removed on purpose:** gamepads and joysticks, the `openmohaa://` link handler, the update checker. Windows, Linux and Intel Macs aren't supported by this fork.
- **Upstream behavior kept:** moving brushwork (doors, lifts) is lit only by a scene's first dynamic light.

## Source and licenses

Built from the `main` branch at the tagged commit, with OpenMoHAA 0.83.0 (upstream `a2f34019`, 2026-04-23, upstream's latest) as its base. The full record of the port (every decision and measurement) is [`docs/METAL_PORT_PLAN.md`](https://github.com/pstngh/openmohaa-metal/blob/main/docs/METAL_PORT_PLAN.md). OpenMoHAA is GPL-2.0 (`COPYING.txt` in the zip); the bundled libraries' licenses are in the zip's `licenses/` and in the app (openal-soft). This software is based in part on the work of the Independent JPEG Group.

Thanks to the OpenMoHAA team for the engine this is built on, and to JayRewd, whose ideas were measured and re-implemented here.

| File | SHA-256 |
|---|---|
| `openmohaa-metal-1.0.0-macos-arm64.zip` | `8624304f5bfc88a02ae9cab583090d3dd9429d151bd0c1734267e716e9130da5` |
