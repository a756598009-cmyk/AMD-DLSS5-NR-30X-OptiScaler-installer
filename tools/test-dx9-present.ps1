# Runs tests\dx9_present_smoke.cpp with exports\release-local\OptiScaler.dll as d3d9.dll beside it and checks
# the test's own output and OptiScaler.log. See test-dx9-present.cmd. The cases that change settings the way the menu
# does need exports\release-local\OptiScaler.pdb (tools\build-optiscaler-fast.cmd writes it).
param(
    [string]$Case = "",
    [string]$Runtime = "exports\dx9-smoke\runtime",
    [string]$XeFG = "..\..\opti\dist\OptiScaler-0.4.9-amd-nr\OptiScaler",
    [string]$Lock = "",
    # Added to every case's OptiScaler.ini, for one-off comparisons: "[Section]~Key=value~..."
    [string]$ExtraIni = "",
    [int]$TimeoutSeconds = 240
)
$ErrorActionPreference = "Stop"
Set-Location (Split-Path -Parent $PSScriptRoot)

$out = [System.IO.Path]::GetFullPath("exports\dx9-smoke")
New-Item -ItemType Directory -Force -Path $out | Out-Null
$exe = Join-Path $out "dx9_present_smoke.exe"
& cl /nologo /std:c++20 /EHsc /O2 /W4 /utf-8 tests\dx9_present_smoke.cpp "/Fe$exe" "/Fo$(Join-Path $out 'dx9_present_smoke.obj')" user32.lib gdi32.lib dbghelp.lib
if ($LASTEXITCODE -ne 0) { throw "cl failed" }

$opti = [System.IO.Path]::GetFullPath("exports\release-local\OptiScaler.dll")
$pdb = [System.IO.Path]::ChangeExtension($opti, "pdb")
foreach ($file in @($opti, $pdb, (Join-Path $Runtime "dlssnr_amd_pass1.dll"), (Join-Path $Runtime "dlssnr_on_amd_weights.bin"))) {
    if (-not (Test-Path $file)) { throw "missing $file" }
}

# Menu (Insert), NR (Numpad 5) and FG (End) keys, a Reset and screen samples. mark= starts a timed phase.
$nrScript = "90:mark=nr-on,150:motion=nr-on,170:menu,190:shot=menu-open,200:menu,215:shot=menu-closed," +
            "230:nr,240:mark=nr-off,300:motion=nr-off,310:nr,320:mark=nr-on-again,380:reset=1600x900," +
            "390:mark=after-reset,400:menu,415:shot=menu-after-reset,425:menu,450:motion=after-reset,460:shot=after-reset"
# With FG, the menu's Model scale slider is set to 0.51 with the menu open (the value the slider writes).
$fgScript = "150:mark=fg-on,200:motion=fg-on,220:menu,232:shot=fg-menu-open,235:float=AmdNrScale=0.51," +
            "244:shot=fg-scale-set,250:menu,265:shot=fg-menu-closed,280:fg,290:mark=fg-off,330:motion=fg-off," +
            "340:fg,350:mark=fg-on-again,420:motion=fg-on-again,430:nr,440:mark=nr-off,480:motion=nr-off,490:nr," +
            "500:mark=nr-on,580:motion=nr-on,650:reset=1600x900,660:mark=after-reset,760:motion=after-reset," +
            "765:shot=fg-after-reset,780:minimize,820:restore,840:menu,850:shot=fg-menu-after-restore,860:menu," +
            "900:motion=after-restore"
$nrLog = @("OptiScaler working as d3d9.dll", "D3D9 device hooks installed", "D3D9 bridge: D3D12 device",
           "D3D9 menu ready", "D3D9 bridge frame \d+: menu open", "D3D9 bridge frame \d+: menu closed",
           "D3D9 bridge frame \d+: D3D9 present, NR true", "NR without upscaling \(FidelityFX Optical Flow",
           "Reset.*: 0, 1600x900", "D3D9 bridge: 1600x900", "D3D9 bridge frame 4\d\d: menu open")
