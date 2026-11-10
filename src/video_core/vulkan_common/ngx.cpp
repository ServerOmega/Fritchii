// SPDX-License-Identifier: GPL-3.0-or-later
// Native Linux NGX backend behind the renderer's existing Streamline lifecycle interface.
#include <algorithm>
#include <cstdlib>
#include <dlfcn.h>
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/vulkan_common/ngx_api.h"
#include "video_core/vulkan_common/streamline.h"

namespace Vulkan::Ngx {
namespace {
Api api;
void* module{};
bool started{}, attached{};
VkDevice attached_device{};
std::vector<std::string> instance_extensions, device_extensions;
std::vector<std::wstring> search_paths;
std::vector<const wchar_t*> path_ptrs;
std::wstring data_path;
NVSDK_NGX_FeatureCommonInfo common_info{};
NVSDK_NGX_FeatureDiscoveryInfo discovery{};
// Same custom-engine identifier as the existing Windows backend. No borrowed NVIDIA App ID.
constexpr char PROJECT_ID[] = "8d3e2f9a-6b41-4c7e-9a52-1f0c3b7d5e64";
void LogNgx(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    LOG_INFO(Render_Vulkan, "NGX: {}", message ? message : "");
}
template<class T> bool Resolve(T& function, const char* name) {
    function = reinterpret_cast<T>(dlsym(module, name));
    if (!function) LOG_WARNING(Render_Vulkan, "Linux NGX missing public SDK symbol {}; using FSR 3.1", name);
    return function != nullptr;
}
bool Load(const std::filesystem::path& path) {
    if (module) return true;
    module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!module) {
        LOG_WARNING(Render_Vulkan, "Linux NGX SDK loader unavailable ({}): {}; using FSR 3.1", path.string(), dlerror());
        return false;
    }
    // New SDKs can query just DLSS SR rather than every NGX feature. Older loaders use the
    // deprecated RequiredExtensions API, normalizing its obsolete EXT address extension below.
    api.instance_requirements = reinterpret_cast<decltype(api.instance_requirements)>(dlsym(module, "NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements"));
    api.device_requirements = reinterpret_cast<decltype(api.device_requirements)>(dlsym(module, "NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements"));
    const bool ok = Resolve(api.extensions, "NVSDK_NGX_VULKAN_RequiredExtensions") &&
        Resolve(api.init, "NVSDK_NGX_VULKAN_Init_with_ProjectID") &&
        Resolve(api.shutdown, "NVSDK_NGX_VULKAN_Shutdown1") &&
        Resolve(api.capabilities, "NVSDK_NGX_VULKAN_GetCapabilityParameters") &&
        Resolve(api.allocate, "NVSDK_NGX_VULKAN_AllocateParameters") &&
        Resolve(api.destroy, "NVSDK_NGX_VULKAN_DestroyParameters") &&
        Resolve(api.create, "NVSDK_NGX_VULKAN_CreateFeature") &&
        Resolve(api.release, "NVSDK_NGX_VULKAN_ReleaseFeature") &&
        Resolve(api.evaluate, "NVSDK_NGX_VULKAN_EvaluateFeature_C");
    if (!ok) { dlclose(module); module = nullptr; api = {}; }
    return ok;
}
}
const Api& GetApi() { return api; }
} // namespace Vulkan::Ngx

