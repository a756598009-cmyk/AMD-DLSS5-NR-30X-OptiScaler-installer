# build-kit.ps1 <runtime dir> <old INT8 set> <reference dir> <nrkit dir> <test image> <license> <kit dir>: lays the RX 6000
# confirmation kit out in <kit dir>\OpenNR-RX6000-test (kit v8's build-kit-v8.ps1 without the diagnostic files).
# <runtime dir>: the next version's build (MochizukiNrRuntime.dll, dlssnr-amd\shaders-rdna2, shaders-rdna2-int8,
# shaders\renodx). <old INT8 set>: 0.6.9's shaders-rdna2-int8. <nrkit dir>: holds kit v8's nrkit.exe. Every binary in
# one LZX cabinet, kit.sha256 over everything it unpacks to, each INT8 set's files that an earlier set has kept once
# (same.txt).
param([string]$runtime, [string]$oldSet, [string]$reference, [string]$diag, [string]$image, [string]$license, [string]$kitRoot)
$ErrorActionPreference = 'Stop'
# Only the FP8 pictures fit (each ~0.2 MiB; 10 MiB for the zip). The INT8 sets are held to FP8 and to this card's FP16.
$Refs = @('img_small_1280x720_q', 'img_small_1920x1080_q', 'img_large_1280x720_q')
New-Item -ItemType Directory -Force -Path $kitRoot | Out-Null
$kitRoot = (Resolve-Path $kitRoot).Path
$kit = Join-Path $kitRoot 'OpenNR-RX6000-test'
$f = Join-Path $kit 'files'
$stage = Join-Path $kitRoot 'cab'
foreach ($d in $kit, $stage) { if (Test-Path -LiteralPath $d) { Remove-Item -LiteralPath $d -Recurse -Force } }
New-Item -ItemType Directory -Force -Path $f, "$stage\dlssnr-amd\shaders", "$stage\reference" | Out-Null
Copy-Item "$runtime\MochizukiNrRuntime.dll" "$stage\"
python -I "$PSScriptRoot\prune.py" "$runtime\dlssnr-amd\shaders-rdna2" "$stage\dlssnr-amd\shaders-rdna2"
python -I "$PSScriptRoot\prune.py" "$runtime\dlssnr-amd\shaders-rdna2-int8" "$stage\dlssnr-amd\shaders-rdna2-int8"
python -I "$PSScriptRoot\prune.py" $oldSet "$stage\dlssnr-amd\shaders-rdna2-int8-old"
Copy-Item -Recurse "$runtime\dlssnr-amd\shaders\renodx" "$stage\dlssnr-amd\shaders\renodx"
python -I "$PSScriptRoot\stripspv.py" "$stage\dlssnr-amd"
if ($LASTEXITCODE) { throw 'stripspv failed' }
$opt = 'C:\VulkanSDK\1.4.357.0\Bin\spirv-opt.exe'
# The INT8 sets' kernels that are the FP16 set's own bytes in 0.6.9 (the ViT GEMMs, vitattn, ...) stay those bytes, so
# the kit times exactly what 0.6.9 runs there and the cabinet keeps them once; only the real INT8 bodies lose their dead
# FP16 branches.
foreach ($x in Get-ChildItem -LiteralPath "$stage\dlssnr-amd" -Directory -Filter 'shaders-rdna2-int8*' | Get-ChildItem -Recurse -Filter *.spv) {
    $rel = $x.FullName.Substring($x.FullName.IndexOf('\shaders-rdna2-int8') + 1) -replace '^shaders-rdna2-int8(-old)?\\', ''
    $f16 = "$stage\dlssnr-amd\shaders-rdna2\$rel"
    if ((Test-Path -LiteralPath $f16) -and (Get-FileHash $f16).Hash -eq (Get-FileHash $x.FullName).Hash) { continue }
    & $opt --eliminate-dead-branches --eliminate-dead-code-aggressive --merge-blocks $x.FullName -o $x.FullName
    if ($LASTEXITCODE) { throw "spirv-opt failed: $($x.FullName)" }
}
$d = "$stage\dlssnr-amd"
$sets = @('shaders-rdna2', 'shaders-rdna2-int8', 'shaders-rdna2-int8-old')
$same = @()
for ($i = 1; $i -lt $sets.Count; ++$i) {
    foreach ($x in Get-ChildItem -LiteralPath "$d\$($sets[$i])" -Recurse -File) {
        $rel = $x.FullName.Substring("$d\$($sets[$i])".Length + 1)
        $h = (Get-FileHash $x.FullName).Hash
        foreach ($src in $sets[0..($i - 1)]) {
            $y = "$d\$src\$rel"
            if ((Test-Path -LiteralPath $y) -and (Get-FileHash $y).Hash -eq $h) { $same += "$($sets[$i])\$rel|$src\$rel"; break }
        }
    }
}
Copy-Item $license "$stage\dlssnr-amd\LICENSE-DLSSNR-AMD.txt"
foreach ($r in $Refs) { Copy-Item "$reference\$r.jxr" "$stage\reference\" }
Copy-Item "$reference\refs.txt" "$stage\reference\"
Copy-Item $image "$stage\test.jxr"
Copy-Item "$diag\nrkit.exe" "$stage\"
$lines = foreach ($x in Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName) {
    '{0}  {1}' -f (Get-FileHash $x.FullName -Algorithm SHA256).Hash.ToLower(), $x.FullName.Substring($stage.Length + 1)
}
Set-Content -LiteralPath "$f\kit.sha256" -Value $lines
foreach ($l in $same) { Remove-Item -LiteralPath (Join-Path $d ($l -split '\|')[0]) }
Set-Content -LiteralPath "$f\same.txt" -Value $same
python -I "$PSScriptRoot\mkcab.py" $stage "$f\kit.cab"
if ($LASTEXITCODE) { throw 'makecab failed' }
Copy-Item "$PSScriptRoot\kit.ps1", "$PSScriptRoot\cases.ps1" "$f\"
Copy-Item "$PSScriptRoot\run-test.bat", "$PSScriptRoot\README.txt" "$kit\"
Remove-Item -LiteralPath $stage -Recurse -Force
'kit laid out in {0}: kit.cab {1:N2} MiB, {2} shared files' -f $kit, ((Get-Item "$f\kit.cab").Length / 1MB), $same.Count
