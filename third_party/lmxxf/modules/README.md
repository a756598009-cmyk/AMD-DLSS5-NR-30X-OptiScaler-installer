# Precompiled HIP GPU Modules (RDNA4 / gfx1201)

This directory contains precompiled AMD GPU code objects (`.hsaco`) loaded by the lmxxf neural rendering pipeline.

## Provenance
- **Upstream Repository**: https://github.com/lmxxf/dlss5-on-amd-9070xt-porting
- **Commit Base**: `2f19eebcd79ac7154fa4de354342cc1c105b218a` (0.46)
- **Target GPU Architecture**: AMD RDNA4 `gfx1201` (RX 9070, RX 9070 XT)
- **License**: MIT, Copyright (c) 2026 Kien (`../LICENSE`)

## Build Method
These modules were compiled from `../hip/*.hip` sources using AMD's Runtime Compiler tool (`rtc_compile.cpp`) through the driver's `amd_comgr_3.dll` (no full HIP SDK required). See `../hip/README.md` and `../hip/build-modules.ps1` for compilation recipes and defines.

## Contents (40 Code Objects)
- `boundary-fast.hsaco`, `boundary_reference.hsaco`
- `c32_fast.hsaco`, `c32_fast_attention.hsaco`, `c32_fused_attention.hsaco`
- `c32_fused_ffn_attention-packed.hsaco`, `c32_fused_ffn_attention.hsaco`
- `c32_prefix_reference.hsaco`, `c32_tiled.hsaco`, `c32_wmma.hsaco`
- `deep_fast-packed.hsaco`, `deep_fast.hsaco`, `deep_reference.hsaco`, `deep_wmma.hsaco`
- `multihead-fast-packed.hsaco`, `multihead-fast-padded-wave-packed.hsaco`, `multihead-fast-padded-wave.hsaco`, `multihead-fast.hsaco`
- `multihead-reference.hsaco`, `multihead-tiled.hsaco`, `multihead-wmma.hsaco`, `multihead_fused_attention.hsaco`
- `prefix_fast.hsaco`, `wave-pointwise.hsaco`
- Since 0.31-0.37: `c32-wave1.hsaco`, `c64-wave2.hsaco` (one head per wave), `c512-m32-mh.hsaco`, `c512-m32-deep.hsaco`
  (C512 at 32 tokens), `vit-wide-deep.hsaco` (ViT projection 64 columns), `vit-stream.hsaco` (ViT byte stream),
  `swin-persistent.hsaco` (C256 persistent stage). The runtime turns each group off when its module is missing.
- Since 0.40-0.41: `c32-wave1-rtz.hsaco` (1080 tier C32), the fast-numeric twins `c32-wave1-fast.hsaco`,
  `c64-wave2-fast.hsaco`, `deep_fast-packed-fast.hsaco`, `vit-stream-fast.hsaco` (`DLSS5_FAST_NUMERIC=1`, the
  runtime's default), and `multi-pass-predict.hsaco`, `multi-pass-skin.hsaco` (upstream's own multi pass, not used
  here). Upstream builds five rows with LLVM 23.1.2; here all of them come from the driver's COMGR (LLVM 21), as before.
- Since 0.42: `c32-wave1-fast-norm900.hsaco`, `swin-persistent-fast.hsaco`.
- `modules.json`: Module metadata and layout definitions.
- `runtime-manifest.json`: Runtime loading configuration.
- `SHA256SUMS`: Checksums of all module files.
