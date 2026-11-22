<#
.SYNOPSIS
  Stage and zip a complete user package (no NVIDIA / author proprietary files).
  Default product: OptiScaler-0.6.10-opennr, the version in OptiScaler/resource.h.
  The danielblnc runtime it drives is 0.8.0 (NeuralHub's neuralhub_host.dll; 0.6.0, 0.5.1, 0.5.0, 0.4.3, 0.4.2, 0.4.1,
  0.4.0, 0.3.1 and 0.3.0 still accepted), supplied by the user.
  The mochizuki runtime (MochizukiNrRuntime.dll and dlssnr-amd\) is not packaged: the AMD-NR
  installer or a local build supplies it, and its model is never in this package.

.EXAMPLE
  .\PACKAGE_RELEASE.ps1
  .\PACKAGE_RELEASE.ps1 -Version 0.6.10-opennr -DepsRoot 'C:\path\with\OptiScaler'
#>
[CmdletBinding()]
param(
    [string]$Version = '0.6.10-opennr',
    [string]$OutDir = 'dist',
    [string]$Name = '',
    [string]$OptiDll = '',
    [string]$DepsRoot = '',
    [switch]$AllowMissingDeps
)
$ErrorActionPreference = 'Stop'

# 不要用 Get-FileHash：它属于 Microsoft.PowerShell.Utility，靠模块自动加载。
# 当环境里的 PSModulePath 指向 PowerShell 7 的模块目录时（CI 里在 shell: pwsh
# 步骤里调 powershell -File 正是这种情况），5.1 子进程加载不到它，会直接报
# CommandNotFoundException，整个打包步骤失败。用 .NET 自己算，不依赖任何模块。
function Get-Sha256([string]$path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $fs = [IO.File]::OpenRead($path)
        try { return ([BitConverter]::ToString($sha.ComputeHash($fs))).Replace('-', '') }
        finally { $fs.Dispose() }
    } finally { $sha.Dispose() }
}

$root = Split-Path -Parent $PSScriptRoot
if (-not $root) { $root = (Get-Location).Path }
if (-not $Name) { $Name = "OptiScaler-$Version" }
$stage = Join-Path $root (Join-Path $OutDir $Name)
$zip = Join-Path $root (Join-Path $OutDir ($Name + '.zip'))

# The newest build wins, so a leftover output from an older build is never packaged by mistake.
# x64/Release/a is where the solution build's post-build step moves the DLL.
if (-not $OptiDll) {
    $newest = Get-Item -LiteralPath @(
        (Join-Path $root 'x64/Release/a/OptiScaler.dll'),
        (Join-Path $root 'exports/release-local/OptiScaler.dll')
    ) -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($newest) { $OptiDll = $newest.FullName }
}
if (!(Test-Path -LiteralPath $OptiDll)) {
    throw 'OptiScaler.dll not found. Build Release first (r18: ordinary Release, multi-slot default).'
}

if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage, (Join-Path $stage 'OptiScaler'), (Join-Path $stage 'Licenses') | Out-Null
Copy-Item -LiteralPath $OptiDll -Destination (Join-Path $stage 'OptiScaler.dll') -Force

$deps = Join-Path $stage 'OptiScaler'
$missing = [System.Collections.Generic.List[string]]::new()

function Find-Dep([string[]]$candidates) {
    foreach ($c in $candidates) {
        if ($c -and (Test-Path -LiteralPath $c -PathType Leaf)) { return $c }
    }
    return $null
}

$depSearch = @()
if ($DepsRoot) {
    $depSearch += (Join-Path $DepsRoot 'OptiScaler')
    $depSearch += $DepsRoot
}
# FSR SDK 2.4's signed DLLs; external/FidelityFX-SDK-v2 (2.3.0) stays for the sources OptiScaler compiles.
$depSearch += (Join-Path $root 'external/FidelityFX-SDK-v2.4/Kits/FidelityFX/signedbin')
$depSearch += (Join-Path $root 'external/FidelityFX-SDK-v2/Kits/FidelityFX/signedbin')
$depSearch += (Join-Path $root 'OptiScaler-AMD-PreSR-R1/OptiScaler')
$depSearch += (Join-Path $root 'OptiScaler-AMD-PreSR-Multipass-v2.25/OptiScaler')

