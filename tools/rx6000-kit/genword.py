# genword.py <dir> <low16>: sets the low 16 bits of the SPIR-V generator word (header word 2) of every .spv under dir,
# so that a driver's shader cache, keyed on the module's bytes, misses (the code is unchanged).
import pathlib, struct, sys
d, low = pathlib.Path(sys.argv[1]), int(sys.argv[2], 0)
n = 0
for f in d.rglob('*.spv'):
    b = bytearray(f.read_bytes())
    assert struct.unpack_from('<I', b, 0)[0] == 0x07230203
    g = struct.unpack_from('<I', b, 8)[0]
    struct.pack_into('<I', b, 8, (g & 0xFFFF0000) | low)
    f.write_bytes(bytes(b)); n += 1
print(d, n, hex(low))
