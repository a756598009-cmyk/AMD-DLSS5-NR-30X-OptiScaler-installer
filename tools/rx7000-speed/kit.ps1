# OpenNR RX 7000 speed kit (RDNA3 kit 7 of the optimisation line): run by run-test.bat. Everything goes into ..\result,
# zipped beside run-test.bat.
# 1. System and Vulkan (vkprobe). 2. The model (download, or the copy in files\dlssnr-amd).
# 3. The full network on the test picture, 40 frames, NR_STEP_TIMES (each kernel's GPU time):
#    nat  = driver compiler, native vitattn everywhere (what 0.6.9 runs)
#    coop = driver compiler, cooperative-matrix vitattn everywhere (0.5.8's body; right at 1080p/1440p in kits 5-7,
#           wrong at 648p/720p); 6 sizes x 3 runs, so the runs show where it is right and steady
#    acofix / acocoop = ACO gfx11 machine code (kit 7's bundles), 4 sizes x 2 runs, kernel times
#    hyb  = a bundle put together here from the kernel times: ACO only for the kernels where it was faster, the
#           driver for the rest (their records left out, so the runtime compiles shaders-rdna3's template)
# Sizes with an RX 9070 XT reference are scored against it; the others against this card's own nat run.
# Our own testing only: KIT_FORCE_RDNA3=1 runs the RDNA3 network on any card, KIT_QUICK=1 two sizes and two runs,
# KIT_QUIET=1 opens no Explorer at the end.
$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'
[Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::InvariantCulture
$files = $PSScriptRoot
$kit = Split-Path $files -Parent
$result = Join-Path $kit 'result'
$work = Join-Path $files 'work'
$stamp = Get-Date -Format 'yyyy-MM-dd-HHmm'
Set-Location -LiteralPath $files
foreach ($d in $result, $work, (Join-Path $work 'ref')) {
    if (Test-Path -LiteralPath $d) { Remove-Item -LiteralPath $d -Recurse -Force }
    New-Item -ItemType Directory -Path $d | Out-Null
}
Get-ChildItem -LiteralPath $files -Recurse -Include *.exe, *.dll | Unblock-File -ErrorAction SilentlyContinue
Start-Transcript -LiteralPath (Join-Path $result 'console.txt') | Out-Null
$summary = New-Object System.Collections.Generic.List[string]
$short = New-Object System.Collections.Generic.List[string]
$t0 = Get-Date
function Line($s) { $summary.Add($s); Write-Host "  $s" }
$quick = $env:KIT_QUICK -eq '1'
$forced = $env:KIT_FORCE_RDNA3 -eq '1'
$frames = 40
$Pics = @(
    @{ name = '1152x648'; w = 1152; h = 648; ref = 'img_small_1152x648' },
    @{ name = '1280x720'; w = 1280; h = 720; ref = 'img_small_1280x720' },
    @{ name = '1706x960'; w = 1706; h = 960; ref = $null },
    @{ name = '1920x1080'; w = 1920; h = 1080; ref = 'img_small_1920x1080' },
    @{ name = '2560x1440'; w = 2560; h = 1440; ref = 'img_small_2560x1440' },
    @{ name = '3840x2160'; w = 3840; h = 2160; ref = $null })
$RefPics = @($Pics | Where-Object { $_.ref })
if ($quick) { $Pics = @($Pics[1], $Pics[3]); $RefPics = $Pics }

function Run($exe, $arguments, $out, $minutes) {
    try {
        $p = Start-Process -FilePath (Join-Path $files $exe) -ArgumentList $arguments -WorkingDirectory $files -NoNewWindow `
            -PassThru -RedirectStandardOutput $out -RedirectStandardError "$out.err" -ErrorAction Stop
    } catch {
        Set-Content -LiteralPath $out -Value "$exe did not start: $($_.Exception.Message)"
        return 'NOSTART'
    }
    $null = $p.Handle
    if (-not $p.WaitForExit($minutes * 60000)) {
        try { $p.Kill() } catch {}
        return 'TIMEOUT'
    }
    $p.WaitForExit()
    $err = "$out.err"
    if ((Test-Path -LiteralPath $err) -and (Get-Item -LiteralPath $err).Length -gt 0) {
        Add-Content -LiteralPath $out -Value "`n--- stderr ---"
        Get-Content -LiteralPath $err | Add-Content -LiteralPath $out
    }
    Remove-Item -LiteralPath $err -ErrorAction SilentlyContinue
    return $p.ExitCode
}
function Sha256($path) {
    $s = [IO.File]::OpenRead($path)
    try { -join ([Security.Cryptography.SHA256]::Create().ComputeHash($s) | ForEach-Object { $_.ToString('x2') }) }
    finally { $s.Dispose() }
}
function Hex($code) { if ($code -is [int]) { '0x{0:X8}' -f $code } else { "$code" } }
function First($path, $pattern) {
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    Get-Content -LiteralPath $path | Where-Object { $_ -match $pattern } | Select-Object -First 1
}
function SetEnv($vars) { foreach ($k in $vars.Keys) { Set-Item -Path "Env:$k" -Value $vars[$k] } }
function ClearEnv($vars) { foreach ($k in $vars.Keys) { Remove-Item -Path "Env:$k" -ErrorAction SilentlyContinue } }
function UseBundle($src) {
    $b = Join-Path $files 'dlssnr-amd\aco-gfx11'
    if (Test-Path -LiteralPath $b) { Remove-Item -LiteralPath $b -Recurse -Force }
    Remove-Item -LiteralPath (Join-Path $files 'dlssnr-amd\aco-cache') -Recurse -Force -ErrorAction SilentlyContinue
    if ($src) { Copy-Item -LiteralPath $src -Destination $b -Recurse }
}
UseBundle $null

Write-Host ''
Write-Host 'OpenNR RX 7000 speed kit. This takes about 30 minutes. Please do not close this window.'
Write-Host ''

Write-Host '[1/5] System and Vulkan...'
$sys = Join-Path $result 'system.txt'
$code = Run 'vkprobe.exe' 'probe' $sys 10
$sysText = if (Test-Path -LiteralPath $sys) { Get-Content -LiteralPath $sys } else { @() }
$sysText | Where-Object { $_ -match '^(windows:|amd software:)' } | ForEach-Object { Line "INFO $_" }
$amd = $sysText | Where-Object { $_ -match '^vulkan device \d+: .*vendor 1002' } | Select-Object -First 1
if ($amd) { Line "INFO $amd" }
if ($code -ne 0) { Line "FAIL vkprobe exit $(Hex $code)" }

Write-Host '[2/5] The model (about 110 MB, unless it is already in files\dlssnr-amd)...'
$m = @{ name = 'dlssnr-small.bin'; size = 113917248; sha = '7e966e2639dbec6596aa0bac3b8761137d2c40ca8cb4aca30df7ca10043c6e33';
        url = 'https://huggingface.co/datasets/zmodelerlover/amd-nr/resolve/main/mochizuki/model-small-1/dlssnr-small.bin' }
try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12 } catch {}
$dst = Join-Path $files "dlssnr-amd\$($m.name)"
$model = $false
for ($try = 0; $try -lt 3 -and -not $model; ++$try) {
    if (-not (Test-Path -LiteralPath $dst) -or (Get-Item -LiteralPath $dst).Length -ne $m.size) {
        if ($try -eq 0 -and (Test-Path -LiteralPath $dst)) { Remove-Item -LiteralPath $dst -Force }
        $part = "$dst.part"
        Remove-Item -LiteralPath $part -ErrorAction SilentlyContinue
        try {
            if ($try -lt 2) { Invoke-WebRequest -Uri $m.url -OutFile $part -UseBasicParsing -TimeoutSec 600 }
            else { Start-BitsTransfer -Source $m.url -Destination $part -ErrorAction Stop }
            Move-Item -LiteralPath $part -Destination $dst -Force
        } catch { Write-Host "    download of $($m.name) failed: $($_.Exception.Message)" }
    }
    if (Test-Path -LiteralPath $dst) {
        if ((Sha256 $dst) -eq $m.sha) { $model = $true } else { Remove-Item -LiteralPath $dst -Force }
    }
}
if ($model) { Line "PASS model $($m.name): SHA-256 verified" } else { Line "FAIL model $($m.name): could not be downloaded and verified" }

