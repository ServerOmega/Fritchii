// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/vulkan_common/ngx.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan::Ngx {
namespace {
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
    const bool ready = Initialize(instance, physical, device, dispatch.vkGetInstanceProcAddr,
                                  dispatch.vkGetDeviceProcAddr);
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
