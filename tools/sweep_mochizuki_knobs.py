#!/usr/bin/env python3
"""Sweep the work-split knobs of the Windows (LLPC) mochizuki network, one pipelines.json cell per build.

    python tools/sweep_mochizuki_knobs.py <out dir> [cell ...] [--res 1080|1440] [--runs 3] [--lock]
        [--cell NAME=PIPELINE:KNOB=VALUE[,PIPELINE:KNOB=VALUE...]] [--dll DLL] [--no-isa] [--no-bench]
        [--keep-images] [--refresh] [--list]

A cell is a set of knob edits to third_party/mochizuki/windows/shaders/rdna4/pipelines.json: a built-in one
(--list prints them), CELL:PIPELINE for one pipeline of a built-in cell, or a --cell definition. With no cell
named, the --cell definitions run, or every built-in cell when there are none; a --cell definition must be named
when other cells are. "ref" is the unedited table. It is built first, every cell is measured against it, and
naming it as a cell runs it against itself (an A/A of the fswin kernels). Per cell:
  1. <out>/up holds a copy of third_party/mochizuki/{windows,toolchain}, made once. It refuses to run once
     third_party has changed; --refresh then copies it again. A new copy (after --refresh, or when <out>/up was
     deleted or its copy interrupted) drops every build and ISA dump in <out>/cells: they were made from the
     old one. The copy's pipelines.json is restored and the cell's knobs are set in it; the file keeps its
     layout.
  2. python <out>/up/windows/build/build_network.py rdna4 --out <out>/cells/<cell>/shaders. The files that
     differ from the reference build are listed. An edit that changes no SPV is inert and is not measured.
  3. nr_graph --per-layer (exports/mochizuki-work/nrgraph, with the arguments of
     exports/mochizuki-work/runs/nrgraph1080_perlayer_run1.txt) with --spv-dir, interleaved with the reference:
     ref, cell, ref, cell, ... --runs times each. Every --out-image must equal the reference's first, byte for
     byte.
  4. mz_bench <dll> 100 [2560 1440] --dump-dir, with the cell's shaders beside a copy of <dll> (default
     exports/mochizuki-work/wave1-final), then mz_compare.py --exact against exports/mochizuki-work/golden
     (b1080_r1, or b1440 at 1440p).
  5. The P1 ISA kit (exports/mochizuki-work/isa/dump-isa.cmd) for VGPR, scratch, LDS and waves per SIMD, at
     1080p only, once per build (a rebuild, such as a cell name reused with other knobs, drops the old dump).
  6. One row per edited pipeline is appended to <out>/sweep.csv. <out>/cells/<cell>/<res>/summary.txt has the
     per-kernel table.

A pipeline WINS when its dispatches are at least 2% faster and every image is byte-identical. Adopt a win
only after it is confirmed with --res 1440 (third_party/mochizuki/UPSTREAM.md records adopted cells).

--lock takes exports/mochizuki-work/locks/rtbuild around the copy and each build (both CPU-heavy), and
locks/gpu around each cell's GPU runs, one cell at a time.
Only NR_EXPAND_GROUP, NR_FWAVES and NR_HWAVES can be set. The knobs in shader-constants.txt (NR_FFWD_WGW, the
gemm MT/NT, ...) are compiled into the host too, and a pipeline-only change would break it.
"""
import argparse
import csv
import datetime
import hashlib
import json
import math
import os
import re
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
TP = REPO / 'third_party' / 'mochizuki'
WORK = REPO / 'exports' / 'mochizuki-work'
HARN = REPO / 'exports' / 'mochizuki-harness'
NG = WORK / 'nrgraph'
MODEL = WORK / 'baseline' / 'dlssnr-amd'
PJ = Path('windows/shaders/rdna4/pipelines.json')

KNOBS = ('NR_EXPAND_GROUP', 'NR_FWAVES', 'NR_HWAVES')
RES = {'1080': (1920, 1080, 'b1080_r1'), '1440': (2560, 1440, 'b1440')}
WIN_PCT = -2.0

