// deception: shadPS4 settings used by the video core, read once from DECEPTION_* environment
// variables (defaults match shadPS4 except the pipeline cache, which is on).
#pragma once
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>
#include "common/types.h"

double DeceptionWindowRefreshRate(); // deception_gpu.cpp: exact rate of the window's display, 0 when unknown
bool DeceptionWaitForVblank(); // deception_gpu.cpp: waits for the window's display to start a refresh; false when it cannot (not Windows)
extern "C" int deception_hdr(void); // deception_settings.cpp: settings.ini hdr
extern "C" int deception_frame_rate(void); // deception_settings.cpp: frame_rate (30, 60, 90, 120, 144, 180)
extern "C" int deception_vblank_boost(void); // deception_settings.cpp: skip_intro's fast logos (0: off)

enum GpuReadbacksMode : int { Disabled, Relaxed, Precise };

class EmulatorSettingsImpl {
public:
    static EmulatorSettingsImpl* GetInstance() { static EmulatorSettingsImpl s; return &s; }
    static bool Flag(const char* name, bool fallback) {
        const char* v = std::getenv(name);
        return v ? (v[0] == '1' || v[0] == 'y' || v[0] == 't') : fallback;
    }
    static long Number(const char* name, long fallback) {
        const char* v = std::getenv(name);
        return v ? std::strtol(v, nullptr, 10) : fallback;
    }
    s32 GetGpuId() { static const auto value = s32(Number("DECEPTION_GPU_ID", -1)); return value; }
    // deception: the video out size: the game's 1920x1080 (it has no PS4 Pro mode).
    u32 GetInternalScreenWidth() { static const auto value = u32(Number("DECEPTION_INTERNAL_WIDTH", 1920)); return value; }
    u32 GetInternalScreenHeight() { static const auto value = u32(Number("DECEPTION_INTERNAL_HEIGHT", 1080)); return value; }
    // deception: Relaxed by default: without readbacks, guest code that reads GPU-written
    // vertices sees stale data; Relaxed costs no measurable frame rate.
    u32 GetReadbacksMode() { static const auto value = u32(Number("DECEPTION_READBACKS", GpuReadbacksMode::Relaxed)); return value; }
    // deception: the PS4's 60 Hz vblank. Deception sets a flip rate of 1 (a flip every second
    // vblank, 30 FPS) and steps its game logic once a frame, so any other rate changes the game's
    // speed; skip_intro's fast logos raise it for a moment (deception_vblank_boost).
    // DECEPTION_VBLANK_HZ fixes it (measurements).
    u32 GetVblankFrequency() {
        static const long fixed = Number("DECEPTION_VBLANK_HZ", -1);
        if (const int boost = deception_vblank_boost(); boost > 0) return u32(boost);
        return fixed > 0 ? u32(fixed) : 60u;
    }
    // The vblank period. A display within 0.5 Hz of the vblank rate (a 59.95 Hz panel at the
    // default 60) sets it exactly: at 60.00 Hz the game made 0.05 frames a second more than the
    // display shows, so after ~35 s the VSync (Fifo) queue was full and the GPU thread waited on
    // the display for 4.9 s of every 5 (frames of extra latency, 33 ms frames). The display is
    // the one the window opened on. An explicit DECEPTION_VBLANK_HZ is used as given.
    std::chrono::nanoseconds GetVblankPeriod() {
        static const bool snap = std::getenv("DECEPTION_VBLANK_HZ") == nullptr;
        return DisplayPeriod(GetVblankFrequency(), snap);
    }
    static std::chrono::nanoseconds DisplayPeriod(double hz, bool snap) {
        static const double display = DeceptionWindowRefreshRate();
        return std::chrono::nanoseconds(s64(1e9 / (snap && std::abs(display - hz) < 0.5 ? display : hz) + 0.5));
    }
    bool IsCopyGpuBuffers() { static const auto value = Flag("DECEPTION_COPY_GPU_BUFFERS", false); return value; }
    bool IsDirectMemoryAccessEnabled() { static const auto value = Flag("DECEPTION_DIRECT_MEMORY_ACCESS", false); return value; }
    bool IsDumpShaders() { static const auto value = Flag("DECEPTION_DUMP_SHADERS", false); return value; }
    // deception: settings.ini hdr (DECEPTION_HDR), read once at start (deception_hdr).
    bool IsHdrAllowed() { static const bool value = deception_hdr() != 0; return value; }
    bool IsNullGPU() { static const auto value = Flag("DECEPTION_NULL_GPU", false); return value; }
    bool IsPatchShaders() { return false; }
    bool IsPipelineCacheArchived() { return false; }
    bool IsPipelineCacheEnabled() { static const auto value = Flag("DECEPTION_PIPELINE_CACHE", true); return value; }
    bool IsReadbackLinearImagesEnabled() { static const auto value = Flag("DECEPTION_READBACK_LINEAR", false); return value; }
    bool IsRenderdocEnabled() { return false; }
    bool IsShaderCollect() { return false; }
    bool IsVkCrashDiagnosticEnabled() { return false; }
    bool IsVkGuestMarkersEnabled() { static const auto value = Flag("DECEPTION_VK_MARKERS", false); return value; }
    bool IsVkHostMarkersEnabled() { static const auto value = Flag("DECEPTION_VK_MARKERS", false); return value; }
    bool IsVkValidationCoreEnabled() { return true; }
    bool IsVkValidationEnabled() { static const auto value = Flag("DECEPTION_VK_VALIDATION", false); return value; }
    bool IsVkValidationGpuEnabled() { return false; }
    bool IsVkValidationSyncEnabled() { static const auto value = Flag("DECEPTION_VK_VALIDATION_SYNC", false); return value; }
};
#define EmulatorSettings (*EmulatorSettingsImpl::GetInstance())
