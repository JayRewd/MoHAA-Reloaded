# OpenMoHAA → native Metal (Apple Silicon)

## Goal

"Create a FULL macOS Silicon Metal native port of openmohaa, with the ultimate goal of stripping OpenGL entirely from the code by the time we're done, and only have it run Metal from start to finish. Look at all possible ways to improve performance while we do it, increase efficiency, etc. Make it as lean and robust as possible."

(The user's words. Never reword them.)

Standing requirement, added by the user on 2026-10-16: "can we also optimize for lowest input lag/delay possible? like lowest overhead". Judge every design choice on input-to-photon latency and overhead as well as fps. Rules in the plan, §4b.

**Status: all phases (0–8) are done** (session 44, 2026-10-20). `docs/METAL_PORT_PLAN.md` holds the closing summary (at the top), every decision (§3), the measurements (§2) and, in §6, what is optional, parked or needs the user. `README.md`'s first section is the user-facing description. Release 1 is out: [v1.0.0](https://github.com/pstngh/openmohaa-metal/releases/tag/v1.0.0) (session 46, ad-hoc signed, `io.github.pstngh.openmohaa-metal`); §6 "Release 1, what is left" has what follows. New work starts only when the user brings it up: read the plan's closing summary and §6 first. **Phase 9 is in progress** (the user's choice of 2026-10-22, §3.1 #36): a native sound engine, our own mixer on Core Audio playing the PC original's Miles behavior, openal-soft out at the end; the plan's Phase 9 is its spec (9a, the survey and design, done in session 57; 9b, the mixer alone (`code/macos/macos_snd.c`), in session 58; 9c, the sound effects on the native driver (`code/client/snd_native_new.cpp`, behind the latched `s_native`, 1 by default since 9e, 0 = OpenAL until 9g), in session 59; 9d, its streams, music, dialogue, movies and saves, in session 60; 9e, the user's listening checkpoint, in session 61 (every default kept); 9f, Apple's headphone 3D as a setting, next).

## Repo layout

- `main` is the work branch (named `metal` until 2026-10-20, when the user renamed it; GitHub redirects old `metal` links). `origin` = pstngh/openmohaa-metal (public). There is no local mirror of upstream any more: upstream is the remote `upstream` (= openmoh/openmohaa), merged as `upstream/main`. Phase 7a deleted the non-macOS and dead files: a merge of `upstream/main` shows delete/modify conflicts for them; keep them deleted (the plan's §6, "Merging upstream").
- `port/` holds the port's tools: `env.sh` (this machine's paths), `build.sh` (flavors), `run.sh`, `bench.sh` (timedemo of `port/bench/`), `shot.sh` + `tgadiff.py` + `tgamontage.py` (screenshots against GL1 and a saved Metal baseline, cfgs in `port/shots/`), `latency.sh` + `mousemove.c` + `display_trace.py` (the input-to-present probe with synthetic mouse, checked against Instruments), `prof.sh` + `sample_tree.py` + `sample_by_library.py` (`sample` profiles), `glcheck.sh` (no OpenGL in the tree or the binary), `skinab.sh` (GPU against CPU skinning, bit for bit), `kbreplay.sh` (posted key and mouse events through SDL's last commit and the native layer, streams compared; the `Sys_Dialog` check), `sndtest.sh` + `sndtest.c` (the sound mixer alone and the native driver's math: offline tests under ASan, a stress test under TSan, silent device modes), `gcmtest.m` (GCMouse against AppKit, with the user's mouse), `roqtest.py` (a RoQ test video), `release.sh` + `release/INSTALL.txt` (the release zip; CI runs it too), `makeicon.swift` (the app icon from `misc/macos/openmohaa-metal.svg`).
- `.github/workflows/macos-metal.yml`: CI on every push to `main` (README's build on a clean macOS 26 runner, the install checked, the zip as a workflow artifact; never a release). Read it with `gh run list/view --repo pstngh/openmohaa-metal`.
- Gitignored, on the SSD: `build.noindex/` (build trees, installs, logs, the sessions' scratch tools in `sNN/`; safe to delete) and `home.noindex/` (the game's `fs_homepath`: configs, logs, demos, screenshots).

## Hard rules

- **Native only.** No translation layers: no MoltenVK, SPIRV-Cross, ANGLE, metal-shaderconverter or GPTK. Shaders are hand-written MSL.
- **Target:** macOS 26+ on Apple Silicon (arm64) only. Dev machine: Apple M4 Mac mini (4P+6E cores, 10-core GPU, 16 GB, Metal 4), macOS 26, Xcode 27.1, a 240 Hz main display and a 60 Hz 4K.
- **Online compatibility.** The client must keep working on existing MOHAA/OpenMoHAA servers and with retail game data. The network protocol, game/cgame logic and the data formats don't change unless the user agreed. Frame pacing and usercmd timing (`com_maxfps`, `cl_movefps`, the 1 ms frame floor of the stock mode) also count as gameplay feel: ask first. The `glconfig_t` layout is ABI with cgame (copied field by field) and never changes. refexport/refimport sit between the client and the renderer, both in this tree, so they change only when needed, additively. Rendering may look different from GL1 when that makes it faster (the user, 2026-10-18: "it may look different if it's faster"; plan §3.1 #20), never by accident: compare screenshots on every change, and record each deliberate difference in §3.1 #20. Visual only: what the server receives and the gameplay never change for it.
- **Disk.** All build output, caches and temp files stay on the SSD (`/Volumes/1TB BLUE`), inside this repo in `build.noindex/` or `home.noindex/`. Always `source port/env.sh` (it sets TMPDIR and the clang module cache). Nothing goes on the internal disk except what macOS itself writes. Game data is read from `$OMM_GAME_DIR` (default `~/Documents/Games/moh`) and never copied or committed. Only the retail `Pak*.pk3` files are symlinked into the run layout, never the user's own dylibs, mods or configs.
- **Metal shader compiler.** `xcrun metal` fails on this machine on purpose. The Metal Toolchain is an exported disk image under `/Volumes/1TB BLUE/gtav/toolchain.noindex/`. `omm_mount_metal` (from `port/env.sh`) mounts it read-only at `/Volumes/MetalToolchainCryptex`, and the compiler is `$OMM_METAL_BIN/metal`. Never write into `/Volumes/1TB BLUE/gtav`. **Never run `xcodebuild -downloadComponent`** (it installs a copy on the internal disk). (The README tells other people to install the component; that's for their Macs, not this one.)
- **Style.** Match each file's existing code style, indentation and line endings. Keep changes outside the renderer small, so upstream merges stay easy. Keep upstream's part of `README.md` unchanged (the port's section sits above it).
- **Commits.** Commit each finished slice on `main`, with clear messages; credit JayRewd in the message when one of his ideas is used (ideas only, re-implemented).
- **Push.** The user said yes: push `main` to origin at the end of every phase or finished piece of work, once the build and benchmark pass (`git push origin main`). Never push to `upstream`. Nothing else public (a release, a tag, a PR to upstream) without the user.

## Build, run, check

```bash
source port/env.sh                 # every shell, first
port/build.sh metal                # the game (~40 s; mounts the Metal Toolchain) -> build.noindex/metal-install/lib/openmohaa: the sealed, ad-hoc-signed arm64 openmohaa.app (openal-soft and the game/cgame dylibs inside; no omohaaded unless configured with -DBUILD_SERVER=ON)
port/build.sh metalref             # the game at HEAD (METAL_REF=<ref> for another): the "before" of an A/B bench; rebuild it at HEAD before each one
port/build.sh gl                   # the GL1 reference (c9dddadd, the last commit with GL1, plus the client's work log)
port/build.sh sdlref               # the last commit with SDL2 (fd94160b): the input layer's reference
port/run.sh metal +map dm/mohdm6   # run against the retail data; extra args are engine args
port/bench.sh run metal 3          # timedemo, 1280x720 window, 3 runs: work ms/frame and the per-frame work p50/p95/p99/p99.9/max µs (also: metalref, gl, sdlref)
port/bench.sh trace metal          # one traced timedemo: GPU utilization and GPU ms per frame
port/prof.sh metal base 20         # `sample` of the looping timedemo; then port/sample_tree.py build.noindex/prof/base.txt callees <function>
port/shot.sh regress base-s35      # the six fixed views on Metal: vs GL1 and vs the saved baseline (base-s35 since the alpha-tested draw order; port/shot.sh savebase <dir> after a deliberate change)
port/shot.sh pair dm6 shot_dm6.cfg # fixed 3D views, GL1 + Metal, per-view diffs (also: port/shot.sh metal <name> <cfg>, port/shot.sh diff <name>)
port/latency.sh metal win-250 20   # cl_latencyStats with 20 s of synthetic mouse (takes the mouse: the user's OK first); MOUSE=0 LATSTATS=0 TRACE=1 = only what the display showed (needs nobody)
port/glcheck.sh                    # no GL names in code/ and cmake/ beyond the documented exceptions, no OpenGL in the binary
port/skinab.sh                     # GPU-skinning A/B flavor, then port/bench.sh run metalab 1; grep SKINAB build.noindex/bench/metalab-1.log | tail -1 (every "differ" 0)
port/release.sh                    # the release zip from metal-install (clean tree, a build of HEAD) -> build.noindex/release/
port/kbreplay.sh build             # then port/kbreplay.sh run | mouse win|full|hidpi | dialog win|full|full720|space: SDL against native, streams must be identical
port/sndtest.sh                    # the sound mixer (code/macos/macos_snd.c) and the native driver's math: offline tests + stress; device [frames] | load frames s | devices: silent, the output idle first
port/run.sh metal +set s_native 0 +map dm/mohdm6   # OpenAL's sound driver until 9g; native is the default since 9e (s_eventLog 1, s_capture <name>)
```

`bench.sh` and `shot.sh` pin `r_uselod 1` (the game's default is 0 since session 46), so their numbers stay comparable with earlier runs and GL1. Compare renderers on the **`work` ms/frame** figure: the stock mode pads every frame to at least 1 ms, so fps saturates near 1000. The GL reference is `r_primitives 2` (the user's setting), which `bench.sh` passes. A 5 µs change needs an A/B inside one binary (the plan's §6 tool notes: the old path behind a cvar, blocks of four frames A B B A, a check mode comparing outputs); whole bench runs spread by 20–40 µs within one sitting. New flavors are cases in `port/build.sh`; put every number in the plan.

## Gotchas

- The tool shell is zsh, which doesn't split an unquoted `$args`: the engine then gets one quoted `+set a 1 +set b 2` argument and silently ignores it. Build engine arguments as arrays in a bash script. Never `cd` in a Bash call (absolute paths, or `git -C`).
- In OpenMoHAA, `wait N` counts **milliseconds**, and `wait 1` is exactly one frame.
- The game keeps no PID file since session 46 (no "Abnormal Exit" dialog after a killed run); builds of older commits (`gl`, `sdlref`, `ci`) still do, and `run.sh` removes their stale PID files.
- Don't use `s_initsound 0` (upstream crashes on the first footstep); mute with `s_volume 0`.
- Spearhead/Breakthrough: `+set com_target_game 1`/`2` (the upstream launchers are deleted); run the .app's binary directly or `open openmohaa.app --args …`.
- Never put files inside the installed `openmohaa.app`: the install signs it and checks the seal (`codesign --verify --deep --strict`), so a file added afterwards breaks it, and a link out of the bundle makes the next install fail. `run.sh`'s Homebrew openal link in fs_basepath serves only the flavors that dlopen it (`gl`).
- A game window takes the real mouse (the bench, the fixed views, replays, skinab): check `ioreg -c IOHIDSystem | awk '/HIDIdleTime/ {print $NF/1e9; exit}'` first and ask if someone is at the machine. An unanswered question blocks the session, so don't ask while the user is away.
- Every window run needs the screen unlocked (`ioreg -n Root -d1 -a | grep -A1 CGSSessionScreenIsLocked`: no output = unlocked). It locks ~20 minutes after the last input, even between runs: keep batches of window runs under one `caffeinate -d`.
- Below ~960 frames a second the GPU clocks down: compare GPU times only at the same render rate. Report the cores in timing tests (`pthread_cpu_number_np`: 0–5 efficiency, 6–9 performance).
- A game started through Launch Services (`open`, Finder) is its own responsible process: macOS asks once per build (ad-hoc: per code hash) for access to the SSD ("removable volume"), and the run waits on that prompt until the user answers. Runs through `run.sh` (a direct exec) don't ask. A downloaded zip's files are quarantined: Gatekeeper's prompts hold the app, the dylibs' `dlopen` and even `omohaaded` from a shell (plan §5 "Release 1"). Archive Utility here moves an expanded archive to the Trash.
- A build started without `fs_homepath` (not through `run.sh`) writes its configs and data into its own folder (the game folder), its few state files into `~/Library/Application Support/openmohaa/` and its pipeline cache into `~/Library/Caches/<bundle ID>/`, both on the internal disk: for sandbox tests set `$HOME` to a folder under `build.noindex/` (the engine and the cache follow `$HOME`; plan §3.2 "Added in session 46, after the release").
- `cl_movefps` (named `cl_cmdRate` until session 47) defaults to 100; a config's `seta cl_cmdRate` is ignored (no carry-over code, the user's choice: players update their configs), so pass `+set cl_movefps` to builds since then and `+set cl_cmdRate` to older flavors. The user's own config holds `cl_cmdRate 125` (v1.0.0). The plan's history keeps the old name.

## Session hand-off protocol

The user asked for regular session switches, so context never gets compacted and the goal never drifts.

1. All lasting state lives in the repo: this file and `docs/METAL_PORT_PLAN.md`. A new session reads both before anything else, starting with the Goal.
2. One session = one coherent piece of work. Switch early: when the piece is done, or as soon as the session is getting long (lots of tool calls or big file reads), well before automatic compaction.
3. Before switching:
   - the build passes;
   - the run/benchmark check for the piece is done and its numbers are in the plan;
   - the plan's status lines are updated;
   - the plan's §6 says exactly where things stand, what to do next, and lists open bugs and gotchas;
   - everything is committed on `main`, and pushed when the piece is finished.
4. If more work is queued, start the next session with `start_session`: context "fresh", cwd this repo, `use_worktree` false, a short title like "OpenMoHAA Metal port — session N: <piece>". The brief tells it to read CLAUDE.md and the plan's §6, and to do that piece under this same protocol. If the app refuses (for example because the turn was started by another session), propose it instead (initiation "own_initiative") so the user can start it with one click. Either way, end with a 2-3 line report.
5. Big decisions that aren't in the plan's decision list go to the user before they're locked in.
