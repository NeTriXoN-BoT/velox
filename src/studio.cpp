#include "studio.hpp"
#include "window_scale.hpp"
#include "art.hpp"
#include "presets.hpp"
#include "playtools.hpp"
#include "controllers.hpp"
#include <dwmapi.h>
#include <windowsx.h>
#include <memory>
#include <commctrl.h>
#include <uxtheme.h>
namespace phone::studio {
namespace {
struct Hit {
    gp::RectF rect;
    std::function<void()> action;
    bool enabled;
};
struct Window {
    HWND hwnd = nullptr, owner = nullptr, edit = nullptr;
    HFONT editFont = nullptr;
    HBRUSH editBrush = nullptr;
    std::wstring initial;
    std::function<void(const std::wstring &)> inputCallback;
    ~Window() {
        if (editFont)
            DeleteObject(editFont);
        if (editBrush)
            DeleteObject(editBrush);
    }
    int type = 0, hover = -1, pressed = -1, bodyHeight = 113;
    float scale = 1;
    experience::AppInfo app;
    Settings global;
    presets::Preset preset;
    playtools::Profile pad;
    std::unique_ptr<controllers::Reader> reader;
    std::vector<Hit> hits;
    std::wstring error, title, message, accept;
    std::function<void(bool)> callback;
    bool creating = true;
    int height() const { return type == 3 ? 370 : type == 2 ? bodyHeight + 227 : type == 1 ? 650 : 660; }
    void measureConfirmation() {
        if (type != 2)
            return;
        gp::Bitmap bitmap(1, 1, PixelFormat32bppARGB);
        gp::Graphics graphics(&bitmap);
        gp::Font font(L"Segoe UI", 14, gp::FontStyleRegular, gp::UnitPixel);
        gp::StringFormat format;
        gp::RectF measured;
        auto display = i18n::tr(message);
        graphics.MeasureString(display.c_str(), -1, &font, gp::RectF(0, 0, 624, 1000), &format, &measured);
        bodyHeight = std::clamp(int(measured.Height + 20), 113, 360);
    }
    void layout(const RECT *suggested = nullptr) {
        if (!hwnd)
            return;
        auto screen = scaling::desktop(hwnd, suggested);
        auto frame = scaling::outer(hwnd, 0, 0, screen.dpi);
        int percent = scaling::fit(screen.width() - (frame.right - frame.left), screen.height() - (frame.bottom - frame.top), 720, height(),
                                   int(std::lround(scaling::ownerScale(owner, screen.dpi, screen.height()) * 100)));
        scale = percent / 100.f;
        RECT current{};
        GetWindowRect(hwnd, &current);
        auto bounds = scaling::outer(hwnd, int(std::lround(720 * scale)), int(std::lround(height() * scale)), screen.dpi);
        int width = bounds.right - bounds.left, h = bounds.bottom - bounds.top;
        auto position = scaling::position(suggested ? *suggested : current, screen, width, h);
        SetWindowPos(hwnd, nullptr, position.x, position.y, width, h, SWP_NOACTIVATE | SWP_NOZORDER);
        if (edit) {
            HFONT next = CreateFontW(-int(std::lround(16 * scale)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            if (next) {
                SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(next), TRUE);
                if (editFont)
                    DeleteObject(editFont);
                editFont = next;
            }
            SetWindowPos(edit, nullptr, int(std::lround(44 * scale)), int(std::lround(209 * scale)), int(std::lround(628 * scale)),
                         int(std::lround(26 * scale)), SWP_NOZORDER | SWP_NOACTIVATE);
        }
        hover = pressed = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    void text(gp::Graphics &g, const std::wstring &s, float x, float y, float w, float h, float size = 13, gp::ARGB color = C_TEXT,
              bool bold = false, bool centered = false, bool raw = false) {
        gp::Font font(L"Segoe UI", size, bold ? gp::FontStyleBold : gp::FontStyleRegular, gp::UnitPixel);
        gp::SolidBrush brush{gp::Color(color)};
        gp::StringFormat format;
        format.SetLineAlignment(gp::StringAlignmentCenter);
        format.SetAlignment(centered ? gp::StringAlignmentCenter : gp::StringAlignmentNear);
        format.SetTrimming(gp::StringTrimmingEllipsisCharacter);
        auto translated = raw ? s : i18n::tr(s);
        g.DrawString(translated.c_str(), -1, &font, gp::RectF(x, y, w, h), &format, &brush);
    }
    void card(gp::Graphics &g, float x, float y, float w, float h) {
        gp::SolidBrush b{gp::Color(C_CARD)};
        art::fillRound(g, gp::RectF(x, y, w, h), 16, b);
    }
    void button(gp::Graphics &g, const std::wstring &label, gp::RectF r, std::function<void()> action, bool primary = false,
                bool enabled = true) {
        int id = int(hits.size());
        hits.push_back({r, std::move(action), enabled});
        if (primary && enabled) {
            gp::LinearGradientBrush b(r, gp::Color(0xff9468ff), gp::Color(0xff367aff), gp::LinearGradientModeHorizontal);
            art::fillRound(g, r, 11, b);
        } else {
            gp::SolidBrush b{gp::Color(!enabled ? 0xff171a23 : hover == id ? 0xff34374b : C_CHIP)};
            art::fillRound(g, r, 11, b);
        }
        text(g, label, r.X, r.Y, r.Width, r.Height, 13, enabled ? C_TEXT : C_FAINT, primary, true);
    }
    void choices(gp::Graphics &g, float y, const std::wstring &label, const std::vector<std::wstring> &names, int selected,
                 std::function<void(int)> choose, std::function<bool(int)> enabled = {}) {
        text(g, label, 32, y, 656, 18, 11, C_DIM, true);
        float gap = 5, width = (656 - gap * (names.size() - 1)) / names.size();
        for (size_t i = 0; i < names.size(); i++)
            button(
                g, names[i], gp::RectF(32 + i * (width + gap), y + 23, width, 34), [choose, i] { choose(int(i)); }, int(i) == selected,
                !enabled || enabled(int(i)));
    }

    void acceptInput() {
        wchar_t value[41]{};
        GetWindowTextW(edit, value, 41);
        std::wstring name(value);
        while (!name.empty() && iswspace(name.front()))
            name.erase(name.begin());
        while (!name.empty() && iswspace(name.back()))
            name.pop_back();
        if (name.empty() || name.find_first_of(L"\r\n\t[]=\x7f") != std::wstring::npos) {
            error = i18n::text(L"Use 1–40 characters without line breaks or INI symbols.");
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
        auto cb = inputCallback;
        close(true);
        if (cb)
            cb(name);
    }
    void close(bool accepted = false) {
        auto cb = callback;
        auto handle = hwnd;
        if (handle)
            DestroyWindow(handle);
        if (cb)
            cb(accepted);
    }
    void draw(gp::Graphics &g) {
        hits.clear();
        g.Clear(gp::Color(C_BG));
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(gp::TextRenderingHintClearTypeGridFit);
        art::logo(g, 30, 24, 42);
        text(g,
             type == 0   ? i18n::text(L"GAME PRESETS")
             : type == 1 ? i18n::text(L"CONTROLLER STUDIO")
             : type == 3 ? i18n::text(L"VELOX · NAME")
                         : i18n::text(L"VELOX · CONFIRM"),
             86, 25, 600, 25, 18, C_TEXT, true);
        text(g, type >= 2 ? title : app.label, 86, 51, 600, 23, 13, C_DIM, false, false, type < 2);
        if (type == 3) {
            text(g, message, 32, 95, 656, 85, 14, C_TEXT);
            card(g, 28, 194, 664, 52);
            if (!error.empty())
                text(g, error, 32, 252, 656, 43, 12, C_WARN);
            button(g, i18n::text(L"Cancel"), gp::RectF(385, 308, 140, 38), [this] { close(false); });
            button(g, accept, gp::RectF(537, 308, 155, 38), [this] { acceptInput(); }, true);
            return;
        }
        if (type == 2) {
            card(g, 28, 100, 664, float(bodyHeight + 22));
            text(g, message, 48, 110, 624, float(bodyHeight), 14, C_TEXT);
            button(g, i18n::text(L"Cancel"), gp::RectF(385, float(height() - 62), 140, 38), [this] { close(false); });
            button(g, accept, gp::RectF(537, float(height() - 62), 155, 38), [this] { close(true); }, true);
            return;
        }
        if (type == 0) {
            card(g, 28, 96, 664, 55);
            text(g, i18n::text(L"A setup made for this game"), 46, 103, 400, 22, 14, C_TEXT, true);
            text(g, i18n::text(L"Global settings stay intact. Hardware changes request a restart."), 46, 125, 505, 17, 11, C_DIM);
            button(
                g, preset.enabled ? i18n::text(L"On") : i18n::text(L"Off"), gp::RectF(606, 107, 66, 32),
                [this] { preset.enabled = !preset.enabled; }, preset.enabled);
            std::vector<std::wstring> displays = {i18n::text(L"Global"), L"HD+", L"FHD", L"FHD+", L"QHD+", L"Fold", i18n::text(L"Tablet")};
            choices(g, 167, i18n::text(L"DISPLAY"), displays, preset.display + 1, [this](int i) {
                preset.display = i - 1;
                preset.enabled = true;
            });
            std::vector<std::wstring> rates = {i18n::text(L"Global")};
            for (int r : refreshRates())
                rates.push_back(std::to_wstring(r));
            int rate = 0;
            for (size_t i = 0; i < refreshRates().size(); i++)
                if (preset.rate == refreshRates()[i])
                    rate = int(i) + 1;
            choices(g, 237, i18n::text(L"FRAME RATE (FPS)"), rates, rate, [this](int i) {
                preset.rate = i ? refreshRates()[i - 1] : 0;
                preset.enabled = true;
            });
            std::vector<std::wstring> cpus = {i18n::text(L"Global")};
            int cpu = 0;
            for (size_t i = 0; i < coreChoices().size(); i++) {
                cpus.push_back(coreChoices()[i] ? std::to_wstring(coreChoices()[i]) : i18n::text(L"Auto"));
                if (preset.cores == coreChoices()[i])
                    cpu = int(i) + 1;
            }
            choices(
                g, 307, i18n::text(L"VIRTUAL CPU CORES"), cpus, cpu,
                [this](int i) {
                    preset.cores = i ? coreChoices()[i - 1] : -1;
                    preset.enabled = true;
                },
                [](int i) { return i == 0 || coreAllowed(coreChoices()[i - 1]); });
            std::vector<std::wstring> memory = {i18n::text(L"Global")};
            int ram = 0;
            for (size_t i = 0; i < memoryChoices().size(); i++) {
                memory.push_back(memoryChoices()[i] ? std::to_wstring(memoryChoices()[i]) : i18n::text(L"Auto"));
                if (preset.memory == memoryChoices()[i])
                    ram = int(i) + 1;
            }
            choices(
                g, 377, i18n::text(L"MEMORY (GB) · WINDOWS HEADROOM IS RESERVED"), memory, ram,
                [this](int i) {
                    preset.memory = i ? memoryChoices()[i - 1] : -1;
                    preset.enabled = true;
                },
                [](int i) { return i == 0 || memoryAllowed(memoryChoices()[i - 1]); });
            choices(
                g, 447, i18n::text(L"GRAPHICS"), {i18n::text(L"Global"), i18n::text(L"Auto"), L"Hardware", i18n::text(L"Software")},
                (preset.gpu == GPU_HOST && host().softwareGpu) ? 3 : preset.gpu + 1,
                [this](int i) {
                    preset.gpu = i - 1;
                    preset.enabled = true;
                },
                [](int i) { return i != 2 || !host().softwareGpu; });
            choices(g, 517, i18n::text(L"OPEN THIS GAME"), {i18n::text(L"Keep window state"), L"Windowed", i18n::text(L"Fullscreen")},
                    preset.fullscreen + 1, [this](int i) {
                        preset.fullscreen = i - 1;
                        preset.enabled = true;
                    });
            button(g, i18n::text(L"Use global settings"), gp::RectF(30, 604, 180, 38), [this] { preset = {}; });
            button(g, i18n::text(L"Cancel"), gp::RectF(414, 604, 115, 38), [this] { close(); });
            button(
                g, i18n::text(L"Save preset"), gp::RectF(541, 604, 150, 38),
                [this] {
                    if (!presets::save(app.package, preset)) {
                        error = i18n::text(L"Couldn't save. Check folder access and free space.");
                        return;
                    }
                    if (owner)
                        PostMessageW(owner, CHANGED, 0, 0);
                    close();
                },
                true);
        } else {
            auto snapshot = reader ? reader->state() : controllers::snapshot();
            auto frame = controllers::filtered(snapshot.frame, pad.deadzone, pad.sensitivity, pad.invertY);
            card(g, 28, 95, 664, 126);
            text(g, frame.connected ? i18n::text(L"Controller connected") : i18n::text(L"Ready when you are"), 46, 106, 350, 25, 17, C_TEXT,
                 true);
            text(g, i18n::text(L"USB or Bluetooth · Xbox-compatible and PlayStation layouts"), 46, 133, 610, 21, 12, C_DIM);
            std::wstring device = i18n::text(L"Automatic · first connected controller");
            bool deviceFound = pad.padDevice.empty();
            for (auto &d : snapshot.devices)
                if (d.id == pad.padDevice) {
                    device = d.name;
                    deviceFound = true;
                }
            if (!deviceFound)
                device = i18n::text(L"Saved controller · not connected");
            button(g, device, gp::RectF(46, 172, 628, 34), [this] {
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, 1, i18n::text(L"Automatic · first connected"));
                auto devices = reader ? reader->state().devices : controllers::snapshot().devices;
                for (size_t i = 0; i < devices.size(); i++)
                    AppendMenuW(menu, MF_STRING, UINT_PTR(2 + i), devices[i].name.c_str());
                POINT pos{};
                GetCursorPos(&pos);
                int id = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pos.x, pos.y, 0, hwnd, nullptr);
                DestroyMenu(menu);
                if (id == 1)
                    pad.padDevice.clear();
                else if (id >= 2 && size_t(id - 2) < devices.size())
                    pad.padDevice = devices[id - 2].id;
            });
            choices(g, 235, i18n::text(L"INPUT MODE · ONLY SENT TO THE FOCUSED PHONE"),
                    {i18n::text(L"Off"), i18n::text(L"Native Android gamepad"), i18n::text(L"Touch mapping")}, pad.padMode,
                    [this](int i) { pad.padMode = i; });
            choices(g, 305, i18n::text(L"BUTTON LAYOUT"), {L"Automatic", L"Xbox-style", L"PlayStation", L"Generic"}, pad.padLayout,
                    [this](int i) { pad.padLayout = i; });
            int dz = std::abs(pad.deadzone - .1f) < .01f    ? 0
                     : std::abs(pad.deadzone - .18f) < .01f ? 1
                     : std::abs(pad.deadzone - .24f) < .01f ? 2
                                                            : 3;
            choices(g, 375, i18n::text(L"STICK DEAD ZONE"), {L"10%", L"18%", L"24%", L"32%"}, dz, [this](int i) {
                const float values[] = {.1f, .18f, .24f, .32f};
                pad.deadzone = values[i];
            });
            int sensitivity = std::clamp(int(std::lround(pad.sensitivity * 2)) - 1, 0, 3);
            choices(g, 445, i18n::text(L"STICK SENSITIVITY"), {L"50%", L"100%", L"150%", L"200%"}, sensitivity,
                    [this](int i) { pad.sensitivity = (i + 1) * .5f; });
            text(g, i18n::text(L"LEFT STICK"), 48, 519, 135, 18, 10, C_DIM, true);
            text(g, i18n::text(L"RIGHT STICK"), 231, 519, 140, 18, 10, C_DIM, true);
            for (int i = 0; i < 2; i++) {
                float x = 115.f + i * 185;
                gp::Pen edge{gp::Color(0xff565579), 2};
                gp::SolidBrush dot{gp::Color(frame.connected ? C_ACCENT : C_FAINT)};
                g.DrawEllipse(&edge, x - 22, 539.f, 44.f, 44.f);
                g.FillEllipse(&dot, x + (i ? frame.rx : frame.lx) * 16 - 5, 561 + (i ? frame.ry : frame.ly) * 16 - 5, 10.f, 10.f);
            }
            std::wstring pressed;
            for (int i = 0; i < controllers::Count; i++)
                if (frame.buttons & (1u << i)) {
                    if (!pressed.empty())
                        pressed += L" · ";
                    pressed += controllers::buttonName(i);
                    if (pressed.size() > 30)
                        break;
                }
            text(g, pressed.empty() ? i18n::text(L"No buttons pressed") : pressed, 402, 526, 250, 22, 12, C_DIM);
            button(
                g, pad.invertY ? i18n::text(L"Camera Y: inverted") : i18n::text(L"Camera Y: normal"), gp::RectF(402, 556, 270, 30),
                [this] { pad.invertY = !pad.invertY; }, pad.invertY);
            button(g, i18n::text(L"Cancel"), gp::RectF(414, 600, 115, 36), [this] { close(); });
            button(
                g, i18n::text(L"Save controller"), gp::RectF(541, 600, 150, 36),
                [this] {
                    if (!playtools::controller(pad)) {
                        error = i18n::text(L"The active game changed. Reopen its Controller Studio.");
                        return;
                    }
                    if (owner)
                        PostMessageW(owner, CHANGED, 0, 0);
                    close();
                },
                true);
        }
        if (!error.empty())
            text(g, error, 32, float(height() - 57), 370, 48, 11, C_BAD);
    }
};

LRESULT CALLBACK inputProcedure(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
    auto *c = reinterpret_cast<Window *>(data);
    if (m == WM_KEYDOWN) {
        if (w == VK_ESCAPE) {
            c->close(false);
            return 0;
        }
        if (w == VK_RETURN) {
            c->acceptInput();
            return 0;
        }
        if (w == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessageW(h, EM_SETSEL, 0, -1);
            return 0;
        }
    }
    if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE))
        return 0;
    return DefSubclassProc(h, m, w, l);
}
LRESULT CALLBACK procedure(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto *c = reinterpret_cast<Window *>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        c = static_cast<Window *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        c->hwnd = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(c));
    }
    if (!c)
        return DefWindowProcW(h, m, w, l);
    auto hit = [&](int x, int y) {
        for (size_t i = 0; i < c->hits.size(); i++)
            if (c->hits[i].enabled && c->hits[i].rect.Contains(x / c->scale, y / c->scale))
                return int(i);
        return -1;
    };
    switch (m) {
    case WM_CREATE: {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(h, 20, &dark, sizeof(dark));
        COLORREF color = RGB(19, 21, 28);
        DwmSetWindowAttribute(h, 35, &color, sizeof(color));
        if (c->owner)
            EnableWindow(c->owner, FALSE);

        if (c->type == 3) {
            c->editBrush = CreateSolidBrush(RGB(26, 29, 38));
            c->editFont = CreateFontW(-int(16 * c->scale), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                      CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            c->edit = CreateWindowExW(0, L"EDIT", c->initial.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                      int(44 * c->scale), int(209 * c->scale), int(628 * c->scale), int(26 * c->scale), h, nullptr,
                                      GetModuleHandleW(nullptr), nullptr);
            SetWindowTheme(c->edit, L"", L"");
            SendMessageW(c->edit, WM_SETFONT, reinterpret_cast<WPARAM>(c->editFont), TRUE);
            SendMessageW(c->edit, EM_LIMITTEXT, 40, 0);
            SendMessageW(c->edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
            SetWindowSubclass(c->edit, inputProcedure, 1, reinterpret_cast<DWORD_PTR>(c));
        }
        if (c->type == 1) {
            c->reader = std::make_unique<controllers::Reader>(h, false);
            SetTimer(h, 1, 50, nullptr);
        }
        return 0;
    }
    case WM_DPICHANGED:
        c->layout(reinterpret_cast<RECT *>(l));
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_EXITSIZEMOVE:
        c->layout();
        return 0;
    case WM_SETTINGCHANGE:
        if (w == SPI_SETWORKAREA)
            c->layout();
        break;
    case WM_TIMER:
        if (c->reader)
            c->reader->read(c->pad.padDevice, c->pad.padLayout);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(h, &ps);
        RECT r{};
        GetClientRect(h, &r);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, r.right, r.bottom);
        auto old = SelectObject(mem, bitmap);
        {
            gp::Graphics g(mem);
            g.ScaleTransform(c->scale, c->scale);
            c->draw(g);
        }
        BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bitmap);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CTLCOLOREDIT:
        SetTextColor(reinterpret_cast<HDC>(w), RGB(242, 243, 247));
        SetBkColor(reinterpret_cast<HDC>(w), RGB(26, 29, 38));
        return reinterpret_cast<LRESULT>(c->editBrush);
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        int hover = hit(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (hover != c->hover) {
            c->hover = hover;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
        c->pressed = hit(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        SetCapture(h);
        return 0;
    case WM_LBUTTONUP: {
        ReleaseCapture();
        int id = hit(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (id >= 0 && id == c->pressed) {
            auto action = c->hits[id].action;
            c->pressed = -1;
            action();
            if (IsWindow(h))
                InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) {
            c->close(false);
            return 0;
        }
        break;
    case WM_CLOSE:
        c->close(false);
        return 0;
    case WM_DESTROY:
        if (c->owner)
            EnableWindow(c->owner, TRUE);
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        if (!c->creating)
            delete c;
        return DefWindowProcW(h, m, w, l);
    }
    return DefWindowProcW(h, m, w, l);
}
void open(Window *c) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW cls{};
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpfnWndProc = procedure;
        cls.lpszClassName = L"VeloxStudio";
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hIcon = LoadIconW(cls.hInstance, MAKEINTRESOURCEW(1));
        registered = RegisterClassW(&cls) != 0;
    }
    c->measureConfirmation();
    UINT dpi = c->owner ? GetDpiForWindow(c->owner) : GetDpiForSystem();
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromWindow(c->owner, MONITOR_DEFAULTTONEAREST), &monitor);
    auto work = monitor.rcWork;
    RECT chrome{0, 0, 0, 0};
    AdjustWindowRectExForDpi(&chrome, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME, dpi);
    int pad = MulDiv(24, dpi, 96);
    c->scale = scaling::fit(work.right - work.left - pad - (chrome.right - chrome.left),
                            work.bottom - work.top - pad - (chrome.bottom - chrome.top), 720, c->height(),
                            int(std::lround(scaling::ownerScale(c->owner, dpi, work.bottom - work.top - pad) * 100))) /
               100.f;
    RECT r{0, 0, int(720 * c->scale), int(c->height() * c->scale)};
    AdjustWindowRectExForDpi(&r, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME, dpi);
    RECT parent{};
    GetWindowRect(c->owner, &parent);
    int width = r.right - r.left, height = r.bottom - r.top;
    int x = std::clamp<int>(parent.left + (parent.right - parent.left - width) / 2, work.left,
                            std::max<LONG>(work.left, work.right - width)),
        y = std::clamp<int>(parent.top + (parent.bottom - parent.top - height) / 2, work.top,
                            std::max<LONG>(work.top, work.bottom - height));
    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME, L"VeloxStudio",
                             c->type == 0   ? i18n::text(L"Velox · Game Preset")
                             : c->type == 1 ? i18n::text(L"Velox · Controller Studio")
                                            : i18n::text(L"Velox · Review"),
                             WS_CAPTION | WS_SYSMENU, x, y, width, height, c->owner, nullptr, GetModuleHandleW(nullptr), c);
    c->creating = false;
    if (h) {
        ShowWindow(h, SW_SHOW);
        if (c->edit) {
            SetFocus(c->edit);
            SendMessageW(c->edit, EM_SETSEL, 0, -1);
        }
    } else
        delete c;
}
} // namespace
void game(HWND owner, const experience::AppInfo &app, const Settings &global) {
    auto *c = new Window;
    c->owner = owner;
    c->app = app;
    c->global = global;
    c->preset = presets::load(app.package);
    open(c);
}
void controller(HWND owner, const experience::AppInfo &app) {
    auto *c = new Window;
    c->owner = owner;
    c->type = 1;
    c->app = app;
    c->pad = playtools::profile();
    if (c->pad.package != app.package) {
        delete c;
        return;
    }
    open(c);
}
void confirm(HWND owner, const std::wstring &title, const std::wstring &message, const std::wstring &accept,
             std::function<void(bool)> callback) {
    auto *c = new Window;
    c->owner = owner;
    c->type = 2;
    c->title = title;
    c->message = message;
    c->accept = accept;
    c->callback = std::move(callback);
    open(c);
}

void input(HWND owner, const std::wstring &title, const std::wstring &message, const std::wstring &initial, const std::wstring &accept,
           std::function<void(const std::wstring &)> callback) {
    auto *c = new Window;
    c->owner = owner;
    c->type = 3;
    c->title = title;
    c->message = message;
    c->initial = initial.substr(0, 40);
    c->accept = accept;
    c->inputCallback = std::move(callback);
    open(c);
}
void preview(gp::Graphics &g, int type, const experience::AppInfo &app, const Settings &global) {
    Window c;
    c.type = type;
    c.app = app;
    c.global = global;
    c.preset = presets::load(app.package);
    c.pad = playtools::profile();
    c.draw(g);
}
} // namespace phone::studio
