#!/usr/bin/env python3
"""Install iOS's 32 removable factory apps (Weather, Notes, Calendar, Mail, Maps, Music, ...) on the running
hotdogb iOS root.

Run with Windows Python from src while a kernel runs:

  python tools/hotdogb-sysapps-install.py            put sysapps + lsreg, add the one-shot boot job
  python tools/hotdogb-sysapps-install.py --remove   remove the boot job again (the apps stay)

They are already on the phone, in /private/var/staged_system_apps (the IPSW's own copies), but installd cannot
install them here. phone/tools/sysapps (built on the Mac with phone/tools/build.sh) does it instead, as a
launchd job at the next boot: renames each app into /Applications, moves its placeholder from
/System/Library/AppPlaceholders to /var/local/sysapps-placeholders, then runs `lsreg -r`. Log:
/var/local/sysapps.log. After that boot the job finds nothing staged and does nothing. Undo on the phone:
`sysapps -u` (as a job: hotdogb-launchd-job.py add sysapps-undo -- /var/local/bin/sysapps -u).
Nothing here writes a partition: only files on the iOS root, through the kernel file server.
"""
import os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PY = sys.executable
FILES = [PY, os.path.join(ROOT, 'tools', 'flame-files.py')]
JOB = [PY, os.path.join(ROOT, 'tools', 'hotdogb-launchd-job.py')]
ENV = dict(os.environ, FLAME_FILES_HOST=os.environ.get('FLAME_FILES_HOST', '192.168.7.2'))
OUT = os.path.join(ROOT, 'phone', 'tools', 'out')


def run(cmd):
    r = subprocess.run(cmd, env=ENV, capture_output=True, text=True)
    out = (r.stdout + r.stderr).strip()
    print(out.splitlines()[-1] if out else '(no output)')
    if r.returncode:
        sys.exit(f'failed: {" ".join(cmd[1:])}')


def main():
    if sys.argv[1:] == ['--remove']:
        run(JOB + ['remove', 'sysapps'])
        return
    for name in ('sysapps', 'lsreg'):
        p = os.path.join(OUT, name)
        if not os.path.isfile(p):
            sys.exit(f'{p} missing: build it on the Mac (phone/tools/build.sh) and copy phone/tools/out over')
        run(FILES + ['put', p, f'/var/local/bin/{name}', '755'])
    run(JOB + ['add', 'sysapps', '--log', '/var/local/sysapps.log', '--', '/var/local/bin/sysapps'])
    print('next: python tools/flame-cmd.py reboot, boot again; the apps move in at that boot '
          '(/var/local/sysapps.log), then reboot once more so SpringBoard lays out the home screen with them')


if __name__ == '__main__':
    main()
