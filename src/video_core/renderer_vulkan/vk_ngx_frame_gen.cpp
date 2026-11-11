// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_vulkan/vk_ngx_frame_gen.h"

#include <algorithm>

#include "common/logging.h"
#include "common/settings.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/ngx.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {
namespace {
NgxFrameGen* current_instance{};

constexpr VkImageUsageFlags CAPTURE_USAGE =
    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
constexpr VkImageUsageFlags OUTPUT_USAGE = VK_IMAGE_USAGE_STORAGE_BIT |
                                           VK_IMAGE_USAGE_SAMPLED_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
constexpr VkAccessFlags ANY_WRITE =
    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
    VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;

VkImageMemoryBarrier ImageBarrier(VkImage image, VkImageAspectFlags aspects, u32 level, u32 layer,
                                  VkAccessFlags src_access, VkAccessFlags dst_access,
                                  VkImageLayout old_layout, VkImageLayout new_layout) {
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange{
            .aspectMask = aspects,
            .baseMipLevel = level,
            .levelCount = 1,
            .baseArrayLayer = layer,
            .layerCount = 1,
        },
    };
}

VkImageMemoryBarrier ColorBarrier(VkImage image, VkAccessFlags src_access, VkAccessFlags dst_access,
                                  VkImageLayout old_layout, VkImageLayout new_layout) {
    return ImageBarrier(image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, src_access, dst_access, old_layout,
                        new_layout);
}

VkImageCopy CopyRegion(VkImageAspectFlags aspect, u32 src_level, u32 src_layer, VkExtent2D extent) {
    return VkImageCopy{
        .srcSubresource = {aspect, src_level, src_layer, 1},
        .srcOffset = {0, 0, 0},
        .dstSubresource = {aspect, 0, 0, 1},
        .dstOffset = {0, 0, 0},
        .extent = {extent.width, extent.height, 1},
    };
}

bool AsyncPresentation() {
    return Settings::values.async_presentation.GetValue();
}
} // namespace

NgxFrameGen::NgxFrameGen(const Device& device_, MemoryAllocator& memory_allocator_,
                         Scheduler& scheduler_)
    : device{device_}, memory_allocator{memory_allocator_}, scheduler{scheduler_},
      feature{std::make_unique<Ngx::FrameGenFeature>()}, multiplier{Ngx::FrameGenMultiplier()} {
    if (multiplier <= 1) {
        return;
    }
    current_instance = this;
    LOG_INFO(Render_Vulkan,
             "DLSS-G: frame generation on at {}x ({} generated per rendered frame); paced by "
             "Fritchii's present thread{}",
             multiplier, multiplier - 1,
             AsyncPresentation() ? ""
                                 : " - asynchronous presentation is off, so generated frames are "
                                   "presented back to back (turn it on for even pacing)");
}

NgxFrameGen::~NgxFrameGen() {
    if (current_instance == this) {
        current_instance = nullptr;
    }
    if (output_image || capture.depth) {
        scheduler.Finish();
    }
    feature->Release();
    if (groups != 0) {
        LOG_INFO(Render_Vulkan, "DLSS-G: stopped after {} rendered frames with {} generated ones",
                 groups, frames_generated);
    }
}

NgxFrameGen* NgxFrameGen::Current() {
    return current_instance;
}

void NgxFrameGen::CreateCapture(const NvidiaDlssInputs& in) {
    if (capture.depth) {
        scheduler.Finish(); // frame generation may still read the old copies
    }
    const auto create = [&](VkFormat format, VkImageAspectFlags view_aspect, vk::Image& image,
                            vk::ImageView& view) {
        image = memory_allocator.CreateImage(VkImageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = {in.render_size.width, in.render_size.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = CAPTURE_USAGE,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        });
        view = device.GetLogical().CreateImageView(VkImageViewCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = *image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .components{},
            .subresourceRange{
                .aspectMask = view_aspect,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        });
    };
    capture.depth_view = {};
    capture.motion_view = {};
    create(in.depth.format, VK_IMAGE_ASPECT_DEPTH_BIT, capture.depth, capture.depth_view);
    create(in.motion_vectors.format, VK_IMAGE_ASPECT_COLOR_BIT, capture.motion,
           capture.motion_view);
    capture.depth_format = in.depth.format;
    capture.motion_format = in.motion_vectors.format;
    capture.depth_aspects = in.depth.barrier_aspect ? in.depth.barrier_aspect
                                                    : VkImageAspectFlags{VK_IMAGE_ASPECT_DEPTH_BIT};
    capture.extent = in.render_size;
    need_reset = true;
    LOG_INFO(Render_Vulkan, "DLSS-G: input copies at {}x{} (depth format {}, motion format {})",
             in.render_size.width, in.render_size.height, static_cast<int>(in.depth.format),
             static_cast<int>(in.motion_vectors.format));
}

