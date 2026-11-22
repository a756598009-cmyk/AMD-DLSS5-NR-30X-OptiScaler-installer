# pack.ps1 <RX 7000 speed kit laid out (...\OpenNR-RX7000-speed\files)> <runtime build dir> <large refs dir> <prewarm list> <out dir>
# Lays the RX 7000 confirmation kit out in <out dir>\OpenNR-RX7000-confirm and zips it. From the speed kit: mz_e2e,
# compare, vkprobe, test.png and the RX 9070 XT references (small model). <large refs dir>: img_large_1280x720_* and
# img_large_1920x1080_* (the 310.8 model on the RX 9070 XT, same harness and frames). From the build:
# MochizukiNrRuntime.dll (k\), dlssnr-amd\shaders and shaders-rdna3, and aco-gfx11 (put in place only for the ACO runs).
# <prewarm list>: the RX 7000 network's manifest-rdna3.txt; only the shaders-rdna3 modules it names go in (every
# pipeline the network builds at 648p-4K with both models), and of shaders\ only renodx\ (the RDNA3 network's runtime
# passes are shaders-rdna3\runtime\), which keeps the zip small. The modules are copied as built: ACO's records are named
# by their bytes.
param([string]$speed, [string]$build, [string]$refs, [string]$prewarm, [string]$out)
$ErrorActionPreference = 'Stop'
$kit = Join-Path $out 'OpenNR-RX7000-confirm'
$f = Join-Path $kit 'files'
if (Test-Path -LiteralPath $kit) { Remove-Item -LiteralPath $kit -Recurse -Force }
New-Item -ItemType Directory -Force -Path "$f\k", "$f\reference", "$f\dlssnr-amd\shaders" | Out-Null
Copy-Item "$build\MochizukiNrRuntime.dll" "$f\k\"
Copy-Item -Recurse "$build\dlssnr-amd\shaders\renodx" "$f\dlssnr-amd\shaders\renodx"
$keep = @(Get-Content -LiteralPath $prewarm | Where-Object { $_ -match '^p (\S+) ' } | ForEach-Object { ($_ -split ' ')[1] })
$src = (Resolve-Path -LiteralPath "$build\dlssnr-amd\shaders-rdna3").Path
foreach ($x in Get-ChildItem -LiteralPath $src -Recurse -File) {
    $rel = $x.FullName.Substring($src.Length + 1).Replace('\', '/')
    if ($x.Extension -eq '.spv' -and $rel -notlike 'runtime/*' -and $keep -notcontains $rel) { continue }
    $to = Join-Path "$f\dlssnr-amd\shaders-rdna3" $rel
    New-Item -ItemType Directory -Force -Path (Split-Path $to) | Out-Null
    Copy-Item -LiteralPath $x.FullName -Destination $to
}
Copy-Item -Recurse "$build\dlssnr-amd\aco-gfx11" "$f\aco-gfx11"
foreach ($s in 'records', 'shaders') {
    New-Item -ItemType Directory -Force -Path "$f\dlssnr-amd\aco\$s" | Out-Null
    Set-Content -LiteralPath "$f\dlssnr-amd\aco\$s\README.txt" -Value 'Empty on purpose: the RX 7000 network uses dlssnr-amd\aco-gfx11.'
}
Copy-Item "$speed\mz_e2e.exe", "$speed\compare.exe", "$speed\vkprobe.exe", "$speed\test.png" "$f\"
Copy-Item "$speed\reference\*.jxr", "$refs\img_large_*.jxr" "$f\reference\"
Copy-Item "$PSScriptRoot\kit.ps1" "$f\"
Copy-Item "$PSScriptRoot\run-test.bat", "$PSScriptRoot\README.txt" "$kit\"
$zip = Join-Path $out 'OpenNR-RX7000-confirm.zip'
Remove-Item -LiteralPath $zip -ErrorAction SilentlyContinue
Compress-Archive -Path $kit -DestinationPath $zip -CompressionLevel Optimal
$h = (Get-FileHash -Algorithm SHA256 $zip).Hash
Set-Content -LiteralPath "$zip.sha256.txt" -Value "$h  OpenNR-RX7000-confirm.zip"
"$zip $((Get-Item $zip).Length) $h"
