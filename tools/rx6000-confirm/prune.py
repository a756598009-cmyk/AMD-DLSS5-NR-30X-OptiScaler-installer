"""prune.py <set dir> <out dir>: the kit's copy of a shader set: the marker files, runtime\\, and only the network
and temporal pipelines the kit's cases load (listed below: the ones the logs of every case at 720p and 1080p name, and the small ones the build opens
without naming them)."""
import shutil, sys
from pathlib import Path
KEEP = """g_attn g_decups g_ffwd3 g_fswin128 g_fswin256 g_fswin32 g_fswin32nh g_fswin64 g_fswindsp128 g_fswindsp256
g_fswindsp32nh g_fswindsp64 g_fswinfusedup128 g_fswinfusedup256 g_fswinfusedup32 g_fswinfusedup32nh g_fswinfusedup64
g_gemmnores g_gemmpool g_gemmproj g_gemmprojc g_gemmprojt g_gemmvact g_gemmvqkvnorms g_gemmvqkvs g_vitattn
g_noisefield g_repack g_upsview g_fswinimagepreds32 g_fswinimagepost32 temporal/temporal_pre_fp32nh temporal/temporal_post_fp32h16
temporal/temporal_pre_fp32 temporal/motion_luma temporal/motion_estimate""".split()
src, dst = Path(sys.argv[1]), Path(sys.argv[2])
if dst.exists():
    shutil.rmtree(dst)
for p in src.rglob('*'):
    if not p.is_file():
        continue
    rel = p.relative_to(src).as_posix()
    if p.suffix == '.spv' and not rel.startswith('runtime/') and rel[:-4] not in KEEP:
        continue
    (dst / rel).parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(p, dst / rel)
print(dst, sum(1 for _ in dst.rglob('*.spv')), 'modules')
