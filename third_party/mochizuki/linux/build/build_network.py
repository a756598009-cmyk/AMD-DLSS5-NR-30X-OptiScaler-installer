#!/usr/bin/env python3
"""Build the Linux network for one GPU architecture from linux/shaders/<arch>/pipelines.json.

    build_network.py <arch> [--int4 | --performance | --ultra | --optimized | --experimental] [--out DIR] [--check DIR]

Writes (default build/linux/<arch>/network, with --int4 build/linux/<arch>/network-int4, with --performance
build/linux/<arch>/network-performance, with --optimized
build/linux/<arch>/network-optimized, with --experimental build/linux/<arch>/network-experimental):
    g_<name>.spv + the four marker files   the network, what --spv-dir points at
    runtime/                                 the arch-neutral passes (linux/shaders/passes)
    g_<name>_grade.spv                      style_grade: the post block with the Natural/Cinematic grade
    temporal/                                the temporal variants (+ their _grade twins) and the motion estimator

The host must be compiled with the same constants: linux/build/arch/<arch>.sh.
--optimized: CC_SILVER_AARDWOLD_OPTIMIZED (pipelines.json with pipelines-optimized.json applied; the same host).
--int4: CC_SILVER_AARDWOLD_OPTIMIZED with its int4 layers (pipelines.json with pipelines-int4.json applied; the host
built with NR_INT4=1); also the host's g_<name>.spv.iu4 markers and hs-pipelines.txt (the pipelines built with the
hard-swish, for the host's guard).
--performance / --ultra: CC_SILVER_AARDWOLD_OPTIMIZED at Optimized layers Performance / Ultra Performance
(pipelines-performance.json / pipelines-ultra.json; as --int4; out network-performance / network-ultra).
--experimental: CC_SMALL_FP8_EXPERIMENTAL (pipelines.json with pipelines-experimental.json applied; the same host).
Each of the three: the network and temporal/ (the runtime passes are the default network's), fixed-attention.txt (the
pipelines whose fixed probability tables the host writes), and when the overlay says so const-heads.txt (the host
proves and marks the constant C=512 attention heads, NR_CONSTH) and block-routes.txt (blocks whose layers run on a
pipeline of their own: "<suffix> <kernel>... : <block>...", from the overlay's "routes").
--check DIR compares every network SPV and marker with DIR byte for byte and
lists the differences (exit 1 if any).
"""
import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

R = Path(__file__).resolve().parents[2]
RUNTIME = ['runtime_alpha', 'runtime_encode', 'runtime_transfer', 'runtime_upscale', 'runtime_prep', 'runtime_depth', 'cascade_lograt', 'cascade_blur', 'cascade_feed', 'runtime_dehalo', 'runtime_predict_gain', 'runtime_predict_apply',
           'runtime_downscale', 'runtime_taps', 'runtime_blit']
# runtime passes built from another pass's source with defines: name -> (source, defines)
RUNTIME_VARIANTS = {'runtime_transfer_store': ('runtime_transfer', ['NR_STORE_NATIVE'])}
MOTION = ['motion_luma', 'motion_estimate']


