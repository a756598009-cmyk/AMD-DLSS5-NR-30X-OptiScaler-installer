<#
.SYNOPSIS
  Install this project's OptiScaler into a game folder.
  Double-click Setup.bat (no args) to pick the game folder, or pass -GameDir.
  Copies danielblnc's runtime to dlssnr_amd_pass1.dll: 0.8.0 (NeuralHub, recommended) as that one
  file, or 0.6.0, 0.5.x, 0.4.x, 0.3.1 or 0.3.0 (version.dll) to dlssnr_amd_pass1-3.dll,
  generates weights locally if needed, then installs OptiScaler as the chosen proxy.

.DESCRIPTION
  Only installs this project. Does not leave danielblnc's version.dll (or NeuralHub's stub) in the game.

  Put these in the SAME folder as Setup.ps1 (the package root):
    OptiScaler.dll              this fork
    OptiScaler.ini              optional
    OptiScaler\                 FFX / XeSS / Agility deps
    version.dll                 danielblnc AMD NR 0.8.0, 0.6.0, 0.5.x, 0.4.x, 0.3.1 or 0.3.0
                                (copied to pass1; to pass1-3 before 0.8.0)
    neuralhub_host.dll          optional, NeuralHub 0.8.0's runtime (used like version.dll)
    neuralhub.exe               optional, NeuralHub 0.8.0's manager; its runtime is taken out of
                                it without running it
    NeuralHub_v0.8.0_setup.exe  optional; needs 7-Zip to take neuralhub.exe out of it (not run)
    nvngx_dlssnr.dll            optional, to generate weights with danielblnc setup; with 0.8.0
                                (310.8.0.0 or later) OptiScaler builds the weights from it at launch
    nvngx_dlss.dll              optional, DLSS-SR through the 0.8.0 host (NeuralHub); copied into
                                the game folder and dlssnr_amd_host
    nvngx_dlssd.dll             optional, DLSS-RR through the 0.8.0 host (NeuralHub); copied into
                                the game folder and dlssnr_amd_host
    (nvngx_dlss.dll / nvngx_dlssd.dll may also sit in a nvngx\ subfolder of the package root)
    dlssnr_on_amd_setup.exe     optional, danielblnc 0.6.0 / 0.5.x / 0.4.x / 0.3.1 / 0.3.0 setup; its
                                version.dll is taken out of it without running it
    dlssnr_on_amd_weights.bin   optional if you already have it

  An installed NeuralHub 0.8.0 is found without help (HKCU\Software\Danielblnc\NeuralHub RuntimePath,
  or %LOCALAPPDATA%\Danielblnc\NeuralHub\runtime\v0.8.0), and so are the weights or the
  nvngx_dlssnr.dll imported into its data folder. NEURALHUB_STORE moves that store, as for NeuralHub.

  When several known runtimes are found (version.dll here, pass1 in the game, NeuralHub, the one
  inside dlssnr_on_amd_setup.exe or neuralhub.exe), the newest is installed and its version is
  printed. Pass -AuthorDll <version.dll | neuralhub_host.dll | neuralhub.exe | setup .exe> to install
  a specific one instead.

.EXAMPLE
  .\Setup.bat
  .\Setup.bat "D:\Games\Foo\Content"
  .\install-amd-presr.ps1 -GameDir 'D:\Games\Foo' -Proxy dxgi.dll
#>
[CmdletBinding()]
param(
    # Empty = open a folder picker (double-click Setup.bat).
    [string]$GameDir = '',
    # dinput8 is NOT a valid proxy for this OptiScaler build (no DirectInput8Create export).
    [ValidateSet('dxgi.dll','winmm.dll','d3d12.dll','winhttp.dll','wininet.dll','dbghelp.dll')]
    [string]$Proxy = 'dxgi.dll',
    [string]$Root,
    [string]$AuthorDll,
    [switch]$NonInteractive,
    # Setup.bat owns the final pause; keep all folder/proxy/confirmation prompts.
    [switch]$NoPause
)
$ErrorActionPreference = 'Stop'

function Pause-Exit([int]$code) {
    if (-not $NonInteractive -and -not $NoPause) {
        Write-Host ''
        Write-Host 'Press any key to exit...'
        [void][Console]::ReadKey($true)
    }
    exit $code
}

# Runtimes taken out of a setup, neuralhub.exe or -AuthorDll's exe, and 7-Zip's work folders.
# All live in %TEMP% and go at the end of the install, failed or not.
$script:tempRuntimes = New-Object System.Collections.Generic.List[string]
function New-TempRuntimePath([string]$ext = '.dll') {
    $p = Join-Path $env:TEMP ('amd-presr-version-' + [guid]::NewGuid().ToString('N') + $ext)
    $script:tempRuntimes.Add($p)
    return $p
}
function Remove-TempRuntimes {
    foreach ($t in $script:tempRuntimes) {
        if ($t -match 'amd-presr-version-' -and (Test-Path -LiteralPath $t)) {
            try { Remove-Item -LiteralPath $t -Recurse -Force } catch { }
        }
    }
    $script:tempRuntimes.Clear()
}

function Fail([string]$msg) {
    # A runtime taken out of the setup or staged out of the game sits in %TEMP% until the end of
    # a successful install; do not leave it behind on a failed one.
    if ($script:stagedA -and $script:stagedA -match 'amd-presr-version-' -and
        (Test-Path -LiteralPath $script:stagedA -PathType Leaf)) {
        try { Remove-Item -LiteralPath $script:stagedA -Force } catch { }
    }
    Remove-TempRuntimes
    Write-Host "ERROR: $msg" -ForegroundColor Red
    Write-Host ''
    Write-Host 'Install FAILED.' -ForegroundColor Red
    Pause-Exit 1
}

# Catch errors outside the individual file-operation handlers (for example,
# an invalid danielblnc setup executable). The batch also catches parser/parameter
# binding failures, which happen before this script body can run.
trap { Fail ("Unexpected install error: " + $_.Exception.Message) }

# $Root must not be a param default of $PSScriptRoot: when invoked via powershell -File,
# $PSScriptRoot is not yet assigned during parameter binding (assigned inside script body).
# Setup.bat uses -File.
if (-not $Root) { $Root = $PSScriptRoot }

# Package layout: everything is in the package root (matching upstream OptiScaler packages).
# Early packages placed DLLs under release\, check there as a fallback.
$release = $Root
if (!(Test-Path -LiteralPath (Join-Path $release 'OptiScaler.dll')) -and
    (Test-Path -LiteralPath (Join-Path $Root 'release\OptiScaler.dll'))) {
    $release = Join-Path $Root 'release'
}

# Do not use Get-FileHash: it belongs to Microsoft.PowerShell.Utility and relies on module autoloading.
# When PSModulePath points to PowerShell 7 module directories (e.g. launched from pwsh
# or in CI within shell: pwsh invoking powershell -File), the 5.1 child process cannot load it
# and throws CommandNotFoundException. Setup.bat via cmd takes this exact path.
# Compute SHA256 using .NET directly without module dependencies.
function Get-Sha256([string]$path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $fs = [IO.File]::OpenRead($path)
        try { return ([BitConverter]::ToString($sha.ComputeHash($fs))).Replace('-', '') }
        finally { $fs.Dispose() }
    } finally { $sha.Dispose() }
}

function Test-OptiProxy([string]$path) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { return $false }
    try {
        $vi = (Get-Item -LiteralPath $path).VersionInfo
        return ($vi.ProductName -eq 'OptiScaler' -or $vi.FileDescription -eq 'OptiScaler')
    } catch { return $false }
}

function Ask-Choice([string]$title, [string[]]$options) {
    Write-Host ''
    Write-Host $title -ForegroundColor Yellow
    for ($i = 0; $i -lt $options.Count; $i++) {
        Write-Host ("  [{0}] {1}" -f ($i + 1), $options[$i])
    }
    do {
        $ans = Read-Host 'Enter number'
        $n = 0
        if ([int]::TryParse($ans, [ref]$n) -and $n -ge 1 -and $n -le $options.Count) { return $n }
        Write-Host 'Invalid choice.'
    } while ($true)
}

