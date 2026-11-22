#pragma once

#include "Config.h"
#include <plugins/PluginApi.h>
#include <proxies/KernelBase_Proxy.h>
#include <proxies/Ntdll_Proxy.h>
#include <filesystem>
#include <vector>
#include <string>
#include <algorithm>

namespace CustomProviders
{
inline std::filesystem::path Root()
{
    return std::filesystem::path(Config::Instance()->MainDllPath.value()) / L"custom";
}

inline std::filesystem::path Folder(const std::string& name)
{
    return Root() / string_to_wstring(name);
}

inline std::filesystem::path DlssgDllPath(const std::string& name)
{
    const auto folder = Folder(name) / L"dlssg";
    std::error_code error;
    const auto standard = folder / L"nvngx_dlssg.dll";
    if (std::filesystem::exists(standard, error))
        return standard;

    for (const auto& entry : std::filesystem::directory_iterator(folder, error))
    {
        if (_wcsicmp(entry.path().extension().c_str(), L".dll") == 0)
            return entry.path();
    }
    return {};
}

inline HMODULE LoadDll(const std::string& name, const std::filesystem::path& path)
{
    auto dll = path.empty() ? nullptr : NtdllProxy::LoadLibraryExW_Ldr(path.c_str(), NULL, NULL);
    if (dll == nullptr)
    {
        LOG_ERROR("Can't load custom DLSSG provider {} from {}", name,
                  wstring_to_string(Folder(name).wstring()));
        return nullptr;
    }

    LOG_INFO("Loaded custom DLSSG provider {} from {}", name, wstring_to_string(path.wstring()));
    PluginApi::InitPlugin(dll,
        (PFN_OptiPluginInit) KernelBaseProxy::GetProcAddress_()(dll, OPTI_PLUGIN_INIT_NAME), name);
    return dll;
}

struct Names { std::vector<std::string> dlssg; };

inline const Names& Scan()
{
    static const auto names = []
    {
        Names found;
        std::error_code error;

        for (const auto& entry : std::filesystem::directory_iterator(Root(), error))
        {
            if (!entry.is_directory(error))
                continue;

            const auto name = wstring_to_string(entry.path().filename().wstring());
            if (!DlssgDllPath(name).empty())
                found.dlssg.push_back(name);
        }

        return found;
    }();

    return names;
}

inline bool IsDlssg(const std::string& name)
{
    const auto& names = Scan().dlssg;
    return std::ranges::any_of(names, [&name](const std::string& item)
    {
        return _stricmp(item.c_str(), name.c_str()) == 0;
    });
}

inline const std::vector<std::string>& DlssgProviders()
{
    return Scan().dlssg;
}
} // namespace CustomProviders
