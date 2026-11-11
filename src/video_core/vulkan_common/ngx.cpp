// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/vulkan_common/ngx.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_dlssg_vk.h>
#include <nvsdk_ngx_helpers_vk.h>

#include "common/logging.h"
#include "common/scm_rev.h"

namespace Vulkan::Ngx {
namespace {
constexpr const char* PROJECT_ID = "8d3e2f9a-6b41-4c7e-9a52-1f0c3b7d5e64";
constexpr std::string_view UNVERSIONED_RUNTIME = "libnvidia-ngx-dlss.so";
constexpr std::string_view VERSIONED_PREFIX = "libnvidia-ngx-dlss.so.";
// DLSS Frame Generation's feature library (DLSS SDK lib/Linux_x86_64/rel).
constexpr std::string_view FG_UNVERSIONED_RUNTIME = "libnvidia-ngx-dlssg.so";
constexpr std::string_view FG_VERSIONED_PREFIX = "libnvidia-ngx-dlssg.so.";
enum class Phase { Disabled, Prepared, Attached };
struct Runtime {
    Phase phase{};
    VkDevice device{};
    std::wstring path;
    std::wstring cache;
    std::string engine_version;
    const wchar_t* search_path{};
    NVSDK_NGX_FeatureCommonInfo info{};
    NVSDK_NGX_Result result{};
    // DLSS Frame Generation
    std::wstring fg_path;              ///< Directory NGX finds libnvidia-ngx-dlssg.so in
    std::array<const wchar_t*, 2> search_paths{};
    bool fg_staged{};                  ///< The library is staged and its extensions are wanted
    bool fg_available{};               ///< Supported on the attached device
    u32 fg_max_generated{1};           ///< NGX's MultiFrameCountMax (3 for 4x)
};
Runtime runtime;

// NGX PathListInfo is a list of directories. NVIDIA's driver/NGX Core loads the
// unversioned feature library name libnvidia-ngx-dlss.so from those directories
// (see the DLSS SDK guide). A versioned SDK file such as
// libnvidia-ngx-dlss.so.310.9.16 is accepted by staging a symlink to that
// unversioned name in the writable cache directory, so the source folder can
// stay read-only and no user-created symlink is required.
std::filesystem::path FindRuntimeFile(const std::filesystem::path& dir,
                                      std::string_view unversioned_name = UNVERSIONED_RUNTIME,
                                      std::string_view versioned_prefix = VERSIONED_PREFIX) {
    std::error_code error;
    if (dir.empty()) {
        return {};
    }
    const auto unversioned = dir / unversioned_name;
    if (std::filesystem::is_regular_file(unversioned, error)) {
        return unversioned;
    }
    if (!std::filesystem::is_directory(dir, error)) {
        return {};
    }
    std::filesystem::path found;
    for (const auto& entry : std::filesystem::directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, error)) {
        std::error_code file_error;
        const auto name = entry.path().filename().string();
        if (name.starts_with(versioned_prefix) && name.size() > versioned_prefix.size() &&
            entry.is_regular_file(file_error)) {
            if (found.empty() || name > found.filename().string()) {
                found = entry.path();
            }
        }
    }
    return found;
}

std::filesystem::path SearchDirectory(const std::filesystem::path& source,
                                      const std::filesystem::path& file,
                                      const std::filesystem::path& cache) {
    if (file.filename() == UNVERSIONED_RUNTIME) {
        return source;
    }
    std::error_code error;
    const auto link = cache / UNVERSIONED_RUNTIME;
    const auto status = std::filesystem::symlink_status(link, error);
    if (status.type() == std::filesystem::file_type::symlink) {
        std::filesystem::remove(link, error);
        if (error) {
            return {};
        }
    } else if (status.type() != std::filesystem::file_type::not_found) {
        return {};
    }
    std::filesystem::create_symlink(std::filesystem::absolute(file), link, error);
    if (error) {
        return {};
    }
    return cache;
}

void SetEngineVersion() {
    if (!runtime.engine_version.empty()) {
        return;
    }
    const std::string_view name = Common::g_build_name;
    const std::string_view version = Common::g_build_version;
    // Use the real build strings when they are present. A bare "Fritchii" is the
    // fallback so this never embeds a stale version number.
    const std::string engine_name = name.empty() ? "Fritchii" : std::string(name);
    if (version.empty()) {
        runtime.engine_version = engine_name;
        return;
    }
    runtime.engine_version = version.front() == 'v' || version.front() == 'V'
                                 ? engine_name + "-" + std::string(version)
                                 : engine_name + "-v" + std::string(version);
}

// Stages a symlink `cache/name` to `file` (replacing an older symlink). False on any error.
bool StageLink(const std::filesystem::path& cache, std::string_view name,
               const std::filesystem::path& file) {
    std::error_code error;
    const auto link = cache / name;
    const auto status = std::filesystem::symlink_status(link, error);
    if (status.type() == std::filesystem::file_type::symlink) {
        std::filesystem::remove(link, error);
        if (error) {
            return false;
        }
    } else if (status.type() != std::filesystem::file_type::not_found) {
        // A real file the player put there: NGX can load it as it is.
        return status.type() == std::filesystem::file_type::regular;
    }
    std::filesystem::create_symlink(std::filesystem::absolute(file), link, error);
    return !error;
}

void NgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature feature) {
    std::string_view text{message ? message : ""};
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    LOG_INFO(Render_Vulkan, "NGX [{}]: {}", static_cast<int>(feature), text);
}

