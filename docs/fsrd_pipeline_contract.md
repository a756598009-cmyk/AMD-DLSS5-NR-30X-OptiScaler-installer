# FSR-RR preprocessor: the pipeline contract

This is the one-page description of what the FSR-RR path (`OptiScaler/shaders/fsrd_preprocess`)
does, what each of its inputs promises, and what was measured and rejected along the way. Code
comments state the constraint a block maintains and point here for the reasoning, so the
shaders stay readable and the reasoning stays in one place instead of being re-derived every
session.

## The shape of a frame

```
 title inputs ──► floor filter (FloorSeed, 5x a-trous passes) ──► floor texture
                        │
                        ▼
                conversion (FSRDInputConv)  ──►  RR signals ──► FFX denoiser ──► composition
                        │                                                              ▲
                        └──────────────► skip signal ─────────────────────────────────┘
```

1. **FloorSeed** sorts a 5x5 neighbourhood, picks an outlier-rejecting percentile (shadows
   cluster below the 30th, sparse specular RT is rejected around the 30th–40th) and clamps that
   colour below the centre pixel. The result is a *lower* estimate, not a mean. It also produces
   the linear depth field the rest of the chain reads.
2. **FloorFilter** runs five bilateral à-trous passes over it, weighted by luma, depth
   coplanarity, surface orientation and an albedo material guide.
3. **FSRDInputConv** splits each pixel into a floor share and a residual, demodulates the
   residual by reflectance, and writes the typed RR signals plus the skip signal.
