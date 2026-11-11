// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// NVIDIA DLSS Frame Generation through native NGX (Linux). Without Streamline nothing presents
// for us: the renderer records the evaluations after a frame is drawn, presents the generated
// frames ahead of the real one, and the present thread paces them (DlssgPacer).
//
// Inputs: depth and motion are the ones DLSS super resolution got this frame (copied when the
// upscaler runs, as the game draws over its own afterwards); the backbuffer is the finished
// frame with the HUD. Frames without a DLSS evaluation (menus, movies) generate nothing.

#pragma once

#include <array>
#include <atomic>
#include <memory>

#include "common/common_types.h"
#include "common/math_util.h"
#include "video_core/nvidia_dlss_inputs.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

namespace Ngx {
class FrameGenFeature;
}

class Device;
class MemoryAllocator;
class Scheduler;
struct Frame;

class NgxFrameGen {
public:
    explicit NgxFrameGen(const Device& device, MemoryAllocator& memory_allocator,
                         Scheduler& scheduler);
    ~NgxFrameGen();

    NgxFrameGen(const NgxFrameGen&) = delete;
    NgxFrameGen& operator=(const NgxFrameGen&) = delete;

    /// The renderer's instance while frame generation can run, else null (GPU thread).
    [[nodiscard]] static NgxFrameGen* Current();

    /// The DLSS upscaler's copies of this frame's depth and motion (GPU thread).
    struct Capture {
        vk::Image depth;
        vk::ImageView depth_view;
        vk::Image motion;
        vk::ImageView motion_view;
        VkFormat depth_format{};
        VkFormat motion_format{};
        VkImageAspectFlags depth_aspects{};
        VkExtent2D extent{};
    };
    /// Called by the DLSS upscaler before it records an evaluation. Returns where the inputs go.
    const Capture* PrepareCapture(const NvidiaDlssInputs& inputs);
    /// Recorded right after the DLSS evaluation, with its inputs in SHADER_READ_ONLY_OPTIMAL.
    static void RecordCapture(vk::CommandBuffer cmdbuf, const NvidiaDlssInputs& inputs,
                              const Capture& capture);

    /// For the frame being composited: how many frames to generate in front of it (0 for none).
    /// `screen` is where the game image is in the frame.
    u32 BeginFrame(Frame* real, VkFormat view_format, const Common::Rectangle<u32>& screen);
    /// Records generated frame `index` (1..count) of this frame into `destination`.
    void GenerateInto(Frame* destination, u32 index);

private:
    void CreateCapture(const NvidiaDlssInputs& inputs);
    void CreateOutput(VkExtent2D extent, VkFormat format);

    const Device& device;
    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;
    std::unique_ptr<Ngx::FrameGenFeature> feature;

    Capture capture;
    // This frame's DLSS inputs, as the upscaler saw them (GPU thread).
    NvidiaDlssInputs captured{};
    u64 capture_sequence{};
    u64 consumed_sequence{};
    bool need_reset{true};

    // The frame being composited (GPU thread).
    VkImage real_image{};
    VkImageView real_view{};
    VkFormat real_format{};
    VkExtent2D real_extent{};
    VkOffset2D subrect_offset{};
    VkExtent2D subrect_extent{};
    NvidiaDlssInputs frame_inputs{};
    bool frame_reset{};
    u32 generations{};
    u64 frame_id{};

    // Output of an evaluation, copied into the generated frame's image.
    vk::Image output_image;
    vk::ImageView output_view;
    VkExtent2D output_extent{};
    VkFormat output_format{};

    u32 multiplier{1};
    u64 frames_generated{};
    u64 groups{};
    std::atomic<bool> failed{};
    std::atomic<u64> evaluations_ok{};
    bool logged_first{};
};

} // namespace Vulkan
