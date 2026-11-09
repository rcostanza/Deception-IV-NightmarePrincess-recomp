// SPDX-License-Identifier: GPL-2.0-or-later
// deception-setup.exe: sets up the release folder it sits in (tools/package.py) from the player's
// Deception dump, so the game starts from deception.exe or a desktop shortcut.
//
// With no arguments it opens a window: the game folder (the dump), its update, the extra DLC folder, the save
// folder, whether to skip the intro logos (settings.ini skip_intro), Install and Exit; after an
// install it offers a desktop shortcut. Run it again to change a folder, or after unpacking a newer
// release over the folder.
// With arguments it installs from the command line:
//
//     deception-setup.exe --game <dump or eboot.bin> [--update <folder>|--no-update] [--save <folder>] [--copy-saves]
//                           [--dlc <folder>|--no-dlc] [--skip-intro|--play-intro]
//                           [--shortcut <folder>|desktop] [--progress]

#include "setup.h"

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kTitle[] = L"Deception setup";

// The old save folder to copy from when the save folder changes and only the old one has saves.
std::optional<fs::path> saves_to_move(const fs::path& root, const fs::path& save) {
    auto old = setup::remembered_paths(root);
    if (!old || setup::same_path(old->save, save) || !setup::has_saves(old->save) || setup::has_saves(save))
        return std::nullopt;
    return old->save;
}

// --- command line -------------------------------------------------------------------------------

// Writes to the standard handles a parent gave us, or else to the console of the shell we were started from.
class Console {
public:
    Console() {
        out_ = GetStdHandle(STD_OUTPUT_HANDLE);
        err_ = GetStdHandle(STD_ERROR_HANDLE);
        if ((!out_ || !err_) && AttachConsole(ATTACH_PARENT_PROCESS)) {
            HANDLE con = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_WRITE, nullptr,
                                     OPEN_EXISTING, 0, nullptr);
            if (!out_)
                out_ = con;
            if (!err_)
                err_ = con;
        }
    }
    void out(const std::wstring& s) const { write(out_, s + L"\n"); }
    void err(const std::wstring& s) const { write(err_, s + L"\n"); }

private:
    static void write(HANDLE h, const std::wstring& s) {
        if (!h || h == INVALID_HANDLE_VALUE)
            return;
        DWORD mode, n;
        if (GetConsoleMode(h, &mode)) {
            WriteConsoleW(h, s.data(), static_cast<DWORD>(s.size()), &n, nullptr);
            return;
        }
        int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
        std::string u(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), u.data(), len, nullptr, nullptr);
        WriteFile(h, u.data(), static_cast<DWORD>(u.size()), &n, nullptr);
    }
    HANDLE out_ = nullptr;
    HANDLE err_ = nullptr;
};

constexpr wchar_t kUsage[] =
    L"usage: deception-setup.exe --game <dump or eboot.bin> [--update <folder>|--no-update]\n"
    L"                             [--save <folder>] [--copy-saves]\n"
    L"                             [--dlc <folder>|--no-dlc] [--skip-intro|--play-intro]\n"
    L"                             [--shortcut <folder>|desktop] [--progress]\n"
    L"  --game        the decrypted game dump (eboot.bin, sce_module, sce_sys, FILE_PS4); only ever read\n"
    L"  --update      its decrypted update (patch), laid over the game (default: the one paths.ini\n"
    L"                names, else <game>-patch next to it, as shadPS4 keeps it); --no-update runs without\n"
    L"  --save        where your saves go (default: the one paths.ini names, else save\\ here)\n"
    L"  --copy-saves  when --save changes, copy the saves from the old save folder\n"
    L"  --dlc         one more folder of DLC to read, e.g. shadPS4's addcont (default: the one\n"
    L"                paths.ini names); --no-dlc reads only the save folder's addcont\n"
    L"  --skip-intro, --play-intro\n"
    L"                skip the start-up logos and notices, or show them again\n"
    L"                (settings.ini skip_intro; default: leave it as it is)\n"
    L"  --shortcut    also write the game's shortcut into this folder, or onto the Desktop\n"
    L"  --progress    also print each step as \"progress <0 to 1> <step>\" (RecompLauncher)\n"
    L"Sets up the folder this exe is in. With no arguments, opens the setup window.";

