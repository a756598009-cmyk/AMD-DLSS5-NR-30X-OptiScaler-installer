# OpenNR RX 7000 diagnostic kit 7 (ACO): run by run-test.bat. Everything goes into ..\result, zipped beside run-test.bat.
# 1. System and Vulkan (vkprobe).
# 2. ACO probe (no model): two tiny shaders as Mesa ACO gfx11 machine code imported through VK_KHR_pipeline_binary,
#    each compared word for word with the driver's own compile.
# 3. The two attention kernels alone on fixed bytes (kreplay, kit 6's cases): the driver's compile of released 0.5.8
#    and of the 0.5.9 fix, then ACO's code for the fix's kernels and for 0.5.8's cooperative-matrix kernels.
# 4. The full network on the test picture against the RX 9070 XT, 4 sizes x 3 runs, network ms, which pipelines
#    were imported: drv (the 0.5.9 fix, driver compiler), acofix (ACO, the fix's SPIR-V), acocoop (ACO, attn and
#    vitattn on cooperative matrices as in 0.5.8).
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
$Pics = @(
    @{ name = 'small_1152x648'; w = 1152; h = 648 },
    @{ name = 'small_1280x720'; w = 1280; h = 720 },
    @{ name = 'small_1920x1080'; w = 1920; h = 1080 },
    @{ name = 'small_2560x1440'; w = 2560; h = 1440 })
if ($quick) { $Pics = @($Pics[0]) }

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
# The gfx11 ACO bundle the network takes: dlssnr-amd\aco-gfx11 = bundles\<which> (fix or coop), its import cache emptied.
function UseBundle($which) {
    $b = Join-Path $files 'dlssnr-amd\aco-gfx11'
    if (Test-Path -LiteralPath $b) { Remove-Item -LiteralPath $b -Recurse -Force }
    Remove-Item -LiteralPath (Join-Path $files 'dlssnr-amd\aco-cache') -Recurse -Force -ErrorAction SilentlyContinue
    if ($which) { Copy-Item -LiteralPath (Join-Path $files "bundles\$which") -Destination $b -Recurse }
}
UseBundle $null

Write-Host ''
Write-Host 'OpenNR RX 7000 diagnostic kit 7 (ACO). This takes about 15 minutes. Please do not close this window.'
Write-Host ''

# 1. System.
Write-Host '[1/6] System and Vulkan...'
$sys = Join-Path $result 'system.txt'
$code = Run 'vkprobe.exe' 'probe' $sys 10
$sysText = if (Test-Path -LiteralPath $sys) { Get-Content -LiteralPath $sys } else { @() }
$sysText | Where-Object { $_ -match '^(windows:|amd software:)' } | ForEach-Object { Line "INFO $_" }
$amd = $sysText | Where-Object { $_ -match '^vulkan device \d+: .*vendor 1002' } | Select-Object -First 1
if ($amd) { Line "INFO $amd" }
if ($code -ne 0) { Line "FAIL vkprobe exit $(Hex $code)" }

# 2. The ACO probe.
Write-Host '[2/6] ACO probe (two tiny shaders)...'
$pr = Join-Path $result 'aco-probe.txt'
$code = Run 'nr_aco_probe.exe' "`"$files\acoprobe`" `"$pr.report`"" $pr 5
if (Test-Path -LiteralPath "$pr.report") { Get-Content -LiteralPath "$pr.report" | Add-Content -LiteralPath $pr; Remove-Item -LiteralPath "$pr.report" }
$probeLines = if (Test-Path -LiteralPath $pr) { Get-Content -LiteralPath $pr | Where-Object { $_ -match 'navi31 record|^RESULT|driver compile|pipeline_binary' } | Select-Object -Unique } else { @() }
foreach ($l in $probeLines) { Line ("PROBE " + $l.Trim()) }
if ($code -ne 0 -and -not $probeLines) { Line "FAIL ACO probe exit $(Hex $code)" }
$probeOk = @($probeLines | Where-Object { $_ -match 'navi31 record.*BIT-EXACT' }).Count
$probeShort = "ACO probe: $probeOk of 2 tiny shaders imported and bit-exact" + $(if ($probeLines | Where-Object { $_ -match 'navi31 record.*(REFUSED|WRONG|did not)' }) { ' (see aco-probe.txt)' } else { '' })

# 3. The model.
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
               'MZ_KCAP', 'MZ_SEQ', 'MZ_ONE_QUEUE', 'NR_RDNA3_SYNC', 'NR_RDNA3_ATTN', 'NR_RDNA3_VITATTN', 'NR_TCHAIN',
               'NR_RDNA3_PERSIST', 'NR_ACO_DIR', 'NR_ACO_NATIVE_KERNELS', 'NR_ACO_CACHE', 'NR_ACO_SHELL_DIR', 'KREPLAY_ACO', 'KREPLAY_ACO_TEMPLATE') {
    Remove-Item "Env:$k" -ErrorAction SilentlyContinue
}
foreach ($f in 'pipeline-rdna3.cache', 'pipeline.cache', 'prewarm', 'build.pending', 'build.failed', 'rdna3-check.txt') {
    Remove-Item -LiteralPath (Join-Path $files "dlssnr-amd\$f") -Recurse -Force -ErrorAction SilentlyContinue
}

