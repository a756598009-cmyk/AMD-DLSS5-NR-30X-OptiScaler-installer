"""Build a harmless synthetic PE fixture; no author DLL is read or executed.

The fixture has a minimal DllMain and a no-op worker at the addresses expected
by one runtime's bootstrap filter (0.3.1 unless --layout names another). It
deliberately is NOT an accepted runtime SHA: only the runtime-load isolation
unit test bypasses the production IdentifyRuntime gate.

    amd_runtime_host_fixture.py OUT [--no-bootstrap] [--layout 0.4.1]
"""
from pathlib import Path
import struct
import sys

# DllMain's CreateThread call site, the bootstrap thread start and the CreateThread IAT slot,
# as RuntimeHostLoad.h pins them.
CONTRACTS = {
    "0.3.1": (0x6D8F, 0x8630, 0x8D788),
    "0.4.0": (0x726F, 0x8D20, 0x9A208),
    "0.4.1": (0x71AF, 0x8C90, 0x9B3B8),
    "0.4.2": (0x714F, 0x8CC0, 0xA1168),
    "0.4.3": (0x714F, 0x90A0, 0xA28B8),
    "0.5.0": (0x6EDF, 0x8EA0, 0xA7578),
    "0.5.1": (0x6EDF, 0x8EA0, 0xAA9C0),
    "0.6.0": (0x70DF, 0x91C0, 0xAF170),
}

args = sys.argv[1:]
layout = "0.3.1"
if "--layout" in args:
    layout = args[args.index("--layout") + 1]
call, start, iat = CONTRACTS[layout]
idata = iat & ~0xFFF
text_raw = (start + 0x100 - 0x1000 + 0x1FF) & ~0x1FF
idata_raw = 0x400 + text_raw
out = bytearray(idata_raw + 0xA00)


def write(offset, fmt, *values):
    struct.pack_into("<" + fmt, out, offset, *values)


def at(rva, data):
    offset = 0x400 + rva - 0x1000 if rva < 0x1000 + text_raw else idata_raw + rva - idata
    out[offset:offset + len(data)] = data


out[:2] = b"MZ"
write(0x3C, "I", 0x80)
out[0x80:0x84] = b"PE\0\0"
write(0x84, "HHIIIHH", 0x8664, 2, 0, 0, 0, 0xF0, 0x2023)
opt = 0x98
write(opt, "HBBIII", 0x20B, 14, 0, text_raw, 0xA00, 0)
write(opt + 16, "IIQII", 0x1000, 0x1000, 0x180000000, 0x1000, 0x200)
write(opt + 40, "HHHHHH", 6, 0, 0, 0, 6, 0)
write(opt + 56, "II", idata + 0x1000, 0x400)
write(opt + 68, "HHQQQQII", 2, 0x100, 0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
write(opt + 112 + 8, "II", idata, 40)  # import directory
write(opt + 112 + 12 * 8, "II", iat, 16)  # IAT
section = opt + 0xF0
out[section:section + 8] = b".text\0\0\0"
write(section + 8, "IIIIIIHHI", idata - 0x1000, 0x1000, text_raw, 0x400, 0, 0, 0, 0, 0x60000020)
section += 40
out[section:section + 8] = b".idata\0\0"
write(section + 8, "IIIIIIHHI", 0xA00, idata, 0xA00, idata_raw, 0, 0, 0, 0, 0xC0000040)

# Entry: do nothing unless DLL_PROCESS_ATTACH; prepare the stack arguments and
# jump to our own CreateThread call. No imports other than CreateThread.
entry = bytes.fromhex("83 fa 01 74 06 b8 01 00 00 00 c3 48 83 ec 38")
entry += bytes.fromhex("48 c7 44 24 28 00 00 00 00 c7 44 24 20 00 00 00 00")
entry += b"\xe9" + struct.pack("<i", call - (0x1000 + len(entry) + 5))
at(0x1000, entry)
if "--no-bootstrap" in args:
    at(0x1000, bytes.fromhex("b8 01 00 00 00 c3"))
# lea r8,[start]; xor ecx,ecx; xor edx,edx; xor r9d,r9d; call [iat]
at(call, b"\x4c\x8d\x05" + struct.pack("<i", start - (call + 7)) + bytes.fromhex("31 c9 31 d2 45 31 c9 ff 15")
   + struct.pack("<i", iat - (call + 20)))
at(call + 20, bytes.fromhex("48 83 c4 38 b8 01 00 00 00 c3"))
# This no-op worker is harmless even if filtering regresses. This test must
# nevertheless reject that load because it did not observe one suppression.
at(start, bytes.fromhex("55 41 57 41 56 41 54 56 57 53 48 81 ec 50 02 00 00 "
                        "48 81 c4 50 02 00 00 5b 5f 5e 41 5c 41 5e 41 5f 5d 31 c0 c3"))
at(idata, struct.pack("<IIIII", idata + 0x100, 0, 0, idata + 0x1A0, iat))
at(idata + 0x100, struct.pack("<QQ", idata + 0x180, 0))
at(iat, struct.pack("<QQ", idata + 0x180, 0))
at(idata + 0x180, b"\0\0CreateThread\0")
at(idata + 0x1A0, b"KERNEL32.dll\0")
target = Path(args[0])
target.write_bytes(out)
print(f"Synthetic bootstrap fixture ({layout}):", target)
