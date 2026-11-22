# Runs tests\shortcut_keys_test.cpp, then tests\screenshot_smoke.cpp on each API with exports\release-local\OptiScaler.dll beside it under the proxy
# name that API loads, and checks the test's output and OptiScaler.log. See test-screenshot.cmd.
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Cases)
if (-not $Cases) { $Cases = @("dx11", "dx11-chord", "dx11-nomenu", "dx11-pictures", "dx12", "dx12-fp16", "dx12-hdr10", "vk", "gl", "dx9") }
$ErrorActionPreference = "Stop"
Set-Location (Split-Path -Parent $PSScriptRoot)

$root = [System.IO.Path]::GetFullPath("exports\screenshot-smoke")
New-Item -ItemType Directory -Force -Path $root | Out-Null
$exe = Join-Path $root "screenshot_smoke.exe"
& cl /nologo /std:c++20 /EHsc /O2 /W4 /utf-8 /Iexternal\vulkan\include tests\screenshot_smoke.cpp "/Fe$exe" `
    "/Fo$(Join-Path $root 'screenshot_smoke.obj')" /link OptiScaler\library\vulkan\vulkan-1.lib user32.lib gdi32.lib `
    ole32.lib shell32.lib windowscodecs.lib opengl32.lib /DELAYLOAD:opengl32.dll /DELAYLOAD:vulkan-1.dll delayimp.lib
if ($LASTEXITCODE -ne 0) { throw "cl failed" }
$opti = [System.IO.Path]::GetFullPath("exports\release-local\OptiScaler.dll")

# Shortcut parsing and matching first: no GPU, no window.
$unit = Join-Path $root "shortcut_keys_test.exe"
& cl /nologo /std:c++20 /EHsc /W4 tests\shortcut_keys_test.cpp "/Fe$unit" "/Fo$(Join-Path $root 'shortcut_keys_test.obj')"
if ($LASTEXITCODE -ne 0) { throw "cl failed" }
& $unit | Select-String "FAIL|failed"
if ($LASTEXITCODE -ne 0) { Write-Host "FAIL shortcut_keys_test"; exit 1 }

# One GPU, shared by every worktree: a directory is the lock, stale after 20 minutes.
$lock = [System.IO.Path]::GetFullPath("..\locks\gpu")
function Enter-Lock {
    while ($true) {
        try { New-Item -ItemType Directory -Path $lock -ErrorAction Stop | Out-Null; return }
        catch {
            $existing = Get-Item $lock -ErrorAction SilentlyContinue
            if ($existing -and $existing.CreationTime -lt (Get-Date).AddMinutes(-20)) { Remove-Item -Force -Recurse $lock; continue }
            Write-Host "waiting for the GPU lock"
            Start-Sleep -Seconds 20
        }
    }
}

$failed = @()
foreach ($case in $Cases) {
    # dx11-chord: Ctrl+F6. dx11-nomenu: ScreenshotWithMenu=false. dx11-pictures: the default folder in Pictures.
    $api = $case
    $mode = ""
    foreach ($m in @("chord", "nomenu", "pictures")) {
        if ($case.EndsWith("-$m")) { $mode = $m; $api = $case.Substring(0, $case.Length - $m.Length - 1) }
    }
    $dir = Join-Path $root $case
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $dir
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    Copy-Item $exe $dir
    $proxy = switch -Wildcard ($api) { "dx1*" { "dxgi.dll" } "vk" { "OptiScaler.dll" } "gl" { "opengl32.dll" } "dx9" { "d3d9.dll" } }
    Copy-Item $opti (Join-Path $dir $proxy)
    $nr = if ($api -eq "dx9") { "true" } else { "false" }
    $key = if ($mode -eq "chord") { "Ctrl+F6" } else { "auto" }
    $path = if ($mode -eq "pictures") { "auto" } else { "shots" }
    $withMenu = if ($mode -eq "nomenu") { "false" } else { "auto" }
    Set-Content -Path (Join-Path $dir "OptiScaler.ini") -Encoding ascii -Value @"
[Log]
LogToFile=true
LogLevel=2
SingleFile=true

[Menu]
DisableSplash=true
ShowFps=true
ScreenshotKey=$key
ScreenshotPath=$path
ScreenshotWithMenu=$withMenu

[Hotfix]
CheckForUpdate=false

[DlssNr]
Enabled=$nr
PresentWithoutUpscaler=false

[FrameGen]
Enabled=false
"@
    Write-Host "=== $case"
    Enter-Lock
    try {
        $arguments = @($api) + $(if ($mode) { @($mode) } else { @() })
        $process = Start-Process -FilePath (Join-Path $dir "screenshot_smoke.exe") -ArgumentList $arguments `
            -WorkingDirectory $dir -NoNewWindow -PassThru -RedirectStandardOutput (Join-Path $dir "stdout.txt")
        $null = $process.Handle
        if (-not $process.WaitForExit(90000)) { $process.Kill(); $process.WaitForExit(); $failed += "${case}: timed out"; continue }
        $code = $process.ExitCode
    }
    finally { Remove-Item $lock -Force -Recurse -ErrorAction SilentlyContinue }

    Get-Content (Join-Path $dir "stdout.txt")
    $log = Get-Content (Join-Path $dir "OptiScaler.log") -Raw -ErrorAction SilentlyContinue
    $saved = [regex]::Matches("$log", "Screenshot saved[^\r\n]*") | ForEach-Object { $_.Value }
    $saved | ForEach-Object { Write-Host "  log: $_" }
    if ($code -ne 0) { $failed += "${case}: exit code $code" }
    # The menu is open when the key is pressed: the frame and its copy with the menu, unless that is off.
    $want = if ($mode -eq "nomenu") { 1 } else { 2 }
    if (@($saved).Count -ne $want) { $failed += "${case}: $(@($saved).Count) 'Screenshot saved' lines in OptiScaler.log, want $want" }
    if ($mode -ne "nomenu" -and "$log" -match "no copy with the menu[^\r\n]*") { $failed += "${case}: $($Matches[0])" }
    if ("$log" -match "Screenshot failed[^\r\n]*") { $failed += "${case}: $($Matches[0])" }
}

if ($failed.Count) { $failed | ForEach-Object { Write-Host "FAIL $_" }; exit 1 }
Write-Host "all screenshot cases passed"
exit 0
