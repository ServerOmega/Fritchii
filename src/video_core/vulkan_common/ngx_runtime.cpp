// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/vulkan_common/ngx.h"

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
    return dirs;
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

bool Start() {
    if (!Settings::values.nvidia_dlss.GetValue()) {
        return false;
    }
    const auto cache = Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "ngx";
    const auto dirs = RuntimeDirectories();
    for (const auto& dir : dirs) {
        if (Prepare(dir, cache)) {
            LOG_INFO(Render_Vulkan, "NVIDIA DLSS (native NGX): runtime found in {}", dir.string());
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
    return ready;
}
} // namespace Vulkan::Ngx
