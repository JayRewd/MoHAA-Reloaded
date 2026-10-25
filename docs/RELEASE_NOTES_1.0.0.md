# MoHAA Reloaded 1.0.0: native Metal for Apple Silicon

> Published on 2026-10-23 as [v1.0.0](https://github.com/pstngh/MoHAA-Reloaded/releases/tag/v1.0.0), from the commit this note is in (first from 08f19e40, then bdce5ca9 the same day for the icon fix, the name MoHAA Reloaded and `com_maxfps 125`, then fb6008ea for `m_filter 0`, `cl_ctrlbindings`, the config after a game-folder change, the HUD's per-frame work and the chat clear of the compass, then the tag moved and the zip replaced again the same day, to f1541d96, for the crosshair drawn as boxes and 14 old GL settings no longer saved, then on 2026-10-25 to this commit for headphone 3D's timbre), replacing the first v1.0.0 of 2026-10-20 (ab81ff5b; its text is this file at 7692472f); the release's text is this file without its title and this note.

The first release of the Metal port of [MoHAA Reloaded](https://github.com/pstngh/MoHAA-Reloaded) (its `metal` branch), a fork of [OpenMoHAA](https://github.com/openmoh/openmohaa) that runs Medal of Honor: Allied Assault, Spearhead and Breakthrough natively on Apple Silicon Macs. It is not an upstream release: problems with this port belong on this repository's tracker, not upstream's.

**You need** an Apple Silicon Mac (M1 or later) with macOS 26 or later, and your own retail game data (`main/` with `Pak0.pk3` to `Pak6.pk3`, plus `mainta/` and `maintt/` for the expansions). The game data is not included.

## Install

1. Download `openmohaa-metal-1.0.0-macos-arm64.zip` and unzip it.
2. **Clear the download's quarantine, once.** This build is signed ad hoc, not notarized by Apple, so macOS refuses to run the downloaded files until the quarantine is cleared. In Terminal, type `xattr -dr com.apple.quarantine ` (with the trailing space), drag the unzipped folder onto the window, and press Return:
   ```bash
   xattr -dr com.apple.quarantine ~/Downloads/openmohaa-metal-1.0.0-macos-arm64
   ```
   "Open Anyway" in System Settings → Privacy & Security is not enough for this game: macOS then runs the app from a temporary read-only copy where it can't find your game data. The game then stops and shows the line to run, with your folder's path.
3. Copy `openmohaa.app` into your MOHAA folder, next to `main/`. The game libraries are inside the app.
4. Start `openmohaa.app`. If the game folder is on an external drive (or in Documents or Desktop), macOS asks whether it may access files there: allow it (it asks again after each update, as does step 2). Spearhead and Breakthrough: `open openmohaa.app --args +set com_target_game 1` (or `2`) in Terminal, in the game folder.

`INSTALL.txt` in the zip has the same steps. Configs, saves, screenshots, demos and logs go into the game folder (`main/configs/`, `main/screenshots/` …), as in the original game, or to `~/Library/Application Support/openmohaa/` if the game folder isn't writable. Settings an earlier OpenMoHAA kept in Application Support are read on the first start. If your game folder already has a `main/configs/omconfig.cfg` from another OpenMoHAA build, that one is used: delete it to keep the settings from Application Support.

## Settings from an earlier build

Your config keeps your settings. A few work differently here:

- **Set `m_filter 0`.** Configs saved by OpenMoHAA on a Mac, or by an earlier 1.0.0 zip of this port, hold `seta m_filter "1"`, which averages each frame's mouse movement with the frame before's: half of every movement shows a frame later. The default is 0 now, but a saved 1 stays: change the line to `seta m_filter 0`.
- **Ctrl bindings need `cl_ctrlbindings 1`.** It is a setting of its own now: `cl_altbindings 1` turns on only the Alt and Ins bindings (`altbind`), where upstream's turned on the Ctrl and Del ones (`ctrlbind`) too. If you use Ctrl or Del bindings, add `seta cl_ctrlbindings 1`. `ctrlbindlist` lists them (it listed the Alt ones).
- **The crosshair** is set by `cg_crosshair_length`, `cg_crosshair_gap`, `cg_crosshair_thickness` and `cg_crosshair_color` (below); `cg_crosshair` (a custom crosshair image) and `cg_crosshair_friend` are gone.
- Lines for settings this build doesn't have any more (`cg_crosshair`, `cg_crosshair_friend`, old GL renderer settings such as `r_allowExtensions` or `r_stencilbits`) do nothing: you can delete them.

## If you downloaded 1.0.0 earlier

The zip was replaced three times on 2026-10-23 and once on 2026-10-25 (its SHA-256 was `9c2f8e97…`, then `fb61b8de…`, then `a0da44f0…`, then `83187026…`; this one's is at the bottom). If you have one of those, replace `openmohaa.app` with this zip's, clear the new download's quarantine (step 2), and allow the folder access again if macOS asks. New in this one: headphone 3D (`s_speaker_type 1`) no longer sounds tinny: sounds in front keep their body, and the sizzle Apple's HRTF adds at 11–14 kHz is evened out to the stereo mix's timbre. Since `a0da44f0…` and earlier also: the crosshair drawn as boxes (below), and 14 old GL settings no longer saved. Since `9c2f8e97…` and `fb61b8de…` also: `m_filter` 0 and `cl_ctrlbindings` (above), your config kept after a mod server (below), the HUD's per-frame work cut by more than half, and the chat and game messages kept clear of the compass.

## If you installed the first 1.0.0

This 1.0.0 replaces the one published on 2026-10-20 (the same version number, a later build of the port, with the new sound and everything below). If you have that one:

- **Replace the app**, and delete the `game.dylib`, `cgame.dylib` and `omohaaded` it put beside it: the game libraries are inside `openmohaa.app` now, and the zip has no dedicated server.
- **`cl_cmdRate` is now `cl_movefps`.** A config line `seta cl_cmdRate …` no longer does anything: change it to `seta cl_movefps …` (85–125; the default is 100), or the game plays at 100. `cl_cmdDisplay` is gone too.
- Clear the new download's quarantine (step 2), and allow the folder access again if macOS asks: each build needs its own permission.
- Fixed since: the Metal pipeline cache when your home folder's name has non-ASCII characters; a crash on custom maps whose terrain has a finer lightmap than the retail maps' when a dynamic light (a shot, an explosion) reaches the terrain; the stop after "Open Anyway" (it now says what to do); a stray footstep after getting off a ladder or letting go of a mounted gun with `cl_movefps` on; your settings and binds reset to the defaults, and your config overwritten with them, after a mod server or a `game_restart`.

## What's in it

- **Metal from start to finish.** The renderer is upstream's GL1 renderer with its drawing rewritten for Metal: hand-written Metal shaders compiled into the binary, a pipeline cache with each map's pipelines compiled during its loading screen, world and static-model geometry kept in GPU memory, skeletal models skinned on the GPU, and no GPU wait inside a frame. OpenGL is gone from the source and the binary. No translation layers.
- **No SDL.** The window, display modes, keyboard and mouse are native AppKit code, with raw mouse input through GCMouse (`in_mouseRaw 1`, the default).
- **No OpenAL: the original's sound.** The port's own mixer on Core Audio plays MOHAA's sound as the PC original did on the Miles Sound System: each sound at its own volume (upstream's OpenAL played every positional one-shot at one level), loud ones neither flattened nor clipped, sounds behind you 3 dB quieter and silent beyond their maximum distance. A sound reaches the headphone jack ~5 ms after it starts instead of ~19, and the mixing costs less CPU than OpenAL's. Music, dialogue, movies and saves play through it too: songs crossfade and resume where they stopped. JayRewd's review of where OpenMoHAA's sound differs from the original was the checklist (ideas only, re-implemented).
- **Sound settings:** `s_speaker_type 1` is headphone 3D through Apple's HRTF (your personalized one when macOS has it): a sound behind, above or below you sounds there. It's off by default (stereo, as the original pans) and switches at once. `s_ambientvolume 0` turns the soundtrack off entirely (no song is opened, decoded or mixed), and the new `cg_tauntsounds 0` mutes other players' taunts (the chat text stays).
- **Like the original, where it counts:** files go into the game folder, and the intro videos are off (`+set cl_playintro 1` brings them back). No PID file, so no "Abnormal Exit" prompt after a crash or a force-quit.
- **The crosshair, drawn as boxes in one draw:** by default the stock crosshair's shape (white arms one pixel thick, four long, a gap of three), centered to the pixel at any size. `cg_crosshair_length`, `cg_crosshair_gap` (from the center) and `cg_crosshair_thickness`, in HUD pixels (the screen's up to 1920×1080, scaled with the height above), and `cg_crosshair_color` (`RRGGBB`) change it, at a map load or `vid_restart`. There is no teammate crosshair any more (Spearhead and Breakthrough had a green one), and no per-frame check for it.
- **Your config survives a mod server.** Joining a server that runs its own game folder (`fs_game`), or a `game_restart`, keeps your settings and binds: upstream reset them to the defaults there and later saved those over your config. A mod's pk3 can't replace your config either. (If that happened to you before, the file holds the defaults now: set your settings again.)
- **Lean.** arm64 and macOS 26 only: one sealed `openmohaa.app` with the game libraries inside, and no OpenGL, SDL or OpenAL in it.
- **Faster than GL1** on the test Mac (the benchmark demo: a 2-minute 8-bot free-for-all on Stalingrad): the CPU's work per frame 0.89–0.95 → **0.205 ms** (4.5× less), the 99th percentile ~2.3 → **0.48 ms** (4.8× less). The HUD's own work each frame is less than half upstream's, for the same picture.
- **Lower input latency.** From reading the mouse to the display showing the frame, at 250 frames a second: **2.0 ms** median in a window, **1.9** in exclusive fullscreen, **1.8** in the fullscreen Space (the first Metal build: 10 ms in a window). Counted from the mouse's own event, the input on screen is typically 3.55 ms old in the Space with `cl_movefps 100` and `com_maxfps 480`, against 4.6 ms in stock mode at 250. macOS Game Mode turns on in fullscreen (the app declares itself a game); measured on against off, it costs nothing in play.
- **A decoupled command clock** (`cl_movefps`, default 100, 85–125): the commands the server runs come from their own clock, and the game renders at whatever `com_maxfps` sets. Each command is exactly 1000 / `cl_movefps` ms of game time (10 ms at 100, 8 at 125) at any frame rate, even at 60 or through a hitch, as in the author's Vulkan port, and goes out in a packet of its own (at `cl_maxpackets 125`) the moment it's due. `cl_movefps 0` is stock: one command per rendered frame.
- **Smooth at any frame rate.** Between commands your own movement is shown as the next command's whole step, as it will go out with the keys you hold now, by the part of it that has passed (JayRewd's idea, re-implemented), and other players and moving things are drawn at each frame's own moment, not at the whole millisecond. A movement key moves the picture on the next frame that reaches the screen, about 2 ms after the press uncapped, and shots are aimed where your crosshair was at the click.
- **Shots and jumps reach the server sooner** (`cl_movesubgrid`, on by default; notuern's idea): with `cl_movefps` on, a fire click or a jump in the first half of a step gets a command of its own at the half step instead of waiting for the next one, and the commands after it stay where they were (that one step split in two). At `cl_movefps 125` a shot reached a local server 1.7 ms after the click on average instead of 3.6, a jump 3.1 instead of 5.7. A run whose two short steps land exactly on a small bump's lip stops there more often; a shot or a jump at a random moment of a run stops it as often as without. `seta cl_movesubgrid 0` puts every command on the grid.
- **The port's defaults:** `com_maxfps 125` (upstream's is 85; with `cl_movefps 0` also the command rate, a stock 125 client's), `m_filter 0` (upstream's is 1 on a Mac: half of every mouse movement a frame later), soldiers and weapons at full detail at any distance (`r_uselod 0`; it isn't saved in configs), the console key works (`ui_console 1`; the game still starts at the main menu with the console closed), and `cl_maxpackets 125` (upstream's is 30), so every command goes out in its own packet.

