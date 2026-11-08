// deception: SDL3 window for the Vulkan swapchain.
#include <algorithm>
#include <cmath>
#include <functional>
#include <tuple>
#include <utility>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <SDL3/SDL.h>
#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#endif
#include "common/assert.h"
#include "common/logging/log.h"
#include "sdl_window.h"
#include "deception_overlay.h"
#include "deception_graphics.h"
#include "deception_settings.h"

namespace Frontend {

// deception: the display the window opens on. DECEPTION_MONITOR is an index into SDL_GetDisplays()
// (the order SDL lists them in); absent or empty means the primary display. An invalid,
// negative or out-of-range value falls back to the primary with a diagnostic. SDL_WINDOWPOS_CENTERED
// is SDL_WINDOWPOS_CENTERED_DISPLAY(0), so the primary stays the safe default on every path.
static SDL_DisplayID MonitorDisplay() {
    auto& settings = DeceptionSettings::Get();
    int detected = 0;
    if (SDL_DisplayID* list = SDL_GetDisplays(&detected)) {
        settings.monitors.clear();
        settings.monitor_names.clear();
        for (int i = 0; i < detected; ++i) {
            SDL_Rect bounds{};
            if (SDL_GetDisplayBounds(list[i], &bounds) && bounds.w > 0 && bounds.h > 0) {
                settings.monitors.push_back(i);
                const char* name = SDL_GetDisplayName(list[i]);
                settings.monitor_names.push_back(name && name[0] ? name : "Display " + std::to_string(i + 1));
            }
        }
        SDL_free(list);
    }
    const char* value = std::getenv("DECEPTION_MONITOR");
    const int index = DeceptionGraphics::ParseMonitor(value ? value : "");
    if (index == DeceptionGraphics::PrimaryMonitor) {
        if (value && value[0]) {
            LOG_WARNING(Frontend, "DECEPTION_MONITOR \"{}\" is not a monitor index; using the primary display", value);
        }
        return SDL_GetPrimaryDisplay();
    }
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    if (!displays || count == 0) {
        LOG_WARNING(Frontend, "No displays available; using the primary display");
        SDL_free(displays);
        return SDL_GetPrimaryDisplay();
    }
    if (index < 0 || index >= count || std::find(settings.monitors.begin(), settings.monitors.end(), index) == settings.monitors.end()) {
        settings.monitor = settings.startup_monitor = -1;
        LOG_WARNING(Frontend, "DECEPTION_MONITOR {} is out of range for {} display(s); using the primary display", value, count);
        SDL_free(displays);
        return SDL_GetPrimaryDisplay();
    }
    const SDL_DisplayID chosen = displays[index];
    SDL_free(displays);
    return chosen;
}

// A display's fullscreen modes (refresh rates and pixel densities collapse), largest first.
static std::vector<std::pair<int, int>> FullscreenModes(SDL_DisplayID display) {
    std::vector<std::pair<int, int>> sizes;
    int count = 0;
    if (SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(display, &count)) {
        for (int i = 0; i < count; ++i) {
            if (modes[i]->w >= 640 && modes[i]->h >= 360) sizes.emplace_back(modes[i]->w, modes[i]->h);
        }
        SDL_free(modes);
    }
    std::sort(sizes.begin(), sizes.end(), std::greater<>());
    sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());
    return sizes;
}

// The window sizes of each aspect ratio for the Display Settings page: the display's fullscreen
// modes of that shape and the common sizes of it, all within the desktop, largest first; a shape
// with none gets the desktop's size fitted to it. A saved window size of another shape than
// aspect_ratio moves to the nearest in height of that shape's.
static void ListResolutions(SDL_DisplayID display) {
    namespace S = DeceptionSettings;
    auto& settings = S::Get();
    settings.modes = FullscreenModes(display);
    const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display);
    settings.desktop_width = desktop ? desktop->w : 0;
    settings.desktop_height = desktop ? desktop->h : 0;
    static constexpr std::pair<int, int> common[] = {
        {1024, 768}, {1280, 960}, {1440, 1080}, {1600, 1200}, {1920, 1440}, {2880, 2160},     // 4:3
        {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3200, 1800}, {3840, 2160}, {5120, 2880}, // 16:9
        {1720, 720}, {2560, 1080}, {3440, 1440}, {3840, 1600}, {5120, 2160},                             // 21:9
        {1920, 540}, {2560, 720}, {3200, 900}, {3840, 1080}, {5120, 1440}, {7680, 2160},                 // 32:9
    };
    std::vector<std::pair<int, int>> candidates(settings.modes);
    candidates.insert(candidates.end(), std::begin(common), std::end(common));
    const std::pair<int, int> saved{settings.window_width.load(), settings.window_height.load()};
    candidates.push_back(saved);
    for (auto& sizes : settings.resolutions) sizes.clear();
    for (const auto& [w, h] : candidates) {
        const int ratio = S::AspectRatioOf(w, h);
        if (ratio >= 0 && (!desktop || (w <= desktop->w && h <= desktop->h))) settings.resolutions[ratio].emplace_back(w, h);
    }
    for (int r = 0; r < S::AspectRatioCount; ++r) {
        auto& sizes = settings.resolutions[r];
        if (sizes.empty()) sizes.push_back(desktop ? S::FitAspect(desktop->w, desktop->h, r) : std::pair{1920, 1080});
        std::sort(sizes.begin(), sizes.end(), std::greater<>());
        sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());
    }
    const int ratio = settings.aspect_ratio;
    if (S::AspectRatioOf(saved.first, saved.second) != ratio) {
        const auto size = S::NearestHeight(settings.resolutions[ratio], saved.second);
        settings.window_width = size.first;
        settings.window_height = size.second;
        std::printf("Display: window size %dx%d is not %s; using %dx%d\n", saved.first, saved.second,
                    S::AspectRatioKeys[ratio].data(), size.first, size.second);
    }
}

