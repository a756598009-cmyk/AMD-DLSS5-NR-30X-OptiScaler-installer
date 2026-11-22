#include "../OptiScaler/dlssnr/amd/DynamicScale.h"
#include <cassert>
#include <iostream>

using AmdPreSr::DynamicScale;
using Clock = DynamicScale::Clock;

// Feeds `frames` frames of `ms` each and returns the scale of the last one.
static float Run(DynamicScale& d, Clock::time_point& t, int frames, int ms, float base = 1.f, int target = 60)
{
    float scale = 0;
    for (int i = 0; i < frames; ++i)
    {
        t += std::chrono::milliseconds(ms);
        scale = d.Step(t, true, target, base);
    }
    return scale;
}

int main()
{
    Clock::time_point t {};
    t += std::chrono::hours(1);

    // Off: the configured scale, whatever the frame rate.
    {
        DynamicScale d;
        for (int i = 0; i < 500; ++i)
        {
            t += std::chrono::milliseconds(40);
            assert(d.Step(t, false, 60, .8f) == .8f);
        }
        assert(d.level == 0);
    }

    // 50 fps against a 60 target: holds for under 2 s, then one step down.
    {
        DynamicScale d;
        assert(Run(d, t, 90, 20) == 1.f);
        assert(Run(d, t, 20, 20) == .85f);
        // Nothing moves during the 10 s hold, even while still slow.
        assert(Run(d, t, 450, 20) == .85f);
        // Still slow after the hold: the next level after another 2 s.
        assert(Run(d, t, 150, 20) == .7f);

        // 100 fps is 15% over the target, but no step up for 60 s after a step down.
        assert(Run(d, t, 5000, 10) == .7f); // 50 s
        assert(Run(d, t, 1100, 10) == .85f); // past 60 s and 5 s of headroom
    }

    // Inside the 15% band neither direction moves.
    {
        DynamicScale d;
        d.level = 2;
        assert(Run(d, t, 3000, 15) == .7f); // 66 fps: over 60, under 69
    }

    // A gap (pause, load) restarts the count instead of adding to it.
    {
        DynamicScale d;
        Run(d, t, 90, 20);
        t += std::chrono::seconds(1);
        assert(Run(d, t, 90, 20) == 1.f);
    }

    // The last level is the floor, and the scale never goes under 25%.
    {
        DynamicScale d;
        d.level = std::size(DynamicScale::kLevels) - 1;
        assert(Run(d, t, 500, 40, .5f) == .25f);
        assert(d.level == std::size(DynamicScale::kLevels) - 1);
    }

    // From 50%, the last level clamps to 25%, under 10% below 27.5%: it stops at 27.5% instead.
    {
        DynamicScale d;
        float scale = 0;
        for (int i = 0; i < 10; ++i)
            scale = Run(d, t, 700, 20, .5f); // 14 s each: 2 s under the target plus the 10 s hold
        assert(scale == DynamicScale::Scale(.5f, 3) && d.level == 3 && d.changes == 3);
    }

    // After kMaxChanges the level stays put, however slow, and still reports a frame rate.
    {
        DynamicScale d;
        d.changes = DynamicScale::kMaxChanges;
        assert(Run(d, t, 500, 20) == 1.f);
        assert(d.level == 0 && d.Fps() > 0);
        // Off and on again resets the count.
        t += std::chrono::milliseconds(20);
        d.Step(t, false, 60, 1.f);
        assert(d.changes == 0);
        assert(Run(d, t, 110, 20) == .85f && d.changes == 1);
    }

    std::cout << "amd_dynamic_scale: ok\n";
    return 0;
}
