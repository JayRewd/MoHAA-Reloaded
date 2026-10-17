#!/usr/bin/env python3
"""GPU time per frame for one process, from a Metal System Trace export.

    xcrun xctrace export --input X.trace \
        --xpath '/trace-toc/run[@number="1"]/data/table[@schema="metal-gpu-intervals"]' > X-gpu.xml
    python3 -I port/gpu_frame_times.py X-gpu.xml [process-name] [timedemo-seconds game-frames]

Prints the process's total GPU busy time (union of all its GPU intervals, so overlapping vertex/fragment
work counts once), the span from its first to last GPU interval and the utilization. With the timedemo's
duration and frame count (from its result line) it finds the timedemo in the trace (the window of that length
with the most trace frames) and prints the GPU busy time per game frame in it.
Also prints per-"frame" stats using the trace's own frame numbers; note that
Apple's GL-on-Metal splits one game frame into several trace frames, so those only mean something for
the Metal renderer (one command buffer per frame).
"""
import statistics
import sys
import xml.etree.ElementTree as ET

COLS = ["start", "duration", "channel", "frame", "latency", "depth", "label", "state", "uuid", "color",
        "process", "gpu", "subtitle", "iosurface", "bytes", "cmdbuffer", "encoder", "submission"]


def union_ns(intervals):
    """Total length of a set of (start, end) intervals, counting overlaps once."""
    intervals = sorted(intervals)
    total, cur_s, cur_e = 0, intervals[0][0], intervals[0][1]
    for s, e in intervals[1:]:
        if s > cur_e:
            total += cur_e - cur_s
            cur_s, cur_e = s, e
        else:
            cur_e = max(cur_e, e)
    return total + cur_e - cur_s


def main():
    path = sys.argv[1]
    proc_name = sys.argv[2] if len(sys.argv) > 2 else "openmohaa"
    ids = {}            # xctrace writes repeated values once (id=) and then refers to them (ref=)
    frames = {}         # frame number -> list of (start, end) in ns
    for _, elem in ET.iterparse(path, events=("end",)):
        if elem.tag != "row":
            continue
        values = {}
        for col, child in zip(COLS, list(elem)):
            if "ref" in child.attrib:
                tag, fmt, text = ids[child.attrib["ref"]]
            else:
                tag, fmt, text = child.tag, child.attrib.get("fmt", ""), child.text or ""
                # remember every element with an id, including nested ones (e.g. the pid inside a process)
                for sub in child.iter():
                    if "id" in sub.attrib:
                        ids[sub.attrib["id"]] = (sub.tag, sub.attrib.get("fmt", ""), sub.text or "")
            values[col] = (tag, fmt, text)
        elem.clear()
        if values.get("process", ("", "", ""))[1].split(" (")[0] != proc_name:
            continue
        if values["frame"][0] == "sentinel" or values["depth"][2] not in ("0", ""):
            continue
        start = int(values["start"][2])
        frames.setdefault(values["frame"][2], []).append((start, start + int(values["duration"][2])))

    busy, first_start = [], []
    for intervals in frames.values():
        busy.append(union_ns(intervals) / 1e6)
        first_start.append(min(intervals)[0])
    if not busy:
        sys.exit(f"no GPU intervals for process '{proc_name}'")
    everything = [iv for intervals in frames.values() for iv in intervals]
    span = (max(e for _, e in everything) - min(s for s, _ in everything)) / 1e6
    total_busy = union_ns(everything) / 1e6
    print(f"{proc_name}: GPU busy {total_busy:.1f} ms over a {span:.1f} ms span "
          f"({100 * total_busy / span:.1f}% utilization)")
    # The timedemo is the stretch with the most frames: find the window of the timedemo's length with the most
    # trace frames (robust to the loading screen, menus and the idle tail), then GPU busy time / game frames.
    if len(sys.argv) > 4:
        demo_ns, game_frames = int(float(sys.argv[3]) * 1e9), int(sys.argv[4])
        starts = sorted(first_start)
        best_count, best_start, j = 0, starts[0], 0
        for i, s in enumerate(starts):
            while starts[j] < s - demo_ns:
                j += 1
            if i - j + 1 > best_count:
                best_count, best_start = i - j + 1, s - demo_ns
        window = [(max(s, best_start), min(e, best_start + demo_ns)) for s, e in everything
                  if e > best_start and s < best_start + demo_ns]
        window_busy = union_ns(window) / 1e6
        print(f"{proc_name}: timedemo window {demo_ns / 1e6:.0f} ms: GPU busy {window_busy:.1f} ms "
              f"({100 * window_busy / (demo_ns / 1e6):.1f}% utilization), "
              f"{window_busy / game_frames:.3f} ms GPU per game frame ({game_frames} frames)")
    busy.sort()
    first_start.sort()
    gaps = [(b - a) / 1e6 for a, b in zip(first_start, first_start[1:])]
    pct = lambda p: busy[min(len(busy) - 1, int(len(busy) * p))]
    print(f"{proc_name}: {len(busy)} frames, GPU busy per frame median {statistics.median(busy):.3f} ms, "
          f"mean {statistics.mean(busy):.3f} ms, p95 {pct(0.95):.3f} ms, p99 {pct(0.99):.3f} ms; "
          f"median frame interval {statistics.median(gaps):.3f} ms")


if __name__ == "__main__":
    main()
