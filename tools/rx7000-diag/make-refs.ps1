# make-refs.ps1 <work> <reference out>: the RX 9070 XT references of the RX 7000 diagnostic kit, made in <work>
# (rel\ the released 0.5.8 DLL, diag\ the same with MZ_STAGE_DUMP, dlssnr-amd\ the released shaders and both models,
# the kit's tools and test.png). Per size and model:
#   fp8      released DLL, the Windows FP8 network (no ACO)
#   fp8d     diag DLL, the same, dump off: must be byte-identical to fp8
#   ref      diag DLL, ACO (the Linux kernel set, the one the RDNA3 network runs), stage dump at recording 36: the
#            picture and per-stage reference
#   aco      released DLL, ACO, no dump: must be byte-identical to ref (the dump does not change the frame)
# Writes <reference out>\img_<model>_<w>x<h>_{q,b,r}.jxr, stages_<model>_<w>x<h>\stages.{txt,bin}, and refs.txt.
param([string]$work, [string]$out)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'sizes.ps1')
Set-Location -LiteralPath $work
New-Item -ItemType Directory -Force -Path $out, 'refrun' | Out-Null
$log = Join-Path $out 'refs.txt'
"RX 9070 XT references, $(Get-Date -Format 'yyyy-MM-dd HH:mm'); 40 frames, 1 pass, RenoDX stage, no pan; stage dump at recording 36" |
    Set-Content -LiteralPath $log
Remove-Item Env:MZ_FORCE_RDNA3, Env:DLSSNR_MODEL, Env:E2E_PAN, Env:MZ_STAGE_DUMP, Env:E2E_CREATE_FLAGS -ErrorAction SilentlyContinue

function Run($dll, $bin, $flags, $dump) {
    $env:E2E_CREATE_FLAGS = $flags
    if ($dump) { $env:MZ_STAGE_DUMP = $dump; $env:MZ_STAGE_FRAME = '36' } else { Remove-Item Env:MZ_STAGE_DUMP -ErrorAction SilentlyContinue }
    & .\mz_e2e.exe ".\$dll\MochizukiNrRuntime.dll" . test.png $bin 40 1 > "$bin.txt" 2>&1
    if ($LASTEXITCODE) { throw "run failed: $dll $bin (see $bin.txt)" }
    Remove-Item Env:MZ_STAGE_DUMP -ErrorAction SilentlyContinue
}
function Same($a, $b) { (Get-FileHash $a).Hash -eq (Get-FileHash $b).Hash }

foreach ($m in $Models) {
    foreach ($s in $Sizes) {
        $w, $h = $s
        $name = "$($m.name)_${w}x${h}"
        $env:E2E_W = "$w"; $env:E2E_H = "$h"; $env:E2E_FLAGS = $m.flags
        $p = "refrun\$name"
        Remove-Item -Recurse -Force "$p.dump" -ErrorAction SilentlyContinue
        Run 'rel' "$p.fp8.bin" 0 $null
        Run 'diag' "$p.fp8d.bin" 0 $null
        Run 'diag' "$p.ref.bin" 2 "$p.dump"
        Run 'rel' "$p.aco.bin" 2 $null
        $offSame = Same "$p.fp8.bin" "$p.fp8d.bin"
        $dumpSame = Same "$p.ref.bin" "$p.aco.bin"
        $acoVsFp8 = (& .\compare.exe --bins "$p.fp8.bin" "$p.ref.bin" $w $h | Select-Object -First 1)
        $net = (Select-String -LiteralPath "$p.ref.bin.txt" -Pattern '^status: ' | Select-Object -First 1).Line
        & .\compare.exe --pack "$p.ref.bin" $w $h (Join-Path $out "img_$name")
        if ($LASTEXITCODE) { throw "pack failed: $name" }
        $sd = Join-Path $out "stages_$name"
        New-Item -ItemType Directory -Force -Path $sd | Out-Null
        Copy-Item "$p.dump\stages.txt", "$p.dump\stages.bin" $sd
        $line = "$name  dump-off identical to released: $offSame; dump does not change the frame: $dumpSame; Windows FP8 against ACO: $acoVsFp8; $net"
        Add-Content -LiteralPath $log -Value $line
        Write-Host $line
    }
}
Remove-Item Env:E2E_W, Env:E2E_H, Env:E2E_FLAGS, Env:E2E_CREATE_FLAGS, Env:MZ_STAGE_FRAME -ErrorAction SilentlyContinue
