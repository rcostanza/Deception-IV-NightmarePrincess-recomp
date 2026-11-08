// SPDX-License-Identifier: GPL-2.0-or-later
// The system UI the PS4 draws over a game (save list, text entry, trophy and screenshot notices) and
// the shader-cache notes, drawn with Dear ImGui by the presenter after the game frame.
#include "deception_overlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>
#include "deception_graphics.h"
#include "deception_settings.h"
#include "imgui.h"
#include "imgui_internal.h" // RegisterUserTexture (save pictures)
#include "imgui_impl_vulkan.h"
#include "../deception_gpu.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

// DejaVu Sans, embedded (third_party/fonts, Bitstream Vera license).
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global deception_font_ttf\n"
    "deception_font_ttf:\n"
    ".incbin \"" DECEPTION_FONT_PATH "\"\n"
    ".global deception_font_ttf_end\n"
    "deception_font_ttf_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden deception_font_ttf\n"
    ".global deception_font_ttf\n"
    "deception_font_ttf:\n"
    ".incbin \"" DECEPTION_FONT_PATH "\"\n"
    ".hidden deception_font_ttf_end\n"
    ".global deception_font_ttf_end\n"
    "deception_font_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char deception_font_ttf[];
extern "C" const unsigned char deception_font_ttf_end[];

namespace DeceptionOverlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
float base_scale = 1.0f;

namespace Ink {
constexpr ImVec4 Rgb(int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); }
constexpr ImVec4 Bg = Rgb(0x10, 0x12, 0x16), Panel = Rgb(0x18, 0x1b, 0x21), Card = Rgb(0x1f, 0x23, 0x2b);
constexpr ImVec4 Line = Rgb(0x2e, 0x34, 0x3f), Text = Rgb(0xe8, 0xea, 0xee), Muted = Rgb(0x8c, 0x93, 0xa0);
constexpr ImVec4 Accent = Rgb(0x3d, 0x7c, 0xd9), AccentDeep = Rgb(0x1c, 0x33, 0x55);
} // namespace Ink

std::atomic<bool> loading{false};
std::atomic<u32> loading_done{0}, loading_total{0};
std::atomic<u32> background_done{0}, background_total{0};
std::atomic<u32> prefetch_done{0}, prefetch_total{0};
std::atomic<s64> prefetch_until{0}; // "ready" stays this long after the last program
// Shader translations: in progress, and started since the note last appeared.
std::atomic<u32> compile_pending{0}, compile_count{0}, compile_waits{0};
std::atomic<s64> compile_until{0};
// A short message in the lower right corner (trophy, screenshot), shown until notice_until.
std::mutex notice_mutex;
std::string notice_text;
std::atomic<s64> notice_until{0}; // steady_clock milliseconds
// The game's text dialog (SetTextEntry), guarded by imgui_mutex.
bool text_entry_active = false;
std::string text_entry_prompt, text_entry_text;
// The game's save list (sceSaveDataDialog): a choice made with the arrows/D-pad.
std::atomic<bool> choice_active{false};
std::string choice_title;
std::string choice_preview_label;
std::vector<std::string> choice_items;
int choice_index = 0;
std::atomic<int> choice_result{-1}; // -1 open, -2 cancelled, else the chosen index
// Save pictures: decoded by BeginChoice, made textures on the present thread (null = none).
// Closed lists' textures wait in choice_retired until the backend has destroyed them.
std::vector<DeceptionPng::Image> choice_pictures;
std::vector<ImTextureData*> choice_textures, choice_retired;
std::atomic<bool> choice_retiring{false};
// The start-up picture (SetSplash): decoded at start, made a texture on the present thread (both
// guarded by imgui_mutex), retired through choice_retired once faded out.
enum SplashState : int { SplashNone, SplashShown, SplashFading, SplashDone };
constexpr s64 kSplashFadeMs = 400, kSplashMaxMs = 60000;
DeceptionPng::Image splash_picture;
ImTextureData* splash_texture = nullptr;
std::atomic<int> splash_state{SplashNone}, splash_lit_frames{0};
std::atomic<s64> splash_since{0}, splash_fade_from{0};
std::chrono::steady_clock::time_point last_present{};
// The Display Settings panel (F10, or Select held for 2 s: src/runtime_pad.c). The row and
// dirty flag are guarded by imgui_mutex; the values live in DeceptionSettings.
std::atomic<bool> display_open{false};

int display_row = 0;
bool display_dirty = false;
// An aspect ratio and window size being chosen on the panel: the window takes them once the rows
// have rested for a second (or the panel closes), not at each step. -1 and {0, 0} when none.
int pending_aspect = -1;
std::pair<int, int> pending_resolution{0, 0};
std::chrono::steady_clock::time_point pending_since{};
constexpr auto kResolutionSettle = std::chrono::seconds(1);
// The exit prompt (F4, or Back on the title screen: src/runtime_pad.c); its focus (0 No, 1 Yes) is
// guarded by imgui_mutex.
std::atomic<bool> exit_open{false};
int exit_focus = 0;
enum DisplayRow : int { RowAspect, RowMode, RowResolution, RowMonitor, RowVsync, RowFps, RowRender, RowSharpen, RowHdr, RowCount };

