// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan.h"

namespace Vulkan {
struct NvidiaDlssImage {
    VkImageView view;
    VkImage image;
    VkFormat format;
    VkImageSubresourceRange range;     ///< The view's level and layer, with the view's aspect.
    VkImageAspectFlags barrier_aspect; ///< Every aspect of the image's format.
};

struct NvidiaDlssInputs {
    NvidiaDlssImage color;          ///< Jittered colour at the render size.
    NvidiaDlssImage depth;          ///< Device depth (reversed) at the render size.
    NvidiaDlssImage motion_vectors; ///< Motion to the previous frame at the render size.
    VkExtent2D render_size;
    VkExtent2D upscale_size;
    /// This frame's sample offset in render pixels, as the game hands its own DLSS.
    std::array<f32, 2> jitter;
    /// Turns a motion vector into render pixels (the render size for UV offsets).
    std::array<f32, 2> motion_vector_scale;
    bool reset;
    bool depth_inverted = true;
    bool hdr = true; ///< Linear HDR colour. False for tonemapped, perceptually encoded colour.
    /// Native NGX only: the DLSS preset letter ('J' to 'M'), or 0 to leave it to NVIDIA.
    /// EDEN_NX2_DLSS_PRESET overrides it.
    char preset = 'K';
};

} // namespace Vulkan
