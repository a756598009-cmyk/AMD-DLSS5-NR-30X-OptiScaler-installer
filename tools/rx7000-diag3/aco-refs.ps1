# aco-refs.ps1 <work> <key,key,...>: the RX 9070 XT's own network (ACO, FP8) in the same clean 1152x648 run as
# capture.ps1 (but no persistent runs), one run per plan key with MZ_STAGE_FULL=<key>: <work>\aco\k<key>\full_<key>_arena.bin and stages.txt.
param([string]$work, [string]$keys)
$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $work
foreach ($k in 'MZ_FORCE_RDNA3', 'MZ_RDNA3_CHECK', 'MZ_KCAP', 'MZ_KCAP_DISP') { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
$env:E2E_W = '1152'; $env:E2E_H = '648'; $env:E2E_FLAGS = '0'; $env:E2E_CREATE_FLAGS = '2'
$env:NR_DIAG_SEED0 = '1'; $env:NR_DIAG_TGATE0 = '1'; $env:MZ_STAGE_FRAME = '3'
# One dispatch per layer: a persistent run's inner values are not in its stage dump.
$env:NR_NO_PERSIST = '1'
New-Item -ItemType Directory -Force -Path aco | Out-Null
foreach ($k in $keys -split ',') {
    $env:MZ_STAGE_DUMP = "aco\k$k"; $env:MZ_STAGE_FULL = "$k"
    & .\mz_e2e.exe .\diag\MochizukiNrRuntime.dll . test.png "aco\out_k$k.bin" 12 1 > "aco\run_k$k.txt" 2>&1
    if ($LASTEXITCODE -or !(Test-Path "aco\k$k\full_${k}_arena.bin")) { throw "ACO run for key $k failed (aco\run_k$k.txt)" }
    Write-Host "key $k $((Get-Item "aco\k$k\full_${k}_arena.bin").Length) bytes"
}
