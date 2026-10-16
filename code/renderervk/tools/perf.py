#!/usr/bin/env python3
"""Frame times of views with GL1 and Vulkan (standard library only).

  perf.py <map> x y z pitch yaw roll [options] [-- +set args...]
  perf.py --list views/perf.txt [options] [-- +set args...]

options:
  --sp              single-player map (spdevmap instead of devmap)
  --frames N        frames per measurement (default 1000)
  --renderers A,B   cl_renderer values (renderer_<name>.so of MOH_BINDIR),
                    default opengl1,vulkan; a copy of an older
                    renderer_vulkan.so under another name compares builds
  --runs N          measurements per renderer, view and size (default 5)
  --sizes WxH,...   window sizes (default 640x480)
  --pause PID,...   SIGSTOP these processes and their descendants while the
                    frames are measured and SIGCONT them right after (about
                    5 s per measurement). Only for processes whose owner
                    agreed to it.
  --cold            delete main/vkpipelines.cache before each run (the
                    Vulkan renderer then compiles every pipeline again,
                    unless the driver has its own cache: set
                    MESA_SHADER_CACHE_DISABLE=true for that)
  --json FILE       also write every measurement to FILE
  --exec CMDS       console commands run once the camera is at the view,
                    ';' separated (e.g. 'cg_acidtrip 4': random dynamic lights)
  --stacks PREFIX   sample the main thread's stack every millisecond with
                    libstacks.so (tools/rtimer/stacks.c) and write each
                    run to PREFIX_<view>_<size>_<renderer>_<run>.txt, with
                    the measured window; tools/stacks.py sums them up

A list file has one view per line: `name map x y z pitch yaw roll [sp]`,
or `views <view list> [shot ...]`: the shots of a view list (the path is
relative to the list file; all its shots when none is named), each with
the list's map and the `set` commands before it in the list (spawned
models, lights), run after loading and before the camera moves.

Each measurement loads the map in a new game (devmap, or spdevmap) with
`fixedtime 16`, puts the camera at the position like a view list `shot`,
waits 2 s of game time, then lets N frames run with the frame rate uncapped
(`com_maxfps 0`, `r_swapInterval 0`, no validation) and measures the wall
clock time they take. `wait` counts game milliseconds and every frame
advances the game by 16 ms, so `wait 16*N` is exactly N frames. The console
output is read from stdout: the two `echo` markers are timestamped here, and
the `com_speeds 1` lines in between give the average front end (rf) and
back end (bk) milliseconds the engine measured (whole milliseconds per
frame, so only the averages mean something). The game keeps running during
the measurement (AI, particles).

With renderer_timed.so in MOH_BINDIR (tools/rtimer/build.sh) the game
runs `cl_renderer timed`, which loads renderer_<renderer>.so and measures
with a microsecond clock the time the main thread spends in the renderer
per frame (RenderScene: front end; EndFrame: back end, driver, GPU waits
and presentation): "renderer", "scene" and "end" in the output. That is
what to compare: when a frame takes less than a millisecond, Com_Frame
waits for the next whole millisecond (`com_maxfps 0` still means at most
1000 frames per second) and com_speeds' whole-millisecond rf/bk are
measured right after that tick.

While the frames run, gpu_busy_percent of amdgpu (sysfs) and the clock of
the core that runs the game's main thread are sampled every 5 ms. The
Vulkan renderer's `vkinfo` command is run after loading and around the
measured frames: per frame GPU time from timestamps (main command buffer
and the copy to the swapchain image), draws, render passes, vertex bytes,
fence waits; for loading: pipelines and memory allocations with the time
spent creating them.

The runs interleave the renderers: run i measures every view and size with
each renderer, starting with renderer i (mod the number of renderers), so
slow changes of the machine (clock, temperature, other jobs) affect all of
them alike. The summary gives per view, size and renderer the median frame
time with the lowest and highest, and the medians of rf and bk.

Frame times are only comparable on an otherwise idle machine. Every
measurement reports how many CPUs other processes kept busy while it ran
(from /proc/stat minus the game's own CPU time; the compositor showing
a thousand frames a second takes about one) and warns above 2.

Uses MOH_BASEPATH, MOH_HOMEPATH and MOH_BINDIR like the other tools; the
window is 640x480 unless --sizes or the extra arguments say otherwise.
"""

