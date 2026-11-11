// SPDX-FileCopyrightText: 2017 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include "common/common_types.h"

namespace Core {

struct PerfStatsResults {
    /// System FPS (LCD VBlanks) in Hz
    double system_fps;
    /// Average game FPS (GPU frame renders) in Hz
    double average_game_fps;
    /// Walltime per system frame, in seconds, excluding any waits
    double frametime;
    /// Ratio of walltime / emulated time elapsed
    double emulation_speed;
    /// Length of this sample window, in seconds
    double interval{};
    /// Game frames in this window
    u32 game_frames{};
    /// Shortest and longest system-frame times in this window, in seconds (0 if none)
    double min_frametime{};
    double max_frametime{};
};

/**
 * Class to manage and query performance/timing statistics. All public functions of this class are
 * thread-safe unless stated otherwise.
 */
class PerfStats {
public:
    explicit PerfStats(u64 title_id_);
    ~PerfStats();

    using Clock = std::chrono::steady_clock;

    void BeginSystemFrame();
    void EndSystemFrame();
    void EndGameFrame();

    PerfStatsResults GetAndResetStats(std::chrono::microseconds current_system_time_us);

    /**
     * Returns the arithmetic mean of all frametime values stored in the performance history.
     */
    double GetMeanFrametime() const;

    /**
     * Gets the ratio between walltime and the emulated time of the previous system frame. This is
     * useful for scaling inputs or outputs moving between the two time domains.
     */
    double GetLastFrameTimeScale() const;

private:
    mutable std::mutex object_mutex;

    /// Title ID for the game that is running. 0 if there is no game running yet
    u64 title_id{0};
    /// Current index for writing to the perf_history array
    std::size_t current_index{0};
    /// Stores an hour of historical frametime data useful for processing and tracking performance
    /// regressions with code changes.
    std::array<double, 216000> perf_history{};

    /// Point when the cumulative counters were reset
    Clock::time_point reset_point = Clock::now();
    /// System time when the cumulative counters were reset
    std::chrono::microseconds reset_point_system_us{0};

    /// Cumulative duration (excluding v-sync/frame-limiting) of frames since last reset
    Clock::duration accumulated_frametime = Clock::duration::zero();
    /// Cumulative number of system frames (LCD VBlanks) presented since last reset
    u32 system_frames = 0;
    /// Cumulative number of game frames (GSP frame submissions) since last reset
    std::atomic<u32> game_frames = 0;

    /// Point when the previous system frame ended
    Clock::time_point previous_frame_end = reset_point;
    /// Point when the current system frame began
    Clock::time_point frame_begin = reset_point;
    /// Total visible duration (including frame-limiting, etc.) of the previous system frame
    Clock::duration previous_frame_length = Clock::duration::zero();
    /// Previously computed fps
    double previous_fps = 0;

    /// Shortest and longest system-frame times since the last GetAndResetStats
    Clock::duration interval_min = Clock::duration::max();
    Clock::duration interval_max = Clock::duration::zero();

    // Accumulates ~1 s of GetAndResetStats windows into one Perf: log line.
    void MaybeLogWindow(const PerfStatsResults& results);
    void LogRunStart();
    void LogRunEnd() const;
    Clock::time_point run_start = reset_point;
    bool logged_run_start{};
    double log_interval{};
    double log_game_frames{};
    double log_frametime_sum{};
    double log_min_ft{};
    double log_max_ft{};
    u32 log_frames{};
    u64 log_fg_rendered{};
    u64 log_fg_generated{};
};

class SpeedLimiter {
public:
    using Clock = std::chrono::steady_clock;

    void DoSpeedLimiting(std::chrono::microseconds current_system_time_us);

private:
    /// Emulated system time (in microseconds) at the last limiter invocation
    std::chrono::microseconds previous_system_time_us{0};
    /// Walltime at the last limiter invocation
    Clock::time_point previous_walltime = Clock::now();

    /// Accumulated difference between walltime and emulated time
    std::chrono::microseconds speed_limiting_delta_err{0};
};

} // namespace Core
