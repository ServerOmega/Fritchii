// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/vulkan_common/ngx.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan::Ngx {
namespace {
// NGX image-clear synchronization (DLSS 310.9.16): NGX can clear an internal image right after a
// transition to GENERAL whose destination scope covers shader writes but not transfer writes;
// synchronization validation reports WRITE_AFTER_WRITE hazards there. The device-procedure
// callback handed to NGX wraps vkCmdPipelineBarrier so such image barriers also cover the transfer
// stage and transfer writes. Only barriers NGX itself records go through it.
// From thanosipsis's workaround (thanosipsis/Lichii-linux-dlss@500c6cde069c), as integrated in
// myshadowww/Lichii#3. EDEN_NX2_NGX_BARRIER_FIX=0 hands NGX the driver's procedures unwrapped.
PFN_vkGetDeviceProcAddr ngx_get_device_proc{};
std::atomic<PFN_vkCmdPipelineBarrier> ngx_pipeline_barrier{};

VKAPI_ATTR void VKAPI_CALL NgxPipelineBarrier(VkCommandBuffer command, VkPipelineStageFlags src,
                                              VkPipelineStageFlags dst, VkDependencyFlags flags,
                                              u32 memory_count, const VkMemoryBarrier* memories,
                                              u32 buffer_count,
                                              const VkBufferMemoryBarrier* buffers,
                                              u32 image_count, const VkImageMemoryBarrier* images) {
    const auto barrier = ngx_pipeline_barrier.load(std::memory_order_acquire);
    const auto needs_transfer = [](const VkImageMemoryBarrier& image) {
        return image.newLayout == VK_IMAGE_LAYOUT_GENERAL &&
               (image.dstAccessMask & VK_ACCESS_SHADER_WRITE_BIT) != 0;
    };
    if (image_count == 0 || images == nullptr ||
        std::none_of(images, images + image_count, needs_transfer)) {
        barrier(command, src, dst, flags, memory_count, memories, buffer_count, buffers,
                image_count, images);
        return;
    }
    std::vector<VkImageMemoryBarrier> patched(images, images + image_count);
    for (auto& image : patched) {
        if (needs_transfer(image)) {
            image.dstAccessMask |= VK_ACCESS_TRANSFER_WRITE_BIT;
        }
    }
    barrier(command, src, dst | VK_PIPELINE_STAGE_TRANSFER_BIT, flags, memory_count, memories,
            buffer_count, buffers, image_count, patched.data());
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL NgxGetDeviceProcAddr(VkDevice device, const char* name) {
    const PFN_vkVoidFunction original = ngx_get_device_proc(device, name);
    if (original != nullptr && name != nullptr && std::strcmp(name, "vkCmdPipelineBarrier") == 0) {
        ngx_pipeline_barrier.store(reinterpret_cast<PFN_vkCmdPipelineBarrier>(original),
                                   std::memory_order_release);
        return reinterpret_cast<PFN_vkVoidFunction>(&NgxPipelineBarrier);
    }
    return original;
}

bool NgxBarrierFixEnabled() {
    const char* value = std::getenv("EDEN_NX2_NGX_BARRIER_FIX");
    return value == nullptr || *value != '0';
}

std::vector<std::filesystem::path> RuntimeDirectories() {
    std::vector<std::filesystem::path> dirs;
    if (const char* path = std::getenv("EDEN_NGX_DIR"); path && *path) {
        dirs.emplace_back(path);
    }
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error) {
        dirs.emplace_back(executable.parent_path() / "ngx");
    }
    const auto data = Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir);
    if (!data.empty()) {
        dirs.emplace_back(data / "ngx");
    }
    // Builds before Fritchii v5.8 used the eden data folder. Read the library from there too, so
    // a player who put it there doesn't have to move it. Nothing is copied or written there.
    const auto legacy = Common::FS::GetLegacyPath(Common::FS::EmuPath::LegacyEdenDir);
    if (!legacy.empty() && legacy != data) {
        dirs.emplace_back(legacy / "ngx");
    }
    return dirs;
}

// DLSS-G's library: the player's folders first, the copy next to the executable (bundled in
// this build's AppImage) last, so a newer library the player adds wins.
std::vector<std::filesystem::path> FrameGenDirectories(std::vector<std::filesystem::path> dirs) {
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error) {
        const auto bundled = executable.parent_path() / "ngx";
        std::erase(dirs, bundled);
        dirs.push_back(bundled);
    }
    return dirs;
}