$fgLog = @("OptiScaler working as d3d9.dll", "D3D9 device hooks installed", "D3D9 bridge: XeFG presenter",
           "D3D9 bridge frame \d+: menu open", "D3D9 bridge frame \d+: menu closed",
           "D3D9 bridge frame 2[89]\d: frame generation off", "D3D9 bridge frame 3[4-9]\d: frame generation on",
           "D3D9 bridge frame 4[3-9]\d: frame generation off", "D3D9 bridge frame [45]\d\d: frame generation on",
           "D3D9 bridge frame 601: XeFG presenter, NR true at scale 0\.51, FG true, XeFG last present 3 frames",
           "Reset.*: 0, 1600x900", "XeFG presenter 1280x720 -> 1600x900: 0", "D3D9 bridge frame 8[4-9]\d: menu open",
           "D3D9 bridge frame 23\d: NR scale 1.00 -> 0.51")

# Generation stays on through the scale change.
$fgNotLog = @("D3D9 bridge frame 2[0-7]\d: frame generation off")

$cases = @(
    # Final image off (the default): the D3D9 route only draws the menu, no D3D12 device.
    @{ Name = "menu-only"; Args = @(); Fg = $false; Present = "false"; Frames = 260; Script = "170:menu,190:shot=menu-only,200:menu"
       Log = @("D3D9 menu ready", "D3D9 bridge frame \d+: menu open", "D3D9 bridge frame \d+: menu closed",
               "D3D9 bridge frame 1: D3D9 present, NR false")
       NotLog = @("D3D9 bridge: D3D12 device") },
    @{ Name = "classic-nr"; Args = @(); Fg = $false; Frames = 470; Script = $nrScript; Log = $nrLog },
    # With the scene still, the screen with NR differs from the screen without it, and two frames without NR match.
    @{ Name = "nr-visible"; Args = @(); Fg = $false; Frames = 200
       Script = "60:still,140:shot=nr-on,150:nr,165:shot=nr-off,170:diff=nr-off,175:diff=nr-on,180:nr"
       Log = @("D3D9 bridge frame 1: D3D9 present, NR true")
       Out = @("diff nr-off: [0-9.]+ of the pixels changed, mean difference 0\.[0-4]",
               "diff nr-on: [0-9.]+ of the pixels changed, mean difference (?:[1-9]|0\.[5-9])") },
    @{ Name = "ex-nr"; Args = @("--ex"); Fg = $false; Frames = 470; Script = $nrScript; Log = $nrLog },
    @{ Name = "chain-nr"; Args = @("--chain"); Fg = $false; Frames = 470; Script = $nrScript; Log = $nrLog },
    @{ Name = "msaa-nr"; Args = @("--msaa"); Fg = $false; Frames = 470; Script = $nrScript; Log = $nrLog },
    @{ Name = "classic-fg"; Args = @(); Fg = $true; Frames = 910; Script = $fgScript; Log = $fgLog; NotLog = $fgNotLog },
    @{ Name = "ex-fg"; Args = @("--ex"); Fg = $true; Frames = 910; Script = $fgScript; Log = $fgLog; NotLog = $fgNotLog },
    @{ Name = "msaa-fg"; Args = @("--msaa"); Fg = $true; Frames = 910; Script = $fgScript; Log = $fgLog; NotLog = $fgNotLog },
    # The menu's Frame Generation section is opened with XeFG running over a still scene (the section the menu's
    # sidebar button selects, written as the button does): the menu changes on screen and generation stays on.
    # Opening that tab turned frame generation off in the archived D3D9 build.
    @{ Name = "fg-section"; Args = @(); Fg = $true; Frames = 320
       Script = "130:still,150:menu,160:shot=fg-section-before,163:diff=fg-section-before,165:tab=2," +
                "180:shot=fg-section,183:diff=fg-section-before,200:menu,210:still,300:motion=after"
       Log = @("D3D9 bridge frame \d+: frame generation on", "D3D9 bridge frame \d+: menu open",
               "D3D9 bridge frame 301: XeFG presenter, NR true at scale 1.00, FG true, XeFG last present 3 frames")
       NotLog = @("D3D9 bridge frame (1[5-9]\d|2\d\d|3\d\d): frame generation off")
       Out = @("(?s)diff fg-section-before: [0-9.]+ of the pixels changed, mean difference 0\.[0-4].*" +
               "diff fg-section-before: (?:0\.0[2-9]|0\.[1-9])\d* of the pixels changed") },
    # The menu's "Final image" checkbox off and on again with the XeFG presenter: the presenter stays and keeps
    # showing frames (without NR and generation), the D3D9 menu host does not start, and NR and generation come back.
    @{ Name = "final-off-fg"; Args = @(); Fg = $true; Frames = 700
       Script = "150:motion=before,200:bool=DlssNrPresent=0,260:motion=final-off,280:menu,295:shot=final-off-menu," +
                "300:motion=final-off-menu,310:menu,350:bool=DlssNrPresent=1,500:motion=final-on,600:motion=after"
       Log = @("D3D9 bridge frame 2\d\d: frame generation off", "D3D9 bridge frame 301: XeFG presenter, NR false",
               "D3D9 bridge frame 2[89]\d: menu open", "D3D9 bridge frame [34]\d\d: frame generation on",
               "D3D9 bridge frame 601: XeFG presenter, NR true at scale 1.00, FG true, XeFG last present 3 frames")
       NotLog = @("releasing the XeFG presenter", "D3D9 menu ready") },
    # FG Output set to none in the menu's Frame Generation section with the XeFG presenter: the menu resets the FG
    # input, generation stops and the frames go on. After exclusive fullscreen the window gets a plain DXGI presenter,
    # as D3D9 cannot show a windowed frame there any more.
    @{ Name = "fg-output-none"; Args = @(); Fg = $true; Frames = 700
       Script = "150:motion=before,195:tab=2,200:menu,230:int=FGOutput=0,260:motion=output-none,280:menu," +
                "300:motion=menu-closed,350:fullscreen,380:motion=fullscreen,400:windowed,500:motion=windowed," +
                "520:menu,535:shot=output-none-menu,545:menu,600:motion=after"
       Log = @("Resetting FGInput to NoFG", "D3D9 bridge frame 2[3-5]\d: frame generation off",
               "(?s)Resetting FGInput.*releasing the XeFG presenter.*Reset: 0, 1280x720, windowed 1.*DXGI presenter \w+ on window",
               "D3D9 bridge frame 601: DXGI presenter, NR true at scale 1.00, FG false", "D3D9 bridge frame 5[2-4]\d: menu open")
       NotLog = @("D3D9 bridge frame (2[6-9]\d|[3-6]\d\d): frame generation on", "no XeFG swapchain") },
    # Picking Auto, then 2X, then Auto again in the MFG combo (the values it writes): auto stays at two generated frames
    # per frame on this route.
    @{ Name = "fg-count"; Args = @(); Fg = $true; Frames = 920
       Script = "200:unset=FGXeFGInterpolationCount,350:int=FGXeFGInterpolationCount=1,650:unset=FGXeFGInterpolationCount,900:motion=end"
       Log = @("D3D9 bridge frame 301: XeFG presenter, NR true at scale 1.00, FG true, XeFG last present 3 frames",
               "D3D9 bridge frame 601: XeFG presenter, NR true at scale 1.00, FG true, XeFG last present 2 frames",
               "D3D9 bridge frame 901: XeFG presenter, NR true at scale 1.00, FG true, XeFG last present 3 frames",
               "(?s)Interpolation count changed -1 -> 2.*Interpolation count changed 2 -> 1.*Interpolation count changed 1 -> 2") },
    # A second device on the presenter's window, both presenting every frame: the bridge stays with the first one and
    # generation stays on.
    @{ Name = "dual-fg"; Args = @(); Fg = $true; Frames = 700
       Script = "150:motion=before,200:dual,260:motion=dual,600:motion=after"
       Log = @("D3D9 bridge frame 601: XeFG presenter, NR true at scale 1.00, FG true, XeFG last present 3 frames")
       Out = @("dual: second device 00000000")
       NotLog = @("(?s)following device.*following device", "D3D9 bridge frame [2-6]\d\d: frame generation off") },
    # The game moves to a new window: after two quiet seconds on the old one the bridge follows it. With FG the
    # presenter moves to the new window and frame generation comes back there.
    @{ Name = "window-nr"; Args = @(); Fg = $false; Frames = 700
       Script = "150:window2,151:cpu=6,600:cpu=0,620:menu,635:shot=window2-menu,645:menu,680:motion=window2"
       Log = @("(?s)following device \w+, window \w+.*following device \w+, window \w+.*D3D9 menu ready.*menu open") },
    @{ Name = "window-fg"; Args = @(); Fg = $true; Frames = 700
       Script = "150:window2,151:cpu=6,600:cpu=0,620:menu,635:shot=window2-menu,645:menu,680:motion=window2"
       Log = @("(?s)XeFG presenter \w+ on window (\w+).*releasing the XeFG presenter.*XeFG presenter \w+ on window (?!\1)\w+.*frame generation on.*menu open",
               "D3D9 bridge frame 301: XeFG presenter, NR true at scale 1.00, FG true, XeFG last present 3 frames") },
    # The game releases its device and creates another on the same window. The XeFG presenter follows the new device
    # at once (the screen keeps changing right after it); without it the bridge follows after two quiet seconds.
    @{ Name = "recreate-fg"; Args = @(); Fg = $true; Frames = 700
       Script = "150:motion=before,200:recreate,203:motion=recreated,400:menu,415:shot=recreate-menu,425:menu,600:motion=after"
       Log = @("(?s)following device \w+, window (\w+).*following device \w+, window \1.*frame generation on",
               "D3D9 bridge frame 4\d\d: menu open")
       NotLog = @("releasing the XeFG presenter") },
    @{ Name = "recreate-nr"; Args = @(); Fg = $false; Frames = 900
       Script = "150:motion=before,200:recreate,201:cpu=6,210:motion=gap,700:cpu=0,720:menu,735:shot=recreate-menu,745:menu,800:motion=after"
       Log = @("(?s)following device \w+, window (\w+).*following device \w+, window \1.*D3D9 menu ready.*menu open") },
    # The D3D12 device is removed while frames go through it: the bridge releases the presenter and D3D9 goes on
    # presenting, with the menu on the D3D9 device and without NR. The window keeps the presenter's last frame (D3D9
    # cannot show a windowed frame where a flip-model swapchain presented), so this case only checks that the game
    # carries on: no crash, no failed Present, the menu toggles.
    @{ Name = "removed-fg"; Args = @(); Fg = $true; Frames = 600; Errors = $true; Frozen = $true
       Script = "150:motion=before,200:remove12=4,260:motion=removed,300:menu,315:shot=removed-menu,325:menu,500:motion=after"
       Log = @("(?s)D3D12 device was removed.*releasing the XeFG presenter; frame generation stays off.*D3D9 menu ready.*menu open.*menu closed")
       Out = @("D3D12 device removed: 00000000") },
    @{ Name = "removed-nr"; Args = @(); Fg = $false; Frames = 600; Errors = $true
       Script = "150:motion=before,200:remove12=4,260:motion=removed,300:menu,315:shot=removed-menu,325:menu,500:motion=after"
       Log = @("(?s)D3D12 device was removed.*menu open")
       Out = @("D3D12 device removed: 00000000") },
    # A D3D9Ex FLIPEX swapchain holds the window's flip-model presentation, so XeFG is not tried: NR and the menu run
    # on the D3D9 device.
    @{ Name = "flipex-fg"; Args = @("--flipex"); Fg = $true; Frames = 400
       Script = "150:motion=nr,200:menu,215:shot=flipex-menu,225:menu,300:motion=after"
       Log = @("XeFG needs a windowed or borderless D3D9 device without D3DSWAPEFFECT_FLIPEX", "shared with D3D12",
               "D3D9 bridge frame \d+: D3D9 present, NR true", "D3D9 menu ready", "menu open")
       NotLog = @("XeFG presenter \w+ on window", "CreateSwapChainForHwnd failed") },
    # A Reset into exclusive fullscreen: the XeFG presenter leaves the window before the Reset and NR runs on the
    # D3D9 device in fullscreen. Back in the window the presenter returns (D3D9 cannot show a windowed frame there
    # any more) with frame generation, and the screen keeps changing.
    # The menu's hold on the window's input is checked with the menu open before, in exclusive fullscreen and after:
    # sent key and mouse messages must not reach the game (the first check is only printed: the window may not have
    # the foreground yet that early). The phase "windowed" times the return of the presenter.
    @{ Name = "fullscreen-fg"; Args = @(); Fg = $true; Frames = 700
       Script = ("100:wndproc,110:menu,120:input,130:menu,150:motion=before,200:fullscreen,250:wndproc,260:motion=fullscreen," +
                 "270:menu,280:input,285:menu,299:mark=windowed,300:windowed,340:mark=after-windowed,390:wndproc," +
                 "400:motion=windowed,480:menu,490:input,495:shot=fullscreen-menu,505:menu,510:input,600:motion=after")
       Out = @(("(?s)wndproc: OptiScaler.*wndproc: another module.*input: the game saw 0 of 3 \(foreground yes\).*" +
                "wndproc: OptiScaler.*input: the game saw 0 of 3 \(foreground yes\).*input: the game saw 3 of 3"))
       Log = @(("(?s)releasing the XeFG presenter.*Reset: 0, \d+x\d+, windowed 0.*D3D9 bridge: \d+x\d+ format \d+, MSAA 0, copies.*" +
                "Reset: 0, 1280x720, windowed 1.*XeFG presenter \w+ on window.*frame generation on.*menu open"),
               "D3D9 bridge frame 601: XeFG presenter, NR true at scale 1.00, FG true, XeFG last present 3 frames") },
    # An InterpolationCount in the INI wins over the D3D9 default of two generated frames.
    @{ Name = "fg-2x"; Args = @(); Fg = $true; Frames = 320; Ini = "[XeFG]`r`nInterpolationCount=1`r`n"; Script = "250:motion=fg-2x"
       Log = @("D3D9 bridge frame 301: XeFG presenter, NR true at scale 1.00, FG true, XeFG last present 2 frames") },
    # A Reset while something holds a buffer of the presenter: its ResizeBuffers fails, the bridge skips frames and
    # keeps the presenter, and once the buffer is let go the presenter takes the new size and shows frames again. The
    # presenter here is the plain DXGI one (FG Output None, then exclusive fullscreen and back), as XeFG's swapchain
    # resizes with one of its buffers held.
    @{ Name = "resize-fail"; Args = @(); Fg = $true; Frames = 800; Errors = $true; Frozen = $true
       Script = ("150:tab=2,155:menu,160:int=FGOutput=0,170:menu,200:fullscreen,250:windowed,300:motion=dxgi,350:hold," +
                 "360:reset=1600x900,400:motion=held,450:unhold,550:motion=released,700:motion=after")
       Log = @(("(?s)DXGI presenter \w+ on window.*Reset: 0, 1600x900.*presenter resize failed \(887A0001\); frames are skipped.*" +
                "XeFG presenter 1280x720 -> 1600x900: 0.*the presenter shows frames again"),
               "D3D9 bridge frame 601: DXGI presenter, NR true at scale 1.00, FG false")
       NotLog = @("(?s)DXGI presenter.*releasing the XeFG presenter", "(?s)DXGI presenter.*D3D9 menu ready")
       Out = @("motion dxgi: 0\.[1-9]", "motion released: 0\.[1-9]", "motion after: 0\.[1-9]") },
    # Thousands of generated frames in a row (the 32-bit bridge lost its D3D12 device after about 2000), with two
    # generated frames per rendered frame.
    @{ Name = "soak-fg"; Args = @(); Fg = $true; Frames = 6000; Ini = "[XeFG]`r`nInterpolationCount=2`r`n"
       Script = "1000:motion=soak-1000,3000:motion=soak-3000,5900:motion=soak-5900"
       Log = @("D3D9 bridge frame 5701: XeFG presenter, NR true at scale 1.00, FG true, XeFG last present 3 frames") },
    # Frame times at 1920x1080 borderless with the scene moving and still, with 8 ms of game CPU work per frame,
    # NR or FG on and off.
    @{ Name = "timing-nr"; Args = @("--borderless", "--size", "1920x1080"); Fg = $false; Frames = 880
       Script = "100:mark=nr-moving,220:still,230:mark=nr-still,350:still,360:cpu=8,370:mark=nr-cpu8,490:nr," +
                "500:mark=off-cpu8,620:cpu=0,630:mark=off-moving,750:still,760:mark=off-still"
       Log = @("D3D9 bridge frame \d+: D3D9 present, NR true") },
    @{ Name = "timing-nr-ex"; Args = @("--ex", "--borderless", "--size", "1920x1080"); Fg = $false; Frames = 880
       Script = "100:mark=nr-moving,220:still,230:mark=nr-still,350:still,360:cpu=8,370:mark=nr-cpu8,490:nr," +
                "500:mark=off-cpu8,620:cpu=0,630:mark=off-moving,750:still,760:mark=off-still"
       Log = @("D3D9 bridge frame \d+: D3D9 present, NR true", "shared with D3D12") },
    @{ Name = "timing-fg"; Args = @("--borderless", "--size", "1920x1080"); Fg = $true; Frames = 930
       Ini = "[XeFG]`r`nInterpolationCount=2`r`n"
       Script = "150:mark=fg-moving,270:still,280:mark=fg-still,400:still,410:cpu=8,420:mark=fg-cpu8,540:fg," +
                "550:mark=nr-only-cpu8,670:cpu=0,680:mark=nr-only-moving,800:nr,810:mark=off-moving"
       Log = @("D3D9 bridge frame \d+: XeFG presenter, NR true at scale [0-9.]+, FG true") }
)
if ($Case) { $cases = @($cases | Where-Object { $_.Name -in ($Case -split ",") }) }
if ($cases.Count -eq 0) { throw "unknown case $Case" }

