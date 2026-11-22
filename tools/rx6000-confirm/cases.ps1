# What the RX 6000 confirmation kit runs (kit.ps1 reads this): the RX 6000 network as the next OpenNR ships it, FP16
# (shaders-rdna2) and INT8 (MZ_RDNA2_INT8=1, shaders-rdna2-int8: the C = 32 attention Q.K in int8), and 0.6.9's INT8 set
# (int8old) for the speed comparison.
# Quality: clean runs (NR_DIAG_SEED0=1, NR_DIAG_TGATE0=1: noise seed 0, no history), one run a case and set, scored
# against the RX 9070 XT's FP8 network (reference\img_<case>), the INT8 sets also against this card's FP16
# output (the RX 9070 XT's INT8 pictures do not fit in 10 MiB).
$Quality = @(
    @{ name = 'small_1280x720'; model = 'small'; w = 1280; h = 720 },
    @{ name = 'small_1920x1080'; model = 'small'; w = 1920; h = 1080 },
    @{ name = 'large_1280x720'; model = 'large'; w = 1280; h = 720 })
$QualityFrames = 8
$QualityRuns = 1
$PassDb = 40.0          # FP16 against FP8 (kits v1-v6)
$PassInt8Db = 45.0      # INT8 against FP8
$PassSameDb = 44.0      # INT8 against FP16, same card, whole frame
$Clean = @{ NR_DIAG_SEED0 = '1'; NR_DIAG_TGATE0 = '1' }
$Int8 = @{ MZ_RDNA2_INT8 = '1' }
# Speed: normal runs (as in a game), the network time from the status line, every case, $SpeedRounds rounds with the
# sets alternating (each first once), the GPU clock sampled in each run; then NR_STEP_TIMES per kernel, every case,
# $StepRounds rounds alternating.
$SpeedFrames = 30
$SpeedRounds = 2
$Steps = @('small_1280x720')
$StepRounds = 1
$StepFrames = 30
$StepCount = 20
# KIT_QUICK=1 (our own runs only): small 720p only, one round each.
if ($env:KIT_QUICK -eq '1') { $Quality = @($Quality[0]); $Steps = @($Steps[0]); $SpeedRounds = 1; $StepRounds = 1 }
