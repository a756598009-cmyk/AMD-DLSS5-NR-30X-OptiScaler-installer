# OpenNR RX 7000 test: run by run-test.bat. Writes everything into ..\result and zips it beside run-test.bat.
# Our own testing only: KIT_FORCE_RDNA3=1 passes MZ_FORCE_RDNA3=1 to the network runs, KIT_QUIET=1 opens no Explorer.
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
$t0 = Get-Date
function Line($s) { $summary.Add($s); Write-Host "  $s" }

# Runs a tool with its output in a file. Returns the exit code, or 'TIMEOUT'.
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

Write-Host ''
Write-Host 'OpenNR RX 7000 test. This takes 10 to 30 minutes. Please do not close this window.'
Write-Host ''

# 1 + 2. System, Vulkan and the fragment layout checks.
Write-Host '[1/5] System, Vulkan and matrix layout probe...'
$sys = Join-Path $result 'system.txt'
$code = Run 'vkprobe.exe' 'probe' $sys 10
$sysText = if (Test-Path -LiteralPath $sys) { Get-Content -LiteralPath $sys } else { @() }
$sysText | Where-Object { $_ -match '^(windows:|amd software:|dxgi adapter .*vendor 1002)' } | ForEach-Object { Line "INFO $_" }
$amd = $sysText | Where-Object { $_ -match '^vulkan device \d+: .*vendor 1002' } | Select-Object -First 1
if ($amd) { Line "INFO $amd" }
$pick = $sysText | Where-Object { $_ -match 'runtime would pick' } | Select-Object -First 1
if ($pick) { Line ("INFO " + $pick.Trim()) }
if ($code -ne 0) { Line "FAIL vkprobe exit $(Hex $code)" }
$checks = $sysText | Where-Object { $_ -match '^CHECK ' }
foreach ($c in $checks) {
    $parts = $c -split ' ', 4
    Line ("{0} layout {1}: {2}" -f $parts[2], $parts[1], $parts[3])
}
if (-not $checks) { Line 'FAIL layout: no check ran' }

# 3. Every pipeline of the RDNA3 path, each compiled in a process of its own.
Write-Host '[2/5] Compiling the RDNA3 shaders one by one (can take several minutes)...'
$pc = Join-Path $result 'pipecheck.txt'
$code = Run 'pipecheck.exe' '"dlssnr-amd\shaders-rdna3" "dlssnr-amd\shaders-rdna3\runtime" "dlssnr-amd\shaders-rdna3\temporal" "dlssnr-amd\shaders\renodx"' $pc 90
$pipes = @(Get-Content -LiteralPath $pc -ErrorAction SilentlyContinue | Where-Object { $_ -match '^PIPE ' })
$ok = @($pipes | Where-Object { $_ -match ' OK ' })
$notOk = @($pipes | Where-Object { $_ -notmatch ' OK ' })
$ms = 0; $ok | ForEach-Object { if ($_ -match ' OK (\d+) ms') { $ms += [int]$Matches[1] } }
$slow = ($ok | Sort-Object { [int](($_ -split ' ')[3]) } -Descending | Select-Object -First 3 | ForEach-Object { $f = $_ -split ' '; (Split-Path $f[1] -Leaf) + ' ' + $f[3] + ' ms' }) -join ', '
if ($code -eq 'TIMEOUT') { Line "FAIL pipecheck: the whole run timed out after 90 min" }
if ($pipes.Count -and -not $notOk.Count) { Line ("PASS pipecheck: {0} of {0} pipelines compiled, {1} s in all; slowest {2}" -f $pipes.Count, [int]($ms / 1000), $slow) }
elseif ($pipes.Count) {
    Line ("FAIL pipecheck: {0} of {1} pipelines compiled; slowest {2}" -f $ok.Count, $pipes.Count, $slow)
    $notOk | ForEach-Object { Line "     $_" }
}
else { Line "FAIL pipecheck: nothing ran (exit $(Hex $code))" }

