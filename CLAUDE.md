# OpenMoHAA → native Metal (Apple Silicon)

## Goal

"Create a FULL macOS Silicon Metal native port of openmohaa, with the ultimate goal of stripping OpenGL entirely from the code by the time we're done, and only have it run Metal from start to finish. Look at all possible ways to improve performance while we do it, increase efficiency, etc. Make it as lean and robust as possible."

(The user's words. Never reword them.)

Standing requirement, added by the user on 2026-10-16: "can we also optimize for lowest input lag/delay possible? like lowest overhead". Judge every design choice on input-to-photon latency and overhead as well as fps. Rules and the planned work are in the plan, §4b.

The full plan, findings, decisions, phase status and the hand-over note are in `docs/METAL_PORT_PLAN.md`. Read its "Next session" section before doing anything.

## Repo layout

- `main` is an untouched mirror of upstream (`upstream` = openmoh/openmohaa). Never commit to it or push it.
- `metal` is the work branch. `origin` = pstngh/openmohaa-metal (public).
- `port/` holds this port's scripts: `env.sh`, `build.sh`, `run.sh`, `bench.sh`, `shot.sh` + `tgadiff.py` (renderer screenshot comparison, cfgs in `port/shots/`), the trace/profile parsers and the reference benchmark demo (`port/bench/`).
- Gitignored, on the SSD: `build.noindex/` (build trees, installs, logs, traces; safe to delete) and `home.noindex/` (the game's `fs_homepath`: configs, logs, demos, screenshots).

## Hard rules

- **Native only.** No translation layers: no MoltenVK, SPIRV-Cross, ANGLE, metal-shaderconverter or GPTK. Shaders are hand-written MSL.
- **Target:** macOS 26+ on Apple Silicon (arm64) only. Dev machine: Apple M4 Mac mini (4P+6E cores, 10-core GPU, 16 GB, Metal 4), macOS 26, Xcode 27.1.
- **Online compatibility.** The client must keep working on existing MOHAA/OpenMoHAA servers and with retail game data. The network protocol, game/cgame logic and the data formats don't change unless a phase explicitly says so and the user agreed. Frame pacing and usercmd timing (`com_maxfps`, the 1 ms frame floor) also count as gameplay feel: ask first. The `glconfig_t` layout is ABI with cgame (copied field by field) and never changes. refexport/refimport sit between the client and the renderer, both in this tree, so they change only when a phase needs it, additively. Rendering must look the same as the GL renderer (compare screenshots).
- **Disk.** All build output, caches and temp files stay on the SSD (`/Volumes/1TB BLUE`), inside this repo in `build.noindex/` or `home.noindex/`. Always `source port/env.sh` (it sets TMPDIR and the clang module cache). Nothing goes on the internal disk except what macOS itself writes. Game data is read from `$OMM_GAME_DIR` (default `~/Documents/Games/moh`) and never copied or committed. Only the retail `Pak*.pk3` files are symlinked into the run layout, never the user's own dylibs, mods or configs.
- **Metal shader compiler.** `xcrun metal` fails on this machine on purpose. The Metal Toolchain is an exported disk image under `/Volumes/1TB BLUE/gtav/toolchain.noindex/`. `omm_mount_metal` (from `port/env.sh`) mounts it read-only at `/Volumes/MetalToolchainCryptex`, and the compiler is `$OMM_METAL_BIN/metal`. Never write into `/Volumes/1TB BLUE/gtav`. **Never run `xcodebuild -downloadComponent`** (it installs a copy on the internal disk).
- **Style.** Match each file's existing code style, indentation and line endings. Keep changes outside the renderer small, so upstream merges stay easy.
- **Commits.** Commit at the end of every phase (or slice) on `metal`, with clear messages.
- **Push.** The user said yes: push `metal` to origin at the end of every phase, once the build and benchmark pass (`git push origin metal`). Never push `main`.

## Build, run, benchmark

```bash
source port/env.sh                 # every shell, first
port/build.sh gl                   # upstream GL1 reference build -> build.noindex/gl-install (~40 s, arm64)
port/build.sh metal                # GL1 + Metal renderer in one install (mounts the Metal Toolchain itself)
port/run.sh gl +map dm/mohdm6      # run against the retail data; extra args are engine args
port/run.sh metal +set cl_renderer metal   # the Metal renderer (cl_renderer opengl1 = GL1 from the same install)
port/bench.sh run gl 3             # timedemo of port/bench/bench_mohdm6.dm_8, 1280x720 window, 3 runs
port/bench.sh run metal 3 +set cl_renderer metal   # the same on Metal (extra engine args after the run count)
port/bench.sh trace gl             # one traced timedemo: GPU utilization and GPU ms per frame
port/shot.sh metal menu shot_menu.cfg      # in-game screenshots from a cfg (also: opengl1), into build.noindex/shots
port/shot.sh diff menu             # per-pixel difference GL1 vs Metal for every screenshot pair of that name
port/shot.sh pair dm6 shot_dm6.cfg # fixed 3D views (fixedtime, tele/face): both renderers + per-view diffs
```

New flavors (e.g. `metal`) are added as cases in `port/build.sh`. Benchmark every flavor with the same command, and put the numbers in the plan. Compare renderers on the **`work` ms/frame** figure: fps saturates near 1000 because the engine pads every frame to at least 1 ms. The GL reference is `r_primitives 2` (the user's setting), which `bench.sh` passes by default.

Gotchas: in OpenMoHAA, `wait N` counts **milliseconds**, and `wait 1` is exactly one frame. A killed run leaves a PID file, and the next start then blocks on a modal "Abnormal Exit" dialog (`run.sh` removes stale PID files). Don't use `s_initsound 0` (upstream crashes on the first footstep); mute with `s_volume 0`. The upstream launchers (`launch_openmohaa_*`) don't work on macOS; run the .app binary directly.

## Session hand-off protocol

The user asked for regular session switches, so context never gets compacted and the goal never drifts.

1. All lasting state lives in the repo: this file and `docs/METAL_PORT_PLAN.md`. A new session reads both before anything else, starting with the Goal.
2. One session = one phase, or one coherent slice of a large phase. Switch early: when the phase or slice is done, or as soon as the session is getting long (lots of tool calls or big file reads), well before automatic compaction. Never roll into the next phase in the same session.
3. Before switching:
   - the build passes;
   - the run/benchmark check for the slice is done and its numbers are in the plan;
   - the plan's status lines are updated;
   - the plan's "Next session" section says exactly where things stand, what to do next, and lists open bugs and gotchas;
   - everything is committed on `metal`, and pushed at the end of a phase.
4. Then start the next session with `start_session`: context "fresh", cwd this repo, `use_worktree` false, a short title like "OpenMoHAA Metal port — session N: <phase>". The brief tells it to read CLAUDE.md and the plan's "Next session" section, and to do that slice under this same protocol. If the app refuses (for example because the turn was started by another session), propose it instead (initiation "own_initiative") so the user can start it with one click. Either way, end with a 2-3 line report.
5. Big decisions that aren't in the plan's decision list go to the user before they're locked in.
