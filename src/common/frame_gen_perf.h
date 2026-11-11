// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include "common/common_types.h"

namespace Common::FrameGenPerf {

// Filled by the DLSS-G path (GPU thread), read by the perf log (any thread).
struct Counters {
    std::atomic<u64> rendered{0};
    std::atomic<u64> generated{0};
    std::atomic<u32> multiplier{0};
};

inline Counters& Get() {
    static Counters counters;
    return counters;
}

inline void Reset() {
    auto& counters = Get();
    counters.rendered.store(0, std::memory_order_relaxed);
    counters.generated.store(0, std::memory_order_relaxed);
    counters.multiplier.store(0, std::memory_order_relaxed);
}

} // namespace Common::FrameGenPerf