foreach ($name in @(
    'amd_fidelityfx_loader_dx12.dll',
    'amd_fidelityfx_upscaler_dx12.dll',
    'amd_fidelityfx_framegeneration_dx12.dll',
    # FSR-RR's Ray Reconstruction provider, loaded from here like the upscaler.
    'amd_fidelityfx_denoiser_dx12.dll'
)) {
    $cands = @()
    foreach ($d in $depSearch) { $cands += (Join-Path $d $name) }
    $hit = Find-Dep $cands
    if ($hit) { Copy-Item -LiteralPath $hit -Destination $deps -Force }
    else { $missing.Add($name) }
}

$vkCands = @(Join-Path $root 'external/FidelityFX-SDK/PrebuiltSignedDLL/amd_fidelityfx_vk.dll')
if ($depSearch.Count) { $vkCands += (Join-Path $depSearch[0] 'amd_fidelityfx_vk.dll') }
$vk = Find-Dep $vkCands
if ($vk) { Copy-Item -LiteralPath $vk -Destination $deps -Force }

$xessDirs = @(
    (Join-Path $root 'external/xess/bin'),
    (Join-Path $root 'OptiScaler-AMD-PreSR-R1/OptiScaler'),
    (Join-Path $root 'OptiScaler-AMD-PreSR-Multipass-v2.25/OptiScaler')
)
if ($DepsRoot) {
    $xessDirs = @((Join-Path $DepsRoot 'OptiScaler'), $DepsRoot) + $xessDirs
}
$xessCopied = 0
foreach ($d in $xessDirs) {
    if (!(Test-Path $d)) { continue }
    Get-ChildItem -LiteralPath $d -Filter 'libxess*.dll' -ErrorAction SilentlyContinue |
        ForEach-Object { Copy-Item $_.FullName -Destination $deps -Force; $xessCopied++ }
    Get-ChildItem -LiteralPath $d -Filter 'libxell*.dll' -ErrorAction SilentlyContinue |
        ForEach-Object { Copy-Item $_.FullName -Destination $deps -Force; $xessCopied++ }
    if ($xessCopied) { break }
}
if ($xessCopied -eq 0) { $missing.Add('libxess*.dll (optional but recommended)') }

New-Item -ItemType Directory -Path (Join-Path $deps 'D3D12_OptiScaler') -Force | Out-Null
# Only Agility D3D12Core (and optional Agility companions). Never sweep a
# user-supplied DepsRoot for every *.dll — that could pick up version.dll.
$agilityNames = @('D3D12Core.dll', 'd3d12SDKLayers.dll')
$agilityCands = @(
    (Join-Path $root 'external/directx_agility_sdk/lib'),
    (Join-Path $root 'OptiScaler-AMD-PreSR-R1/OptiScaler/D3D12_OptiScaler')
)
if ($DepsRoot) {
    $agilityCands = @((Join-Path $DepsRoot 'OptiScaler/D3D12_OptiScaler')) + $agilityCands
}
foreach ($d in $agilityCands) {
    if (!(Test-Path $d)) { continue }
    foreach ($n in $agilityNames) {
        $p = Join-Path $d $n
        if (Test-Path -LiteralPath $p) {
            Copy-Item -LiteralPath $p -Destination (Join-Path $deps 'D3D12_OptiScaler') -Force
        }
    }
    break
}

if ($missing.Count -gt 0) {
    $msg = "Missing upscaler dependency binaries:`n  " + ($missing -join "`n  ") + "`n" +
           "These live in the git submodules (external/xess, external/FidelityFX-SDK*,`n" +
           "and they are *.dll so they are not committed directly).`n" +
           "Fix: git submodule update --init --recursive`n" +
           "Or pass -DepsRoot pointing at a folder that has an OptiScaler\ subfolder."
    if ($AllowMissingDeps) {
        Write-Warning $msg
    } else {
        throw $msg
    }
}

# Licenses
$optiLic = Join-Path $root 'LICENSE'
if (Test-Path $optiLic) {
    Copy-Item $optiLic (Join-Path $stage 'Licenses/OptiScaler_LICENSE.txt') -Force
}
if (Test-Path (Join-Path $root 'Licenses')) {
    Get-ChildItem (Join-Path $root 'Licenses') -File | Copy-Item -Destination (Join-Path $stage 'Licenses') -Force
}
foreach ($pair in @(
    @('external/xess/LICENSE.txt', 'XeSS_LICENSE.txt'),
    @('external/FidelityFX-SDK/docs/license.md', 'FidelityFX_v1_LICENSE.md'),
    @('external/FidelityFX-SDK-v2/docs/license.md', 'FidelityFX_v2_LICENSE.md'),
    @('external/FidelityFX-SDK-v2.4/docs/license.md', 'FidelityFX_v2_LICENSE.md'),
    @('external/directx_agility_sdk/LICENSE.txt', 'DirectX_LICENSE.txt')
)) {
    $p = Join-Path $root $pair[0]
    if (Test-Path $p) { Copy-Item $p (Join-Path $stage ('Licenses/' + $pair[1])) -Force }
}

