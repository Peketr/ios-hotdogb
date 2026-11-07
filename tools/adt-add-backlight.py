#!/usr/bin/env python3
"""Give an ADT the iPhone X's /backlight node (from research/ipsw-x/d22ap.adt).

  python tools/adt-add-backlight.py IN.adt OUT.adt [--remove]

iOS's CoreBrightness (in backboardd) found kernel 0152's AppleARMBacklight service but never set a brightness on
it, and the UI fell to 0.2 fps (it dims in software instead, which takes Core Animation off the GPU). A real
iPhone describes its backlight in /device-tree/backlight: DBVMin/DBVMax, max-nit-value, the nits table
(backlight-marketing-table), use-Nits2DACTable, AAB/AAP flags ... This copies that node as it is, without the
function-* references (GPIO/PMU calls into nodes we do not have), the placeholders iBoot fills and the phandle.
--remove takes it out again. The node is Apple's data: the ADTs stay out of git (work/boot).
"""
import os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402

D22AP = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'research', 'ipsw-x', 'd22ap.adt')


def name_of(n):
    return dict((k, v) for k, f, v in n.props).get('name', b'').split(b'\0')[0].decode()


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) != 2:
        sys.exit(__doc__)
    root, _ = adt.parse(open(args[0], 'rb').read())
    root.children = [c for c in root.children if name_of(c) != 'backlight']
    if '--remove' not in sys.argv:
        src, _ = adt.parse(open(D22AP, 'rb').read())
        bl = next((c for c in src.children if name_of(c) == 'backlight'), None)
        if bl is None:
            sys.exit(f'{D22AP}: no /backlight')
        bl.props = [(k, f, v) for k, f, v in bl.props
                    if not f and not k.startswith('function-') and k != 'AAPL,phandle']
        bl.props.sort(key=lambda p: p[0] != 'name')     # name first, as every other node
        bl.children = []
        root.children.append(bl)
        print(f'/backlight: {len(bl.props)} properties:', ', '.join(k for k, f, v in bl.props))
    else:
        print('/backlight removed')
    open(args[1], 'wb').write(adt.serialize(root))


if __name__ == '__main__':
    main()