# The P6 cells, in order.
CELLS = {
    # N1: g_fswinpds256 is the only pipeline that spills (256 VGPR, 16 B scratch) and the only 256-wide
    # head-split one still at EG=4; its non-persistent sibling fswindsp256 is at 2.
    'N1': [('fswinpds256', 'NR_EXPAND_GROUP', '2')],
    # (a) EG=2 on the head-split kernels still at 4.
    'a': [('fswinfusedup128', 'NR_EXPAND_GROUP', '2'), ('fswinfusedup64', 'NR_EXPAND_GROUP', '2'),
          ('fswindsp128', 'NR_EXPAND_GROUP', '2')],
    # (b) EG=4 back where round 9 chose 2, on an older driver.
    'b': [('fswinp64', 'NR_EXPAND_GROUP', '4'), ('fswinp128', 'NR_EXPAND_GROUP', '4'),
          ('fswindsp64', 'NR_EXPAND_GROUP', '4'), ('fswinpup256', 'NR_EXPAND_GROUP', '4')],
    # (c) the one pipeline where NR_FWAVES is live: NR_HWAVES=1 makes it inert at C>=64 (fswin_t.comp,
    # NR_THREADS), and the DS, fused-upsample and K_REGS bodies pin it to 1 at C=32.
    'c': [('fswin32', 'NR_FWAVES', '2')],
    # (d) the token split instead of the head split at C=64.
    'd': [('fswinp64', 'NR_HWAVES', '0'), ('fswinp64', 'NR_FWAVES', '2')],
}

ROW = re.compile(r'^(\S+)/(\S+)\s+(\d+)\s+(\d+)\s+([0-9.]+)\s+([0-9.]+)\s+[0-9.]+%\s*$')
TOTAL = re.compile(r'^(\d+) dispatches, one submit: ([0-9.]+) ms', re.M)
CSV_FIELDS = ['date', 'cell', 'res', 'pipeline', 'edits', 'spv_changed', 'runs', 'ref_us', 'cell_us', 'delta_us',
              'delta_pct', 'ref_us_runs', 'cell_us_runs', 'ref_total_ms', 'cell_total_ms', 'total_delta_ms',
              'images_identical', 'runtime_exact', 'vgpr', 'scratch_b', 'lds_b', 'waves_simd', 'rga_max_live',
              'verdict']


def log(msg):
    print(msg, flush=True)


class Lock:
    """exports/mochizuki-work/locks/<name>, the mkdir lock every package uses (stale after 60 min)."""

    def __init__(self, name, on):
        self.path, self.on = WORK / 'locks' / name, on
        self.label = 'P6-sweep pid %d' % os.getpid()

    def __enter__(self):
        if not self.on:
            return self
        waited = False
        while True:
            try:
                self.path.mkdir()
                (self.path / 'owner.txt').write_text('%s %s\n' % (self.label, datetime.datetime.now().isoformat()))
                break
            except FileExistsError:
                try:
                    age = time.time() - (self.path / 'owner.txt').stat().st_mtime
                except OSError:
                    age = None
                if age is not None and age > 3600:
                    shutil.rmtree(self.path, ignore_errors=True)
                    continue
                if not waited:
                    log('waiting for lock %s' % self.path.name)
                    waited = True
                time.sleep(20)
        return self

    def __exit__(self, *exc):
        if not self.on:
            return
        try:
            mine = (self.path / 'owner.txt').read_text().startswith(self.label + ' ')
        except OSError:
            mine = False
        if mine:
            shutil.rmtree(self.path, ignore_errors=True)


def same_tree(a, b, skip=()):
    fa = {p.relative_to(a).as_posix() for p in a.rglob('*') if p.is_file() and '__pycache__' not in p.parts}
    fb = {p.relative_to(b).as_posix() for p in b.rglob('*') if p.is_file() and '__pycache__' not in p.parts}
    fa, fb = fa - set(skip), fb - set(skip)
    return fa == fb and all((a / f).read_bytes() == (b / f).read_bytes() for f in fa)


def diff_tree(a, b):
    """Relative paths of the files that differ between two build outputs, or exist on one side only."""
    files = {p.relative_to(a).as_posix() for p in a.rglob('*') if p.is_file()}
    files |= {p.relative_to(b).as_posix() for p in b.rglob('*') if p.is_file()}
    return sorted(f for f in files
                  if not ((a / f).is_file() and (b / f).is_file() and (a / f).read_bytes() == (b / f).read_bytes()))


