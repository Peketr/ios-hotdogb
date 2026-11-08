#!/usr/bin/env python3
"""Give an ADT an /arm-io/bluetooth node for iOS 16's bluetoothd (hotdogb, Bluetooth stage 2).

  python tools/adt-add-bluetooth.py IN.adt OUT.adt [--remove]

bluetoothd exits at start unless an IOService named "bluetooth" (or marconi-bt) exists, and takes its HCI
transport from that node's IODeviceTree properties (usr/sbin/bluetoothd 0x1000e7684 / 0x100156280):
transport-encoding 0 USB, 1 H4, 2 H4DS, 3 H4BC (the iPhone X), 4 H5, 6 APPLEBT, 7 PCIe; transport-speed.
SweetPlatformExpert publishes /arm-io children as nubs named after the node, so the node is enough.
H4 on iOS 16 is the "com.apple.uart.sk.bluetooth" kernel control (kernel 0163, bsd/dev/sweet_btuart.c) and a
Skywalk user pipe that btd (phone/bt) serves.

The other properties follow the iPhone X's uart1/bluetooth node (research/ipsw-x/d22ap.adt), without its
calibration placeholders (syscfg), function-* GPIO references and phandle; local-mac-address is the ADT's
own locally administered mac-address-bluetooth0. --remove takes the node out again.
"""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402


def name_of(n):
    return dict((k, v) for k, f, v in n.props).get('name', b'').split(b'\0')[0].decode()


def u32(v):
    return struct.pack('<I', v)


def cstr(s):
    return s.encode() + b'\0'


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) != 2:
        sys.exit(__doc__)
    root, _ = adt.parse(open(args[0], 'rb').read())
    armio = next((c for c in root.children if name_of(c) == 'arm-io'), None)
    if armio is None:
        sys.exit('no /arm-io')
    armio.children = [c for c in armio.children if name_of(c) != 'bluetooth']
    if '--remove' in sys.argv:
        print('/arm-io/bluetooth removed')
    else:
        mac = root.prop('mac-address-bluetooth0') or bytes([0x02, 0x50, 0x49, 0x58, 0x45, 0x4d])
        n = adt.Node()
        n.props = [
            ('name', 0, cstr('bluetooth')),
            ('compatible', 0, cstr('bluetooth,n88')),
            ('device_type', 0, cstr('bluetooth')),
            ('transport-encoding', 0, u32(1)),		# H4
            ('transport-speed', 0, u32(3000000)),
            ('local-mac-address', 0, bytes(mac[:6])),
            ('vendor-id', 0, u32(0x5ac)),
            ('product-id', 0, u32(0x12a0)),
            ('supported-profiles', 0, u32(0x2ffb)),
            ('coex', 0, u32(2)),
        ]
        armio.children.append(n)
        print('/arm-io/bluetooth:', ', '.join(k for k, f, v in n.props))
    open(args[1], 'wb').write(adt.serialize(root))


if __name__ == '__main__':
    main()
