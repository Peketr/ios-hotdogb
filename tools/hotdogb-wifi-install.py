#!/usr/bin/env python3
"""Install Wi-Fi (wland + modem/WLAN firmware + its launchd boot job) on the running hotdogb iOS root.

Run with Windows Python from src while a kernel runs (192.168.7.1 on the phone's USB adapter):

  python tools/hotdogb-wifi-install.py [--fw DIR] [--wland FILE] [--no-job]

  --fw     firmware folder from tools/hotdogb-fw-pull.sh (default ../hotdogb/fw):
           modem/  modem.mdt + modem.bNN, wlanmdsp.mbn, *.jsn, bdwlan.bin   -> /var/local/fw/modem
           rmtfs/  modemst1.img modemst2.img fsg.img fsc.img (IMEI etc.)    -> /var/local/fw/rmtfs
  --wland  phone/wifi/out/wland built on the Mac with BOARD=hotdogb phone/wifi/build.sh
  --no-job leave launchd alone (start `wland up 0` by hand instead)

Uses the kernel's file server (tools/flame-files.py, FLAME_FILES_HOST=192.168.7.2). launchd reads its job
cache at boot, so after this: `python tools/flame-cmd.py reboot`, then boot again. The job runs `wland auto`
once per phone boot, logging to /var/local/wland.log (`flame-files.py get /var/local/wland.log X`).
Joining a network: /var/local/wlan.conf (`ssid=NAME`, `psk=PASS` lines) - write it yourself and put it
with flame-files.py; it lives on the phone only, never in the repo.
Nothing here leaves the phone's iOS root: no partitions are written.
"""
import argparse, os, plistlib, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FILES = [sys.executable, os.path.join(ROOT, 'tools', 'flame-files.py')]
ENV = dict(os.environ, FLAME_FILES_HOST=os.environ.get('FLAME_FILES_HOST', '192.168.7.2'))
JOB_KEY = '/System/Library/LaunchDaemons/org.iosonpixel.wland.plist'
JOB = {
    'Label': 'org.iosonpixel.wland',
    'ProgramArguments': ['/var/local/bin/wland', 'auto'],
    'RunAtLoad': True, 'KeepAlive': False, 'UserName': 'root',
    'EnvironmentVariables': {'PATH': '/var/local/bin:/usr/bin:/bin:/usr/sbin:/sbin'},
}


def files(*args, check=True):
    r = subprocess.run(FILES + list(args), env=ENV, capture_output=True, text=True)
    if check and r.returncode:
        sys.exit(f'flame-files {" ".join(args)}: {r.stdout.strip()} {r.stderr.strip()}')
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--fw', default=os.path.join(ROOT, '..', 'hotdogb', 'fw'))
    ap.add_argument('--wland', default=os.path.join(ROOT, 'phone', 'wifi', 'out', 'wland'))
    ap.add_argument('--no-job', action='store_true')
    a = ap.parse_args()
    modem, rmtfs = os.path.join(a.fw, 'modem'), os.path.join(a.fw, 'rmtfs')
    need = ['modem.mdt', 'wlanmdsp.mbn', 'bdwlan.bin']
    missing = [n for n in need if not os.path.isfile(os.path.join(modem, n))]
    missing += [p for p in (a.wland,) if not os.path.isfile(p)]
    if missing:
        sys.exit('missing: ' + ', '.join(missing))

    for d in ('/var/local', '/var/local/bin', '/var/local/fw', '/var/local/fw/modem', '/var/local/fw/rmtfs',
              '/var/local/fw/rw'):
        files('mkdir', d, '755', check=False)
    files('put', a.wland, '/var/local/bin/wland', '755')
    print('wland -> /var/local/bin/wland')
    n = 0
    for name in sorted(os.listdir(modem)):
        p = os.path.join(modem, name)
        if os.path.isfile(p):
            files('put', p, '/var/local/fw/modem/' + name, '644')
            n += 1
    print(f'{n} firmware files -> /var/local/fw/modem')
    if os.path.isdir(rmtfs):
        for name in sorted(os.listdir(rmtfs)):
            if name.endswith('.img'):
                files('put', os.path.join(rmtfs, name), '/var/local/fw/rmtfs/' + name, '600')
                print(f'rmtfs {name}')
    else:
        print('no rmtfs folder: the modem err-fatals ~40 s after boot without it')

    if a.no_job:
        return
    cur = os.path.join(a.fw, 'launchd.plist.cur')
    files('get', '/System/Library/xpc/launchd.plist', cur)
    with open(cur, 'rb') as f:
        p = plistlib.load(f)
    if JOB_KEY in p['LaunchDaemons'] and p['LaunchDaemons'][JOB_KEY] == JOB:
        print('launchd job already installed')
        return
    if files('get', '/System/Library/xpc/launchd.plist.pre-wland', os.devnull, check=False).returncode:
        files('put', cur, '/System/Library/xpc/launchd.plist.pre-wland', '644')
        print('kept the original job cache as launchd.plist.pre-wland on the phone')
    p['LaunchDaemons'][JOB_KEY] = JOB
    new = os.path.join(a.fw, 'launchd.plist.new')
    with open(new, 'wb') as f:
        plistlib.dump(p, f, fmt=plistlib.FMT_BINARY)
    files('put', new, '/System/Library/xpc/launchd.plist', '644')
    print(f'launchd job org.iosonpixel.wland added ({len(p["LaunchDaemons"])} jobs); '
          'reboot (python tools/flame-cmd.py reboot) and boot again')


if __name__ == '__main__':
    main()