$baseEnv = @{ NR_STEP_TIMES = '20' }
if ($forced) { $baseEnv.MZ_FORCE_RDNA3 = '1'; $baseEnv.MZ_RDNA3_CHECK = '0'; Line 'INFO KIT_FORCE_RDNA3=1: the RDNA3 network is forced, its checks skipped' }
foreach ($k in 'MZ_FORCE_RDNA3', 'MZ_RDNA3_CHECK', 'DLSSNR_MODEL', 'E2E_FLAGS', 'E2E_PAN', 'E2E_CREATE_FLAGS', 'MZ_STAGE_DUMP',
               'MZ_KCAP', 'MZ_SEQ', 'MZ_ONE_QUEUE', 'NR_RDNA3_SYNC', 'NR_RDNA3_ATTN', 'NR_RDNA3_VITATTN', 'NR_RDNA3_VITATTN_COOP_MIN',
               'NR_TCHAIN', 'NR_STEP_TIMES', 'NR_RDNA3_PERSIST', 'NR_ACO_DIR', 'NR_ACO_NATIVE_KERNELS', 'NR_ACO_CACHE', 'NR_ACO_SHELL_DIR') {
    Remove-Item "Env:$k" -ErrorAction SilentlyContinue
}
foreach ($f in 'pipeline-rdna3.cache', 'pipeline.cache', 'prewarm', 'build.pending', 'build.failed', 'rdna3-check.txt') {
    Remove-Item -LiteralPath (Join-Path $files "dlssnr-amd\$f") -Recurse -Force -ErrorAction SilentlyContinue
}