int run_cli(const std::vector<std::wstring>& args) {
    Console con;
    std::wstring game, save, shortcut, dlc, update;
    bool copy_saves = false, no_dlc = false, no_update = false, progress = false;
    std::optional<bool> skip_intro;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::wstring& a = args[i];
        std::wstring* value = a == L"--game"       ? &game
                              : a == L"--save"     ? &save
                              : a == L"--shortcut" ? &shortcut
                              : a == L"--dlc"      ? &dlc
                              : a == L"--update"   ? &update
                                                   : nullptr;
        if (value && i + 1 < args.size()) {
            *value = args[++i];
        } else if (a == L"--copy-saves") {
            copy_saves = true;
        } else if (a == L"--progress") {
            progress = true;
        } else if (a == L"--skip-intro" || a == L"--play-intro") {
            skip_intro = a == L"--skip-intro";
        } else if (a == L"--no-dlc") {
            no_dlc = true;
        } else if (a == L"--no-update") {
            no_update = true;
        } else {
            con.err(a == L"--help" || a == L"-h" ? kUsage : std::wstring(kUsage) + L"\n\nunexpected: " + a);
            return 2;
        }
    }
    if (game.empty()) {
        con.err(std::wstring(kUsage) + L"\n\n--game is required");
        return 2;
    }
    try {
        setup::Options opt;
        opt.root = setup::exe_folder();
        opt.game = game;
        auto old = setup::remembered_paths(opt.root);
        opt.save = !save.empty() ? fs::path(save) : old ? old->save : setup::default_save();
        opt.dlc = no_dlc ? fs::path() : !dlc.empty() ? fs::path(dlc) : old ? old->dlc : fs::path();
        opt.update = no_update            ? fs::path()
                     : !update.empty()    ? fs::path(update)
                     : old && !old->update.empty() ? old->update
                                          : setup::update_beside(opt.game);
        if (copy_saves)
            opt.copy_saves_from = saves_to_move(opt.root, opt.save);
        opt.skip_intro = skip_intro;
        if (progress)
            opt.progress = [&](double done, const std::wstring& what) { con.out(setup::progress_line(done, what)); };
        fs::path exe = setup::install(opt, [&](const std::wstring& line) { con.out(line); });
        if (!shortcut.empty()) {
            fs::path lnk = (shortcut == L"desktop" ? setup::desktop_folder() : fs::path(shortcut)) / setup::kShortcutName;
            setup::create_shortcut(exe, lnk);
            con.out(L"shortcut " + lnk.wstring());
        }
        con.out(L"\nDone. Run " + exe.wstring());
        return 0;
    } catch (const setup::Error& e) {
        con.err(e.message);
        return 1;
    }
}

// --- window -------------------------------------------------------------------------------------
// Drawn like the game's dialogs and its Display Settings page (F10; gpu/shim/deception_overlay.cpp):
// a near-black panel in a gold frame with corner flourishes, the title in Cinzel, text in Josefin
// Slab (both embedded in setup.rc and loaded for this process only), and the action that is ready on
// an amber band. Install stays disabled until the game folder holds a Deception dump, the update and
// DLC folders (optional) are usable when named, and a save folder is named.

enum : int {
    kGameEdit = 101,
    kGameBrowse,
    kUpdateEdit,
    kUpdateBrowse,
    kDlcEdit,
    kDlcBrowse,
    kSaveEdit,
    kSaveBrowse,
    kPlay,
    kSkipIntro,
};
// The fonts in setup.rc (RCDATA), the same as the game's Display Settings page.
enum : int { kTitleFont = 201, kTextFont = 202 };
enum : UINT {
    kMsgLog = WM_APP + 1, // lParam: std::wstring* line
    kMsgDone,             // wParam: 1 ok; lParam: std::wstring* exe path or error
};

namespace color {
// The game's dialogs, as the Display Settings page draws them (gpu/shim/deception_overlay.cpp
// PageInk, here in sRGB): a near-black panel in a gold frame, cream text, and the action that is
// ready on an amber band.
constexpr COLORREF Back = RGB(0x0b, 0x08, 0x06), Panel = RGB(0x07, 0x04, 0x03), Field = RGB(0x14, 0x0e, 0x0a);
constexpr COLORREF GoldHi = RGB(0xf0, 0xc0, 0x70), Gold = RGB(0xc0, 0x8a, 0x3c), GoldDim = RGB(0x6e, 0x4a, 0x1e);
constexpr COLORREF Hair = RGB(0x4a, 0x32, 0x16), BandMid = RGB(0x9a, 0x5a, 0x1c), BandEdge = RGB(0x2c, 0x16, 0x08);
constexpr COLORREF Text = RGB(0xf5, 0xec, 0xd8), Dim = RGB(0xbc, 0xaa, 0x8a), Off = RGB(0x5e, 0x52, 0x42);
constexpr COLORREF Title = RGB(0xf2, 0xd4, 0x9c), Help = RGB(0xe8, 0xda, 0xbd);
constexpr COLORREF Good = RGB(0xe6, 0xc8, 0x7e), Bad = RGB(0xe8, 0x6a, 0x48);
} // namespace color

// One folder row: its label, the field and Browse, and what the check says about the folder.
struct Folder {
    const wchar_t* label;
    int y;
    HWND edit = nullptr, browse = nullptr;
    std::wstring note;
    bool ok = false;
};

