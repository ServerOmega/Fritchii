// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#ifdef EGLANTINE_NGX
#include "video_core/vulkan_common/ngx.h"
namespace Vulkan { namespace NvidiaDlss = Ngx; }
#else
#include "video_core/vulkan_common/streamline.h"
namespace Vulkan { namespace NvidiaDlss = Streamline; }
#endif
