# Runs tests\dx9_x86_smoke.cpp against the 32-bit D3D9 bridge staged in exports\dx9-x86 (see
# tools\test-dx9-x86.cmd, which builds it first). Each case writes its own OptiScaler.ini beside the
# host, clears the logs, runs the smoke program and checks its exit code and the logs. The whole set
# holds the GPU lock (wt\locks\gpu).
param(
    [string]$Runtime = 'E:\Games\The Sims 4\Game\Bin\dlssnr_amd_pass1.dll',
    [string]$Weights = 'E:\Games\The Sims 4\Game\Bin\dlssnr_on_amd_weights.bin',
    [string]$XeFG = 'E:\Projetos\opti_rr_dlss5\opti\dist\OptiScaler-0.4.9-amd-nr\OptiScaler',
    [string]$OptiScaler = '',
    [string[]]$Only = @()
)
$ErrorActionPreference = 'Stop'
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$root = Split-Path $PSScriptRoot -Parent
$stage = Join-Path $root 'exports\dx9-x86'
$host64 = Join-Path $stage 'OptiScaler64'
$smoke = Join-Path $stage 'dx9_x86_smoke.exe'
$lock = 'E:\Projetos\opti_rr_dlss5\wt\locks\gpu'

if (-not $OptiScaler) { $OptiScaler = Join-Path $root 'exports\release-local\OptiScaler.dll' }
Copy-Item $OptiScaler (Join-Path $host64 'OptiScaler.dll') -Force
foreach ($pair in @(@($Runtime, 'dlssnr_amd_pass1.dll'), @($Weights, 'dlssnr_on_amd_weights.bin'),
        @((Join-Path $XeFG 'libxess_fg.dll'), 'libxess_fg.dll'), @((Join-Path $XeFG 'libxell.dll'), 'libxell.dll'))) {
    $target = Join-Path $host64 $pair[1]
    if (-not (Test-Path $target) -or (Get-Item $pair[0]).Length -ne (Get-Item $target).Length) {
        Copy-Item $pair[0] $target -Force
    }
}

function Write-Ini([string[]]$lines) {
    $base = @('[Log]', 'LogToFile=true', 'LogLevel=1', '[Menu]', 'ShortcutKey=auto')
    Set-Content -Path (Join-Path $host64 'OptiScaler.ini') -Value ($base + $lines) -Encoding ascii
}
function Clear-Logs([string]$case) {
    foreach ($name in 'OptiScaler.log', 'OptiScaler.previous.log', 'amd_presr.log', 'dx9-host.log', 'dx9-x86.log',
        'dlssnr_on_amd.log') {
        Remove-Item (Join-Path $host64 $name) -ErrorAction SilentlyContinue
    }
    # The copies of the last run of this case, so none of them is taken for this run's.
    Get-ChildItem $stage -Filter "$case.*" -ErrorAction SilentlyContinue | Remove-Item -ErrorAction SilentlyContinue
}
# The size the last completed NR frames ran at, from amd_presr.log ('' when none completed).
function Last-NrSize {
    $path = Join-Path $host64 'amd_presr.log'
    if (-not (Test-Path $path)) { return '' }
    $last = Select-String -Path $path -Pattern 'Completed AMD pre-SR passes=\d+ at (\d+x\d+)' | Select-Object -Last 1
    if ($last) { return $last.Matches[0].Groups[1].Value }
    return ''
}
function Count([string]$log, [string]$text) {
    $path = Join-Path $host64 $log
    if (-not (Test-Path $path)) { return 0 }
    return @(Select-String -Path $path -SimpleMatch -Pattern $text).Count
}

Add-Type -AssemblyName System.Windows.Forms
$refresh = [int](Get-CimInstance Win32_VideoController | Where-Object { $_.CurrentRefreshRate } |
        Select-Object -First 1).CurrentRefreshRate