# INI
$iniSrc = Join-Path $root 'OptiScaler.ini'
if (!(Test-Path $iniSrc)) { throw "Missing $iniSrc" }
$ini = Get-Content -LiteralPath $iniSrc -Raw
$ini = $ini -replace '(?m)^Dx12Upscaler=.*$', 'Dx12Upscaler=ffx'
$ini = $ini -replace '(?m)^LogToFile=.*$', 'LogToFile=true'
$ini = $ini -replace '(?m)^LogLevel=.*$', 'LogLevel=2'
$ini = [regex]::Replace($ini, '(?ms)(\[FrameGen\].*?^Enabled=)[^\r\n]*', '$1false')
$ini = [regex]::Replace($ini, '(?ms)^\[DlssNr\].*?(?=^\[|\z)', @"
[DlssNr]
; Product $Version - NR slots default 3 (2-5 in-game, 1-5 here).
; Requires DLSS-NR-on-AMD 0.8.0 (NeuralHub; 0.6.0, 0.5.1, 0.5.0, 0.4.3, 0.4.2, 0.4.1, 0.4.0, 0.3.1 and
; 0.3.0 still work; https://github.com/danielblnc/DLSS-NR-on-AMD)
; as dlssnr_amd_pass1.dll (0.8.0, one module, hosted from dlssnr_amd_host\) or dlssnr_amd_pass1-3.dll
; (Setup finds an installed NeuralHub, copies version.dll or neuralhub_host.dll from the package
; folder, or takes it out of neuralhub.exe or dlssnr_on_amd_setup.exe).
; AmdEveryFrame=false keeps the model's temporal history (Ins menu: "Disable temporal
; stabilization", off by default).
; AmdGraphicsWait=1 is New wait mode (1-pixel draw of 0.3.1 and later; still testing).
; Set 0 for Original wait.
; Unsafe dirty insert stays off (AmdGraphicsUnsafe=0).
Enabled=false
RunBeforeSR=true
; danielblnc or mochizuki on the final D3D9, D3D10, D3D11, D3D12, Vulkan or OpenGL image of a game without an
; upscaler, HUD included. Off by default; Vulkan needs it on before the game starts. A 32-bit D3D9, D3D10, D3D11 or
; OpenGL game runs this from OptiScaler64\, beside x86\d3d9.dll, x86\dxgi.dll or x86\opengl32.dll (the installer puts
; them in place).
PresentWithoutUpscaler=false
; NR runtime: daniel (dlssnr_amd_pass1-3.dll and dlssnr_on_amd_weights.bin), lmxxf
; (LmxxfNrRuntime.dll, lmxxf-modules\, shaders\ and native-game-tiled-assets\) or mochizuki
; (MochizukiNrRuntime.dll and its dlssnr-amd\ folder). Restart after changing.
NrBackend=daniel
; lmxxf: fit a render resolution above 1080p onto the 1080 network. Can hitch at ~2K; off by default.
LmxxfFitLarge=false
; lmxxf: each pass reads its own result from the previous frame, moved by the game's motion vectors.
LmxxfTemporal=true
; lmxxf with LmxxfTemporal: where the result differs from the previous frame's by less than the
; threshold (in 1/255), blend it toward that frame, by the strength at no difference. 0 turns it off.
LmxxfSmoothStrength=0.8
LmxxfSmoothThreshold=10
; lmxxf smoothing: blend only the model's change toward the previous frame and pass the game's own
; picture through as it arrived, so the upscaler keeps its sample detail.
LmxxfSmoothResidual=false
; lmxxf: the model's Style, 0 Standard (NVIDIA's default), 1 Natural, 2 Cinematic. A change rebuilds the
; network, a hitch of a second or two.
LmxxfStyle=1
; lmxxf: reuse part of the model's work where the picture has not changed (faster on still scenes,
; slightly lossy, one pass only).
LmxxfVitReuse=false
; lmxxf: run the 1080 tier on 1088 rows instead of 1152, about 4% faster, slightly lossy at the bottom.
LmxxfCompact1080=false
; lmxxf: the 71-block model instead of the Small one of NVIDIA's 310.9.16 DLL, which runs at any number of passes
; when native-game-tiled-assets\small\ is there (about a third faster).
LmxxfOriginalModel=false
; lmxxf, three passes: run two and work out the third from them, about a third less time, close to a
; real third pass. Skin protection (two or three passes): skin-coloured areas keep pass 1's result
; (by colour, so brown and orange surfaces too).
LmxxfPassEstimate=true
LmxxfSkinProtect=false
; lmxxf: give the model a white point measured from the frame instead of a fixed one, for games that
; hand very dark linear colour (KCD2 at night) where the model shows grain or a green tint.
LmxxfAutoExposure=false
; mochizuki's tuning comes only from its own keys below, never the danielblnc, lmxxf or NVIDIA ones.
; Its network always runs inside upstream's RenoDX stage, whose MochizukiReno* settings (RenoDX's own,
; its defaults) further down hold the model, pass, resolution, composition, look and encoding controls.
; MochizukiTemporal=auto follows LmxxfTemporal.
MochizukiTemporal=true
MochizukiHistoryStrength=1
MochizukiStabilizerStrength=0
MochizukiStabilizerThreshold=2
; mochizuki ACO: the network as machine code made in advance by ACO (the Linux driver's compiler) from
; dlssnr-amd\aco\, about a fifth faster, the same picture; tested by its author on AMD Software 26.9.2.
; A change takes effect when the game starts again. Int4: with ACO on an RX 9000, upstream's
; CC_SILVER_AARDWOLD_OPTIMIZED (int4 mixed's successor), the 310.8 model made faster, the picture very close to it;
; switches without a restart. Its weights are made once from dlssnr.bin at its first use. OptimizedLayers: 0 Quality
; (no int4 layers), 1 Balanced, 2 Performance (also fixed attention on the first full-resolution layers), 3 Ultra
; Performance (more int4 layers, the fastest, the default).
MochizukiAco=true
MochizukiInt4=false
MochizukiOptimizedLayers=3
; mochizuki on an RX 6000: the INT8 form of its network (dlssnr-amd\shaders-rdna2-int8), about a tenth faster than the
; FP16 one; false keeps FP16, which also runs without that folder or when the INT8 pipelines fail. Other cards ignore
; it. Taken when the game starts.
MochizukiRdna2Int8=true
; OriginalModel: the 310.8 model instead of 310.9.16's new one (dlssnr-amd\dlssnr-small.bin, about a third faster,
; the default when it is there). Int4 always runs on the 310.8 model. ApplyModel=false runs the network
; at full cost but shows the original frame.
MochizukiOriginalModel=false
; Experimental: with ACO on an RX 9000, upstream's CC_SMALL_FP8_EXPERIMENTAL, the new model made faster, the picture
; somewhat different; off with OriginalModel or Int4. Its weights are made once from dlssnr-small.bin at its first use.
MochizukiExperimental=false
; Placement in a native D3D12 game with an upscaler: before (its input), after (its output, at display resolution) or
; present (the finished picture, HUD included, on optical flow, SDR only). Taken when the game starts.
MochizukiPlacement=before
MochizukiApplyModel=true
; mochizuki dynamic resolution: exact rebuilds the network for every render resolution (a second or
; two without NR each time); auto keeps one network, built for the largest resolution seen, once the
; render resolution drops below the colour buffer; always does so from the first frame.
MochizukiDynamicResolution=auto
; mochizuki Preprocess: changes the picture the network is shown and takes it back out of its answer.
; Off by default. Exposure 0 off, 1 auto (for games that do not hand their exposure over), 2 fixed
; (bias alone), either replacing the RenoDX stage's divisor; bias -8..8 EV; curve 0 none, 1 neutral,
; 2 reinhard, 3 filmic, 4 gt, 5 aces, 6 agx, in place of the stage's; contrast 0.5-2; saturation 0.05-2.
; auto is off unless the game is one that needs it (Crimson Desert); true or false is kept as chosen.
MochizukiPreprocess=auto
MochizukiPreprocessExposure=1
MochizukiPreprocessBias=0
MochizukiPreprocessCurve=3
MochizukiPreprocessContrast=1
MochizukiPreprocessSaturation=1
; NoiseEveryFrame: frames without motion vectors move the noise seed every frame. LayerMode is read and
; ignored (older builds' 30-layer switch). StyleGrade: Natural and Cinematic graded after the network, as
; NVIDIA's DLL does; false gives the picture these styles had before.
MochizukiNoiseEveryFrame=false
MochizukiLayerMode=0
MochizukiStyleGrade=true
; mochizuki RenoDX stage: RenoDX's encode and composition around the network (RenoComposition is not used
; with mochizuki). RenoPasses (1-4, 1-30 with RenoUnlockPasses), RenoResolutionScale and
; RenoSourceEncoding rebuild the network; the game's exposure is the stage's feed (RenoFeedMode).
MochizukiRenoIntensity=1
MochizukiRenoStyle=0
MochizukiRenoLocalTone=1
MochizukiRenoLocalStructure=1
MochizukiRenoSkinStructure=-1
MochizukiRenoAutoMask=false
MochizukiRenoSkinIndependent=true
MochizukiRenoPasses=1
MochizukiRenoUnlockPasses=false
MochizukiRenoChainedHistory=true
MochizukiRenoPass2Transfer=1
MochizukiRenoPass2Color=1
MochizukiRenoPass2Intensity=1
MochizukiRenoPass2FollowPass1=true
MochizukiRenoPass2Style=0
MochizukiRenoPass2LocalTone=1
MochizukiRenoPass2Structure=1
MochizukiRenoPass2Skin=-1
MochizukiRenoPass2AutoMask=false
MochizukiRenoPass3Transfer=1
MochizukiRenoPass3Color=1
MochizukiRenoPass3Intensity=1
MochizukiRenoPass3FollowPass1=true
MochizukiRenoPass3Style=0
MochizukiRenoPass3LocalTone=1
MochizukiRenoPass3Structure=1
MochizukiRenoPass3Skin=-1
MochizukiRenoPass3AutoMask=false
MochizukiRenoPass4Transfer=1
MochizukiRenoPass4Color=1
MochizukiRenoPass4Intensity=1
MochizukiRenoPass4FollowPass1=true
MochizukiRenoPass4Style=0
MochizukiRenoPass4LocalTone=1
MochizukiRenoPass4Structure=1
MochizukiRenoPass4Skin=-1
MochizukiRenoPass4AutoMask=false
MochizukiRenoColorStrength=1
MochizukiRenoTransferStrength=1
MochizukiRenoChromaClamp=1
MochizukiRenoTransferMode=0
MochizukiRenoStyleHighlightGuard=0
MochizukiRenoDetailStability=0
MochizukiRenoSdrBlackRestore=0
MochizukiRenoDisplayPedestal=0
MochizukiRenoCodecMode=2
MochizukiRenoNeuralFloorGuard=true
MochizukiRenoResolutionScale=1
MochizukiRenoFollowInputRes=false
MochizukiRenoLookUpsample=0
MochizukiRenoLookMode=false
MochizukiRenoLookStrength=1
MochizukiRenoLookBrighten=1
MochizukiRenoLookDarken=1
MochizukiRenoLookColour=1
MochizukiRenoLookHue=1
MochizukiRenoLookShadows=1
MochizukiRenoLookMidtones=1
MochizukiRenoLookHighlights=1
MochizukiRenoLookMaxBrighten=0
MochizukiRenoLookMaxDarken=0
MochizukiRenoLookMaxColour=0
MochizukiRenoLookTone=1
MochizukiRenoLookDetail=1
MochizukiRenoLookDetailRadius=1
MochizukiRenoLookHalo=0
MochizukiRenoLookHaloBrightSide=0
MochizukiRenoLookStabilize=0
MochizukiRenoLookStabilizeMs=60
MochizukiRenoLookStabilizeDetail=false
MochizukiRenoSourceEncoding=0
MochizukiRenoLinearUnitNits=0
MochizukiRenoProxyAnchor=4
MochizukiRenoPaperWhiteScale=2.5375
MochizukiRenoDiffuseWhiteNits=203
MochizukiRenoPQCalibration=1
MochizukiRenoFeedMode=auto
MochizukiRenoNormGovernor=2
MochizukiRenoNormCutAware=true
MochizukiRenoNormAttackStops=4
MochizukiRenoNormReleaseStops=0.5
MochizukiRenoNormSlewStops=0.02
MochizukiRenoMVecScaleX=1
MochizukiRenoMVecScaleY=1
AmdModelScale=1
AmdDynamicScale=false
AmdDynamicTargetFps=60
AmdEncoding=1
AmdEffectStrength=1.0
AmdStyle=0
AmdToneLift=0
AmdUseGameExposure=true
AmdEveryFrame=false
AmdFixedSeed=true
AmdSlots=3
AmdGraphicsWait=1
AmdGraphicsUnsafe=0
; One danielblnc job the GPU holds this long (ms) switches NR off; switch it on again to retry. 0 turns it off.
AmdStallStandDownMs=2000
AmdNeuralLighting=true
AmdNeuralLightingStrength=0.5
; auto: 0 and 2 with an upscaler, 0.8 and 4 on the final image (PresentWithoutUpscaler).
AmdStabilizerStrength=auto
AmdStabilizerThreshold=auto
; RenoDX composition after danielblnc and lmxxf (mochizuki has the RenoDX stage): off by default (RenoIntensity 0-2, RenoGuard
; 1-8x, RenoColour 0-1, RenoChromaClamp 0.25-2 stops, RenoPedestal takes off the black lift).
RenoComposition=false
RenoIntensity=1
RenoGuard=2
RenoColour=1
RenoChromaClamp=1
RenoPedestal=false
; Person protection, lmxxf and mochizuki: people keep the first pass with 2+ passes; needs person-model\ (installer).
; Model 0 PP-HumanSeg, 1 YOLO11n-seg, 2 YuNet (faces, input 320/384/416/448); strength and detail 0-1.
NrPersonPartition=false
NrPersonModel=0
NrFaceInputSize=320
NrPersonStrength=1
NrPersonDetailGain=1
NrPersonDebugMask=false
Passes=1
LocalTone=0
LocalStructure=1
SkinStructure=1
; Run the model over the frame Ray Reconstruction finished instead of over the upscaler's input.
; This is the placement FSR-RR needs: its denoise and enlargement are one dispatch, so there is no
; pre-SR seam to attach to. RRWorkingScale is relative to the finished frame (1.0 = every pixel of
; it); the AMD runtime does not supersample, so it stops at 1.0.
; RRPasses applies to the NVIDIA feature chain only -- the AMD backend runs one pass chain from
; Passes above, one runtime module per pass, capped at three by dlssnr_amd_pass1-3.dll.
; lmxxf reads Passes too, 1 to 3: its network runs again on its own output, each pass at full cost.
; On the AMD backend this key alone chooses the placement (Ins menu: "Processing point"):
; false runs the model before Super Resolution, true after the finished frame. RunBeforeSR above
; is kept for the NVIDIA backend and is not consulted there.
ApplyAfterRR=false
RRWorkingScale=1.0
RRPasses=1

"@)
# 这两段只在源 ini 里还没有的时候才追加。
# 无条件追加的写法在源 ini 哪天自带 [AmdLook]/[AmdRtgi] 时会写出重复段 ——
# 而追加的那份是 Enabled=false，可能把用户调好的值顶掉。
$amdLookBlock = @"

[AmdLook]
Enabled=false
Appearance=2
Mix=1
MaterialDetail=1.15
ShapeDefinition=1.2
LocalLighting=1.15
SkinDetail=1.1
SkinSoftness=0.486
DetectSkin=true
SpecularControl=0.58
HighlightRollOff=0.9
ColourSeparation=0
ShadowDepth=0.2
AntiHalo=0.901
FlatAreaProtection=0
Inspect=0
Tone=0
ExposureEV=1
Contrast=1
Saturation=1
HighlightCompression=0
"@

$amdRtgiBlock = @"

[AmdRtgi]
Enabled=false
Quality=2
Denoiser=1
Inspect=0
Mix=1
Lighting=5
Occlusion=1
Ambient=1
Thickness=0.1
Smoothness=0.5
Fade=0.3
Fov=60
FarPlane=600
Contact=0
Saturation=1
Radius=1
"@

if ($ini -notmatch '(?m)^\[AmdLook\]') { $ini += $amdLookBlock }
if ($ini -notmatch '(?m)^\[AmdRtgi\]') { $ini += $amdRtgiBlock }
[IO.File]::WriteAllText((Join-Path $stage 'OptiScaler.ini'), $ini, [Text.UTF8Encoding]::new($false))

$rtgiSrc = Join-Path $root 'package-amd-presr/experimental_lighting'
if (Test-Path $rtgiSrc) {
    $rtgiDst = Join-Path $stage 'experimental_lighting'
    New-Item -ItemType Directory -Path $rtgiDst -Force | Out-Null
    Get-ChildItem -LiteralPath $rtgiSrc -File | Copy-Item -Destination $rtgiDst -Force
}

# The lmxxf runtime is open source and ships in the package; its weights do not.
$lmxxfDll = Join-Path $root 'exports/lmxxf-runtime/LmxxfNrRuntime.dll'
if (!(Test-Path -LiteralPath $lmxxfDll -PathType Leaf)) {
    throw "Missing $lmxxfDll. Build it with tools\build-lmxxf-runtime.cmd exports\lmxxf-runtime."
}
Copy-Item -LiteralPath $lmxxfDll -Destination (Join-Path $stage 'LmxxfNrRuntime.dll') -Force
Copy-Item -Path (Join-Path $root 'third_party/lmxxf/modules') -Destination (Join-Path $stage 'lmxxf-modules') -Recurse -Force
# The RX 9060 series' kernels; the runtime picks them by GPU.
Copy-Item -Path (Join-Path $root 'third_party/lmxxf/modules-gfx1200') -Destination (Join-Path $stage 'lmxxf-modules-gfx1200') -Recurse -Force
# Top-level *.hlsl only: the runtime compiles its own shader-cache\ on first use.
New-Item -ItemType Directory -Path (Join-Path $stage 'shaders') -Force | Out-Null
Get-ChildItem -LiteralPath (Join-Path $root 'third_party/lmxxf/shaders') -Filter '*.hlsl' -File |
    Copy-Item -Destination (Join-Path $stage 'shaders') -Force

# The 32-bit proxies and their 64-bit host (tools\build-dx9-x86.cmd): a 32-bit D3D9, D3D10 or D3D11, or OpenGL game
# gets d3d9.dll, dxgi.dll or opengl32.dll beside it, and everything else here in OptiScaler64\ with
# OptiScalerDx9Host.exe, which runs OptiScaler.
$x86Dir = Join-Path $stage 'x86'
New-Item -ItemType Directory -Force -Path $x86Dir | Out-Null
foreach ($pair in @(@('exports/dx9-x86/d3d9.dll', 'd3d9.dll'), @('exports/dxgi-x86/dxgi.dll', 'dxgi.dll'),
        @('exports/gl-x86/opengl32.dll', 'opengl32.dll'),
        @('exports/dx9-x86/OptiScaler64/OptiScalerDx9Host.exe', 'OptiScalerDx9Host.exe'))) {
    $from = Join-Path $root $pair[0]
    if (!(Test-Path -LiteralPath $from -PathType Leaf)) {
        throw "Missing $from. Build it with tools\build-dx9-x86.cmd."
    }
    Copy-Item -LiteralPath $from -Destination (Join-Path $x86Dir $pair[1]) -Force
}

# Installer + README.
$readme = Join-Path $root 'README.md'
if (!(Test-Path $readme)) { throw "Missing $readme" }
$installerSrc = Join-Path $root 'tools/install-amd-presr.ps1'
if (!(Test-Path $installerSrc)) { throw "Missing $installerSrc" }
Copy-Item $installerSrc (Join-Path $stage 'Setup.ps1') -Force
@'
@echo off
setlocal
title OptiScaler AMD pre-SR Setup
rem No args: Setup.ps1 opens a folder picker and proxy menu.
rem Optional: Setup.bat "D:\GameFolder" [dxgi.dll]
if "%~2"=="" (
  powershell -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0Setup.ps1" -GameDir "%~1" -NoPause
) else (
  powershell -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0Setup.ps1" -GameDir "%~1" -Proxy "%~2" -NoPause
)
set "EC=%ERRORLEVEL%"
if not "%EC%"=="0" echo Setup failed ^(exit code %EC%^). See the error above.
pause
exit /b %EC%
'@ | Set-Content -LiteralPath (Join-Path $stage 'Setup.bat') -Encoding ASCII

$uninstallSrc = Join-Path $root 'tools/uninstall-amd-presr.ps1'
if (!(Test-Path $uninstallSrc)) { throw "Missing $uninstallSrc" }
Copy-Item $uninstallSrc (Join-Path $stage 'Uninstall_OptiScaler_NR.ps1') -Force
@'
@echo off
setlocal
title OptiScaler AMD pre-SR Uninstall
rem Double-click in the game folder. Optional: Uninstall_OptiScaler_NR.bat "D:\GameFolder"
if "%~1"=="" (
  powershell -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0Uninstall_OptiScaler_NR.ps1" -NoPause
) else (
  powershell -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0Uninstall_OptiScaler_NR.ps1" -GameDir "%~1" -NoPause
)
set "EC=%ERRORLEVEL%"
if not "%EC%"=="0" echo Uninstall failed ^(exit code %EC%^). See the error above.
pause
exit /b %EC%
'@ | Set-Content -LiteralPath (Join-Path $stage 'Uninstall_OptiScaler_NR.bat') -Encoding ASCII
Copy-Item $readme (Join-Path $stage 'README.md') -Force

# 绊线：这些文件名一旦出现在 stage 里就拒绝打包（含子目录，例如 Agility 误扫入 version.dll）。
# 原作者 pass（dlssnr_amd_pass*.dll）必须不在包内 —— README 明写「包里没有原作者 pass」。
# Keep this filename-only and case-insensitive: the same expression validates the
# staged tree and every entry in the finished archive.
# version-<ver>.dll is an older author runtime that Setup renamed next to Setup.bat.
# NeuralHub (0.8.0): neuralhub_host.dll (the runtime), neuralhub_stub.dll, neuralhub.exe and
# NeuralHub_v*_setup.exe. dlssnr_amd_host\ is what OptiScaler makes in a game folder for 0.8.0
# (runtime copy, nvngx copy, built weights): nothing under it may ship, whatever its name.
$forbidden = '(?i)^(nvngx.*\.dll|dlssnr_amd_pass.*\.dll|dlssnr_on_amd_weights\.bin|version(-[0-9.]+)?\.dll|dlssnr_on_amd_setup\.exe|neuralhub_host\.dll|neuralhub_stub\.dll|neuralhub.*\.exe)$'
$forbiddenDir = '(?i)(^|[\\/])dlssnr_amd_host([\\/]|$)'
$badAll = Get-ChildItem -LiteralPath $stage -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match $forbidden -or $_.FullName.Substring($stage.Length) -match $forbiddenDir }
if ($badAll) {
    throw "Refusing to package proprietary/user-supplied file: $(($badAll | ForEach-Object { $_.FullName.Substring($stage.Length+1) }) -join ', ')"
}

