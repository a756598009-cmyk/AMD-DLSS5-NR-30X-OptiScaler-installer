# Long runs of the Vulkan final-image route to find what makes a danielblnc network job hold the GPU for
# seconds (Detroit: Become Human, 2026-10-03: one job of 3646 ms at 4X with the menu open, right after
# Encoding was changed three times). Each scenario runs the "screen soak" mode of vulkan_present_smoke for
# the given minutes, restarting the game when the stall stand-down turns NR off, and the summary counts the
# runtime's SPIKE lines per scenario.
#   tools\soak-vulkan-nr.ps1 -Minutes 40
# Needs the folder tools\test-vulkan-present.cmd prepared (exports\vulkan-present-screen) and an unlocked
# desktop: the game window goes borderless over the monitor and the menu is opened with real key input.
param(
    [int]$Minutes = 40,
    [string[]]$Scenarios = @('4x-menu-cycle', '4x-menu', '3x-cycle', '3x'),
    [string]$GpuLock = 'E:\Projetos\opti_rr_dlss5\wt\locks\gpu'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$src = 'exports\vulkan-present-screen'
$out = 'exports\vulkan-soak'
New-Item -ItemType Directory -Force $out | Out-Null

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$build = "call `"$vs\VC\Auxiliary\Build\vcvarsall.bat`" x64 >nul && cl /nologo /std:c++20 /EHsc /W4 /utf-8 " +
    "/Iexternal\vulkan\include tests\vulkan_present_smoke.cpp /Fe$out\vulkan_present_smoke.exe /Fo$out\vulkan_present_smoke.obj " +
    "/link OptiScaler\library\vulkan\vulkan-1.lib user32.lib gdi32.lib d3d11.lib dxgi.lib dbghelp.lib"
cmd /c $build | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'soak build failed' }
Copy-Item "$src\*.dll", "$src\*.bin" $out -Force
Copy-Item 'exports\release-local\OptiScaler.dll', 'exports\release-local\OptiScaler.pdb' $out -Force

function Ini([int]$generated) {
    @('[FrameGen]', 'Enabled=true', 'FGInput=upscaler', 'FGOutput=xefg', '[XeFG]', "InterpolationCount=$generated",
      '[DlssNr]', 'Enabled=true', 'PresentWithoutUpscaler=true', 'NrBackend=daniel', 'AmdModelScale=1',
      '[Menu]', 'ShowFps=true', '[Log]', 'LogToFile=true', 'LogLevel=2') -join "`r`n"
}

$summary = @()
foreach ($name in $Scenarios) {
    $generated = if ($name -like '4x*') { 3 } else { 2 }
    $menu = if ($name -like '*menu*') { 1 } else { 0 }
    $cycle = if ($name -like '*cycle*') { 4 } else { 0 }
    $dir = Join-Path $out $name
    New-Item -ItemType Directory -Force $dir | Out-Null
    Copy-Item "$out\*.dll", "$out\*.bin", "$out\*.pdb", "$out\vulkan_present_smoke.exe" $dir -Force
    Set-Content -LiteralPath "$dir\OptiScaler.ini" -Value (Ini $generated) -Encoding ascii
    Remove-Item "$dir\*.log" -ErrorAction SilentlyContinue
    $deadline = (Get-Date).AddMinutes($Minutes)
    $runs = 0; $standDowns = 0; $spikes = @()
    while ((Get-Date) -lt $deadline) {
        $left = [math]::Max(1, [int][math]::Ceiling(($deadline - (Get-Date)).TotalMinutes))
        while (-not (New-Item -ItemType Directory $GpuLock -ErrorAction SilentlyContinue)) {
            if (((Get-Date) - (Get-Item $GpuLock).CreationTime).TotalMinutes -gt 20 + $Minutes) { Remove-Item $GpuLock -Recurse -Force }
            Start-Sleep 20
        }
        try {
            $p = Start-Process -FilePath "$dir\vulkan_present_smoke.exe" -WorkingDirectory $dir -NoNewWindow -PassThru `
                -ArgumentList "screen soak $left $menu $cycle" -RedirectStandardOutput "$dir\run$runs.txt"
            $null = $p.Handle
            if (-not $p.WaitForExit(($left + 5) * 60000)) { $p.Kill(); "timeout" | Add-Content "$dir\run$runs.txt" }
            if ($p.ExitCode -eq 2) { $standDowns++ }
            elseif ($p.ExitCode -ne 0) { Write-Host "$name run $runs exit $($p.ExitCode)"; Get-Content "$dir\run$runs.txt" -Tail 5 }
        } finally { Remove-Item $GpuLock -Recurse -Force -ErrorAction SilentlyContinue }
        $runs++
        if ($p.ExitCode -ne 0 -and $p.ExitCode -ne 2) { break }
        # The runtime's log goes on in the same folder; keep the session's lines apart before the next start.
        if (Test-Path "$dir\dlssnr_on_amd.log") { Move-Item "$dir\dlssnr_on_amd.log" "$dir\dlssnr_on_amd.$runs.log" -Force }
        if (Test-Path "$dir\OptiScaler.log") { Move-Item "$dir\OptiScaler.log" "$dir\OptiScaler.$runs.log" -Force }
    }
    $jobs = 0
    foreach ($log in Get-ChildItem "$dir\dlssnr_on_amd.*.log") {
        foreach ($line in Select-String -LiteralPath $log.FullName -Pattern 'SPIKE at .* took (\d+) ms') {
            $spikes += [int]$line.Matches[0].Groups[1].Value
        }
        $last = Select-String -LiteralPath $log.FullName -Pattern 'network job (\d+) done' | Select-Object -Last 1
        if ($last) { $jobs += [int]$last.Matches[0].Groups[1].Value }
    }
    $row = [pscustomobject]@{
        scenario = $name; minutes = $Minutes; runs = $runs; standDowns = $standDowns; jobs = $jobs
        spikes = $spikes.Count; over1s = @($spikes | Where-Object { $_ -ge 1000 }).Count
        maxMs = if ($spikes) { ($spikes | Measure-Object -Maximum).Maximum } else { 0 }
    }
    $summary += $row
    $row | Format-List | Out-String | Write-Host
    $summary | ConvertTo-Json | Set-Content "$out\summary.json"
}
$summary | Format-Table | Out-String | Write-Host
