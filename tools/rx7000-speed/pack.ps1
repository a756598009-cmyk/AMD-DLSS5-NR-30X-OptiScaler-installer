# pack.ps1 <kit 7 laid out (...\OpenNR-RX7000-diag7\files)> <runtime build dir> <out dir>
# Lays the RX 7000 speed kit out in <out dir>\OpenNR-RX7000-speed and zips it. From kit 7: mz_e2e, compare, vkprobe,
# test.png, the RX 9070 XT references and the two gfx11 ACO bundles (fix = the 0.5.9 kernels, coop = 0.5.8's attention).
# From the build: MochizukiNrRuntime.dll (k\), dlssnr-amd\shaders and shaders-rdna3 (with g_vitattncm.spv).
param([string]$k7, [string]$build, [string]$out)
$ErrorActionPreference = 'Stop'
$kit = Join-Path $out 'OpenNR-RX7000-speed'
$f = Join-Path $kit 'files'
if (Test-Path -LiteralPath $kit) { Remove-Item -LiteralPath $kit -Recurse -Force }
New-Item -ItemType Directory -Force -Path "$f\k", "$f\reference", "$f\bundles", "$f\dlssnr-amd" | Out-Null
Copy-Item "$build\MochizukiNrRuntime.dll" "$f\k\"
foreach ($s in 'shaders', 'shaders-rdna3') { Copy-Item -Recurse "$build\dlssnr-amd\$s" "$f\dlssnr-amd\$s" }
foreach ($s in 'records', 'shaders') {
    New-Item -ItemType Directory -Force -Path "$f\dlssnr-amd\aco\$s" | Out-Null
    Set-Content -LiteralPath "$f\dlssnr-amd\aco\$s\README.txt" -Value 'Empty on purpose: the RX 7000 network uses dlssnr-amd\aco-gfx11.'
}
foreach ($b in 'fix', 'coop') { Copy-Item -Recurse "$k7\bundles\$b" "$f\bundles\$b" }
Copy-Item "$k7\mz_e2e.exe", "$k7\compare.exe", "$k7\vkprobe.exe", "$k7\test.png" "$f\"
Copy-Item "$k7\reference\*.jxr" "$f\reference\"
Copy-Item "$PSScriptRoot\kit.ps1" "$f\"
Copy-Item "$PSScriptRoot\run-test.bat", "$PSScriptRoot\README.txt" "$kit\"
$zip = Join-Path $out 'OpenNR-RX7000-speed.zip'
Remove-Item -LiteralPath $zip -ErrorAction SilentlyContinue
Compress-Archive -Path $kit -DestinationPath $zip -CompressionLevel Optimal
$h = (Get-FileHash -Algorithm SHA256 $zip).Hash
Set-Content -LiteralPath "$zip.sha256.txt" -Value "$h  OpenNR-RX7000-speed.zip"
"$zip $((Get-Item $zip).Length) $h"