# 4. The models, from the same place the OpenNR installer takes them.
Write-Host '[3/5] Downloading the models (about 250 MB, once)...'
$models = @(
    @{ name = 'dlssnr.bin'; size = 147756560; sha = '2b41c888cf4155b8958c665ba64018ab0bd25c85fc71a2b6db86d0d04d1f7fbd';
       url = 'https://huggingface.co/datasets/zmodelerlover/amd-nr/resolve/main/mochizuki/model-1/dlssnr.bin' },
    @{ name = 'dlssnr-small.bin'; size = 113917248; sha = '7e966e2639dbec6596aa0bac3b8761137d2c40ca8cb4aca30df7ca10043c6e33';
       url = 'https://huggingface.co/datasets/zmodelerlover/amd-nr/resolve/main/mochizuki/model-small-1/dlssnr-small.bin' })
try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12 } catch {}
$have = @{}
foreach ($m in $models) {
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
            $h = Sha256 $dst
            if ($h -eq $m.sha) { $verified = $true } else { Remove-Item -LiteralPath $dst -Force }
        }
    }
    $have[$m.name] = $verified
    if ($verified) { Line "PASS model $($m.name): SHA-256 verified" }
    else { Line "FAIL model $($m.name): could not be downloaded and verified" }
}

# 5 + 6. The network on the test image, each run its own process, against the RX 9070 XT references.
Write-Host '[4/5] Running the network on the test image (6 runs)...'
Remove-Item Env:MZ_FORCE_RDNA3, Env:DLSSNR_MODEL, Env:E2E_FLAGS, Env:E2E_PAN -ErrorAction SilentlyContinue
if ($env:KIT_FORCE_RDNA3 -eq '1') { $env:MZ_FORCE_RDNA3 = '1'; Line 'INFO KIT_FORCE_RDNA3=1: the RDNA3 network is forced (MZ_FORCE_RDNA3=1)' }
foreach ($f in 'pipeline-rdna3.cache', 'pipeline.cache', 'prewarm', 'build.pending', 'build.failed') {
    Remove-Item -LiteralPath (Join-Path $files "dlssnr-amd\$f") -Recurse -Force -ErrorAction SilentlyContinue
}
$sizes = @(@(1280, 720), @(1920, 1080), @(2560, 1440))
$runs = @()
foreach ($model in 'new', 'old') { foreach ($s in $sizes) { $runs += , @($model, $s[0], $s[1]) } }
$first = $true
$fp8Only = $false
foreach ($r in $runs) {
    $model, $w, $h = $r
    $name = "${model}_${h}p"
    $file = if ($model -eq 'new') { 'dlssnr-small.bin' } else { 'dlssnr.bin' }
    if (-not $have[$file]) { Line "SKIPPED run ${name}: no $file"; continue }
    if ($fp8Only) { Line "SKIPPED run ${name}: the FP8 network was picked (see run new_720p)"; continue }
    Write-Host "    $name ..."
    $dir = Join-Path $result "runs\$name"
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    $log = Join-Path $files 'mochizuki_nr.log'
    Remove-Item -LiteralPath $log -ErrorAction SilentlyContinue
    $env:E2E_W = "$w"; $env:E2E_H = "$h"
    $env:E2E_FLAGS = if ($model -eq 'old') { '8192' } else { '0' }
    # Paths relative to files\: the tools take ANSI arguments, the kit's folder name may not be.
    $binRel = "work\$name.bin"
    $bin = Join-Path $files $binRel
    $out = Join-Path $dir 'harness.txt'
    $code = Run 'mz_e2e.exe' ".\MochizukiNrRuntime.dll . test.png $binRel 40 1" $out $(if ($first) { 30 } else { 15 })
    $first = $false
    if (Test-Path -LiteralPath $log) { Copy-Item -LiteralPath $log -Destination $dir }
    $lt = if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log } else { @() }
    $ht = Get-Content -LiteralPath $out -ErrorAction SilentlyContinue
    $net = $lt | Where-Object { $_ -match '\[mochizuki\] the (RDNA3|FP8) network' } | Select-Object -First 1
    $netName = if ($net -match 'RDNA3') { 'RDNA3' } elseif ($net -match 'FP8') { 'FP8' } else { '?' }
    $used = $lt | Where-Object { $_ -match '\[nr\] .*(small|original) model' } | Select-Object -First 1
    $usedName = if ($used -match 'original model') { 'old' } elseif ($used -match 'small') { 'new' } else { '?' }
    $status = $ht | Where-Object { $_ -match '^status: ' } | Select-Object -First 1
    $netMs = if ($status -match 'network ([\d.]+) ms \(p95 ([\d.]+)\)') { "network $($Matches[1]) ms (p95 $($Matches[2]))" } else { 'no network time' }
    $fm = $ht | Where-Object { $_ -match '^frame ms: ' } | Select-Object -First 1
    $wall = if ($fm -match 'median=([\d.]+)') { "frame $($Matches[1]) ms" } else { '' }
    $ready = $ht | Where-Object { $_ -match '^ready after ' } | Select-Object -First 1
    $build = if ($ready -match 'ready after ([\d.]+) s') { "built in $($Matches[1]) s" } else { '' }
    if ($netName -eq 'FP8' -and $env:KIT_FORCE_RDNA3 -ne '1') {
        Line "SKIPPED run ${name}: this card picked the FP8 network (not an RX 7000); the kit has only the RDNA3 network"
        $fp8Only = $true
        continue
    }
    if ($code -ne 0) {
        $why = ($ht | Where-Object { $_ -match 'FAIL' } | Select-Object -First 1)
        if ($code -eq 'TIMEOUT') { $why = 'timed out' }
        elseif (-not $why) { $why = "exit $(Hex $code)" }
        $lastErr = $lt | Where-Object { $_ -match 'error|fail|unsupported|exception' } | Select-Object -Last 1
        Line "FAIL run ${name}: $netName network, $why"
        if ($lastErr) { Line "     log: $lastErr" }
        continue
    }
    $cmpOut = Join-Path $dir 'compare.txt'
    $cmpArgs = "$binRel $w $h reference\ref_${model}_${h}p.jxr ..\result\runs\$name\diff_x16.png"
    if ($h -eq 1080) { $cmpArgs += " ..\result\runs\$name\output.jpg" }
    $null = Run 'compare.exe' $cmpArgs $cmpOut 5
    $cmp = Get-Content -LiteralPath $cmpOut -ErrorAction SilentlyContinue | Where-Object { $_ -match '^COMPARE ' } | Select-Object -First 1
    $verdict = 'FAIL'
    $numbers = 'no comparison'
    if ($cmp -match 'psnr (\S+) dB, mean\|d\| ([\d.]+), max\|d\| ([\d.]+), corr (\S+), identical ([\d.]+)%, nan/inf (\d+), area (\S+)') {
        $psnr = if ($Matches[1] -eq 'inf') { 999 } else { [double]$Matches[1] }
        if ($psnr -ge 40 -and [int]$Matches[6] -eq 0) { $verdict = 'PASS' }
        $numbers = "PSNR $($Matches[1]) dB, mean|d| $($Matches[2]), max|d| $($Matches[3]), corr $($Matches[4]), $($Matches[5])% identical, NaN/Inf $($Matches[6]) (area $($Matches[7]))"
    } elseif ($cmp) { $numbers = $cmp }
    $modelNote = if ($usedName -ne $model) { " (asked for the $model model, got $usedName)" } else { '' }
    Line "$verdict run ${name}: $netName network$modelNote, $build, $netMs, $wall; vs RX 9070 XT: $numbers"
    Remove-Item -LiteralPath $bin -ErrorAction SilentlyContinue
}
Remove-Item Env:E2E_W, Env:E2E_H, Env:E2E_FLAGS, Env:MZ_FORCE_RDNA3 -ErrorAction SilentlyContinue

# 7. Summary and the zip.
Write-Host '[5/5] Writing the result...'
$head = @("OpenNR RX 7000 test kit 1, $((Get-Date).ToString('yyyy-MM-dd HH:mm')), $([int]((Get-Date) - $t0).TotalMinutes) min", '')
$tail = @('', 'Raw outputs: system.txt (probe), pipecheck.txt, runs\<model>_<size>\ (harness.txt, mochizuki_nr.log,',
          'compare.txt, diff_x16.png; at 1080p output.jpg), console.txt.')
Set-Content -LiteralPath (Join-Path $result 'SUMMARY.txt') -Value ($head + $summary + $tail)
Stop-Transcript | Out-Null
Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
$zip = Join-Path $kit "OpenNR-RX7000-result-$stamp.zip"
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