namespace Vulkan::Streamline {
using namespace Ngx;
std::filesystem::path UserFolder() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::StreamlineDir).parent_path() / "ngx";
}
DlssFiles FindDlssFiles() {
    DlssFiles files;
    files.folder = UserFolder();
    const char* sdk = std::getenv("EDEN_NGX_SDK_LIBRARY");
    auto loader = sdk && *sdk ? std::filesystem::path{sdk} : files.folder / "libnvsdk_ngx.so";
    std::error_code ec;
    if (std::filesystem::is_regular_file(loader, ec)) files.interposer = loader;
    std::vector<std::filesystem::path> dirs{files.folder, "/usr/lib", "/usr/lib/x86_64-linux-gnu"};
    if (const char* dir = std::getenv("EDEN_NGX_DLSS_DIR"); dir && *dir) dirs.insert(dirs.begin(), dir);
    for (const auto& dir : dirs) {
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.path().filename().string().starts_with("libnvidia-ngx-dlss.so") &&
                entry.is_regular_file(ec)) {
                if (files.model.empty() || entry.path().filename() > files.model.filename()) files.model = entry.path();
            }
        }
        if (!files.model.empty()) break;
        ec.clear();
    }
    return files;
}
// Authenticode is Windows-only; Linux never calls these for native libraries.
bool IsStreamlineModule(const std::filesystem::path&) { return false; }
bool IsSignedByNvidia(const std::filesystem::path&) { return false; }

bool Start() {
    if (started) return true;
    if (!Settings::values.nvidia_dlss.GetValue() && !std::getenv("EDEN_NGX_PROBE")) return false;
    const auto files = FindDlssFiles();
    const char* sdk = std::getenv("EDEN_NGX_SDK_LIBRARY");
    const auto loader = sdk && *sdk ? std::filesystem::path{sdk} : files.folder / "libnvsdk_ngx.so";
    if (!Load(loader)) return false;
    std::error_code ec;
    std::filesystem::create_directories(UserFolder() / "logs", ec);
    if (ec) { LOG_WARNING(Render_Vulkan, "Linux NGX log directory unavailable; using FSR 3.1"); return false; }
    data_path = (UserFolder() / "logs").wstring();
    search_paths.clear(); path_ptrs.clear();
    if (const char* dir = std::getenv("EDEN_NGX_DLSS_DIR"); dir && *dir) search_paths.emplace_back(std::filesystem::path{dir}.wstring());
    search_paths.emplace_back(files.folder.wstring());
    if (!files.model.empty()) search_paths.emplace_back(files.model.parent_path().wstring());
    for (const auto& path : search_paths) path_ptrs.push_back(path.c_str());
    common_info = {};
    common_info.PathListInfo = {path_ptrs.data(), static_cast<unsigned>(path_ptrs.size())};
    common_info.LoggingInfo = {LogNgx, NVSDK_NGX_LOGGING_LEVEL_ON, false};
    discovery = {};
    discovery.SDKVersion = NVSDK_NGX_Version_API;
    discovery.FeatureID = NVSDK_NGX_Feature_SuperSampling;
    discovery.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    discovery.Identifier.v.ProjectDesc = {PROJECT_ID, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "Fritchii-5.6-linux-ngx"};
    discovery.ApplicationDataPath = data_path.c_str(); discovery.FeatureInfo = &common_info;
    unsigned ni{}, nd{};
    const char **ie{}, **de{};
    const auto result = api.extensions(&ni, &ie, &nd, &de);
    if (NVSDK_NGX_FAILED(result) || (ni && !ie) || (nd && !de)) {
        LOG_WARNING(Render_Vulkan, "Linux NGX required-extension query failed ({:#x}); using FSR 3.1", static_cast<unsigned>(result));
        return false;
    }
    instance_extensions.clear(); device_extensions.clear();
    for (unsigned i = 0; i < ni; ++i) instance_extensions.emplace_back(ie[i]);
    for (unsigned i = 0; i < nd; ++i) device_extensions.emplace_back(de[i]);
    if (api.instance_requirements && api.device_requirements) {
        VkExtensionProperties* required{}; unsigned count{};
        const auto query = api.instance_requirements(&discovery, &count, &required);
        if (NVSDK_NGX_FAILED(query) || (count && !required)) {
            LOG_WARNING(Render_Vulkan, "Linux NGX DLSS instance requirements failed ({:#x}); using FSR 3.1", static_cast<unsigned>(query));
            return false;
        }
        instance_extensions.clear();
        for (unsigned i = 0; i < count; ++i) instance_extensions.emplace_back(required[i].extensionName);
    }
    // RequiredExtensions is deprecated and advertises EXT BDA. The renderer uses its
    // promoted KHR feature structure; the mutually exclusive EXT extension must not be enabled.
    for (auto& name : device_extensions)
        if (name == VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) name = VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME;
    started = true;
    LOG_INFO(Render_Vulkan, "Linux NGX SDK loaded from {}; model {}; awaiting NVIDIA Vulkan device", loader.string(),
        files.model.empty() ? "driver/default search" : files.model.string());
    return true;
}
bool WantsDevice() { return started && !attached; }
std::vector<std::string> InstanceExtensions() { return started ? instance_extensions : std::vector<std::string>{}; }
std::vector<std::string> DeviceExtensions() { return started ? device_extensions : std::vector<std::string>{}; }

