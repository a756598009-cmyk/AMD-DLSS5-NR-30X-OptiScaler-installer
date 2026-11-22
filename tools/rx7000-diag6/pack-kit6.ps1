# pack-kit6.ps1 <kit dir> <zip>: zips <kit dir>\OpenNR-RX7000-diag6 as it ships (no models, caches, logs or results).
param([string]$kitRoot, [string]$zip)
$ErrorActionPreference = 'Stop'
$kit = Join-Path $kitRoot 'OpenNR-RX7000-diag6'
foreach ($d in 'result', 'OpenNR-RX7000-diag6-result-*.zip', 'files\work', 'files\mochizuki_nr.log',
               'files\dlssnr-amd\*.bin', 'files\dlssnr-amd\*.part', 'files\dlssnr-amd\*.cache', 'files\dlssnr-amd\prewarm',
               'files\dlssnr-amd\build.*', 'files\dlssnr-amd\rdna3-check.txt', 'files\dlssnr-amd\float-colour-shared') {
    Get-Item -Path (Join-Path $kit $d) -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force
}
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
New-Item -ItemType Directory -Force -Path (Split-Path $zip) | Out-Null
Remove-Item -LiteralPath $zip -ErrorAction SilentlyContinue
$za = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($f in Get-ChildItem -LiteralPath $kit -Recurse -File) {
        $rel = 'OpenNR-RX7000-diag6/' + $f.FullName.Substring($kit.Length + 1).Replace('\', '/')
        $null = [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($za, $f.FullName, $rel, [IO.Compression.CompressionLevel]::Optimal)
    }
} finally { $za.Dispose() }
'{0} {1:N1} MB, {2} files, SHA-256 {3}' -f $zip, ((Get-Item -LiteralPath $zip).Length / 1MB), (Get-ChildItem -LiteralPath $kit -Recurse -File).Count, (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
