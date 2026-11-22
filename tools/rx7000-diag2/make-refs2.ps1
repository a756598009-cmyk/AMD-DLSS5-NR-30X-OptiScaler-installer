# make-refs2.ps1 <work> <reference out>: the RX 9070 XT references kit 2 adds to kit 1's, made in <work> (diag\ the
# released 0.5.8 DLL with MZ_STAGE_DUMP, dlssnr-amd\ the released shaders and both models, mz_e2e.exe, compare.exe,
# stagecmp.exe, test.png). "Clean" runs: NR_DIAG_SEED0=1 and NR_DIAG_TGATE0=1 (noise seed 0, no history), so every
# frame computes the same thing and a value can only change between runs through the network itself; 12 frames,
# stage dump at recording 3, ACO (the Linux kernel set, the one the RDNA3 network runs). Each size is run twice and
# must give the same picture and the same dump.
# Writes <reference out>\clean_<model>_<w>x<h>_{q,b,r}.jxr, cstages_<model>_<w>x<h>\stages.{txt,bin}, refs2.txt.
param([string]$work, [string]$out)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'sizes2.ps1')
Set-Location -LiteralPath $work
New-Item -ItemType Directory -Force -Path $out, 'refrun2' | Out-Null
$log = Join-Path $out 'refs2.txt'
"RX 9070 XT clean references, $(Get-Date -Format 'yyyy-MM-dd HH:mm'); ACO, NR_DIAG_SEED0=1 NR_DIAG_TGATE0=1, $CleanFrames frames, dump at recording $CleanDumpAt" |
    Set-Content -LiteralPath $log
foreach ($k in 'MZ_FORCE_RDNA3', 'DLSSNR_MODEL', 'E2E_PAN', 'MZ_STAGE_DUMP', 'MZ_STAGE_FULL', 'NR_ARENA_REUSE', 'NR_TCHAIN') {
    Remove-Item "Env:$k" -ErrorAction SilentlyContinue
}
$env:E2E_CREATE_FLAGS = '2'; $env:NR_DIAG_SEED0 = '1'; $env:NR_DIAG_TGATE0 = '1'; $env:MZ_STAGE_FRAME = "$CleanDumpAt"
foreach ($c in $Runs) {
    $name = $c.name; $w = $c.w; $h = $c.h
    $env:E2E_W = "$w"; $env:E2E_H = "$h"; $env:E2E_FLAGS = $(if ($c.model -eq 'large') { '8192' } else { '0' })
    $p = "refrun2\$name"
    foreach ($k in 1, 2) {
        Remove-Item -Recurse -Force "$p.d$k" -ErrorAction SilentlyContinue
        $env:MZ_STAGE_DUMP = "$p.d$k"
        & .\mz_e2e.exe .\diag\MochizukiNrRuntime.dll . test.png "$p.c$k.bin" $CleanFrames 1 > "$p.c$k.txt" 2>&1
        if ($LASTEXITCODE) { throw "run failed: $name $k (see $p.c$k.txt)" }
    }
    Remove-Item Env:MZ_STAGE_DUMP
    $same = (Get-FileHash "$p.c1.bin").Hash -eq (Get-FileHash "$p.c2.bin").Hash
    $runs = (& .\stagecmp.exe --runs "$p.d1" "$p.d2" | Select-Object -First 1)
    & .\compare.exe --pack "$p.c1.bin" $w $h (Join-Path $out "clean_$name")
    if ($LASTEXITCODE) { throw "pack failed: $name" }
    $sd = Join-Path $out "cstages_$name"
    New-Item -ItemType Directory -Force -Path $sd | Out-Null
    Copy-Item "$p.d1\stages.txt", "$p.d1\stages.bin" $sd
    $line = "$name  two runs identical: $same; $runs"
    Add-Content -LiteralPath $log -Value $line
    Write-Host $line
}
foreach ($k in 'E2E_W', 'E2E_H', 'E2E_FLAGS', 'E2E_CREATE_FLAGS', 'MZ_STAGE_FRAME', 'NR_DIAG_SEED0', 'NR_DIAG_TGATE0') {
    Remove-Item "Env:$k" -ErrorAction SilentlyContinue
}
