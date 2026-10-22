# OpenMoHAA for Apple Silicon 1.1.0: a native Metal port

> Draft for the GitHub release v1.1.0, not published: the release's text will be this file without its title and this note, with the zip's SHA-256 filled in below. It waits for the user's online test and OK.

The second release of [pstngh/openmohaa-metal](https://github.com/pstngh/openmohaa-metal), a fork of [OpenMoHAA](https://github.com/openmoh/openmohaa) that runs Medal of Honor: Allied Assault, Spearhead and Breakthrough natively on Apple Silicon Macs. It is not an upstream release: problems with this port belong on this repository's tracker, not upstream's.

**You need** an Apple Silicon Mac (M1 or later) with macOS 26 or later, and your own retail game data (`main/` with `Pak0.pk3` to `Pak6.pk3`, plus `mainta/` and `maintt/` for the expansions). The game data is not included.

## Updating from 1.0.0

- **The app is all you need now:** `game.dylib` and `cgame.dylib` are inside `openmohaa.app`. Replace the app in your game folder and delete the `game.dylib`, `cgame.dylib` and `omohaaded` that 1.0.0 put beside it (they aren't used any more; the zip has no dedicated server).
- **`cl_cmdRate` is now `cl_movefps`.** A config line `seta cl_cmdRate …` no longer does anything: change it to `seta cl_movefps …` (85–125; the default is 100), or the game plays at 100.
- **`cl_cmdDisplay` is gone:** there is one way of showing your movement between commands now (below), so a config line for it does nothing.
- Clear the new download's quarantine as for 1.0.0 (step 2 below). macOS also asks again for access to the game folder if it is on an external drive or in Documents or Desktop: each build needs its own permission.

## Install

1. Download `openmohaa-metal-1.1.0-macos-arm64.zip` and unzip it.
2. **Clear the download's quarantine, once.** This build is signed ad hoc, not notarized by Apple, so macOS refuses to run the downloaded files until the quarantine is cleared. In Terminal, type `xattr -dr com.apple.quarantine ` (with the trailing space), drag the unzipped folder onto the window, and press Return:
   ```bash
   xattr -dr com.apple.quarantine ~/Downloads/openmohaa-metal-1.1.0-macos-arm64
   ```
   "Open Anyway" in System Settings → Privacy & Security is not enough for this game: macOS then runs the app from a temporary read-only copy where it can't find your game data ("Couldn't load default.cfg").
3. Copy `openmohaa.app` into your MOHAA folder, next to `main/`.
4. Start `openmohaa.app`. If the game folder is on an external drive (or in Documents or Desktop), macOS asks whether it may access files there: allow it (it asks again after each update, as does step 2). Spearhead and Breakthrough: `open openmohaa.app --args +set com_target_game 1` (or `2`) in Terminal, in the game folder.

`INSTALL.txt` in the zip has the same steps. Configs, saves, screenshots, demos and logs go into the game folder (`main/configs/`, `main/screenshots/` …), as in the original game, or to `~/Library/Application Support/openmohaa/` if the game folder isn't writable.

## What's new

- **Smooth at any frame rate.** Above a few hundred frames a second, and at caps like 240 that don't divide 1000, the picture used to move in uneven steps (from shown frame to shown frame, 0.55 to 1.45 times the expected distance), because the game's time only counted whole milliseconds. Now other players and moving things are drawn at each frame's own moment, and your own movement between commands is shown as the next command's whole step, as it will go out with the keys you hold now, by the part of it that has passed (JayRewd's idea, re-implemented): uncapped, steps of about 0.94 to 1.13 times the expected, and much less stutter on small bumps in the terrain.
- **Keys on screen sooner** with `cl_movefps` on: a movement key moves the picture on the next frame that reaches the screen, about 2 ms after the press uncapped (2.9 ms in 1.0.0) and at 240 frames a second (6 ms in 1.0.0).
- **Every movement step the same length.** Each command the server runs is exactly 1000 / `cl_movefps` ms of game time (8 ms at 125, 10 at 100), at any frame rate, even at 60 or through a hitch, as in the author's Vulkan port. Before, the game's small clock corrections made some steps a millisecond or two longer or shorter, as with stock clients. Commands still go out in packets of their own (at `cl_maxpackets 125`) the moment they're due.
- **`cl_movefps`** (was `cl_cmdRate`): the name says what it sets, movement steps a second, apart from the frame rate. Its range is 85–125 now, the most `cl_maxpackets` sends, so each command gets a packet of its own (shorter steps catch on small bumps more often). `0` is still stock: one command per rendered frame.
- **New defaults:** soldiers and weapons at full detail at any distance (`r_uselod 0`; it isn't saved in configs), the console key works (`ui_console 1`; the game still starts at the main menu with the console closed), and `cl_maxpackets 125` (upstream's is 30), so every command goes out in its own packet.
- **One app:** the game libraries are inside `openmohaa.app` and sealed with it. No dedicated server in the zip (build it with `-DBUILD_SERVER=ON` for local tests; for servers, upstream's builds are the tested ones).
- **Fixed:** the Metal pipeline cache now works when your home folder's name has non-ASCII characters.

The settings that gave the shortest input-to-screen time on the test Mac (240 Hz):

```
seta cl_movefps 100
seta com_maxfps 480
seta r_fullscreen 1
seta r_noborder 1
seta r_swapInterval 0
seta in_mouseRaw 1
```

The README's [Settings that matter here](https://github.com/pstngh/openmohaa-metal#settings-that-matter-here) explains each one and the display modes.

## Online

The network protocol, the server-side game code and the data formats are upstream's, so this client plays on existing MOHAA and OpenMoHAA servers. What a server receives with `cl_movefps` on is one command per step, each exactly one step long, the steps a stock client at that rate sends on average; a local server that logged every command ran every one, at every frame rate, with a simulated ping, jitter and bots. The game reports upstream's version (0.83.0, with this build's commit) and sends no `cl_guid`, as the original game's clients don't.

## Known issues and limits

- **Tested on one Mac only:** an M4 Mac mini (10-core GPU) on macOS 26.6 with a 240 Hz and a 60 Hz 4K display. Reports from M1/M2/M3 Macs, laptops and ProMotion displays are welcome.
- **Not notarized:** signed ad hoc, hence the quarantine step.
- **Exclusive fullscreen with other displays:** when another app animates on another display, about a fifth of the frames at 480 a second are replaced before they're shown. Keep other displays still, or use the fullscreen Space (`r_noborder 1`).
- **At low frame caps** (about 60 or less) with `cl_movefps` on, the game keeps a CPU core busy between frames to send each command on time.
- **Not done:** low-latency vsync, Game Mode and audio latency measurements.
- **Removed on purpose:** gamepads and joysticks, the `openmohaa://` link handler, the update checker. Windows, Linux and Intel Macs aren't supported by this fork.
- **Upstream behavior kept:** moving brushwork (doors, lifts) is lit only by a scene's first dynamic light.

## Source and licenses

Built from the `main` branch at the tagged commit, with OpenMoHAA 0.83.0 (upstream `a2f34019`, 2026-04-23, still upstream's latest) as its base. The full record of the port (every decision and measurement) is [`docs/METAL_PORT_PLAN.md`](https://github.com/pstngh/openmohaa-metal/blob/main/docs/METAL_PORT_PLAN.md). OpenMoHAA is GPL-2.0 (`COPYING.txt` in the zip); the bundled libraries' licenses are in the zip's `licenses/` and in the app (openal-soft). This software is based in part on the work of the Independent JPEG Group.

Thanks to the OpenMoHAA team for the engine this is built on, and to JayRewd, whose ideas were measured and re-implemented here.

| File | SHA-256 |
|---|---|
| `openmohaa-metal-1.1.0-macos-arm64.zip` | (filled in when the zip is final) |
