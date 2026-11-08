#!/usr/bin/env python3
"""Sum a Turnip GPU trace (MESA_GPU_TRACES=print_csv) by render pass kind.

  python tools/hotdogb-gputrace.py TRACE.csv [SECONDS]      (default: the last 25 s of GPU time)

Getting the trace (SweetMetal2 reads these when a process opens its Metal device):
  /var/local/mtl2-env       MESA_GPU_TRACES=print_csv / MESA_GPU_TRACEFILE=/var/local/gputrace-bb.csv
  /var/local/gputrace-bb.csv must exist beforehand (empty, mode 666): backboardd's sandbox cannot create it.
  /var/local/mtl2-clients   "backboardd" only for the run (other clients would overwrite the file: Mesa opens it "w").
Rows: frame,batch,time_ns,event,args. start_X/end_X pairs nest; a render pass's gmem/sysmem/binning work
is summed into it. Passes are grouped by size, attachments, tiling and bins; work outside passes
(copies, clears) is listed by event.
"""
import collections, csv, sys


def args_of(s):
    out = {}
    for part in s.split(', '):
        if '=' in part:
            k, v = part.split('=', 1)
            out[k.strip()] = v.strip().rstrip(',')
    return out


def main():
    path = sys.argv[1]
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 25.0
    rows = []
    with open(path, newline='', errors='replace') as f:
        for r in csv.reader(f):
            if len(r) < 4 or not r[2].isdigit():
                continue
            rows.append((int(r[2]), r[3], ','.join(r[4:])))
    if not rows:
        sys.exit('no rows')
    t_hi = max(r[0] for r in rows)
    t_lo = t_hi - int(secs * 1e9)
    rows = [r for r in rows if r[0] >= t_lo]
    span = (t_hi - min(r[0] for r in rows)) / 1e9

    open_ev = collections.defaultdict(list)     # name -> start times
    rp = None                                   # current pass: dict
    kinds = collections.defaultdict(lambda: collections.Counter())
    kind_n = collections.Counter()
    kind_time = collections.Counter()
    outside = collections.Counter()
    outside_n = collections.Counter()
    for t, ev, a in rows:
        if ev == 'start_render_pass':
            p = args_of(a)
            rp = {'t0': t, 'a': p, 'work': collections.Counter()}
            continue
        if ev == 'end_render_pass':
            if rp is None:
                continue
            e = args_of(a)
            p = rp['a']
            key = (f"{p.get('width')}x{p.get('height')} att {p.get('attachment_count')} "
                   f"{'tiled' if e.get('tiledRender') == 'true' else 'SYSMEM'} bins {p.get('numberOfBins')} "
                   f"clr/ld/st cpp {p.get('clearCPP')}/{p.get('loadCPP')}/{p.get('storeCPP')} "
                   f"draws~{min(int(e.get('drawCount', '0') or 0), 99) // 10 * 10}+"
                   + (f" ({e.get('tilingDisableReason')})" if e.get('tilingDisableReason') else ''))
            kind_n[key] += 1
            kind_time[key] += t - rp['t0']
            kinds[key].update(rp['work'])
            rp = None
            continue
        if ev.startswith('start_'):
            open_ev[ev[6:]].append(t)
        elif ev.startswith('end_'):
            name = ev[4:]
            if open_ev[name]:
                d = t - open_ev[name].pop()
                if rp is not None:
                    rp['work'][name] += d
                elif name != 'cmd_buffer':
                    outside[name] += d
                    outside_n[name] += 1

    total = sum(kind_time.values()) + sum(outside.values())
    print(f'{path}: {span:.1f} s of GPU time, {sum(kind_n.values())} render passes; '
          f'passes {sum(kind_time.values()) / 1e6:.0f} ms, outside passes {sum(outside.values()) / 1e6:.0f} ms '
          f'({total / 1e9 / span * 100:.0f}% of the span)')
    print('\nrender passes by kind (ms total, count, ms each; work inside: ms):')
    for key, ns in kind_time.most_common(15):
        w = kinds[key]
        inner = ', '.join(f'{k} {v / 1e6:.0f}' for k, v in w.most_common(5))
        print(f'  {ns / 1e6:8.0f} ms  {kind_n[key]:5d} x {ns / kind_n[key] / 1e6:6.2f}  {key}\n            [{inner}]')
    print('\noutside render passes (ms total, count):')
    for k, ns in outside.most_common(10):
        print(f'  {ns / 1e6:8.0f} ms  {outside_n[k]:5d}  {k}')


if __name__ == '__main__':
    main()
