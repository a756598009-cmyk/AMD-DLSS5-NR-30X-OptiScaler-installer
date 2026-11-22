# OpenNR RX 7000 diagnostic kit 2: run by run-test.bat. Everything goes into ..\result, zipped beside run-test.bat.
# The fixed DLL (fix\: the RDNA3 network without persistent runs) against the released 0.5.8 one (rel\, and rdiag\
# = 0.5.8 with the stage dump) at kit 1's worst sizes. Our own testing only: KIT_FORCE_RDNA3=1 runs the RDNA3 network
# on any card (MZ_FORCE_RDNA3=1, MZ_RDNA3_CHECK=0), KIT_QUICK=1 takes two sizes and two runs, KIT_QUIET=1 opens no
# Explorer at the end.
$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'
$files = $PSScriptRoot
$kit = Split-Path $files -Parent
$result = Join-Path $kit 'result'
$work = Join-Path $files 'work'
$stamp = Get-Date -Format 'yyyy-MM-dd-HHmm'
Set-Location -LiteralPath $files
. (Join-Path $files 'sizes2.ps1')
foreach ($d in $result, $work) {
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
if ($quick) { $Runs = @($Runs[0], $Runs[4]) }
$runsPerSize = if ($quick) { 2 } else { 3 }
$frames = 40

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

Write-Host ''
Write-Host 'OpenNR RX 7000 diagnostic kit 2. This takes about 10 to 15 minutes. Please do not close this window.'
Write-Host ''

# 1. System and the matrix layout probes.
Write-Host '[1/6] System, Vulkan and matrix layout probe...'
$sys = Join-Path $result 'system.txt'
$code = Run 'vkprobe.exe' 'probe' $sys 10
$sysText = if (Test-Path -LiteralPath $sys) { Get-Content -LiteralPath $sys } else { @() }
$sysText | Where-Object { $_ -match '^(windows:|amd software:)' } | ForEach-Object { Line "INFO $_" }
$amd = $sysText | Where-Object { $_ -match '^vulkan device \d+: .*vendor 1002' } | Select-Object -First 1
if ($amd) { Line "INFO $amd" }
$pick = $sysText | Where-Object { $_ -match 'runtime would pick' } | Select-Object -First 1
if ($pick) { Line ("INFO " + $pick.Trim()) }
if ($code -ne 0) { Line "FAIL vkprobe exit $(Hex $code)" }
foreach ($c in ($sysText | Where-Object { $_ -match '^CHECK ' })) {
    $parts = $c -split ' ', 4
    Line ("{0} layout {1}: {2}" -f $parts[2], $parts[1], $parts[3])
}

# 2. The models, from the same place the OpenNR installer takes them.
Write-Host '[2/6] Downloading the models (about 250 MB, once)...'
$downloads = @(
    @{ name = 'dlssnr.bin'; size = 147756560; sha = '2b41c888cf4155b8958c665ba64018ab0bd25c85fc71a2b6db86d0d04d1f7fbd';
       url = 'https://huggingface.co/datasets/zmodelerlover/amd-nr/resolve/main/mochizuki/model-1/dlssnr.bin' },
    @{ name = 'dlssnr-small.bin'; size = 113917248; sha = '7e966e2639dbec6596aa0bac3b8761137d2c40ca8cb4aca30df7ca10043c6e33';
       url = 'https://huggingface.co/datasets/zmodelerlover/amd-nr/resolve/main/mochizuki/model-small-1/dlssnr-small.bin' })
try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12 } catch {}
$have = @{}
foreach ($m in $downloads) {
    $dst = Join-Path $files "dlssnr-amd\$($m.name)"
    $verified = $false
    for ($try = 0; $try -lt 3 -and -not $verified; ++$try) {
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
            if ((Sha256 $dst) -eq $m.sha) { $verified = $true } else { Remove-Item -LiteralPath $dst -Force }
        }
    }
    $have[$m.name] = $verified
    if ($verified) { Line "PASS model $($m.name): SHA-256 verified" } else { Line "FAIL model $($m.name): could not be downloaded and verified" }
}
$modelFile = @{ small = 'dlssnr-small.bin'; large = 'dlssnr.bin' }

