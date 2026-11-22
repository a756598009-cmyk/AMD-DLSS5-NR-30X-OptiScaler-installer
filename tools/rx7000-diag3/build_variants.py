"""Kit 3's SPIR-V: one pipeline of the RDNA3 network as it ships, and that pipeline with one suspect changed.

    python build_variants.py <out dir> <pipeline> [--variants] [--check <shipped shaders-rdna3 dir>]

Builds third_party/mochizuki/linux/shaders/rdna3 as tools/build-mochizuki-runtime.cmd does (build_network.py rdna3
--tree linux --no-quad: glslang -E, unroll_glsl.py, glslang -V), into <out dir>/<pipeline>/base.spv and, with
--variants, one .spv a variant plus variants.txt ("<name> <file> <what changed>"). A variant whose SPIR-V is the same
as base's (a define that pipeline does not read) or that does not compile is left out and listed in skipped.txt.
--check compares base.spv with the shipped g_<pipeline>.spv byte for byte.
"""
import argparse
import json
import pathlib
import re
import shutil
import subprocess
import sys

R = pathlib.Path(__file__).resolve().parents[2] / 'third_party' / 'mochizuki'
SH = R / 'linux' / 'shaders' / 'rdna3'
GLSLANG = R / 'toolchain' / 'glslang' / 'bin' / 'glslang'
UNROLL = R / 'windows' / 'build' / 'unroll_glsl.py'

# Defines flipped one at a time, to the value that drops the optimisation (the simpler path), where the pipeline
# sets them. Value None: remove the define.
FLIPS = [
    ('fwaves1', 'NR_FWAVES', '1', 'one wave per window (no LDS shared between two waves)'),
    ('qk_apart', 'NR_QK_TOGETHER', '0', 'Q and K projected in separate passes'),
    ('prob_early', 'NR_PROB_LATE_SHUFFLE', '0', 'softmax probabilities shuffled early (no late shuffle)'),
    ('v_noswap', 'NR_V_SWAP', '0', 'V not register-resident (V through lds_v)'),
    ('oob_nobranch', 'NR_OOB_BRANCH', '0', 'out-of-range tiles masked without the branch'),
    ('coherent', 'NR_COHERENT_ACT', '1', 'activation views kept coherent'),
    ('wave_uniform', 'NR_WAVE_UNIFORM', '1', 'wave index read through readfirstlane'),
    ('tchain0', 'NR_TCHAIN', '0', 'no tile-counter code'),
    ('stream0', 'NR_STREAM_MLP', '0', 'MLP not streamed'),
    ('live15', 'NR_LIVE15_NOOOB', '0', 'all-live body keeps its out-of-range checks'),
    ('exp_hi', 'NR_EXP_NOHI', '0', 'exp keeps its high-half step'),
    ('dwsel0', 'NR_OP_DWSEL', '0', 'operand put without dword selects'),
    ('act_alt0', 'NR_ACT_ALT', '0', 'activation without swapped multiplicands'),
    ('pkn0', 'NR_PKN', '0', 'no packed narrowing'),
    ('qkscale0', 'NR_QK_SCALE_FAST', '0', 'Q/K scale as written (no fast mode)'),
    ('prob_unbounded', 'NR_PROB_BOUNDED', '0', 'probabilities converted with clamps'),
    ('norm_f16', 'NR_NORM_F32', '0', 'Q/K norm in packed f16'),
    ('act_f16', 'NR_ACT_F32', '0', 'activation in f16'),
    ('resid_f16', 'NR_RESIDUAL_F32', '0', 'residual product in f16'),
    ('pack_c32_0', 'NR_PACK_C32', '0', 'C=32 operands not packed'),
    ('pool_scalar', 'NR_POOL_VECTOR_STORE', '0', 'pool stored per component'),
    ('pool_addr', 'NR_POOL_ADDR_UNIFORM', '0', 'pool address not forced uniform'),
    ('pool_dpp0', 'NR_POOL_DPP16', '0', 'pool reduction without DPP'),
    ('pool_st16_0', 'NR_POOL_ST16', '0', 'pool stores not 16-bit'),
    ('ds_qpair0', 'NR_DS_QPAIR', '0', 'downsample projection without q pairs'),
    ('ds_ident0', 'NR_DS_IDENTITY_FAST', '0', 'downsample identity without the fast path'),
    # ffwd3_t.comp, gemm1x1.comp, attn.comp (the C=512 level)
    ('remap_pc0', 'NR_GEMM_REMAP_PC', '0', 'workgroup order not remapped from push constants'),
    ('ffwd_pair0', 'NR_FFWD_PAIR', '0', 'FFN without paired fragments'),
    ('ffwd_koff0', 'NR_FFWD_KOFF', '0', 'FFN without the k offset split'),
    ('ffwd_gmajor0', 'NR_FFWD_GMAJOR', '0', 'FFN group-major order off'),
    ('occ_lds0', 'NR_OCC_LDS', None, 'no LDS occupancy padding'),
    ('koff_split0', 'NR_KOFF_SPLIT', '0', 'GEMM without the k offset split'),
    ('swap_ab0', 'NR_SWAP_AB', '0', 'GEMM operands not swapped'),
    ('epi_lanebyte0', 'NR_EPI_LANEBYTE', '0', 'GEMM epilogue without lane bytes'),
    ('epi_frag0', 'NR_EPI_FRAGMENT', '0', 'GEMM epilogue not per fragment'),
    ('attn_oob0', 'NR_ATTN_OOB_BRANCH', '0', 'attention masks out-of-range keys without the branch'),
    ('attn_pairq0', 'NR_ATTN_PAIR_QUANT', '0', 'attention quantises one value at a time'),
    ('attn_qkswap0', 'NR_ATTN_QKSWAP', '0', 'attention Q/K not swapped'),
    ('attn_vfrag0', 'NR_ATTN_VFRAG', '0', 'attention V not as fragments'),
    ('attn_wpair0', 'NR_ATTN_WPAIR', '0', 'attention windows not paired'),
    ('attn_apipe1', 'NR_APIPE', '1', 'attention without the two-stage pipeline'),
    ('attn_koff0', 'NR_ATTN_KOFF', '0', 'attention without the k offset split'),
    ('attn_exp2_0', 'NR_ATTN_EXP2', '0', 'attention softmax with exp, not exp2'),
]
# Flips taken together in the "allsafe" variants: the ones about ordering, exchange and memory, not arithmetic.
SAFE = ['qk_apart', 'prob_early', 'v_noswap', 'oob_nobranch', 'coherent', 'wave_uniform', 'tchain0', 'stream0',
        'exp_hi', 'dwsel0', 'act_alt0', 'pool_addr', 'pool_dpp0', 'remap_pc0', 'occ_lds0', 'attn_oob0', 'attn_apipe1']