$script:first = $true
$script:notRdna3 = $false
function E2E($tag, $w, $h, $vars, $dir) {
    $r = @{ ok = $false; why = ''; net = '?'; ms = ''; build = ''; bin = "work\$tag.bin"; imp = 0; fb = 0; steps = @{}; vit = '' }
    if (-not $model) { $r.why = 'no model'; return $r }
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    $log = Join-Path $files 'mochizuki_nr.log'
    Remove-Item -LiteralPath $log, (Join-Path $files $r.bin) -ErrorAction SilentlyContinue
    $all = @{ E2E_W = "$w"; E2E_H = "$h"; E2E_FLAGS = '0' }
    foreach ($k in $baseEnv.Keys) { $all[$k] = $baseEnv[$k] }
    foreach ($k in $vars.Keys) { $all[$k] = $vars[$k] }
    SetEnv $all
    $out = Join-Path $dir "harness_$tag.txt"
    $code = Run 'mz_e2e.exe' ".\k\MochizukiNrRuntime.dll . test.png $($r.bin) $frames 1" $out $(if ($script:first) { 40 } else { 20 })
    ClearEnv $all
    $script:first = $false
    if (Test-Path -LiteralPath $log) { Copy-Item -LiteralPath $log -Destination (Join-Path $dir "log_$tag.txt") }
    $lt = if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log } else { @() }
    $net = $lt | Where-Object { $_ -match '\[mochizuki\] the (RDNA3|FP8) network' } | Select-Object -First 1
    $r.net = if ($net -match 'RDNA3') { 'RDNA3' } elseif ($net -match 'FP8') { 'FP8' } else { '?' }
    $r.imp = @($lt | Where-Object { $_ -match '\[nr\] ACO gfx11 \S+: imported' }).Count
    $r.fb = @($lt | Where-Object { $_ -match '\[nr\] ACO gfx11 \S+: not imported' }).Count
    $v = $lt | Where-Object { $_ -match '\[nr\] RDNA3 vitattn: (\S+) body' } | Select-Object -First 1
    if ($v -match 'vitattn: (\S+) body') { $r.vit = $Matches[1] }
    foreach ($l in $lt) {
        if ($l -match '\[nr\]\s+(g_\S+?)(\.spv)?\s+(\d+) x\s+([\d.]+) ms a frame') {
            $k = $Matches[1] -replace '^g_', ''
            if ($k -eq 'vitattncm') { $k = 'vitattn' }
            $r.steps[$k] = [double]$Matches[4]
        }
    }
    $status = First $out '^status: '
    if ($status -match 'network ([\d.]+) ms') { $r.ms = $Matches[1] }
    $ready = First $out '^ready after '
    if ($ready -match 'ready after ([\d.]+) s') { $r.build = "ready in $($Matches[1]) s" }
    if ($code -ne 0) {
        $why = First $out 'FAIL'
        if ($code -eq 'TIMEOUT') { $why = 'timed out' } elseif (-not $why) { $why = "exit $(Hex $code)" }
        $off = $lt | Where-Object { $_ -match 'RDNA3 network off|crash|build failed' } | Select-Object -Last 1
        $r.why = "$why$(if ($off) { " | $off" })"
        return $r
    }
    $r.ok = $true
    return $r
}
function Score($bin, $c, $out) {
    if ($c.ref) {
        $null = Run 'compare.exe' "$bin $($c.w) $($c.h) reference\$($c.ref)" $out 5
        $l = First $out '^COMPARE '
        if ($l -match 'psnr (\S+) dB \(worst area (\S+)\), nan/inf (\d+)') {
            return @{ psnr = $(if ($Matches[1] -eq 'inf') { 999.0 } else { [double]$Matches[1] }); text = "$($Matches[1]) dB"; bad = [int]$Matches[3]; min = 40 }
        }
        return @{ psnr = -1.0; text = 'no comparison'; bad = 0; min = 40 }
    }
    $own = "work\ref\$($c.name).bin"
    if (-not (Test-Path -LiteralPath (Join-Path $files $own))) { return @{ psnr = -1.0; text = 'no nat run to compare'; bad = 0; min = 45 } }
    $d = RunsDiff $own $bin $c.w $c.h $out
    if ($d -eq 'identical') { return @{ psnr = 999.0; text = 'same as nat'; bad = 0; min = 45 } }
    if ($d -match '\((\S+) dB') { return @{ psnr = [double]$Matches[1]; text = "$($Matches[1]) dB vs nat"; bad = 0; min = 45 } }
    return @{ psnr = -1.0; text = $d; bad = 0; min = 45 }
}
function RunsDiff($a, $b, $w, $h, $out) {
    $null = Run 'compare.exe' "--bins $a $b $w $h" $out 5
    $l = First $out '^RUNS '
    if ($l -match '^RUNS identical') { return 'identical' }
    if ($l -match 'psnr (\S+) dB.*differing pixels in (.*)$') { return "differ ($($Matches[1]) dB, pixels $($Matches[2]))" }
    return 'not compared'
}

