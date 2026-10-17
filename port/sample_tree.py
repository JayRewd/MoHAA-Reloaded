#!/usr/bin/env python3
"""Call paths around one function in the main thread of a macOS `sample` report.

    python3 -I port/sample_tree.py X.txt callees <function> [levels]   samples (inclusive) of each callee path below
                                                                         the function, merged over all its occurrences
    python3 -I port/sample_tree.py X.txt callers <function> [levels]   samples of the function by caller chain

<function> is the bare symbol (e.g. RB_EndSurface, MB_PrepareDraw, "-[AGXG16GFamilyCommandBuffer commit]").
"""
import collections
import re
import sys

LINE = re.compile(r"^(?P<indent>[\s+!:|]*)(?P<count>\d+) (?P<frame>.+?)\s+\(in (?P<lib>[^)]+)\)")


def name(frame):
    frame = re.sub(r"\s+\+ \d.*$", "", frame)
    if frame.startswith("-[") or frame.startswith("+["):
        return frame.split("]")[0] + "]"
    return frame.split("(")[0].strip().split(" ")[0]


def main_thread(path):
    lines, thread, in_graph = [], -1, False
    for raw in open(path, errors="replace"):
        if raw.startswith("Call graph:"):
            in_graph = True
            continue
        if in_graph and raw.strip() == "":
            break
        if not in_graph:
            continue
        if re.match(r"^\s+\d+ Thread_", raw):
            thread += 1
            continue
        if thread == 0:
            m = LINE.match(raw)
            if m:
                lines.append((len(m.group("indent")), int(m.group("count")), name(m.group("frame"))))
    return lines


def main():
    path, mode, func = sys.argv[1], sys.argv[2], sys.argv[3]
    levels = int(sys.argv[4]) if len(sys.argv) > 4 else (3 if mode == "callees" else 4)
    agg, total, stack = collections.Counter(), 0, []
    for depth, count, fn in main_thread(path):
        while stack and stack[-1][0] >= depth:
            stack.pop()
        outer = next((j for j in range(len(stack)) if stack[j][1] == func), None)
        if mode == "callees":
            if fn == func and outer is None:
                total += count
            if outer is not None and len(stack) - outer <= levels:
                agg[" > ".join([s[1] for s in stack[outer + 1:]] + [fn])] += count
        elif fn == func and outer is None:
            total += count
            agg[" < ".join(s[1] for s in reversed(stack[-levels:]))] += count
        stack.append((depth, fn))
    print(f"{total} samples in {func}")
    for path_, n in agg.most_common(40):
        print(f"{n:6d}  {path_}")


if __name__ == "__main__":
    main()
