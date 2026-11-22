#pragma once

// 007 First Light (Glacier engine) hides the Path Tracing and DLSS Ray Reconstruction
// menu options behind internally computed device capability flags which stay false on
// non-Nvidia GPUs no matter what the API level spoofing reports.
// StartWatcher spawns a background thread that flips those flags once the engine's
// graphics feature object shows up and keeps re-asserting them in case the game
// recomputes them (device reset, settings changes).

namespace FirstLightPTUnlock
{
void StartWatcher();
}