// The colours of the game's own menus (Main Menu, Options): teal bars on a dark, see-through
// backdrop inside a thin cyan frame.
namespace PageInk {
constexpr ImU32 Rgb(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
constexpr ImU32 Shade = Rgb(0x02, 0x08, 0x0e, 0xdc), Panel = Rgb(0x04, 0x16, 0x1e, 0xd8);
constexpr ImU32 Frame = Rgb(0x8c, 0xec, 0xf4, 0xe6), FrameDim = Rgb(0x3a, 0x9c, 0xb0, 0xa0);
constexpr ImU32 Row = Rgb(0x0c, 0x4c, 0x5c, 0xe0), RowLow = Rgb(0x06, 0x2c, 0x38, 0xe0);
// The overlay draws to an sRGB image: these are linear, Focus shows as #2ac0d6 to #0a6e86.
constexpr ImU32 Focus = Rgb(0x06, 0x86, 0xab), FocusLow = Rgb(0x01, 0x28, 0x3c);
constexpr ImU32 Text = Rgb(0xff, 0xff, 0xff), TextDim = Rgb(0xc4, 0xe8, 0xee), Shadow = Rgb(0x00, 0x18, 0x22, 0xc0);
constexpr ImU32 Help = Rgb(0xb0, 0xe4, 0xec);
// A setting that does not apply in the current mode: greyed out and passed over by Up/Down.
constexpr ImU32 RowOff = Rgb(0x0a, 0x26, 0x2e, 0xb0), RowOffLow = Rgb(0x05, 0x16, 0x1c, 0xb0), TextOff = Rgb(0x5c, 0x7c, 0x84);
} // namespace PageInk

s64 NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// The game's text dialog: what is typed, and how to finish (keyboard or controller).
void TextEntryBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.42f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.32f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("##text_entry", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(Ink::Accent, "%s", text_entry_prompt.c_str());
    ImGui::Separator();
    ImGui::SetWindowFontScale(1.4f);
    ImGui::Text("%s_", text_entry_text.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    ImGui::TextDisabled("Keyboard: type, Backspace to erase, Enter = OK, Esc = cancel");
    ImGui::TextDisabled("Controller: Cross (A) = OK, Circle (B) = cancel");
    ImGui::End();
}

// One save (or one answer) of the game's save list as a card: its picture on the left when the list
// has pictures, then its lines: the first is the name, the rest in grey. `draw` false only
// measures. Returns the card's height.
float SaveCard(ImVec2 at, float width, const std::string& text, ImTextureData* picture, bool pictures, bool selected,
               bool draw) {
    const float px = base_scale;
    const float pad = 12.0f * px, gap = 16.0f * px;
    const ImVec2 picture_size(192.0f * px, 108.0f * px);
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize(), name_size = size * 1.15f;
    const float text_x = pad + (pictures ? picture_size.x + gap : 0.0f);
    const float wrap = std::max(width - text_x - pad, 40.0f * px);
    std::vector<std::string_view> lines;
    for (size_t start = 0; start <= text.size();) {
        const size_t end = std::min(text.find('\n', start), text.size());
        lines.emplace_back(text.data() + start, end - start);
        start = end + 1;
    }
    const auto line_gap = [&](size_t i) { return i == 0 && lines.size() > 1 ? 4.0f * px : 1.0f * px; };
    float text_h = 0.0f;
    for (size_t i = 0; i < lines.size(); ++i) {
        const char* begin = lines[i].data();
        text_h += font->CalcTextSizeA(i == 0 ? name_size : size, FLT_MAX, wrap, begin, begin + lines[i].size()).y +
                  line_gap(i);
    }
    const float height = std::max(pictures ? picture_size.y : 0.0f, text_h) + pad * 2.0f;
    if (!draw) {
        return height;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 max(at.x + width, at.y + height);
    const float rounding = 6.0f * px;
    dl->AddRectFilled(at, max, ImGui::GetColorU32(selected ? Ink::AccentDeep : Ink::Card), rounding);
    dl->AddRect(at, max, ImGui::GetColorU32(selected ? Ink::Accent : Ink::Line), rounding, 0,
                (selected ? 2.0f : 1.0f) * px);
    if (pictures) {
        const ImVec2 p0(at.x + pad, at.y + (height - picture_size.y) * 0.5f);
        const ImVec2 p1(p0.x + picture_size.x, p0.y + picture_size.y);
        if (picture) {
            dl->AddImage(picture->GetTexRef(), p0, p1);
        } else { // the new save: no picture yet
            dl->AddRectFilled(p0, p1, ImGui::GetColorU32(Ink::Panel), 4.0f * px);
        }
        dl->AddRect(p0, p1, ImGui::GetColorU32(Ink::Line), 4.0f * px, 0, 1.0f * px);
    }
    float y = at.y + (height - text_h) * 0.5f;
    for (size_t i = 0; i < lines.size(); ++i) {
        const float s = i == 0 ? name_size : size;
        const char* begin = lines[i].data();
        const char* end = begin + lines[i].size();
        dl->AddText(font, s, ImVec2(at.x + text_x, y), ImGui::GetColorU32(i == 0 ? Ink::Text : Ink::Muted), begin,
                    end, wrap);
        y += font->CalcTextSizeA(s, FLT_MAX, wrap, begin, end).y + line_gap(i);
    }
    return height;
}

// The game's save list (sceSaveDataDialog): the action as the heading, then a card per save.
// Asked to overwrite, the new save is shown first (its picture and the details of the one it
// replaces), then the answers.
void ChoiceBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float px = base_scale;
    const float width = std::min(760.0f * px, viewport->WorkSize.x * 0.92f);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.97f);
    ImGui::Begin("##choice", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    const bool confirm = !choice_preview_label.empty();
    const size_t details = confirm ? choice_title.find("\n\n") : std::string::npos;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::SetWindowFontScale(1.5f);
    ImGui::TextColored(Ink::Text, "%s", choice_title.substr(0, details).c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopTextWrapPos();
    ImGui::Separator();
    const float inner = ImGui::GetContentRegionAvail().x;
    const float card_gap = 8.0f * px;
    if (confirm) {
        std::string text = choice_preview_label;
        if (details != std::string::npos) text += "\n" + choice_title.substr(details + 2);
        ImTextureData* preview = choice_textures.size() > choice_items.size() ? choice_textures.back() : nullptr;
        const float h = SaveCard(ImGui::GetCursorScreenPos(), inner, text, preview, true, false, true);
        ImGui::Dummy(ImVec2(inner, h + card_gap));
    }
    const int count = int(choice_items.size());
    const size_t listed = std::min(choice_textures.size(), choice_items.size());
    const bool pictures = !confirm && std::any_of(choice_textures.begin(), choice_textures.begin() + listed,
                                                  [](ImTextureData* tex) { return tex != nullptr; });
    const auto picture_of = [&](int i) { return pictures && i < int(listed) ? choice_textures[i] : nullptr; };
    float total = 0.0f;
    for (int i = 0; i < count; ++i) {
        total += SaveCard(ImVec2(), inner, choice_items[i], picture_of(i), pictures, false, false) +
                 (i + 1 < count ? card_gap : 0.0f);
    }
    // A long list scrolls in its own area, between the heading and the keys.
    const float most = viewport->WorkSize.y * 0.86f - ImGui::GetCursorPosY() - ImGui::GetFrameHeightWithSpacing() * 2.0f;
    ImGui::BeginChild("##saves", ImVec2(inner, std::min(total, std::max(most, 120.0f * px))), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
    for (int i = 0; i < count; ++i) {
        const bool selected = i == choice_index;
        const float h = SaveCard(ImGui::GetCursorScreenPos(), inner, choice_items[i], picture_of(i), pictures,
                                 selected, true);
        ImGui::Dummy(ImVec2(inner, h));
        if (selected) {
            ImGui::SetScrollHereY(0.5f);
        }
        if (i + 1 < count) {
            ImGui::Dummy(ImVec2(inner, card_gap));
        }
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::Spacing();
    ImGui::TextDisabled("Up/Down or D-pad: choose   Enter or Cross (A): OK   Esc or Circle (B): cancel");
    ImGui::End();
}

template <typename T, std::size_t N>
int StepValue(const std::array<T, N>& values, int current, int delta) {
    const int i = DeceptionGraphics::IndexOf(values, current, 0);
    return values[(i + delta + int(N)) % int(N)];
}

// imgui_mutex held: the resolution the panel shows, the one being chosen or the window's.
std::pair<int, int> ShownResolution() {
    const auto& s = DeceptionSettings::Get();
    return pending_resolution.first ? pending_resolution
                                    : std::pair<int, int>{s.window_width.load(), s.window_height.load()};
}

// imgui_mutex held: the aspect ratio the panel shows, the one being chosen or the setting.
int ShownAspect() {
    return pending_aspect >= 0 ? pending_aspect : DeceptionSettings::Get().aspect_ratio.load();
}

// imgui_mutex held: hands the chosen aspect ratio and window size to the window (which follows
// aspect_ratio, window_width and window_height) once they have rested for kResolutionSettle, or at
// once with `now`.
void SettleResolution(bool now) {
    if ((pending_aspect < 0 && !pending_resolution.first) ||
        (!now && std::chrono::steady_clock::now() - pending_since < kResolutionSettle)) {
        return;
    }
    auto& s = DeceptionSettings::Get();
    if (pending_resolution.first) {
        s.window_width = pending_resolution.first;
        s.window_height = pending_resolution.second;
    }
    if (pending_aspect >= 0) s.aspect_ratio = pending_aspect;
    pending_aspect = -1;
    pending_resolution = {0, 0};
}

// imgui_mutex held: the size the fullscreen modes show for the panel's aspect ratio.
std::pair<int, int> ShownFullscreen() {
    const auto& s = DeceptionSettings::Get();
    if (pending_aspect < 0 && s.fullscreen_width > 0) return {s.fullscreen_width.load(), s.fullscreen_height.load()};
    return DeceptionSettings::PickFullscreen(s.display_mode == DeceptionGraphics::Fullscreen, ShownAspect(),
                                           {s.desktop_width, s.desktop_height}, s.modes)
        .picture;
}

// imgui_mutex held: the focused row's value one step forward (delta 1) or back (-1).
void ChangeDisplayRow(int row, int delta) {
    namespace G = DeceptionGraphics;
    auto& s = DeceptionSettings::Get();
    switch (row) {
    case RowMode: s.display_mode = (s.display_mode + delta + G::DisplayModeCount) % G::DisplayModeCount; break;
    case RowAspect: { // the window moves to the new shape's size nearest in height
        const int next = (ShownAspect() + delta + DeceptionSettings::AspectRatioCount) % DeceptionSettings::AspectRatioCount;
        pending_aspect = next;
        pending_resolution = DeceptionSettings::NearestHeight(s.resolutions[next], ShownResolution().second);
        pending_since = std::chrono::steady_clock::now();
        break;
    }
    case RowResolution: { // windowed only, listed largest first: right steps up to a larger size
        const auto& sizes = s.resolutions[ShownAspect()];
        if (sizes.empty() || s.display_mode != G::Windowed) return;
        const std::pair<int, int> now = ShownResolution();
        int at = 0;
        while (at + 1 < int(sizes.size()) && sizes[at] > now) ++at;
        at = std::clamp(at - delta, 0, int(sizes.size()) - 1);
        pending_resolution = sizes[at];
        pending_since = std::chrono::steady_clock::now();
        break;
    }
    case RowMonitor: { // the primary, then each listed display
        const int count = int(s.monitors.size()) + 1;
        int at = 0;
        for (int i = 0; i < int(s.monitors.size()); ++i) {
            if (s.monitors[i] == s.monitor) at = i + 1;
        }
        at = (at + delta + count) % count;
        s.monitor = at == 0 ? G::PrimaryMonitor : s.monitors[at - 1];
        break;
    }
    case RowVsync: { // On, Off, Off with tearing
        const int at = (s.vsync ? 0 : s.tearing ? 2 : 1) + delta;
        const int next = (at + 3) % 3;
        s.vsync = next == 0;
        s.tearing = next == 2;
        break;
    }
    case RowFps: s.frame_rate = StepValue(G::FrameRates, s.frame_rate.load(), delta); break;
    case RowRender:
        s.render_resolution = s.render_resolution == DeceptionSettings::Render4k ? DeceptionSettings::Render1080p
                                                                                   : DeceptionSettings::Render4k;
        break;
    case RowSharpen: s.sharpening = !s.sharpening; break;
    case RowHdr: s.hdr = !s.hdr; break;
    default: return;
    }
    display_dirty = true;
}

// imgui_mutex held. Saving happens once, on close.
void CloseDisplaySettings() {
    SettleResolution(true);
    display_open = false;
    if (display_dirty) {
        display_dirty = false;
        DeceptionSettings::SaveDisplay();
    }
}

struct DisplayRowText {
    const char* label;
    std::string value;
    std::string help;
    bool changes = true; ///< Left/Right change it (arrows around the value)
    bool enabled = true; ///< applies in the current window mode; else greyed out and skipped
};

DisplayRowText DisplayRowAt(int row) {
    auto& s = DeceptionSettings::Get();
    switch (row) {
    case RowMode: {
        static const char* const names[] = {"Windowed", "Borderless windowed", "Fullscreen"};
        std::string help = "Windowed, a borderless window covering the screen, or exclusive fullscreen.";
        if (s.display_mode != DeceptionGraphics::Windowed) {
            const auto [fw, fh] = ShownFullscreen();
            help += " Fullscreen picks its size from the screen and the aspect ratio, now " + std::to_string(fw) +
                    " x " + std::to_string(fh) + ", with black bars when the screen is of another shape.";
        }
        return {"Window mode", names[std::clamp(s.display_mode.load(), 0, 2)], help};
    }
    case RowAspect: {
        const int ratio = ShownAspect();
        std::string text(DeceptionSettings::AspectRatioKeys[ratio]);
        if (ratio == DeceptionSettings::Aspect16x9) text += "  (PS4)";
        if (ratio != s.startup_aspect_ratio) text += "  (after restart)";
        return {"Aspect ratio", text,
                "The picture's shape. 21:9 and 32:9 show more of the level to the sides, for ultrawide screens, "
                "4:3 more above and below; "
                "the menus stay 16:9. A window takes sizes of this shape; fullscreen picks one from the screen. "
                "The game takes a new shape after a restart."};
    }
    case RowResolution: {
        const auto [w, h] = ShownResolution();
        const bool windowed = s.display_mode == DeceptionGraphics::Windowed;
        return {"Windowed resolution", std::to_string(w) + " x " + std::to_string(h) + "  " + DeceptionSettings::AspectLabel(w, h),
                "The window's size, among those of the aspect ratio that fit the screen.", windowed, windowed};
    }
    case RowMonitor: {
        std::string name = "Primary display";
        for (size_t i = 0; i < s.monitors.size() && i < s.monitor_names.size(); ++i) {
            if (s.monitors[i] == s.monitor) name = std::to_string(s.monitors[i] + 1) + ": " + s.monitor_names[i];
        }
        return {"Monitor", name, "The display the game window is on."};
    }
    case RowVsync:
        return {"VSync", s.vsync ? "On" : s.tearing ? "Off, tearing allowed" : "Off",
                "On waits for the monitor: no tearing, slightly more input delay. Off shows the newest frame "
                "at each refresh, still without tearing. Off, tearing allowed shows each frame at once: the "
                "least delay, and the only way past twice the refresh rate."};
    case RowFps: {
        const int chosen = s.frame_rate.load(), runs = DeceptionSettings::EffectiveFrameRate();
        std::string value = std::to_string(chosen) + " FPS";
        if (const char* env = std::getenv("DECEPTION_VBLANK_HZ"); env && env[0]) {
            value += "  (DECEPTION_VBLANK_HZ overrides)";
        } else if (runs != chosen) {
            value += "  (display is " + std::to_string(int(DeceptionSettings::DisplayRefresh() + 0.5)) +
                     " Hz: runs at " + std::to_string(runs) + ")";
        }
        return {"Max FPS", value,
                "The game's frame rate, at normal game speed: 60 is the PS4's, 30 for PCs that cannot hold it. "
                "Applies at once. It is held "
                "to what the display refreshes at, and the PC must keep up with it: when it cannot, the "
                "game runs slower instead of skipping frames."};
    }
    case RowRender: {
        const int value = s.render_resolution;
        std::string text = value == DeceptionSettings::Render4k ? "4K (3840 x 2160)" : "1080p";
        if (value != s.startup_render_resolution) text += "  (after restart)";
        return {"Render resolution", text,
                "4K as on PS4 Pro, filtered down to the window: sharper. 1080p as on PS4: lighter on the GPU. "
                "Applies after a restart."};
    }
    case RowSharpen:
        return {"Sharpening", s.sharpening ? "On" : "Off",
                "AMD FSR 1 RCAS sharpening of the picture at the window's size: crisper edges and detail, "
                "without halos. Costs well under a millisecond a frame. Applies at once; F9 switches it over the game."};
    case RowHdr: {
        std::string text = s.hdr ? "On" : "Off";
        if (s.hdr != s.startup_hdr) {
            text += "  (after restart)";
        } else if (s.hdr && s.hdr_display == 0) {
            text += "  (this display has no HDR)";
        }
        return {"HDR", text,
                "The game's own HDR output, for an HDR display (in Windows, turn on Use HDR in Display "
                "settings first). Applies after a restart."};
    }
    default: return {"", "", ""};
    }
}

// Text as the game's menus draw it: white, with a soft dark shadow below and to the right.
void PageText(ImDrawList* dl, ImFont* font, float size, ImVec2 at, const char* text, ImU32 color = PageInk::Text) {
    const float d = std::max(1.0f, size * 0.05f);
    dl->AddText(font, size, ImVec2(at.x + d, at.y + d), PageInk::Shadow, text);
    dl->AddText(font, size, at, color, text);
}

// An arrow ("<" for dir -1, ">" for 1) as one stroke, centred on `at`.
void Chevron(ImDrawList* dl, ImVec2 at, float size, int dir, ImU32 color, float thickness) {
    const float w = size * 0.28f * float(dir), h = size * 0.5f;
    const ImVec2 points[3] = {ImVec2(at.x - w, at.y - h), ImVec2(at.x + w, at.y), ImVec2(at.x - w, at.y + h)};
    dl->AddPolyline(points, 3, color, ImDrawFlags_None, thickness);
}

// The game's bar: a band whose ends slant inward by `slant` at the bottom (its menu items, the
// header tab and the button hints), filled top to bottom from `top` to `low`.
void SlantedBar(ImDrawList* dl, ImVec2 a, ImVec2 b, float slant, ImU32 top, ImU32 low) {
    const int first = dl->VtxBuffer.Size;
    const ImVec2 points[4] = {a, ImVec2(b.x, a.y), ImVec2(b.x - slant, b.y), ImVec2(a.x + slant, b.y)};
    dl->AddConvexPolyFilled(points, 4, top);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, first, dl->VtxBuffer.Size, a, ImVec2(a.x, b.y), top, low);
}
void SlantedOutline(ImDrawList* dl, ImVec2 a, ImVec2 b, float slant, ImU32 color, float thickness) {
    const ImVec2 points[4] = {a, ImVec2(b.x, a.y), ImVec2(b.x - slant, b.y), ImVec2(a.x + slant, b.y)};
    dl->AddPolyline(points, 4, color, ImDrawFlags_Closed, thickness);
}

// The Display Settings panel, drawn like the game's own menus over the dimmed game: a framed
// panel with the title on a tab above it, one teal bar per setting (the focused one bright, with
// arrows around its value), the focused setting's help below, and the buttons on tabs at the
// bottom.
void DisplaySettingsBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float px = viewport->Size.y / 1080.0f;
    ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->Size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##display_settings", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const ImVec2 o = viewport->Pos, size = viewport->Size;
    const float right = o.x + size.x, bottom = o.y + size.y, line = std::max(1.0f, 2.0f * px);
    dl->AddRectFilled(o, ImVec2(right, bottom), PageInk::Shade);

    // The panel, centred, and the title tab on its top edge.
    const float row_h = 64.0f * px, gap = 12.0f * px, pad = 30.0f * px;
    const float panel_w = std::min(size.x - 80.0f * px, 1180.0f * px);
    const float panel_h = pad * 2.0f + float(RowCount) * row_h + float(RowCount - 1) * gap;
    const float x0 = o.x + (size.x - panel_w) * 0.5f, x1 = x0 + panel_w;
    const float top = o.y + std::max(110.0f * px, (size.y - panel_h) * 0.42f);
    dl->AddRectFilled(ImVec2(x0, top), ImVec2(x1, top + panel_h), PageInk::Panel, 10.0f * px);
    dl->AddRect(ImVec2(x0, top), ImVec2(x1, top + panel_h), PageInk::Frame, 10.0f * px, 0, line);

    const char* title = "DISPLAY SETTINGS";
    const float title_size = 46.0f * px, title_w = font->CalcTextSizeA(title_size, FLT_MAX, 0.0f, title).x;
    const float tab_w = title_w + 120.0f * px, tab_h = 62.0f * px, tab_x = o.x + (size.x - tab_w) * 0.5f;
    const ImVec2 tab_a(tab_x, top - tab_h + 8.0f * px), tab_b(tab_x + tab_w, top + 8.0f * px);
    SlantedBar(dl, tab_a, tab_b, 18.0f * px, PageInk::RowLow, PageInk::Panel);
    SlantedOutline(dl, tab_a, tab_b, 18.0f * px, PageInk::Frame, line);
    PageText(dl, font, title_size, ImVec2(tab_x + (tab_w - title_w) * 0.5f, tab_a.y + (tab_h - title_size) * 0.5f),
             title);

    // One bar per setting: its name on the left, its value on the right.
    const float label = 32.0f * px, value = 30.0f * px, slant = 14.0f * px;
    const float bx0 = x0 + pad, bx1 = x1 - pad;
    float y = top + pad;
    std::string help;
    for (int row = 0; row < RowCount; ++row) {
        const DisplayRowText text = DisplayRowAt(row);
        const bool focus = row == display_row;
        const ImVec2 a(bx0, y), b(bx1, y + row_h);
        const float mid = y + row_h * 0.5f;
        const bool off = !text.enabled;
        SlantedBar(dl, a, b, slant, focus ? PageInk::Focus : off ? PageInk::RowOff : PageInk::Row,
                   focus ? PageInk::FocusLow : off ? PageInk::RowOffLow : PageInk::RowLow);
        SlantedOutline(dl, a, b, slant, focus ? PageInk::Frame : PageInk::FrameDim, focus ? line * 1.5f : line);
        const ImU32 ink = focus ? PageInk::Text : off ? PageInk::TextOff : PageInk::TextDim;
        PageText(dl, font, label, ImVec2(bx0 + slant + 28.0f * px, mid - label * 0.56f), text.label, ink);
        const float vw = font->CalcTextSizeA(value, FLT_MAX, 0.0f, text.value.c_str()).x;
        const float value_x = bx1 - slant - 70.0f * px - vw;
        PageText(dl, font, value, ImVec2(value_x, mid - value * 0.56f), text.value.c_str(), ink);
        if (focus && text.changes) {
            const float chevron = row_h * 0.36f, stroke = 3.0f * px;
            Chevron(dl, ImVec2(value_x - 26.0f * px, mid), chevron, -1, PageInk::Text, stroke);
            Chevron(dl, ImVec2(bx1 - slant - 40.0f * px, mid), chevron, 1, PageInk::Text, stroke);
        }
        if (focus) help = text.help;
        y += row_h + gap;
    }

    // The focused setting's help under the panel, wrapped to its width.
    const float help_size = 26.0f * px;
    dl->AddText(font, help_size, ImVec2(x0 + 8.0f * px, top + panel_h + 22.0f * px), PageInk::Help, help.c_str(), nullptr,
                panel_w - 16.0f * px);

    // The buttons, each on a tab, along the bottom.
    const char* const hints[3] = {"Up/Down  Choose",
                                  "Left/Right, Cross (A)  Change",
                                  "Circle (B), Select, Esc  Back"};
    const float hint_size = 24.0f * px, hint_h = 46.0f * px, hint_gap = 24.0f * px;
    float hint_w[3], total = 0.0f;
    for (int i = 0; i < 3; ++i) {
        hint_w[i] = font->CalcTextSizeA(hint_size, FLT_MAX, 0.0f, hints[i]).x + 70.0f * px;
        total += hint_w[i] + (i ? hint_gap : 0.0f);
    }
    float hx = o.x + (size.x - total) * 0.5f;
    const float hy = bottom - hint_h - 30.0f * px;
    for (int i = 0; i < 3; ++i) {
        const ImVec2 a(hx, hy), b(hx + hint_w[i], hy + hint_h);
        SlantedBar(dl, a, b, 12.0f * px, PageInk::Row, PageInk::RowLow);
        SlantedOutline(dl, a, b, 12.0f * px, PageInk::FrameDim, line);
        PageText(dl, font, hint_size, ImVec2(hx + 35.0f * px, hy + (hint_h - hint_size) * 0.5f), hints[i]);
        hx += hint_w[i] + hint_gap;
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

// The exit prompt, drawn like the Display Settings panel: the question on a tab over a small
// framed panel holding No and Yes, the focused one bright, and the buttons on tabs at the bottom.
void ExitPromptBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float px = viewport->Size.y / 1080.0f;
    ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->Size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##exit_prompt", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const ImVec2 o = viewport->Pos, size = viewport->Size;
    const float right = o.x + size.x, bottom = o.y + size.y, line = std::max(1.0f, 2.0f * px);
    dl->AddRectFilled(o, ImVec2(right, bottom), PageInk::Shade);

    const float row_h = 64.0f * px, gap = 12.0f * px, pad = 30.0f * px;
    const float panel_w = std::min(size.x - 80.0f * px, 640.0f * px), panel_h = pad * 2.0f + row_h * 2.0f + gap;
    const float x0 = o.x + (size.x - panel_w) * 0.5f, x1 = x0 + panel_w;
    const float top = o.y + (size.y - panel_h) * 0.5f;
    dl->AddRectFilled(ImVec2(x0, top), ImVec2(x1, top + panel_h), PageInk::Panel, 10.0f * px);
    dl->AddRect(ImVec2(x0, top), ImVec2(x1, top + panel_h), PageInk::Frame, 10.0f * px, 0, line);

    const char* title = "EXIT DECEPTION?";
    const float title_size = 46.0f * px, title_w = font->CalcTextSizeA(title_size, FLT_MAX, 0.0f, title).x;
    const float tab_w = title_w + 120.0f * px, tab_h = 62.0f * px, tab_x = o.x + (size.x - tab_w) * 0.5f;
    const ImVec2 tab_a(tab_x, top - tab_h + 8.0f * px), tab_b(tab_x + tab_w, top + 8.0f * px);
    SlantedBar(dl, tab_a, tab_b, 18.0f * px, PageInk::RowLow, PageInk::Panel);
    SlantedOutline(dl, tab_a, tab_b, 18.0f * px, PageInk::Frame, line);
    PageText(dl, font, title_size, ImVec2(tab_x + (tab_w - title_w) * 0.5f, tab_a.y + (tab_h - title_size) * 0.5f),
             title);

    const char* const answers[2] = {"No", "Yes"};
    const float text_size = 34.0f * px, slant = 14.0f * px;
    float y = top + pad;
    for (int i = 0; i < 2; ++i) {
        const bool focus = i == exit_focus;
        const ImVec2 a(x0 + pad, y), b(x1 - pad, y + row_h);
        SlantedBar(dl, a, b, slant, focus ? PageInk::Focus : PageInk::Row, focus ? PageInk::FocusLow : PageInk::RowLow);
        SlantedOutline(dl, a, b, slant, focus ? PageInk::Frame : PageInk::FrameDim, focus ? line * 1.5f : line);
        const float tw = font->CalcTextSizeA(text_size, FLT_MAX, 0.0f, answers[i]).x;
        PageText(dl, font, text_size, ImVec2((a.x + b.x - tw) * 0.5f, y + (row_h - text_size) * 0.5f), answers[i],
                 focus ? PageInk::Text : PageInk::TextDim);
        y += row_h + gap;
    }

    const char* const hints[3] = {"Up/Down  Choose", "Cross (A), Enter  Confirm",
                                  "Circle (B), Esc  Cancel"};
    const float hint_size = 24.0f * px, hint_h = 46.0f * px, hint_gap = 24.0f * px;
    float hint_w[3], total = 0.0f;
    for (int i = 0; i < 3; ++i) {
        hint_w[i] = font->CalcTextSizeA(hint_size, FLT_MAX, 0.0f, hints[i]).x + 70.0f * px;
        total += hint_w[i] + (i ? hint_gap : 0.0f);
    }
    float hx = o.x + (size.x - total) * 0.5f;
    const float hy = bottom - hint_h - 30.0f * px;
    for (int i = 0; i < 3; ++i) {
        const ImVec2 a(hx, hy), b(hx + hint_w[i], hy + hint_h);
        SlantedBar(dl, a, b, 12.0f * px, PageInk::Row, PageInk::RowLow);
        SlantedOutline(dl, a, b, 12.0f * px, PageInk::FrameDim, line);
        PageText(dl, font, hint_size, ImVec2(hx + 35.0f * px, hy + (hint_h - hint_size) * 0.5f), hints[i]);
        hx += hint_w[i] + hint_gap;
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

// imgui_mutex held. The exit prompt takes every key and button while it is open: up/down (or
// left/right) moves, confirm answers the focused choice, cancel or Start answers No.
bool HandleExitEvent(const SDL_Event& event) {
    int move = 0, answer = -1;
    if (event.type == SDL_EVENT_KEY_DOWN) {
        const SDL_Keycode key = event.key.key;
        move = key == SDLK_UP || key == SDLK_W || key == SDLK_LEFT || key == SDLK_A     ? -1
               : key == SDLK_DOWN || key == SDLK_S || key == SDLK_RIGHT || key == SDLK_D ? 1
                                                                                          : 0;
        if (!event.key.repeat) {
            if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE) answer = exit_focus;
            if (key == SDLK_ESCAPE || key == SDLK_BACKSPACE || key == SDLK_F4) answer = 0;
        }
    } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        const u8 button = event.gbutton.button;
        const u8 confirm_btn = SDL_GAMEPAD_BUTTON_SOUTH;
        const u8 cancel_btn = SDL_GAMEPAD_BUTTON_EAST;
        move = button == SDL_GAMEPAD_BUTTON_DPAD_UP || button == SDL_GAMEPAD_BUTTON_DPAD_LEFT     ? -1
               : button == SDL_GAMEPAD_BUTTON_DPAD_DOWN || button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT ? 1
                                                                                                    : 0;
        if (button == confirm_btn) answer = exit_focus;
        if (button == cancel_btn || button == SDL_GAMEPAD_BUTTON_START) answer = 0;
    } else {
        return event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP;
    }
    if (move) {
        exit_focus = std::clamp(exit_focus + move, 0, 1);
    }
    if (answer >= 0) {
        exit_open = false;
        if (answer == 1) {
            std::puts("Overlay: exit confirmed; closing the game");
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT; // the window thread ends the process as when the window is closed
            SDL_PushEvent(&quit);
        }
    }
    return true;
}

// The panel takes every key and button while it is open (the game sees neutral input).
bool HandleDisplayEvent(const SDL_Event& event) {
    int move = 0, change = 0;
    bool close = false;
    if (event.type == SDL_EVENT_KEY_DOWN) {
        const SDL_Keycode key = event.key.key;
        move = key == SDLK_UP || key == SDLK_W ? -1 : key == SDLK_DOWN || key == SDLK_S ? 1 : 0;
        change = key == SDLK_LEFT || key == SDLK_A ? -1
                 : key == SDLK_RIGHT || key == SDLK_D || key == SDLK_RETURN || key == SDLK_KP_ENTER ||
                         key == SDLK_SPACE
                     ? 1
                     : 0;
        close = !event.key.repeat && (key == SDLK_ESCAPE || key == SDLK_BACKSPACE || key == SDLK_F10);
    } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        const u8 button = event.gbutton.button;
        const u8 confirm_btn = SDL_GAMEPAD_BUTTON_SOUTH;
        const u8 cancel_btn = SDL_GAMEPAD_BUTTON_EAST;
        move = button == SDL_GAMEPAD_BUTTON_DPAD_UP ? -1 : button == SDL_GAMEPAD_BUTTON_DPAD_DOWN ? 1 : 0;
        change = button == SDL_GAMEPAD_BUTTON_DPAD_LEFT ? -1
                 : button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT || button == confirm_btn ? 1
                                                                                      : 0;
        close = button == cancel_btn || button == SDL_GAMEPAD_BUTTON_START || button == SDL_GAMEPAD_BUTTON_BACK;
    } else {
        return event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP;
    }
    if (move) { // greyed-out rows are passed over
        for (int i = 0; i < RowCount; ++i) {
            display_row = (display_row + move + RowCount) % RowCount;
            if (DisplayRowAt(display_row).enabled) break;
        }
    }
    if (change) {
        ChangeDisplayRow(display_row, change);
    }
    if (close) {
        CloseDisplaySettings();
    }
    return true;
}

void LoadingScreen() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.4f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##loading", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    const u32 done = loading_done, total = loading_total;
    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextUnformatted(total ? "Loading shader cache..." : "Preparing graphics...");
    ImGui::SetWindowFontScale(1.0f);
    if (total) {
        char count[64];
        std::snprintf(count, sizeof(count), "%u / %u (%u%%)", done, total, u32(u64(done) * 100 / total));
        ImGui::ProgressBar(float(done) / float(total), ImVec2(-1.0f, 0.0f), count);
    }
    ImGui::End();
}

void BackgroundLoading() {
    const u32 done = background_done, total = std::max(background_total.load(), 1u);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + pad, viewport->WorkPos.y + viewport->WorkSize.y - pad),
                            ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::Begin("##background_loading", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextDisabled("Loading shader cache %u%%", u32(u64(done) * 100 / total));
    ImGui::End();
}

bool NoticeActive() {
    return notice_until.load(std::memory_order_relaxed) > NowMs();
}

// Lower-right notes stack upward from `bottom`: the notice, then the shader notes.
template <typename Body>
void CornerNote(const char* id, float& bottom, float alpha, Body&& body) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad, bottom), ImGuiCond_Always,
                            ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(alpha);
    ImGui::Begin(id, nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    body();
    bottom -= ImGui::GetWindowHeight() + pad / 2;
    ImGui::End();
}

void Notice(float& bottom) {
    std::string text;
    {
        std::scoped_lock lock{notice_mutex};
        text = notice_text;
    }
    CornerNote("##notice", bottom, 0.6f, [&] { ImGui::TextUnformatted(text.c_str()); });
}

// DECEPTION_SHADER_NOTES=1: shader cache and compile notes in the corner.
const bool shader_notes = [] {
    const char* env = std::getenv("DECEPTION_SHADER_NOTES");
    return env && *env != '0';
}();

bool PrefetchActive() {
    const u32 total = prefetch_total;
    return shader_notes && total && (prefetch_done < total || prefetch_until > NowMs());
}

void ShaderPrefetch(float& bottom) {
    const u32 done = prefetch_done, total = std::max(prefetch_total.load(), 1u);
    CornerNote("##shader_prefetch", bottom, 0.35f, [&] {
        if (done >= total) {
            ImGui::TextDisabled("Shader cache ready (%u)", total);
            return;
        }
        ImGui::TextDisabled("Reading shader cache %u%% (%u/%u)", u32(u64(done) * 100 / total), done, total);
        ImGui::ProgressBar(float(done) / float(total), ImVec2(ImGui::GetContentRegionAvail().x, 4.0f * base_scale),
                           "");
    });
}

bool CompileNoteActive() {
    return shader_notes && (compile_pending || compile_until > NowMs());
}

void ShaderCompiles(float& bottom) {
    const u32 pending = compile_pending, count = compile_count, waits = compile_waits;
    CornerNote("##shader_compiles", bottom, 0.35f, [&] {
        if (pending) {
            ImGui::TextDisabled("Compiling shaders (%u left)", pending);
        } else {
            ImGui::TextDisabled("Compiled %u new shader%s", count, count == 1 ? "" : "s");
        }
        if (waits) {
            ImGui::TextDisabled("Waited for %u build%s", waits, waits == 1 ? "" : "s");
        }
    });
}

void ConfigureContext() {
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.BackendPlatformName = "deception";
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 10.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 5.0f;
    style.WindowPadding = ImVec2(20.0f, 16.0f);
    style.ItemSpacing = ImVec2(10.0f, 7.0f);
    style.WindowBorderSize = 1.0f;
    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = Ink::Text;
    c[ImGuiCol_TextDisabled] = Ink::Muted;
    c[ImGuiCol_WindowBg] = ImVec4(Ink::Bg.x, Ink::Bg.y, Ink::Bg.z, 0.97f);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Border] = Ink::Line;
    c[ImGuiCol_Separator] = Ink::Line;
    c[ImGuiCol_FrameBg] = Ink::Card;
    c[ImGuiCol_PlotHistogram] = Ink::Accent;
    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(deception_font_ttf),
                                   int(deception_font_ttf_end - deception_font_ttf), 18.0f, &font_config);
}