def prepare_up(out, refresh, lock):
    """The scratch copy of third_party (build_network.py resolves its root at parents[2]). Every build and ISA
    dump in <out>/cells was made from it, so a new copy drops them all."""
    up = out / 'up'
    with Lock('rtbuild', lock):
        # pipelines.pristine.json is copied last: without it an earlier copy was interrupted.
        if up.exists() and (refresh or not (up / 'pipelines.pristine.json').is_file()):
            shutil.rmtree(up)
        if not up.exists():
            # The builds in <out>/cells came from an earlier copy (before --refresh, or before <out>/up was
            # deleted), maybe of an older third_party: rebuild them, and drop what was read from them (the ISA
            # kit's counts, the reference's check against the DLL's shaders). The timing measurements stay.
            stale = [p for sub in ('shaders', 'isa') for p in (out / 'cells').glob('*/' + sub)]
            for p in stale:
                shutil.rmtree(p)
            (out / 'cells' / 'ref' / 'vs_dll_shaders.txt').unlink(missing_ok=True)
            if stale:
                log('dropped %d shader builds and ISA dumps of an earlier copy: %s'
                    % (len(stale), ' '.join(p.relative_to(out).as_posix() for p in stale)))
            shutil.copytree(TP / 'windows', up / 'windows', ignore=shutil.ignore_patterns('__pycache__'))
            shutil.copytree(TP / 'toolchain', up / 'toolchain')
            shutil.copy2(up / PJ, up / 'pipelines.pristine.json')
            log('copied %s to %s' % (TP, up))
            return up
        pristine = (up / 'pipelines.pristine.json').read_bytes()
        if pristine != (TP / PJ).read_bytes() or \
                not same_tree(TP / 'windows', up / 'windows', [PJ.relative_to('windows').as_posix()]) or \
                not same_tree(TP / 'toolchain', up / 'toolchain'):
            sys.exit('third_party/mochizuki changed since %s was copied: rerun with --refresh' % up)
    return up


def apply_edits(text, edits):
    """Set each (pipeline, knob, value) in the pipelines.json text, keeping the layout; returns the new text."""
    want = json.loads(text)
    for pipe, key, val in edits:
        if key not in KNOBS:
            sys.exit('%s is not a work-split knob (%s); shader-constants knobs are shared with the host'
                     % (key, ', '.join(KNOBS)))
        if pipe not in want['pipelines']:
            sys.exit('no pipeline %s in pipelines.json' % pipe)
        d = want['pipelines'][pipe]['defines']
        hit = [i for i, x in enumerate(d) if x.split('=', 1)[0] == key]
        if hit:
            d[hit[0]] = '%s=%s' % (key, val)
        else:
            d.append('%s=%s' % (key, val))
        pat = re.compile(r'^(  "%s": \{"source": "[^"]+",\r?\n   "defines": \[)([^\]]*)(\]\},?)\r?$'
                         % re.escape(pipe), re.M)
        m = list(pat.finditer(text))
        if len(m) != 1:
            sys.exit('pipelines.json: the entry of %s matched %d times' % (pipe, len(m)))
        m = m[0]
        items, n = re.subn(r'"%s(=[^"]*)?"' % re.escape(key), '"%s=%s"' % (key, val), m.group(2))
        if n > 1:
            sys.exit('%s: %s is defined %d times' % (pipe, key, n))
        if n == 0:
            items += ', "%s=%s"' % (key, val)
        text = text[:m.start(2)] + items + text[m.end(2):]
    if json.loads(text) != want:
        sys.exit('pipelines.json edit check failed')
    return text


def build(up, edits, dst):
    """Restore the copy's pipelines.json, apply the edits, build into dst. Returns (ok, log text, seconds)."""
    text = (up / 'pipelines.pristine.json').read_text()
    (up / PJ).write_text(apply_edits(text, edits) if edits else text)
    t0 = time.time()
    r = subprocess.run([sys.executable, str(up / 'windows/build/build_network.py'), 'rdna4', '--out', str(dst)],
                       capture_output=True, text=True)
    (up / PJ).write_text(text)
    return r.returncode == 0, r.stdout + r.stderr, time.time() - t0


