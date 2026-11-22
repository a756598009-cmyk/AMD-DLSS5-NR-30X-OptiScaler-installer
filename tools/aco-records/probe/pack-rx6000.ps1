# pack-rx6000.ps1 <laid out kit dir (OpenNR-RX6000-aco-probe)> <zip>: the zip and <zip>.sha256.txt.
param([string]$kit, [string]$zip)
$ErrorActionPreference = 'Stop'
$name = Split-Path $kit -Leaf
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
if (Test-Path -LiteralPath $zip) { [IO.File]::Delete($zip) }
$za = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($f in Get-ChildItem -LiteralPath $kit -Recurse -File) {
        $rel = "$name/" + $f.FullName.Substring($kit.Length + 1).Replace('\', '/')
        $null = [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($za, $f.FullName, $rel, [IO.Compression.CompressionLevel]::Optimal)
    }
} finally { $za.Dispose() }
$h = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
Set-Content -LiteralPath "$zip.sha256.txt" -Value "$h  $(Split-Path $zip -Leaf)"
'{0} {1} bytes, {2} files, SHA-256 {3}' -f $zip, (Get-Item -LiteralPath $zip).Length, (Get-ChildItem -LiteralPath $kit -Recurse -File).Count, $h
