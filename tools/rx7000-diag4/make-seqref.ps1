# make-seqref.ps1 <work> <out>: the sequence check's reference on this machine (RX 9070 XT): the FP8 network (ACO, one
# dispatch per layer: NR_NO_PERSIST=1) in the kit's clean frame (seed 0, no history, recording 3), every value each
# dispatch makes, every 32nd 256-byte block, at the three replay sizes. <work> holds mz_e2e.exe, test.png,
# k4\MochizukiNrRuntime.dll and dlssnr-amd\ (shaders, aco\, dlssnr-small.bin).
param([string]$work, [string]$out)
$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $work
foreach ($k in 'MZ_FORCE_RDNA3', 'MZ_RDNA3_CHECK', 'MZ_SEQ_REFS', 'MZ_SEQ_VARIANTS') { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
$env:E2E_FLAGS = '0'; $env:E2E_CREATE_FLAGS = '2'; $env:NR_NO_PERSIST = '1'; $env:NR_DIAG_SEED0 = '1'; $env:NR_DIAG_TGATE0 = '1'
$env:MZ_SEQ_REF = '1'; $env:MZ_SEQ_RUNS = '1'; $env:MZ_SEQ_SAMPLE = '32'; $env:MZ_SEQ_FRAME = '3'
foreach ($sz in @(@(1152, 648), @(1280, 720), @(1920, 1080))) {
    $s = "$($sz[0])x$($sz[1])"
    $env:E2E_W = "$($sz[0])"; $env:E2E_H = "$($sz[1])"; $env:MZ_SEQ = "$out\$s"
    & .\mz_e2e.exe .\k4\MochizukiNrRuntime.dll . test.png "seqref_$s.bin" 6 1 > "seqref_$s.txt" 2>&1
    if ($LASTEXITCODE -or !(Test-Path "$out\$s\seq.txt")) { throw "reference run at $s failed (seqref_$s.txt)" }
    if (-not (Select-String -Path mochizuki_nr.log -Pattern 'the FP8 network' -Quiet)) { throw "not the FP8 network at $s" }
    "$s $((Get-ChildItem "$out\$s" -Filter ref_k*.bin).Count) values"
}
