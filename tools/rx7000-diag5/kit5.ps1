# OpenNR RX 7000 diagnostic kit 5: run by run-test.bat. Everything goes into ..\result, zipped beside run-test.bat.
# 1. Dispatch 11 (attn, the C=512 window attention) alone on fixed bytes (kreplay, kit 3's method): the released
#    pipeline, the RDNA2 tree's native body (no cooperative matrices) and the released one with one suspect changed,
#    each with zeros / garbage outside its inputs and with garbage / zeros left in LDS and registers before it.
# 2. The sequence replay (MZ_SEQ, kit 4) with the native attention, to see the next dispatch that goes wrong if any.
# 3. The full network, released 0.5.8 (rel\) against the kit 5 DLL (k5\) with the native attention, 3 runs at four
#    sizes, against the RX 9070 XT; the kit 5 DLL with the released attention, and with the best variant of step 1 if
#    one was deterministic and right, at the two small sizes.
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

$Seq = @(
    @{ tag = '1152x648'; w = 1152; h = 648 },
    @{ tag = '1280x720'; w = 1280; h = 720 },
    @{ tag = '1920x1080'; w = 1920; h = 1080 })
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
Write-Host 'OpenNR RX 7000 diagnostic kit 5. This takes about 10 minutes. Please do not close this window.'
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

# 2. Dispatch 11 alone (kreplay, no model needed): every variant in attn\kit.txt.
Write-Host '[2/6] The attention dispatch alone, 16 versions (about 2 minutes)...'
$attnRes = Join-Path $result 'attn'
New-Item -ItemType Directory -Path $attnRes -Force | Out-Null
$code = Run 'kreplay.exe' "kit `"$files\attn`" `"$attnRes`"" (Join-Path $attnRes 'console.txt') 20
$attnSum = Join-Path $attnRes 'SUMMARY.txt'
$best = $null
if (Test-Path -LiteralPath $attnSum) {
    $at = Get-Content -LiteralPath $attnSum
    $i = [array]::IndexOf($at, 'IN SHORT')
    for ($j = $i + 1; $j -lt $at.Count -and $at[$j] -ne ''; ++$j) { Line ("ATTN" + $at[$j]) }
    $right = $at | Where-Object { $_ -match '^\s+of those right .*: (.*)$' } | Select-Object -First 1
    if ($right -and $right -match ': (.*)$' -and $Matches[1] -ne 'none') {
        foreach ($n in ($Matches[1] -split ', ')) {
            $v = ($n -split ' ')[0]
            if ($v -ne 'nat' -and $v -ne 'base' -and (Test-Path -LiteralPath (Join-Path $files "attn\spv\$v.spv"))) { $best = $v; break }
        }
    }
} else { Line "FAIL the attention dispatch alone: no answer (kreplay exit $(Hex $code))" }
if ($best) { Line "INFO the released attention with '$best' is deterministic and right alone: it runs in the full network too" }

# 3. The small model, from the same place the OpenNR installer takes it (or already copied in from kit 1, 2 or 4).
Write-Host '[3/6] The model (about 110 MB, unless it is already in files\dlssnr-amd)...'
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
               'MZ_SEQ_TO', 'MZ_SEQ_ALL', 'MZ_ONE_QUEUE', 'NR_RDNA3_SYNC', 'NR_RDNA3_ATTN', 'NR_TCHAIN', 'NR_ARENA_REUSE',
               'NR_NO_PERSIST', 'NR_RDNA3_PERSIST', 'NR_INV_BARRIERS', 'NR_DIAG_SEED0', 'NR_DIAG_TGATE0') {
    Remove-Item "Env:$k" -ErrorAction SilentlyContinue
}
foreach ($f in 'pipeline-rdna3.cache', 'pipeline.cache', 'prewarm', 'build.pending', 'build.failed', 'rdna3-check.txt') {
    Remove-Item -LiteralPath (Join-Path $files "dlssnr-amd\$f") -Recurse -Force -ErrorAction SilentlyContinue
}
$script:first = $true
$script:notRdna3 = $false

