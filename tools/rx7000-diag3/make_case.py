"""One kit 3 case from a capture (MZ_KCAP), its read and write sets (kreplay readset) and the ACO network's values.

    python make_case.py <capture dir> <sets.txt> <rdna3 stages.txt> <aco dir> <out dir> <id> [--raw-inputs]

The case runs the captured dispatch on the bytes it reads, with every activation value it reads replaced by the
RX 9070 XT's own FP8 network's value of the same plan key (the RX 7800 XT's network matched those up to here:
kit 2's clean stage comparison), so its answer can be measured against the FP8 network's value of the keys it
writes. Values in the FP16 twin go in as e4m3 (data.bin is half the size) and come out as f16. --raw-inputs keeps
the captured bytes (the forced RDNA3 network's own values on this card) instead.
"""
import argparse
import pathlib
import struct


def parse_capture(p):
    c = {'buffers': {}, 'bindings': []}
    for line in p.read_text().splitlines():
        k, *v = line.split()
        if k == 'buffer':
            c['buffers'][int(v[0])] = int(v[1])
        elif k == 'binding':
            c['bindings'].append((int(v[0]), int(v[1])))
        elif k in ('spirv', 'push_range', 'dispatch', 'frame'):
            c[k] = v[0]
        elif k == 'grid':
            c['grid'] = v
        elif k == 'push':
            c['push'] = v
    return c