// imgui_mutex held. The backend frees a texture some frames after its last use.
void RetireChoiceTextures() {
    for (ImTextureData* tex : choice_textures) {
        if (tex) {
            tex->WantDestroyNextFrame = true;
            choice_retired.push_back(tex);
        }
    }
    choice_textures.clear();
    choice_pictures.clear();
    choice_retiring = !choice_retired.empty();
}

// imgui_mutex held, before NewFrame: new pictures become textures, destroyed ones are freed.
void UpdateChoiceTextures() {
    if (!choice_pictures.empty()) {
        choice_textures.assign(choice_pictures.size(), nullptr);
        for (size_t i = 0; i < choice_pictures.size(); ++i) {
            const DeceptionPng::Image& picture = choice_pictures[i];
            if (picture.rgba.empty()) {
                continue;
            }
            ImTextureData* tex = IM_NEW(ImTextureData)();
            tex->Create(ImTextureFormat_RGBA32, picture.width, picture.height);
            std::memcpy(tex->GetPixels(), picture.rgba.data(), picture.rgba.size());
            ImGui::RegisterUserTexture(tex);
            choice_textures[i] = tex;
        }
        choice_pictures.clear();
    }
    std::erase_if(choice_retired, [](ImTextureData* tex) {
        if (tex->Status != ImTextureStatus_Destroyed) {
            return false;
        }
        ImGui::UnregisterUserTexture(tex);
        IM_DELETE(tex);
        return true;
    });
    choice_retiring = !choice_retired.empty();
}