struct Ui {
    HWND wnd = nullptr, play = nullptr, install = nullptr, exit = nullptr, skip_intro = nullptr;
    Folder game{L"GAME FOLDER", 90}, update{L"UPDATE FOLDER (OPTIONAL)", 166}, dlc{L"EXTRA DLC FOLDER (OPTIONAL)", 242},
        save{L"SAVE FOLDER", 318};
    std::vector<HWND> inputs; // disabled while installing (Play Game too, and until the folder is set up)
    HFONT font = nullptr, bold = nullptr, title = nullptr;
    HBRUSH field = nullptr;
    UINT dpi = 96;
    bool busy = false, skip = true;
    std::wstring status;
    bool status_bad = false;
    fs::path root;
};
Ui ui;

// Layout in 96-dpi units; the client area is kWidth x kHeight.
constexpr int kWidth = 640, kHeight = 570;
constexpr int kRowLeft = 44, kRowRight = 596, kRowHeight = 66, kFieldLeft = 58, kBrowseLeft = 480, kBrowseWidth = 100;
constexpr int kPanelTop = 14, kPanelBottom = 448, kButtonsTop = 516;

int px(int v) {
    return MulDiv(v, static_cast<int>(ui.dpi), 96);
}
float pxf(float v) {
    return v * static_cast<float>(ui.dpi) / 96.0f;
}

RECT rect(int x, int y, int w, int h) {
    return {px(x), px(y), px(x + w), px(y + h)};
}

std::wstring text_of(HWND h) {
    std::wstring s(static_cast<size_t>(GetWindowTextLengthW(h)), L'\0');
    GetWindowTextW(h, s.data(), static_cast<int>(s.size()) + 1);
    size_t a = s.find_first_not_of(L" \t\""), b = s.find_last_not_of(L" \t\"");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}

// --- drawing ---
// Flat fills and text with GDI; the gold lines, curls and the amber band with GDI+, antialiased.

void fill(HDC dc, RECT r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void text(HDC dc, const std::wstring& s, RECT r, HFONT font, COLORREF c, UINT format) {
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    DrawTextW(dc, s.c_str(), static_cast<int>(s.size()), &r, format | DT_NOPREFIX);
    SelectObject(dc, old);
}

Gdiplus::Color gp(COLORREF c, BYTE alpha = 255) {
    return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
}

// A GDI+ canvas on `dc`, antialiased; destroyed before GDI draws on the DC again.
struct Canvas {
    Gdiplus::Graphics g;
    explicit Canvas(HDC dc) : g(dc) { g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias); }
    void line(float x0, float y0, float x1, float y1, COLORREF c, float w) {
        Gdiplus::Pen pen(gp(c), w);
        g.DrawLine(&pen, x0, y0, x1, y1);
    }
    void rect(float x0, float y0, float x1, float y1, COLORREF c, float w) {
        Gdiplus::Pen pen(gp(c), w);
        g.DrawRectangle(&pen, x0, y0, x1 - x0, y1 - y0);
    }
    void diamond(float x, float y, float r, COLORREF c) {
        Gdiplus::SolidBrush brush(gp(c));
        Gdiplus::PointF pts[4] = {{x, y - r}, {x + r, y}, {x, y + r}, {x - r, y}};
        g.FillPolygon(&brush, pts, 4);
    }
    // A thin gold rule with a diamond at each end and one in the middle.
    void rule(float x0, float x1, float y) {
        line(x0, y, x1, y, color::Gold, pxf(1.2f));
        diamond(x0, y, pxf(3.0f), color::Gold);
        diamond(x1, y, pxf(3.0f), color::Gold);
        diamond((x0 + x1) * 0.5f, y, pxf(4.5f), color::GoldHi);
    }
    // A scrolled flourish in a frame's corner (cx, cy), curling inward along (sx, sy).
    void flourish(float cx, float cy, float sx, float sy) {
        const float k = pxf(15.0f), w = pxf(1.4f);
        auto at = [&](float x, float y) { return Gdiplus::PointF(cx + sx * x * k, cy + sy * y * k); };
        Gdiplus::Pen hi(gp(color::GoldHi), w), mid(gp(color::Gold), w);
        g.DrawBezier(&hi, at(0.35f, 0.35f), at(1.6f, 0.2f), at(2.6f, 1.4f), at(1.7f, 1.5f));
        g.DrawBezier(&hi, at(0.35f, 0.35f), at(0.2f, 1.6f), at(1.4f, 2.6f), at(1.5f, 1.7f));
        const float r = 0.24f * k;
        Gdiplus::PointF a = at(1.75f, 1.25f), b = at(1.25f, 1.75f);
        g.DrawEllipse(&mid, a.X - r, a.Y - r, 2 * r, 2 * r);
        g.DrawEllipse(&mid, b.X - r, b.Y - r, 2 * r, 2 * r);
        g.DrawBezier(&mid, at(0.6f, 0.3f), at(1.8f, 0.0f), at(2.6f, 0.0f), at(3.4f, 0.3f));
        g.DrawBezier(&mid, at(0.3f, 0.6f), at(0.0f, 1.8f), at(0.0f, 2.6f), at(0.3f, 3.4f));
        const Gdiplus::PointF c = at(0.55f, 0.55f);
        diamond(c.X, c.Y, 0.32f * k, color::GoldHi);
    }
    // The game's dialog frame: a bright gold line, a darker one just within it, a flourish in each corner.
    void frame(RECT r) {
        const float x0 = float(r.left), y0 = float(r.top), x1 = float(r.right), y1 = float(r.bottom), in = pxf(5.0f);
        rect(x0, y0, x1, y1, color::GoldHi, pxf(2.2f));
        rect(x0 + in, y0 + in, x1 - in, y1 - in, color::GoldDim, pxf(1.0f));
        flourish(x0, y0, 1, 1);
        flourish(x1, y0, -1, 1);
        flourish(x0, y1, 1, -1);
        flourish(x1, y1, -1, -1);
    }
    // The ornament at an end of the amber band: a gold lance pointing at it (dir 1 points right), a
    // crescent behind its tip and a diamond at its point.
    void ornament(float tx, float ty, float size, int dir) {
        const float d = float(dir), w = pxf(1.4f);
        line(tx - d * size * 1.6f, ty, tx - d * size * 0.35f, ty, color::GoldHi, w);
        const float cx = tx - d * size * 0.95f, r = size * 0.42f;
        Gdiplus::Pen pen(gp(color::GoldHi), w);
        g.DrawArc(&pen, cx - r, ty - r, 2 * r, 2 * r, dir > 0 ? 99.0f : -81.0f, 162.0f);
        diamond(cx, ty, size * 0.16f, color::GoldHi);
        diamond(tx - d * size * 0.25f, ty, size * 0.2f, color::GoldHi);
    }
    // The chosen answer's band: amber, brightest in the middle, between gold lines.
    void band(RECT r) {
        const float x0 = float(r.left), y0 = float(r.top), x1 = float(r.right), y1 = float(r.bottom), mid = (x0 + x1) * 0.5f;
        Gdiplus::LinearGradientBrush left(Gdiplus::PointF(x0 - 1, y0), Gdiplus::PointF(mid + 1, y0), gp(color::BandEdge),
                                          gp(color::BandMid));
        Gdiplus::LinearGradientBrush right(Gdiplus::PointF(mid - 1, y0), Gdiplus::PointF(x1 + 1, y0), gp(color::BandMid),
                                           gp(color::BandEdge));
        g.FillRectangle(&left, x0, y0, mid - x0, y1 - y0);
        g.FillRectangle(&right, mid, y0, x1 - mid, y1 - y0);
        line(x0, y0, x1, y0, color::Gold, pxf(1.2f));
        line(x0, y1, x1, y1, color::Gold, pxf(1.2f));
    }
};

