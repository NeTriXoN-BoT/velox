// Velox: a launcher for an Android emulator set up for gaming and app testing.
// Native Win32 + GDI+. The window is drawn by hand; every frame rebuilds a list of clickable
// areas, so the pages below read top to bottom like the screen looks.

#include "common.hpp" // windows.h first, with the right settings

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <atomic>
#include <cmath>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <map>

#include "art.hpp"
#include "overlay.hpp"
#include "phone.hpp"
#include "setup.hpp"
#include "sysmon.hpp"
#include "update.hpp"
#include "experience.hpp"
#include "playtools.hpp"
#include "controllers.hpp"
#include "presets.hpp"
#include "vault.hpp"
#include "studio.hpp"
#include "instances.hpp"

using namespace phone;

namespace {

constexpr UINT WM_APP_REFRESH = WM_APP + 1;
constexpr UINT_PTR TIMER_WATCH = 1, TIMER_CURSOR = 2, TIMER_ANIM = 3;
constexpr int ID_NUMBER = 100, ID_SEARCH = 101;
constexpr float WIN_W = 900, WIN_H = 640, SIDE_W = 232;

enum Page { HOME, LIBRARY, INSTANCES, CONTROLS, DISPLAY, PERFORMANCE, LOOK, PHONE, RECOVERY, SYSTEM, UPDATES, PAGE_COUNT };
const wchar_t *PAGE_NAMES[] = {i18n::text(L"Overview"), i18n::text(L"Library"),     i18n::text(L"Instances"), i18n::text(L"Controls"),
                               i18n::text(L"Display"),  i18n::text(L"Performance"), i18n::text(L"Look"),      i18n::text(L"Phone"),
                               i18n::text(L"Recovery"), i18n::text(L"System"),      i18n::text(L"Updates")};
const wchar_t *PAGE_ICONS[] = {L"\xE80F", L"\xE8F1", L"\xE8A5", L"\xE765", L"\xE7F4", L"\xEC4A",
                               L"\xE790", L"\xE8EA", L"\xE8F1", L"\xE9D9", L"\xE895"};

struct Hit {
    gp::RectF r;
    int id;
    bool enabled;
    std::function<void()> act;
};

struct App {
    HWND hwnd = nullptr, numberEdit = nullptr, searchEdit = nullptr;
    float s = 1.f;
    Settings set;
    bool firstRun = false;
    Page page = LIBRARY;
    int libraryFilter = 0, libraryPage = 0;
    std::wstring librarySearch, pendingGame;
    bool pendingEditor = false;
    std::optional<Settings> pendingSettings;
    std::wstring activeGame;
    bool vaultAllow = false;
    bool closeAfterVault = false;
    bool exitAfterStop = false;
    std::vector<instances::Info> phones;
    int instancePage = 0;
    std::vector<vault::Entry> backups;
    int backupPage = 0;
    std::wstring selectedBackup;
    ULONGLONG pendingSince = 0;
    std::map<std::wstring, std::unique_ptr<gp::Bitmap>> appIcons;
    std::vector<std::pair<gp::RectF, experience::AppInfo>> libraryCards;
    std::mutex lock; // message, preview, fps
    std::wstring message;
    bool messageError = false;
    ULONGLONG messageTime = 0;
    std::vector<Hit> hits;
    int nextId = 0, hover = -1, pressed = -1;
    std::unique_ptr<gp::Font> fTitle, fH2, fText, fBold, fSmall, fSmallBold, fTiny, fBig, fHuge, fIcon, fIconBig;
    HFONT editFont = nullptr;
    HBRUSH editBrush = nullptr;
    std::unique_ptr<gp::Bitmap> background, preview;
    // Wallpaper gallery: the built-in designs, then pictures from the wallpaper folders.
    struct Wall {
        int builtin = -1;
        std::wstring file, name;
        std::unique_ptr<gp::Bitmap> source, thumb; // small copy of the picture, and with branding
    };
    std::vector<Wall> walls;
    std::atomic<unsigned> wallGen{0};
    std::deque<int> fps;
    std::atomic<bool> busy{false}; // a tool is running
    // updates
    std::atomic<int> updateState{0}; // 0 idle, 1 checking, 2 up to date, 3 available, 4 downloading, 5 ready, 6 failed
    std::wstring updateVersion, updateNotes, updateError;
    float updateProgress = 0;
    // setup
    bool licenseAccepted = false;
    int wallPage = 0;
    // The changelog, structured: versions newest first, each with New / Improved / Fixed lines.
    struct ChangeVersion {
        std::wstring version, date;
        std::vector<std::pair<int, std::wstring>> items; // kind: 0 new, 1 improved, 2 fixed
    };
    std::vector<ChangeVersion> changes;
};
App g;

float dp(float v) {
    return v * g.s;
}
void refresh() {
    if (g.hwnd)
        PostMessageW(g.hwnd, WM_APP_REFRESH, 0, 0);
}

void say(const std::wstring &m, bool error = false) {
    {
        std::lock_guard<std::mutex> l(g.lock);
        g.message.assign(m, 0, std::min<size_t>(m.size(), 2048));
        if (m.size() > 2048)
            g.message += L"…";
        g.messageError = error;
        g.messageTime = GetTickCount64();
    }
    refresh();
}
Report reporter() {
    return [](const std::wstring &m, bool e) { say(m, e); };
}

bool running() {
    return phone::state == State::Running;
}
bool idle() {
    return phone::state == State::Stopped || phone::state == State::Running;
}
// Only when we know what the running phone was started with; otherwise there is nothing to compare.
Settings sessionSettings() {
    return g.activeGame.empty() ? g.set : presets::apply(g.set, g.activeGame);
}
bool needsRestart() {
    return running() &&
           (phone::navigationNeedsRestart() || (phone::runningKnown() && signature(sessionSettings()) != signature(phone::running)));
}

void save() {
    saveSettings(g.set);
    experience::hostPreferences(g.set);
}

LRESULT CALLBACK fieldProc(HWND h, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(h, fieldProc, 1);
        return DefSubclassProc(h, message, wp, lp);
    }
    if (message == WM_CHAR && wp == 1) {
        SendMessageW(h, EM_SETSEL, 0, -1);
        return 0;
    }
    if (message == WM_CHAR && (wp == VK_ESCAPE || wp == VK_RETURN)) {
        if (wp == VK_ESCAPE && GetDlgCtrlID(h) == ID_SEARCH)
            SetWindowTextW(h, L"");
        else
            SetFocus(GetParent(h));
        return 0;
    }
    if (message == WM_ERASEBKGND)
        return 1;
    if (message == WM_PAINT || message == WM_PRINTCLIENT || message == WM_PRINT) {
        PAINTSTRUCT paint{};
        HDC destination = message == WM_PAINT ? BeginPaint(h, &paint) : reinterpret_cast<HDC>(wp);
        RECT rect{};
        GetClientRect(h, &rect);
        // Use the same buffered bitmap path as the surrounding Velox cards.
        // Native editing, selection and caret remain with EDIT; only its paint
        // output and cue share our palette and avoid direct themed overdraw.
        HDC buffer = CreateCompatibleDC(destination);
        HBITMAP bitmap = CreateCompatibleBitmap(destination, std::max<LONG>(1, rect.right), std::max<LONG>(1, rect.bottom));
        HGDIOBJ previous = SelectObject(buffer, bitmap);
        FillRect(buffer, &rect, g.editBrush);
        DefSubclassProc(h, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(buffer), PRF_CLIENT | PRF_ERASEBKGND);
        if (!GetWindowTextLengthW(h)) {
            FillRect(buffer, &rect, g.editBrush);
            auto font = SelectObject(buffer, g.editFont);
            SetTextColor(buffer, RGB(0x9A, 0xA0, 0xAE));
            SetBkMode(buffer, TRANSPARENT);
            auto cue =
                i18n::tr(GetDlgCtrlID(h) == ID_SEARCH ? L"Search games and apps" : L"Emulator default · e.g. +4915112345678 for Germany");
            DrawTextW(buffer, cue.c_str(), -1, &rect, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(buffer, font);
        }
        BitBlt(destination, 0, 0, rect.right, rect.bottom, buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, previous);
        DeleteObject(bitmap);
        DeleteDC(buffer);
        if (message == WM_PAINT)
            EndPaint(h, &paint);
        return 0;
    }
    return DefSubclassProc(h, message, wp, lp);
}

void placeField(HWND field, int x, int y, int width, int height) {
    RECT now{};
    GetWindowRect(field, &now);
    MapWindowPoints(nullptr, g.hwnd, reinterpret_cast<POINT *>(&now), 2);
    if (now.left != x || now.top != y || now.right - now.left != width || now.bottom - now.top != height)
        SetWindowPos(field, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}
void changeLanguage(int language) {
    if (g.set.language == language)
        return;
    g.set.language = language;
    i18n::language(language);
    save();
    InvalidateRect(g.numberEdit, nullptr, TRUE);
    InvalidateRect(g.searchEdit, nullptr, TRUE);
    refresh();
}
void languagePicker(gp::Graphics &gr);

// ------------------------------------------------------------------------------------------
// Drawing helpers

gp::SolidBrush &solid(gp::ARGB c) {
    static gp::SolidBrush b(gp::Color(0));
    b.SetColor(gp::Color(c));
    return b;
}

void text(gp::Graphics &gr, const std::wstring &s, const gp::RectF &r, const gp::Font &f, gp::ARGB c,
          gp::StringAlignment align = gp::StringAlignmentNear, gp::StringAlignment valign = gp::StringAlignmentCenter, bool wrap = false,
          bool raw = false) {
    auto display = raw ? s : i18n::tr(s);
    gp::StringFormat fmt;
    fmt.SetAlignment(align);
    fmt.SetLineAlignment(valign);
    if (!wrap)
        fmt.SetFormatFlags(gp::StringFormatFlagsNoWrap);
    fmt.SetTrimming(gp::StringTrimmingEllipsisCharacter);
    gr.DrawString(display.c_str(), -1, &f, r, &fmt, &solid(c));
}

void roundFill(gp::Graphics &gr, const gp::RectF &r, float rad, gp::ARGB c) {
    art::fillRound(gr, r, rad, solid(c));
}

void gradientFill(gp::Graphics &gr, const gp::RectF &r, float rad) {
    gp::LinearGradientBrush lg(gp::RectF(r.X - 1, r.Y, r.Width + 2, r.Height), gp::Color(C_ACCENT), gp::Color(C_ACCENT2),
                               gp::LinearGradientModeHorizontal);
    art::fillRound(gr, r, rad, lg);
}

int addHit(const gp::RectF &r, bool enabled, std::function<void()> act) {
    int id = g.nextId++;
    g.hits.push_back({r, id, enabled && (!vault::progress.running || g.vaultAllow), std::move(act)});
    return id;
}

enum class Btn { Primary, Secondary, Ghost };

void button(gp::Graphics &gr, const gp::RectF &r, const std::wstring &label, Btn style, bool enabled, std::function<void()> act,
            const wchar_t *icon = nullptr) {
    int id = addHit(r, enabled, std::move(act));
    bool hov = g.hover == id && enabled, down = g.pressed == id && enabled;
    float rad = std::min(dp(10), r.Height / 2);
    if (style == Btn::Primary && enabled) {
        gradientFill(gr, r, rad);
        if (down)
            roundFill(gr, r, rad, 0x33000000);
        else if (hov)
            roundFill(gr, r, rad, 0x22FFFFFF);
    } else if (style != Btn::Ghost || hov) {
        roundFill(gr, r, rad, !enabled ? 0xFF1C1F27 : down ? 0xFF3A4050 : hov ? C_CHIP_HOVER : C_CHIP);
    }
    gp::ARGB fg = !enabled ? C_FAINT : style == Btn::Ghost ? (hov ? C_TEXT : C_DIM) : C_TEXT;
    const gp::Font &baseFont = style == Btn::Primary ? *g.fBold : *g.fText;
    auto localized = i18n::tr(label);
    std::unique_ptr<gp::Font> adjusted;
    if (i18n::language() == 1) {
        gp::RectF measured;
        gr.MeasureString(localized.c_str(), -1, &baseFont, gp::PointF(), &measured);
        float available = r.Width - dp(icon ? 32 : 12);
        if (measured.Width > available) {
            gp::FontFamily family;
            baseFont.GetFamily(&family);
            adjusted = std::make_unique<gp::Font>(&family, baseFont.GetSize() * std::max(0.78f, available / measured.Width),
                                                  baseFont.GetStyle(), gp::UnitPixel);
        }
    }
    const gp::Font &f = adjusted ? *adjusted : baseFont;
    if (icon) {
        gp::RectF m;
        auto caption = i18n::tr(label);
        gr.MeasureString(caption.c_str(), -1, &f, gp::PointF(0, 0), &m);
        float total = dp(16) + dp(8) + m.Width, x = r.X + (r.Width - total) / 2;
        text(gr, icon, gp::RectF(x, r.Y, dp(16), r.Height), *g.fIcon, fg, gp::StringAlignmentCenter);
        text(gr, label, gp::RectF(x + dp(24), r.Y, r.Width, r.Height), f, fg);
    } else {
        text(gr, label, r, f, fg, gp::StringAlignmentCenter);
    }
}

// A row of options; `labels` and `enabled` are per option.
void segments(gp::Graphics &gr, const gp::RectF &r, const std::vector<std::wstring> &labels, int selected,
              const std::function<bool(int)> &enabled, const std::function<void(int)> &pick,
              const std::vector<std::wstring> *sub = nullptr) {
    float gap = dp(6), w = (r.Width - gap * (labels.size() - 1)) / labels.size();
    for (size_t i = 0; i < labels.size(); i++) {
        gp::RectF c(r.X + i * (w + gap), r.Y, w, r.Height);
        bool en = enabled(static_cast<int>(i)), sel = static_cast<int>(i) == selected;
        int id = addHit(c, en, [pick, i] { pick(static_cast<int>(i)); });
        if (sel && en)
            gradientFill(gr, c, dp(10));
        else
            roundFill(gr, c, dp(10), !en ? 0xFF181A21 : g.hover == id ? C_CHIP_HOVER : C_CHIP);
        gp::ARGB fg = !en ? C_FAINT : sel ? C_TEXT : C_DIM;
        if (sub) {
            text(gr, labels[i], gp::RectF(c.X, c.Y + c.Height / 2 - dp(17), c.Width, dp(18)), *g.fBold,
                 en ? (sel ? C_TEXT : 0xFFD5D8E0) : C_FAINT, gp::StringAlignmentCenter);
            text(gr, (*sub)[i], gp::RectF(c.X, c.Y + c.Height / 2 + dp(1), c.Width, dp(16)), *g.fSmall, sel && en ? 0xFFE6E2FF : C_FAINT,
                 gp::StringAlignmentCenter);
        } else {
            text(gr, labels[i], c, sel ? *g.fBold : *g.fText, fg, gp::StringAlignmentCenter);
        }
    }
}

void languagePicker(gp::Graphics &gr) {
    float x = dp(WIN_W - 32 - 216);
    text(gr, i18n::text(L"Language / Sprache"), gp::RectF(x, dp(8), dp(216), dp(17)), *g.fTiny, C_DIM, gp::StringAlignmentCenter);
    segments(
        gr, gp::RectF(x, dp(29), dp(216), dp(31)), {i18n::text(L"English"), i18n::text(L"Deutsch")}, g.set.language,
        [](int) { return true; }, [](int n) { changeLanguage(n); });
}

void toggle(gp::Graphics &gr, const gp::RectF &r, const std::wstring &title, const std::wstring &sub, bool on, bool enabled,
            std::function<void()> act, const wchar_t *icon = nullptr) {
    int id = addHit(r, enabled, std::move(act));
    roundFill(gr, r, dp(12), g.hover == id && enabled ? C_CHIP_HOVER : C_CARD);
    float tx = r.X + dp(16);
    if (icon) {
        text(gr, icon, gp::RectF(r.X + dp(14), r.Y, dp(22), r.Height), *g.fIcon, enabled ? C_DIM : C_FAINT, gp::StringAlignmentCenter);
        tx = r.X + dp(48);
    }
    text(gr, title, gp::RectF(tx, r.Y + r.Height / 2 - dp(19), r.GetRight() - tx - dp(76), dp(20)), *g.fBold, enabled ? C_TEXT : C_FAINT);
    text(gr, sub, gp::RectF(tx, r.Y + r.Height / 2 + dp(1), r.GetRight() - tx - dp(76), dp(18)), *g.fSmall, C_DIM);
    gp::RectF sw(r.GetRight() - dp(16) - dp(42), r.Y + (r.Height - dp(24)) / 2, dp(42), dp(24));
    if (on && enabled)
        gradientFill(gr, sw, sw.Height / 2);
    else
        roundFill(gr, sw, sw.Height / 2, 0xFF353A47);
    float k = dp(18), kx = on ? sw.GetRight() - dp(3) - k : sw.X + dp(3);
    gr.FillEllipse(&solid(enabled ? C_TEXT : C_FAINT), kx, sw.Y + dp(3), k, k);
}

void label(gp::Graphics &gr, const std::wstring &s, float x, float y, float w) {
    text(gr, s, gp::RectF(x, y, w, dp(18)), *g.fSmallBold, C_FAINT);
}

void card(gp::Graphics &gr, const gp::RectF &r) {
    roundFill(gr, r, dp(14), C_CARD);
}

// ------------------------------------------------------------------------------------------
// Actions

void restartToApply();

void startPhone() {
    if (!installed()) {
        auto message = i18n::text(L"Open this phone's controls to finish Android setup or select its SDK.");
        instances::failure(message);
        say(message, true);
        return;
    }
    if (!idle() || running() || g.busy || vault::progress.running)
        return;
    if (vault::blocked()) {
        g.page = RECOVERY;
        say(i18n::text(L"Complete or undo the interrupted recovery before starting the phone."), true);
        instances::failure(i18n::text(L"Complete or undo the interrupted recovery before starting this phone."));
        return;
    }
    if (!host().hypervisor) {
        say(i18n::text(L"Windows Hypervisor Platform is off. Turn it on in the Phone page, then restart the PC."), true);
        instances::failure(i18n::text(L"Turn on Windows Hypervisor Platform from the Phone page, then restart Windows."));
        return;
    }
    Settings s = g.pendingSettings.value_or(sessionSettings());
    if (s.root && !rootReady()) {
        // Don't get stuck: start normally and say why.
        s.root = false;
        say(i18n::text(L"The root image wasn't found, so the phone starts without root. Set up root again on the Phone page."), true);
    }
    g.pendingSettings.reset();
    if (s.bootScreen) {
        std::wstring sub =
            std::wstring(displays()[s.display].name) + L"  ·  " + std::to_wstring(s.rate) + L" HZ" + (s.root ? L"  ·  ROOT" : L"");
        for (auto &c : sub)
            c = static_cast<wchar_t>(towupper(c));
        int style = s.bootStyle < 5 ? s.bootStyle : static_cast<int>(GetTickCount64() / 1000 % art::bootStyleNames().size());
        overlay::showBootScreen(sub, s.frame, style);
    }
    phone::state = State::Starting;
    std::thread([s] {
        phone::start(s, reporter());
        PostMessageW(g.hwnd, WM_APP_REFRESH, 3, 0); // apply toggles changed while it was booting
    }).detach();
    refresh();
}

void stopPhone(bool thenStart) {
    if (!running())
        return;
    if (!thenStart) {
        g.pendingSettings.reset();
        g.pendingGame.clear();
        g.activeGame.clear();
        presets::remember(L"");
    }
    phone::state = State::Stopping;
    std::thread([thenStart] {
        phone::stop(reporter());
        refresh();
        if (thenStart && phone::state == State::Stopped)
            PostMessageW(g.hwnd, WM_APP_REFRESH, 1, 0);
    }).detach();
    refresh();
}

void restartToApply() {
    std::wstring error;
    if (!instances::budget(sessionSettings(), error))
        return say(error, true);
    stopPhone(true);
}

void tool(std::function<void()> fn) {
    if (g.busy.exchange(true)) {
        say(i18n::text(L"One moment, the last action is still running."));
        return;
    }
    std::thread([fn] {
        fn();
        g.busy = false;
        refresh();
    }).detach();
}

void launchGame(const std::wstring &component, bool editControls = false) {
    if (!idle() || g.busy || vault::progress.running || !experience::validComponent(component))
        return;
    auto package = component.substr(0, component.find('/'));
    auto wanted = presets::apply(g.set, package);
    if (running() && presets::restartNeeded(wanted, phone::running)) {
        std::wstring error;
        if (!instances::budget(wanted, error))
            return say(error, true);
        studio::confirm(g.hwnd, i18n::text(L"Apply this game's preset?"),
                        presets::summary(g.set, package) +
                            i18n::text(L". Restarting closes the apps currently on the phone. Your global settings stay intact."),
                        i18n::text(L"Restart and play"), [component, package, wanted, editControls](bool accepted) {
                            if (!accepted)
                                return;
                            std::wstring error;
                            if (!instances::budget(wanted, error)) {
                                say(error, true);
                                return;
                            }
                            g.pendingGame = component;
                            g.pendingEditor = editControls;
                            g.pendingSince = GetTickCount64();
                            g.pendingSettings = wanted;
                            g.activeGame = package;
                            stopPhone(true);
                        });
        return;
    }
    g.activeGame = package;
    if (!running()) {
        g.pendingGame = component;
        g.pendingEditor = editControls;
        g.pendingSince = GetTickCount64();
        g.pendingSettings = wanted;
        startPhone();
        return;
    }
    tool([component, package, editControls] {
        bool opened = experience::launch(component, reporter());
        if (opened) {
            presets::remember(package);
            PostMessageW(g.hwnd, playtools::ACTION_MESSAGE, playtools::FocusPhone, 0);
        }
        if (opened && editControls) {
            for (int i = 0; i < 40; i++) {
                Sleep(250);
                playtools::pollPhone();
                if (playtools::profile().package == package) {
                    PostMessageW(g.hwnd, playtools::ACTION_MESSAGE, playtools::Editor, 0);
                    break;
                }
            }
        }
        experience::requestLibrary();
    });
}

void installOrSend(std::vector<std::wstring> files) {
    if (!running())
        return say(i18n::text(L"Start the phone first, then drop the files again."), true);
    tool([files] {
        for (auto &f : files) {
            if (appPackageFile(f))
                installApk(f, reporter());
            else
                sendFile(f, reporter());
        }
        experience::requestLibrary();
    });
}

std::vector<std::wstring> pickFiles(bool apkOnly) {
    std::vector<wchar_t> buf(32768, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter = apkOnly ? L"Android apps and bundles (*.apk;*.apks;*.xapk;*.apkm)\0*.apk;*.apks;*.xapk;*.apkm\0All files\0*.*\0"
                              : L"All files\0*.*\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_ALLOWMULTISELECT;
    std::vector<std::wstring> out;
    if (!GetOpenFileNameW(&ofn))
        return out;
    std::wstring first = buf.data();
    const wchar_t *p = buf.data() + first.size() + 1;
    if (!*p)
        return {first};
    for (; *p; p += wcslen(p) + 1)
        out.push_back(first + L"\\" + p);
    return out;
}

void openFolder(const std::wstring &path) {
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Live settings go to the phone in the background. Each request has a number; one that is no
// longer the newest is skipped, and phone::applyLive runs them one at a time, so quick toggles
// always end on the last choice.
std::atomic<unsigned> g_liveGen{0};
void applyLiveAsync() {
    if (!running())
        return;
    Settings s = sessionSettings();
    phone::running.boost = s.boost;
    phone::running.snappy = s.snappy;
    phone::running.showTouches = s.showTouches;
    phone::running.autoRotate = s.autoRotate;
    unsigned my = ++g_liveGen;
    std::thread([s, my] {
        if (my == g_liveGen)
            applyLive(s);
    }).detach();
}

// ---- updates

void checkForUpdates(bool userAsked) {
    if (!instances::id().empty())
        return;
    if (!update::configured()) {
        if (userAsked)
            say(i18n::text(L"Updates aren't set up for this copy yet."), true);
        return;
    }
    if (g.updateState == 1 || g.updateState == 4)
        return;
    g.updateState = 1;
    refresh();
    bool autoInstall = g.set.autoUpdate;
    std::thread([userAsked, autoInstall] {
        update::Info info;
        std::wstring err;
        WritePrivateProfileStringW(L"update", L"lastcheck", std::to_wstring(GetTickCount64() / 1000).c_str(), iniPath().c_str());
        SYSTEMTIME st;
        GetSystemTime(&st);
        FILETIME ft;
        SystemTimeToFileTime(&st, &ft);
        ULARGE_INTEGER now{{ft.dwLowDateTime, ft.dwHighDateTime}};
        WritePrivateProfileStringW(L"update", L"checked", std::to_wstring(now.QuadPart / 10000000ull).c_str(), iniPath().c_str());
        if (!update::check(info, err)) {
            g.updateError = err;
            g.updateState = 6;
            if (userAsked)
                say(i18n::text(L"Couldn't check for updates: ") + err, true);
            refresh();
            return;
        }
        if (!update::isNewer(info.version, GP_VERSION_WSTRING)) {
            g.updateState = 2;
            if (userAsked)
                say(i18n::text(L"Velox is up to date."));
            refresh();
            return;
        }
        g.updateVersion = info.version;
        g.updateNotes = info.notes;
        g.updateState = 3;
        refresh();
        if (!autoInstall && !userAsked)
            return;
        g.updateState = 4;
        g.updateProgress = 0;
        bool ok = update::download(
            info,
            [](float f) {
                g.updateProgress = f;
                refresh();
            },
            err);
        g.updateState = ok ? 5 : 6;
        if (!ok) {
            g.updateError = err;
            say(i18n::text(L"The update couldn't be downloaded: ") + err, true);
        } else {
            say(i18n::text(L"Version ") + info.version +
                i18n::text(L" is ready. It's installed the next time Velox opens, or restart it now."));
        }
        refresh();
    }).detach();
}

void restartForUpdate() {
    if (!instances::id().empty() || instances::otherLaunchers())
        return say(
            i18n::text(L"Close the other phones' controls, then update from the main Velox window. Their Android phones can stay running."),
            true);
    if (phone::state == State::Starting || phone::state == State::Stopping)
        return say(i18n::text(L"Wait until the phone has finished starting or stopping."), true);
    overlay::restoreCursor();
    if (update::applyPending()) {
        DestroyWindow(g.hwnd);
        ExitProcess(0);
    }
    say(i18n::text(L"The update couldn't be installed. It will be tried again next time."), true);
}

// ---- changelog drawing

std::vector<App::ChangeVersion> parseChanges(const std::wstring &textIn, const std::wstring &defaultVersion = L"");

const wchar_t *KIND_NAMES[] = {L"NEW", L"IMPROVED", L"FIXED"};
const gp::ARGB KIND_COLORS[] = {0xFF9F86FF, 0xFF5E9BFF, 0xFF3DD68C};

std::wstring niceDate(const std::wstring &iso) {
    static const wchar_t *months[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    if (iso.size() < 10)
        return iso;
    int m = _wtoi(iso.substr(5, 2).c_str()), d = _wtoi(iso.substr(8, 2).c_str());
    if (m < 1 || m > 12)
        return iso;
    if (i18n::language() == 1) {
        static const wchar_t *germanMonths[] = {L"Jan.", L"Feb.", L"März", L"Apr.", L"Mai", L"Juni",
                                               L"Juli", L"Aug.", L"Sept.", L"Okt.", L"Nov.", L"Dez."};
        return std::to_wstring(d) + L". " + germanMonths[m - 1] + L" " + iso.substr(0, 4);
    }
    return std::wstring(months[m - 1]) + L" " + std::to_wstring(d) + L", " + iso.substr(0, 4);
}

// Draws versions top-down from y and returns where it ended; `limit` > 0 stops after that many
// versions. Text wraps to the width.
float drawChanges(gp::Graphics &gr, const std::vector<App::ChangeVersion> &list, float x, float y, float w, int limit = 0,
                  const wchar_t *heading = nullptr) {
    gp::StringFormat wrap;
    wrap.SetTrimming(gp::StringTrimmingWord);
    int shown = 0;
    for (const auto &v : list) {
        if (limit > 0 && shown++ >= limit)
            break;
        std::wstring head = heading ? std::wstring(heading) + v.version : i18n::text(L"Version ") + v.version;
        text(gr, head, gp::RectF(x, y, w * 0.6f, dp(24)), *g.fBig, C_TEXT);
        if (!v.date.empty())
            text(gr, niceDate(v.date), gp::RectF(x + w * 0.5f, y, w * 0.5f, dp(24)), *g.fSmall, C_FAINT, gp::StringAlignmentFar);
        y += dp(34);
        for (int kind = 0; kind < 3; kind++) {
            bool any = false;
            for (auto &it : v.items)
                any = any || it.first == kind;
            if (!any)
                continue;
            gp::RectF m;
            const wchar_t *label = i18n::text(KIND_NAMES[kind]);
            gr.MeasureString(label, -1, g.fSmallBold.get(), gp::PointF(0, 0), &m);
            gp::RectF pill(x, y, m.Width + dp(16), dp(20));
            gp::Color kc(KIND_COLORS[kind]);
            roundFill(gr, pill, dp(10), gp::Color(46, kc.GetR(), kc.GetG(), kc.GetB()).GetValue());
            text(gr, label, pill, *g.fSmallBold, KIND_COLORS[kind], gp::StringAlignmentCenter);
            y += dp(28);
            for (auto &it : v.items) {
                if (it.first != kind)
                    continue;
                gr.FillEllipse(&solid(KIND_COLORS[kind]), x + dp(5), y + dp(6), dp(6), dp(6));
                gp::RectF box;
                gr.MeasureString(it.second.c_str(), -1, g.fText.get(), gp::RectF(0, 0, w - dp(22), dp(400)), &wrap, &box);
                gr.DrawString(it.second.c_str(), -1, g.fText.get(), gp::RectF(x + dp(20), y - dp(2), w - dp(22), box.Height + dp(4)), &wrap,
                              &solid(0xFFD5D8E0));
                y += box.Height + dp(6);
            }
            y += dp(8);
        }
        y += dp(10);
        gp::Pen rule(gp::Color(C_LINE), 1.f);
        gr.DrawLine(&rule, x, y, x + w, y);
        y += dp(18);
    }
    return y;
}

// The full changelog in a window of its own, scrolled with the wheel or the keys.
struct ChangelogWin {
    HWND hwnd = nullptr;
    float scroll = 0, contentH = 0;
} g_clog;

LRESULT CALLBACK changelogProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        {
            gp::Graphics gr(mem);
            gr.SetSmoothingMode(gp::SmoothingModeAntiAlias);
            gr.SetTextRenderingHint(gp::TextRenderingHintClearTypeGridFit);
            float W = static_cast<float>(rc.right), H = static_cast<float>(rc.bottom), pad = dp(32), headH = dp(96);
            gr.Clear(gp::Color(C_BG));
            gr.SetClip(gp::RectF(0, headH, W, H - headH));
            float endY = drawChanges(gr, g.changes, pad, headH + dp(8) - g_clog.scroll, W - 2 * pad - dp(10));
            g_clog.contentH = endY + g_clog.scroll - headH;
            gr.ResetClip();
            // Header on top, with a soft edge where the list scrolls under it.
            gr.FillRectangle(&solid(C_BG), 0.f, 0.f, W, headH);
            art::logo(gr, pad, dp(26), dp(44));
            text(gr, i18n::text(L"What's new in Velox"), gp::RectF(pad + dp(58), dp(22), W, dp(30)), *g.fTitle, C_TEXT);
            text(gr, i18n::text(L"Every version, newest first."), gp::RectF(pad + dp(60), dp(52), W, dp(20)), *g.fSmall, C_DIM);
            gp::LinearGradientBrush fade(gp::PointF(0, headH - 1), gp::PointF(0, headH + dp(16)), gp::Color(C_BG),
                                         gp::Color(0, 0x0E, 0x10, 0x15));
            gr.FillRectangle(&fade, 0.f, headH, W, dp(16));
            float view = H - headH, maxScroll = std::max(0.f, g_clog.contentH - view);
            if (maxScroll > 0) {
                float th = std::max(dp(30), view * view / g_clog.contentH), ty = headH + (view - th) * (g_clog.scroll / maxScroll);
                roundFill(gr, gp::RectF(W - dp(10), ty, dp(4), th), dp(2), C_CHIP_HOVER);
            }
        }
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEWHEEL:
    case WM_KEYDOWN: {
        RECT rc;
        GetClientRect(h, &rc);
        float view = rc.bottom - dp(96), maxScroll = std::max(0.f, g_clog.contentH - view), step = 0;
        if (m == WM_MOUSEWHEEL)
            step = -GET_WHEEL_DELTA_WPARAM(w) / 120.f * dp(64);
        else if (w == VK_DOWN)
            step = dp(40);
        else if (w == VK_UP)
            step = -dp(40);
        else if (w == VK_NEXT || w == VK_SPACE)
            step = view * 0.9f;
        else if (w == VK_PRIOR)
            step = -view * 0.9f;
        else if (w == VK_HOME)
            step = -1e9f;
        else if (w == VK_END)
            step = 1e9f;
        else if (w == VK_ESCAPE) {
            DestroyWindow(h);
            return 0;
        }
        g_clog.scroll = std::clamp(g_clog.scroll + step, 0.f, maxScroll);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_SIZE:
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        g_clog.hwnd = nullptr;
        EnableWindow(g.hwnd, TRUE);
        SetForegroundWindow(g.hwnd);
        return 0;
    default:
        return DefWindowProcW(h, m, w, l);
    }
}

void showChangelog() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = changelogProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
        wc.lpszClassName = L"VeloxChangelog";
        RegisterClassExW(&wc);
        registered = true;
    }
    if (g_clog.hwnd) {
        SetForegroundWindow(g_clog.hwnd);
        return;
    }
    RECT pr;
    GetWindowRect(g.hwnd, &pr);
    int w = static_cast<int>(dp(640)), h = static_cast<int>(dp(720));
    g_clog.scroll = 0;
    g_clog.hwnd = CreateWindowExW(0, L"VeloxChangelog", i18n::text(L"Velox changelog"),
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, pr.left + (pr.right - pr.left - w) / 2,
                                  pr.top + (pr.bottom - pr.top - h) / 2, w, h, g.hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_clog.hwnd, 20, &dark, sizeof(dark));
    COLORREF cap = RGB(0x0E, 0x10, 0x15);
    DwmSetWindowAttribute(g_clog.hwnd, 35, &cap, sizeof(cap));
    EnableWindow(g.hwnd, FALSE);
    ShowWindow(g_clog.hwnd, SW_SHOW);
}

// ---- text window (license, changelog)

LRESULT CALLBACK textWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(w);
        SetTextColor(dc, RGB(0xD5, 0xD8, 0xE0));
        SetBkColor(dc, RGB(0x1A, 0x1D, 0x26));
        return reinterpret_cast<LRESULT>(g.editBrush);
    }
    case WM_SIZE: {
        HWND edit = GetDlgItem(h, 1);
        RECT rc;
        GetClientRect(h, &rc);
        int pad = static_cast<int>(dp(16));
        MoveWindow(edit, pad, pad, rc.right - 2 * pad, rc.bottom - 2 * pad, TRUE);
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(reinterpret_cast<HDC>(w), &rc, g.editBrush);
        return 1;
    }
    case WM_KEYDOWN:
        if (w == VK_ESCAPE)
            DestroyWindow(h);
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        EnableWindow(g.hwnd, TRUE);
        SetForegroundWindow(g.hwnd);
        return 0;
    default:
        return DefWindowProcW(h, m, w, l);
    }
}