void StartSplashFade(const char* why) {
    int shown = SplashShown;
    splash_fade_from = NowMs();
    if (splash_state.compare_exchange_strong(shown, SplashFading)) {
        std::printf("Splash: %s after %.1f s\n", why, double(splash_fade_from - splash_since) / 1000.0);
    }
}

// imgui_mutex held, before NewFrame and UpdateChoiceTextures: the picture becomes a texture, and
// one faded out is retired.
void UpdateSplashTexture() {
    if (!splash_picture.rgba.empty()) {
        splash_texture = IM_NEW(ImTextureData)();
        splash_texture->Create(ImTextureFormat_RGBA32, splash_picture.width, splash_picture.height);
        std::memcpy(splash_texture->GetPixels(), splash_picture.rgba.data(), splash_picture.rgba.size());
        ImGui::RegisterUserTexture(splash_texture);
        splash_picture = {};
    }
    const int state = splash_state;
    if (state == SplashShown && NowMs() - splash_since > kSplashMaxMs) {
        StartSplashFade("no game picture yet, hidden");
    }
    if (state == SplashFading && NowMs() - splash_fade_from >= kSplashFadeMs) {
        splash_state = SplashDone;
        if (splash_texture) {
            splash_texture->WantDestroyNextFrame = true;
            choice_retired.push_back(splash_texture);
            choice_retiring = true;
            splash_texture = nullptr;
        }
    }
}