$baseEnv = @{}
if ($forced) { $baseEnv = @{ MZ_FORCE_RDNA3 = '1'; MZ_RDNA3_CHECK = '0' }; Line 'INFO KIT_FORCE_RDNA3=1: the RDNA3 network is forced, its checks skipped' }
foreach ($k in 'MZ_FORCE_RDNA3', 'MZ_RDNA3_CHECK', 'DLSSNR_MODEL', 'E2E_FLAGS', 'E2E_PAN', 'E2E_CREATE_FLAGS', 'MZ_STAGE_DUMP',
               'MZ_STAGE_FULL', 'MZ_STAGE_FRAME', 'NR_TCHAIN', 'NR_ARENA_REUSE', 'NR_NO_PERSIST', 'NR_RDNA3_PERSIST',
               'NR_INV_BARRIERS', 'NR_DIAG_SEED0', 'NR_DIAG_TGATE0') {
    Remove-Item "Env:$k" -ErrorAction SilentlyContinue
}
foreach ($f in 'pipeline-rdna3.cache', 'pipeline.cache', 'prewarm', 'build.pending', 'build.failed', 'rdna3-check.txt') {
    Remove-Item -LiteralPath (Join-Path $files "dlssnr-amd\$f") -Recurse -Force -ErrorAction SilentlyContinue
}
$script:first = $true
$script:notRdna3 = $false
$clean = @{ NR_DIAG_SEED0 = '1'; NR_DIAG_TGATE0 = '1' }

# One harness run of $dll (rel, rdiag or fix) on test.png; leaves work\<tag>.bin and returns what the run said.
function E2E($tag, $dll, $c, $n, $vars, $dir) {
    $r = @{ ok = $false; why = ''; net = '?'; ms = ''; build = ''; persist = ''; bin = "work\$tag.bin" }
    if (-not $have[$modelFile[$c.model]]) { $r.why = "no $($modelFile[$c.model])"; return $r }
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    $log = Join-Path $files 'mochizuki_nr.log'
    Remove-Item -LiteralPath $log, (Join-Path $files $r.bin) -ErrorAction SilentlyContinue
    $all = @{ E2E_W = "$($c.w)"; E2E_H = "$($c.h)"; E2E_FLAGS = $(if ($c.model -eq 'large') { '8192' } else { '0' }) }
    foreach ($k in $baseEnv.Keys) { $all[$k] = $baseEnv[$k] }
    foreach ($k in $vars.Keys) { $all[$k] = $vars[$k] }
    SetEnv $all
    $out = Join-Path $dir "harness_$tag.txt"
    $code = Run 'mz_e2e.exe' ".\$dll\MochizukiNrRuntime.dll . test.png $($r.bin) $n 1" $out $(if ($script:first) { 40 } else { 15 })
    ClearEnv $all
    $script:first = $false
    if (Test-Path -LiteralPath $log) { Copy-Item -LiteralPath $log -Destination (Join-Path $dir "log_$tag.txt") }
    $lt = if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log } else { @() }
    $net = $lt | Where-Object { $_ -match '\[mochizuki\] the (RDNA3|FP8) network' } | Select-Object -First 1
    $r.net = if ($net -match 'RDNA3') { 'RDNA3' } elseif ($net -match 'FP8') { 'FP8' } else { '?' }
    $status = First $out '^status: '
    if ($status -match 'network ([\d.]+) ms') { $r.ms = "network $($Matches[1]) ms" }
    $ready = First $out '^ready after '
    if ($ready -match 'ready after ([\d.]+) s') { $r.build = "ready in $($Matches[1]) s" }
    $r.persist = if (First $out 'merged runs') { 'persistent runs on' } else { 'persistent runs off' }
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
function Score($bin, $ref, $c, $out, $pictures) {
    $a = "$bin $($c.w) $($c.h) reference\$ref"
    if ($pictures) { $a += " ..\result\$pictures\diff_x16.png ..\result\$pictures\output.jpg" }
    $null = Run 'compare.exe' $a $out 5
    $l = First $out '^COMPARE '
    if ($l -match 'psnr (\S+) dB \(worst area (\S+)\), nan/inf (\d+)') {
        return @{ psnr = $(if ($Matches[1] -eq 'inf') { 999.0 } else { [double]$Matches[1] }); text = "$($Matches[1]) dB"; bad = [int]$Matches[3] }
    }
    return @{ psnr = -1.0; text = 'no comparison'; bad = 0 }
}
function RunsDiff($a, $b, $c, $out) {
    $null = Run 'compare.exe' "--bins $a $b $($c.w) $($c.h)" $out 5
    $l = First $out '^RUNS '
    if ($l -match '^RUNS identical') { return 'identical' }
    if ($l -match 'psnr (\S+) dB.*differing pixels in (.*)$') { return "differ ($($Matches[1]) dB, pixels $($Matches[2]))" }
    return 'not compared'
}

