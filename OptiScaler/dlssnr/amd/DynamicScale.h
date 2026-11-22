#pragma once
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iterator>

namespace AmdPreSr
{
// Dynamic NR resolution: picks the model scale for each rendered frame from the frame rate.
//
// A scale change rebuilds the runtime's staging, restarts the model's history and leaves frames
// without NR for one to five seconds (Cyberpunk: median 1.25 s over 31 changes in one session,
// 2.8 s over 15 in another), so this steps
// between a few levels and changes rarely: down after 2 s under the target, up after 5 s with 15%
// headroom, nothing for 10 s after any change, and no step up for 60 s after a step down, so a level
// that cannot hold the target is not retried every few seconds.
//
// The runtime also keeps a little more VRAM after every rebuild, in every host it has been used in,
// so the number of changes is capped per session and the leak stays bounded. Turning the option
// off and on again resets the count.
//
// Step runs once per rendered frame. The interval between calls is the rendered frame time whether
// or not frame generation runs, which the present-side frame times are not.
struct DynamicScale
{
    using Clock = std::chrono::steady_clock;
    static constexpr float kLevels[] { 1.f, .85f, .7f, .55f, .4f };
    static constexpr int kMaxChanges = 8;

    float Step(Clock::time_point now, bool enabled, int targetFps, float base)
    {
        using namespace std::chrono_literals;
        const double dtMs =
            last == Clock::time_point {} ? 0 : std::chrono::duration<double, std::milli>(now - last).count();
        last = now;

        if (!enabled)
        {
            level = 0;
            changes = 0;
            emaMs = slowMs = fastMs = 0;
            return base;
        }

        // A pause, a load or a menu says nothing about the game's frame rate. Frames inside the hold
        // run without NR while the model rebuilds, so they are faster than the game really is.
        if (dtMs <= 0 || dtMs > 250)
        {
            emaMs = slowMs = fastMs = 0;
        }
        else if (now >= hold)
        {
            emaMs = emaMs > 0 ? emaMs * .9 + dtMs * .1 : dtMs;
            const double targetMs = 1000.0 / std::max(1, targetFps);
            slowMs = emaMs > targetMs ? slowMs + dtMs : 0;
            fastMs = emaMs * 1.15 < targetMs ? fastMs + dtMs : 0;

            if (changes >= kMaxChanges)
            {
                // Keeps measuring for the menu, but the level stays where it is.
            }
            // The 25% floor can flatten the last levels into nearly the current scale. A rebuild for
            // that little is not worth its cost, so a step must cut the scale by at least 10%.
            else if (slowMs >= 2000 && level + 1 < std::size(kLevels) &&
                     Scale(base, level + 1) <= Scale(base, level) * .9f)
            {
                ++level;
                ++changes;
                hold = now + 10s;
                upAllowed = now + 60s;
                emaMs = slowMs = fastMs = 0;
            }
            else if (fastMs >= 5000 && level > 0 && now >= upAllowed)
            {
                --level;
                ++changes;
                hold = now + 10s;
                emaMs = slowMs = fastMs = 0;
            }
        }

        return Scale(base, level);
    }

    static float Scale(float base, std::size_t level) { return std::max(.25f, base * kLevels[level]); }

    // The smoothed rendered frame rate, or 0 while it is being measured again after a change.
    double Fps() const { return emaMs > 0 ? 1000.0 / emaMs : 0; }

    std::size_t level = 0;
    int changes = 0;
    Clock::time_point last {}, hold {}, upAllowed {};
    double emaMs = 0, slowMs = 0, fastMs = 0;
};
} // namespace AmdPreSr
