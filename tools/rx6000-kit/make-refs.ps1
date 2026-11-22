# make-refs.ps1 <work> <reference out>: the RX 9070 XT references of the RX 6000 kit. <work> holds the kit's
# MochizukiNrRuntime.dll, mz_e2e.exe, compare.exe, test.png and dlssnr-amd\ with shaders\ (the FP8 network),
# shaders-rdna2\ and both models. Per quality case of cases.ps1: the FP8 network's clean output packed as
# img_<case>_{q,b,r}.jxr, and the RDNA2 network forced on this card (MZ_FORCE_RDNA2=1) scored against it.
param([string]$work, [string]$out)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'cases.ps1')
Set-Location -LiteralPath $work
New-Item -ItemType Directory -Force -Path $out, 'refrun' | Out-Null
$log = Join-Path $out 'refs.txt'
"RX 9070 XT references, $(Get-Date -Format 'yyyy-MM-dd HH:mm'); FP8 network, clean (NR_DIAG_SEED0=1 NR_DIAG_TGATE0=1), $QualityFrames frames, 1 pass" |
    Set-Content -LiteralPath $log
Remove-Item Env:MZ_FORCE_RDNA3, Env:MZ_FORCE_RDNA2, Env:DLSSNR_MODEL, Env:E2E_PAN, Env:NR_STEP_TIMES -ErrorAction SilentlyContinue
foreach ($k in $Clean.Keys) { Set-Item "Env:$k" $Clean[$k] }

function Run($bin) {
    Remove-Item mochizuki_nr.log -ErrorAction SilentlyContinue
    & .\mz_e2e.exe .\MochizukiNrRuntime.dll . test.png $bin $QualityFrames 1 > "$bin.txt" 2>&1
    if ($LASTEXITCODE) { throw "run failed: $bin (see $bin.txt)" }
    Copy-Item mochizuki_nr.log "$bin.log"
    (Get-FileHash $bin -Algorithm SHA256).Hash.Substring(0, 16)
}
foreach ($c in $Quality) {
    $env:E2E_W = "$($c.w)"; $env:E2E_H = "$($c.h)"; $env:E2E_FLAGS = $(if ($c.model -eq 'large') { '8192' } else { '0' })
    $p = "refrun\$($c.name)"
    Remove-Item Env:MZ_FORCE_RDNA2 -ErrorAction SilentlyContinue
    $f8 = Run "$p.fp8.bin"
    if (-not (Select-String -LiteralPath "$p.fp8.bin.log" -Pattern '\] the FP8 network' -Quiet)) { throw "not the FP8 network: $p" }
    & .\compare.exe --pack "$p.fp8.bin" $c.w $c.h (Join-Path $out "img_$($c.name)")
    if ($LASTEXITCODE) { throw "pack failed: $($c.name)" }
    $env:MZ_FORCE_RDNA2 = '1'
    $r2 = Run "$p.rdna2.bin"
    $score = (& .\compare.exe "$p.rdna2.bin" $c.w $c.h (Join-Path $out "img_$($c.name)") | Select-String '^COMPARE ').Line
    $full = (& .\compare.exe --bins "$p.fp8.bin" "$p.rdna2.bin" $c.w $c.h | Select-Object -First 1)
    $line = "$($c.name)  FP8 sha $f8; RDNA2 forced sha $r2, against the reference: $score; whole frame: $full"
    Add-Content -LiteralPath $log -Value $line
    Write-Host $line
}
Remove-Item Env:E2E_W, Env:E2E_H, Env:E2E_FLAGS, Env:MZ_FORCE_RDNA2 -ErrorAction SilentlyContinue
foreach ($k in $Clean.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