# 3. Pictures: the released DLL and the fixed one, 3 runs each, against the RX 9070 XT; then the fix with full
# memory barriers between dispatches (NR_INV_BARRIERS=0) at the two worst sizes.
$good = @{ rel = @(); fix = @() }; $bad = @{ rel = @(); fix = @() }; $vary = @{ rel = @(); fix = @() }
$sets = @(@{ dll = 'rel'; what = 'released 0.5.8'; vars = @{}; runs = $Runs; tag = 'rel' },
          @{ dll = 'fix'; what = 'fixed (no persistent runs)'; vars = @{}; runs = $Runs; tag = 'fix' },
          @{ dll = 'fix'; what = 'fixed + full barriers (NR_INV_BARRIERS=0)'; vars = @{ NR_INV_BARRIERS = '0' };
             runs = @($Runs | Where-Object { $_.name -eq 'small_1152x648' -or $_.name -eq 'small_1280x720' }); tag = 'fixfb' })
$step = 0
foreach ($set in $sets) {
    ++$step
    Write-Host "[3/6] Pictures ($step/3), $($set.what): $($set.runs.Count) sizes x $runsPerSize runs..."
    foreach ($c in $set.runs) {
        if ($script:notRdna3) { break }
        $name = $c.name
        Write-Host "    $name ..."
        $dir = Join-Path $result "runs\$($set.tag)\$name"
        $scores = @(); $fails = @(); $info = $null
        for ($k = 1; $k -le $runsPerSize; ++$k) {
            $r = E2E "$($set.tag).$name.r$k" $set.dll $c $frames $set.vars $dir
            if ($r.net -eq 'FP8' -and -not $forced) {
                Line "SKIPPED all runs: this card picked the FP8 network (not an RX 7000)"
                $script:notRdna3 = $true
                break
            }
            if (-not $r.ok) { $fails += "run $k failed: $($r.why)"; continue }
            if (-not $info) { $info = $r }
            $pic = if ($k -eq 1 -and $name -eq 'small_1152x648' -and $set.tag -ne 'fixfb') { "runs\$($set.tag)\$name" } else { $null }
            $scores += Score $r.bin "img_$name" $c (Join-Path $dir "compare_r$k.txt") $pic
        }
        if ($script:notRdna3) { break }
        $diffs = @()
        for ($k = 2; $k -le $runsPerSize; ++$k) {
            $a = "work\$($set.tag).$name.r1.bin"; $b = "work\$($set.tag).$name.r$k.bin"
            if ((Test-Path -LiteralPath (Join-Path $files $a)) -and (Test-Path -LiteralPath (Join-Path $files $b))) {
                $diffs += "r1-r${k}: " + (RunsDiff $a $b $c (Join-Path $dir "runs_r1_r$k.txt"))
            }
        }
        Remove-Item -Path (Join-Path $files "work\$($set.tag).$name.r*.bin") -ErrorAction SilentlyContinue
        $varies = @($diffs | Where-Object { $_ -notmatch 'identical$' }).Count -gt 0
        $okAll = $scores.Count -eq $runsPerSize -and @($scores | Where-Object { $_.psnr -lt 40 -or $_.bad -gt 0 }).Count -eq 0
        $verdict = if ($fails.Count) { 'FAIL' } elseif ($okAll) { 'GOOD' } else { 'BAD ' }
        if ($good.ContainsKey($set.tag)) {
            if ($okAll -and -not $fails.Count) { $good[$set.tag] += $name } else { $bad[$set.tag] += $name }
            if ($varies) { $vary[$set.tag] += $name }
        }
        $extra = if ($info) { "$($info.net) network, $($info.persist), $($info.ms), first $($info.build)" } else { '' }
        Line ("{0} {1,-5} {2}: PSNR vs RX 9070 XT {3}; runs {4}; {5}" -f $verdict, $set.tag, $name,
              (($scores | ForEach-Object { $_.text }) -join ' / '), $(if ($diffs.Count) { $diffs -join ', ' } else { '-' }), $extra)
        foreach ($f in $fails) { Line "     $f" }
    }
}

