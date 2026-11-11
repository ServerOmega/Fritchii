// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "common/settings_enums.h"
#include "video_core/dlss_mode.h"
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

/// NVIDIA's render-size range for a quality mode at this output size, from NGX's optimal-settings
/// query. Empty when the runtime has no answer for the mode. Recording worker only.
std::optional<VideoCommon::DlssMode::Range> OptimalRange(Settings::NvidiaDlssQuality mode,
                                                         VkExtent2D output);

// ---- DLSS Frame Generation (DLSS-G) through native NGX ---------------------------------------
// NGX's Vulkan DLSS-G (libnvidia-ngx-dlssg.so) only computes the generated frames; Fritchii
// records the evaluations and presents and paces the frames itself (vk_ngx_frame_gen.cpp).

/// The multiplier asked for (1 = off, 2 to 4) by Graphics > DLSS Frame Generation or EDEN_DLSSG.
/// Read at game start (Start()).
[[nodiscard]] u32 FrameGenRequested();
/// Finds and stages libnvidia-ngx-dlssg.so for NGX's search path. Called by Start() after the
/// DLSS runtime was prepared; false when the library is missing (DLSS keeps running without it).
bool PrepareFrameGen(const std::filesystem::path& dir, const std::filesystem::path& cache);
/// The device extensions DLSS-G asks for (empty when it isn't wanted).
std::vector<std::string> FrameGenDeviceExtensions(VkInstance instance, VkPhysicalDevice physical);
/// Turns DLSS-G off for this session (DLSS super resolution keeps running).
void DropFrameGen(const char* reason);
/// DLSS-G can run on the attached device.
[[nodiscard]] bool IsFrameGenAvailable();
/// The multiplier frame generation runs at (2 to 4; capped by the GPU), or 1 when it's off.
[[nodiscard]] u32 FrameGenMultiplier();

/// One DLSS-G evaluation: NGX writes one generated frame between the previous backbuffer and this
/// one into `output`. multi_frame_index counts 1..multi_frame_count (multiplier - 1).
struct FrameGenInputs {
    NvidiaDlssImage backbuffer;       ///< The real frame as it would be presented (HUD included)
    VkExtent2D backbuffer_size;
    VkOffset2D subrect_offset;        ///< The game image inside the backbuffer
    VkExtent2D subrect_size;
    NvidiaDlssImage depth;            ///< This frame's DLSS depth (copy), at the render size
    NvidiaDlssImage motion_vectors;   ///< This frame's DLSS motion (copy), at the render size
    VkExtent2D render_size;
    NvidiaDlssImage output;           ///< Same size and format as the backbuffer, storage
    std::array<f32, 2> jitter;
    std::array<f32, 2> motion_vector_scale;
    bool reset;
    bool depth_inverted;
    u32 multi_frame_count;
    u32 multi_frame_index;
    u64 frame_id;
};

// Like Feature: recording worker only; reconfiguration and destruction require GPU idle.
class FrameGenFeature {
public:
    FrameGenFeature();
    ~FrameGenFeature();
    FrameGenFeature(const FrameGenFeature&) = delete;
    FrameGenFeature& operator=(const FrameGenFeature&) = delete;
    /// Creates the NGX feature when needed (first call or a new size or format), then evaluates.
    bool Evaluate(VkCommandBuffer command, const FrameGenInputs& inputs);
    [[nodiscard]] u32 LastResult() const;
    /// The last Evaluate created the feature (for the log).
    [[nodiscard]] bool JustCreated() const;
    /// Result of the last feature creation, and whether it failed (for the log).
    [[nodiscard]] u32 CreateResult() const;
    [[nodiscard]] bool CreateFailed() const;
    void Release();
private:
    struct State;
    std::unique_ptr<State> state;
};

// Evaluate runs only on the recording worker. Reconfiguration and destruction require GPU idle.
class Feature {
public:
    Feature();
    ~Feature();
    Feature(const Feature&) = delete;
    Feature& operator=(const Feature&) = delete;
    /// mode is the quality mode to run in (not Automatic), picked with VideoCommon::DlssMode.
    bool NeedsRecreate(const NvidiaDlssInputs& inputs, Settings::NvidiaDlssQuality mode) const;
    bool Evaluate(VkCommandBuffer command, const NvidiaDlssInputs& inputs,
                  Settings::NvidiaDlssQuality mode, const NvidiaDlssImage& output);
    u32 LastResult() const;
private:
    struct State;
    std::unique_ptr<State> state;
};

} // namespace Ngx
} // namespace Vulkan
