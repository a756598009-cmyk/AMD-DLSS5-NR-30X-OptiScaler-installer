#pragma once
#include "SysUtils.h"

void HookFSR2VkExeInputs();

// ffx_fsr2_api_vk_x64: physical device and the backend identity for FSR2Vk_IsVkDescription
void HookFSR2VkBackendInputs(HMODULE module);

// Routing for the generic ffx_fsr2_api_x64 hooks when a context runs on the Vulkan backend
bool FSR2Vk_IsVkDescription(const void* contextDescription);
bool FSR2Vk_HasContext(void* context);
int32_t FSR2Vk_ContextCreate(void* original, void* context, void* contextDescription);
int32_t FSR2Vk_ContextDispatch(void* original, void* context, const void* dispatchDescription);
int32_t FSR2Vk_ContextDestroy(void* original, void* context);