NVSDK_NGX_FeatureDiscoveryInfo Discovery(NVSDK_NGX_Feature feature = NVSDK_NGX_Feature_SuperSampling) {
    NVSDK_NGX_FeatureDiscoveryInfo discovery{};
    discovery.SDKVersion = NVSDK_NGX_Version_API;
    discovery.FeatureID = feature;
    discovery.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    discovery.Identifier.v.ProjectDesc.ProjectId = PROJECT_ID;
    discovery.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    discovery.Identifier.v.ProjectDesc.EngineVersion = runtime.engine_version.c_str();
    discovery.ApplicationDataPath = runtime.cache.c_str();
    discovery.FeatureInfo = &runtime.info;
    return discovery;
}

std::vector<std::string> Names(NVSDK_NGX_Result result, u32 count,
                               const VkExtensionProperties* properties) {
    runtime.result = result;
    std::vector<std::string> names;
    if (NVSDK_NGX_FAILED(result)) {
        runtime.phase = Phase::Disabled;
        return names;
    }
    for (u32 i = 0; i < count; ++i) {
        names.emplace_back(properties[i].extensionName);
    }
    return names;
}

bool EnvEnabled(const char* name) {
    const char* value = std::getenv(name);
    return !value || std::atoi(value) != 0;
}

struct Configuration {
    u32 width, height, out_width, out_height;
    int flags;
    NVSDK_NGX_PerfQuality_Value quality;
    u32 preset;
    bool operator==(const Configuration&) const = default;
};
NVSDK_NGX_PerfQuality_Value PerfQualityFor(Settings::NvidiaDlssQuality mode) {
    using Quality = Settings::NvidiaDlssQuality;
    switch (mode) {
    case Quality::Native:
        return NVSDK_NGX_PerfQuality_Value_DLAA;
    case Quality::Balanced:
        return NVSDK_NGX_PerfQuality_Value_Balanced;
    case Quality::Performance:
        return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    case Quality::UltraPerformance:
        return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    case Quality::Quality:
    case Quality::Automatic:
    default:
        return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    }
}

