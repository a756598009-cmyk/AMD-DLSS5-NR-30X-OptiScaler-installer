# rehash_manifest.py <manifest> <shaders dir>: the module hashes of a prewarm manifest (mz_interpose.cpp) made again
# from the files it names, and its end checksum; for a shader folder whose SPIR-V generator word was changed.
import pathlib, re, sys

M = (1 << 64) - 1


def h64(data):
    n = len(data)
    h = 0xcbf29ce484222325 ^ n
    i = 0
    while i + 8 <= n:
        w = int.from_bytes(data[i:i + 8], 'little')
        h = ((h ^ w) * 0x9e3779b97f4a7c15) & M
        h ^= h >> 32
        i += 8
    while i < n:
        h = ((h ^ data[i]) * 0x100000001b3) & M
        i += 1
    return h


man, shaders = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
lines = man.read_bytes().decode().split('\n')
assert lines[-1] == '' and lines[-2].startswith('end ')
body = lines[:-2]
out = []
for l in body:
    m = re.match(r'^p (\S+) m=([0-9a-f]{16}):(\d+) (.*)$', l)
    if m and m.group(1) != '-':
        data = (shaders / m.group(1)).read_bytes()
        assert len(data) == int(m.group(3)), l
        l = 'p %s m=%016x:%d %s' % (m.group(1), h64(data), len(data), m.group(4))
    out.append(l)
text = '\n'.join(out) + '\n'
text += 'end %016x\n' % h64(text.encode())
man.write_bytes(text.encode())
print(man, len(out) - 4, 'pipelines')
