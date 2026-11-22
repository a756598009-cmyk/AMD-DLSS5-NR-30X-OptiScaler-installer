# OpenNR RX 7000 diagnostic kit: run by run-test.bat. Everything goes into ..\result, zipped beside run-test.bat.
# Our own testing only: KIT_FORCE_RDNA3=1 runs the RDNA3 network on any card (MZ_FORCE_RDNA3=1, MZ_RDNA3_CHECK=0),
# KIT_QUICK=1 takes three sizes, one model and fewer runs, KIT_QUIET=1 opens no Explorer at the end.
$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'
$files = $PSScriptRoot
$kit = Split-Path $files -Parent
$result = Join-Path $kit 'result'
$work = Join-Path $files 'work'
$stamp = Get-Date -Format 'yyyy-MM-dd-HHmm'
Set-Location -LiteralPath $files
. (Join-Path $files 'sizes.ps1')
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
if ($quick) { $Sizes = @(@(960, 540), @(1152, 648), @(1920, 1080)); $Models = @($Models[0]) }
$runsPerSize = if ($quick) { 2 } else { 3 }
$frames = 40
$dumpAt = 36

# Runs a tool with its output in a file. Returns the exit code, or 'TIMEOUT' / 'NOSTART'.
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
Write-Host 'OpenNR RX 7000 diagnostic kit. This takes about 45 to 90 minutes. Please do not close this window.'
Write-Host ''

