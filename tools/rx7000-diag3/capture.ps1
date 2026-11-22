# capture.ps1 <work> <dispatch,dispatch,...>: on this machine (RX 9070 XT), the RDNA3 network forced, a clean 1152x648 run
# (small model, seed 0, no history, 12 frames) per dispatch with MZ_KCAP at recording 3, into <work>\cap\D<n>.
# <work> holds mz_e2e.exe, test.png, diag\MochizukiNrRuntime.dll (built with MZ_KCAP) and dlssnr-amd\ (shaders and models).
param([string]$work, [string]$disp)
$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $work
$env:MZ_RDNA3_CHECK = '0'; $env:MZ_FORCE_RDNA3 = '1'; $env:E2E_W = '1152'; $env:E2E_H = '648'; $env:E2E_FLAGS = '0'
$env:E2E_CREATE_FLAGS = '2'; $env:NR_DIAG_SEED0 = '1'; $env:NR_DIAG_TGATE0 = '1'; $env:MZ_KCAP_FRAME = '3'
foreach ($d in $disp -split ",") {
    $env:MZ_KCAP = "cap\D$d"; $env:MZ_KCAP_DISP = "$d"
    & .\mz_e2e.exe .\diag\MochizukiNrRuntime.dll . test.png "cap\out_D$d.bin" 12 1 > "cap\run_D$d.txt" 2>&1
    if ($LASTEXITCODE -or !(Test-Path "cap\D$d\capture.txt")) { throw "capture of dispatch $d failed (cap\run_D$d.txt)" }
    Write-Host "D$d $((Get-Content "cap\D$d\capture.txt" | Select-String '^spirv').Line)"
}
