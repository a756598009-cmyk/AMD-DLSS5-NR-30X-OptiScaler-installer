"""Kit 6's SPIR-V for the two RDNA3 attention kernels run alone: attn (dispatch 11) and vitattn (dispatch 21).

    python build_k6.py <out dir> [--check <shipped shaders-rdna3 dir>]

Built as the shipped network is (glslang -E, unroll_glsl.py, glslang -V). Writes <out>/<kernel>/<name>.spv and
<out>/<kernel>/variants.txt ("<name> <file> <what changed>"): rel (as released in 0.5.8), fix (as shipped by this
tree), nat (the RDNA2 native body), then diagnostics. skipped.txt lists what did not build or equals another file.
--check: fix.spv must equal the shipped g_<kernel>.spv and nat.spv g_<kernel>nat.spv.
"""
import argparse
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

R = pathlib.Path(__file__).resolve().parents[2] / 'third_party' / 'mochizuki'
SH = R / 'linux' / 'shaders' / 'rdna3'
GLSLANG = R / 'toolchain' / 'glslang' / 'bin' / 'glslang'
UNROLL = R / 'windows' / 'build' / 'unroll_glsl.py'
CHUNK = 'vit_attn_vt_chunk.glsl'

# The V^T gather element by element on the probed gfx11 map (A component k of lane l is row l%16, column k).
ELEM_V = r'''
#extension GL_KHR_shader_subgroup_basic : require
#undef NR_LOAD_A_COL
#define NR_LOAD_A_COL(frag, arr, off, stride) do { \
    const uint nr_b = uint(off) + (gl_SubgroupInvocationID & 15u); \
    for (uint nr_c = 0u; nr_c < 16u; ++nr_c) (frag)[nr_c] = (arr)[nr_b + nr_c * uint(stride)]; } while (false)
'''


def sub1(text, old, new):
    if text.count(old) != 1:
        raise SystemExit(f'anchor not found once: {old[:60]!r}')
    return text.replace(old, new)


def t_velem(src, inc):
    return sub1(src, '#include "swin_math.glsl"\n', '#include "swin_math.glsl"\n' + ELEM_V), inc


def t_noguard(src, inc):
    return re.sub(r'#if \(NR_HD \+ NR_QPAD\) % 8 != 0[^\n]*\n#error[^\n]*\n#endif\n', '', src, count=1), inc


NOVADDR = {'NR_VKMAN': '0', 'NR_VADDR': '0'}
KERNELS = {
    'attn': ('attn.comp', ['attn_qkv_epi.glsl'], [
        ('rel', {'NR_QPAD': '4'}, t_noguard, 'as released in 0.5.8: Q/K rows in LDS 72 bytes apart (8-byte aligned)'),
    ]),
    'vitattn': ('vit_attn.comp', [CHUNK], [
        ('rel', {'NR_VT_STAGE': None}, None, 'as released in 0.5.8: V^T loaded ColumnMajor from the arena (a gather)'),
        ('velem', {'NR_VT_STAGE': None}, t_velem, 'rel with the V^T gather element by element (no coopMatLoad for V)'),
        ('vkman0', {'NR_VT_STAGE': None, 'NR_VKMAN': '0'}, None,
         'rel with K loaded by coopMatLoad (A RowMajor) instead of by hand'),
        ('vsplit0', {'NR_VT_STAGE': None, 'NR_VSPLIT_TAIL': '0', **NOVADDR}, None,
         'rel with every key chunk guarded (one chunk body, no literal addressing)'),
        ('vtrans0', {'NR_VT_STAGE': None, 'NR_VTRANS': '0', **NOVADDR}, None,
         'the untransposed kernel: P through LDS (64-element rows), V as B RowMajor (also a gather)'),
        ('voutvec0', {'NR_VT_STAGE': None, 'NR_VOUT_VEC': '0', 'NR_VOUT_PAIR': '0'}, None,
         'rel with scalar output stores'),
        ('fixvkman0', {'NR_VKMAN': '0'}, None, 'fix with K loaded by coopMatLoad (A RowMajor) instead of by hand'),
    ]),
}


