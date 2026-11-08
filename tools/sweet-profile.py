#!/usr/bin/env python3
"""Count a sample burst (kernel 0159, "sample NAMES COUNT MS") into a short profile.

  python tools/sweet-profile.py [LIVE.txt] [-o OUT.txt] [--top N]

Reads the newest logs/runs/*/live.txt (or LIVE.txt), takes the last burst
("#### SAMPLE BURST START" .. "SMP --- burst done") and prints, per process:
  - how often each thread was running (the busy threads),
  - the hottest pcs (self time) and the addresses most often on the stack
    (inclusive: each counted once per thread sample).
The output keeps the "SMP name[pid] slide .. mh .." header lines, so it can be
symbolicated on the Mac as it is:

  python3 tools/sweet-crash.py CACHE ROOT OUT.txt

A thread that is in a syscall shows the syscall stub in pc (kernel time on its
behalf); one preempted in user space shows where it was.
"""
import collections, glob, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    args = sys.argv[1:]
    out_path, top = None, 25
    if '-o' in args:
        i = args.index('-o')
        out_path = args[i + 1]
        del args[i:i + 2]
    if '--top' in args:
        i = args.index('--top')
        top = int(args[i + 1])
        del args[i:i + 2]
    if args:
        path = args[0]
    else:
        runs = sorted(glob.glob(os.path.join(ROOT, 'logs', 'runs', '*', 'live.txt')), key=os.path.getmtime)
        if not runs:
            sys.exit('no logs/runs/*/live.txt')
        path = runs[-1]
    with open(path, 'rb') as f:
        text = f.read().replace(b'\0', b'').decode('utf-8', 'replace').splitlines()
    start = max((i for i, l in enumerate(text) if '#### SAMPLE BURST START' in l), default=None)
    if start is None:
        sys.exit(f'no sample burst in {path}')
    end = next((i for i in range(start, len(text)) if 'SMP --- burst done' in text[i]), len(text))
    burst = text[start:end + 1]

    headers = {}
    threads = collections.defaultdict(collections.Counter)    # name -> tid -> samples
    self_pc = collections.defaultdict(collections.Counter)    # name -> pc -> samples
    incl = collections.defaultdict(collections.Counter)       # name -> address -> samples
    samples = 0
    for l in burst:
        m = re.search(r'SMP (\S+)\[(\d+)\] slide (0x[0-9a-f]+) mh (0x[0-9a-f]+)', l)
        if m:
            headers.setdefault(m.group(1), m.group(0))
            continue
        m = re.search(r'SMP (\S+) t(\d+) tid (\d+) pc (0x[0-9a-f]+) lr (0x[0-9a-f]+) sp 0x[0-9a-f]+ bt(.*)', l)
        if not m:
            continue
        name, tid, pc, lr = m.group(1), m.group(3), m.group(4), m.group(5)
        bt = re.findall(r'0x[0-9a-f]+', m.group(6))
        samples += 1
        threads[name][tid] += 1
        self_pc[name][pc] += 1
        for a in dict.fromkeys([pc, lr] + bt):
            incl[name][a] += 1

    out = [f'# {path}: {burst[0].split("####", 1)[1].strip()}; {samples} running-thread samples']
    for name in sorted(threads):
        out.append('')
        out.append(headers.get(name, f'SMP {name}[?] (no header line)'))
        out.append('# busy threads (samples running): ' +
                   ', '.join(f'tid {t} {n}' for t, n in threads[name].most_common(12)))
        out.append(f'# self (pc), top {top}:')
        for a, n in self_pc[name].most_common(top):
            out.append(f'HOT {name} {n:5d} pc {a}')
        out.append(f'# inclusive (on the stack), top {top}:')
        for a, n in incl[name].most_common(top):
            out.append(f'INC {name} {n:5d} at {a}')
    report = '\n'.join(out) + '\n'
    print(report, end='')
    if out_path:
        with open(out_path, 'w') as f:
            f.write(report)
        print(f'written to {out_path}')


if __name__ == '__main__':
    main()