void showText(const std::wstring &title, const std::wstring &body) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = textWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
        wc.lpszClassName = L"VeloxText";
        RegisterClassExW(&wc);
        registered = true;
    }
    RECT pr;
    GetWindowRect(g.hwnd, &pr);
    int w = static_cast<int>(dp(620)), h = static_cast<int>(dp(560));
    HWND win = CreateWindowExW(0, L"VeloxText", title.c_str(), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
                               pr.left + (pr.right - pr.left - w) / 2, pr.top + (pr.bottom - pr.top - h) / 2, w, h, g.hwnd, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(win, 20, &dark, sizeof(dark));
    COLORREF cap = RGB(0x1A, 0x1D, 0x26);
    DwmSetWindowAttribute(win, 35, &cap, sizeof(cap));
    HWND edit = CreateWindowExW(0, L"EDIT", body.c_str(), WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                0, 0, 10, 10, win, reinterpret_cast<HMENU>(1), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(g.editFont), TRUE);
    SetWindowTheme(edit, L"DarkMode_Explorer", nullptr);
    SendMessageW(win, WM_SIZE, 0, 0);
    EnableWindow(g.hwnd, FALSE);
    ShowWindow(win, SW_SHOW);
}

// ------------------------------------------------------------------------------------------
// Pages

float X0() {
    return dp(SIDE_W + 32);
}
float CW() {
    return dp(WIN_W - SIDE_W - 64);
}

void pageHeader(gp::Graphics &gr, const std::wstring &title, const std::wstring &sub) {
    text(gr, title, gp::RectF(X0(), dp(24), CW(), dp(30)), *g.fTitle, C_TEXT);
    text(gr, sub, gp::RectF(X0(), dp(56), CW(), dp(20)), *g.fText, C_DIM);
}

std::wstring settingsLine(const Settings &s) {
    return std::wstring(displays()[s.display].name) + L"  ·  " + std::to_wstring(s.rate) + L" Hz  ·  " + std::to_wstring(coresFor(s)) +
           i18n::text(L" cores  ·  ") + std::to_wstring(memoryFor(s)) + L" GB" + (s.root ? L"  ·  Root" : L"");
}

void drawPreview(gp::Graphics &gr, const gp::RectF &r) {
    card(gr, r);
    gp::RectF ph(r.X + dp(18), r.Y + dp(18), r.Width - dp(36), r.Height - dp(36));
    // A small phone: dark body with the screen inside.
    roundFill(gr, ph, dp(22), 0xFF07080B);
    gp::RectF scr(ph.X + dp(6), ph.Y + dp(6), ph.Width - dp(12), ph.Height - dp(12));
    gp::GraphicsPath clip;
    art::roundRectPath(clip, scr, dp(17));
    gr.SetClip(&clip);
    {
        std::lock_guard<std::mutex> l(g.lock);
        gp::Bitmap *img = g.preview ? g.preview.get() : (g.walls.empty() ? nullptr : g.walls[0].thumb.get());
        if (img) {
            gr.SetInterpolationMode(gp::InterpolationModeHighQualityBicubic);
            float iw = static_cast<float>(img->GetWidth()), ih = static_cast<float>(img->GetHeight());
            float sc = std::max(scr.Width / iw, scr.Height / ih);
            gr.DrawImage(img, gp::RectF(scr.X + (scr.Width - iw * sc) / 2, scr.Y + (scr.Height - ih * sc) / 2, iw * sc, ih * sc));
        }
    }
    if (!running()) {
        roundFill(gr, scr, 0, 0x99000000);
        art::logo(gr, scr.X + scr.Width / 2 - dp(28), scr.Y + scr.Height / 2 - dp(44), dp(56));
        text(gr, phone::state == State::Starting ? i18n::text(L"Starting…") : i18n::text(L"Off"),
             gp::RectF(scr.X, scr.Y + scr.Height / 2 + dp(20), scr.Width, dp(20)), *g.fBold, C_TEXT, gp::StringAlignmentCenter);
    }
    gr.ResetClip();
}

