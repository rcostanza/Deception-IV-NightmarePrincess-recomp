// SPDX-License-Identifier: GPL-2.0-or-later
#include "deception_settings.h"
#include "deception_graphics.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>

double DeceptionWindowRefreshRate(); // deception_gpu.cpp: the window's display, 0 when unknown

namespace DeceptionSettings {

namespace {

const char* Path() {
    const char* env = std::getenv("DECEPTION_CONFIG");
    return env && env[0] ? env : "settings.ini";
}

// The file read: Path() when it exists, else DECEPTION_CONFIG_DEFAULT (the launcher's: the one the
// setup wrote next to the exe, while Path() is the save folder's). Writes always go to Path().
const char* ReadPath() {
    if (FILE* file = std::fopen(Path(), "r")) {
        std::fclose(file);
        return Path();
    }
    const char* fallback = std::getenv("DECEPTION_CONFIG_DEFAULT");
    return fallback && fallback[0] ? fallback : Path();
}

bool Truthy(std::string_view value) {
    std::string lower;
    for (const char c : value) lower += char(std::tolower(static_cast<unsigned char>(c)));
    return lower == "1" || lower == "true" || lower == "yes" || lower == "on";
}

void Set(Values& v, const std::string& key, const std::string& value) {
    const int i = std::atoi(value.c_str());
    if (key == "display_mode") {
        v.display_mode = DeceptionGraphics::ParseDisplayMode(value);
    } else if (key == "vsync") {
        v.vsync = Truthy(value);
    } else if (key == "tearing") {
        v.tearing = Truthy(value);
    } else if (key == "monitor") {
        v.monitor = DeceptionGraphics::ParseMonitor(value);
    } else if (key == "output_resolution" || key == "window") { // window: its name before Deception
        const auto [w, h] = DeceptionGraphics::ParseWindowSize(value);
        v.window_width = w;
        v.window_height = h;
    } else if (key == "aspect_ratio") {
        int ratio = Aspect16x9;
        for (int r = 0; r < AspectRatioCount; ++r) {
            if (value == AspectRatioKeys[r]) ratio = r;
        }
        if (!value.empty() && value != AspectRatioKeys[ratio]) {
            std::printf("Settings: aspect_ratio=%s is not 4:3, 16:9, 21:9 or 32:9; using 16:9\n", value.c_str());
        }
        v.startup_aspect_ratio = v.aspect_ratio = ratio;
    } else if (key == "skip_intro") {
        v.skip_intro = Truthy(value);
    } else if (key == "sharpening") {
        v.sharpening = Truthy(value);
    } else if (key == "hdr") {
        v.startup_hdr = v.hdr = Truthy(value);
    } else if (key == "frame_rate") {
        // An older settings.ini's key: the game runs at its native 30 FPS (its logic steps once
        // per flip), so the line is dropped on the next save.
    } else if (key == "audio_channels") {
        v.audio_channels = ParseAudioChannels(value);
    } else if (key == "render_resolution") {
        // An older settings.ini's key: the game renders 1080p only (its engine sizes every target
        // at 1920x1080), so the line is dropped on the next save.
    } else {
        std::printf("Settings: unknown key %s\n", key.c_str());
    }
}

std::vector<std::pair<std::string, std::string>> DisplayKeys() {
    const auto& v = Get();
    const int monitor = v.monitor;
    return {
        {"display_mode", std::string(DeceptionGraphics::DisplayModeKeys[std::clamp(v.display_mode.load(), 0, 2)])},
        {"monitor", monitor < 0 ? std::string() : std::to_string(monitor)},
        {"aspect_ratio", std::string(AspectRatioKeys[std::clamp(v.aspect_ratio.load(), 0, AspectRatioCount - 1)])},
        {"output_resolution", std::to_string(v.window_width.load()) + "x" + std::to_string(v.window_height.load())},
        {"vsync", v.vsync ? "1" : "0"},
        {"tearing", v.tearing ? "1" : "0"},
        {"sharpening", v.sharpening ? "1" : "0"},
        {"hdr", v.hdr ? "1" : "0"},
    };
}

std::mutex save_mutex;
// The display keys as Path() holds them: loaded at start, updated by each save.
std::vector<std::pair<std::string, std::string>> saved_keys;

bool Exists(const char* path) {
    FILE* file = std::fopen(path, "r");
    if (file) std::fclose(file);
    return file != nullptr;
}

// save_mutex held. Writes the display keys into Path() in place, from ReadPath()'s lines.
void WriteDisplay() {
    auto keys = DisplayKeys();
    saved_keys = keys;
    std::vector<std::string> lines;
    if (FILE* file = std::fopen(ReadPath(), "r")) {
        char line[1024];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of("\r\n") + 1);
            lines.push_back(text);
        }
        std::fclose(file);
    }
    for (std::string& line : lines) {
        const auto eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        // An old window= line becomes output_resolution= (the same setting, renamed).
        const std::string name = line.substr(0, eq) == "window" ? "output_resolution" : line.substr(0, eq);
        const auto key = std::find_if(keys.begin(), keys.end(), [&](const auto& k) { return name == k.first; });
        if (key == keys.end()) continue;
        line = key->first + "=" + key->second;
        keys.erase(key);
    }
    for (const auto& [key, value] : keys) lines.push_back(key + "=" + value);
    FILE* file = std::fopen(Path(), "wb");
    if (!file) {
        std::printf("Settings: cannot write %s\n", Path());
        return;
    }
    for (const std::string& line : lines) std::fprintf(file, "%s\n", line.c_str());
    std::fclose(file);
    std::printf("Settings: saved display settings to %s\n", Path());
}