// deception: a window opens at the size in settings.ini (window), fitted to the chosen display's
// usable area (with room for the title bar); one too large opens maximized, restoring to that size
// shrunk to fit. The picture is scaled to the window either way. True when shrunk.
static bool StartSize(s32& width, s32& height, SDL_DisplayID display) {
    constexpr int title_room = 48;
    const int w = DeceptionSettings::Get().window_width, h = DeceptionSettings::Get().window_height;
    double scale = 1.0;
    SDL_Rect usable{};
    if (SDL_GetDisplayUsableBounds(display, &usable) && usable.w > 0 && usable.h > title_room) {
        scale = std::min({1.0, double(usable.w) / w, double(usable.h - title_room) / h});
    }
    width = std::max(320, int(w * scale));
    height = std::max(180, int(h * scale));
    return scale < 1.0;
}

WindowSDL::WindowSDL(s32 width_, s32 height_, const char* title) : width{width_}, height{height_} {
    // Gamepads are sampled by runtime_pad.c; their events are pumped here with the window's.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        UNREACHABLE_MSG("Failed to initialize SDL video: {}", SDL_GetError());
    }
    const SDL_DisplayID display = MonitorDisplay();
    ListResolutions(display);
    resolution = {DeceptionSettings::Get().window_width.load(), DeceptionSettings::Get().window_height.load()};
    {
        // The game's shape is fixed for the run: the window's size, or what fullscreen picks.
        auto& settings = DeceptionSettings::Get();
        const int mode = settings.display_mode;
        auto [w, h] = resolution;
        if (mode != DeceptionGraphics::Windowed) {
            const auto choice = DeceptionSettings::PickFullscreen(mode == DeceptionGraphics::Fullscreen, settings.aspect_ratio,
                                                                {settings.desktop_width, settings.desktop_height}, settings.modes);
            std::tie(w, h) = choice.picture;
        }
        settings.startup_width_4k = DeceptionSettings::WideWidth4k(w, h);
        std::printf("Display: %s %dx%d (%s): the game's picture is %dx2160%s\n",
                    mode == DeceptionGraphics::Windowed ? "window" : "fullscreen picture", w, h,
                    DeceptionSettings::AspectLabel(w, h).c_str(), settings.startup_width_4k.load(),
                    settings.startup_width_4k > 3840 ? ", wider than the PS4's" : "");
    }
    const bool too_large = StartSize(width_, height_, display);
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width_);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height_);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_MAXIMIZED_BOOLEAN, too_large);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true); // until Show()
    base_title = title;
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    ASSERT_MSG(window, "Failed to create window: {}", SDL_GetError());
    // deception: DECEPTION_WINDOW_ICON names a BMP (SDL's built-in format) for the window icon.
    if (const char* icon_path = std::getenv("DECEPTION_WINDOW_ICON")) {
        if (SDL_Surface* icon = SDL_LoadBMP(icon_path)) {
            SDL_SetWindowIcon(window, icon);
            SDL_DestroySurface(icon);
        }
    }

    const char* driver = SDL_GetCurrentVideoDriver();
    const SDL_PropertiesID wp = SDL_GetWindowProperties(window);
