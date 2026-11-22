# The sizes and models of the RX 7000 diagnostic kit (kit.ps1 and make-refs.ps1 read this).
# The /16 level of the small model's plan: h % 8 == 4 passed on the RX 7900 XT, h % 8 == 0 failed; the list covers
# both, and both for w (see the handoff).
$Sizes = @(
    @(960, 540), @(1024, 576), @(1152, 648), @(1280, 720), @(1280, 800), @(1344, 756), @(1536, 864),
    @(1600, 900), @(1920, 1080), @(1920, 1200), @(2048, 1152), @(2560, 1440))
$Models = @(@{ name = 'small'; flags = '0' }, @{ name = 'large'; flags = '8192' })
# The /16 level (C=256, the fswinp256 run) of each plan, h x w in tokens (nr_native_plan, measured with a plan probe).
$Level16 = @{
    "small_960x540" = @(36, 60)
    "large_960x540" = @(36, 60)
    "small_1024x576" = @(36, 64)
    "large_1024x576" = @(36, 64)
    "small_1152x648" = @(48, 72)
    "large_1152x648" = @(44, 72)
    "small_1280x720" = @(48, 84)
    "large_1280x720" = @(48, 84)
    "small_1280x800" = @(52, 80)
    "large_1280x800" = @(52, 80)
    "small_1344x756" = @(48, 84)
    "large_1344x756" = @(48, 84)
    "small_1536x864" = @(56, 96)
    "large_1536x864" = @(56, 96)
    "small_1600x900" = @(64, 100)
    "large_1600x900" = @(60, 100)
    "small_1920x1080" = @(72, 120)
    "large_1920x1080" = @(68, 120)
    "small_1920x1200" = @(76, 120)
    "large_1920x1200" = @(76, 120)
    "small_2048x1152" = @(72, 128)
    "large_2048x1152" = @(72, 128)
    "small_2560x1440" = @(92, 160)
    "large_2560x1440" = @(92, 160)
}
