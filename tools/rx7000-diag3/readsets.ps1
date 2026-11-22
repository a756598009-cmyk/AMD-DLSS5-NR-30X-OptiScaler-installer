# readsets.ps1 <work> <dispatch,dispatch,...>: kreplay readset on each capture <work>\cap\D<n> with the shipped
# pipeline it names, into <work>\cap\D<n>\sets.txt (and sets.answer.bin).
param([string]$work, [string]$disp)
$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $work
foreach ($d in $disp -split ',') {
    $spv = ((Get-Content "cap\D$d\capture.txt" | Select-String '^spirv ').Line -split ' ')[1]
    $t = Measure-Command { & .\kreplay.exe readset "cap\D$d" "dlssnr-amd\shaders-rdna3\$spv" "cap\D$d\sets.txt" | Out-Host }
    Write-Host ("D{0} {1}: {2:N0} s, exit {3}" -f $d, $spv, $t.TotalSeconds, $LASTEXITCODE)
}