const NgxFrameGen::Capture* NgxFrameGen::PrepareCapture(const NvidiaDlssInputs& in) {
    if (failed.load(std::memory_order_relaxed) || multiplier <= 1 || !in.render_size.width ||
        !in.render_size.height) {
        return nullptr;
    }
    if (!capture.depth || capture.extent.width != in.render_size.width ||
        capture.extent.height != in.render_size.height ||
        capture.depth_format != in.depth.format ||
        capture.motion_format != in.motion_vectors.format) {
        CreateCapture(in);
    }
    captured = in;
    ++capture_sequence;
    return &capture;
}

void NgxFrameGen::RecordCapture(vk::CommandBuffer cmdbuf, const NvidiaDlssInputs& in,
                                const Capture& capture) {
    constexpr VkImageLayout READ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    constexpr VkImageLayout SRC = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    constexpr VkImageLayout DST = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    const VkImageAspectFlags depth_aspects =
        in.depth.barrier_aspect ? in.depth.barrier_aspect
                                : VkImageAspectFlags{VK_IMAGE_ASPECT_DEPTH_BIT};
    const VkImageAspectFlags motion_aspects =
        in.motion_vectors.barrier_aspect ? in.motion_vectors.barrier_aspect
                                         : VkImageAspectFlags{VK_IMAGE_ASPECT_COLOR_BIT};
    const auto source = [](const NvidiaDlssImage& image, VkImageAspectFlags aspects,
                           VkAccessFlags src, VkAccessFlags dst, VkImageLayout from,
                           VkImageLayout to) {
        return ImageBarrier(image.image, aspects, image.range.baseMipLevel,
                            image.range.baseArrayLayer, src, dst, from, to);
    };
    const std::array before{
        source(in.depth, depth_aspects, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
               READ, SRC),
        source(in.motion_vectors, motion_aspects, VK_ACCESS_SHADER_READ_BIT,
               VK_ACCESS_TRANSFER_READ_BIT, READ, SRC),
        ImageBarrier(*capture.depth, capture.depth_aspects, 0, 0, VK_ACCESS_SHADER_READ_BIT,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, DST),
        ImageBarrier(*capture.motion, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, VK_ACCESS_SHADER_READ_BIT,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, DST),
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                           {}, {}, before);
    cmdbuf.CopyImage(in.depth.image, SRC, *capture.depth, DST,
                     CopyRegion(VK_IMAGE_ASPECT_DEPTH_BIT, in.depth.range.baseMipLevel,
                                in.depth.range.baseArrayLayer, in.render_size));
    cmdbuf.CopyImage(in.motion_vectors.image, SRC, *capture.motion, DST,
                     CopyRegion(VK_IMAGE_ASPECT_COLOR_BIT, in.motion_vectors.range.baseMipLevel,
                                in.motion_vectors.range.baseArrayLayer, in.render_size));
    const std::array after{
        source(in.depth, depth_aspects, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
               SRC, READ),
        source(in.motion_vectors, motion_aspects, VK_ACCESS_TRANSFER_READ_BIT,
               VK_ACCESS_SHADER_READ_BIT, SRC, READ),
        ImageBarrier(*capture.depth, capture.depth_aspects, 0, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_ACCESS_SHADER_READ_BIT, DST, READ),
        ImageBarrier(*capture.motion, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, DST, READ),
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                           {}, {}, after);
}

