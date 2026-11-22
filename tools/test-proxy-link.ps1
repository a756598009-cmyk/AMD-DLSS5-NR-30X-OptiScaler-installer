# Requires an x64 MSVC developer environment and a build from tools\build-optiscaler-fast.cmd (it writes the map).
# Checks three things about exports\release-local\OptiScaler.dll:
#  1. No symbol that the FidelityFX 3.1 static libraries define is supplied by the FidelityFX SDK v2 objects the
#     project compiles, so the 3.1 upscaler code keeps calling its own helpers.
#  2. The graphics DLLs are delay-loaded, not static imports.
#  3. Loading the DLL as winmm.dll brings in no graphics module that the 0.4.9 release DLL does not.
$ErrorActionPreference = "Stop"
Set-Location (Split-Path -Parent $PSScriptRoot)

$dll = [System.IO.Path]::GetFullPath("exports\release-local\OptiScaler.dll")
$map = [System.IO.Path]::ChangeExtension($dll, ".map")
$baseline = $args[0]
if ([string]::IsNullOrWhiteSpace($baseline)) { $baseline = "..\..\opti\dist\OptiScaler-0.4.9-amd-nr\OptiScaler.dll" }
$out = [System.IO.Path]::GetFullPath("exports\proxy-link")
New-Item -ItemType Directory -Force -Path $out | Out-Null
$failed = 0
function Fail([string]$text) { Write-Host "FAIL: $text"; $script:failed++ }

if ((Get-Item $map).LastWriteTime -lt (Get-Item $dll).LastWriteTime.AddSeconds(-60)) { throw "map older than the DLL: $map" }

$v1 = @{}
foreach ($lib in Get-ChildItem "OptiScaler\library\fsr31", "OptiScaler\library\fsr2", "OptiScaler\library\fsr2_212" -Filter *.lib) {
    if ($lib.BaseName.EndsWith("d")) { continue }
    foreach ($line in & dumpbin /nologo /linkermember:2 $lib.FullName) {
        if ($line -match '^\s+\d+\s+(\S+)$') { $v1[$Matches[1]] = $lib.Name }
    }
}
if ($v1.Count -lt 500) { throw "only $($v1.Count) symbols read from the FidelityFX 3.1 libraries" }

$v2Objects = "ffx_assert.obj", "ffx_message.obj", "ffx_object_management.obj", "ffx_backends_dx12.obj", "ffx_dx12.obj",
    "ffx_opticalflow.obj", "ffx_opticalflow_shaderblobs.obj"
$fromV2 = 0
foreach ($line in Get-Content $map) {
    if ($line -notmatch '^\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+[0-9a-f]{16}\s+.*\s(\S+)$') { continue }
    $symbol = $Matches[1]
    $object = $Matches[2]
    if ($object.Contains(":") -or $v2Objects -notcontains $object.ToLowerInvariant()) { continue }
    $fromV2++
    # String literals, float constants, exception tables and std templates are COMDATs: identical copies fold.
    if ($symbol -match '^(\?\?_C@|__real@|__xmm@|__ymm@|_TI|_CTA?\d|_CT\?)' -or $symbol -match '@std@@') { continue }
    if ($v1.ContainsKey($symbol)) { Fail "$symbol comes from $object but $($v1[$symbol]) defines it too" }
}
if ($fromV2 -lt 20) { Fail "only $fromV2 map symbols from the SDK v2 objects; map format not understood" }
Write-Host "ok: $fromV2 symbols from the SDK v2 objects, none shared with the FidelityFX 3.1 libraries"

$static = @()
foreach ($line in & dumpbin /nologo /dependents $dll) {
    if ($line -match 'delay load dependencies') { break }
    if ($line -match '^\s+(\S+\.dll)\s*$') { $static += $Matches[1].ToLowerInvariant() }
}
foreach ($name in "d3d9.dll", "d3d11.dll", "d3d12.dll", "dxgi.dll", "vulkan-1.dll", "opengl32.dll", "d3dcompiler_47.dll") {
    if ($static -contains $name) { Fail "$name is a static import" }
}
Write-Host "ok: static imports $($static -join ', ')"

& cl /nologo /EHsc /W4 tests\proxy_load_probe.cpp "/Fe$out\proxy_load_probe.exe" "/Fo$out\proxy_load_probe.obj" | Out-Null
if ($LASTEXITCODE -ne 0) { throw "probe build failed" }
function Probe([string]$source, [string]$name) {
    $dir = Join-Path $out $name
    Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    Copy-Item $source (Join-Path $dir "winmm.dll")
    $text = & "$out\proxy_load_probe.exe" (Join-Path $dir "winmm.dll")
    $text | Set-Content (Join-Path $dir "probe.txt")
    if ($LASTEXITCODE -ne 0) { throw "probe failed for $source" }
    return @($text | Where-Object { $_ -match '^after (\S+)=1$' } | ForEach-Object { ($_ -split '[ =]')[1] })
}
$loaded = Probe $dll "head"
Write-Host "head loads: $($loaded -join ', ')"
if (Test-Path $baseline) {
    $base = Probe ([System.IO.Path]::GetFullPath($baseline)) "baseline"
    Write-Host "0.4.9 loads: $($base -join ', ')"
    foreach ($name in $loaded) { if ($base -notcontains $name) { Fail "$name loads with the proxy, 0.4.9 does not load it" } }
} else {
    Write-Host "no baseline at $baseline; checking d3d11 and dxgi only"
    foreach ($name in "d3d11.dll", "dxgi.dll") { if ($loaded -contains $name) { Fail "$name loads with the proxy" } }
}

if ($failed) { Write-Host "FAILED: $failed"; exit 1 }
Write-Host "PASS"
exit 0
