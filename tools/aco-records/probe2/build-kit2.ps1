# build-kit2.ps1 <build dir (build-probe2.cmd's out)> <RX 6000 kit zip with shaders-rdna2> <kit dir> <zip>
# Lays out the kit, captures this RDNA4 card's containers of the probes as files\ref-gfx12, packs it.
param([string]$build, [string]$netzip, [string]$kit, [string]$zip)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
if (Test-Path -LiteralPath $kit) { Remove-Item -LiteralPath $kit -Recurse -Force }
$f = Join-Path $kit 'files'
New-Item -ItemType Directory -Force (Join-Path $f 'net'), (Join-Path $f 'ref-gfx12') | Out-Null
Copy-Item "$build\nr_aco_probe2.exe", "$build\base\probe_a.spv", "$build\base\probe_b.spv" $f
Copy-Item -Recurse "$build\base\records" $f
Add-Type -AssemblyName System.IO.Compression.FileSystem
$z = [IO.Compression.ZipFile]::OpenRead($netzip)
try {
    foreach ($n in 'g_gemmproj.spv', 'g_fswin32.spv') {
        $e = $z.Entries | Where-Object { $_.FullName -like "*/shaders-rdna2/$n" } | Select-Object -First 1
        [IO.Compression.ZipFileExtensions]::ExtractToFile($e, (Join-Path $f "net\$n"), $true)
    }
} finally { $z.Dispose() }
# The reference containers: this card's driver (RDNA4, LLPC) compiling the probes.
$ref = Join-Path $build 'ref-run'
if (Test-Path -LiteralPath $ref) { Remove-Item -LiteralPath $ref -Recurse -Force }
& (Join-Path $f 'nr_aco_probe2.exe') capture $f $ref | Out-Null
if ($LASTEXITCODE) { throw "reference capture failed ($LASTEXITCODE)" }
foreach ($p in 'probe_a', 'probe_b') {
    $b = Join-Path $ref "blobs\$p.0.bin"
    if ([IO.File]::ReadAllBytes($b)[48] -ne 0x4e) { throw "$p reference is not gfx1201" }
    Copy-Item $b (Join-Path $f "ref-gfx12\$p.bin")
}
Copy-Item "$here\rx6000\README.txt", "$here\rx6000\run-test.bat" $kit
& "$here\..\probe\pack-rx6000.ps1" $kit $zip