# One harness run of $dll (rel or k5) on test.png; leaves work\<tag>.bin and returns what the run said.
function E2E($tag, $dll, $w, $h, $n, $vars, $dir) {
    $r = @{ ok = $false; why = ''; net = '?'; ms = ''; build = ''; bin = "work\$tag.bin"; attn = '' }
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
    $at = $lt | Where-Object { $_ -match 'RDNA3 window attention: (.*)$' } | Select-Object -First 1
    if ($at -and $at -match 'RDNA3 window attention: (.*)$') { $r.attn = $Matches[1] -replace '^.*[\\/]', '' }
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

# 4. The sequence replay with the native attention (kit 4's MZ_SEQ): a clean frame (noise seed 0, no history), 6
# recordings, recording 3's bytes replayed after it, with the released barriers; variants scrub and zero.
$seqFiles = @()
if ($model) {
    Write-Host "[4/6] Sequence check with the native attention, $($Seq.Count) sizes..."
    $i = 0
    foreach ($c in $Seq) {
        if ($script:notRdna3) { break }
        ++$i
        Write-Host "    ($i/$($Seq.Count)) $($c.tag) ..."
        $dir = Join-Path $result "seq\$($c.tag)"
        $d = "work\seq_$($c.tag)"
        $v = @{ NR_DIAG_SEED0 = '1'; NR_DIAG_TGATE0 = '1'; NR_RDNA3_SYNC = 'released'; NR_RDNA3_ATTN = 'native'; MZ_SEQ = $d;
                MZ_SEQ_FRAME = '3'; MZ_SEQ_SAMPLE = '32'; MZ_SEQ_VARIANTS = 'scrub,zero'; MZ_SEQ_REFS = "reference\seqref\$($c.w)x$($c.h)" }
        $r = E2E "seq_$($c.tag)" 'k5' $c.w $c.h 6 $v $dir
        if ($r.net -eq 'FP8' -and -not $forced) {
            Line 'SKIPPED the network steps: this card picked the FP8 network (not an RX 7000)'
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
        $secs = ($txt | Where-Object { $_ -match '^seconds ' }) -replace '^seconds ', ''
        Line ("SEQ {0} (attention {1}): in sequence changes run to run at {2}; differs from the same dispatch alone at {3}; alone changes at {4} ({5} s)" -f
              $c.tag, $r.attn, $bv, $ba, $av, $secs)
        foreach ($x in ($res | Where-Object { $_ -match '^RESULT variant ' })) { Line ("     " + ($x -replace '^RESULT variant ', '')) }
        $firstAt = $null
        foreach ($s in $bv, $ba, $av) { if ($s -match '^dispatch (\d+)') { $n = [int]$Matches[1]; if ($null -eq $firstAt -or $n -lt $firstAt) { $firstAt = $n } } }
        if ($null -ne $firstAt) {
            $k = [array]::IndexOf($txt, ($txt | Where-Object { $_ -match "^S $firstAt " } | Select-Object -First 1))
            if ($k -ge 0) {
                Line "     first offender: $($txt[$k])"
                for ($j = $k + 1; $j -lt $txt.Count -and $txt[$j] -match '^  '; ++$j) { Line "       $($txt[$j].Trim())" }
            }
        }
        # Against the RX 9070 XT: the attention's own value (key 74), and the first value off by more than 0.1.
        $k74 = $txt | Where-Object { $_ -match '^  ref key 74:' } | Select-Object -First 1
        if ($k74) { Line "     attention output against the RX 9070 XT: $($k74.Trim())" }
        $cur = ''
        foreach ($t in $txt) {
            if ($t -match '^S (\d+) (\S+)') { $cur = "dispatch $($Matches[1]) ($($Matches[2]))" }
            elseif ($t -match '^  ref key (\d+): .*twin rel ([\d.]+|inf)') {
                $rel = if ($Matches[2] -eq 'inf') { 1e9 } else { [double]$Matches[2] }
                if ($rel -gt 0.1) { Line "     first value off against the RX 9070 XT (twin rel over 0.1): $cur, $($t.Trim())"; break }
            }
        }
        Remove-Item -LiteralPath (Join-Path $files $r.bin) -ErrorAction SilentlyContinue
    }
}

# 5. The full network, 40 frames as kits 1, 2 and 4: released 0.5.8 and the kit 5 DLL with the native attention at
# four sizes; the kit 5 DLL with the released attention (= kit 4's fix) and with the best variant of step 2 at two.
$good = @{}; $bad = @{}; $vary = @{}
$sets = @(@{ dll = 'rel'; what = 'released 0.5.8'; vars = @{}; pics = $Pics; tag = 'rel' },
          @{ dll = 'k5'; what = 'kit 5 DLL, native attention'; vars = @{ NR_RDNA3_ATTN = 'native' }; pics = $Pics; tag = 'nat' },
          @{ dll = 'k5'; what = 'kit 5 DLL, released attention'; vars = @{ NR_RDNA3_ATTN = 'released' }; pics = @($Pics | Select-Object -First 2); tag = 'k5rel' })
if ($best) {
    $sets += @{ dll = 'k5'; what = "kit 5 DLL, attention '$best'"; vars = @{ NR_RDNA3_ATTN = (Join-Path $files "attn\spv\$best.spv") };
                pics = @($Pics | Select-Object -First 2); tag = 'best' }
}
$step = 0
if ($model -and -not $script:notRdna3) {
    foreach ($set in $sets) {
        ++$step
        Write-Host "[5/6] Pictures ($step/$($sets.Count)), $($set.what): $($set.pics.Count) sizes x $runsPerSize runs..."
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
                $pic = if ($k -eq 1 -and $name -eq 'small_1152x648' -and ($set.tag -eq 'rel' -or $set.tag -eq 'nat')) { "runs\$($set.tag)\$name" } else { $null }
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
            $extra = if ($info) { "$($info.net) network$(if ($info.attn) { ", attention $($info.attn)" }), $($info.ms), first $($info.build)" } else { '' }
            Line ("{0} {1,-5} {2}: PSNR vs RX 9070 XT {3}; runs {4}; {5}" -f $verdict, $set.tag, $name,
                  (($scores | ForEach-Object { $_.text }) -join ' / '), $(if ($diffs.Count) { $diffs -join ', ' } else { '-' }), $extra)
            foreach ($f in $fails) { Line "     $f" }
        }
        if ($script:notRdna3) { Line 'SKIPPED the pictures: this card picked the FP8 network (not an RX 7000)'; break }
    }
}

# Summary and the zip.
Write-Host '[6/6] Writing the result...'
foreach ($l in ($summary | Where-Object { $_ -match '^ATTN' })) { $short.Add(($l -replace '^ATTN\s*', 'Dispatch 11 alone | ')) }
if ($script:notRdna3) { $short.Add('This card did not take the RX 7000 network, so the network steps were skipped. Please tell us which card this is.') }
else {
    foreach ($sf in $seqFiles) {
        $bv = (First $sf.file '^RESULT base varies: ') -replace '^RESULT base varies: ', ''
        $ba = (First $sf.file '^RESULT base differs from alone: ') -replace '^RESULT base differs from alone: ', ''
        $short.Add("Replay $($sf.tag), native attention: changes run to run from $bv; first differs from the dispatch alone at $ba")
    }
    foreach ($t in $sets | ForEach-Object { $_.tag }) {
        if (-not $good.ContainsKey($t)) { continue }
        $label = @{ rel = 'Released 0.5.8      '; nat = 'Native attention    '; k5rel = 'Released attention  '; best = "Attention '$best'" }[$t]
        $short.Add("${label}: right (40 dB or more, every run): $(if ($good[$t].Count) { $good[$t] -join ', ' } else { 'none' }); " +
                   "changes run to run: $(if ($vary[$t].Count) { $vary[$t] -join ', ' } else { 'none' })")
    }
}
$head = @("OpenNR RX 7000 diagnostic kit 5 (the attention), $((Get-Date).ToString('yyyy-MM-dd HH:mm')), $([int]((Get-Date) - $t0).TotalMinutes) min", '',
          'IN SHORT') + ($short | ForEach-Object { "  $_" }) + @('', 'DETAILS')
$tail = @('', 'How to read this: ATTN lines are dispatch 11 (the C=512 window attention) alone on the RX 9070 XT''s inputs:',
          'the released pipeline, the native one (no matrix instructions) and one change each (attn\SUMMARY.txt has every run).',
          'SEQ lines replay the network dispatch by dispatch with the native attention ("in sequence" = as the game runs them,',
          '"alone" = in a submit of its own; scrub / zero = garbage / zeros left in LDS and registers before each dispatch).',
          'Pictures: GOOD = every run 40 dB or more from the RX 9070 XT; "identical" runs = no flicker.',
          'Raw files: attn\, seq\<size>\seq.txt, runs\, system.txt, console.txt.')
Set-Content -LiteralPath (Join-Path $result 'SUMMARY.txt') -Value ($head + $summary + $tail)
Stop-Transcript | Out-Null
Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
$zip = Join-Path $kit "OpenNR-RX7000-diag5-result-$stamp.zip"
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