# 1. System and the matrix layout probes (as the first kit).
Write-Host '[1/7] System, Vulkan and matrix layout probe...'
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
Write-Host '[2/7] Downloading the models (about 250 MB, once)...'
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
Remove-Item Env:MZ_FORCE_RDNA3, Env:MZ_RDNA3_CHECK, Env:DLSSNR_MODEL, Env:E2E_FLAGS, Env:E2E_PAN, Env:E2E_CREATE_FLAGS, Env:MZ_STAGE_DUMP, `
    Env:MZ_STAGE_FULL, Env:NR_TCHAIN, Env:NR_ARENA_REUSE -ErrorAction SilentlyContinue
foreach ($f in 'pipeline-rdna3.cache', 'pipeline.cache', 'prewarm', 'build.pending', 'build.failed', 'rdna3-check.txt') {
    Remove-Item -LiteralPath (Join-Path $files "dlssnr-amd\$f") -Recurse -Force -ErrorAction SilentlyContinue
}
$script:first = $true
$script:notRdna3 = $false

# One harness run: the network on test.png at w x h for $frames frames. $data is the folder holding dlssnr-amd
# (relative to files\), $dll 'rel' (the released 0.5.8 DLL) or 'diag' (the same with the stage dump), $vars extra
# environment. Leaves work\<tag>.bin and returns what the run said.
function E2E($tag, $dll, $data, $model, $w, $h, $vars, $dir) {
    $r = @{ ok = $false; why = ''; net = '?'; ms = ''; build = ''; bin = "work\$tag.bin" }
    if (-not $have[$modelFile[$model]]) { $r.why = "no $($modelFile[$model])"; return $r }
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    # The runtime reads dlssnr-amd beside its DLL when it is there (the variants), else beside mz_e2e.exe (files\).
    $data = if (Test-Path -LiteralPath (Join-Path $files "$dll\dlssnr-amd")) { $dll } else { '.' }
    $log = Join-Path $files "$data\mochizuki_nr.log"
    Remove-Item -LiteralPath $log, (Join-Path $files $r.bin) -ErrorAction SilentlyContinue
    $all = @{ E2E_W = "$w"; E2E_H = "$h"; E2E_FLAGS = $(if ($model -eq 'large') { '8192' } else { '0' }) }
    foreach ($k in $baseEnv.Keys) { $all[$k] = $baseEnv[$k] }
    foreach ($k in $vars.Keys) { $all[$k] = $vars[$k] }
    SetEnv $all
    $out = Join-Path $dir "harness_$tag.txt"
    $code = Run 'mz_e2e.exe' ".\$dll\MochizukiNrRuntime.dll $data test.png $($r.bin) $frames 1" $out $(if ($script:first) { 40 } else { 15 })
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
# The output against the RX 9070 XT reference: '<psnr> dB' and the AREA lines in a file.
function Score($bin, $model, $w, $h, $out, $pictures) {
    $ref = "reference\img_${model}_${w}x${h}"
    $a = "$bin $w $h $ref"
    if ($pictures) { $a += " ..\result\$pictures\diff_x16.png ..\result\$pictures\output.jpg" }
    $null = Run 'compare.exe' $a $out 5
    $c = First $out '^COMPARE '
    if ($c -match 'psnr (\S+) dB \(worst area (\S+)\), nan/inf (\d+)') {
        return @{ psnr = $(if ($Matches[1] -eq 'inf') { 999.0 } else { [double]$Matches[1] }); text = "$($Matches[1]) dB"; area = $Matches[2]; bad = [int]$Matches[3] }
    }
    return @{ psnr = -1.0; text = 'no comparison'; area = ''; bad = 0 }
}
function RunsDiff($a, $b, $w, $h, $out) {
    $null = Run 'compare.exe' "--bins $a $b $w $h" $out 5
    $l = First $out '^RUNS '
    if ($l -match '^RUNS identical') { return 'identical' }
    if ($l -match 'psnr (\S+) dB.*differing pixels in (.*)$') { return "differ ($($Matches[1]) dB, pixels $($Matches[2]))" }
    return 'not compared'
}
function L16($model, $w, $h) {
    $l = $Level16["${model}_${w}x${h}"]
    if (-not $l) { return '' }
    '/16 level {0}x{1}, h%8={2} w%8={3}' -f $l[0], $l[1], ($l[0] % 8), ($l[1] % 8)
}

# 3. The released network, 3 runs a size and model, each against the RX 9070 XT.
Write-Host "[3/7] The released 0.5.8 network: $($Sizes.Count) sizes x $($Models.Count) models x $runsPerSize runs..."
$good = @(); $bad = @(); $vary = @()
foreach ($m in $Models) {
    foreach ($s in $Sizes) {
        if ($script:notRdna3) { break }
        $w, $h = $s
        $name = "$($m.name)_${w}x${h}"
        Write-Host "    $name ..."
        $dir = Join-Path $result "runs\$name"
        $scores = @(); $fails = @(); $info = $null
        for ($k = 1; $k -le $runsPerSize; ++$k) {
            $r = E2E "$name.r$k" 'rel' '.' $m.name $w $h @{} $dir
            if ($r.net -eq 'FP8' -and -not $forced) {
                Line "SKIPPED all runs: this card picked the FP8 network (not an RX 7000)"
                $script:notRdna3 = $true
                break
            }
            if (-not $r.ok) { $fails += "run $k failed: $($r.why)"; continue }
            if (-not $info) { $info = $r }
            $pic = if ($k -eq 1 -and ($h -eq 648 -or $h -eq 1080)) { "runs\$name" } else { $null }
            $sc = Score $r.bin $m.name $w $h (Join-Path $dir "compare_r$k.txt") $pic
            $scores += $sc
        }
        if ($script:notRdna3) { break }
        $diffs = @()
        for ($k = 2; $k -le $runsPerSize; ++$k) {
            if ((Test-Path -LiteralPath (Join-Path $files "work\$name.r1.bin")) -and (Test-Path -LiteralPath (Join-Path $files "work\$name.r$k.bin"))) {
                $diffs += "r1-r${k}: " + (RunsDiff "work\$name.r1.bin" "work\$name.r$k.bin" $w $h (Join-Path $dir "runs_r1_r$k.txt"))
            }
        }
        Remove-Item -Path (Join-Path $files "work\$name.r*.bin") -ErrorAction SilentlyContinue
        $varies = @($diffs | Where-Object { $_ -notmatch 'identical$' }).Count -gt 0
        $ps = ($scores | ForEach-Object { $_.text }) -join ' / '
        $okAll = $scores.Count -eq $runsPerSize -and @($scores | Where-Object { $_.psnr -lt 40 -or $_.bad -gt 0 }).Count -eq 0
        $verdict = if ($fails.Count) { 'FAIL' } elseif ($okAll) { 'GOOD' } else { 'BAD ' }
        if ($okAll -and -not $fails.Count) { $good += $name } else { $bad += $name }
        if ($varies) { $vary += $name }
        $extra = if ($info) { "$($info.net) network, $($info.ms), first $($info.build)" } else { '' }
        Line ("{0} {1} ({2}): PSNR vs RX 9070 XT {3}; runs {4}; {5}" -f $verdict, $name, (L16 $m.name $w $h), $ps,
              $(if ($diffs.Count) { $diffs -join ', ' } else { '-' }), $extra)
        foreach ($f in $fails) { Line "     $f" }
    }
}

# 4. Stage dumps: the same runs with the diagnostic DLL, each activation's statistics after the dispatch that made it,
# against the RX 9070 XT's (the ACO network, whose kernel set is the RDNA3 network's), twice for run-to-run changes.
$stageFirst = @{}
if (-not $script:notRdna3) {
    Write-Host "[4/7] Per-stage capture: $($Sizes.Count * $Models.Count) sizes and models, twice..."
    foreach ($m in $Models) {
        foreach ($s in $Sizes) {
            $w, $h = $s
            $name = "$($m.name)_${w}x${h}"
            Write-Host "    $name ..."
            $dir = Join-Path $result "stages\$name"
            $dumps = @()
            foreach ($k in 1, 2) {
                $d = "work\dump_$name.d$k"
                Remove-Item -LiteralPath (Join-Path $files $d) -Recurse -Force -ErrorAction SilentlyContinue
                $r = E2E "$name.d$k" 'diag' '.' $m.name $w $h @{ MZ_STAGE_DUMP = $d; MZ_STAGE_FRAME = "$dumpAt" } $dir
                if (-not $r.ok) { Line "FAIL stages ${name}: run $k failed: $($r.why)"; continue }
                if (Test-Path -LiteralPath (Join-Path $files "$d\stages.bin")) { $dumps += $d }
                if ($k -eq 1) {
                    $sc = Score $r.bin $m.name $w $h (Join-Path $dir 'compare_d1.txt') $null
                    $dumpPsnr = $sc.text
                }
                Remove-Item -LiteralPath (Join-Path $files $r.bin) -ErrorAction SilentlyContinue
            }
            if (-not $dumps.Count) { Line "FAIL stages ${name}: no dump was written"; continue }
            $cmp = Join-Path $dir 'stagecmp.txt'
            $null = Run 'stagecmp.exe' "reference\stages_$name $($dumps[0]) ..\result\stages\$name\table.txt" $cmp 5
            $st = First $cmp '^STAGE first divergence: '
            $cnt = First $cmp '^STAGE \S+ vs '
            $per = First $cmp '^PERSIST '
            $rr = 'one run only'
            if ($dumps.Count -eq 2) {
                $null = Run 'stagecmp.exe' "--runs $($dumps[0]) $($dumps[1])" (Join-Path $dir 'stagecmp_runs.txt') 5
                $rr = (First (Join-Path $dir 'stagecmp_runs.txt') '^RUNS ') -replace '^RUNS ', ''
            }
            Copy-Item -LiteralPath (Join-Path $files "$($dumps[0])\stages.txt"), (Join-Path $files "$($dumps[0])\stages.bin") -Destination $dir
            $fd = ($st -replace '^STAGE first divergence: ', '')
            if ($fd -match '^key (\d+)') { $stageFirst[$name] = [int]$Matches[1] }
            $counts = if ($cnt -match 'compared \((.*)\)') { $Matches[1] } else { '' }
            Line "STAGES ${name}: first divergence: $fd"
            Line "     values: $counts; runs: $rr; dump run PSNR $dumpPsnr; $($per -replace '^PERSIST test: ', 'persistent runs: ')"
            foreach ($d in $dumps) { Remove-Item -LiteralPath (Join-Path $files $d) -Recurse -Force -ErrorAction SilentlyContinue }
        }
    }
}

# 5. The first diverging value of the flicker size whole (raw bytes), for a look at which tokens are wrong.
if (-not $script:notRdna3) {
    Write-Host '[5/7] Raw capture of the first wrong value at 1152x648...'
    foreach ($name in @('small_1152x648')) {
        if (-not $stageFirst.ContainsKey($name)) { Line "INFO full dump ${name}: no diverging value, nothing to capture"; continue }
        $key = $stageFirst[$name]
        $w, $h = 1152, 648
        $d = "work\full_$name"
        $dir = Join-Path $result "full\$name"
        $r = E2E "$name.full" 'diag' '.' 'small' $w $h @{ MZ_STAGE_DUMP = $d; MZ_STAGE_FRAME = "$dumpAt"; MZ_STAGE_FULL = "$key" } $dir
        Remove-Item -LiteralPath (Join-Path $files $r.bin) -ErrorAction SilentlyContinue
        $raw = Get-ChildItem -LiteralPath (Join-Path $files $d) -Filter 'full_*' -ErrorAction SilentlyContinue
        $mb = ($raw | Measure-Object -Property Length -Sum).Sum / 1MB
        if (-not $raw) { Line "FAIL full dump ${name} key ${key}: nothing written ($($r.why))" }
        elseif ($mb -gt 80) { Line ("INFO full dump {0} key {1}: {2:N0} MB, too big to send, left out" -f $name, $key, $mb) }
        else {
            Copy-Item -Path (Join-Path $files "$d\*") -Destination $dir
            Line ("INFO full dump {0} key {1}: {2:N1} MB raw ({3})" -f $name, $key, $mb, (($raw | ForEach-Object { $_.Name }) -join ', '))
        }
        Remove-Item -LiteralPath (Join-Path $files $d) -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# 6. Variants, small model: tile counters on, arena reuse off, edge bodies on (if LLPC compiles them). Run A is the
# released DLL, run B the diagnostic one with its stage dump.
$variantRows = New-Object System.Collections.Generic.List[string]
if (-not $script:notRdna3) {
    Write-Host '[6/7] Variants (tile counters, no arena reuse, edge bodies)...'
    $variants = @(
        @{ name = 'tchain'; what = 'NR_TCHAIN=1 (tile counters on)'; vars = @{ NR_TCHAIN = '1' }; data = '.' },
        @{ name = 'noreuse'; what = 'NR_ARENA_REUSE=0 (every value its own memory)'; vars = @{ NR_ARENA_REUSE = '0' }; data = '.' },
        @{ name = 'edge7'; what = 'NR_EDGE_BODIES=1 on all seven persistent runs (mauri870 original)'; vars = @{}; data = 'v_edge7' },
        @{ name = 'edge6'; what = 'NR_EDGE_BODIES=1 on six persistent runs, fswinp256 as released'; vars = @{}; data = 'v_edge6' })
    foreach ($v in $variants) {
        $vdir = Join-Path $result "variants\$($v.name)"
        New-Item -ItemType Directory -Path $vdir -Force | Out-Null
        if ($v.data -ne '.') {
            # Its own data folder: the released shaders with the variant's .spv over them, the models linked.
            $pc = Join-Path $vdir 'pipecheck.txt'
            # edge6 is edge7's files without fswinp256 (byte for byte the same): edge7's check answers for them.
            $code = 0
            $skip = if ($v.name -eq 'edge6') { 'g_fswinp256.spv' } else { '' }
            $prev = Join-Path $result 'variants\edge7\pipecheck.txt'
            if ($skip -and (Test-Path -LiteralPath $prev)) {
                Get-Content -LiteralPath $prev | Where-Object { $_ -notmatch [regex]::Escape($skip) } | Set-Content -LiteralPath $pc
            } else { $code = Run 'pipecheck.exe' 'variants\edge7' $pc 60 }
            $pipes = @(Get-Content -LiteralPath $pc -ErrorAction SilentlyContinue | Where-Object { $_ -match '^PIPE ' })
            $notOk = @($pipes | Where-Object { $_ -notmatch ' OK ' })
            if (-not $pipes.Count -or $notOk.Count) {
                Line ("VARIANT {0}: not run, LLPC does not compile it here: {1}" -f $v.name,
                      $(if ($pipes.Count) { ($notOk | ForEach-Object { ($_ -replace '^PIPE \S*\\', '') }) -join '; ' } else { "pipecheck exit $(Hex $code)" }))
                continue
            }
            # The runtime takes dlssnr-amd beside its DLL: rel\ and diag\ each get the DLL and a dlssnr-amd.
            Remove-Item -LiteralPath (Join-Path $files $v.data) -Recurse -Force -ErrorAction SilentlyContinue
            foreach ($which in 'rel', 'diag') {
                $root = Join-Path $files "$($v.data)\$which"
                New-Item -ItemType Directory -Path "$root\dlssnr-amd\shaders" -Force | Out-Null
                Copy-Item -LiteralPath (Join-Path $files "$which\MochizukiNrRuntime.dll") -Destination $root
                Copy-Item -LiteralPath (Join-Path $files 'dlssnr-amd\shaders-rdna3') -Destination "$root\dlssnr-amd\shaders-rdna3" -Recurse
                Copy-Item -LiteralPath (Join-Path $files 'dlssnr-amd\shaders\renodx') -Destination "$root\dlssnr-amd\shaders\renodx" -Recurse
                Get-ChildItem -LiteralPath (Join-Path $files 'variants\edge7') -Filter *.spv | Where-Object { $_.Name -ne $skip } |
                    Copy-Item -Destination "$root\dlssnr-amd\shaders-rdna3" -Force
                foreach ($mf in $modelFile.Values) {
                    $src = Join-Path $files "dlssnr-amd\$mf"
                    if (-not (Test-Path -LiteralPath $src)) { continue }
                    try { New-Item -ItemType HardLink -Path "$root\dlssnr-amd\$mf" -Target $src -ErrorAction Stop | Out-Null }
                    catch { Copy-Item -LiteralPath $src -Destination "$root\dlssnr-amd\$mf" }
                }
            }
            Line ("INFO variant {0}: {1} of {1} changed pipelines compiled" -f $v.name, $pipes.Count)
        }
        Line "VARIANT $($v.name): $($v.what)"
        foreach ($s in $Sizes) {
            $w, $h = $s
            $name = "small_${w}x${h}"
            Write-Host "    $($v.name) $name ..."
            $dir = Join-Path $vdir $name
            $a = E2E "$($v.name).$name.a" $(if ($v.data -eq '.') { 'rel' } else { "$($v.data)\rel" }) '.' 'small' $w $h $v.vars $dir
            $d = "work\vdump_$($v.name)_$name"
            $bv = @{ MZ_STAGE_DUMP = $d; MZ_STAGE_FRAME = "$dumpAt" }
            foreach ($k in $v.vars.Keys) { $bv[$k] = $v.vars[$k] }
            $b = E2E "$($v.name).$name.b" $(if ($v.data -eq '.') { 'diag' } else { "$($v.data)\diag" }) '.' 'small' $w $h $bv $dir
            $pa = if ($a.ok) { (Score $a.bin 'small' $w $h (Join-Path $dir 'compare_a.txt') $null).text } else { "failed ($($a.why))" }
            $pb = if ($b.ok) { (Score $b.bin 'small' $w $h (Join-Path $dir 'compare_b.txt') $null).text } else { "failed ($($b.why))" }
            $rr = if ($a.ok -and $b.ok) { RunsDiff $a.bin $b.bin $w $h (Join-Path $dir 'runs_a_b.txt') } else { '-' }
            $fd = '-'
            if ($b.ok -and (Test-Path -LiteralPath (Join-Path $files "$d\stages.bin"))) {
                $cmp = Join-Path $dir 'stagecmp.txt'
                $null = Run 'stagecmp.exe' "reference\stages_$name $d ..\result\variants\$($v.name)\$name\table.txt" $cmp 5
                $fd = (First $cmp '^STAGE first divergence: ') -replace '^STAGE first divergence: ', ''
                if ($fd -match '^(key \d+ \(block \d+ layer \d+, [^,]+).*test dispatch \d+ (\S+);.*(C=256 persistent run \(fswinp256\): \S+)') {
                    $fd = "$($Matches[1]) by $($Matches[2]); $($Matches[3])"
                }
            }
            Remove-Item -LiteralPath (Join-Path $files $a.bin), (Join-Path $files $b.bin) -ErrorAction SilentlyContinue
            Remove-Item -LiteralPath (Join-Path $files $d) -Recurse -Force -ErrorAction SilentlyContinue
            Line ("     {0} ({1}): PSNR A {2}, B {3}; A-B {4}; first divergence {5}" -f $name, (L16 'small' $w $h), $pa, $pb, $rr, $fd)
        }
        if ($v.data -ne '.') { Remove-Item -LiteralPath (Join-Path $files $v.data) -Recurse -Force -ErrorAction SilentlyContinue }
    }
}

# 7. Summary and the zip.
Write-Host '[7/7] Writing the result...'
if ($script:notRdna3) { $short.Add('This card did not take the RX 7000 network, so nothing was tested. Please tell us which card this is.') }
else {
    $short.Add("Sizes that look right (all runs within 40 dB of an RX 9070 XT): $(if ($good.Count) { $good -join ', ' } else { 'none' })")
    $short.Add("Sizes that look wrong or failed: $(if ($bad.Count) { $bad -join ', ' } else { 'none' })")
    $short.Add("Sizes whose picture changes from run to run: $(if ($vary.Count) { $vary -join ', ' } else { 'none' })")
}
$head = @("OpenNR RX 7000 diagnostic kit 2 (release 0.5.8), $((Get-Date).ToString('yyyy-MM-dd HH:mm')), $([int]((Get-Date) - $t0).TotalMinutes) min", '',
          'IN SHORT') + ($short | ForEach-Object { "  $_" }) + @('', 'DETAILS')
$tail = @('', 'How to read this: GOOD = every run matches the RX 9070 XT reference (PSNR 40 dB or more), BAD = it does not.',
          '"runs identical" means the same picture every time; "differ" means it changes from run to run (flicker).',
          'STAGES lines name the first network layer whose output differs from the RX 9070 XT, and the shader that made it.',
          'Raw files: system.txt, runs\ (harness, log, compare per run), stages\ (per-stage tables), variants\, full\, console.txt.')
Set-Content -LiteralPath (Join-Path $result 'SUMMARY.txt') -Value ($head + $summary + $tail)
Stop-Transcript | Out-Null
Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
$zip = Join-Path $kit "OpenNR-RX7000-diag-result-$stamp.zip"
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