$hashes = Get-ChildItem -LiteralPath $stage -Recurse -File |
    Where-Object { $_.Name -ne 'SHA256SUMS.txt' } |
    Sort-Object FullName |
    ForEach-Object {
        # 正斜杠：清单是 coreutils 格式（`sha256sum -c` 用），反斜杠分隔符在 git-bash /
        # Linux 上认不出来。Windows 侧 PowerShell 用正斜杠访问文件同样正常。
        '{0} *{1}' -f (Get-Sha256 $_.FullName), ($_.FullName.Substring($stage.Length + 1) -replace '\\', '/')
    }
# 不要用 Set-Content -Encoding UTF8：Windows PowerShell 5.1 的 -Encoding UTF8 会写 BOM，
# BOM 直接粘在第一个哈希前面，用户跑 `sha256sum -c SHA256SUMS.txt` 会看到
# "1 line is improperly formatted"，第一项永远验不过。走 .NET 的无 BOM UTF-8。
# 行尾用 LF 而不是 CRLF：CRLF 会在文件名后留下 \r，`sha256sum -c` 把它当成文件名的一部分，
# 23 项全部 "No such file or directory"。LF + 正斜杠才能让标准工具真的验得了。
# 记事本（Win10 1809+）与 PowerShell 读 LF 都正常。
[IO.File]::WriteAllText((Join-Path $stage 'SHA256SUMS.txt'),
    (($hashes -join "`n") + "`n"),
    [Text.UTF8Encoding]::new($false))

