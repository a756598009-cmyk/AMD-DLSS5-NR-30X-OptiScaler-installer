# pack-kit3.ps1 <kit dir> <zip>: zips <kit dir>\OpenNR-RX7000-diag3 as it ships (no results).
param([string]$kitRoot, [string]$zip)
$ErrorActionPreference = 'Stop'
$kit = Join-Path $kitRoot 'OpenNR-RX7000-diag3'
foreach ($d in 'result', 'OpenNR-RX7000-diag3-result-*.zip') {
    Get-Item -Path (Join-Path $kit $d) -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force
}
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
New-Item -ItemType Directory -Force -Path (Split-Path $zip) | Out-Null
Remove-Item -LiteralPath $zip -ErrorAction SilentlyContinue
$za = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($f in Get-ChildItem -LiteralPath $kit -Recurse -File) {
        $rel = 'OpenNR-RX7000-diag3/' + $f.FullName.Substring($kit.Length + 1).Replace('\', '/')
        $null = [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($za, $f.FullName, $rel, [IO.Compression.CompressionLevel]::Optimal)
    }
} finally { $za.Dispose() }
'{0} {1:N1} MB ({2} bytes), {3} files, SHA-256 {4}' -f $zip, ((Get-Item -LiteralPath $zip).Length / 1MB),
    (Get-Item -LiteralPath $zip).Length, (Get-ChildItem -LiteralPath $kit -Recurse -File).Count, (Get-FileHash -LiteralPath $zip).Hash
