#include "overlay.hpp"

#include <atomic>
#include <cmath>
#include <thread>

#include "art.hpp"
#include "phone.hpp"

namespace phone::overlay {
namespace {

std::atomic<bool> g_bootBusy{false};
std::atomic<bool> g_previewMode{false}, g_liveBootQueued{false};

struct EmuWindows {
    HWND main = nullptr, display = nullptr;
    uint64_t score = 0;
};

EmuWindows findEmuWindows(DWORD pid) {
    struct Ctx {
        DWORD pid;
        EmuWindows found;
    } ctx{pid, {}};
    if (!pid)
        return {};
    EnumWindows(
        [](HWND w, LPARAM p) -> BOOL {
            auto *c = reinterpret_cast<Ctx *>(p);
            DWORD owner = 0;
            GetWindowThreadProcessId(w, &owner);
            if (owner != c->pid || !IsWindowVisible(w))
                return TRUE;
            wchar_t title[128] = L"";
            GetWindowTextW(w, title, 128);
            if (wcsncmp(title, L"Android Emulator", 16) != 0)
                return TRUE;
            c->found.main = w;
            EnumChildWindows(
                w,
                [](HWND ch, LPARAM p2) -> BOOL {
                    wchar_t cls[64] = L"";
                    GetClassNameW(ch, cls, 64);
                    if (wcscmp(cls, L"subWin") == 0) {
                        auto *found = reinterpret_cast<EmuWindows *>(p2);
                        RECT bounds{};
                        if (GetClientRect(ch, &bounds) && bounds.right >= 60 && bounds.bottom >= 60) {
                            uint64_t score = uint64_t(bounds.right) * bounds.bottom + (IsWindowVisible(ch) ? 1ull << 50 : 0);
                            if (score > found->score) {
                                found->display = ch;
                                found->score = score;
                            }
                        }
                    }
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&c->found));
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

std::atomic<bool> g_previewClose{false};

LRESULT CALLBACK bootProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        g_previewClose = true;
        return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE)
            g_previewClose = true;
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    default:
        return DefWindowProcW(h, m, w, l);
    }
}

// High resolution frame pacing, so the animation runs at a steady 60 fps.
class FrameTimer {
  public:
    FrameTimer() { timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS); }
    ~FrameTimer() {
        if (timer_)
            CloseHandle(timer_);
    }
    void waitUntil(ULONGLONG targetMs) {
        ULONGLONG now = GetTickCount64();
        if (targetMs <= now)
            return;
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>((targetMs - now) * 10000);
        if (timer_ && SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE))
            WaitForSingleObject(timer_, 100);
        else
            Sleep(static_cast<DWORD>(targetMs - now));
    }

  private:
    HANDLE timer_ = nullptr;
};

