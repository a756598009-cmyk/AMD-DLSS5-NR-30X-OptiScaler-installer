# build-kit2.ps1 <release dir> <diag runtime dir> <fix runtime dir> <kit 1 reference dir> <kit 2 reference dir> <kit dir>
# Lays kit 2 out in <kit dir>\OpenNR-RX7000-diag2: the released 0.5.8 DLL and RDNA3 shaders (<release dir>\main and
# \rdna3, the HF zips unpacked), 0.5.8 with the stage dump (rdiag), the fixed DLL, kit 1's picture references for
# kit 2's sizes (sizes2.ps1), the clean references (make-refs2.ps1) and the tools (..\rx7000-diag\build-tools.cmd).
param([string]$release, [string]$diag, [string]$fix, [string]$reference, [string]$reference2, [string]$kitRoot)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'sizes2.ps1')
$kit = Join-Path $kitRoot 'OpenNR-RX7000-diag2'
$f = Join-Path $kit 'files'
if (Test-Path -LiteralPath $kit) { Remove-Item -LiteralPath $kit -Recurse -Force }
New-Item -ItemType Directory -Force -Path "$f\rel", "$f\rdiag", "$f\fix", "$f\dlssnr-amd\shaders-rdna3\runtime",
    "$f\dlssnr-amd\shaders-rdna3\temporal", "$f\dlssnr-amd\shaders\renodx", "$f\reference" | Out-Null
Copy-Item "$release\main\MochizukiNrRuntime.dll" "$f\rel\"
Copy-Item "$diag\MochizukiNrRuntime.dll" "$f\rdiag\"
Copy-Item "$fix\MochizukiNrRuntime.dll" "$f\fix\"
Copy-Item "$release\rdna3\dlssnr-amd.shaders-rdna3\*" "$f\dlssnr-amd\shaders-rdna3\"
Copy-Item "$release\rdna3\dlssnr-amd.shaders-rdna3.runtime\*" "$f\dlssnr-amd\shaders-rdna3\runtime\"
Copy-Item "$release\rdna3\dlssnr-amd.shaders-rdna3.temporal\*" "$f\dlssnr-amd\shaders-rdna3\temporal\"
Copy-Item "$release\main\dlssnr-amd.shaders.renodx\*" "$f\dlssnr-amd\shaders\renodx\"
Copy-Item "$release\main\dlssnr-amd\LICENSE-DLSSNR-AMD.txt" "$f\dlssnr-amd\"
foreach ($c in $Runs) {
    Copy-Item "$reference\img_$($c.name)_*.jxr", "$reference2\clean_$($c.name)_*.jxr" "$f\reference\"
    Copy-Item -Recurse "$reference2\cstages_$($c.name)" "$f\reference\"
}
& cmd /c "`"$PSScriptRoot\..\rx7000-diag\build-tools.cmd`" `"$f`"" | Select-Object -Last 1
if ($LASTEXITCODE) { throw 'tools build failed' }
Remove-Item "$f\pipecheck.exe"
Copy-Item "$PSScriptRoot\kit2.ps1", "$PSScriptRoot\sizes2.ps1" "$f\"
Copy-Item 'E:\Projetos\opti_rr_dlss5\arrumar_cor\original_sem_nr.png' "$f\test.png"
Copy-Item "$PSScriptRoot\run-test.bat", "$PSScriptRoot\README.txt" "$kit\"
'kit laid out in ' + $kit
