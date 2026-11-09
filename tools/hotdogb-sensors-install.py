#!/usr/bin/env python3
"""Install the motion sensors (sensord + SLPI firmware + sensor configs + its launchd boot job) on the running hotdogb.

Run with Windows Python from src while a kernel runs (192.168.7.1 on the phone's USB adapter):

  python tools/hotdogb-sensors-install.py [--sensord FILE] [--no-job] [--hw-platform MTP] [--soc-id 339]

  --sensord  phone/sensors/out/sensord, built on the Mac with phone/sensors/build.sh (default that path)
  --no-job   leave launchd alone (start `sensord up 0` by hand instead)

What goes where (none of it in git; nothing outside the phone's iOS root is written):
  ../hotdogb/fw/modem-image/image/slpi.mdt + slpi.bNN  -> /var/local/fw/slpi/
  ../hotdogb/sensors/etc/config/*.json, sns_reg_config  -> /var/local/sensors/etc/   (the SLPI's
      /vendor/etc/sensors; extracted from the 7T's vendor_a.img in WSL:
      cd ../hotdogb/sensors && debugfs -R 'rdump /etc/sensors .' ../fw/vendor_a.img && mv sensors etc)
  socinfo values (Android's /sys/devices/soc0, which sns_reg_config names) -> /var/local/sensors/socinfo/
  an empty /var/local/sensors/persist/registry/registry/ (the SLPI writes its registry there on first start)
The job runs `sensord auto` once per phone boot (45 s in), log /var/local/sensord.log; samples in
/var/local/imu.txt. launchd reads its job cache at boot: `python tools/flame-cmd.py reboot`, then boot again.
Turn it off without removing it: put an empty /var/local/sensord.off.
"""
import argparse, os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOP = os.path.dirname(ROOT)
FILES = [sys.executable, os.path.join(ROOT, 'tools', 'flame-files.py')]
JOBS = [sys.executable, os.path.join(ROOT, 'tools', 'hotdogb-launchd-job.py')]
ENV = dict(os.environ, FLAME_FILES_HOST=os.environ.get('FLAME_FILES_HOST', '192.168.7.2'), MSYS_NO_PATHCONV='1')
FW = os.path.join(TOP, 'hotdogb', 'fw', 'modem-image', 'image')
ETC = os.path.join(TOP, 'hotdogb', 'sensors', 'etc')
R = '/var/local/sensors'


def files(*args, check=True):
    r = subprocess.run(FILES + list(args), env=ENV, capture_output=True, text=True)
    if check and r.returncode:
        sys.exit(f'flame-files {" ".join(args)}: {r.stdout.strip()} {r.stderr.strip()}')
    return r


def put(local, remote, mode='644'):
    print(files('put', local, remote, mode).stdout.strip())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--sensord', default=os.path.join(ROOT, 'phone', 'sensors', 'out', 'sensord'))
    ap.add_argument('--no-job', action='store_true')
    ap.add_argument('--hw-platform', default='MTP')
    ap.add_argument('--soc-id', default='339')
    a = ap.parse_args()

    if not os.path.isfile(a.sensord):
        sys.exit(f'no {a.sensord}: build it on the Mac (phone/sensors/build.sh)')
    fw = sorted(f for f in os.listdir(FW) if f.startswith('slpi.') and (f == 'slpi.mdt' or f[5:6] == 'b'))
    if 'slpi.mdt' not in fw:
        sys.exit(f'no slpi.mdt in {FW} (tools/hotdogb-fw-pull.py)')
    cfg = os.path.join(ETC, 'config')
    if not os.path.isdir(cfg) or not os.path.isfile(os.path.join(ETC, 'sns_reg_config')):
        sys.exit(f'no sensor configs in {ETC}: see the extraction step at the top of this script')

    for d in ('/var/local/bin', '/var/local/fw', '/var/local/fw/slpi', R, f'{R}/etc', f'{R}/etc/config', f'{R}/socinfo',
              f'{R}/persist', f'{R}/persist/registry', f'{R}/persist/registry/registry', f'{R}/dsp', f'{R}/acdb',
              f'{R}/other'):
        files('mkdir', d, '755', check=False)
    put(a.sensord, '/var/local/bin/sensord', '755')
    for f in fw:
        put(os.path.join(FW, f), f'/var/local/fw/slpi/{f}')
    put(os.path.join(ETC, 'sns_reg_config'), f'{R}/etc/sns_reg_config')
    if os.path.isfile(os.path.join(ETC, 'hals.conf')):
        put(os.path.join(ETC, 'hals.conf'), f'{R}/etc/hals.conf')
    for f in sorted(os.listdir(cfg)):
        if f.endswith('.json'):
            put(os.path.join(cfg, f), f'{R}/etc/config/{f}')
    # Android's /sys/devices/soc0 for SM8150 (msmnile): the configs select on soc_id 339 + hw_platform
    soc = {'soc_id': a.soc_id, 'hw_platform': a.hw_platform, 'platform_subtype': 'Unknown', 'platform_subtype_id': '0',
           'platform_version': '65536', 'revision': '2.0'}
    with tempfile.TemporaryDirectory() as tmp:
        for k, v in soc.items():
            p = os.path.join(tmp, k)
            with open(p, 'w', newline='\n') as fh:
                fh.write(v + '\n')
            put(p, f'{R}/socinfo/{k}')
    print(f'installed sensord, {len(fw)} SLPI firmware files, the sensor configs and socinfo ({a.hw_platform}, soc {a.soc_id})')
    if a.no_job:
        print('no launchd job: start it by hand with `/var/local/bin/sensord up 0`')
        return
    r = subprocess.run(JOBS + ['add', 'sensord', '--', '/var/local/bin/sensord', 'auto'], env=ENV, capture_output=True, text=True)
    print(r.stdout.strip() or r.stderr.strip())
    if r.returncode:
        sys.exit(r.returncode)
    print('boot job org.iosonpixel.sensord added: reboot the phone (python tools/flame-cmd.py reboot), then boot again')


if __name__ == '__main__':
    main()
