# produce-rdna2.ps1 -Shaders <shaders-rdna2 dir> -Out <dir> [-Dll <runtime export>] [-Family navi21] [-Layouts <file>]
#
# The RDNA2 network's ACO bundle in one step: <Out>\aco-gfx103x\shaders (the given shaders-rdna2, as it is) and
# <Out>\aco-gfx103x\records (one gfx10.3 record per network kernel), which the runtime takes from
# dlssnr-amd\aco-gfx103x (MochizukiNrRuntime.cpp Rdna2Aco). Steps:
#  1. layouts: the runtime of -Dll (a build with MZ_LOG_LAYOUT, rdna2-aco or later) runs the RDNA2 network forced
#     (MZ_FORCE_RDNA2=1) with these shaders at each size, both models, Style off, Natural and Cinematic, 2 frames each, and its
#     "[nr] layout" lines become <Out>\layouts.txt (-Layouts <file> skips this: no GPU needed then);
#  2. records: produce.py <Family> <Shaders> <Out> --layouts layouts.txt (Mesa 25.3.6 RADV, no GPU);
#  3. <Out>\summary.txt: records made, refused kernels with the reason and ACO's VGPRs / scratch.
# The layout runs take E:\Projetos\opti_rr_dlss5\gpu-timing.lock (atomic create, deleted after) unless -NoLock.
param(
    [Parameter(Mandatory)][string]$Shaders,
    [Parameter(Mandatory)][string]$Out,
    [string]$Dll = (Join-Path $PSScriptRoot '..\..\exports\mochizuki-rdna2-aco'),
    [string]$Family = 'navi21',
    [string]$Layouts,
    [string[]]$Sizes = @('1152x648', '1280x720', '1920x1080', '2560x1440'),
    [string]$E2E = 'E:\Projetos\opti_rr_dlss5\wt-rdna2\exports\rdna2-work\mz_e2e.exe',
    [string]$Kit = 'E:\Projetos\opti_rr_dlss5\wt-rdna3\exports\rx7000-kit',
    [int]$Jobs = 6,
    [switch]$NoLock
)
$ErrorActionPreference = 'Stop'
$Shaders = (Resolve-Path $Shaders).Path
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$layoutFile = Join-Path $Out 'layouts.txt'

