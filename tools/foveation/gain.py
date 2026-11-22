# gain.py <out dir> <size> <image.png> <ref tag> <tag>... : how the mean luminance moves, per zone (centre, ramp,
# edge, all): full NR over the input, foveated over the input, foveated over full NR. Linear light.
import sys, re, os
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

def load(path, w, h):
    return np.fromfile(path, dtype=np.float16).astype(np.float32).reshape(h, w, 4)[..., :3]

def last(dirn, tag):
    fs = [f for f in os.listdir(dirn) if f.startswith(tag + '_f') and f.endswith('.bin')]
    return os.path.join(dirn, max(fs, key=lambda f: int(re.search(r'_f(\d+)\.bin$', f).group(1))))

def zones(dirn, tag, w, h):
    t = open(os.path.join(dirn, tag + '.txt')).read()
    ax, bx, _, ay, by, _ = map(float, re.search(r'x a=([\d.]+) b=([\d.]+) e=([\d.]+), y a=([\d.]+) b=([\d.]+) e=([\d.]+)', t).groups())
    tx = np.abs(2 * (np.arange(w) + .5) / w - 1); ty = np.abs(2 * (np.arange(h) + .5) / h - 1)
    c = (ty <= ay)[:, None] & (tx <= ax)[None, :]
    e = (ty >= ay + by)[:, None] | (tx >= ax + bx)[None, :]
    return {'centre': c, 'ramp': ~c & ~e, 'edge': e, 'all': np.ones_like(c)}

dirn, size, image, ref, tags = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5:]
w, h = map(int, size.split('x'))
y = np.array([.2126, .7152, .0722], np.float32)
inp = np.asarray(Image.open(image).convert('RGB').resize((w, h), Image.LANCZOS), np.float32) / 255
inp = np.where(inp <= .04045, inp / 12.92, ((inp + .055) / 1.055) ** 2.4) @ y
full = np.clip(load(last(dirn, ref), w, h), 0, None) @ y
for tag in tags:
    fov = np.clip(load(last(dirn, tag), w, h), 0, None) @ y
    z = zones(dirn, tag, w, h)
    print(tag + ': ' + ', '.join(f'{k} full/in {100 * (full[m].mean() / inp[m].mean() - 1):+.1f}% fov/in '
                                  f'{100 * (fov[m].mean() / inp[m].mean() - 1):+.1f}% fov/full '
                                  f'{100 * (fov[m].mean() / full[m].mean() - 1):+.1f}%' for k, m in z.items()))
