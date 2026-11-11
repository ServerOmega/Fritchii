// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_vulkan/vk_reflex.h"

#include <algorithm>
#include <chrono>

#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {
namespace {
std::atomic<u64> submission_present_id{};

// Longer than any frame the driver would hold one for; three of these turn the sleep off.
constexpr u64 SLEEP_TIMEOUT_NS = 200'000'000;
constexpr u64 REPORT_INTERVAL = 600;
} // namespace

u64 ReflexSubmissionPresentId() {
    return submission_present_id.load(std::memory_order_relaxed);
}

Reflex::Reflex(const Device& device_, Swapchain& swapchain_)
    : device{device_}, swapchain{swapchain_} {
    static constexpr VkSemaphoreTypeCreateInfo type_ci{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .pNext = nullptr,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };
    timeline = device.GetLogical().CreateSemaphore(VkSemaphoreCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &type_ci,
        .flags = 0,
    });
    LOG_INFO(Render_Vulkan, "Reflex: {}",
             swapchain.LatencyEnabled()
                 ? "on (sleep at each frame start, markers per rendered frame)"
                 : "asked for, but the swapchain has no latency mode yet");
}

Reflex::~Reflex() {
    submission_present_id.store(0, std::memory_order_relaxed);
    if (sleep_value != 0) {
        // The driver may still signal the last sleep.
        void(timeline.Wait(sleep_value, SLEEP_TIMEOUT_NS));
    }
}

bool Reflex::Active() const {
    return swapchain.LatencyEnabled();
}

void Reflex::EnsureFrame() {
    if (open) {
        return;
    }
    ++frame;
    open = true;
    if (!Active()) {
        submission_present_id.store(0, std::memory_order_relaxed);
        return;
    }
    const u64 id = RenderedPresentId();
    swapchain.LatencyMarker(id, VK_LATENCY_MARKER_SIMULATION_START_NV);
    swapchain.LatencyMarker(id, VK_LATENCY_MARKER_SIMULATION_END_NV);
    swapchain.LatencyMarker(id, VK_LATENCY_MARKER_RENDERSUBMIT_START_NV);
    submission_present_id.store(id, std::memory_order_relaxed);
}

void Reflex::RenderSubmitEnd() {
    if (open && Active()) {
        swapchain.LatencyMarker(RenderedPresentId(), VK_LATENCY_MARKER_RENDERSUBMIT_END_NV);
    }
}

void Reflex::NextFrame() {
    open = false;
    if (Active()) {
        Sleep();
        if (frame % REPORT_INTERVAL == 0) {
            Report();
        }
    }
    EnsureFrame();
}

void Reflex::Sleep() {
    if (!sleep_enabled) {
        return;
    }
    const u64 value = sleep_value + 1;
    if (!swapchain.LatencySleep(*timeline, value)) {
        return;
    }
    sleep_value = value;
    if (timeline.Wait(value, SLEEP_TIMEOUT_NS)) {
        ++sleeps;
        if (!logged_first) {
            LOG_INFO(Render_Vulkan, "Reflex: first frame start sleep done");
            logged_first = true;
        }
        return;
    }
    if (++sleep_timeouts >= 3) {
        LOG_WARNING(Render_Vulkan,
                    "Reflex: vkLatencySleepNV never signalled in time ({} times); frame start "
                    "sleep off, markers stay on",
                    sleep_timeouts);
        sleep_enabled = false;
    }
}

void Reflex::Report() {
    const auto reports = swapchain.LatencyTimings();
    u64 count{};
    double total{}, submit{}, present{}, gpu{}, queue{};
    for (const VkLatencyTimingsFrameReportNV& r : reports) {
        if (r.simStartTimeUs == 0 || r.gpuRenderEndTimeUs < r.simStartTimeUs) {
            continue;
        }
        ++count;
        total += static_cast<double>(r.gpuRenderEndTimeUs - r.simStartTimeUs);
        submit += static_cast<double>(r.renderSubmitEndTimeUs - r.renderSubmitStartTimeUs);
        present += static_cast<double>(r.presentEndTimeUs - r.presentStartTimeUs);
        gpu += static_cast<double>(r.gpuRenderEndTimeUs - r.gpuRenderStartTimeUs);
        if (r.osRenderQueueEndTimeUs >= r.osRenderQueueStartTimeUs) {
            queue += static_cast<double>(r.osRenderQueueEndTimeUs - r.osRenderQueueStartTimeUs);
        }
    }
    if (count == 0) {
        LOG_INFO(Render_Vulkan,
                 "Reflex: frame {}: no complete latency reports yet ({} reports, {} sleeps)",
                 frame, reports.size(), sleeps);
        return;
    }
    const auto ms = [count](double us) { return us / static_cast<double>(count) / 1000.0; };
    LOG_INFO(Render_Vulkan,
             "Reflex: frame {}: last {} frames: frame start to GPU done {:.2f} ms, render submit "
             "{:.2f} ms, present call {:.2f} ms, GPU render {:.2f} ms, OS queue {:.2f} ms ({} "
             "sleeps)",
             frame, count, ms(total), ms(submit), ms(present), ms(gpu), ms(queue), sleeps);
}

} // namespace Vulkan
