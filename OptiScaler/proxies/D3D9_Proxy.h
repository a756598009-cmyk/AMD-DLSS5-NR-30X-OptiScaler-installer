#pragma once
#include <windows.h>

namespace D3D9Proxy
{
// The system d3d9.dll, and whether the device hooks are installed (off in pass-through mode).
void Init(HMODULE systemModule, bool hook);
HMODULE Module();
} // namespace D3D9Proxy
