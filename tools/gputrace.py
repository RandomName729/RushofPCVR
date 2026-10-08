#!/usr/bin/env python3
"""Sums up where the GPU's time goes from a Turnip trace.

The driver writes the trace when the core runs with
    MESA_GPU_TRACES=print MESA_GPU_TRACEFILE=<file>
(tools/quest-sandbox-test.sh passes such settings through). Every line is an event with the time
the GPU reached it, in nanoseconds. This groups the render passes by what they draw to and how
the driver chose to render them, and prints the milliseconds each group costs per frame.

    gputrace.py <trace> [frames per second] [--passes]

Without a frame rate the figures are per second of trace. --passes lists the passes of one
frame in order.
"""
import re
import sys
from collections import defaultdict

LINE = re.compile(r'^(\d+)\s+[+-]\d+: (\w+)(?:: (.*))?$')


def fields(text):
    result = {}
    for part in (text or '').split(', '):
        if '=' in part:
            name, value = part.split('=', 1)
            result[name.strip()] = value.strip()
    return result


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    list_passes = '--passes' in sys.argv
    path = args[0]
    fps = float(args[1]) if len(args) > 1 else None

    events = []
    with open(path, encoding='utf-8', errors='replace') as handle:
        # A trace cut out of a longer one begins in the middle of a line.
        handle.readline()
        for line in handle:
            match = LINE.match(line.rstrip())
            if match:
                events.append((int(match.group(1)), match.group(2), fields(match.group(3))))
    if not events:
        print('no events')
        return

    passes = []
    current = None
    others = defaultdict(lambda: [0, 0])
    open_events = {}
    for ts, name, info in events:
        if name == 'start_render_pass':
            current = {'start': ts, 'info': info, 'parts': defaultdict(int), 'tiles': 0}
        elif name == 'end_render_pass':
            if current is not None:
                current['end'] = ts
                current['result'] = info
                passes.append(current)
            current = None
        elif name.startswith('start_'):
            open_events[name[6:]] = ts
            if name == 'start_draw_ib_gmem' and current is not None:
                current['tiles'] += 1
        elif name.startswith('end_'):
            kind = name[4:]
            start = open_events.pop(kind, None)
            if start is None:
                continue
            # Stores are stamped out of order (their end is taken before the blit runs).
            length = max(ts - start, 0)
            if current is not None:
                current['parts'][kind] += length
            elif kind not in ('cmd_buffer',):
                others[kind][0] += 1
                others[kind][1] += length

    first = min(e[0] for e in events)
    last = max(e[0] for e in events)
    seconds = (last - first) / 1e9
    frames = fps * seconds if fps else seconds
    unit = 'frame' if fps else 'second'
    print(f'{len(events)} events over {seconds:.2f} s, {len(passes)} render passes'
          f' ({len(passes) / frames:.1f} per {unit})')

    groups = defaultdict(lambda: {'count': 0, 'time': 0, 'draws': 0, 'parts': defaultdict(int)})
    busy = 0
    for p in passes:
        info, result = p['info'], p['result']
        tiled = result.get('tiledRender') == 'true'
        reason = result.get('tilingDisableReason', '')
        key = (f"{info.get('width')}x{info.get('height')}",
               f"{info.get('attachment_count')} att" + (' +depth' if info.get('hasDepth') == 'true' else ''),
               f"load {info.get('loadCPP')} store {info.get('storeCPP')} clear {info.get('clearCPP')}",
               'tiled' if tiled else 'direct' + (f' ({reason})' if reason else ''))
        group = groups[key]
        length = p['end'] - p['start']
        busy += length
        group['count'] += 1
        group['time'] += length
        group['draws'] += int(result.get('drawCount', 0))
        for kind, value in p['parts'].items():
            group['parts'][kind] += value

    print(f'time inside render passes: {busy / 1e6 / frames:.2f} ms per {unit}')
    print(f'{"ms":>8} {"passes":>7} {"draws":>7}  {"ms each":>8}  what')
    for key, group in sorted(groups.items(), key=lambda item: -item[1]['time']):
        parts = ', '.join(f'{kind} {value / 1e6 / frames:.2f}'
                          for kind, value in sorted(group['parts'].items(), key=lambda i: -i[1])
                          if value / 1e6 / frames >= 0.05)
        print(f"{group['time'] / 1e6 / frames:8.2f} {group['count'] / frames:7.1f}"
              f" {group['draws'] / frames:7.1f}  {group['time'] / 1e6 / group['count']:8.3f}"
              f"  {' | '.join(key)}" + (f'   [{parts}]' if parts else ''))
    for kind, (count, length) in sorted(others.items(), key=lambda item: -item[1][1]):
        print(f'{length / 1e6 / frames:8.2f} {count / frames:7.1f}                    outside passes: {kind}')

    if list_passes:
        # One frame's worth, starting at the first pass after the middle of the trace.
        per_frame = max(int(round(len(passes) / frames)), 1) if fps else 60
        begin = len(passes) // 2
        previous_end = None
        for p in passes[begin:begin + per_frame]:
            info, result = p['info'], p['result']
            gap = (p['start'] - previous_end) / 1e6 if previous_end is not None else 0.0
            previous_end = p['end']
            tiled = result.get('tiledRender') == 'true'
            print(f"  +{gap:6.3f} {(p['end'] - p['start']) / 1e6:7.3f} ms  {info.get('width')}x{info.get('height')}"
                  f" att {info.get('attachment_count')} depth {info.get('hasDepth')}"
                  f" load {info.get('loadCPP')} store {info.get('storeCPP')} clear {info.get('clearCPP')}"
                  f" draws {result.get('drawCount')} {'tiled x' + str(p['tiles']) if tiled else 'direct'}"
                  f" lrz {result.get('lrzStatus')} {result.get('tilingDisableReason', '')}")


if __name__ == '__main__':
    main()
