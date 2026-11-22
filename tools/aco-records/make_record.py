"""Make an NRPAL record from a RADV capture (nr_aco_capture.exe + the patched RADV).

    make_record.py <shader.spv> <capture prefix A> <capture prefix B> <out.nrp>

A and B are two captures of the same SPIR-V with two different NR_ADDR32_HI sentinels (SENT_A, SENT_B).
Where the two code streams differ must be exactly the literal dwords that hold the sentinel; each of those
instructions is re-encoded with m0 in place of the literal (llvm-mc) and padded with s_nop 0, so every
offset in the code stays where ACO put it. m0 carries the PAL address32 high word (the prologue sets it).

Record layout (nr_pal_binary.hpp, nr::pal::Record): NRPAL001 for gfx12 (as upstream's), NRPAL002 for
gfx10.3/gfx11 with the code object machine and per-lane scratch bytes.
Exit 3 = this shader cannot be a record (reason on stderr); the caller then leaves it to the driver.
"""
import os, struct, subprocess, sys

SENT_A, SENT_B = 0x5EADB17D, 0x6B1E2C49
MC = os.path.join(os.environ.get('HIP_PATH', r'C:\Program Files\AMD\ROCm\7.2'), 'bin', 'llvm-mc.exe')
FAMILY = {  # RADV family -> (llvm cpu, ELF e_flags machine)
    'NAVI31': ('gfx1100', 0x41), 'NAVI32': ('gfx1101', 0x46), 'NAVI33': ('gfx1102', 0x47),
    'PHOENIX': ('gfx1103', 0x44), 'GFX1150': ('gfx1150', 0x43), 'GFX1151': ('gfx1151', 0x4a),
    'NAVI21': ('gfx1030', 0x36), 'NAVI22': ('gfx1031', 0x37), 'NAVI23': ('gfx1032', 0x38),
    'NAVI24': ('gfx1034', 0x3e), 'VANGOGH': ('gfx1033', 0x39), 'REMBRANDT': ('gfx1035', 0x3d),
    'RAPHAEL_MENDOCINO': ('gfx1036', 0x45), 'GFX1200': ('gfx1200', 0x48), 'GFX1201': ('gfx1201', 0x4e),
}
UD_PUSH, UD_INLINE, UD_GRID = 1, 2, 19


class Reject(Exception):
    pass


