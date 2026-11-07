#!/usr/bin/env python3
"""Replace the default iOS 16 wallpaper's HEIC layers with PNGs on the running hotdogb iOS root.

Run with Windows Python from src while a kernel runs:

  python tools/hotdogb-wallpaper-png.py [DIR]        (DIR default ../hotdogb/wallpaper)
  python tools/hotdogb-wallpaper-png.py --restore    (put the HEIC originals back)

DIR/heic/iOSRoot/...  the original layers (7z x ROOT.img "iOSRoot/Library/Wallpaper/Collections/iOS_16~iphone
                      .wallpaperCollection/Wallpapers/7400.WWDC_2022-375w-812h@3x~iphone.wallpaper/*.ca/assets/*.HEIC")
DIR/png/iOSRoot/...   the same names holding PNG data (pillow-heif; ImageIO sniffs content, not the extension)

Why (the flame's "HEIC decodes to nothing" lesson, HANDOFF 2026-10-21): the lock-screen wallpaper (WWDC 2022, five
HEIC layers in CA packages) decodes through CollectionsPoster into 420f surfaces that Metal cannot use; on hotdogb
the first Metal run faulted the GPU 1.7 s after those surfaces appeared (read at address 0), which lost
backboardd's Metal device. PNG layers decode to plain BGRA. The originals stay in /var/local/wallpaper-heic-backup
on the phone and in DIR/heic here. Apple's files: DIR stays out of git.
"""
import os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FILES = [sys.executable, os.path.join(ROOT, 'tools', 'flame-files.py')]
ENV = dict(os.environ, FLAME_FILES_HOST=os.environ.get('FLAME_FILES_HOST', '192.168.7.2'))
BACKUP = '/var/local/wallpaper-heic-backup'


def files(*args, check=True):
    r = subprocess.run(FILES + list(args), env=ENV, capture_output=True, text=True)
    if check and r.returncode:
        sys.exit(f'flame-files {" ".join(args)}: {r.stdout.strip()} {r.stderr.strip()}')
    return r


def layers(top):
    """(local path, phone path) for every .HEIC under top/iOSRoot"""
    out = []
    for d, _, names in os.walk(os.path.join(top, 'iOSRoot')):
        for n in sorted(names):
            if n.endswith('.HEIC'):
                p = os.path.join(d, n)
                out.append((p, '/' + os.path.relpath(p, os.path.join(top, 'iOSRoot')).replace(os.sep, '/')))
    return out


def main():
    args = [a for a in sys.argv[1:] if a != '--restore']
    restore = '--restore' in sys.argv
    top = args[0] if args else os.path.join(ROOT, '..', 'hotdogb', 'wallpaper')
    src = layers(os.path.join(top, 'heic' if restore else 'png'))
    if not src:
        sys.exit(f'no layers under {top}/{"heic" if restore else "png"}/iOSRoot')
    if not restore:
        files('mkdir', BACKUP, '755', check=False)
        for p, remote in layers(os.path.join(top, 'heic')):
            files('put', p, f'{BACKUP}/{os.path.basename(os.path.dirname(os.path.dirname(remote)))}-'
                  f'{os.path.basename(remote)}', '644')
    for p, remote in src:
        files('put', p, remote, '644')
        print(f'{"HEIC" if restore else "PNG "} {os.path.getsize(p):>8} -> {remote}')
    print('done: reboot (python tools/flame-cmd.py reboot) so CollectionsPoster decodes them again')


if __name__ == '__main__':
    main()
