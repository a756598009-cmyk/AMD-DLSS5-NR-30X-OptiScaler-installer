# OpenNR RX 6000 confirmation kit (mz-gfx11-hybrid): run by run-test.bat. Everything goes into ..\result, zipped
# beside run-test.bat. Kit v8 without the micro-benchmark and the dot2 probes, on the next version's runtime and sets:
# FP16 (unchanged) and INT8 (MZ_RDNA2_INT8=1, the default) as the next version ships it (the C = 32 bodies' attention
# Q.K in int8, kit v8's int8qk), with its pipeline check, and int8old (0.6.9's INT8 set, with the
# same runtime), swapped into shaders-rdna2-int8 for its runs (UsePlan), for the speed comparison.
# Our own testing only: KIT_FORCE_RDNA2=1 runs the RDNA2 network on any card (MZ_FORCE_RDNA2=1), KIT_QUICK=1 (cases.ps1),
# KIT_QUIET=1 opens no Explorer at the end, KIT_BLOCK=<file name>[ <first argument>] acts as if App Control refused it
# ('nrkit.exe' every tool, 'nrkit.exe probe' the probe only).
$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'
$files = $PSScriptRoot
$kit = Split-Path $files -Parent
$result = Join-Path $kit 'result'
$work = Join-Path $files 'work'
$stamp = Get-Date -Format 'yyyy-MM-dd-HHmm'
Set-Location -LiteralPath $files
. (Join-Path $files 'cases.ps1')
foreach ($d in $result, $work) {
    if (Test-Path -LiteralPath $d) { Remove-Item -LiteralPath $d -Recurse -Force }
    New-Item -ItemType Directory -Path $d | Out-Null
}
Start-Transcript -LiteralPath (Join-Path $result 'console.txt') | Out-Null
$summary = New-Object System.Collections.Generic.List[string]
$short = New-Object System.Collections.Generic.List[string]
$t0 = Get-Date
function Line($s) { $summary.Add($s); Write-Host "  $s" }
$forced = $env:KIT_FORCE_RDNA2 -eq '1'
$Sets = @(@{ name = 'fp16'; dir = 'shaders-rdna2'; vars = @{}; check = 'rdna2-check' },
          @{ name = 'int8'; dir = 'shaders-rdna2-int8'; vars = $Int8; check = 'rdna2-int8-check' })
if (Select-String -LiteralPath (Join-Path $files 'kit.sha256') -Pattern 'shaders-rdna2-int8-old\\' -Quiet) {
    $Sets += @{ name = 'int8old'; dir = 'shaders-rdna2-int8-old'; vars = $Int8; check = $null; qk = $true }
}
# The runtime reads the INT8 network from shaders-rdna2-int8 only: int8old is renamed into it for its runs.
$script:plan = 'new'
function UsePlan($s) {
    if (-not $s.vars.ContainsKey('MZ_RDNA2_INT8')) { return }
    $want = if ($s.qk) { 'old' } else { 'new' }
    if ($script:plan -eq $want) { return }
    $d = Join-Path $files 'dlssnr-amd'
    Rename-Item -LiteralPath "$d\shaders-rdna2-int8" -NewName "shaders-rdna2-int8-$($script:plan)"
    Rename-Item -LiteralPath "$d\shaders-rdna2-int8-$want" -NewName 'shaders-rdna2-int8'
    $script:plan = $want
}

