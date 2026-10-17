#!/usr/bin/env python3
"""Self time per library (and top functions) for one thread of a macOS `sample` report.

    sample <pid> <seconds> -file X.txt
    python3 -I port/sample_by_library.py X.txt [thread-index (0 = main)] [subtree-function]

With subtree-function (e.g. RB_ExecuteRenderCommands), only samples under that function are counted.
"""
import collections
import re
import sys

LINE = re.compile(r"^(?P<indent>[\s+!:|]*)(?P<count>\d+) (?P<frame>.+?)\s+\(in (?P<lib>[^)]+)\)")


def main():
    path = sys.argv[1]
    want_thread = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    subtree = sys.argv[3] if len(sys.argv) > 3 else None
    lines, thread, in_graph = [], -1, False
    for raw in open(path, errors="replace"):
        if raw.startswith("Call graph:"):
            in_graph = True
            continue
        if in_graph and raw.strip() == "":
            break
        if not in_graph:
            continue
        if "Thread_" in raw and re.match(r"^\s+\d+ Thread_", raw):
            thread += 1
            continue
        if thread == want_thread:
            m = LINE.match(raw)
            if m:
                lines.append((len(m.group("indent")), int(m.group("count")), m.group("frame"), m.group("lib")))
    by_lib, by_func = collections.Counter(), collections.Counter()
    total = 0
    stack = []          # (depth, inside-subtree)
    for i, (depth, count, frame, lib) in enumerate(lines):
        while stack and stack[-1][0] >= depth:
            stack.pop()
        inside = (stack[-1][1] if stack else False) or (subtree is not None and frame.split(" ")[0] == subtree)
        if subtree is None:
            inside = True
        stack.append((depth, inside))
        children = 0
        for d2, c2, _, _ in lines[i + 1:]:
            if d2 <= depth:
                break
            if d2 == min(x[0] for x in lines[i + 1:i + 2]):
                children += c2
        self_count = count - children
        if inside and self_count > 0:
            by_lib[lib] += self_count
            by_func[f"{frame.split(' ')[0]} ({lib})"] += self_count
            total += self_count
    print(f"{total} samples" + (f" under {subtree}" if subtree else ""))
    for lib, n in by_lib.most_common(12):
        print(f"  {100 * n / total:5.1f}%  {lib}")
    print("top functions:")
    for fn, n in by_func.most_common(25):
        print(f"  {100 * n / total:5.1f}%  {fn}")


if __name__ == "__main__":
    main()