# 4. Clean runs (noise seed 0, no history, so a value changes between runs only through the network itself), twice
# per DLL with the stage dump at recording 3: the first value that differs from the RX 9070 XT and the first that
# changes between the two runs, each with the dispatch that made it.
$cleanFirst = @{}
if (-not $script:notRdna3) {
    Write-Host "[4/6] Clean runs with the per-stage capture: $($Runs.Count) sizes x 2 DLLs x 2 runs..."
    foreach ($dll in 'rdiag', 'fix') {
        foreach ($c in $Runs) {
            $name = $c.name
            Write-Host "    $dll $name ..."
            $dir = Join-Path $result "clean\$dll\$name"
            $dumps = @(); $ps = @(); $bins = @(); $info = $null
            foreach ($k in 1, 2) {
                $d = "work\cdump_$dll.$name.d$k"
                Remove-Item -LiteralPath (Join-Path $files $d) -Recurse -Force -ErrorAction SilentlyContinue
                $v = @{ MZ_STAGE_DUMP = $d; MZ_STAGE_FRAME = "$CleanDumpAt" }
                foreach ($x in $clean.Keys) { $v[$x] = $clean[$x] }
                $r = E2E "$dll.$name.c$k" $dll $c $CleanFrames $v $dir
                if (-not $r.ok) { Line "FAIL clean $dll ${name}: run $k failed: $($r.why)"; continue }
                if (-not $info) { $info = $r }
                if (Test-Path -LiteralPath (Join-Path $files "$d\stages.bin")) { $dumps += $d }
                $ps += (Score $r.bin "clean_$name" $c (Join-Path $dir "compare_c$k.txt") $null).text
                $bins += $r.bin
            }
            $rr = if ($bins.Count -eq 2) { RunsDiff $bins[0] $bins[1] $c (Join-Path $dir 'runs_c1_c2.txt') } else { '-' }
            foreach ($b in $bins) { Remove-Item -LiteralPath (Join-Path $files $b) -ErrorAction SilentlyContinue }
            if (-not $dumps.Count) { Line "FAIL clean $dll ${name}: no dump was written"; continue }
            $cmp = Join-Path $dir 'stagecmp.txt'
            $null = Run 'stagecmp.exe' "reference\cstages_$name $($dumps[0]) ..\result\clean\$dll\$name\table.txt" $cmp 5
            $fd = (First $cmp '^STAGE first divergence: ') -replace '^STAGE first divergence: ', ''
            $cnt = First $cmp '^STAGE \S+ vs '
            $counts = if ($cnt -match 'compared \((.*)\)') { $Matches[1] } else { '' }
            $rv = 'one run only'
            if ($dumps.Count -eq 2) {
                $rf = Join-Path $dir 'stagecmp_runs.txt'
                $null = Run 'stagecmp.exe' "--runs $($dumps[0]) $($dumps[1])" $rf 5
                $rv = (First $rf '^RUNS ') -replace '^RUNS ', ''
                # A second dump of each run for the analysis at home (stages.bin is ~150 KB).
                New-Item -ItemType Directory -Path "$dir\d2" -Force | Out-Null
                Copy-Item -LiteralPath (Join-Path $files "$($dumps[1])\stages.txt"), (Join-Path $files "$($dumps[1])\stages.bin") -Destination "$dir\d2"
            }
            Copy-Item -LiteralPath (Join-Path $files "$($dumps[0])\stages.txt"), (Join-Path $files "$($dumps[0])\stages.bin") -Destination $dir
            $key = $null
            if ($rv -match 'the first: key (\d+)') { $key = [int]$Matches[1] } elseif ($fd -match '^key (\d+)') { $key = [int]$Matches[1] }
            if ($null -ne $key) { $cleanFirst["$dll.$name"] = $key }
            Line ("CLEAN {0,-5} {1}: PSNR vs RX 9070 XT {2}; runs {3}; {4}, {5}" -f $dll, $name, ($ps -join ' / '), $rr,
                  $(if ($info) { $info.persist } else { '?' }), $(if ($info) { $info.ms } else { '' }))
            Line "     first divergence: $fd"
            Line "     values: $counts; runs: $rv"
            foreach ($d in $dumps) { Remove-Item -LiteralPath (Join-Path $files $d) -Recurse -Force -ErrorAction SilentlyContinue }
        }
    }
}