# Windows App Control (Smart App Control, App Control for Business) refusing a file is Win32 error 4551 (AppLocker
# and software restriction policies: 1260). Windows PowerShell passes on only the message, in the PC's language.
$blockText = @(4551, 1260 | ForEach-Object { (New-Object ComponentModel.Win32Exception $_).Message })
$blocked = New-Object System.Collections.Generic.List[string]
function Blocked($name) { if (-not $blocked.Contains($name)) { $blocked.Add($name); Line "BLOCKED BY WINDOWS APP CONTROL $name" } }
# Starts a tool with its output in a file: the process, or 'BLOCKED' / 'NOSTART'.
function Launch($exe, $arguments, $out) {
    $path = if ([IO.Path]::IsPathRooted($exe)) { $exe } else { Join-Path $files $exe }
    try {
        if ($env:KIT_BLOCK -and "$(Split-Path $path -Leaf) $arguments".StartsWith($env:KIT_BLOCK)) { throw (New-Object ComponentModel.Win32Exception 4551) }
        Start-Process -FilePath $path -ArgumentList $arguments -WorkingDirectory $files -NoNewWindow `
            -PassThru -RedirectStandardOutput $out -RedirectStandardError "$out.err" -ErrorAction Stop
    } catch {
        $why = $_.Exception.Message
        Set-Content -LiteralPath $out -Value "$exe did not start: $why"
        if (@($blockText | Where-Object { $why.Contains($_) }).Count -or $why -match '0x800711C7') { Blocked (Split-Path $path -Leaf); 'BLOCKED' }
        else { 'NOSTART' }
    }
}
# Runs a tool with its output in a file. Returns the exit code, or 'TIMEOUT' / 'NOSTART' / 'BLOCKED'.
function Run($exe, $arguments, $out, $minutes) {
    $p = Launch $exe $arguments $out
    if ($p -is [string]) { return $p }
    $null = $p.Handle
    if (-not $p.WaitForExit([int]($minutes * 60000))) {
        # The harness's own children (the runtime's pipeline check in rundll32) go with it.
        & taskkill.exe /T /F /PID $p.Id 2>&1 | Out-Null
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
function Minutes() { [int]((Get-Date) - $t0).TotalMinutes }
function Avg($xs) { $v = @($xs | Where-Object { $_ -ne $null }); if ($v.Count) { ($v | Measure-Object -Average).Average } else { -1.0 } }

Write-Host ''
Write-Host 'OpenNR RX 6000 confirmation kit. This takes about 15 to 20 minutes. Please do not close this window.'
Write-Host ''

# 0. The kit's binaries out of kit.cab (LZX, Windows' own expand.exe, which keeps the zip under 10 MB): the tools, the
# runtime, the shader sets, the references and the test image. The files the shader sets share are in it once
# (same.txt: '<copy>|<source>') and copied here; every file is checked against kit.sha256.
Write-Host '[0/9] Unpacking...'
$Image = 'test.jxr'
$out = Join-Path $result 'unpack.txt'
foreach ($p in 'new', 'old') { Remove-Item -LiteralPath (Join-Path $files "dlssnr-amd\shaders-rdna2-int8-$p") -Recurse -Force -ErrorAction SilentlyContinue }
$code = Run "$env:SystemRoot\System32\expand.exe" "`"$files\kit.cab`" -F:* `"$files`"" $out 5
$n = 0
foreach ($l in @(Get-Content -LiteralPath (Join-Path $files 'same.txt'))) {
    $cp, $sr = $l -split '\|'
    $src = Join-Path $files "dlssnr-amd\$sr"; $dst = Join-Path $files "dlssnr-amd\$cp"
    if (Test-Path -LiteralPath $src) { New-Item -ItemType Directory -Force -Path (Split-Path $dst) | Out-Null; Copy-Item -LiteralPath $src -Destination $dst; ++$n }
}
$bad = @()
foreach ($l in @(Get-Content -LiteralPath (Join-Path $files 'kit.sha256'))) {
    if ($l -notmatch '^([0-9a-f]{64})  (.+)$') { continue }
    $p = Join-Path $files $Matches[2]
    if (-not (Test-Path -LiteralPath $p) -or (Sha256 $p) -ne $Matches[1]) { $bad += $Matches[2] }
}
if ($code -eq 0 -and -not $bad.Count) { Line "PASS kit unpacked: $(@(Get-ChildItem -LiteralPath $files -Recurse -Filter *.spv).Count) shader modules ($n shared), every file SHA-256 verified" }
else {
    Line "FAIL kit unpack: expand exit $(Hex $code), $($bad.Count) files missing or wrong$(if ($bad.Count) { ': ' + (($bad | Select-Object -First 5) -join ', ') })"
    Line 'If an antivirus removed a file, allow the folder and run the test again.'
}
Get-ChildItem -LiteralPath $files -Recurse -Include *.exe, *.dll | Unblock-File -ErrorAction SilentlyContinue
$unpacked = $code -eq 0 -and -not $bad.Count

# 1. System and Vulkan.
Write-Host '[1/9] System and Vulkan...'
$sys = Join-Path $result 'system.txt'
$code = Run 'nrkit.exe' 'probe --report' $sys 3
$sysText = @(if (Test-Path -LiteralPath $sys) { Get-Content -LiteralPath $sys })
$sysText | Where-Object { $_ -match '^(windows:|amd software:|cpu threads)' } | ForEach-Object { Line "INFO $_" }
if ($code -ne 0 -and $code -ne 'BLOCKED') { Line "FAIL the probe (nrkit.exe probe): exit $(Hex $code)" }
# Smart App Control as the registry has it (0 off, 1 on, 2 evaluation), and the cards as Windows lists them.
$sac = try { (Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy' -ErrorAction Stop).VerifiedAndReputablePolicyState } catch { $null }
$sacText = switch ("$sac") { '0' { 'Off' } '1' { 'On' } '2' { 'Evaluation' } default { 'unknown' } }
Line "INFO Smart App Control (registry): $sacText"
$winCards = @(try { Get-CimInstance Win32_VideoController -ErrorAction Stop } catch { })
foreach ($v in $winCards) { Line "INFO windows lists: $($v.Name), $($v.PNPDeviceID -replace '^(PCI\\VEN_\w+&DEV_\w+).*$', '$1'), driver $($v.DriverVersion)" }
$dev = @(); $in = $false
foreach ($l in $sysText) {
    if ($l -match '^vulkan device \d+: ') { if ($dev.Count) { break }; $in = $l -match 'vendor 1002' }
    if ($in) { $dev += $l }
}
$pick = ''; $card = ''
if (-not $dev.Count) {
    # No answer from the probe: the card from Windows' list and the runtime's RDNA2 device ids (vkprobe.cpp). The
    # network tests go on unless the card is known not to get the RX 6000 network; the first start shows what it picks.
    $amd = $winCards | Where-Object { $_.PNPDeviceID -match 'VEN_1002&DEV_([0-9A-F]{4})' } | Select-Object -First 1
    if ($amd -and $amd.PNPDeviceID -match 'DEV_([0-9A-F]{4})') {
        $id = [Convert]::ToInt32($Matches[1], 16)
        $rdna2 = ($id -ge 0x73A0 -and $id -le 0x73FF) -or ($id -ge 0x7420 -and $id -le 0x743F) -or $id -in 0x163F, 0x1681, 0x164E, 0x1506
        $card = "$($amd.Name) (from Windows' list, device $($Matches[1]))"
        if ($rdna2) { $pick = 'RDNA2 network (an RDNA2 device id, from Windows)' }
        Line "INFO card from Windows' list: $card$(if ($rdna2) { ', an RDNA2 device id' } else { ', not an RDNA2 device id: the first start decides' })"
    } elseif ($code -eq 0) { Line 'FAIL no AMD Vulkan device'; $pick = 'none (no AMD Vulkan device)' }
    else { Line 'WARN the card is unknown (no probe, no AMD card in Windows'' list): the first start decides' }
}
else {
    Line "INFO $($dev[0])"
    foreach ($pat in '^\s+api ', '^\s+subgroup size', '^\s+max shared memory', '^\s+integer dot product', '^\s+runtime would pick') {
        $l = $dev | Where-Object { $_ -match $pat } | Select-Object -First 1
        if ($l) { Line ("INFO   " + $l.Trim()) }
    }
    $p = $dev | Where-Object { $_ -match 'runtime would pick: (.*)$' } | Select-Object -First 1
    if ($p -match 'runtime would pick: (.*)$') { $pick = $Matches[1] }
}
$notRdna2 = -not $unpacked -or (-not $forced -and $pick -and $pick -notmatch '^RDNA2 network')
if ($forced) { Line 'INFO KIT_FORCE_RDNA2=1: the RDNA2 network is forced on this card (MZ_FORCE_RDNA2=1)' }
elseif ($notRdna2) { Line "SKIPPED the network tests: this card would not get the RX 6000 network ($pick)" }

# 2. The models, from the same place the OpenNR installer takes them.
$have = @{}
$modelFile = @{ small = 'dlssnr-small.bin'; large = 'dlssnr.bin' }
if (-not $notRdna2) {
    Write-Host '[2/9] Downloading the models (about 250 MB, once)...'
    $downloads = @(
        @{ name = 'dlssnr-small.bin'; size = 113917248; sha = '7e966e2639dbec6596aa0bac3b8761137d2c40ca8cb4aca30df7ca10043c6e33';
           url = 'https://huggingface.co/datasets/zmodelerlover/amd-nr/resolve/main/mochizuki/model-small-1/dlssnr-small.bin' },
        @{ name = 'dlssnr.bin'; size = 147756560; sha = '2b41c888cf4155b8958c665ba64018ab0bd25c85fc71a2b6db86d0d04d1f7fbd';
           url = 'https://huggingface.co/datasets/zmodelerlover/amd-nr/resolve/main/mochizuki/model-1/dlssnr.bin' })
    try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12 } catch {}
    New-Item -ItemType Directory -Force -Path (Join-Path $files 'dlssnr-amd') | Out-Null
    foreach ($m in $downloads) {
        $dst = Join-Path $files "dlssnr-amd\$($m.name)"
        $verified = $false
        for ($try = 0; $try -lt 3 -and -not $verified; ++$try) {
            if (-not (Test-Path -LiteralPath $dst) -or (Get-Item -LiteralPath $dst).Length -ne $m.size) {
                Remove-Item -LiteralPath $dst -ErrorAction SilentlyContinue
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
        if ($verified) { Line "PASS model $($m.name): SHA-256 verified" }
        else { Line "FAIL model $($m.name): could not be downloaded and verified" }
    }
}

# Parses pipecheck output into one row a file: @{ file; state; ms; text }.
function PipeRows($path) {
    $rows = @()
    foreach ($l in @(if (Test-Path -LiteralPath $path) { Get-Content -LiteralPath $path })) {
        if ($l -notmatch '^PIPE (\S+) (OK|FAIL|CRASH|TIMEOUT|NOTRUN)\s*(.*)$') { continue }
        $file = $Matches[1]; $state = $Matches[2]; $rest = $Matches[3]
        $ms = if ($rest -match '^(\d+) ms') { [int]$Matches[1] } elseif ($rest -match 'after (\d+) ms') { [int]$Matches[1] } `
              elseif ($rest -match 'after (\d+) s') { 1000 * [int]$Matches[1] } else { 0 }
        $rows += @{ file = ($file -replace '^.*dlssnr-amd\\', ''); state = $state; ms = $ms; text = $rest }
    }
    $rows
}

# 4. Every pipeline of every set compiled once, each in its own process with a time limit, with the driver's own
# statistics and disassembly of each (PIPECHECK_ISA, VK_AMD_shader_info: result\isa\<set>), then the dot2 spelling
# probes the same way (diagnostic kit v8). The sets are in their own folders here (int8old as unpacked).
$pipesOk = $false; $pipeBlocked = $false
if ($unpacked) {
    Write-Host '[4/9] Compiling every shader of every form once, with what the driver made of it (a few minutes)...'
    $env:PIPECHECK_COOP = '0'; $env:PIPECHECK_TIMEOUT_S = '300'; $env:PIPECHECK_BUDGET_S = '1500'
    $pc = Join-Path $result 'pipecheck.txt'
    Remove-Item -LiteralPath $pc -ErrorAction SilentlyContinue
    $code = 0
    $what = @($Sets | ForEach-Object { @{ name = $_.name; dirs = @("dlssnr-amd\$($_.dir)") } })
    $what[0].dirs += 'dlssnr-amd\shaders\renodx'
    if (Test-Path -LiteralPath (Join-Path $files 'diag\probes')) { $what += @{ name = 'probes'; dirs = @('diag\probes') } }
    foreach ($w in $what) {
        if ($code -eq 'BLOCKED') { break }
        $env:PIPECHECK_ISA = Join-Path $result "isa\$($w.name)"
        New-Item -ItemType Directory -Force -Path $env:PIPECHECK_ISA | Out-Null
        $all = @(Get-ChildItem -LiteralPath @($w.dirs | ForEach-Object { Join-Path $files $_ }) -Recurse -Filter *.spv | Sort-Object FullName)
        $list = ($all | ForEach-Object { '"' + $_.FullName.Substring($files.Length + 1) + '"' }) -join ' '
        $one = Join-Path $result "pipecheck-$($w.name).txt"
        $c = Run 'nrkit.exe' "pipecheck $list" $one 15
        if ($c -ne 0 -and $code -eq 0) { $code = $c }
        Get-Content -LiteralPath $one -ErrorAction SilentlyContinue | Add-Content -LiteralPath $pc
    }
    Remove-Item Env:PIPECHECK_COOP, Env:PIPECHECK_TIMEOUT_S, Env:PIPECHECK_BUDGET_S, Env:PIPECHECK_ISA -ErrorAction SilentlyContinue
    if ($code -ne 'BLOCKED') {
        # the products in each set: one v_dot2 a pair of FP16 values, or two v_fma_mix (the 9070's LLPC); v_dot4 in INT8
        foreach ($w in $what) {
            $d2 = 0; $mix = 0; $d4 = 0; $n = 0
            foreach ($x in Get-ChildItem -LiteralPath (Join-Path $result "isa\$($w.name)") -Filter *.isa.txt -ErrorAction SilentlyContinue) {
                $t = Get-Content -LiteralPath $x.FullName -Raw; ++$n
                $d2 += ([regex]::Matches($t, 'v_dot2c?_f32_f16')).Count; $mix += ([regex]::Matches($t, 'v_fma_mix_f32')).Count
                $d4 += ([regex]::Matches($t, 'v_dot4c?_i32_i?u?8')).Count
            }
            Line "ISA $($w.name): $n pipelines disassembled; v_dot2_f32_f16 $d2, v_fma_mix_f32 $mix, v_dot4_i32_i8 $d4"
        }
        Get-Content -LiteralPath (Join-Path $result 'pipecheck-fp16.txt') -ErrorAction SilentlyContinue |
            Where-Object { $_ -match '^ISA .*(g_fswin(32nh|64|128|256)|g_gemmproj|temporal_post)' } | ForEach-Object { Line "INFO $($_ -replace '^ISA \S*\\', 'ISA ')" }
    }
    $rows = @(PipeRows $pc | Where-Object { $_.file -notlike 'diag\*' -and $_.file -notlike '*probes\*' })
    $badRows = @($rows | Where-Object { $_.state -ne 'OK' })
    $table = @('pipeline  state  ms') + @($rows | Sort-Object { -$_.ms } | ForEach-Object { '{0}  {1}  {2}  {3}' -f $_.file, $_.state, $_.ms, $_.text })
    Set-Content -LiteralPath (Join-Path $result 'pipelines-by-time.txt') -Value $table
    foreach ($s in $Sets | Where-Object { $code -ne 'BLOCKED' }) {
        $mine = @($rows | Where-Object { $_.file -like "$($s.dir)\*" })
        $ok = @($mine | Where-Object { $_.state -eq 'OK' })
        $sum = ($mine | ForEach-Object { $_.ms } | Measure-Object -Sum).Sum
        $slow = ($ok | Sort-Object { -$_.ms } | Select-Object -First 3 | ForEach-Object { '{0} {1:N1} s' -f ($_.file -replace '^.*\\', ''), ($_.ms / 1000) }) -join ', '
        Line ('{0} pipelines {1}: {2} of {3} compiled, {4:N0} s in all; slowest {5}' -f $(if ($ok.Count -eq $mine.Count -and $mine.Count) { 'PASS' } else { 'FAIL' }),
              $s.name, $ok.Count, $mine.Count, ($sum / 1000), $slow)
    }
    if ($code -eq 'TIMEOUT') { Line 'FAIL pipelines: a compile stage ran out of its 15 minutes' }
    foreach ($b in $badRows) { Line "     $($b.state) $($b.file) $($b.text)" }
    $pipesOk = $rows.Count -and -not @($badRows | Where-Object { $_.file -notlike 'shaders-rdna2-int8*' }).Count
    $int8Ok = $rows.Count -and -not $badRows.Count
    $pipeBlocked = $code -eq 'BLOCKED'
    if ($code -eq 'BLOCKED') { Line 'SKIPPED the shader compile and the network tests: Windows App Control blocked nrkit.exe' }
    elseif (-not $pipesOk) { Line 'SKIPPED the network tests: an FP16 shader did not compile' }
    elseif (-not $int8Ok) { Line 'WARN an INT8 shader did not compile: the INT8 runs below will fail' }
}

# 4b. The product micro-benchmark of the diagnostic kit v8 (bench.comp): MACs a second by tokens a lane, weights through
# scalar loads or LDS, FP16 spellings or int8, streamed or cached; three rounds, the GPU clock sampled meanwhile.
if ($unpacked -and -not $pipeBlocked -and (Test-Path -LiteralPath (Join-Path $files 'diag\bench'))) {
    Write-Host '[5/9] Speed of the product (micro-benchmark), three rounds...'
    $flag = Join-Path $work 'bench.stop'
    $clock = Launch 'nrkit.exe' "gpuclock 600 `"$flag`"" (Join-Path $result 'bench-gpuclock.txt')
    if ($clock -is [string]) { $clock = $null }
    $spv = (Get-ChildItem -LiteralPath (Join-Path $files 'diag\bench') -Filter *.spv | Sort-Object Name | ForEach-Object { "`"diag\bench\$($_.Name)`"" }) -join ' '
    foreach ($r in 1..3) {
        if ($r -eq 1) { $env:BENCH_ISA = Join-Path $result 'isa\bench'; New-Item -ItemType Directory -Force -Path $env:BENCH_ISA | Out-Null }
        $c = Run 'nrkit.exe' "bench 1024 $spv" (Join-Path $result "bench-r$r.txt") 10
        Remove-Item Env:BENCH_ISA -ErrorAction SilentlyContinue
        $n = @(Get-Content -LiteralPath (Join-Path $result "bench-r$r.txt") -ErrorAction SilentlyContinue | Where-Object { $_ -match '^BENCH .* ms$' }).Count
        Line "$(if ($n) { 'PASS' } else { 'FAIL' }) bench round ${r}: $n variants timed (exit $(Hex $c))"
    }
    Set-Content -LiteralPath $flag -Value 'stop'
    if ($clock) { if (-not $clock.WaitForExit(10000)) { try { $clock.Kill() } catch {} } }
    Remove-Item -LiteralPath (Join-Path $result 'bench-gpuclock.txt.err') -ErrorAction SilentlyContinue
    Get-Content -LiteralPath (Join-Path $result 'bench-gpuclock.txt') -ErrorAction SilentlyContinue | Where-Object { $_ -match '^CLOCK' } | ForEach-Object { Line "INFO bench $_" }
    # MAC rate of each variant, best of the rounds (65536 lanes, 1024 rows, KC channels, T tokens a lane)
    $best = @{}
    foreach ($r in 1..3) {
        foreach ($l in @(Get-Content -LiteralPath (Join-Path $result "bench-r$r.txt") -ErrorAction SilentlyContinue)) {
            if ($l -match '^BENCH (\S+) ([\d.]+) ms$') { $ms = [double]::Parse($Matches[2], [Globalization.CultureInfo]::InvariantCulture); if (-not $best[$Matches[1]] -or $ms -lt $best[$Matches[1]]) { $best[$Matches[1]] = $ms } }
        }
    }
    $rows = foreach ($k in $best.Keys | Sort-Object) {
        if ($k -match '_k(\d+)_t(\d)') { $macs = 65536.0 * 1024 * [int]$Matches[1] * [int]$Matches[2]; '{0,-26} {1,8:N3} ms {2,6:N2} TMAC/s' -f $k, $best[$k], ($macs / $best[$k] / 1e9) }
    }
    Set-Content -LiteralPath (Join-Path $result 'bench-summary.txt') -Value $rows
    $rows | ForEach-Object { Line "BENCH $_" }
}
$run = $pipesOk -and $have['dlssnr-small.bin']

# One harness run: the network on the test image. $c a case of cases.ps1, $vars extra environment. Leaves work\<tag>.bin.
function E2E($tag, $c, $frames, $vars, $dir, $minutes = 6) {
    $r = @{ ok = $false; why = ''; net = '?'; i8 = ''; ms = ''; netms = -1.0; median = ''; build = ''; secs = -1.0; check = ''; vram = ''
            menu = ''; sha = ''; bin = "work\$tag.bin"; log = @() }
    if (-not $have[$modelFile[$c.model]]) { $r.why = "no $($modelFile[$c.model])"; return $r }
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    $log = Join-Path $files 'mochizuki_nr.log'
    Remove-Item -LiteralPath $log, (Join-Path $files $r.bin) -ErrorAction SilentlyContinue
    $all = @{ E2E_W = "$($c.w)"; E2E_H = "$($c.h)"; E2E_FLAGS = $(if ($c.model -eq 'large') { '8192' } else { '0' }); E2E_CREATE_FLAGS = '0' }
    foreach ($k in $baseEnv.Keys) { $all[$k] = $baseEnv[$k] }
    foreach ($k in $vars.Keys) { $all[$k] = $vars[$k] }
    if (-not $all.ContainsKey('E2E_WAIT_MIN')) { $all['E2E_WAIT_MIN'] = "$([Math]::Max(1, $minutes - 1))" }
    SetEnv $all
    $out = Join-Path $dir "harness_$tag.txt"
    $code = Run 'nrkit.exe' "e2e .\MochizukiNrRuntime.dll . $Image $($r.bin) $frames 1" $out $minutes
    if (First $out 'load runtime: error (4551|1260)') { Blocked 'MochizukiNrRuntime.dll' }
    ClearEnv $all
    if (Test-Path -LiteralPath $log) { Copy-Item -LiteralPath $log -Destination (Join-Path $dir "log_$tag.txt") }
    $lt = @(if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log })
    $r.log = $lt
    $net = $lt | Where-Object { $_ -match '\[mochizuki\] the (RDNA2|RDNA3|FP8) network' } | Select-Object -First 1
    if ($net -match 'the RDNA2 network.*\(INT8 and FP16 products') { $r.net = 'RDNA2-INT8' }
    elseif ($net -match 'the (RDNA2|RDNA3|FP8) network') { $r.net = $Matches[1] }
    $i8 = $lt | Where-Object { $_ -match '\[mochizuki\] MZ_RDNA2_INT8: (.*)$' } | Select-Object -First 1
    if ($i8 -match 'MZ_RDNA2_INT8: (.*)$') { $r.i8 = $Matches[1] }
    $chk = $lt | Where-Object { $_ -match 'RDNA2 pipeline check (passed|failed|could not run)' } | Select-Object -Last 1
    if ($chk -match '\] (RDNA2 pipeline check.*)$') { $r.check = $Matches[1] }
    elseif ($lt | Where-Object { $_ -match 'RDNA2 pipeline check: \d+ pipelines' }) { $r.check = 'RDNA2 pipeline check started, never finished' }
    $vr = $lt | Where-Object { $_ -match 'VRAM: the network at' } | Select-Object -Last 1
    if ($vr -match 'estimated at (\d+) MB; this process grew by (\d+) MB') { $r.vram = "VRAM estimated $($Matches[1]) MB, grew $($Matches[2]) MB" }
    $status = First $out '^status: '
    if ($status -match 'network ([\d.]+) ms \(p95 ([\d.]+)\)') { $r.ms = "network $($Matches[1]) ms (p95 $($Matches[2]))"; $r.netms = [double]$Matches[1] }
    $fm = First $out '^frame ms: '
    if ($fm -match 'median=([\d.]+)') { $r.median = "frame median $($Matches[1]) ms" }
    $ready = First $out '^ready after '
    if ($ready -match 'ready after ([\d.]+) s') { $r.build = "ready in $($Matches[1]) s"; $r.secs = [double]$Matches[1] }
    $menu = First $out '^network \d+, menu: '
    if ($menu) { $r.menu = $menu }
    if ($code -ne 0) {
        $why = First $out 'FAIL'
        if ($code -eq 'TIMEOUT') { $why = "timed out after $minutes min" } elseif ($code -eq 'BLOCKED') { $why = 'blocked by Windows App Control' } elseif (-not $why) { $why = "exit $(Hex $code)" }
        $off = $lt | Where-Object { $_ -match 'network off|crash|build failed|unsupported' } | Select-Object -Last 1
        $r.why = "$why$(if ($off) { " | $($off -replace '^\S+ ', '')" })"
        return $r
    }
    if ($vars.ContainsKey('MZ_RDNA2_INT8') -and $r.net -ne 'RDNA2-INT8') { $r.why = "not the INT8 network ($($r.net)$(if ($r.i8) { ': ' + $r.i8 }))"; return $r }
    $r.ok = $true
    if (Test-Path -LiteralPath (Join-Path $files $r.bin)) { $r.sha = (Sha256 (Join-Path $files $r.bin)).Substring(0, 16) }
    return $r
}
# Against a reference: the worst area's PSNR (compare.exe), or -1 without that reference.
function Score($bin, $c, $prefix, $out, $pictures) {
    if (-not (Test-Path -LiteralPath (Join-Path $files "reference\$($prefix)_$($c.name)_q.jxr"))) { return @{ psnr = -1.0; text = 'no reference'; bad = 0 } }
    $a = "$bin $($c.w) $($c.h) reference\$($prefix)_$($c.name)"
    if ($pictures) { $a += " ..\result\$pictures\diff_x16.png ..\result\$pictures\output.jpg" }
    $null = Run 'nrkit.exe' "compare $a" $out 3
    $l = First $out '^COMPARE '
    if ($l -match 'psnr (\S+) dB \(worst area (\S+)\), nan/inf (\d+)') {
        return @{ psnr = $(if ($Matches[1] -eq 'inf') { 999.0 } else { [double]$Matches[1] }); text = "$($Matches[1]) dB"; bad = [int]$Matches[3] }
    }
    return @{ psnr = -1.0; text = 'no comparison'; bad = 0 }
}
# Two outputs at full resolution: 'identical', or the PSNR.
function RunsDiff($a, $b, $c, $out) {
    $null = Run 'nrkit.exe' "compare --bins $a $b $($c.w) $($c.h)" $out 3
    $l = First $out '^RUNS '
    if ($l -match '^RUNS identical') { return @{ text = 'identical'; psnr = 999.0 } }
    if ($l -match 'psnr (\S+) dB') { return @{ text = "$($Matches[1]) dB"; psnr = $(if ($Matches[1] -eq 'inf') { 999.0 } else { [double]$Matches[1] }) } }
    return @{ text = 'not compared'; psnr = -1.0 }
}
# The runtime's pipeline check of a set (dlssnr-amd\<check>\pipelines.txt + .log): each shader's compile ms into $out.
function CheckTable($check, $out) {
    $lst = Join-Path $files "dlssnr-amd\$check\pipelines.txt"; $log = Join-Path $files "dlssnr-amd\$check\pipelines.log"
    if (-not (Test-Path -LiteralPath $lst) -or -not (Test-Path -LiteralPath $log)) { return }
    $names = @(Get-Content -LiteralPath $lst)
    $rows = @()
    foreach ($l in Get-Content -LiteralPath $log) {
        if ($l -match '^ok (\d+) (\d+)' -and [int]$Matches[1] -lt $names.Count) {
            $rows += @{ file = ($names[[int]$Matches[1]] -replace '^.*\\dlssnr-amd\\', ''); ms = [int]$Matches[2] }
        }
    }
    $rows = @($rows | Sort-Object { -$_.ms })
    Set-Content -LiteralPath $out -Value (@("pipeline  ms (the runtime's check: cold, $($rows.Count) shaders on its threads)") + @($rows | ForEach-Object { "$($_.file)  $($_.ms)" }))
}

$baseEnv = @{}
if ($forced) { $baseEnv['MZ_FORCE_RDNA2'] = '1' }
Remove-Item Env:MZ_FORCE_RDNA2, Env:MZ_FORCE_RDNA3, Env:MZ_RDNA2_INT8, Env:MZ_RDNA2_CHECK, Env:MZ_RDNA3_CHECK, Env:MZ_CHECK_CACHE, `
    Env:DLSSNR_MODEL, Env:E2E_FLAGS, Env:E2E_CREATE_FLAGS, Env:E2E_PAN, Env:NR_STEP_TIMES, Env:NR_DIAG_SEED0, Env:NR_DIAG_TGATE0, `
    Env:NR_TCHAIN, Env:E2E_WAIT_MIN -ErrorAction SilentlyContinue
foreach ($f in 'pipeline-rdna2.cache', 'pipeline-rdna2-int8.cache', 'pipeline.cache', 'build.pending', 'build.failed', 'rdna2-check.txt',
               'rdna2-check', 'rdna2-int8-check.txt', 'rdna2-int8-check', 'binaries-rdna2', 'binaries-rdna2-int8') {
    Remove-Item -LiteralPath (Join-Path $files "dlssnr-amd\$f") -Recurse -Force -ErrorAction SilentlyContinue
}
$stop = -not $run

# 5. The first start of each form, with the runtime's own pipeline check, as a game's first start.
$cold = @{}; $setOk = @{ fp16 = $true; int8 = $true; int8old = $true }
if ($run) {
    Write-Host '[6/9] First start of each form (FP16, then INT8), a few minutes each...'
    $c = $Quality | Where-Object { $_.name -eq 'small_1280x720' } | Select-Object -First 1
    foreach ($s in $Sets) {
        if (-not $s.check) { continue }   # int8old: compiled by pipecheck, no first start of its own
        UsePlan $s
        $r = E2E "first_$($s.name)" $c 10 $s.vars (Join-Path $result "first\$($s.name)") 25
        $cold[$s.name] = $r.secs
        Line "FIRST START $($s.name): $(if ($r.ok) { $r.build } else { "FAILED: $($r.why)" }); $(if ($r.check) { $r.check } else { 'no check line' })"
        if ($r.i8) { Line "INFO   MZ_RDNA2_INT8: $($r.i8)" }
        if ($r.menu) { Line "INFO   $($r.menu)" }
        CheckTable $s.check (Join-Path $result "check-by-time-$($s.name).txt")
        if (-not $r.ok -and $r.check -match 'check failed|never finished') {
            Line "WARN the runtime's pipeline check keeps the $($s.name) network off: $($r.check); the runs below skip the check"
            $script:checkBad = $true
            $baseEnv['MZ_RDNA2_CHECK'] = '0'
            $r = E2E "first_$($s.name)_nocheck" $c 10 $s.vars (Join-Path $result "first\$($s.name)") 25
            Line "INFO first $($s.name) build without the check: $(if ($r.ok) { $r.build } else { "FAILED: $($r.why)" })"
        }
        if (-not $r.ok) { $setOk[$s.name] = $false }
        elseif ($r.net -notmatch '^RDNA2') { Line "SKIPPED the network tests: the runtime picked the $($r.net) network, not the RX 6000 one"; $stop = $true }
    }
    if (-not $setOk.fp16) { $stop = $true }
}
$baseEnv['MZ_RDNA2_CHECK'] = '0'   # checked once above; the runs below go straight to the network

# 6. Quality: clean runs, two a case and form. FP16 against the RX 9070 XT's FP8 output (PASS at $PassDb dB, as kits
# v1-v6), INT8 against FP8 (PASS at $PassInt8Db dB), against the same INT8 network on the RX 9070 XT and against this
# card's FP16 output (PASS at $PassSameDb dB, whole frame).
$verdict = @{}; $hash = @{}; $vary = @{}; $q8 = @{}
if (-not $stop) {
    Write-Host "[7/9] Picture check: $($Quality.Count) cases x $($Sets.Count) forms x $QualityRuns run(s)..."
    foreach ($c in $Quality) {
        if (-not $have[$modelFile[$c.model]]) { Line "SKIPPED $($c.name): $($modelFile[$c.model]) missing"; continue }
        foreach ($s in $Sets) {
            if (-not $setOk[$s.name]) { continue }
            Write-Host "    $($s.name) $($c.name) ..."
            $dir = Join-Path $result "quality\$($s.name)\$($c.name)"
            $v = @{}; foreach ($k in $Clean.Keys) { $v[$k] = $Clean[$k] }; foreach ($k in $s.vars.Keys) { $v[$k] = $s.vars[$k] }
            $sc = @(); $fails = @(); $info = $null
            UsePlan $s
            for ($k = 1; $k -le $QualityRuns; ++$k) {
                $r = E2E "$($s.name).$($c.name).r$k" $c $QualityFrames $v $dir 10
                if (-not $r.ok) { $fails += "run $k failed: $($r.why)"; break }
                if (-not $info) { $info = $r; $hash["$($s.name).$($c.name)"] = $r.sha }
            }
            $a = "work\$($s.name).$($c.name).r1.bin"
            $runs = if ($QualityRuns -gt 1 -and -not $fails.Count) { RunsDiff $a "work\$($s.name).$($c.name).r2.bin" $c (Join-Path $dir 'runs_r1_r2.txt') } else { @{ text = 'not compared' } }
            $vary["$($s.name).$($c.name)"] = $QualityRuns -gt 1 -and $runs.text -ne 'identical'
            $fp8 = if (-not $fails.Count) { Score $a $c 'img' (Join-Path $dir 'compare_fp8.txt') "quality\$($s.name)\$($c.name)" } else { @{ psnr = -1.0; text = '-'; bad = 0 } }
            $extra = ''; $ok = -not $fails.Count -and $fp8.bad -eq 0
            if ($s.name -eq 'fp16') {
                if ($fp8.psnr -ge 0 -and $fp8.psnr -lt $PassDb) { $ok = $false }
            } else {
                if ($fp8.psnr -ge 0 -and $fp8.psnr -lt $PassInt8Db) { $ok = $false }
                $same = Score $a $c $(if ($s.qk) { 'i8qk' } else { 'i8' }) (Join-Path $dir 'compare_i8_9070.txt') $null
                $f16bin = "work\fp16.$($c.name).r1.bin"
                $vs16 = if (Test-Path -LiteralPath (Join-Path $files $f16bin)) { RunsDiff $f16bin $a $c (Join-Path $dir 'int8_vs_fp16.txt') } else { @{ text = 'no FP16 run'; psnr = -1.0 } }
                if ($vs16.psnr -ge 0 -and $vs16.psnr -lt $PassSameDb) { $ok = $false }
                $q8["$($s.name).$($c.name)"] = @{ fp8 = $fp8.psnr; vs16 = $vs16.psnr; i8 = $same.psnr }
                $extra = "$(if ($same.psnr -ge 0) { "; against the RX 9070 XT's INT8: $($same.text)" }); against this card's FP16: $($vs16.text)"
            }
            $vd = if ($ok) { 'PASS' } else { 'FAIL' }
            $verdict["$($s.name).$($c.name)"] = $vd
            Line ("{0} {1} {2}: against FP8 {3}{4}{5}; sha {6}" -f $vd, $s.name, $c.name, $fp8.text, $extra, $(if ($QualityRuns -gt 1) { "; runs $($runs.text)" }), $(if ($info) { $info.sha } else { '-' }))
            foreach ($f in $fails) { Line "     $f" }
        }
        Remove-Item -Path (Join-Path $files "work\*.$($c.name).r2.bin") -ErrorAction SilentlyContinue
    }
}
Remove-Item -Path (Join-Path $files 'work\*.bin') -ErrorAction SilentlyContinue

# 7. Speed: normal runs as in a game, every case, FP16 and INT8 alternating, $SpeedRounds rounds.
$speed = @{}
if (-not $stop) {
    Write-Host "[8/9] Speed: $($Quality.Count) cases, $SpeedRounds rounds, FP16 and INT8 alternating..."
    foreach ($c in $Quality) {
        if (-not $have[$modelFile[$c.model]]) { continue }
        Write-Host "    $($c.name) ..."
        $dir = Join-Path $result "speed\$($c.name)"
        foreach ($s in $Sets) { $speed["$($s.name).$($c.name)"] = @() }
        for ($k = 1; $k -le $SpeedRounds; ++$k) {
            $order = @(for ($j = 0; $j -lt $Sets.Count; ++$j) { $Sets[($j + $k - 1) % $Sets.Count] })   # each set first once
            foreach ($s in $order) {
                if (-not $setOk[$s.name]) { continue }
                UsePlan $s
                $clock = $null; $flag = Join-Path $work 'clock.stop'; $clockOut = Join-Path $dir "gpuclock_$($s.name)_r$k.txt"
                New-Item -ItemType Directory -Path $dir -Force | Out-Null
                if ($true) {
                    Remove-Item -LiteralPath $flag -ErrorAction SilentlyContinue
                    $clock = Launch 'nrkit.exe' "gpuclock 400 `"$flag`"" $clockOut
                    if ($clock -is [string]) { $clock = $null }
                }
                $r = E2E "$($c.name).$($s.name).speed.r$k" $c $SpeedFrames $s.vars $dir
                if ($clock) {
                    Set-Content -LiteralPath $flag -Value 'stop'
                    if (-not $clock.WaitForExit(10000)) { try { $clock.Kill() } catch {} }
                    Remove-Item -LiteralPath "$clockOut.err" -ErrorAction SilentlyContinue
                }
                Remove-Item -LiteralPath (Join-Path $files $r.bin), (Join-Path $files "$($r.bin).prev") -ErrorAction SilentlyContinue
                if ($r.ok -and $r.netms -gt 0) { $speed["$($s.name).$($c.name)"] += $r.netms }
                else { Line "FAIL speed $($s.name) $($c.name) r${k}: $($r.why)" }
                if ($k -eq 1) {
                    $ck = First $clockOut '^CLOCK '
                    Line "SPEED $($s.name) $($c.name) r1: $($r.ms), $($r.median); $($r.vram); GPU $(if ($ck) { $ck -replace '^CLOCK ', '' } else { 'no clock reading' })"
                }
            }
        }
        $d = $speed["fp16.$($c.name)"]; $a = $speed["int8.$($c.name)"]
        $ad = Avg $d; $aa = Avg $a
        $rel = if ($ad -gt 0 -and $aa -gt 0) { '; INT8 {0:+0.0;-0.0}% against FP16 ({1:N2}x)' -f (100.0 * ($aa - $ad) / $ad), ($ad / $aa) } else { '' }
        $o = $speed["int8old.$($c.name)"]; $ao = Avg $o
        $withQk = if ($o) { '; INT8 of 0.6.9 {0} ms (avg {1:N1}){2}' -f (($o | ForEach-Object { '{0:N1}' -f $_ }) -join ' / '), $ao,
                         $(if ($ao -gt 0 -and $aa -gt 0) { ', INT8 {0:+0.0;-0.0}% against it' -f (100.0 * ($aa - $ao) / $ao) }) } else { '' }
        Line ("SPEED {0}: FP16 {1} ms (avg {2:N1}), INT8 {3} ms (avg {4:N1}){5}{6}" -f $c.name, (($d | ForEach-Object { '{0:N1}' -f $_ }) -join ' / '), $ad,
              (($a | ForEach-Object { '{0:N1}' -f $_ }) -join ' / '), $aa, $rel, $withQk)
    }
}

# 8. The time of each shader (NR_STEP_TIMES), every set and case, $StepRounds rounds with the sets alternating, the GPU
# clock sampled in each run (steps\<case>\gpuclock_<set>_r<k>.txt).
function StepRun($tag, $c, $dir, $name, $label, $vars, $quiet) {
    $v = @{ NR_STEP_TIMES = "$StepCount" }; foreach ($k in $vars.Keys) { $v[$k] = $vars[$k] }
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    $flag = Join-Path $work 'clock.stop'; $clockOut = Join-Path $dir "gpuclock_$name.txt"
    Remove-Item -LiteralPath $flag -ErrorAction SilentlyContinue
    $clock = Launch 'nrkit.exe' "gpuclock 400 `"$flag`"" $clockOut
    if ($clock -is [string]) { $clock = $null }
    $r = E2E $tag $c $StepFrames $v $dir
    if ($clock) {
        Set-Content -LiteralPath $flag -Value 'stop'
        if (-not $clock.WaitForExit(10000)) { try { $clock.Kill() } catch {} }
        Remove-Item -LiteralPath "$clockOut.err" -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath (Join-Path $files $r.bin), (Join-Path $files "$($r.bin).prev") -ErrorAction SilentlyContinue
    $lt = $r.log
    $i = -1
    for ($j = 0; $j -lt $lt.Count; ++$j) { if ($lt[$j] -match 'NR_STEP_TIMES: \d+ frames') { $i = $j } }
    if (-not $r.ok -or $i -lt 0) { Line "FAIL $label $($c.name): $(if ($r.why) { $r.why } else { 'no NR_STEP_TIMES table in the log' })"; return }
    $tab = @($lt[$i] -replace '^.*NR_STEP_TIMES: ', '')
    for ($j = $i + 1; $j -lt $lt.Count -and $lt[$j] -match '\[nr\]\s+\S+\.spv\s'; ++$j) { $tab += ($lt[$j] -replace '^.*\[nr\]\s+', '') }
    Set-Content -LiteralPath (Join-Path $result "steps_$name.txt") -Value $tab
    if ($quiet) { return }
    $top = ($tab | Select-Object -Skip 1 -First 6 | ForEach-Object { if ($_ -match '^(\S+)\.spv\s+(\d+) x\s+([\d.]+) ms') { "$($Matches[1]) $($Matches[3])" } }) -join ', '
    $ck = First $clockOut '^CLOCK '
    Line "$label $($c.name): $($tab[0]); top: $top; GPU $(if ($ck) { $ck -replace '^CLOCK ', '' } else { 'no clock reading' })"
}
if (-not $stop) {
    Write-Host "[9/9] The time of each shader: $($Steps.Count) cases, $StepRounds rounds..."
    for ($k = 1; $k -le $StepRounds; ++$k) {
        foreach ($n in $Steps) {
            $c = $Quality | Where-Object { $_.name -eq $n } | Select-Object -First 1
            if (-not $c -or -not $have[$modelFile[$c.model]]) { continue }
            Write-Host "    $n round $k ..."
            $order = @(for ($j = 0; $j -lt $Sets.Count; ++$j) { $Sets[($j + $k - 1) % $Sets.Count] })
            foreach ($s in $order) {
                if ($setOk[$s.name]) { UsePlan $s; StepRun "$n.$($s.name).steps.r$k" $c (Join-Path $result "steps\$n") "$($s.name)_$($n)_r$k" "STEPS $($s.name) r$k" $s.vars ($k -gt 1) }
            }
        }
    }
}

# Summary and the zip.
Write-Host 'Writing the result...'
$card = if ($dev.Count) { $dev[0] -replace '^vulkan device \d+: ', '' } elseif ($card) { $card } elseif ($blocked.Contains('nrkit.exe')) { 'unknown' } else { 'no AMD Vulkan device' }
if ($blocked.Count) {
    $short.Add("BLOCKED BY WINDOWS APP CONTROL: $($blocked -join ', ')")
    $short.Add('  Windows stopped the file(s) above from running. That is Smart App Control (or an App Control policy): it blocks')
    $short.Add('  programs it does not know that carry no signature, and this kit''s tools have none.')
    $short.Add('  Please open Windows Security > App & browser control > Smart App Control settings and tell us what it')
    $short.Add("  says: On, Evaluation or Off (this PC's registry says: $sacText).")
    $short.Add('  Please do not turn Smart App Control off for this: on most Windows versions it cannot be turned back on')
    $short.Add('  without reinstalling Windows. If your Windows offers to allow just this file, you can allow it and run the')
    $short.Add('  test again. If it does not, send this result anyway: it still tells us what we need.')
    $ci = @(try { Get-WinEvent -FilterHashtable @{ LogName = 'Microsoft-Windows-CodeIntegrity/Operational'; Id = 3033, 3034, 3076, 3077; StartTime = $t0 } -MaxEvents 20 -ErrorAction Stop } catch { })
    Set-Content -LiteralPath (Join-Path $result 'appcontrol-events.txt') -Value $(if ($ci.Count) { $ci | ForEach-Object { "$($_.TimeCreated) $($_.Id) $($_.Message)" } } else { 'no Code Integrity events readable' })
}
$short.Add("Card: $card")
if ($notRdna2) { $short.Add('This card would not use the RX 6000 network, so the network was not tested. Please tell us which card this is.') }
else {
    $short.Add("Shaders compile: $(if ($pipeBlocked) { 'not run (Windows App Control)' } elseif (-not $pipesOk) { 'NO (see the pipelines lines)' } elseif ($int8Ok) { 'yes, both forms' } else { 'FP16 yes, INT8 NO' })")
    if ($script:checkBad) { $short.Add('The runtime''s own shader check failed (see WARN): in a game that network would stay off') }
    $fs = @(); foreach ($k in 'fp16', 'int8') { if ($cold.ContainsKey($k) -and $cold[$k] -ge 0) { $fs += ('{0} {1:N1} s' -f $k, $cold[$k]) } }
    if ($fs.Count) { $short.Add("First start (with the shader check): $($fs -join ', ')") }
    foreach ($who in 'fp16', 'int8', 'int8old') {
        $keys = @($verdict.Keys | Where-Object { $_ -like "$who.*" })
        if (-not $keys.Count) { continue }
        $p = @($keys | Where-Object { $verdict[$_] -eq 'PASS' })
        $f = @($keys | Where-Object { $verdict[$_] -ne 'PASS' } | ForEach-Object { $_.Substring($who.Length + 1) })
        $v = @($keys | Where-Object { $vary[$_] } | ForEach-Object { $_.Substring($who.Length + 1) })
        $short.Add("Picture ($who): PASS $($p.Count) of $($keys.Count)$(if ($f.Count) { "; FAIL: $($f -join ', ')" })$(if ($v.Count) { "; changes run to run: $($v -join ', ')" })")
    }
    $rows = @()
    foreach ($c in $Quality) {
        $ad = Avg $speed["fp16.$($c.name)"]; $aa = Avg $speed["int8.$($c.name)"]; $ao = Avg $speed["int8old.$($c.name)"]
        $o = if ($ao -gt 0) { ' / {0:N1}' -f $ao } else { '' }
        if ($ad -gt 0) { $rows += $(if ($aa -gt 0) { '{0} {1:N1} / {2:N1}{3} ({4:N2}x)' -f $c.name, $ad, $aa, $o, ($ad / $aa) } else { '{0} {1:N1}' -f $c.name, $ad }) }
    }
    $hdr = if ($Sets.Count -gt 2) { 'FP16 / INT8 / INT8 of 0.6.9' } else { 'FP16 / INT8' }
    if ($rows.Count) { $short.Add("Network ms per frame ($hdr): $($rows -join '; ')") }
}
UsePlan $Sets[1]   # the folders as unpacked
$head = @("OpenNR RX 6000 confirmation kit (mochizuki RDNA2 network of the next OpenNR: FP16, INT8 with Q.K in INT8, and 0.6.9's INT8), $((Get-Date).ToString('yyyy-MM-dd HH:mm')), $(Minutes) min", '',
          'IN SHORT') + ($short | ForEach-Object { "  $_" }) + @('', 'DETAILS')
$tail = @('', "How to read this: FP16 PASS = within $PassDb dB PSNR (worst area) of the RX 9070 XT's FP8 picture; INT8 PASS = within",
          "$PassInt8Db dB of it and within $PassSameDb dB (whole frame) of this card's FP16 picture. ""identical"" = the same picture both runs.",
          'int8 is the INT8 network as the next OpenNR ships it (the C = 32 attention Q.K in INT8, kit v8''s int8qk); int8old is 0.6.9''s INT8 set.',
          'ISA lines: the driver''s own disassembly counts (isa\<set>\); BENCH: the product micro-benchmark (bench-summary.txt).',
          'SPEED is the network alone (status line), with the GPU clock while busy. STEPS: the per-shader times (NR_STEP_TIMES), steps_<set>_<case>_r<round>.txt.',
          'Raw files: unpack.txt, system.txt, appcontrol-events.txt, pipecheck*.txt, isa\, bench-*.txt, pipelines-by-time.txt, check-by-time-*.txt, first\, quality\, speed\, steps\, steps_*.txt, console.txt.')
Set-Content -LiteralPath (Join-Path $result 'SUMMARY.txt') -Value ($head + $summary + $tail)
foreach ($s in $Sets | Where-Object { $_.check }) {
    foreach ($f in "$($s.check).txt", "$($s.check)\pipelines.txt", "$($s.check)\pipelines.log") {
        $p = Join-Path $files "dlssnr-amd\$f"
        if (Test-Path -LiteralPath $p) { Copy-Item -LiteralPath $p -Destination (Join-Path $result ('runtime-' + ($f -replace '\\', '-'))) }
    }
}
Stop-Transcript | Out-Null
Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
$zip = Join-Path $kit "OpenNR-RX6000-confirm-result-$stamp.zip"
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
if ($blocked.Count) { $short | Select-Object -First 8 | ForEach-Object { Write-Host $_ -ForegroundColor Yellow }; Write-Host '' }
if (Test-Path -LiteralPath $zip) {
    Write-Host 'Done. Please send this file:' -ForegroundColor Green
    Write-Host "  $zip" -ForegroundColor Green
    if (-not $env:KIT_QUIET) { Start-Process explorer.exe -ArgumentList "/select,`"$zip`"" }
} else {
    Write-Host 'Done, but the zip could not be made. Please zip and send the folder:' -ForegroundColor Yellow
    Write-Host "  $result" -ForegroundColor Yellow
}
