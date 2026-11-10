#!/usr/bin/env python3
"""Put the Wi-Fi network wland joins by itself at boot (/var/local/wlan.conf) on the running hotdogb.

  python tools/hotdogb-wlan-conf.py            asks for the network name and its password (not echoed)
  python tools/hotdogb-wlan-conf.py --remove   deletes it again

Why: wifid knows the network but refuses to auto-join it ("Below minimum RSSI threshold", HANDOFF 2026-11-07:
our scan results carry noise = snr = 0), so after a reboot the phone stays off Wi-Fi until you pick it in
Settings. wland reads wlan.conf when it starts (~20 s into boot), joins after its first scan and rejoins on a
lost link; iOS then runs DHCP on en0 as usual. Settings may label the network "Unsecured" (cosmetic).
The password goes only to the phone (mode 600): it is written to a temporary file that is deleted at once,
never to the repo. Takes effect at the next phone boot.
"""
import getpass, os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FILES = [sys.executable, os.path.join(ROOT, 'tools', 'flame-files.py')]
ENV = dict(os.environ, FLAME_FILES_HOST=os.environ.get('FLAME_FILES_HOST', '192.168.7.2'), MSYS_NO_PATHCONV='1')
REMOTE = '/var/local/wlan.conf'


def main():
    if sys.argv[1:] == ['--remove']:
        sys.exit(subprocess.run(FILES + ['rm', REMOTE], env=ENV).returncode)
    if sys.argv[1:]:
        sys.exit(__doc__)
    ssid = input('Wi-Fi network name (SSID): ').strip()
    if not ssid or len(ssid.encode()) > 32:
        sys.exit('the network name must be 1-32 bytes')
    psk = getpass.getpass('password (empty = open network): ')
    if psk and not 8 <= len(psk) <= 63:
        sys.exit('a WPA2 password is 8-63 characters')
    fd, tmp = tempfile.mkstemp()
    try:
        with os.fdopen(fd, 'w', newline='\n') as f:
            f.write(f'ssid={ssid}\n' + (f'psk={psk}\n' if psk else ''))
        r = subprocess.run(FILES + ['put', tmp, REMOTE, '600'], env=ENV, capture_output=True, text=True)
    finally:
        os.remove(tmp)
    if r.returncode:
        sys.exit(f'flame-files put: {r.stdout.strip()} {r.stderr.strip()}')
    print(f'{REMOTE} on the phone: wland joins "{ssid}" at the next boot (wland.log: "net: will join")')


if __name__ == '__main__':
    main()
