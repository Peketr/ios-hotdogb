#!/usr/bin/env python3
"""Install btd (phone/bt, Bluetooth stage 1) and the WCN3998 firmware on the running hotdogb.

  python tools/hotdogb-bt-install.py            put btd + firmware, add the boot job org.iosonpixel.btd
  python tools/hotdogb-bt-install.py --no-job   files only
  python tools/hotdogb-bt-install.py --remove   remove the boot job

Firmware: ../hotdogb/fw/bluetooth/image (from the bluetooth_a partition, `flame-files.py part bluetooth_a`;
not in git) -> /var/local/fw/bt. btd: phone/bt/out/btd (built on the Mac with phone/bt/build.sh).
The job runs `btd auto` once at boot (log /var/local/btd.log; skip: /var/local/btd.off). Reboot afterwards.
"""
import os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PY = sys.executable
FILES = [PY, os.path.join(ROOT, 'tools', 'flame-files.py')]
JOB = [PY, os.path.join(ROOT, 'tools', 'hotdogb-launchd-job.py')]
ENV = dict(os.environ, FLAME_FILES_HOST=os.environ.get('FLAME_FILES_HOST', '192.168.7.2'), MSYS_NO_PATHCONV='1')
FW = os.path.join(ROOT, '..', 'hotdogb', 'fw', 'bluetooth', 'image')
FW_FILES = ['crbtfw21.tlv', 'crnv21.bin', 'crbtfw20.tlv', 'crnv20.bin']


def run(cmd, check=True):
    r = subprocess.run(cmd, env=ENV, capture_output=True, text=True)
    out = (r.stdout + r.stderr).strip()
    print(out.splitlines()[-1] if out else '(no output)')
    if check and r.returncode:
        sys.exit(f'failed: {" ".join(cmd[1:])}')


def main():
    a = sys.argv[1:]
    if a == ['--remove']:
        run(JOB + ['remove', 'btd'])
        return
    btd = os.path.join(ROOT, 'phone', 'bt', 'out', 'btd')
    if not os.path.isfile(btd):
        sys.exit(f'{btd} missing: build it on the Mac (phone/bt/build.sh) and copy phone/bt/out over')
    for d in ('/var/local/bin', '/var/local/fw', '/var/local/fw/bt'):
        run(FILES + ['mkdir', d, '755'], check=False)
    run(FILES + ['put', btd, '/var/local/bin/btd', '755'])
    for f in FW_FILES:
        p = os.path.join(FW, f)
        if not os.path.isfile(p):
            sys.exit(f'{p} missing: pull bluetooth_a first')
        run(FILES + ['put', p, f'/var/local/fw/bt/{f}', '644'])
    if '--no-job' not in a:
        run(JOB + ['add', 'btd', '--log', '/var/local/btd.log', '--', '/var/local/bin/btd', 'auto'])
    print('next: python tools/flame-cmd.py reboot, boot again; ~45 s after boot see /var/local/btd.log')


if __name__ == '__main__':
    main()