void drawHome(gp::Graphics &gr) {
    const float x = X0(), w = CW();
    State st = phone::state;
    std::wstring head = st == State::Running    ? i18n::text(L"Your phone is running")
                        : st == State::Starting ? i18n::text(L"Starting your phone…")
                        : st == State::Stopping ? i18n::text(L"Closing your phone…")
                                                : i18n::text(L"Ready to play");
    pageHeader(gr, head, installed() ? settingsLine(running() ? phone::running : g.set) : L"");
    drawPreview(gr, gp::RectF(x, dp(96), dp(220), dp(452)));

    float rx = x + dp(240), rw = w - dp(240), y = dp(96);
    card(gr, gp::RectF(rx, y, rw, dp(176)));
    const Display &d = displays()[g.set.display];
    std::pair<std::wstring, std::wstring> rows[] = {
        {i18n::text(L"Display"), std::wstring(d.name) + L"  ·  " + d.detail},
        {i18n::text(L"Refresh rate"), std::to_wstring(g.set.rate) + L" Hz"},
        {i18n::text(L"Performance"), std::to_wstring(coresFor(g.set)) + i18n::text(L" cores  ·  ") + std::to_wstring(memoryFor(g.set)) +
                                         L" GB  ·  " +
                                         (g.set.gpu == GPU_SOFTWARE ? i18n::text(L"Software graphics") : i18n::text(L"GPU graphics"))},
        {i18n::text(L"Mode"), g.set.root ? L"Root (Magisk)" : i18n::text(L"Stock Android 15")},
        {L"Start", g.set.quickBoot ? i18n::text(L"Instant start when possible") : i18n::text(L"Clean full boot")},
    };
    for (int i = 0; i < 5; i++) {
        float ry = y + dp(14) + i * dp(30);
        text(gr, rows[i].first, gp::RectF(rx + dp(18), ry, dp(110), dp(24)), *g.fText, C_DIM);
        text(gr, rows[i].second, gp::RectF(rx + dp(128), ry, rw - dp(146), dp(24)), *g.fBold, C_TEXT);
    }
    y += dp(190);

    // Live frame rate.
    card(gr, gp::RectF(rx, y, rw, dp(96)));
    int last = -1;
    std::vector<int> hist;
    {
        std::lock_guard<std::mutex> l(g.lock);
        hist.assign(g.fps.begin(), g.fps.end());
        if (!hist.empty())
            last = hist.back();
    }
    text(gr, i18n::text(L"SCREEN FPS"), gp::RectF(rx + dp(18), y + dp(12), dp(120), dp(16)), *g.fSmallBold, C_FAINT);
    text(gr, running() && last >= 0 ? std::to_wstring(last) : L"–", gp::RectF(rx + dp(16), y + dp(30), dp(120), dp(52)), *g.fHuge,
         running() ? C_TEXT : C_FAINT);
    text(gr, running() ? i18n::text(L"of ") + std::to_wstring(phone::running.rate) + L" Hz" : i18n::text(L"phone is off"),
         gp::RectF(rx + dp(18), y + dp(72), dp(120), dp(16)), *g.fSmall, C_DIM);
    gp::RectF spark(rx + dp(140), y + dp(16), rw - dp(158), dp(64));
    if (hist.size() > 1) {
        float top = static_cast<float>(std::max(60, phone::running.rate));
        std::vector<gp::PointF> pts;
        for (size_t i = 0; i < hist.size(); i++)
            pts.push_back({spark.X + spark.Width * i / 59.f, spark.GetBottom() - spark.Height * std::min(1.f, hist[i] / top)});
        gp::GraphicsPath area;
        area.AddLines(pts.data(), static_cast<INT>(pts.size()));
        area.AddLine(pts.back(), gp::PointF(pts.back().X, spark.GetBottom()));
        area.AddLine(gp::PointF(pts.back().X, spark.GetBottom()), gp::PointF(pts.front().X, spark.GetBottom()));
        area.CloseFigure();
        gp::LinearGradientBrush fill(spark, gp::Color(90, 0x8B, 0x6C, 0xFF), gp::Color(0, 0x3D, 0x7B, 0xFF),
                                     gp::LinearGradientModeVertical);
        gr.FillPath(&fill, &area);
        gp::Pen line(gp::Color(C_ACCENT), dp(2));
        line.SetLineJoin(gp::LineJoinRound);
        gr.DrawLines(&line, pts.data(), static_cast<INT>(pts.size()));
    } else {
        gp::Pen base(gp::Color(C_LINE), dp(1));
        gr.DrawLine(&base, spark.X, spark.GetBottom(), spark.GetRight(), spark.GetBottom());
    }
    y += dp(110);

    // Quick actions.
    bool on = running() && !g.busy;
    float tw = (rw - dp(16)) / 3, th = dp(64);
    struct Act {
        const wchar_t *icon, *name;
        bool enabled;
        std::function<void()> fn;
    };
    bool rec = phone::recording();
    Act acts[] = {
        {L"\xE710", i18n::text(L"Install app"), on,
         [] {
             auto f = pickFiles(true);
             if (!f.empty())
                 installOrSend(f);
         }},
        {L"\xE724", i18n::text(L"Send files"), on,
         [] {
             auto f = pickFiles(false);
             if (!f.empty())
                 installOrSend(f);
         }},
        {L"\xE722", i18n::text(L"Screenshot"), on, [] { tool([] { screenshot(reporter()); }); }},
        {rec ? L"\xE71A" : L"\xE7C8", rec ? i18n::text(L"Stop recording") : i18n::text(L"Record"), running(),
         [] { tool([] { toggleRecording(reporter()); }); }},
        {L"\xE7AD", i18n::text(L"Rotate"), on, [] { tool([] { phone::rotate(); }); }},
        {L"\xE8B7", i18n::text(L"Captures"), true, [] { openFolder(capturesFolder()); }},
    };
    for (int i = 0; i < 6; i++) {
        gp::RectF t(rx + (i % 3) * (tw + dp(8)), y + (i / 3) * (th + dp(8)), tw, th);
        int id = addHit(t, acts[i].enabled, acts[i].fn);
        bool hov = g.hover == id && acts[i].enabled;
        roundFill(gr, t, dp(12), hov ? C_CHIP_HOVER : C_CARD);
        gp::ARGB fg = acts[i].enabled ? C_TEXT : C_FAINT;
        if (rec && i == 3)
            fg = C_BAD;
        text(gr, acts[i].icon, gp::RectF(t.X, t.Y + dp(10), t.Width, dp(22)), *g.fIconBig, fg, gp::StringAlignmentCenter);
        text(gr, acts[i].name, gp::RectF(t.X, t.Y + dp(36), t.Width, dp(18)), *g.fSmall, acts[i].enabled ? C_DIM : C_FAINT,
             gp::StringAlignmentCenter);
    }
}

std::wstring chooseFolder(const wchar_t *title) {
    BROWSEINFOW info{};
    info.hwndOwner = g.hwnd;
    info.lpszTitle = title;
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    std::wstring result;
    if (auto id = SHBrowseForFolderW(&info)) {
        wchar_t folder[MAX_PATH]{};
        if (SHGetPathFromIDListW(id, folder))
            result = folder;
        CoTaskMemFree(id);
    }
    return result;
}
void refreshVault() {
    g.backups = vault::list();
    if (g.backupPage * 2 >= int(g.backups.size()))
        g.backupPage = std::max(0, int(g.backups.size() + 1) / 2 - 1);
}
void reloadRecovery() {
    loadSettings(g.set, g.firstRun);
    i18n::language(g.set.language);
    findPaths();
    presets::invalidate();
    g.activeGame.clear();
    g.pendingSettings.reset();
    g.pendingGame.clear();
    playtools::release();
    setAutoRotation(g.set.autoRotate);
    setGestureNavigation(g.set.gestureNavigation, g.set.reverseButtons);
    experience::hostPreferences(g.set);
    overlay::setCursorStyle(g.set.cursor);
    refreshVault();
    refresh();
}
void showPreset(const experience::AppInfo &app) {
    studio::game(g.hwnd, app, g.set);
}
void drawRecovery(gp::Graphics &gr) {
    const float x = X0(), w = CW();
    pageHeader(gr, i18n::text(L"Velox Vault"), i18n::text(L"Private recovery points. A way back that keeps your old phone intact."));
    card(gr, gp::RectF(x, dp(96), w, dp(98)));
    text(gr, L"\xE8F1", gp::RectF(x + dp(14), dp(103), dp(42), dp(48)), *g.fIconBig, C_ACCENT, gp::StringAlignmentCenter);
    text(gr,
         vault::progress.running ? vault::progress.text()
         : running()             ? i18n::text(L"Stop the phone to make a consistent backup")
                                 : i18n::text(L"Your phone, ready to come back"),
         gp::RectF(x + dp(70), dp(105), w - dp(90), dp(28)), *g.fBold, C_TEXT);
    text(gr, i18n::text(L"Backups contain apps, accounts and saves. Keep them private."),
         gp::RectF(x + dp(70), dp(137), w - dp(90), dp(22)), *g.fSmall, C_DIM);
    if (vault::progress.running) {
        roundFill(gr, gp::RectF(x + dp(18), dp(174), w - dp(36), dp(6)), dp(3), C_CHIP);
        gradientFill(gr, gp::RectF(x + dp(18), dp(174), std::max(dp(4), (w - dp(36)) * vault::progress.fraction), dp(6)), dp(3));
    }
    float bw = (w - dp(16)) / 3;
    button(gr, gp::RectF(x, dp(207), bw, dp(38)), i18n::text(L"Create backup"), Btn::Primary, !running() && idle() && !g.busy, [] {
        tool([] {
            vault::create(reporter());
            PostMessageW(g.hwnd, WM_APP + 45, 0, 0);
        });
    });
    button(gr, gp::RectF(x + bw + dp(8), dp(207), bw, dp(38)), i18n::text(L"Browse backup"), Btn::Secondary, !g.busy, [] {
        auto folder = chooseFolder(i18n::text(L"Choose a private Velox .vbackup folder"));
        if (folder.empty())
            return;
        auto entry = vault::inspect(folder);
        if (!entry.valid) {
            say(i18n::text(L"Choose a completed Velox backup with manifest.vlx."), true);
            return;
        }
        g.backups.insert(g.backups.begin(), entry);
        g.selectedBackup = entry.path;
        g.backupPage = 0;
    });
    if (vault::progress.running) {
        g.vaultAllow = true;
        button(gr, gp::RectF(x + 2 * (bw + dp(8)), dp(207), bw, dp(38)), i18n::text(L"Cancel"), Btn::Secondary, true,
               [] { vault::progress.cancel = true; });
        g.vaultAllow = false;
    } else
        button(gr, gp::RectF(x + 2 * (bw + dp(8)), dp(207), bw, dp(38)), i18n::text(L"Vault location"), Btn::Secondary, !g.busy, [] {
            auto f = chooseFolder(i18n::text(L"Choose where private phone backups are kept"));
            if (!f.empty()) {
                if (vault::setFolder(f)) {
                    refreshVault();
                    say(i18n::text(L"Vault location updated."));
                } else
                    say(i18n::text(L"Choose a writable local folder without links."), true);
            }
        });
    label(gr, i18n::text(L"RECOVERY POINTS"), x, dp(268), w);
    button(gr, gp::RectF(x + w - dp(142), dp(260), dp(65), dp(28)), i18n::text(L"Previous"), Btn::Ghost, g.backupPage > 0,
           [] { --g.backupPage; });
    button(gr, gp::RectF(x + w - dp(70), dp(260), dp(65), dp(28)), i18n::text(L"Next"), Btn::Ghost,
           (g.backupPage + 1) * 2 < int(g.backups.size()), [] { ++g.backupPage; });
    if (g.backups.empty()) {
        card(gr, gp::RectF(x, dp(303), w, dp(180)));
        text(gr, i18n::text(L"Make your first recovery point"), gp::RectF(x + dp(24), dp(326), w - dp(48), dp(34)), *g.fBig, C_TEXT);
        text(gr,
             i18n::text(
                 L"Stop the phone, then create a verified copy. Restores use a separate data folder; the previous phone stays available "
                 L"through Undo."),
             gp::RectF(x + dp(24), dp(369), w - dp(48), dp(80)), *g.fText, C_DIM, gp::StringAlignmentNear, gp::StringAlignmentCenter, true);
    }
    for (int row = 0; row < 2; row++) {
        size_t i = g.backupPage * 2 + row;
        if (i >= g.backups.size())
            break;
        auto entry = g.backups[i];
        gp::RectF r(x, dp(301 + row * 105), w, dp(94));
        int id = addHit(r, true, [entry] { g.selectedBackup = entry.path; });
        roundFill(gr, r, dp(14), entry.path == g.selectedBackup ? 0xff2b2544 : g.hover == id ? C_CHIP : C_CARD);
        text(gr, entry.name, gp::RectF(x + dp(18), r.Y + dp(11), w - dp(36), dp(25)), *g.fBold, C_TEXT);
        std::wstring detail = std::to_wstring((entry.bytes + (1 << 19)) >> 20) + i18n::text(L" MB · ") + std::to_wstring(entry.files) +
                              i18n::text(L" files · ") + (entry.valid ? i18n::text(L"Ready to verify") : i18n::text(L"Needs attention"));
        text(gr, detail, gp::RectF(x + dp(18), r.Y + dp(45), w - dp(36), dp(24)), *g.fSmall, entry.valid ? C_DIM : C_WARN);
    }
    bool selected = !g.selectedBackup.empty() && !g.busy;
    button(gr, gp::RectF(x, dp(520), bw, dp(36)), i18n::text(L"Verify files"), Btn::Secondary, selected, [] {
        auto path = g.selectedBackup;
        tool([path] { vault::verify(path, reporter()); });
    });
    button(gr, gp::RectF(x + bw + dp(8), dp(520), bw, dp(36)), i18n::text(L"Restore as new phone"), Btn::Primary,
           selected && !running() && idle(), [] {
               auto path = g.selectedBackup;
               studio::confirm(
                   g.hwnd, i18n::text(L"Restore this recovery point?"),
                   i18n::text(L"A separate phone will be prepared and checked. Your current phone data stays intact. Controls and phone "
                              L"preferences switch to the backup; Undo returns to the previous setup."),
                   i18n::text(L"Prepare restore"), [path](bool yes) {
                       if (!yes)
                           return;
                       tool([path] {
                           auto prepared = vault::prepare(path, reporter());
                           if (prepared) {
                               auto value = std::make_unique<std::wstring>(*prepared);
                               if (PostMessageW(g.hwnd, WM_APP + 46, 0, reinterpret_cast<LPARAM>(value.get())))
                                   value.release();
                           }
                       });
                   });
           });
    button(gr, gp::RectF(x + 2 * (bw + dp(8)), dp(520), bw, dp(36)), i18n::text(L"Undo last restore"), Btn::Secondary,
           !running() && idle() && !g.busy && vault::canUndo(), [] {
               studio::confirm(
                   g.hwnd, i18n::text(L"Return to the previous phone?"),
                   i18n::text(
                       L"The current recovered phone remains on disk. Your previous phone, controls and preferences become active again."),
                   i18n::text(L"Undo restore"), [](bool yes) {
                       if (yes && vault::undo(reporter()))
                           reloadRecovery();
                   });
           });
    button(gr, gp::RectF(x, dp(571), bw, dp(32)), i18n::text(L"Diagnostics"), Btn::Ghost, !g.busy,
           [] { showText(i18n::text(L"Velox diagnostics · review before sharing"), vault::diagnostics()); });
    button(gr, gp::RectF(x + bw + dp(8), dp(571), bw, dp(32)), i18n::text(L"Open vault"), Btn::Ghost, !g.busy, [] {
        createDirs(vault::folder());
        openFolder(vault::folder());
    });
    button(gr, gp::RectF(x + 2 * (bw + dp(8)), dp(571), bw, dp(32)), i18n::text(L"Refresh"), Btn::Ghost, !g.busy, refreshVault);
}

void refreshInstances() {
    g.phones = instances::list();
    if (g.instancePage * 3 >= int(g.phones.size()))
        g.instancePage = std::max(0, (int(g.phones.size()) + 2) / 3 - 1);
}

void drawInstances(gp::Graphics &gr) {
    const float x = X0(), w = CW();
    pageHeader(gr, i18n::text(L"Velox instances"), i18n::text(L"A separate phone for every account and play style."));
    card(gr, gp::RectF(x, dp(96), w, dp(70)));
    int active = 0, ram = 0, cores = 0;
    for (auto &i : g.phones)
        if (i.active) {
            ++active;
            ram += i.memory;
            cores += i.cores;
        }
    text(gr,
         std::to_wstring(active) + i18n::text(L" running · ") + std::to_wstring(cores) + i18n::text(L" cores · ") + std::to_wstring(ram) +
             i18n::text(L" GB allocated"),
         gp::RectF(x + dp(18), dp(104), w - dp(200), dp(25)), *g.fBold, C_TEXT);
    text(gr, i18n::text(L"Shared Android. Separate apps, saves and controls."), gp::RectF(x + dp(18), dp(133), w - dp(200), dp(19)),
         *g.fSmall, C_DIM);
    button(
        gr, gp::RectF(x + w - dp(172), dp(113), dp(154), dp(36)), i18n::text(L"New phone"), Btn::Primary,
        !g.busy && !paths().sdk.empty() && g.phones.size() < 9,
        [] {
            auto sdk = paths().sdk;
            tool([sdk] {
                instances::create(sdk, reporter());
                PostMessageW(g.hwnd, WM_APP + 56, 0, 0);
            });
        },
        L"\xE710");
    for (int row = 0; row < 3; row++) {
        size_t index = g.instancePage * 3 + row;
        if (index >= g.phones.size())
            break;
        auto info = g.phones[index];
        float y = dp(181 + row * 126);
        card(gr, gp::RectF(x, y, w, dp(116)));
        art::logo(gr, x + dp(16), y + dp(13), dp(30));
        text(gr, info.name, gp::RectF(x + dp(58), y + dp(10), w - dp(200), dp(27)), *g.fH2, C_TEXT);
        text(gr,
             info.current    ? i18n::text(L"THIS PHONE")
             : info.starting ? i18n::text(L"STARTING")
             : info.active   ? i18n::text(L"RUNNING")
                             : i18n::text(L"READY"),
             gp::RectF(x + w - dp(145), y + dp(15), dp(125), dp(22)), *g.fSmallBold, info.active ? C_GOOD : C_DIM, gp::StringAlignmentFar);
        text(gr,
             info.starting ? i18n::text(L"Android is starting…")
             : info.active ? std::to_wstring(info.cores) + i18n::text(L" cores · ") + std::to_wstring(info.memory) +
                                 i18n::text(L" GB · independent phone data")
             : !info.error.empty() ? info.error
                                   : i18n::text(L"Ready for its own apps, accounts and games"),
             gp::RectF(x + dp(58), y + dp(41), w - dp(78), dp(22)), *g.fSmall, !info.active && !info.error.empty() ? C_BAD : C_DIM);
        button(gr, gp::RectF(x + dp(17), y + dp(75), dp(161), dp(29)), i18n::text(L"Open controls"), Btn::Secondary, !g.busy,
               [info] { instances::command(info.id, instances::Command::Open, reporter()); });
        button(gr, gp::RectF(x + dp(187), y + dp(75), dp(145), dp(29)),
               info.starting ? i18n::text(L"Starting…")
               : info.active ? i18n::text(L"Stop phone")
                             : i18n::text(L"Start phone"),
               info.active ? Btn::Secondary : Btn::Primary, !g.busy && !info.starting,
               [info] { instances::command(info.id, info.active ? instances::Command::Stop : instances::Command::Start, reporter()); });
        button(gr, gp::RectF(x + w - dp(174), y + dp(75), dp(157), dp(29)), i18n::text(L"Close controls"), Btn::Ghost,
               !g.busy && info.window && !info.current,
               [info] { instances::command(info.id, instances::Command::CloseControls, reporter()); });
    }
    button(gr, gp::RectF(x, dp(578), dp(108), dp(30)), i18n::text(L"Previous"), Btn::Ghost, g.instancePage > 0, [] { --g.instancePage; });
    text(gr, i18n::text(L"Each phone has its own CPU and RAM settings."), gp::RectF(x + dp(115), dp(579), w - dp(230), dp(28)), *g.fSmall,
         C_DIM, gp::StringAlignmentCenter);
    button(gr, gp::RectF(x + w - dp(108), dp(578), dp(108), dp(30)), i18n::text(L"Next"), Btn::Ghost,
           (g.instancePage + 1) * 3 < int(g.phones.size()), [] { ++g.instancePage; });
}

