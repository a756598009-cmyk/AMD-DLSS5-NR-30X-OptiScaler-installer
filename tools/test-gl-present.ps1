# Called by test-gl-present.cmd, which sets up the x64 MSVC environment.
$ErrorActionPreference = "Stop"
Set-Location (Split-Path -Parent $PSScriptRoot)

$mode = if ($args.Count -gt 0) { $args[0] } else { "menu" }
$runtime = if ($args.Count -gt 1) { $args[1] } else { "" }
# "threaded" renders on a thread of its own, which never pumps messages, while the window thread moves the window.
$threaded = $args.Count -gt 2 -and $args[2] -eq "threaded"
if ($mode -notin @("menu", "nr", "fg")) { throw "mode must be menu, nr or fg" }
if ($mode -ne "menu" -and -not (Test-Path (Join-Path $runtime "dlssnr_amd_pass1.dll"))) {
    throw "$mode needs a runtime folder with dlssnr_amd_pass1.dll and dlssnr_on_amd_weights.bin"
}

$out = [System.IO.Path]::GetFullPath("exports\gl-present\$mode$(if ($threaded) { "-threaded" })")
New-Item -ItemType Directory -Force -Path $out | Out-Null

& cl /nologo /std:c++20 /EHsc /W4 /utf-8 tests\gl_present_smoke.cpp "/Fe$out\gl_present_smoke.exe" "/Fo$out\gl_present_smoke.obj" /link opengl32.lib gdi32.lib user32.lib dwmapi.lib
if ($LASTEXITCODE -ne 0) { throw "cl failed" }

Copy-Item -Force "exports\release-local\OptiScaler.dll" "$out\opengl32.dll"
$files = @()
if ($mode -ne "menu") { $files += "dlssnr_amd_pass1.dll", "dlssnr_on_amd_weights.bin" }
if ($mode -eq "fg") { $files += "libxess_fg.dll", "libxell.dll" }
foreach ($name in $files) {
    $source = Get-Item (Join-Path $runtime $name)
    $target = Join-Path $out $name
    if (-not (Test-Path $target) -or (Get-Item $target).Length -ne $source.Length) { Copy-Item -Force $source.FullName $target }
}

$nr = if ($mode -eq "menu") { "false" } else { "true" }
$fg = if ($mode -eq "fg") { "true" } else { "false" }
@"
[Log]
LogToFile=true
LogLevel=2

[Menu]
DisableSplash=true

[Hotfix]
CheckForUpdate=false

[DlssNr]
Enabled=$nr
PresentWithoutUpscaler=$nr
NrBackend=daniel

[FrameGen]
Enabled=$fg
FGOutput=$(if ($mode -eq "fg") { "xefg" } else { "auto" })

[V-Sync]
ForceVsync=false
"@ | Set-Content -Encoding ascii "$out\OptiScaler.ini"
Remove-Item -Force -ErrorAction SilentlyContinue "$out\OptiScaler.log"