def glslang(arch, src, defines, out):
    cmd = [str(R / 'toolchain/glslang/bin/glslang'), '-V', '--target-env', 'vulkan1.3',
           '-I' + str(R / 'linux/shaders' / arch / 'include'), '-I' + str(R / 'linux/shaders/passes/include')]
    cmd += ['-D' + d for d in defines] + [str(src), '-o', str(out)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f'glslang failed on {src}:\n{r.stdout}{r.stderr}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('arch')
    ap.add_argument('--int4', action='store_true')
    ap.add_argument('--performance', action='store_true')
    ap.add_argument('--ultra', action='store_true')
    ap.add_argument('--optimized', action='store_true')
    ap.add_argument('--experimental', action='store_true')
    ap.add_argument('--out', type=Path)
    ap.add_argument('--check', type=Path)
    a = ap.parse_args()
    table = json.loads((R / 'linux/shaders' / a.arch / 'pipelines.json').read_text())
    if a.int4 + a.performance + a.ultra + a.optimized + a.experimental > 1:
        sys.exit('one of --int4, --performance, --ultra, --optimized, --experimental')
    overlay = ('int4' if a.int4 else 'performance' if a.performance else 'ultra' if a.ultra else 'optimized' if a.optimized
               else 'experimental' if a.experimental else '')
    if overlay:
        i4 = json.loads((R / 'linux/shaders' / a.arch / f'pipelines-{overlay}.json').read_text())
        default = table['pipelines']
        def derive(e, c):
            return dict(e, defines=[x for x in e['defines'] if x not in c.get('remove', [])] + c.get('add', []))
        assert not set(i4['change']) - set(default), 'pipelines-%s.json changes unknown pipelines' % overlay
        table['pipelines'] = {k: derive(e, i4['change'].get(k, {})) for k, e in default.items()}
        table['pipelines'].update({k: derive(default[c['base']], c) for k, c in i4['new'].items()})
    out = a.out or R / 'build/linux' / a.arch / (f'network-{overlay}' if overlay else 'network')
    if out.exists():
        shutil.rmtree(out)
    (out / 'runtime').mkdir(parents=True)
    (out / 'temporal').mkdir()

    pipelines = table['pipelines']
    # post_no_out1 in the manifest tells the host that the post kernels never write their second output (it makes
    # that image 1x1): it has to say what they are built with.
    consts = dict(r.split() for r in table['markers']['shader-constants.txt'][1:])
    if consts.get('post_no_out1') == '1' and 'NR_POST_NO_OUT1=1' not in pipelines['fswinimagepost32']['defines']:
        sys.exit('pipelines.json: post_no_out1 1, but fswinimagepost32 is built without NR_POST_NO_OUT1=1')
    for name, e in pipelines.items():
        glslang(a.arch, R / 'linux/shaders' / a.arch / e['source'], e['defines'], out / f'g_{name}.spv')
    for name, text in table['markers'].items():
        (out / name).write_text('\n'.join(text) + '\n' if isinstance(text, list) else text + '\n')
    for name, v in table['variants'].items():
        base = pipelines[v['base']]
        glslang(a.arch, R / 'linux/shaders' / a.arch / base['source'], base['defines'] + v['add'],
                out / 'temporal' / f'{name}.spv')
    grade = table['style_grade']
    for name in grade['pipelines']:
        e = pipelines[name]
        glslang(a.arch, R / 'linux/shaders' / a.arch / e['source'], e['defines'] + grade['add'], out / f'g_{name}_grade.spv')
    for name in grade['variants']:
        base = pipelines[table['variants'][name]['base']]
        glslang(a.arch, R / 'linux/shaders' / a.arch / base['source'],
                base['defines'] + table['variants'][name]['add'] + grade['add'], out / 'temporal' / f'{name}_grade.spv')
    for k in MOTION:
        glslang(a.arch, R / 'linux/shaders/passes' / f'{k}.comp', [], out / 'temporal' / f'{k}.spv')
    shutil.copy2(out / 'shader-constants.txt', out / 'temporal' / 'shader-constants.txt')
    if a.int4 or a.performance or a.ultra:
        for name in i4['iu4']:
            (out / f'g_{name}.spv.iu4').write_text('0\n')
        hs = sorted(k for k, e in pipelines.items() if 'NR_ACT_HS=2' in e['defines'])
        (out / 'hs-pipelines.txt').write_text('\n'.join(hs) + '\n')
    if overlay:
        fixp = sorted(k for k, e in pipelines.items() if 'NR_FIXP=1' in e['defines'])
        (out / 'fixed-attention.txt').write_text('\n'.join(fixp) + '\n')
        consth = sorted(k for k, e in pipelines.items() if 'NR_CONSTH=1' in e['defines'])
        if consth:
            (out / 'const-heads.txt').write_text('\n'.join(consth) + '\n')
        # the pipelines with the int4 QKV products: the host runs them only on layers that have the tables (nr_graph.cpp)
        q4 = sorted(k for k, e in pipelines.items() if 'NR_SWQ4=1' in e['defines'])
        if q4:
            (out / 'int4-qkv.txt').write_text('\n'.join(q4) + '\n')
        if i4.get('routes'):
            lines = []
            for r in i4['routes']:
                assert all(f'{k}{r["suffix"]}' in pipelines for k in r['kernels']), r
                lines.append(' '.join([r['suffix']] + r['kernels'] + [':'] + [str(b) for b in r['blocks']]))
            (out / 'block-routes.txt').write_text('\n'.join(lines) + '\n')
        (out / 'runtime').rmdir()
    else:
        for k in RUNTIME:
            glslang(a.arch, R / 'linux/shaders/passes' / f'{k}.comp', [], out / 'runtime' / f'{k}.spv')
    print(f'{out}: {len(pipelines) + len(grade["pipelines"])} network pipelines, '
          f'{len(table["variants"]) + len(grade["variants"]) + len(MOTION)} temporal, '
          f'{0 if overlay else len(RUNTIME)} runtime')

    if a.check:
        bad = [p.name for p in sorted(out.iterdir()) if p.is_file()
               if not (a.check / p.name).is_file() or (a.check / p.name).read_bytes() != p.read_bytes()]
        print('check against', a.check, ':', 'identical' if not bad else 'DIFFERENT: ' + ' '.join(bad))
        sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