// The start-up picture fitted to the window (black bars like the game's frame), under the rest.
void Splash() {
    const int state = splash_state;
    if (!splash_texture || (state != SplashShown && state != SplashFading)) {
        return;
    }
    float alpha = 1.0f;
    if (state == SplashFading) {
        alpha = std::clamp(1.0f - float(NowMs() - splash_fade_from) / float(kSplashFadeMs), 0.0f, 1.0f);
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float w = float(splash_texture->Width), h = float(splash_texture->Height);
    const float scale = std::min(viewport->Size.x / w, viewport->Size.y / h);
    const ImVec2 size(w * scale, h * scale);
    const ImVec2 p0(viewport->Pos.x + (viewport->Size.x - size.x) * 0.5f,
                    viewport->Pos.y + (viewport->Size.y - size.y) * 0.5f);
    ImGui::GetBackgroundDrawList()->AddImage(splash_texture->GetTexRef(), p0, ImVec2(p0.x + size.x, p0.y + size.y),
                                             ImVec2(0, 0), ImVec2(1, 1),
                                             IM_COL32(255, 255, 255, int(alpha * 255.0f + 0.5f)));
}

// The save list takes the keys and buttons that move and confirm; the rest stays with the game
// (held neutral by CapturesInput).
bool HandleChoiceEvent(const SDL_Event& event) {
    int move = 0, finish = 0; // finish: 1 accept, 2 cancel
    if (event.type == SDL_EVENT_KEY_DOWN) {
        const SDL_Keycode key = event.key.key;
        move = key == SDLK_UP || key == SDLK_W ? -1 : key == SDLK_DOWN || key == SDLK_S ? 1 : 0;
        finish = key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE ? 1
                 : key == SDLK_ESCAPE || key == SDLK_BACKSPACE                   ? 2
                                                                                 : 0;
    } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        const u8 button = event.gbutton.button;
        move = button == SDL_GAMEPAD_BUTTON_DPAD_UP ? -1 : button == SDL_GAMEPAD_BUTTON_DPAD_DOWN ? 1 : 0;
        const u8 confirm_btn = SDL_GAMEPAD_BUTTON_SOUTH;
        const u8 cancel_btn = SDL_GAMEPAD_BUTTON_EAST;
        finish = button == confirm_btn ? 1 : button == cancel_btn ? 2 : 0;
    } else {
        return event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP;
    }
    const int count = int(choice_items.size());
    if (move && count) {
        choice_index = (choice_index + move + count) % count;
    }
    if (finish) {
        choice_result = finish == 1 && count ? choice_index : -2;
        choice_active = false;
        RetireChoiceTextures();
    }
    return true;
}