Configuration Config(const NvidiaDlssInputs& in, Settings::NvidiaDlssQuality mode) {
    const auto quality = PerfQualityFor(mode);
    int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    if (in.depth_inverted) flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    if (in.hdr && EnvEnabled("EDEN_NX2_DLSS_HDR")) flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    if (in.hdr && EnvEnabled("EDEN_NX2_DLSS_AUTO_EXPOSURE"))
        flags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    u32 preset = in.preset >= 'J' && in.preset <= 'M'
                     ? NVSDK_NGX_DLSS_Hint_Render_Preset_J + static_cast<u32>(in.preset - 'J')
                     : NVSDK_NGX_DLSS_Hint_Render_Preset_Default;
    if (const char* value = std::getenv("EDEN_NX2_DLSS_PRESET")) {
        if (value[0] >= 'J' && value[0] <= 'M') {
            preset = NVSDK_NGX_DLSS_Hint_Render_Preset_J + static_cast<u32>(value[0] - 'J');
        } else if (std::string_view{value} == "default") {
            preset = NVSDK_NGX_DLSS_Hint_Render_Preset_Default;
        }
    }
    return {in.render_size.width, in.render_size.height, in.upscale_size.width,
            in.upscale_size.height, flags, quality, preset};
}

NVSDK_NGX_Resource_VK Resource(const NvidiaDlssImage& image, VkExtent2D extent, bool write) {
    return NVSDK_NGX_Create_ImageView_Resource_VK(image.view, image.image, image.range,
                                                image.format, extent.width, extent.height, write);
}
} // namespace

bool Prepare(const std::filesystem::path& path, const std::filesystem::path& cache) {
    if (runtime.phase == Phase::Attached) return true;
    runtime.phase = Phase::Disabled;
    const auto file = FindRuntimeFile(path);
    if (file.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(cache, error);
    if (error) return false;
    const auto search = SearchDirectory(path, file, cache);
    if (search.empty()) return false;
    SetEngineVersion();
    runtime.path = std::filesystem::absolute(search).wstring();
    runtime.cache = std::filesystem::absolute(cache).wstring();
    runtime.search_path = runtime.path.c_str();
    runtime.info = {};
    runtime.info.PathListInfo = {&runtime.search_path, 1};
    runtime.fg_path.clear();
    runtime.fg_staged = false;
    runtime.fg_available = false;
    runtime.fg_max_generated = 1;
    // EDEN_NGX_LOG=1|2: NGX's own log lines (normal or verbose) in Fritchii's log.
    if (const char* level = std::getenv("EDEN_NGX_LOG"); level && level[0] >= '1' && level[0] <= '2') {
        runtime.info.LoggingInfo.LoggingCallback = NgxLog;
        runtime.info.LoggingInfo.MinimumLoggingLevel =
            level[0] == '2' ? NVSDK_NGX_LOGGING_LEVEL_VERBOSE : NVSDK_NGX_LOGGING_LEVEL_ON;
        runtime.info.LoggingInfo.DisableOtherLoggingSinks = false;
    }
    runtime.phase = Phase::Prepared;
    return true;
}

bool PrepareFrameGen(const std::filesystem::path& dir, const std::filesystem::path& cache) {
    if (runtime.phase != Phase::Prepared) return false;
    const auto file = FindRuntimeFile(dir, FG_UNVERSIONED_RUNTIME, FG_VERSIONED_PREFIX);
    if (file.empty()) return false;
    std::filesystem::path search = dir;
    if (file.filename() != FG_UNVERSIONED_RUNTIME) {
        // Like the DLSS library: NGX gets the unversioned name (and the versioned one) in the
        // writable cache, as links to the player's file.
        if (!StageLink(cache, FG_UNVERSIONED_RUNTIME, file) ||
            !StageLink(cache, file.filename().string(), file)) {
            LOG_WARNING(Render_Vulkan, "DLSS-G: could not stage {} in {}", file.string(),
                        cache.string());
            return false;
        }
        search = cache;
    }
    runtime.fg_path = std::filesystem::absolute(search).wstring();
    runtime.search_paths = {runtime.path.c_str(), runtime.fg_path.c_str()};
    const unsigned count = runtime.fg_path == runtime.path ? 1u : 2u;
    runtime.info.PathListInfo = {runtime.search_paths.data(), count};
    runtime.fg_staged = true;
    LOG_INFO(Render_Vulkan, "DLSS-G: runtime found: {} (NGX search path{} {}{}{})",
             file.string(), count == 2 ? "s" : "", std::filesystem::path(runtime.path).string(),
             count == 2 ? ", " : "", count == 2 ? std::filesystem::path(runtime.fg_path).string() : "");
    return true;
}

void DropFrameGen(const char* reason) {
    if (runtime.fg_staged || runtime.fg_available) {
        LOG_WARNING(Render_Vulkan, "DLSS-G: off for this session: {}", reason);
    }
    runtime.fg_staged = false;
    runtime.fg_available = false;
}

bool IsFrameGenAvailable() {
    return runtime.phase == Phase::Attached && runtime.fg_available;
}

u32 FrameGenMultiplier() {
    if (!IsFrameGenAvailable()) return 1;
    const u32 max_multiplier = std::max<u32>(runtime.fg_max_generated, 1) + 1;
    return std::clamp<u32>(FrameGenRequested(), 1, max_multiplier);
}

std::vector<std::string> FrameGenDeviceExtensions(VkInstance instance, VkPhysicalDevice physical) {
    if (!WantsDevice() || !runtime.fg_staged) return {};
    const auto discovery = Discovery(NVSDK_NGX_Feature_FrameGeneration);
    NVSDK_NGX_FeatureRequirement requirement{};
    const NVSDK_NGX_Result supported =
        NVSDK_NGX_VULKAN_GetFeatureRequirements(instance, physical, &discovery, &requirement);
    LOG_INFO(Render_Vulkan,
             "DLSS-G: feature requirements query {:#x}: support bits {:#x} (0 = supported), "
             "minimum GPU architecture {:#x}, minimum OS '{}'",
             static_cast<u32>(supported), static_cast<u32>(requirement.FeatureSupported),
             requirement.MinHWArchitecture, requirement.MinOSVersion);
    if (NVSDK_NGX_SUCCEED(supported) &&
        requirement.FeatureSupported != NVSDK_NGX_FeatureSupportResult_Supported) {
        DropFrameGen("NGX reports DLSS-G unsupported on this GPU or driver");
        return {};
    }
    u32 count{};
    VkExtensionProperties* properties{};
    const auto result = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
        instance, physical, &discovery, &count, &properties);
    if (NVSDK_NGX_FAILED(result)) {
        LOG_WARNING(Render_Vulkan, "DLSS-G: device extension query failed ({:#x})",
                    static_cast<u32>(result));
        DropFrameGen("no device extension list");
        return {};
    }
    std::vector<std::string> names;
    for (u32 i = 0; i < count; ++i) {
        names.emplace_back(properties[i].extensionName);
    }
    return names;
}

