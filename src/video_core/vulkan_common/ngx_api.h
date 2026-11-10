// SPDX-License-Identifier: GPL-3.0-or-later
// Declarations only: optional user-installed NVIDIA SDK loaded through its public API.
#pragma once
#include <vulkan/vulkan_core.h>
#include <nvsdk_ngx_vk.h>
namespace Vulkan::Ngx {
struct Api {
    decltype(&NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements) instance_requirements{};
    decltype(&NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements) device_requirements{};
    decltype(&NVSDK_NGX_VULKAN_RequiredExtensions) extensions{};
    decltype(&NVSDK_NGX_VULKAN_Init_with_ProjectID) init{};
    decltype(&NVSDK_NGX_VULKAN_Shutdown1) shutdown{};
    decltype(&NVSDK_NGX_VULKAN_GetCapabilityParameters) capabilities{};
    decltype(&NVSDK_NGX_VULKAN_AllocateParameters) allocate{};
    decltype(&NVSDK_NGX_VULKAN_DestroyParameters) destroy{};
    decltype(&NVSDK_NGX_VULKAN_CreateFeature) create{};
    decltype(&NVSDK_NGX_VULKAN_ReleaseFeature) release{};
    decltype(&NVSDK_NGX_VULKAN_EvaluateFeature_C) evaluate{};
};
const Api& GetApi();
} // namespace Vulkan::Ngx