std::wstring searchKey(const std::wstring &value) {
    if (value.empty())
        return {};
    int length = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr, 0);
    if (length <= 0)
        return value;
    std::wstring lowered(length, L'\0');
    if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(), int(value.size()), lowered.data(), length, nullptr, nullptr,
                       0))
        return value;
    return lowered;
}

void drawLibrary(gp::Graphics &gr) {
    g.libraryCards.clear();
    const float x = X0(), w = CW();
    pageHeader(gr, i18n::text(L"Your game library"), i18n::text(L"Your games. Your apps. One play space."));
    button(
        gr, gp::RectF(x + w - dp(120), dp(28), dp(120), dp(34)), i18n::text(L"Install app"), Btn::Secondary, running() && !g.busy,
        [] {
            auto files = pickFiles(true);
            if (!files.empty())
                installOrSend(files);
        },
        L"\xE710");
    card(gr, gp::RectF(x, dp(96), w, dp(52)));
    text(gr, L"\xE721", gp::RectF(x + dp(12), dp(96), dp(24), dp(52)), *g.fIcon, C_ACCENT, gp::StringAlignmentCenter);

    if (g.searchEdit)
        placeField(g.searchEdit, static_cast<int>(x + dp(46)), static_cast<int>(dp(112)), static_cast<int>(w - dp(166)),
                   static_cast<int>(dp(24)));
    button(gr, gp::RectF(x + w - dp(106), dp(105), dp(96), dp(34)),
           experience::libraryBusy() ? i18n::text(L"Loading…") : i18n::text(L"Refresh"), Btn::Secondary,
           running() && !experience::libraryBusy(), [] { experience::requestLibrary(); });
    segments(
        gr, gp::RectF(x, dp(162), w, dp(36)),
        {i18n::text(L"All apps"), i18n::text(L"Favorites"), i18n::text(L"Games"), i18n::text(L"Recent")}, g.libraryFilter,
        [](int) { return true; },
        [](int i) {
            g.libraryFilter = i;
            g.libraryPage = 0;
        });
    auto apps = experience::library();
    auto lowerText = [](const std::wstring &s) { return searchKey(s); };
    std::wstring search = lowerText(g.librarySearch);
    apps.erase(std::remove_if(apps.begin(), apps.end(),
                              [&](const auto &a) {
                                  return (!search.empty() && lowerText(a.label).find(search) == std::wstring::npos &&
                                          lowerText(a.package).find(search) == std::wstring::npos) ||
                                         (g.libraryFilter == 1 && !a.favorite) || (g.libraryFilter == 2 && !a.game) ||
                                         (g.libraryFilter == 3 && !a.played);
                              }),
               apps.end());
    if (g.libraryFilter == 3)
        std::stable_sort(apps.begin(), apps.end(), [](auto &a, auto &b) { return a.played > b.played; });
    if (apps.empty()) {
        card(gr, gp::RectF(x, dp(214), w, dp(302)));
        art::logo(gr, x + w / 2 - dp(33), dp(250), dp(66));
        std::wstring title = experience::libraryBusy()                     ? i18n::text(L"Building your library…")
                             : !running() && experience::library().empty() ? i18n::text(L"Your next game starts here")
                                                                           : i18n::text(L"Nothing here yet");
        text(gr, title, gp::RectF(x, dp(336), w, dp(28)), *g.fH2, C_TEXT, gp::StringAlignmentCenter);
        std::wstring hint = g.libraryFilter == 1   ? i18n::text(L"Use the star on an app to keep it close.")
                            : g.libraryFilter == 2 ? i18n::text(L"Right-click an app and choose Mark as game to add it here.")
                            : g.libraryFilter == 3 ? i18n::text(L"Open a game from Velox to see it here.")
                                                   : i18n::text(L"Start your phone to discover its apps, or install a game from Overview.");
        text(gr, hint, gp::RectF(x + dp(24), dp(374), w - dp(48), dp(48)), *g.fText, C_DIM, gp::StringAlignmentCenter,
             gp::StringAlignmentCenter, true);
        button(gr, gp::RectF(x + w / 2 - dp(90), dp(442), dp(180), dp(40)),
               running() ? i18n::text(L"Install a game") : i18n::text(L"Start phone"), Btn::Primary, idle() && !g.busy, [] {
                   if (!running())
                       startPhone();
                   else {
                       auto files = pickFiles(true);
                       if (!files.empty())
                           installOrSend(files);
                   }
               });
    } else {
        int pages = static_cast<int>((apps.size() + 5) / 6);
        g.libraryPage = std::clamp(g.libraryPage, 0, pages - 1);
        float cw = (w - dp(20)) / 3;
        for (int slot = 0; slot < 6; slot++) {
            size_t index = g.libraryPage * 6 + slot;
            if (index >= apps.size())
                break;
            auto app = apps[index];
            float cx = x + (slot % 3) * (cw + dp(10)), cy = dp(214) + (slot / 3) * dp(160);
            gp::RectF r(cx, cy, cw, dp(150));
            g.libraryCards.emplace_back(r, app);
            card(gr, r);
            if (!g.appIcons.count(app.icon) && exists(app.icon)) {
                auto image = std::make_unique<gp::Bitmap>(app.icon.c_str());
                if (image->GetLastStatus() == gp::Ok)
                    g.appIcons.emplace(app.icon, std::move(image));
            }
            auto icon = g.appIcons.find(app.icon);
            if (icon != g.appIcons.end())
                gr.DrawImage(icon->second.get(), cx + dp(14), cy + dp(12), dp(44), dp(44));
            else
                art::logo(gr, cx + dp(14), cy + dp(12), dp(44));
            button(
                gr, gp::RectF(cx + cw - dp(80), cy + dp(12), dp(32), dp(32)), L"", Btn::Ghost, !g.busy, [app] { showPreset(app); },
                L"\xE713");
            button(gr, gp::RectF(cx + cw - dp(44), cy + dp(12), dp(32), dp(32)), app.favorite ? L"★" : L"☆", Btn::Ghost,
                   running() && !g.busy, [app] { tool([app] { experience::favorite(app, reporter()); }); });
            text(gr, app.label, gp::RectF(cx + dp(14), cy + dp(65), cw - dp(28), dp(22)), *g.fBold, C_TEXT, gp::StringAlignmentNear,
                 gp::StringAlignmentCenter, false, true);
            text(gr,
                 presets::load(app.package).enabled ? i18n::text(L"CUSTOM PRESET")
                 : app.game                         ? i18n::text(L"GAME")
                                                    : i18n::text(L"ANDROID APP"),
                 gp::RectF(cx + dp(14), cy + dp(87), cw - dp(28), dp(16)), *g.fTiny, C_FAINT);
            float half = (cw - dp(32)) / 2;
            button(gr, gp::RectF(cx + dp(12), cy + dp(112), half, dp(28)), app.game ? i18n::text(L"Play") : i18n::text(L"Open"),
                   Btn::Primary, idle() && !g.busy, [app] { launchGame(app.component); });
            button(gr, gp::RectF(cx + dp(20) + half, cy + dp(112), half, dp(28)), i18n::text(L"Controls"), Btn::Secondary,
                   idle() && !g.busy, [app] {
                       g.page = CONTROLS;
                       ShowWindow(g.searchEdit, SW_HIDE);
                       launchGame(app.component, true);
                   });
        }
        text(gr,
             std::to_wstring(apps.size()) + i18n::text(L" apps  ·  ") + std::to_wstring(g.libraryPage + 1) + L" / " +
                 std::to_wstring(pages),
             gp::RectF(x, dp(548), w - dp(160), dp(30)), *g.fSmall, C_DIM);
        button(gr, gp::RectF(x + w - dp(150), dp(548), dp(68), dp(30)), i18n::text(L"Previous"), Btn::Secondary, g.libraryPage > 0,
               [] { --g.libraryPage; });
        button(gr, gp::RectF(x + w - dp(72), dp(548), dp(72), dp(30)), i18n::text(L"Next"), Btn::Secondary, g.libraryPage + 1 < pages,
               [] { ++g.libraryPage; });
    }
}

void drawControls(gp::Graphics &gr) {
    const float x = X0(), w = CW();
    auto profile = playtools::profile();
    pageHeader(gr, i18n::text(L"Control Studio"), i18n::text(L"Built around the way you play."));
    card(gr, gp::RectF(x, dp(96), w, dp(74)));
    text(gr, L"\xE765", gp::RectF(x + dp(14), dp(96), dp(42), dp(74)), *g.fIconBig, C_ACCENT, gp::StringAlignmentCenter);
    std::wstring name = profile.package;
    for (auto &a : experience::library())
        if (a.package == name) {
            name = a.label;
            break;
        }
    text(gr, name.empty() ? i18n::text(L"Choose a game from Library") : name, gp::RectF(x + dp(72), dp(107), w - dp(90), dp(26)), *g.fBig,
         C_TEXT);
    text(gr,
         profile.package.empty() ? i18n::text(L"Each game gets its own saved control layout.")
         : profile.landscape     ? i18n::text(L"Landscape profile · saved automatically")
                                 : i18n::text(L"Portrait profile · saved automatically"),
         gp::RectF(x + dp(72), dp(137), w - dp(90), dp(20)), *g.fSmall, C_DIM);
    bool can = running() && !profile.package.empty() && profile.package != L"app.velox.home";
    toggle(
        gr, gp::RectF(x, dp(182), w, dp(56)), i18n::text(L"Keyboard and mouse mapping"),
        i18n::text(L"Only active while the phone has focus. Esc always releases mouse aim."), profile.enabled, can,
        [] { playtools::enabled(!playtools::profile().enabled); }, L"\xE7FC");
    button(gr, gp::RectF(x, dp(252), dp(200), dp(40)),
           playtools::editing() ? i18n::text(L"Editing on phone…") : i18n::text(L"Edit on phone"), Btn::Primary, can,
           [] { playtools::edit(); });
    button(gr, gp::RectF(x + dp(212), dp(252), dp(170), dp(40)), i18n::text(L"Choose a game"), Btn::Secondary, true, [] {
        g.page = LIBRARY;
        ShowWindow(g.searchEdit, SW_SHOW);
    });
    button(gr, gp::RectF(x + dp(394), dp(252), w - dp(394), dp(40)), i18n::text(L"Controller Studio"), Btn::Secondary,
           !profile.package.empty() && !g.busy, [profile] {
               experience::AppInfo app;
               app.package = profile.package;
               app.label = profile.package;
               for (auto &a : experience::library())
                   if (a.package == app.package) {
                       app = a;
                       break;
                   }
               studio::controller(g.hwnd, app);
           });
    text(gr, std::to_wstring(profile.bindings.size()) + i18n::text(L" bindings  ·  ") + playtools::status(),
         gp::RectF(x, dp(304), w, dp(24)), *g.fSmall, C_DIM);
    card(gr, gp::RectF(x, dp(344), w, dp(130)));
    text(gr, i18n::text(L"Keyboard  ·  Mouse  ·  Controller  ·  Your layout"), gp::RectF(x + dp(18), dp(355), w - dp(36), dp(26)), *g.fBold,
         C_TEXT);
    text(gr,
         i18n::text(
             L"Click the phone to place a binding, then press a key. Drag it to move it.\nRight-click for joystick, swipe and mouse aim "
             L"options, including controller sticks and buttons.\nF8 locks mouse aim; Esc releases it. F10 closes the editor."),
         gp::RectF(x + dp(18), dp(389), w - dp(36), dp(72)), *g.fText, C_DIM, gp::StringAlignmentNear, gp::StringAlignmentCenter, true);
    toggle(gr, gp::RectF(x, dp(488), w, dp(48)), i18n::text(L"Velox side toolbar"),
           i18n::text(L"Right-click it to dock, collapse or reorder its buttons."), playtools::toolbarVisible(), true,
           [] { playtools::toolbarVisible(!playtools::toolbarVisible()); });
    float bw = (w - dp(16)) / 3;
    button(gr, gp::RectF(x, dp(550), bw, dp(36)), L"Velox Home", Btn::Secondary, running() && !g.busy,
           [] { tool([] { experience::openHome(reporter()); }); });
    button(gr, gp::RectF(x + bw + dp(8), dp(550), bw, dp(36)), i18n::text(L"Use as default Home"), Btn::Secondary, running() && !g.busy,
           [] { tool([] { experience::openHome(reporter(), true); }); });
    button(gr, gp::RectF(x + 2 * (bw + dp(8)), dp(550), bw, dp(36)), i18n::text(L"Velox Settings"), Btn::Secondary, running() && !g.busy,
           [] { tool([] { experience::openSettings(reporter()); }); });
}

void drawDisplay(gp::Graphics &gr) {
    const float x = X0(), w = CW();
    pageHeader(gr, i18n::text(L"Display"), i18n::text(L"Screen, rotation and Android navigation."));
    label(gr, i18n::text(L"RESOLUTION"), x, dp(96), w);
    float cw = (w - dp(16)) / 3, ch = dp(64);
    for (size_t i = 0; i < displays().size(); i++) {
        const Display &d = displays()[i];
        gp::RectF c(x + (i % 3) * (cw + dp(8)), dp(118) + (i / 3) * (ch + dp(8)), cw, ch);
        bool sel = g.set.display == static_cast<int>(i);
        int id = addHit(c, true, [i] {
            g.set.display = static_cast<int>(i);
            save();
        });
        if (sel)
            gradientFill(gr, c, dp(12));
        else
            roundFill(gr, c, dp(12), g.hover == id ? C_CHIP_HOVER : C_CARD);
        // A little screen shape in the right proportions.
        float sh = dp(38), sw = sh * d.w / d.h;
        gp::RectF shape(c.X + dp(14), c.Y + (ch - sh) / 2, sw, sh);
        gp::Pen pen(gp::Color(sel ? 0xFFFFFFFF : C_DIM), dp(1.6f));
        gp::GraphicsPath p;
        art::roundRectPath(p, shape, dp(4));
        gr.DrawPath(&pen, &p);
        float tx = c.X + dp(14) + sw + dp(14);
        text(gr, d.name, gp::RectF(tx, c.Y + dp(13), c.GetRight() - tx - dp(8), dp(20)), *g.fBold, C_TEXT);
        text(gr, d.detail, gp::RectF(tx, c.Y + dp(33), c.GetRight() - tx - dp(8), dp(18)), *g.fSmall, sel ? 0xFFE6E2FF : C_DIM);
    }
    float y = dp(118) + 2 * (ch + dp(8)) + dp(14);
    label(gr, i18n::text(L"REFRESH RATE"), x, y, w);
    text(gr, i18n::text(L"Your monitor: ") + std::to_wstring(host().monitorHz) + L" Hz", gp::RectF(x, y, w, dp(18)), *g.fSmall, C_FAINT,
         gp::StringAlignmentFar);
    std::vector<std::wstring> labels;
    int sel = 0;
    for (size_t i = 0; i < refreshRates().size(); i++) {
        labels.push_back(std::to_wstring(refreshRates()[i]) + L" Hz");
        if (refreshRates()[i] == g.set.rate)
            sel = static_cast<int>(i);
    }
    segments(
        gr, gp::RectF(x, y + dp(22), w, dp(40)), labels, sel, [](int) { return true; },
        [](int i) {
            g.set.rate = refreshRates()[i];
            save();
        });
    y += dp(80);
    toggle(
        gr, gp::RectF(x, y, w, dp(56)), i18n::text(L"Device frame"),
        i18n::text(L"A sleek phone body around the screen instead of a plain window."), g.set.frame, true,
        [] {
            g.set.frame = !g.set.frame;
            save();
        },
        L"\xE8EA");
    y += dp(64);
    toggle(
        gr, gp::RectF(x, y, w, dp(56)), i18n::text(L"Automatic rotation"),
        i18n::text(L"The phone follows your game's portrait or landscape orientation."), g.set.autoRotate, true,
        [] {
            g.set.autoRotate = !g.set.autoRotate;
            setAutoRotation(g.set.autoRotate);
            if (running())
                phone::running.autoRotate = g.set.autoRotate;
            save();
        },
        L"\xE7AD");
    y += dp(64);
    label(gr, i18n::text(L"ANDROID NAVIGATION"), x, y, w);
    segments(
        gr, gp::RectF(x, y + dp(22), w, dp(36)),
        {i18n::text(L"Back · Home · Recents"), i18n::text(L"Recents · Home · Back"), i18n::text(L"Gestures")},
        g.set.gestureNavigation ? 2
        : g.set.reverseButtons  ? 1
                                : 0,
        [](int) { return true; },
        [](int i) {
            g.set.gestureNavigation = i == 2;
            if (i != 2)
                g.set.reverseButtons = i == 1;
            setGestureNavigation(g.set.gestureNavigation, g.set.reverseButtons);
            if (running()) {
                phone::running.gestureNavigation = g.set.gestureNavigation;
                phone::running.reverseButtons = g.set.reverseButtons;
            }
            save();
        });
    y += dp(70);
    if (needsRestart()) {
        button(gr, gp::RectF(x, y, dp(220), dp(38)), i18n::text(L"Restart to apply"), Btn::Primary, idle(), restartToApply, L"\xE72C");
    }
}