# 5. Raw bytes of one value at 1152x648 (clean run, fixed DLL; the released one when the fix has nothing to show):
# the first that changes run to run, else the first that differs from the RX 9070 XT. The FP16 twin only.
if (-not $script:notRdna3) {
    Write-Host '[5/6] Raw capture of one value at 1152x648...'
    $c = $Runs | Where-Object { $_.name -eq 'small_1152x648' } | Select-Object -First 1
    $dll = if ($cleanFirst.ContainsKey('fix.small_1152x648')) { 'fix' } elseif ($cleanFirst.ContainsKey('rdiag.small_1152x648')) { 'rdiag' } else { $null }
    if (-not $c -or -not $dll) { Line 'INFO full dump: nothing differs at 1152x648, nothing to capture' }
    else {
        $key = $cleanFirst["$dll.small_1152x648"]
        $d = "work\full_$dll"
        $dir = Join-Path $result "full\$dll.small_1152x648"
        $v = @{ MZ_STAGE_DUMP = $d; MZ_STAGE_FRAME = "$CleanDumpAt"; MZ_STAGE_FULL = "$key" }
        foreach ($x in $clean.Keys) { $v[$x] = $clean[$x] }
        $r = E2E "$dll.small_1152x648.full" $dll $c $CleanFrames $v $dir
        Remove-Item -LiteralPath (Join-Path $files $r.bin) -ErrorAction SilentlyContinue
        $twin = Get-ChildItem -LiteralPath (Join-Path $files $d) -Filter 'full_*_twin.bin' -ErrorAction SilentlyContinue
        if (-not $twin) { $twin = Get-ChildItem -LiteralPath (Join-Path $files $d) -Filter 'full_*_arena.bin' -ErrorAction SilentlyContinue }
        if (-not $twin) { Line "FAIL full dump $dll key ${key}: nothing written ($($r.why))" }
        elseif ($twin.Length -gt 32MB) { Line ("INFO full dump {0} key {1}: {2:N0} MB, too big to send, left out" -f $dll, $key, ($twin.Length / 1MB)) }
        else {
            Copy-Item -LiteralPath $twin.FullName, (Join-Path $files "$d\stages.txt") -Destination $dir
            Line ("INFO full dump {0} small_1152x648 key {1}: {2:N1} MB ({3})" -f $dll, $key, ($twin.Length / 1MB), $twin.Name)
        }
        Remove-Item -LiteralPath (Join-Path $files $d) -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# 6. Summary and the zip.
Write-Host '[6/6] Writing the result...'
if ($script:notRdna3) { $short.Add('This card did not take the RX 7000 network, so nothing was tested. Please tell us which card this is.') }
else {
    foreach ($t in 'rel', 'fix') {
        $label = if ($t -eq 'rel') { 'Released 0.5.8' } else { 'Fixed DLL     ' }
        $short.Add("${label}: right (all runs within 40 dB of an RX 9070 XT): $(if ($good[$t].Count) { $good[$t] -join ', ' } else { 'none' })")
        $short.Add("${label}: wrong or failed: $(if ($bad[$t].Count) { $bad[$t] -join ', ' } else { 'none' })")
        $short.Add("${label}: picture changes from run to run: $(if ($vary[$t].Count) { $vary[$t] -join ', ' } else { 'none' })")
    }
}
$head = @("OpenNR RX 7000 diagnostic kit 2 (fix for 0.5.8), $((Get-Date).ToString('yyyy-MM-dd HH:mm')), $([int]((Get-Date) - $t0).TotalMinutes) min", '',
          'IN SHORT') + ($short | ForEach-Object { "  $_" }) + @('', 'DETAILS')
$tail = @('', 'How to read this: GOOD = every run matches the RX 9070 XT reference (PSNR 40 dB or more), BAD = it does not.',
          '"runs identical" means the same picture every time; "differ" means it changes from run to run (flicker).',
          'rel = released 0.5.8, fix = the fixed DLL, fixfb = the fixed DLL with full memory barriers.',
          'CLEAN lines: noise seed 0 and no history, so nothing carries over from frame to frame; "first divergence" is',
          'the first network value that differs from the RX 9070 XT, "runs" the first that changes between two runs.',
          'Raw files: system.txt, runs\, clean\ (per-stage tables and dumps), full\, console.txt.')
Set-Content -LiteralPath (Join-Path $result 'SUMMARY.txt') -Value ($head + $summary + $tail)
Stop-Transcript | Out-Null
Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
$zip = Join-Path $kit "OpenNR-RX7000-diag2-result-$stamp.zip"
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
