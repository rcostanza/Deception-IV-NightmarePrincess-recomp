// SPDX-License-Identifier: GPL-2.0-or-later
// User settings, read once at start from settings.ini (DECEPTION_CONFIG overrides the path) and then
// from environment variables, which win. One "key=value" per line, '#' starts a comment.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "deception_graphics.h"

namespace DeceptionSettings {

/// Speaker layout the game's 7.1 audio is mixed down to (src/runtime_audio.c): the playback
/// device's own, stereo, 5.1 or 7.1. settings.ini and DECEPTION_AUDIO_CHANNELS name them as below.
enum AudioChannels : int { AudioChannelsAuto = 0, AudioStereo, Audio51, Audio71, AudioChannelsCount };
inline constexpr std::string_view AudioChannelKeys[AudioChannelsCount] = {"auto", "stereo", "5.1", "7.1"};
/// A layout key (also "2.0", "6" and "8"); anything else is automatic.
inline int ParseAudioChannels(std::string_view value) {
    for (int c = 0; c < AudioChannelsCount; ++c) {
        if (value == AudioChannelKeys[c]) return c;
    }
    if (value == "2.0" || value == "2") return AudioStereo;
    if (value == "6") return Audio51;
    if (value == "8") return Audio71;
    return AudioChannelsAuto;
}


/// "21:9" and the like for a size (within 3%: 2560x1080 and 3440x1440 are 21:9), else "2.10:1".
inline std::string AspectLabel(int w, int h) {
    static constexpr struct { double ratio; const char* name; } known[] = {
        {5.0 / 4, "5:4"}, {4.0 / 3, "4:3"}, {16.0 / 10, "16:10"}, {16.0 / 9, "16:9"},
        {21.0 / 9, "21:9"}, {32.0 / 10, "32:10"}, {32.0 / 9, "32:9"},
    };
    if (w <= 0 || h <= 0) return "";
    const double ratio = double(w) / h;
    for (const auto& k : known) {
        if (std::abs(ratio / k.ratio - 1.0) <= 0.03) return k.name;
    }
    char text[16];
    std::snprintf(text, sizeof text, "%.2f:1", ratio);
    return text;
}

/// The picture's shape (key aspect_ratio, DECEPTION_ASPECT_RATIO): 16:9 (the PS4's, the default),
/// 4:3, 21:9 or 32:9. A window takes sizes of that shape; the fullscreen modes pick one from the screen.
enum AspectRatio : int { Aspect4x3, Aspect16x9, Aspect21x9, Aspect32x9, AspectRatioCount };
inline constexpr std::string_view AspectRatioKeys[AspectRatioCount] = {"4:3", "16:9", "21:9", "32:9"};
inline constexpr double AspectRatioValues[AspectRatioCount] = {4.0 / 3.0, 16.0 / 9.0, 21.0 / 9.0, 32.0 / 9.0};

/// The AspectRatio a size is (as AspectLabel names it: 2560x1080 and 3440x1440 are 21:9), -1 none.
inline int AspectRatioOf(int w, int h) {
    const std::string label = AspectLabel(w, h);
    for (int r = 0; r < AspectRatioCount; ++r) {
        if (label == AspectRatioKeys[r]) return r;
    }
    return -1;
}

/// The largest size of `ratio` within w x h, in even pixels: w x h itself when it has that shape.
inline std::pair<int, int> FitAspect(int w, int h, int ratio) {
    if (w <= 0 || h <= 0 || AspectRatioOf(w, h) == ratio) return {w, h};
    const double aspect = AspectRatioValues[std::clamp(ratio, 0, AspectRatioCount - 1)];
    if (double(w) / h > aspect) return {int(h * aspect / 2.0 + 0.5) * 2, h};
    return {w, int(w / aspect / 2.0 + 0.5) * 2};
}

/// What a fullscreen mode shows for `ratio` on a screen with this desktop and these fullscreen
/// modes: `mode` is the display mode to switch to ({0, 0}: keep the desktop's) and `picture` the
/// game's size within it. Borderless keeps the desktop and fits the shape in it; exclusive
/// fullscreen takes the largest mode of that shape, else the desktop's with the shape fitted.
struct FullscreenChoice {
    std::pair<int, int> mode, picture;
};
inline FullscreenChoice PickFullscreen(bool exclusive, int ratio, std::pair<int, int> desktop,
                                       const std::vector<std::pair<int, int>>& modes) {
    if (exclusive) {
        std::pair<int, int> best{0, 0};
        for (const auto& m : modes) {
            if (AspectRatioOf(m.first, m.second) == ratio && m.first * m.second > best.first * best.second) best = m;
        }
        if (best.first > 0) return {best, best};
    }
    return {{0, 0}, FitAspect(desktop.first, desktop.second, ratio)};
}

/// The size of `sizes` (all of one shape) nearest in height to `height`; {0, 0} when empty.
inline std::pair<int, int> NearestHeight(const std::vector<std::pair<int, int>>& sizes, int height) {
    std::pair<int, int> best{0, 0};
    for (const auto& s : sizes) {
        if (!best.first || std::abs(s.second - height) < std::abs(best.second - height)) best = s;
    }
    return best;
}

struct Values {
    /// DeceptionGraphics::DisplayMode (key display_mode: windowed, borderless, fullscreen);
    /// borderless by default, so the picture is scaled to the desktop resolution.
    std::atomic<int> display_mode{DeceptionGraphics::Borderless};
    std::atomic<bool> vsync{true};
    /// With VSync off: present at once, tearing allowed (Vulkan Immediate), instead of the newest
    /// frame at each refresh (Mailbox, which some drivers hold to twice the refresh rate).
    std::atomic<bool> tearing{false};
    /// SDL display index the window opens on, -1 the primary (key monitor, DECEPTION_MONITOR).
    std::atomic<int> monitor{-1};
    int startup_monitor = -1;
    std::vector<int> monitors; ///< SDL indices, populated before the guest starts
    std::vector<std::string> monitor_names; ///< SDL's name of each of `monitors`
    /// AspectRatio (key aspect_ratio): the shape of the window's sizes and of what the fullscreen
    /// modes pick. The game takes the shape it starts with (startup_width_4k).
    std::atomic<int> aspect_ratio{Aspect16x9};
    int startup_aspect_ratio = Aspect16x9;
    /// Window size (key output_resolution, "1920x1080"), of aspect_ratio's shape (another is moved
    /// to the nearest of `resolutions` at start). The window follows changes.
    std::atomic<int> window_width{1920}, window_height{1080};
    /// Window sizes of each AspectRatio that fit the start display's desktop, largest first: its
    /// fullscreen modes of that shape and the common sizes of it. Listed before the guest starts.
    std::vector<std::pair<int, int>> resolutions[AspectRatioCount];
    /// The start display's fullscreen modes and desktop size (PickFullscreen's screen).
    std::vector<std::pair<int, int>> modes;
    int desktop_width = 0, desktop_height = 0;
    /// What the fullscreen modes show now (PickFullscreen's picture), set by the window thread.
    std::atomic<int> fullscreen_width{0}, fullscreen_height{0};
    /// Hurry through the start-up logos and confirm the autosave notice (src/runtime_intro.c);
    /// read once, before the game starts. On by default.
    bool skip_intro = true;
    /// The game's frame rate: 30, 60, 90, 120, 144 or 180 (DeceptionGraphics::FrameRates). The PS4 vblank
    /// (videoout driver) and the engine's target frame rate (src/runtime_game.c, at each flip)
    /// follow it, so the game speed stays the same; the Display Settings panel changes it live.
    std::atomic<int> frame_rate{60};
    /// Speaker layout of the game's audio (AudioChannels), read when the game opens its ports.
    std::atomic<int> audio_channels{AudioChannelsAuto};
    /// FSR 1 RCAS sharpening of the picture as the window shows it (key sharpening,
    /// DECEPTION_SHARPENING; on by default): the presenter reads it at each frame.
    std::atomic<bool> sharpening{true};
    /// HDR output (key hdr, DECEPTION_HDR; off by default): the game is told the display takes
    /// BT.2020 PQ and switches its own output to HDR. Read once, before the game starts.
    bool startup_hdr = false;
    std::atomic<bool> hdr{false};
    /// Whether the window's display offers an HDR10 surface: -1 not known yet, set by the
    /// swapchain (vk_swapchain.cpp).
    std::atomic<int> hdr_display{-1};
};

Values& Get();

/// Reads the file, then the environment overrides. Called once at start. When the save folder has
/// no settings.ini yet (Path()), the default file is copied there so it always holds one.
void Load();

/// Writes the display keys (display_mode, monitor, aspect_ratio, window, vsync, tearing, frame_rate,
/// sharpening, hdr) into Path() in place: their lines are replaced, missing ones
/// appended, the rest kept (from the default file until Path() exists).
void SaveDisplay();

/// SaveDisplay() when a display key differs from what Path() holds: called as the game closes, so
/// a change made on a page that was still open is not lost.
void SaveDisplayIfChanged();

/// Refresh rate of the window's display in Hz (0 unknown), looked up at most every 500 ms.
double DisplayRefresh();

/// The frame rate the game runs at: frame_rate, lowered to the highest of FrameRates the window's
/// display refreshes at, since a frame shown later than the engine's target slows the game (it
/// does not skip frames). VSync off with tearing is not bound to the refresh. Any thread.
int EffectiveFrameRate();

} // namespace DeceptionSettings
