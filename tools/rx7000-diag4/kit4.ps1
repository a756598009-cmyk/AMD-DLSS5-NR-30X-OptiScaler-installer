# OpenNR RX 7000 diagnostic kit 4: run by run-test.bat. Everything goes into ..\result, zipped beside run-test.bat.
# 1. The sequence check (MZ_SEQ, local patch 21): the RDNA3 network's own dispatches replayed on this card from one
#    clean frame's bytes, each dispatch's output in the sequence against the same dispatch alone, run to run, and
#    against the RX 9070 XT's FP8 network (reference\seqref), with other barriers / submits / a scrub between them.
# 2. The fix: the first barrier variant that made the replay right (else twinbuf) as NR_RDNA3_SYNC, and the full
#    network, released 0.5.8 (rel\) against the kit 4 DLL (k4\), 3 runs at four sizes, against the RX 9070 XT.
# Our own testing only: KIT_FORCE_RDNA3=1 runs the RDNA3 network on any card, KIT_QUICK=1 one size and two runs,
# KIT_QUIET=1 opens no Explorer at the end.
$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'
$files = $PSScriptRoot
$kit = Split-Path $files -Parent
$result = Join-Path $kit 'result'
$work = Join-Path $files 'work'
$stamp = Get-Date -Format 'yyyy-MM-dd-HHmm'
Set-Location -LiteralPath $files
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
$runsPerSize = if ($quick) { 2 } else { 3 }
$frames = 40

# The replays (sizes with their FP8 reference) and the picture sizes.
$Seq = @(
    @{ tag = '1152x648'; w = 1152; h = 648; vars = @{} },
    @{ tag = '1152x648_noreuse'; w = 1152; h = 648; vars = @{ NR_ARENA_REUSE = '0' } },
    @{ tag = '1152x648_onequeue'; w = 1152; h = 648; vars = @{ MZ_ONE_QUEUE = '1' } },
    @{ tag = '1280x720'; w = 1280; h = 720; vars = @{} },
    @{ tag = '1280x720_noreuse'; w = 1280; h = 720; vars = @{ NR_ARENA_REUSE = '0' } },
    @{ tag = '1920x1080'; w = 1920; h = 1080; vars = @{} },
    @{ tag = '1920x1080_noreuse'; w = 1920; h = 1080; vars = @{ NR_ARENA_REUSE = '0' } })
$Pics = @(
    @{ name = 'small_1152x648'; w = 1152; h = 648 },
    @{ name = 'small_1280x720'; w = 1280; h = 720 },
    @{ name = 'small_1920x1080'; w = 1920; h = 1080 },
    @{ name = 'small_2560x1440'; w = 2560; h = 1440 })
if ($quick) { $Seq = @($Seq[0]); $Pics = @($Pics[0]) }

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
Write-Host 'OpenNR RX 7000 diagnostic kit 4. This takes about 10 minutes. Please do not close this window.'
Write-Host ''

# 1. System and the matrix layout probes.
Write-Host '[1/5] System, Vulkan and matrix layout probe...'
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

# 2. The small model, from the same place the OpenNR installer takes it (or already copied in from kit 1 or 2).
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