void DrawWindows() {
    SettleResolution(false);
    Splash();
    if (loading) {
        LoadingScreen();
    }
    float corner = ImGui::GetMainViewport()->WorkPos.y + ImGui::GetMainViewport()->WorkSize.y - 12.0f * base_scale;
    if (NoticeActive() && !display_open && !exit_open) { // their button hints use the bottom of the screen
        Notice(corner);
    }
    if (PrefetchActive() && !loading) {
        ShaderPrefetch(corner);
    }
    if (CompileNoteActive() && !loading) {
        ShaderCompiles(corner);
    }
    if (background_total && !loading) {
        BackgroundLoading();
    }
    if (text_entry_active) {
        TextEntryBox();
    }
    if (choice_active) {
        ChoiceBox();
    }
    if (display_open && !choice_active) {
        DisplaySettingsBox();
    } else if (exit_open && !choice_active) {
        ExitPromptBox();
    }
}

} // namespace

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ConfigureContext();

    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(*static_cast<const vk::Instance*>(user),
                                                                       name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    // The font atlas plus the save list pictures (up to 33, and a closed list's until freed).
    info.DescriptorPoolSize = 80;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    initialized = true;
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    if (choice_active) {
        return HandleChoiceEvent(event);
    }
    if (display_open) {
        return HandleDisplayEvent(event);
    }
    if (exit_open) {
        return HandleExitEvent(event);
    }
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F10 && !event.key.repeat && !text_entry_active) {
        display_row = 0;
        display_open = true;
        return true;
    }
    // F9 turns sharpening on or off over the game itself, to compare (saved at once). Left to the
    // pad recorder when DECEPTION_PAD_RECORD is set (src/runtime_pad.c), which starts on F9.
    static const bool pad_record = [] {
        const char* record = std::getenv("DECEPTION_PAD_RECORD");
        return record && record[0];
    }();
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F9 && !event.key.repeat && !text_entry_active &&
        !pad_record) {
        auto& s = DeceptionSettings::Get();
        s.sharpening = !s.sharpening;
        ShowNotice(s.sharpening ? "Sharpening: On" : "Sharpening: Off", 1500);
        DeceptionSettings::SaveDisplay();
        return true;
    }
    // F4 asks to close the game anywhere; Alt+F4 still closes the window at once.
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F4 && !event.key.repeat && !text_entry_active &&
        !(event.key.mod & SDL_KMOD_ALT)) {
        exit_focus = 0;
        exit_open = true;
        return true;
    }
    return false;
}