def parse_ng(text):
    rows = [(m.group(2), int(m.group(4)), float(m.group(5)), m.group(1))
            for m in (ROW.match(line.strip()) for line in text.splitlines()) if m]
    m = TOTAL.search(text)
    return rows, float(m.group(2)) if m else None


def kernel_us(rows, kernel):
    """Microseconds a frame spent in one kernel (sum of us each x n over its per-layer rows), None if absent."""
    v = [n * us for k, n, us, _ in rows if k == kernel]
    return sum(v) if v else None


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run_ng(res, shaders, cache, img, txt):
    w, h, _ = RES[res]
    cmd = [str(NG / 'nr_graph.exe'), '--plan', str(NG / ('plan%s.txt' % res)), '--unpacked', str(MODEL / 'model'),
           '--model-pack', str(MODEL / 'dlssnr.bin'), '--spv-dir', str(shaders), '--host-boundary', '--reuse',
           '--source-width', str(w), '--source-height', str(h), '--accumulation', 'fp32',
           '--pipeline-cache', str(cache), '--warmup', '20', '--repeats', '100', '--out-image', str(img),
           '--per-layer']
    with open(txt, 'w', encoding='utf-8', errors='replace') as f:
        f.write(' '.join(cmd) + '\n')
        f.flush()
        rc = subprocess.run(cmd, cwd=str(txt.parent), stdout=f, stderr=subprocess.STDOUT).returncode
    rows, total = parse_ng(txt.read_text(encoding='utf-8', errors='replace'))
    if rc != 0 or total is None:
        return None
    return rows, total


def stats_file(p):
    d = {}
    if p.is_file():
        for line in p.read_text().splitlines():
            if '=' in line:
                k, v = line.split('=', 1)
                d[k.strip()] = v.strip()
    return d