void drawPerformance(gp::Graphics &gr) {
    const float x = X0(), w = CW();
    pageHeader(gr, i18n::text(L"Performance"), i18n::text(L"How much of this PC the phone may use."));
    float y = dp(96);
    card(gr, gp::RectF(x, y, w, dp(82)));
    text(gr, L"\xE9F5", gp::RectF(x + dp(14), y, dp(24), dp(44)), *g.fIconBig, C_ACCENT, gp::StringAlignmentCenter);
    text(gr, i18n::text(L"Detected on this PC"), gp::RectF(x + dp(48), y + dp(14), w - dp(230), dp(20)), *g.fBold, C_TEXT);
    text(gr, hostSummary(), gp::RectF(x + dp(18), y + dp(51), w - dp(36), dp(18)), *g.fSmall, C_DIM);
    button(gr, gp::RectF(x + w - dp(176), y + dp(16), dp(160), dp(38)), i18n::text(L"Use recommended"), Btn::Secondary, true, [] {
        refreshHost(g.hwnd);
        Settings r = recommended();
        Settings &s = g.set;
        s.display = r.display;
        s.rate = r.rate;
        s.cores = r.cores;
        s.memory = r.memory;
        s.gpu = r.gpu;
        s.boost = r.boost;
        s.snappy = r.snappy;
        save();
        say(i18n::text(L"Recommended settings for this PC are set."));
    });
    y += dp(98);

    label(gr, i18n::text(L"VIRTUAL CPU CORES"), x, y, w);
    text(gr,
         L"Auto: " + std::to_wstring(coresFor(Settings{})) +
             (cpuLimit() < std::min(host().threads, MAX_CPU_CORES) ? i18n::text(L" · Android limit: ") + std::to_wstring(cpuLimit()) : L""),
         gp::RectF(x, y, w, dp(18)), *g.fSmall, C_DIM, gp::StringAlignmentFar);
    std::vector<std::wstring> cl;
    int csel = 0;
    for (size_t i = 0; i < coreChoices().size(); i++) {
        int c = coreChoices()[i];
        cl.push_back(c == 0 ? i18n::text(L"Auto") : std::to_wstring(c));
        if (c == (g.set.cores == 0 ? 0 : coresFor(g.set)))
            csel = static_cast<int>(i);
    }
    segments(
        gr, gp::RectF(x, y + dp(22), w, dp(38)), cl, csel, [](int i) { return coreAllowed(coreChoices()[i]); },
        [](int i) {
            g.set.cores = coreChoices()[i];
            save();
        });
    y += dp(76);
    label(gr, i18n::text(L"MEMORY (GB)"), x, y, w);
    text(gr, L"Auto: " + std::to_wstring(memoryFor(Settings{})) + i18n::text(L" GB · Windows memory reserved"),
         gp::RectF(x + dp(130), y, w - dp(130), dp(18)), *g.fSmall, C_DIM);
    std::vector<std::wstring> ml;
    int msel = 0;
    for (size_t i = 0; i < memoryChoices().size(); i++) {
        int m = memoryChoices()[i];
        ml.push_back(m == 0 ? i18n::text(L"Auto") : std::to_wstring(m));
        if (m == (g.set.memory == 0 ? 0 : memoryFor(g.set)))
            msel = static_cast<int>(i);
    }
    segments(
        gr, gp::RectF(x, y + dp(22), w, dp(38)), ml, msel, [](int i) { return memoryAllowed(memoryChoices()[i]); },
        [](int i) {
            g.set.memory = memoryChoices()[i];
            save();
        });
    y += dp(76);
    label(gr, i18n::text(L"GRAPHICS"), x, y, w);
    std::wstring gpuName = host().gpu.empty() ? L"GPU" : host().gpu;
    segments(
        gr, gp::RectF(x, y + dp(22), w, dp(38)), {i18n::text(L"Auto"), L"GPU  ·  " + gpuName, i18n::text(L"Software")},
        (g.set.gpu == GPU_HOST && host().softwareGpu) ? GPU_SOFTWARE : g.set.gpu,
        [](int i) { return i != GPU_HOST || !host().softwareGpu; },
        [](int i) {
            g.set.gpu = i;
            save();
        });
    y += dp(76);
    float half = (w - dp(10)) / 2;
    toggle(gr, gp::RectF(x, y, half, dp(56)), i18n::text(L"Performance boost"), i18n::text(L"Phone gets priority on this PC"), g.set.boost,
           true, [] {
               g.set.boost = !g.set.boost;
               save();
               applyLiveAsync();
           });
    toggle(gr, gp::RectF(x + half + dp(10), y, half, dp(56)), i18n::text(L"Snappy animations"), i18n::text(L"Faster screen transitions"),
           g.set.snappy, true, [] {
               g.set.snappy = !g.set.snappy;
               save();
               applyLiveAsync();
           });
    y += dp(64);
    toggle(
        gr, gp::RectF(x, y, w, dp(56)), i18n::text(L"Instant start"),
        i18n::text(L"Resume where you left off. Boots normally after Vulkan games."), g.set.quickBoot, true,
        [] {
            g.set.quickBoot = !g.set.quickBoot;
            save();
        },
        L"\xE945");
    if (needsRestart())
        button(gr, gp::RectF(x + w - dp(200), dp(24), dp(200), dp(36)), i18n::text(L"Restart to apply"), Btn::Primary, idle(),
               restartToApply);
}

constexpr int THUMB_W = 216, THUMB_H = 400;

std::unique_ptr<gp::Bitmap> branded(gp::Bitmap &source, int brand) {
    auto t = std::make_unique<gp::Bitmap>(THUMB_W, THUMB_H, PixelFormat32bppARGB);
    gp::Graphics tg(t.get());
    tg.DrawImage(&source, 0, 0, THUMB_W, THUMB_H);
    art::drawBranding(tg, THUMB_W, THUMB_H, brand);
    return t;
}

// Builds the gallery in the background (pictures can be large); the newest request wins.
void loadWalls() {
    unsigned my = ++g.wallGen;
    int brand = g.set.brand;
    std::thread([my, brand] {
        std::vector<App::Wall> list;
        for (size_t i = 0; i < art::wallpapers().size(); i++) {
            App::Wall w;
            w.builtin = static_cast<int>(i);
            w.name = art::wallpapers()[i].name;
            w.source = art::renderWallpaper(static_cast<int>(i), THUMB_W, THUMB_H);
            list.push_back(std::move(w));
        }
        for (auto &f : wallpaperFiles()) {
            gp::Bitmap img(f.c_str());
            if (img.GetLastStatus() != gp::Ok || img.GetWidth() == 0)
                continue;
            App::Wall w;
            w.file = f;
            w.name = fileName(f);
            w.name = w.name.substr(0, w.name.find_last_of(L'.'));
            const std::wstring mascotPrefix = L"Velox-chan - ";
            if (w.name.rfind(mascotPrefix, 0) == 0)
                w.name.erase(0, mascotPrefix.size());
            w.source = art::fitImage(img, THUMB_W, THUMB_H, art::BRAND_NONE);
            list.push_back(std::move(w));
            if (my != g.wallGen)
                return;
        }
        for (auto &w : list)
            w.thumb = branded(*w.source, brand);
        if (my != g.wallGen)
            return;
        {
            std::lock_guard<std::mutex> l(g.lock);
            g.walls = std::move(list);
        }
        refresh();
    }).detach();
}

// Re-draws the branding on every thumbnail (quick: they are small).
void rebrandWalls() {
    std::lock_guard<std::mutex> l(g.lock);
    for (auto &w : g.walls)
        if (w.source)
            w.thumb = branded(*w.source, g.set.brand);
}