if ($Layouts) { Copy-Item $Layouts $layoutFile -Force }
else {
    $run = Join-Path $Out 'layout-run'
    if (Test-Path $run) { Remove-Item -Recurse -Force $run }
    New-Item -ItemType Directory -Force "$run\dlssnr-amd" | Out-Null
    Copy-Item "$Dll\MochizukiNrRuntime.dll" $run
    foreach ($s in 'shaders', 'shaders-rdna3') { Copy-Item -Recurse "$Dll\dlssnr-amd\$s" "$run\dlssnr-amd\$s" }
    Copy-Item -Recurse $Shaders "$run\dlssnr-amd\shaders-rdna2"
    foreach ($m in 'dlssnr.bin', 'dlssnr-small.bin') {
        try { New-Item -ItemType HardLink -Path "$run\dlssnr-amd\$m" -Target "$Kit\refwork\dlssnr-amd\$m" | Out-Null }
        catch { Copy-Item "$Kit\refwork\dlssnr-amd\$m" "$run\dlssnr-amd\$m" }   # another drive
    }
    Copy-Item $E2E $run
    Copy-Item "$Kit\OpenNR-RX7000-test\files\test.png" $run
    $lock = 'E:\Projetos\opti_rr_dlss5\gpu-timing.lock'
    if (-not $NoLock) {
        while ($true) {
            try { $fs = [IO.File]::Open($lock, 'CreateNew', 'Write'); $w = New-Object IO.StreamWriter($fs); $w.Write("produce-rdna2 layout runs $(Get-Date -Format s)"); $w.Close(); break }
            catch { Write-Host 'waiting for the GPU lock'; Start-Sleep -Seconds 20 }
        }
    }
    $keep = @{}
    foreach ($k in 'MZ_FORCE_RDNA2', 'MZ_LOG_LAYOUT', 'MZ_RDNA2_CHECK', 'E2E_W', 'E2E_H', 'E2E_FLAGS', 'E2E_RENO_STYLE') { $keep[$k] = [Environment]::GetEnvironmentVariable($k) }
    Push-Location $run
    try {
        $env:MZ_FORCE_RDNA2 = '1'; $env:MZ_LOG_LAYOUT = '1'; $env:MZ_RDNA2_CHECK = '0'
        $lines = @()
        foreach ($model in @(@('small', '0'), @('large', '8192'))) { foreach ($size in $Sizes) { foreach ($style in '', '1', '2') {
            $w, $h = $size.Split('x')
            $env:E2E_W = $w; $env:E2E_H = $h; $env:E2E_FLAGS = $model[1]; $env:E2E_RENO_STYLE = $style
            $name = "$($model[0])_$($size)$(if ($style) { '_style' })"
            Remove-Item mochizuki_nr.log -ErrorAction SilentlyContinue
            cmd /c ".\mz_e2e.exe .\MochizukiNrRuntime.dll . test.png $name.bin 2 1 > $name.txt 2>&1"
            $n = @(Select-String -Path mochizuki_nr.log -Pattern '\[nr\] layout ' -ErrorAction SilentlyContinue).Count
            Write-Host "layout run $name exit $LASTEXITCODE, $n layout lines"
            if (Test-Path mochizuki_nr.log) { Copy-Item mochizuki_nr.log "$name.log"; $lines += Get-Content mochizuki_nr.log | Where-Object { $_ -match '\[nr\] layout ' } }
        } } }
        $lines | ForEach-Object { 'layout ' + ($_ -split '\[nr\] layout ', 2)[1] } | Sort-Object -Unique | Set-Content $layoutFile
    }
    finally {
        Pop-Location
        foreach ($k in $keep.Keys) { [Environment]::SetEnvironmentVariable($k, $keep[$k]) }
        if (-not $NoLock) { Remove-Item $lock -Force -ErrorAction SilentlyContinue }
    }
}
Write-Host "$(@(Get-Content $layoutFile).Count) layouts in $layoutFile"

$bundle = Join-Path $Out 'aco-gfx103x'
if (Test-Path $bundle) { Remove-Item -Recurse -Force $bundle }
python -I (Join-Path $PSScriptRoot 'produce.py') $Family $Shaders $Out --layouts $layoutFile --jobs $Jobs
if ($LASTEXITCODE) { throw "produce.py failed ($LASTEXITCODE)" }
New-Item -ItemType Directory -Force $bundle | Out-Null
Copy-Item -Recurse $Shaders "$bundle\shaders"
Move-Item "$Out\records" "$bundle\records"

$log = Get-Content "$Out\produce-log.txt"
$ok = @($log | Where-Object { $_ -like 'OK *' })
$no = @($log | Where-Object { $_ -like 'REJECT *' })
$sum = @("$($ok.Count) records ($Family) for $Shaders, $($no.Count) kernels left to the driver", '', 'refused:')
$sum += $no | ForEach-Object { '  ' + ($_ -replace '^REJECT ', '') }
$sum += '', 'records (name vgpr lds user inlines):'
$sum += $ok | ForEach-Object {
    $f = @{}; foreach ($kv in ($_ -replace '^OK ', '') -split ' ') { $p = $kv.Split('=', 2); if ($p.Count -eq 2) { $f[$p[0]] = $p[1] } }
    '  {0} vgpr {1} lds {2} user {3} inlines {4}' -f $f.name, $f.vgpr, $f.lds, $f.user, $f.inlines
}
$sum | Set-Content "$Out\summary.txt"
$sum | Select-Object -First 3 | Write-Host
Write-Host "bundle: $bundle (copy it to dlssnr-amd\aco-gfx103x)"
