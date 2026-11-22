# build-kit.ps1 <runtime dir> <reference dir> <kit dir> <aco bundle> <prewarm list>
# Lays the kit out in <kit dir>\OpenNR-RX6000-test: the runtime (<runtime dir>: MochizukiNrRuntime.dll and
# dlssnr-amd\shaders-rdna2 + shaders\renodx, as tools\build-mochizuki-runtime.cmd exports them) with the shipped set's
# generator word (cases.ps1) and its prewarm list (manifest-rdna2.txt, rehashed to that word), the ACO bundle as
# produce-rdna2.ps1 makes it (aco-gfx103x\shaders + records, untouched: the records match its SPIR-V), the references
# (make-refs.ps1) and the tools (build-tools.cmd).
param([string]$runtime, [string]$reference, [string]$kitRoot, [string]$aco, [string]$manifest, [int]$word = 0x7761)
$ErrorActionPreference = 'Stop'
$runtime, $reference, $aco, $manifest = $runtime, $reference, $aco, $manifest | ForEach-Object { (Resolve-Path $_).Path }
New-Item -ItemType Directory -Force -Path $kitRoot | Out-Null
$kitRoot = (Resolve-Path $kitRoot).Path
$kit = Join-Path $kitRoot 'OpenNR-RX6000-test'
$f = Join-Path $kit 'files'
if (Test-Path -LiteralPath $kit) { Remove-Item -LiteralPath $kit -Recurse -Force }
New-Item -ItemType Directory -Force -Path "$f\dlssnr-amd\shaders", "$f\dlssnr-amd\prewarm", "$f\reference" | Out-Null
Copy-Item "$runtime\MochizukiNrRuntime.dll" "$f\"
Copy-Item -Recurse "$runtime\dlssnr-amd\shaders-rdna2" "$f\dlssnr-amd\shaders-rdna2"
Copy-Item -Recurse "$runtime\dlssnr-amd\shaders\renodx" "$f\dlssnr-amd\shaders\renodx"
python -I "$PSScriptRoot\genword.py" "$f\dlssnr-amd\shaders-rdna2" $word
python -I "$PSScriptRoot\genword.py" "$f\dlssnr-amd\shaders\renodx" $word
Copy-Item $manifest "$f\dlssnr-amd\prewarm\manifest-rdna2.txt"
python -I "$PSScriptRoot\rehash_manifest.py" "$f\dlssnr-amd\prewarm\manifest-rdna2.txt" "$f\dlssnr-amd\shaders-rdna2"
if ($LASTEXITCODE) { throw 'prewarm list rehash failed' }
Copy-Item "$PSScriptRoot\..\..\third_party\mochizuki\LICENSE" "$f\dlssnr-amd\LICENSE-DLSSNR-AMD.txt"
Copy-Item -Recurse $aco "$f\aco-gfx103x"
Copy-Item "$reference\img_*.jxr", "$reference\refs.txt" "$f\reference\"
& cmd /c "`"$PSScriptRoot\build-tools.cmd`" `"$f`"" | Select-Object -Last 1
if ($LASTEXITCODE) { throw 'tools build failed' }
Copy-Item "$PSScriptRoot\kit.ps1", "$PSScriptRoot\cases.ps1" "$f\"
Copy-Item 'E:\Projetos\opti_rr_dlss5\arrumar_cor\original_sem_nr.png' "$f\test.png"
Copy-Item "$PSScriptRoot\run-test.bat", "$PSScriptRoot\README.txt" "$kit\"
'kit laid out in ' + $kit