def parse_stages(p):
    disp, vals = {}, []
    for line in p.read_text().splitlines():
        f = line.split()
        if f[:1] == ['D']:
            disp[int(f[1])] = f[2]
        elif f[:1] == ['V']:
            vals.append({'key': int(f[2]), 'disp': int(f[3]), 'first': int(f[4]), 'last': int(f[5]),
                         'off': int(f[6]), 'bytes': int(f[7])})
    return disp, vals


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cap', type=pathlib.Path)
    ap.add_argument('sets', type=pathlib.Path)
    ap.add_argument('stages', type=pathlib.Path)
    ap.add_argument('aco', type=pathlib.Path)
    ap.add_argument('out', type=pathlib.Path)
    ap.add_argument('id')
    ap.add_argument('--raw-inputs', action='store_true')
    a = ap.parse_args()
    cap = parse_capture(a.cap / 'capture.txt')
    disp_names, vals = parse_stages(a.stages)
    i = int(cap['dispatch'])
    sizes = cap['buffers']
    # The activation arena and its FP16 twin: the pair (n, 2n) whose n holds every value's slot.
    top = max(v['off'] + v['bytes'] for v in vals)
    act = twin = None
    for b, n in sizes.items():
        for b2, n2 in sizes.items():
            if n2 == 2 * n and n >= top and (act is None or n > sizes[act]):
                act, twin = b, b2
    if act is None:
        raise SystemExit('no activation arena among the captured buffers')
    reads, writes = [], []
    for line in a.sets.read_text().splitlines():
        f = line.split()
        if f[:1] == ['read']:
            reads.append((int(f[1]), int(f[2]), int(f[3])))
        elif f[:1] == ['write']:
            writes.append((int(f[1]), int(f[2]), int(f[3])))
        elif f[:1] == ['check'] and f[1] != 'OK':
            raise SystemExit(f'{a.sets}: the read set check failed')
    live = [v for v in vals if v['disp'] < i <= v['last']]
    made = [v for v in vals if v['disp'] == i]

    def region0(v, b):
        return (v['off'], v['bytes']) if b == act else (2 * v['off'], 2 * v['bytes']) if b == twin else None

    # A live value the dispatch reads part of goes in whole: on the RX 7000 it may read more of it (its padding).
    extra = []
    for b, off, n in reads:
        for v in live:
            r = region0(v, b)
            if r and r[0] < off + n and off < r[0] + r[1]:
                extra.append((b, r[0], r[1]))
    merged = []
    for b, off, n in sorted(reads + extra):
        if merged and merged[-1][0] == b and off <= merged[-1][1] + merged[-1][2]:
            pb, po, pn = merged[-1]
            merged[-1] = (b, po, max(po + pn, off + n) - po)
        else:
            merged.append((b, off, n))
    reads = merged

    def region(v, b):   # a value's bytes in buffer b
        return (v['off'], v['bytes']) if b == act else (2 * v['off'], 2 * v['bytes']) if b == twin else None

    aco_cache = {}

    def aco(key):
        if key not in aco_cache:
            p = a.aco / f'k{key}' / f'full_{key}_arena.bin'
            aco_cache[key] = p.read_bytes() if p.is_file() else None
        return aco_cache[key]

    a.out.mkdir(parents=True, exist_ok=True)
    data = bytearray()
    lines = []
    subst = []
    # Cut each read range at the slots of the live values it covers: those parts come from the ACO network.
    for b, off, n in reads:
        cuts = []
        for v in (live if not a.raw_inputs else []):
            r = region(v, b)
            if not r or aco(v['key']) is None:
                continue
            lo, hi = max(off, r[0]), min(off + n, r[0] + r[1])
            if lo < hi:
                cuts.append((lo, hi, v))
        cuts.sort(key=lambda x: x[0])
        at = off
        raw = (a.cap / f'buf{b}.bin').open('rb')
        for lo, hi, v in cuts + [(off + n, off + n, None)]:
            if lo > at:   # captured bytes
                raw.seek(at)
                lines.append(f'slice {b} {at} {lo - at} {len(data)}')
                data += raw.read(lo - at)
            if v is not None:
                src = aco(v['key'])
                if b == twin:   # the twin: f16 of each e4m3 byte, so the e4m3 bytes themselves (half the size)
                    s0 = (lo - 2 * v['off']) // 2
                    e0 = (hi - 2 * v['off'] + 1) // 2
                    lines.append(f'slice {b} {2 * v["off"] + 2 * s0} {2 * (e0 - s0)} {len(data)} e4m3')
                    data += src[s0:e0]
                else:
                    lines.append(f'slice {b} {lo} {hi - lo} {len(data)}')
                    data += src[lo - v['off']:hi - v['off']]
                subst.append(f'key {v["key"]} {"twin" if b == twin else "arena"} [{lo},{hi})')
            at = max(at, hi)
        raw.close()
    (a.out / 'data.bin').write_bytes(bytes(data))
    # Outputs: each write range, named by the value made here it falls in, with the ACO network's value as reference.
    outs = []
    for b, off, n in writes:
        name, ref, kind, enc = None, '-', 'raw', '-'
        for v in made:
            r = region(v, b)
            if r and r[0] <= off and off + n <= r[0] + r[1]:
                name = f'key{v["key"]}.{"twin" if b == twin else "arena"}'
                src = aco(v['key'])
                if b == twin:
                    kind = 'f16'
                    if src is not None and off % 2 == 0 and n % 2 == 0:
                        s0 = (off - 2 * v['off']) // 2
                        ref, enc = f'ref_{len(outs)}.bin', 'e4m3'
                        (a.out / ref).write_bytes(src[s0:s0 + n // 2])
                else:
                    kind = 'f16'
                    if src is not None:
                        ref, enc = f'ref_{len(outs)}.bin', 'raw'
                        (a.out / ref).write_bytes(src[off - v['off']:off - v['off'] + n])
                break
        if name is None:
            name = f'buf{b}@{off}'
        outs.append(f'out {b} {off} {n} {kind} {name} {ref} {enc}')
    spirv = cap['spirv']
    head = [f'name {a.id}', f'about dispatch {i} {disp_names.get(i, spirv)} at {cap["frame"]}',
            f'spirv {spirv}', 'grid ' + ' '.join(cap['grid']), f'push_range {cap["push_range"]}', 'push ' + ' '.join(cap['push'])]
    head += [f'buffer {b} {n}' for b, n in sorted(sizes.items())]
    head += [f'binding {x} {y}' for x, y in cap['bindings']]
    (a.out / 'case.txt').write_text('\n'.join(head + lines + outs) + '\n')
    print(f'{a.id}: {len(lines)} slices, data.bin {len(data) / 1e6:.1f} MB, {len(outs)} outputs; from ACO: ' +
          ('; '.join(subst) if subst else 'nothing'))


if __name__ == '__main__':
    main()
