"""A capture with the ACO network's values in place of the forced RDNA3 network's own.

    python aco_capture.py <capture dir> <rdna3 stages.txt> <aco dir> <out dir>

On the RX 9070 XT the forced RDNA3 network's values are wrong from the first layer on (its kernels assume gfx11's
fragment layout), and by the C=512 level they saturate: a dispatch there gives the same answer whatever it reads, so
kreplay readset cannot tell what it reads. This writes every value live at the captured dispatch (made before it,
read at or after it) from the FP8 network's dump of the same key: as f16 of each e4m3 byte into the FP16 twin, as
the bytes themselves into the arena. The weight buffers are hard links to the capture's.
"""
import os
import pathlib
import shutil
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from make_case import parse_capture, parse_stages  # noqa: E402


def e4m3_to_f16(b):
    v = np.frombuffer(b, dtype=np.uint8).astype(np.int32)
    s = (v & 0x80) << 8
    e = (v >> 3) & 15
    m = v & 7
    out = np.zeros(v.shape, dtype=np.int32)
    norm = e > 0
    out[norm] = s[norm] | ((e[norm] - 7 + 15) << 10) | (m[norm] << 7)
    sub = (e == 0) & (m > 0)
    f = (m[sub].astype(np.float32) / 8.0) * np.float32(2.0 ** -6)
    out[sub] = s[sub] | (f.astype(np.float16).view(np.uint16).astype(np.int32) & 0x7FFF)
    out[(e == 0) & (m == 0)] = s[(e == 0) & (m == 0)]
    nan = (e == 15) & (m == 7)
    out[nan] = s[nan] | 0x7E00
    return out.astype(np.uint16).tobytes()


def main():
    cap, stages, aco, out = (pathlib.Path(x) for x in sys.argv[1:5])
    c = parse_capture(cap / 'capture.txt')
    _, vals = parse_stages(stages)
    i = int(c['dispatch'])
    sizes = c['buffers']
    top = max(v['off'] + v['bytes'] for v in vals)
    act = twin = None
    for b, n in sizes.items():
        for b2, n2 in sizes.items():
            if n2 == 2 * n and n >= top and (act is None or n > sizes[act]):
                act, twin = b, b2
    out.mkdir(parents=True, exist_ok=True)
    shutil.copy2(cap / 'capture.txt', out / 'capture.txt')
    for b in sizes:
        dst = out / f'buf{b}.bin'
        if dst.exists():
            dst.unlink()
        if b not in (act, twin):
            os.link(cap / f'buf{b}.bin', dst)
    arena = bytearray((cap / f'buf{act}.bin').read_bytes())
    tw = bytearray((cap / f'buf{twin}.bin').read_bytes())
    done = []
    for v in vals:
        if not (v['disp'] < i <= v['last']):
            continue
        p = aco / f'k{v["key"]}' / f'full_{v["key"]}_arena.bin'
        if not p.is_file():
            print(f'key {v["key"]}: no ACO dump, the captured bytes stay')
            continue
        src = p.read_bytes()
        arena[v['off']:v['off'] + v['bytes']] = src
        tw[2 * v['off']:2 * v['off'] + 2 * v['bytes']] = e4m3_to_f16(src)
        done.append(v['key'])
    (out / f'buf{act}.bin').write_bytes(arena)
    (out / f'buf{twin}.bin').write_bytes(tw)
    print(f'{out}: keys {done} from ACO')


if __name__ == '__main__':
    main()
