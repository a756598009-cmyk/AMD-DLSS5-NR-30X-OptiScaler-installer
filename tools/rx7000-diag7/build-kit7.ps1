# build-kit7.ps1 <kit 6 files dir> <rdna3-aco build dir> <acowork> <kit root>
# Lays kit 7 out in <kit root>\OpenNR-RX7000-diag7. From kit 6 (laid out, ...\OpenNR-RX7000-diag6\files): the tools
# (mz_e2e, compare, vkprobe), test.png, the RX 9070 XT references, the dispatch 11 / 21 cases and their driver SPIR-V
# (rel = 0.5.8, attn fix = the 0.5.9 kernel, vit nat = the 0.5.9 kernel). From this branch: the DLL (k7\), the whole
# dlssnr-amd tree as built (shaders-rdna3 = the 0.5.9 fix), kreplay with ACO (tools7\), the ACO probe (probe\), the
# Linux SPIR-V and gfx11 records of both bundles (spv-fix / rec-fix, spv-coop / rec-coop).
param([string]$k6, [string]$build, [string]$work, [string]$kitRoot)
$ErrorActionPreference = 'Stop'
$kit = Join-Path $kitRoot 'OpenNR-RX7000-diag7'
$f = Join-Path $kit 'files'
if (Test-Path -LiteralPath $kit) { Remove-Item -LiteralPath $kit -Recurse -Force }
New-Item -ItemType Directory -Force -Path "$f\k7", "$f\reference", "$f\bundles", "$f\alone-records", "$f\acoprobe\records" | Out-Null
Copy-Item "$build\MochizukiNrRuntime.dll" "$f\k7\"
New-Item -ItemType Directory -Force -Path "$f\dlssnr-amd" | Out-Null
foreach ($s in 'shaders', 'shaders-rdna3') { Copy-Item -Recurse "$build\dlssnr-amd\$s" "$f\dlssnr-amd\$s" }
# ACO is asked for when dlssnr-amd\aco has records\ and shaders\ (MochizukiNrRuntime.cpp ConfigureAco); the RDNA3 network
# then takes dlssnr-amd\aco-gfx11 (the bundle the script puts there), never these.
foreach ($s in 'records', 'shaders') {
    New-Item -ItemType Directory -Force -Path "$f\dlssnr-amd\aco\$s" | Out-Null
    Set-Content -LiteralPath "$f\dlssnr-amd\aco\$s\README.txt" -Value 'Empty on purpose: the RX 7000 network uses dlssnr-amd\aco-gfx11.'
}
foreach ($b in 'fix', 'coop') {
    Copy-Item -Recurse "$work\spv-$b" "$f\bundles\$b\shaders"
    Copy-Item -Recurse "$work\rec-$b\records" "$f\bundles\$b\records"
    Copy-Item "$work\rec-$b\records\*.nrp" "$f\alone-records\" -Force
}
Copy-Item "$k6\mz_e2e.exe", "$k6\compare.exe", "$k6\vkprobe.exe", "$k6\test.png", "$work\tools7\kreplay.exe", "$work\probe\nr_aco_probe.exe" "$f\"
Copy-Item -Recurse "$k6\probe" "$f\"
Copy-Item "$k6\reference\*.jxr" "$f\reference\"
Copy-Item "$work\probe\probe_a.spv", "$work\probe\probe_b.spv" "$f\acoprobe\"
foreach ($m in 'navi31', 'gfx1201') { Copy-Item -Recurse "$work\probe\records\$m" "$f\acoprobe\records\$m" }
# The kernels alone: driver (kit 6's SPIR-V) and ACO (the Linux SPIR-V the records are made from).
$alone = @(
    @{ dir = 'attn'; src = 'attn'; case = 'D11'; about = 'dispatch 11 attn at 1152x648 (C=512 window attention, writes key 74)';
       vars = @(@('rel', "$k6\attn\spv\rel.spv", 'driver: released 0.5.8 (cooperative matrices, Q/K rows 72 bytes apart)'),
                @('fix', "$k6\attn\spv\fix.spv", 'driver: the 0.5.9 fix (Q/K rows 64 bytes apart)')) },
    @{ dir = 'vit'; src = 'vit'; case = 'D21'; about = 'dispatch 21 vitattn at 1152x648 (ViT global attention, writes key 91)';
       vars = @(@('rel', "$k6\vit\spv\rel.spv", 'driver: released 0.5.8 (cooperative matrices)'),
                @('fix', "$k6\vit\spv\nat.spv", 'driver: the 0.5.9 fix (native body, no cooperative matrices)')) },
    @{ dir = 'attn-aco'; src = 'attn'; case = 'D11'; about = 'dispatch 11 attn at 1152x648, ACO machine code';
       vars = @(@('acocoop', "$work\spv-coop\g_attn.spv", 'ACO: 0.5.8''s kernel (cooperative matrices, 72-byte rows)'),
                @('acofix', "$work\spv-fix\g_attn.spv", 'ACO: the 0.5.9 fix''s kernel')) },
    @{ dir = 'vit-aco'; src = 'vit'; case = 'D21'; about = 'dispatch 21 vitattn at 1152x648, ACO machine code';
       vars = @(@('acocoop', "$work\spv-coop\g_vitattn.spv", 'ACO: 0.5.8''s kernel (cooperative matrices)'),
                @('acofix', "$work\spv-fix\g_vitattn.spv", 'ACO: the 0.5.9 fix''s kernel (native body)')) })
foreach ($a in $alone) {
    New-Item -ItemType Directory -Force -Path "$f\$($a.dir)\spv", "$f\$($a.dir)\cases" | Out-Null
    Copy-Item -Recurse "$k6\$($a.src)\cases\$($a.case)" "$f\$($a.dir)\cases\"
    $kt = @('processes 2', 'rand_runs 20', 'scrub_runs 20', "case $($a.case) cases/$($a.case) 50 $($a.about)")
    foreach ($v in $a.vars) { Copy-Item $v[1] "$f\$($a.dir)\spv\$($v[0]).spv"; $kt += "variant $($v[0]) spv/$($v[0]).spv $($v[2])" }
    Set-Content -LiteralPath "$f\$($a.dir)\kit.txt" -Value $kt
}
Copy-Item "$PSScriptRoot\kit7.ps1" "$f\"
Copy-Item "$PSScriptRoot\run-test.bat" "$kit\"
Copy-Item "$PSScriptRoot\README.txt" "$kit\README-diag7.txt"
'kit laid out in ' + $kit