BARRIER_FENCED = ('memoryBarrierShared(); memoryBarrierBuffer(); barrier(); memoryBarrierShared(); '
                  'memoryBarrierBuffer();')
LDS_STORE = re.compile(r'\b(lds_\w+|nr_wl|nr_bl|image_features|ups_projected|nr_act_lds)\s*\[[^;=]*\]\s*=[^=]')
COOP_STORE = re.compile(r'coopMatStore\w*\s*\([^;]*\b(lds_\w+|ups_projected)\b')


def fence(text):
    """Every workgroup barrier also fences shared and buffer memory on both sides."""
    n = len(re.findall(r'\bbarrier\s*\(\s*\)\s*;', text))
    return re.sub(r'\bbarrier\s*\(\s*\)\s*;', BARRIER_FENCED, text), n


def lds_fence(text):
    """memoryBarrierShared() after every statement that stores to shared memory (memory order only, legal anywhere)."""
    out, n = [], 0
    for line in text.split('\n'):
        s = line.rstrip()
        if s.endswith(';') and s.count('(') == s.count(')') and (LDS_STORE.search(s) or COOP_STORE.search(s)):
            line = s + ' memoryBarrierShared();'
            n += 1
        out.append(line)
    return '\n'.join(out), n


def sg_barrier(text):
    """subgroupMemoryBarrierShared() after every subgroup shuffle/broadcast statement."""
    out, n = [], 0
    for line in text.split('\n'):
        s = line.rstrip()
        if s.endswith(';') and s.count('(') == s.count(')') and re.search(r'\bsubgroup(Shuffle\w*|Broadcast\w*)\s*\(', s):
            line = s + ' subgroupMemoryBarrierShared();'
            n += 1
        out.append(line)
    return '\n'.join(out), n


def compile_one(defines, out, transform=None, unroll=True):
    pre = out.with_suffix('.pre.comp')
    r = subprocess.run([str(GLSLANG), '-E', '-I' + str(SH / 'include'), '-I' + str(R / 'windows/shaders/passes/include')] +
                       ['-D' + d for d in defines] + [str(SH / 'fswin_t.comp' if not SRC else SRC)],
                       capture_output=True, text=True)
    if r.returncode:
        return 'preprocess failed: ' + (r.stdout + r.stderr).strip().splitlines()[-1][:200], None
    text = r.stdout
    note = None
    if transform:
        text, n = transform(text)
        note = n
        if not n:
            return 'nothing to change', None
    pre.write_text(text)
    if unroll:
        u = subprocess.run([sys.executable, str(UNROLL), str(pre), str(pre)], capture_output=True, text=True)
        if u.returncode:
            pre.unlink()
            return 'unroll failed', None
    c = subprocess.run([str(GLSLANG), '-V', '--target-env', 'vulkan1.3', '-I' + str(SH / 'include'),
                        '-I' + str(R / 'windows/shaders/passes/include'), str(pre), '-o', str(out)],
                       capture_output=True, text=True)
    pre.unlink()
    if c.returncode:
        return 'glslang failed: ' + ' | '.join(l for l in (c.stdout + c.stderr).splitlines() if 'ERROR' in l)[:300], None
    return None, note