$good = @{}; $vary = @{}; $msAll = @{}; $steps = @{}; $impAll = @{}
function RunSet($set, $pics, $runs) {
    $tag = $set.tag
    $good[$tag] = @(); $vary[$tag] = @(); $msAll[$tag] = @{}; $steps[$tag] = @{}; $impAll[$tag] = ''
    foreach ($c in $pics) {
        $name = $c.name
        Write-Host "    $name ..."
        $dir = Join-Path $result "runs\$tag\$name"
        $scores = @(); $fails = @(); $info = $null; $ms = @(); $st = @{}
        for ($k = 1; $k -le $runs; ++$k) {
            $r = E2E "$tag.$name.r$k" $c.w $c.h $set.vars $dir
            if ($r.net -eq 'FP8' -and -not $forced) { $script:notRdna3 = $true; break }
            if (-not $r.ok) { $fails += "run $k failed: $($r.why)"; continue }
            if (-not $info) { $info = $r }
            if ($r.ms) { $ms += [double]$r.ms }
            foreach ($kk in $r.steps.Keys) { if (-not $st.ContainsKey($kk)) { $st[$kk] = @() }; $st[$kk] += $r.steps[$kk] }
            if ($tag -eq 'nat' -and $k -eq 1 -and -not $c.ref) { Copy-Item -LiteralPath (Join-Path $files $r.bin) -Destination (Join-Path $files "work\ref\$name.bin") }
            $scores += Score $r.bin $c (Join-Path $dir "compare_r$k.txt")
        }
        if ($script:notRdna3) { return }
        $diffs = @()
        for ($k = 2; $k -le $runs; ++$k) {
            $a = "work\$tag.$name.r1.bin"; $b = "work\$tag.$name.r$k.bin"
            if ((Test-Path -LiteralPath (Join-Path $files $a)) -and (Test-Path -LiteralPath (Join-Path $files $b))) {
                $diffs += "r1-r${k}: " + (RunsDiff $a $b $c.w $c.h (Join-Path $dir "runs_r1_r$k.txt"))
            }
        }
        Remove-Item -Path (Join-Path $files "work\$tag.$name.r*.bin") -ErrorAction SilentlyContinue
        $varies = @($diffs | Where-Object { $_ -notmatch 'identical$' -and $_ -notmatch '\((5\d|[6-9]\d|\d{3,})\.' }).Count -gt 0
        $okAll = $scores.Count -eq $runs -and @($scores | Where-Object { $_.psnr -lt $_.min -or $_.bad -gt 0 }).Count -eq 0
        $verdict = if ($fails.Count) { 'FAIL' } elseif ($okAll -and -not $varies) { 'GOOD' } else { 'BAD ' }
        if ($verdict -eq 'GOOD') { $good[$tag] += $name }
        if ($varies) { $vary[$tag] += $name }
        if ($ms.Count) { $msAll[$tag][$name] = ($ms | Measure-Object -Average).Average }
        foreach ($kk in $st.Keys) { $steps[$tag]["$name|$kk"] = ($st[$kk] | Measure-Object -Average).Average }
        $acoInfo = ''
        if ($info -and $set.bundle) { $acoInfo = "; ACO imported $($info.imp) pipelines, driver $($info.fb)"; $impAll[$tag] = "imported $($info.imp), driver $($info.fb)" }
        $extra = if ($info) { "$($info.net), vitattn $($info.vit), network ms $(($ms | ForEach-Object { $_.ToString('0.00') }) -join ' / '), first $($info.build)$acoInfo" } else { '' }
        Line ("{0} {1,-7} {2,-9}: {3}; runs {4}; {5}" -f $verdict, $tag, $name, (($scores | ForEach-Object { $_.text }) -join ' / '),
              $(if ($diffs.Count) { $diffs -join ', ' } else { '-' }), $extra)
        foreach ($f in $fails) { Line "     $f" }
    }
}