void OpenDisplaySettings() {
    std::scoped_lock lock{imgui_mutex};
    if (!display_open) {
        display_row = 0;
        display_open = true;
    }
}

bool DisplaySettingsOpen() {
    return display_open;
}

void OpenExitPrompt() {
    std::scoped_lock lock{imgui_mutex};
    if (!exit_open) {
        exit_focus = 0;
        exit_open = true;
    }
}

void ShowNotice(const std::string& text, int milliseconds) {
    {
        std::scoped_lock lock{notice_mutex};
        notice_text = text;
    }
    notice_until.store(NowMs() + milliseconds, std::memory_order_relaxed);
}

void SetBackgroundLoading(u32 done, u32 total) {
    background_done = done;
    background_total = total;
}

void SetShaderPrefetch(u32 done, u32 total) {
    if (total && done >= total) {
        prefetch_until = NowMs() + 2000;
    }
    prefetch_done = done;
    prefetch_total = total;
}

void NoteShaderCompiles(u32 pending, u32 started, u32 waits) {
    const s64 now = NowMs();
    if (!compile_pending && compile_until <= now) {
        compile_count = 0; // a new burst
        compile_waits = 0;
    }
    compile_count += started;
    compile_waits += waits;
    compile_pending = pending;
    compile_until = now + 2000;
}

