# build-kit5.ps1 <kit 2 files dir> <kit 5 DLL dir> <seqref dir> <work> <kit dir>
# Lays kit 5 out in <kit dir>\OpenNR-RX7000-diag5: from kit 2 (unpacked) the released 0.5.8 DLL (rel\), the released
# shaders plus g_attnnat.spv (the only new file in shaders-rdna3), the tools (mz_e2e, compare, vkprobe and its probes),
# test.png and the RX 9070 XT picture references of the four sizes; the kit 5 DLL (k5\); kit 4's FP8 references of
# the sequence check; and the attention part (attn\): kreplay.exe, the dispatch 11 case and the SPIR-V of
# build_attn5.py. <work> holds case\D11, spv\ (build_attn5.py) and tools5\kreplay.exe.
param([string]$kit2, [string]$k5, [string]$seqref, [string]$work, [string]$kitRoot)
$ErrorActionPreference = 'Stop'
$kit = Join-Path $kitRoot 'OpenNR-RX7000-diag5'
$f = Join-Path $kit 'files'
if (Test-Path -LiteralPath $kit) { Remove-Item -LiteralPath $kit -Recurse -Force }
New-Item -ItemType Directory -Force -Path "$f\rel", "$f\k5", "$f\reference\seqref", "$f\attn\spv", "$f\attn\cases" | Out-Null
Copy-Item "$kit2\rel\MochizukiNrRuntime.dll" "$f\rel\"
Copy-Item "$k5\MochizukiNrRuntime.dll" "$f\k5\"
Copy-Item -Recurse "$kit2\dlssnr-amd" "$f\"
Get-ChildItem "$f\dlssnr-amd" -Filter *.bin | Remove-Item
Copy-Item "$k5\dlssnr-amd\shaders-rdna3\g_attnnat.spv" "$f\dlssnr-amd\shaders-rdna3\"
Copy-Item -Recurse "$kit2\probe" "$f\"
Copy-Item "$kit2\mz_e2e.exe", "$kit2\compare.exe", "$kit2\vkprobe.exe", "$kit2\test.png" "$f\"
foreach ($n in 'small_1152x648', 'small_1280x720', 'small_1920x1080', 'small_2560x1440') { Copy-Item "$kit2\reference\img_${n}_*.jxr" "$f\reference\" }
foreach ($s in '1152x648', '1280x720', '1920x1080') {
    New-Item -ItemType Directory -Force -Path "$f\reference\seqref\$s" | Out-Null
    Copy-Item "$seqref\$s\ref_k*.bin" "$f\reference\seqref\$s\"
}
Copy-Item "$work\tools5\kreplay.exe" "$f\"
Copy-Item -Recurse "$work\case\D11" "$f\attn\cases\"
$kt = @('processes 2', 'rand_runs 20', 'scrub_runs 20',
        'case D11 cases/D11 50 dispatch 11 attn at 1152x648 (C=512 window attention, CCSplitSwin16HQKVAttn, writes key 74)')
foreach ($l in Get-Content "$work\spv\variants.txt") {
    $p = $l -split ' ', 3
    Copy-Item "$work\spv\$($p[1])" "$f\attn\spv\"
    $kt += "variant $($p[0]) spv/$($p[1]) $($p[2])"
}
Set-Content -LiteralPath "$f\attn\kit.txt" -Value $kt
Copy-Item "$PSScriptRoot\kit5.ps1" "$f\"
Copy-Item "$PSScriptRoot\run-test.bat", "$PSScriptRoot\README.txt" "$kit\"
'kit laid out in ' + $kit
