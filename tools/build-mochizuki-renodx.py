"""build-mochizuki-renodx.py <upstream> <out> <scratch> <glslang>: the RenoDX stage's Vulkan shaders (nr_renodx.cpp).

RenoDX's HLSL is built with Slang the way RenoDX builds its own Vulkan path (RENODX_VULKAN), as upstream's
windows/build/build_renodx.sh does (v0.0.8.2): the resolve again with NR_PREP_RESOLVE for [Preprocess]'s proxy, the
preprocess and debug kernels and Passes (fast) in GLSL. Slang 2026.19 is downloaded into
<upstream>/toolchain on first use. After TheAutomatic/dlss-5-amd-project's tools/build/mochizuki-renodx.py (GPL-3.0).
"""
import hashlib
import io
import sys
import subprocess
import urllib.request
import zipfile
from pathlib import Path

KERNELS = ('v6_linearize v6_autoscale v6_exposure_scale v6_encode v6_resolve '
           'v6_pedestal_reduce v6_pedestal_reduce_unshaped v6_commit v6_commit_exposure '
           'v6_resolve_commit legacy_encode legacy_decode legacy_reduce legacy_reduce_unshaped '
           'legacy_apply legacy_decode_reduce look_compose look_upsample look_band').split()
PREP_RESOLVE = 'v6_resolve v6_resolve_commit v6_pedestal_reduce_unshaped'.split()
FAST = 'st_fb_in st_prep st_box st_apply'.split()
SLANG = 'https://github.com/shader-slang/slang/releases/download/v2026.19/slang-2026.19-windows-x86_64.zip'
SLANG_SHA = 'fc922f214b5d115f663932e59e80e0f6b6cc6fae140979cf307da6459b6f8f33'


def slangc(upstream):
    target = upstream / 'toolchain/slang-2026.19'
    compiler = target / 'bin/slangc.exe'
    if not compiler.is_file():
        data = urllib.request.urlopen(SLANG).read()
        if hashlib.sha256(data).hexdigest() != SLANG_SHA:
            raise SystemExit('slang checksum mismatch')
        zipfile.ZipFile(io.BytesIO(data)).extractall(target)
    return compiler


def main(upstream, out, scratch, glslang):
    upstream, out, scratch = Path(upstream), Path(out), Path(scratch)
    source = upstream / 'windows/shaders/renodx'
    prep = upstream / 'windows/shaders/renodx-prep'
    slang = slangc(upstream)
    out.mkdir(parents=True, exist_ok=True)
    scratch.mkdir(parents=True, exist_ok=True)
    for kernel in KERNELS:
        wrapper = scratch / ('vk_' + kernel + '.comp.slang')
        wrapper.write_text('#define RENODX_VULKAN 1\n#include "' + kernel + '.cs_5_1.hlsl"\n', encoding='utf8')
        shifts = ['-fvk-t-shift', '0', 'all', '-fvk-u-shift', '4', 'all'] if kernel.startswith('legacy_reduce') or kernel == 'legacy_apply' else []
        subprocess.run([str(slang), str(wrapper), '-I', str(source), '-target', 'spirv', '-profile', 'glsl_460',
                        '-entry', 'main', '-stage', 'compute', '-O2', *shifts, '-o', str(out / ('renodx_' + kernel + '.spv'))], check=True)
    for kernel in PREP_RESOLVE:
        wrapper = scratch / ('vk_' + kernel + '_prep.comp.slang')
        wrapper.write_text('#define RENODX_VULKAN 1\n#define NR_PREP_RESOLVE 1\n#include "' + kernel + '.cs_5_1.hlsl"\n',
                           encoding='utf8')
        subprocess.run([str(slang), str(wrapper), '-I', str(source), '-I', str(prep), '-target', 'spirv', '-profile',
                        'glsl_460', '-entry', 'main', '-stage', 'compute', '-O2',
                        '-o', str(out / ('renodx_' + kernel + '_prep.spv'))], check=True)
    for kernel in ('renodx_prep', 'renodx_prep_encode', 'renodx_debug'):
        subprocess.run([str(glslang), '-V', '--target-env', 'vulkan1.3', '-I' + str(prep),
                        str(prep / (kernel + '.comp')), '-o', str(out / (kernel + '.spv'))], check=True)
    fast = upstream / 'windows/shaders/renodx-fast'
    for kernel in FAST:
        subprocess.run([str(glslang), '-V', '--target-env', 'vulkan1.3', '-I' + str(fast),
                        str(fast / (kernel + '.comp')), '-o', str(out / ('renodx_' + kernel + '.spv'))], check=True)
    (out / 'RenoDX-LICENSE.txt').write_bytes((source / 'LICENSE').read_bytes())


if __name__ == '__main__':
    main(*sys.argv[1:5])
