$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\OptiScaler\dlssnr\amd\PresentExperimental.h')
$shader = [regex]::Match($source, 'const char\* shader = R"\((Texture2D<int2>[\s\S]*?)\)";').Groups[1].Value
if (-not $shader) { throw 'Optical Flow densify shader not found' }
# vcvars names the SDK it found, which is not always under Program Files (x86).
$kitRoot = if ($env:WindowsSdkDir) { $env:WindowsSdkDir } else { Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10' }
$fxc = Get-ChildItem -LiteralPath (Join-Path $kitRoot 'bin') -Filter fxc.exe -Recurse -File |
    Where-Object FullName -Match '\\x64\\fxc.exe$' | Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
if (-not $fxc) { throw 'fxc.exe not found' }
$hlsl = Join-Path $env:TEMP 'opti-flow-densify.hlsl'
$bytecode = Join-Path $env:TEMP 'opti-flow-densify.cso'
Set-Content -LiteralPath $hlsl -Value $shader
& $fxc /nologo /T cs_5_0 /E main /Fo $bytecode $hlsl
if ($LASTEXITCODE -ne 0) { throw 'Optical Flow densify shader failed to compile' }
