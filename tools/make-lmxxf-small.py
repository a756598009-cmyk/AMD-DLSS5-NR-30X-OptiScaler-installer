"""Make lmxxf 0.44's Small model weights (native-game-tiled-assets\\small\\) from NVIDIA's nvngx_dlssnr.dll 310.9.16.

Upstream converts with Development/HIP/experiments/small-model/convert_weights.py, which reads layout maps from its
release/ folder; that folder is not in its git. This takes the converter and its evidence from the upstream clone at
the pinned commit, recovers those maps by matching the DLL's Original records against the lmxxf weights we already
ship (made from the same model), proves them by converting the Original model again and comparing every file bit for
bit, then converts the Small model and checks each file against upstream's model-manifest-small.json.

  py -3 -I tools\\make-lmxxf-small.py --dll <nvngx_dlssnr.dll> --weights <native-game-tiled-assets> --out <folder>

The output is NVIDIA-derived: never commit it. It is published as the lmxxf-weights-small payload component.
"""
import argparse, hashlib, io, json, subprocess, sys, tarfile, tempfile
from pathlib import Path
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('--dll', type=Path, required=True)
p.add_argument('--weights', type=Path, required=True, help='the lmxxf-weights folder (native-game-tiled-assets)')
p.add_argument('--out', type=Path, required=True)
p.add_argument('--upstream', type=Path, default=ROOT.parent / 'dlss5-on-amd-9070xt-porting')
p.add_argument('--ref', default='c24e3a97cdf596d0c997cee7deda91c981d520ce')
a = p.parse_args()
a.dll, a.weights, a.out = a.dll.resolve(), a.weights.resolve(), a.out.resolve()  # the converter runs elsewhere

RESULTS = 'Development/results/dlssnr-310-9-16-analysis-20261018'
MANIFEST = 'Development/results/small-model-20261018/model-manifest-small.json'
CONVERTER = 'Development/HIP/experiments/small-model/convert_weights.py'