// The box the edit sits in: a dark field in a dark gold line.
RECT field_box(const Folder& f) {
    return rect(kFieldLeft, f.y + 32, kBrowseLeft - kFieldLeft - 10, 28);
}

void paint(HDC dc) {
    RECT client;
    GetClientRect(ui.wnd, &client);
    fill(dc, client, color::Back);
    RECT panel = rect(14, kPanelTop, kWidth - 28, kPanelBottom - kPanelTop);
    fill(dc, panel, color::Panel);
    for (const Folder* f : {&ui.game, &ui.update, &ui.dlc, &ui.save})
        fill(dc, field_box(*f), color::Field);
    {
        Canvas c(dc);
        c.frame(panel);
        c.rule(pxf(kWidth * 0.28f), pxf(kWidth * 0.72f), pxf(66));
        for (const Folder* f : {&ui.game, &ui.update, &ui.dlc, &ui.save}) {
            RECT row = rect(kRowLeft, f->y, kRowRight - kRowLeft, kRowHeight);
            c.rect(float(row.left), float(row.top), float(row.right), float(row.bottom), color::Hair, pxf(1.0f));
            RECT box = field_box(*f);
            c.rect(float(box.left), float(box.top), float(box.right), float(box.bottom), color::GoldDim, pxf(1.0f));
        }
        c.rule(pxf(40), pxf(kWidth - 40), pxf(kPanelBottom + 56));
    }
    text(dc, L"DECEPTION SETUP", rect(0, 26, kWidth, 34), ui.title, color::Title, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    for (const Folder* f : {&ui.game, &ui.update, &ui.dlc, &ui.save}) {
        text(dc, f->label, rect(kFieldLeft, f->y + 6, 260, 24), ui.bold, color::Text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        text(dc, f->note, rect(kFieldLeft + 260, f->y + 6, kRowRight - 16 - kFieldLeft - 260, 24), ui.font,
             f->ok ? color::Good : color::Bad, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    text(dc, ui.status, rect(28, kPanelBottom + 8, kWidth - 56, 42), ui.font, ui.status_bad ? color::Bad : color::Help,
         DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
}

// Owner-drawn buttons, as the game's answers: the action that is ready on the amber band between two
// ornaments, the others in a dark gold outline, and dim text when disabled. Browse is a small
// outlined box; the intro option a gold check box.
void draw_button(const DRAWITEMSTRUCT& d) {
    HDC dc = d.hDC;
    RECT r = d.rcItem;
    const bool browse = d.CtlID == kGameBrowse || d.CtlID == kUpdateBrowse || d.CtlID == kDlcBrowse || d.CtlID == kSaveBrowse;
    fill(dc, r, d.hwndItem == ui.skip_intro || browse ? color::Panel : color::Back);
    bool disabled = d.itemState & ODS_DISABLED, pressed = d.itemState & ODS_SELECTED, focus = d.itemState & ODS_FOCUS;
    wchar_t label[128] = {};
    GetWindowTextW(d.hwndItem, label, 128);
    if (d.hwndItem == ui.skip_intro) {
        RECT box = {r.left + px(2), r.top + (r.bottom - r.top - px(18)) / 2, r.left + px(20), 0};
        box.bottom = box.top + px(18);
        fill(dc, box, color::Field);
        {
            Canvas c(dc);
            c.rect(float(box.left), float(box.top), float(box.right - 1), float(box.bottom - 1),
                   focus ? color::GoldHi : color::GoldDim, focus ? pxf(1.8f) : pxf(1.0f));
            if (ui.skip) {
                Gdiplus::Pen pen(gp(color::GoldHi), pxf(2.0f));
                Gdiplus::PointF check[3] = {{float(box.left) + pxf(4), float(box.top) + pxf(9)},
                                            {float(box.left) + pxf(8), float(box.top) + pxf(13)},
                                            {float(box.left) + pxf(14), float(box.top) + pxf(4)}};
                c.g.DrawLines(&pen, check, 3);
            }
        }
        RECT t = {box.right + px(10), r.top, r.right, r.bottom};
        text(dc, label, t, ui.font, disabled ? color::Off : color::Help, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    const bool primary = !browse && !disabled && d.hwndItem == (setup::ready(ui.root) && !IsWindowEnabled(ui.install) ? ui.play : ui.install);
    {
        Canvas c(dc);
        const float x0 = float(r.left), y0 = float(r.top), x1 = float(r.right - 1), y1 = float(r.bottom - 1);
        if (primary) {
            const float orn = (y1 - y0) * 0.5f;
            RECT band = {r.left + int(orn * 1.2f), r.top + px(2), r.right - int(orn * 1.2f), r.bottom - px(2)};
            c.band(band);
            c.ornament(float(band.left) + orn * 0.2f, (y0 + y1) * 0.5f, orn, 1);
            c.ornament(float(band.right) - orn * 0.2f, (y0 + y1) * 0.5f, orn, -1);
        } else {
            const float in = browse ? 0.0f : pxf(10.0f);
            c.rect(x0 + in, y0 + pxf(2), x1 - in, y1 - pxf(2), focus ? color::Gold : browse ? color::GoldDim : color::Hair,
                   focus ? pxf(1.6f) : pxf(1.0f));
        }
        if (focus && primary) c.rect(x0 + pxf(1), y0 + pxf(1), x1 - pxf(1), y1 - pxf(1), color::GoldDim, pxf(1.0f));
    }
    RECT t = r;
    if (pressed) OffsetRect(&t, px(1), px(1));
    text(dc, label, t, browse ? ui.font : ui.bold, disabled ? color::Off : primary || focus ? color::Text : color::Dim,
         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// --- state ---

void set_status(std::wstring s, bool bad = false) {
    ui.status = std::move(s);
    ui.status_bad = bad;
    RECT r = rect(0, kPanelBottom + 4, kWidth, 48);
    InvalidateRect(ui.wnd, &r, FALSE);
}

// Checks the four folders, shows the result in each row and enables Install when all are usable.
void validate() {
    std::error_code ec;
    std::wstring game = text_of(ui.game.edit), update = text_of(ui.update.edit), dlc = text_of(ui.dlc.edit),
                 save = text_of(ui.save.edit);
    ui.game.ok = false;
    if (game.empty()) {
        ui.game.note = L"Required: your decrypted dump";
    } else if (!fs::is_directory(setup::game_folder(game), ec)) {
        ui.game.note = L"Folder not found";
    } else if (auto missing = setup::missing_game_files(game); !missing.empty()) {
        ui.game.note = L"Not a Deception dump: no " + missing.front();
    } else {
        ui.game.ok = true;
        ui.game.note = L"Deception dump found";
    }
    ui.update.ok = false;
    if (update.empty()) {
        ui.update.ok = true;
        ui.update.note = L"Optional: none chosen";
    } else if (!fs::is_directory(setup::game_folder(update), ec)) {
        ui.update.note = L"Folder not found";
    } else if (auto missing = setup::missing_update_files(setup::game_folder(update)); !missing.empty()) {
        ui.update.note = L"Not an update: no " + missing.front();
    } else {
        ui.update.ok = true;
        ui.update.note = L"Update found";
    }
    ui.dlc.ok = dlc.empty() || fs::is_directory(fs::path(dlc), ec);
    ui.dlc.note = dlc.empty() ? L"Optional: none chosen" : ui.dlc.ok ? L"Folder found" : L"Folder not found";
    ui.save.ok = !save.empty();
    ui.save.note = !ui.save.ok                         ? L"Required"
                   : fs::is_directory(fs::path(save), ec) ? L"Folder found"
                                                        : L"Created when you install";
    EnableWindow(ui.install, !ui.busy && ui.game.ok && ui.update.ok && ui.dlc.ok && ui.save.ok);
    InvalidateRect(ui.wnd, nullptr, FALSE);
    InvalidateRect(ui.install, nullptr, FALSE);
    InvalidateRect(ui.play, nullptr, FALSE);
}

void set_busy(bool busy) {
    ui.busy = busy;
    for (HWND h : ui.inputs)
        EnableWindow(h, !busy);
    EnableWindow(ui.play, !busy && setup::ready(ui.root));
    validate();
}

// Starts the game as a double-click would (its own folder, no console), then closes the setup.
void play() {
    fs::path exe = ui.root / L"deception.exe";
    auto started = reinterpret_cast<INT_PTR>(
        ShellExecuteW(ui.wnd, L"open", exe.c_str(), nullptr, ui.root.c_str(), SW_SHOWNORMAL));
    if (started <= 32) {
        MessageBoxW(ui.wnd, (L"Cannot start " + exe.wstring()).c_str(), kTitle, MB_OK | MB_ICONERROR);
        return;
    }
    DestroyWindow(ui.wnd);
}

// The shell's folder picker, starting in the folder the field names when it exists.
void browse(HWND edit, const wchar_t* title) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetTitle(title);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    std::wstring current = text_of(edit);
    if (!current.empty()) {
        std::error_code ec;
        fs::path start = setup::game_folder(current);
        IShellItem* item = nullptr;
        if (fs::is_directory(start, ec) &&
            SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
            dlg->SetFolder(item);
            item->Release();
        }
    }
    IShellItem* result = nullptr;
    if (SUCCEEDED(dlg->Show(ui.wnd)) && SUCCEEDED(dlg->GetResult(&result))) {
        PWSTR path = nullptr;
        if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            SetWindowTextW(edit, path); // EN_CHANGE validates it
            CoTaskMemFree(path);
        }
        result->Release();
    }
    dlg->Release();
}

void start_install() {
    if (ui.busy || !IsWindowEnabled(ui.install))
        return;
    std::wstring game = text_of(ui.game.edit), save = text_of(ui.save.edit), dlc = text_of(ui.dlc.edit);
    std::wstring update = text_of(ui.update.edit);
    setup::Options opt;
    opt.root = ui.root;
    opt.game = game;
    opt.save = save;
    opt.dlc = dlc;
    opt.update = update;
    opt.skip_intro = ui.skip;
    try {
        if (auto old = saves_to_move(opt.root, opt.save)) {
            std::wstring ask = L"Copy your saves to the new save folder?\n\nFrom: " + old->wstring() + L"\nTo: " + save;
            int answer = MessageBoxW(ui.wnd, ask.c_str(), kTitle, MB_YESNOCANCEL | MB_ICONQUESTION);
            if (answer == IDCANCEL)
                return;
            if (answer == IDYES)
                opt.copy_saves_from = old;
        }
    } catch (const setup::Error& e) {
        MessageBoxW(ui.wnd, e.message.c_str(), kTitle, MB_OK | MB_ICONERROR);
        return;
    }
    set_status(L"Installing...");
    set_busy(true);
    std::thread([opt, wnd = ui.wnd] {
        auto log = [wnd](const std::wstring& line) {
            PostMessageW(wnd, kMsgLog, 0, reinterpret_cast<LPARAM>(new std::wstring(line)));
        };
        try {
            fs::path exe = setup::install(opt, log);
            PostMessageW(wnd, kMsgDone, 1, reinterpret_cast<LPARAM>(new std::wstring(exe.wstring())));
        } catch (const setup::Error& e) {
            PostMessageW(wnd, kMsgDone, 0, reinterpret_cast<LPARAM>(new std::wstring(e.message)));
        }
    }).detach();
}

void finish_install(bool ok, const std::wstring& value) {
    set_busy(false);
    if (!ok) {
        set_status(L"Install failed. Check the folders and press Install again.", true);
        MessageBoxW(ui.wnd, value.c_str(), kTitle, MB_OK | MB_ICONERROR);
        return;
    }
    set_status(L"Done. Press Play Game to start.");
    if (MessageBoxW(ui.wnd, L"Done!\n\nAdd a shortcut to the game on your desktop?", kTitle,
                    MB_YESNO | MB_ICONQUESTION) == IDYES) {
        try {
            setup::create_shortcut(value, setup::desktop_folder() / setup::kShortcutName);
            set_status(L"Done, with a desktop shortcut. Press Play Game to start.");
        } catch (const setup::Error& e) {
            MessageBoxW(ui.wnd, e.message.c_str(), kTitle, MB_OK | MB_ICONERROR);
        }
    }
    SetFocus(ui.play); // Enter now starts the game
}

// --- controls ---

HWND add(const wchar_t* cls, const wchar_t* label, DWORD style, RECT r, int id) {
    HWND c = CreateWindowExW(0, cls, label, WS_CHILD | WS_VISIBLE | style, r.left, r.top, r.right - r.left,
                             r.bottom - r.top, ui.wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(ui.font), FALSE);
    return c;
}

void add_folder(Folder& f, int edit_id, int browse_id) {
    RECT box = field_box(f);
    // The edit inside its drawn box, its text centred on the box's height.
    int h = px(20);
    RECT r = {box.left + px(7), box.top + (box.bottom - box.top - h) / 2, box.right - px(7), 0};
    r.bottom = r.top + h;
    f.edit = add(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, r, edit_id);
    f.browse = add(WC_BUTTONW, L"Browse...", WS_TABSTOP | BS_OWNERDRAW, rect(kBrowseLeft, f.y + 31, kBrowseWidth, 30),
                   browse_id);
}

void create_controls() {
    // Tab order: the four folders top to bottom, the intro box, then the buttons.
    add_folder(ui.game, kGameEdit, kGameBrowse);
    add_folder(ui.update, kUpdateEdit, kUpdateBrowse);
    add_folder(ui.dlc, kDlcEdit, kDlcBrowse);
    add_folder(ui.save, kSaveEdit, kSaveBrowse);
    ui.skip_intro = add(WC_BUTTONW, L"Skip the intro to the title menu (skip_intro=0 in settings.ini brings it back)",
                        WS_TABSTOP | BS_OWNERDRAW, rect(kRowLeft + 4, 398, kRowRight - kRowLeft - 8, 30), kSkipIntro);
    // Play Game, Install and Exit, centred under the rule.
    constexpr int kButtonWidth = 156, kButtonGap = 12, kButtonsLeft = (kWidth - 3 * kButtonWidth - 2 * kButtonGap) / 2;
    ui.play = add(WC_BUTTONW, L"Play Game", WS_TABSTOP | BS_OWNERDRAW,
                  rect(kButtonsLeft, kButtonsTop, kButtonWidth, 36), kPlay);
    ui.install = add(WC_BUTTONW, L"Install", WS_TABSTOP | BS_OWNERDRAW,
                     rect(kButtonsLeft + kButtonWidth + kButtonGap, kButtonsTop, kButtonWidth, 36), IDOK);
    ui.exit = add(WC_BUTTONW, L"Exit", WS_TABSTOP | BS_OWNERDRAW,
                  rect(kButtonsLeft + 2 * (kButtonWidth + kButtonGap), kButtonsTop, kButtonWidth, 36), IDCANCEL);
    ui.inputs = {ui.game.edit, ui.game.browse, ui.update.edit, ui.update.browse, ui.dlc.edit, ui.dlc.browse, ui.save.edit, ui.save.browse,
                 ui.skip_intro, ui.exit};

    // Start from what the last install chose (paths.ini here, else the per-user copy), else an empty
    // game and DLC folder and save\ here.
    std::optional<setup::Paths> paths;
    try {
        paths = setup::remembered_paths(ui.root);
    } catch (const setup::Error&) {
    }
    fs::path save = paths ? paths->save : setup::default_save();
    if (paths) {
        SetWindowTextW(ui.game.edit, paths->game.c_str());
        SetWindowTextW(ui.update.edit, paths->update.c_str());
        SetWindowTextW(ui.dlc.edit, paths->dlc.c_str());
    }
    SetWindowTextW(ui.save.edit, save.c_str());
    // What settings.ini says now; Install writes the box's state, so unticking it brings the logos back.
    ui.skip = setup::skip_intro(ui.root, save);
    set_status(setup::ready(ui.root) ? L"The game is set up. Press Play Game, or change the folders and press Install."
               : paths              ? L"This version isn't set up yet. Press Install."
                                    : L"Choose your game dump (and its update and a DLC folder if you have them), then press Install.");
    set_busy(false);
}

LRESULT CALLBACK wnd_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE) {
            validate();
            return 0;
        }
        switch (LOWORD(wp)) {
        case IDOK:
            start_install();
            return 0;
        case kPlay:
            play();
            return 0;
        case IDCANCEL:
            if (!ui.busy)
                DestroyWindow(wnd);
            return 0;
        case kSkipIntro:
            ui.skip = !ui.skip;
            InvalidateRect(ui.skip_intro, nullptr, FALSE);
            return 0;
        case kGameBrowse:
            browse(ui.game.edit, L"Select the game folder (the one with eboot.bin)");
            // shadPS4 keeps the update next to the game as <game>-patch: take it when none is named.
            if (text_of(ui.update.edit).empty())
                if (fs::path update = setup::update_beside(text_of(ui.game.edit)); !update.empty())
                    SetWindowTextW(ui.update.edit, update.c_str());
            return 0;
        case kUpdateBrowse:
            browse(ui.update.edit, L"Select the game's update folder (the patch, with its own eboot.bin)");
            return 0;
        case kDlcBrowse:
            browse(ui.dlc.edit, L"Select your DLC folder (e.g. shadPS4's addcont)");
            return 0;
        case kSaveBrowse:
            browse(ui.save.edit, L"Select the save folder");
            return 0;
        }
        break;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: { // a disabled edit asks with WM_CTLCOLORSTATIC
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, IsWindowEnabled(reinterpret_cast<HWND>(lp)) ? color::Text : color::Dim);
        SetBkColor(dc, color::Field);
        return reinterpret_cast<LRESULT>(ui.field);
    }
    case WM_DRAWITEM:
        draw_button(*reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
        return TRUE;
    case WM_ERASEBKGND:
        return 1; // WM_PAINT draws everything
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(wnd, &ps);
        RECT client;
        GetClientRect(wnd, &client);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, client.right, client.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        paint(mem);
        BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(wnd, &ps);
        return 0;
    }
    case kMsgLog: {
        std::unique_ptr<std::wstring> line(reinterpret_cast<std::wstring*>(lp));
        set_status(*line);
        return 0;
    }
    case kMsgDone: {
        std::unique_ptr<std::wstring> value(reinterpret_cast<std::wstring*>(lp));
        finish_install(wp == 1, *value);
        return 0;
    }
    case WM_CLOSE:
        if (!ui.busy)
            DestroyWindow(wnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

HFONT make_font(const LOGFONTW& base, const wchar_t* face, int height, int weight) {
    LOGFONTW lf = base;
    lf.lfHeight = -px(height);
    lf.lfWeight = weight;
    lf.lfQuality = CLEARTYPE_QUALITY;
    if (face)
        lstrcpynW(lf.lfFaceName, face, LF_FACESIZE);
    return CreateFontIndirectW(&lf);
}

// Makes a TTF in setup.rc (RCDATA `id`) usable by this process only; false when it is missing.
bool load_font(int id) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL data = res ? LoadResource(nullptr, res) : nullptr;
    void* bytes = data ? LockResource(data) : nullptr;
    DWORD count = 0;
    return bytes && AddFontMemResourceEx(bytes, SizeofResource(nullptr, res), nullptr, &count) && count;
}

int run_window(HINSTANCE inst) {
    ui.root = setup::exe_folder();
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    ui.dpi = GetDpiForSystem();
    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, ui.dpi);
    // Cinzel Bold and Josefin Slab SemiBold, as the game's pages; the system's font if they are missing.
    const bool title_font = load_font(kTitleFont), text_font = load_font(kTextFont);
    const wchar_t* text_face = text_font ? L"Josefin Slab SemiBold" : nullptr;
    ui.font = make_font(ncm.lfMessageFont, text_face, 15, FW_NORMAL);
    ui.bold = make_font(ncm.lfMessageFont, text_face, 16, text_font ? FW_NORMAL : FW_SEMIBOLD);
    ui.title = make_font(ncm.lfMessageFont, title_font ? L"Cinzel" : nullptr, 28, FW_BOLD);
    Gdiplus::GdiplusStartupInput gdiplus_input;
    ULONG_PTR gdiplus = 0;
    Gdiplus::GdiplusStartup(&gdiplus, &gdiplus_input, nullptr);
    ui.field = CreateSolidBrush(color::Field);

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"deception_setup";
    RegisterClassExW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT r = {0, 0, px(kWidth), px(kHeight)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, ui.dpi);
    ui.wnd = CreateWindowExW(0, wc.lpszClassName, kTitle, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                             r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    // A dark title bar in the window's background colour (Windows 11; older versions ignore it).
    BOOL dark = TRUE;
    DwmSetWindowAttribute(ui.wnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    COLORREF caption = color::Back;
    DwmSetWindowAttribute(ui.wnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    create_controls();
    ShowWindow(ui.wnd, SW_SHOW);
    SetFocus(ui.game.edit);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(ui.wnd, &msg)) // Tab between controls, Enter = Install, Esc = Exit
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DeleteObject(ui.font);
    DeleteObject(ui.bold);
    DeleteObject(ui.title);
    DeleteObject(ui.field);
    Gdiplus::GdiplusShutdown(gdiplus);
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args(argv + 1, argv + argc);
    LocalFree(argv);
    if (!args.empty())
        return run_cli(args);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int rc = run_window(inst);
    CoUninitialize();
    return rc;
}