if (-not $refresh) { $refresh = 60 }
$results = [ordered]@{}
$screen = $false
$unhookedHosts = 0
# A case that needs keys runs again (twice at most) when another program held the foreground, and a timed
# case runs again when a host had to be replaced during it.
function Run-Case([string]$name, [string[]]$ini, [string[]]$arguments, [scriptblock]$check) {
    if ($Only.Count -and $Only -notcontains $name) { return }
    for ($try = 0; $try -lt 3; $try++) {
        Run-Once $name $ini $arguments $check
        if ($results[$name] -notlike '*skipped*' -and $results[$name] -notlike '*retry:*') { return }
        Write-Host "${name}: $($results[$name]); running it again"
    }
}
function Run-Once([string]$name, [string[]]$ini, [string[]]$arguments, [scriptblock]$check) {
    Write-Ini $ini
    Clear-Logs $name
    $out = Join-Path $stage "$name.out.txt"
    $process = Start-Process -FilePath $smoke -ArgumentList $arguments -WorkingDirectory $stage -PassThru `
        -NoNewWindow -RedirectStandardOutput $out
    $null = $process.Handle
    if (-not $process.WaitForExit(240000)) {
        $process.Kill()
        Get-Process OptiScalerDx9Host -ErrorAction SilentlyContinue | Stop-Process -Force
        $results[$name] = 'FAIL (no exit in 240 s)'
        return
    }
    Get-Process OptiScalerDx9Host -ErrorAction SilentlyContinue | Wait-Process -Timeout 15 -ErrorAction SilentlyContinue
    # A case that clipped the cursor and did not end by itself must not leave it clipped.
    [Windows.Forms.Cursor]::Clip = [Drawing.Rectangle]::Empty
    $text = Get-Content $out -Raw
    $bmp = Join-Path $stage "$name.bmp"
    if (Test-Path $bmp) {
        Add-Type -AssemblyName System.Drawing
        $image = [Drawing.Image]::FromFile($bmp)
        $image.Save((Join-Path $stage "$name.png"), [Drawing.Imaging.ImageFormat]::Png)
        $image.Dispose()
        Remove-Item $bmp
    }
    Start-Sleep -Milliseconds 500
    Get-ChildItem $host64 -Filter *.log | ForEach-Object {
        # A host still leaving can hold its log open for a moment; that copy is left out.
        try { Copy-Item $_.FullName (Join-Path $stage "$name.$($_.Name)") -Force } catch { } }
    $problems = @()
    if ($process.ExitCode -ne 0) { $problems += "exit $($process.ExitCode)" }
    if ($check) { $problems += @(& $check $text | Where-Object { $_ }) }
    if ($text -match 'SKIP ') { $problems += 'skipped: the test window could not take the foreground' }
    # A host OptiScaler did not hook is replaced; said in the result, and counted for the whole run.
    $unhooked = Count 'dx9-host.log' 'OptiScaler did not wrap the swap chain'
    if ($name -ne 'unhooked') { $script:unhookedHosts += $unhooked }
    $results[$name] = if ($problems.Count) { 'FAIL (' + ($problems -join '; ') + ')' } else { 'PASS' }
    if ($unhooked -and $name -ne 'unhooked') { $results[$name] += " ($unhooked host(s) without OptiScaler's hooks replaced)" }
    if ($text -match 'ms_per_frame=([0-9.]+)') { $results[$name] += " ($($Matches[1]) ms per frame)" }
    Write-Host "== $name : $($results[$name])"
    Write-Host $text
}

while ($true) {
    try { New-Item -ItemType Directory -Path $lock -ErrorAction Stop | Out-Null; break }
    catch {
        if ((Test-Path $lock) -and ((Get-Date) - (Get-Item $lock).CreationTime).TotalMinutes -gt 20) {
            Remove-Item $lock -Force -Recurse -ErrorAction SilentlyContinue
        }
        else { Start-Sleep -Seconds 20 }
    }
}
try {
    Run-Case 'native' @() @('--native', '--frames', '150') {
        param($text)
        $script:screen = $text -match 'screen=ok'
    }
    $screenArg = if ($screen) { @('--screen') } else { @() }
    if (-not $screen) { Write-Host 'Screen capture shows no change even for the native D3D9 path; screen checks are skipped.' }

    Run-Case 'bridge' @() (@('--frames', '300', '--expect-bridged', '--shot-at', '250', 'bridge.bmp') + $screenArg) {
        param($text)
        if ((Count 'OptiScaler.log' 'OptiScaler working as OptiScaler.dll') -eq 0) { 'OptiScaler.dll did not start in the host' }
        if ((Count 'dx9-host.log' 'presented') -eq 0) { 'the host logged no presented frame' }
    }
    Run-Case 'bridge-ex' @() (@('--ex', '--frames', '300', '--expect-bridged') + $screenArg) {
        param($text)
        if ($text -notmatch 'transport=1') { 'D3D9Ex did not use a shared texture' }
    }
    Run-Case 'shared-memory' @('[Dx9X86]', 'SharedMemory=1') (@('--frames', '300', '--expect-bridged') + $screenArg) {
        param($text)
        if ($text -notmatch 'transport=2') { 'the shared-memory transport was not used' }
    }
    $nr = @('[DlssNr]', 'Enabled=true', 'PresentWithoutUpscaler=true', 'NrBackend=daniel')
    Run-Case 'nr' $nr (@('--frames', '500', '--expect-bridged', '--key-at', '250', '0x65', '--key-at', '350', '0x65') + $screenArg) {
        param($text)
        $done = Count 'amd_presr.log' 'Completed AMD pre-SR passes'
        if ($done -lt 5) { "fewer than 240 NR frames completed ($done log lines)" }
        if ((Count 'OptiScaler.log' 'setting DlssNrEnabled to false') -eq 0) { 'NR was not turned off' }
    }
    # The window on a thread that is blocked while the game renders and presents (an engine with a render
    # thread): the child window must still come, and a lost host must still be replaced.
    Run-Case 'window-thread' $nr (@('--ex', '--window-thread', '--frames', '300', '--expect-bridged') + $screenArg) {
        param($text)
        if ((Count 'amd_presr.log' 'Completed AMD pre-SR passes') -lt 3) { 'NR did not run' }
    }
    Run-Case 'window-thread-restart' $nr @('--window-thread', '--frames', '900', '--vsync', '--work', '10', '--kill-host-at',
        '200') {
        param($text)
        if ((Count 'dx9-x86.log' 'host 2 connected') -eq 0) { 'no second host' }
    }
    $fg = $nr + @('[FrameGen]', 'Enabled=true', 'FGInput=upscaler', 'FGOutput=xefg', '[XeFG]', 'InterpolationCount=2')
    # At the display's rate (--vsync), so the frames after the Reset span the host restart that
    # XeFG's refused resize costs.
    Run-Case 'nr-fg' $fg (@('--frames', '1300', '--vsync', '--expect-bridged', '--expect-fg', '--key-at', '300',
            '0x23', '--key-at', '400', '0x23', '--reset-at', '550', '1600x900') + $screenArg) {
        param($text)
        if ((Count 'OptiScaler.log' 'Created Dx11wDx12SC') -eq 0) { 'no Dx11wDx12 swap chain' }
        if ((Count 'OptiScaler.log' 'XeFG swapchain created') -eq 0) { 'no XeFG swap chain' }
        $done = Count 'amd_presr.log' 'Completed AMD pre-SR passes'
        if ($done -lt 5) { "fewer than 240 NR frames completed ($done log lines)" }
    }
    # The archived bridge froze the picture when NR and frame generation were both turned off: turn
    # both off and sample the screen afterwards.
    Run-Case 'nr-fg-off' $fg (@('--frames', '500', '--expect-bridged', '--key-at', '300', '0x23', '--key-at', '320',
            '0x65') + $screenArg) {
        param($text)
        if ((Count 'OptiScaler.log' 'setting DlssNrEnabled to false') -eq 0) { 'NR was not turned off' }
        if ((Count 'OptiScaler.log' 'setting FGEnabled to false') -eq 0) { 'frame generation was not turned off' }
    }
    # The archived bridge lost frame generation when the NR model scale changed: open the menu, drag the
    # final-image Model scale slider (where the menu draws it at 1280x720; twice, as a scripted click is
    # sometimes missed) and keep going with XeFG on.
    Run-Case 'nr-scale-fg' $fg @('--frames', '900', '--expect-bridged', '--expect-fg', '--key-at', '300', '0x2D',
        '--drag-at', '340', '470', '395', '420', '395', '--drag-at', '380', '470', '395', '420', '395', '--shot-at', '420',
        'nr-scale-fg.bmp', '--key-at', '440', '0x2D') {
        param($text)
        $size = Last-NrSize
        if (-not $size -or $size -eq '1280x720') { 'NR did not run at a new model scale' }
    }
    # The same drag, by a menu that must follow the mouse the way the player sees it. 'menu-follow': the
    # cursor shows (a game's own menus); a first click on an empty spot, then the cursor moved to the
    # slider. The menu must follow the cursor, not raw moves counted from the first click. 'menu-pinned':
    # the game hides the cursor and holds it on one point outside the menu (Source in play re-centres
    # it), so the menu can follow the mouse only by the raw moves its input sink reads, here also on
    # XeFG's present thread. 'menu-steer': the cursor shows and reaches the slider by relative moves only, as a
    # player's mouse moves it (pointer acceleration, and a sweep past the window's top edge where the cursor
    # goes on and the window ends). 'menu-shown-pinned': the cursor shows but the game holds it on one point,
    # so only raw moves reach the slider. 'menu-hidden-absolute': the cursor is hidden and moved by absolute
    # moves (a pen tablet or a remote desktop), which bring no raw counts.
    $follow = @('--drag-at', '330', '200', '395', '200', '395', '--drag-at', '370', '470', '395', '420', '395')
    $drags = @('--drag-at', '340', '470', '395', '420', '395', '--drag-at', '380', '470', '395', '420', '395')
    $pinned = @('--hide-cursor', '--pin', '200', '395') + $drags
    $steer = @('--steer') + $drags
    $shownPinned = @('--pin', '200', '395') + $drags
    $hiddenAbsolute = @('--hide-cursor') + $follow
    foreach ($case in @(@('menu-follow', $nr, $follow), @('menu-follow-fg', $fg, $follow), @('menu-pinned', $nr, $pinned),
            @('menu-pinned-fg', $fg, $pinned), @('menu-steer', $nr, $steer), @('menu-steer-fg', $fg, $steer),
            @('menu-shown-pinned', $nr, $shownPinned), @('menu-hidden-absolute', $nr, $hiddenAbsolute))) {
        $arguments = @('--frames', '900', '--expect-bridged', '--key-at', '300', '0x2D') + $case[2] + @('--shot-at',
            '420', "$($case[0]).bmp", '--key-at', '440', '0x2D')
        if ($case[0] -like '*-fg') { $arguments += '--expect-fg' }
        Run-Case $case[0] $case[1] $arguments {
            param($text)
            # The model scale is read from the NR frames; a runtime that failed to start (a known, rare race
            # in its bootstrap) shows nothing about the menu.
            if ((Count 'amd_presr.log' 'Could not install AMD runtime bootstrap isolation') -gt 0) {
                'retry: the NR runtime did not start' }
            elseif ((Last-NrSize) -in @('', '1280x720')) { 'the menu did not follow the mouse to the Model scale slider' }
        }
    }
    # The window procedure is replaced at frame 50 by one that calls nothing before it (as an engine or
    # overlay may do after the device exists): the menu must still hold the game's input.
    Run-Case 'menu' $nr @('--frames', '300', '--subclass-at', '50', '--menu-at', '100', '--shot-at', '150',
        'menu.bmp') $null
    Run-Case 'reset' $nr (@('--frames', '400', '--expect-bridged', '--reset-at', '200', '1600x900') + $screenArg) {
        param($text)
        if ($text -notmatch 'hostStarts=1 ') { 'the host was restarted for a resize without frame generation' }
    }
    Run-Case 'restart-nr' $nr @('--frames', '900', '--vsync', '--work', '10', '--kill-host-at', '200') {
        param($text)
        if ((Count 'dx9-x86.log' 'host 2 connected') -eq 0) { 'no second host' }
    }
    Run-Case 'restart' $fg @('--frames', '900', '--vsync', '--work', '10', '--kill-host-at', '200', '--expect-fg') {
        param($text)
        if ((Count 'dx9-x86.log' 'host 2 connected') -eq 0) { 'no second host' }
    }
    # A key held before the menu opens and released while it is open: the game must see the release.
    Run-Case 'stuck-key' $nr @('--frames', '320', '--expect-bridged', '--hold-key', '60', '160', '0x57', '--key-at', '100',
        '0x2D', '--key-at', '220', '0x2D') $null
    # Text typed into the menu: Ctrl + click on the final-image Model scale slider opens its text field,
    # 0.6 and Enter set it, and NR must then run at 0.6 of 1280x720.
    Run-Case 'text-input' $fg @('--frames', '520', '--expect-bridged', '--expect-fg', '--key-at', '100', '0x2D',
        '--ctrl-click-at', '160', '470', '395', '--type-at', '200', '0.6', '--shot-at', '230', 'text-input.bmp',
        '--key-at', '400', '0x2D') {
        param($text)
        $size = Last-NrSize
        if ($size -ne '768x432') { "NR did not run at the typed model scale (last completed at '$size')" }
    }
    # The game releases its device and window and makes new ones: a new host for the new window.
    Run-Case 'recreate' @() (@('--frames', '500', '--expect-bridged', '--recreate-at', '200', '--shot-at', '480',
            'recreate.bmp') + $screenArg) {
        param($text)
        if ((Count 'dx9-x86.log' 'host 2 connected') -eq 0) { 'no host for the new window' }
    }
    Run-Case 'recreate-ex' @() (@('--ex', '--frames', '500', '--expect-bridged', '--recreate-at', '200') + $screenArg) {
        param($text)
        if ((Count 'dx9-x86.log' 'host 2 connected') -eq 0) { 'no host for the new window' }
    }
    Run-Case 'window-thread-fg' $fg (@('--ex', '--window-thread', '--frames', '600', '--expect-bridged', '--expect-fg') +
        $screenArg) {
        param($text)
        if ((Count 'OptiScaler.log' 'XeFG swapchain created') -eq 0) { 'no XeFG swap chain' }
    }
    Run-Case 'minimize-fg' $fg (@('--frames', '700', '--expect-bridged', '--expect-fg', '--minimize-at', '250', '350') +
        $screenArg) $null
    # A fullscreen request for half-rate vsync: the device must still be made (windowed, at interval one in
    # D3D9), and the host must present at interval 2, so a frame takes at least two refreshes.
    $halfRate = {
        param($text)
        if ((Count 'dx9-host.log' 'at interval 2') -eq 0) { 'the host did not present at interval 2' }
        if ($text -match 'ms_per_frame=([0-9.]+)' -and [double]$Matches[1] -lt 1600 / $refresh) {
            "frames came faster than half the display rate ($($Matches[1]) ms at $refresh Hz)"
        }
    }
    Run-Case 'fullscreen-interval2' @() (@('--fullscreen', '--interval', '2', '--frames', '200', '--expect-bridged') +
        $screenArg) $halfRate
    Run-Case 'fullscreen-interval2-ex' @() (@('--ex', '--fullscreen', '--interval', '2', '--frames', '200',
            '--expect-bridged') + $screenArg) $halfRate
    # A second device presenting to a second window stays native and costs no host; once the main window
    # goes away, the second window gets a host of its own.
    Run-Case 'two-devices' @() (@('--second-device', '--frames', '600', '--expect-bridged') + $screenArg) {
        param($text)
        if ((Count 'dx9-x86.log' 'presents to a new window') -ne 0) { 'the host was replaced for the second window' }
    }
    Run-Case 'two-devices-switch' @() (@('--second-device', '--switch-at', '200', '--frames', '500', '--expect-bridged') +
        $screenArg) {
        param($text)
        if ((Count 'dx9-x86.log' 'host 2 connected') -eq 0) { 'no host for the second window' }
    }
    # A program that keeps moving between two windows: quick moves count as failures, so the proxy stops
    # starting hosts and the program presents natively.
    Run-Case 'two-devices-flip' @() @('--second-device', '--flip-every', '300', '--work', '2', '--frames', '8000') {
        param($text)
        if ((Count 'dx9-x86.log' 'host failed 4 times in a row') -eq 0) { 'the session did not go native' }
    }
    # The child window asked for again after the host took one: still one child.
    Run-Case 'late-request' @() (@('--frames', '300', '--expect-bridged', '--late-request', '100') + $screenArg) $null
    Run-Case 'late-request-ex' @() (@('--ex', '--frames', '300', '--expect-bridged', '--late-request', '100') +
        $screenArg) $null
    # Under the menu, keys the game polls read up and the game's cursor clip is lifted.
    Run-Case 'menu-polls' $nr @('--frames', '300', '--menu-at', '100', '--poll-key', '0x51', '--clip') $null
    # Present with a source and a destination rectangle: natively, and through the host.
    Run-Case 'present-rects-native' @() (@('--native', '--present-rects', '--frames', '200') + $screenArg) $null
    Run-Case 'present-rects' @() (@('--present-rects', '--frames', '300', '--expect-bridged') + $screenArg) $null
    # The game's device reported lost for 30 frames, then reset: presented natively meanwhile, no host lost.
    Run-Case 'device-lost' $nr (@('--frames', '400', '--expect-bridged', '--lose-device-at', '150') + $screenArg) {
        param($text)
        if ((Count 'dx9-x86.log' "the game's device is lost") -eq 0) { 'the proxy did not log the lost device' }
    }

    # Camera motion: a still textured scene panned right at a known speed. Without NR the window shows the
    # same picture moved (the measure's own check); with NR, the motion OptiScaler's optical flow gives NR
    # and how much the picture changes beyond the move are reported.
    # Checked: the picture moved as the scene did (within one frame's move), less than $limit of 255 changed
    # beyond the move, and with NR, every motion line of the runtime log within 75 to 110 % of the pan.
    function Check-Pan([string]$text, [int]$pan, [double]$limit, [bool]$withNr) {
        if ($text -notmatch 'moved (\d+) px in \d+ frames \(the scene moved (\d+)\), mean change left ([0-9.]+)') {
            return 'no stability measure'
        }
        if ([math]::Abs([int]$Matches[1] - [int]$Matches[2]) -gt $pan) { "the picture moved $($Matches[1]) px, the scene $($Matches[2])" }
        if ([double]$Matches[3] -ge $limit) { "the picture changed by $($Matches[3]) of 255 beyond the move (limit $limit)" }
        if (-not $withNr) { return }
        # A runtime that did not start writes no log; that fails this case, not the whole run.
        $log = Join-Path $host64 'dlssnr_on_amd.log'
        $lines = if (Test-Path $log) { @(Select-String -Path $log -Pattern 'job \d+ motion: mean \|mv\| = \(([0-9.]+), ([0-9.]+)\)') } else { @() }
        if (-not $lines.Count) { 'no motion lines in the runtime log' }
        foreach ($line in $lines) {
            $x = [double]$line.Matches[0].Groups[1].Value
            $y = [double]$line.Matches[0].Groups[2].Value
            if ($x -lt 0.75 * $pan -or $x -gt 1.1 * $pan + 0.05 -or $y -gt 0.5) { "motion given to NR off the pan: $($line.Line)" }
        }
    }
    Run-Case 'pan-bridge' @() (@('--pan', '8', '--frames', '300', '--expect-bridged', '--measure-at', '200', '5') +
        $screenArg) { param($text) Check-Pan $text 8 0.5 $false }
    Run-Case 'still-nr' $nr @('--pan', '0', '--frames', '300', '--expect-bridged', '--measure-at', '200', '5') {
        param($text) Check-Pan $text 0 2 $true
    }
    Run-Case 'pan-nr' $nr (@('--pan', '8', '--frames', '400', '--expect-bridged', '--measure-at', '300', '5') +
        $screenArg) { param($text) Check-Pan $text 8 2 $true }
    Run-Case 'pan-fast-nr' $nr (@('--pan', '40', '--frames', '400', '--expect-bridged', '--measure-at', '300', '2') +
        $screenArg) { param($text) Check-Pan $text 40 2 $true }

    # What the screen shows with XeFG on (tests\screen_look.h): no rows of black while the scene pans, and NR in
    # every frame shown of a still scene at a quarter of the display rate (every generated frame is shown there),
    # as strong as with frame generation off, against the same scene with NR off.
    Run-Case 'look-pan-fg' $fg @('--ex', '--pan', '8', '--size', '1600x900', '--frames', '300', '--look-at', '250', '6',
        'look-pan-fg', '--expect-fg') $null
    $slow = @('--ex', '--pan', '0', '--size', '1600x900', '--work', [string][int](4000 / $refresh), '--frames', '60')
    $plain = @('[DlssNr]', 'Enabled=false', 'PresentWithoutUpscaler=true', 'NrBackend=daniel') + $fg[4..9]
    $lookValues = {
        param($text)
        if ($text -match 'p10 ([0-9.]+) p50 ([0-9.]+) p90 ([0-9.]+) mean ([0-9.]+)') {
            return @([double]$Matches[1], [double]$Matches[2], [double]$Matches[4])
        }
        return @(0, 0, 0)
    }
    $script:nrStrength = 0
    Run-Case 'look-still-plain' $plain ($slow + @('--look-at', '45', '6', 'look-still-plain', '--expect-fg')) $null
    Run-Case 'look-still-nr' $nr ($slow + @('--look-at', '45', '6', 'look-still-nr', '--look-ref', 'look-still-plain.ref')) {
        param($text)
        $script:nrStrength = (& $lookValues $text)[1]
        if ($script:nrStrength -lt 1) { "NR did not change the still scene (p50 $($script:nrStrength))" }
    }
    Run-Case 'look-still-nr-fg' $fg ($slow + @('--look-at', '45', '6', 'look-still-nr-fg', '--look-ref',
            'look-still-plain.ref', '--expect-fg')) {
        param($text)
        $v = & $lookValues $text
        if ($v[0] -lt 0.75 * $script:nrStrength -or $v[2] -lt 0.85 * $script:nrStrength) {
            "NR missing from frames shown with FG: p10 $($v[0]), mean $($v[2]), against p50 $($script:nrStrength) without FG"
        }
    }

    # OptiScaler without its DXGI hooks (no overlay menu, no frame generation, no DXGI spoofing): each host
    # must report it, be replaced, and after four the game presents natively.
    Run-Case 'unhooked' @('[Menu]', 'OverlayMenu=false', '[Spoofing]', 'Dxgi=false') (@('--frames', '200',
            '--expect-unhooked', '--timeout', '180') + $screenArg) {
        param($text)
        if ((Count 'dx9-host.log' 'OptiScaler did not wrap the swap chain') -lt 4) { 'the hosts did not report it' }
    }

    # Frame time at 1920x1080 with 8 ms of game CPU per frame: native, the bridge, NR, each pipelined
    # and waiting for the host at every Present. The time is reported, not judged; each case must have
    # done what it times, in one host.
    $timing = @('--size', '1920x1080', '--frames', '300', '--work', '8')
    $oneHost = {
        param($text)
        if ($text -notmatch 'hostStarts=1 faults=0 ') { 'retry: the host was replaced during the timed frames' }
        if ((Count 'dx9-host.log' 'presented') -eq 0) { 'the host logged no presented frame' }
    }
    $timedNr = {
        param($text)
        & $oneHost $text
        $done = Count 'amd_presr.log' 'Completed AMD pre-SR passes'
        if ($done -lt 3) { "NR did not run ($done completed log lines)" }
        if ((Last-NrSize) -ne '1920x1080') { "NR did not run at 1920x1080 (last completed at '$(Last-NrSize)')" }
    }
    Run-Case 'time-native' @() (@('--native') + $timing) $null
    Run-Case 'time-bridge' @() ($timing + @('--expect-bridged')) $oneHost
    Run-Case 'time-bridge-sync' @('[Dx9X86]', 'Pipelined=false') ($timing + @('--expect-bridged')) $oneHost
    Run-Case 'time-nr' $nr ($timing + @('--expect-bridged')) $timedNr
    Run-Case 'time-nr-sync' ($nr + @('[Dx9X86]', 'Pipelined=false')) ($timing + @('--expect-bridged')) $timedNr
    Run-Case 'time-nr-ex' $nr ($timing + @('--ex', '--expect-bridged')) $timedNr
    Run-Case 'time-nr-fg' $fg ($timing + @('--expect-bridged', '--expect-fg')) $timedNr
}
finally {
    Remove-Item $lock -Force -Recurse -ErrorAction SilentlyContinue
}

Write-Host ''
Write-Host "hosts that came up without OptiScaler's DXGI hooks (outside the 'unhooked' case): $unhookedHosts"
$failed = 0
foreach ($entry in $results.GetEnumerator()) {
    Write-Host ("{0,-18} {1}" -f $entry.Key, $entry.Value)
    if ($entry.Value -notlike 'PASS*') { $failed++ }
}
exit [int]($failed -ne 0)
