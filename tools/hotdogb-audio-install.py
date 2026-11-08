#!/usr/bin/env python3
"""Install the SweetAudio HAL plug-in (phone/audio/hal) on the running hotdogb, through the kernel file server.

  python tools/hotdogb-audio-install.py SweetAudio.tgz        (tar of SweetAudio.driver, from the Mac:
      tar -C phone/audio/hal/out -czf SweetAudio.tgz SweetAudio.driver)
  python tools/hotdogb-audio-install.py --remove

Also enables Apple's VirtualAudio.plugin again (tools/sweet-root-tweaks.py moved it to HAL.disabled after a
mediaserverd crash; ios-topaz's plug-in gives it the "Hawking" / "Codec" devices it needs): a symlink
/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin -> ../HAL.disabled/VirtualAudio.plugin; --remove removes it too.
mediaserverd loads HAL plug-ins at its start: reboot the phone afterwards.
audiod (phone/audio, boot job org.iosonpixel.audiod) must run too: it reads /var/local/audio.fifo.
"""
import os, subprocess, sys, tarfile, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FILES = [sys.executable, os.path.join(ROOT, 'tools', 'flame-files.py')]
ENV = dict(os.environ, FLAME_FILES_HOST=os.environ.get('FLAME_FILES_HOST', '192.168.7.2'), MSYS_NO_PATHCONV='1')
DEST = '/System/Library/Audio/Plug-Ins/HAL/SweetAudio.driver'
VA_LINK = '/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin'
VA_TARGET = '../HAL.disabled/VirtualAudio.plugin'


def files(*args, check=True):
    r = subprocess.run(FILES + list(args), env=ENV, capture_output=True, text=True)
    if check and r.returncode:
        sys.exit(f'flame-files {" ".join(args)}: {r.stdout.strip()} {r.stderr.strip()}')
    return r


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    if sys.argv[1] == '--remove':
        for f in ('_CodeSignature/CodeResources', 'SweetAudio', 'Info.plist'):
            files('rm', f'{DEST}/{f}', check=False)
        for d in ('_CodeSignature', ''):
            files('rm', f'{DEST}/{d}'.rstrip('/'), check=False)
        files('rm', VA_LINK, check=False)
        print(f'removed {DEST} and the VirtualAudio link (reboot the phone)')
        return
    with tempfile.TemporaryDirectory() as tmp:
        with tarfile.open(sys.argv[1]) as tf:
            for m in tf.getmembers():
                if m.name.startswith('/') or '..' in m.name.split('/'):
                    sys.exit(f'refusing {m.name}')
            tf.extractall(tmp)
        src = os.path.join(tmp, 'SweetAudio.driver')
        if not os.path.isfile(os.path.join(src, 'SweetAudio')):
            sys.exit('no SweetAudio.driver/SweetAudio in the archive')
        for d in ('/System/Library/Audio/Plug-Ins/HAL', DEST):
            files('mkdir', d, '755', check=False)
        for dirpath, dirnames, filenames in os.walk(src):
            rel = os.path.relpath(dirpath, src).replace('\\', '/')
            rdir = DEST if rel == '.' else f'{DEST}/{rel}'
            if rel != '.':
                files('mkdir', rdir, '755', check=False)
            for f in filenames:
                mode = '755' if f == 'SweetAudio' else '644'
                print(files('put', os.path.join(dirpath, f), f'{rdir}/{f}', mode).stdout.strip())
    r = files('link', VA_TARGET, VA_LINK, check=False)
    print(f'VirtualAudio: {VA_LINK} -> {VA_TARGET}: {r.stdout.strip() or r.stderr.strip()}')
    print(f'installed {DEST}: reboot the phone (mediaserverd loads HAL plug-ins at its start)')


if __name__ == '__main__':
    main()