def occupancy(s):
    """Waves per SIMD from the driver's stats, as P4/occupancy.py computes them: gfx1201 wave32 allocates VGPRs
    in blocks of 24 out of 1536, and 128 KB of LDS a WGP is shared by the workgroups over its 4 SIMDs."""
    if 'numUsedVgprs' not in s:
        return None
    v = int(s['numUsedVgprs'])
    vlim = min(16, int(s.get('numPhysicalVgprs', 1536)) // (math.ceil(v / 24) * 24))
    x, y, z = (int(t) for t in s['computeWorkGroupSize'].split('x'))
    waves = math.ceil(x * y * z / 32)
    lds = int(s['ldsSizePerLocalWorkGroup'])
    return min(vlim, (131072 // lds) * waves // 4 if lds else 16)


def run_isa(shaders, dst, keep):
    """The P1 kit on one shader build; keeps .stats, RGA's tables and the .isa of the kernels in keep."""
    dst.parent.mkdir(parents=True, exist_ok=True)
    with open(str(dst) + '.log', 'w', encoding='utf-8', errors='replace') as f:
        rc = subprocess.run(['cmd', '/c', str(WORK / 'isa' / 'dump-isa.cmd'), str(dst), str(shaders)],
                            stdout=f, stderr=subprocess.STDOUT).returncode
    for p in list(dst.glob('*.elf')) + [p for p in dst.glob('*.isa') if p.stem not in keep]:
        p.unlink()
    for p in (dst / 'rga').glob('*'):
        if not any(p.name.startswith('gfx1201_%s_comp.' % k) for k in keep):
            p.unlink()
    for p in (dst / 'rt' / 'dump.bin', dst / 'rt' / 'dlssnr-amd' / 'pipeline.cache'):
        if p.is_file():
            p.unlink()
    return rc == 0


def isa_info(isa_dir, kernel):
    s = stats_file(isa_dir / ('g_%s.stats' % kernel))
    rga = {}
    p = isa_dir / 'rga_crosscheck.csv'
    if p.is_file():
        with open(p, newline='') as f:
            rga = {r['pipeline']: r for r in csv.DictReader(f)}
    r = rga.get('g_' + kernel, {})
    return {'vgpr': s.get('numUsedVgprs', '?'), 'scratch_b': s.get('scratchMemUsageInBytes', '?'),
            'lds_b': s.get('ldsSizePerLocalWorkGroup', '?'), 'waves_simd': occupancy(s) if s else '?',
            'rga_max_live': r.get('max_live_vgprs', '?')}


def prepare_rt(out, dll):
    """<out>/rt: a copy of the runtime DLL with its model and warm pipeline.cache; shaders are set per cell."""
    rt = out / 'rt'
    src = dll.parent / 'dlssnr-amd'
    if not (rt / 'MochizukiNrRuntime.dll').is_file():
        (rt / 'dlssnr-amd').mkdir(parents=True, exist_ok=True)
        shutil.copy2(dll, rt / 'MochizukiNrRuntime.dll')
        try:
            os.link(src / 'dlssnr.bin', rt / 'dlssnr-amd' / 'dlssnr.bin')
        except OSError:
            shutil.copy2(src / 'dlssnr.bin', rt / 'dlssnr-amd' / 'dlssnr.bin')
        if (src / 'pipeline.cache').is_file():
            shutil.copy2(src / 'pipeline.cache', rt / 'dlssnr-amd' / 'pipeline.cache')
    return rt


def run_bench(rt, shaders, res, dst):
    """mz_bench with the cell's shaders, then mz_compare --exact against the golden. Returns yes/no/fail."""
    shd = rt / 'dlssnr-amd' / 'shaders'
    if shd.exists():
        shutil.rmtree(shd)
    shutil.copytree(shaders, shd)
    if dst.exists():
        shutil.rmtree(dst)
    dst.mkdir(parents=True)
    w, h, golden = RES[res]
    cmd = [str(HARN / 'mz_bench.exe'), str(rt / 'MochizukiNrRuntime.dll'), '100']
    if res != '1080':
        cmd += [str(w), str(h)]
    cmd += ['--dump-dir', str(dst / 'dump')]
    with open(dst / 'mz_bench.txt', 'w', encoding='utf-8', errors='replace') as f:
        rc = subprocess.run(cmd, cwd=str(dst), stdout=f, stderr=subprocess.STDOUT).returncode
    if rc != 0:
        return 'fail (mz_bench exit %d)' % rc
    with open(dst / 'mz_compare.txt', 'w', encoding='utf-8', errors='replace') as f:
        rc = subprocess.run([sys.executable, str(HARN / 'mz_compare.py'), str(WORK / 'golden' / golden),
                             str(dst / 'dump'), '--exact'], stdout=f, stderr=subprocess.STDOUT).returncode
    return 'yes' if rc == 0 else 'no'


def resolve_cells(names, custom):
    """[(cell name, edits)] for the cells named on the command line. With no names, the --cell definitions run,
    or the built-in cells when there are none. A --cell definition that is not named is an error."""
    cells = {}
    for spec in custom:
        name, _, body = spec.partition('=')
        if not re.fullmatch(r'[A-Za-z0-9_-]+', name) or name == 'ref':
            sys.exit('--cell %s: the name must be letters, digits, _ or -, and not ref' % spec)
        if name in cells:
            sys.exit('--cell %s is defined twice' % name)
        edits = []
        for item in body.split(','):
            m = re.fullmatch(r'(\w+):(\w+)=(\w+)', item.strip())
            if not m:
                sys.exit('--cell %s: expected NAME=PIPELINE:KNOB=VALUE[,...]' % spec)
            edits.append(m.groups())
        cells[name] = edits
    names = names or list(cells) or list(CELLS)
    unnamed = [n for n in cells if n not in names]
    if unnamed:
        sys.exit('--cell %s is defined but not in the cells to run (%s): name it, or name no cells'
                 % (' '.join(unnamed), ' '.join(names)))
    out = []
    for n in names:
        base, _, pipe = n.partition(':')
        if n in cells:
            out.append((n, cells[n]))
        elif base == 'ref':
            out.append(('ref', []))
        elif base in CELLS:
            e = [x for x in CELLS[base] if not pipe or x[0] == pipe]
            if not e:
                sys.exit('cell %s has no pipeline %s' % (base, pipe))
            out.append((n.replace(':', '-'), e))
        else:
            sys.exit('unknown cell %s (built-in: %s)' % (n, ' '.join(CELLS)))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out', type=Path)
    ap.add_argument('cells', nargs='*')
    ap.add_argument('--cell', action='append', default=[], help='NAME=PIPELINE:KNOB=VALUE[,...]')
    ap.add_argument('--res', choices=sorted(RES), default='1080')
    ap.add_argument('--runs', type=int, default=3)
    ap.add_argument('--dll', type=Path, default=WORK / 'wave1-final' / 'MochizukiNrRuntime.dll')
    ap.add_argument('--lock', action='store_true')
    ap.add_argument('--no-isa', action='store_true')
    ap.add_argument('--no-bench', action='store_true')
    ap.add_argument('--keep-images', action='store_true')
    ap.add_argument('--refresh', action='store_true')
    ap.add_argument('--list', action='store_true')
    a = ap.parse_args()
    if a.list:
        for n, e in CELLS.items():
            log('%-3s %s' % (n, '  '.join('%s:%s=%s' % x for x in e)))
        return 0
    cells = resolve_cells(a.cells, a.cell)
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    up = prepare_up(out, a.refresh, a.lock)

    ref = out / 'cells' / 'ref'
    if not (ref / 'shaders' / 'g_fswin32.spv').is_file():
        # An ISA dump of an earlier reference build would be read as this one's.
        shutil.rmtree(ref / 'isa', ignore_errors=True)
        with Lock('rtbuild', a.lock):
            ok, text, secs = build(up, [], ref / 'shaders')
        (ref / 'build.log').write_text(text)
        if not ok:
            sys.exit('reference build failed, see %s' % (ref / 'build.log'))
        shipped = a.dll.parent / 'dlssnr-amd' / 'shaders'
        d = diff_tree(shipped, ref / 'shaders') if shipped.is_dir() else ['(no %s)' % shipped]
        note = 'identical to %s' % shipped if not d else 'DIFFERENT from %s: %s' % (shipped, ' '.join(d))
        (ref / 'vs_dll_shaders.txt').write_text(note + '\n')
        log('ref built in %.0f s, %s' % (secs, note))
    if not a.no_isa and a.res == '1080' and not (ref / 'isa' / 'isa_counts.csv').is_file():
        with Lock('gpu', a.lock):
            log('ref: ISA kit')
            fswin = {p.stem[2:] for p in (ref / 'shaders').glob('g_fswin*.spv')}
            run_isa(ref / 'shaders', ref / 'isa', {'g_' + k for k in fswin})
    rt = None if a.no_bench else prepare_rt(out, a.dll.resolve())

    csv_path = out / 'sweep.csv'
    new_csv = not csv_path.is_file()
    for name, edits in cells:
        cdir = out / 'cells' / name
        rdir = cdir / a.res
        rdir.mkdir(parents=True, exist_ok=True)
        pipes = list(dict.fromkeys(p for p, _, _ in edits))
        edit_text = {p: ' '.join('%s=%s' % (k, v) for q, k, v in edits if q == p) for p in pipes}
        log('== cell %s (%s): %s' % (name, a.res, '; '.join('%s %s' % (p, edit_text[p]) for p in pipes) or 'A/A'))
        rows = []
        if name == 'ref':
            shaders, changed = ref / 'shaders', []
            pipes = sorted({p.stem[2:] for p in shaders.glob('g_fswin*.spv')})
            edit_text = {p: '-' for p in pipes}
        else:
            shaders = cdir / 'shaders'
            built = (shaders / 'g_fswin32.spv').is_file() and (cdir / 'edits.json').is_file() and \
                json.loads((cdir / 'edits.json').read_text()) == [list(e) for e in edits]
            if not built:
                # The ISA kit runs only when isa_counts.csv is missing: drop the dump of an earlier build.
                (cdir / 'edits.json').unlink(missing_ok=True)
                shutil.rmtree(cdir / 'isa', ignore_errors=True)
                with Lock('rtbuild', a.lock):
                    ok, text, secs = build(up, edits, shaders)
                (cdir / 'build.log').write_text(text)
                if not ok:
                    err = next((ln for ln in text.splitlines() if 'ERROR' in ln or 'error' in ln), text[-200:])
                    log('  build FAILED: %s' % err.strip())
                    rows = [dict(cell=name, pipeline=p, edits=edit_text[p], verdict='build failed: ' + err.strip())
                            for p in pipes]
                    shutil.rmtree(shaders, ignore_errors=True)
                else:
                    log('  built in %.0f s' % secs)
                    (cdir / 'edits.json').write_text(json.dumps([list(e) for e in edits]))
            changed = diff_tree(ref / 'shaders', shaders) if not rows else []
            (cdir / 'changed.txt').write_text('\n'.join(changed) + '\n')
            if not rows:
                log('  changed vs ref: %s' % (' '.join(changed) or 'nothing'))
        live = [p for p in pipes if name == 'ref' or 'g_%s.spv' % p in changed]
        for p in pipes:
            if not rows and p not in live:
                rows.append(dict(cell=name, pipeline=p, edits=edit_text[p], spv_changed='no',
                                 verdict='inert (SPV unchanged)'))
        if live:
            ref_res, cell_res, imgs = [], [], []
            cache = out / ('nrgraph%s.pipeline.cache' % a.res)
            if not cache.is_file():
                shutil.copy2(NG / 'nrgraph.pipeline.cache', cache)
            with Lock('gpu', a.lock):
                for i in range(1, a.runs + 1):
                    for tag, shd, acc in (('ref', ref / 'shaders', ref_res), ('cell', shaders, cell_res)):
                        img = rdir / ('%s_r%d.bin' % (tag, i))
                        r = run_ng(a.res, shd, cache, img, rdir / ('%s_r%d.txt' % (tag, i)))
                        if r is None:
                            sys.exit('nr_graph failed, see %s' % (rdir / ('%s_r%d.txt' % (tag, i))))
                        acc.append(r)
                        imgs.append((img.name, sha(img)))
                        if not a.keep_images:
                            img.unlink()
                    log('  run %d: total ref %.3f, cell %.3f ms' % (i, ref_res[-1][1], cell_res[-1][1]))
                bench = 'skipped'
                if rt is not None:
                    bench = run_bench(rt, shaders, a.res, rdir / 'bench')
                    if not a.keep_images:
                        shutil.rmtree(rdir / 'bench' / 'dump', ignore_errors=True)
                    for p in (rdir / 'bench' / 'dump.bin',):
                        if p.is_file():
                            p.unlink()
                    log('  runtime mz_compare --exact vs golden: %s' % bench)
                isa_dir = cdir / 'isa'
                if not a.no_isa and a.res == '1080' and name != 'ref' and not (isa_dir / 'isa_counts.csv').is_file():
                    log('  ISA kit')
                    run_isa(shaders, isa_dir, {'g_' + p for p in live})
            same = sum(h == imgs[0][1] for _, h in imgs)
            ident = 'yes %d/%d' % (same, len(imgs)) if same == len(imgs) else 'NO %d/%d' % (same, len(imgs))
            (rdir / 'images.txt').write_text('\n'.join('%s %s' % x for x in imgs) + '\n')
            rt_tot = [t for _, t in ref_res]
            ce_tot = [t for _, t in cell_res]
            # The frame drifts over a batch (clocks, heat): the total is judged on the median pair delta.
            tot_delta = statistics.median(c - r for r, c in zip(rt_tot, ce_tot))
            summ = ['cell %s (%s), %d interleaved runs each; edits: %s' % (
                name, a.res, a.runs, '; '.join('%s %s' % (p, edit_text[p]) for p in pipes)),
                'changed files vs ref: %s' % (' '.join(changed) or '-'),
                'nr_graph --out-image vs ref_r1: %s' % ident, 'runtime (mz_bench, mz_compare --exact): %s' % bench,
                'total ms: ref %s -> median %.3f | cell %s -> median %.3f | median pair delta %+.3f' % (
                    '/'.join('%.3f' % t for t in rt_tot), statistics.median(rt_tot),
                    '/'.join('%.3f' % t for t in ce_tot), statistics.median(ce_tot), tot_delta), '',
                '%-50s %3s %-24s %-24s %s' % ('kernel', 'n', 'ref us each (runs) med', 'cell us each (runs) med',
                                              'delta us  %')]
            for fam, k, n in [(f, k, n) for k, n, _, f in ref_res[0][0]]:
                rv = [us for rows_, _ in ref_res for kk, nn, us, ff in rows_ if kk == k and ff == fam]
                cv = [us for rows_, _ in cell_res for kk, nn, us, ff in rows_ if kk == k and ff == fam]
                if not rv or not cv:
                    continue
                rm, cm = statistics.median(rv), statistics.median(cv)
                summ.append('%-50s %3d %-24s %-24s %+7.1f %+5.1f%%%s' % (
                    fam + '/' + k, n, '%s %.1f' % ('/'.join('%.1f' % x for x in rv), rm),
                    '%s %.1f' % ('/'.join('%.1f' % x for x in cv), cm), cm - rm, 100 * (cm - rm) / rm,
                    '  <-' if k in live else ''))
            for p in live:
                rv = [kernel_us(r, p) for r, _ in ref_res]
                cv = [kernel_us(r, p) for r, _ in cell_res]
                row = dict(cell=name, pipeline=p, edits=edit_text[p], spv_changed='yes' if name != 'ref' else '-',
                           runs=a.runs, images_identical=ident, runtime_exact=bench,
                           ref_total_ms='%.3f' % statistics.median(rt_tot),
                           cell_total_ms='%.3f' % statistics.median(ce_tot),
                           total_delta_ms='%+.3f' % tot_delta)
                if None in rv or None in cv:
                    row['verdict'] = 'not dispatched at %s' % a.res
                else:
                    rm, cm = statistics.median(rv), statistics.median(cv)
                    pct = 100 * (cm - rm) / rm
                    row.update(ref_us='%.1f' % rm, cell_us='%.1f' % cm, delta_us='%+.1f' % (cm - rm),
                               delta_pct='%+.2f' % pct, ref_us_runs='/'.join('%.1f' % x for x in rv),
                               cell_us_runs='/'.join('%.1f' % x for x in cv))
                    identical = ident.startswith('yes') and bench in ('yes', 'skipped')
                    if name == 'ref':
                        row['verdict'] = 'A/A'
                    elif not identical:
                        row['verdict'] = 'NOT IDENTICAL'
                    elif pct <= WIN_PCT:
                        row['verdict'] = 'WIN (identical, %.1f%%)' % pct
                    elif pct >= 0:
                        row['verdict'] = 'identical, slower (%+.1f%%)' % pct
                    else:
                        row['verdict'] = 'identical, below the 2%% bar (%+.1f%%)' % pct
                if not a.no_isa and a.res == '1080':
                    r0 = isa_info(ref / 'isa', p)
                    r1 = isa_info(cdir / 'isa', p) if name != 'ref' else r0
                    for k in ('vgpr', 'scratch_b', 'lds_b', 'waves_simd', 'rga_max_live'):
                        row[k] = str(r0[k]) if r0[k] == r1[k] else '%s->%s' % (r0[k], r1[k])
                rows.append(row)
            summ += ['', 'per edited pipeline:']
            summ += ['  %-18s %s' % (r['pipeline'], '  '.join('%s=%s' % (k, r[k]) for k in CSV_FIELDS[4:]
                                                               if k in r)) for r in rows]
            (rdir / 'summary.txt').write_text('\n'.join(summ) + '\n')
        now = datetime.datetime.now().isoformat(timespec='seconds')
        with open(csv_path, 'a', newline='') as f:
            w = csv.DictWriter(f, CSV_FIELDS)
            if new_csv:
                w.writeheader()
                new_csv = False
            for r in rows:
                r.setdefault('res', a.res)
                w.writerow(dict({k: '' for k in CSV_FIELDS}, date=now, **r))
        for r in rows:
            log('  %-18s %s' % (r['pipeline'], '  '.join('%s=%s' % (k, r[k]) for k in
                                                        ('delta_us', 'delta_pct', 'vgpr', 'scratch_b', 'waves_simd',
                                                         'images_identical', 'runtime_exact', 'verdict') if r.get(k))))
    return 0


if __name__ == '__main__':
    sys.exit(main())