std::optional<VideoCommon::DlssMode::Range> OptimalRange(Settings::NvidiaDlssQuality mode,
                                                         VkExtent2D output) {
    if (!IsDlssAvailable() || mode == Settings::NvidiaDlssQuality::Automatic) return std::nullopt;
    // The optimal-settings callback lives in the capability parameters, not in allocated ones.
    NVSDK_NGX_Parameter* parameters{};
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetCapabilityParameters(&parameters)) || !parameters) {
        return std::nullopt;
    }
    u32 optimal_width{}, optimal_height{}, max_width{}, max_height{}, min_width{}, min_height{};
    float sharpness{};
    const NVSDK_NGX_Result result = NGX_DLSS_GET_OPTIMAL_SETTINGS(
        parameters, output.width, output.height, PerfQualityFor(mode), &optimal_width,
        &optimal_height, &max_width, &max_height, &min_width, &min_height, &sharpness);
    NVSDK_NGX_VULKAN_DestroyParameters(parameters);
    // A mode the runtime doesn't offer reports a zero size.
    if (NVSDK_NGX_FAILED(result) || !optimal_width || !optimal_height || !max_width ||
        !max_height) {
        return std::nullopt;
    }
    return VideoCommon::DlssMode::Range{{min_width, min_height}, {max_width, max_height}};
}

