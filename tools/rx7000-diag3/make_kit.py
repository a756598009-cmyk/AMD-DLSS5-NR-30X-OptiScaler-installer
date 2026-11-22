"""Assemble kit 3 from the work folder: cases, SPIR-V, kit.txt, the tool and the instructions.

    python make_kit.py <work> <kit dir> [--runs N] [--processes N] [--rand-runs N]

<work> is what capture.ps1, readsets.ps1, aco-refs.ps1 and build_variants.py filled (cap\\D<n>, aco\\, r3stage\\,
spv\\<pipeline>\\, dlssnr-amd\\shaders-rdna3\\, kreplay.exe). The kit is <kit dir>\\OpenNR-RX7000-diag3.
"""
import argparse
import pathlib
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
# dispatch -> whether its pipeline's variants run there (the first dispatch of each pipeline that has a variant set)
# (10 gemmprojc, 11 attn and 12 are not here: on the RX 9070 XT they give the same answer whatever they read, so
# kreplay readset cannot find their inputs.) Capture folder: D<n>, or D<n>a (aco_capture.py) where the forced
# network's own values had saturated.
# 4 to 7 (fswindsp64 .. fswin256) are left out for size: kit 2 never named one of them as the first value to change.
CASES = [(1, True, ''), (2, False, ''), (3, False, ''), (8, True, ''), (9, True, 'a')]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('work', type=pathlib.Path)
    ap.add_argument('kit', type=pathlib.Path)
    ap.add_argument('--runs', type=int, default=50)
    ap.add_argument('--processes', type=int, default=2)
    ap.add_argument('--rand-runs', type=int, default=20)
    a = ap.parse_args()
    root = a.kit / 'OpenNR-RX7000-diag3'
    if root.exists():
        shutil.rmtree(root)
    files = root / 'files'
    (files / 'cases').mkdir(parents=True)
    kit = [f'processes {a.processes}', f'rand_runs {a.rand_runs}']
    for d, variants, suffix in CASES:
        cid = f'D{d:02d}'
        cap = a.work / 'cap' / f'D{d}{suffix}'
        r = subprocess.run([sys.executable, str(HERE / 'make_case.py'), str(cap), str(cap / 'sets.txt'),
                            str(a.work / 'r3stage' / 'stages.txt'), str(a.work / 'aco'), str(files / 'cases' / cid), cid],
                           capture_output=True, text=True)
        if r.returncode:
            sys.exit(f'{cid}: {r.stdout}{r.stderr}')
        print(r.stdout.strip())
        spv = [l.split()[1] for l in (cap / 'capture.txt').read_text().splitlines() if l.startswith('spirv ')][0]
        pipe = spv[2:-4]
        about = [l for l in (files / 'cases' / cid / 'case.txt').read_text().splitlines() if l.startswith('about ')][0][6:]
        kit.append(f'case {cid} cases/{cid} {a.runs} {about}')
        vdir = a.work / 'spv' / pipe
        out = files / 'spv' / pipe
        out.mkdir(parents=True, exist_ok=True)
        if variants and (vdir / 'variants.txt').is_file():
            for line in (vdir / 'variants.txt').read_text().splitlines():
                name, f, about_v = line.split(' ', 2)
                if name == 'rolled':   # LLPC 26.10.1 crashes compiling it (RX 9070 XT, kit 3's first run)
                    continue
                shutil.copy2(vdir / f, out / f)
                kit.append(f'variant {name} spv/{pipe}/{f} {about_v}')
        else:
            if not (out / 'base.spv').is_file():
                shutil.copy2(a.work / 'dlssnr-amd' / 'shaders-rdna3' / spv, out / 'base.spv')
            kit.append(f'variant base spv/{pipe}/base.spv the pipeline as released')
    (files / 'kit.txt').write_text('\n'.join(kit) + '\n')
    shutil.copy2(a.work / 'kreplay.exe', files / 'kreplay.exe')
    shutil.copy2(HERE / 'run-test.bat', root / 'run-test.bat')
    shutil.copy2(HERE / 'README.txt', root / 'README.txt')
    n = sum(1 for _ in root.rglob('*') if _.is_file())
    size = sum(p.stat().st_size for p in root.rglob('*') if p.is_file())
    print(f'{root}: {n} files, {size / 1e6:.1f} MB')


if __name__ == '__main__':
    main()
