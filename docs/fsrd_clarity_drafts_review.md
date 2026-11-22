# Review: Zakrisson-C/OptiScaler branch `fsrd-clarity-drafts`

Reviewed 2026-09-18. The branch shares our merge base (`18ac686a`, "Merge tag 'v0.9.2'
into ffx-denoise-experimental"), so it is a sibling fork of the same experimental line
and its commits sit in exactly our area: the DLSS-RR to FSR-RR preprocessing chain.

Two commits on top of the shared base:

| commit | date | subject |
|---|---|---|
| `002ab13f` | 2026-09-07 | FSRD preprocess: first-draft clarity, detail and stability changes |
| `4ca5b6c6` | 2026-09-17 | FSRD preprocess: floor material guide, noise-aware clamp, LDS barrier |

Patches stored at `tools_tmp/zakrisson/<sha>.patch`.

## Already present in our tree - no action

- **`FSRDOutputComp` LDS barrier ordering.** Their headline correctness fix: the
  out-of-bounds return ran before `PopulateSharedMemory`, so the edge threads never
  reached `GroupMemoryBarrierWithGroupSync()` and the LDS slots they own kept the
  previous dispatch's contents, corrupting the 5x5 SSIM in `GetRawColorSimilarity` and
  producing a garbage strip along an edge. Our tree already returns *after* the fill on
  the denoisable path and before it inside the blit branch, with the same reasoning in
  the comment. Independently fixed; nothing to port.
- **Plane-relative coplanarity test in `FSRDFloor`.** They describe the raw depth
  difference growing with tap offset on oblique surfaces; our file already predicts the
  tap depth from the centre gradient (`predictedDepth`) and measures the residual. Same
  defect, same solution.
- **Debug views wrapped with `frac()`.** Their `frac(hitDist * 0.1)` and
  `frac(viewSpacePos.z * 0.1)` made an absent buffer indistinguishable from a
  10-unit one. Ours already use monotone log/Turbo remaps.
- `FSRDOutputComp` and `FSRDFloorSeed` trailing-newline noise.

## Worth porting - ranked

### 1. Demodulation divisor floor (highest value, smallest change)

Our `FSRDInputConv` clamps the albedos to `1e-4` and then divides by them, so the
demodulation gain reaches 1e4 on dark surfaces. Radiance noise multiplied by 1e4 lands
outside what FP16 resolves usefully and the denoiser sees a signal whose magnitude swings
by orders of magnitude between frames. Their fix floors only the *divisor* at `8e-3`
(gain <= 125), keeps remodulation against the true albedo, and lets the difference fall
into the existing residual path so the energy is not lost. They also add an
`isSplitValid` term that routes everything down the diffuse path where both reflectances
are effectively zero and the ratio is meaningless.

Our counterparts: `FSRDInputConv.hlsl` lines 880-881 (`max(..., 1e-4f)`) and 1093-1107
(`rcpTotalWeight`, `demodDiffuse`).

### 2. Hard steps replaced by transition bands

Three binary classifications we share verbatim, each of which flips per frame when the
tested quantity hovers near its edge, and each of which feeds branches that behave very
differently on either side:

- `const float isEmissive = (totalAlbedo > 5.9f);` (ours: same line, same constant) ->
  `SoftAbove(totalAlbedo, 5.9f, 0.5f)`. The two sides demodulate differently
  (`diffAlbedo *= 1 - isEmissive`, specular lerped to 0.1), so the classification itself
  becomes a source of temporal instability on animated signage and video panels.
- `floorColor.rgb = min(rawColor, floorColor.rgb);` (ours: same) -> `SoftMin`. An exact
  min of two smooth fields creases along their crossing curve, and the two sides route
  differently: below it the split is normal, on it `denoiserColor` collapses to zero and
  the pixel travels entirely through the skip path.
- The specular tracking gate `(roughness < 0.2f)` -> `SoftBelow(roughness, 0.30f, 0.15f)`
  ramping the hit distance, so the specular reprojection hands over continuously instead
  of toggling per pixel and per frame. Our architecture handles the hit distance
  differently (typed direct/indirect signals), so this one ports as an idea, not a patch.

Their `SoftAbove` / `SoftBelow` / `SoftMin` helpers in `FSRDPreprocessCommon.hlsli` are
self-contained and small.

### 3. Normals as a floor-filter edge stop, at zero resource cost

`FSRDFloorSeed` binds `InNormals` at t1 and never samples it (true in our tree too). They
octahedrally encode it into the spare `.zw` of the depth-gradient buffer, which aliases
the RGBA16F Motion scratch that the packing shader overwrites later, and `FSRDFloor`
reads it as an orientation weight (`pow(saturate(dot(n0, n1)), sharpness)`). Depth alone
cannot separate a crease from a silhouette, so the wavelet has to stay conservative
everywhere and raster lighting ends up imprinted in the denoiser signal.

Our `FSRDFloorSeed` has `OutDepthGradient : register(u2)` as `half2`, ours writes
`GetSafeSignedFP16(gradient)`, and our Motion buffer is `R16G16B16A16_FLOAT` scratch at
that point - so widening to `half4(gradient, octNormal)` is compatible.

### 4. Plane-offset clamp in the coplanarity test

Ours extrapolates the centre gradient without bound. Theirs clamps the plane offset to
+/-50% of centre depth, on the grounds that a one-pixel central difference extrapolated
over a 16-pixel stride is only trustworthy up to a point; past that the tap is treated as
off-plane. Small, defensible, applies directly to our existing plane test.

### 5. Luminance normaliser symmetry

Ours normalises the luminance delta by the centre alone
(`rcp(max(centerLum, 1e-1f))`, then `(centerLum - lum) * rcpCenterLum`). That is
asymmetric: a dark centre beside a bright tap is stopped hard while the bright centre
looking back at the same pair blurs freely, so the two sides of one luminance edge are
routed differently - one into the skip path, one into the denoiser. Their `LumSymmetry`
term blends to `max(centerLum, tapLum)` and removes it. Verbatim the same defect in our
file.

### 6. Bias-mask routing (needs adaptation, but conceptually the best fit)

`InBiasMask` is bound at t8 in our converter and never sampled. DLSS-RR's
bias-current-color mask marks pixels whose colour should come from the current frame
rather than history: particles, alpha layers, decals, animated and video textures. Their
mechanism drives the floor to the raw colour
(`floorColor.rgb = lerp(floorColor.rgb, rawColor, biasWeight)`), which leaves nothing for
the denoiser and re-adds the full colour verbatim from the skip signal - reusing the
floor as the escape hatch instead of adding a mechanism.

This is the same class as the specular routing we just added, but strictly better
informed: a title-provided mask rather than a threshold on a reconstructed displacement.
It also moves the whole pixel rather than only the specular share, so it covers diffuse
particles and billboards too. Notes: CP77 provides the mask
(`DLSS.Input.Bias.Current.Color.Mask = true`); 007 does not, so this helps other titles
rather than 007. Their `hasBiasMask` flag keeps the absent case explicit.

### 7. Floor albedo material guide

The floor's only appearance discriminator is luminance, which cannot tell an albedo edge
(where the kernel should stop) from a shadow or reflection edge (where it should not), so
lighting is captured into the floor and returned blurred through the skip signal. Diffuse
albedo is view-independent and illumination-free, so it can separate them; they add it as
a 4th SRV to `FSRDFloor` plus a `GetAlbedoAgreement` helper (relative intensity and
chroma, so dark and bright patches of the same paint agree) and a confidence gate so
near-black albedo falls back to the previous behaviour. Costs an SRV and a constants
layout change.

### 8. Two diagnostic views worth having

- `DemodGain` - `1 / albedo` used as the demodulation divisor, log2 scaled. Red is where
  radiance noise is amplified hardest; it makes item 1 measurable on screen.
- `DenoiserFraction` - the share of each pixel routed to the denoiser rather than around
  it. This is the GPU-side visual counterpart of the `specular routing` number our probe
  prints, and it covers the whole signal split rather than only the specular share.

## Merge hazards

- **Flag bits collide.** Their `FSRDConvFlags::HasBiasMask = 1 << 4` is our
  `HasSpecHitDistance = 1 << 4`. We also just took bits 10, 12 and 13 for
  `HasSpecularMvec`, `TitleLinearDepth` and `HasResponsivityMask`.
- **Debug modes collide.** Their 18/19 (`InBiasMask`, `DemodGain`) are our 18/19
  (`DebugRawIndirectSpecular`, `DebugEffectiveRoughness`), their 20/21 are our 20/21, and
  we just took 29-33. Debug names must be renumbered, never cherry-picked.
- **Signal-path hunks will not apply.** Their `FSRDInputConv` still has the
  Mode 1 / Mode 2 signal split (`FLAGS_MODE_2_SIGNAL`); our fork replaced it with typed
  direct/indirect signals (`FLAGS_SPECULAR_SIGNAL_INDIRECT`, separate
  `demodSpecular` / `demodDiffuse`). Port those hunks by intent.
- **Constants layouts move together.** Their `FloorFilter::Constants` grows 48 -> 64
  bytes and `Conversion::Constants` gains a field; our `FSRDShaderData.h` static_asserts
  (48 and, after today's work, 416) are the third leg of that contract.
- Their `FSRDFloor` also differs from ours in the pre-existing `depthNormScale`
  formulation (`adaptiveScale * rcpDepthScale * RcpCrossBlNorm` there, versus
  `(1 + 2 * smoothness) * RcpCrossBlNorm * rcp(1 + |centerDepth|)` here), so diffing
  patch-to-patch will mislead; compare against the shared base instead.

## Suggested order

1. Demodulation divisor floor and the `isEmissive` / floor-clamp / hit-distance ramps -
   small, self-contained, and aimed at noise and flicker we are already chasing.
2. Normals edge stop plus the plane-offset clamp - floor quality, no new resources.
3. The two diagnostic views, so the above stay measurable.
4. Bias-mask routing, reusing the routing weight our specular path already carries, once a
   title with a mask (CP77) is being tuned.
5. Floor albedo guide last: it costs an SRV and a layout change.
