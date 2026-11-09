#!/usr/bin/env python3
"""List or compare the kalloc_type views of XNU kernels.

    kalloc-type-sigs.py KERNEL              print "name signature flags size" lines
    kalloc-type-sigs.py MAC_KERNEL KERNEL   compare: exit 1 on any difference

Every kalloc_type_view (__DATA_CONST,__kalloc_type) carries the type signature
and the data-only / pointer-array flags that Apple clang's XNU builtins produced.
The patched Linux clang (clang-xnu-builtins.py) must give the same values, or
types land in other heaps than in a Mac-built kernel.
"""
import collections, struct, sys


def views(path):
    d = open(path, 'rb').read()
    if struct.unpack_from('<I', d)[0] != 0xfeedfacf:
        sys.exit(f'{path}: not a 64-bit Mach-O')
    ncmds = struct.unpack_from('<I', d, 16)[0]
    off, segs, secs = 32, [], {}
    for _ in range(ncmds):
        cmd, sz = struct.unpack_from('<II', d, off)
        if cmd == 0x19:  # LC_SEGMENT_64
            seg = d[off + 8:off + 24].rstrip(b'\0').decode()
            vm, _vms, fo, fs = struct.unpack_from('<QQQQ', d, off + 24)
            segs.append((vm, fo, fs))
            for i in range(struct.unpack_from('<I', d, off + 64)[0]):
                so = off + 72 + 80 * i
                name = d[so:so + 16].rstrip(b'\0').decode()
                addr, size, fo2 = struct.unpack_from('<QQI', d, so + 32)
                secs[(seg, name)] = (addr, size, fo2)
        off += sz

    def cstr(a):
        for vm, fo, fs in segs:
            if vm <= a < vm + fs:
                o = fo + a - vm
                return d[o:d.index(b'\0', o)].decode(errors='replace')
        return f'?{a:x}'

    if ('__DATA_CONST', '__kalloc_type') not in secs:
        sys.exit(f'{path}: no __DATA_CONST,__kalloc_type')
    _addr, size, fo = secs[('__DATA_CONST', '__kalloc_type')]
    # struct kalloc_type_view (64 bytes): zone_view {zone, stats, name, next},
    # kt_signature, kt_flags (u32), kt_size (u32), padding
    out = []
    for o in range(fo, fo + size, 64):
        _z, _s, name, _n, sig = struct.unpack_from('<QQQQQ', d, o)
        flags, ksize = struct.unpack_from('<II', d, o + 40)
        out.append((cstr(name), cstr(sig), flags, ksize))
    return out


def main():
    if len(sys.argv) == 2:
        for n, s, f, z in views(sys.argv[1]):
            print(n, repr(s), hex(f), z)
        return
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    a = collections.Counter(views(sys.argv[1]))
    b = collections.Counter(views(sys.argv[2]))
    only_a, only_b = a - b, b - a
    print(f'{sum(a.values())} views in {sys.argv[1]}, {sum(b.values())} in {sys.argv[2]}')
    for k in sorted(only_a):
        print('  only first: ', k[0], repr(k[1]), hex(k[2]), k[3])
    for k in sorted(only_b):
        print('  only second:', k[0], repr(k[1]), hex(k[2]), k[3])
    if only_a or only_b:
        print(f'{sum(only_a.values())} / {sum(only_b.values())} views differ')
        sys.exit(1)
    print('identical signatures, flags and sizes')


main()