std::vector<std::wstring> pickImages() {
    std::vector<wchar_t> buf(32768, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter = L"Pictures (*.png;*.jpg;*.jpeg;*.bmp;*.gif)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_ALLOWMULTISELECT;
    std::vector<std::wstring> out;
    if (!GetOpenFileNameW(&ofn))
        return out;
    std::wstring first = buf.data();
    const wchar_t *p = buf.data() + first.size() + 1;
    if (!*p)
        return {first};
    for (; *p; p += wcslen(p) + 1)
        out.push_back(first + L"\\" + p);
    return out;
}

void drawLook(gp::Graphics &gr) {
    const float x = X0(), w = CW();
    pageHeader(gr, i18n::text(L"Look"), i18n::text(L"Mouse cursor, wallpapers and the boot screen."));
    float y = dp(96);
    label(gr, i18n::text(L"CURSOR OVER THE PHONE"), x, y, w);
    const int perRow = 5;
    float tw = (w - dp(8) * (perRow - 1)) / perRow, th = dp(58);
    for (size_t i = 0; i < art::cursorNames().size(); i++) {
        gp::RectF t(x + (i % perRow) * (tw + dp(8)), y + dp(22) + (i / perRow) * (th + dp(8)), tw, th);
        bool sel = g.set.cursor == static_cast<int>(i);
        int id = addHit(t, true, [i] {
            g.set.cursor = static_cast<int>(i);
            overlay::setCursorStyle(g.set.cursor);
            save();
        });
        if (sel) {
            gradientFill(gr, t, dp(12));
            roundFill(gr, gp::RectF(t.X + dp(2), t.Y + dp(2), t.Width - dp(4), t.Height - dp(4)), dp(10), C_CARD);
        } else {
            roundFill(gr, t, dp(12), g.hover == id ? C_CHIP_HOVER : C_CARD);
        }
        art::drawCursorPreview(gr, static_cast<int>(i), gp::RectF(t.X + dp(9), t.Y + (th - dp(26)) / 2, dp(26), dp(26)));
        text(gr, art::cursorNames()[i], gp::RectF(t.X + dp(40), t.Y, t.Width - dp(42), th), *g.fText, sel ? C_TEXT : C_DIM);
    }
    y += dp(22) + 2 * (th + dp(8)) + dp(8);

    // Wallpapers, five at a time: built-in designs, your packs, then a tile to add pictures.
    label(gr, i18n::text(L"WALLPAPERS"), x, y, w);
    std::lock_guard<std::mutex> wallLock(g.lock);
    const int total = static_cast<int>(g.walls.size()) + 1, perPage = 5, pages = (total + perPage - 1) / perPage;
    g.wallPage = std::clamp(g.wallPage, 0, pages - 1);
    button(gr, gp::RectF(x + w - dp(64), y - dp(7), dp(30), dp(28)), L"‹", Btn::Ghost, g.wallPage > 0, [] { g.wallPage--; });
    button(gr, gp::RectF(x + w - dp(30), y - dp(7), dp(30), dp(28)), L"›", Btn::Ghost, g.wallPage < pages - 1, [] { g.wallPage++; });
    std::vector<std::wstring> brandLabels;
    for (auto *n : art::brandNames())
        brandLabels.push_back(n);
    text(gr, i18n::text(L"Branding"), gp::RectF(x + w - dp(448), y - dp(7), dp(70), dp(28)), *g.fSmall, C_FAINT, gp::StringAlignmentFar);
    segments(
        gr, gp::RectF(x + w - dp(370), y - dp(7), dp(300), dp(28)), brandLabels, g.set.brand, [](int) { return true; },
        [](int i) {
            g.set.brand = i;
            save();
            std::thread([] {
                rebrandWalls();
                refresh();
            }).detach();
        });
    float ww = (w - dp(8) * (perPage - 1)) / perPage, wh = ww * 1.72f;
    bool can = running() && !g.busy;
    for (int k = 0; k < perPage; k++) {
        int i = g.wallPage * perPage + k;
        if (i >= total)
            break;
        gp::RectF t(x + k * (ww + dp(8)), y + dp(26), ww, wh);
        bool add = i == total - 1;
        if (add) {
            int id = addHit(gp::RectF(t.X, t.Y, t.Width, t.Height - dp(40)), true, [] {
                auto files = pickImages();
                if (files.empty())
                    return;
                int n = addWallpapers(files);
                say(n ? i18n::text(L"Added ") + std::to_wstring(n) + (n == 1 ? i18n::text(L" picture") : i18n::text(L" pictures")) +
                            i18n::text(L" to your wallpapers.")
                      : i18n::text(L"Those pictures couldn't be added."),
                    n == 0);
                loadWalls();
            });
            roundFill(gr, t, dp(12), g.hover == id ? C_CHIP_HOVER : C_CARD);
            gp::Pen dash(gp::Color(C_FAINT), dp(1.5f));
            gp::REAL pattern[] = {4.f, 3.f};
            dash.SetDashPattern(pattern, 2);
            gp::GraphicsPath p;
            art::roundRectPath(p, gp::RectF(t.X + dp(6), t.Y + dp(6), t.Width - dp(12), t.Height - dp(12)), dp(9));
            gr.DrawPath(&dash, &p);
            text(gr, L"\xE710", gp::RectF(t.X, t.Y + t.Height / 2 - dp(46), t.Width, dp(28)), *g.fIconBig, C_TEXT,
                 gp::StringAlignmentCenter);
            text(gr, i18n::text(L"PNG or JPG"), gp::RectF(t.X, t.Y + t.Height / 2 - dp(12), t.Width, dp(18)), *g.fSmall, C_FAINT,
                 gp::StringAlignmentCenter);
            button(gr, gp::RectF(t.X + dp(10), t.GetBottom() - dp(38), t.Width - dp(20), dp(28)), i18n::text(L"Open folder"), Btn::Ghost,
                   true, [] { openFolder(wallpaperFolder()); });
            text(gr, i18n::text(L"Add pictures…"), gp::RectF(t.X, t.GetBottom() + dp(4), t.Width, dp(18)), *g.fSmall, C_DIM,
                 gp::StringAlignmentCenter);
            continue;
        }
        const App::Wall &wall = g.walls[i];
        int builtin = wall.builtin;
        std::wstring file = wall.file;
        int id = addHit(t, can, [builtin, file] {
            int brand = g.set.brand;
            if (builtin >= 0)
                tool([builtin, brand] { applyWallpaper(builtin, brand, reporter()); });
            else
                tool([file, brand] { applyWallpaperFile(file, brand, reporter()); });
        });
        if (wall.thumb) {
            gp::GraphicsPath clip;
            art::roundRectPath(clip, t, dp(12));
            gr.SetClip(&clip);
            gr.SetInterpolationMode(gp::InterpolationModeHighQualityBicubic);
            gr.DrawImage(wall.thumb.get(), t);
            gr.ResetClip();
        }
        if (g.hover == id && can) {
            gp::Pen ring(gp::Color(C_ACCENT), dp(2));
            gp::GraphicsPath p;
            art::roundRectPath(p, t, dp(12));
            gr.DrawPath(&ring, &p);
        }
        text(gr, wall.name, gp::RectF(t.X, t.GetBottom() + dp(4), t.Width, dp(18)), *g.fSmall, C_DIM, gp::StringAlignmentCenter,
             gp::StringAlignmentCenter, false, true);
    }
    y += dp(24) + wh + dp(30);
    // Boot screen: off, one of the styles, or a different one each start.
    label(gr, i18n::text(L"BOOT SCREEN"), x, y, w);
    int bootSel = g.set.bootScreen ? g.set.bootStyle + 1 : 0;
    button(
        gr, gp::RectF(x + w - dp(110), y - dp(7), dp(110), dp(28)), i18n::text(L"Preview"), Btn::Ghost, !overlay::bootScreenVisible(),
        [] {
            int st = g.set.bootStyle < 5 ? g.set.bootStyle : static_cast<int>(GetTickCount64() / 1000 % art::bootStyleNames().size());
            overlay::previewBootScreen(st);
        },
        L"\xE768");
    std::vector<std::wstring> bootLabels = {i18n::text(L"Off")};
    for (auto *n : art::bootStyleNames())
        bootLabels.push_back(n);
    bootLabels.push_back(i18n::text(L"Random"));
    segments(
        gr, gp::RectF(x, y + dp(22), w, dp(36)), bootLabels, bootSel, [](int) { return true; },
        [](int i) {
            g.set.bootScreen = i > 0;
            if (i > 0)
                g.set.bootStyle = i - 1;
            save();
        });
    if (!running())
        text(gr, i18n::text(L"Start the phone, then click a wallpaper to apply it."), gp::RectF(x, y + dp(66), w, dp(18)), *g.fSmall,
             C_FAINT);
}

// A ring gauge with the value in the middle.
void gauge(gp::Graphics &gr, const gp::RectF &r, float value, const std::wstring &big, const std::wstring &name, const std::wstring &sub) {
    card(gr, r);
    float d = dp(96), cx = r.X + r.Width / 2, cy = r.Y + dp(14) + d / 2;
    gp::RectF ring(cx - d / 2, cy - d / 2, d, d);
    gp::Pen track(gp::Color(C_CHIP), dp(9));
    gr.DrawArc(&track, ring, 135, 270);
    if (value >= 0) {
        gp::LinearGradientBrush lg(gp::RectF(ring.X - 2, ring.Y - 2, ring.Width + 4, ring.Height + 4), gp::Color(C_ACCENT),
                                   gp::Color(C_ACCENT2), gp::LinearGradientModeHorizontal);
        gp::Pen arc(&lg, dp(9));
        arc.SetStartCap(gp::LineCapRound);
        arc.SetEndCap(gp::LineCapRound);
        float sweep = std::max(1.f, 270.f * std::clamp(value, 0.f, 100.f) / 100.f);
        gr.DrawArc(&arc, ring, 135, sweep);
    }
    text(gr, big, gp::RectF(ring.X, cy - dp(16), d, dp(28)), *g.fH2, C_TEXT, gp::StringAlignmentCenter);
    text(gr, name, gp::RectF(ring.X, cy + dp(12), d, dp(16)), *g.fSmallBold, C_FAINT, gp::StringAlignmentCenter);
    text(gr, sub, gp::RectF(r.X + dp(8), r.GetBottom() - dp(30), r.Width - dp(16), dp(18)), *g.fSmall, C_DIM, gp::StringAlignmentCenter);
}

void sparkline(gp::Graphics &gr, const gp::RectF &r, const std::vector<float> &v, float top) {
    if (v.size() < 2) {
        gp::Pen base(gp::Color(C_LINE), dp(1));
        gr.DrawLine(&base, r.X, r.GetBottom(), r.GetRight(), r.GetBottom());
        return;
    }
    std::vector<gp::PointF> pts;
    for (size_t i = 0; i < v.size(); i++)
        pts.push_back({r.X + r.Width * (60 - v.size() + i) / 59.f, r.GetBottom() - r.Height * std::clamp(v[i] / top, 0.f, 1.f)});
    gp::GraphicsPath area;
    area.AddLines(pts.data(), static_cast<INT>(pts.size()));
    area.AddLine(pts.back(), gp::PointF(pts.back().X, r.GetBottom()));
    area.AddLine(gp::PointF(pts.back().X, r.GetBottom()), gp::PointF(pts.front().X, r.GetBottom()));
    area.CloseFigure();
    gp::LinearGradientBrush fill(r, gp::Color(80, 0x8B, 0x6C, 0xFF), gp::Color(0, 0x3D, 0x7B, 0xFF), gp::LinearGradientModeVertical);
    gr.FillPath(&fill, &area);
    gp::Pen line(gp::Color(C_ACCENT), dp(1.8f));
    line.SetLineJoin(gp::LineJoinRound);
    gr.DrawLines(&line, pts.data(), static_cast<INT>(pts.size()));
}

std::wstring fmt1(const wchar_t *f, double v) {
    wchar_t buf[64];
    swprintf(buf, 64, f, v);
    return buf;
}

void drawSystem(gp::Graphics &gr) {
    const float x = X0(), w = CW();
    pageHeader(gr, i18n::text(L"System"), i18n::text(L"Live usage of this PC and your phone."));
    languagePicker(gr);
    sysmon::Sample s = sysmon::latest();
    std::vector<sysmon::Sample> hist = sysmon::history();
    const HostInfo &h = host();
    float y = dp(96), gw = (w - dp(16)) / 3, gh = dp(164);
    gauge(gr, gp::RectF(x, y, gw, gh), s.cpu, s.cpu >= 0 ? fmt1(L"%.0f%%", s.cpu) : L"–", L"CPU",
          h.cpu.empty() ? i18n::text(L"Processor") : h.cpu);
    std::wstring vram = s.vramUsedGB >= 0 && h.vramMB
                            ? fmt1(L"%.1f", s.vramUsedGB) + i18n::text(L" of ") + fmt1(L"%.0f GB VRAM", h.vramMB / 1024.0)
                            : h.gpu;
    gauge(gr, gp::RectF(x + gw + dp(8), y, gw, gh), s.gpu, s.gpu >= 0 ? fmt1(L"%.0f%%", s.gpu) : L"–", L"GPU", vram);
    float ramPct = s.ramTotalGB > 0 ? 100.f * s.ramUsedGB / s.ramTotalGB : -1;
    gauge(gr, gp::RectF(x + 2 * (gw + dp(8)), y, gw, gh), ramPct, ramPct >= 0 ? fmt1(L"%.0f%%", ramPct) : L"–", i18n::text(L"MEMORY"),
          s.ramTotalGB > 0 ? fmt1(L"%.1f", s.ramUsedGB) + i18n::text(L" of ") + fmt1(L"%.0f GB", s.ramTotalGB) : L"");
    y += gh + dp(12);

    // The phone's own share.
    gp::RectF pc(x, y, w, dp(104));
    card(gr, pc);
    art::logo(gr, pc.X + dp(16), pc.Y + dp(16), dp(30));
    text(gr, i18n::text(L"Phone"), gp::RectF(pc.X + dp(56), pc.Y + dp(14), dp(120), dp(20)), *g.fBold, C_TEXT);
    text(gr, running() ? i18n::text(L"Running") : i18n::text(L"Off"), gp::RectF(pc.X + dp(56), pc.Y + dp(32), dp(120), dp(18)), *g.fSmall,
         running() ? C_GOOD : C_FAINT);
    std::vector<float> pcpu, pgpu, pram, pfps;
    for (auto &e : hist) {
        pcpu.push_back(std::max(0.f, e.phoneCpu));
        pgpu.push_back(std::max(0.f, e.phoneGpu));
        pram.push_back(e.phoneRamGB);
    }
    {
        std::lock_guard<std::mutex> l(g.lock);
        for (int f : g.fps)
            pfps.push_back(static_cast<float>(f));
    }
    struct Col {
        const wchar_t *name;
        std::wstring value;
        const std::vector<float> *series;
        float top;
    };
    bool on = running();
    Col cols[] = {
        {L"CPU", on && s.phoneCpu >= 0 ? fmt1(L"%.0f%%", s.phoneCpu) : L"–", &pcpu, 100},
        {L"GPU", on && s.phoneGpu >= 0 ? fmt1(L"%.0f%%", s.phoneGpu) : L"–", &pgpu, 100},
        {i18n::text(L"MEMORY"), on ? fmt1(L"%.1f GB", s.phoneRamGB) : L"–", &pram,
         static_cast<float>(std::max(2, memoryFor(phone::running)))},
        {i18n::text(L"SCREEN FPS"), on && !pfps.empty() ? fmt1(L"%.0f", pfps.back()) : L"–", &pfps,
         static_cast<float>(std::max(60, phone::running.rate))},
    };
    float cx0 = pc.X + dp(170), colW = (pc.GetRight() - dp(16) - cx0) / 4;
    for (int i = 0; i < 4; i++) {
        float cxx = cx0 + i * colW;
        text(gr, cols[i].name, gp::RectF(cxx, pc.Y + dp(14), colW - dp(10), dp(16)), *g.fSmallBold, C_FAINT);
        text(gr, cols[i].value, gp::RectF(cxx, pc.Y + dp(30), colW - dp(10), dp(26)), *g.fH2, on ? C_TEXT : C_FAINT);
        sparkline(gr, gp::RectF(cxx, pc.Y + dp(62), colW - dp(14), dp(28)), on ? *cols[i].series : std::vector<float>{}, cols[i].top);
    }
    y += dp(116);

    // Specs sheet.
    sysmon::Specs sp = sysmon::specs();
    sp.display = std::to_wstring(h.monitorW) + L" × " + std::to_wstring(h.monitorH) + L"  ·  " + std::to_wstring(h.monitorHz) + L" Hz";
    gp::RectF sc(x, y, w, dp(600 - 96 - 164 - 12 - 116 - 8) - dp(2));
    card(gr, sc);
    std::pair<const wchar_t *, std::wstring> rows[] = {
        {i18n::text(L"Processor"), sp.cpu}, {i18n::text(L"Graphics"), sp.gpu},
        {i18n::text(L"Memory"), sp.memory}, {i18n::text(L"Display"), sp.display},
        {L"Windows", sp.windows},           {i18n::text(L"Virtualization"), sp.virtualization},
        {L"Android", sp.android},           {i18n::text(L"Emulator"), sp.emulator},
    };
    float rowH = (sc.Height - dp(16)) / 4, colW2 = (sc.Width - dp(24)) / 2;
    for (int i = 0; i < 8; i++) {
        float rx = sc.X + dp(16) + (i % 2) * (colW2 + dp(8)), ry = sc.Y + dp(8) + (i / 2) * rowH;
        text(gr, rows[i].first, gp::RectF(rx, ry, dp(92), rowH), *g.fSmall, C_FAINT);
        text(gr, rows[i].second, gp::RectF(rx + dp(92), ry, colW2 - dp(94), rowH), *g.fSmall, C_TEXT);
    }
}
void drawPhone(gp::Graphics &gr) {
    const float x = X0(), w = CW();
    pageHeader(gr, i18n::text(L"Phone"), i18n::text(L"Root, phone number and testing helpers."));
    button(gr, gp::RectF(x + w - dp(240), dp(28), dp(108), dp(34)), L"Velox Home", Btn::Secondary, running() && !g.busy,
           [] { tool([] { experience::openHome(reporter()); }); });
    button(gr, gp::RectF(x + w - dp(122), dp(28), dp(122), dp(34)), i18n::text(L"Velox Settings"), Btn::Secondary, running() && !g.busy,
           [] { tool([] { experience::openSettings(reporter()); }); });
    float y = dp(96);
    bool ready = rootReady();
    std::wstring rootSub = ready        ? i18n::text(L"Real root for testing. Banking apps and some games refuse rooted phones.")
                           : g.set.root ? i18n::text(L"Root image not found. Turn this off to start normally, or set up root again.")
                                        : i18n::text(L"Not set up on this PC yet.");
    toggle(
        gr, gp::RectF(x, y, w, dp(56)), L"Root (Magisk)", rootSub, g.set.root, ready || g.set.root,
        [] {
            g.set.root = !g.set.root;
            save();
        },
        L"\xEA18");
    y += dp(64);
    if (!ready) {
        bool can = running() && !phone::running.root && !g.busy;
        button(
            gr, gp::RectF(x, y, dp(200), dp(38)), i18n::text(L"Set up root"), Btn::Primary, can,
            [] { tool([] { setupRoot(reporter()); }); }, L"\xE7EF");
        text(gr,
             running() ? i18n::text(L"Downloads Magisk v31.1 (10 MB, checked) and patches a copy of the phone's boot image.")
                       : i18n::text(L"Start the phone first. This downloads Magisk v31.1 (10 MB) from GitHub."),
             gp::RectF(x + dp(214), y, w - dp(214), dp(38)), *g.fSmall, C_DIM);
        y += dp(52);
    }
    label(gr, i18n::text(L"PHONE NUMBER"), x, y + dp(4), w);
    card(gr, gp::RectF(x, y + dp(26), w, dp(42)));
    if (g.numberEdit) {
        placeField(g.numberEdit, static_cast<int>(x + dp(14)), static_cast<int>(y + dp(37)), static_cast<int>(w - dp(28)),
                   static_cast<int>(dp(22)));
    }
    y += dp(84);
    toggle(
        gr, gp::RectF(x, y, w, dp(56)), i18n::text(L"Show taps"),
        i18n::text(L"Shows where you touch the screen, handy for recordings and tutorials."), g.set.showTouches, true,
        [] {
            g.set.showTouches = !g.set.showTouches;
            save();
            applyLiveAsync();
        },
        L"\xE7C9");
    y += dp(72);
    card(gr, gp::RectF(x, y, w, dp(96)));
    text(gr, i18n::text(L"Phone files"), gp::RectF(x + dp(18), y + dp(12), w, dp(20)), *g.fBold, C_TEXT);
    text(gr, paths().avdDir.empty() ? i18n::text(L"Not installed") : paths().avdDir, gp::RectF(x + dp(18), y + dp(34), w - dp(36), dp(18)),
         *g.fSmall, C_DIM);
    text(gr, L"Android: " + (paths().sdk.empty() ? std::wstring(i18n::text(L"not found")) : paths().sdk),
         gp::RectF(x + dp(18), y + dp(52), w - dp(36), dp(18)), *g.fSmall, C_FAINT);
    if (!paths().avdDir.empty()) {
        button(
            gr, gp::RectF(x + w - dp(150), y + dp(12), dp(134), dp(30)), i18n::text(L"Open folder"), Btn::Ghost, true,
            [] { openFolder(paths().avdDir); }, L"\xE8B7");
    }
    y += dp(108);
    if (instances::id().empty()) {
        toggle(
            gr, gp::RectF(x, y, w, dp(56)), i18n::text(L"Start phone with Velox"),
            i18n::text(L"Open your main phone automatically when Velox opens."), g.set.autoStart, true,
            [] {
                g.set.autoStart = !g.set.autoStart;
                save();
            },
            L"\xE768");
        y += dp(64);
    }
    if (!host().hypervisor) {
        card(gr, gp::RectF(x, y, w, dp(60)));
        text(gr, i18n::text(L"Windows Hypervisor Platform is off, which the phone needs."), gp::RectF(x + dp(18), y, w - dp(200), dp(60)),
             *g.fText, C_WARN);
        button(gr, gp::RectF(x + w - dp(170), y + dp(11), dp(154), dp(38)), i18n::text(L"Turn it on"), Btn::Primary, true, [] {
            std::thread([] {
                bool ok = setup::enableHypervisor();
                say(ok ? i18n::text(L"Done. Restart the PC to finish turning it on.")
                       : i18n::text(L"Windows couldn't turn it on (or the prompt was declined)."),
                    !ok);
            }).detach();
        });
    }
}

void drawUpdates(gp::Graphics &gr) {
    const float x = X0(), w = CW();
    pageHeader(gr, i18n::text(L"Updates"), i18n::text(L"Velox keeps itself up to date."));
    if (!instances::id().empty()) {
        card(gr, gp::RectF(x, dp(96), w, dp(165)));
        text(gr, i18n::text(L"One Velox update for all your phones"), gp::RectF(x + dp(22), dp(113), w - dp(44), dp(32)), *g.fBig, C_TEXT);
        text(gr,
             i18n::text(L"Updates are managed from the main Velox window. Close the other phones' controls before installing an update."),
             gp::RectF(x + dp(22), dp(155), w - dp(44), dp(50)), *g.fText, C_DIM, gp::StringAlignmentNear, gp::StringAlignmentCenter, true);
        button(gr, gp::RectF(x + dp(22), dp(215), dp(185), dp(32)), i18n::text(L"Open main Velox"), Btn::Primary, true,
               [] { instances::command(L"", instances::Command::Open, reporter()); });
        return;
    }
    float y = dp(96);
    card(gr, gp::RectF(x, y, w, dp(92)));
    art::logo(gr, x + dp(18), y + dp(18), dp(56));
    text(gr, L"Velox " GP_VERSION_WSTRING, gp::RectF(x + dp(90), y + dp(18), w - dp(300), dp(26)), *g.fH2, C_TEXT);
    int us = g.updateState;
    std::wstring line = !update::configured() ? i18n::text(L"Updates aren't set up for this copy.")
                        : us == 1             ? i18n::text(L"Checking for updates…")
                        : us == 2             ? i18n::text(L"You have the latest version.")
                        : us == 3             ? i18n::text(L"Version ") + g.updateVersion + i18n::text(L" is available.")
                        : us == 4             ? i18n::text(L"Downloading ") + g.updateVersion + L"…  " +
                                        std::to_wstring(static_cast<int>(g.updateProgress * 100)) + L"%"
                        : us == 5 ? i18n::text(L"Version ") + g.updateVersion + i18n::text(L" is ready to install.")
                        : us == 6 ? i18n::text(L"Last check failed: ") + g.updateError
                                  : i18n::text(L"Checks automatically every few hours.");
    text(gr, line, gp::RectF(x + dp(90), y + dp(48), w - dp(300), dp(20)), *g.fSmall, us == 6 ? C_BAD : C_DIM);
    if (us == 5) {
        button(gr, gp::RectF(x + w - dp(196), y + dp(27), dp(180), dp(38)), i18n::text(L"Restart and update"), Btn::Primary, true,
               restartForUpdate);
    } else if (us == 3) {
        button(gr, gp::RectF(x + w - dp(196), y + dp(27), dp(180), dp(38)), i18n::text(L"Download update"), Btn::Primary, true,
               [] { checkForUpdates(true); });
    } else {
        button(
            gr, gp::RectF(x + w - dp(196), y + dp(27), dp(180), dp(38)), i18n::text(L"Check now"), Btn::Secondary,
            update::configured() && us != 1 && us != 4, [] { checkForUpdates(true); }, L"\xE72C");
    }
    y += dp(106);
    toggle(
        gr, gp::RectF(x, y, w, dp(56)), i18n::text(L"Update automatically"),
        i18n::text(L"Downloads new versions in the background; they're signed and checked first."), g.set.autoUpdate, update::configured(),
        [] {
            g.set.autoUpdate = !g.set.autoUpdate;
            save();
        },
        L"\xE895");
    y += dp(72);
    label(gr, i18n::text(L"WHAT'S NEW"), x, y, w);
    gp::RectF notes(x, y + dp(22), w, dp(210));
    card(gr, notes);
    // An update on its way shows what it brings; otherwise this version's changes.
    bool coming = !g.updateNotes.empty() && us >= 3 && us <= 5;
    std::vector<App::ChangeVersion> list = coming ? parseChanges(g.updateNotes, g.updateVersion) : g.changes;
    gr.SetClip(gp::RectF(notes.X, notes.Y + dp(4), notes.Width, notes.Height - dp(8)));
    float endY =
        drawChanges(gr, list, notes.X + dp(20), notes.Y + dp(16), notes.Width - dp(40), 1, coming ? i18n::text(L"Coming in ") : nullptr);
    gr.ResetClip();
    if (endY > notes.GetBottom()) {
        gp::LinearGradientBrush fade(gp::PointF(0, notes.GetBottom() - dp(56)), gp::PointF(0, notes.GetBottom() - dp(2)),
                                     gp::Color(0, 0x1A, 0x1D, 0x26), gp::Color(C_CARD));
        gr.FillRectangle(&fade, notes.X + dp(2), notes.GetBottom() - dp(56), notes.Width - dp(4), dp(54));
    }
    button(gr, gp::RectF(x + w - dp(150), y - dp(7), dp(150), dp(28)), i18n::text(L"Full changelog"), Btn::Ghost, true, showChangelog,
           L"\xE8A5");
    y += dp(244);
    text(gr, L"Android Emulator and Android 15 by Google (Android SDK License). Magisk by topjohnwu and rootAVD by newbit (GPL-3.0).",
         gp::RectF(x, y, w - dp(110), dp(34)), *g.fTiny, C_FAINT, gp::StringAlignmentNear, gp::StringAlignmentNear, true);
    button(gr, gp::RectF(x + w - dp(100), y - dp(4), dp(100), dp(28)), i18n::text(L"Open log"), Btn::Ghost, exists(paths().log),
           [] { ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + paths().log + L"\"").c_str(), nullptr, SW_SHOWNORMAL); });
}

void drawSetup(gp::Graphics &gr) {
    const float x = X0(), w = CW();
    bool havePhone = !paths().avdDir.empty();
    languagePicker(gr);
    pageHeader(gr, i18n::text(L"Set up your phone"),
               havePhone ? i18n::text(L"Your phone was found; only Android itself is missing. About 2.2 GB from Google.")
                         : i18n::text(L"A one-time download of Android from Google. About 2.2 GB."));
    auto chooseSdk = [] {
        BROWSEINFOW bi{};
        bi.hwndOwner = g.hwnd;
        bi.lpszTitle = i18n::text(L"Choose your Android SDK folder (the one with emulator and platform-tools)");
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        if (PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi)) {
            wchar_t path[MAX_PATH];
            if (SHGetPathFromIDListW(pidl, path)) {
                if (phone::useSdkFolder(path)) {
                    if (g.set.autoStart)
                        PostMessageW(g.hwnd, WM_APP_REFRESH, 1, 0);
                    else
                        say(i18n::text(L"Android SDK found. Press Start phone to play."));
                } else
                    say(i18n::text(L"That folder doesn't contain Google's Android emulator and platform tools."), true);
            }
            CoTaskMemFree(pidl);
        }
        refresh();
    };
    float y = dp(96);
    bool hv = host().hypervisor;
    std::wstring home = setup::home();
    int freeGb = setup::freeGB(home);
    bool space = freeGb >= 10;
    auto check = [&](bool ok, const std::wstring &title, const std::wstring &sub, float yy) {
        card(gr, gp::RectF(x, yy, w, dp(58)));
        text(gr, ok ? L"\xE73E" : L"\xE7BA", gp::RectF(x + dp(14), yy, dp(24), dp(58)), *g.fIconBig, ok ? C_GOOD : C_WARN,
             gp::StringAlignmentCenter);
        text(gr, title, gp::RectF(x + dp(50), yy + dp(10), w - dp(230), dp(20)), *g.fBold, C_TEXT);
        text(gr, sub, gp::RectF(x + dp(50), yy + dp(31), w - dp(230), dp(18)), *g.fSmall, C_DIM);
    };
    check(hv, i18n::text(L"Windows Hypervisor Platform"),
          hv ? i18n::text(L"On. The phone runs at full speed.") : i18n::text(L"Off. Needed to run the phone; a restart follows."), y);
    if (!hv) {
        button(gr, gp::RectF(x + w - dp(166), y + dp(10), dp(150), dp(38)), i18n::text(L"Turn it on"), Btn::Secondary, true, [] {
            std::thread([] {
                bool ok = setup::enableHypervisor();
                say(ok ? i18n::text(L"Done. Restart the PC, then open Velox again.")
                       : i18n::text(L"Windows couldn't turn it on (or the prompt was declined)."),
                    !ok);
            }).detach();
        });
    }
    y += dp(66);
    check(space, i18n::text(L"Install folder"), home + L"  ·  " + std::to_wstring(freeGb) + i18n::text(L" GB free, about 10 GB needed"), y);
    button(gr, gp::RectF(x + w - dp(166), y + dp(10), dp(150), dp(38)), i18n::text(L"Change…"), Btn::Secondary, !setup::progress.running,
           [] {
               BROWSEINFOW bi{};
               bi.hwndOwner = g.hwnd;
               bi.lpszTitle = i18n::text(L"Choose where Velox keeps Android and your phone");
               bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
               if (PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi)) {
                   wchar_t path[MAX_PATH];
                   if (SHGetPathFromIDListW(pidl, path)) {
                       std::wstring p = path;
                       if (!p.empty() && p.back() == L'\\')
                           p.pop_back();
                       setup::setHome(p + L"\\Velox");
                   }
                   CoTaskMemFree(pidl);
               }
               refresh();
           });
    y += dp(76);

    int id = addHit(gp::RectF(x, y, dp(30), dp(30)), !setup::progress.running, [] { g.licenseAccepted = !g.licenseAccepted; });
    roundFill(gr, gp::RectF(x + dp(4), y + dp(4), dp(22), dp(22)), dp(6),
              g.licenseAccepted ? C_ACCENT : (g.hover == id ? C_CHIP_HOVER : C_CHIP));
    if (g.licenseAccepted)
        text(gr, L"\xE73E", gp::RectF(x + dp(4), y + dp(4), dp(22), dp(22)), *g.fIcon, C_TEXT, gp::StringAlignmentCenter);
    text(gr, i18n::text(L"I accept the Android SDK License Agreement from Google."), gp::RectF(x + dp(38), y, w - dp(200), dp(30)),
         *g.fText, C_TEXT);
    button(gr, gp::RectF(x + w - dp(166), y - dp(2), dp(150), dp(34)), i18n::text(L"Read license"), Btn::Ghost, true, [] {
        say(i18n::text(L"Getting the license from Google…"));
        std::thread([] {
            std::wstring text, err;
            if (setup::fetchLicense(text, err)) {
                auto *copy = new std::wstring(text);
                PostMessageW(g.hwnd, WM_APP_REFRESH, 2, reinterpret_cast<LPARAM>(copy));
                say(L"");
            } else {
                say(i18n::text(L"Couldn't get the license: ") + err + L". It's also at developer.android.com/studio/terms", true);
            }
        }).detach();
    });
    y += dp(46);
    text(gr,
         i18n::text(L"Everything comes straight from dl.google.com and is checked against its official checksum. "
                    L"Nothing is sent anywhere, and the launcher never needs admin rights for this."),
         gp::RectF(x, y, w, dp(40)), *g.fSmall, C_DIM, gp::StringAlignmentNear, gp::StringAlignmentNear, true);
    y += dp(56);

    if (setup::progress.running) {
        gp::RectF bar(x, y + dp(8), w, dp(8));
        roundFill(gr, bar, dp(4), C_CHIP);
        gradientFill(gr, gp::RectF(bar.X, bar.Y, std::max(dp(8), bar.Width * setup::progress.fraction), bar.Height), dp(4));
        text(gr, setup::progress.text(), gp::RectF(x, y + dp(24), w - dp(130), dp(20)), *g.fText, C_DIM);
        button(gr, gp::RectF(x + w - dp(120), y + dp(22), dp(120), dp(34)), i18n::text(L"Cancel"), Btn::Secondary, true,
               [] { setup::progress.cancel = true; });
    } else {
        bool can = hv && space && g.licenseAccepted;
        button(
            gr, gp::RectF(x, y, dp(260), dp(48)), i18n::text(L"Download and set up"), Btn::Primary, can,
            [] {
                setup::start();
                SetTimer(g.hwnd, TIMER_ANIM, 200, nullptr);
            },
            L"\xE896");
        button(gr, gp::RectF(x + dp(272), y, w - dp(272), dp(48)), i18n::text(L"Use existing Android SDK…"), Btn::Secondary, true,
               chooseSdk);
        if (setup::progress.failed) {
            std::lock_guard<std::mutex> l(setup::progress.lock);
            text(gr, setup::progress.error, gp::RectF(x, y + dp(56), w, dp(40)), *g.fSmall, C_BAD, gp::StringAlignmentNear,
                 gp::StringAlignmentNear, true);
        }
    }
}