SRC = None


def main():
    global SRC
    ap = argparse.ArgumentParser()
    ap.add_argument('out', type=pathlib.Path)
    ap.add_argument('pipeline')
    ap.add_argument('--variants', action='store_true')
    ap.add_argument('--check', type=pathlib.Path)
    a = ap.parse_args()
    table = json.loads((SH / 'pipelines.json').read_text())['pipelines']
    e = table[a.pipeline]
    SRC = SH / e['source']
    unroll = e['source'] in {'fswin_t.comp', 'ffwd3_t.comp', 'attn.comp', 'vit_attn.comp'}
    d = a.out / a.pipeline
    if d.exists():
        shutil.rmtree(d)
    d.mkdir(parents=True)
    base = list(e['defines'])
    err, _ = compile_one(base, d / 'base.spv', unroll=unroll)
    if err:
        sys.exit(f'{a.pipeline}: base: {err}')
    if a.check:
        same = (a.check / f'g_{a.pipeline}.spv').read_bytes() == (d / 'base.spv').read_bytes()
        print(f'{a.pipeline}: base.spv {"identical to" if same else "DIFFERENT from"} the shipped g_{a.pipeline}.spv')
        if not same:
            sys.exit(1)
    lines = [f'base base.spv the pipeline as released ({len(base)} defines)']
    skipped = []
    if a.variants:
        names = {d_.split('=')[0]: d_ for d_ in base}
        built = {}

        def try_variant(name, defines, about, transform=None):
            out = d / f'{name}.spv'
            err, note = compile_one(defines, out, transform, unroll=unroll)
            if err:
                skipped.append(f'{name}: {err}')
                return False
            if out.read_bytes() == (d / 'base.spv').read_bytes():
                out.unlink()
                skipped.append(f'{name}: same SPIR-V as base')
                return False
            if note is not None:
                about += f' ({note} sites)'
            lines.append(f'{name} {name}.spv {about}')
            built[name] = defines
            return True

        flips = {}
        for name, key, val, about in FLIPS:
            if key not in names or names[key] == f'{key}={val}':
                continue
            defs = [x for x in base if x.split('=')[0] != key] + ([f'{key}={val}'] if val is not None else [])
            if try_variant(name, defs, f'{key}={val if val is not None else "(removed)"}: {about}'):
                flips[name] = (key, val)
        try_variant('fence', base, 'every barrier() fenced with memoryBarrierShared/memoryBarrierBuffer both sides', fence)
        try_variant('ldsfence', base, 'memoryBarrierShared() after every shared-memory store', lds_fence)
        try_variant('sgfence', base, 'subgroupMemoryBarrierShared() after every subgroup shuffle/broadcast', sg_barrier)
        if unroll:
            err, _ = compile_one(base, d / 'rolled.spv', None, unroll=False)
            if err:
                skipped.append(f'rolled: {err}')
            else:
                lines.append('rolled rolled.spv loops left rolled (no unroll_glsl.py): LLPC compiles them its own way')
        safe = [n for n in SAFE if n in flips]
        if safe:
            keys = {flips[n][0]: flips[n][1] for n in safe}
            defs = [x for x in base if x.split('=')[0] not in keys] + [f'{k}={v}' for k, v in keys.items() if v is not None]

            def both(t):
                t, n1 = fence(t)
                t, n2 = lds_fence(t)
                return t, n1 + n2
            if not try_variant('allsafe', defs, 'all of ' + ','.join(safe) + ' + fence + ldsfence', both):
                # A combination that does not compile: drop flips until it does, last first.
                while len(safe) > 1:
                    safe.pop()
                    keys = {flips[n][0]: flips[n][1] for n in safe}
                    defs = [x for x in base if x.split('=')[0] not in keys] + [f'{k}={v}' for k, v in keys.items() if v is not None]
                    if try_variant('allsafe', defs, 'all of ' + ','.join(safe) + ' + fence + ldsfence', both):
                        break
            if 'fwaves1' in flips and 'allsafe' in built:
                try_variant('allsafe1w', [x for x in built['allsafe'] if x.split('=')[0] != 'NR_FWAVES'] + ['NR_FWAVES=1'],
                            'allsafe + NR_FWAVES=1', both)
    (d / 'variants.txt').write_text('\n'.join(lines) + '\n')
    (d / 'skipped.txt').write_text('\n'.join(skipped) + '\n')
    print(f'{a.pipeline}: {len(lines)} pipelines, {len(skipped)} skipped')
    for s in skipped:
        print('  skipped', s)


if __name__ == '__main__':
    main()