void runBootScreen(std::wstring subtitle, bool round, bool preview, int style) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = bootProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"VeloxBootScreen";
        RegisterClassExW(&wc);
        registered = true;
    }
    HWND hwnd = nullptr;
    EmuWindows emu;
    DWORD pid = 0;
    ULONGLONG start = GetTickCount64(), lastLookup = 0, firstVisible = 0;
    float shownProgress = 0, fadeOutAt = -1;
    int w = 0, h = 0;
    int childX = 0, childY = 0;
    std::unique_ptr<art::BootScreen> screenArt;
    HDC screenDC = GetDC(nullptr), mem = CreateCompatibleDC(screenDC);
    HBITMAP dib = nullptr;
    HGDIOBJ oldBmp = nullptr;
    void *bits = nullptr;
    FrameTimer pacer;

    for (;;) {
        ULONGLONG frameStart = GetTickCount64();
        float t = (frameStart - (firstVisible ? firstVisible : start)) / 1000.f;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            DispatchMessageW(&msg);

        RECT r{};
        HWND targetHost = nullptr;
        bool target = false;
        if (preview) {
            RECT work;
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
            int ph = static_cast<int>((work.bottom - work.top) * 0.72f), pw = ph * 9 / 20;
            r.left = work.left + (work.right - work.left - pw) / 2;
            r.top = work.top + (work.bottom - work.top - ph) / 2;
            r.right = r.left + pw;
            r.bottom = r.top + ph;
            target = true;
        } else {
            if (frameStart - lastLookup > (emu.display && !IsWindowVisible(emu.display) ? 120u : 400u) || !emu.display || !IsWindow(emu.display)) {
                lastLookup = frameStart;
                pid = emulatorPid();
                emu = findEmuWindows(pid);
            }
            targetHost = emu.display;
            while (targetHost && targetHost != emu.main && !IsWindowVisible(targetHost))
                targetHost = GetParent(targetHost);
            target = emu.display && targetHost && IsWindowVisible(targetHost) && IsWindowVisible(emu.main) &&
                     !IsIconic(emu.main) && GetClientRect(emu.display, &r) && r.right >= 60 && r.bottom >= 60;
            if (target && targetHost != emu.display)
                MapWindowPoints(emu.display, targetHost, reinterpret_cast<POINT *>(&r), 2);
        }

        if (target && !firstVisible) {
            firstVisible = frameStart;
            t = 0;
        }
        // Progress follows the real boot, eased so it never jumps.
        float goal = preview ? std::min(1.f, t / 5.f) : boot.progress.load();
        shownProgress += (goal - shownProgress) * 0.10f;
        if (goal >= 1.f && shownProgress > 0.985f)
            shownProgress = 1.f;
        bool finished = preview ? (t > 6.2f || g_previewClose) : ((firstVisible && boot.done && shownProgress >= 1.f) || !boot.active);
        if (finished && fadeOutAt < 0)
            fadeOutAt = std::max(t, boot.resume && !preview ? 0.6f : 1.2f);
        if (!preview && frameStart - start > 300000)
            break; // never stay up forever
        float fadeIn = std::min(1.f, t / 0.35f), fadeOut = fadeOutAt < 0 || t < fadeOutAt ? 1.f : 1.f - (t - fadeOutAt) / 0.45f;
        if (fadeOutAt >= 0 && t >= fadeOutAt + 0.45f)
            break;
        float alpha = std::clamp(std::min(fadeIn, fadeOut), 0.f, 1.f);

        if (!target) {
            if (hwnd)
                ShowWindow(hwnd, SW_HIDE);
            pacer.waitUntil(frameStart + 30);
            continue;
        }
        if (hwnd && (!IsWindow(hwnd) || (!preview && GetParent(hwnd) != targetHost))) {
            if (IsWindow(hwnd))
                DestroyWindow(hwnd);
            hwnd = nullptr;
        }
        if (!hwnd) {
            // A child moves with the display in the compositor, independently of rendering.
            // A desktop popup positioned using coordinates sampled before drawing visibly lags
            // and snaps back while the phone is dragged.
            DWORD ex = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | (preview ? WS_EX_TOPMOST : WS_EX_TRANSPARENT);
            DPI_AWARENESS_CONTEXT oldDpi = nullptr;
            if (!preview)
                oldDpi = SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(targetHost));
            hwnd = CreateWindowExW(ex, L"VeloxBootScreen", L"Velox", preview ? WS_POPUP : WS_CHILD, r.left,
                                   r.top, 1, 1, preview ? nullptr : targetHost, nullptr, inst, nullptr);
            if (oldDpi)
                SetThreadDpiAwarenessContext(oldDpi);
            if (!hwnd)
                break;
            childX = r.left;
            childY = r.top;
        }
        if (!IsWindow(hwnd)) {
            // Qt can replace the display child during startup or a skin change.
            // Reattach on the next frame instead of dropping the whole boot screen.
            hwnd = nullptr;
            emu = {};
            lastLookup = 0;
            continue;
        }
        int W = r.right - r.left, H = r.bottom - r.top;
        if (W < 60 || H < 60) {
            pacer.waitUntil(frameStart + 30);
            continue;
        }
        if (W != w || H != h) {
            if (dib) {
                SelectObject(mem, oldBmp);
                DeleteObject(dib);
            }
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = W;
            bi.bmiHeader.biHeight = -H;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            dib = CreateDIBSection(screenDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (!dib)
                break;
            oldBmp = SelectObject(mem, dib);
            w = W;
            h = H;
            screenArt = std::make_unique<art::BootScreen>(W, H, style);
        }
        {
            gp::Bitmap canvas(W, H, W * 4, PixelFormat32bppPARGB, static_cast<BYTE *>(bits));
            gp::Graphics g(&canvas);
            g.Clear(gp::Color(0, 0, 0, 0));
            if (round || preview) {
                gp::GraphicsPath clip;
                art::roundRectPath(clip, gp::RectF(0, 0, static_cast<float>(W), static_cast<float>(H)), std::min(W, H) * 0.075f);
                g.SetClip(&clip);
            }
            std::wstring status = preview ? (t < 1.5f  ? i18n::text(L"Starting Android")
                                             : t < 4.f ? i18n::text(L"Loading apps and services")
                                                       : L"Ready")
                                          : boot.text();
            int pct = static_cast<int>(shownProgress * 100 + 0.5f);
            screenArt->draw(g, t, shownProgress, status + L"   " + std::to_wstring(pct) + L"%", subtitle);
        }
        BLENDFUNCTION bf{AC_SRC_OVER, 0, static_cast<BYTE>(alpha * 255), AC_SRC_ALPHA};
        POINT pos{r.left, r.top}, zero{0, 0};
        SIZE size{W, H};
        if (!preview) {
            // Keep local coordinates. Sampling a screen rectangle and mapping
            // it back after the parent moves introduces the same drag jitter
            // this child window is meant to avoid.
            if (childX != r.left || childY != r.top) {
                SetWindowPos(hwnd, HWND_TOP, r.left, r.top, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                childX = r.left;
                childY = r.top;
            } else if (GetWindow(hwnd, GW_HWNDPREV))
                SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
        UpdateLayeredWindow(hwnd, screenDC, preview ? &pos : nullptr, &size, mem, &zero, 0, &bf, ULW_ALPHA);
        if (!IsWindowVisible(hwnd))
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        pacer.waitUntil(frameStart + 16);
    }
    if (hwnd && IsWindow(hwnd))
        DestroyWindow(hwnd);
    if (dib) {
        SelectObject(mem, oldBmp);
        DeleteObject(dib);
    }
    DeleteDC(mem);
    ReleaseDC(nullptr, screenDC);
    g_previewMode = false;
    g_bootBusy = false;
}

// ---- cursor

int g_cursorStyle = 0;
HCURSOR g_cursor = nullptr;
bool g_applied = false;
DWORD g_pidCache = 0;
ULONGLONG g_pidTime = 0;

int cursorSize() {
    int size = GetSystemMetricsForDpi(SM_CXCURSOR, GetDpiForSystem());
    DWORD base = 0, len = sizeof(base);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Cursors", L"CursorBaseSize", RRF_RT_REG_DWORD, nullptr, &base, &len) ==
            ERROR_SUCCESS &&
        base > 0)
        size = std::max(size, static_cast<int>(base * GetDpiForSystem() / 96));
    return std::clamp(size, 24, 256);
}

