# pack-kit.ps1 <kit dir> <zip>: zips <kit dir>\OpenNR-RX6000-test as it ships (the cabinet and the scripts; nothing a run
# unpacked, downloaded or wrote).
param([string]$kitRoot, [string]$zip)
$ErrorActionPreference = 'Stop'
$kit = Join-Path (Resolve-Path $kitRoot).Path 'OpenNR-RX6000-test'
$ship = @('run-test.bat', 'README.txt', 'files\kit.ps1', 'files\cases.ps1', 'files\kit.cab', 'files\kit.sha256', 'files\same.txt')
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
New-Item -ItemType Directory -Force -Path (Split-Path $zip) | Out-Null
Remove-Item -LiteralPath $zip -ErrorAction SilentlyContinue
$za = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($rel in $ship) {
        $null = [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($za, (Join-Path $kit $rel), 'OpenNR-RX6000-test/' + $rel.Replace('\', '/'),
                                                                        [IO.Compression.CompressionLevel]::Optimal)
    }
} finally { $za.Dispose() }
'{0} {1:N2} MiB, {2} files, SHA-256 {3}' -f $zip, ((Get-Item -LiteralPath $zip).Length / 1MB), $ship.Count, (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
