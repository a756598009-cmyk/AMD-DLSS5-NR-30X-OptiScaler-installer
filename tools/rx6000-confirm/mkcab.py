"""mkcab.py <folder> <out.cab>: every file under <folder> into one LZX:21 cabinet (makecab, built into Windows; expand.exe
takes it apart with the paths), in path order (similar shaders together compress best)."""
import subprocess, sys, tempfile
from pathlib import Path
src, out = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
lines = ['.OPTION EXPLICIT', '.Set CabinetNameTemplate=' + out.name, '.Set DiskDirectoryTemplate=' + str(out.parent),
         '.Set Cabinet=on', '.Set Compress=on', '.Set CompressionType=LZX', '.Set CompressionMemory=21',
         '.Set MaxDiskSize=0', '.Set CabinetFileCountThreshold=0', '.Set FolderSizeThreshold=0', '.Set MaxCabinetSize=0',
         '.Set UniqueFiles=on', '.Set RptFileName=nul', '.Set InfFileName=nul']
for p in sorted(src.rglob('*')):
    if p.is_file():
        lines.append(f'"{p}" "{p.relative_to(src)}"')
with tempfile.NamedTemporaryFile('w', suffix='.ddf', delete=False) as f:
    f.write('\n'.join(lines) + '\n')
r = subprocess.run(['makecab', '/F', f.name], capture_output=True, text=True)
print(r.stdout[-400:], r.stderr[-400:])
print(out, out.stat().st_size if out.exists() else 'missing')
