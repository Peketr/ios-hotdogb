#!/usr/bin/env python3
"""Read the modem/WLAN firmware and the modem's remote-filesystem partitions from the running OnePlus 7T.

Run with Windows Python from src while a kernel (0148+) runs, 192.168.7.1 on the phone's USB adapter:

  python tools/hotdogb-fw-pull.py [--out DIR]

Uses the kernel file server's read-only PARTS/PART commands (tools/flame-files.py, sweet_files.c), so
neither Android nor TWRP is needed; nothing on the phone is written. Out (default ../hotdogb/fw, outside
git - Qualcomm/OnePlus firmware and the phone's IMEI data):
  parts.txt     every GPT partition: name, LUN, first block, blocks (4 KiB)
  modem_X.img   the modem partition (vfat, NON-HLOS), unpacked by WSL's 7z into modem-image/
  modem/        what wland serves: modem.mdt + modem.bNN, wlanmdsp.mbn, *.jsn (bdwlan.bin: pick by hand)
  rmtfs/        modemst1.img modemst2.img fsg.img fsc.img (also a backup of them)
"""
import argparse, os, re, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FILES = [sys.executable, os.path.join(ROOT, 'tools', 'flame-files.py')]
ENV = dict(os.environ, FLAME_FILES_HOST=os.environ.get('FLAME_FILES_HOST', '192.168.7.2'))


def files(*args):
    r = subprocess.run(FILES + list(args), env=ENV, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f'flame-files {" ".join(args)}: {r.stdout.strip()} {r.stderr.strip()}')
    return r.stdout


def wslpath(p):
    p = os.path.abspath(p)
    return '/mnt/' + p[0].lower() + p[2:].replace('\\', '/')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(ROOT, '..', 'hotdogb', 'fw'))
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    for d in ('', 'modem', 'rmtfs', 'modem-image'):
        os.makedirs(os.path.join(out, d), exist_ok=True)

    table = files('parts')
    open(os.path.join(out, 'parts.txt'), 'w').write(table)
    parts = {}
    for line in table.splitlines():
        f = line.split()
        if len(f) == 4:
            parts[f[0]] = (int(f[1]), int(f[2]), int(f[3]))
    print(f'{len(parts)} partitions; the modem ones:')
    for n in sorted(parts):
        if re.match(r'(modem|fsg|fsc|persist|bluetooth|dsp)', n):
            lun, first, blocks = parts[n]
            print(f'  {n:12} LUN {lun} {blocks * 4096 >> 10} KiB')

    modem = next((n for n in ('modem_a', 'modem') if n in parts), None)
    if not modem:
        sys.exit('no modem partition in parts.txt')
    img = os.path.join(out, modem + '.img')
    print(files('part', modem, img).strip())
    for n in ('modemst1', 'modemst2', 'fsg', 'fsc'):
        if n in parts:
            print(files('part', n, os.path.join(out, 'rmtfs', n + '.img')).strip())
        else:
            print(f'(no {n} partition)')

    shutil.rmtree(os.path.join(out, 'modem-image'))
    r = subprocess.run(['wsl.exe', '-d', 'Ubuntu', '--', '7z', 'x', '-y', '-bd', '-o' + wslpath(os.path.join(out, 'modem-image')),
                        wslpath(img)], capture_output=True, text=True)
    if r.returncode:
        sys.exit('7z: ' + r.stdout[-500:] + r.stderr[-500:])
    image = os.path.join(out, 'modem-image')
    for d in os.listdir(image):
        if d.lower() == 'image' and os.path.isdir(os.path.join(image, d)):
            image = os.path.join(image, d)
            break
    names = sorted(os.listdir(image))
    print(f'modem image: {len(names)} files in {os.path.relpath(image, out)}')
    n = 0
    for name in names:
        low = name.lower()
        if re.fullmatch(r'modem\.mdt|modem\.b\d+|wlanmdsp\.mbn|.*\.jsn', low):
            shutil.copyfile(os.path.join(image, name), os.path.join(out, 'modem', low))
            n += 1
    print(f'modem/: {n} files')
    cands = [x for x in names if re.search(r'bdwlan|bdf|regdb', x, re.I)]
    print('board data candidates:', ' '.join(cands) if cands else '(none in the modem image)')


if __name__ == '__main__':
    main()
