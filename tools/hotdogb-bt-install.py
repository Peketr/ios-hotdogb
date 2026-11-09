#!/usr/bin/env python3
"""Install btd (phone/bt) and the WCN3998 firmware on the running hotdogb.

  python tools/hotdogb-bt-install.py            put btd + firmware, boot job org.iosonpixel.btd = `btd serve`
  python tools/hotdogb-bt-install.py --scan     same, but the stage 1 job `btd auto` (one scan, power off)
  python tools/hotdogb-bt-install.py --no-job   files only
  python tools/hotdogb-bt-install.py --remove   remove the boot job

Firmware: ../hotdogb/fw/bluetooth/image (from the bluetooth_a partition, `flame-files.py part bluetooth_a`;
not in git) -> /var/local/fw/bt. btd: phone/bt/out/btd (built on the Mac with phone/bt/build.sh).
`btd serve` (stage 2, KeepAlive): brings the controller up ~20 s after boot and serves bluetoothd through the
kernel bridge "com.apple.uart.bluetooth" (kernel 0165); bluetoothd's job gets
BT_HCI_TRANSPORT=H4:POSIX:com.apple.uart.bluetooth,3000000 so it opens that instead of Apple's Skywalk channel.
Needs an ADT with /arm-io/bluetooth (tools/adt-add-bluetooth.py).
bluetoothd waits for BlueTool's boot script: bluetool-shim.dylib (DYLD_INSERT_LIBRARIES in com.apple.BlueTool)
answers ReleaseType = Vendor, so BlueTool runs /etc/bluetool/D22AP.{boot,init}.script (phone/bt/bluetool).
btd goes to /var/local/bin/btd2: never replace the binary of the KeepAlive job in the boot that runs it. Log /var/local/btd.log; idle: /var/local/btd.off.
The job is replaced on every run of this script. Reboot afterwards.
"""
import os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PY = sys.executable
FILES = [PY, os.path.join(ROOT, 'tools', 'flame-files.py')]
JOB = [PY, os.path.join(ROOT, 'tools', 'hotdogb-launchd-job.py')]
ENV = dict(os.environ, FLAME_FILES_HOST=os.environ.get('FLAME_FILES_HOST', '192.168.7.2'), MSYS_NO_PATHCONV='1')
FW = os.path.join(ROOT, '..', 'hotdogb', 'fw', 'bluetooth', 'image')
BTD = '/var/local/bin/btd2'
SHIM = '/var/local/lib/bluetool-shim.dylib'
TRANSPORT = 'H4:POSIX:com.apple.uart.bluetooth,3000000'
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
    import filecmp, tempfile
    cur = os.path.join(tempfile.mkdtemp(), 'btd')
    same = subprocess.run(FILES + ['get', BTD, cur], env=ENV, capture_output=True).returncode == 0 and         filecmp.cmp(btd, cur, shallow=False)
    if same:
        print(f'{BTD} unchanged, not replaced')
    else:
        print(f'replacing {BTD}: if its job runs in this boot, reboot right after this script (no restart)')
        run(FILES + ['put', btd, BTD, '755'])
    shim = os.path.join(ROOT, 'phone', 'bt', 'out', 'bluetool-shim.dylib')
    run(FILES + ['mkdir', '/var/local/lib', '755'], check=False)
    run(FILES + ['put', shim, SHIM, '755'])
    run(FILES + ['mkdir', '/private/etc/bluetool', '755'], check=False)
    for k in ('boot', 'init'):
        run(FILES + ['put', os.path.join(ROOT, 'phone', 'bt', 'bluetool', f'D22AP.{k}.script'),
                     f'/private/etc/bluetool/D22AP.{k}.script', '644'])
    for f in FW_FILES:
        p = os.path.join(FW, f)
        if not os.path.isfile(p):
            sys.exit(f'{p} missing: pull bluetooth_a first')
        run(FILES + ['put', p, f'/var/local/fw/bt/{f}', '644'])
    if '--no-job' not in a:
        run(JOB + ['remove', 'btd'], check=False)
        if '--scan' in a:
            run(JOB + ['add', 'btd', '--log', '/var/local/btd.log', '--', BTD, 'auto'])
        else:
            run(JOB + ['add', 'btd', '--log', '/var/local/btd.log', '--keep', '--', BTD, 'serve'])
    if '--no-job' not in a and '--scan' not in a:
        run(JOB + ['env', 'com.apple.bluetoothd', 'BT_HCI_TRANSPORT=' + TRANSPORT])
        run(JOB + ['env', 'com.apple.BlueTool', 'DYLD_INSERT_LIBRARIES=' + SHIM])
    print('next: python tools/flame-cmd.py reboot, boot again; see /var/local/btd.log')


if __name__ == '__main__':
    main()