u32 requested_multiplier = 1;

u32 ReadFrameGenRequest() {
    // EDEN_DLSSG=0|1 (off), 2, 3 or 4 overrides Graphics > DLSS Frame Generation.
    if (const char* value = std::getenv("EDEN_DLSSG"); value && *value) {
        const int multiplier = std::atoi(value);
        return multiplier >= 2 && multiplier <= 4 ? static_cast<u32>(multiplier) : 1u;
    }
    switch (Settings::values.nvidia_dlss_frame_gen.GetValue()) {
    case Settings::DlssFrameGen::X2:
        return 2;
    case Settings::DlssFrameGen::X3:
        return 3;
    case Settings::DlssFrameGen::X4:
        return 4;
    case Settings::DlssFrameGen::Off:
    default:
        return 1;
    }
}

std::string Joined(const std::vector<std::filesystem::path>& dirs) {
    std::string text;
    for (const auto& dir : dirs) {
        if (!text.empty()) {
            text += ", ";
        }
        text += dir.string();
    }
    return text.empty() ? "(no search paths)" : text;
}
} // namespace

u32 FrameGenRequested() {
    return requested_multiplier;
}

bool Start() {
    requested_multiplier = ReadFrameGenRequest();
    if (!Settings::values.nvidia_dlss.GetValue()) {
        if (requested_multiplier > 1) {
            LOG_WARNING(Render_Vulkan, "DLSS-G: {}x asked for, but NVIDIA DLSS is off; frame "
                                       "generation needs it", requested_multiplier);
        }
        return false;
    }
    const auto cache = Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "ngx";
    const auto dirs = RuntimeDirectories();
    for (const auto& dir : dirs) {
        if (Prepare(dir, cache)) {
            LOG_INFO(Render_Vulkan, "NVIDIA DLSS (native NGX): runtime found in {}", dir.string());
            if (requested_multiplier > 1) {
                const auto fg_dirs = FrameGenDirectories(dirs);
                const bool staged = std::ranges::any_of(
                    fg_dirs, [&](const auto& fg_dir) { return PrepareFrameGen(fg_dir, cache); });
                if (!staged) {
                    LOG_WARNING(Render_Vulkan,
                                "DLSS-G: {}x asked for, but libnvidia-ngx-dlssg.so is missing "
                                "from {}; running without frame generation",
                                requested_multiplier, Joined(fg_dirs));
                }
            } else {
                LOG_INFO(Render_Vulkan, "DLSS-G: off (Graphics > DLSS Frame Generation)");
            }
            return true;
        }
    }
    LOG_WARNING(Render_Vulkan,
                "NVIDIA DLSS (native NGX): runtime missing from {}; keeping fallback",
                Joined(dirs));
    return false;
}
bool AttachDevice(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                  u32, vk::DeviceDispatch& dispatch) {
    const bool barrier_fix = NgxBarrierFixEnabled();
    ngx_get_device_proc = dispatch.vkGetDeviceProcAddr;
    const bool ready = Initialize(instance, physical, device, dispatch.vkGetInstanceProcAddr,
                                  barrier_fix ? &NgxGetDeviceProcAddr
                                              : dispatch.vkGetDeviceProcAddr);
    LOG_INFO(Render_Vulkan, "NVIDIA DLSS (native NGX): image-clear barrier fix {}",
             barrier_fix ? "on" : "off (EDEN_NX2_NGX_BARRIER_FIX=0)");
    LOG_INFO(Render_Vulkan, "NVIDIA DLSS (native NGX): device {} (result {:#x})",
             ready ? "ready" : "unavailable", LastResult());
    if (ready && FrameGenRequested() > 1) {
        if (IsFrameGenAvailable()) {
            LOG_INFO(Render_Vulkan, "DLSS-G: device ready, {}x asked for, running at {}x",
                     FrameGenRequested(), FrameGenMultiplier());
        } else {
            LOG_WARNING(Render_Vulkan, "DLSS-G: unavailable on this device; DLSS runs without it");
        }
    }
    return ready;
}
} // namespace Vulkan::Ngx
