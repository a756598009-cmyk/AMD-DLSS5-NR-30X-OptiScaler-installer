# What the RX 6000 kit runs (kit.ps1 and make-refs.ps1 read this).
# Quality: clean runs (NR_DIAG_SEED0=1, NR_DIAG_TGATE0=1: noise seed 0, no history), so every frame is the same work
# and the output does not depend on the frame times; each against the RX 9070 XT FP8 network's clean output. Both
# models at four sizes, with the driver's compiler and with ACO's machine code (aco-gfx103x).
$Quality = @(
    @{ name = 'small_1152x648'; model = 'small'; w = 1152; h = 648 },
    @{ name = 'small_1280x720'; model = 'small'; w = 1280; h = 720 },
    @{ name = 'small_1920x1080'; model = 'small'; w = 1920; h = 1080 },
    @{ name = 'small_2560x1440'; model = 'small'; w = 2560; h = 1440 },
    @{ name = 'large_1152x648'; model = 'large'; w = 1152; h = 648 },
    @{ name = 'large_1280x720'; model = 'large'; w = 1280; h = 720 },
    @{ name = 'large_1920x1080'; model = 'large'; w = 1920; h = 1080 },
    @{ name = 'large_2560x1440'; model = 'large'; w = 2560; h = 1440 })
$QualityFrames = 8
$QualityRuns = 3
$PassDb = 40.0
$Clean = @{ NR_DIAG_SEED0 = '1'; NR_DIAG_TGATE0 = '1' }
# Speed: normal runs (as in a game), the network time from the status line, every quality case, $SpeedRounds rounds
# with the driver and ACO alternating; then NR_STEP_TIMES per pipeline for $Steps with both.
$SpeedFrames = 30
$SpeedRounds = 3
$Steps = @('small_1280x720', 'large_1280x720')
$StepFrames = 30
$StepCount = 20
# The first start: the network built on a driver that has compiled none of its shaders. The kit's shader sets carry
# SPIR-V generator words of their own (tools\rx6000-kit\genword.py at build time, kit.ps1 at run time) so that no
# driver cache holds them: the shipped set 0x7761, the old first start (no prewarm list, the check's cache off) 0x7762,
# ACO's templates 0x7763.
$WordBefore = 0x7762
$WordAco = 0x7763
# KIT_QUICK=1 (our own runs only): two cases, two quality runs, one speed round.
if ($env:KIT_QUICK -eq '1') { $Quality = @($Quality[1], $Quality[5]); $QualityRuns = 2; $SpeedRounds = 1 }