void markApplied(bool on) {
    g_applied = on;
    WritePrivateProfileStringW(L"state", L"cursor", on ? L"1" : nullptr, iniPath().c_str());
}

} // namespace

void showBootScreen(const std::wstring &subtitle, bool roundCorners, int style) {
    if (g_bootBusy.exchange(true)) {
        if (!g_previewMode || g_liveBootQueued.exchange(true))
            return;
        g_previewClose = true;
        std::thread([subtitle, roundCorners, style] {
            for (int attempt = 0; attempt < 500 && boot.active; ++attempt) {
                bool expected = false;
                if (g_bootBusy.compare_exchange_strong(expected, true)) {
                    g_previewMode = false;
                    g_liveBootQueued = false;
                    runBootScreen(subtitle, roundCorners, false, style);
                    return;
                }
                Sleep(10);
            }
            g_liveBootQueued = false;
        }).detach();
        return;
    }
    g_previewMode = false;
    std::thread(runBootScreen, subtitle, roundCorners, false, style).detach();
}

void previewBootScreen(int style) {
    if (g_liveBootQueued || g_bootBusy.exchange(true))
        return;
    g_previewClose = false;
    g_previewMode = true;
    std::thread(runBootScreen, std::wstring(L"PREVIEW  ·  FULL HD+  ·  120 HZ"), true, true, style).detach();
}

bool bootScreenVisible() {
    return g_bootBusy || g_liveBootQueued;
}

void setCursorStyle(int style) {
    if (style == g_cursorStyle && (style == 0 || g_cursor))
        return;
    restoreCursor();
    if (g_cursor)
        DestroyCursor(g_cursor);
    g_cursor = nullptr;
    g_cursorStyle = style;
    if (style > 0)
        g_cursor = art::makeCursor(style, cursorSize());
}

void tickCursor() {
    if (!g_cursor)
        return;
    ULONGLONG now = GetTickCount64();
    if (now - g_pidTime > 1500) {
        g_pidCache = cachedEmulatorPid();
        g_pidTime = now;
    }
    bool over = false;
    if (g_pidCache) {
        POINT p;
        GetCursorPos(&p);
        HWND w = WindowFromPoint(p);
        DWORD pid = 0;
        if (w)
            GetWindowThreadProcessId(GetAncestor(w, GA_ROOT), &pid);
        over = pid == g_pidCache;
    }
    if (over && !g_applied) {
        if (HCURSOR copy = CopyCursor(g_cursor)) {
            if (SetSystemCursor(copy, 32512 /* OCR_NORMAL */))
                markApplied(true);
            else
                DestroyCursor(copy);
        }
    } else if (!over && g_applied) {
        restoreCursor();
    }
}

void restoreCursor() {
    bool wasApplied = g_applied || GetPrivateProfileIntW(L"state", L"cursor", 0, iniPath().c_str()) == 1;
    if (wasApplied)
        SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0); // reloads the user's own cursors
    if (wasApplied)
        markApplied(false);
}

} // namespace phone::overlay
