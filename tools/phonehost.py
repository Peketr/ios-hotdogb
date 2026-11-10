#!/usr/bin/env python3
"""Which address reaches the running phone: USB Ethernet first, else its Wi-Fi address.

The kernel's file server (tcp 2223) and command server (udp 5141) listen on every interface, and wland
hands Wi-Fi packets for the phone's DHCP lease to iOS, so with the USB cable out the tools still reach the
phone over Wi-Fi when the PC is on the same network. Used by flame-files.py and flame-cmd.py.

FLAME_FILES_HOST picks the address:
  unset / 192.168.7.2 / auto   USB (192.168.7.2) if it answers, else the phone's Wi-Fi address
                               (unset on a Mac stays 127.0.0.1, FlameBridge)
  anything else                exactly that address, no fallback
FLAME_PHONE_WIFI=IP            the phone's Wi-Fi address, if you know it (skips the search)
The Wi-Fi address found is kept in logs/phone-wifi-ip; when it stops answering the PC's /24 networks are
searched again for the file server (about 2 s per network).

  python tools/phonehost.py          prints the address that works now (searches if needed)
"""
import concurrent.futures, os, socket, sys

USB = '192.168.7.2'
PORT = 2223
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE = os.path.join(ROOT, 'logs', 'phone-wifi-ip')


def probe(ip, timeout):
    """True if ip:2223 is the phone's file server (an unknown request gets "ERR 22")."""
    try:
        with socket.create_connection((ip, PORT), timeout=timeout) as s:
            s.settimeout(timeout + 1)
            s.sendall(b'PING\n')
            line = b''
            while not line.endswith(b'\n') and len(line) < 32:
                c = s.recv(1)
                if not c:
                    break
                line += c
            return line.startswith(b'ERR ')
    except OSError:
        return False


def local_nets():
    ips = set()
    try:
        ips.update(a[4][0] for a in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET))
    except OSError:
        pass
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as u:
            u.connect(('8.8.8.8', 53))		# no packet sent: just the default route's address
            ips.add(u.getsockname()[0])
    except OSError:
        pass
    nets = []
    for ip in sorted(ips):
        if ip.startswith(('127.', '169.254.', '192.168.7.')):
            continue
        net = ip.rsplit('.', 1)[0]
        if net not in nets:
            nets.append(net)
    return nets


def search(log):
    for net in local_nets():
        log(f'searching {net}.0/24 for the phone over Wi-Fi...')
        ips = [f'{net}.{i}' for i in range(1, 255)]
        with concurrent.futures.ThreadPoolExecutor(128) as ex:
            for ip, ok in zip(ips, ex.map(lambda ip: probe(ip, 0.8), ips)):
                if ok:
                    return ip
    return None


def remember(ip):
    try:
        os.makedirs(os.path.dirname(CACHE), exist_ok=True)
        with open(CACHE, 'w') as f:
            f.write(ip + '\n')
    except OSError:
        pass


def resolve(quiet=False):
    log = (lambda m: None) if quiet else (lambda m: print(f'phonehost: {m}', file=sys.stderr, flush=True))
    want = os.environ.get('FLAME_FILES_HOST') or ('127.0.0.1' if sys.platform == 'darwin' else 'auto')
    if want not in (USB, 'auto'):
        return want
    if probe(USB, 0.7):
        return USB
    tries = [os.environ.get('FLAME_PHONE_WIFI', '').strip()]
    try:
        tries.append(open(CACHE).read().strip())
    except OSError:
        pass
    for ip in dict.fromkeys(t for t in tries if t):
        if probe(ip, 2):
            log(f'USB link down: using the phone over Wi-Fi at {ip}')
            remember(ip)
            return ip
    ip = search(log)
    if ip:
        log(f'USB link down: found the phone over Wi-Fi at {ip}')
        remember(ip)
        return ip
    log('phone not reachable over USB (192.168.7.2) or Wi-Fi (is wland up and the PC on the same network?)')
    return USB


if __name__ == '__main__':
    print(resolve())