function Ask-GameFolder {
    # IFileDialog + FOS_PICKFOLDERS: address bar works. A parentless console
    # dialog often shows an empty nav pane unless it has an owner window.
    try {
        Add-Type -ReferencedAssemblies System.Windows.Forms, System.Drawing -TypeDefinition @"
using System;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Windows.Forms;
public static class AmdFolderPick {
    [ComImport, Guid("DC1C5A9C-E88A-4dde-A5A1-60F82A20AEF7")] class FileOpenDialogRCW { }
    [ComImport, Guid("42f85136-db7e-439c-85f1-e4075d135fc8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IFileDialog {
        [PreserveSig] int Show(IntPtr parent);
        void SetFileTypes(uint count, IntPtr filters);
        void SetFileTypeIndex(uint index);
        void GetFileTypeIndex(out uint index);
        void Advise(IntPtr sink, out uint cookie);
        void Unadvise(uint cookie);
        void SetOptions(uint options);
        void GetOptions(out uint options);
        void SetDefaultFolder(IntPtr folder);
        void SetFolder(IntPtr folder);
        void GetFolder(out IntPtr folder);
        void GetCurrentSelection(out IntPtr item);
        void SetFileName([MarshalAs(UnmanagedType.LPWStr)] string name);
        void GetFileName(out IntPtr name);
        void SetTitle([MarshalAs(UnmanagedType.LPWStr)] string title);
        void SetOkButtonLabel([MarshalAs(UnmanagedType.LPWStr)] string text);
        void SetFileNameLabel([MarshalAs(UnmanagedType.LPWStr)] string label);
        void GetResult(out IntPtr item);
        void AddPlace(IntPtr item, int order);
        void SetDefaultExtension([MarshalAs(UnmanagedType.LPWStr)] string ext);
        void Close(int hr);
        void SetClientGuid(ref Guid guid);
        void ClearClientData();
        void SetFilter(IntPtr filter);
        void GetResults(out IntPtr items);
        void GetSelectedItems(out IntPtr items);
    }
    [ComImport, Guid("43826d1e-e718-42ee-bc55-a1e261c37bfe"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IShellItem {
        void BindToHandler(IntPtr bc, ref Guid bh, ref Guid riid, out IntPtr ppv);
        void GetParent(out IntPtr ppsi);
        void GetDisplayName(uint sigdnName, out IntPtr ppszName);
        void GetAttributes(uint sfgaoMask, out uint psfgaoAttribs);
        void Compare(IntPtr psi, uint hint, out int piOrder);
    }
    const uint FOS_PICKFOLDERS = 0x20;
    const uint FOS_FORCEFILESYSTEM = 0x40000;
    const uint FOS_NOCHANGEDIR = 0x8;
    const uint SIGDN_FILESYSPATH = 0x80058000;
    const uint SIGDN_NORMALDISPLAY = 0;
    static readonly Guid IID_IShellItem = new Guid("43826d1e-e718-42ee-bc55-a1e261c37bfe");
    // This PC / My Computer — populates the left navigation tree.
    const string ThisPcParsingName = "::{20D04FE0-3AEA-1069-A2D8-08002B30309D}";
    [DllImport("shell32.dll", CharSet = CharSet.Unicode, PreserveSig = true)]
    static extern int SHCreateItemFromParsingName(
        [MarshalAs(UnmanagedType.LPWStr)] string path,
        IntPtr pbc, ref Guid riid, out IntPtr ppv);
    static IntPtr ItemFromParsingName(string path) {
        Guid iid = IID_IShellItem;
        IntPtr item;
        int hr = SHCreateItemFromParsingName(path, IntPtr.Zero, ref iid, out item);
        return hr == 0 ? item : IntPtr.Zero;
    }
    public static string PickFolder(string title) {
        var dlg = (IFileDialog)new FileOpenDialogRCW();
        uint opts;
        dlg.GetOptions(out opts);
        // Do NOT set FOS_FORCEFILESYSTEM: it can leave the left nav tree empty
        // for a parentless console host. FOS_PICKFOLDERS is enough.
        dlg.SetOptions(opts | FOS_PICKFOLDERS | FOS_NOCHANGEDIR);
        if (!string.IsNullOrEmpty(title)) dlg.SetTitle(title);
        // Parentless IFileDialog often shows a blank nav pane. A tiny hidden
        // owner window gives the shell a host to hang the tree on.
        IntPtr owner = IntPtr.Zero;
        try {
            var form = new Form {
                ShowInTaskbar = false,
                WindowState = FormWindowState.Minimized,
                StartPosition = FormStartPosition.Manual,
                Location = new Point(-32000, -32000),
                Size = new Size(1, 1)
            };
            form.Show();
            form.Hide();
            owner = form.Handle;
        } catch { owner = IntPtr.Zero; }
        IntPtr thisPc = ItemFromParsingName(ThisPcParsingName);
        if (thisPc != IntPtr.Zero) {
            try { dlg.SetDefaultFolder(thisPc); } catch { }
            try { dlg.SetFolder(thisPc); } catch { }
            Marshal.Release(thisPc);
        }
        int hr = dlg.Show(owner);
        if (owner != IntPtr.Zero) {
            try {
                var form = Control.FromHandle(owner) as Form;
                if (form != null) form.Dispose();
            } catch { }
        }
        if (hr != 0) return null;
        IntPtr result;
        dlg.GetResult(out result);
        var isi = (IShellItem)Marshal.GetObjectForIUnknown(result);
        IntPtr pathPtr;
        isi.GetDisplayName(SIGDN_FILESYSPATH, out pathPtr);
        string path = Marshal.PtrToStringUni(pathPtr);
        Marshal.FreeCoTaskMem(pathPtr);
        Marshal.Release(result);
        return path;
    }
}
"@ -ErrorAction Stop
        $picked = [AmdFolderPick]::PickFolder('Select the game folder that contains the game .exe')
        if ($picked) { return $picked }
        return $null
    } catch {
        Add-Type -AssemblyName System.Windows.Forms
        $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
        $dlg.Description = 'Select the game folder that contains the game .exe'
        $dlg.ShowNewFolderButton = $false
        if ($dlg.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { return $dlg.SelectedPath }
        return $null
    }
}

# Double-click Setup.bat: no path argument → open a folder picker.
if ([string]::IsNullOrWhiteSpace($GameDir)) {
    if ($NonInteractive) { Fail 'GameDir is required in -NonInteractive mode.' }
    Write-Host 'Pick the game folder (the one with the game .exe)…' -ForegroundColor Yellow
    $GameDir = Ask-GameFolder
    if ([string]::IsNullOrWhiteSpace($GameDir)) {
        Write-Host 'Cancelled — no folder selected.'
        Pause-Exit 0
    }
}

# Interactive proxy pick (like older 1.7.x installers). dinput8 is invalid for this build.
if (-not $NonInteractive -and -not $PSBoundParameters.ContainsKey('Proxy')) {
    $proxyOptions = @(
        'dxgi.dll (default; if the game fails to start, try winmm.dll)',
        'winmm.dll (recommended by some games)',
        'd3d12.dll',
        'winhttp.dll',
        'wininet.dll',
        'dbghelp.dll'
    )
    $pi = Ask-Choice 'Which proxy DLL should OptiScaler install as?' $proxyOptions
    $Proxy = ($proxyOptions[$pi - 1] -split '\s+')[0]
    Write-Host "Selected proxy: $Proxy"
}

if (!(Test-Path -LiteralPath $GameDir -PathType Container)) {
    Fail "Game folder not found: $GameDir"
}
$game = (Resolve-Path -LiteralPath $GameDir).Path

# Store packages: WindowsApps is not a writable install target (ACL / TrustedInstaller).
if ($game -match '(?i)\\WindowsApps\\') {
    Fail @"
Refusing to install into WindowsApps:
  $game

That path is not a reliable write target.
Pick the writable game folder (the one that contains the game .exe and accepts file copies).
"@
}

$probe = Join-Path $game ('.write-probe-' + [guid]::NewGuid().ToString('N') + '.tmp')
try {
    [IO.File]::WriteAllText($probe, 'ok')
    Remove-Item -LiteralPath $probe -Force
} catch {
    Fail "Cannot write to $game ($($_.Exception.Message)). Pick a writable folder next to the game .exe."
}

# The game must be closed: it holds dxgi/pass/weights while running.
function Test-FileLocked([string]$path) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { return $false }
    try {
        $fs = [IO.File]::Open($path, 'Open', 'ReadWrite', 'None')
        $fs.Close()
        return $false
    } catch [System.IO.IOException] { return $true }
    catch { return $false }
}

$locked = @()
foreach ($name in @($Proxy, 'dlssnr_amd_pass1.dll', 'dlssnr_amd_pass2.dll', 'dlssnr_amd_pass3.dll', 'dlssnr_on_amd_weights.bin',
        'dlssnr_amd_host\neuralhub_host.dll', 'dlssnr_amd_host\dlssnr_on_amd_weights.bin')) {
    $p = Join-Path $game $name
    if (Test-FileLocked $p) { $locked += $name }
}
if ($locked.Count -gt 0) {
    Fail @"
The game is still running (file lock detected):
  $($locked -join ', ')

Close the game completely, then run Setup.bat again.
"@
}
# Also flag a live process whose image lives in the game folder.
$gameExes = @(Get-ChildItem -LiteralPath $game -Filter '*.exe' -File -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name)
if ($gameExes.Count -gt 0) {
    $running = Get-Process -ErrorAction SilentlyContinue | Where-Object {
        $n = "$($_.ProcessName).exe"
        ($gameExes -contains $n)
    }
    if ($running) {
        $names = ($running | ForEach-Object { "$($_.ProcessName).exe (PID $($_.Id))" }) -join ', '
        Fail @"
A game process is still running:
  $names

Close the game completely, then run Setup.bat again.
"@
    }
}

if (!(Test-Path -LiteralPath (Join-Path $release 'OptiScaler.dll'))) {
    Fail @"
Missing OptiScaler.dll.
Looked in:
  $release\OptiScaler.dll
  $Root\release\OptiScaler.dll

Run Setup.bat from the folder you unzipped the package into, so that
OptiScaler.dll sits next to Setup.ps1.
"@
}

function Confirm-Continue([string]$title) {
    if ($NonInteractive) { Fail $title }
    $choice = Ask-Choice $title @('Cancel and exit', 'Continue anyway')
    if ($choice -eq 1) { Write-Host 'Cancelled.'; Pause-Exit 0 }
}

function Find-FirstFile([string[]]$paths) {
    foreach ($p in $paths) {
        if ($p -and (Test-Path -LiteralPath $p -PathType Leaf)) { return $p }
    }
    return $null
}

# Hash: only known danielblnc runtimes are supported. The RVA layout is pinned to
# each binary — a different build will not run correctly. Fail closed.
# 0.3.0 = AmdLayout.h kAmd03; 0.3.1 = kAmd031 (mapped 2026-09-16); 0.4.0 = kAmd040 and
# 0.4.1 = kAmd041 (2026-09-26). 0.4.2 = kAmd042, public since 2026-09-27. 0.4.3 = kAmd043, public since
# 2026-09-28. 0.5.0 = kAmd050, public since 2026-09-29. 0.5.1 = kAmd051, public since 2026-10-01 and the
# recommended one until 0.8.0. 0.6.0 = kAmd060, danielblnc's early build for his supporters: accepted when the
# user has it, never shipped. 0.8.0 = kAmd080, NeuralHub 0.8.0's neuralhub_host.dll and the recommended one:
# a single module (no pass2/pass3), hosted by OptiScaler as dlssnr_amd_host\neuralhub_host.dll.
$expectedA080 = 'BE9172B68D0E77F98F04F941F4E2ABE2C73EB01B8AAFE94B26EE31EEFEFB5EB8'
$expectedA030 = '8321CAE728D28CB7632D0D58D3D913E91132BF7645C126505698FBE4CD5A0138'
$expectedA031 = 'B108D6407EB7F094A4F9111EDD778EEE7B978B648D413A9FC7AEEDFDD914C154'
$expectedA040 = 'D62BE3D8B9FBB3C6C81982C4DDB3DFA00EB9662E3206925CBE5B7E1BC6798B80'
$expectedA041 = '823063EB4C76B1334FD1800C41798873AE61D4016AF0406F1F0B9DCE57B1D376'
$expectedA042 = '8AA2DCC5B6596ACA97995DBFD4E0A9790D8C15108495E0ED154DD15DBB5B465A'
$expectedA043 = 'D1E320862A8763AC39E7CE194536D4B6C55BA61BAE9E8A92753CEC32DF67A457'
$expectedA050 = 'CDDFB09E019347957BF7B96C95C0E900E8D3062DFAED697A8A96B0A039AEC31A'
$expectedA051 = '493B4A3B80A21F7255109172AB7BB01BA08D35F2941718F441768F1ABFC48ACD'
$expectedA060 = '195C4A891B6EAC4C1CB7671E10FF62BBBE2B17F1DFAE1344DC5A6714E4775721'
$knownA0217  = 'BC97F3B06718E19042ACAF227BFE15D1E43D4977F9DC2E39994FCC511445FF4E'
$expectedAuthor = @($expectedA030, $expectedA031, $expectedA040, $expectedA041, $expectedA042, $expectedA043, $expectedA050, $expectedA051, $expectedA060, $expectedA080)
# Runtimes that run as one module: only pass1 is installed, and stale pass2/pass3 are removed.
# OptiScaler copies pass1 to dlssnr_amd_host\neuralhub_host.dll and, without weights, builds
# them there from nvngx_dlssnr.dll.
$singleModule = @($expectedA080)
# The oldest nvngx_dlssnr.dll a single-module runtime builds weights from.
$minNvForBuild = [version]'310.8.0.0'
# Newest first. When more than one known runtime is at hand (an old version.dll next to
# Setup.bat or in the game, a newer dlssnr_on_amd_setup.exe), the newest one is installed.
$runtimeOrder = @(
    @{ Hash = $expectedA080; Name = '0.8.0' },
    @{ Hash = $expectedA060; Name = '0.6.0' },
    @{ Hash = $expectedA051; Name = '0.5.1' },
    @{ Hash = $expectedA050; Name = '0.5.0' },
    @{ Hash = $expectedA043; Name = '0.4.3' },
    @{ Hash = $expectedA042; Name = '0.4.2' },
    @{ Hash = $expectedA041; Name = '0.4.1' },
    @{ Hash = $expectedA040; Name = '0.4.0' },
    @{ Hash = $expectedA031; Name = '0.3.1' },
    @{ Hash = $expectedA030; Name = '0.3.0' }
)

# Version name of a known runtime hash, or $null.
function Get-RuntimeName([string]$hash) {
    foreach ($r in $runtimeOrder) { if ($hash -and $r.Hash -ieq $hash) { return $r.Name } }
    return $null
}

# Higher is newer; -1 for an unknown hash.
function Get-RuntimeRank([string]$hash) {
    for ($k = 0; $k -lt $runtimeOrder.Count; $k++) {
        if ($hash -and $runtimeOrder[$k].Hash -ieq $hash) { return $runtimeOrder.Count - $k }
    }
    return -1
}

# dlssnr_on_amd_setup.exe 0.3.0 and 0.3.1 append version.dll; since 0.3.3 it sits inside the
# setup's own .rdata. Either way, take the embedded image out by its PE headers (end of the last
# section's raw data) and keep it only if its SHA256 is one of $known. The setup is never run
# for this. neuralhub.exe (NeuralHub 0.8.0's manager) holds the runtime raw too (at 0x320562), next
# to three other images (neuralhub_stub.dll and the two FidelityFX DLLs): every embedded image is
# looked at and the first whose SHA256 is a known runtime is taken.
function Get-EmbeddedRuntime([string]$exe, [string[]]$known) {
    if (-not $exe -or !(Test-Path -LiteralPath $exe -PathType Leaf)) { return $null }
    $bytes = [IO.File]::ReadAllBytes($exe)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        # Offset 0 is the setup's own header.
        $i = 1
        while (($i = [Array]::IndexOf($bytes, [byte]0x4D, $i)) -ge 0) {
            if ($i + 0x40 -le $bytes.Length -and $bytes[$i + 1] -eq 0x5A) {
                $nt = $i + [BitConverter]::ToInt32($bytes, $i + 0x3C)
                if ($nt -gt $i -and $nt + 24 -le $bytes.Length -and [BitConverter]::ToUInt32($bytes, $nt) -eq 0x4550) {
                    $count = [BitConverter]::ToUInt16($bytes, $nt + 6)
                    $table = $nt + 24 + [BitConverter]::ToUInt16($bytes, $nt + 20)
                    $end = [long]0
                    for ($s = 0; $s -lt $count -and $table + 40 * ($s + 1) -le $bytes.Length; $s++) {
                        $o = $table + 40 * $s
                        $raw = [long][BitConverter]::ToUInt32($bytes, $o + 20) + [BitConverter]::ToUInt32($bytes, $o + 16)
                        if ($raw -gt $end) { $end = $raw }
                    }
                    if ($end -gt 0 -and $i + $end -le $bytes.Length) {
                        $hash = ([BitConverter]::ToString($sha.ComputeHash($bytes, $i, [int]$end))).Replace('-', '')
                        if ($known -contains $hash) {
                            $out = New-TempRuntimePath
                            $fs = [IO.File]::Create($out)
                            try { $fs.Write($bytes, $i, [int]$end) } finally { $fs.Dispose() }
                            return $out
                        }
                    }
                }
            }
            $i++
        }
    } finally { $sha.Dispose() }
    return $null
}

function Find-SevenZip {
    $cmd = Get-Command '7z.exe' -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($cmd) { return $cmd.Source }
    foreach ($base in @($env:ProgramFiles, $env:ProgramW6432, ${env:ProgramFiles(x86)})) {
        if (-not $base) { continue }
        $p = Join-Path $base '7-Zip\7z.exe'
        if (Test-Path -LiteralPath $p -PathType Leaf) { return $p }
    }
    return $null
}

function Test-NeuralHubExe([string]$exe) {
    if ((Split-Path -Leaf $exe) -match '^(?i)neuralhub') { return $true }
    try { return ((Get-Item -LiteralPath $exe).VersionInfo.ProductName -eq 'NeuralHub') } catch { return $false }
}

# The known runtime inside an exe, in %TEMP%, or $null. The exe is never run. A raw image (the
# danielblnc setups, neuralhub.exe) is taken out by its PE headers. NeuralHub's NSIS setup
# (NeuralHub_v0.8.0_setup.exe) packs neuralhub.exe with solid LZMA, which only 7-Zip unpacks here:
# 7-Zip takes neuralhub.exe out into %TEMP%, and the runtime is taken out of that.
function Get-RuntimeFromExe([string]$exe) {
    $found = Get-EmbeddedRuntime $exe $expectedAuthor
    # neuralhub.exe itself is the manager, not an archive: what is not raw in it is not there.
    if ($found -or -not (Test-NeuralHubExe $exe) -or (Split-Path -Leaf $exe) -ieq 'neuralhub.exe') { return $found }
    $name = Split-Path -Leaf $exe
    $sevenZip = Find-SevenZip
    if (-not $sevenZip) {
        Write-Host ("NOTE: {0} is a NeuralHub setup; taking neuralhub.exe out of it needs 7-Zip (7z.exe), which is not installed." -f $name) -ForegroundColor Yellow
        Write-Host '  Install NeuralHub 0.8.0 (Setup then finds its runtime), or put neuralhub.exe or neuralhub_host.dll' -ForegroundColor Yellow
        Write-Host '  next to Setup.bat, or pass -AuthorDll <neuralhub.exe | neuralhub_host.dll>.' -ForegroundColor Yellow
        return $null
    }
    $work = New-TempRuntimePath ''
    [void][IO.Directory]::CreateDirectory($work)
    try {
        Write-Host ("Taking neuralhub.exe out of {0} with 7-Zip (the setup is not run)..." -f $name)
        # 7-Zip's stderr must not become a terminating error under ErrorActionPreference Stop.
        $eap = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $null = & $sevenZip e '-y' '-bd' ('-o' + $work) '--' $exe 'neuralhub.exe' 2>&1
            $code = $LASTEXITCODE
        } finally { $ErrorActionPreference = $eap }
        $hubExe = Join-Path $work 'neuralhub.exe'
        if ($code -ne 0 -or !(Test-Path -LiteralPath $hubExe -PathType Leaf)) {
            Write-Host ("NOTE: 7-Zip found no neuralhub.exe in {0} (exit code {1})." -f $name, $code) -ForegroundColor Yellow
            return $null
        }
        return (Get-EmbeddedRuntime $hubExe $expectedAuthor)
    } finally {
        try { Remove-Item -LiteralPath $work -Recurse -Force } catch { }
    }
}

# NeuralHub's store: NEURALHUB_STORE (its test mode, registry.ini in place of HKCU), else
# HKCU\Software\Danielblnc\NeuralHub, else %LOCALAPPDATA%\Danielblnc\NeuralHub.
function Get-NeuralHubInfo {
    $info = @{ Store = $null; RuntimePath = $null; RuntimeVersion = $null; RuntimeAbi = $null }
    if ($env:NEURALHUB_STORE) {
        $info.Store = $env:NEURALHUB_STORE
        $regIni = Join-Path $env:NEURALHUB_STORE 'registry.ini'
        if (Test-Path -LiteralPath $regIni -PathType Leaf) {
            $inSection = $false
            foreach ($line in [IO.File]::ReadAllLines($regIni)) {
                if ($line -match '^\s*\[(.+?)\]\s*$') { $inSection = ($Matches[1] -ieq 'NeuralHub'); continue }
                if ($inSection -and $line -match '^\s*(\w+)\s*=\s*(.*?)\s*$' -and $info.ContainsKey($Matches[1])) {
                    $key = $Matches[1]
                    if ($key -ne 'Store') { $info[$key] = $Matches[2] }
                }
            }
        }
        return $info
    }
    try {
        $k = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\Danielblnc\NeuralHub')
        if ($k) {
            try {
                $info.Store = [string]$k.GetValue('StorePath')
                $info.RuntimePath = [string]$k.GetValue('RuntimePath')
                $info.RuntimeVersion = [string]$k.GetValue('RuntimeVersion')
                $info.RuntimeAbi = $k.GetValue('RuntimeAbi')
            } finally { $k.Close() }
        }
    } catch { }
    if (-not $info.Store -and $env:LOCALAPPDATA) { $info.Store = Join-Path $env:LOCALAPPDATA 'Danielblnc\NeuralHub' }
    return $info
}
$hub = Get-NeuralHubInfo

# neuralhub_host.dll of an installed NeuralHub: the one RuntimePath names, then runtime\v0.8.0.
# The SHA256 decides, as for every other candidate.
function Get-NeuralHubRuntimes {
    $list = @()
    if ($hub.RuntimePath) { $list += (Join-Path $hub.RuntimePath 'neuralhub_host.dll') }
    if ($hub.Store) { $list += (Join-Path $hub.Store 'runtime\v0.8.0\neuralhub_host.dll') }
    return $list
}

# NeuralHub's data folder caches the weights and keeps the nvngx_dlssnr.dll imported into it
# (data\index.ini [nr] File=).
function Get-NeuralHubWeights {
    if (-not $hub.Store) { return $null }
    return (Join-Path $hub.Store 'data\dlssnr_on_amd_weights.bin')
}
function Get-NeuralHubNv {
    if (-not $hub.Store) { return $null }
    $index = Join-Path $hub.Store 'data\index.ini'
    if (!(Test-Path -LiteralPath $index -PathType Leaf)) { return $null }
    $inNr = $false
    foreach ($line in [IO.File]::ReadAllLines($index)) {
        if ($line -match '^\s*\[(.+?)\]\s*$') { $inNr = ($Matches[1] -ieq 'nr'); continue }
        if ($inNr -and $line -match '^\s*File\s*=\s*(.+?)\s*$') {
            $leaf = $Matches[1]
            if ($leaf -match '[\\/]|\.\.') { return $null }
            $p = Join-Path $hub.Store ('data\' + $leaf)
            if (Test-Path -LiteralPath $p -PathType Leaf) { return $p }
            return $null
        }
    }
    return $null
}

# File version of an nvngx_dlssnr.dll, or $null.
function Get-NvVersion([string]$path) {
    try {
        $vi = (Get-Item -LiteralPath $path).VersionInfo
        if ($vi.FileMajorPart -le 0) { return $null }
        return [version]('{0}.{1}.{2}.{3}' -f $vi.FileMajorPart, $vi.FileMinorPart, $vi.FileBuildPart, $vi.FilePrivatePart)
    } catch { return $null }
}

# NeuralHub's stub (neuralhub_stub.dll, installed in a game as version.dll, dxgi.dll, ...) loads
# the runtime by itself; it cannot stay next to OptiScaler. It exports NeuralHubStub_Abi.
function Test-NeuralHubStub([string]$path) {
    try {
        $item = Get-Item -LiteralPath $path
        if ($item.Length -gt 4MB) { return $false }
        $text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($path))
        return $text.Contains('NeuralHubStub_Abi')
    } catch { return $false }
}

# Walk every candidate and accept only a file whose SHA256 is a known runtime.
# A game may have B installed as version.dll (README allows that); the first
# same-named file must not block a valid pass1.dll sitting next to it.
# A known -AuthorDll is used as given (the way to install an older runtime on purpose);
# otherwise the newest known runtime among the candidates wins, so a 0.3.1 version.dll
# saved next to Setup.bat by an earlier install does not shadow a newer one in the game.
function Find-AuthorRuntime {
    if ($AuthorDll -and (Test-Path -LiteralPath $AuthorDll -PathType Leaf)) {
        try { if (Get-RuntimeName (Get-Sha256 $AuthorDll)) { return $AuthorDll } } catch { }
    }
    $best = $null
    $bestRank = -1
    $hubRuntimes = @(Get-NeuralHubRuntimes)
    foreach ($c in @(
            (Join-Path $Root 'version.dll'),
            (Join-Path $Root 'neuralhub_host.dll'),
            (Join-Path $Root 'dlssnr_amd_pass1.dll'),
            (Join-Path $game 'version.dll'),
            (Join-Path $game 'dlssnr_amd_pass1.dll')
        ) + $hubRuntimes) {
        if (-not $c -or !(Test-Path -LiteralPath $c -PathType Leaf)) { continue }
        try {
            $h = Get-Sha256 $c
        } catch { continue }
        $rank = Get-RuntimeRank $h
        if ($rank -lt 0) {
            if ($hubRuntimes -contains $c -and -not $script:hubNoted) {
                $script:hubNoted = $true
                $ver = if ($hub.RuntimeVersion) { $hub.RuntimeVersion } else { 'unknown version' }
                Write-Host ("NOTE: NeuralHub's runtime ({0}) at {1} is not one this package supports (0.8.0); skipped." -f $ver, $c) -ForegroundColor Yellow
            }
            continue
        }
        Write-Host ("Found danielblnc runtime {0}: {1}" -f (Get-RuntimeName $h), $c)
        if ($rank -gt $bestRank) { $best = $c; $bestRank = $rank }
    }
    return $best
}

# --- detect lmxxf components ---
$lmxxfRuntime = $null
foreach ($candidate in @(
        (Join-Path $release 'LmxxfNrRuntime.dll'),
        (Join-Path $Root 'LmxxfNrRuntime.dll'),
        (Join-Path $Root 'exports\lmxxf-runtime\LmxxfNrRuntime.dll'),
        (Join-Path $game 'LmxxfNrRuntime.dll')
    )) {
    if (Test-Path -LiteralPath $candidate -PathType Leaf) { $lmxxfRuntime = $candidate; break }
}
$lmxxfMods = $null
foreach ($candidate in @(
        (Join-Path $release 'lmxxf-modules'),
        (Join-Path $Root 'lmxxf-modules'),
        (Join-Path $Root 'third_party\lmxxf\modules'),
        (Join-Path $game 'lmxxf-modules')
    )) {
    if ((Test-Path -LiteralPath $candidate -PathType Container) -and
        (Test-Path -LiteralPath (Join-Path $candidate 'SHA256SUMS') -PathType Leaf)) {
        $lmxxfMods = $candidate
        break
    }
}
$lmxxfShaders = $null
foreach ($candidate in @(
        (Join-Path $release 'shaders'),
        (Join-Path $Root 'shaders'),
        (Join-Path $Root 'third_party\lmxxf\shaders'),
        (Join-Path $game 'shaders')
    )) {
    if ((Test-Path -LiteralPath $candidate -PathType Container) -and
        (Test-Path -LiteralPath (Join-Path $candidate 'native_codec_encode.hlsl') -PathType Leaf)) {
        $lmxxfShaders = $candidate
        break
    }
}
$lmxxfWeights = $null
foreach ($candidate in @(
        (Join-Path $release 'native-game-tiled-assets'),
        (Join-Path $Root 'native-game-tiled-assets'),
        (Join-Path $Root 'DLSS5-AMD\native-game-tiled-assets'),
        (Join-Path $game 'native-game-tiled-assets')
    )) {
    if (Test-Path -LiteralPath $candidate -PathType Container) {
        $lmxxfWeights = $candidate
        break
    }
}

# --- detect danielblnc components ---
$setup   = Join-Path $Root 'dlssnr_on_amd_setup.exe'
# Exes the runtime is taken out of (never run): danielblnc's setup, NeuralHub's manager and
# NeuralHub's setup, all next to Setup.bat.
$exeSources = @()
if (Test-Path -LiteralPath $setup -PathType Leaf) { $exeSources += $setup }
$hubManager = Join-Path $Root 'neuralhub.exe'
if (Test-Path -LiteralPath $hubManager -PathType Leaf) { $exeSources += $hubManager }
$exeSources += @(Get-ChildItem -LiteralPath $Root -Filter 'NeuralHub*.exe' -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -ine 'neuralhub.exe' } | Sort-Object Name -Descending |
    Select-Object -ExpandProperty FullName)