bool WantsDevice() { return runtime.phase == Phase::Prepared; }
bool IsDlssAvailable() { return runtime.phase == Phase::Attached; }
u32 LastResult() { return static_cast<u32>(runtime.result); }

std::vector<std::string> InstanceExtensions() {
    if (!WantsDevice()) return {};
    const auto discovery = Discovery();
    u32 count{};
    VkExtensionProperties* properties{};
    const auto result = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(
        &discovery, &count, &properties);
    auto names = Names(result, count, properties);
    if (runtime.phase == Phase::Prepared && runtime.fg_staged) {
        // DLSS-G's instance extensions; a failed query only turns frame generation off.
        const auto fg_discovery = Discovery(NVSDK_NGX_Feature_FrameGeneration);
        u32 fg_count{};
        VkExtensionProperties* fg_properties{};
        const auto fg_result = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(
            &fg_discovery, &fg_count, &fg_properties);
        if (NVSDK_NGX_FAILED(fg_result)) {
            LOG_WARNING(Render_Vulkan, "DLSS-G: instance extension query failed ({:#x})",
                        static_cast<u32>(fg_result));
            DropFrameGen("no instance extension list");
        } else {
            std::string listed;
            for (u32 i = 0; i < fg_count; ++i) {
                std::string name{fg_properties[i].extensionName};
                listed += (listed.empty() ? "" : ", ") + name;
                if (std::find(names.begin(), names.end(), name) == names.end()) {
                    names.push_back(std::move(name));
                }
            }
            LOG_INFO(Render_Vulkan, "DLSS-G: instance extensions: {}",
                     listed.empty() ? "none" : listed);
        }
    }
    return names;
}
std::vector<std::string> DeviceExtensions(VkInstance instance, VkPhysicalDevice physical) {
    if (!WantsDevice()) return {};
    const auto discovery = Discovery();
    u32 count{};
    VkExtensionProperties* properties{};
    const auto result = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
        instance, physical, &discovery, &count, &properties);
    return Names(result, count, properties);
}

namespace {
// After NGX attached the device: whether DLSS-G is available, and how many frames it generates.
void ProbeFrameGen() {
    runtime.fg_available = false;
    NVSDK_NGX_Parameter* capabilities{};
    const NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&capabilities);
    if (NVSDK_NGX_FAILED(result) || !capabilities) {
        LOG_WARNING(Render_Vulkan, "DLSS-G: no capability parameters ({:#x})",
                    static_cast<u32>(result));
        DropFrameGen("no capability parameters");
        return;
    }
    int available{};
    int init_result{};
    int needs_driver{};
    unsigned int driver_major{};
    unsigned int driver_minor{};
    unsigned int max_generated{};
    const NVSDK_NGX_Result available_result =
        capabilities->Get(NVSDK_NGX_Parameter_FrameGeneration_Available, &available);
    capabilities->Get(NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult, &init_result);
    capabilities->Get(NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver, &needs_driver);
    capabilities->Get(NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMajor, &driver_major);
    capabilities->Get(NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMinor, &driver_minor);
    const NVSDK_NGX_Result max_result =
        capabilities->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &max_generated);
    NVSDK_NGX_VULKAN_DestroyParameters(capabilities);
    runtime.fg_max_generated = NVSDK_NGX_SUCCEED(max_result) && max_generated > 1 ? max_generated : 1;
    LOG_INFO(Render_Vulkan,
             "DLSS-G: capability: available {} (query {:#x}), init result {:#x}, needs newer "
             "driver {} (minimum {}.{}), MultiFrameCountMax {} (query {:#x}) -> up to {}x",
             available, static_cast<u32>(available_result), static_cast<u32>(init_result),
             needs_driver, driver_major, driver_minor, max_generated,
             static_cast<u32>(max_result), runtime.fg_max_generated + 1);
    if (NVSDK_NGX_FAILED(available_result) || !available) {
        DropFrameGen(needs_driver ? "NVIDIA driver too old for DLSS-G"
                                  : "NGX reports DLSS-G not available");
        return;
    }
    runtime.fg_available = true;
}
} // namespace