4. The **denoiser** (AMD's `amd_fidelityfx_denoiser_dx12.dll`, RR 1.2) is dispatched on those
   signals.
5. **FSRDOutputComp** remodulates, adds the skip signal back, correlates against the raw colour,
   and optionally grafts the handover's high band onto RR's low band.

## Per-pixel energy accounting

The invariant the split rests on, in linear radiance at the pixel:

```
 floor          = isolation * filter(raw)                     the spatial estimate
 residual       = max(0, raw - floor)                         what the denoiser is handed
 out            = floor + denoise(demod_remod(residual)) + skip
 skip           = floor + routedRadiance + unmappedShare
```

- The residual closure is hard (`max(0, ...)`) and deliberately so: a knee would lift small
  negative residuals above zero and publish those pixels on both paths at once.
- `unmappedShare` is the part the demodulate/remodulate round trip could not represent (FP16
  clipping, or the divisor floor). It travels around the denoiser, which is what keeps the
  pixel's energy whole - and also what puts it on screen unfiltered. **Measured: 0.0000 on every
  frame probed on 007 First Light**, so it is not a term that needs tuning there.
- `routedRadiance` is specular radiance the title itself declared unresponsive. Everything that
  used to feed it from a reflected-image motion field is gone; see the dead ends below.

## The floor, the handover, and why each is shaped this way

**The raw-preserving blend is gated by albedo, not by colour.** The blend asks whether the raw
sample resembles the floor, and on a noisy input the noise answers that question itself: flat
surfaces pass raw grain through while the floor texture stays smooth. Albedo answers it
noiselessly, because it is material rather than illumination, so the gate trusts it where it
carries information and refuses the blend where it does not. Near-black albedo carries none
(unlit billboards, very dark paint), so the gate treats it as flat.

**The handover exists for exact-zero-roughness pixels.** RR reads exact zero as a perfect mirror
and reprojects it through a virtual hit position derived from the specular ray length; a title
that publishes no such length leaves that reconstruction undefined, so RR accumulates confident
but misaligned history over those surfaces. The handover gives them to the spatial floor and
withholds the residual - the high-frequency part the floor's median and à-trous passes already
rejected. It is a spatial denoise, not a passthrough: the published colour is the filtered floor,
never the raw input.

**It is a frequency graft, not a ratio.** A partial handover only ever produced a ratio of the
two paths' faults: RR is blurry but temporally informed, the floor is sharp but filtered
spatially only, and any blend of the two gives a fraction of each fault. They fail in different
bands, so composition takes the low band from RR where its temporal information is real and its
softness costs nothing, and the high band from the floor where the strokes are. Because that
combination happens after denoising it takes nothing from the denoiser's input, which is what
makes it safe to widen past the zero-roughness pixels it was written for.

**The rank filter is shaped around one property: a pixel that is not an outlier comes back
untouched.** That is what keeps text and clean pixels at their exact original values, and it is
why the filter escalates to a wider window only where the narrow one could not resolve a
trustworthy median - clustered noise, where no single sample reads as an outlier. Its acceptance
test is by magnitude rather than by rank, because a stroke peak is always the largest of its
neighbours and a rank test cannot tell a peak from an impulse. Its confidence is graded rather
than hard, because a hard yes/no flips between frames for a pixel near the boundary and nothing
in this path is temporal, so the flip reads as flicker.

## Inputs

| Slot | Resource | Flag | Notes |
|---|---|---|---|
| t0–t8 | colour, depth, motion, normals, roughness, spec hit distance, diffuse/specular albedo, bias mask | — / `HasBiasMask` | Required, except the bias mask. |
| t9 | floor texture | — | Ours, not the title's; written by the floor chain. |
| t10 | resource inspector view | — | Ours; a diagnostic binding. |
| t11 | emissive | `HasEmissiveInput` | Optional. |
| t12–t13 | specular / diffuse ray lengths | `HasCombinedSpecHitDistance`, config | Optional. |
| t14 | title linear view depth | `TitleLinearDepth` | Optional, validated, own sign convention. |
| t15 | responsivity hint | `HasResponsivityMask` | Optional; polarity is a title property. |

The slots are **positional**: the order in `FSRDShaderData.h`'s `Conversion::Input`, in the
dispatch's designated initializer and in the shader's `register(tN)` declarations must agree, and
nothing in either compiler checks it. `verify_fsrd_mirrors.py` does.

## Knobs

Basic (the FSR-RR page): denoiser signal types, floor isolation, floor detail boost, the handover
mode and its strength/detail blend.

Advanced (the "Advanced Settings..." window): everything else, including the denoiser's own
tunables (disocclusion, radiance clip, gaussian relaxation), the floor filter's per-term weights,
the debug views and the resource inspector. **Existing users who never open that window see less,
not a different image**: every advanced default is the value the code had before the window
existed.

Inert at their defaults, i.e. safe to ignore: `FloorSoftMin` (the ceiling clamp is off),
`FloorEnvelopeBias`, `ResponsivityTrustThreshold`, `SpecularMvecTrustThreshold` (removed),
`UseTitleLinearDepth`, `NormalsInViewSpace`.

## Title quirks

**007 First Light** (the reference title for all of this):

- Publishes `DLSS.WorldToViewMatrix` filled with `FLT_MAX`. Inverting it produced NaN camera
  positions and the denoiser then refused to publish radiance. Fixed by validating the matrix
  (finite, non-zero determinant, finite camera position) and falling back to the Streamline
  camera basis.
- Publishes **no specular ray length** at all. RR writes no denoised output for the indirect
  specular signal without a finite one, so the converter falls back to the primary surface's view
  distance. A wrong-but-plausible length denoises; a missing one is discarded wholesale.
- Has **no exact-zero-roughness pixels** (`isZeroRoughness` is 0 everywhere), so the zero-rough
  handover mask never fires on it. Use handover mode 2 for this title.
- Publishes the specular MVec at 1280x720 `R16G16_FLOAT`, same as its primary motion. See the
  dead ends.

## Measured and rejected

Each of these was implemented, measured and removed. They are recorded so the next session does
not re-derive them:

- **Specular MVec routing**: published specular radiance unfiltered wherever the reflected
  image's motion disagreed with the surface's by more than a threshold. The disagreement tracked
  the *camera's speed*, not any misalignment: 0% of the frame routed while still and 67% while
  moving, which put the current frame's raw specular noise on screen under motion. Judging the
  disagreement as a fraction of the motion reduced it without making it a defensible measure.
  Removed, input and all. Raising the threshold from a pixel count to a fraction is the general
  lesson: an absolute threshold cannot mean the same thing at 2 px/frame and 160 px/frame.
- **Floor/raw ceiling clamp**: replaced the floor with a low pass of the raw sample wherever the
  floor exceeded it. The share it clamps away is the share it replaces with the *raw*, so it
  republished the raw's grain through the skip signal - visible because the floor is a low
  percentile, which makes the substituted sample a large jump. Averaging the ceiling attenuated
  it but could not remove it. The non-negativity that matters belongs on the residual (`max(0,
  raw - floor)`), where it is exact.
- **Floor envelope bias**: premised on the floor exceeding the raw on roughly half the frame.
  Measured: the crossing is ~0.1% of the frame on a typical frame, spiking to ~65% in some views
  (mostly where detail boost pushes the floor up). The premise was wrong, so the knob aims at
  something that is not the problem. The knob remains, off by default.
- **Floor detail boost** is what fixed the floor's softness: it re-injects the filter's own
  high-frequency luminance residual on the final pass, so the floor stops being a 31 px low pass
  where it takes a pixel over.
- **Floor confidence** (withholding the floor where the seed's neighbourhood was inconsistent)
  changed nothing the default did not already do, and the instability channel that fed it was
  carried through five filter passes for it. Both removed.
- **Five detail-preserving filters** (hybrid median, structure-tensor steered, Kuwahara, adaptive
  rank, albedo-guided) existed to be compared. Adaptive rank survived; the rest were an A/B that
  had already answered its question, and removing them cut the conversion shader by 43%.
- **Demod divisor floor**: lowering it moved the loss between the divisor floor and FP16 clipping
  rather than recovering anything, because the unmapped share is zero either way.

## Diagnosing a frame

The instrumentation is one switch: **Diagnostics** in the advanced window. Everything else is
off in a shipping configuration, because a probe that runs while nobody reads it is pure cost.

Turn it on and the input probe records every 60th conversion and logs, per frame: the RR-facing
linear depth, motion and normals; the specular signal and its share of the frame; the skip
signal's magnitude; and the floor/raw crossing share. That last one is what to watch when the
image looks soft - on those pixels the residual collapses to zero and the skip signal is the
floor filter's own low pass.

There is no denoiser-output readback probe. It answered the question it was written for ("the
1.2 dispatch runs but publishes no radiance", which turned out to be a NaN camera position) and
the answer is now guarded by the camera-matrix validation; the same information is available per
pixel through the composition `DenoiserOutput` view, for free.

Debug views (`Debug View` in the advanced window) render the quantities per pixel. The ones
worth knowing: `FloorColor` (the filter's raw output - smooth by construction), `SkipSignal` and
`SkipRawInject` (what reaches the screen unfiltered), `DenoiserFraction` (share of the pixel
reaching the denoiser), `SpecularSplit`, `TitleLinearDepthDiff` (answers the title's depth sign
convention), `InResponsivityMask`.

## Where things live

`PrepareDenoiseConvInput` is the frame's orchestration and reads as the list of steps it takes;
each step is its own method beside it:

| Step | What it owns |
|---|---|
| (inline) | Resource gathering, the subrect bases, the frame extents |
| `ResolveSpecularHitDistance` | Picking the specular ray length from the title's candidates |
| `AcquireOptionalInputs` | The resources a title may publish beyond the required set |
| `ResolveDiffuseHitDistance` | The diffuse ray length |
| `ResolveCameraMatrices` | View and projection, including the Streamline fallbacks |
| `ResolveSignalTypes` | The automatic Direct/Indirect classification, and its lock |

Two rules kept the split honest, and are worth keeping if it grows further: a step that can
contribute to readiness carries its own `isReady` local (or takes the caller's) so nothing is
rewritten in the move, and the extraction script verified which shape each block needed instead
of assuming it.

## History worth knowing

The frames this fork was debugged on left switches behind that no longer exist; the code they
guarded is gone with them, and this is the part of them worth remembering:

- **Deferred dispatch** recorded the RR dispatch into an OptiScaler-owned command list and
  submitted it at present time. It existed because the RR 1.2 driver execution hangs when its
  dispatch is followed by an upscaler dispatch on the same command list. The direct path is the
  one that works today (and is what every measurement in this document was taken on), so the
  workaround was removed rather than kept as a dormant second pipeline. If that hang returns,
  this is the note to remember.
- **UpscalerOnly, DisableDenoiserDispatch, DisableUpscalerDispatch** were bisect switches for
  isolating the same crash; **DisableNativeDenoiser** forced the title's own denoiser setup to
  take its disabled path; **NoDriverProvider** refused the upscaler dll's driver-side provider
  registration so two FFX clients would not share the per-device driver state.
- **Specular MVec routing** is in the rejected list above.

## Working on this code

```
python OptiScaler/shaders/shader_tools/verify_fsrd_mirrors.py     # before anything else
python ../tools_tmp/build_fsrd_shader.py FSRDInputConv            # regenerate cso + header
msbuild OptiScaler.sln -p:Configuration=Release -p:Platform=x64
```

The verifier is the guard rail for the five hand-maintained mirrors: the constant-buffer layouts,
both flag words, the two debug-mode numberings, and the positional resource order. Run it first;
it catches in a second what otherwise shows up as a wrong parameter or a wrong texture binding
with no error anywhere.