$nv      = Find-FirstFile @(
    (Join-Path $Root 'nvngx_dlssnr.dll'),
    (Join-Path $game 'nvngx_dlssnr.dll')
)
# NGXC DLSS model/provider DLLs used by the 0.8.0 host NGX path (AmdPreSr::ArmDlssNgx):
# DLSS-SR from nvngx_dlss.dll and DLSS-RR from nvngx_dlssd.dll. Copied next to the game's exe
# and into dlssnr_amd_host (beside neuralhub_host.dll); ArmDlssNgx points the DLSS search at
# the host folder's copies when the host module is armed. Not needed for runtimes before 0.8.0.
$dlssModelNames = @('nvngx_dlss.dll', 'nvngx_dlssd.dll')
$dlssModelSources = @{}
foreach ($dlssModelName in $dlssModelNames) {
    $dlssModelSources[$dlssModelName] = Find-FirstFile @(
        (Join-Path $Root $dlssModelName),
        (Join-Path $Root ("nvngx\" + $dlssModelName)),
        (Join-Path $game $dlssModelName),
        (Join-Path $game ("dlssnr_amd_host\" + $dlssModelName))
    )
}
function Find-Weights {
    return (Find-FirstFile @(
        (Join-Path $Root 'dlssnr_on_amd_weights.bin'),
        (Join-Path $game 'dlssnr_on_amd_weights.bin'),
        (Join-Path $game 'dlssnr_amd_host\dlssnr_on_amd_weights.bin'),
        (Get-NeuralHubWeights)
    ))
}
$weights = Find-Weights

# -AuthorDll may name an exe (neuralhub.exe, NeuralHub's or danielblnc's setup): use the runtime inside it.
if ($AuthorDll -and (Test-Path -LiteralPath $AuthorDll -PathType Leaf) -and $AuthorDll -match '(?i)\.exe$') {
    $fromAuthorExe = Get-RuntimeFromExe $AuthorDll
    if (-not $fromAuthorExe) {
        Fail "-AuthorDll $AuthorDll holds no DLSS-NR-on-AMD runtime this package supports."
    }
    Write-Host ("Took danielblnc runtime {0} out of {1} (not run)." -f (Get-RuntimeName (Get-Sha256 $fromAuthorExe)), $AuthorDll) -ForegroundColor Green
    $AuthorDll = $fromAuthorExe
}
$srcA = Find-AuthorRuntime

$canLmxxf  = [bool]($lmxxfRuntime -and $lmxxfMods)
# Weights in NeuralHub's data folder alone do not ask for this backend.
$weightsHere = Find-FirstFile @((Join-Path $Root 'dlssnr_on_amd_weights.bin'), (Join-Path $game 'dlssnr_on_amd_weights.bin'))
$canDaniel = [bool]($srcA -or $exeSources.Count -gt 0 -or $weightsHere)

$installLmxxf  = $false
$installDaniel = $false
$activeBackend = 'lmxxf'

if ($canLmxxf -and $canDaniel) {
    if ($NonInteractive) {
        $installLmxxf  = $true
        $installDaniel = $true
        $activeBackend = 'lmxxf'
    } else {
        $choice = Ask-Choice "Detected files for both neural rendering backends. Select installation mode:" @(
            "Install lmxxf backend (using native-game-tiled-assets)",
            "Install danielblnc backend (using dlssnr_amd_pass / weights.bin)",
            "Install both backends (coexist in game folder, switch via OptiScaler.ini)"
        )
        switch ($choice) {
            1 { $installLmxxf = $true; $installDaniel = $false; $activeBackend = 'lmxxf' }
            2 { $installLmxxf = $false; $installDaniel = $true; $activeBackend = 'daniel' }
            3 {
                $installLmxxf  = $true
                $installDaniel = $true
                $defChoice = Ask-Choice "Select default active backend in OptiScaler.ini:" @(
                    "Default: lmxxf backend",
                    "Default: danielblnc backend"
                )
                $activeBackend = if ($defChoice -eq 1) { 'lmxxf' } else { 'daniel' }
            }
        }
    }
} elseif ($canLmxxf) {
    $installLmxxf  = $true
    $installDaniel = $false
    $activeBackend = 'lmxxf'
    Write-Host ''
    Write-Host "Detected lmxxf backend components. Preparing to install lmxxf backend." -ForegroundColor Cyan
    Write-Host "NOTE: danielblnc backend files not detected (missing NeuralHub, neuralhub.exe, dlssnr_on_amd_setup.exe or version.dll)." -ForegroundColor DarkYellow
} elseif ($canDaniel) {
    $installLmxxf  = $false
    $installDaniel = $true
    $activeBackend = 'daniel'
    Write-Host ''
    Write-Host "Detected danielblnc backend components. Preparing to install danielblnc backend." -ForegroundColor Cyan
    Write-Host "NOTE: lmxxf backend components not detected (missing LmxxfNrRuntime.dll or lmxxf-modules)." -ForegroundColor DarkYellow
} else {
    Fail @"
No valid neural rendering backend files detected.
- If using lmxxf backend: ensure LmxxfNrRuntime.dll and lmxxf-modules (and native-game-tiled-assets weights folder) are present.
- If using danielblnc backend: install NeuralHub 0.8.0, or place neuralhub.exe / neuralhub_host.dll + nvngx_dlssnr.dll,
  or dlssnr_on_amd_setup.exe + nvngx_dlssnr.dll (or existing version.dll + weights.bin) next to Setup.bat.
"@
}

# --- process danielblnc runtime if selected ---
$stagedA = $null
# A single-module runtime (0.8.0): pass1 only, and without weights OptiScaler builds them at launch.
$single = $false
$buildWeights = $false
if ($installDaniel) {
    $gameHasNv = Test-Path -LiteralPath (Join-Path $game 'nvngx_dlssnr.dll') -PathType Leaf
    $gameHasW  = Test-Path -LiteralPath (Join-Path $game 'dlssnr_on_amd_weights.bin') -PathType Leaf
    if (-not $gameHasNv -and -not $gameHasW) {
        $srcNv = Join-Path $Root 'nvngx_dlssnr.dll'
        if (Test-Path -LiteralPath $srcNv -PathType Leaf) {
            Copy-Item -LiteralPath $srcNv -Destination (Join-Path $game 'nvngx_dlssnr.dll') -Force
            Write-Host 'Copied nvngx_dlssnr.dll into the game folder (danielblnc runtime expects it there).' -ForegroundColor Green
            $nv = Join-Path $game 'nvngx_dlssnr.dll'
        }
    }

    # Always look inside every exe that is present: a user updating from 0.3.1 drops the new setup
    # next to Setup.bat while the old runtime is still there (the version.dll an earlier install
    # saved beside Setup.bat, or pass1 in the game), and must get the setup's newer runtime. The
    # same goes for neuralhub.exe and NeuralHub's setup. None of them is run.
    $considerExe = {
        param([string]$exe)
        $exeName = Split-Path -Leaf $exe
        $fromSetup = Get-RuntimeFromExe $exe
        if ($fromSetup) {
            $setupHash = Get-Sha256 $fromSetup
            $setupName = Get-RuntimeName $setupHash
            $foundHash = $null
            if ($srcA) { $foundHash = Get-Sha256 $srcA }
            $foundName = Get-RuntimeName $foundHash
            $newer = (Get-RuntimeRank $setupHash) -gt (Get-RuntimeRank $foundHash)
            # Only a known -AuthorDll that was actually picked holds against a newer setup.
            $explicitA = [bool]($AuthorDll -and $srcA -and ($srcA -ieq $AuthorDll))
            if (-not $srcA -or ($newer -and -not $explicitA)) {
                if ($srcA) {
                    Write-Host ("{3} holds runtime {0}, newer than the {1} at {2}; installing {0}." -f $setupName, $foundName, $srcA, $exeName) -ForegroundColor Yellow
                }
                $srcA = $fromSetup
                # Removed with the other staged copies once the install is done.
                $stagedA = $fromSetup
                Write-Host "Took danielblnc runtime $setupName out of $exeName (not run): $srcA" -ForegroundColor Green
            } else {
                if ($newer) {
                    Write-Host ("WARNING: {2} holds runtime {0}, newer than the {1} given by -AuthorDll; installing {1} as asked." -f $setupName, $foundName, $exeName) -ForegroundColor Yellow
                } elseif ($setupHash -ine $foundHash) {
                    Write-Host ("NOTE: {3} holds runtime {0}, older than the {1} at {2}; installing {1}." -f $setupName, $foundName, $srcA, $exeName) -ForegroundColor Yellow
                }
                try { Remove-Item -LiteralPath $fromSetup -Force } catch { }
            }
        } elseif ($srcA) {
            Write-Host ("NOTE: {2} holds no runtime this package supports (0.8.0, 0.6.0, 0.5.x, 0.4.x, 0.3.1, 0.3.0); installing the {0} at {1}." -f (Get-RuntimeName (Get-Sha256 $srcA)), $srcA, $exeName) -ForegroundColor Yellow
        }
    }
    foreach ($exe in $exeSources) { . $considerExe $exe }

    # An installed NeuralHub whose runtime folder is missing or older still has its manager in the
    # store, with the runtime inside. Looked at only while no 0.8.0 is at hand.
    $storeManager = $null
    if ($hub.Store) { $storeManager = Join-Path $hub.Store 'neuralhub.exe' }
    $haveRank = -1
    if ($srcA) { $haveRank = Get-RuntimeRank (Get-Sha256 $srcA) }
    if ($storeManager -and (Test-Path -LiteralPath $storeManager -PathType Leaf) -and
        $haveRank -lt (Get-RuntimeRank $expectedA080) -and
        -not ($AuthorDll -and $srcA -and ($srcA -ieq $AuthorDll)) -and
        -not ($exeSources | Where-Object { [IO.Path]::GetFullPath($_) -ieq [IO.Path]::GetFullPath($storeManager) })) {
        Write-Host "Looking for the runtime in the installed NeuralHub manager: $storeManager"
        . $considerExe $storeManager
    }

    # Before 0.8.0 this checks whether nvngx_dlssnr.dll can build the weights in place of the setup.
    function Find-BuildNv {
        $seen = @()
        foreach ($c in @($nv, (Join-Path $game 'nvngx_dlssnr.dll'), (Join-Path $Root 'nvngx_dlssnr.dll'), (Get-NeuralHubNv))) {
            if (-not $c -or !(Test-Path -LiteralPath $c -PathType Leaf)) { continue }
            $full = [IO.Path]::GetFullPath($c)
            if ($seen -contains $full) { continue }
            $seen += $full
            $v = Get-NvVersion $c
            if ($v -and $v -ge $minNvForBuild) { return $c }
            $what = if ($v) { "version $v" } else { 'of unknown version' }
            Write-Host ("NOTE: {0} is nvngx_dlssnr.dll {1}; 0.8.0 builds its weights only from {2} or later." -f $c, $what, $minNvForBuild) -ForegroundColor Yellow
        }
        return $null
    }
    $srcIsSingle = [bool]($srcA -and ($singleModule -contains (Get-Sha256 $srcA)))
    $buildNv = $null
    if ($srcIsSingle -and -not $weights) { $buildNv = Find-BuildNv }

    if ((-not $srcA -or (-not $weights -and -not $buildNv)) -and (Test-Path -LiteralPath $setup -PathType Leaf)) {
        Write-Host ''
        Write-Host 'version.dll and/or weights.bin not found yet.' -ForegroundColor Yellow
        Write-Host 'Launching danielblnc setup (dlssnr_on_amd_setup.exe) to create them…' -ForegroundColor Yellow
        if ($nv) { Write-Host "  nvngx_dlssnr found: $nv" } else {
            Write-Host '  NOTE: no nvngx_dlssnr.dll next to Setup.bat or in the game folder.' -ForegroundColor Yellow
            Write-Host '  The danielblnc setup will ask you to locate it if it needs one for weights.' -ForegroundColor Yellow
        }
        Write-Host '  In the danielblnc UI: pick the GAME folder if asked, finish install/close when done.'
        Push-Location $Root
        try {
            $p = Start-Process -FilePath $setup -WorkingDirectory $Root -Wait -PassThru
            Write-Host "  danielblnc setup exit code: {0}" -f $p.ExitCode
        } finally { Pop-Location }

        $weights = Find-Weights
        if (-not $srcA) {
            $srcA = Find-AuthorRuntime
        }
    }

    if (-not $srcA -or !(Test-Path -LiteralPath $srcA -PathType Leaf)) {
        Fail @"
Still missing a known DLSS-NR-on-AMD runtime (version.dll or neuralhub_host.dll).
Supported: 0.8.0 (recommended), 0.6.0, 0.5.1, 0.5.0, 0.4.3, 0.4.2, 0.4.1, 0.4.0, 0.3.1 or 0.3.0.
0.8.0 comes with NeuralHub (NeuralHub_v0.8.0_setup.exe):
1. Install NeuralHub 0.8.0 and run Setup.bat again (its runtime is found without help), or
2. Put neuralhub.exe (the installed manager) or neuralhub_host.dll next to Setup.bat, or
3. Put NeuralHub_v0.8.0_setup.exe next to Setup.bat with 7-Zip installed (it is unpacked, not run)
For the older ones, run dlssnr_on_amd_setup.exe yourself and put the version.dll it produces next
to Setup.bat (or leave it in the game folder).
Download from https://github.com/danielblnc/DLSS-NR-on-AMD/releases
"@
    }

    $hashA = Get-Sha256 $srcA
    $nameA = Get-RuntimeName $hashA
    if ($nameA) {
        Write-Host ("danielblnc runtime {0} (SHA256 {1})" -f $nameA, $hashA) -ForegroundColor Green
        if ((Get-RuntimeRank $hashA) -lt (Get-RuntimeRank $expectedA080)) {
            Write-Host ("NOTE: runtime {0} is supported, but 0.8.0 (NeuralHub) is recommended." -f $nameA) -ForegroundColor Yellow
        }
    } else {
        Write-Host ("danielblnc runtime SHA256: {0}" -f $hashA)
    }
    if ($expectedAuthor -notcontains $hashA) {
        $what = 'unknown build'
        if ($hashA -eq $knownA0217) { $what = 'this is 0.2.17, not 0.3.0/0.3.1/0.4.x/0.5.x/0.6.0/0.8.0' }
        Fail @"
$srcA is not a supported DLSS-NR-on-AMD runtime ($what).
  file:     $srcA
  got:      $hashA
  expected: $expectedA080 (0.8.0), $expectedA060 (0.6.0), $expectedA051 (0.5.1), $expectedA050 (0.5.0), $expectedA043 (0.4.3), $expectedA042 (0.4.2), $expectedA041 (0.4.1), $expectedA040 (0.4.0), $expectedA031 (0.3.1) or $expectedA030 (0.3.0)
Get 0.8.0 with NeuralHub from https://github.com/danielblnc/DLSS-NR-on-AMD/releases
"@
    }
    $single = $singleModule -contains $hashA

    try {
        $srcAFull = [IO.Path]::GetFullPath($srcA)
        $gameFull = [IO.Path]::GetFullPath($game)
        $rootFull = [IO.Path]::GetFullPath($Root)
        $gamePrefix = $gameFull.TrimEnd('\') + '\'
        $inGame = $srcAFull.StartsWith($gamePrefix, [StringComparison]::OrdinalIgnoreCase)
        $pkgIsGame = ($rootFull.TrimEnd('\') -ieq $gameFull.TrimEnd('\'))
        if ($inGame) {
            $pkgVersion = Join-Path $Root 'version.dll'
            if ($pkgIsGame -or (Test-Path -LiteralPath $pkgVersion -PathType Leaf)) {
                # Never overwrite a version.dll next to Setup.bat here: it may be another runtime.
                # The package copy is refreshed, keeping the old one, after the install.
                $stagedA = New-TempRuntimePath
            } else {
                $stagedA = $pkgVersion
            }
            if ($srcAFull -ieq [IO.Path]::GetFullPath($stagedA)) {
                # Already the staged location.
            } else {
                Copy-Item -LiteralPath $srcAFull -Destination $stagedA -Force
                Write-Host "Copied danielblnc runtime outside game folder: $stagedA"
            }
            $srcA = $stagedA
        }
    } catch {
        Fail "Could not stage danielblnc runtime from $srcA : $($_.Exception.Message)"
    }

    if ($single -and -not $weights) {
        # 0.8.0 builds dlssnr_on_amd_weights.bin itself: OptiScaler hands it nvngx_dlssnr.dll from
        # beside dlssnr_amd_pass1.dll (or the game's exe) in dlssnr_amd_host, on the first launch.
        if (-not $buildNv -and -not $srcIsSingle) { $buildNv = Find-BuildNv }
        if (-not $buildNv) {
            Fail @"
Missing dlssnr_on_amd_weights.bin, and no nvngx_dlssnr.dll $minNvForBuild or later to build it from.
Put nvngx_dlssnr.dll (310.8.0.0 or later; NVIDIA's DLSS Ray Reconstruction/NR DLL) next to Setup.bat
or in the game folder, or import it in NeuralHub, or put an existing dlssnr_on_amd_weights.bin there.
OptiScaler builds the weights from it on the first launch.
"@
        }
        $gameNv = Join-Path $game 'nvngx_dlssnr.dll'
        $hostDir = Join-Path $game 'dlssnr_amd_host'
        if ([IO.Path]::GetFullPath($buildNv) -ieq [IO.Path]::GetFullPath($gameNv)) {
            # Already beside dlssnr_amd_pass1.dll.
        } elseif (!(Test-Path -LiteralPath $gameNv -PathType Leaf)) {
            Copy-Item -LiteralPath $buildNv -Destination $gameNv -Force
            Write-Host "Copied nvngx_dlssnr.dll into the game folder (OptiScaler builds the weights from it): $buildNv" -ForegroundColor Green
        } else {
            # The game's own nvngx_dlssnr.dll is too old and stays; the host folder's copy comes first.
            [void][IO.Directory]::CreateDirectory($hostDir)
            Copy-Item -LiteralPath $buildNv -Destination (Join-Path $hostDir 'nvngx_dlssnr.dll') -Force
            Write-Host "Copied nvngx_dlssnr.dll into dlssnr_amd_host (OptiScaler builds the weights from it): $buildNv" -ForegroundColor Green
        }
        $nv = $buildNv
        $buildWeights = $true
    } else {
        if (-not $weights) {
            $weights = Join-Path $Root 'dlssnr_on_amd_weights.bin'
        }
        if (-not (Test-Path -LiteralPath $weights -PathType Leaf)) {
            if ((Test-Path -LiteralPath $setup) -and $nv) {
                Write-Host 'weights.bin still missing, running danielblnc setup again with nvngx...'
                Push-Location $Root
                try { Start-Process -FilePath $setup -WorkingDirectory $Root -Wait | Out-Null } finally { Pop-Location }
                $weights = Find-Weights
            }
        }
        if (-not $weights -or !(Test-Path -LiteralPath $weights -PathType Leaf)) {
            Fail @"
Missing dlssnr_on_amd_weights.bin.
Run dlssnr_on_amd_setup.exe (with nvngx_dlssnr.dll available), then retry.
With runtime 0.8.0 (NeuralHub) nvngx_dlssnr.dll 310.8.0.0 or later is enough: OptiScaler builds the weights.
"@
        }
    }

    if ($nv) {
        $nvSize = (Get-Item -LiteralPath $nv).Length
        Write-Host ("nvngx size: {0} bytes  ({1})" -f $nvSize, $nv)
        if ($nvSize -lt 1MB) {
            Confirm-Continue ("nvngx is only {0} bytes — may be the wrong file. Continue?" -f $nvSize)
        }
    }

    if ($buildWeights) {
        Write-Host 'dlssnr_on_amd_weights.bin: none yet; OptiScaler builds it from nvngx_dlssnr.dll on the first launch (dlssnr_amd_host).' -ForegroundColor Yellow
    } else {
        $wsize = (Get-Item -LiteralPath $weights).Length
        $whash = Get-Sha256 $weights
        Write-Host ("weights.bin size={0}  SHA256={1}" -f $wsize, $whash)
        Write-Host '  (weights SHA256 is per-machine; not compared to a fixed value)'
        if ($wsize -lt 1MB) {
            Confirm-Continue ("weights.bin is only {0} bytes, which looks truncated or wrong. Continue?" -f $wsize)
        }
    }
}

# --- inspect common injection DLLs ---
$proxies = @(
    'dxgi.dll','winmm.dll','d3d12.dll','version.dll',
    'winhttp.dll','wininet.dll','dbghelp.dll','dinput8.dll'
)
$found = @()
foreach ($name in $proxies) {
    $p = Join-Path $game $name
    if (Test-Path -LiteralPath $p -PathType Leaf) {
        $item = Get-Item -LiteralPath $p
        $found += [pscustomobject]@{
            Name = $name
            Path = $p
            Size = $item.Length
            IsOptiScaler = (Test-OptiProxy $p)
        }
    }
}

Write-Host ''
Write-Host "Game folder: $game"
Write-Host "Proxy:       $Proxy   (OptiScaler.dll installed under this name)"
if ($installDaniel) {
    if ($single) {
        Write-Host "danielblnc runtime:      $srcA  -> will be copied as dlssnr_amd_pass1.dll (0.8.0 runs as one module; OptiScaler hosts it as dlssnr_amd_host\neuralhub_host.dll)"
    } else {
        Write-Host "danielblnc runtime:      $srcA  -> will be copied as dlssnr_amd_pass1/2/3.dll"
    }
    Write-Host 'NOTE: danielblnc version.dll is NOT left in the game folder (this package only installs OptiScaler as the proxy).'
}
if ($installLmxxf) {
    Write-Host "lmxxf runtime:           $lmxxfRuntime  -> will be copied to game folder"
}
if ($found.Count -eq 0) {
    Write-Host 'No common injection DLLs found in the game folder.' -ForegroundColor Green
} else {
    Write-Host 'Existing injection-related DLLs:' -ForegroundColor Yellow
    foreach ($f in $found) {
        $tag = if ($f.IsOptiScaler) { ' [OptiScaler]' } else { '' }
        Write-Host ("  - {0}  ({1} bytes){2}" -f $f.Name, $f.Size, $tag)
    }
    Write-Host 'For ReShade or another mod: choose Ignore only if you know they can coexist.'
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$backup = Join-Path $game "backup-amd-presr-$stamp"
$skipBackup = $false
$backupCreated = $false
$toMove = @()
# Files the user chose to keep. The install step must not overwrite these.
$keep = @{}

foreach ($f in $found) {
    $isTarget = ($f.Name -ieq $Proxy)
    # danielblnc native version.dll (hash already known) must not stay next to OptiScaler.
    $isAuthorNative = $false
    if ($f.Name -ieq 'version.dll' -and -not $f.IsOptiScaler) {
        try {
            $h = Get-Sha256 $f.Path
            if ($expectedAuthor -contains $h -or $h -eq $knownA0217) { $isAuthorNative = $true }
        } catch { }
    }
    if ($isAuthorNative) {
        Write-Host ("{0} is the danielblnc NR runtime — moving aside (cannot coexist with OptiScaler)." -f $f.Name) -ForegroundColor Yellow
        $toMove += $f
        continue
    }
    # NeuralHub installs its stub under a proxy name; the stub loads the runtime into the game itself.
    if (-not $f.IsOptiScaler -and (Test-NeuralHubStub $f.Path)) {
        Write-Host ("{0} is NeuralHub's stub (it loads the danielblnc runtime by itself). Moving it aside, since it cannot coexist with OptiScaler." -f $f.Name) -ForegroundColor Yellow
        $toMove += $f
        continue
    }
    if ($f.IsOptiScaler) {
        if ($isTarget) {
            if ($NonInteractive) {
                $toMove += $f
                continue
            }
            $choice = Ask-Choice ("{0} is already an OptiScaler install. How to continue?" -f $f.Name) @(
                'Cancel install'
                'Backup existing files, then install'
                'Direct overwrite (clean in-place overwrite)'
            )
            switch ($choice) {
                1 { Write-Host 'Cancelled.'; Pause-Exit 0 }
                2 {
                    $skipBackup = $false
                    $toMove += $f
                }
                3 {
                    $skipBackup = $true
                    # Direct overwrite: do not move or backup; Install-One overwrites directly
                }
            }
        } else {
            # An existing OptiScaler proxy with a DIFFERENT name (e.g. winmm.dll while installing dxgi.dll).
            # Two OptiScaler proxies will hook the process twice, causing crashes / double injection.
            if ($NonInteractive) {
                Write-Host ("{0} is a previous OptiScaler proxy — moving to backup to prevent duplicate injection with {1}." -f $f.Name, $Proxy) -ForegroundColor Yellow
                $toMove += $f
                continue
            }
            $choice = Ask-Choice ("{0} is a previous OptiScaler proxy (cannot coexist with {1}; causes double injection). How to continue?" -f $f.Name, $Proxy) @(
                'Cancel install'
                'Move previous OptiScaler proxy to backup (Recommended)'
                'Ignore and leave in place (Not recommended; may crash)'
            )
            switch ($choice) {
                1 { Write-Host 'Cancelled.'; Pause-Exit 0 }
                2 { $toMove += $f }
                3 {
                    $keep[$f.Name] = $true
                    Write-Host ("WARNING: Leaving {0} in place alongside {1}." -f $f.Name, $Proxy) -ForegroundColor Red
                }
            }
        }
    } else {
        if ($NonInteractive) {
            if ($isTarget) {
                Fail ("{0} exists and is not OptiScaler. Refusing to overwrite in -NonInteractive. Backup/remove it or pick another -Proxy." -f $f.Name)
            }
            Write-Host ("WARNING: {0} exists (not OptiScaler); leaving it." -f $f.Name)
            $keep[$f.Name] = $true
            continue
        }
        $choice = Ask-Choice ("{0} exists and is not OptiScaler. How to continue?" -f $f.Name) @(
            'Cancel install'
            'Backup and move aside, then install'
            'Ignore and continue (keep this file)'
        )
        switch ($choice) {
            1 { Write-Host 'Cancelled.'; Pause-Exit 0 }
            2 { $toMove += $f }
            3 {
                if ($isTarget) {
                    Fail ("You chose to keep {0}, but that is also the proxy name we would install as. Pick a different -Proxy (e.g. winmm.dll) or choose Backup and move aside." -f $f.Name)
                }
                $keep[$f.Name] = $true
                Write-Host ("Keeping {0}." -f $f.Name)
            }
        }
    }
}

# Create the backup folder if backup is enabled or if there are files to safely move aside
if ((-not $skipBackup) -or ($toMove.Count -gt 0)) {
    if (-not (Test-Path -LiteralPath $backup)) {
        [void][System.IO.Directory]::CreateDirectory($backup)
    }
    $backupCreated = $true
}

# Any file scheduled for movement is ALWAYS safely backed up to .moved; NEVER silently deleted
foreach ($f in $toMove) {
    if (-not (Test-Path -LiteralPath $backup)) {
        [void][System.IO.Directory]::CreateDirectory($backup)
        $backupCreated = $true
    }
    Copy-Item -LiteralPath $f.Path -Destination (Join-Path $backup $f.Name) -Force
    Move-Item -LiteralPath $f.Path -Destination (Join-Path $backup ($f.Name + '.moved')) -Force
    Write-Host ("Moved {0} -> backup" -f $f.Name)
}

function Install-One([string]$src, [string]$rel) {
    $leaf = Split-Path -Leaf $rel
    if ($keep.ContainsKey($leaf)) {
        Write-Host ("Skip (user kept existing file): {0}" -f $rel)
        return
    }
    $dest = Join-Path $game $rel
    # Reuse a verified file that is already at the destination (e.g. weights/pass in game dir).
    try {
        $srcFull = [IO.Path]::GetFullPath($src)
        $destFull = [IO.Path]::GetFullPath($dest)
        if ($srcFull -ieq $destFull) {
            Write-Host ("Skip (already in place): {0}" -f $rel)
            return
        }
    } catch { }
    # Weights are immutable neural network parameters (~700MB total).
    # They are not original game files and must never bloat backup directories.
    $isWeight = ($rel -ieq 'dlssnr_on_amd_weights.bin') -or
                ($rel -ilike 'native-game-tiled-assets\*') -or
                ($rel -ilike 'native-game-tiled-assets/*')

    try {
        if (Test-Path -LiteralPath $dest) {
            # If a weight file already exists at the destination, keep it in place (no backup, no re-copy)
            if ($isWeight) {
                return
            }
            if (-not $skipBackup) {
                $save = Join-Path $backup $rel
                $sdir = Split-Path -Parent $save
                if ($sdir) { [void][System.IO.Directory]::CreateDirectory($sdir) }
                Copy-Item -LiteralPath $dest -Destination $save -Force
                $backupCreated = $true
            }
        }
        $ddir = Split-Path -Parent $dest
        if ($ddir) { [void][System.IO.Directory]::CreateDirectory($ddir) }
        Copy-Item -LiteralPath $src -Destination $dest -Force
    } catch [System.IO.IOException] {
        Fail @"
Install failed on $rel :
  $($_.Exception.Message)

The file is probably locked by a running game.
Close the game completely, then run Setup.bat again.
Partial files (if any) are under:
  $backup
"@
    }
}

function Set-IniSettings([string]$iniPath, [string]$sectionName, [System.Collections.IDictionary]$settings) {
    if (-not (Test-Path -LiteralPath $iniPath -PathType Leaf)) { return }
    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.AddRange([System.IO.File]::ReadAllLines($iniPath))
    $sectionIdx = -1
    for ($i = 0; $i -lt $lines.Count; ++$i) {
        if ($lines[$i] -match ('^\s*\[' + [regex]::Escape($sectionName) + '\]\s*$')) {
            $sectionIdx = $i
            break
        }
    }
    if ($sectionIdx -lt 0) {
        $lines.Add("")
        $lines.Add("[$sectionName]")
        foreach ($k in $settings.Keys) {
            $lines.Add("$k = $($settings[$k])")
        }
    } else {
        $endIdx = $sectionIdx + 1
        while ($endIdx -lt $lines.Count -and $lines[$endIdx] -notmatch '^\s*\[') { ++$endIdx }
        foreach ($k in $settings.Keys) {
            $found = $false
            for ($i = $sectionIdx + 1; $i -lt $endIdx; ++$i) {
                if ($lines[$i] -match ('^\s*' + [regex]::Escape($k) + '\s*=')) {
                    $lines[$i] = "$k = $($settings[$k])"
                    $found = $true
                    break
                }
            }
            if (-not $found) {
                $lines.Insert($endIdx, "$k = $($settings[$k])")
                ++$endIdx
            }
        }
    }
    [System.IO.File]::WriteAllLines($iniPath, $lines, [System.Text.UTF8Encoding]::new($true))
}

Write-Host ''
Write-Host "Installing OptiScaler as $Proxy ..." -ForegroundColor Cyan
Install-One (Join-Path $release 'OptiScaler.dll') $Proxy

$ini = Join-Path $release 'OptiScaler.ini'
$gameIni = Join-Path $game 'OptiScaler.ini'
if (Test-Path -LiteralPath $ini -PathType Leaf) {
    if (-not (Test-Path -LiteralPath $gameIni -PathType Leaf)) {
        # First install: copy package ini. Upgrades keep the game ini and upsert [DlssNr] keys below.
        Install-One $ini 'OptiScaler.ini'
    }
}
$deps = Join-Path $release 'OptiScaler'
if (Test-Path -LiteralPath $deps) {
    Get-ChildItem -LiteralPath $deps -Recurse -File | ForEach-Object {
        $rel = Join-Path 'OptiScaler' $_.FullName.Substring($deps.Length).TrimStart('\','/')
        Install-One $_.FullName $rel
    }
}

# --- Install selected backends ---
if ($installDaniel) {
    if ($single) {
        Write-Host 'Installing danielblnc runtime 0.8.0 (dlssnr_amd_pass1.dll, one module)...' -ForegroundColor Cyan
        Install-One $srcA 'dlssnr_amd_pass1.dll'
        # Passes 2 and 3 are copies for the runtimes before 0.8.0; one left by an earlier install goes.
        foreach ($p in 2..3) {
            $stale = Join-Path $game "dlssnr_amd_pass$p.dll"
            if (Test-Path -LiteralPath $stale -PathType Leaf) {
                try {
                    Remove-Item -LiteralPath $stale -Force
                    Write-Host ("  removed stale dlssnr_amd_pass{0}.dll (0.8.0 runs as one module)" -f $p) -ForegroundColor DarkYellow
                } catch {
                    Fail "Could not remove stale $stale : $($_.Exception.Message). Close the game, then run Setup.bat again."
                }
            }
        }
    } else {
        Write-Host 'Installing danielblnc runtime (dlssnr_amd_pass1-3.dll) + weights.bin...' -ForegroundColor Cyan
        foreach ($p in 1..3) {
            Install-One $srcA ("dlssnr_amd_pass$p.dll")
        }
    }
    $gameWeightsBin = Join-Path $game 'dlssnr_on_amd_weights.bin'
    $hostWeightsBin = Join-Path $game 'dlssnr_amd_host\dlssnr_on_amd_weights.bin'
    if (Test-Path -LiteralPath $gameWeightsBin -PathType Leaf) {
        Write-Host 'dlssnr_on_amd_weights.bin already present in game folder.' -ForegroundColor Green
    } elseif ($buildWeights) {
        Write-Host 'dlssnr_on_amd_weights.bin: OptiScaler builds it from nvngx_dlssnr.dll on the first launch (in dlssnr_amd_host).' -ForegroundColor Yellow
    } elseif ($single -and (Test-Path -LiteralPath $hostWeightsBin -PathType Leaf)) {
        Write-Host 'dlssnr_on_amd_weights.bin already built in dlssnr_amd_host.' -ForegroundColor Green
    } else {
        Install-One $weights 'dlssnr_on_amd_weights.bin'
    }

    if ($single) {
        # DLSS model DLLs for the 0.8.0 host NGX path. When present they are copied next to the
        # game's exe and into dlssnr_amd_host (the host folder ArmDlssNgx reads them from); a
        # copy already in place is left as-is. Missing ones do not stop the install: only the
        # matching feature (SR from nvngx_dlss.dll, RR from nvngx_dlssd.dll) keeps working with
        # NVIDIA's stock DLL placed there manually.
        $hostModelDir = Join-Path $game 'dlssnr_amd_host'
        foreach ($dlssModelName in $dlssModelNames) {
            $srcModel = $dlssModelSources[$dlssModelName]
            if (-not $srcModel) {
                Write-Host "NOTE: $dlssModelName not found; DLSS '$(if ($dlssModelName -ieq 'nvngx_dlss.dll') { 'DLSS-SR' } else { 'DLSS-RR' })' with the 0.8.0 host needs it next to Setup.bat (or in a nvngx\ subfolder). Place NVIDIA's $dlssModelName there and run Setup.bat again." -ForegroundColor DarkYellow
                continue
            }
            $alreadyGame = (Test-Path -LiteralPath (Join-Path $game $dlssModelName) -PathType Leaf) -and
                ((Get-Item -LiteralPath (Join-Path $game $dlssModelName)).Length -eq (Get-Item -LiteralPath $srcModel).Length)
            if (-not $alreadyGame) {
                Install-One $srcModel $dlssModelName
            }
            $alreadyHost = (Test-Path -LiteralPath (Join-Path $hostModelDir $dlssModelName) -PathType Leaf) -and
                ((Get-Item -LiteralPath (Join-Path $hostModelDir $dlssModelName)).Length -eq (Get-Item -LiteralPath $srcModel).Length)
            if (-not $alreadyHost) {
                if (-not (Test-Path -LiteralPath $hostModelDir -PathType Container)) {
                    [void][IO.Directory]::CreateDirectory($hostModelDir)
                }
                Copy-Item -LiteralPath $srcModel -Destination (Join-Path $hostModelDir $dlssModelName) -Force
                Write-Host "Copied $dlssModelName into dlssnr_amd_host (ArmDlssNgx reads the host module's DLSS provider from there)." -ForegroundColor Green
            } else {
                Write-Host "$dlssModelName already in dlssnr_amd_host." -ForegroundColor Green
            }
        }
    }
}

if ($installLmxxf) {
    Write-Host 'Installing lmxxf runtime + modules + shaders...' -ForegroundColor Cyan
    Install-One $lmxxfRuntime 'LmxxfNrRuntime.dll'
    Get-ChildItem -LiteralPath $lmxxfMods -Recurse -File | ForEach-Object {
        $rel = Join-Path 'lmxxf-modules' $_.FullName.Substring($lmxxfMods.Length).TrimStart('\','/')
        Install-One $_.FullName $rel
    }
    # The RX 9060 series' kernels sit beside them ("lmxxf-modules-gfx1200", or "modules-gfx1200" in the repo);
    # the runtime picks the folder by GPU.
    $lmxxfMods1200 = "$lmxxfMods-gfx1200"
    if (Test-Path -LiteralPath (Join-Path $lmxxfMods1200 'SHA256SUMS') -PathType Leaf) {
        Get-ChildItem -LiteralPath $lmxxfMods1200 -Recurse -File | ForEach-Object {
            $rel = Join-Path 'lmxxf-modules-gfx1200' $_.FullName.Substring($lmxxfMods1200.Length).TrimStart('\','/')
            Install-One $_.FullName $rel
        }
    }
    if ($lmxxfShaders) {
        $shaderKeep = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
        Get-ChildItem -LiteralPath $lmxxfShaders -Recurse -File | ForEach-Object {
            $relLeaf = $_.FullName.Substring($lmxxfShaders.Length).TrimStart('\','/')
            $rel = Join-Path 'shaders' $relLeaf
            # Keep set is package top-level *.hlsl only (relLeaf has no subdir).
            if ($_.Extension -ieq '.hlsl' -and ($relLeaf -notmatch '[\\/]')) {
                [void]$shaderKeep.Add($_.Name)
            }
            Install-One $_.FullName $rel
        }
        # Install is upsert-only for named files; purge retired native_/preblock_ *.hlsl left by older packages.
        $gameShadersDir = Join-Path $game 'shaders'
        if ((Test-Path -LiteralPath $gameShadersDir -PathType Container) -and $shaderKeep.Count -gt 0) {
            $stale = @(Get-ChildItem -LiteralPath $gameShadersDir -Filter '*.hlsl' -File -ErrorAction SilentlyContinue |
                Where-Object { ($_.Name -match '^(native_|preblock_)') -and (-not $shaderKeep.Contains($_.Name)) })
            foreach ($sf in $stale) {
                try {
                    Remove-Item -LiteralPath $sf.FullName -Force
                    Write-Host ("  removed stale shader: {0}" -f $sf.Name) -ForegroundColor DarkYellow
                } catch {
                    Write-Host ("  WARN: could not remove stale shader {0}: {1}" -f $sf.Name, $_.Exception.Message) -ForegroundColor Yellow
                }
            }
            if ($stale.Count -gt 0) {
                Write-Host ("  purged {0} stale shader(s); live set={1}" -f $stale.Count, $shaderKeep.Count) -ForegroundColor Cyan
            }
        }
        Write-Host "  shaders installed from $lmxxfShaders"
    } else {
        Write-Host 'NOTE: lmxxf shaders not found in package; PrepareFrame may fail until shaders/ is beside OptiScaler.' -ForegroundColor DarkYellow
    }
    $gameWeights = Join-Path $game 'native-game-tiled-assets'
    if (Test-Path -LiteralPath (Join-Path $gameWeights 'block0-ffn.f16') -PathType Leaf) {
        Write-Host 'native-game-tiled-assets already present in game folder.' -ForegroundColor Green
    } elseif ($lmxxfWeights) {
        if ([IO.Path]::GetFullPath($lmxxfWeights) -ine [IO.Path]::GetFullPath($gameWeights)) {
            Write-Host "Installing native-game-tiled-assets from $lmxxfWeights..." -ForegroundColor Cyan
            Get-ChildItem -LiteralPath $lmxxfWeights -Recurse -File | ForEach-Object {
                $rel = Join-Path 'native-game-tiled-assets' $_.FullName.Substring($lmxxfWeights.Length).TrimStart('\','/')
                Install-One $_.FullName $rel
            }
        } else {
            Write-Host 'native-game-tiled-assets already present in game folder.' -ForegroundColor Green
        }
    } else {
        Write-Host 'NOTE: native-game-tiled-assets not found in package. If using lmxxf, place native-game-tiled-assets in the game folder.' -ForegroundColor DarkYellow
    }


}

# --- Configure OptiScaler.ini: upsert package [DlssNr] defaults (no full-file overwrite) ---
# Existing game ini is kept; these keys are inserted or updated so release defaults reach upgrades.
if (Test-Path -LiteralPath $gameIni -PathType Leaf) {
    Set-IniSettings $gameIni 'DlssNr' ([ordered]@{
        'Enabled' = 'true'
        'RunBeforeSR' = 'true'
        'NrBackend' = $activeBackend
        'LmxxfDiagnostic' = 'off'
        'LmxxfFitLarge' = 'false'
        'AmdModelScale' = '1'
        'AmdEncoding' = 'auto'
        'AmdEveryFrame' = 'auto'
    })
    Write-Host "Upserted OptiScaler.ini [DlssNr] defaults (Enabled=true, NrBackend=$activeBackend, LmxxfFitLarge=false, ...)" -ForegroundColor Green
}

# Align DLSS5-AMD\native-game-flags.txt with final ini (runtime reads flags/env, not OptiScaler.ini).
# Rewrite FIT_LARGE every install when lmxxf is installed so upsert cannot disagree with a stale flags file.
if ($installLmxxf) {
    $fitLarge = $true
    if (Test-Path -LiteralPath $gameIni -PathType Leaf) {
        $fitLine = Select-String -Path $gameIni -Pattern '^\s*LmxxfFitLarge\s*=' -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($fitLine -and $fitLine.Line -match '=\s*(false|0)\s*$') { $fitLarge = $false }
    }
    $flagsDir = Join-Path $game 'DLSS5-AMD'
    New-Item -ItemType Directory -Force -Path $flagsDir | Out-Null
    $flagsPath = Join-Path $flagsDir 'native-game-flags.txt'
    $fitLineOut = "DLSS5_FIT_LARGE=$(if ($fitLarge) { '1' } else { '0' })"
    if (Test-Path -LiteralPath $flagsPath -PathType Leaf) {
        $existing = [IO.File]::ReadAllText($flagsPath)
        if ($existing -match '(?m)^DLSS5_FIT_LARGE=.*$') {
            $existing = [regex]::Replace($existing, '(?m)^DLSS5_FIT_LARGE=.*$', $fitLineOut)
        } else {
            if ($existing.Length -gt 0 -and -not $existing.EndsWith("`n") -and -not $existing.EndsWith("`r")) {
                $existing += "`r`n"
            }
            $existing += ($fitLineOut + "`r`n")
        }
        [IO.File]::WriteAllText($flagsPath, $existing)
    } else {
        [IO.File]::WriteAllText($flagsPath, ($fitLineOut + "`r`n"))
    }
    Write-Host "  aligned $flagsPath ($fitLineOut) with OptiScaler.ini LmxxfFitLarge" -ForegroundColor Green
}

# Uninstaller is copied into the game folder. Double-click it there; it
# targets that directory (no folder picker). Look next to Setup first —
# $release may be a legacy release\ subfolder that does not contain it.
$ps1Src = $null
foreach ($candidate in @(
        (Join-Path $Root 'Uninstall_OptiScaler_NR.ps1'),
        (Join-Path $release 'Uninstall_OptiScaler_NR.ps1'),
        (Join-Path $PSScriptRoot 'uninstall-amd-presr.ps1')
    )) {
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        $ps1Src = $candidate
        break
    }
}
if (-not $ps1Src) {
    Fail 'Missing Uninstall_OptiScaler_NR.ps1 next to Setup.ps1. Setup copies it into the game folder.'
}
Install-One $ps1Src 'Uninstall_OptiScaler_NR.ps1'

$batSrc = $null
foreach ($candidate in @(
        (Join-Path $Root 'Uninstall_OptiScaler_NR.bat'),
        (Join-Path $release 'Uninstall_OptiScaler_NR.bat')
    )) {
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        $batSrc = $candidate
        break
    }
}
if ($batSrc) {
    Install-One $batSrc 'Uninstall_OptiScaler_NR.bat'
} else {
    $batDest = Join-Path $game 'Uninstall_OptiScaler_NR.bat'
    @'
@echo off
setlocal
title OptiScaler AMD pre-SR Uninstall
rem Double-click in the game folder. Optional: Uninstall_OptiScaler_NR.bat "D:\GameFolder"
if "%~1"=="" (
  powershell -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0Uninstall_OptiScaler_NR.ps1" -NoPause
) else (
  powershell -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0Uninstall_OptiScaler_NR.ps1" -GameDir "%~1" -NoPause
)
set "EC=%ERRORLEVEL%"
if not "%EC%"=="0" echo Uninstall failed ^(exit code %EC%^). See the error above.
pause
exit /b %EC%
'@ | Set-Content -LiteralPath $batDest -Encoding ASCII
    Write-Host "Wrote uninstaller: $batDest" -ForegroundColor Green
}

# Small install record so Uninstall knows which proxy name this install used.
# Uninstall still verifies OptiScaler by VersionInfo; this is a hint, not a trust root.
$installMark = Join-Path $game 'amd-presr-install.txt'
try {
    @(
        'project=OptiScaler AMD pre-SR',
        ('proxy=' + $Proxy),
        ('backend=' + $activeBackend),
        ('installed=' + (Get-Date -Format 'yyyy-MM-ddTHH:mm:ssK')),
        ('game=' + $game)
    ) -join "`r`n" | Set-Content -LiteralPath $installMark -Encoding ASCII
    Write-Host "Wrote install record: $installMark" -ForegroundColor Green
} catch {
    Write-Host "NOTE: could not write install record: $($_.Exception.Message)" -ForegroundColor Yellow
}

# Keep reusable danielblnc files in the package folder for the next game.
# Never write them into the game folder — that would re-inject danielblnc version.dll next to OptiScaler.
# Summary must report what is truly kept in the package directory, not $srcA.
# When package directory is the game directory, $srcA points to a TEMP file deleted below.
# When package directory is not the game directory, the kept file is $Root\version.dll.
$keptA = $null
$keptW = $null
try {
    $rootFull = [IO.Path]::GetFullPath($Root)
    $gameFull = [IO.Path]::GetFullPath($game)
    $pkgIsGame = ($rootFull.TrimEnd('\') -ieq $gameFull.TrimEnd('\'))
    if (-not $pkgIsGame) {
        $pkgVersion = Join-Path $Root 'version.dll'
        if ($srcA -and (Test-Path -LiteralPath $srcA -PathType Leaf) -and -not (Test-Path -LiteralPath $pkgVersion -PathType Leaf)) {
            Copy-Item -LiteralPath $srcA -Destination $pkgVersion -Force
            Write-Host "Saved version.dll next to Setup.bat for the next install." -ForegroundColor Green
        } elseif ($installDaniel -and $srcA -and (Test-Path -LiteralPath $srcA -PathType Leaf) -and
            ([IO.Path]::GetFullPath($srcA) -ine [IO.Path]::GetFullPath($pkgVersion))) {
            # An older known runtime saved here by an earlier install would come back on the next
            # game; keep it under its version name and save the one just installed.
            $pkgHash = Get-Sha256 $pkgVersion
            $newHash = Get-Sha256 $srcA
            $pkgRank = Get-RuntimeRank $pkgHash
            if ($pkgRank -ge 0 -and $pkgRank -lt (Get-RuntimeRank $newHash)) {
                $pkgOld = Join-Path $Root ('version-{0}.dll' -f (Get-RuntimeName $pkgHash))
                Move-Item -LiteralPath $pkgVersion -Destination $pkgOld -Force
                Copy-Item -LiteralPath $srcA -Destination $pkgVersion -Force
                Write-Host ("Saved runtime {0} as version.dll next to Setup.bat; the {1} one is now {2}." -f (Get-RuntimeName $newHash), (Get-RuntimeName $pkgHash), (Split-Path -Leaf $pkgOld)) -ForegroundColor Green
            }
        }
        if (Test-Path -LiteralPath $pkgVersion -PathType Leaf) { $keptA = $pkgVersion }
        $pkgWeights = Join-Path $Root 'dlssnr_on_amd_weights.bin'
        if ($weights -and (Test-Path -LiteralPath $weights -PathType Leaf) -and
            -not (Test-Path -LiteralPath $pkgWeights -PathType Leaf)) {
            Copy-Item -LiteralPath $weights -Destination $pkgWeights -Force
            Write-Host "Saved dlssnr_on_amd_weights.bin next to Setup.bat for the next install." -ForegroundColor Green
        }
        if (Test-Path -LiteralPath $pkgWeights -PathType Leaf) { $keptW = $pkgWeights }
    }
    if ($stagedA -and (Test-Path -LiteralPath $stagedA -PathType Leaf)) {
        $stagedFull = [IO.Path]::GetFullPath($stagedA)
        if ($stagedFull.StartsWith($env:TEMP, [StringComparison]::OrdinalIgnoreCase) -or
            $stagedFull -match 'amd-presr-version-') {
            try { Remove-Item -LiteralPath $stagedA -Force } catch { }
        }
    }
} catch {
    Write-Host "NOTE: could not copy danielblnc files into the package folder: $($_.Exception.Message)" -ForegroundColor Yellow
}
Remove-TempRuntimes

Write-Host ''
Write-Host 'Done.' -ForegroundColor Green
Write-Host "  Game:           $game"
Write-Host "  Proxy:          $Proxy"
if (Test-Path -LiteralPath $backup) {
    if ($skipBackup -and $toMove.Count -gt 0) {
        Write-Host "  Backup:         $backup  (retained moved proxy)"
    } else {
        Write-Host "  Backup:         $backup"
    }
} else {
    Write-Host '  Backup:         (none - direct overwrite)'
}
Write-Host "  Active Backend: $activeBackend" -ForegroundColor Cyan
if ($installDaniel -and $installLmxxf) {
    Write-Host "  Installed:      Both backends (lmxxf + danielblnc) coexisting" -ForegroundColor Green
    Write-Host "  Tip:            To switch backend, edit OptiScaler.ini ([DlssNr] NrBackend=lmxxf or daniel) or re-run Setup.bat." -ForegroundColor Yellow
} elseif ($installLmxxf) {
    Write-Host "  Installed:      OptiScaler + lmxxf runtime (LmxxfNrRuntime.dll, modules, shaders)" -ForegroundColor Green
} else {
    if ($single -and $buildWeights) {
        Write-Host "  Installed:      OptiScaler + danielblnc runtime 0.8.0 (dlssnr_amd_pass1.dll; weights built at first launch)" -ForegroundColor Green
    } elseif ($single) {
        Write-Host "  Installed:      OptiScaler + danielblnc runtime 0.8.0 (dlssnr_amd_pass1.dll + weights)" -ForegroundColor Green
    } else {
        Write-Host "  Installed:      OptiScaler + danielblnc runtime (dlssnr_amd_pass1-3.dll + weights)" -ForegroundColor Green
    }
}
if ($keptA -or $keptW) {
    Write-Host "  Package keeps:  $keptA"
    if ($keptW) { Write-Host "                  $keptW" }
} else {
    Write-Host '  Package keeps:  (nothing — the package folder is the game folder)'
}
Write-Host ''
Write-Host 'Next (in game):' -ForegroundColor Yellow
Write-Host '  1. Launch the game'
Write-Host '  2. Press Insert (Ins) to open the OptiScaler menu'
Write-Host '  3. Ensure DLSSNR is enabled'
if ($installDaniel -and $installLmxxf) {
    Write-Host "  4. Switch backends anytime in OptiScaler.ini ([DlssNr] NrBackend=$activeBackend) or by re-running Setup.bat"
}
Write-Host ''
Write-Host 'Install SUCCEEDED.' -ForegroundColor Green
Pause-Exit 0