$baseEnv = @{}
if ($forced) { $baseEnv = @{ MZ_FORCE_RDNA3 = '1'; MZ_RDNA3_CHECK = '0' }; Line 'INFO KIT_FORCE_RDNA3=1: the RDNA3 network is forced, its checks skipped' }
foreach ($k in 'MZ_FORCE_RDNA3', 'MZ_RDNA3_CHECK', 'DLSSNR_MODEL', 'E2E_FLAGS', 'E2E_PAN', 'E2E_CREATE_FLAGS', 'MZ_STAGE_DUMP',
               'MZ_KCAP', 'MZ_SEQ', 'MZ_SEQ_REF', 'MZ_SEQ_REFS', 'MZ_SEQ_VARIANTS', 'MZ_SEQ_RUNS', 'MZ_SEQ_ISO', 'MZ_SEQ_FROM',
               'MZ_SEQ_TO', 'MZ_SEQ_ALL', 'MZ_ONE_QUEUE', 'NR_RDNA3_SYNC', 'NR_TCHAIN', 'NR_ARENA_REUSE', 'NR_NO_PERSIST',
               'NR_RDNA3_PERSIST', 'NR_INV_BARRIERS', 'NR_DIAG_SEED0', 'NR_DIAG_TGATE0') {
    Remove-Item "Env:$k" -ErrorAction SilentlyContinue
}
foreach ($f in 'pipeline-rdna3.cache', 'pipeline.cache', 'prewarm', 'build.pending', 'build.failed', 'rdna3-check.txt') {
    Remove-Item -LiteralPath (Join-Path $files "dlssnr-amd\$f") -Recurse -Force -ErrorAction SilentlyContinue
}
$script:first = $true
$script:notRdna3 = $false

