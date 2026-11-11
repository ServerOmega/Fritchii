// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// NVIDIA DLSS through the platform backend, using the game-specific input contract.

#pragma once

#include <array>
#include <atomic>
#include <memory>
#include "video_core/nvidia_dlss_inputs.h"
#ifdef EGLANTINE_NGX
#include "video_core/vulkan_common/ngx.h"
#endif

#include "common/common_types.h"
#include "common/settings_enums.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace VideoCore {
struct DlssStatus;
}

namespace Vulkan {

class Device;
class MemoryAllocator;
class Scheduler;

class NvidiaDlssUpscaler {
public:
    using Image = NvidiaDlssImage;
    using Inputs = NvidiaDlssInputs;

    /// Throws vk::Exception when the selected backend has no DLSS for this device.
    explicit NvidiaDlssUpscaler(const Device& device, Scheduler& scheduler,
                                    MemoryAllocator& memory_allocator);
    ~NvidiaDlssUpscaler();

    /// Records one evaluation into the output image (RGBA16F at the upscale size, GENERAL
    /// layout). Returns false when DLSS can't run, so the caller can fall back to FSR 3.1.
    bool Upscale(const Inputs& inputs);

    [[nodiscard]] bool Failed() const noexcept {
        return failed.load();
    }

    /// Evaluations that succeeded in this process, for the status bar.
    [[nodiscard]] static VideoCore::DlssStatus Status();

    [[nodiscard]] VkImage OutputImage() const noexcept {
        return *output_image;
    }
    [[nodiscard]] VkImageView OutputView() const noexcept {
        return *output_view;
    }
    /// Nearest-neighbour clamp sampler (normalized coordinates), for reading the output.
    [[nodiscard]] VkSampler PointSampler() const noexcept {
        return *point_sampler;
    }
    [[nodiscard]] VkSampler LinearSampler() const noexcept {
        return *linear_sampler;
    }

private:
    void CreateOutput(VkExtent2D upscale_size);

    const Device& device;
    Scheduler& scheduler;
    MemoryAllocator& memory_allocator;
#ifdef EGLANTINE_STREAMLINE
    void* set_options{}; ///< slDLSSSetOptions
    void* get_optimal_settings{}; ///< slDLSSGetOptimalSettings
#endif
#ifdef EGLANTINE_NGX
    std::unique_ptr<Ngx::Feature> feature;
#endif
    VkExtent2D render_extent{};
    VkExtent2D upscale_extent{};
    bool hdr_input = true;
#ifdef EGLANTINE_NGX
    bool depth_inverted_input = true;
#endif
    Settings::NvidiaDlssQuality quality_mode{};
    Settings::DlssPreset model_preset{};
    Settings::NvidiaDlssQuality evaluated_mode{}; ///< Recorded on the scheduler's worker
    u32 frame_index{};
    vk::Image output_image;
    vk::ImageView output_view;
    vk::Sampler point_sampler;
    vk::Sampler linear_sampler;
    std::atomic<bool> failed{};
#ifdef EGLANTINE_STREAMLINE
    std::atomic<bool> vram_warned{};
#endif
};

} // namespace Vulkan
