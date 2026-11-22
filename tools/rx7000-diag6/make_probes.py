"""Kit 6's two probe cases for kreplay (`kreplay case <dir> <spv> <runs> <fill> <out>`), with their references.

    python make_probes.py <out dir>

<out>/LDS: lds_stride_test.comp at NR_S=36 (lds36.spv, attn's released 72-byte rows) and 32 (lds32.spv, kit 6's
64-byte rows): out a_rowmajor = each tile transposed, b_colmajor = each tile. <out>/VT: vt_stage_test.comp (vt.spv):
out colmajor (the released gather) and staged (kit 6's LDS staging), both V^T of each tile. Inputs are f16 in [-4, 4],
so every product is exact and a right answer is 100% equal.
"""
import pathlib
import subprocess
import sys

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
R = HERE.parents[1] / 'third_party' / 'mochizuki'
GLSLANG = R / 'toolchain' / 'glslang' / 'bin' / 'glslang'
INC = R / 'linux' / 'shaders' / 'rdna3' / 'include'


def glsl(src, out, *defines):
    subprocess.run([str(GLSLANG), '-V', '--target-env', 'vulkan1.3', f'-I{INC}', *[f'-D{d}' for d in defines],
                    str(HERE / src), '-o', str(out)], check=True, capture_output=True)


def main():
    out = pathlib.Path(sys.argv[1])
    lds, vt = out / 'LDS', out / 'VT'
    for d in lds, vt:
        d.mkdir(parents=True, exist_ok=True)

    glsl('lds_stride_test.comp', lds / 'lds36.spv', 'NR_S=36')
    glsl('lds_stride_test.comp', lds / 'lds32.spv', 'NR_S=32')
    v = np.random.default_rng(2).uniform(-4, 4, 16384).astype(np.float16)
    v.tofile(lds / 'data.bin')
    t = v.reshape(64, 16, 16)
    t.transpose(0, 2, 1).tofile(lds / 'ref_a.bin')
    t.tofile(lds / 'ref_b.bin')
    (lds / 'case.txt').write_text('name LDS\nabout attn\'s LDS row stride\nspirv lds.spv\ngrid 16 1 1\npush_range 0\n'
                                  'buffer 0 98304\nbinding 0 0\nslice 0 0 32768 0\n'
                                  'out 0 32768 32768 f16 a_rowmajor ref_a.bin raw\n'
                                  'out 0 65536 32768 f16 b_colmajor ref_b.bin raw\n')

    glsl('vt_stage_test.comp', vt / 'vt.spv')
    v = np.random.default_rng(1).uniform(-4, 4, 16384).astype(np.float16)
    v.tofile(vt / 'data.bin')
    ref = np.zeros(8192, np.float16)
    for wg in range(16):
        base = wg * 16 * 64 + 512
        for n in range(2):
            tile = v[base + n * 256:base + n * 256 + 256].reshape(16, 16)
            ref[wg * 512 + n * 256:wg * 512 + n * 256 + 256] = tile.T.reshape(-1)
    ref.tofile(vt / 'ref.bin')
    (vt / 'case.txt').write_text('name VT\nabout vitattn V^T\nspirv vt.spv\ngrid 16 1 1\npush_range 0\n'
                                 'buffer 0 131072\nbinding 0 0\nbinding 2 0\nslice 0 0 32768 0\n'
                                 'out 0 32768 16384 f16 colmajor ref.bin raw\n'
                                 'out 0 65536 16384 f16 staged ref.bin raw\n')
    print(f'probes in {out}')


if __name__ == '__main__':
    main()
