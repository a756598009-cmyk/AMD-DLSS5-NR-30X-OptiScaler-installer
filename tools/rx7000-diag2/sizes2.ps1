# What kit 2 runs (kit2.ps1 and make-refs2.ps1 read this): the worst sizes of kit 1, small model, and the large
# model at two of them.
$Runs = @(
    @{ name = 'small_1152x648'; model = 'small'; w = 1152; h = 648 },
    @{ name = 'small_1280x720'; model = 'small'; w = 1280; h = 720 },
    @{ name = 'small_1344x756'; model = 'small'; w = 1344; h = 756 },
    @{ name = 'small_960x540'; model = 'small'; w = 960; h = 540 },
    @{ name = 'small_1920x1080'; model = 'small'; w = 1920; h = 1080 },
    @{ name = 'small_2560x1440'; model = 'small'; w = 2560; h = 1440 },
    @{ name = 'large_1152x648'; model = 'large'; w = 1152; h = 648 },
    @{ name = 'large_1920x1080'; model = 'large'; w = 1920; h = 1080 })
# Clean runs (noise seed 0, no history): every frame the same work, the dump early.
$CleanFrames = 12
$CleanDumpAt = 3
