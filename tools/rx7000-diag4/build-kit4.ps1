# build-kit4.ps1 <kit 2 files dir> <kit 4 DLL> <seqref dir> <kit dir>
# Lays kit 4 out in <kit dir>\OpenNR-RX7000-diag4: from kit 2 (unpacked) the released 0.5.8 DLL (rel\), the released
# RDNA3 and RenoDX shaders, the tools (mz_e2e, compare, vkprobe and its probes), test.png and the RX 9070 XT picture
# references of kit 4's four sizes; the kit 4 DLL (k4\); the FP8 references of the sequence check (make-seqref.ps1).
param([string]$kit2, [string]$dll, [string]$seqref, [string]$kitRoot)
$ErrorActionPreference = 'Stop'
$kit = Join-Path $kitRoot 'OpenNR-RX7000-diag4'
$f = Join-Path $kit 'files'
if (Test-Path -LiteralPath $kit) { Remove-Item -LiteralPath $kit -Recurse -Force }
New-Item -ItemType Directory -Force -Path "$f\rel", "$f\k4", "$f\reference\seqref" | Out-Null
Copy-Item "$kit2\rel\MochizukiNrRuntime.dll" "$f\rel\"
Copy-Item $dll "$f\k4\"
Copy-Item -Recurse "$kit2\dlssnr-amd" "$f\"
Get-ChildItem "$f\dlssnr-amd" -Filter *.bin | Remove-Item
Copy-Item -Recurse "$kit2\probe" "$f\"
Copy-Item "$kit2\mz_e2e.exe", "$kit2\compare.exe", "$kit2\vkprobe.exe", "$kit2\test.png" "$f\"
foreach ($n in 'small_1152x648', 'small_1280x720', 'small_1920x1080', 'small_2560x1440') { Copy-Item "$kit2\reference\img_${n}_*.jxr" "$f\reference\" }
foreach ($s in '1152x648', '1280x720', '1920x1080') {
    New-Item -ItemType Directory -Force -Path "$f\reference\seqref\$s" | Out-Null
    Copy-Item "$seqref\$s\ref_k*.bin" "$f\reference\seqref\$s\"
}
Copy-Item "$PSScriptRoot\kit4.ps1" "$f\"
Copy-Item "$PSScriptRoot\run-test.bat", "$PSScriptRoot\README.txt" "$kit\"
'kit laid out in ' + $kit
