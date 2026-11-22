# build-kit6.ps1 <kit 5 dir> <kit 6 DLL dir> <work> <kit dir>
# Lays kit 6 out in <kit dir>\OpenNR-RX7000-diag6. From kit 5 (laid out, OpenNR-RX7000-diag5\files): the released 0.5.8
# DLL (rel\), its dlssnr-amd tree, the tools (mz_e2e, compare, vkprobe and its probes), test.png, the RX 9070 XT picture
# references and kit 4's FP8 sequence references. From this tree: the kit 6 DLL (k6\) and its four RDNA3 attention
# SPIR-V (g_attn, g_attnnat, g_vitattn, g_vitattnnat; every other file of shaders-rdna3 and shaders is 0.5.8's), 0.5.8's
# g_attn/g_vitattn kept in spv-rel\ and kit 6's in spv-k6\. <work> holds kreplay.exe (build-tools.cmd), spv\ (build_k6.py),
# mprobe\ (make_probes.py), case\D11 (kit 5's) and case\D21.
param([string]$k5, [string]$k6, [string]$work, [string]$kitRoot)
$ErrorActionPreference = 'Stop'
$kit = Join-Path $kitRoot 'OpenNR-RX7000-diag6'
$f = Join-Path $kit 'files'
if (Test-Path -LiteralPath $kit) { Remove-Item -LiteralPath $kit -Recurse -Force }
New-Item -ItemType Directory -Force -Path "$f\rel", "$f\k6", "$f\spv-rel", "$f\spv-k6", "$f\reference" | Out-Null
Copy-Item "$k5\rel\MochizukiNrRuntime.dll" "$f\rel\"
Copy-Item "$k6\MochizukiNrRuntime.dll" "$f\k6\"
Copy-Item -Recurse "$k5\dlssnr-amd" "$f\"
Get-ChildItem "$f\dlssnr-amd" -Filter *.bin | Remove-Item
foreach ($n in 'g_attn.spv', 'g_vitattn.spv') {
    Copy-Item "$f\dlssnr-amd\shaders-rdna3\$n" "$f\spv-rel\"
    Copy-Item "$k6\dlssnr-amd\shaders-rdna3\$n" "$f\spv-k6\"
}
foreach ($n in 'g_attn.spv', 'g_attnnat.spv', 'g_vitattn.spv', 'g_vitattnnat.spv') { Copy-Item "$k6\dlssnr-amd\shaders-rdna3\$n" "$f\dlssnr-amd\shaders-rdna3\" -Force }
Copy-Item -Recurse "$k5\probe" "$f\"
Copy-Item "$k5\mz_e2e.exe", "$k5\compare.exe", "$k5\vkprobe.exe", "$k5\test.png", "$work\kreplay.exe" "$f\"
Copy-Item "$k5\reference\*.jxr" "$f\reference\"
Copy-Item -Recurse "$k5\reference\seqref" "$f\reference\"
Copy-Item -Recurse "$work\mprobe" "$f\"
foreach ($ks in @(@{ dir = 'attn'; kern = 'attn'; case = 'D11'; about = 'dispatch 11 attn at 1152x648 (C=512 window attention, CCSplitSwin16HQKVAttn, writes key 74)' },
                  @{ dir = 'vit'; kern = 'vitattn'; case = 'D21'; about = 'dispatch 21 vitattn at 1152x648 (ViT global attention, CCVit1DAttention, writes key 91)' })) {
    New-Item -ItemType Directory -Force -Path "$f\$($ks.dir)\spv", "$f\$($ks.dir)\cases" | Out-Null
    Copy-Item -Recurse "$work\case\$($ks.case)" "$f\$($ks.dir)\cases\"
    $kt = @('processes 2', 'rand_runs 20', 'scrub_runs 20', "case $($ks.case) cases/$($ks.case) 50 $($ks.about)")
    foreach ($l in Get-Content "$work\spv\$($ks.kern)\variants.txt") {
        $p = $l -split ' ', 3
        Copy-Item "$work\spv\$($ks.kern)\$($p[1])" "$f\$($ks.dir)\spv\"
        $kt += "variant $($p[0]) spv/$($p[1]) $($p[2])"
    }
    Set-Content -LiteralPath "$f\$($ks.dir)\kit.txt" -Value $kt
}
Copy-Item "$PSScriptRoot\kit6.ps1" "$f\"
Copy-Item "$PSScriptRoot\run-test.bat" "$kit\"
Copy-Item "$PSScriptRoot\README.txt" "$kit\README-diag6.txt"
'kit laid out in ' + $kit
