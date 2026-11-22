#pragma once

#include "SysUtils.h"

#include "../../plugin_sdk/optiscaler_plugin.h"

#include <imgui/imgui.h>

// OptiScaler side of plugin_sdk/optiscaler_plugin.h
class PluginApi
{
  public:
    struct TabInfo
    {
        std::string name;
        std::string description;
    };

    // Calls OptiPluginInit of a loaded ASI plugin, once per module
    static void InitPlugin(HMODULE module, PFN_OptiPluginInit init, const std::filesystem::path& path);

    static std::vector<TabInfo> GetTabs();

    // Draws a registered tab inside of the menu, colorMap adjusts colors for the HDR menu
    static void DrawTab(size_t index, float menuScale, ImVec4 (*colorMap)(const ImVec4&));

    // Draws the cards plugins added to the Custom tab, returns false when there are none
    static bool DrawCustomCards(float menuScale, ImVec4 (*colorMap)(const ImVec4&));

    // The upscaler a plugin reported in the last half second (ReportUpscaler), false when there is none
    static bool ReportedUpscaler(std::string* name, float* gpuTimeMs);

    // Called by StreamlineHooks: installs the plugins' Streamline routes on the interposer it just hooked (2.x),
    // and removes them before it unhooks the interposer
    static void OnInterposerHooked(HMODULE module, uint32_t major, uint32_t minor, uint32_t patch);
    static void OnInterposerUnhooking();

    // The ray reconstruction provider a plugin registered for NGX-direct games (version 4), nullptr when none
    static const OptiNgxRayReconstruction* NgxRayReconstruction();

    // Brackets work a plugin records into the game's command list (version 5); also used around the NGX ray
    // reconstruction provider's evaluate. plugin can be nullptr.
    static void BeginCommandListWork(OptiPlugin* plugin, void* commandList);
    static void EndCommandListWork(OptiPlugin* plugin, void* commandList);
};
