"""The RDNA3 network's Linux SPIR-V (what gfx11 ACO records are made from), as released or a variant.

    python tools\\rx7000-diag7\\build_spv.py <variant> <out dir>

variant:
  fix   the tree as it is (attn NR_QPAD 0, vitattn NR_NATIVE=1: the 0.5.9 fix)
  coop  0.5.8's attn and vitattn (attn NR_QPAD 4, vitattn on cooperative matrices; the kernels LLPC gets wrong)
Runs third_party/mochizuki/linux/build/build_network.py rdna3 on a copy of linux/ (with the toolchain junction).
"""
import os, re, shutil, subprocess, sys, tempfile
from pathlib import Path

UP = Path(__file__).resolve().parents[2] / 'third_party' / 'mochizuki'


def main():
    variant, out = sys.argv[1], Path(sys.argv[2]).resolve()
    if variant not in ('fix', 'coop'):
        sys.exit('variant: fix | coop')
    tmp = Path(tempfile.mkdtemp(prefix='r3spv-'))
    shutil.copytree(UP / 'linux', tmp / 'linux')
    os.symlink(UP / 'toolchain', tmp / 'toolchain', target_is_directory=True) if False else \
        subprocess.check_call(['cmd', '/c', 'mklink', '/J', str(tmp / 'toolchain'), str(UP / 'toolchain')],
                              stdout=subprocess.DEVNULL)
    sh = tmp / 'linux' / 'shaders' / 'rdna3'
    if variant == 'coop':
        pj = (sh / 'pipelines.json').read_text()
        pj = pj.replace(', "NR_TCHAIN=1", "NR_NATIVE=1"]}', ', "NR_TCHAIN=1"]}', 1)
        pj = pj.replace('"NR_TCHAIN=1", "NR_APIPE=2"]}', '"NR_TCHAIN=1", "NR_APIPE=2", "NR_QPAD=4"]}', 1)
        (sh / 'pipelines.json').write_text(pj)
        a = (sh / 'attn.comp').read_text()
        a, n = re.subn(r'#if \(NR_HD \+ NR_QPAD\) % 8 != 0\n#error [^\n]*\n#endif\n', '', a)
        if n != 1:
            sys.exit('attn.comp: the QPAD guard was not found')
        (sh / 'attn.comp').write_text(a)
    if out.exists():
        shutil.rmtree(out)
    subprocess.check_call([sys.executable, str(tmp / 'linux' / 'build' / 'build_network.py'), 'rdna3', '--out', str(out)])
    subprocess.call(['cmd', '/c', 'rmdir', str(tmp / 'toolchain')])
    shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    main()