void NgxFrameGen::CreateOutput(VkExtent2D extent, VkFormat format) {
    if (output_image) {
        scheduler.Finish();
    }
    output_view = {};
    output_image = memory_allocator.CreateImage(VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {extent.width, extent.height, 1},
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
        .format = format,
        .components{},
        .subresourceRange{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    });
    output_extent = extent;
    output_format = format;
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([image = *output_image](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                               ColorBarrier(image, 0, VK_ACCESS_SHADER_WRITE_BIT,
                                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL));
    });
    need_reset = true;
    LOG_INFO(Render_Vulkan, "DLSS-G: output image {}x{} (format {})", extent.width, extent.height,
             static_cast<int>(format));
}

u32 NgxFrameGen::BeginFrame(Frame* real, VkFormat view_format,
                            const Common::Rectangle<u32>& screen) {
    generations = 0;
    if (multiplier <= 1 || failed.load(std::memory_order_relaxed)) {
        return 0;
    }
    if (capture_sequence == consumed_sequence) {
        // No DLSS evaluation since the last frame (menus, movies, loading): shown as it is, and
        // the next generated frame starts over.
        if (!need_reset) {
            LOG_INFO(Render_Vulkan,
                     "DLSS-G: no DLSS evaluation this frame; frames are shown without "
                     "generation until DLSS runs again");
        }
        need_reset = true;
        return 0;
    }
    consumed_sequence = capture_sequence;
    if (!logged_first) {
        const VkFormatFeatureFlags features =
            device.GetPhysical().GetFormatProperties(view_format).optimalTilingFeatures;
        LOG_INFO(Render_Vulkan,
                 "DLSS-G: first frame with DLSS inputs: backbuffer {}x{} format {} (storage {}, "
                 "sampled {}), game image at {},{} {}x{}, render size {}x{}, depth {}",
                 real->width, real->height, static_cast<int>(view_format),
                 (features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ? "yes" : "no",
                 (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? "yes" : "no", screen.left,
                 screen.top, screen.GetWidth(), screen.GetHeight(),
                 captured.render_size.width, captured.render_size.height,
                 captured.depth_inverted ? "inverted" : "standard");
        logged_first = true;
        if (!(features & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
            LOG_ERROR(Render_Vulkan,
                      "DLSS-G: the swapchain view format {} can't be a storage image here; frame "
                      "generation is off for this session",
                      static_cast<int>(view_format));
            failed = true;
            return 0;
        }
    }
    if (!real->width || !real->height) {
        return 0;
    }
    const VkExtent2D extent{real->width, real->height};
    if (!output_image || output_extent.width != extent.width ||
        output_extent.height != extent.height || output_format != view_format) {
        CreateOutput(extent, view_format);
    }
    real_image = *real->image;
    real_view = *real->image_view;
    real_format = view_format;
    real_extent = extent;
    const u32 left = std::min(screen.left, extent.width);
    const u32 top = std::min(screen.top, extent.height);
    const u32 right = std::clamp(screen.right, left, extent.width);
    const u32 bottom = std::clamp(screen.bottom, top, extent.height);
    subrect_offset = {static_cast<s32>(left), static_cast<s32>(top)};
    subrect_extent = {right - left, bottom - top};
    frame_inputs = captured;
    frame_reset = need_reset || captured.reset;
    need_reset = false;
    ++frame_id;
    ++groups;
    generations = multiplier - 1;
    if (groups % 600 == 1) {
        LOG_INFO(Render_Vulkan,
                 "DLSS-G: {} rendered frames with generation so far, {} generated frames, {} "
                 "evaluations ok",
                 groups, frames_generated, evaluations_ok.load(std::memory_order_relaxed));
    }
    return generations;
}

void NgxFrameGen::GenerateInto(Frame* destination, u32 index) {
    if (index == 0 || index > generations) {
        return;
    }
    ++frames_generated;
    Ngx::FrameGenInputs inputs{
        .backbuffer = {real_view, real_image, real_format, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                       VK_IMAGE_ASPECT_COLOR_BIT},
        .backbuffer_size = real_extent,
        .subrect_offset = subrect_offset,
        .subrect_size = subrect_extent,
        .depth = {*capture.depth_view, *capture.depth, capture.depth_format,
                  {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}, capture.depth_aspects},
        .motion_vectors = {*capture.motion_view, *capture.motion, capture.motion_format,
                           {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, VK_IMAGE_ASPECT_COLOR_BIT},
        .render_size = capture.extent,
        .output = {*output_view, *output_image, output_format,
                   {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, VK_IMAGE_ASPECT_COLOR_BIT},
        .jitter = frame_inputs.jitter,
        .motion_vector_scale = frame_inputs.motion_vector_scale,
        .reset = frame_reset,
        .depth_inverted = frame_inputs.depth_inverted,
        .multi_frame_count = generations,
        .multi_frame_index = index,
        .frame_id = frame_id,
    };
    const VkImage destination_image = *destination->image;
    const VkExtent2D destination_extent{
        std::min(destination->width, real_extent.width),
        std::min(destination->height, real_extent.height),
    };
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([this, inputs, destination_image,
                      destination_extent](vk::CommandBuffer cmdbuf) {
        if (inputs.multi_frame_index == 1) {
            // The real frame was just drawn (window adapt pass) and stays in GENERAL.
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                                   ColorBarrier(inputs.backbuffer.image, ANY_WRITE,
                                                VK_ACCESS_SHADER_READ_BIT |
                                                    VK_ACCESS_TRANSFER_READ_BIT,
                                                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL));
        }
        // The output was last read by the copy into the previous generated frame.
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                               ColorBarrier(inputs.output.image, VK_ACCESS_TRANSFER_READ_BIT,
                                            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                                            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL));
        const bool evaluated = !failed.load(std::memory_order_relaxed) &&
                               feature->Evaluate(*cmdbuf, inputs);
        if (feature->JustCreated()) {
            LOG_INFO(Render_Vulkan,
                     "DLSS-G: feature created ({:#x}): backbuffer {}x{} format {}, subrect {},{} "
                     "{}x{}, from {}x{} depth and motion, {} generated per frame",
                     feature->CreateResult(), inputs.backbuffer_size.width,
                     inputs.backbuffer_size.height, static_cast<int>(inputs.backbuffer.format),
                     inputs.subrect_offset.x, inputs.subrect_offset.y, inputs.subrect_size.width,
                     inputs.subrect_size.height, inputs.render_size.width,
                     inputs.render_size.height, inputs.multi_frame_count);
        }
        if (evaluated) {
            if (evaluations_ok.fetch_add(1, std::memory_order_relaxed) == 0) {
                LOG_INFO(Render_Vulkan,
                         "DLSS-G: evaluate ok ({:#x}), frame {} of {}, subrect {},{} {}x{}, "
                         "reset {}",
                         feature->LastResult(), inputs.multi_frame_index,
                         inputs.multi_frame_count, inputs.subrect_offset.x,
                         inputs.subrect_offset.y, inputs.subrect_size.width,
                         inputs.subrect_size.height, inputs.reset);
            }
        } else if (!failed.exchange(true)) {
            LOG_ERROR(Render_Vulkan,
                      "DLSS-G: {} failed ({:#x}) at backbuffer {}x{}, subrect {},{} {}x{}, render "
                      "{}x{}; frame generation is off for this session, frames are shown as they "
                      "are (NGX's own reason is in ~/.cache/fritchii/ngx/nvngx_dlssg_*.log)",
                      feature->CreateFailed() ? "feature creation" : "evaluate",
                      feature->LastResult(), inputs.backbuffer_size.width,
                      inputs.backbuffer_size.height, inputs.subrect_offset.x,
                      inputs.subrect_offset.y, inputs.subrect_size.width,
                      inputs.subrect_size.height, inputs.render_size.width,
                      inputs.render_size.height);
        }
        // Copy the generated frame (or, after a failure, the real one) into the present frame.
        const VkImage source = evaluated ? inputs.output.image : inputs.backbuffer.image;
        const std::array to_copy{
            ColorBarrier(inputs.output.image, VK_ACCESS_SHADER_WRITE_BIT,
                         VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_GENERAL),
            ColorBarrier(destination_image, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, {}, {}, to_copy);
        cmdbuf.CopyImage(source, VK_IMAGE_LAYOUT_GENERAL, destination_image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         CopyRegion(VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, destination_extent));
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               0,
                               ColorBarrier(destination_image, VK_ACCESS_TRANSFER_WRITE_BIT,
                                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                                VK_ACCESS_TRANSFER_READ_BIT,
                                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            VK_IMAGE_LAYOUT_GENERAL));
    });
    // NGX leaves its own pipeline and descriptor state in the command buffer.
    scheduler.InvalidateState();
}

} // namespace Vulkan