import argparse
import glob
import json
import os
import pty
import re
import shutil
import signal
import statistics
import subprocess
import sys
import threading
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(TOOLS, "..", "..", ".."))
BASEPATH = os.environ.get("MOH_BASEPATH", os.path.expanduser("~/Documents/moh"))
HOMEPATH = os.environ.get("MOH_HOMEPATH", "/tmp/moh-vktest")
# absolute: the game runs in it, and LD_PRELOAD (--stacks) must find libstacks.so from there
BINDIR = os.path.abspath(os.environ.get("MOH_BINDIR", os.path.join(REPO, "build-vk", "RelWithDebInfo")))
CPUS = os.cpu_count() or 1
HZ = os.sysconf("SC_CLK_TCK")
BUSY = 2.0  # CPUs used by other processes above which a measurement is flagged
TIMED = os.path.exists(os.path.join(BINDIR, "renderer_timed.so"))
GPU_BUSY = sorted(glob.glob("/sys/class/drm/card*/device/gpu_busy_percent"))


class View:
    def __init__(self, name, map, x, y, z, pitch, yaw, roll, sp=False, setup=()):
        self.name, self.map, self.sp = name, map, sp
        self.pos = (x, y, z)
        self.angles = (pitch, yaw, roll)
        self.setup = list(setup)  # console commands run before the camera moves


def list_views(path, shots):
    """the shots of a view list (shots.py's format) as views: each gets the
    list's map and the `set` commands that come before it in the list"""
    views, map_, sp, setup = [], None, False, []
    with open(path) as f:
        for line in f:
            w = line.split("#", 1)[0].strip()
            cmd, _, rest = w.partition(" ")
            if cmd in ("map", "spmap"):
                map_, sp, setup = rest, cmd == "spmap", []
            elif cmd == "set":
                setup.append(rest)
            elif cmd == "shot":
                p = rest.split()
                if not shots or p[0] in shots:
                    views.append(View(p[0], map_, *p[1:7], sp=sp, setup=setup))
    missing = set(shots) - {v.name for v in views}
    if missing:
        sys.exit(f"{path}: no shot {' '.join(sorted(missing))}")
    return views


def cfg_name(view):
    return "perf_" + re.sub(r"[^A-Za-z0-9_]", "_", view.name) + ".cfg"


def write_cfg(view, frames, commands=()):
    cfg = ["set thereisnomonkey 1", "set fixedtime 16",
           f"{'spdevmap' if view.sp else 'devmap'} {view.map}", "wait 3000", "finishloadingscreen", "wait 1000",
           "god", "notarget", "noclip", "ui_hud 0", "cg_drawviewmodel 0", *view.setup,
           "tele %s %s %s" % view.pos, "face %s %s %s" % view.angles, *commands,
           "vkinfo", "echo perf_ready", "wait 2000",
           "com_speeds 1", "vkinfo", "echo perf_start", "set rt_record 1", f"wait {16 * frames}", "set rt_record 0",
           "vkinfo", "echo perf_end", "com_speeds 0", "wait 100", "quit"]
    os.makedirs(os.path.join(HOMEPATH, "main"), exist_ok=True)
    with open(os.path.join(HOMEPATH, "main", cfg_name(view)), "w") as f:
        f.write("\n".join(cfg) + "\n")


def lines(fd):
    buf = b""
    while True:
        try:
            data = os.read(fd, 4096)
        except OSError:  # EIO once the game has exited
            data = b""
        if not data:
            break
        buf += data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            yield line
    os.close(fd)


