#!/usr/bin/env python3
"""Install SweetMetal2.bundle (metal2vulkan + Turnip, phone/metal2/build.sh) on the running hotdogb iOS root.

Run with Windows Python from src while a kernel runs (the flame version, tools/flame-metal2-install.sh, needs ssh):

  python tools/hotdogb-metal2-install.py [BUNDLE_OR_TGZ] [--clients NAMES]
  python tools/hotdogb-metal2-install.py --clients-only [--clients NAMES]   (just the client list)

  BUNDLE_OR_TGZ  SweetMetal2.bundle folder or the Mac's SweetMetal2.tgz (default work/metal2/SweetMetal2.bundle,
                 else phone/metal2/out/SweetMetal2.bundle)
  --clients      processes besides backboardd that get a Metal device, comma-separated, written to
                 /var/local/mtl2-clients (given: always; not given: only when the file is missing, then
                 "*,-chronod" = all but chronod, the flame's list: Safari's WebContent draws nothing without
                 Metal, chronod's widgets come out white with it; SpringBoard needs one or the unlock swipe
                 does nothing)

Writes /System/Library/Extensions/SweetMetal2.bundle, an empty /var/local/mtl2.log (mode 666: every Metal
client appends to it) and the pipeline cache folders /var/local/m2v-cache/<process> (777: clients run as mobile). backboardd loads the bundle when it starts, so after this:
`python tools/flame-cmd.py reboot` and boot again with sweetmtl=2 (hotdogb-run-current.sh has it).
Log: flame-files.py get /var/local/mtl2.log X; pipeline caches in /var/local/m2v-cache/<process>.
"""
import argparse, os, subprocess, sys, tarfile, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FILES = [sys.executable, os.path.join(ROOT, 'tools', 'flame-files.py')]
ENV = dict(os.environ, FLAME_FILES_HOST=os.environ.get('FLAME_FILES_HOST', '192.168.7.2'))
DEST = '/System/Library/Extensions/SweetMetal2.bundle'
NEED = ['Info.plist', 'SweetMetal2', 'libvulkan_freedreno.dylib', '_CodeSignature/CodeResources']
MODES = {'SweetMetal2': '755', 'libvulkan_freedreno.dylib': '755'}


def files(*args, check=True):
    r = subprocess.run(FILES + list(args), env=ENV, capture_output=True, text=True)
    if check and r.returncode:
        sys.exit(f'flame-files {" ".join(args)}: {r.stdout.strip()} {r.stderr.strip()}')
    return r


DEFAULT_CLIENTS = '*,-chronod'


def write_clients(clients, d):
    """/var/local/mtl2-clients, one name per line"""
    path = os.path.join(d, 'clients')
    with open(path, 'w', newline='\n') as f:
        f.write('\n'.join(c.strip() for c in clients.split(',') if c.strip()) + '\n')
    files('put', path, '/var/local/mtl2-clients', '644')
    print('/var/local/mtl2-clients:', clients)


def install_bundle(src):
    tmp = None
    if os.path.isfile(src):
        tmp = tempfile.mkdtemp(prefix='m2-')
        with tarfile.open(src) as t:
            t.extractall(tmp, filter='data')
        src = os.path.join(tmp, 'SweetMetal2.bundle')
    missing = [n for n in NEED if not os.path.isfile(os.path.join(src, n)) or not os.path.getsize(os.path.join(src, n))]
    if missing:
        sys.exit(f'incomplete bundle {src}: no ' + ', '.join(missing))
    files('mkdir', DEST, '755', check=False)
    files('mkdir', DEST + '/_CodeSignature', '755', check=False)
    for n in NEED:
        p = os.path.join(src, n)
        files('put', p, f'{DEST}/{n}', MODES.get(n, '644'))
        print(f'{n:30} {os.path.getsize(p):>10} -> {DEST}/{n}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('bundle', nargs='?')
    ap.add_argument('--clients')
    ap.add_argument('--clients-only', action='store_true', help='only write /var/local/mtl2-clients')
    a = ap.parse_args()
    with tempfile.TemporaryDirectory() as d:
        if a.clients_only:
            write_clients(a.clients or DEFAULT_CLIENTS, d)
            print('done: restart the clients (python tools/flame-cmd.py reboot, then boot again)')
            return
        src = a.bundle
        if not src:
            for c in ('work/metal2/SweetMetal2.bundle', 'phone/metal2/out/SweetMetal2.bundle'):
                if os.path.isdir(os.path.join(ROOT, c)):
                    src = os.path.join(ROOT, c)
                    break
            else:
                sys.exit('no bundle: pass the SweetMetal2.bundle folder or SweetMetal2.tgz')
        install_bundle(src)

        empty = os.path.join(d, 'empty')
        open(empty, 'wb').close()
        files('mkdir', '/var/local', '755', check=False)
        files('put', empty, '/var/local/mtl2.log', '666')
        print('/var/local/mtl2.log emptied (666)')
        # each process makes /var/local/m2v-cache/<name> for its pipelines, as its own user (SpringBoard, Safari,
        # ... are mobile): without a world-writable parent the device is refused ("ShaderCache ... Permission denied")
        for c in ('/var/local/m2v-cache', '/var/local/m2v-cache/backboardd', '/var/local/m2v-cache/SpringBoard'):
            files('mkdir', c, '777', check=False)
            files('chmod', c, '777')
        print('/var/local/m2v-cache (777)')
        if a.clients:
            write_clients(a.clients, d)
        elif files('get', '/var/local/mtl2-clients', os.path.join(d, 'have'), check=False).returncode == 0:
            print('/var/local/mtl2-clients kept:', open(os.path.join(d, 'have')).read().split())
        else:
            write_clients(DEFAULT_CLIENTS, d)
    print('done: python tools/flame-cmd.py reboot, then boot again with sweetmtl=2')


if __name__ == '__main__':
    main()