#ifdef _WIN32
    if (driver && !std::strcmp(driver, "windows")) {
        window_info.type = WindowSystemType::Windows;
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    } else
#endif
    if (driver && !std::strcmp(driver, "x11")) {
        window_info.type = WindowSystemType::X11;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        window_info.render_surface = reinterpret_cast<void*>(SDL_GetNumberProperty(wp, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    } else if (driver && !std::strcmp(driver, "wayland")) {
        window_info.type = WindowSystemType::Wayland;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    } else {
        UNREACHABLE_MSG("Unsupported SDL video driver {}", driver ? driver : "(none)");
    }
    monitor = DeceptionSettings::Get().monitor;
    ApplyDisplayMode(DeceptionSettings::Get().display_mode);
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    width = w;
    height = h;
    LOG_INFO(Frontend, "Window {}x{} on {}", w, h, driver);
}

// Windowed, borderless (desktop-sized, no mode change) or exclusive fullscreen at the display's
// largest mode of the aspect ratio (the desktop's when it has none); the fullscreen modes show the
// picture PickFullscreen fits in it (fullscreen_width/height). Window thread only (SDL). The
// swapchain follows the size events.
void WindowSDL::ApplyDisplayMode(int mode) {
    display_mode = mode;
    aspect_ratio = DeceptionSettings::Get().aspect_ratio;
    if (mode == DeceptionGraphics::Windowed) {
        SDL_SetWindowFullscreen(window, false);
    } else {
        auto& settings = DeceptionSettings::Get();
        const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
        SDL_DisplayMode chosen{};
        const SDL_DisplayMode* target = SDL_GetDesktopDisplayMode(display);
        const auto choice = DeceptionSettings::PickFullscreen(
            mode == DeceptionGraphics::Fullscreen, aspect_ratio, target ? std::pair{target->w, target->h} : std::pair{0, 0},
            FullscreenModes(display));
        settings.fullscreen_width = choice.picture.first;
        settings.fullscreen_height = choice.picture.second;
        if (choice.mode.first &&
            SDL_GetClosestFullscreenDisplayMode(display, choice.mode.first, choice.mode.second, 0.0f, false, &chosen)) {
            target = &chosen;
        }
        const bool exclusive = mode == DeceptionGraphics::Fullscreen && target;
        if (!SDL_SetWindowFullscreenMode(window, exclusive ? target : nullptr)) {
            LOG_WARNING(Frontend, "Fullscreen mode: {}", SDL_GetError());
        }
        SDL_SetWindowFullscreen(window, true);
    }
    SDL_SyncWindow(window);
    std::printf("Window: %s\n", DeceptionGraphics::DisplayModeNames[mode]);
}

// A new window size: a window takes it (shrunk to fit the display's usable area), centred; the
// fullscreen modes pick their own (ApplyDisplayMode) and use it when the window is windowed again.
void WindowSDL::ApplyResolution(std::pair<int, int> size) {
    resolution = size;
    if (display_mode == DeceptionGraphics::Windowed) {
        const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
        s32 w = 0, h = 0;
        StartSize(w, h, display);
        SDL_RestoreWindow(window);
        SDL_SetWindowSize(window, w, h);
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display));
        SDL_SyncWindow(window);
    }
    std::printf("Window: resolution %dx%d\n", size.first, size.second);
}

// Moves the window to another display (an index into SDL_GetDisplays(), or PrimaryMonitor),
// centred; a fullscreen window leaves fullscreen for the move and re-enters it there.
void WindowSDL::ApplyMonitor(int index) {
    monitor = index;
    SDL_DisplayID display = SDL_GetPrimaryDisplay();
    int count = 0;
    if (SDL_DisplayID* displays = SDL_GetDisplays(&count)) {
        if (index >= 0 && index < count) display = displays[index];
        SDL_free(displays);
    }
    if (SDL_GetDisplayForWindow(window) == display) return;
    if (display_mode != DeceptionGraphics::Windowed) {
        SDL_SetWindowFullscreen(window, false);
        SDL_SyncWindow(window);
    }
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    SDL_SyncWindow(window);
    if (display_mode != DeceptionGraphics::Windowed) ApplyDisplayMode(display_mode);
    std::printf("Window: moved to display %d\n", index);
}

WindowSDL::~WindowSDL() {
    SDL_DestroyWindow(window);
}

void WindowSDL::BeginTextInput(const std::string& initial, const std::string& prompt) {
    std::scoped_lock lock{text_mutex};
    text = initial;
    text_prompt = prompt;
    text_state = 0;
    text_requested = true;
}

int WindowSDL::PollTextInput(std::string& out) {
    std::scoped_lock lock{text_mutex};
    out = text;
    return text_state;
}

void WindowSDL::UpdateTextTitle() {
    const std::string title = text_active ? base_title + " \u2014 " + text_prompt + ": " + text + "_  (Enter = OK, Esc = cancel)"
                                          : base_title;
    SDL_SetWindowTitle(window, title.c_str());
    DeceptionOverlay::SetTextEntry(text_active, text_prompt, text);
}

