"""Offline look at a driver pipeline binary saved by nr_aco_probe2 (blobs/<shader>.0.bin).

    analyse_blob.py <blob> [--ref <other blob>] [--dis] [--json out.json]

Prints the header, sections, notes, symbols and PAL metadata of every ELF inside the blob (also inside a zlib/gzip/
lzma/zstd/lz4 stream when it is one), with --ref the differences against another blob (e.g. the RX 9070 XT
reference in files/ref-gfx12), with --dis the .text disassembled by the HIP SDK's llvm-mc for the e_flags machine.
Run with python -I; the blob is data only.
"""
import json, os, struct, subprocess, sys, zlib, lzma

MC = os.path.join(os.environ.get('HIP_PATH', r'C:\Program Files\AMD\ROCm\7.2'), 'bin', 'llvm-mc.exe')
MACH = {0x36: 'gfx1030', 0x37: 'gfx1031', 0x38: 'gfx1032', 0x39: 'gfx1033', 0x3e: 'gfx1034', 0x3d: 'gfx1035',
        0x45: 'gfx1036', 0x41: 'gfx1100', 0x46: 'gfx1101', 0x47: 'gfx1102', 0x44: 'gfx1103', 0x48: 'gfx1200',
        0x4e: 'gfx1201', 0x33: 'gfx1010', 0x34: 'gfx1011', 0x35: 'gfx1012'}


def unpack(b, p=0):
    c = b[p]; p += 1
    if c <= 0x7f: return c, p
    if c >= 0xe0: return c - 256, p
    if 0x80 <= c <= 0x8f or c in (0xde, 0xdf):
        n = c & 15 if c <= 0x8f else struct.unpack_from('>H' if c == 0xde else '>I', b, p)[0]
        p += 0 if c <= 0x8f else (2 if c == 0xde else 4)
        d = {}
        for _ in range(n):
            k, p = unpack(b, p); v, p = unpack(b, p); d[str(k)] = v
        return d, p
    if 0x90 <= c <= 0x9f or c in (0xdc, 0xdd):
        n = c & 15 if c <= 0x9f else struct.unpack_from('>H' if c == 0xdc else '>I', b, p)[0]
        p += 0 if c <= 0x9f else (2 if c == 0xdc else 4)
        a = []
        for _ in range(n):
            v, p = unpack(b, p); a.append(v)
        return a, p
    if 0xa0 <= c <= 0xbf or c in (0xd9, 0xda, 0xdb):
        if c <= 0xbf: n = c & 31
        else:
            w = {0xd9: 1, 0xda: 2, 0xdb: 4}[c]; n = int.from_bytes(b[p:p + w], 'big'); p += w
        return b[p:p + n].decode(errors='replace'), p + n
    if c in (0xc4, 0xc5, 0xc6):
        w = {0xc4: 1, 0xc5: 2, 0xc6: 4}[c]; n = int.from_bytes(b[p:p + w], 'big'); p += w
        return 'bin:' + b[p:p + n].hex(), p + n
    if 0xcc <= c <= 0xcf:
        w = 1 << (c - 0xcc); return int.from_bytes(b[p:p + w], 'big'), p + w
    if 0xd0 <= c <= 0xd3:
        w = 1 << (c - 0xd0); return int.from_bytes(b[p:p + w], 'big', signed=True), p + w
    if c == 0xca: return struct.unpack_from('>f', b, p)[0], p + 4
    if c == 0xcb: return struct.unpack_from('>d', b, p)[0], p + 8
    if c in (0xc0, 0xc2, 0xc3): return {0xc0: None, 0xc2: False, 0xc3: True}[c], p
    raise ValueError('msgpack tag 0x%x' % c)


def elf(b, at):
    x = b[at:]
    h = dict(osabi=x[7], abiversion=x[8], type=struct.unpack_from('<H', x, 16)[0], machine=struct.unpack_from('<H', x, 18)[0],
             version=struct.unpack_from('<I', x, 20)[0], entry=struct.unpack_from('<Q', x, 24)[0], phoff=struct.unpack_from('<Q', x, 32)[0],
             flags=struct.unpack_from('<I', x, 48)[0], phnum=struct.unpack_from('<H', x, 56)[0])
    shoff, = struct.unpack_from('<Q', x, 40); shnum, shstr = struct.unpack_from('<HH', x, 60)
    secs = [dict(zip(('name', 'type', 'flags', 'addr', 'offset', 'size', 'link', 'info', 'align', 'entsize'),
                     struct.unpack_from('<IIQQQQIIQQ', x, shoff + 64 * i))) for i in range(shnum)]
    st = secs[shstr]
    for s in secs:
        n = x[st['offset'] + s['name']:st['offset'] + st['size']]; s['name'] = n[:n.index(0)].decode(errors='replace') if 0 in n else '?'
    notes, syms, md = [], [], None
    for s in secs:
        if s['type'] == 7:
            p = s['offset']
            while p + 12 <= s['offset'] + s['size']:
                ns, ds, ty = struct.unpack_from('<III', x, p)
                owner = x[p + 12:p + 12 + ns].rstrip(b'\0').decode(errors='replace'); d = p + 12 + ((ns + 3) & ~3)
                desc = x[d:d + ds]; p = d + ((ds + 3) & ~3)
                notes.append(dict(owner=owner, type=ty, size=ds, desc=desc.hex() if ty != 32 and ds <= 256 else None))
                if ty == 32 and md is None: md = unpack(desc)[0]
        if s['type'] == 2 and s['entsize'] == 24:
            ss = secs[s['link']]
            for p in range(s['offset'], s['offset'] + s['size'], 24):
                nm, info, other, shndx, val, size = struct.unpack_from('<IBBHQQ', x, p)
                n = x[ss['offset'] + nm:]; syms.append(dict(name=n[:n.index(0)].decode(errors='replace'), info=info, shndx=shndx, value=val, size=size))
    end = max([shoff + 64 * shnum] + [s['offset'] + s['size'] for s in secs if s['type'] != 8])
    return dict(at=at, end=at + end, header=h, sections=secs, notes=notes, symbols=syms, metadata=md, raw=x[:end])


