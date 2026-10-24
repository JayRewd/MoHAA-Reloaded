# MoHAA Reloaded

<p align="center">
  <img src="icons/mohreloaded-banner.png" alt="MoHAA Reloaded" width="800">
</p>

<p align="center">
  <img src="icons/mohaa.svg" width="96" alt="Medal of Honor: Allied Assault">&nbsp;&nbsp;&nbsp;
  <img src="icons/mohaa-spearhead.svg" width="96" alt="Spearhead">&nbsp;&nbsp;&nbsp;
  <img src="icons/mohaa-breakthrough.svg" width="96" alt="Breakthrough">
</p>
<p align="center"><b>Allied Assault</b> · <b>Spearhead</b> · <b>Breakthrough</b></p>

**MoHAA Reloaded** brings [OpenMoHAA](https://github.com/openmoh/openmohaa) natively to modern hardware, with renderers of its own instead of OpenGL. It plays Medal of Honor: Allied Assault, Spearhead and Breakthrough with your retail game data, on existing MOHAA and OpenMoHAA servers.

There are two ports, each on its own branch. This branch, `main`, holds only this page and its icons.

| Branch | For | Renderer | Status |
|---|---|---|---|
| [**`metal`**](https://github.com/pstngh/MoHAA-Reloaded/tree/metal) | macOS on Apple Silicon (M1 or later, macOS 26 or later) | Metal | [Released](https://github.com/pstngh/MoHAA-Reloaded/releases) |
| [**`vulkan`**](https://github.com/pstngh/MoHAA-Reloaded/tree/vulkan) | 64-bit Linux and Windows | Vulkan | [Released](https://github.com/pstngh/MoHAA-Reloaded/releases) |

Each branch's README has its install steps, settings and limits: [`metal`](https://github.com/pstngh/MoHAA-Reloaded/blob/metal/README.md), [`vulkan`](https://github.com/pstngh/MoHAA-Reloaded/blob/vulkan/README.md).

> [!IMPORTANT]
> **This is a fork, not the upstream project.** Problems with these ports belong on [this repository's tracker](https://github.com/pstngh/MoHAA-Reloaded/issues), with the branch you play (`metal` or `vulkan`), not upstream's tracker or Discord.

## Which one

- **A Mac with Apple Silicon:** `metal`. Download it from [Releases](https://github.com/pstngh/MoHAA-Reloaded/releases).
- **A Linux PC:** `vulkan`. Download `openmohaa-vulkan-1.0.0-linux-amd64.tar.gz` from [Releases](https://github.com/pstngh/MoHAA-Reloaded/releases). Any GPU with a Vulkan 1.1 driver; developed on AMD with Mesa's RADV driver, also played on NVIDIA.
- **Windows:** `vulkan`. Download `openmohaa-vulkan-1.0.0-windows-x64.zip` from [Releases](https://github.com/pstngh/MoHAA-Reloaded/releases). It is built from the same code as the Linux build, but nobody has played it on Windows yet: reports are welcome.
- **Intel Macs:** neither for now (`metal` needs Apple Silicon; `vulkan` isn't built or tested on macOS).

## What the two share

Both ports are built for low input latency and even frame times, measured at every step:

- **The same game online.** The network protocol, the server-side game code and the data formats are upstream's: both play on stock MOHAA and OpenMoHAA servers.
- **Movement apart from the frame rate.** The commands the server runs come from their own clock, 85 to 125 a second, and every one is the same length at any frame rate (`cl_movefps`); a shot or a jump gets a command of its own at the half step (`cl_movesubgrid`).
- **Exact frame caps.** `com_maxfps 240` runs at 240 frames a second, not 250.
- **Their own sound mixers, no OpenAL.** `metal` mixes on Core Audio, with Apple's HRTF for headphones; `vulkan` has the Omaha mixer (miniaudio, with HRTF for headphones).

They differ in much else (the window and input layers, the defaults, the sound mixers): see each README.

## Settings guide

Both ports keep upstream's settings and add a few of their own. Type them in the console (the `~` key, under Esc): `seta name value` changes a setting and saves it in your config, `set name value` only until you quit. At launch, `+set name value` does the same: on macOS `open openmohaa.app --args +set name value`, on Linux `./openmohaa +set name value` (or after a launcher's name), on Windows at the end of a shortcut's target. Display settings take effect after `vid_restart`, sound settings after `snd_restart`.

Your config keeps what it holds: one saved by an earlier build (of these ports or upstream's) may still say `com_maxfps 85`, `cl_maxpackets 30` or `ui_console 0`. Set the new values yourself (on `vulkan`, `cl_cmdfps` of earlier builds is `cl_movefps` now). Configs live in the game folder's `main/configs/` on `metal` (or `~/Library/Application Support/openmohaa/` if the folder isn't writable), in `~/.config/openmohaa/` on Linux and `%APPDATA%\openmohaa\` on Windows.

### For the lowest input latency

`metal`, measured on an M4 Mac mini with a 240 Hz display:

```
seta cl_movefps 100
seta com_maxfps 480
seta r_fullscreen 1
seta r_noborder 1
seta in_mouseRaw 1
```

`vulkan`, as played on a 240 Hz display:

```
seta cl_movefps 100
seta com_maxfps 250
seta r_fullscreen 1
```

### Settings both ports have

| Setting | Default (upstream's) | What it does |
|---|---|---|
| `cl_movefps` | `100` (new) | **Movement apart from the frame rate.** The commands the server runs come from their own clock, 85 to 125 a second, each exactly 1000 / this many ms of movement (10 at 100, 8 at 125) at any frame rate; the view turns with every frame. `0` is stock: one command per rendered frame, and at most 1000 frames a second. |
| `cl_movesubgrid` | `1` (new) | With `cl_movefps` on, a fire click or a jump in the first half of a step gets a command of its own at the half step: shots and jumps reach the server sooner. `0` keeps every command on the grid. |
| `com_maxfps` | `125` (`85`) | The frame cap, exact (240 is 240, not 250); `0` is uncapped. With the movement clock on, movement doesn't depend on it. |
| `cl_maxpackets` | `125` (`30`) | Every command goes out in its own packet. |
| `r_fullscreen`, `r_noborder` | `1`, `0` | Fullscreen at `r_mode`'s size. On `metal`, `1` and `1` is the fullscreen Space (the cleanest mode); on `vulkan`, `r_fullscreen 0` with `r_noborder 1` is a borderless window. `0`, `0` is a window (more latency on both). |
| `r_uselod` | `0` (`1`) | `0`: soldiers and weapons at full detail at any distance. Not saved in configs: set it at launch or in `autoexec.cfg`. |
| `s_speaker_type` | `0` | `1` is **headphone 3D**: sounds behind, above or below you sound there (`metal`: Apple's HRTF, your personalized one when macOS has it; `vulkan`: the SADIE KU100 set, mixed at 48 kHz; set it at launch). `0` is stereo. |
| `s_ambientvolume` | `1` | `0` turns the soundtrack off entirely: no song is opened or played. |
| `cg_tauntsounds` | `1` (new) | `0` turns other players' taunts off: they aren't played at all (the chat text stays); your own still play. |
| `cg_crosshair_length`, `cg_crosshair_gap`, `cg_crosshair_thickness`, `cg_crosshair_color` | `4`, `3`, `1`, `FFFFFF` (new) | The crosshair, drawn as boxes in HUD pixels: by default the stock image's shape, in one draw. |
| `ui_console` | `1` (`0`) | The console key works; the game still starts at the main menu. |
| `cl_playintro` | `0` (`1`) | `1` plays the intro videos at startup. |

### Only on `metal`

| Setting | Default | What it does |
|---|---|---|
| `in_mouseRaw` | `1` | Raw mouse deltas through GCMouse, no macOS acceleration; `0` uses AppKit's mouse events. |
| `r_borderlessWindow` | `0` | With `r_fullscreen 1` and `r_noborder 1`: a borderless window over the screen instead of a fullscreen Space. |
| `com_busyWait` | `0` | `1` spins the CPU through the frame wait: about 0.05 to 0.1 ms sooner, for a full core. |

### Only on `vulkan`

| Setting | Default | What it does |
|---|---|---|
| `r_vkSubmitThread` | `0` | `1`: a thread of its own submits and presents the frames, for drivers that stall there (5 swapchain images, 3 with vsync). |
| `r_maxDlights` | `4` | Dynamic lights a scene keeps, the nearest the view, 0 to 64. |
| `r_ext_multisample` | `0` | `2` to `16`: multisampling. |

### Commands

| Command | What it does |
|---|---|
| `vid_restart`, `snd_restart` | Restart the renderer or the sound with the settings you changed. |
| `connect <address>`, `reconnect`, `disconnect` | Join a server, join the last one again, leave. |
| `record <name>`, `stoprecord`, `demo <name>` | Record a demo, stop, play one back (`main/demos/`). |
| `screenshot`, `screenshotJPEG` (`vulkan` also `screenshotPNG`) | Save a screenshot to `main/screenshots/`. |
| `bind <key> <command>`, `unbind <key>`, `bindlist` | Key bindings. |
| `altbind`, `ctrlbind`, `altbindlist`, `ctrlbindlist` | Bindings for Alt and Ctrl combinations. |
| `exec <file>`, `writeconfig <file>` | Run a config file, save your settings to one. |
| `cvarlist [text]`, `cmdlist` | List the settings (those that start with `text`) and commands. |

## Getting the code

```bash
git clone -b metal https://github.com/pstngh/MoHAA-Reloaded.git mohaa-reloaded-metal
```

```bash
git clone -b vulkan https://github.com/pstngh/MoHAA-Reloaded.git mohaa-reloaded-vulkan
```

A plain clone gives only `main`: this page.

## Contributing

Pull requests go to `metal` or `vulkan`, not to `main`. Say which branch an issue is about.

## Credits and license

- The [OpenMoHAA](https://github.com/openmoh/openmohaa) team, for the engine (built on [ioquake3](https://github.com/ioquake/ioq3) and the F.A.K.K SDK).
- JayRewd, for the Omaha sound mixer and renderer work merged into `vulkan`, and the ideas measured and re-implemented in both ports; notuern, for testing on NVIDIA and the sub-grid of `metal`'s movement.
- OpenMoHAA is GPL-2.0 ([`COPYING.txt`](COPYING.txt)), and so are these ports. They are independent projects, not affiliated with or endorsed by Electronic Arts.
