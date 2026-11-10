// SPDX-License-Identifier: GPL-3.0-or-later
// Linux DLSS SR through NVIDIA's public Vulkan NGX API; no Streamline/DLL dependency.
#include <array>
#include <cstdlib>
#include <string_view>
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_streamline_dlss.h"
#include "video_core/vulkan_common/ngx_api.h"
#include "video_core/vulkan_common/streamline.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {
namespace {
constexpr VkFormat OUTPUT_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkImageUsageFlags OUTPUT_USAGE = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
int EnvInt(const char* name, int fallback) {
    const char* value = std::getenv(name); return value ? std::atoi(value) : fallback;
}
NVSDK_NGX_PerfQuality_Value ModeFor(VkExtent2D render, VkExtent2D upscale) {
    const float ratio = static_cast<float>(render.width) / static_cast<float>(upscale.width);
    if (ratio >= 0.99f) return NVSDK_NGX_PerfQuality_Value_DLAA;
    if (ratio >= 0.65f) return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    if (ratio >= 0.57f) return NVSDK_NGX_PerfQuality_Value_Balanced;
    if (ratio >= 0.49f) return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
}
// EDEN_NX2_DLSS_PRESET overrides Graphics > DLSS preset (same as the Windows Streamline path).
unsigned Preset() {
    const char* value = std::getenv("EDEN_NX2_DLSS_PRESET");
    if (value && value[0] >= 'J' && value[0] <= 'M')
        return static_cast<unsigned>(NVSDK_NGX_DLSS_Hint_Render_Preset_J) + static_cast<unsigned>(value[0] - 'J');
    if (value && std::string_view{value} == "default") return NVSDK_NGX_DLSS_Hint_Render_Preset_Default;
    switch (Settings::values.nvidia_dlss_preset.GetValue()) {
    case Settings::DlssPreset::Default:
        return NVSDK_NGX_DLSS_Hint_Render_Preset_Default;
    case Settings::DlssPreset::J:
        return NVSDK_NGX_DLSS_Hint_Render_Preset_J;
    case Settings::DlssPreset::L:
        return NVSDK_NGX_DLSS_Hint_Render_Preset_L;
    case Settings::DlssPreset::M:
        return NVSDK_NGX_DLSS_Hint_Render_Preset_M;
    case Settings::DlssPreset::K:
    default:
        return NVSDK_NGX_DLSS_Hint_Render_Preset_K;
    }
}
VkImageMemoryBarrier Barrier(const StreamlineDlssUpscaler::Image& image, VkAccessFlags src_access,
                             VkAccessFlags dst_access, VkImageLayout old_layout,
                             VkImageLayout new_layout) {
    VkImageSubresourceRange range = image.range;
    range.aspectMask = image.barrier_aspect;
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image.image,
        .subresourceRange = range,
    };
}

NVSDK_NGX_Resource_VK Resource(const StreamlineDlssUpscaler::Image& in, VkExtent2D extent, bool write) {
    NVSDK_NGX_Resource_VK resource{};
    resource.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
    auto& info = resource.Resource.ImageViewInfo;
    info.Image = in.image; info.ImageView = in.view;
    info.Format = in.format; info.Width = extent.width; info.Height = extent.height;
    info.SubresourceRange = in.range;
    resource.ReadWrite = write;
    return resource;
}
} // namespace

StreamlineDlssUpscaler::StreamlineDlssUpscaler(const Device& device_, Scheduler& scheduler_,
                                             MemoryAllocator& memory_allocator_)
    : device{device_}, scheduler{scheduler_}, memory_allocator{memory_allocator_} {
    if (!Streamline::IsDlssAvailable()) throw vk::Exception(VK_ERROR_FEATURE_NOT_PRESENT);
    NVSDK_NGX_Parameter* params{};
    const auto result = Ngx::GetApi().allocate(&params);
    if (NVSDK_NGX_FAILED(result) || !params) {
        LOG_ERROR(Render_Vulkan, "Linux NGX parameter allocation failed ({:#x}); using FSR 3.1", static_cast<unsigned>(result));
        throw vk::Exception(VK_ERROR_FEATURE_NOT_PRESENT);
    }
    ngx_params = params;
    point_sampler = device.GetLogical().CreateSampler(VkSamplerCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .mipLodBias = 0.0f,
        .anisotropyEnable = VK_FALSE,
        .maxAnisotropy = 1.0f,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_NEVER,
        .minLod = 0.0f,
        .maxLod = VK_LOD_CLAMP_NONE,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    });
}

