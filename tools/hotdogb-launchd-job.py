#!/usr/bin/env python3
"""Add or remove an org.iosonpixel.* launchd boot job on the running hotdogb iOS root (no shell needed).

Run with Windows Python from src while a kernel runs (192.168.7.1 on the phone's USB adapter):

  python tools/hotdogb-launchd-job.py add NAME [--log FILE] [--keep] -- PROGRAM [ARGS...]
  python tools/hotdogb-launchd-job.py remove NAME
  python tools/hotdogb-launchd-job.py list

NAME becomes the label org.iosonpixel.NAME; the job runs once at boot as root (RunAtLoad; --keep: launchd
restarts it when it exits, at most every 10 s), stdout + stderr to FILE (default /var/local/NAME.log). Edits /System/Library/xpc/launchd.plist through the
kernel file server (tools/flame-files.py); the first edit keeps the original as launchd.plist.pre-jobs on the
phone. launchd reads its job cache at boot: `python tools/flame-cmd.py reboot`, then boot again.
Example: python tools/hotdogb-launchd-job.py add oslog-wifid --keep -- /var/local/bin/oslog -l wifid
"""
import os, plistlib, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FILES = [sys.executable, os.path.join(ROOT, 'tools', 'flame-files.py')]
ENV = dict(os.environ, FLAME_FILES_HOST=os.environ.get('FLAME_FILES_HOST', '192.168.7.2'))
CACHE = '/System/Library/xpc/launchd.plist'
PREFIX = 'org.iosonpixel.'


def files(*args, check=True):
    r = subprocess.run(FILES + list(args), env=ENV, capture_output=True, text=True)
    if check and r.returncode:
        sys.exit(f'flame-files {" ".join(args)}: {r.stdout.strip()} {r.stderr.strip()}')
    return r


def key(name):
    return f'/System/Library/LaunchDaemons/{PREFIX}{name}.plist'


def main():
    a = sys.argv[1:]
    if not a or a[0] not in ('add', 'remove', 'list') or (a[0] != 'list' and len(a) < 2):
        sys.exit(__doc__)
    tmp = tempfile.mkdtemp()
    cur = os.path.join(tmp, 'launchd.plist')
    files('get', CACHE, cur)
    with open(cur, 'rb') as f:
        p = plistlib.load(f)
    jobs = p['LaunchDaemons']
    if a[0] == 'list':
        for k, j in sorted(jobs.items()):
            if j.get('Label', '').startswith(PREFIX):
                print(j['Label'], j.get('ProgramArguments'), '->', j.get('StandardOutPath'))
        return
    name = a[1]
    if a[0] == 'remove':
        if jobs.pop(key(name), None) is None:
            print(f'no job {PREFIX}{name}')
            return
    else:
        rest = a[2:]
        log, keep = f'/var/local/{name}.log', False
        while rest and rest[0] in ('--log', '--keep'):
            if rest[0] == '--keep':
                keep, rest = True, rest[1:]
            elif len(rest) >= 2:
                log, rest = rest[1], rest[2:]
            else:
                sys.exit(__doc__)
        if not rest or rest[0] != '--' or len(rest) < 2:
            sys.exit(__doc__)
        jobs[key(name)] = {
            'Label': PREFIX + name,
            'ProgramArguments': rest[1:],
            'RunAtLoad': True, 'KeepAlive': keep, 'UserName': 'root',
            **({'ThrottleInterval': 10} if keep else {}),
            'EnvironmentVariables': {'PATH': '/var/local/bin:/usr/bin:/bin:/usr/sbin:/sbin'},
            'StandardOutPath': log, 'StandardErrorPath': log,
        }
    if files('get', CACHE + '.pre-jobs', os.devnull, check=False).returncode:
        files('put', cur, CACHE + '.pre-jobs', '644')
        print('kept the job cache as launchd.plist.pre-jobs on the phone')
    new = os.path.join(tmp, 'launchd.new')
    with open(new, 'wb') as f:
        plistlib.dump(p, f, fmt=plistlib.FMT_BINARY)
    files('put', new, CACHE, '644')
    print(f'{a[0]} {PREFIX}{name}: {len(jobs)} jobs; reboot (python tools/flame-cmd.py reboot) and boot again')


if __name__ == '__main__':
    main()