$runs = if ($quick) { 2 } else { 3 }
$sets = @(
    @{ tag = 'nat'; what = 'driver compiler, native ViT attention (as 0.6.9)'; vars = @{ NR_RDNA3_VITATTN = 'native' }; pics = $Pics; runs = $runs },
    @{ tag = 'coop'; what = 'driver compiler, matrix ViT attention'; vars = @{ NR_RDNA3_VITATTN = 'coop' }; pics = $Pics; runs = $runs },
    @{ tag = 'acofix'; what = 'ACO machine code, the 0.5.9 kernels'; vars = @{ E2E_CREATE_FLAGS = '2' }; bundle = 'bundles\fix'; pics = $RefPics; runs = 2 },
    @{ tag = 'acocoop'; what = 'ACO machine code, 0.5.8''s matrix attention'; vars = @{ E2E_CREATE_FLAGS = '2' }; bundle = 'bundles\coop'; pics = $RefPics; runs = 2 })
$step = 0
if ($model) {
    foreach ($set in $sets) {
        ++$step
        UseBundle $(if ($set.bundle) { Join-Path $files $set.bundle } else { $null })
        Write-Host "[3/5] Pictures ($step/5), $($set.what): $($set.pics.Count) sizes x $($set.runs) runs..."
        RunSet $set $set.pics $set.runs
        if ($script:notRdna3) { Line 'SKIPPED the pictures: this card picked the FP8 network (not an RX 7000)'; break }
    }
}