def cpu_times():
    """(busy, total) jiffies of all CPUs"""
    with open("/proc/stat") as f:
        v = [int(x) for x in f.readline().split()[1:]]
    idle = v[3] + v[4]
    total = sum(v[:8])
    return total - idle, total


def proc_cpu(pid):
    """utime + stime of a process, all threads, in jiffies"""
    try:
        with open(f"/proc/{pid}/stat") as f:
            fields = f.read().rsplit(")", 1)[1].split()
        return int(fields[11]) + int(fields[12])
    except OSError:
        return 0


def descendants(pids):
    """the processes and all their descendants that still exist"""
    children = {}
    for d in os.listdir("/proc"):
        if not d.isdigit():
            continue
        try:
            with open(f"/proc/{d}/stat") as f:
                ppid = int(f.read().rsplit(")", 1)[1].split()[1])
        except (OSError, ValueError, IndexError):
            continue
        children.setdefault(ppid, []).append(int(d))
    out, todo = [], list(pids)
    while todo:
        p = todo.pop()
        if p in out or not os.path.exists(f"/proc/{p}"):
            continue
        out.append(p)
        todo += children.get(p, [])
    return out


class Pauser:
    """SIGSTOP/SIGCONT of the processes the user allowed to pause"""

    def __init__(self, pids):
        self.pids = pids
        self.stopped = []

    def stop(self):
        if not self.pids or self.stopped:
            return
        for p in descendants(self.pids):
            try:
                os.kill(p, signal.SIGSTOP)
                self.stopped.append(p)
            except OSError:
                pass

    def cont(self):
        for p in self.stopped:
            try:
                os.kill(p, signal.SIGCONT)
            except OSError:
                pass
        self.stopped = []


class Sampler(threading.Thread):
    """every 5 ms: amdgpu's gpu_busy_percent and the clock (MHz) of the
    core the game's main thread runs on"""

    def __init__(self, pid):
        super().__init__(daemon=True)
        self.pid = pid
        self.gpu, self.mhz = [], []
        self.done = threading.Event()

    def run(self):
        while not self.done.wait(0.005):
            try:
                if GPU_BUSY:
                    with open(GPU_BUSY[0]) as f:
                        self.gpu.append(int(f.read()))
                with open(f"/proc/{self.pid}/task/{self.pid}/stat") as f:
                    cpu = int(f.read().rsplit(")", 1)[1].split()[36])
                with open(f"/sys/devices/system/cpu/cpu{cpu}/cpufreq/scaling_cur_freq") as f:
                    self.mhz.append(int(f.read()) / 1000)
            except (OSError, ValueError, IndexError):
                pass

    def stop(self):
        self.done.set()
        self.join()
        return (statistics.mean(self.gpu) if self.gpu else None,
                statistics.mean(self.mhz) if self.mhz else None)