// Gives the save folder its own settings.ini at start: the default file copied as it is (the
// environment overrides stay out of it), or the display keys when there is none.
void CopyToSaveFolder() {
    const char* env = std::getenv("DECEPTION_CONFIG"); /* the launcher's or run.py's save folder */
    if (!env || !env[0] || Exists(Path())) return;
    const char* source = ReadPath();
    if (std::string_view(source) == Path()) {
        WriteDisplay();
        return;
    }
    FILE* in = std::fopen(source, "rb");
    if (!in) return;
    std::string data;
    char buffer[4096];
    for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), in)) > 0;) data.append(buffer, n);
    std::fclose(in);
    FILE* out = std::fopen(Path(), "wb");
    if (!out) {
        std::printf("Settings: cannot write %s\n", Path());
        return;
    }
    const bool ok = std::fwrite(data.data(), 1, data.size(), out) == data.size();
    if (std::fclose(out) != 0 || !ok) {
        std::printf("Settings: cannot write %s\n", Path());
        std::remove(Path());
        return;
    }
    std::printf("Settings: copied %s to %s\n", source, Path());
}

} // namespace

Values& Get() {
    static Values values;
    return values;
}

void Load() {
    auto& v = Get();
    const char* path = ReadPath();
    if (FILE* file = std::fopen(path, "r")) {
        char line[1024];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            const auto eq = text.find('=');
            if (text.empty() || text[0] == '#' || eq == std::string::npos) {
                continue;
            }
            Set(v, text.substr(0, eq), text.substr(eq + 1));
        }
        std::fclose(file);
        std::printf("Settings: %s\n", path);
        if (std::string_view(path) != Path()) std::printf("Settings: changes are saved to %s\n", Path());
    }
    const std::pair<const char*, const char*> env_keys[] = {
        {"DECEPTION_DISPLAY_MODE", "display_mode"}, {"DECEPTION_VSYNC", "vsync"},
        {"DECEPTION_TEARING", "tearing"},
        {"DECEPTION_MONITOR", "monitor"},           {"DECEPTION_WINDOW", "output_resolution"},
        {"DECEPTION_OUTPUT_RESOLUTION", "output_resolution"},
        {"DECEPTION_ASPECT_RATIO", "aspect_ratio"},
        {"DECEPTION_AUDIO_CHANNELS", "audio_channels"},
        {"DECEPTION_SKIP_INTRO", "skip_intro"},
        {"DECEPTION_HDR", "hdr"},
        {"DECEPTION_SHARPENING", "sharpening"},
    };
    for (const auto& [env, key] : env_keys) {
        if (const char* value = std::getenv(env); value && value[0]) {
            Set(v, key, value);
        }
    }
    v.startup_monitor = v.monitor;
    std::scoped_lock lock{save_mutex};
    saved_keys = DisplayKeys();
    CopyToSaveFolder();
}

void SaveDisplay() {
    std::scoped_lock lock{save_mutex};
    WriteDisplay();
}

void SaveDisplayIfChanged() {
    std::scoped_lock lock{save_mutex};
    if (DisplayKeys() != saved_keys) WriteDisplay();
}

double DisplayRefresh() {
    static std::atomic<double> refresh{0.0};
    static std::atomic<long long> next_check{0};
    const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    if (now >= next_check.load(std::memory_order_relaxed)) {
        next_check.store(now + 500, std::memory_order_relaxed);
        refresh.store(DeceptionWindowRefreshRate(), std::memory_order_relaxed);
    }
    return refresh.load(std::memory_order_relaxed);
}

} // namespace DeceptionSettings

extern "C" int deception_skip_intro(void) {
    return DeceptionSettings::Get().skip_intro ? 1 : 0;
}

static std::atomic<int> vblank_boost{0};
extern "C" void deception_set_vblank_boost(int hz) { vblank_boost.store(hz, std::memory_order_relaxed); }
extern "C" int deception_vblank_boost(void) { return vblank_boost.load(std::memory_order_relaxed); }

extern "C" int deception_hdr(void) {
    return DeceptionSettings::Get().startup_hdr ? 1 : 0;
}

