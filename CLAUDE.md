# OpenMoHAA → native Metal (Apple Silicon)

## Goal

"Create a FULL macOS Silicon Metal native port of openmohaa, with the ultimate goal of stripping OpenGL entirely from the code by the time we're done, and only have it run Metal from start to finish. Look at all possible ways to improve performance while we do it, increase efficiency, etc. Make it as lean and robust as possible."

(The user's words. Never reword them.)

Standing requirement, added by the user on 2026-10-16: "can we also optimize for lowest input lag/delay possible? like lowest overhead". Judge every design choice on input-to-photon latency and overhead as well as fps. Rules and the planned work are in the plan, §4b.

The full plan, findings, decisions, phase status and the hand-over note are in `docs/METAL_PORT_PLAN.md`. Read its "Next session" section before doing anything.

## Repo layout

- `main` is an untouched mirror of upstream (`upstream` = openmoh/openmohaa). Never commit to it or push it. Phase 7a deleted the non-macOS and dead files on `metal`: a merge of `main` shows delete/modify conflicts for them; keep them deleted (the plan's Phase 7, "Session 15 result").
- `metal` is the work branch. `origin` = pstngh/openmohaa-metal (public).
- `port/` holds this port's scripts: `env.sh`, `build.sh`, `run.sh`, `bench.sh`, `shot.sh` + `tgadiff.py` + `tgamontage.py` (renderer screenshot comparison and side-by-side tiles, cfgs in `port/shots/`), `roqtest.py` (a RoQ test video for the cinematic path), `latency.sh` + `mousemove.c` + `display_trace.py` (the input-to-present latency probe with synthetic mouse movement, checked against Instruments), `prof.sh` + `sample_tree.py` + `sample_by_library.py` (CPU profiles of the looping timedemo with `sample`), `glcheck.sh` (no OpenGL left in the tree or the binary), `skinab.sh` + `skinab_patch.py` (the GPU-skinning A/B build: GPU vs CPU vertex outputs compared bit for bit), `kbreplay.sh` + `kbreplay.m` + `kbreplay_patch.py` (Phase 6: in-process posted key and mouse events through SDL (the last SDL commit) and the native layer, the `SE_KEY`/`SE_CHAR`/`SE_MOUSE` streams compared; also the `Sys_Dialog` check), `gcmtest.m` (GCMouse against AppKit's mouse events, with the user's own mouse), the trace/profile parsers and the reference benchmark demo (`port/bench/`).
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
port/build.sh metal                # the game: Metal only, the renderer linked in, the native AppKit layer for the window and input (code/macos; no SDL since 6g) (mounts the Metal Toolchain itself) -> build.noindex/metal-install/lib/openmohaa (~40 s): a sealed, ad-hoc-signed arm64 openmohaa.app (openal-soft built from code/thirdparty, in Contents/Frameworks) + game/cgame dylibs + omohaaded
port/build.sh sdlref               # the SDL baseline: the last commit with SDL2 (SDL_REF in build.sh, fd94160b, the code of db88239d), from a git archive export -> build.noindex/sdlref-install; for the user's half of 6f (OMM_FLAVOR=sdlref, port/bench.sh run sdlref 3)
port/build.sh metalref             # the Metal game at the last commit (METAL_REF=<ref> for another), the "before" of a before/after bench: port/bench.sh run metalref 3
port/build.sh gl                   # the GL1 reference: the last commit with GL1 (c9dddadd), built from a git archive export -> build.noindex/gl-install
port/run.sh metal +map dm/mohdm6   # run against the retail data; extra args are engine args (port/run.sh gl = the GL1 reference)
port/bench.sh run metal 3          # timedemo of port/bench/bench_mohdm6.dm_8, 1280x720 window, 3 runs (extra engine args after the run count); prints work ms/frame and the per-frame work p50/p95/p99/p99.9/max µs
port/bench.sh run gl 3             # the same on the GL1 reference
port/bench.sh trace metal          # one traced timedemo: GPU utilization and GPU ms per frame
port/prof.sh metal base 20         # `sample` of the main thread over the looping timedemo; then port/sample_tree.py build.noindex/prof/base.txt callees RB_EndSurface
port/shot.sh metal menu shot_menu.cfg      # in-game screenshots from a cfg (also: opengl1 = the GL1 reference), into build.noindex/shots
port/shot.sh diff menu             # per-pixel difference GL1 vs Metal for every screenshot pair of that name
port/shot.sh pair dm6 shot_dm6.cfg # fixed 3D views (fixedtime, tele/face): GL1 reference + Metal, per-view diffs
port/shot.sh regress base-s29      # the six fixed views on Metal only: vs GL1 per view and vs a saved Metal baseline (port/shot.sh savebase <dir>; base-s29 since the µs limiter, base-s9 before); engine args after the dir go to every run
port/latency.sh metal win-250 20   # cl_latencyStats in dm/mohdm6 with 20 s of synthetic mouse (takes the mouse!); TRACE=1 checks it against the display; OMM_FLAVOR=sdlref runs that install; MOUSE=0 LATSTATS=0 TRACE=1 = only what the display showed (no input, no probe)
port/glcheck.sh                    # no GL names in code/ and cmake/ beyond the documented exceptions, no OpenGL in the binary
port/skinab.sh                     # GPU-skinning A/B flavor (metalab), then: port/bench.sh run metalab 1; grep SKINAB build.noindex/bench/metalab-1.log | tail -1 (every "differ" 0)
port/kbreplay.sh build             # Phase 6: scratch flavors kbsdl (SDL, from SDL_REF's export) + kbnat (native, the work tree) with the replay command; then port/kbreplay.sh run: both in a window (~15 s each, takes the keyboard focus), streams must be identical
port/kbreplay.sh mouse win         # the mouse replay (also: full = exclusive fullscreen, hidpi = the 2x display): takes the real cursor (and the screen in fullscreen); streams must be identical (hidpi: only the UI cursor lines differ)
port/kbreplay.sh dialog win        # Sys_Dialog with the game up (also: full, full720 = a mode switch, space, start = the startup's "Abnormal Exit" question): what covers the dialog, the cursor, the game afterwards; both logs, no verdict
```

New flavors are added as cases in `port/build.sh`. Benchmark every flavor with the same command, and put the numbers in the plan. OpenGL is gone from the tree since Phase 4; GL1 comparisons (screenshots, bench) use the `gl` reference flavor, whose renderer never changes unless `GL1_REF` in `port/build.sh` does (`GL1_PATCHES` there adds client-only benchmark logging, such as the per-frame work log). `cl_renderer` is ignored (read-only "metal"). Compare renderers on the **`work` ms/frame** figure: fps saturates near 1000 because the engine pads every frame to at least 1 ms. The GL reference is `r_primitives 2` (the user's setting), which `bench.sh` passes by default.

Gotchas: in OpenMoHAA, `wait N` counts **milliseconds**, and `wait 1` is exactly one frame. A killed run leaves a PID file, and the next start then blocks on a modal "Abnormal Exit" dialog (`run.sh` removes stale PID files). Don't use `s_initsound 0` (upstream crashes on the first footstep); mute with `s_volume 0`. Spearhead/Breakthrough: `+set com_target_game 1`/`2` (the upstream launchers are deleted, Phase 7a); run the .app binary directly. Never put files inside the installed `openmohaa.app`: the install signs it and checks the seal (`codesign --verify --deep --strict`), so a file added after the install breaks the seal, and a link out of the bundle makes the next install fail (Phase 7b). Since Phase 7c the app carries its own openal-soft (`Contents/Frameworks`, linked directly); `run.sh`'s Homebrew openal link in fs_basepath serves only the flavors that still dlopen it (`gl`, `metalref` before 7c).

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