def parse_vkinfo(line):
    return {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", line)}


RTIMER = re.compile(r"rtimer: (\d+) frames, ms per frame: interval ([\d.]+), renderer ([\d.]+) ([\d.]+) ([\d.]+), "
                    r"scene ([\d.]+) ([\d.]+), endframe ([\d.]+) ([\d.]+) ([\d.]+), beginframe ([\d.]+)"
                    r"(?:, interval ([\d.]+) ([\d.]+) ([\d.]+) ([\d.]+))?(?:, 2d ([\d.]+), load ([\d.]+))?")


def run(renderer, view, extra, pauser, cold, stacks=None):
    for d in ("configs", "save"):
        shutil.rmtree(os.path.join(HOMEPATH, "main", d), ignore_errors=True)
    if cold:
        try:
            os.remove(os.path.join(HOMEPATH, "main", "vkpipelines.cache"))
        except OSError:
            pass
    env = dict(os.environ)
    if stacks:
        env["LD_PRELOAD"] = os.path.join(BINDIR, "libstacks.so")
        env["STACKS_OUT"] = stacks
    if TIMED:
        # renderer_timed.so (tools/rtimer) loads renderer_<renderer>.so
        env["MOH_TIMED_RENDERER"] = renderer
    cmd = ["./openmohaa", "+set", "fs_basepath", BASEPATH, "+set", "fs_homepath", HOMEPATH,
           "+set", "cl_renderer", "timed" if TIMED else renderer, "+set", "r_fullscreen", "0", "+set", "r_mode", "3",
           "+set", "s_volume", "0", "+set", "s_musicvolume", "0", "+set", "s_ambientvolume", "0",
           "+set", "in_nograb", "1", "+set", "in_mouse", "0", "+set", "ui_gmbox", "0",
           "+set", "com_maxfps", "0", "+set", "r_swapInterval", "0", "+set", "r_vkValidation", "0"]
    cmd += extra + ["+exec", cfg_name(view)]
    # a terminal makes the game's stdout line buffered, so the markers are
    # read when they are printed (a pipe would be block buffered)
    master, slave = pty.openpty()
    proc = subprocess.Popen(cmd, cwd=BINDIR, env=env, stdin=subprocess.DEVNULL, stdout=slave, stderr=slave)
    os.close(slave)
    start = end = None
    rf, bk, n = 0, 0, 0
    timed, sampler, gpuBusy, mhz = None, None, None, None
    vkinfo = []
    try:
        for raw in lines(master):
            line = raw.decode("latin-1", "replace").strip()
            if line == "perf_ready":
                pauser.stop()
            elif line == "perf_start":
                start = time.monotonic()
                cpu0, game0 = cpu_times(), proc_cpu(proc.pid)
                sampler = Sampler(proc.pid)
                sampler.start()
            elif line == "perf_end":
                end = time.monotonic()
                cpu1, game1 = cpu_times(), proc_cpu(proc.pid)
                gpuBusy, mhz = sampler.stop()
                sampler = None
                pauser.cont()
            elif line.startswith("vkinfo: frames="):
                vkinfo.append(parse_vkinfo(line))
            elif line.startswith("rtimer: ") and RTIMER.match(line):
                v = RTIMER.match(line).groups()
                timed = {"frames": int(v[0]), "interval": float(v[1]), "renderer": float(v[2]),
                         "renderer_median": float(v[3]), "renderer_p99": float(v[4]),
                         "scene": float(v[5]), "scene_median": float(v[6]),
                         "endframe": float(v[7]), "endframe_median": float(v[8]), "endframe_p99": float(v[9]),
                         "beginframe": float(v[10])}
                if v[11] is not None:
                    timed.update(interval_median=float(v[11]), interval_p99=float(v[12]),
                                 interval_max=float(v[13]), interval_sd=float(v[14]))
                if v[15] is not None:
                    timed.update(twod=float(v[15]), load=float(v[16]))
            elif start is not None and end is None:
                m = re.match(r"frame:\s*\d+ all:\s*\d+ sv:\s*-?\d+ ev:\s*-?\d+ cl:\s*-?\d+ gm:\s*\d+ rf:\s*(\d+) bk:\s*(\d+)", line)
                if m:
                    rf += int(m.group(1))
                    bk += int(m.group(2))
                    n += 1
    finally:
        pauser.cont()
        if sampler:
            sampler.stop()
        if proc.poll() is None:
            proc.kill()
        proc.wait()
    if start is None or end is None:
        print(f"{renderer}: no measurement (exit status {proc.returncode})", file=sys.stderr)
        return None
    if stacks and os.path.exists(stacks):
        with open(stacks, "a") as f:
            f.write(f"window {int(start * 1e9)} {int(end * 1e9)}\n")
    secs = end - start
    busy = (cpu1[0] - cpu0[0]) / max(cpu1[1] - cpu0[1], 1) * CPUS
    others = busy - (game1 - game0) / HZ / secs
    # vkinfo after loading, before and after the measured frames
    load = vkinfo[0] if len(vkinfo) == 3 else None
    frames = vkinfo[2] if len(vkinfo) == 3 else None
    return {"secs": secs, "rf": rf / max(n, 1), "bk": bk / max(n, 1), "n": n, "others": others,
            "gpu": gpuBusy, "mhz": mhz, "timed": timed, "vkload": load, "vkframes": frames}


def per_frame(vk, key, scale=1.0):
    return vk.get(key, 0) * scale / max(vk["frames"], 1)


def gpu_ms(vk, key):
    return vk[key] / 1000 / max(vk["gpuFrames"], 1)


def parse_size(s):
    w, h = s.lower().split("x")
    return int(w), int(h)


def size_args(size):
    if size == (640, 480):
        return []
    return ["+set", "r_mode", "-1", "+set", "r_customwidth", str(size[0]), "+set", "r_customheight", str(size[1])]


def main():
    argv = sys.argv[1:]
    extra = []
    if "--" in argv:
        i = argv.index("--")
        argv, extra = argv[:i], argv[i + 1:]
    ap = argparse.ArgumentParser()
    ap.add_argument("view", nargs="*", help="map x y z pitch yaw roll")
    ap.add_argument("--list")
    ap.add_argument("--sp", action="store_true")
    ap.add_argument("--frames", type=int, default=1000)
    ap.add_argument("--renderers", default="opengl1,vulkan")
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--sizes", default="640x480")
    ap.add_argument("--pause", default="")
    ap.add_argument("--cold", action="store_true")
    ap.add_argument("--stacks")
    ap.add_argument("--json")
    ap.add_argument("--exec", default="", help="console commands run at the view, ';' separated")
    args = ap.parse_args(argv)

    views = []
    if args.list:
        with open(args.list) as f:
            for line in f:
                w = line.split("#", 1)[0].split()
                if w and w[0] == "views":
                    views += list_views(os.path.join(os.path.dirname(args.list), w[1]), w[2:])
                elif w:
                    views.append(View(*w[:8], sp=len(w) > 8 and w[8] == "sp"))
    if args.view:
        if len(args.view) != 7:
            ap.error("a view is: map x y z pitch yaw roll")
        views.append(View(args.view[0], *args.view, sp=args.sp))
    if not views:
        ap.error("no view")
    renderers = args.renderers.split(",")
    sizes = [parse_size(s) for s in args.sizes.split(",")]
    pauser = Pauser([int(p) for p in args.pause.split(",") if p])
    for v in views:
        write_cfg(v, args.frames, [c.strip() for c in args.exec.split(';') if c.strip()])

    # SIGTERM runs the finally blocks too, so paused processes are continued
    signal.signal(signal.SIGTERM, lambda *a: sys.exit(1))

    results = []
    for i in range(args.runs):
        for v in views:
            for size in sizes:
                for k in range(len(renderers)):
                    r = renderers[(i + k) % len(renderers)]
                    load = os.getloadavg()[0]
                    stacks = (f"{args.stacks}_{re.sub(r'[^A-Za-z0-9_]', '_', v.name)}_{size[0]}x{size[1]}_{r}_{i + 1}.txt"
                              if args.stacks else None)
                    res = run(r, v, size_args(size) + extra, pauser, args.cold, stacks)
                    if res is None:
                        continue
                    res.update(run=i, view=v.name, size="%dx%d" % size, renderer=r, load=load)
                    results.append(res)
                    ms = 1000 * res["secs"] / args.frames
                    t, vk, vl = res["timed"], res["vkframes"], res["vkload"]
                    print(f"run {i + 1} {v.name:15s} {res['size']:9s} {r:8s} {ms:6.3f} ms/frame ({1000 / ms:6.1f} fps)"
                          + (f"  renderer {t['renderer']:6.3f} (scene {t['scene']:6.3f} 2d {t.get('twod', 0):6.3f} end {t['endframe']:6.3f})"
                             f" load {t.get('load', 0):5.0f} ms" if t else
                             f"  rf {res['rf']:5.2f} bk {res['bk']:5.2f}")
                          + (f"  GPU {res['gpu']:3.0f} %" if res["gpu"] is not None else "")
                          + (f"  {res['mhz']:4.0f} MHz" if res["mhz"] is not None else "")
                          + (f"  gpu frame {gpu_ms(vk, 'gpuFrameUs'):5.3f} (copy {gpu_ms(vk, 'gpuPresentUs'):5.3f})"
                             f" {per_frame(vk, 'draws'):5.0f} draws ({per_frame(vk, 'staticDraws'):4.0f} static, {per_frame(vk, 'skinDraws'):4.0f} skinned) {per_frame(vk, 'renderPasses'):4.2f} passes"
                             f" {per_frame(vk, 'vertexBytes', 1 / 1024):5.0f} KB" if vk and vk["gpuFrames"] else "")
                          + (f"  load: {vl['pipelines']} pipelines {vl['pipelineUs'] / 1000:.0f} ms,"
                             f" {vl['textures']} textures {vl['textureUs'] / 1000:.0f} ms,"
                             f" {vl['allocations']} allocations {vl['allocationUs'] / 1000:.0f} ms" if vl else "")
                          + f"  others {res['others']:4.1f} CPUs, load {load:5.2f}"
                          + ("  WARNING: busy machine" if res["others"] > BUSY else ""), flush=True)
                    if args.json:
                        with open(args.json, "w") as f:
                            json.dump({"frames": args.frames, "extra": extra, "results": results}, f, indent=1)

    print()
    print(f"medians over {args.runs} runs (lowest-highest), ms per frame: frame time, fps of the median"
          + (", renderer time (main thread in the renderer), its scene (front end) and endframe (back end) parts" if TIMED else
             ", com_speeds rf/bk") + ", GPU busy")
    for v in views:
        for size in sizes:
            for r in renderers:
                rs = [x for x in results if x["view"] == v.name and x["size"] == "%dx%d" % size and x["renderer"] == r]
                if not rs:
                    continue
                ms = [1000 * x["secs"] / args.frames for x in rs]
                med = statistics.median(ms)
                line = f"{v.name:15s} {'%dx%d' % size:9s} {r:8s} {med:6.3f} ({min(ms):.3f}-{max(ms):.3f}) {1000 / med:6.1f} fps"
                ts = [x["timed"] for x in rs if x["timed"]]
                if ts:
                    rt = [t["renderer"] for t in ts]
                    line += (f"  renderer {statistics.median(rt):6.3f} ({min(rt):.3f}-{max(rt):.3f})"
                             f" scene {statistics.median(t['scene'] for t in ts):6.3f}"
                             f" 2d {statistics.median(t.get('twod', 0) for t in ts):6.3f}"
                             f" end {statistics.median(t['endframe'] for t in ts):6.3f}"
                             f" load {statistics.median(t.get('load', 0) for t in ts):5.0f} ms")
                else:
                    line += f"  rf {statistics.median(x['rf'] for x in rs):5.2f} bk {statistics.median(x['bk'] for x in rs):5.2f}"
                gs = [x["gpu"] for x in rs if x["gpu"] is not None]
                if gs:
                    line += f"  GPU {statistics.median(gs):3.0f} %"
                ms_ = [x["mhz"] for x in rs if x["mhz"] is not None]
                if ms_:
                    line += f" {statistics.median(ms_):4.0f} MHz"
                vks = [x["vkframes"] for x in rs if x["vkframes"] and x["vkframes"]["gpuFrames"]]
                if vks:
                    line += (f"  gpu frame {statistics.median(gpu_ms(x, 'gpuFrameUs') for x in vks):5.3f}"
                             f" copy {statistics.median(gpu_ms(x, 'gpuPresentUs') for x in vks):5.3f}")
                print(line + f"  others {max(x['others'] for x in rs):4.1f} CPUs max, n={len(rs)}")


if __name__ == "__main__":
    main()