void SetLoading(bool active, u32 done, u32 total) {
    loading_done = done;
    loading_total = total;
    loading = active;
}

void SetSplash(DeceptionPng::Image picture) {
    std::scoped_lock lock{imgui_mutex};
    if (picture.rgba.empty() || splash_state != SplashNone) {
        return;
    }
    splash_picture = std::move(picture);
    splash_since = NowMs();
    splash_state = SplashShown;
}

bool SplashWaiting() {
    return splash_state == SplashShown;
}

// Two picture frames in a row: a single stray one (a buffer not yet cleared) keeps the splash.
void NoteSplashFrame(bool picture) {
    if (!picture) {
        splash_lit_frames = 0;
    } else if (++splash_lit_frames >= 2) {
        StartSplashFade("game picture");
    }
}

bool Visible() {
    const int splash = splash_state;
    return initialized && (splash == SplashShown || splash == SplashFading || loading ||background_total || PrefetchActive() || CompileNoteActive() ||
                           text_entry_active || choice_active || choice_retiring || NoticeActive() || display_open ||
                           exit_open);
}

bool CapturesInput() {
    return text_entry_active || choice_active || display_open || exit_open;
}

void BeginChoice(const std::string& title, const std::vector<std::string>& items,
                 std::vector<DeceptionPng::Image> pictures, int focus, const std::string& preview_label,
                 DeceptionPng::Image preview) {
    std::scoped_lock lock{imgui_mutex};
    RetireChoiceTextures();
    choice_pictures = std::move(pictures);
    choice_pictures.resize(items.size());
    choice_preview_label = preview_label;
    if (!preview_label.empty()) choice_pictures.push_back(std::move(preview));
    choice_title = title;
    choice_items = items;
    choice_index = focus >= 0 && focus < int(items.size()) ? focus : 0;
    choice_result = -1;
    choice_active = true;
}

int PollChoice() {
    return choice_result;
}

void SetTextEntry(bool active, const std::string& prompt, const std::string& text) {
    std::scoped_lock lock{imgui_mutex};
    text_entry_active = active;
    text_entry_prompt = prompt;
    text_entry_text = text;
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1).
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    UpdateSplashTexture();
    UpdateChoiceTextures();
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    DrawWindows();
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

} // namespace DeceptionOverlay

// A message from the runtime to the player (src/runtime_pad.c: where the Display Settings are).
extern "C" void deception_gpu_notice(const char* text, int milliseconds) {
    if (text && text[0]) {
        DeceptionOverlay::ShowNotice(text, milliseconds);
    }
}

// Called by the runtime (src/runtime_services.c) when the game unlocks a trophy: its name as a
// notice in the corner.
extern "C" void deception_gpu_toast_trophy(int id, const char* title, const char* detail, const unsigned char* png,
                                            uint64_t png_size) {
    (void)id;
    (void)detail;
    (void)png;
    (void)png_size;
    if (title && title[0]) {
        DeceptionOverlay::ShowNotice(std::string("Trophy: ") + title, 5000);
    }
}