New-Item -ItemType Directory -Force -Path (Join-Path $root $OutDir) | Out-Null
if (Test-Path $zip) { Remove-Item $zip -Force }
# Not Compress-Archive: under Windows PowerShell 5.1 it writes entries as OptiScaler\libxess.dll, and the
# installer's pins and every unzip tool expect the forward slashes the zip format specifies.
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$out = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
    $stageFull = (Resolve-Path -LiteralPath $stage).Path.TrimEnd('\') + '\'
    foreach ($file in Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName) {
        $entry = $file.FullName.Substring($stageFull.Length).Replace('\', '/')
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($out, $file.FullName, $entry,
            [IO.Compression.CompressionLevel]::Optimal)
    }
} finally {
    $out.Dispose()
}

# Validate the actual artifact, not only the staging tree. This catches a changed
# archive command, a stale/wrapped staging directory, or anything injected between
# the preflight above and Compress-Archive. An unreadable archive also fails closed.
try {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
        $badZip = @($archive.Entries |
            Where-Object { ($_.FullName -match $forbiddenDir) -or
                (-not [string]::IsNullOrEmpty($_.Name) -and $_.Name -match $forbidden) } |
            ForEach-Object { $_.FullName })
    } finally {
        $archive.Dispose()
    }
    if ($badZip.Count -gt 0) {
        throw "Forbidden proprietary/user-supplied file in zip: $($badZip -join ', ')"
    }
} catch {
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    throw "Package archive validation failed; zip removed: $($_.Exception.Message)"
}
Write-Host "Product: $Version"
Write-Host "Staged:  $stage"
Write-Host "Zip:     $zip"
Write-Host "Opti:    $OptiDll"
