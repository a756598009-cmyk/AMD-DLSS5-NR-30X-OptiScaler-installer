"""The RDNA3 network with edge bodies back on its persistent runs (mauri870's own defines, without local patch 12).

    python build_edge.py <out dir> [--keep-p256]

Builds third_party/mochizuki/linux/shaders/rdna3 as tools/build-mochizuki-runtime.cmd does, with NR_EDGE_BODIES=1
instead of 0 on the seven persistent runs (fswinp256, fswinpds*, fswinpup*); --keep-p256 leaves fswinp256 without
them (LLPC crashed on fswinp256 with edge bodies). Only the .spv files that change are kept in <out dir>.
"""
import pathlib, shutil, subprocess, sys, tempfile

R = pathlib.Path(__file__).resolve().parents[2] / 'third_party' / 'mochizuki'
out = pathlib.Path(sys.argv[1]).resolve()
keep = '--keep-p256' in sys.argv
read_text = pathlib.Path.read_text


def patched(self, *a, **k):
    t = read_text(self, *a, **k)
    if self.name == 'pipelines.json' and self.parent.name == 'rdna3':
        import json
        j = json.loads(t)
        for name, e in j['pipelines'].items():
            if keep and name == 'fswinp256':
                continue
            e['defines'] = ['NR_EDGE_BODIES=1' if d == 'NR_EDGE_BODIES=0' else d for d in e['defines']]
        t = json.dumps(j)
    return t


with tempfile.TemporaryDirectory() as tmp:
    base, edge = pathlib.Path(tmp) / 'base', pathlib.Path(tmp) / 'edge'
    args = ['rdna3', '--tree', 'linux', '--no-quad']
    script = R / 'windows' / 'build' / 'build_network.py'
    subprocess.run([sys.executable, str(script)] + args + ['--out', str(base)], check=True, stdout=subprocess.DEVNULL)
    sys.path.insert(0, str(script.parent))
    pathlib.Path.read_text = patched
    import build_network
    sys.argv = ['build_network.py'] + args + ['--out', str(edge)]
    build_network.main()
    pathlib.Path.read_text = read_text
    if out.exists():
        shutil.rmtree(out)
    changed = []
    for p in sorted(edge.rglob('*')):
        if p.is_file():
            q = base / p.relative_to(edge)
            if not q.is_file() or q.read_bytes() != p.read_bytes():
                dst = out / p.relative_to(edge)
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(p, dst)
                changed.append(str(p.relative_to(edge)))
    print('changed:', ' '.join(changed))