bool Initialize(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                PFN_vkGetInstanceProcAddr get_instance, PFN_vkGetDeviceProcAddr get_device) {
    if (!WantsDevice()) return false;
    runtime.result = NVSDK_NGX_VULKAN_Init_with_ProjectID(
        PROJECT_ID, NVSDK_NGX_ENGINE_TYPE_CUSTOM, runtime.engine_version.c_str(),
        runtime.cache.c_str(), instance, physical, device, get_instance, get_device,
        &runtime.info);
    if (NVSDK_NGX_FAILED(runtime.result)) {
        runtime.phase = Phase::Disabled;
        return false;
    }
    runtime.device = device;
    runtime.phase = Phase::Attached;
    NVSDK_NGX_Parameter* capabilities{};
    runtime.result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&capabilities);
    int available{};
    if (NVSDK_NGX_SUCCEED(runtime.result) && capabilities) {
        runtime.result = capabilities->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
        NVSDK_NGX_VULKAN_DestroyParameters(capabilities);
    }
    if (NVSDK_NGX_FAILED(runtime.result) || !available) {
        Stop();
        return false;
    }
    if (runtime.fg_staged) {
        ProbeFrameGen();
    }
    return true;
}

void Stop() {
    if (runtime.phase == Phase::Attached) NVSDK_NGX_VULKAN_Shutdown1(runtime.device);
    runtime.phase = Phase::Disabled;
    runtime.device = VK_NULL_HANDLE;
    runtime.fg_available = false;
    runtime.fg_staged = false;
}

struct Feature::State {
    NVSDK_NGX_Handle* handle{};
    NVSDK_NGX_Parameter* parameters{};
    std::optional<Configuration> config;
    NVSDK_NGX_Result result{};
    void Release() {
        if (handle) NVSDK_NGX_VULKAN_ReleaseFeature(handle);
        if (parameters) NVSDK_NGX_VULKAN_DestroyParameters(parameters);
        handle = nullptr;
        parameters = nullptr;
        config.reset();
    }
};
Feature::Feature() : state{std::make_unique<State>()} {}
Feature::~Feature() { state->Release(); }
u32 Feature::LastResult() const { return static_cast<u32>(state->result); }
bool Feature::NeedsRecreate(const NvidiaDlssInputs& inputs,
                            Settings::NvidiaDlssQuality mode) const {
    return !state->config || *state->config != Config(inputs, mode);
}

