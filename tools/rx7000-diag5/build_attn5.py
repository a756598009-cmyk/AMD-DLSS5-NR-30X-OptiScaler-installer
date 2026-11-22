"""Kit 5's SPIR-V for dispatch 11 (attn, C=512 window attention): the released RDNA3 pipeline, the RDNA2 native
body (attnnat), and the released pipeline with one suspect changed.

    python build_attn5.py <out dir> [--check <shipped shaders-rdna3 dir>]

Built as the shipped network is (glslang -E, unroll_glsl.py, glslang -V). Writes <out>/<name>.spv and
<out>/variants.txt ("<name> <file> <what changed>"), base first, nat second; skipped.txt lists what did not build or
came out as base's SPIR-V. --check: base.spv must equal the shipped g_attn.spv and nat.spv the built g_attnnat.spv.
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

# Element-wise stand-ins for the cooperative matrix loads and stores (the RDNA2 tree's coopmm.glsl, on this tree's
# probed gfx11 map: A/B component k of lane l is row/column l%16, k; accumulator component c is row 2c + l/16,
# column l%16). With them a fragment is only ever moved by its components.
ELEM_ST = r'''
#undef NR_STORE_ACC_COL
#define NR_STORE_ACC_COL(frag, arr, off, stride) do { \
    const uint nr_b = uint(off) + (gl_SubgroupInvocationID & 15u) * uint(stride) + (gl_SubgroupInvocationID >> 4); \
    for (uint nr_c = 0u; nr_c < 8u; ++nr_c) (arr)[nr_b + 2u * nr_c] = (frag)[nr_c]; } while (false)
'''
ELEM_LD = r'''
#undef NR_LOAD_A
#define NR_LOAD_A(frag, arr, off, stride) do { subgroupMemoryBarrierShared(); \
    const uint nr_b = uint(off) + (gl_SubgroupInvocationID & 15u) * uint(stride); \
    for (uint nr_c = 0u; nr_c < 16u; ++nr_c) (frag)[nr_c] = (arr)[nr_b + nr_c]; } while (false)
#undef NR_LOAD_B
#define NR_LOAD_B(frag, arr, off, stride) do { subgroupMemoryBarrierShared(); \
    const uint nr_b = uint(off) + (gl_SubgroupInvocationID & 15u) * uint(stride); \
    for (uint nr_c = 0u; nr_c < 16u; ++nr_c) (frag)[nr_c] = (arr)[nr_b + nr_c]; } while (false)
'''
ZLDS = '''
    for (uint z_ = tid; z_ < uint(NR_QKVW * NR_WIN * NR_QS); z_ += uint(NR_THREADS)) { lds_q[z_] = NR_QK_TYPE(0); lds_k[z_] = NR_QK_TYPE(0); }
    for (uint z_ = tid; z_ < uint(NR_QKVW * NR_VSIZE); z_ += uint(NR_THREADS)) lds_v[z_] = NR_E4M3(0);
    for (uint z_ = tid; z_ < uint(NR_AWAVES * NR_IROWS * NR_PS); z_ += uint(NR_THREADS)) lds_p[z_] = NR_E4M3(0);
    barrier();
'''
EXT = '#extension GL_KHR_shader_subgroup_basic : require\n'


def sub1(text, old, new):
    if text.count(old) != 1:
        raise SystemExit(f'anchor not found once: {old[:60]!r}')
    return text.replace(old, new)


def t_elemst(src, epi):
    return sub1(src, '#include "swin_math.glsl"\n', '#include "swin_math.glsl"\n' + EXT + ELEM_ST), epi


def t_elemio(src, epi):
    return sub1(src, '#include "swin_math.glsl"\n', '#include "swin_math.glsl"\n' + EXT + ELEM_ST + ELEM_LD), epi


def t_zlds(src, epi):
    return sub1(src, '    const uint lane = tid % 32u;\n', '    const uint lane = tid % 32u;\n' + ZLDS), epi


def t_zfrag(src, epi):
    z = 'NR_FRAG_E4M3(float16_t(0.0))'
    epi = sub1(epi, 'NR_FRAG_E4M3 vfr0, vfr1, qf0, qf1, kf0, kf1;',
               f'NR_FRAG_E4M3 vfr0 = {z}, vfr1 = {z}, qf0 = {z}, qf1 = {z}, kf0 = {z}, kf1 = {z};')
    src = sub1(src, '            NR_FRAG_B pf[4];\n',
               '            NR_FRAG_B pf[4];\n            pf[0] = NR_FRAG_B(float16_t(0.0)); pf[1] = pf[0]; pf[2] = pf[0]; pf[3] = pf[0];\n')
    src = sub1(src, '                NR_FRAG_E4M3 result;\n', f'                NR_FRAG_E4M3 result = {z};\n')
    return src, epi


def t_zall(src, epi):
    src, epi = t_zlds(src, epi)
    return t_zfrag(src, epi)


BARRIER_FENCED = 'memoryBarrierShared(); memoryBarrierBuffer(); barrier(); memoryBarrierShared(); memoryBarrierBuffer();'


def t_fence(src, epi):
    src = sub1(src, '#define NR_QKVBAR() barrier()\n', '#define NR_QKVBAR() ' + BARRIER_FENCED.rstrip(';') + '\n')
    return re.sub(r'\bbarrier\s*\(\s*\)\s*;', BARRIER_FENCED, src), epi


def t_sgfence(src, epi):
    def f(t):
        out = []
        for line in t.split('\n'):
            s = line.rstrip()
            if s.endswith(';') and s.count('(') == s.count(')') and re.search(r'\bsubgroup(Shuffle\w*|Broadcast\w*)\s*\(', s):
                line = s + ' subgroupMemoryBarrierShared();'
            out.append(line)
        return '\n'.join(out)
    return f(src), f(epi)


# name, defines changed ({key: value or None to remove}), source transform, what it tests
VARIANTS = [
    ('qkswap0', {'NR_ATTN_QKSWAP': '0'}, None,
     'NR_ATTN_QKSWAP=0: Q and K projected as X.W (no operand swap) and scattered element by element into LDS'),
    ('transpose0', {'NR_ATTN_TRANSPOSE': '0'}, None,
     'NR_ATTN_TRANSPOSE=0: logits Q.K^T, probabilities through lds_p, context P.V (no hand-built B operand)'),
    ('exp2_0', {'NR_ATTN_EXP2': '0'}, None, 'NR_ATTN_EXP2=0: softmax exponent one logit at a time'),
    ('apipe1', {'NR_APIPE': '1'}, None, 'NR_APIPE=1: QKV loads not pipelined'),
    ('qpad0', {'NR_QPAD': '0'}, None,
     'NR_QPAD=0: Q/K rows in LDS 64 bytes apart (16-byte aligned) instead of 72 (8-byte aligned)'),
    ('occ_lds0', {'NR_OCC_LDS': None}, None, 'no LDS occupancy padding'),
    ('coherent', {'NR_COHERENT_ACT': '1'}, None, 'NR_COHERENT_ACT=1: activation views coherent'),
    ('fence', {}, t_fence, 'every barrier() fenced with memoryBarrierShared/Buffer on both sides'),
    ('sgfence', {}, t_sgfence, 'subgroupMemoryBarrierShared() after every subgroup shuffle/broadcast'),
    ('zlds', {}, t_zlds, 'LDS (Q, K, V, P) zeroed at the start, then a barrier'),
    ('zfrag', {}, t_zfrag, 'the fragments built by components (Q/K/V epilogue, P, result) start at zero'),
    ('zall', {}, t_zall, 'zlds + zfrag'),
    ('elemst', {}, t_elemst, 'FP16 accumulator stores (Q, K, V into LDS, the output) element by element, no coopMatStore'),
    ('elemio', {}, t_elemio, 'elemst + the LDS operand loads element by element: no coopMatLoad/Store left, only the MMAs'),
]


def compile_one(defines, src_text, epi_text, out):
    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        inc = td / 'inc'
        inc.mkdir()
        (inc / 'attn_qkv_epi.glsl').write_text(epi_text)
        src = td / 'attn.comp'
        src.write_text(src_text)
        pre = td / 'attn.pre.comp'
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
    src0 = (SH / 'attn.comp').read_text()
    epi0 = (SH / 'include' / 'attn_qkv_epi.glsl').read_text()
    if a.out.exists():
        shutil.rmtree(a.out)
    a.out.mkdir(parents=True)
    base = list(table['attn']['defines'])
    lines, skipped = [], []
    for name, defs, about in (('base', base, 'the pipeline as released'),
                              ('nat', list(table['attnnat']['defines']), 'the RDNA2 native body (attnnat): no cooperative matrices')):
        err = compile_one(defs, src0, epi0, a.out / f'{name}.spv')
        if err:
            sys.exit(f'{name}: {err}')
        lines.append(f'{name} {name}.spv {about}')
    if a.check:
        for name, shipped in (('base', 'g_attn.spv'), ('nat', 'g_attnnat.spv')):
            p = a.check / shipped
            if p.is_file():
                same = p.read_bytes() == (a.out / f'{name}.spv').read_bytes()
                print(f'{name}.spv {"identical to" if same else "DIFFERENT from"} {p}')
                if not same:
                    sys.exit(1)
    basebytes = (a.out / 'base.spv').read_bytes()
    for name, change, transform, about in VARIANTS:
        defs = [d for d in base if d.split('=')[0] not in change] + [f'{k}={v}' for k, v in change.items() if v is not None]
        src, epi = (transform(src0, epi0) if transform else (src0, epi0))
        out = a.out / f'{name}.spv'
        err = compile_one(defs, src, epi, out)
        if err:
            skipped.append(f'{name}: {err}')
            continue
        if out.read_bytes() == basebytes:
            out.unlink()
            skipped.append(f'{name}: same SPIR-V as base')
            continue
        lines.append(f'{name} {name}.spv {about}')
    (a.out / 'variants.txt').write_text('\n'.join(lines) + '\n')
    (a.out / 'skipped.txt').write_text('\n'.join(skipped) + '\n')
    print(f'{len(lines)} pipelines, {len(skipped)} skipped')
    for s in skipped:
        print('  skipped', s)


if __name__ == '__main__':
    main()