The settings that gave the shortest input-to-screen time on the test Mac (240 Hz):

```
seta cl_movefps 100
seta com_maxfps 480
seta r_fullscreen 1
seta r_noborder 1
seta r_swapInterval 0
seta in_mouseRaw 1
seta m_filter 0
```

The README's [Settings that matter here](https://github.com/pstngh/MoHAA-Reloaded#settings-that-matter-here) explains each one and the display modes.

## Online

The network protocol, the server-side game code and the data formats are upstream's, so this client plays on existing MOHAA and OpenMoHAA servers. What a server receives with `cl_movefps` on is one command per step, each exactly one step long, the steps a stock client at that rate sends on average, and for a shot or a jump in the first half of a step that step split in two (4 + 4 ms at 125, the steps a stock client at 250 frames a second sends all the time); a local server that logged every command ran every one, at every frame rate, with a simulated ping, jitter and bots. The game reports upstream's version (0.83.0, with this build's commit). It sends no `cl_guid` (it keeps no `qkey` file), as the original game's clients don't: servers that track players by GUID see you without one. It was played on public servers during development, in both command modes and with the new sound. The zip has no dedicated server (`-DBUILD_SERVER=ON` builds one for local tests); for servers, upstream's builds are the tested ones.

## Looks

On fixed test views across multiplayer, objective and single-player maps, it matches GL1 except for single edge and tie pixels. On purpose: foliage and fences are drawn after the opaque surfaces (a few tie pixels differ); with 32 dynamic lights in a scene the world is lit (MOHAA lit no wall at all on arm64); doors, lifts and other moving brushwork are lit by every dynamic light that reaches them (MOHAA let only a scene's first one in); models keep full detail at any distance (`r_uselod 0`); the chat and game messages are placed by the compass's real size, clear of it at every resolution (upstream's chat started under the compass below 1280 pixels wide); the crosshair is the stock image's white arms without its dark pixel at each arm's inner end, crisp above 1080p where the image was scaled, with no teammate or custom crosshair images; and `r_gamma` is applied in the final shader pass instead of the display's gamma table.

## Known issues and limits

- **Tested on one Mac only:** an M4 Mac mini (10-core GPU) on macOS 26.6 with a 240 Hz and a 60 Hz 4K display. Reports from M1/M2/M3 Macs, laptops and ProMotion displays are welcome.
- **Not notarized:** signed ad hoc, hence the quarantine step.
- **Exclusive fullscreen with other displays:** when another app animates on another display, about a fifth of the frames at 480 a second are replaced before they're shown. Keep other displays still, or use the fullscreen Space (`r_noborder 1`).
- **At low frame caps** (about 60 or less) with `cl_movefps` on, the game keeps a CPU core busy between frames to send each command on time.
- **Not done:** a frame-cap suggestion from frame-time percentiles; sounds muffled through walls.
- **Removed on purpose:** gamepads and joysticks, the `openmohaa://` link handler, the update checker. Windows, Linux and Intel Macs aren't supported by this fork.

## Source and licenses

Built from the `metal` branch at the tagged commit, with OpenMoHAA 0.83.0 (upstream `a2f34019`, 2026-04-23, still upstream's latest) as its base. The full record of the port (every decision and measurement) is [`docs/METAL_PORT_PLAN.md`](https://github.com/pstngh/MoHAA-Reloaded/blob/metal/docs/METAL_PORT_PLAN.md). OpenMoHAA is GPL-2.0 (`COPYING.txt` in the zip); the bundled libraries' licenses are in the zip's `licenses/`. This software is based in part on the work of the Independent JPEG Group.

Thanks to the OpenMoHAA team for the engine this is built on, to JayRewd, whose ideas were measured and re-implemented here, and to notuern for the sub-grid.

| File | SHA-256 |
|---|---|
| `openmohaa-metal-1.0.0-macos-arm64.zip` | `281d9c575ebabe9d0c5a6881021f46086d67361b44e022ef297a15ccd8c4e777` |