#ifdef _WIN32
// DWM cloaking: a cloaked window is shown, sized and drawn into, but not composed on screen.
bool WindowSDL::Cloak(bool on) {
    const BOOL value = on;
    return window_info.type == WindowSystemType::Windows &&
           SUCCEEDED(DwmSetWindowAttribute(static_cast<HWND>(window_info.render_surface), DWMWA_CLOAK,
                                           &value, sizeof(value)));
}
#endif

bool WindowSDL::PollEvents() {
    if (!shown && show_requested.load(std::memory_order_acquire)) {
        shown = true;
#ifdef _WIN32
        // Shown cloaked: it takes its borderless/fullscreen size and the presenter draws the start-up
        // picture into it before anyone sees it, where a plain show shows it black for a moment.
        cloaked = Cloak(true);
        shown_at = SDL_GetTicks();
#endif
        SDL_ShowWindow(window);
        SDL_SyncWindow(window);
        int w = 0, h = 0; // a window created hidden takes its borderless/fullscreen size only now
        SDL_GetWindowSizeInPixels(window, &w, &h);
        width = w;
        height = h;
        shown_done.store(true, std::memory_order_release);
#ifdef _WIN32
        if (!cloaked)
#endif
            revealed.store(true, std::memory_order_release);
    }
#ifdef _WIN32
    // Revealed once the presenter has drawn into it, or after a second whatever happened.
    if (cloaked && (reveal_requested.load(std::memory_order_acquire) || SDL_GetTicks() - shown_at > 1000)) {
        Cloak(false);
        cloaked = false;
        revealed.store(true, std::memory_order_release);
    }
#endif
    {
        std::scoped_lock lock{text_mutex};
        if (text_requested) { // SDL text input must be toggled from the window thread
            text_requested = false;
            text_active = true;
            SDL_StartTextInput(window);
            UpdateTextTitle();
        }
    }
    if (const int mode = DeceptionSettings::Get().display_mode;
        mode != display_mode || (mode != DeceptionGraphics::Windowed && DeceptionSettings::Get().aspect_ratio != aspect_ratio)) {
        ApplyDisplayMode(mode);
    }
    if (const int index = DeceptionSettings::Get().monitor; index != monitor) {
        ApplyMonitor(index);
    }
    if (const std::pair<int, int> size{DeceptionSettings::Get().window_width.load(),
                                       DeceptionSettings::Get().window_height.load()};
        size != resolution) {
        ApplyResolution(size);
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (text_active && (event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_KEY_DOWN)) {
            std::scoped_lock lock{text_mutex};
            if (event.type == SDL_EVENT_TEXT_INPUT) {
                text += event.text.text;
            } else if (event.key.key == SDLK_BACKSPACE && !text.empty()) {
                size_t cut = text.size() - 1; // drop one UTF-8 code point
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
                text.erase(cut);
            } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER || event.key.key == SDLK_ESCAPE) {
                text_state = event.key.key == SDLK_ESCAPE ? 2 : 1;
                text_active = false;
                SDL_StopTextInput(window);
            }
            UpdateTextTitle();
            continue;
        }
        // The controller finishes the text dialog too: Cross (A) accepts, Circle (B) cancels.
        if (text_active && event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
            (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ||
             event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST)) {
            std::scoped_lock lock{text_mutex};
            text_state = event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ? 1 : 2;
            text_active = false;
            SDL_StopTextInput(window);
            UpdateTextTitle();
            continue;
        }
        if (DeceptionOverlay::HandleEvent(event)) {
            continue;
        }
        switch (event.type) {
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED: {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            width = w;
            height = h;
            break;
        }
        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_WINDOW_HIDDEN:
        case SDL_EVENT_WINDOW_OCCLUDED:
            is_minimized.store(true, std::memory_order_release);
            break;
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_SHOWN:
        case SDL_EVENT_WINDOW_EXPOSED: {
            is_minimized.store(false, std::memory_order_release);
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            if (w > 0 && h > 0) {
                width = w;
                height = h;
            }
            break;
        }
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            is_open = false;
            break;
        default:
            break;
        }
    }
    if (window) {
        const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
        const bool min = (flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN | SDL_WINDOW_OCCLUDED)) != 0;
        const bool was_min = is_minimized.exchange(min, std::memory_order_acq_rel);
        if (was_min && !min) {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            if (w > 0 && h > 0) {
                width = w;
                height = h;
            }
        }
    }
    return is_open;
}

bool WindowSDL::IsMinimized() const {
    if (is_minimized.load(std::memory_order_acquire)) {
        return true;
    }
    return width.load(std::memory_order_relaxed) <= 0 || height.load(std::memory_order_relaxed) <= 0;
}

} // namespace Frontend
