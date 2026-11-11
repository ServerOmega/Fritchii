// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <optional>
#include <string>
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/dlss_mode.h"
#include "video_core/renderer_base.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_streamline_dlss.h"
#include "video_core/vulkan_common/ngx.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {
namespace {
constexpr VkFormat OUTPUT_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkImageUsageFlags OUTPUT_USAGE = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

std::atomic<u64> evaluations{};
std::atomic<u32> last_evaluation{}; ///< Mode, requested mode and preset, a byte each
VkImageMemoryBarrier Barrier(const NvidiaDlssUpscaler::Image& image, VkAccessFlags src_access,
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

} // namespace

NvidiaDlssUpscaler::NvidiaDlssUpscaler(const Device& device_, Scheduler& scheduler_,
                                    MemoryAllocator& memory_allocator_)
    : device{device_}, scheduler{scheduler_}, memory_allocator{memory_allocator_},
      feature{std::make_unique<Ngx::Feature>()} {
    if (!Ngx::IsDlssAvailable()) throw vk::Exception(VK_ERROR_FEATURE_NOT_PRESENT);
    VkSamplerCreateInfo sampler_info{
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
    };
    point_sampler = device.GetLogical().CreateSampler(sampler_info);
    // Native (DLAA) output is scaled to the guest destination with this one.
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    linear_sampler = device.GetLogical().CreateSampler(sampler_info);
}

NvidiaDlssUpscaler::~NvidiaDlssUpscaler() {
    scheduler.WaitWorker();
    void(device.GetLogical().WaitIdle());
    feature.reset();
}

void NvidiaDlssUpscaler::CreateOutput(VkExtent2D upscale_size) {
    output_view = {};
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


namespace {
// Graphics > DLSS preset, as the Streamline backend reads it. EDEN_NX2_DLSS_PRESET still overrides
// it in Ngx::Feature.
char PresetLetter(Settings::DlssPreset preset) {
    switch (preset) {
    case Settings::DlssPreset::Default:
        return 0;
    case Settings::DlssPreset::J:
        return 'J';
    case Settings::DlssPreset::L:
        return 'L';
    case Settings::DlssPreset::M:
        return 'M';
    case Settings::DlssPreset::K:
    default:
        return 'K';
    }
}
} // namespace

bool NvidiaDlssUpscaler::Upscale(const Inputs& frame) {
    if (failed) return false;
    Inputs in = frame;
    // Graphics > DLSS quality and preset apply while playing (Settings::RefreshNvidiaDlssRuntimeSettings).
    const auto settings = Settings::GetNvidiaDlssRuntimeSettings();
    in.preset = PresetLetter(settings.preset);
    const bool extent_changed = !output_image || in.render_size.width != render_extent.width ||
        in.render_size.height != render_extent.height || in.upscale_size.width != upscale_extent.width ||
        in.upscale_size.height != upscale_extent.height;
    // A new preset recreates the NGX feature, which needs the GPU idle like the other options.
    const bool options_changed = extent_changed || in.hdr != hdr_input ||
                                 in.depth_inverted != depth_inverted_input ||
                                 quality_mode != settings.quality ||
                                 model_preset != settings.preset;
    if (options_changed && output_image) scheduler.Finish();
    if (extent_changed) {
        CreateOutput(in.upscale_size);
        render_extent = in.render_size;
        upscale_extent = in.upscale_size;
    }
    hdr_input = in.hdr;
    depth_inverted_input = in.depth_inverted;
    quality_mode = settings.quality;
    model_preset = settings.preset;
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([this, in, options_changed, requested = settings.quality,
                      preset_setting = settings.preset, index = frame_index++,
                      output_image_handle = *output_image,
                      output_view_handle = *output_view](vk::CommandBuffer cmdbuf) {
        const auto clear_output = [&] {
            const VkImageSubresourceRange range{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            };
            const auto barrier = [&](VkAccessFlags src_access, VkAccessFlags dst_access) {
                return VkImageMemoryBarrier{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = src_access,
                    .dstAccessMask = dst_access,
                    .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = output_image_handle,
                    .subresourceRange = range,
                };
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                   barrier(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                                           VK_ACCESS_TRANSFER_WRITE_BIT));
            cmdbuf.ClearColorImage(output_image_handle, VK_IMAGE_LAYOUT_GENERAL,
                                   VkClearColorValue{}, range);
            cmdbuf.PipelineBarrier(
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                barrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT));
        };
        constexpr VkAccessFlags WRITES =
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        // The inputs come from the game's draws and dispatches and stay in GENERAL otherwise;
        // DLSS reads them as sampled images.
        const std::array to_read{
            Barrier(in.color, WRITES, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(in.depth, WRITES, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(in.motion_vectors, WRITES, VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        };

        const std::array to_general{
            Barrier(in.color, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | WRITES,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(in.depth, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | WRITES,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(in.motion_vectors, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | WRITES,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
        };
        const VkMemoryBarrier output_written{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        };

        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, to_read);
        const Image output{output_view_handle, output_image_handle, OUTPUT_FORMAT,
                           {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, VK_IMAGE_ASPECT_COLOR_BIT};
        // Pick the mode like the Streamline backend (docs/user/DLSS.md): the requested one when the
        // render size fits NVIDIA's range for it, else the mode Automatic picks.
        bool selected = true;
        if (options_changed || index == 0) {
            namespace Mode = VideoCommon::DlssMode;
            bool any_range = false;
            const auto selection = Mode::Select(
                requested, {in.render_size.width, in.render_size.height},
                {in.upscale_size.width, in.upscale_size.height},
                [&](Settings::NvidiaDlssQuality mode) -> std::optional<Mode::Range> {
                    const auto range = Ngx::OptimalRange(mode, in.upscale_size);
                    any_range |= range.has_value();
                    return range;
                });
            if (selection.fallback) {
                const Mode::Range range = selection.requested_range.value_or(Mode::Range{});
                LOG_WARNING(Render_Vulkan,
                            "NVIDIA DLSS (native NGX): requested {} but using {} for {}x{} -> {}x{}; "
                            "requested range {}x{} to {}x{} (query {})",
                            Settings::CanonicalizeEnum(requested),
                            selection.mode ? Settings::CanonicalizeEnum(*selection.mode) : "FSR 3.1",
                            in.render_size.width, in.render_size.height, in.upscale_size.width,
                            in.upscale_size.height, range.min.width, range.min.height,
                            range.max.width, range.max.height,
                            selection.requested_range ? "supported" : "unavailable");
            }
            if (selection.mode) {
                evaluated_mode = *selection.mode;
            } else if (!any_range && requested == Settings::NvidiaDlssQuality::Automatic) {
                // A runtime without the optimal-settings query: keep the ratio-based pick
                // Automatic used before quality modes existed.
                evaluated_mode = Mode::Preferred({in.render_size.width, in.render_size.height},
                                                 {in.upscale_size.width, in.upscale_size.height});
                LOG_WARNING(Render_Vulkan, "NVIDIA DLSS (native NGX): no optimal-settings answer; "
                                           "using {} from the size ratio",
                            Settings::CanonicalizeEnum(evaluated_mode));
            } else {
                LOG_WARNING(Render_Vulkan,
                            "NVIDIA DLSS (native NGX): no mode accepts {}x{} -> {}x{}",
                            in.render_size.width, in.render_size.height, in.upscale_size.width,
                            in.upscale_size.height);
                selected = false;
            }
        }
        Inputs evaluation = in;
        evaluation.reset |= options_changed;
        const bool evaluated =
            !failed && selected && feature->Evaluate(*cmdbuf, evaluation, evaluated_mode, output);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, output_written, {}, to_general);
        if (!evaluated) {
            if (!failed.exchange(true)) {
                LOG_ERROR(Render_Vulkan, "NVIDIA DLSS (native NGX): evaluation failed ({:#x}); keeping fallback",
                          feature->LastResult());
            }
            clear_output();
            return;
        }
        if (index == 0 || options_changed) {
            LOG_INFO(Render_Vulkan, "NVIDIA DLSS (native NGX): evaluation succeeded ({:#x}), {}x{} -> {}x{}, mode {}, preset {}, {}, depth {}",
                     feature->LastResult(), in.render_size.width, in.render_size.height,
                     in.upscale_size.width, in.upscale_size.height,
                     Settings::CanonicalizeEnum(evaluated_mode),
                     in.preset ? std::string(1, in.preset) : std::string("default"),
                     in.hdr ? "HDR" : "LDR", in.depth_inverted ? "inverted" : "standard");
        }
        last_evaluation.store(static_cast<u32>(evaluated_mode) |
                                  (static_cast<u32>(requested) << 8) |
                                  (static_cast<u32>(preset_setting) << 16),
                              std::memory_order_relaxed);
        evaluations.fetch_add(1, std::memory_order_release);
    });
    scheduler.InvalidateState();
    return true;
}

VideoCore::DlssStatus NvidiaDlssUpscaler::Status() {
    const u64 count = evaluations.load(std::memory_order_acquire);
    const u32 last = last_evaluation.load(std::memory_order_relaxed);
    return {
        .evaluations = count,
        .mode = static_cast<Settings::NvidiaDlssQuality>(last & 0xff),
        .requested = static_cast<Settings::NvidiaDlssQuality>((last >> 8) & 0xff),
        .preset = static_cast<Settings::DlssPreset>((last >> 16) & 0xff),
    };
}
} // namespace Vulkan