def streams(b):
    """The blob itself, plus what it decompresses to if it is a known stream."""
    out = [('raw', b)]
    for name, f in (('zlib', zlib.decompress), ('gzip', lambda d: zlib.decompress(d, 31)), ('lzma', lzma.decompress)):
        try: out.append((name, f(b)))
        except Exception: pass
    for mod, fn in (('zstandard', lambda m, d: m.ZstdDecompressor().decompress(d, max_output_size=1 << 26)),
                    ('lz4.frame', lambda m, d: m.decompress(d))):
        try:
            m = __import__(mod, fromlist=['x']); out.append((mod, fn(m, b)))
        except Exception: pass
    return out


def flat(v, path='', out=None):
    out = {} if out is None else out
    if isinstance(v, dict):
        for k, x in v.items(): flat(x, path + '/' + k, out)
    elif isinstance(v, list) and any(isinstance(x, (dict, list)) for x in v):
        for i, x in enumerate(v): flat(x, path + '[%d]' % i, out)
    else: out[path] = v
    return out


def analyse(path):
    b = open(path, 'rb').read()
    res = dict(file=path, size=len(b), head=b[:64].hex(), elfs=[])
    for name, s in streams(b):
        at = [i for i in range(len(s) - 3) if s[i:i + 4] == b'\x7fELF'][:8]
        for a in at:
            try:
                e = elf(s, a); e['stream'] = name; res['elfs'].append(e)
            except Exception as ex:
                res['elfs'].append(dict(stream=name, at=a, error=str(ex)))
    return res


def show(r):
    print('%s: %d bytes, head %s' % (r['file'], r['size'], r['head']))
    if not r['elfs']: print('  no ELF found (raw or decompressed): an opaque container')
    for e in r['elfs']:
        if 'error' in e: print('  ELF @%d in %s: unreadable (%s)' % (e['at'], e['stream'], e['error'])); continue
        h = e['header']
        print('  ELF @%d..%d in %s: OSABI %d ABIVERSION %d type %d machine %d flags 0x%x (%s) phnum %d' % (
            e['at'], e['end'], e['stream'], h['osabi'], h['abiversion'], h['type'], h['machine'], h['flags'], MACH.get(h['flags'] & 0xff, '?'), h['phnum']))
        for s in e['sections']: print('    %-24s type %2d flags 0x%x off 0x%x size %d align %d' % (s['name'], s['type'], s['flags'], s['offset'], s['size'], s['align']))
        for n in e['notes']: print('    note %s type %d %d bytes%s' % (n['owner'], n['type'], n['size'], ' ' + n['desc'] if n['desc'] else ''))
        for s in e['symbols']: print('    symbol %-28s info 0x%02x shndx %d value 0x%x size %d' % (s['name'], s['info'], s['shndx'], s['value'], s['size']))
        if e['metadata'] is not None: print('    metadata:', json.dumps(e['metadata'], indent=2, default=str).replace('\n', '\n    '))


def first_elf(r):
    return next((e for e in r['elfs'] if 'error' not in e), None)


def diff(a, b):
    ea, eb = first_elf(a), first_elf(b)
    if not ea or not eb: print('diff: one side has no readable ELF'); return
    print('== differences %s vs %s' % (a['file'], b['file']))
    for k in ea['header']:
        if ea['header'][k] != eb['header'][k]: print('  header %s: %r vs %r' % (k, ea['header'][k], eb['header'][k]))
    na, nb = [s['name'] for s in ea['sections']], [s['name'] for s in eb['sections']]
    if na != nb: print('  sections: %s vs %s' % (na, nb))
    oa = [(n['owner'], n['type']) for n in ea['notes']]; ob = [(n['owner'], n['type']) for n in eb['notes']]
    if oa != ob: print('  notes: %s vs %s' % (oa, ob))
    fa, fb = flat(ea['metadata'] or {}), flat(eb['metadata'] or {})
    for k in sorted(set(fa) | set(fb)):
        if fa.get(k, '<missing>') != fb.get(k, '<missing>'): print('  metadata %s: %r vs %r' % (k, fa.get(k, '<missing>'), fb.get(k, '<missing>')))


def dis(r):
    e = first_elf(r)
    if not e: return
    t = next(s for s in e['sections'] if s['name'] == '.text')
    code = e['raw'][t['offset']:t['offset'] + t['size']]
    cpu = MACH.get(e['header']['flags'] & 0xff, 'gfx1030')
    out = subprocess.run([MC, '-arch=amdgcn', '-mcpu=' + cpu, '--disassemble', '-show-encoding'],
                         input=' '.join('0x%02x' % c for c in code).encode(), capture_output=True)
    print('== .text (%s, %d bytes)' % (cpu, len(code))); sys.stdout.write(out.stdout.decode(errors='replace'))


if __name__ == '__main__':
    args = sys.argv[1:]
    if not args: print(__doc__); sys.exit(2)
    r = analyse(args[0]); show(r)
    if '--ref' in args: diff(r, analyse(args[args.index('--ref') + 1]))
    if '--dis' in args: dis(r)
    if '--json' in args:
        for e in r['elfs']: e.pop('raw', None)
        json.dump(r, open(args[args.index('--json') + 1], 'w'), indent=1, default=str)
