#!/usr/bin/env python3
"""What the display really showed, from an Instruments Metal System Trace (port/latency.sh with TRACE=1).

  display_trace.py <trace> [game log written with cl_latencyStats 2]

Per display: swaps per second, how many swaps showed an openmohaa frame, whether they went direct to display
(and the reason Core Animation gives when not), and the status of openmohaa's presented handlers (2 = shown
with a time, 3 = reported as never shown). With the game log, every present time the probe reported is matched
to the display swap at that instant and its frame: the check that the probe's present times are real.
PROCESS=<name> looks at another process's frames instead of openmohaa's (e.g. a test program).
"""
import bisect
import collections
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

PROCESS = os.environ.get('PROCESS', 'openmohaa')  # the process whose frames are looked at


def table(trace, schema):
    xml = subprocess.run(['xcrun', 'xctrace', 'export', '--input', trace, '--xpath',
                          f'/trace-toc/run[@number="1"]/data/table[@schema="{schema}"]'],
                         capture_output=True, check=True).stdout
    root = ET.fromstring(xml)
    names = [c.find('mnemonic').text for c in root.iter('col')]
    ids, out = {}, []
    for row in root.iter('row'):
        vals = {}
        for name, el in zip(names, row):
            if 'ref' in el.attrib:
                el = ids[el.attrib['ref']]
            else:
                for sub in el.iter():
                    if 'id' in sub.attrib:
                        ids[sub.attrib['id']] = sub
            vals[name] = el
        out.append(vals)
    return out


def fmt(el):
    return el.attrib.get('fmt') or el.text or ''


def pct(a, f):
    return a[min(len(a) - 1, int(f * len(a)))]


def main():
    trace = sys.argv[1]
    epoch_us = int(table(trace, 'time-info')[0]['mabs-epoch'].text) * 125 / 3 / 1000  # mach ticks -> us (Apple Silicon)

    swaps = table(trace, 'displayed-surfaces-interval')
    by_display = collections.defaultdict(list)
    for v in swaps:
        by_display[fmt(v['display-name'])].append(v)
    frames_at = {}  # swap time (us, host clock) -> openmohaa frame numbers shown
    for name, vs in sorted(by_display.items()):
        starts = [int(v['start'].text) for v in vs]
        span = (starts[-1] - starts[0]) / 1e9 if len(starts) > 1 else 0
        ours = [v for v in vs if PROCESS in fmt(v['event-label'])]
        direct = collections.Counter(fmt(v['direct-to-display']) for v in ours)
        reasons = collections.Counter()
        for v in ours:
            for m in re.finditer(re.escape(PROCESS) + r' \(\d+\):[^/]*?cannot go direct to display due to ([^/)]*)', fmt(v['event-label'])):
                reasons[m.group(1).strip()] += 1
            t = round(epoch_us + int(v['start'].text) / 1000)
            frames_at[t] = [int(f) for m in re.finditer(re.escape(PROCESS) + r' \(\d+\):Frame ([\d,]+)', fmt(v['event-label']))
                            for f in m.group(1).split(',')]
        res = collections.Counter(fmt(v['resolution']) for v in vs)
        print(f'{name} {", ".join(res)}: {len(vs)} swaps in {span:.2f} s ({len(vs) / span if span else 0:.0f}/s), '
              f'{len(ours)} with an openmohaa frame; direct to display: {dict(direct)}')
        for r, c in reasons.most_common(3):
            print(f'    not direct ({c}x): {r}')

    handlers = [v for v in table(trace, 'ca-client-presented-handler') if PROCESS in fmt(v['process'])]
    print('openmohaa presented handlers by status (2 = shown with a time, 3 = never shown):',
          dict(collections.Counter(v['status'].text for v in handlers)))

    if len(sys.argv) < 3:
        return
    requests = sorted(epoch_us + int(v['timestamp'].text) / 1000 for v in table(trace, 'ca-client-present-request')
                      if PROCESS in fmt(v['process']))
    probe = []
    for line in open(sys.argv[2], errors='replace'):
        m = re.match(r'cl_latencyStats frame: tag -?\d+ commit (\d+) gpu (-?\d+) present (-?\d+)', line)
        if m:
            probe.append((int(m.group(1)), int(m.group(3))))
    times = sorted(frames_at)
    matched, same_frame, outside, nearest = 0, 0, 0, []
    for commit, present in probe:
        if present <= 0:
            continue
        i = bisect.bisect_left(requests, commit)
        near = [(abs(requests[j] - commit), j) for j in (i - 1, i) if 0 <= j < len(requests)]
        if not near or min(near)[0] > 2000:
            outside += 1
            continue
        k = min(near)[1]  # this frame's present request; Instruments numbers frames from 1, modulo 1000
        j = bisect.bisect_left(times, present - 2)
        cands = [times[x] for x in (j - 1, j) if 0 <= x < len(times)]
        if cands:
            nearest.append(min((present - t for t in cands), key=abs))
        if j < len(times) and abs(times[j] - present) <= 2:
            matched += 1
            same_frame += (k + 1) % 1000 in frames_at[times[j]]
    if nearest:
        nearest.sort()
        print(f'probe present times inside the trace: {len(nearest)}; at an openmohaa swap (within 2 us): {matched}, '
              f'showing that very frame: {same_frame}; present - nearest swap (us) p5 {pct(nearest, .05):.0f} '
              f'p50 {pct(nearest, .5):.0f} p95 {pct(nearest, .95):.0f}')
    else:
        print(f'no probe present times inside the trace ({outside} outside)')


if __name__ == '__main__':
    main()