# 4. The kernel table and the hybrid bundle.
$picks = @{}; $table = New-Object System.Collections.Generic.List[string]
if ($model -and -not $script:notRdna3) {
    Write-Host '[4/5] Kernel times and the hybrid bundle...'
    $kernels = @($steps['nat'].Keys | ForEach-Object { ($_ -split '\|')[1] } | Sort-Object -Unique)
    $csv = New-Object System.Collections.Generic.List[string]
    $csv.Add('size,kernel,nat,coop,acofix,acocoop')
    function Sum($tag, $k) {
        $s = 0.0
        foreach ($c in $RefPics) { $v = $steps[$tag]["$($c.name)|$k"]; if ($null -eq $v) { return $null }; $s += $v }
        return $s
    }
    foreach ($c in $Pics) {
        foreach ($k in $kernels) {
            $row = @($c.name, $k)
            foreach ($t in 'nat', 'coop', 'acofix', 'acocoop') { $v = if ($steps.ContainsKey($t)) { $steps[$t]["$($c.name)|$k"] } else { $null }; $row += $(if ($null -ne $v) { $v.ToString('0.0000', [Globalization.CultureInfo]::InvariantCulture) } else { '' }) }
            $csv.Add($row -join ',')
        }
    }
    Set-Content -LiteralPath (Join-Path $result 'kernel-times.csv') -Value $csv
    $coopOk = @($RefPics | Where-Object { $good['acocoop'] -notcontains $_.name }).Count -eq 0
    $table.Add(('{0,-22} {1,9} {2,9} {3,9} {4,9}  pick (ms a frame, summed over {5})' -f 'kernel', 'nat', 'coop', 'acofix', 'acocoop', (($RefPics | ForEach-Object { $_.name }) -join ' + ')))
    foreach ($k in $kernels) {
        $d = Sum 'nat' $k; $cp = Sum 'coop' $k; $af = Sum 'acofix' $k; $ac = Sum 'acocoop' $k
        if ($k -eq 'vitattn' -and $null -ne $d) {
            # The driver as the runtime now runs it: the matrix body from 1080p up, the native one below.
            $d = 0.0
            foreach ($c in $RefPics) { $t = if ($c.h -ge 1080) { 'coop' } else { 'nat' }; $v = $steps[$t]["$($c.name)|$k"]; if ($null -ne $v) { $d += $v } }
        }
        $pick = 'driver'
        if ($null -ne $d -and $null -ne $af -and $af -lt 0.97 * $d -and $d - $af -gt 0.02) { $pick = 'acofix' }
        if (($k -eq 'attn' -or $k -eq 'vitattn') -and $coopOk -and $null -ne $ac -and $null -ne $d -and $ac -lt 0.97 * $d -and $d - $ac -gt 0.02 -and ($null -eq $af -or $ac -lt $af)) { $pick = 'acocoop' }
        $picks[$k] = $pick
        $f = { param($x) if ($null -ne $x) { $x.ToString('0.000') } else { '-' } }
        $table.Add(('{0,-22} {1,9} {2,9} {3,9} {4,9}  {5}' -f $k, (& $f $d), (& $f $cp), (& $f $af), (& $f $ac), $pick))
    }
    Set-Content -LiteralPath (Join-Path $result 'kernel-table.txt') -Value $table
    # The hybrid bundle: kit 7's fix bundle; a kernel the driver wins gets shaders-rdna3's SPIR-V (no record matches it,
    # so the runtime compiles that template), an acocoop kernel gets the coop bundle's SPIR-V and records.
    $hyb = Join-Path $work 'hyb'
    Copy-Item -LiteralPath (Join-Path $files 'bundles\fix') -Destination $hyb -Recurse
    Copy-Item -Path (Join-Path $files 'bundles\coop\records\*') -Destination (Join-Path $hyb 'records') -Force
    $nAco = 0
    foreach ($k in $picks.Keys) {
        $n = "g_$k.spv"
        if (-not (Test-Path -LiteralPath (Join-Path $hyb "shaders\$n"))) { continue }
        switch ($picks[$k]) {
            'driver' {
                Copy-Item -LiteralPath (Join-Path $files "dlssnr-amd\shaders-rdna3\$n") -Destination (Join-Path $hyb "shaders\$n") -Force
                # With g_vitattncm.spv beside it the runtime takes the matrix body from 1080p up, as without ACO.
                if ($k -eq 'vitattn') { Copy-Item -LiteralPath (Join-Path $files 'dlssnr-amd\shaders-rdna3\g_vitattncm.spv') -Destination (Join-Path $hyb 'shaders') -Force }
            }
            'acocoop' { Copy-Item -LiteralPath (Join-Path $files "bundles\coop\shaders\$n") -Destination (Join-Path $hyb "shaders\$n") -Force; ++$nAco }
            default { ++$nAco }
        }
    }
    Line "INFO hybrid bundle: $nAco kernels as ACO machine code ($((@($picks.Keys | Where-Object { $picks[$_] -ne 'driver' }) | Sort-Object) -join ', ')), the rest by the driver"
    UseBundle $hyb
    Write-Host "[5/5] Pictures (5/5), the hybrid bundle: $($RefPics.Count) sizes x 2 runs..."
    RunSet @{ tag = 'hyb'; vars = @{ E2E_CREATE_FLAGS = '2' }; bundle = 'hyb' } $RefPics 2
}
UseBundle $null

