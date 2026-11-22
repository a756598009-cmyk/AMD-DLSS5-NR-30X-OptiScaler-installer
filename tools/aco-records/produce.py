"""Make ACO records for a folder of network SPIR-V.

    produce.py <family> <shaders dir> <out dir> [--all] [--jobs N]

family is RADV's name (navi31, navi32, navi33, phoenix, navi21, navi22, navi23, navi24, vangogh, rembrandt,
raphael_mendocino, gfx1201).
Writes <out>/records/<fnv64 of the SPIR-V>.nrp and <out>/produce-log.txt. Only the files the runtime takes
records for (g_*, temporal_pre_fp32*, temporal_post_fp32*, as nr_pipeline_binary.hpp network_path) unless
--all. Needs the patched Mesa build (aco-tools/build/radv) and nr_aco_capture.exe.
"""
import concurrent.futures as cf, os, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_record

# The work folder with the Mesa build and nr_aco_capture.exe (HANDOFF-rdna3-aco.md, record generation).
TOOLS = os.environ.get('NR_ACO_TOOLS', r'E:\Projetos\opti_rr_dlss5\aco-tools')
RADV = os.path.join(TOOLS, 'build', 'radv', 'src', 'amd', 'vulkan', 'vulkan_radeon.dll')
CAPTURE = os.path.join(TOOLS, 'producer', 'nr_aco_capture.exe')


# RADV's gfx10.3 families (no cooperative matrices): one navi21 set serves them all (HANDOFF-rdna2-aco.md).
GFX103 = ('navi21', 'navi22', 'navi23', 'navi24', 'vangogh', 'rembrandt', 'raphael_mendocino')


def network_path(name):
    return name.startswith(('g_', 'temporal_pre_fp32', 'temporal_post_fp32'))


LAYOUTS = {}   # name -> (push bytes, "b:t b:t ...") from the runtime's MZ_LOG_LAYOUT lines (--layouts <log>)


def load_layouts(log):
    seen = {}
    for line in open(log, errors='replace'):
        if '[nr] layout ' in line:
            line = 'layout ' + line.split('[nr] layout ', 1)[1]
        if not line.startswith('layout '):
            continue
        rest = line.split()[1:]
        name, push, binds = rest[0], int(rest[2]), ' '.join(rest[4:])
        seen.setdefault(name, set()).add((push, binds))
    for name, s in seen.items():
        if len({b for _, b in s}) > 1:
            print('WARNING %s is made with %d set layouts: %s' % (name, len(s), sorted(s)), flush=True)
        LAYOUTS[name] = max(s)   # one record per SPIR-V: the largest push range, the (single) layout


def capture(family, spv, prefix, sentinel):
    env = dict(os.environ, RADV_FORCE_FAMILY=family, NR_ADDR32_HI='0x%x' % sentinel, NR_RECORD_OUT=prefix,
               MESA_SHADER_CACHE_DISABLE='true')
    name = os.path.basename(spv)
    if name in LAYOUTS:
        # A combined image sampler (or sampled image, sampler) before another binding: RADV and the AMD driver size
        # it differently, so every later binding's offset would differ (temporal_pre/post on the RX 9070 XT: wrong).
        types = [int(x.split(':')[1]) for x in LAYOUTS[name][1].split()]
        if any(t in (0, 1, 2) for t in types[:-1]):
            raise make_record.Reject('a sampled image before another binding (descriptor sizes differ from the driver)')
        env['NR_CAPTURE_PUSH'] = str(LAYOUTS[name][0])
        env['NR_CAPTURE_LAYOUT'] = LAYOUTS[name][1]
    elif LAYOUTS:
        raise make_record.Reject('not in the runtime layout log (not used by the network)')
    if family.lower().startswith('gfx12'):
        env['NR_CAPTURE_FP8'] = '1'
    if family.lower() in GFX103:
        env['NR_CAPTURE_NO_COOP'] = '1'
    r = subprocess.run([CAPTURE, RADV, spv, prefix], env=env, capture_output=True, timeout=1800)
    open(prefix + '.err', 'wb').write(r.stderr)
    if r.returncode != 0 or not os.path.exists(prefix + '.txt'):
        raise make_record.Reject('capture failed (%d): %s' % (r.returncode, r.stderr.decode(errors='replace')[-400:]))


def stats(family, spv, prefix):
    """ACO's VGPRs and scratch for a kernel refused before its capture (layout), with its own reflected layout."""
    env = dict(os.environ, RADV_FORCE_FAMILY=family, NR_ADDR32_HI='0x%x' % make_record.SENT_A, NR_RECORD_OUT=prefix,
               MESA_SHADER_CACHE_DISABLE='true')
    if family.lower() in GFX103:
        env['NR_CAPTURE_NO_COOP'] = '1'
    try:
        subprocess.run([CAPTURE, RADV, spv, prefix], env=env, capture_output=True, timeout=1800)
        a = make_record.read_capture(prefix)
        return {'vgpr': a['num_vgprs'], 'scratch': a['scratch_bytes_per_wave'] // 32, 'spilled': a['spilled_vgprs']}
    except Exception:
        return {}


def one(family, spv, out, tmp):
    base = os.path.join(tmp, os.path.basename(spv))
    t0 = time.time()
    try:
        capture(family, spv, base + '.a', make_record.SENT_A)
        capture(family, spv, base + '.b', make_record.SENT_B)
        key = '%016x' % make_record.fnv(open(spv, 'rb').read())
        info = make_record.make(spv, base + '.a', base + '.b', os.path.join(out, 'records', key + '.nrp'))
        info['s'] = round(time.time() - t0, 1)
        return 'OK', info
    except make_record.Reject as e:
        info = {'name': os.path.basename(spv)}
        if not os.path.exists(base + '.a.txt'):
            info.update(stats(family, spv, base + '.s'))
        info['why'] = '"%s"' % e
        return 'REJECT', info
    except subprocess.TimeoutExpired:
        return 'REJECT', {'name': os.path.basename(spv), 'why': 'capture timed out'}


def main():
    args = sys.argv[1:]
    jobs = 6
    if '--jobs' in args:
        i = args.index('--jobs'); jobs = int(args[i + 1]); del args[i:i + 2]
    if '--layouts' in args:
        i = args.index('--layouts'); load_layouts(args[i + 1]); del args[i:i + 2]
    every = '--all' in args
    args = [a for a in args if a != '--all']
    family, src, out = args
    os.makedirs(os.path.join(out, 'records'), exist_ok=True)
    spvs = []
    for root, _, files in os.walk(src):
        for f in sorted(files):
            if f.endswith('.spv') and (every or network_path(f)):
                spvs.append(os.path.join(root, f))
    tmp = tempfile.mkdtemp(prefix='nrcap-')
    lines = []
    with cf.ThreadPoolExecutor(jobs) as ex:
        for status, info in ex.map(lambda s: one(family, s, out, tmp), spvs):
            line = '%s %s' % (status, ' '.join('%s=%s' % kv for kv in info.items()))
            print(line, flush=True)
            lines.append(line)
    ok = sum(l.startswith('OK') for l in lines)
    lines.append('%d of %d records for %s from %s' % (ok, len(spvs), family, src))
    print(lines[-1])
    open(os.path.join(out, 'produce-log.txt'), 'w').write('\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()
