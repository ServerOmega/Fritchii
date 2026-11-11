// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>

#include "common/settings.h"

namespace Vulkan {

/// What Graphics > NVIDIA Reflex asks for; EDEN_REFLEX=0|1|2 overrides it.
/// Read once per session (device and swapchain creation, renderer start).
[[nodiscard]] inline Settings::NvidiaReflex RequestedReflexMode() {
#ifdef EGLANTINE_STREAMLINE
    return Settings::NvidiaReflex::Off; // Streamline owns latency in these builds
#else
    if (const char* value = std::getenv("EDEN_REFLEX"); value && *value) {
        switch (value[0]) {
        case '1':
            return Settings::NvidiaReflex::On;
        case '2':
            return Settings::NvidiaReflex::Boost;
        default:
            return Settings::NvidiaReflex::Off;
        }
    }
    return Settings::values.nvidia_reflex.GetValue();
#endif
}

} // namespace Vulkan