bool Feature::Evaluate(VkCommandBuffer command, const NvidiaDlssInputs& in,
                       Settings::NvidiaDlssQuality mode, const NvidiaDlssImage& output) {
    if (!IsDlssAvailable() || !in.render_size.width || !in.render_size.height ||
        !in.upscale_size.width || !in.upscale_size.height) return false;
    const Configuration config = Config(in, mode);
    const bool recreate = NeedsRecreate(in, mode);
    if (recreate) {
        state->Release();
        state->result = NVSDK_NGX_VULKAN_AllocateParameters(&state->parameters);
        if (NVSDK_NGX_FAILED(state->result)) return false;
        for (const char* key : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                               NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                               NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                               NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                               NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
                               NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality}) {
            state->parameters->Set(key, config.preset);
        }
        NVSDK_NGX_DLSS_Create_Params create{};
        create.Feature = {config.width, config.height, config.out_width, config.out_height,
                          config.quality};
        create.InFeatureCreateFlags = config.flags;
        state->result = NGX_VULKAN_CREATE_DLSS_EXT1(runtime.device, command, 1, 1,
                                                   &state->handle, state->parameters, &create);
        if (NVSDK_NGX_FAILED(state->result)) return false;
        state->config = config;
    }
    auto color = Resource(in.color, in.render_size, false);
    auto depth = Resource(in.depth, in.render_size, false);
    auto motion = Resource(in.motion_vectors, in.render_size, false);
    auto target = Resource(output, in.upscale_size, true);
    NVSDK_NGX_VK_DLSS_Eval_Params evaluate{};
    evaluate.Feature.pInColor = &color;
    evaluate.Feature.pInOutput = &target;
    evaluate.pInDepth = &depth;
    evaluate.pInMotionVectors = &motion;
    evaluate.InJitterOffsetX = in.jitter[0];
    evaluate.InJitterOffsetY = in.jitter[1];
    evaluate.InMVScaleX = in.motion_vector_scale[0];
    evaluate.InMVScaleY = in.motion_vector_scale[1];
    evaluate.InRenderSubrectDimensions = {config.width, config.height};
    evaluate.InReset = in.reset || recreate;
    evaluate.InPreExposure = 1.0f;
    evaluate.InExposureScale = 1.0f;
    state->result = NGX_VULKAN_EVALUATE_DLSS_EXT(command, state->handle, state->parameters, &evaluate);
    return NVSDK_NGX_SUCCEED(state->result);
}
namespace {
struct FrameGenConfig {
    u32 width, height, format, render_width, render_height;
    bool operator==(const FrameGenConfig&) const = default;
};

// EDEN_DLSSG_MVEC_SCALE=pixels: hand DLSS-G the motion scale in render pixels like DLSS super
// resolution gets it. Default: normalized ([-1, 1] across the render size), as
// nvsdk_ngx_params_dlssg.h documents mvecScale and Streamline passes it.
bool MotionInPixels() {
    static const bool pixels = [] {
        const char* value = std::getenv("EDEN_DLSSG_MVEC_SCALE");
        return value && std::string_view{value} == "pixels";
    }();
    return pixels;
}

// EDEN_DLSSG_SUBRECT=0: generate over the whole backbuffer instead of only the game image.
bool UseSubrect() {
    static const bool use = [] {
        const char* value = std::getenv("EDEN_DLSSG_SUBRECT");
        return !value || value[0] != '0';
    }();
    return use;
}

void Identity(float (&matrix)[4][4]) {
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            matrix[row][column] = row == column ? 1.0f : 0.0f;
        }
    }
}
} // namespace

struct FrameGenFeature::State {
    NVSDK_NGX_Handle* handle{};
    NVSDK_NGX_Parameter* parameters{};
    std::optional<FrameGenConfig> config;
    NVSDK_NGX_Result result{};
    bool just_created{};
    void Release() {
        if (handle) NVSDK_NGX_VULKAN_ReleaseFeature(handle);
        if (parameters) NVSDK_NGX_VULKAN_DestroyParameters(parameters);
        handle = nullptr;
        parameters = nullptr;
        config.reset();
    }
};
FrameGenFeature::FrameGenFeature() : state{std::make_unique<State>()} {}
FrameGenFeature::~FrameGenFeature() { state->Release(); }
u32 FrameGenFeature::LastResult() const { return static_cast<u32>(state->result); }
bool FrameGenFeature::JustCreated() const { return state->just_created; }
void FrameGenFeature::Release() { state->Release(); }

