// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2017 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <mutex>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <fmt/chrono.h>
#include <fmt/ranges.h>
#include "common/frame_gen_perf.h"
#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/perf_stats.h"

using namespace std::chrono_literals;
using DoubleSecs = std::chrono::duration<double, std::chrono::seconds::period>;
using std::chrono::duration_cast;
using std::chrono::microseconds;

// Purposefully ignore the first five frames, as there's a significant amount of overhead in
// booting that we shouldn't account for
constexpr std::size_t IgnoreFrames = 5;

namespace Core {

PerfStats::PerfStats(u64 title_id_) : title_id(title_id_) {}

PerfStats::~PerfStats() {
    LogRunEnd();

    if (!Settings::values.record_frame_times || title_id == 0) {
        return;
    }

    const std::time_t t = std::time(nullptr);
    std::ostringstream stream;
    std::copy(perf_history.begin() + IgnoreFrames, perf_history.begin() + current_index,
              std::ostream_iterator<double>(stream, "\n"));

    const auto path = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir);
    // %F Date format expanded is "%Y-%m-%d"
    const auto filename = fmt::format("{}_{:016X}.csv",
        [&] {
            std::ostringstream oss;
            oss << std::put_time(std::localtime(&t), "%F-%H-%M");
            return oss.str();
        }(),
        title_id);

    const auto filepath = path / filename;

    if (Common::FS::CreateParentDir(filepath)) {
        Common::FS::IOFile file(filepath, Common::FS::FileAccessMode::Write,
                                Common::FS::FileType::TextFile);
        void(file.WriteString(stream.str()));
    }
}

void PerfStats::BeginSystemFrame() {
    std::scoped_lock lock{object_mutex};

    frame_begin = Clock::now();
}

void PerfStats::EndSystemFrame() {
    std::scoped_lock lock{object_mutex};

    auto frame_end = Clock::now();
    const auto frame_time = frame_end - frame_begin;
    if (current_index < perf_history.size()) {
        perf_history[current_index++] =
            std::chrono::duration<double, std::milli>(frame_time).count();
    }
    accumulated_frametime += frame_time;
    system_frames += 1;
    if (frame_time < interval_min) {
        interval_min = frame_time;
    }
    if (frame_time > interval_max) {
        interval_max = frame_time;
    }

    previous_frame_length = frame_end - previous_frame_end;
    previous_frame_end = frame_end;
}

void PerfStats::EndGameFrame() {
    game_frames.fetch_add(1, std::memory_order_relaxed);
}

double PerfStats::GetMeanFrametime() const {
    std::scoped_lock lock{object_mutex};

    if (current_index <= IgnoreFrames) {
        return 0;
    }

    const double sum = std::accumulate(perf_history.begin() + IgnoreFrames,
                                       perf_history.begin() + current_index, 0.0);
    return sum / static_cast<double>(current_index - IgnoreFrames);
}

PerfStatsResults PerfStats::GetAndResetStats(microseconds current_system_time_us) {
    std::scoped_lock lock{object_mutex};

    const auto now = Clock::now();
    // Walltime elapsed since stats were reset
    const auto interval = duration_cast<DoubleSecs>(now - reset_point).count();

    const auto system_us_per_second = (current_system_time_us - reset_point_system_us) / interval;
    const auto current_frames = static_cast<double>(game_frames.load(std::memory_order_relaxed));
    const auto current_fps = current_frames / interval;
    const double mean_frametime =
        system_frames == 0
            ? 0.0
            : duration_cast<DoubleSecs>(accumulated_frametime).count() /
                  static_cast<double>(system_frames);
    const double min_frametime =
        system_frames == 0 ? 0.0 : duration_cast<DoubleSecs>(interval_min).count();
    const double max_frametime =
        system_frames == 0 ? 0.0 : duration_cast<DoubleSecs>(interval_max).count();
    const PerfStatsResults results{
        .system_fps = static_cast<double>(system_frames) / interval,
        .average_game_fps = (current_fps + previous_fps) / 2.0,
        .frametime = mean_frametime,
        .emulation_speed = system_us_per_second.count() / 1'000'000.0,
        .interval = interval,
        .game_frames = game_frames.load(std::memory_order_relaxed),
        .min_frametime = min_frametime,
        .max_frametime = max_frametime,
    };

    // Reset counters
    reset_point = now;
    reset_point_system_us = current_system_time_us;
    accumulated_frametime = Clock::duration::zero();
    system_frames = 0;
    game_frames.store(0, std::memory_order_relaxed);
    previous_fps = current_fps;
    interval_min = Clock::duration::max();
    interval_max = Clock::duration::zero();

    MaybeLogWindow(results);

    return results;
}