def fnv(b):
    h = 1469598103934665603
    for x in b:
        h = ((h ^ x) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def read_capture(prefix):
    info = {'set': {}, 'ud': {}}
    for line in open(prefix + '.txt'):
        k, *v = line.split()
        if k == 'set':
            info['set'][int(v[0])] = (int(v[1]), int(v[2]))
        elif k == 'ud':
            info['ud'][int(v[0])] = (int(v[1]), int(v[2]))
        elif k == 'block':
            info['block'] = tuple(int(x) for x in v)
        elif k == 'family':
            info[k] = v[0]
        else:
            info[k] = int(v[0], 0)
    info['code'] = open(prefix + '.code', 'rb').read()
    if os.path.exists(prefix + '.err'):
        for line in open(prefix + '.err', errors='replace'):
            if line.startswith('NR_M0 '):
                info['m0'] = {k: int(v) for k, v in (x.split('=') for x in line.split()[1:])}
    return info


def disasm(code, cpu):
    hexs = ' '.join('0x%02x' % x for x in code)
    r = subprocess.run([MC, '-arch=amdgcn', '-mcpu=' + cpu, '--disassemble', '-show-encoding'],
                       input=hexs.encode(), capture_output=True)
    dec = []
    for line in r.stdout.decode(errors='replace').splitlines():
        if '; encoding: [' not in line:
            continue
        text, enc = line.split('; encoding: [')
        dec.append((text.strip(), bytes(int(x, 16) for x in enc.rstrip(']').split(','))))
    # Walk the code against the decoded stream: a dword this llvm-mc cannot decode (newer opcodes) becomes
    # ('?', dword) and the walk goes on with the next decoded instruction.
    out, pos, i = [], 0, 0
    while pos < len(code):
        if i < len(dec) and code[pos:pos + len(dec[i][1])] == dec[i][1]:
            out.append(dec[i]); pos += len(dec[i][1]); i += 1
        else:
            out.append(('?', code[pos:pos + 4])); pos += 4
    if i != len(dec):
        raise Reject('disassembly out of step with the code')
    return out


def assemble(text, cpu):
    r = subprocess.run([MC, '-arch=amdgcn', '-mcpu=' + cpu, '-show-encoding'], input=(text + '\n').encode(),
                       capture_output=True)
    for line in r.stdout.decode(errors='replace').splitlines():
        if '; encoding: [' in line:
            enc = line.split('; encoding: [')[1].rstrip(']')
            return bytes(int(x, 16) for x in enc.split(','))
    raise Reject('llvm-mc cannot encode: %s (%s)' % (text, r.stderr.decode(errors='replace').strip()))


def m0_code(a, b, cpu):
    """ACO itself encodes the sentinel as m0 (patch aco_assembler.cpp, NR_M0 line on its stderr). The two
    captures, made with different sentinels, must then be byte-identical: the sentinel served only as the
    address high word, never in arithmetic."""
    if a['code'] != b['code'] or a['exec_size'] != b['exec_size']:
        raise Reject('the two captures differ: the address word is used beyond a pointer')
    m = a.get('m0')
    if not m:
        raise Reject('no NR_M0 report from ACO')
    if m['writes'] or m['constdata']:
        raise Reject('the code writes m0 or has constant data (%s)' % m)
    return a['code'][:a['exec_size']], (m['subst'], 0)


def m0_code_llvm(a, b, cpu):
    """Fallback without the ACO patch: re-encode each sentinel literal with llvm-mc."""
    ca, cb = a['code'][:a['exec_size']], b['code'][:b['exec_size']]
    if len(ca) != len(cb) or a['code'][a['exec_size']:] != b['code'][b['exec_size']:]:
        raise Reject('the two captures differ in size')
    ins = disasm(ca, cpu)
    out, pos, fixed = bytearray(), 0, 0
    lit_a, lit_b = struct.pack('<I', SENT_A), struct.pack('<I', SENT_B)
    for text, enc in ins:
        other = cb[pos:pos + len(enc)]
        if text == '?' and (enc != other or enc == lit_a):
            raise Reject('sentinel in an instruction llvm-mc cannot decode at 0x%x' % pos)
        if enc != other:
            # Only the trailing literal may differ, and it must be the sentinel in both.
            if len(enc) < 8 or enc[:-4] != other[:-4] or enc[-4:] != lit_a or other[-4:] != lit_b:
                raise Reject('captures differ outside a sentinel literal at 0x%x: %s' % (pos, text))
            t = text.replace('0x%x' % SENT_A, 'm0')
            if t == text:
                raise Reject('sentinel literal not in operand text at 0x%x: %s' % (pos, text))
            new = assemble(t, cpu)
            if len(new) != len(enc) - 4:
                raise Reject('re-encoding with m0 changed size at 0x%x: %s' % (pos, t))
            out += new + struct.pack('<I', 0xBF800000)   # s_nop 0 keeps every offset
            fixed += 1
        else:
            if lit_a in enc and (len(enc) % 4 == 0) and enc[-4:] == lit_a:
                raise Reject('sentinel literal identical in both captures at 0x%x' % pos)
            out += enc
        pos += len(enc)
    # m0 must hold the address word for the whole program.
    for text, _ in disasm(bytes(out), cpu):
        op = text.split(None, 1)
        if len(op) == 2 and op[1].split(',')[0].strip() == 'm0':
            if not text.startswith(('s_cmp', 's_bitcmp')):
                raise Reject('the code writes m0: ' + text)
        if text.startswith(('s_getpc', 's_setpc', 's_swappc', 's_call')):
            raise Reject('PC-relative or call code: ' + text)
    unknown = sum(t == '?' for t, _ in ins)
    return bytes(out), (fixed, unknown)


def make(spv_path, pa, pb, out_path):
    spv = open(spv_path, 'rb').read()
    a, b = read_capture(pa), read_capture(pb)
    fam = a['family'].upper()
    if fam not in FAMILY:
        raise Reject('unknown family ' + fam)
    cpu, mach = FAMILY[fam]
    gfx12 = cpu.startswith('gfx12')
    gfx10 = cpu.startswith('gfx10')
    if a['wave_size'] != 32:
        raise Reject('wave%d' % a['wave_size'])
    exec_code, (fixed, unknown) = m0_code(a, b, cpu)
    tail = a['code'][a['exec_size']:]
    for i in range(0, len(tail), 4):
        if tail[i:i + 4] != b'\x00\x00\x9f\xbf':
            raise Reject('constant data after the code')
    code = exec_code + tail
    if set(a['set']) - {0}:
        raise Reject('descriptor sets other than 0')
    if 0 not in a['set'] or a['set'][0][1] != 1:
        raise Reject('no 32-bit set 0 pointer')
    ud = dict(a['ud'])
    push = ud.pop(UD_PUSH, (-1, 0))[0]
    grid = -1
    if UD_GRID in ud:
        g = ud.pop(UD_GRID)
        if g[1] != 3:
            raise Reject('grid size as a pointer')
        grid = g[0]
    inlines = []
    if UD_INLINE in ud:
        start, n = ud.pop(UD_INLINE)
        mask = a['inline_push_const_mask']
        dws = [i for i in range(64) if mask >> i & 1]
        if len(dws) != n:
            raise Reject('inline push constant count')
        inlines = [(start + k, dw) for k, dw in enumerate(dws)]
    if ud:
        raise Reject('unsupported user SGPRs %s' % sorted(ud))
    rsrc1, rsrc2, rsrc3 = a['rsrc1'], a['rsrc2'], a['rsrc3']
    user = a['num_user_sgprs']
    if user != (rsrc2 >> 1) & 31:
        raise Reject('user SGPR count differs from rsrc2')
    scratch = a['scratch_bytes_per_wave']
    if scratch and (gfx12 or gfx10):
        raise Reject('scratch on %s: %d bytes a lane, vgpr=%d spilled=%d' % (cpu, scratch // 32, a['num_vgprs'],
                                                                          a['spilled_vgprs']))
    # gfx10.3's container (nr_pal_binary.hpp synthesize) has no IEEE mode field; ACO leaves it off for compute.
    if gfx10 and rsrc1 >> 23 & 1:
        raise Reject('IEEE mode on %s' % cpu)
    wg = a['block'][0] * a['block'][1] * a['block'][2]
    push_bytes = (a['push_constant_size'] + 3) & ~3 if a['loads_push_constants'] else 0
    hdr = bytearray(96)
    hdr[0:8] = b'NRPAL001' if gfx12 else b'NRPAL002'
    struct.pack_into('<QQ', hdr, 8, fnv(spv), fnv(code))
    struct.pack_into('<8I', hdr, 24, rsrc1, rsrc2, rsrc3, a['exec_size'], wg, a['lds_size'], push_bytes, user)
    struct.pack_into('<3i', hdr, 56, a['set'][0][0], push, grid)
    struct.pack_into('<2I', hdr, 68, len(inlines), len(code))
    if not gfx12:
        struct.pack_into('<2I', hdr, 76, mach, scratch // 32)
    body = b''.join(struct.pack('<2i', r, d) for r, d in inlines)
    open(out_path, 'wb').write(bytes(hdr) + body + code)
    return dict(name=os.path.basename(spv_path), key='%016x' % fnv(spv), fixed=fixed, undecoded=unknown, vgpr=a['num_vgprs'],
                lds=a['lds_size'], scratch=scratch // 32, user=user, inlines=len(inlines), push=push,
                grid=grid, exec=a['exec_size'], spilled=a['spilled_vgprs'])


if __name__ == '__main__':
    try:
        print(make(*sys.argv[1:5]))
    except Reject as e:
        print('REJECT %s: %s' % (os.path.basename(sys.argv[1]), e), file=sys.stderr)
        sys.exit(3)