bool FrameGenFeature::Evaluate(VkCommandBuffer command, const FrameGenInputs& in) {
    state->just_created = false;
    if (!IsFrameGenAvailable() || !in.backbuffer_size.width || !in.backbuffer_size.height ||
        !in.render_size.width || !in.render_size.height) {
        return false;
    }
    const FrameGenConfig config{in.backbuffer_size.width, in.backbuffer_size.height,
                                static_cast<u32>(in.backbuffer.format), in.render_size.width,
                                in.render_size.height};
    if (!state->config || *state->config != config) {
        state->Release();
        state->result = NVSDK_NGX_VULKAN_AllocateParameters(&state->parameters);
        if (NVSDK_NGX_FAILED(state->result)) return false;
        NVSDK_NGX_DLSSG_Create_Params create{};
        create.Width = config.width;
        create.Height = config.height;
        create.NativeBackbufferFormat = config.format;
        create.RenderWidth = config.render_width;
        create.RenderHeight = config.render_height;
        create.DynamicResolutionScaling = false;
        state->result = NGX_VK_CREATE_DLSSG(command, 1, 1, &state->handle, state->parameters, &create);
        if (NVSDK_NGX_FAILED(state->result)) {
            state->handle = nullptr;
            return false;
        }
        state->config = config;
        state->just_created = true;
    }
    auto backbuffer = Resource(in.backbuffer, in.backbuffer_size, false);
    auto depth = Resource(in.depth, in.render_size, false);
    auto motion = Resource(in.motion_vectors, in.render_size, false);
    auto output = Resource(in.output, in.backbuffer_size, true);
    NVSDK_NGX_VK_DLSSG_Eval_Params evaluate{};
    evaluate.pBackbuffer = &backbuffer;
    evaluate.pDepth = &depth;
    evaluate.pMVecs = &motion;
    evaluate.pOutputInterpFrame = &output;

    NVSDK_NGX_DLSSG_Opt_Eval_Params options{};
    options.multiFrameCount = std::max<u32>(in.multi_frame_count, 1);
    options.multiFrameIndex = std::clamp<u32>(in.multi_frame_index, 1, options.multiFrameCount);
    // The emulator knows no camera: identity matrices, as the Streamline backend passes them.
    // Frame generation works from the motion vectors, depth and its own optical flow.
    Identity(options.cameraViewToClip);
    Identity(options.clipToCameraView);
    Identity(options.clipToLensClip);
    Identity(options.clipToPrevClip);
    Identity(options.prevClipToClip);
    options.jitterOffset[0] = in.jitter[0];
    options.jitterOffset[1] = in.jitter[1];
    const float rw = static_cast<float>(in.render_size.width);
    const float rh = static_cast<float>(in.render_size.height);
    if (MotionInPixels()) {
        options.mvecScale[0] = in.motion_vector_scale[0];
        options.mvecScale[1] = in.motion_vector_scale[1];
    } else {
        options.mvecScale[0] = in.motion_vector_scale[0] / rw;
        options.mvecScale[1] = in.motion_vector_scale[1] / rh;
    }
    options.cameraUp[1] = 1.0f;
    options.cameraRight[0] = 1.0f;
    options.cameraFwd[2] = 1.0f;
    options.cameraNear = 0.1f;
    options.cameraFar = 1000.0f;
    options.cameraFOV = 1.0f;
    options.cameraAspectRatio = rw / rh;
    options.colorBuffersHDR = false;
    options.depthInverted = in.depth_inverted;
    options.cameraMotionIncluded = true;
    options.reset = in.reset || state->just_created;
    options.mvecsSubrectBase = {0, 0};
    options.mvecsSubrectSize = {in.render_size.width, in.render_size.height};
    options.depthSubrectBase = {0, 0};
    options.depthSubrectSize = {in.render_size.width, in.render_size.height};
    const bool subrect = UseSubrect() && in.subrect_size.width && in.subrect_size.height;
    const NVSDK_NGX_Coordinates base{subrect ? static_cast<u32>(in.subrect_offset.x) : 0u,
                                     subrect ? static_cast<u32>(in.subrect_offset.y) : 0u};
    const NVSDK_NGX_Dimensions size{subrect ? in.subrect_size.width : in.backbuffer_size.width,
                                    subrect ? in.subrect_size.height : in.backbuffer_size.height};
    options.backbufferSubrectBase = base;
    options.backbufferSubrectSize = size;
    options.outputInterpSubrectBase = base;
    options.outputInterpSubrectSize = size;
    state->parameters->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID,
                           static_cast<unsigned long long>(in.frame_id));
    state->result =
        NGX_VK_EVALUATE_DLSSG(command, state->handle, state->parameters, &evaluate, &options);
    return NVSDK_NGX_SUCCEED(state->result);
}
} // namespace Vulkan::Ngx