# 4. The attention kernels alone.
$aloneShort = @()
foreach ($ks in @(@{ dir = 'attn'; what = 'attention (dispatch 11), driver compiler'; aco = $null },
                  @{ dir = 'vit'; what = 'ViT attention (dispatch 21), driver compiler'; aco = $null },
                  @{ dir = 'attn-aco'; what = 'attention (dispatch 11), ACO'; aco = 'g_attn.spv' },
                  @{ dir = 'vit-aco'; what = 'ViT attention (dispatch 21), ACO'; aco = 'g_vitattn.spv' })) {
    Write-Host "[4/6] The $($ks.what) alone..."
    $res = Join-Path $result $ks.dir
    New-Item -ItemType Directory -Path $res -Force | Out-Null
    $v = @{}
    if ($ks.aco) { $v = @{ KREPLAY_ACO = (Join-Path $files 'alone-records'); KREPLAY_ACO_TEMPLATE = (Join-Path $files "dlssnr-amd\shaders-rdna3\$($ks.aco)") } }
    SetEnv $v
    $code = Run 'kreplay.exe' "kit `"$files\$($ks.dir)`" `"$res`"" (Join-Path $res 'console.txt') 20
    ClearEnv $v
    $sum = Join-Path $res 'SUMMARY.txt'
    if (-not (Test-Path -LiteralPath $sum)) { Line "FAIL the $($ks.what) alone: no answer (kreplay exit $(Hex $code))"; continue }
    $at = Get-Content -LiteralPath $sum
    $i = [array]::IndexOf($at, 'IN SHORT')
    for ($j = $i + 1; $j -lt $at.Count -and $at[$j] -ne ''; ++$j) { Line ("ALONE $($ks.dir)" + $at[$j]); $aloneShort += "$($ks.dir)$($at[$j])" }
    if ($ks.aco) {
        $imp = @(Get-ChildItem -LiteralPath $res -Recurse -File | ForEach-Object { Get-Content -LiteralPath $_.FullName } |
                 Where-Object { $_ -match '^ACO (imported|not imported|no record)' } | Select-Object -Unique)
        foreach ($l in $imp) { Line "     $l" }
    }
}

# 5. The full network.
$script:first = $true
$script:notRdna3 = $false
function E2E($tag, $w, $h, $n, $vars, $dir) {
    $r = @{ ok = $false; why = ''; net = '?'; ms = ''; build = ''; bin = "work\$tag.bin"; imp = 0; fb = 0; fbnames = @() }
    if (-not $model) { $r.why = 'no model'; return $r }
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    $log = Join-Path $files 'mochizuki_nr.log'
    Remove-Item -LiteralPath $log, (Join-Path $files $r.bin) -ErrorAction SilentlyContinue
    $all = @{ E2E_W = "$w"; E2E_H = "$h"; E2E_FLAGS = '0' }
    foreach ($k in $baseEnv.Keys) { $all[$k] = $baseEnv[$k] }
    foreach ($k in $vars.Keys) { $all[$k] = $vars[$k] }
    SetEnv $all
    $out = Join-Path $dir "harness_$tag.txt"
    $code = Run 'mz_e2e.exe' ".\k7\MochizukiNrRuntime.dll . test.png $($r.bin) $n 1" $out $(if ($script:first) { 40 } else { 20 })
    ClearEnv $all
    $script:first = $false
    if (Test-Path -LiteralPath $log) { Copy-Item -LiteralPath $log -Destination (Join-Path $dir "log_$tag.txt") }
    $lt = if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log } else { @() }
    $net = $lt | Where-Object { $_ -match '\[mochizuki\] the (RDNA3|FP8) network' } | Select-Object -First 1
    $r.net = if ($net -match 'RDNA3') { 'RDNA3' } elseif ($net -match 'FP8') { 'FP8' } else { '?' }
    $r.imp = @($lt | Where-Object { $_ -match '\[nr\] ACO gfx11 \S+: imported' }).Count
    $r.fbnames = @($lt | Where-Object { $_ -match '\[nr\] ACO gfx11 (\S+): not imported' } | ForEach-Object { $Matches[1] })
    $r.fb = $r.fbnames.Count
    $r.acooff = $lt | Where-Object { $_ -match 'ACO off for the RDNA3 network|ACO asked for, but' } | Select-Object -First 1
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
$sets = @(@{ tag = 'drv'; what = 'driver compiler (the 0.5.9 fix)'; vars = @{ E2E_CREATE_FLAGS = '0' }; bundle = $null },
          @{ tag = 'acofix'; what = 'ACO, the fix''s kernels'; vars = @{ E2E_CREATE_FLAGS = '2' }; bundle = 'fix' },
          @{ tag = 'acocoop'; what = 'ACO, 0.5.8''s attention kernels (cooperative matrices)'; vars = @{ E2E_CREATE_FLAGS = '2' }; bundle = 'coop' })