double PerfStats::GetLastFrameTimeScale() const {
    std::scoped_lock lock{object_mutex};

    constexpr double FRAME_LENGTH = 1.0 / 60;
    return duration_cast<DoubleSecs>(previous_frame_length).count() / FRAME_LENGTH;
}

namespace {

const char* PinningRequest() {
    if (const char* value = std::getenv("EDEN_NX2_PIN_THREADS"); value && value[0] != '\0') {
        if (value[0] == '0') {
            return "off (EDEN_NX2_PIN_THREADS=0)";
        }
        if (value[0] == '1') {
            return "force (EDEN_NX2_PIN_THREADS=1)";
        }
        return "on (EDEN_NX2_PIN_THREADS)";
    }
    return Settings::values.thread_pinning.GetValue() ? "on (Settings)" : "off (Settings)";
}

const char* DlssgRequest() {
    if (const char* value = std::getenv("EDEN_DLSSG"); value && value[0] != '\0') {
        switch (value[0]) {
        case '2':
            return "2x (EDEN_DLSSG=2)";
        case '3':
            return "3x (EDEN_DLSSG=3)";
        case '4':
            return "4x (EDEN_DLSSG=4)";
        default:
            return "off (EDEN_DLSSG)";
        }
    }
    switch (Settings::values.nvidia_dlss_frame_gen.GetValue()) {
    case Settings::DlssFrameGen::X2:
        return "2x";
    case Settings::DlssFrameGen::X3:
        return "3x";
    case Settings::DlssFrameGen::X4:
        return "4x";
    case Settings::DlssFrameGen::Off:
    default:
        return "off";
    }
}

const char* ReflexRequest() {
    if (const char* value = std::getenv("EDEN_REFLEX"); value && value[0] != '\0') {
        switch (value[0]) {
        case '1':
            return "On (EDEN_REFLEX=1)";
        case '2':
            return "Boost (EDEN_REFLEX=2)";
        default:
            return "Off (EDEN_REFLEX=0)";
        }
    }
    switch (Settings::values.nvidia_reflex.GetValue()) {
    case Settings::NvidiaReflex::On:
        return "On";
    case Settings::NvidiaReflex::Boost:
        return "Boost";
    case Settings::NvidiaReflex::Off:
    default:
        return "Off";
    }
}

const char* FgTag(u32 multiplier) {
    switch (multiplier) {
    case 2:
        return "2x";
    case 3:
        return "3x";
    case 4:
        return "4x";
    default:
        return "off";
    }
}

} // Anonymous namespace

void PerfStats::LogRunStart() {
    auto& fg = Common::FrameGenPerf::Get();
    log_fg_rendered = fg.rendered.load(std::memory_order_relaxed);
    log_fg_generated = fg.generated.load(std::memory_order_relaxed);
    run_start = Clock::now();
    logged_run_start = true;
    LOG_INFO(Frontend,
             "Perf: run start pinning={} dlssg={} reflex={}",
             PinningRequest(), DlssgRequest(), ReflexRequest());
}

