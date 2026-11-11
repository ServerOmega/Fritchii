// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// NVIDIA Reflex through VK_NV_low_latency2 (Graphics > NVIDIA Reflex, EDEN_REFLEX).
//
// The swapchain is created with latency mode on and its sleep mode set (vk_swapchain.cpp). Each
// emulated frame starts on the GPU thread right after the previous one was handed to the
// present queue: there the renderer sleeps in vkLatencySleepNV until the driver wants the next
// frame, and marks the frame's stages with one present id per rendered frame. The GPU thread's
// command translation counts as render submission; the guest CPU's work is outside what the
// renderer can see, so simulation start and end are marked together at the frame start.

#pragma once

#include <atomic>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Swapchain;

/// The present id queue submissions currently belong to (VkLatencySubmissionPresentIdNV), or 0.
[[nodiscard]] u64 ReflexSubmissionPresentId();

class Reflex {
public:
    explicit Reflex(const Device& device, Swapchain& swapchain);
    ~Reflex();

    Reflex(const Reflex&) = delete;
    Reflex& operator=(const Reflex&) = delete;

    /// At the start of Composite: opens a frame if the last one was closed (first frame).
    void EnsureFrame();
    /// After the frame was handed to the present queue: sleeps until the driver wants the next
    /// frame and opens it.
    void NextFrame();
    /// After the frame's last submission.
    void RenderSubmitEnd();

    /// Present id of the rendered frame (markers name it) and of generated frame `slot` (1..3).
    [[nodiscard]] u64 RenderedPresentId() const {
        return frame * 8 + 7;
    }
    [[nodiscard]] u64 GeneratedPresentId(u32 slot) const {
        return frame * 8 + (slot & 3);
    }
    [[nodiscard]] bool Active() const;

private:
    void Sleep();
    void Report();

    const Device& device;
    Swapchain& swapchain;
    vk::Semaphore timeline;
    u64 sleep_value{};
    u64 frame{};
    bool open{};
    bool sleep_enabled{true};
    u32 sleep_timeouts{};
    u64 sleeps{};
    bool logged_first{};
};

} // namespace Vulkan
