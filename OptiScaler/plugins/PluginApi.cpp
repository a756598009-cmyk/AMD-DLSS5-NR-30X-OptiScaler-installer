#include "pch.h"

#include "PluginApi.h"

#include "Config.h"
#include "menu/menu_common.h"

#include <imgui/imgui_internal.h>

#include <charconv>

#include <proxies/KernelBase_Proxy.h>
#include "detours/detours.h"
#include <hooks/D3D12_Hooks.h>

struct OptiPlugin
{
    std::string name;
    std::string section; // Plugin.<name> in OptiScaler.ini
};

namespace
{
struct PluginTab
{
    OptiPlugin* plugin;
    std::string name;
    std::string description;
    OptiDrawTabFn draw;
    void* userData;
    bool faulted = false;
};

// Plugins live as long as the process, the API keeps pointers to them
std::vector<std::unique_ptr<OptiPlugin>> _plugins;
std::vector<HMODULE> _initedModules;
std::vector<std::shared_ptr<PluginTab>> _tabs;
std::vector<std::shared_ptr<PluginTab>> _cards; // Custom tab cards, name is the card title
std::mutex _mutex;

// The upscaler a plugin runs itself (ReportUpscaler)
struct ReportedUpscalerState
{
    std::string name;
    float gpuTimeMs = 0.0f;
    ULONGLONG at = 0; // GetTickCount64 of the last report
};
ReportedUpscalerState _reported;
std::mutex _reportedMutex;

// Streamline functions plugins handle themselves (RegisterStreamlineRoutes)
struct StreamlineRoute
{
    std::string exportName;
    void* handler = nullptr;
    void** next = nullptr;
    bool installed = false;
};

struct PluginStreamlineRoutes
{
    OptiPlugin* plugin = nullptr;
    std::vector<StreamlineRoute> routes;
    OptiStreamlineHookedFn hooked = nullptr;
    void* hookedUserData = nullptr;
};

std::vector<PluginStreamlineRoutes> _slRoutes;
std::recursive_mutex _slMutex;
HMODULE _slModule = nullptr; // interposer the routes are installed on, null while none is hooked
uint32_t _slVersion[3] = {};

// State of the draw callback that is running, widgets do nothing without it
struct TabDrawState
{
    std::optional<ScopedCard> card;
    bool disabled = false;
    ImVec4 (*colorMap)(const ImVec4&) = nullptr;
};

thread_local TabDrawState* _draw = nullptr;
float _menuScale = 1.0f;

// Plugin code is called through these, a crash in it disables the plugin instead of the game
bool CallDraw(OptiDrawTabFn draw, void* userData)
{
    __try
    {
        draw(userData);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool CallInit(PFN_OptiPluginInit init, const OptiPluginApi* api, OptiPlugin* plugin, bool* result)
{
    __try
    {
        *result = init(api, plugin);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void ShowItemTooltip(const char* text)
{
    if (text == nullptr || !ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        return;

    ImGui::BeginTooltip();
    ImGui::TextUnformatted(text);
    ImGui::EndTooltip();
}

void WrappedText(const char* text, const ImVec4* color)
{
    if (color != nullptr)
        ImGui::PushStyleColor(ImGuiCol_Text, _draw->colorMap != nullptr ? _draw->colorMap(*color) : *color);

    ImGui::TextWrapped("%s", text);

    if (color != nullptr)
        ImGui::PopStyleColor();
}

// Copies at most bufferSize - 1 bytes without cutting a UTF-8 character in half
void CopyUtf8(const std::string& value, char* buffer, size_t bufferSize)
{
    if (buffer == nullptr || bufferSize == 0)
        return;

    size_t length = std::min(value.size(), bufferSize - 1);

    // Continuation bytes are 10xxxxxx
    if (length < value.size())
    {
        while (length > 0 && (static_cast<unsigned char>(value[length]) & 0xC0) == 0x80)
            length--;
    }

    memcpy(buffer, value.data(), length);
    buffer[length] = '\0';
}

// "auto" is treated as a missing value, like for OptiScaler options
std::optional<std::string> ReadValue(OptiPlugin* plugin, const char* key)
{
    if (plugin == nullptr || key == nullptr)
        return std::nullopt;

    auto value = Config::Instance()->GetRawValue(plugin->section.c_str(), key);
    if (!value.has_value())
        return std::nullopt;

    std::string lower = *value;
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return (char) std::tolower(c); });

    if (lower == "auto")
        return std::nullopt;

    return value;
}

template <typename T> std::optional<T> ParseNumber(const std::string& text)
{
    T value {};
    const char* end = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(text.data(), end, value);

    if (ec != std::errc() || ptr != end)
        return std::nullopt;

    return value;
}

void WriteValue(OptiPlugin* plugin, const char* key, const std::string& value)
{
    if (plugin == nullptr || key == nullptr)
        return;

    Config::Instance()->SetRawValue(plugin->section.c_str(), key, value.c_str());
}

// API functions -----------------------------

bool Api_RegisterTab(OptiPlugin* plugin, const char* name, const char* description, OptiDrawTabFn draw, void* userData)
{
    if (plugin == nullptr || name == nullptr || name[0] == '\0' || draw == nullptr)
        return false;

    auto tab = std::make_shared<PluginTab>();
    tab->plugin = plugin;
    tab->name = name;
    tab->description = description != nullptr ? description : "";
    tab->draw = draw;
    tab->userData = userData;

    std::lock_guard<std::mutex> lock(_mutex);
    _tabs.push_back(std::move(tab));

    LOG_INFO("[{}] Registered tab: {}", plugin->name, name);

    return true;
}

bool Api_RegisterCustomCard(OptiPlugin* plugin, const char* title, OptiDrawTabFn draw, void* userData)
{
    if (plugin == nullptr || title == nullptr || title[0] == '\0' || draw == nullptr)
        return false;

    auto card = std::make_shared<PluginTab>();
    card->plugin = plugin;
    card->name = title;
    card->draw = draw;
    card->userData = userData;

    std::lock_guard<std::mutex> lock(_mutex);
    _cards.push_back(std::move(card));

    LOG_INFO("[{}] Registered Custom tab card: {}", plugin->name, title);

    return true;
}

void Api_ReportUpscaler(OptiPlugin* plugin, const char* name, float gpuTimeMs)
{
    if (plugin == nullptr || name == nullptr || name[0] == '\0')
        return;

    std::lock_guard<std::mutex> lock(_reportedMutex);

    if (_reported.name != name)
        _reported.name = name;

    _reported.gpuTimeMs = std::isfinite(gpuTimeMs) && gpuTimeMs > 0.0f ? gpuTimeMs : 0.0f;
    _reported.at = GetTickCount64();
}

PluginStreamlineRoutes* FindStreamlineRoutes(OptiPlugin* plugin)
{
    for (auto& entry : _slRoutes)
    {
        if (entry.plugin == plugin)
            return &entry;
    }

    return nullptr;
}

bool CallStreamlineHooked(OptiStreamlineHookedFn hooked, HMODULE module, const uint32_t* version, void* userData)
{
    __try
    {
        hooked(module, version[0], version[1], version[2], userData);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Installs the routes of one plugin on the hooked interposer, caller holds _slMutex
void InstallRoutes(PluginStreamlineRoutes& entry)
{
    if (_slModule == nullptr)
        return;

    std::vector<StreamlineRoute*> pending;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    for (auto& route : entry.routes)
    {
        if (route.installed)
            continue;

        auto target = reinterpret_cast<void*>(KernelBaseProxy::GetProcAddress_()(_slModule, route.exportName.c_str()));
        if (target == nullptr)
        {
            LOG_WARN("[{}] Streamline route {}: not exported by this interposer", entry.plugin->name, route.exportName);
            continue;
        }

        *route.next = target;
        if (DetourAttach(route.next, route.handler) != NO_ERROR)
        {
            LOG_ERROR("[{}] Streamline route {}: attach failed", entry.plugin->name, route.exportName);
            *route.next = nullptr;
            continue;
        }

        pending.push_back(&route);
    }

    auto result = DetourTransactionCommit();
    if (result != NO_ERROR)
    {
        LOG_ERROR("[{}] Streamline routes: commit failed {:X}", entry.plugin->name, result);
        for (auto* route : pending)
            *route->next = nullptr;
        return;
    }

    for (auto* route : pending)
        route->installed = true;

    LOG_INFO("[{}] Installed {} Streamline route(s) on interposer {}.{}.{}", entry.plugin->name, pending.size(),
             _slVersion[0], _slVersion[1], _slVersion[2]);
}

// Tells one plugin which interposer is hooked, caller holds _slMutex. The plugin may register its routes from here.
void NotifyHooked(OptiPlugin* plugin)
{
    auto* entry = FindStreamlineRoutes(plugin);
    if (entry == nullptr || entry->hooked == nullptr || _slModule == nullptr)
        return;

    auto hooked = entry->hooked;
    auto userData = entry->hookedUserData;
    if (!CallStreamlineHooked(hooked, _slModule, _slVersion, userData))
    {
        LOG_ERROR("[{}] Crashed in its Streamline callback, it is not called again", plugin->name);
        if ((entry = FindStreamlineRoutes(plugin)) != nullptr)
            entry->hooked = nullptr;
    }
}

bool Api_RegisterStreamlineRoutes(OptiPlugin* plugin, const OptiStreamlineRoute* routes, uint32_t count)
{
    if (plugin == nullptr || routes == nullptr || count == 0)
        return false;

    std::vector<StreamlineRoute> copy;
    for (uint32_t i = 0; i < count; i++)
    {
        const auto& route = routes[i];
        if (route.exportName == nullptr || route.exportName[0] == '\0' || route.handler == nullptr ||
            route.next == nullptr)
        {
            LOG_ERROR("[{}] Streamline route {} is invalid, none registered", plugin->name, i);
            return false;
        }

        *route.next = nullptr;
        copy.push_back({ route.exportName, route.handler, route.next, false });
    }

    std::lock_guard<std::recursive_mutex> lock(_slMutex);

    auto* entry = FindStreamlineRoutes(plugin);
    if (entry != nullptr && !entry->routes.empty())
    {
        LOG_ERROR("[{}] Streamline routes are already registered", plugin->name);
        return false;
    }

    if (entry == nullptr)
    {
        _slRoutes.push_back({ plugin });
        entry = &_slRoutes.back();
    }

    entry->routes = std::move(copy);
    LOG_INFO("[{}] Registered {} Streamline route(s)", plugin->name, count);

    InstallRoutes(*entry);
    return true;
}

bool Api_SetStreamlineCallback(OptiPlugin* plugin, OptiStreamlineHookedFn hooked, void* userData)
{
    if (plugin == nullptr)
        return false;

    std::lock_guard<std::recursive_mutex> lock(_slMutex);

    auto* entry = FindStreamlineRoutes(plugin);
    if (entry == nullptr)
    {
        _slRoutes.push_back({ plugin });
        entry = &_slRoutes.back();
    }

    entry->hooked = hooked;
    entry->hookedUserData = userData;

    // The interposer may already be hooked (games that link it at startup)
    NotifyHooked(plugin);
    return true;
}

// ---- NGX ray reconstruction provider (version 4)
std::mutex _ngxRrMutex;
OptiNgxRayReconstruction _ngxRr {};
bool _ngxRrSet = false;

bool Api_RegisterNgxRayReconstruction(OptiPlugin* plugin, const OptiNgxRayReconstruction* provider)
{
    if (plugin == nullptr || provider == nullptr || provider->size < sizeof(OptiNgxRayReconstruction) ||
        provider->create == nullptr || provider->evaluate == nullptr || provider->release == nullptr)
        return false;

    std::lock_guard<std::mutex> lock(_ngxRrMutex);
    if (_ngxRrSet)
    {
        LOG_WARN("[{}] An NGX ray reconstruction provider is already registered", plugin->name);
        return false;
    }

    _ngxRr = *provider;
    _ngxRr.size = sizeof(OptiNgxRayReconstruction);
    _ngxRrSet = true;
    LOG_INFO("[{}] Registered as the NGX ray reconstruction provider", plugin->name);
    return true;
}

// ---- Work a plugin records into the game's command list (version 5)
// Per thread: pairs come from the game's thread inside its own call (slEvaluateFeature, NGX evaluate)
thread_local int _cmdListWorkDepth = 0;
thread_local bool _cmdListWorkRestore = false; // tracking was on at the outermost Begin: End restores
std::atomic<bool> _cmdListWorkLogged { false };
std::atomic<bool> _cmdListWorkEndLogged { false };

static std::string CmdListWorkOwner(OptiPlugin* plugin)
{
    return plugin != nullptr ? plugin->name : std::string("NGX ray reconstruction");
}

void Api_BeginExternalCommandListWork(OptiPlugin* plugin, void* commandList)
{
    if (commandList == nullptr || _cmdListWorkDepth++ > 0)
        return;

    auto config = Config::Instance();
    const bool restoreCompute = config->RestoreComputeSignature.value_or_default();
    const bool restoreGraphics = config->RestoreGraphicSignature.value_or_default();

    if (!_cmdListWorkLogged.exchange(true))
        LOG_INFO("[{}] Command list work bracket in use: restore compute {}, graphics {}, extended {}",
                 CmdListWorkOwner(plugin), restoreCompute, restoreGraphics,
                 config->ExtendedStateRestore.value_or_default());

    // Tracking already off: one of OptiScaler's own upscaler calls is running and restores itself
    _cmdListWorkRestore = D3D12Hooks::IsRootSignatureTrackingEnabled();
    if (!_cmdListWorkRestore)
        return;

    if (restoreCompute || restoreGraphics)
        D3D12Hooks::HookToCommandListLate(static_cast<ID3D12GraphicsCommandList*>(commandList));

    // The plugin's bindings must not be recorded as the game's
    D3D12Hooks::SetRootSignatureTracking(false);
}

void Api_EndExternalCommandListWork(OptiPlugin* plugin, void* commandList)
{
    if (commandList == nullptr || _cmdListWorkDepth == 0 || --_cmdListWorkDepth > 0)
        return;

    if (!_cmdListWorkRestore)
        return;

    _cmdListWorkRestore = false;
    auto* cmdList = static_cast<ID3D12GraphicsCommandList*>(commandList);

    if (!_cmdListWorkEndLogged.exchange(true))
        LOG_INFO("[{}] First command list work end: game root signature known {}", CmdListWorkOwner(plugin),
                 D3D12Hooks::CanRestoreRootSignature(cmdList));

    D3D12Hooks::RestoreRoot(cmdList);
    D3D12Hooks::SetRootSignatureTracking(true);
}

void Api_Section(const char* title)
{
    if (_draw == nullptr)
        return;

    _draw->card.reset();
    _draw->card.emplace(title);
}

void Api_Separator()
{
    if (_draw != nullptr)
        ImGui::Separator();
}

void Api_SameLine()
{
    if (_draw != nullptr)
        ImGui::SameLine();
}

void Api_Spacing()
{
    if (_draw != nullptr)
        ImGui::Spacing();
}

void Api_Disabled(bool disabled)
{
    if (_draw == nullptr || _draw->disabled == disabled)
        return;

    if (disabled)
        ImGui::BeginDisabled();
    else
        ImGui::EndDisabled();

    _draw->disabled = disabled;
}

void Api_ItemTooltip(const char* text)
{
    if (_draw != nullptr)
        ShowItemTooltip(text);
}

void Api_Text(const char* text)
{
    if (_draw != nullptr && text != nullptr)
        WrappedText(text, nullptr);
}

void Api_TextDisabled(const char* text)
{
    if (_draw == nullptr || text == nullptr)
        return;

    // Disabled color isn't tone mapped by the menu either
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

void Api_TextColored(float r, float g, float b, float a, const char* text)
{
    if (_draw == nullptr || text == nullptr)
        return;

    const ImVec4 color(r, g, b, a);
    WrappedText(text, &color);
}

bool Api_Button(const char* label) { return _draw != nullptr && label != nullptr && ImGui::Button(label); }

bool Api_Checkbox(const char* label, bool* value)
{
    return _draw != nullptr && label != nullptr && value != nullptr && ImGui::Checkbox(label, value);
}

bool Api_SliderFloat(const char* label, float* value, float min, float max, const char* format)
{
    if (_draw == nullptr || label == nullptr || value == nullptr)
        return false;

    return ImGui::SliderFloat(label, value, min, max, format != nullptr ? format : "%.3f",
                              ImGuiSliderFlags_AlwaysClamp);
}

bool Api_SliderInt(const char* label, int32_t* value, int32_t min, int32_t max, const char* format)
{
    if (_draw == nullptr || label == nullptr || value == nullptr)
        return false;

    return ImGui::SliderInt(label, value, min, max, format != nullptr ? format : "%d", ImGuiSliderFlags_AlwaysClamp);
}

bool Api_Combo(const char* label, int32_t* current, const char* const* items, int32_t itemCount)
{
    if (_draw == nullptr || label == nullptr || current == nullptr || (items == nullptr && itemCount > 0))
        return false;

    const bool validIndex = *current >= 0 && *current < itemCount && items[*current] != nullptr;
    const char* preview = validIndex ? items[*current] : "";
    bool changed = false;

    if (ImGui::BeginCombo(label, preview))
    {
        for (int32_t i = 0; i < itemCount; i++)
        {
            ImGui::PushID(i);

            const bool selected = *current == i;
            if (ImGui::Selectable(items[i] != nullptr ? items[i] : "", selected) && !selected)
            {
                *current = i;
                changed = true;
            }

            if (selected)
                ImGui::SetItemDefaultFocus();

            ImGui::PopID();
        }

        ImGui::EndCombo();
    }

    return changed;
}

bool Api_InputText(const char* label, char* buffer, size_t bufferSize)
{
    if (_draw == nullptr || label == nullptr || buffer == nullptr || bufferSize == 0)
        return false;

    return ImGui::InputText(label, buffer, bufferSize);
}

bool Api_ColorEdit(const char* label, float* rgba, bool withAlpha)
{
    if (_draw == nullptr || label == nullptr || rgba == nullptr)
        return false;

    return withAlpha ? ImGui::ColorEdit4(label, rgba) : ImGui::ColorEdit3(label, rgba);
}

float Api_GetMenuScale() { return _menuScale; }

bool Api_IsMenuOpen() { return MenuCommon::IsVisible(); }

bool Api_GetBool(OptiPlugin* plugin, const char* key, bool defaultValue)
{
    auto value = ReadValue(plugin, key);
    if (!value.has_value())
        return defaultValue;

    std::string lower = *value;
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return (char) std::tolower(c); });

    if (lower == "true" || lower == "1")
        return true;

    if (lower == "false" || lower == "0")
        return false;

    return defaultValue;
}

int32_t Api_GetInt(OptiPlugin* plugin, const char* key, int32_t defaultValue)
{
    auto value = ReadValue(plugin, key);
    if (!value.has_value())
        return defaultValue;

    return ParseNumber<int32_t>(*value).value_or(defaultValue);
}

float Api_GetFloat(OptiPlugin* plugin, const char* key, float defaultValue)
{
    auto value = ReadValue(plugin, key);
    if (!value.has_value())
        return defaultValue;

    return ParseNumber<float>(*value).value_or(defaultValue);
}

bool Api_GetString(OptiPlugin* plugin, const char* key, char* buffer, size_t bufferSize, const char* defaultValue)
{
    auto value = ReadValue(plugin, key);

    if (!value.has_value())
    {
        CopyUtf8(defaultValue != nullptr ? defaultValue : "", buffer, bufferSize);
        return false;
    }

    CopyUtf8(*value, buffer, bufferSize);
    return true;
}

void Api_SetBool(OptiPlugin* plugin, const char* key, bool value) { WriteValue(plugin, key, value ? "true" : "false"); }

void Api_SetInt(OptiPlugin* plugin, const char* key, int32_t value) { WriteValue(plugin, key, std::to_string(value)); }

// Shortest text that reads back as the same float
void Api_SetFloat(OptiPlugin* plugin, const char* key, float value)
{
    WriteValue(plugin, key, std::format("{}", value));
}

void Api_SetString(OptiPlugin* plugin, const char* key, const char* value)
{
    WriteValue(plugin, key, value != nullptr ? value : "");
}

void Api_ResetValue(OptiPlugin* plugin, const char* key)
{
    if (plugin != nullptr && key != nullptr)
        Config::Instance()->DeleteRawValue(plugin->section.c_str(), key);
}

bool Api_SaveSettings(OptiPlugin* plugin)
{
    if (plugin == nullptr)
        return false;

    return Config::Instance()->SaveRawValues();
}

void Api_Log(OptiPlugin* plugin, OptiLogLevel level, const char* message)
{
    if (plugin == nullptr || message == nullptr)
        return;

    switch (level)
    {
    case OptiLog_Debug:
        LOG_DEBUG("[{}] {}", plugin->name, message);
        break;
    case OptiLog_Warn:
        LOG_WARN("[{}] {}", plugin->name, message);
        break;
    case OptiLog_Error:
        LOG_ERROR("[{}] {}", plugin->name, message);
        break;
    default:
        LOG_INFO("[{}] {}", plugin->name, message);
        break;
    }
}

const OptiPluginApi _api = {
    .size = sizeof(OptiPluginApi),
    .version = OPTI_PLUGIN_API_VERSION,

    .RegisterTab = Api_RegisterTab,

    .Section = Api_Section,
    .Separator = Api_Separator,
    .SameLine = Api_SameLine,
    .Spacing = Api_Spacing,
    .Disabled = Api_Disabled,
    .ItemTooltip = Api_ItemTooltip,

    .Text = Api_Text,
    .TextDisabled = Api_TextDisabled,
    .TextColored = Api_TextColored,

    .Button = Api_Button,
    .Checkbox = Api_Checkbox,
    .SliderFloat = Api_SliderFloat,
    .SliderInt = Api_SliderInt,
    .Combo = Api_Combo,
    .InputText = Api_InputText,
    .ColorEdit = Api_ColorEdit,

    .GetMenuScale = Api_GetMenuScale,
    .IsMenuOpen = Api_IsMenuOpen,

    .GetBool = Api_GetBool,
    .GetInt = Api_GetInt,
    .GetFloat = Api_GetFloat,
    .GetString = Api_GetString,
    .SetBool = Api_SetBool,
    .SetInt = Api_SetInt,
    .SetFloat = Api_SetFloat,
    .SetString = Api_SetString,
    .ResetValue = Api_ResetValue,
    .SaveSettings = Api_SaveSettings,

    .Log = Api_Log,

    .RegisterCustomCard = Api_RegisterCustomCard,
    .ReportUpscaler = Api_ReportUpscaler,

    .RegisterStreamlineRoutes = Api_RegisterStreamlineRoutes,
    .SetStreamlineCallback = Api_SetStreamlineCallback,

    .RegisterNgxRayReconstruction = Api_RegisterNgxRayReconstruction,

    .BeginExternalCommandListWork = Api_BeginExternalCommandListWork,
    .EndExternalCommandListWork = Api_EndExternalCommandListWork,
};

// Runs one tab or card callback with the widget state set up, a crash in it disables that entry
void DrawEntry(PluginTab& tab, float menuScale, ImVec4 (*colorMap)(const ImVec4&), bool asCard)
{
    _menuScale = menuScale;

    if (tab.faulted)
    {
        ImGui::TextColored(colorMap(ImVec4(1.0f, 0.4f, 0.4f, 1.0f)), "%s",
                           asCard ? "A plugin crashed while drawing its card and it was disabled.\nCheck the log for details."
                                  : "This plugin crashed while drawing its tab and was disabled.\nCheck the log for details.");
        return;
    }

    // Lock isn't held while the plugin runs, it can register more tabs from here
    TabDrawState state;
    state.colorMap = colorMap;

    // A card's content goes into a card with its title, a tab starts its cards itself
    if (asCard)
        state.card.emplace(tab.name.c_str());

    ImGuiErrorRecoveryState recovery;
    ImGui::ErrorRecoveryStoreState(&recovery);

    ImGui::PushID(tab.plugin->name.c_str());
    ImGui::PushID(tab.name.c_str());
    ImGui::PushItemWidth(220.0f * menuScale);

    _draw = &state;
    const bool finished = CallDraw(tab.draw, tab.userData);
    _draw = nullptr;

    if (!finished)
    {
        LOG_ERROR("[{}] Crashed while drawing {} {}, disabling it", tab.plugin->name, asCard ? "card" : "tab", tab.name);
        tab.faulted = true;

        // Closes whatever was left open when the plugin crashed, popups of a combo for example.
        // Also pops the IDs and the disabled state from above, but not the item width.
        auto& io = ImGui::GetIO();
        const bool enableAssert = io.ConfigErrorRecoveryEnableAssert;
        const bool enableTooltip = io.ConfigErrorRecoveryEnableTooltip;
        io.ConfigErrorRecoveryEnableAssert = false;
        io.ConfigErrorRecoveryEnableTooltip = false;

        ImGui::ErrorRecoveryTryToRecoverState(&recovery);

        io.ConfigErrorRecoveryEnableAssert = enableAssert;
        io.ConfigErrorRecoveryEnableTooltip = enableTooltip;

        ImGui::PopItemWidth();
        state.card.reset();
        return;
    }

    if (state.disabled)
        ImGui::EndDisabled();

    state.card.reset();

    ImGui::PopItemWidth();
    ImGui::PopID();
    ImGui::PopID();
}
} // namespace

void PluginApi::InitPlugin(HMODULE module, PFN_OptiPluginInit init, const std::filesystem::path& path)
{
    if (module == nullptr || init == nullptr)
        return;

    OptiPlugin* plugin = nullptr;

    {
        std::lock_guard<std::mutex> lock(_mutex);

        // Late loaded plugins can be reported more than once
        if (std::ranges::find(_initedModules, module) != _initedModules.end())
            return;

        _initedModules.push_back(module);

        // Name shouldn't change when the plugin is switched to late loading
        std::string name = wstring_to_string(path.stem().wstring());
        constexpr std::string_view lateSuffix = "-loadlate";

        if (name.size() > lateSuffix.size())
        {
            std::string tail = name.substr(name.size() - lateSuffix.size());
            std::ranges::transform(tail, tail.begin(), [](unsigned char c) { return (char) std::tolower(c); });

            if (tail == lateSuffix)
                name.resize(name.size() - lateSuffix.size());
        }

        auto& created = _plugins.emplace_back(std::make_unique<OptiPlugin>());
        created->name = name;
        created->section = "Plugin." + name;
        plugin = created.get();
    }

    LOG_INFO("Initializing plugin API for {}", plugin->name);

    bool accepted = false;
    const bool finished = CallInit(init, &_api, plugin, &accepted);

    if (finished && accepted)
        return;

    if (!finished)
        LOG_ERROR("[{}] Crashed in OptiPluginInit, its tabs are removed", plugin->name);
    else
        LOG_INFO("[{}] Declined the plugin API", plugin->name);

    std::lock_guard<std::mutex> lock(_mutex);
    std::erase_if(_tabs, [plugin](const auto& tab) { return tab->plugin == plugin; });
    std::erase_if(_cards, [plugin](const auto& card) { return card->plugin == plugin; });
}

std::vector<PluginApi::TabInfo> PluginApi::GetTabs()
{
    std::lock_guard<std::mutex> lock(_mutex);

    std::vector<TabInfo> tabs;
    tabs.reserve(_tabs.size());

    for (const auto& tab : _tabs)
        tabs.push_back({ tab->name, tab->description });

    return tabs;
}

void PluginApi::DrawTab(size_t index, float menuScale, ImVec4 (*colorMap)(const ImVec4&))
{
    std::shared_ptr<PluginTab> tab;

    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (index >= _tabs.size())
            return;

        tab = _tabs[index];
    }

    DrawEntry(*tab, menuScale, colorMap, false);
}

bool PluginApi::DrawCustomCards(float menuScale, ImVec4 (*colorMap)(const ImVec4&))
{
    std::vector<std::shared_ptr<PluginTab>> cards;

    {
        std::lock_guard<std::mutex> lock(_mutex);
        cards = _cards;
    }

    for (const auto& card : cards)
        DrawEntry(*card, menuScale, colorMap, true);

    return !cards.empty();
}

bool PluginApi::ReportedUpscaler(std::string* name, float* gpuTimeMs)
{
    std::lock_guard<std::mutex> lock(_reportedMutex);

    if (_reported.at == 0 || GetTickCount64() - _reported.at > 500)
        return false;

    if (name != nullptr)
        *name = _reported.name;

    if (gpuTimeMs != nullptr)
        *gpuTimeMs = _reported.gpuTimeMs;

    return true;
}

void PluginApi::OnInterposerHooked(HMODULE module, uint32_t major, uint32_t minor, uint32_t patch)
{
    std::lock_guard<std::recursive_mutex> lock(_slMutex);

    _slModule = module;
    _slVersion[0] = major;
    _slVersion[1] = minor;
    _slVersion[2] = patch;

    // Callbacks first (a plugin checks the version and registers its routes there), then every registered route
    std::vector<OptiPlugin*> plugins;
    for (auto& entry : _slRoutes)
        plugins.push_back(entry.plugin);

    for (auto* plugin : plugins)
        NotifyHooked(plugin);

    for (auto& entry : _slRoutes)
        InstallRoutes(entry);
}

void PluginApi::OnInterposerUnhooking()
{
    std::lock_guard<std::recursive_mutex> lock(_slMutex);

    if (_slModule == nullptr)
        return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    for (auto& entry : _slRoutes)
    {
        for (auto& route : entry.routes)
        {
            if (route.installed)
                DetourDetach(route.next, route.handler);
        }
    }

    auto result = DetourTransactionCommit();
    if (result != NO_ERROR)
        LOG_ERROR("Streamline routes: detach failed {:X}", result);

    for (auto& entry : _slRoutes)
    {
        for (auto& route : entry.routes)
        {
            if (route.installed)
            {
                *route.next = nullptr;
                route.installed = false;
            }
        }
    }

    _slModule = nullptr;
}

void PluginApi::BeginCommandListWork(OptiPlugin* plugin, void* commandList)
{
    Api_BeginExternalCommandListWork(plugin, commandList);
}

void PluginApi::EndCommandListWork(OptiPlugin* plugin, void* commandList)
{
    Api_EndExternalCommandListWork(plugin, commandList);
}

const OptiNgxRayReconstruction* PluginApi::NgxRayReconstruction()
{
    std::lock_guard<std::mutex> lock(_ngxRrMutex);
    return _ngxRrSet ? &_ngxRr : nullptr;
}