void PerfStats::MaybeLogWindow(const PerfStatsResults& results) {
    if (results.interval <= 0.0 || results.game_frames == 0) {
        return;
    }
    if (!logged_run_start) {
        LogRunStart();
    }
    auto& fg = Common::FrameGenPerf::Get();
    const u64 fg_rendered = fg.rendered.load(std::memory_order_relaxed);
    const u64 fg_generated = fg.generated.load(std::memory_order_relaxed);
    if (log_interval <= 0.0) {
        log_min_ft = results.min_frametime;
        log_max_ft = results.max_frametime;
    } else {
        log_min_ft = std::min(log_min_ft, results.min_frametime);
        log_max_ft = std::max(log_max_ft, results.max_frametime);
    }
    log_interval += results.interval;
    log_game_frames += results.game_frames;
    log_frames += results.game_frames;
    if (log_interval < 1.0) {
        return;
    }
    const double rendered_fps = log_game_frames / log_interval;
    const u64 gen = fg_generated - log_fg_generated;
    const double presented_fps = (log_game_frames + static_cast<double>(gen)) / log_interval;
    const double avg_ms = rendered_fps > 0.0 ? 1000.0 / rendered_fps : 0.0;
    const double t = duration_cast<DoubleSecs>(Clock::now() - run_start).count();
    const u32 multiplier = fg.multiplier.load(std::memory_order_relaxed);
    LOG_INFO(Frontend,
             "Perf: t={:.1f}s rendered={:.2f} fps presented={:.2f} fps frame={:.2f} ms "
             "min={:.2f} ms max={:.2f} ms frames={} fg={}",
             t, rendered_fps, presented_fps, avg_ms, log_min_ft * 1000.0, log_max_ft * 1000.0,
             log_frames, FgTag(multiplier));
    log_interval = 0.0;
    log_game_frames = 0.0;
    log_frametime_sum = 0.0;
    log_min_ft = 0.0;
    log_max_ft = 0.0;
    log_frames = 0;
    log_fg_rendered = fg_rendered;
    log_fg_generated = fg_generated;
}

void PerfStats::LogRunEnd() const {
    if (current_index <= IgnoreFrames) {
        return;
    }
    std::vector<double> times(perf_history.begin() + IgnoreFrames,
                              perf_history.begin() + current_index);
    std::sort(times.begin(), times.end());
    const double sum = std::accumulate(times.begin(), times.end(), 0.0);
    const double mean_ms = sum / static_cast<double>(times.size());
    const auto at = [&](double p) {
        const auto i = static_cast<std::size_t>(
            std::clamp(p * static_cast<double>(times.size() - 1), 0.0,
                       static_cast<double>(times.size() - 1)));
        return times[i];
    };
    const double p99_ms = at(0.99);
    const double p999_ms = at(0.999);
    const double max_ms = times.back();
    const auto fps = [](double ms) { return ms > 0.0 ? 1000.0 / ms : 0.0; };
    LOG_INFO(Frontend,
             "Perf: run end frames={} avg={:.2f} fps 1%_low={:.2f} fps 0.1%_low={:.2f} fps "
             "max={:.2f} ms",
             times.size(), fps(mean_ms), fps(p99_ms), fps(p999_ms), max_ms);
}

void SpeedLimiter::DoSpeedLimiting(microseconds current_system_time_us) {
    if (Settings::values.use_multi_core.GetValue() ||
        !Settings::SpeedLimitEnabled()) {
        return;
    }

    auto now = Clock::now();

    const double sleep_scale = Settings::SpeedLimit() / 100.0;

    // Max lag caused by slow frames. Shouldn't be more than the length of a frame at the current
    // speed percent or it will clamp too much and prevent this from properly limiting to that
    // percent. High values means it'll take longer after a slow frame to recover and start
    // limiting
    const microseconds max_lag_time_us = duration_cast<microseconds>(
        std::chrono::duration<double, std::chrono::microseconds::period>(25ms / sleep_scale));
    speed_limiting_delta_err += duration_cast<microseconds>(
        std::chrono::duration<double, std::chrono::microseconds::period>(
            (current_system_time_us - previous_system_time_us) / sleep_scale));
    speed_limiting_delta_err -= duration_cast<microseconds>(now - previous_walltime);
    speed_limiting_delta_err =
        std::clamp(speed_limiting_delta_err, -max_lag_time_us, max_lag_time_us);

    if (speed_limiting_delta_err > microseconds::zero()) {
        std::this_thread::sleep_for(speed_limiting_delta_err);
        auto now_after_sleep = Clock::now();
        speed_limiting_delta_err -= duration_cast<microseconds>(now_after_sleep - now);
        now = now_after_sleep;
    }

    previous_system_time_us = current_system_time_us;
    previous_walltime = now;
}

} // namespace Core