def derive(dev, blob, assets, out):
    """The release/ maps convert_weights.py reads, each the position of every raw byte of a matrix (or half of a
    vector) in the weights file, found where the value matches in every block of that kind."""
    sys.path.insert(0, str(dev))
    from decode_tinlayout_global import e4m3fn
    records = json.loads((dev / RESULTS.split('/', 1)[1] / 'records-ht.json').read_text())
    raw = {r['name']: blob[r['file_offset']:r['file_offset'] + r['payload_size']] for r in records}

    def asset(name):
        f32, f16 = assets / (name + '.f32'), assets / (name + '.f16')
        return np.fromfile(f32, '<f4') if f32.exists() else np.fromfile(f16, '<f2').astype('<f4')

    def match(sources, targets, what):
        rows = lambda xs: np.ascontiguousarray(np.stack([np.asarray(x, np.float32).view(np.uint32) for x in xs], 1))
        s, t = rows(sources), rows(targets)
        key = [('', np.uint32)] * s.shape[1]
        sv, tv = s.view(key).ravel(), t.view(key).ravel()
        order = np.argsort(tv, kind='stable')
        lo, hi = np.searchsorted(tv[order], sv, 'left'), np.searchsorted(tv[order], sv, 'right')
        pos = np.where(hi - lo == 1, order[np.minimum(lo, len(order) - 1)], -1)
        if what and (np.any(pos < 0) or np.unique(pos).size != pos.size):
            raise SystemExit(f'{what}: no unique match')
        return pos

    def save(path, **arrays):
        path.parent.mkdir(parents=True, exist_ok=True)
        np.savez(path, **arrays)

    # C32 (preblock) blocks.
    c32 = [raw[f'block{b}.layer0.layer'][:20672] for b in (1, 2, 3, 4, 67, 68, 69)]
    ffn = [asset(f'block{b}-ffn') for b in (1, 2, 3, 4, 67, 68, 69)]
    att = [asset(f'block{b}-attention') for b in (1, 2, 3, 4, 67, 68, 69)]
    w1 = match([e4m3fn(r[:4096]) for r in c32], [f[512:4608] for f in ffn], 'c32 w1')
    w2 = match([e4m3fn(r[4096:8192]) for r in c32], [f[4608:8704] for f in ffn], 'c32 w2')
    save(out / 'preblock-ffn-byte-layout/layout.npz', w1_hidden=w1 // 32, w1_input=w1 % 32, w2_output=w2 // 128,
         w2_hidden=w2 % 128)
    v = match([e4m3fn(r[10336:11360]) for r in c32], [x[2048:3072] for x in att], 'c32 v')
    for offset, base in [(8288, 0), (9312, 1024)]:
        if not np.array_equal(match([e4m3fn(r[offset:offset + 1024]) for r in c32], [x[base:base + 1024] for x in att],
                                    'c32 q/k'), v):
            raise SystemExit('c32 q/k do not share the v layout')
    pr = match([e4m3fn(r[19568:20592]) for r in c32], [x[3072:4096] for x in att], 'c32 projection')
    half = [r.view('<f2').astype(np.float32) for r in c32]
    bias = match([h[5680:9776] for h in half], [x[4096:8192] for x in att], 'c32 bias')
    skip = match([h[10296:10328] for h in half], [x[8193:8225] for x in att], 'c32 skip')
    save(out / 'preblock-attention-layout/matrix-layout.npz', v_output=v // 32, v_input=v % 32,
         projection_output=pr // 32, projection_input=pr % 32, skip_channel=skip)
    save(out / 'preblock-attention-layout/bias-layout.npz', query=bias // 64, key=bias % 64)

    # C64/C128/C256 window blocks; the encoder and decoder ones of a channel count share a layout.
    sizes = {64: 61760, 128: 197184, 256: 689232}
    contract = {64: (0xa0a0, 0xe0a0, 0xf0b0), 128: (0x24120, 0x2c120, 0x30130), 256: (0x88220, 0x98220, 0xa8240)}
    for C, size in sizes.items():
        blocks = [b for b in list(range(5, 23)) + list(range(49, 66)) if b not in (56, 62)
                  and raw[f'block{b}.layer0.layer'].size in (size, {64: 69936, 128: 229936, 256: 820288}[C])]
        rs = [raw[f'block{b}.layer0.layer'][:size] for b in blocks]
        ffn = [asset(f'block{b}-ffn') for b in blocks]
        att = [asset(f'block{b}-attention') for b in blocks]
        heads, n, b2 = C // 32, C * C, 4 * C * C
        b3 = b2 + C * 128
        w1 = match([e4m3fn(r[:b2]) for r in rs], [f[:b2] for f in ffn], f'c{C} w1')
        w2 = match([e4m3fn(r[b2:b3]) for r in rs], [f[b2:2 * b2] for f in ffn], f'c{C} w2')
        w3 = match([e4m3fn(r[b3:b3 + n]) for r in rs], [f[2 * b2:2 * b2 + n] for f in ffn], f'c{C} w3')
        save(out / f'native-c{C}/ffn-layout/layout.npz', w1_hidden=w1 // C, w1_input=w1 % C, w2_output=w2 // (4 * C),
             w2_hidden=w2 % (4 * C), w3_output=w3 // C, w3_input=w3 % C)
        # v: every raw byte that lands in the v matrix, in raw order; q and k sit 0x800 and 0x400 bytes before.
        whole = match([e4m3fn(r) for r in rs], [x[2 * n:3 * n] for x in att], None)
        offsets = np.nonzero(whole >= 0)[0]
        positions = whole[offsets]
        if positions.size != n or np.unique(positions).size != n:
            raise SystemExit(f'c{C} v: no unique match')
        for delta, base in [(-0x800, 0), (-0x400, n)]:
            if not np.array_equal(match([e4m3fn(r[offsets + delta]) for r in rs], [x[base:base + n] for x in att], None),
                                  positions):
                raise SystemExit(f'c{C} q/k do not follow v')
        bias_begin, scale, ats = contract[C]
        p_begin = {64: 0xe0b0, 128: 0x2c130, 256: 0x98240}[C]
        pr = match([e4m3fn(r[p_begin:p_begin + n]) for r in rs], [x[3 * n:4 * n] for x in att], f'c{C} projection')
        save(out / f'native-c{C}/attention-layout/matrix-layout.npz', v_output=positions // C, v_input=positions % C,
             v_offsets=offsets, p_output=pr // C, p_input=pr % C)
        halves = [r.view('<f2').astype(np.float32) for r in rs]
        nb = heads * 4096
        bias = match([h[bias_begin // 2:scale // 2] for h in halves], [x[4 * n:4 * n + nb] for x in att], f'c{C} bias')
        save(out / f'native-c{C}/attention-layout/bias-layout.npz', head=bias // 4096, query=bias % 4096 // 64,
             key=bias % 64)
        skip = match([h[ats // 2:ats // 2 + C] for h in halves], [x[4 * n + nb + heads:] for x in att], f'c{C} skip')
        np.save(out / f'native-c{C}/attention-layout/skip-channels.npy', skip)
    # DS maps by upstream's own address-bit rule (derive_native_ds_layout.py); the Original comparison proves them.
    for C in (128, 256):
        d = C.bit_length() - 1
        i = np.arange(2 * C * C, dtype=np.int32)
        gather = lambda bits: sum(((i >> bit) & 1) << k for k, bit in enumerate(bits)).astype(np.int32)
        save(out / f'native-c{C}/ds-layout/layout.npz', input=gather([1, 0, 4, 5, 2] + list(range(d + 6, 2 * d + 1))),
             output=gather([3, 6, 7, 8, 9] + list(range(10, d + 6))))


with tempfile.TemporaryDirectory(prefix='lmxxf-small-') as td:
    tree = Path(td)
    tar = subprocess.run(['git', '-C', str(a.upstream), 'archive', a.ref, '--', ':(glob)Development/*.py', CONVERTER, RESULTS,
                          MANIFEST], check=True, capture_output=True).stdout
    tarfile.open(fileobj=io.BytesIO(tar)).extractall(tree, filter='data')
    blob = np.fromfile(a.dll, np.uint8)
    if hashlib.sha256(blob.tobytes()).hexdigest() != '2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f':
        raise SystemExit(f'{a.dll} is not nvngx_dlssnr.dll 310.9.16')
    derive(tree / 'Development', blob, a.weights, tree / 'release')
    convert = lambda *args: subprocess.run([sys.executable, '-I', str(tree / CONVERTER), '--dll', str(a.dll), *args],
                                           cwd=tree, check=True)
    convert('--model', 'original', '--output', str(tree / 'original'), '--compare', str(a.weights))
    a.out.mkdir(parents=True, exist_ok=True)
    convert('--model', 'small', '--output', str(a.out))
    expected = json.loads((tree / MANIFEST).read_text())['files']
    bad = [n for n, f in expected.items() if hashlib.sha256((a.out / n).read_bytes()).hexdigest() != f['sha256']]
    if bad or len(list(a.out.iterdir())) != len(expected) + 1:
        raise SystemExit(f'Small weights differ from upstream 0.44: {bad}')
print(f'{a.out}: {len(expected)} files, every one as upstream 0.44 lists it')
