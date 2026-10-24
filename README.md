# MoHAA Reloaded

**MoHAA Reloaded** brings [OpenMoHAA](https://github.com/openmoh/openmohaa) natively to modern hardware, with renderers of its own instead of OpenGL. It plays Medal of Honor: Allied Assault, Spearhead and Breakthrough with your retail game data, on existing MOHAA and OpenMoHAA servers.

There are two ports, each on its own branch. This branch, `main`, holds only this page.

| Branch | For | Renderer | Status |
|---|---|---|---|
| [**`metal`**](https://github.com/pstngh/MoHAA-Reloaded/tree/metal) | macOS on Apple Silicon (M1 or later, macOS 26 or later) | Metal | [Released](https://github.com/pstngh/MoHAA-Reloaded/releases) |
| [**`vulkan`**](https://github.com/pstngh/MoHAA-Reloaded/tree/vulkan) | Linux (Windows builds, untested so far) | Vulkan | In development: build it, or take a build from CI |

Each branch's README has its install steps, settings and limits: [`metal`](https://github.com/pstngh/MoHAA-Reloaded/blob/metal/README.md), [`vulkan`](https://github.com/pstngh/MoHAA-Reloaded/blob/vulkan/README.md).

> [!IMPORTANT]
> **This is a fork, not the upstream project.** Problems with these ports belong on [this repository's tracker](https://github.com/pstngh/MoHAA-Reloaded/issues), with the branch you play (`metal` or `vulkan`), not upstream's tracker or Discord.

## Which one

- **A Mac with Apple Silicon:** `metal`. Download it from [Releases](https://github.com/pstngh/MoHAA-Reloaded/releases).
- **A Linux PC:** `vulkan`. Any GPU with a Vulkan 1.1 driver; developed on AMD with Mesa's RADV driver, also played on NVIDIA.
- **Windows:** `vulkan` builds for Windows (its CI makes an installer), but nobody has played it on Windows yet. Reports are welcome.
- **Intel Macs:** neither for now (`metal` needs Apple Silicon; `vulkan` isn't built or tested on macOS).

## What the two share

Both ports are built for low input latency and even frame times, measured at every step:

- **The same game online.** The network protocol, the server-side game code and the data formats are upstream's: both play on stock MOHAA and OpenMoHAA servers.
- **Movement apart from the frame rate.** The commands the server runs come from their own clock, 85 to 125 a second, and every one is the same length at any frame rate (`cl_movefps` on `metal`, `cl_cmdfps` on `vulkan`).
- **Exact frame caps.** `com_maxfps 240` runs at 240 frames a second, not 250.
- **Their own sound mixers, no OpenAL.** `metal` mixes on Core Audio, with Apple's HRTF for headphones; `vulkan` has the Omaha mixer (miniaudio, with HRTF for headphones).

They differ in much else (the window and input layers, the defaults, the sound mixers): see each README.

## Getting the code

```bash
git clone -b metal https://github.com/pstngh/MoHAA-Reloaded.git mohaa-reloaded-metal
```

```bash
git clone -b vulkan https://github.com/pstngh/MoHAA-Reloaded.git mohaa-reloaded-vulkan
```

A plain clone gives this page only.

## Contributing

Pull requests go to `metal` or `vulkan`, not to `main`. Say which branch an issue is about.

## Credits and license

- The [OpenMoHAA](https://github.com/openmoh/openmohaa) team, for the engine (built on [ioquake3](https://github.com/ioquake/ioq3) and the F.A.K.K SDK).
- JayRewd, for the Omaha sound mixer and renderer work merged into `vulkan`, and the ideas measured and re-implemented in both ports; notuern, for testing on NVIDIA and the sub-grid of `metal`'s movement.
- OpenMoHAA is GPL-2.0 ([`COPYING.txt`](COPYING.txt)), and so are these ports. They are independent projects, not affiliated with or endorsed by Electronic Arts.
