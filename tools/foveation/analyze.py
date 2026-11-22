# analyze.py <out dir> <size> <ref tag> <tag>... : PSNR of each foveated run against the full-resolution run, by zone,
# from mz_fov's dumps (<tag>_fN.bin, RGBA16F). Zones from the run's grid line: centre = full density on both axes,
# edge = past the fall-off on either axis, ramp = the rest. sRGB-encoded [0,1] for PSNR. With --pan N, the last two
# dumps of each run also give the temporal error: mean |out_t(x) - out_t-1(x - N)| x 255, away from the wrap seam.
# --unorm: the dumps hold display colour already (E2E_FORMAT=rgba8). --crops writes side-by-side PNGs (input | full NR | foveated) of the centre and a corner.
import sys, re, glob, os
import numpy as np
from PIL import Image

def load(path, w, h):
    a = np.fromfile(path, dtype=np.float16).astype(np.float32).reshape(h, w, 4)[..., :3]
    return a

UNORM = '--unorm' in sys.argv

def srgb(x):
    x = np.clip(x, 0, 1)
    if UNORM:
        return x
    return np.where(x <= 0.0031308, 12.92 * x, 1.055 * np.power(x, 1 / 2.4) - 0.055)

def psnr(a, b, m=None):
    d = (a - b) ** 2
    if m is not None:
        d = d[m]
    mse = d.mean()
    return 99.0 if mse <= 1e-12 else 10 * np.log10(1 / mse)

def grid(dirn, tag):
    t = open(os.path.join(dirn, tag + '.txt')).read()
    m = re.search(r'x a=([\d.]+) b=([\d.]+) e=([\d.]+), y a=([\d.]+) b=([\d.]+) e=([\d.]+)', t)
    g = re.search(r'-> (\d+)x(\d+)', t)
    net = re.search(r'network ([\d.]+) ms', t)
    return [float(v) for v in m.groups()], (g.group(0)[3:] if g else ''), float(net.group(1)) if net else 0

def zones(w, h, p):
    ax, bx, _, ay, by, _ = p
    tx = np.abs(2 * (np.arange(w) + 0.5) / w - 1)
    ty = np.abs(2 * (np.arange(h) + 0.5) / h - 1)
    cx, cy = tx <= ax, ty <= ay
    ex, ey = tx >= ax + bx, ty >= ay + by
    centre = cy[:, None] & cx[None, :]
    edge = ey[:, None] | ex[None, :]
    return centre, edge, ~centre & ~edge

def dumps(dirn, tag):
    fs = glob.glob(os.path.join(dirn, tag + '_f*.bin'))
    return sorted(fs, key=lambda f: int(re.search(r'_f(\d+)\.bin$', f).group(1)))

def temporal(dirn, tag, w, h, pan, masks):
    fs = dumps(dirn, tag)
    if len(fs) < 2:
        return None
    errs = []
    for a, b in zip(fs[:-1], fs[1:]):
        fa, fb = int(re.search(r'_f(\d+)', a).group(1)), int(re.search(r'_f(\d+)', b).group(1))
        prev, cur = srgb(load(a, w, h)), srgb(load(b, w, h))
        shifted = np.roll(prev, pan, axis=1)
        diff = np.abs(cur - shifted).mean(axis=2) * 255
        seam = (fb * pan) % w
        x = np.arange(w)
        ok = (np.minimum(np.abs(x - seam), w - np.abs(x - seam)) > 96)[None, :].repeat(h, 0)
        errs.append([diff[ok & m].mean() for m in masks])
    return np.mean(errs, axis=0)

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    opts = dict(a[2:].split('=') if '=' in a else (a[2:], '1') for a in sys.argv[1:] if a.startswith('--'))
    dirn, size, ref, tags = args[0], args[1], args[2], args[3:]
    w, h = map(int, size.split('x'))
    pan = int(opts.get('pan', 0))
    ref_img = srgb(load(dumps(dirn, ref)[-1], w, h))
    _, _, ref_net = grid(dirn, ref)
    inp = None
    if 'input' in opts:
        im = Image.open(opts['input']).convert('RGB').resize((w, h), Image.LANCZOS)
        inp = np.asarray(im, dtype=np.float32) / 255
    print(f'{size} reference {ref}: network {ref_net:.2f} ms')
    if inp is not None:
        print(f'  input vs full NR (how much NR changes the frame): {psnr(inp, ref_img):.2f} dB')
    for tag in tags:
        p, work, net = grid(dirn, tag)
        c, e, r = zones(w, h, p)
        img = srgb(load(dumps(dirn, tag)[-1], w, h))
        line = (f'  {tag}: grid {work}, network {net:.2f} ms ({100 * (1 - net / ref_net):+.0f}% saved), PSNR all '
                f'{psnr(img, ref_img):.2f}, centre {psnr(img, ref_img, c):.2f} ({100 * c.mean():.0f}% of px), '
                f'ramp {psnr(img, ref_img, r):.2f}, edge {psnr(img, ref_img, e):.2f} dB')
        if inp is not None:
            line += f'; edge input vs full NR {psnr(inp, ref_img, e):.2f}'
        # The centre's global shift: mean luminance of the foveated answer over the full one there, and the centre's
        # PSNR once that gain is taken out.
        lum = lambda x: x @ np.array([.2126, .7152, .0722], np.float32)
        gain = lum(img)[c].mean() / max(lum(ref_img)[c].mean(), 1e-6)
        line += f'; centre gain {100 * (gain - 1):+.1f}%, without it {psnr(np.clip(img / gain, 0, 1), ref_img, c):.2f} dB'
        print(line)
        if pan:
            tr, tf = temporal(dirn, ref, w, h, pan, (c, r, e)), temporal(dirn, tag, w, h, pan, (c, r, e))
            if tr is not None and tf is not None:
                print(f'    temporal |t - (t-1) shifted| x255: full NR centre/ramp/edge {tr[0]:.3f}/{tr[1]:.3f}/{tr[2]:.3f},'
                      f' foveated {tf[0]:.3f}/{tf[1]:.3f}/{tf[2]:.3f}')
        if 'crops' in opts:
            os.makedirs(os.path.join(dirn, 'crops'), exist_ok=True)
            cw, ch = w // 6, h // 5
            for name, (x0, y0) in {'centre': (w // 2 - cw // 2, h // 2 - ch // 2), 'corner': (16, 16),
                                   'left': (16, h // 2 - ch // 2)}.items():
                tiles = ([inp] if inp is not None else []) + [ref_img, img]
                row = np.concatenate([t[y0:y0 + ch, x0:x0 + cw] for t in tiles], axis=1)
                Image.fromarray((np.clip(row, 0, 1) * 255 + .5).astype(np.uint8)).save(
                    os.path.join(dirn, 'crops', f'{tag}_{name}.png'))
            full = np.concatenate([ref_img, img], axis=1)
            Image.fromarray((np.clip(full, 0, 1) * 255 + .5).astype(np.uint8)).resize((w, h // 2)).save(
                os.path.join(dirn, 'crops', f'{tag}_full_vs_fov.png'))

main()
