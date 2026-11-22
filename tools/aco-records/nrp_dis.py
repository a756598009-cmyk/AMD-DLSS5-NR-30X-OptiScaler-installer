# Disassemble the code of an NRPAL record (or raw code with a non-record file): nrp_dis.py <file> <mcpu> [out.s]
# Uses llvm-mc from the HIP SDK (has the AMDGPU target).
import struct, subprocess, sys, os
MC = os.path.join(os.environ.get('HIP_PATH', r'C:\Program Files\AMD\ROCm\7.2'), 'bin', 'llvm-mc.exe')
f, cpu = sys.argv[1], sys.argv[2]
b = open(f, 'rb').read()
if b[:6] == b'NRPAL0':
    n = struct.unpack_from('<I', b, 68)[0]; ex = struct.unpack_from('<I', b, 36)[0]
    code = b[96 + 8 * n:96 + 8 * n + ex]
else:
    code = b
hexs = ' '.join('0x%02x' % x for x in code)
out = subprocess.run([MC, '-arch=amdgcn', '-mcpu=' + cpu, '--disassemble', '-show-encoding'],
                     input=hexs.encode(), capture_output=True).stdout.decode(errors='replace')
if len(sys.argv) > 3: open(sys.argv[3], 'w').write(out)
else: sys.stdout.write(out)
