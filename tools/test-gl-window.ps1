# Called by test-gl-window.cmd, which sets up the x64 MSVC environment.
$ErrorActionPreference = "Stop"
Set-Location (Split-Path -Parent $PSScriptRoot)

$mode = if ($args.Count -gt 0) { $args[0] } else { "recreate" }
$runtime = if ($args.Count -gt 1) { $args[1] } else { "" }
$fg = $args.Count -gt 2 -and $args[2] -eq "fg"
if ($mode -notin @("recreate", "resize", "two", "twosame")) { throw "mode must be recreate, resize, two or twosame" }
if (-not (Test-Path (Join-Path $runtime "dlssnr_amd_pass1.dll"))) {
    throw "a runtime folder with dlssnr_amd_pass1.dll and dlssnr_on_amd_weights.bin is needed"
}

$out = [System.IO.Path]::GetFullPath("exports\gl-window\$mode$(if ($fg) { "-fg" })")
New-Item -ItemType Directory -Force -Path $out | Out-Null

& cl /nologo /std:c++20 /EHsc /W4 /utf-8 tests\gl_window_smoke.cpp "/Fe$out\gl_window_smoke.exe" "/Fo$out\gl_window_smoke.obj" /link opengl32.lib gdi32.lib user32.lib
if ($LASTEXITCODE -ne 0) { throw "cl failed" }

Copy-Item -Force "exports\release-local\OptiScaler.dll" "$out\opengl32.dll"
$files = @("dlssnr_amd_pass1.dll", "dlssnr_on_amd_weights.bin")
if ($fg) { $files += "libxess_fg.dll", "libxell.dll" }
foreach ($name in $files) {
    $source = Get-Item (Join-Path $runtime $name)
    $target = Join-Path $out $name
    if (-not (Test-Path $target) -or (Get-Item $target).Length -ne $source.Length) { Copy-Item -Force $source.FullName $target }
}

@"
[Log]
LogToFile=true
LogLevel=2

[Menu]
DisableSplash=true

[Hotfix]
CheckForUpdate=false

[DlssNr]
Enabled=true
PresentWithoutUpscaler=true
NrBackend=daniel

[FrameGen]
Enabled=$(if ($fg) { "true" } else { "false" })
FGOutput=$(if ($fg) { "xefg" } else { "auto" })
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
    $process = Start-Process -FilePath "$out\gl_window_smoke.exe" -ArgumentList $mode -WorkingDirectory $out -PassThru `
        -NoNewWindow -RedirectStandardOutput "$out\stdout.txt"
    $null = $process.Handle
    if (-not $process.WaitForExit(150000)) { $process.Kill(); throw "timeout: the app did not exit in 150 s" }
    $code = $process.ExitCode
}
finally { Remove-Item -Force -Recurse $lock -ErrorAction SilentlyContinue }

Get-Content "$out\stdout.txt"
$log = Get-Content "$out\OptiScaler.log" -Raw
$failed = $code -ne 0
function Expect([string]$line, [int]$least = 1, [int]$most = [int]::MaxValue) {
    $count = ([regex]::Matches($log, [regex]::Escape($line))).Count
    if ($count -ge $least -and $count -le $most) { Write-Host "ok: $line ($count)" }
    else { Write-Host "WRONG COUNT: $line ($count, expected $least to $most)"; $script:failed = $true }
}
Expect "OptiScaler working as opengl32.dll"
Expect "OpenGL final-image NR: first frame processed"
if ($mode -like "two*") {
    # The second window passes through: one window followed, one menu setup, one crossing, one presenter.
    Expect "OpenGL: OptiScaler follows the presents to window" 1 1
    Expect "OpenGL menu: ImGui renderer ready" 1 1
    Expect "OpenGL menu: opened" 1 1
    Expect "OpenGL menu: closed" 1 1
    Expect "crosses as a D3D12 texture" 1 1
    Expect "OpenGL final-image NR: back buffer 1280x720" 1 1
    if ($fg) {
        Expect "OpenGL XeFG presenter created at 1280x720" 1 1
        Expect "OpenGL XeFG: the game window changed" 0 0
        Expect "generated 1, result 0"
    }
}
elseif ($mode -eq "recreate") {
    Expect "OpenGL final-image NR: back buffer 1280x720" 1 1
    if ($fg) {
        Expect "OpenGL XeFG: the game window changed" 1 1
        Expect "OpenGL XeFG presenter created at 1280x720" 2 2
    }
}
else {
    # The 150 sizes come every 8 frames; the crossing is built for the first size and the last.
    Expect "crosses as a D3D12 texture" 2 6
    Expect "OpenGL final-image NR: back buffer 688x424"
    if ($fg) { Expect "OpenGL XeFG presenter created at 688x424" }
}
Select-String -Path "$out\OptiScaler.log" -Pattern "OpenGL|error" | Select-Object -First 60 | ForEach-Object { $_.Line }
if ($failed) { Write-Host "FAIL ($mode$(if ($fg) { ' fg' }), exit $code)"; exit 1 }
Write-Host "PASS ($mode$(if ($fg) { ' fg' }))"
exit 0