# One harness run of $dll (rel or k4) on test.png; leaves work\<tag>.bin and returns what the run said.
function E2E($tag, $dll, $w, $h, $n, $vars, $dir) {
    $r = @{ ok = $false; why = ''; net = '?'; ms = ''; build = ''; bin = "work\$tag.bin" }
    if (-not $model) { $r.why = 'no model'; return $r }
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    $log = Join-Path $files 'mochizuki_nr.log'
    Remove-Item -LiteralPath $log, (Join-Path $files $r.bin) -ErrorAction SilentlyContinue
    $all = @{ E2E_W = "$w"; E2E_H = "$h"; E2E_FLAGS = '0' }
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
function Score($bin, $ref, $w, $h, $out, $pictures) {
    $a = "$bin $w $h reference\$ref"
    if ($pictures) { $a += " ..\result\$pictures\diff_x16.png ..\result\$pictures\output.jpg" }
    $null = Run 'compare.exe' $a $out 5
    $l = First $out '^COMPARE '
    if ($l -match 'psnr (\S+) dB \(worst area (\S+)\), nan/inf (\d+)') {
        return @{ psnr = $(if ($Matches[1] -eq 'inf') { 999.0 } else { [double]$Matches[1] }); text = "$($Matches[1]) dB"; bad = [int]$Matches[3] }
    }
    return @{ psnr = -1.0; text = 'no comparison'; bad = 0 }
}
function RunsDiff($a, $b, $w, $h, $out) {
    $null = Run 'compare.exe' "--bins $a $b $w $h" $out 5
    $l = First $out '^RUNS '
    if ($l -match '^RUNS identical') { return 'identical' }
    if ($l -match 'psnr (\S+) dB.*differing pixels in (.*)$') { return "differ ($($Matches[1]) dB, pixels $($Matches[2]))" }
    return 'not compared'
}

# 3. The sequence check. A clean frame (noise seed 0, no history), 6 recordings; recording 3's bytes are replayed
# after it. The frame itself runs the released barriers (NR_RDNA3_SYNC=released).
$seqFiles = @()
$reproduced = $false
if ($model) {
    Write-Host "[3/5] Sequence check: the network replayed dispatch by dispatch, $($Seq.Count) cases..."
    $i = 0
    foreach ($c in $Seq) {
        if ($script:notRdna3) { break }
        ++$i
        Write-Host "    ($i/$($Seq.Count)) $($c.tag) ..."
        $dir = Join-Path $result "seq\$($c.tag)"
        $d = "work\seq_$($c.tag)"
        $v = @{ NR_DIAG_SEED0 = '1'; NR_DIAG_TGATE0 = '1'; NR_RDNA3_SYNC = 'released'; MZ_SEQ = $d; MZ_SEQ_FRAME = '3';
                MZ_SEQ_SAMPLE = '32'; MZ_SEQ_REFS = "reference\seqref\$($c.w)x$($c.h)" }
        foreach ($k in $c.vars.Keys) { $v[$k] = $c.vars[$k] }
        $r = E2E "seq_$($c.tag)" 'k4' $c.w $c.h 6 $v $dir
        if ($r.net -eq 'FP8' -and -not $forced) {
            Line 'SKIPPED everything: this card picked the FP8 network (not an RX 7000)'
            $script:notRdna3 = $true
            break
        }
        $st = Join-Path $files "$d\seq.txt"
        if (-not (Test-Path -LiteralPath $st)) { Line "FAIL seq $($c.tag): no answer ($($r.why))"; continue }
        Copy-Item -LiteralPath $st -Destination (Join-Path $dir 'seq.txt')
        $seqFiles += @{ tag = $c.tag; file = (Join-Path $dir 'seq.txt') }
        $txt = Get-Content -LiteralPath $st
        $failed = $txt | Where-Object { $_ -match '^FAILED ' } | Select-Object -First 1
        if ($failed) { Line "FAIL seq $($c.tag): $failed" }
        $res = $txt | Where-Object { $_ -match '^RESULT ' }
        $bv = ($res | Where-Object { $_ -match '^RESULT base varies: ' }) -replace '^RESULT base varies: ', ''
        $ba = ($res | Where-Object { $_ -match '^RESULT base differs from alone: ' }) -replace '^RESULT base differs from alone: ', ''
        $av = ($res | Where-Object { $_ -match '^RESULT alone varies: ' }) -replace '^RESULT alone varies: ', ''
        if (($bv -and $bv -ne 'none') -or ($ba -and $ba -ne 'none')) { $reproduced = $true }
        $secs = ($txt | Where-Object { $_ -match '^seconds ' }) -replace '^seconds ', ''
        Line ("SEQ {0}: in sequence changes run to run at {1}; differs from the same dispatch alone at {2}; alone changes at {3} ({4} s)" -f
              $c.tag, $bv, $ba, $av, $secs)
        foreach ($x in ($res | Where-Object { $_ -match '^RESULT variant ' })) { Line ("     " + ($x -replace '^RESULT variant ', '')) }
        # The first dispatch that goes wrong, with its own lines (how much, where in the value, against the FP8 net).
        $firstAt = $null
        foreach ($s in $bv, $ba, $av) { if ($s -match '^dispatch (\d+)') { $n = [int]$Matches[1]; if ($null -eq $firstAt -or $n -lt $firstAt) { $firstAt = $n } } }
        if ($null -ne $firstAt) {
            $k = [array]::IndexOf($txt, ($txt | Where-Object { $_ -match "^S $firstAt " } | Select-Object -First 1))
            if ($k -ge 0) {
                Line "     first offender: $($txt[$k])"
                for ($j = $k + 1; $j -lt $txt.Count -and $txt[$j] -match '^  '; ++$j) { Line "       $($txt[$j].Trim())" }
            }
        }
        # Against the RX 9070 XT, at the C=512 level's first value and the frame's last few.
        $refs = @($txt | Where-Object { $_ -match '^  ref key' })
        if ($refs.Count) {
            $worst = $refs | ForEach-Object { if ($_ -match 'ref key (\d+): .*twin rel ([\d.]+|inf)') { @{ k = $_; rel = $(if ($Matches[2] -eq 'inf') { 1e9 } else { [double]$Matches[2] }) } } } |
                     Where-Object { $_ } | Sort-Object { $_.rel } -Descending | Select-Object -First 1
            if ($worst) { Line "     worst against the RX 9070 XT: $($worst.k.Trim())" }
        }
        Remove-Item -LiteralPath (Join-Path $files $r.bin) -ErrorAction SilentlyContinue
    }
}

# 4. The fix: the first runtime barrier mode whose replay is deterministic and equal to the dispatches alone in every
# case that ran (submit is not a mode the runtime can have: noted only).
$fixMode = 'twinbuf'
$why = 'the default candidate (no case reproduced the problem, or no variant cured it)'
if ($seqFiles.Count) {
    foreach ($cand in 'full', 'global', 'twinbuf', 'zero') {
        $ok = $true
        foreach ($sf in $seqFiles) {
            $l = First $sf.file "^RESULT variant ${cand}: "
            if (-not $l -or $l -notmatch 'varies none; differs from alone none') { $ok = $false }
        }
        if ($ok -and $reproduced) { $fixMode = $cand; $why = "the first variant that made every replay deterministic and equal to the dispatches alone"; break }
    }
    if ($reproduced -and $why -notmatch '^the first') {
        $sub = $true
        foreach ($sf in $seqFiles) { $l = First $sf.file '^RESULT variant submit: '; if (-not $l -or $l -notmatch 'varies none; differs from alone none') { $sub = $false } }
        if ($sub) { $why = 'only one submit per dispatch cured the replay (not a runtime mode); the default candidate' }
    }
}
Line "INFO problem seen in the replay: $(if ($reproduced) { 'yes' } else { 'no' }); fix tried in the full network: NR_RDNA3_SYNC=$fixMode ($why)"

# 5. The full network, 40 frames as kits 1 and 2: released 0.5.8 and the kit 4 DLL with the fix, 3 runs each, against the
# RX 9070 XT; the kit 4 DLL with the released barriers (its no-persistent-run change alone) and with the zero scrub
# at the two smallest sizes as controls.
$good = @{}; $bad = @{}; $vary = @{}
$sets = @(@{ dll = 'rel'; what = 'released 0.5.8'; vars = @{}; pics = $Pics; tag = 'rel' },
          @{ dll = 'k4'; what = "fixed (NR_RDNA3_SYNC=$fixMode)"; vars = @{ NR_RDNA3_SYNC = $fixMode }; pics = $Pics; tag = 'fix' },
          @{ dll = 'k4'; what = 'kit 4 DLL, released barriers'; vars = @{ NR_RDNA3_SYNC = 'released' }; pics = @($Pics | Select-Object -First 2); tag = 'k4rel' })
if ($fixMode -ne 'zero') {
    $sets += @{ dll = 'k4'; what = 'kit 4 DLL, zero scrub'; vars = @{ NR_RDNA3_SYNC = 'zero' }; pics = @($Pics | Select-Object -First 2); tag = 'zero' }
}
$step = 0
if ($model -and -not $script:notRdna3) {
    foreach ($set in $sets) {
        ++$step
        Write-Host "[4/5] Pictures ($step/$($sets.Count)), $($set.what): $($set.pics.Count) sizes x $runsPerSize runs..."
        $good[$set.tag] = @(); $bad[$set.tag] = @(); $vary[$set.tag] = @()
        foreach ($c in $set.pics) {
            $name = $c.name
            Write-Host "    $name ..."
            $dir = Join-Path $result "runs\$($set.tag)\$name"
            $scores = @(); $fails = @(); $info = $null
            for ($k = 1; $k -le $runsPerSize; ++$k) {
                $r = E2E "$($set.tag).$name.r$k" $set.dll $c.w $c.h $frames $set.vars $dir
                if ($r.net -eq 'FP8' -and -not $forced) { $script:notRdna3 = $true; break }
                if (-not $r.ok) { $fails += "run $k failed: $($r.why)"; continue }
                if (-not $info) { $info = $r }
                $pic = if ($k -eq 1 -and $name -eq 'small_1152x648' -and ($set.tag -eq 'rel' -or $set.tag -eq 'fix')) { "runs\$($set.tag)\$name" } else { $null }
                $scores += Score $r.bin "img_$name" $c.w $c.h (Join-Path $dir "compare_r$k.txt") $pic
            }
            if ($script:notRdna3) { break }
            $diffs = @()
            for ($k = 2; $k -le $runsPerSize; ++$k) {
                $a = "work\$($set.tag).$name.r1.bin"; $b = "work\$($set.tag).$name.r$k.bin"
                if ((Test-Path -LiteralPath (Join-Path $files $a)) -and (Test-Path -LiteralPath (Join-Path $files $b))) {
                    $diffs += "r1-r${k}: " + (RunsDiff $a $b $c.w $c.h (Join-Path $dir "runs_r1_r$k.txt"))
                }
            }
            Remove-Item -Path (Join-Path $files "work\$($set.tag).$name.r*.bin") -ErrorAction SilentlyContinue
            $varies = @($diffs | Where-Object { $_ -notmatch 'identical$' }).Count -gt 0
            $okAll = $scores.Count -eq $runsPerSize -and @($scores | Where-Object { $_.psnr -lt 40 -or $_.bad -gt 0 }).Count -eq 0
            $verdict = if ($fails.Count) { 'FAIL' } elseif ($okAll) { 'GOOD' } else { 'BAD ' }
            if ($okAll -and -not $fails.Count) { $good[$set.tag] += $name } else { $bad[$set.tag] += $name }
            if ($varies) { $vary[$set.tag] += $name }
            $extra = if ($info) { "$($info.net) network, $($info.ms), first $($info.build)" } else { '' }
            Line ("{0} {1,-5} {2}: PSNR vs RX 9070 XT {3}; runs {4}; {5}" -f $verdict, $set.tag, $name,
                  (($scores | ForEach-Object { $_.text }) -join ' / '), $(if ($diffs.Count) { $diffs -join ', ' } else { '-' }), $extra)
            foreach ($f in $fails) { Line "     $f" }
        }
        if ($script:notRdna3) { Line 'SKIPPED the pictures: this card picked the FP8 network (not an RX 7000)'; break }
    }
}

# Summary and the zip.
Write-Host '[5/5] Writing the result...'
if ($script:notRdna3) { $short.Add('This card did not take the RX 7000 network, so nothing was tested. Please tell us which card this is.') }
else {
    foreach ($sf in $seqFiles) {
        $bv = (First $sf.file '^RESULT base varies: ') -replace '^RESULT base varies: ', ''
        $ba = (First $sf.file '^RESULT base differs from alone: ') -replace '^RESULT base differs from alone: ', ''
        $short.Add("Replay $($sf.tag): changes run to run from $bv; first differs from the dispatch alone at $ba")
    }
    $short.Add("Fix tried: NR_RDNA3_SYNC=$fixMode ($why)")
    foreach ($t in $sets | ForEach-Object { $_.tag }) {
        if (-not $good.ContainsKey($t)) { continue }
        $label = @{ rel = 'Released 0.5.8 '; fix = 'Fixed DLL      '; k4rel = 'k4, old barrier'; zero = 'k4, zero scrub ' }[$t]
        $short.Add("${label}: right (40 dB or more, every run): $(if ($good[$t].Count) { $good[$t] -join ', ' } else { 'none' }); " +
                   "changes run to run: $(if ($vary[$t].Count) { $vary[$t] -join ', ' } else { 'none' })")
    }
}
$head = @("OpenNR RX 7000 diagnostic kit 4 (sequence check and fix), $((Get-Date).ToString('yyyy-MM-dd HH:mm')), $([int]((Get-Date) - $t0).TotalMinutes) min", '',
          'IN SHORT') + ($short | ForEach-Object { "  $_" }) + @('', 'DETAILS')
$tail = @('', 'How to read this: SEQ lines replay the RX 7000 network dispatch by dispatch from one clean frame. "in sequence" is',
          'dispatches 1..i in one submit as the game runs them; "alone" is dispatch i in a submit of its own after the queue',
          'went idle, on the bytes dispatches 1..i-1 left. Variants: full/global/twinbuf = other barriers between dispatches,',
          'submit = one submit per dispatch, scrub = garbage left in LDS and registers before each dispatch, zero = zeros there.',
          'Pictures: GOOD = every run 40 dB or more from the RX 9070 XT; "identical" runs = no flicker.',
          'Raw files: seq\<case>\seq.txt (every dispatch), runs\, system.txt, console.txt.')
Set-Content -LiteralPath (Join-Path $result 'SUMMARY.txt') -Value ($head + $summary + $tail)
Stop-Transcript | Out-Null
Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
$zip = Join-Path $kit "OpenNR-RX7000-diag4-result-$stamp.zip"
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