StreamlineDlssUpscaler::~StreamlineDlssUpscaler() {
    void(device.GetLogical().WaitIdle());
    const auto& api = Ngx::GetApi();
    if (ngx_handle) api.release(static_cast<NVSDK_NGX_Handle*>(ngx_handle));
    if (ngx_params) api.destroy(static_cast<NVSDK_NGX_Parameter*>(ngx_params));
}

void StreamlineDlssUpscaler::CreateOutput(VkExtent2D upscale_size) {
    if (output_image) {
        scheduler.Finish(); // the old output may still be in use
    }
    output_image = memory_allocator.CreateImage(VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = OUTPUT_FORMAT,
        .extent = {upscale_size.width, upscale_size.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = OUTPUT_USAGE,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
    output_view = device.GetLogical().CreateImageView(VkImageViewCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = *output_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = OUTPUT_FORMAT,
        .components{},
        .subresourceRange{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    });
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([image = *output_image](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                               VkImageMemoryBarrier{
                                   .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                   .pNext = nullptr,
                                   .srcAccessMask = 0,
                                   .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                   .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                   .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = image,
                                   .subresourceRange{
                                       .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                       .baseMipLevel = 0,
                                       .levelCount = 1,
                                       .baseArrayLayer = 0,
                                       .layerCount = 1,
                                   },
                               });
    });
}

bool StreamlineDlssUpscaler::Upscale(const Inputs& in) {
    if (failed) return false;
    if (!in.render_size.width || !in.render_size.height || !in.upscale_size.width || !in.upscale_size.height) {
        failed = true; return false;
    }
    int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    if (in.depth_inverted) flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    if (in.hdr && EnvInt("EDEN_NX2_DLSS_HDR", 1)) flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    if (in.hdr && EnvInt("EDEN_NX2_DLSS_AUTO_EXPOSURE", 1)) flags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    const bool changed = !output_image || in.render_size.width != render_extent.width ||
        in.render_size.height != render_extent.height || in.upscale_size.width != upscale_extent.width ||
        in.upscale_size.height != upscale_extent.height || flags != ngx_flags;
    if (changed) {
        // Feature release/recreation is legal only after the previous evaluations finish.
        scheduler.Finish();
        CreateOutput(in.upscale_size);
        render_extent = in.render_size; upscale_extent = in.upscale_size; ngx_flags = flags;
    }
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([this, in, changed, flags, output = *output_image, view = *output_view](vk::CommandBuffer cmdbuf) {
        if (failed) return;
        const auto& api = Ngx::GetApi();
        auto* params = static_cast<NVSDK_NGX_Parameter*>(ngx_params);
        auto* handle = static_cast<NVSDK_NGX_Handle*>(ngx_handle);
        NVSDK_NGX_Result result = NVSDK_NGX_Result_Success;
        const VkImageSubresourceRange output_range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        const Image out{view, output, OUTPUT_FORMAT, output_range, VK_IMAGE_ASPECT_COLOR_BIT};
        const auto clear_output = [&] {
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                Barrier(out, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL));
            cmdbuf.ClearColorImage(output, VK_IMAGE_LAYOUT_GENERAL, VkClearColorValue{}, output_range);
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                Barrier(out, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL));
        };
        if (changed) {
            if (handle) { api.release(handle); handle = nullptr; ngx_handle = nullptr; }
            params->Reset();
            params->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
            params->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
            params->Set(NVSDK_NGX_Parameter_Width, in.render_size.width);
            params->Set(NVSDK_NGX_Parameter_Height, in.render_size.height);
            params->Set(NVSDK_NGX_Parameter_OutWidth, in.upscale_size.width);
            params->Set(NVSDK_NGX_Parameter_OutHeight, in.upscale_size.height);
            params->Set(NVSDK_NGX_Parameter_PerfQualityValue, static_cast<int>(ModeFor(in.render_size, in.upscale_size)));
            params->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, flags);
            params->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
            for (const char* name : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
                NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality}) params->Set(name, Preset());
            result = api.create(*cmdbuf, NVSDK_NGX_Feature_SuperSampling, params, &handle);
            ngx_handle = handle;
            if (NVSDK_NGX_FAILED(result) || !handle) {
                failed = true;
                LOG_ERROR(Render_Vulkan, "Linux NGX DLSS CreateFeature failed ({:#x}); using FSR 3.1 next frame", static_cast<unsigned>(result));
                clear_output(); return;
            }
            LOG_INFO(Render_Vulkan, "Linux NGX DLSS SR feature created: {}x{} -> {}x{}, quality {}, preset {}, flags {:#x}",
                in.render_size.width, in.render_size.height, in.upscale_size.width, in.upscale_size.height,
                static_cast<int>(ModeFor(in.render_size, in.upscale_size)), Preset(), flags);
        }
        constexpr VkAccessFlags WRITES = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        const std::array to_read{
            Barrier(in.color, WRITES, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(in.depth, WRITES, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(in.motion_vectors, WRITES, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(out, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL)};
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, to_read);
        auto color = Resource(in.color, in.render_size, false);
        auto depth = Resource(in.depth, in.render_size, false);
        auto motion = Resource(in.motion_vectors, in.render_size, false);
        auto target = Resource(out, in.upscale_size, true);
        params->Set(NVSDK_NGX_Parameter_Color, static_cast<void*>(&color));
        params->Set(NVSDK_NGX_Parameter_Depth, static_cast<void*>(&depth));
        params->Set(NVSDK_NGX_Parameter_MotionVectors, static_cast<void*>(&motion));
        params->Set(NVSDK_NGX_Parameter_Output, static_cast<void*>(&target));
        params->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, in.jitter[0]);
        params->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, in.jitter[1]);
        // NGX takes render-pixel motion, not Streamline's normalized screen motion.
        params->Set(NVSDK_NGX_Parameter_MV_Scale_X, in.motion_vector_scale[0]);
        params->Set(NVSDK_NGX_Parameter_MV_Scale_Y, in.motion_vector_scale[1]);
        params->Set(NVSDK_NGX_Parameter_Reset, (in.reset || changed) ? 1 : 0);
        params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, in.render_size.width);
        params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, in.render_size.height);
        params->Set(NVSDK_NGX_Parameter_ExposureTexture, static_cast<void*>(nullptr));
        // Inputs has no separate exposure texture: preserve SL's exposure convention (tonemapped
        // OoT: unit exposure; HDR: NGX auto exposure unless explicitly disabled).
        params->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
        params->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
        result = api.evaluate(*cmdbuf, handle, params, nullptr);
        const std::array to_general{
            Barrier(in.color, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | WRITES, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(in.depth, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | WRITES, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(in.motion_vectors, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | WRITES, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL)};
        const VkMemoryBarrier written{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT};
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, written, {}, to_general);
        if (NVSDK_NGX_FAILED(result)) {
            failed = true;
            LOG_ERROR(Render_Vulkan, "Linux NGX DLSS EvaluateFeature failed ({:#x}); using FSR 3.1 next frame", static_cast<unsigned>(result));
            clear_output();
        } else if (frame_index++ == 0) {
            LOG_INFO(Render_Vulkan, "Linux NGX DLSS SR first evaluation recorded successfully");
        }
    });
    scheduler.InvalidateState();
    return true;
}
} // namespace Vulkan