void drawSidebar(gp::Graphics &gr, float H) {
    art::logo(gr, dp(20), dp(22), dp(40));
    text(gr, L"Velox", gp::RectF(dp(70), dp(20), dp(150), dp(24)), *g.fBig, C_TEXT);
    gp::RectF chip(dp(70), dp(45), dp(60), dp(18));
    int vid = addHit(chip, true, showChangelog);
    roundFill(gr, chip, dp(9), g.hover == vid ? C_CHIP_HOVER : C_CHIP);
    text(gr, L"v" GP_VERSION_WSTRING, chip, *g.fTiny, C_DIM, gp::StringAlignmentCenter);
    if (g.updateState == 5 || g.updateState == 3)
        gr.FillEllipse(&solid(C_GOOD), chip.GetRight() + dp(6), chip.Y + dp(5), dp(8), dp(8));

    bool setupMode = !installed();
    float y = dp(88);
    for (int p = 0; p < PAGE_COUNT; p++) {
        gp::RectF r(dp(12), y, dp(SIDE_W - 24), dp(31));
        bool en = !setupMode || p == UPDATES || (p == RECOVERY && !paths().sdk.empty());
        bool sel = g.page == p && en;
        int id = addHit(r, en, [p] {
            g.page = static_cast<Page>(p);
            ShowWindow(g.numberEdit, p == PHONE ? SW_SHOW : SW_HIDE);
            ShowWindow(g.searchEdit, p == LIBRARY ? SW_SHOW : SW_HIDE);
            if (p == LIBRARY)
                experience::requestLibrary();
            sysmon::setActive(p == SYSTEM);
        });
        if (sel) {
            roundFill(gr, r, dp(10), C_CHIP);
            gradientFill(gr, gp::RectF(r.X, r.Y + dp(10), dp(3), dp(20)), dp(1.5f));
        } else if (g.hover == id && en) {
            roundFill(gr, r, dp(10), 0xFF1C1F28);
        }
        gp::ARGB fg = !en ? 0xFF3E4350 : sel ? C_TEXT : C_DIM;
        text(gr, PAGE_ICONS[p], gp::RectF(r.X + dp(12), r.Y, dp(20), r.Height), *g.fIcon, sel ? C_ACCENT : fg, gp::StringAlignmentCenter);
        text(gr, PAGE_NAMES[p], gp::RectF(r.X + dp(44), r.Y, r.Width - dp(50), r.Height), sel ? *g.fBold : *g.fText, fg);
        if (p == UPDATES && (g.updateState == 5 || g.updateState == 3))
            gr.FillEllipse(&solid(C_GOOD), r.GetRight() - dp(20), r.Y + dp(16), dp(8), dp(8));
        y += dp(34);
    }

    // Status and the main button at the bottom.
    State st = phone::state;
    float by = H - dp(150);
    gp::RectF status(dp(12), by, dp(SIDE_W - 24), dp(56));
    roundFill(gr, status, dp(12), C_CARD);
    gp::ARGB dot = st == State::Running ? C_GOOD : st == State::Stopped ? C_FAINT : C_WARN;
    gr.FillEllipse(&solid(dot), status.X + dp(14), status.Y + dp(15), dp(9), dp(9));
    std::wstring s1 = setupMode               ? i18n::text(L"Not set up")
                      : st == State::Running  ? i18n::text(L"Running")
                      : st == State::Starting ? i18n::text(L"Starting")
                      : st == State::Stopping ? i18n::text(L"Closing")
                                              : i18n::text(L"Off");
    text(gr, s1, gp::RectF(status.X + dp(30), status.Y + dp(8), status.Width - dp(36), dp(22)), *g.fBold, C_TEXT);
    std::wstring s2 = st == State::Running    ? (needsRestart() ? i18n::text(L"Restart to apply changes")
                                                                : std::wstring(displays()[phone::running.display].name) + L"  ·  " +
                                                                   std::to_wstring(phone::running.rate) + L" Hz")
                      : st == State::Starting ? phone::boot.text()
                      : setupMode             ? i18n::text(L"Download Android first")
                                              : i18n::text(L"Press Start to play");
    text(gr, s2, gp::RectF(status.X + dp(14), status.Y + dp(30), status.Width - dp(20), dp(18)), *g.fSmall,
         needsRestart() ? C_WARN : C_DIM);
    if (st == State::Starting) {
        gp::RectF bar(status.X + dp(14), status.GetBottom() - dp(6), status.Width - dp(28), dp(3));
        roundFill(gr, bar, dp(1.5f), C_CHIP);
        gradientFill(gr, gp::RectF(bar.X, bar.Y, std::max(dp(3), bar.Width * phone::boot.progress), bar.Height), dp(1.5f));
    }
    gp::RectF main(dp(12), H - dp(84), dp(SIDE_W - 24), dp(48));
    if (setupMode) {
        button(gr, main, i18n::text(L"Start phone"), Btn::Primary, false, [] {}, L"\xE768");
    } else if (st == State::Running && needsRestart()) {
        float half = (main.Width - dp(8)) * 0.62f;
        button(gr, gp::RectF(main.X, main.Y, half, main.Height), i18n::text(L"Restart"), Btn::Primary, true, restartToApply, L"\xE72C");
        button(gr, gp::RectF(main.X + half + dp(8), main.Y, main.Width - half - dp(8), main.Height), i18n::text(L"Stop"), Btn::Secondary,
               true, [] { stopPhone(false); });
    } else if (st == State::Running) {
        button(gr, main, i18n::text(L"Stop phone"), Btn::Secondary, true, [] { stopPhone(false); }, L"\xE71A");
    } else {
        const wchar_t *lbl = st == State::Starting   ? i18n::text(L"Starting…")
                             : st == State::Stopping ? i18n::text(L"Closing…")
                                                     : i18n::text(L"Start phone");
        button(gr, main, lbl, Btn::Primary, st == State::Stopped, startPhone, L"\xE768");
    }
}

void buildBackground(int cw, int ch) {
    g.background = std::make_unique<gp::Bitmap>(cw, ch, PixelFormat32bppPARGB);
    gp::Graphics gr(g.background.get());
    gr.Clear(gp::Color(C_BG));
    gr.FillRectangle(&solid(C_SIDE), 0.f, 0.f, dp(SIDE_W), static_cast<float>(ch));
    // Soft colour in the corners, like the wallpapers.
    auto glow = [&](float cx, float cy, float rx, float ry, gp::ARGB c, int a) {
        gp::GraphicsPath p;
        p.AddEllipse(cx - rx, cy - ry, 2 * rx, 2 * ry);
        gp::PathGradientBrush b(&p);
        gp::Color k(c);
        b.SetCenterColor(gp::Color(static_cast<BYTE>(a), k.GetR(), k.GetG(), k.GetB()));
        gp::Color e(0, k.GetR(), k.GetG(), k.GetB());
        int n = 1;
        b.SetSurroundColors(&e, &n);
        gr.FillPath(&b, &p);
    };
    gr.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    glow(dp(40), dp(10), dp(260), dp(180), C_ACCENT, 40);
    glow(static_cast<float>(cw) - dp(60), dp(0), dp(420), dp(200), C_ACCENT2, 34);
    glow(static_cast<float>(cw) - dp(200), static_cast<float>(ch), dp(360), dp(160), C_ACCENT, 22);
    gp::Pen line(gp::Color(C_LINE), 1.f);
    gr.DrawLine(&line, dp(SIDE_W), 0.f, dp(SIDE_W), static_cast<float>(ch));
}

void paint(HDC dc, int cw, int ch) {
    gp::Graphics gr(dc);
    gr.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    gr.SetTextRenderingHint(gp::TextRenderingHintClearTypeGridFit);
    gr.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
    if (!g.background || static_cast<int>(g.background->GetWidth()) != cw || static_cast<int>(g.background->GetHeight()) != ch)
        buildBackground(cw, ch);
    gr.DrawImage(g.background.get(), 0, 0, cw, ch);
    if (g.searchEdit && IsWindowVisible(g.searchEdit) != (g.page == LIBRARY && installed()))
        ShowWindow(g.searchEdit, g.page == LIBRARY && installed() ? SW_SHOW : SW_HIDE);
    if (g.numberEdit && IsWindowVisible(g.numberEdit) != (g.page == PHONE && installed()))
        ShowWindow(g.numberEdit, g.page == PHONE && installed() ? SW_SHOW : SW_HIDE);
    g.hits.clear();
    g.nextId = 0;
    drawSidebar(gr, static_cast<float>(ch));
    if (!installed() && g.page != UPDATES && !(g.page == RECOVERY && !paths().sdk.empty()))
        drawSetup(gr);
    else {
        switch (g.page) {
        case HOME:
            drawHome(gr);
            break;
        case LIBRARY:
            drawLibrary(gr);
            break;
        case INSTANCES:
            drawInstances(gr);
            break;
        case CONTROLS:
            drawControls(gr);
            break;
        case DISPLAY:
            drawDisplay(gr);
            break;
        case PERFORMANCE:
            drawPerformance(gr);
            break;
        case LOOK:
            drawLook(gr);
            break;
        case PHONE:
            drawPhone(gr);
            break;
        case RECOVERY:
            drawRecovery(gr);
            break;
        case SYSTEM:
            drawSystem(gr);
            break;
        default:
            drawUpdates(gr);
            break;
        }
    }
    std::wstring msg;
    bool err;
    {
        std::lock_guard<std::mutex> l(g.lock);
        msg = g.message;
        err = g.messageError;
    }
    if (msg.empty() && g.page == PERFORMANCE)
        msg = i18n::text(L"Unavailable choices are greyed out. Memory limits leave room for Windows.");
    if (msg.empty() && running() && g.page == HOME)
        msg = i18n::text(L"Tip: drop APKs, app bundles or other files here to put them on the phone.");
    if (!msg.empty()) {
        gp::RectF bar(X0(), static_cast<float>(ch) - dp(40), CW(), dp(28));
        text(gr, err ? L"\xE7BA" : L"\xE946", gp::RectF(bar.X, bar.Y, dp(18), bar.Height), *g.fIcon, err ? C_BAD : C_FAINT,
             gp::StringAlignmentCenter);
        text(gr, msg, gp::RectF(bar.X + dp(26), bar.Y, bar.Width - dp(26), bar.Height), *g.fSmall, err ? C_BAD : C_DIM);
    }
}

// ------------------------------------------------------------------------------------------
// Background work: FPS and the live preview

void monitorLoop() {
    ULONGLONG lastPreview = 0;
    for (;;) {
        Sleep(1000);
        bool visible = g.hwnd && IsWindowVisible(g.hwnd) && !IsIconic(g.hwnd);
        if (visible && g.page == SYSTEM)
            refresh(); // live gauges, also while the phone is off
        if (!running() || !visible) {
            if (!running()) {
                std::lock_guard<std::mutex> l(g.lock);
                g.fps.clear();
            }
            continue;
        }
        if (g.page == HOME || g.page == SYSTEM) {
            int f = sampleFps();
            if (f >= 0) {
                std::lock_guard<std::mutex> l(g.lock);
                g.fps.push_back(f);
                while (g.fps.size() > 60)
                    g.fps.pop_front();
            }
        }
        ULONGLONG now = GetTickCount64();
        if (g.page == HOME && GetForegroundWindow() == g.hwnd && now - lastPreview > 4000 && !g.busy) {
            lastPreview = now;
            std::string png = adb(L"exec-out screencap -p", 8000);
            if (png.size() > 1000) {
                IStream *stream = SHCreateMemStream(reinterpret_cast<const BYTE *>(png.data()), static_cast<UINT>(png.size()));
                if (stream) {
                    gp::Bitmap full(stream);
                    if (full.GetLastStatus() == gp::Ok && full.GetWidth() > 0) {
                        int tw = 360, th = static_cast<int>(360.0 * full.GetHeight() / full.GetWidth());
                        auto thumb = std::make_unique<gp::Bitmap>(tw, th, PixelFormat32bppARGB);
                        {
                            gp::Graphics tg(thumb.get());
                            tg.SetInterpolationMode(gp::InterpolationModeHighQualityBicubic);
                            tg.DrawImage(&full, 0, 0, tw, th);
                        }
                        art::savePng(*thumb, dataFolder() + L"\\preview.png");
                        std::lock_guard<std::mutex> l(g.lock);
                        g.preview = std::move(thumb);
                    }
                    stream->Release();
                }
            }
        }
        refresh();
    }
}

// ------------------------------------------------------------------------------------------
// Window

void createFonts() {
    const wchar_t *face = L"Segoe UI";
    g.fTitle = std::make_unique<gp::Font>(face, dp(24), gp::FontStyleBold, gp::UnitPixel);
    g.fH2 = std::make_unique<gp::Font>(face, dp(19), gp::FontStyleBold, gp::UnitPixel);
    g.fBig = std::make_unique<gp::Font>(face, dp(16.5f), gp::FontStyleBold, gp::UnitPixel);
    g.fText = std::make_unique<gp::Font>(face, dp(13.5f), gp::FontStyleRegular, gp::UnitPixel);
    g.fBold = std::make_unique<gp::Font>(face, dp(13.5f), gp::FontStyleBold, gp::UnitPixel);
    g.fSmall = std::make_unique<gp::Font>(face, dp(12), gp::FontStyleRegular, gp::UnitPixel);
    g.fSmallBold = std::make_unique<gp::Font>(face, dp(11), gp::FontStyleBold, gp::UnitPixel);
    g.fTiny = std::make_unique<gp::Font>(face, dp(10.5f), gp::FontStyleRegular, gp::UnitPixel);
    g.fHuge = std::make_unique<gp::Font>(face, dp(38), gp::FontStyleBold, gp::UnitPixel);
    gp::FontFamily fluent(L"Segoe Fluent Icons");
    const wchar_t *iconFace = fluent.IsAvailable() ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    g.fIcon = std::make_unique<gp::Font>(iconFace, dp(15), gp::FontStyleRegular, gp::UnitPixel);
    g.fIconBig = std::make_unique<gp::Font>(iconFace, dp(20), gp::FontStyleRegular, gp::UnitPixel);
    if (g.editFont)
        DeleteObject(g.editFont);
    g.editFont = CreateFontW(-static_cast<int>(dp(14)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    if (g.numberEdit)
        SendMessageW(g.numberEdit, WM_SETFONT, reinterpret_cast<WPARAM>(g.editFont), TRUE);
    if (g.searchEdit)
        SendMessageW(g.searchEdit, WM_SETFONT, reinterpret_cast<WPARAM>(g.editFont), TRUE);
}

void loadThumbs() {
    loadWalls();
    std::wstring prev = dataFolder() + L"\\preview.png";
    if (exists(prev)) {
        gp::Bitmap file(prev.c_str());
        if (file.GetLastStatus() == gp::Ok) {
            // Copy, so the file isn't kept open.
            auto copy = std::make_unique<gp::Bitmap>(file.GetWidth(), file.GetHeight(), PixelFormat32bppARGB);
            gp::Graphics cg(copy.get());
            cg.DrawImage(&file, 0, 0, file.GetWidth(), file.GetHeight());
            g.preview = std::move(copy);
        }
    }
}

// Turns "2.0.0 | 2026-11-04" / "[New]" / "- text" lines into versions. Update notes from a
// manifest use "New: text" lines (or plain bullets) and come through the same way.
std::vector<App::ChangeVersion> parseChanges(const std::wstring &textIn, const std::wstring &defaultVersion) {
    std::vector<App::ChangeVersion> out;
    int kind = 0;
    size_t pos = 0;
    while (pos <= textIn.size()) {
        size_t end = textIn.find(L'\n', pos);
        if (end == std::wstring::npos)
            end = textIn.size();
        std::wstring line = textIn.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' '))
            line.pop_back();
        while (!line.empty() && line.front() == L' ')
            line.erase(0, 1);
        if (line.empty() || line[0] == L'#')
            continue;
        if (iswdigit(line[0])) {
            App::ChangeVersion v;
            size_t bar = line.find(L'|');
            v.version = line.substr(0, bar);
            while (!v.version.empty() && v.version.back() == L' ')
                v.version.pop_back();
            if (bar != std::wstring::npos) {
                v.date = line.substr(bar + 1);
                while (!v.date.empty() && v.date.front() == L' ')
                    v.date.erase(0, 1);
            }
            out.push_back(std::move(v));
            kind = 0;
            continue;
        }
        if (line == L"[New]") {
            kind = 0;
            continue;
        }
        if (line == L"[Improved]") {
            kind = 1;
            continue;
        }
        if (line == L"[Fixed]") {
            kind = 2;
            continue;
        }
        int k = kind;
        for (auto [prefix, pk] : {std::pair{L"New: ", 0}, std::pair{L"Improved: ", 1}, std::pair{L"Fixed: ", 2}}) {
            if (line.rfind(prefix, 0) == 0) {
                k = pk;
                line = line.substr(wcslen(prefix));
            }
        }
        for (const wchar_t *bullet : {L"- ", L"•  ", L"• "})
            if (line.rfind(bullet, 0) == 0)
                line = line.substr(wcslen(bullet));
        if (out.empty())
            out.push_back({defaultVersion, L"", {}});
        out.back().items.push_back({k, line});
    }
    return out;
}

void loadChangelog() {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(202), RT_RCDATA);
    HGLOBAL data = res ? LoadResource(nullptr, res) : nullptr;
    if (!data)
        return;
    std::string s(static_cast<const char *>(LockResource(data)), SizeofResource(nullptr, res));
    g.changes = parseChanges(widen(s));
}

