// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include "video_core/nvidia_dlss_inputs.h"

namespace Vulkan {
namespace vk { struct DeviceDispatch; }
namespace Ngx {

bool Prepare(const std::filesystem::path& runtime, const std::filesystem::path& cache);
bool Initialize(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                PFN_vkGetInstanceProcAddr get_instance, PFN_vkGetDeviceProcAddr get_device);
bool Start();
bool WantsDevice();
bool IsDlssAvailable();
std::vector<std::string> InstanceExtensions();
std::vector<std::string> DeviceExtensions(VkInstance instance, VkPhysicalDevice physical);
bool AttachDevice(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                  u32 graphics_family, vk::DeviceDispatch& dispatch);
void Stop();
u32 LastResult();

// Evaluate runs only on the recording worker. Reconfiguration and destruction require GPU idle.
class Feature {
public:
    Feature();
    ~Feature();
    Feature(const Feature&) = delete;
    Feature& operator=(const Feature&) = delete;
    bool NeedsRecreate(const NvidiaDlssInputs& inputs) const;
    bool Evaluate(VkCommandBuffer command, const NvidiaDlssInputs& inputs,
                  const NvidiaDlssImage& output);
    u32 LastResult() const;
private:
    struct State;
    std::unique_ptr<State> state;
};

} // namespace Ngx
} // namespace Vulkan
