// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>
#include <string_view>

#include "common/oot_4k.h"
#include "common/settings_enums.h"

namespace VideoCommon {

enum class OcarinaUpscaleBackend { Native, Fsr, Dlss };

constexpr OcarinaUpscaleBackend SelectOcarinaUpscaler(bool fsr_enabled, bool dlss_enabled,
                                                    bool dlss_available,
                                                    std::string_view override) {
    if (override == "native") {
        return OcarinaUpscaleBackend::Native;
    }
    if (override == "fsr") {
        return OcarinaUpscaleBackend::Fsr;
    }
    if ((dlss_enabled || override == "dlss") && dlss_available) {
        return OcarinaUpscaleBackend::Dlss;
    }
    return fsr_enabled ? OcarinaUpscaleBackend::Fsr : OcarinaUpscaleBackend::Native;
}

/// Whether the "OoT anti-aliasing" choice asks for AMD FSR (3.1, or 4 with FSR 3.1 as fallback).
constexpr bool OcarinaFsrChosen(Settings::OotAntiAliasing choice) {
    return choice == Settings::OotAntiAliasing::Fsr3 || choice == Settings::OotAntiAliasing::Fsr4;
}

/// The backend for the "OoT anti-aliasing" choice and the NVIDIA DLSS setting. An FSR choice wins
/// over DLSS; with Game or No sharpening, DLSS replaces the game's pass when it's on and available
/// (as before FSR came in).
constexpr OcarinaUpscaleBackend SelectOcarinaUpscaler(Settings::OotAntiAliasing choice,
                                                    bool dlss_enabled, bool dlss_available,
                                                    std::string_view override) {
    const bool fsr = OcarinaFsrChosen(choice);
    return SelectOcarinaUpscaler(fsr, dlss_enabled && !fsr, dlss_available, override);
}

/// DLSS jitter for Ocarina (boot): the game renders without jitter, so Fritchii offsets the scene
/// draws' viewport by a Halton (2, 3) sub-pixel amount each frame and hands DLSS the offset;
/// without it DLSS (DLAA included) sees the same samples every frame and can't resolve edges.
/// 1 (default) = the offset as FSR gets it: the content's shift in render pixels, x right and y
/// down. That is the convention of FSR's projection-jitter sample and of NVIDIA's DLSS guide
/// (3.7.3: "the same co-ordinate and direction system as motion vectors", [0,0] at the top left),
/// and engines (Unreal's FSR and DLSS plug-ins) hand both the same TemporalJitterPixels.
/// 2 = y negated, for when edges wobble or crawl with 1. 0 = off (the old unjittered input).
/// EDEN_NX2_DLSS_JITTER=0/1/2 overrides the default.
inline int OcarinaDlssJitterMode() {
    static const int mode = [] {
        const char* value = std::getenv("EDEN_NX2_DLSS_JITTER");
        if (value == nullptr || *value == '\0') {
            return 1;
        }
        const int parsed = std::atoi(value);
        return parsed >= 0 && parsed <= 2 ? parsed : 1;
    }();
    return mode;
}

/// Whether EDEN_NX2_DLSS_JITTER set the jitter mode (for the log line).
inline bool OcarinaDlssJitterOverridden() {
    const char* value = std::getenv("EDEN_NX2_DLSS_JITTER");
    return value != nullptr && *value != '\0';
}

/// EDEN_NX2_OOT_DLAA_1440P=1 (boot): DLSS Native (DLAA) keeps Ocarina's 2560x1440 scene at
/// 2560x1440 (the window adapting filter then stretches it), as before. By default Native
/// upscales to 3840x2160 instead: DLAA at the output size needs a 4K scene, and Ocarina renders
/// at most 1440p (resolution scaling is off while DLSS replaces its upscale).
inline bool OcarinaDlaaAt1440p() {
    static const bool keep = [] {
        const char* value = std::getenv("EDEN_NX2_OOT_DLAA_1440P");
        return value != nullptr && *value == '1';
    }();
    return keep;
}

/// Texture LOD bias added to Ocarina's mipmapped samplers while FSR upscales. AMD recommends
/// log2(render / output) - 1: -1.58 for docked 1440p -> 4K. With jittered DLSS (the default),
/// NVIDIA's log2(render / output): -0.58 for 1440p -> 4K, the gentler end of the DLSS guide's
/// range (3.5), which also stays inside its [-1, 0] for DLAA at 1440p.
inline float OcarinaFsrLodBias(OcarinaUpscaleBackend backend) {
    if (backend == OcarinaUpscaleBackend::Fsr) {
        return -1.58f;
    }
    // 4K DLAA (EDEN_NX2_OOT_4K=1): render and output are both 3840x2160, so log2(1) = 0.
    static const bool dlaa_4k = Common::Oot4kDlaaRequested();
    if (backend == OcarinaUpscaleBackend::Dlss && dlaa_4k) {
        return 0.0f;
    }
    return backend == OcarinaUpscaleBackend::Dlss && OcarinaDlssJitterMode() != 0 ? -0.58f : 0.0f;
}

/// Whether a sampler with that bias keeps it for an image: only docked (handheld isn't measured),
/// not for images the GPU wrote (render targets would read a mip too sharp), and not once the
/// upscaler failed this session (the game's own pass runs again).
constexpr bool KeepOcarinaFsrLodBias(bool docked, bool upscaler_failed, bool gpu_written) {
    return docked && !upscaler_failed && !gpu_written;
}

} // namespace VideoCommon
