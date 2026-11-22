# build-kit.ps1 <release dir> <diag runtime dir> <edge dir> <reference dir> <kit dir>
# Lays the kit out in <kit dir>\OpenNR-RX7000-diag: the released 0.5.8 mochizuki files (<release dir>\main and \rdna3,
# the HF zips unpacked), the diagnostic DLL (tools\build-mochizuki-runtime.cmd with MZ_STAGE_DUMP), the edge-body
# variants (build_edge.py), the references (make-refs.ps1) and the tools (build-tools.cmd).
param([string]$release, [string]$diag, [string]$edge, [string]$reference, [string]$kitRoot)
$ErrorActionPreference = 'Stop'
$kit = Join-Path $kitRoot 'OpenNR-RX7000-diag'
$f = Join-Path $kit 'files'
if (Test-Path -LiteralPath $kit) { Remove-Item -LiteralPath $kit -Recurse -Force }
New-Item -ItemType Directory -Force -Path "$f\rel", "$f\diag", "$f\dlssnr-amd\shaders-rdna3\runtime", "$f\dlssnr-amd\shaders-rdna3\temporal",
    "$f\dlssnr-amd\shaders\renodx", "$f\variants", "$f\reference" | Out-Null
Copy-Item "$release\main\MochizukiNrRuntime.dll" "$f\rel\"
Copy-Item "$diag\MochizukiNrRuntime.dll" "$f\diag\"
Copy-Item "$release\rdna3\dlssnr-amd.shaders-rdna3\*" "$f\dlssnr-amd\shaders-rdna3\"
Copy-Item "$release\rdna3\dlssnr-amd.shaders-rdna3.runtime\*" "$f\dlssnr-amd\shaders-rdna3\runtime\"
Copy-Item "$release\rdna3\dlssnr-amd.shaders-rdna3.temporal\*" "$f\dlssnr-amd\shaders-rdna3\temporal\"
Copy-Item "$release\main\dlssnr-amd.shaders.renodx\*" "$f\dlssnr-amd\shaders\renodx\"
Copy-Item "$release\main\dlssnr-amd\LICENSE-DLSSNR-AMD.txt" "$f\dlssnr-amd\"
# edge6 (fswinp256 left as released) is edge7 without g_fswinp256.spv: the kit takes it from edge7.
Copy-Item -Recurse "$edge\edge7" "$f\variants\edge7"
Copy-Item -Recurse "$reference\*" "$f\reference\"
Remove-Item "$f\reference\refs.txt" -ErrorAction SilentlyContinue
& cmd /c "`"$PSScriptRoot\build-tools.cmd`" `"$f`"" | Select-Object -Last 1
if ($LASTEXITCODE) { throw 'tools build failed' }
Copy-Item "$PSScriptRoot\kit.ps1", "$PSScriptRoot\sizes.ps1" "$f\"
Copy-Item 'E:\Projetos\opti_rr_dlss5\arrumar_cor\original_sem_nr.png' "$f\test.png"
Copy-Item "$PSScriptRoot\run-test.bat", "$PSScriptRoot\README.txt" "$kit\"
'kit laid out in ' + $kit
