"""stripspv.py <dir>: the SPIR-V modules under <dir> without their debug instructions (OpSource*, OpString, OpLine,
OpNoLine, OpModuleProcessed, OpMemberName, and OpName but on struct types), in place. The host reads the block names
(nrvk.hpp e4m3_view_bindings: which bindings take the FP16 twins), so those stay; nothing the driver compiles changes."""
import struct, sys
from pathlib import Path

DROP = {2, 3, 4, 6, 7, 8, 317, 330}   # OpSourceContinued, OpSource, OpSourceExtension, OpMemberName, OpString, OpLine, OpNoLine, OpModuleProcessed


def strip(b):
    w = list(struct.unpack(f'<{len(b) // 4}I', b))
    if w[0] != 0x07230203:
        return b
    structs, i = set(), 5
    while i < len(w):
        op, wc = w[i] & 0xFFFF, w[i] >> 16
        if op == 30:
            structs.add(w[i + 1])
        i += wc
    out, i = w[:5], 5
    while i < len(w):
        op, wc = w[i] & 0xFFFF, w[i] >> 16
        if not (op in DROP or (op == 5 and w[i + 1] not in structs)):
            out += w[i:i + wc]
        i += wc
    return struct.pack(f'<{len(out)}I', *out)


n = 0
for p in Path(sys.argv[1]).rglob('*.spv'):
    b = p.read_bytes()
    s = strip(b)
    if s != b:
        p.write_bytes(s)
        n += 1
print(f'{n} modules stripped')