def compile_one(src_name, defines, src_text, inc_texts, out):
    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        inc = td / 'inc'
        inc.mkdir()
        for n, t in inc_texts.items():
            (inc / n).write_text(t)
        src = td / src_name
        src.write_text(src_text)
        pre = td / ('pre_' + src_name)
        r = subprocess.run([str(GLSLANG), '-E', '-I' + str(inc), '-I' + str(SH / 'include'),
                            '-I' + str(R / 'windows/shaders/passes/include')] + ['-D' + d for d in defines] + [str(src)],
                           capture_output=True, text=True)
        if r.returncode:
            return 'preprocess failed: ' + ' | '.join(l for l in (r.stdout + r.stderr).splitlines() if 'ERROR' in l)[:300]
        pre.write_text(r.stdout)
        u = subprocess.run([sys.executable, str(UNROLL), str(pre), str(pre)], capture_output=True, text=True)
        if u.returncode:
            return 'unroll failed: ' + (u.stdout + u.stderr).strip()[-200:]
        c = subprocess.run([str(GLSLANG), '-V', '--target-env', 'vulkan1.3', '-I' + str(SH / 'include'),
                            '-I' + str(R / 'windows/shaders/passes/include'), str(pre), '-o', str(out)],
                           capture_output=True, text=True)
        if c.returncode:
            return 'glslang failed: ' + ' | '.join(l for l in (c.stdout + c.stderr).splitlines() if 'ERROR' in l)[:300]
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out', type=pathlib.Path)
    ap.add_argument('--check', type=pathlib.Path)
    a = ap.parse_args()
    table = json.loads((SH / 'pipelines.json').read_text())['pipelines']
    if a.out.exists():
        shutil.rmtree(a.out)
    for kern, (src_name, incs, variants) in KERNELS.items():
        od = a.out / kern
        od.mkdir(parents=True)
        src0 = (SH / src_name).read_text()
        inc0 = {n: (SH / 'include' / n).read_text() for n in incs}
        base = list(table[kern]['defines'])
        lines, skipped, made = [], [], {}
        for name, defs, about in (('fix', base, 'as shipped by this tree (kit 6)'),
                                  ('nat', list(table[kern + 'nat']['defines']),
                                   'the RDNA2 native body: no cooperative matrices')):
            err = compile_one(src_name, defs, src0, inc0, od / f'{name}.spv')
            if err:
                sys.exit(f'{kern} {name}: {err}')
            lines.append(f'{name} {name}.spv {about}')
            made[name] = (od / f'{name}.spv').read_bytes()
        if a.check:
            for name, shipped in (('fix', f'g_{kern}.spv'), ('nat', f'g_{kern}nat.spv')):
                same = (a.check / shipped).read_bytes() == made[name]
                print(f'{kern} {name}.spv {"identical to" if same else "DIFFERENT from"} {shipped}')
                if not same:
                    sys.exit(1)
        for name, change, transform, about in variants:
            defs = [d for d in base if d.split('=')[0] not in change] + \
                   [f'{k}={v}' for k, v in change.items() if v is not None]
            src, inc = (transform(src0, inc0) if transform else (src0, inc0))
            out = od / f'{name}.spv'
            err = compile_one(src_name, defs, src, inc, out)
            if err:
                skipped.append(f'{name}: {err}')
                continue
            b = out.read_bytes()
            dup = [n for n, v in made.items() if v == b]
            if dup:
                out.unlink()
                skipped.append(f'{name}: same SPIR-V as {dup[0]}')
                continue
            made[name] = b
            lines.append(f'{name} {name}.spv {about}')
        lines.sort(key=lambda l: {'rel': 0, 'fix': 1, 'nat': 2}.get(l.split()[0], 3))
        (od / 'variants.txt').write_text('\n'.join(lines) + '\n')
        (od / 'skipped.txt').write_text('\n'.join(skipped) + '\n')
        print(f'{kern}: {len(lines)} pipelines, {len(skipped)} skipped')
        for s in skipped:
            print('  skipped', s)


if __name__ == '__main__':
    main()