bool PrepareDeviceExtensions(VkInstance instance, VkPhysicalDevice physical) {
    if (!started) return false;
    if (!api.device_requirements || !api.instance_requirements) return true;
    VkExtensionProperties* required{}; unsigned count{};
    const auto query = api.device_requirements(instance, physical, &discovery, &count, &required);
    if (NVSDK_NGX_FAILED(query) || (count && !required)) {
        LOG_WARNING(Render_Vulkan, "Linux NGX DLSS device requirements failed ({:#x}); using FSR 3.1", static_cast<unsigned>(query));
        return false;
    }
    device_extensions.clear();
    for (unsigned i = 0; i < count; ++i) {
        std::string name = required[i].extensionName;
        if (name == VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) name = VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME;
        if (std::ranges::find(device_extensions, name) == device_extensions.end()) device_extensions.push_back(name);
    }
    return true;
}

bool AttachDevice(VkInstance instance, VkPhysicalDevice physical, VkDevice device, u32, vk::DeviceDispatch& dld) {
    if (!WantsDevice()) return false;
    const auto properties_fn = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(dld.vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties"));
    VkPhysicalDeviceProperties properties{};
    if (properties_fn) properties_fn(physical, &properties);
    if (properties.vendorID != 0x10de) {
        LOG_WARNING(Render_Vulkan, "Linux NGX requires an NVIDIA GPU; using FSR 3.1");
        Stop(); return false;
    }
    const auto result = api.init(PROJECT_ID, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "Fritchii-5.6-linux-ngx", data_path.c_str(),
        instance, physical, device, dld.vkGetInstanceProcAddr, dld.vkGetDeviceProcAddr, &common_info, NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(result)) {
        LOG_WARNING(Render_Vulkan, "Linux NGX Init_with_ProjectID failed ({:#x}); using FSR 3.1", static_cast<unsigned>(result));
        Stop(); return false;
    }
    attached_device = device; // Shutdown also when capability discovery fails.
    NVSDK_NGX_Parameter* capabilities{};
    const auto cap_result = api.capabilities(&capabilities);
    int available{};
    if (NVSDK_NGX_SUCCEED(cap_result) && capabilities)
        capabilities->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    if (capabilities) api.destroy(capabilities);
    if (!available) {
        LOG_WARNING(Render_Vulkan, "Linux NGX DLSS Super Resolution unavailable (capabilities {:#x}); using FSR 3.1", static_cast<unsigned>(cap_result));
        Stop(); return false;
    }
    attached = true;
    LOG_INFO(Render_Vulkan, "NVIDIA DLSS available through Linux NGX (custom project {})", PROJECT_ID);
    return true;
}
bool IsDlssAvailable() { return attached; }
void Stop() {
    if (attached_device) api.shutdown(attached_device);
    attached_device = VK_NULL_HANDLE; attached = false; started = false;
    // Keep the SDK module mapped: its worker/thread-local destructors may outlive a renderer.
}
} // namespace Vulkan::Streamline
