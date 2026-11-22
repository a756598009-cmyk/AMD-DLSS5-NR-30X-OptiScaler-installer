r"""dlssnr-amd\aco-gfx11 as shipped: ACO's gfx11 code only for the kernels it runs faster on RX 7000.

The RX 7000 speed kit (RX 7900 XTX and RX 7800 XT, 2026-11-16) timed every kernel with the driver's compile and with
ACO's (kit 7's records, third_party/mochizuki/windows/data/aco/gfx11) at 1152x648, 1280x720, 1920x1080 and 2560x1440.
ACO was 3% and 0.02 ms or more faster on both cards for the kernels below, vitattn as ACO's compile of the matrix body
(records-coop). A kernel whose ACO SPIR-V differs from shaders-rdna3's has it in data's shaders\ and goes in the
bundle's shaders\ (the runtime takes it from there); the others' records were made from shaders-rdna3's own SPIR-V.
Every other kernel stays the driver's. Records are named by the FNV-1a hash of the SPIR-V they were made from.

usage: build-mochizuki-aco-gfx11.py <shaders-rdna3> <data/aco/gfx11> <out aco-gfx11>
"""
import pathlib
import shutil
import sys

FASTER = ["fswin32nh", "fswinfusedup64", "gemmproj", "gemmprojc", "gemmprojt", "gemmvact", "gemmvqkvs", "vitattn"]


def fnv(data: bytes) -> str:
    h = 1469598103934665603
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return f"{h:016x}.nrp"


def main() -> None:
    rdna3, data, out = (pathlib.Path(a) for a in sys.argv[1:4])
    if out.exists():
        shutil.rmtree(out)
    (out / "records").mkdir(parents=True)
    (out / "shaders").mkdir()
    for k in FASTER:
        own = data / "shaders" / f"g_{k}.spv"
        spv = (own if own.is_file() else rdna3 / f"g_{k}.spv").read_bytes()
        rec = next((r for r in (data / d / fnv(spv) for d in ("records", "records-coop")) if r.is_file()), None)
        if not rec:
            sys.exit(f"no gfx11 record for g_{k}.spv ({fnv(spv)}): the shader changed, make its record again")
        shutil.copyfile(rec, out / "records" / rec.name)
        if own.is_file():
            (out / "shaders" / own.name).write_bytes(spv)
    print(f"aco-gfx11: {len(FASTER)} records ({', '.join(FASTER)})")


if __name__ == "__main__":
    main()