Write-Host 'Writing the result...'
if ($script:notRdna3) { $short.Add('This card did not take the RX 7000 network, so the network steps were skipped. Please tell us which card this is.') }
else {
    foreach ($t in 'nat', 'coop', 'acofix', 'acocoop', 'hyb') {
        if (-not $good.ContainsKey($t)) { continue }
        $ms = ($msAll[$t].Keys | Sort-Object { [int]($_ -split 'x')[1] } | ForEach-Object { "$_ $($msAll[$t][$_].ToString('0.00'))" }) -join ', '
        $short.Add(("{0,-8}: right and steady at: {1}; changes run to run: {2}; network ms: {3}{4}" -f $t,
                    $(if ($good[$t].Count) { $good[$t] -join ', ' } else { 'none' }), $(if ($vary[$t].Count) { $vary[$t] -join ', ' } else { 'none' }),
                    $ms, $(if ($impAll[$t]) { "; $($impAll[$t])" } else { '' })))
    }
}
$head = @("OpenNR RX 7000 speed kit, $((Get-Date).ToString('yyyy-MM-dd HH:mm')), $([int]((Get-Date) - $t0).TotalMinutes) min", '',
          'IN SHORT') + ($short | ForEach-Object { "  $_" }) + @('', 'KERNELS') + ($table | ForEach-Object { "  $_" }) + @('', 'DETAILS')
$tail = @('', 'How to read this: nat = driver compiler with the native ViT attention (0.6.9), coop = driver compiler with the',
          'matrix ViT attention, acofix / acocoop = ACO machine code (kit 7''s bundles), hyb = ACO only for the kernels it',
          'won here. GOOD = every run 40 dB or more from the RX 9070 XT (sizes without a reference: 45 dB or more from this',
          'card''s nat run) and runs identical or within 50 dB of each other. network ms = the network''s GPU time a frame.',
          'kernel-times.csv / kernel-table.txt: each kernel''s GPU ms a frame (NR_STEP_TIMES); in the table vitattn''s nat column is',
          'the driver as the runtime now picks it (native below 1080p, matrix from 1080p up). Raw logs in runs\<set>\<size>\.')
Set-Content -LiteralPath (Join-Path $result 'SUMMARY.txt') -Value ($head + $summary + $tail)
Stop-Transcript | Out-Null
Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
$zip = Join-Path $kit "OpenNR-RX7000-speed-result-$stamp.zip"
Remove-Item -LiteralPath $zip -ErrorAction SilentlyContinue
try {
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $za = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($f in Get-ChildItem -LiteralPath $result -Recurse -File) {
            $rel = $f.FullName.Substring($result.Length + 1).Replace('\', '/')
            $null = [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($za, $f.FullName, $rel, [IO.Compression.CompressionLevel]::Optimal)
        }
    } finally { $za.Dispose() }
} catch { Write-Host "zip: $($_.Exception.Message)"; Remove-Item -LiteralPath $zip -ErrorAction SilentlyContinue }
Write-Host ''
if (Test-Path -LiteralPath $zip) {
    Write-Host 'Done. Please send this file:' -ForegroundColor Green
    Write-Host "  $zip" -ForegroundColor Green
    if (-not $env:KIT_QUIET) { Start-Process explorer.exe -ArgumentList "/select,`"$zip`"" }
} else {
    Write-Host 'Done, but the zip could not be made. Please zip and send the folder:' -ForegroundColor Yellow
    Write-Host "  $result" -ForegroundColor Yellow
}