void resizeToContent() {
    RECT rc{0, 0, static_cast<LONG>(dp(WIN_W)), static_cast<LONG>(dp(WIN_H))};
    AdjustWindowRectExForDpi(&rc, GetWindowLongW(g.hwnd, GWL_STYLE), FALSE, 0, static_cast<UINT>(g.s * 96));
    SetWindowPos(g.hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

int hitAt(POINT p) {
    for (auto it = g.hits.rbegin(); it != g.hits.rend(); ++it)
        if (it->r.Contains(static_cast<float>(p.x), static_cast<float>(p.y)))
            return it->id;
    return -1;
}

const Hit *hitById(int id) {
    for (auto &h : g.hits)
        if (h.id == id)
            return &h;
    return nullptr;
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g.hwnd = hwnd;
        g.s = GetDpiForWindow(hwnd) / 96.f;
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
        COLORREF caption = RGB(0x13, 0x15, 0x1C);
        DwmSetWindowAttribute(hwnd, 35, &caption, sizeof(caption));
        g.editBrush = CreateSolidBrush(RGB(0x1A, 0x1D, 0x26));
        g.numberEdit = CreateWindowExW(0, L"EDIT", g.set.number.c_str(), WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_NUMBER)), nullptr, nullptr);
        SendMessageW(g.numberEdit, EM_LIMITTEXT, 20, 0);

        g.searchEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SEARCH)), nullptr, nullptr);
        SendMessageW(g.searchEdit, EM_LIMITTEXT, 160, 0);
        for (HWND field : {g.numberEdit, g.searchEdit}) {
            SetWindowTheme(field, L"", L"");
            SendMessageW(field, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
            SetWindowSubclass(field, fieldProc, 1, 0);
        }
        ShowWindow(g.searchEdit, g.page == LIBRARY ? SW_SHOW : SW_HIDE);
        createFonts();
        DragAcceptFiles(hwnd, TRUE);
        resizeToContent();
        SetTimer(hwnd, TIMER_WATCH, 500, nullptr);
        SetTimer(hwnd, TIMER_CURSOR, 30, nullptr);
        playtools::initialize(hwnd);
        return 0;
    }
    case WM_EXITSIZEMOVE:
    case WM_DISPLAYCHANGE:
        refreshHost(hwnd);
        refresh();
        return 0;
    case WM_DPICHANGED: {
        g.s = HIWORD(wp) / 96.f;
        createFonts();
        g.background.reset();
        auto *r = reinterpret_cast<RECT *>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        resizeToContent();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, RGB(0xF2, 0xF3, 0xF7));
        SetBkColor(dc, RGB(0x1A, 0x1D, 0x26));
        return reinterpret_cast<LRESULT>(g.editBrush);
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_SEARCH && HIWORD(wp) == EN_CHANGE) {
            wchar_t search[200];
            GetWindowTextW(g.searchEdit, search, 200);
            g.librarySearch = search;
            g.libraryPage = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (LOWORD(wp) == ID_NUMBER && HIWORD(wp) == EN_CHANGE) {
            wchar_t buf[32] = L"";
            GetWindowTextW(g.numberEdit, buf, 32);
            std::wstring clean;
            for (const wchar_t *c = buf; *c; c++)
                if ((*c >= L'0' && *c <= L'9') || (*c == L'+' && clean.empty()))
                    clean += *c;
            if (std::wstring(buf) != clean) {
                DWORD start = 0, end = 0;
                SendMessageW(g.numberEdit, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
                SetWindowTextW(g.numberEdit, clean.c_str());
                SendMessageW(g.numberEdit, EM_SETSEL, std::min<size_t>(start, clean.size()), std::min<size_t>(end, clean.size()));
            }
            if (clean != g.set.number) {
                g.set.number = clean;
                save();
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_TIMER:
        if (wp == TIMER_CURSOR) {
            if (g.closeAfterVault && !vault::progress.running && !g.busy) {
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
                return 0;
            }
            if (g.page == RECOVERY && vault::progress.running)
                InvalidateRect(hwnd, nullptr, FALSE);
            playtools::tick();
            if (g.set.cursor > 0 && running())
                overlay::tickCursor();
            else
                overlay::restoreCursor();
            return 0;
        }
        if (wp == TIMER_ANIM) {
            if (!setup::progress.running) {
                KillTimer(hwnd, TIMER_ANIM);
                if (setup::progress.done) {
                    findPaths();
                    if (g.set.autoStart)
                        PostMessageW(hwnd, WM_APP_REFRESH, 1, 0);
                    else
                        say(i18n::text(L"Your phone is set up. Press Start phone to play."));
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wp == TIMER_WATCH) {
            syncRecordingState();
            State st = phone::state;
            SetPropW(hwnd, L"VeloxPhoneState", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(st) + 1));
            if (g.page == INSTANCES)
                refreshInstances();
            if (g.exitAfterStop && st == State::Stopped && !g.busy) {
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
                return 0;
            }
            bool alive = emulatorRunning();
            if (!g.pendingGame.empty()) {
                if (st == State::Running && !g.busy) {
                    auto component = g.pendingGame;
                    bool edit = g.pendingEditor;
                    g.pendingGame.clear();
                    launchGame(component, edit);
                } else if (st == State::Stopped && !g.pendingSettings && GetTickCount64() - g.pendingSince > 30000) {
                    g.pendingGame.clear();
                }
            }
            if (st == State::Running && !alive) {
                phone::onExitedOutside();
                say(i18n::text(L"The phone was closed."));
            } else if (st == State::Stopped && alive) {
                phone::state = State::Running; // started some other way, or by an earlier Velox
                if (!phone::adoptRunning()) {
                    phone::distrustSnapshot();
                    phone::running = g.set;
                }
            }
            {
                std::lock_guard<std::mutex> l(g.lock);
                if (!g.message.empty() && !g.messageError && GetTickCount64() - g.messageTime > 12000 && idle())
                    g.message.clear();
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case instances::CONTROL: {
        auto action = static_cast<instances::Command>(wp);
        if (action == instances::Command::Start)
            startPhone();
        else if (action == instances::Command::Stop) {
            if (idle() && running())
                stopPhone(false);
        } else if (action == instances::Command::CloseControls)
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return 0;
    }
    case WM_APP + 56:
        refreshInstances();
        refresh();
        return 0;
    case playtools::ACTION_MESSAGE: {
        using namespace playtools;
        auto action = static_cast<Action>(wp);
        if (action == FocusPhone) {
            auto preset = presets::load(g.activeGame);
            if (preset.enabled && preset.fullscreen >= 0)
                playtools::setFullscreen(preset.fullscreen == 1);
            playtools::focusPhone();
            return 0;
        }
        if (action == ScreenResolution) {
            g.page = DISPLAY;
            ShowWindow(g.searchEdit, SW_HIDE);
            ShowWindow(g.numberEdit, SW_HIDE);
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
        } else if (action == Library) {
            g.page = LIBRARY;
            ShowWindow(g.searchEdit, SW_SHOW);
            ShowWindow(g.numberEdit, SW_HIDE);
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
            experience::requestLibrary();
        } else if (action == Editor) {
            playtools::edit();
        } else if (action == EnableMapping) {
            playtools::enabled(!playtools::profile().enabled);
        } else if (action == Fullscreen) {
            playtools::toggleFullscreen();
        } else if (action == RotationLock) {
            g.set.autoRotate = !g.set.autoRotate;
            setAutoRotation(g.set.autoRotate);
            save();
            say(g.set.autoRotate ? i18n::text(L"Automatic rotation on.") : i18n::text(L"Rotation locked."));
        } else if (running()) {
            if (action == Back || action == Home || action == Recents || action == playtools::Settings || action == Power)
                playtools::release();
            tool([action] {
                switch (action) {
                case Back:
                    adb(L"shell input keyevent KEYCODE_BACK");
                    break;
                case Home:
                    adb(L"shell input keyevent KEYCODE_HOME");
                    break;
                case Recents:
                    adb(L"shell input keyevent KEYCODE_APP_SWITCH");
                    break;
                case VolumeUp:
                    adb(L"shell input keyevent KEYCODE_VOLUME_UP");
                    break;
                case VolumeDown:
                    adb(L"shell input keyevent KEYCODE_VOLUME_DOWN");
                    break;
                case Rotate:
                    phone::rotate();
                    break;
                case Screenshot:
                    phone::screenshot(reporter());
                    break;
                case Record:
                    phone::toggleRecording(reporter());
                    break;
                case playtools::Settings:
                    experience::openSettings(reporter());
                    break;
                case Power:
                    adb(L"shell input keyevent KEYCODE_POWER");
                    break;
                default:
                    break;
                }
            });
        }
        refresh();
        return 0;
    }
    case WM_APP + 41: {
        std::unique_ptr<experience::GuestPreferences> prefs(reinterpret_cast<experience::GuestPreferences *>(lp));
        int currentNavigation = g.set.gestureNavigation ? 2 : g.set.reverseButtons ? 1 : 0;
        if (g.set.autoRotate == prefs->rotate && currentNavigation == prefs->navigation && g.set.language == prefs->language)
            return 0;
        g.set.language = prefs->language;
        i18n::language(g.set.language);
        g.set.autoRotate = prefs->rotate;
        g.set.gestureNavigation = prefs->navigation == 2;
        if (prefs->navigation != 2)
            g.set.reverseButtons = prefs->navigation == 1;
        setAutoRotation(g.set.autoRotate);
        setGestureNavigation(g.set.gestureNavigation, g.set.reverseButtons);
        save();
        say(i18n::text(L"Settings updated from your phone."));
        return 0;
    }
    case studio::CHANGED:
        refresh();
        return 0;
    case WM_APP + 45:
        refreshVault();
        refresh();
        return 0;
    case WM_APP + 46: {
        std::unique_ptr<std::wstring> prepared(reinterpret_cast<std::wstring *>(lp));
        if (!g.closeAfterVault && vault::activate(*prepared, reporter()))
            reloadRecovery();
        return 0;
    }
    case WM_APP_REFRESH:
        if (wp == 1)
            startPhone(); // after a restart-to-apply stop
        if (wp == 3 && running() &&
            (g.set.boost != phone::running.boost || g.set.snappy != phone::running.snappy ||
             g.set.showTouches != phone::running.showTouches || g.set.autoRotate != phone::running.autoRotate))
            applyLiveAsync();
        if (wp == 2) {
            std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring *>(lp));
            showText(L"Android SDK License Agreement", *text);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        paint(mem, rc.right, rc.bottom);
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        // The hover may now be over a different widget than before this frame.
        POINT p;
        GetCursorPos(&p);
        ScreenToClient(hwnd, &p);
        int h = hitAt(p);
        if (h != g.hover && GetCapture() != hwnd) {
            g.hover = h;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        int h = hitAt({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (h != g.hover) {
            g.hover = h;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        g.hover = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            const Hit *h = hitById(hitAt(p));
            SetCursor(LoadCursor(nullptr, h && h->enabled ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        g.pressed = hitAt({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        ReleaseCapture();
        int h = hitAt({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        int p = g.pressed;
        g.pressed = -1;
        if (h >= 0 && h == p) {
            const Hit *hit = hitById(h);
            if (hit && hit->enabled) {
                auto act = hit->act; // the list is rebuilt by the next paint
                act();
            }
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_RBUTTONUP: {
        if (g.page != LIBRARY)
            break;
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        for (auto &item : g.libraryCards)
            if (item.first.Contains(static_cast<float>(point.x), static_cast<float>(point.y))) {
                auto app = item.second;
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, 1, i18n::text(L"Play"));
                AppendMenuW(menu, MF_STRING, 2, i18n::text(L"Edit controls"));
                AppendMenuW(menu, MF_STRING, 5, i18n::text(L"Game preset…"));
                AppendMenuW(menu, MF_STRING | (running() ? 0 : MF_GRAYED), 3,
                            app.favorite ? i18n::text(L"Remove from favorites") : i18n::text(L"Add to favorites"));
                AppendMenuW(menu, MF_STRING | (running() ? 0 : MF_GRAYED), 4,
                            app.game ? i18n::text(L"Remove from Games") : i18n::text(L"Mark as game"));
                ClientToScreen(hwnd, &point);
                int action = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, point.x, point.y, 0, hwnd, nullptr);
                DestroyMenu(menu);
                if (action == 5)
                    studio::game(g.hwnd, app, g.set);
                if (action == 1)
                    launchGame(app.component);
                if (action == 2) {
                    g.page = CONTROLS;
                    launchGame(app.component, true);
                }
                if (action == 3)
                    tool([app] { experience::favorite(app, reporter()); });
                if (action == 4)
                    tool([app] { experience::classify(app, reporter()); });
                refresh();
                return 0;
            }
        break;
    }
    case WM_DROPFILES: {
        auto drop = reinterpret_cast<HDROP>(wp);
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::wstring> files;
        for (UINT i = 0; i < n; i++) {
            wchar_t f[MAX_PATH];
            DragQueryFileW(drop, i, f, MAX_PATH);
            if (!isDir(f))
                files.push_back(f);
        }
        DragFinish(drop);
        if (!files.empty())
            installOrSend(files);
        return 0;
    }

    case WM_CLOSE:
        if (vault::progress.running) {
            g.closeAfterVault = true;
            vault::progress.cancel = true;
            say(i18n::text(L"Cancelling recovery safely before closing…"));
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_ENDSESSION:
        overlay::restoreCursor();
        return 0;
    case WM_DESTROY:
        playtools::shutdown();
        overlay::restoreCursor();
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LONG WINAPI crashGuard(EXCEPTION_POINTERS *) {
    if (playtools::mouseCaptured())
        ClipCursor(nullptr);
    overlay::restoreCursor(); // never leave the custom cursor behind
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int show) {
    std::wstring contextError;
    if (!instances::configure(contextError)) {
        MessageBoxW(nullptr, contextError.c_str(), L"Velox instance", MB_OK | MB_ICONERROR);
        return 1;
    }
    bool afterUpdate = cmdLine && wcsstr(cmdLine, L"--after-update");
    // One launcher at a time: bring the open one to the front instead. Right after an update the
    // old copy is still closing, so wait for it.
    HANDLE mutex = nullptr;
    for (int i = 0; i < (afterUpdate ? 40 : 1); i++) {
        mutex = CreateMutexW(nullptr, TRUE, instances::mutexName().c_str());
        if (GetLastError() != ERROR_ALREADY_EXISTS)
            break;
        CloseHandle(mutex);
        mutex = nullptr;
        if (afterUpdate)
            Sleep(250);
    }
    if (!mutex) {
        if (HWND other = instances::window(instances::id())) {
            if (instances::initialCommand() == instances::Command::Open) {
                ShowWindow(other, SW_RESTORE);
                SetForegroundWindow(other);
            } else
                PostMessageW(other, instances::CONTROL, static_cast<WPARAM>(instances::initialCommand()), 0);
        }
        return 0;
    }
    SetUnhandledExceptionFilter(crashGuard);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    gp::GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    gp::GdiplusStartup(&token, &gsi, nullptr);

    overlay::restoreCursor(); // in case an earlier session ended badly
    update::cleanup();
    loadSettings(g.set, g.firstRun);
    i18n::language(g.set.language);
    // A downloaded update is installed before anything else starts.
    if (instances::id().empty() && g.set.autoUpdate && !instances::otherLaunchers() && update::pendingReady() && update::applyPending()) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return 0;
    }
    findPaths();
    bool recovered = vault::recoverInterrupted(reporter());
    if (!recovered)
        g.page = RECOVERY;
    loadSettings(g.set, g.firstRun);
    i18n::language(g.set.language);
    findPaths();
    setAutoRotation(g.set.autoRotate);
    setGestureNavigation(g.set.gestureNavigation, g.set.reverseButtons);
    g.activeGame = presets::active();
    refreshVault();
    refreshInstances();
    experience::load();
    experience::hostPreferences(g.set);
    host();
    if (g.firstRun)
        saveSettings(g.set);
    overlay::setCursorStyle(g.set.cursor);
    if (emulatorRunning()) {
        phone::state = State::Running;
        // The phone kept running while Velox was closed; reload what it was started with.
        if (!phone::adoptRunning()) {
            phone::distrustSnapshot();
            phone::running = g.set;
        }
    }
    loadThumbs();
    loadChangelog();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = static_cast<HICON>(
        LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    auto windowClass = instances::windowClass();
    wc.lpszClassName = windowClass.c_str();
    RegisterClassExW(&wc);
    UINT dpi = GetDpiForSystem();
    auto windowTitle = instances::id().empty() ? L"Velox" : L"Velox · " + i18n::tr(instances::name());
    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, windowTitle.c_str(), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN, CW_USEDEFAULT,
        CW_USEDEFAULT, MulDiv(static_cast<int>(WIN_W), dpi, 96), MulDiv(static_cast<int>(WIN_H), dpi, 96), nullptr, nullptr, inst, nullptr);
    if (!installed())
        g.page = HOME;
    if (g.firstRun && installed())
        say(i18n::text(L"Welcome! Settings were picked for this PC: ") + hostSummary());
    wchar_t justUpdated[32] = L"";
    GetPrivateProfileStringW(L"update", L"justupdated", L"", justUpdated, 32, iniPath().c_str());
    if (*justUpdated) {
        WritePrivateProfileStringW(L"update", L"justupdated", nullptr, iniPath().c_str());
        say(std::wstring(i18n::text(L"Updated to ")) + justUpdated + i18n::text(L". See what's new under Updates."));
    }
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    if (instances::initialCommand() != instances::Command::Open) {
        g.exitAfterStop = instances::initialCommand() == instances::Command::Stop;
        PostMessageW(hwnd, instances::CONTROL, static_cast<WPARAM>(instances::initialCommand()), 0);
    }
    if (instances::initialCommand() == instances::Command::Open && instances::id().empty() && g.set.autoStart && installed() &&
        show != SW_HIDE && recovered)
        PostMessageW(hwnd, WM_APP_REFRESH, 1, 0);
    std::thread(monitorLoop).detach();
    std::thread([] {
        ULONGLONG nextSync = 0;
        for (;;) {
            Sleep(500);
            if (phone::state != State::Running)
                continue;
            if (GetTickCount64() >= nextSync) {
                nextSync = GetTickCount64() + 2500;
                if (auto prefs = experience::tick(reporter())) {
                    auto message = std::make_unique<experience::GuestPreferences>(*prefs);
                    if (PostMessageW(g.hwnd, WM_APP + 41, 0, reinterpret_cast<LPARAM>(message.get())))
                        message.release();
                }
                refresh();
            }
            playtools::pollPhone();
        }
    }).detach();
    std::thread([] {
        NavigationResult lastNavigation = NavigationResult::Unchanged;
        for (;;) {
            Sleep(100);
            NavigationResult navigation = syncNavigation();
            if (navigation == NavigationResult::NeedsRoot && lastNavigation != navigation)
                say(i18n::text(L"Reverse button order needs Root enabled on the Phone page."), true);
            if (navigation == NavigationResult::RestartRequired && lastNavigation != navigation)
                say(i18n::text(L"Restart the phone to finish setting up reversed buttons."));
            if (navigation == NavigationResult::Unsupported && lastNavigation != navigation)
                say(i18n::text(L"Reversed buttons aren't supported by this phone's launcher version."), true);
            if (navigation == NavigationResult::Applied &&
                (lastNavigation == NavigationResult::NeedsRoot || lastNavigation == NavigationResult::RestartRequired ||
                 lastNavigation == NavigationResult::Unsupported))
                say(i18n::text(L"Navigation updated."));
            if (navigation != NavigationResult::Unchanged)
                lastNavigation = navigation;
            syncOrientation();
        }
    }).detach();
    std::thread([] { startAdbServer(); }).detach();
    // Check for updates at most every 6 hours.
    wchar_t checked[32] = L"";
    GetPrivateProfileStringW(L"update", L"checked", L"0", checked, 32, iniPath().c_str());
    SYSTEMTIME st;
    GetSystemTime(&st);
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER now{{ft.dwLowDateTime, ft.dwHighDateTime}};
    if (update::configured() && now.QuadPart / 10000000ull - _wtoi64(checked) > 6 * 3600)
        checkForUpdates(false);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (m.message == WM_KEYDOWN && m.wParam == VK_RETURN && m.hwnd == g.numberEdit)
            SetFocus(g.hwnd);
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE && GetParent(m.hwnd) && GetParent(m.hwnd) != g.hwnd) {
            DWORD owner = 0;
            GetWindowThreadProcessId(GetParent(m.hwnd), &owner);
            if (owner == GetCurrentProcessId()) {
                DestroyWindow(GetParent(m.hwnd));
                continue;
            }
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    overlay::restoreCursor();
    ExitProcess(static_cast<UINT>(m.wParam)); // don't wait for background work; the phone keeps running
}
