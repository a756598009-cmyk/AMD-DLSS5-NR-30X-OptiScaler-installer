# pack-kit.ps1: leaves only what the kit ships in exports\rx7000-kit\OpenNR-RX7000-test and zips it beside it.
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\exports\rx7000-kit'))
$kit = Join-Path $root 'OpenNR-RX7000-test'
$files = Join-Path $kit 'files'
$drop = @('result', 'OpenNR-RX7000-result-*.zip', 'files\work', 'files\mochizuki_nr.log',
          'files\dlssnr-amd\*.bin', 'files\dlssnr-amd\*.part', 'files\dlssnr-amd\*.cache', 'files\dlssnr-amd\prewarm',
          'files\dlssnr-amd\build.*', 'files\dlssnr-amd\float-colour-shared', 'files\dlssnr-amd\aco-cache')
foreach ($d in $drop) { Get-Item -Path (Join-Path $kit $d) -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force }
$zip = Join-Path $root 'OpenNR-RX7000-test.zip'
Remove-Item -LiteralPath $zip -ErrorAction SilentlyContinue
Compress-Archive -Path $kit -DestinationPath $zip -CompressionLevel Optimal
Get-ChildItem -LiteralPath $kit -Recurse -File | ForEach-Object { $_.FullName.Substring($kit.Length + 1) } |
    Set-Content -LiteralPath (Join-Path $root 'kit-files.txt')
'{0} {1:N1} MB' -f $zip, ((Get-Item -LiteralPath $zip).Length / 1MB)