# One GPU, shared by every worktree: a directory is the lock, stale after 20 minutes.
$lock = [System.IO.Path]::GetFullPath("..\locks\gpu")
while ($true) {
    try { New-Item -ItemType Directory -Path $lock -ErrorAction Stop | Out-Null; break }
    catch {
        $existing = Get-Item $lock -ErrorAction SilentlyContinue
        if ($existing -and $existing.CreationTime -lt (Get-Date).AddMinutes(-20)) { Remove-Item -Force -Recurse $lock; continue }
        Write-Host "waiting for the GPU lock"
        Start-Sleep -Seconds 20
    }
}
try {
    $frames = if ($mode -eq "menu") { 300 } else { 900 }
    $process = Start-Process -FilePath "$out\gl_present_smoke.exe" -ArgumentList $frames, $(if ($mode -eq "menu") { "menu" } else { "nr" }), $(if ($threaded) { "threaded" } else { "inline" }) `
        -WorkingDirectory $out -PassThru -NoNewWindow -RedirectStandardOutput "$out\stdout.txt"
    $null = $process.Handle
    if (-not $process.WaitForExit(150000)) { $process.Kill(); throw "timeout: the app did not exit in 150 s" }
    $code = $process.ExitCode
}
finally { Remove-Item -Force -Recurse $lock -ErrorAction SilentlyContinue }

Get-Content "$out\stdout.txt"
$log = Get-Content "$out\OptiScaler.log" -Raw
$expected = @(
    "OptiScaler working as opengl32.dll",
    "OpenGL: wglSwapBuffers hooked",
    "OpenGL menu: ImGui renderer ready",
    "OpenGL menu: opened",
    "OpenGL menu: frame drawn with the menu open",
    "OpenGL menu: closed",
    "OpenGL: swap interval set to 0"
)
if ($mode -ne "menu") {
    $expected += "OpenGL final-image NR: back buffer 1280x720", "OpenGL final-image NR: first frame processed", "ms average over 120 presents"
}
if ($mode -eq "fg") {
    $expected += "OpenGL XeFG presenter created", "OpenGL XeFG: 2 generated frames per frame",
        "OpenGL XeFG paused while frame generation is off", "OpenGL final-image route: NR off, XeFG runs on its guides"
}
$failed = $code -ne 0
if ($threaded -and -not ((Get-Content "$out\stdout.txt" -Raw) -match "window moved [1-9]")) {
    Write-Host "MISSING: the window thread never moved the window"; $failed = $true
}
if ($mode -eq "fg") {
    # XeFG's status line every 30 frames: "presented" counts the frames shown for one rendered frame.
    $lines = Get-Content "$out\OptiScaler.log"
    function Presented([int]$from, [int]$to) {
        $most = 0
        for ($i = [Math]::Max($from, 0); $i -lt [Math]::Min($to, $lines.Count); $i++) {
            if ($lines[$i] -match 'OpenGL XeFG: frame \d+, presented (\d+)') { $most = [Math]::Max($most, [int]$Matches[1]) }
        }
        return $most
    }
    function LineOf([string]$text) { for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i].Contains($text)) { return $i } }; return -1 }
    function LastLineOf([string]$text) { for ($i = $lines.Count - 1; $i -ge 0; $i--) { if ($lines[$i].Contains($text)) { return $i } }; return -1 }
    $opened = LineOf "OpenGL menu: opened"
    $closed = LineOf "OpenGL menu: closed"
    $resumed = LastLineOf "OpenGL final-image route: NR on"
    $checks = @(
        @{ Text = "XeFG presents 3 frames per rendered frame"; Ok = (Presented 0 $lines.Count) -ge 3 },
        @{ Text = "XeFG keeps generating with the menu open"; Ok = $opened -ge 0 -and $closed -gt $opened -and (Presented $opened $closed) -ge 2 },
        @{ Text = "XeFG generates again after the FG and NR keys"; Ok = $resumed -ge 0 -and (Presented $resumed $lines.Count) -ge 2 },
        @{ Text = "XeFG does not pause for the menu"; Ok = -not ($log.Contains("paused while OptiScaler menu is open")) },
        @{ Text = "XeFG keeps running with NR off"; Ok = -not ($log.Contains("paused while final-image NR is off")) }
    )
    foreach ($check in $checks) {
        if ($check.Ok) { Write-Host "ok: $($check.Text)" } else { Write-Host "MISSING: $($check.Text)"; $failed = $true }
    }
}
if (-not (Get-Content "$out\stdout.txt" -Raw).Contains("swap interval after the presents: 0")) {
    Write-Host "MISSING: the game's swap interval of 1 was not forced to 0"; $failed = $true
}
foreach ($line in $expected) {
    if ($log.Contains($line)) { Write-Host "ok: $line" } else { Write-Host "MISSING: $line"; $failed = $true }
}
Select-String -Path "$out\OptiScaler.log" -Pattern "OpenGL|error" | Select-Object -First 60 | ForEach-Object { $_.Line }
if ($failed) { Write-Host "FAIL ($mode, exit $code)"; exit 1 }
Write-Host "PASS ($mode)"
exit 0
