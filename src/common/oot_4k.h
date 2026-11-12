// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>
#include <string_view>

#include "common/common_types.h"
#include "common/literals.h"

namespace Common {

/// EDEN_NX2_OOT_4K=1: experimental native 4K picture/scene for Ocarina of Time. Off by default.
inline bool Oot4kRequested() {
    const char* const value = std::getenv("EDEN_NX2_OOT_4K");
    return value != nullptr && std::string_view{value} == "1";
}

/// Scene-scale 1.5 (2160/1440) is on with 4K unless EDEN_NX2_OOT_4K_SCALE=0, so a test can tell
/// whether the 3D scene follows the 4K picture or stays on the 1440-line baseline.
inline bool Oot4kScaleRequested() {
    if (!Oot4kRequested()) {
        return false;
    }
    const char* const value = std::getenv("EDEN_NX2_OOT_4K_SCALE");
    return !(value != nullptr && std::string_view{value} == "0");
}

/// With 4K on, NVIDIA DLSS runs as DLAA on the 3840x2160 scene unless EDEN_NX2_OOT_4K_DLAA=0,
/// which keeps the game's own anti-aliasing pass (the a23a737 behaviour).
inline bool Oot4kDlaaRequested() {
    if (!Oot4kRequested()) {
        return false;
    }
    const char* const value = std::getenv("EDEN_NX2_OOT_4K_DLAA");
    return !(value != nullptr && std::string_view{value} == "0");
}

/// Extra application-pool bytes reserved when 4K is on: 400 MiB, a little above NX2navi's
/// rounded 1.25 * 312320 KiB (391168 KiB / 382 MiB) so a slightly larger shipped heap still fits.
inline u64 Oot4kAppPoolExtraBytes() {
    using namespace Common::Literals;
    return Oot4kRequested() ? 400_MiB : 0;
}

} // namespace Common
