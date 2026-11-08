#!/usr/bin/env python3
"""Swipe benchmark for the running hotdogb, no hands needed: the kernel's command server injects the touches.

  python tools/hotdogb-swipebench.py [SECONDS] [--no-unlock]      (default 30 s)

Wakes the screen (power-key-free: a tap), swipes up to unlock (no passcode), then swipes between home screen
pages 1 <-> 2 for SECONDS and averages the kernel's 5 s lines that fall inside that window:
  SWEET FB: 5s: N swaps (F fps) ...        frames iOS presented
  SWEET GPU: 5s: ... GPU busy X ms (P%) ... backboardd n/ms ...
Reads the newest logs/runs/*/live.txt (the run's live recorder must be running).
"""
import glob, os, re, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CMD = [sys.executable, os.path.join(ROOT, 'tools', 'flame-cmd.py')]


def cmd(text):
    subprocess.run(CMD + ['--wait', '0.2', text], capture_output=True, text=True)


def live():
    runs = sorted(glob.glob(os.path.join(ROOT, 'logs', 'runs', '*', 'live.txt')), key=os.path.getmtime)
    if not runs:
        sys.exit('no logs/runs/*/live.txt')
    return runs[-1]


def lines(path):
    with open(path, 'rb') as f:
        return f.read().replace(b'\0', b'').decode('utf-8', 'replace').splitlines()


def stamp(line):
    m = re.match(r'\[\s*([0-9.]+)\]', line)
    return float(m.group(1)) if m else None


def main():
    secs = int(next((a for a in sys.argv[1:] if a.isdigit()), '30'))
    path = live()
    print(f'live log: {path}')
    if '--no-unlock' not in sys.argv:
        cmd('tap 540 1200')
        time.sleep(1.0)
        cmd('swipe 540 2350 540 900 300')	# up from the bottom edge: unlock
        time.sleep(2.0)
    n0 = len(lines(path))
    t_end = time.time() + secs
    n = 0
    while time.time() < t_end:
        cmd('swipe 900 1200 150 1200 250' if n % 2 == 0 else 'swipe 150 1200 900 1200 250')
        n += 1
        time.sleep(0.6)
    time.sleep(6.0)	# let the last 5 s window close
    new = lines(path)[n0:]
    sw = [stamp(l) for l in new if 'SWEET CMD: swipe' in l and stamp(l) is not None]
    if not sw:
        sys.exit('no swipes seen in the live log (is the live recorder running?)')
    lo, hi = sw[0] + 5, sw[-1]		# whole 5 s windows inside the swiping
    fps, busy, bb = [], [], []
    for l in new:
        t = stamp(l)
        if t is None or not (lo <= t <= hi + 5):
            continue
        m = re.search(r'SWEET FB: 5s: (\d+) swaps', l)
        if m:
            fps.append(int(m.group(1)) / 5)
        m = re.search(r'GPU busy (\d+) ms \((\d+)%\).*?backboardd (\d+)/(\d+)ms', l)
        if m:
            busy.append(int(m.group(2)))
            bb.append((int(m.group(3)), int(m.group(4))))
    print(f'{n} swipes over {secs} s; {len(fps)} FB windows, {len(busy)} GPU windows (kernel time {lo:.0f}-{hi:.0f})')
    if fps:
        print(f'fps: mean {sum(fps) / len(fps):.1f}  min {min(fps):.1f}  max {max(fps):.1f}   {" ".join(f"{x:.1f}" for x in fps)}')
    if busy:
        subs = sum(b[0] for b in bb)
        ms = sum(b[1] for b in bb)
        print(f'GPU busy: mean {sum(busy) / len(busy):.0f}%; backboardd {subs} submits, {ms / subs if subs else 0:.1f} ms each')


if __name__ == '__main__':
    main()