$good = @{}; $vary = @{}; $msAll = @{}; $impAll = @{}
$step = 0
if ($model) {
    foreach ($set in $sets) {
        ++$step
        UseBundle $set.bundle
        Write-Host "[5/6] Pictures ($step/$($sets.Count)), $($set.what): $($Pics.Count) sizes x $runsPerSize runs..."
        $good[$set.tag] = @(); $vary[$set.tag] = @(); $msAll[$set.tag] = @(); $impAll[$set.tag] = ''
        foreach ($c in $Pics) {
            $name = $c.name
            Write-Host "    $name ..."
            $dir = Join-Path $result "runs\$($set.tag)\$name"
            $scores = @(); $fails = @(); $info = $null; $ms = @()
            for ($k = 1; $k -le $runsPerSize; ++$k) {
                $r = E2E "$($set.tag).$name.r$k" $c.w $c.h $frames $set.vars $dir
                if ($r.net -eq 'FP8' -and -not $forced) { $script:notRdna3 = $true; break }
                if (-not $r.ok) { $fails += "run $k failed: $($r.why)"; continue }
                if (-not $info) { $info = $r }
                if ($r.ms) { $ms += $r.ms }
                $pic = if ($k -eq 1 -and $name -eq 'small_1152x648') { "runs\$($set.tag)\$name" } else { $null }
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
            if ($okAll -and -not $fails.Count) { $good[$set.tag] += $name }
            if ($varies) { $vary[$set.tag] += $name }
            if ($ms.Count) { $msAll[$set.tag] += "$($name -replace '^small_', '') $($ms -join '/')" }
            $acoInfo = ''
            if ($info -and $set.bundle) {
                $acoInfo = "; ACO imported $($info.imp) pipelines, driver for $($info.fb)$(if ($info.fb) { ' (' + ($info.fbnames -join ', ') + ')' })$(if ($info.acooff) { ' | ' + $info.acooff })"
                $impAll[$set.tag] = "imported $($info.imp), driver $($info.fb)"
            }
            $extra = if ($info) { "$($info.net) network, network ms $($ms -join ' / '), first $($info.build)$acoInfo" } else { '' }
            Line ("{0} {1,-7} {2}: PSNR vs RX 9070 XT {3}; runs {4}; {5}" -f $verdict, $set.tag, $name,
                  (($scores | ForEach-Object { $_.text }) -join ' / '), $(if ($diffs.Count) { $diffs -join ', ' } else { '-' }), $extra)
            foreach ($f in $fails) { Line "     $f" }
        }
        if ($script:notRdna3) { Line 'SKIPPED the pictures: this card picked the FP8 network (not an RX 7000)'; break }
    }
}
UseBundle $null

# 6. Summary and the zip.
Write-Host '[6/6] Writing the result...'
$short.Add($probeShort)
foreach ($l in $aloneShort) { $short.Add("Alone | $l") }
if ($script:notRdna3) { $short.Add('This card did not take the RX 7000 network, so the network steps were skipped. Please tell us which card this is.') }
else {
    foreach ($t in $sets | ForEach-Object { $_.tag }) {
        if (-not $good.ContainsKey($t)) { continue }
        $short.Add(("{0,-8}: right (40 dB or more, every run): {1}; changes run to run: {2}; network ms: {3}{4}" -f $t,
                    $(if ($good[$t].Count) { $good[$t] -join ', ' } else { 'none' }), $(if ($vary[$t].Count) { $vary[$t] -join ', ' } else { 'none' }),
                    ($msAll[$t] -join ', '), $(if ($impAll[$t]) { "; $($impAll[$t])" } else { '' })))
    }
}
$head = @("OpenNR RX 7000 diagnostic kit 7 (ACO machine code), $((Get-Date).ToString('yyyy-MM-dd HH:mm')), $([int]((Get-Date) - $t0).TotalMinutes) min", '',
          'IN SHORT') + ($short | ForEach-Object { "  $_" }) + @('', 'DETAILS')
$tail = @('', 'How to read this: PROBE lines are two tiny shaders as ACO machine code (no network). ALONE lines are dispatch 11',
          '(attn) and dispatch 21 (vitattn) alone on the RX 9070 XT''s inputs (attn\, vit\: driver compiler; attn-aco\, vit-aco\: ACO).',
          'Pictures: drv = driver compiler, acofix / acocoop = ACO machine code; GOOD = every run 40 dB or more from the',
          'RX 9070 XT; "identical" runs = no flicker; network ms = the network''s GPU time a frame.',
          'Raw files: aco-probe.txt, attn*\, vit*\, runs\<set>\<size>\ (logs list every pipeline imported), system.txt, console.txt.')
Set-Content -LiteralPath (Join-Path $result 'SUMMARY.txt') -Value ($head + $summary + $tail)
Stop-Transcript | Out-Null
Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
$zip = Join-Path $kit "OpenNR-RX7000-diag7-result-$stamp.zip"
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