function Enter-Lock {
    if (-not $Lock) { return }
    while ($true) {
        try { New-Item -ItemType Directory -Path $Lock -ErrorAction Stop | Out-Null; return } catch { }
        $item = Get-Item $Lock -ErrorAction SilentlyContinue
        if ($item -and $item.CreationTime -lt (Get-Date).AddMinutes(-20)) { Remove-Item $Lock -Force -Recurse }
        else { Start-Sleep -Seconds 20 }
    }
}

$failed = @()
foreach ($c in $cases) {
    $dir = Join-Path $out $c.Name
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    Copy-Item $exe $dir
    Copy-Item $opti (Join-Path $dir "d3d9.dll")
    New-Item -ItemType HardLink -Path (Join-Path $dir "OptiScaler.pdb") -Target $pdb | Out-Null
    foreach ($name in @("dlssnr_amd_pass1.dll", "dlssnr_on_amd_weights.bin", "dlssnr_on_amd.ini")) {
        $source = Join-Path $Runtime $name
        if (Test-Path $source) { New-Item -ItemType HardLink -Path (Join-Path $dir $name) -Target ([System.IO.Path]::GetFullPath($source)) | Out-Null }
    }
    $fgSection = "[FrameGen]`r`nEnabled=false`r`n"
    if ($c.Fg) {
        foreach ($name in @("libxess_fg.dll", "libxell.dll")) { Copy-Item (Join-Path $XeFG $name) $dir }
        $fgSection = "[FrameGen]`r`nEnabled=true`r`nFGInput=upscaler`r`nFGOutput=xefg`r`n"
    }
    Set-Content -Path (Join-Path $dir "OptiScaler.ini") -Encoding ascii -Value (
        "[DlssNr]`r`nEnabled=true`r`nPresentWithoutUpscaler=$(if ($c.Present) { $c.Present } else { 'true' })`r`n" +
        "NrBackend=daniel`r`n" + $fgSection + $c.Ini + ($ExtraIni -replace '~', "`r`n") + "`r`n" +
        "[Log]`r`nLogToFile=true`r`nLogLevel=2`r`nSingleFile=true`r`n")

    $arguments = @($c.Args) + @("--frames", $c.Frames, "--shots", "`"$dir`"", "--script", "`"$($c.Script)`"")
    Enter-Lock
    try {
        $process = Start-Process -FilePath (Join-Path $dir "dx9_present_smoke.exe") -ArgumentList $arguments `
            -WorkingDirectory $dir -NoNewWindow -PassThru -RedirectStandardOutput (Join-Path $dir "stdout.txt") `
            -RedirectStandardError (Join-Path $dir "stderr.txt")
        $null = $process.Handle
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $process.Kill()
            $process.WaitForExit()
            $failed += "$($c.Name): timed out"
            continue
        }
        $code = $process.ExitCode
    }
    finally {
        if ($Lock) { Remove-Item $Lock -Force -Recurse -ErrorAction SilentlyContinue }
    }

    $stdout = Get-Content (Join-Path $dir "stdout.txt") -Raw
    $log = Get-Content (Join-Path $dir "OptiScaler.log") -Raw -ErrorAction SilentlyContinue
    $problems = @()
    if ($code -ne 0) { $problems += "exit code $code" }
    if ($stdout -notmatch "DONE 0 present failures") { $problems += "present failures" }
    foreach ($pattern in $c.Log) { if ($log -notmatch $pattern) { $problems += "log lacks: $pattern" } }
    foreach ($pattern in $c.NotLog) { if ($log -match $pattern) { $problems += "log has: $pattern" } }
    foreach ($pattern in $c.Out) { if ($stdout -notmatch $pattern) { $problems += "output lacks: $pattern" } }
    foreach ($line in ($stdout -split "`r?`n" | Where-Object { $_ -match "^motion (\S+): ([0-9.]+)" })) {
        $null = $line -match "^motion (\S+): ([0-9.]+)"
        if ([double]$Matches[2] -lt 0.02 -and -not ($c.Frozen -and $Matches[1] -ne "before")) { $problems += "frozen on screen: $line" }
    }
    if (-not $c.Errors -and $log -match "\[E\] .*D3D9") { $problems += "D3D9 error in the log" }

    Write-Host "== $($c.Name): $(if ($problems.Count) { 'FAIL' } else { 'PASS' })"
    $stdout -split "`r?`n" | Where-Object { $_ -match "^(phase|motion|shot|reset|diff)" } | ForEach-Object { Write-Host "   $_" }
    foreach ($p in $problems) { Write-Host "   $p" }
    if ($problems.Count) { $failed += $c.Name }
}

if ($failed.Count) { Write-Host "FAILED: $($failed -join ', ')"; exit 1 }
Write-Host "D3D9 present tests passed"
exit 0
