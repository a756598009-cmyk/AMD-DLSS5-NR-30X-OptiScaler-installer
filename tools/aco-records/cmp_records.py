"""Compare record headers: cmp_records.py <dir A> <dir B> (records named by key, as in records/)."""
import os, struct, sys


def head(p):
    b = open(p, 'rb').read()
    n = struct.unpack_from('<I', b, 68)[0]
    f = dict(ver=b[:8].decode(), rsrc1=hex(struct.unpack_from('<I', b, 24)[0]),
             rsrc2=hex(struct.unpack_from('<I', b, 28)[0]), rsrc3=hex(struct.unpack_from('<I', b, 32)[0]))
    for k, o in (('exec', 36), ('wg', 40), ('lds', 44), ('push_bytes', 48), ('user', 52)):
        f[k] = struct.unpack_from('<I', b, o)[0]
    for k, o in (('set0', 56), ('push', 60), ('grid', 64)):
        f[k] = struct.unpack_from('<i', b, o)[0]
    f['mach'], f['scratch'] = struct.unpack_from('<2I', b, 76)
    f['inlines'] = [struct.unpack_from('<2i', b, 96 + 8 * i) for i in range(n)]
    return f


a, b = sys.argv[1], sys.argv[2]
for f in sorted(os.listdir(a)):
    if not f.endswith('.nrp'):
        continue
    if not os.path.exists(os.path.join(b, f)):
        print(f, 'only in A'); continue
    x, y = head(os.path.join(a, f)), head(os.path.join(b, f))
    diff = {k: (x[k], y[k]) for k in x if x[k] != y[k]}
    print(f, 'same header' if not diff else diff)
