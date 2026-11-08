#include "playtools.hpp"
#include "window_scale.hpp"
#include "experience.hpp"
#include "controllers.hpp"
#include "presets.hpp"
#include <set>
#include <stdexcept>
#include "art.hpp"
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <array>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <thread>

namespace phone::playtools {
namespace {
HWND launcher = nullptr, bar = nullptr, editor = nullptr, tip = nullptr, emu = nullptr, display = nullptr, stockBar = nullptr;
std::atomic<DWORD> phonePid{0};
std::atomic<HWND> focusEmu{nullptr};
RECT viewport{};
float scale = 1;
int requestedPercent = 0, effectivePercent = 0;
std::array<INT_PTR, 10> fitApplied{}, fitPending{};
ULONGLONG fitAfter = 0;
RECT fitGeometry{};
bool showBar = true, leftDock = false, collapsed = false, isFullscreen = false, stockWasVisible = false, nativeControls = false;
WINDOWPLACEMENT oldPlacement = [] {
    WINDOWPLACEMENT p{};
    p.length = sizeof(p);
    return p;
}();
LONG_PTR oldStyle = 0;
std::mutex lock;
Profile current;
std::wstring message = i18n::text(L"Choose a game, then add its controls.");
bool editingNow = false;
std::atomic<bool> aimLocked{false};
bool editorTakesFocus = true;
std::array<bool, 512> held{};
std::array<bool, 14> sticks{};
std::atomic<bool> quitting{false}, cancelWanted{false};
std::thread inputThread;
HHOOK keyboardHook = nullptr, mouseHook = nullptr;
struct Contact {
    char op;
    int id;
    float x, y;
};
std::deque<Contact> events;
std::map<int, ULONGLONG> swipes;
float aimX = .5f, aimY = .5f;
int selected = -1, dragging = -1;
bool choosingKey = false, choosingEnd = false;
struct Tool {
    int action;
    const wchar_t *icon;
    const wchar_t *name;
};
const std::array<Tool, 15> tools{{{Back, L"\xE72B", i18n::text(L"Back")},
                                  {Home, L"\xE80F", i18n::text(L"Home")},
                                  {Recents, L"\xE8A5", i18n::text(L"Recent apps")},
                                  {VolumeUp, L"\xE995", i18n::text(L"Volume up")},
                                  {VolumeDown, L"\xE993", i18n::text(L"Volume down")},
                                  {RotationLock, L"\xE7AD", i18n::text(L"Rotation lock")},
                                  {Fullscreen, L"\xE740", i18n::text(L"Fullscreen")},
                                  {Screenshot, L"\xE722", i18n::text(L"Screenshot")},
                                  {Record, L"\xE714", i18n::text(L"Record screen")},
                                  {Library, L"\xE8F1", i18n::text(L"Game library")},
                                  {Editor, L"\xE765", i18n::text(L"Edit controls")},
                                  {EnableMapping, L"\xE7FC", i18n::text(L"Enable mapped controls")},
                                  {Settings, L"\xE713", i18n::text(L"Velox Settings")},
                                  {Power, L"\xE7E8", i18n::text(L"Power button")},
                                  {ScreenResolution, L"\xE7F4", i18n::text(L"Screen resolution")}}};
std::vector<int> order = [] {
    std::vector<int> values;
    for (size_t i = 0; i < tools.size(); i++)
        values.push_back(static_cast<int>(i));
    return values;
}();
std::wstring ini() {
    return dataFolder() + L"\\experience.ini";
}
bool safePackage(const std::wstring &s) {
    static const std::wregex p(L"[A-Za-z][A-Za-z0-9_]*(\\.[A-Za-z][A-Za-z0-9_]*)+");
    return s.size() < 200 && std::regex_match(s, p);
}
std::wstring profilePath() {
    return dataFolder() + L"\\controls\\" + current.package + (current.landscape ? L"-landscape.ini" : L"-portrait.ini");
}
void persist() {
    if (!safePackage(current.package))
        return;
    createDirs(dataFolder() + L"\\controls");
    std::wofstream file{std::filesystem::path(profilePath() + L".new")};
    file << L"[controls]\nenabled=" << current.enabled << L"\npadmode=" << current.padMode << L"\npadlayout=" << current.padLayout
         << L"\npaddevice=" << current.padDevice << L"\ndeadzone=" << current.deadzone << L"\nsensitivity=" << current.sensitivity
         << L"\ninverty=" << current.invertY << L"\n";
    for (size_t i = 0; i < current.bindings.size(); i++) {
        auto &b = current.bindings[i];
        file << L"binding" << i << L"=" << static_cast<int>(b.kind) << L"," << b.key << L"," << b.x << L"," << b.y << L"," << b.x2 << L","
             << b.y2 << L"," << b.radius << L"\n";
    }
    file.close();
    MoveFileExW((profilePath() + L".new").c_str(), profilePath().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}
void enqueue(char op, int id, float x = 0, float y = 0) {
    if (events.size() > 512) {
        events.clear();
        cancelWanted = true;
        held.fill(false);
        return;
    }
    if (op == 'M' && !events.empty() && events.back().op == 'M' && events.back().id == id)
        events.back() = {op, id, x, y};
    else
        events.push_back({op, id, x, y});
}
void unlockAim() {
    if (aimLocked) {
        aimLocked = false;
        ClipCursor(nullptr);
    }
}
void cancel() {
    cancelWanted = true;
    events.clear();
    held.fill(false);
    sticks.fill(false);
    swipes.clear();
    unlockAim();
}
bool gameFocus() {
    DWORD pid = 0;
    HWND foreground = GetForegroundWindow();
    GetWindowThreadProcessId(foreground, &pid);
    HWND target = focusEmu.load();
    return phonePid && pid == phonePid && state == State::Running && target && !IsIconic(target) &&
           (foreground == target || GetAncestor(foreground, GA_ROOT) == target);
}
bool reservedKey(int key) {
    return key == VK_ESCAPE || key == VK_F8 || key == VK_F10 || key == VK_MENU || key == VK_LMENU || key == VK_RMENU || key == VK_LWIN ||
           key == VK_RWIN;
}
bool keyInput(int key, bool down) {
    if (key == VK_LSHIFT || key == VK_RSHIFT)
        key = VK_SHIFT;
    if (key == VK_LCONTROL || key == VK_RCONTROL)
        key = VK_CONTROL;
    if (key < 0 || key >= static_cast<int>(held.size()))
        return false;
    if (key == VK_ESCAPE && down && aimLocked) {
        cancel();
        return true;
    }
    if (key == VK_F8) {
        for (auto &b : current.bindings)
            if (b.kind == Kind::Aim) {
                if (!down) {
                    held[VK_F8] = false;
                    return true;
                }
                if (held[VK_F8])
                    return true;
                if (aimLocked) {
                    cancel();
                } else {
                    aimLocked = true;
                    aimX = b.x;
                    aimY = b.y;
                    enqueue('D', 15, aimX, aimY);
                    RECT clip = viewport;
                    InflateRect(&clip, -3, -3);
                    ClipCursor(&clip);
                    SetCursorPos((clip.left + clip.right) / 2, (clip.top + clip.bottom) / 2);
                }
                held[VK_F8] = true;
                return true;
            }
        return false;
    }
    bool match = false;
    for (auto &b : current.bindings)
        if (b.kind == Kind::Stick ? (key == 'W' || key == 'A' || key == 'S' || key == 'D')
                                  : (b.kind == Kind::Tap || b.kind == Kind::Swipe) && b.key == key)
            match = true;
    if (!match)
        return false;
    if (held[key] == down)
        return down;
    held[key] = down;
    for (size_t i = 0; i < current.bindings.size(); i++) {
        auto &b = current.bindings[i];
        int id = static_cast<int>(i);
        if (b.kind == Kind::Stick && (key == 'W' || key == 'A' || key == 'S' || key == 'D')) {
            float x = (held['D'] ? 1.f : 0) - (held['A'] ? 1.f : 0), y = (held['S'] ? 1.f : 0) - (held['W'] ? 1.f : 0);
            float length = std::sqrt(x * x + y * y);
            if (length > 0) {
                float width = static_cast<float>(std::max<LONG>(1, viewport.right - viewport.left)),
                      height = static_cast<float>(std::max<LONG>(1, viewport.bottom - viewport.top)), shortSide = std::min(width, height);
                enqueue(sticks[i] ? 'M' : 'D', id, b.x + x / length * b.radius * shortSide / width,
                        b.y + y / length * b.radius * shortSide / height);
                sticks[i] = true;
            } else if (sticks[i]) {
                enqueue('U', id);
                sticks[i] = false;
            }
        } else if (b.key == key && b.kind == Kind::Tap)
            enqueue(down ? 'D' : 'U', id, b.x, b.y);
        else if (b.key == key && b.kind == Kind::Swipe && down) {
            enqueue('D', id, b.x, b.y);
            swipes[id] = GetTickCount64();
        }
    }
    return true;
}
LRESULT CALLBACK keyProc(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION) {
        auto *k = reinterpret_cast<KBDLLHOOKSTRUCT *>(l);
        if (!(k->flags & LLKHF_INJECTED)) {
            // Keep desktop escape routes available even if a profile binds Tab.
            if (k->vkCode == VK_LWIN || k->vkCode == VK_RWIN || k->vkCode == VK_MENU || k->vkCode == VK_LMENU || k->vkCode == VK_RMENU ||
                (k->flags & LLKHF_ALTDOWN) || (GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000)) {
                cancelWanted = true;
                return CallNextHookEx(keyboardHook, code, w, l);
            }
            std::unique_lock<std::mutex> g(lock, std::try_to_lock);
            if (!g.owns_lock()) {
                cancelWanted = true;
                return CallNextHookEx(keyboardHook, code, w, l);
            }
            if (!editingNow && current.enabled && gameFocus() &&
                keyInput(static_cast<int>(k->vkCode), w == WM_KEYDOWN || w == WM_SYSKEYDOWN))
                return 1;
        }
    }
    return CallNextHookEx(keyboardHook, code, w, l);
}
LRESULT CALLBACK mouseProc(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION) {
        auto *m = reinterpret_cast<MSLLHOOKSTRUCT *>(l);
        if (!(m->flags & LLMHF_INJECTED)) {
            std::unique_lock<std::mutex> g(lock, std::try_to_lock);
            if (!g.owns_lock()) {
                if (w != WM_MOUSEMOVE)
                    cancelWanted = true;
                return CallNextHookEx(mouseHook, code, w, l);
            }
            if (!editingNow && current.enabled && gameFocus()) {
                int key = w == WM_LBUTTONDOWN || w == WM_LBUTTONUP ? VK_LBUTTON : w == WM_RBUTTONDOWN || w == WM_RBUTTONUP ? VK_RBUTTON : 0;
                if (key && keyInput(key, w == WM_LBUTTONDOWN || w == WM_RBUTTONDOWN))
                    return 1;
                if (w == WM_MOUSEMOVE && aimLocked) {
                    int cx = (viewport.left + viewport.right) / 2, cy = (viewport.top + viewport.bottom) / 2;
                    int dx = m->pt.x - cx, dy = m->pt.y - cy;
                    if (dx || dy) {
                        aimX += dx * 1.8f / std::max<LONG>(1, viewport.right - viewport.left);
                        aimY += dy * 1.8f / std::max<LONG>(1, viewport.bottom - viewport.top);
                        if (aimX < .08f || aimX > .92f || aimY < .08f || aimY > .92f) {
                            enqueue('U', 15);
                            for (auto &b : current.bindings)
                                if (b.kind == Kind::Aim) {
                                    aimX = b.x;
                                    aimY = b.y;
                                    break;
                                }
                            enqueue('D', 15, aimX, aimY);
                        } else
                            enqueue('M', 15, aimX, aimY);
                        SetCursorPos(cx, cy);
                    }
                    return 1;
                }
            }
        }
    }
    return CallNextHookEx(mouseHook, code, w, l);
}

struct InputPipe {
    HANDLE process = nullptr, write = nullptr, read = nullptr;
    void close() {
        if (write) {
            CloseHandle(write);
            write = nullptr;
        }
        if (process) {
            if (WaitForSingleObject(process, 250) == WAIT_TIMEOUT)
                TerminateProcess(process, 0);
            CloseHandle(process);
            process = nullptr;
        }
        if (read) {
            CloseHandle(read);
            read = nullptr;
        }
    }
    ~InputPipe() { close(); }
    bool send(const std::string &s) {
        if (!write)
            return false;
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD n = 0;
        bool ok = WriteFile(write, s.data(), static_cast<DWORD>(s.size()), &n, &ov) != FALSE;
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent, 100) == WAIT_OBJECT_0)
                ok = GetOverlappedResult(write, &ov, &n, FALSE) != FALSE;
            else {
                CancelIoEx(write, &ov);
                GetOverlappedResult(write, &ov, &n, TRUE);
            }
        }
        CloseHandle(ov.hEvent);
        return ok && n == s.size();
    }
    bool open(const std::wstring &custom = {}, bool immediate = false) {
        close();
        std::wstring command = custom.empty() ? experience::inputCommand() : custom;
        if (command.empty())
            return false;
        std::wstring pipe = L"\\\\.\\pipe\\VeloxInput-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        write = CreateNamedPipeW(pipe.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 65536, 65536, 0,
                                 nullptr);
        if (write == INVALID_HANDLE_VALUE) {
            write = nullptr;
            return false;
        }
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE childRead = CreateFileW(pipe.c_str(), GENERIC_READ, 0, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr),
               childWrite = nullptr;
        if (childRead == INVALID_HANDLE_VALUE) {
            close();
            return false;
        }
        if (!CreatePipe(&read, &childWrite, &sa, 65536)) {
            CloseHandle(childRead);
            close();
            return false;
        }
        SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof(si);
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.StartupInfo.wShowWindow = SW_HIDE;
        si.StartupInfo.hStdInput = childRead;
        si.StartupInfo.hStdOutput = childWrite;
        si.StartupInfo.hStdError = childWrite;
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        std::vector<BYTE> attributes(bytes);
        si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        BOOL initialized = InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &bytes);
        HANDLE handles[] = {childRead, childWrite};
        BOOL updated = initialized && UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                                                sizeof(handles), nullptr, nullptr);
        PROCESS_INFORMATION pi{};
        BOOL started = updated && CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                                 CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &si.StartupInfo, &pi);
        if (initialized)
            DeleteProcThreadAttributeList(si.lpAttributeList);
        CloseHandle(childRead);
        CloseHandle(childWrite);
        if (!started) {
            close();
            return false;
        }
        CloseHandle(pi.hThread);
        process = pi.hProcess;
        if (immediate)
            return true;
        std::string output;
        ULONGLONG begin = GetTickCount64();
        while (GetTickCount64() - begin < 5000 && !quitting) {
            DWORD available = 0;
            if (!PeekNamedPipe(read, nullptr, 0, nullptr, &available, nullptr))
                break;
            if (available) {
                char buffer[2048];
                DWORD n = 0;
                if (!ReadFile(read, buffer, std::min<DWORD>(available, sizeof(buffer)), &n, nullptr))
                    break;
                output.append(buffer, n);
                if (output.find("VELOX_INPUT_READY") != std::string::npos)
                    return true;
                if (output.find("VELOX_INPUT_ERROR") != std::string::npos)
                    break;
            }
            if (WaitForSingleObject(process, 0) != WAIT_TIMEOUT)
                break;
            Sleep(10);
        }
        {
            std::lock_guard<std::mutex> g(lock);
            message = i18n::text(L"Game controls couldn't connect. Reopen the phone and try again.");
        }
        close();
        return false;
    }
};
void touchController(const controllers::Frame &f) {
    for (int i = 0; i < controllers::Count; i++)
        keyInput(256 + i, (f.buttons & (1u << i)) != 0);
    for (size_t i = 0; i < current.bindings.size(); i++) {
        auto &b = current.bindings[i];
        if (b.kind != Kind::PadLeft && b.kind != Kind::PadRight)
            continue;
        float x = b.kind == Kind::PadLeft ? f.lx : f.rx, y = b.kind == Kind::PadLeft ? f.ly : f.ry;
        if (std::hypot(x, y) > .001f) {
            float w = static_cast<float>(std::max<LONG>(1, viewport.right - viewport.left)),
                  h = static_cast<float>(std::max<LONG>(1, viewport.bottom - viewport.top)), side = std::min(w, h);
            enqueue(sticks[i] ? 'M' : 'D', int(i), b.x + x * b.radius * side / w, b.y + y * b.radius * side / h);
            sticks[i] = true;
        } else if (sticks[i]) {
            enqueue('U', int(i));
            sticks[i] = false;
        }
    }
}
void inputLoop() {
    InputPipe pipe, native;
    controllers::Reader reader(launcher);
    ULONGLONG retry = 0, nativeRetry = 0;
    bool wasActive = false, wasPad = false;
    std::string lastNative;
    while (!quitting) {
        std::deque<Contact> batch;
        bool reset = cancelWanted.exchange(false), active = false, wanted = false, padWanted = false, padActive = false;
        Profile p;
        {
            std::lock_guard<std::mutex> g(lock);
            p = current;
        }
        auto frame = controllers::filtered(reader.read(p.padDevice, p.padLayout), p.deadzone, p.sensitivity, p.invertY);
        {
            std::lock_guard<std::mutex> g(lock);
            bool focus = !editingNow && gameFocus();
            active = (current.enabled || current.padMode == 2) && focus;
            padActive = current.padMode != 0 && focus && frame.connected;
            wanted = ((current.enabled && !current.bindings.empty()) || current.padMode == 2) && !editingNow && state == State::Running;
            padWanted = current.padMode == 1 && frame.connected && !editingNow && state == State::Running;
            if (!active && (wasActive || !events.empty() || aimLocked || !swipes.empty() ||
                            std::any_of(held.begin(), held.end(), [](bool v) { return v; }))) {
                cancel();
                reset = true;
            }
            if (wasPad && !padActive) {
                cancel();
                reset = true;
            }
            wasActive = active;
            wasPad = padActive;
            if (padActive && current.padMode == 2)
                touchController(frame);
            if (active) {
                for (auto it = swipes.begin(); it != swipes.end();) {
                    auto &b = current.bindings[it->first];
                    float t = std::min(1.f, (GetTickCount64() - it->second) / 160.f);
                    enqueue('M', it->first, b.x + (b.x2 - b.x) * t, b.y + (b.y2 - b.y) * t);
                    if (t >= 1) {
                        enqueue('U', it->first);
                        it = swipes.erase(it);
                    } else
                        ++it;
                }
            }
            batch.swap(events);
        }
        if (state != State::Running || !wanted) {
            if (pipe.process)
                pipe.send("C\n");
            pipe.close();
            batch.clear();
        }
        if (wanted && !pipe.process && experience::companionReady() && GetTickCount64() > retry) {
            retry = GetTickCount64() + 5000;
            if (pipe.open()) {
                std::lock_guard<std::mutex> g(lock);
                message = i18n::text(L"Touch controls ready · Esc releases mouse aim");
                cancel();
                batch.clear();
                reset = true;
            }
        }
        if (pipe.process) {
            bool ok = !reset || pipe.send("C\n");
            for (auto &e : batch) {
                if (!ok)
                    break;
                std::ostringstream out;
                out << e.op << ' ' << e.id;
                if (e.op != 'U')
                    out << ' ' << std::lround(std::clamp(e.x, 0.f, 1.f) * 10000) << ' ' << std::lround(std::clamp(e.y, 0.f, 1.f) * 10000);
                out << '\n';
                ok = pipe.send(out.str());
            }
            DWORD available = 0;
            if (!ok || WaitForSingleObject(pipe.process, 0) != WAIT_TIMEOUT ||
                !PeekNamedPipe(pipe.read, nullptr, 0, nullptr, &available, nullptr)) {
                pipe.close();
                std::lock_guard<std::mutex> g(lock);
                cancel();
                message = i18n::text(L"Touch controls disconnected. Reconnecting automatically…");
            } else if (available) {
                char b[2048];
                DWORD n = 0;
                ReadFile(pipe.read, b, std::min<DWORD>(available, sizeof(b)), &n, nullptr);
                if (std::string(b, n).find("VELOX_INPUT_ERROR") != std::string::npos) {
                    pipe.close();
                    std::lock_guard<std::mutex> g(lock);
                    cancel();
                    message = i18n::text(L"Android refused touch input. Re-enable controls to reconnect.");
                }
            }
        }
        if (!padWanted) {
            if (native.process)
                native.send(controllers::report({}));
            native.close();
            lastNative.clear();
        }
        if (padWanted && !native.process && experience::companionReady() && GetTickCount64() > nativeRetry) {
            nativeRetry = GetTickCount64() + 5000;
            std::wstring command = L"\"" + paths().sdk + L"\\platform-tools\\adb.exe\" -s " + adbSerial() + L" shell /system/bin/hid -";
            bool ready = native.open(command, true) && native.send(controllers::registration()) && native.send(controllers::report({}));
            if (ready) {
                ready = false;
                for (int i = 0; i < 20 && !quitting && state == State::Running; i++) {
                    auto r = adb(L"shell content call --uri content://app.velox.home.bridge --method gamepad", 2000);
                    if (r.find("ready=true") != std::string::npos) {
                        ready = true;
                        break;
                    }
                    Sleep(50);
                }
            }
            if (!ready) {
                native.close();
                std::lock_guard<std::mutex> g(lock);
                message = i18n::text(L"Native controller couldn't connect. Touch mapping is available.");
            } else {
                lastNative.clear();
                std::lock_guard<std::mutex> g(lock);
                message = i18n::text(L"Velox Controller connected to Android");
            }
        }
        if (native.process) {
            bool allowed = false;
            {
                std::lock_guard<std::mutex> g(lock);
                allowed = !editingNow && current.package == p.package && current.padMode == 1 && gameFocus();
            }
            auto report = controllers::report(allowed && frame.connected ? frame : controllers::Frame{});
            bool ok = true;
            if (report != lastNative || reset) {
                ok = native.send(report);
                lastNative = report;
            }
            DWORD available = 0;
            if (!ok || WaitForSingleObject(native.process, 0) != WAIT_TIMEOUT ||
                !PeekNamedPipe(native.read, nullptr, 0, nullptr, &available, nullptr)) {
                native.close();
                lastNative.clear();
            } else if (available) {
                char output[2048];
                DWORD n = 0;
                ReadFile(native.read, output, std::min<DWORD>(available, sizeof(output)), &n, nullptr);
            }
        }
        Sleep(wanted || padWanted ? 8 : 50);
    }
    if (pipe.process)
        pipe.send("C\nQ\n");
    if (native.process)
        native.send(controllers::report({}));
}

void drawText(gp::Graphics &gr, const std::wstring &s, const gp::RectF &r, float size, gp::ARGB color, bool bold = false) {
    gp::Font f(L"Segoe UI", size, bold ? gp::FontStyleBold : gp::FontStyleRegular, gp::UnitPixel);
    gp::SolidBrush brush{gp::Color(color)};
    gp::StringFormat fmt;
    fmt.SetAlignment(gp::StringAlignmentCenter);
    fmt.SetLineAlignment(gp::StringAlignmentCenter);
    fmt.SetTrimming(gp::StringTrimmingEllipsisCharacter);
    auto translated = i18n::tr(s);
    gr.DrawString(translated.c_str(), -1, &f, r, &fmt, &brush);
}
int visibleTools(int height) {
    return collapsed ? 0 : std::max(1, std::min<int>(order.size(), (height - 88) / 38));
}
int atTool(int y, int height) {
    if (y < 48)
        return -1;
    int i = (y - 48) / 38;
    return i < visibleTools(height) ? i : -2;
}
void menu(HWND h, int selectedTool) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 100, collapsed ? i18n::text(L"Expand toolbar") : i18n::text(L"Collapse toolbar"));
    AppendMenuW(m, MF_STRING, 101, leftDock ? i18n::text(L"Dock on right") : i18n::text(L"Dock on left"));
    AppendMenuW(m, MF_STRING | (stockBar ? 0 : MF_GRAYED), 102,
                nativeControls ? i18n::text(L"Hide Android emulator controls") : i18n::text(L"Show Android emulator controls"));
    AppendMenuW(m, MF_STRING, WindowScaling, i18n::text(L"Window scaling"));
    AppendMenuW(m, MF_STRING, Rotate, i18n::text(L"Rotate phone manually"));
    if (selectedTool >= 0) {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | (selectedTool == 0 ? MF_GRAYED : 0), 103, i18n::text(L"Move button up"));
        AppendMenuW(m, MF_STRING | (selectedTool + 1 == static_cast<int>(order.size()) ? MF_GRAYED : 0), 104,
                    i18n::text(L"Move button down"));
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    for (auto &t : tools)
        AppendMenuW(m, MF_STRING, t.action, i18n::text(t.name));
    POINT p;
    GetCursorPos(&p);
    int result = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, p.x, p.y, 0, h, nullptr);
    DestroyMenu(m);
    if (result == 100) {
        collapsed = !collapsed;
        WritePrivateProfileStringW(L"toolbar", L"collapsed", collapsed ? L"1" : L"0", ini().c_str());
    } else if (result == 101) {
        leftDock = !leftDock;
        WritePrivateProfileStringW(L"toolbar", L"left", leftDock ? L"1" : L"0", ini().c_str());
    } else if (result == 102 && stockBar) {
        nativeControls = !nativeControls;
        ShowWindowAsync(stockBar, nativeControls ? SW_SHOWNOACTIVATE : SW_HIDE);
    } else if (result == 103 || result == 104) {
        if (selectedTool >= 0) {
            int other = selectedTool + (result == 103 ? -1 : 1);
            if (other >= 0 && other < static_cast<int>(order.size()))
                std::swap(order[selectedTool], order[other]);
            std::wstring s;
            for (int i : order)
                s += std::to_wstring(i) + L",";
            WritePrivateProfileStringW(L"toolbar", L"order", s.c_str(), ini().c_str());
        }
    } else if (result > 0 && result < 100)
        PostMessageW(launcher, ACTION_MESSAGE, result, 0);
}
LRESULT CALLBACK barProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    static int hover = -1;
    switch (m) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP image = CreateCompatibleBitmap(dc, r.right, r.bottom);
        auto old = SelectObject(mem, image);
        {
            gp::Graphics gr(mem);
            renderToolbar(gr, r.right, r.bottom, hover, collapsed);
        }
        BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(image);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        RECT r;
        GetClientRect(h, &r);
        int next = atTool(static_cast<int>(GET_Y_LPARAM(l) / scale), static_cast<int>(r.bottom / scale));
        if (next != hover) {
            hover = next;
            InvalidateRect(h, nullptr, FALSE);
        }
        TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
        TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE:
        hover = -3;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        RECT r;
        GetClientRect(h, &r);
        int id = atTool(static_cast<int>(GET_Y_LPARAM(l) / scale), static_cast<int>(r.bottom / scale));
        if (id == -1) {
            collapsed = !collapsed;
            WritePrivateProfileStringW(L"toolbar", L"collapsed", collapsed ? L"1" : L"0", ini().c_str());
        } else if (id == -2)
            menu(h, -1);
        else
            PostMessageW(launcher, ACTION_MESSAGE, tools[order[id]].action, 0);
        return 0;
    }
    case WM_RBUTTONUP: {
        RECT r;
        GetClientRect(h, &r);
        menu(h, atTool(static_cast<int>(GET_Y_LPARAM(l) / scale), static_cast<int>(r.bottom / scale)));
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

void drawEditor(gp::Graphics &gr, int w, int h) {
    gr.Clear(gp::Color(0xff010203));
    gr.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    std::lock_guard<std::mutex> g(lock);
    gp::SolidBrush card{gp::Color(0xf0171a2a)};
    art::fillRound(gr, gp::RectF(8, 8, static_cast<float>(w) - 16, 76), 14, card);
    drawText(gr, i18n::text(L"VELOX  /  CONTROL STUDIO"), gp::RectF(12, 14, static_cast<float>(w) - 24, 24), 15, 0xffbdadff, true);
    drawText(gr,
             choosingKey   ? i18n::text(L"Press a key · Alt/Win/Esc/F8/F10 reserved")
             : choosingEnd ? i18n::text(L"Click the swipe destination")
                           : i18n::text(L"Click to add · Drag to position · Right-click for options"),
             gp::RectF(12, 42, static_cast<float>(w) - 24, 30), 11, C_TEXT);
    for (size_t i = 0; i < current.bindings.size(); i++) {
        auto &b = current.bindings[i];
        float x = b.x * w, y = b.y * h;
        gp::SolidBrush fill{gp::Color(i == static_cast<size_t>(selected) ? 0xff7657ed : 0xff252b43)};
        gp::Pen edge{gp::Color(0xffc7b8ff), 2};
        gr.FillEllipse(&fill, x - 25, y - 25, 50.f, 50.f);
        gr.DrawEllipse(&edge, x - 25, y - 25, 50.f, 50.f);
        if (b.kind == Kind::Stick || b.kind == Kind::PadLeft || b.kind == Kind::PadRight) {
            gp::Pen ring{gp::Color(0xaa9678ff), 2};
            float radius = b.radius * std::min(w, h);
            gr.DrawEllipse(&ring, x - radius, y - radius, radius * 2, radius * 2);
        }
        if (b.kind == Kind::Swipe) {
            gp::Pen line{gp::Color(C_ACCENT), 3};
            line.SetEndCap(gp::LineCapArrowAnchor);
            gr.DrawLine(&line, x, y, b.x2 * w, b.y2 * h);
        }
        drawText(gr,
                 b.kind == Kind::Stick      ? L"WASD"
                 : b.kind == Kind::Aim      ? L"AIM"
                 : b.kind == Kind::PadLeft  ? L"PAD L"
                 : b.kind == Kind::PadRight ? L"PAD R"
                                            : keyName(b.key),
                 gp::RectF(x - 30, y - 20, 60, 40), 11, C_TEXT, true);
    }
    art::fillRound(gr, gp::RectF(8, static_cast<float>(h) - 52, static_cast<float>(w) - 16, 44), 12, card);
    drawText(gr, i18n::text(L"Esc / F10: save and close  ·  Changes save automatically"),
             gp::RectF(12, static_cast<float>(h) - 48, static_cast<float>(w) - 24, 36), 11, C_TEXT);
}
void editorMenu(HWND h) {
    std::lock_guard<std::mutex> g(lock);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, i18n::text(L"Add tap binding"));
    AppendMenuW(m, MF_STRING, 2, i18n::text(L"Add WASD joystick"));
    AppendMenuW(m, MF_STRING, 3, i18n::text(L"Add swipe binding"));
    AppendMenuW(m, MF_STRING, 4, i18n::text(L"Add mouse aim (F8)"));
    AppendMenuW(m, MF_STRING, 20, i18n::text(L"Add controller left stick"));
    AppendMenuW(m, MF_STRING, 21, i18n::text(L"Add controller right stick"));
    HMENU pads = CreatePopupMenu();
    for (int i = 0; i < controllers::Count; i++)
        AppendMenuW(pads, MF_STRING, 100 + i, controllers::buttonName(i));
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(pads), i18n::text(L"Add controller button"));
    if (selected >= 0) {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, 5, i18n::text(L"Change keyboard binding"));
        AppendMenuW(m, MF_STRING, 6, i18n::text(L"Bind left mouse button"));
        AppendMenuW(m, MF_STRING, 7, i18n::text(L"Bind right mouse button"));
        if (current.bindings[selected].kind == Kind::Swipe)
            AppendMenuW(m, MF_STRING, 8, i18n::text(L"Set swipe destination"));
        AppendMenuW(m, MF_STRING, 9, i18n::text(L"Delete binding"));
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 10, current.enabled ? i18n::text(L"Disable mapped controls") : i18n::text(L"Enable mapped controls"));
    AppendMenuW(m, MF_STRING, 11, i18n::text(L"Save and close"));
    POINT p;
    GetCursorPos(&p);
    POINT local = p;
    ScreenToClient(h, &local);
    RECT r;
    GetClientRect(h, &r);
    int result = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, p.x, p.y, 0, h, nullptr);
    DestroyMenu(m);
    if (((result >= 1 && result <= 4) || result == 20 || result == 21 || (result >= 100 && result < 100 + controllers::Count)) &&
        current.bindings.size() < 14) {
        Kind kind = result == 20    ? Kind::PadLeft
                    : result == 21  ? Kind::PadRight
                    : result >= 100 ? Kind::Tap
                    : result == 1   ? Kind::Tap
                    : result == 2   ? Kind::Stick
                    : result == 3   ? Kind::Swipe
                                    : Kind::Aim;
        bool duplicate = false;
        for (auto &b : current.bindings)
            if ((kind == Kind::Aim || kind == Kind::Stick || kind == Kind::PadLeft || kind == Kind::PadRight) && b.kind == kind)
                duplicate = true;
        if (!duplicate) {
            Binding b;
            b.kind = kind;
            if (result >= 100)
                b.key = 256 + result - 100;
            b.x = std::clamp(local.x / static_cast<float>(r.right), .06f, .94f);
            b.y = std::clamp(local.y / static_cast<float>(r.bottom), .12f, .88f);
            b.x2 = std::min(.95f, b.x + .2f);
            b.y2 = b.y;
            current.bindings.push_back(b);
            selected = static_cast<int>(current.bindings.size() - 1);
            choosingKey = result < 100 && (kind == Kind::Tap || kind == Kind::Swipe);
        }
    }
    if (selected >= 0) {
        auto &b = current.bindings[selected];
        if (result == 5)
            choosingKey = true;
        if (result == 6)
            b.key = VK_LBUTTON;
        if (result == 7)
            b.key = VK_RBUTTON;
        if (result == 8)
            choosingEnd = true;
        if (result == 9) {
            current.bindings.erase(current.bindings.begin() + selected);
            selected = -1;
        }
    }
    if (result == 10)
        current.enabled = !current.enabled;
    persist();
    InvalidateRect(h, nullptr, FALSE);
    if (result == 11)
        PostMessageW(h, WM_CLOSE, 0, 0);
}
LRESULT CALLBACK editorProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP b = CreateCompatibleBitmap(dc, r.right, r.bottom);
        auto old = SelectObject(mem, b);
        {
            gp::Graphics gr(mem);
            drawEditor(gr, r.right, r.bottom);
        }
        BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(b);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (editorTakesFocus)
            SetFocus(h);
        std::lock_guard<std::mutex> g(lock);
        RECT r;
        GetClientRect(h, &r);
        float x = GET_X_LPARAM(l) / static_cast<float>(r.right), y = GET_Y_LPARAM(l) / static_cast<float>(r.bottom);
        if (choosingEnd && selected >= 0) {
            current.bindings[selected].x2 = x;
            current.bindings[selected].y2 = y;
            choosingEnd = false;
            persist();
        } else {
            selected = -1;
            for (size_t i = 0; i < current.bindings.size(); i++)
                if (std::hypot((current.bindings[i].x - x) * r.right, (current.bindings[i].y - y) * r.bottom) < 30) {
                    selected = static_cast<int>(i);
                    break;
                }
            if (selected < 0 && y > 0.12f && GET_Y_LPARAM(l) < r.bottom - 60 && current.bindings.size() < 14) {
                Binding b;
                b.x = x;
                b.y = y;
                current.bindings.push_back(b);
                selected = static_cast<int>(current.bindings.size() - 1);
                choosingKey = true;
            }
            dragging = selected;
            SetCapture(h);
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (w & MK_LBUTTON) {
            std::lock_guard<std::mutex> g(lock);
            if (dragging >= 0) {
                RECT r;
                GetClientRect(h, &r);
                current.bindings[dragging].x = std::clamp(GET_X_LPARAM(l) / static_cast<float>(r.right), .02f, .98f);
                current.bindings[dragging].y = std::clamp(GET_Y_LPARAM(l) / static_cast<float>(r.bottom), .02f, .98f);
                InvalidateRect(h, nullptr, FALSE);
            }
        }
        return 0;
    case WM_LBUTTONUP: {
        ReleaseCapture();
        std::lock_guard<std::mutex> g(lock);
        dragging = -1;
        persist();
        return 0;
    }
    case WM_RBUTTONUP: {
        {
            std::lock_guard<std::mutex> guard(lock);
            RECT rect;
            GetClientRect(h, &rect);
            selected = -1;
            for (size_t i = 0; i < current.bindings.size(); i++)
                if (std::hypot(current.bindings[i].x * rect.right - GET_X_LPARAM(l),
                               current.bindings[i].y * rect.bottom - GET_Y_LPARAM(l)) < 30) {
                    selected = static_cast<int>(i);
                    break;
                }
        }
        editorMenu(h);
        return 0;
    }
    case WM_KEYDOWN: {
        std::lock_guard<std::mutex> g(lock);
        if (w == VK_ESCAPE || w == VK_F10) {
            if (choosingKey || choosingEnd) {
                choosingKey = choosingEnd = false;
            } else
                PostMessageW(h, WM_CLOSE, 0, 0);
        } else if (choosingKey && selected >= 0 && w > 0 && w < 256 && !reservedKey(static_cast<int>(w))) {
            bool duplicate = false;
            for (size_t i = 0; i < current.bindings.size(); i++)
                if (static_cast<int>(i) != selected && current.bindings[i].kind != Kind::Aim && current.bindings[i].kind != Kind::Stick &&
                    current.bindings[i].key == static_cast<int>(w))
                    duplicate = true;
            if (!duplicate) {
                current.bindings[selected].key = static_cast<int>(w);
                choosingKey = false;
                persist();
            }
        } else if (w == VK_DELETE && selected >= 0) {
            current.bindings.erase(current.bindings.begin() + selected);
            selected = -1;
            persist();
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_CLOSE: {
        std::lock_guard<std::mutex> g(lock);
        persist();
        editingNow = false;
        selected = dragging = -1;
        choosingKey = choosingEnd = false;
        DestroyWindow(h);
        editor = nullptr;
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}
void findWindows() {
    DWORD pid = emulatorPid();
    bool samePhone = pid == phonePid && emu && IsWindow(emu) && display && IsWindow(display);
    static ULONGLONG nextScan = 0;
    if (samePhone && stockBar && IsWindow(stockBar))
        return;
    if (samePhone && GetTickCount64() < nextScan)
        return;
    nextScan = GetTickCount64() + 250;
    if (!samePhone) {
        phonePid = pid;
        focusEmu = nullptr;
        stockWasVisible = false;
        nativeControls = false;
        isFullscreen = false;
        fitApplied = {};
        fitPending = {};
        effectivePercent = 0;
        emu = display = nullptr;
    }
    stockBar = nullptr;
    if (!pid)
        return;
    struct C {
        DWORD pid;
        HWND main, display, stock;
    } c{pid, emu, display, nullptr};
    EnumWindows(
        [](HWND w, LPARAM l) -> BOOL {
            auto &c = *reinterpret_cast<C *>(l);
            DWORD p = 0;
            GetWindowThreadProcessId(w, &p);
            if (p != c.pid)
                return TRUE;
            wchar_t title[160]{};
            GetWindowTextW(w, title, 160);
            if (wcsncmp(title, L"Android Emulator - Gaming_Phone:", 30) != 0)
                return TRUE;
            c.main = w;
            EnumChildWindows(
                w,
                [](HWND child, LPARAM target) -> BOOL {
                    wchar_t name[80]{};
                    GetClassNameW(child, name, 80);
                    if (wcscmp(name, L"subWin") != 0)
                        return TRUE;
                    *reinterpret_cast<HWND *>(target) = child;
                    return FALSE;
                },
                reinterpret_cast<LPARAM>(&c.display));
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&c));
    emu = c.main;
    display = c.display;
    if (!emu)
        return;
    // Qt calls both the side toolbar and extended controls "Emulator". Match
    // our phone's owned, narrow tool window; never hide its settings dialog.
    EnumWindows(
        [](HWND w, LPARAM l) -> BOOL {
            auto &c = *reinterpret_cast<C *>(l);
            DWORD p = 0;
            GetWindowThreadProcessId(w, &p);
            if (p != c.pid || GetWindow(w, GW_OWNER) != c.main)
                return TRUE;
            wchar_t cls[80]{}, title[160]{};
            GetClassNameW(w, cls, 80);
            GetWindowTextW(w, title, 160);
            if (wcsncmp(cls, L"Qt", 2) != 0 || (wcscmp(title, i18n::text(L"Emulator")) != 0 && wcscmp(title, L"Android Emulator") != 0))
                return TRUE;
            RECT r{};
            if (!GetWindowRect(w, &r))
                return TRUE;
            int width = r.right - r.left, height = r.bottom - r.top;
            int maximumWidth = MulDiv(120, GetDpiForWindow(c.main), 96);
            if (width > 0 && width <= maximumWidth && height > width * 2) {
                c.stock = w;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&c));
    stockBar = c.stock;
}

// Qt owns the phone's aspect ratio. Refit only on an environment/policy change,
// after its geometry settles, rather than sending competing resize requests each tick.
void fitPhoneWindow() {
    if (!emu || !display || isFullscreen || IsZoomed(emu))
        return;
    DWORD windowPid = 0, displayPid = 0;
    GetWindowThreadProcessId(emu, &windowPid);
    GetWindowThreadProcessId(display, &displayPid);
    if (!phonePid || windowPid != phonePid || displayPid != phonePid || !IsChild(emu, display))
        return;
    RECT window{}, screenRect{};
    if (!GetWindowRect(emu, &window) || !GetWindowRect(display, &screenRect))
        return;
    int screenW = screenRect.right - screenRect.left, screenH = screenRect.bottom - screenRect.top;
    if (screenW < 40 || screenH < 40)
        return;
    auto monitor = scaling::desktop(emu);
    const auto &render = displays()[std::clamp(running.display, 0, int(displays().size()) - 1)];
    bool landscape = screenW > screenH;
    int renderW = landscape ? std::max(render.w, render.h) : std::min(render.w, render.h);
    int renderH = landscape ? std::min(render.w, render.h) : std::max(render.w, render.h);
    effectivePercent = int(std::lround(100.0 * screenW / renderW));
    std::array<INT_PTR, 10> key{reinterpret_cast<INT_PTR>(emu),
                                monitor.work.left,
                                monitor.work.top,
                                monitor.work.right,
                                monitor.work.bottom,
                                monitor.dpi,
                                renderW,
                                renderH,
                                requestedPercent,
                                (showBar ? 1 : 0) | (nativeControls ? 2 : 0) | (leftDock ? 4 : 0)};
    if (key == fitApplied)
        return;
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    if (GetGUIThreadInfo(GetWindowThreadProcessId(emu, nullptr), &info) && (info.flags & GUI_INMOVESIZE)) {
        fitAfter = GetTickCount64() + 350;
        return;
    }
    if (key != fitPending || !EqualRect(&fitGeometry, &window)) {
        fitPending = key;
        fitGeometry = window;
        fitAfter = GetTickCount64() + 350;
        return;
    }
    if (GetTickCount64() < fitAfter)
        return;
    int toolbar = showBar ? int(std::lround(61 * monitor.dpi / 96.f)) : 0;
    if (nativeControls && stockBar && IsWindowVisible(stockBar)) {
        RECT native{};
        if (GetWindowRect(stockBar, &native))
            toolbar += native.right - native.left + MulDiv(7, monitor.dpi, 96);
    }
    // Skin borders scale with the viewport; native caption/borders use physical DPI.
    RECT frame = scaling::outer(emu, 0, 0, monitor.dpi);
    int frameW = std::max(0L, frame.right - frame.left), frameH = std::max(0L, frame.bottom - frame.top);
    double ratioW = std::max(1.0, double(window.right - window.left - frameW) / screenW);
    double ratioH = std::max(1.0, double(window.bottom - window.top - frameH) / screenH);
    int availableW = std::max(1, int((monitor.width() - toolbar - frameW) / ratioW));
    int availableH = std::max(1, int((monitor.height() - frameH) / ratioH));
    int percent = scaling::phone(availableW, availableH, renderW, renderH, requestedPercent);
    int width = int(std::lround(renderW * percent / 100.0 * ratioW)) + frameW;
    int height = int(std::lround(renderH * percent / 100.0 * ratioH)) + frameH;
    int leftSpace = leftDock && showBar ? int(std::lround(61 * monitor.dpi / 96.f)) : 0;
    RECT anchor = window;
    anchor.left -= leftSpace;
    auto position = scaling::position(anchor, monitor, width + toolbar, height);
    position.x += leftSpace;
    fitApplied = key;
    if (std::abs(window.right - window.left - width) > 2 || std::abs(window.bottom - window.top - height) > 2 ||
        window.left != position.x || window.top != position.y)
        SetWindowPos(emu, nullptr, position.x, position.y, width, height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

} // namespace

void windowScale(int percent) {
    percent = scaling::preference(percent, 10, 100);
    if (percent != requestedPercent) {
        requestedPercent = percent;
        fitApplied = {};
    }
}
int windowPercent() {
    return state == State::Running && IsWindow(emu) ? effectivePercent : 0;
}

std::wstring keyName(int key) {
    if (key >= 256 && key < 256 + controllers::Count) {
        static const wchar_t *names[] = {L"PAD A",  L"PAD B",  L"PAD X",  L"PAD Y",  L"PAD LB", L"PAD RB", L"BACK",   L"START",
                                         L"PAD LS", L"PAD RS", L"PAD UP", L"PAD DN", L"PAD LT", L"PAD RT", L"PAD L2", L"PAD R2"};
        return names[key - 256];
    }
    if (key == VK_LBUTTON)
        return L"LMB";
    if (key == VK_RBUTTON)
        return L"RMB";
    UINT code = MapVirtualKeyW(key, MAPVK_VK_TO_VSC);
    wchar_t name[64];
    if (GetKeyNameTextW(code << 16, name, 64))
        return name;
    return i18n::text(L"Key ") + std::to_wstring(key);
}
Profile profile() {
    std::lock_guard<std::mutex> g(lock);
    return current;
}
std::wstring status() {
    std::lock_guard<std::mutex> g(lock);
    return message;
}
void release() {
    std::lock_guard<std::mutex> g(lock);
    cancel();
}
bool editing() {
    std::lock_guard<std::mutex> g(lock);
    return editingNow;
}
bool mouseCaptured() {
    return aimLocked;
}
void enabled(bool on) {
    std::lock_guard<std::mutex> g(lock);
    cancel();
    current.enabled = on;
    persist();
}
namespace {
Profile readProfile(const std::wstring &package, bool landscape) {
    Profile out;
    out.package = package;
    out.landscape = landscape;
    auto file = dataFolder() + L"\\controls\\" + package + (landscape ? L"-landscape.ini" : L"-portrait.ini");
    out.padMode = std::clamp(int(GetPrivateProfileIntW(L"controls", L"padmode", 0, file.c_str())), 0, 2);
    out.padLayout = std::clamp(int(GetPrivateProfileIntW(L"controls", L"padlayout", 0, file.c_str())), 0, 3);
    wchar_t device[128]{};
    GetPrivateProfileStringW(L"controls", L"paddevice", L"", device, 128, file.c_str());
    out.padDevice = device;
    auto number = [&](const wchar_t *key, float fallback, float low, float high) {
        wchar_t value[64]{};
        GetPrivateProfileStringW(L"controls", key, L"", value, 64, file.c_str());
        float n = *value ? wcstof(value, nullptr) : fallback;
        return std::isfinite(n) ? std::clamp(n, low, high) : fallback;
    };
    out.deadzone = number(L"deadzone", .18f, .05f, .4f);
    out.sensitivity = number(L"sensitivity", 1.f, .5f, 2.f);
    out.invertY = GetPrivateProfileIntW(L"controls", L"inverty", 0, file.c_str()) != 0;
    out.enabled = GetPrivateProfileIntW(L"controls", L"enabled", 0, file.c_str()) != 0;
    for (int i = 0; i < 14; i++) {
        wchar_t value[256];
        GetPrivateProfileStringW(L"controls", (L"binding" + std::to_wstring(i)).c_str(), L"", value, 256, file.c_str());
        if (!*value)
            break;
        std::wstring s = value;
        std::replace(s.begin(), s.end(), L',', L' ');
        std::wistringstream in(s);
        int kind;
        Binding b;
        if (!(in >> kind >> b.key >> b.x >> b.y >> b.x2 >> b.y2 >> b.radius) || kind < 0 || kind > 5 || b.key < 1 ||
            b.key >= 256 + controllers::Count || reservedKey(b.key) || !std::isfinite(b.x + b.y + b.x2 + b.y2 + b.radius))
            continue;
        b.kind = static_cast<Kind>(kind);
        b.x = std::clamp(b.x, 0.f, 1.f);
        b.y = std::clamp(b.y, 0.f, 1.f);
        b.x2 = std::clamp(b.x2, 0.f, 1.f);
        b.y2 = std::clamp(b.y2, 0.f, 1.f);
        b.radius = std::clamp(b.radius, .02f, .3f);
        out.bindings.push_back(b);
    }
    return out;
}
} // namespace
void selectProfile(const std::wstring &package, bool landscape) {
    if (!safePackage(package))
        return;
    std::lock_guard<std::mutex> g(lock);
    if (editingNow || (current.package == package && current.landscape == landscape))
        return;
    cancel();
    current = Profile{};
    current.package = package;
    current.landscape = landscape;
    current = readProfile(package, landscape);
    message = current.bindings.empty() ? i18n::text(L"No bindings yet. Open Control Studio to add them.")
                                       : i18n::text(L"Controls loaded for this game.");
}

namespace {
void layoutNeed(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
void layoutPlain(const std::wstring &path) {
    std::filesystem::path built;
    for (const auto &part : std::filesystem::absolute(path)) {
        built /= part;
        auto a = GetFileAttributesW(built.c_str());
        layoutNeed(a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_REPARSE_POINT),
                   "Control files cannot use links or cloud placeholders.");
    }
}
struct Layout {
    std::wstring package;
    presets::Preset preset;
    std::array<Profile, 2> profiles;
};
std::string controlsText(const Profile &p) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "[controls]\nenabled=" << p.enabled << "\npadmode=" << p.padMode << "\npadlayout=" << p.padLayout
        << "\npaddevice=\ndeadzone=" << p.deadzone << "\nsensitivity=" << p.sensitivity << "\ninverty=" << p.invertY << '\n';
    for (size_t i = 0; i < p.bindings.size(); ++i) {
        auto &b = p.bindings[i];
        out << "binding" << i << '=' << int(b.kind) << ',' << b.key << ',' << b.x << ',' << b.y << ',' << b.x2 << ',' << b.y2 << ','
            << b.radius << '\n';
    }
    return out.str();
}
std::string presetText(const presets::Preset &v) {
    return "[preset]\nenabled=" + std::to_string(v.enabled) + "\ndisplay=" + std::to_string(v.display) +
           "\nrate=" + std::to_string(v.rate) + "\ncores=" + std::to_string(v.cores) + "\nmemory=" + std::to_string(v.memory) +
           "\ngpu=" + std::to_string(v.gpu) + "\nfullscreen=" + std::to_string(v.fullscreen) + "\n";
}
Layout parseLayout(const std::wstring &file) {
    layoutPlain(file);
    layoutNeed(std::filesystem::is_regular_file(file) && std::filesystem::file_size(file) <= 65536,
               "Choose a Velox controls file smaller than 64 KB.");
    std::ifstream in(std::filesystem::path(file), std::ios::binary);
    in.imbue(std::locale::classic());
    std::string tag, package;
    int version = 0;
    Layout l;
    layoutNeed(bool(in >> tag >> version) && tag == "velox-controls" && version == 1, "Unsupported Velox controls format.");
    layoutNeed(bool(in >> tag >> package) && tag == "package", "The controls file has no game identity.");
    l.package = widen(package);
    layoutNeed(safePackage(l.package), "Invalid Android package in controls file.");
    auto &p = l.preset;
    int on = 0;
    layoutNeed(bool(in >> tag >> on >> p.display >> p.rate >> p.cores >> p.memory >> p.gpu >> p.fullscreen) && tag == "preset" && on >= 0 &&
                   on <= 1,
               "Invalid game preset.");
    p.enabled = on;
    auto choice = [](int n, const std::vector<int> &options) { return std::find(options.begin(), options.end(), n) != options.end(); };
    layoutNeed(p.display >= -1 && p.display < int(displays().size()) && (p.rate == 0 || choice(p.rate, refreshRates())) &&
                   (p.cores == -1 || choice(p.cores, coreChoices())) && (p.memory == -1 || choice(p.memory, memoryChoices())) &&
                   p.gpu >= -1 && p.gpu <= 2 && p.fullscreen >= -1 && p.fullscreen <= 1,
               "The preset contains unsupported values.");
    for (int orientation = 0; orientation < 2; ++orientation) {
        auto &profile = l.profiles[orientation];
        int direction = -1, enabled = 0, invert = 0, count = 0;
        layoutNeed(bool(in >> tag >> direction >> enabled >> profile.padMode >> profile.padLayout >> profile.deadzone >>
                        profile.sensitivity >> invert >> count) &&
                       tag == "profile" && direction == orientation && enabled >= 0 && enabled <= 1 && invert >= 0 && invert <= 1 &&
                       profile.padMode >= 0 && profile.padMode <= 2 && profile.padLayout >= 0 && profile.padLayout <= 3 &&
                       std::isfinite(profile.deadzone) && profile.deadzone >= .05f && profile.deadzone <= .4f &&
                       std::isfinite(profile.sensitivity) && profile.sensitivity >= .5f && profile.sensitivity <= 2 && count >= 0 &&
                       count <= 14,
                   "Invalid portrait or landscape controller profile.");
        profile.package = l.package;
        profile.landscape = orientation;
        profile.enabled = enabled;
        profile.invertY = invert;
        std::set<int> keys;
        for (int n = 0; n < count; ++n) {
            int kind = -1;
            Binding b;
            layoutNeed(bool(in >> tag >> kind >> b.key >> b.x >> b.y >> b.x2 >> b.y2 >> b.radius) && tag == "binding" && kind >= 0 &&
                           kind <= 5 && b.key >= 1 && b.key < 256 + controllers::Count && !reservedKey(b.key) &&
                           keys.insert(b.key).second && std::isfinite(b.x + b.y + b.x2 + b.y2 + b.radius) && b.x >= 0 && b.x <= 1 &&
                           b.y >= 0 && b.y <= 1 && b.x2 >= 0 && b.x2 <= 1 && b.y2 >= 0 && b.y2 <= 1 && b.radius >= .02f && b.radius <= .3f,
                       "Invalid or duplicate control binding.");
            b.kind = static_cast<Kind>(kind);
            profile.bindings.push_back(b);
        }
    }
    layoutNeed(bool(in >> tag) && tag == "end" && !(in >> tag), "The controls file has unexpected trailing data.");
    return l;
}
void layoutWrite(const std::wstring &file, const std::string &bytes) {
    layoutPlain(file);
    createDirs(std::filesystem::path(file).parent_path().wstring());
    std::ofstream out(std::filesystem::path(file), std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), bytes.size());
    out.close();
    layoutNeed(out.good(), "Couldn't write controls. Check free space and folder access.");
}
} // namespace

namespace {
struct LayoutJournal {
    std::wstring folder, package;
    unsigned had = 0;
    int state = 0;
};
std::wstring layoutJournalFile() {
    return dataFolder() + L"\\layout-last.txt";
}
std::array<std::wstring, 3> layoutTargets(const std::wstring &package) {
    return {dataFolder() + L"\\controls\\" + package + L"-portrait.ini", dataFolder() + L"\\controls\\" + package + L"-landscape.ini",
            dataFolder() + L"\\presets\\" + package + L".ini"};
}
LayoutJournal layoutJournal() {
    auto file = layoutJournalFile();
    layoutPlain(file);
    if (!exists(file))
        return {};
    layoutNeed(std::filesystem::file_size(file) < 1024, "The previous controls import journal is invalid.");
    std::ifstream in{std::filesystem::path(file)};
    std::string magic, folder, package, extra;
    LayoutJournal j;
    layoutNeed(bool(in >> magic >> folder >> package >> j.had >> j.state) && magic == "velox-layout-import-1" && !(in >> extra) &&
                   folder.size() < 64 && !folder.empty() && folder.find_first_not_of("0123456789-") == std::string::npos &&
                   safePackage(widen(package)) && j.had <= 7 && j.state >= 0 && j.state <= 2,
               "The previous controls import journal is invalid.");
    j.folder = widen(folder);
    j.package = widen(package);
    return j;
}
void saveLayoutJournal(const LayoutJournal &j) {
    auto file = layoutJournalFile();
    auto fresh = file + L".new";
    layoutWrite(fresh, "velox-layout-import-1\n" + narrow(j.folder) + "\n" + narrow(j.package) + "\n" + std::to_string(j.had) + "\n" +
                           std::to_string(j.state) + "\n");
    layoutPlain(file);
    layoutNeed(MoveFileExW(fresh.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
               "Couldn't save the controls import journal.");
}
void revertLayoutJournal(LayoutJournal j) {
    layoutNeed(!j.folder.empty() && j.state != 2, "There is no controls import to undo.");
    auto history = dataFolder() + L"\\layout-history\\" + j.folder;
    auto targets = layoutTargets(j.package);
    layoutPlain(history);
    for (int i = 0; i < 3; ++i) {
        layoutPlain(targets[i]);
        auto original = history + L"\\" + std::to_wstring(i) + L".ini";
        layoutPlain(original);
        if (j.had & (1u << i))
            layoutNeed(std::filesystem::is_regular_file(original) && std::filesystem::file_size(original) < (1u << 20),
                       "A previous controls file is missing. Its history was kept.");
    }
    for (int i = 0; i < 3; ++i) {
        if (j.had & (1u << i)) {
            auto fresh = targets[i] + L".undo-new";
            layoutPlain(fresh);
            layoutNeed(CopyFileW((history + L"\\" + std::to_wstring(i) + L".ini").c_str(), fresh.c_str(), FALSE) &&
                           MoveFileExW(fresh.c_str(), targets[i].c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
                       "Couldn't restore previous controls. Their history was kept.");
        } else if (exists(targets[i]))
            layoutNeed(DeleteFileW(targets[i].c_str()), "Couldn't undo a newly imported controls file.");
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, targets[i].c_str());
    }
    j.state = 2;
    saveLayoutJournal(j);
    presets::invalidate();
    if (current.package == j.package)
        current = readProfile(j.package, current.landscape);
}
} // namespace
bool canUndoImport() {
    try {
        auto j = layoutJournal();
        return !j.folder.empty() && j.state != 2;
    } catch (...) {
        return false;
    }
}
bool undoImport(const Report &report) {
    try {
        std::lock_guard<std::mutex> guard(lock);
        layoutNeed(!editingNow, "Close Control Studio before undoing an import.");
        cancel();
        revertLayoutJournal(layoutJournal());
        report(i18n::text(L"Previous game setup restored. Import history is kept."), false);
        return true;
    } catch (const std::exception &e) {
        report(widen(e.what()), true);
        return false;
    }
}
bool exportLayout(const std::wstring &package, const std::wstring &file, const Report &report) {
    try {
        layoutNeed(safePackage(package), "Choose a game before exporting its controls.");
        std::lock_guard<std::mutex> guard(lock);
        auto p = presets::load(package);
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << "velox-controls 1\npackage " << narrow(package) << "\npreset " << p.enabled << ' ' << p.display << ' ' << p.rate << ' '
            << p.cores << ' ' << p.memory << ' ' << p.gpu << ' ' << p.fullscreen << '\n';
        for (int o = 0; o < 2; ++o) {
            auto profile = readProfile(package, o);
            out << "profile " << o << ' ' << profile.enabled << ' ' << profile.padMode << ' ' << profile.padLayout << ' '
                << profile.deadzone << ' ' << profile.sensitivity << ' ' << profile.invertY << ' ' << profile.bindings.size() << '\n';
            for (auto &b : profile.bindings)
                out << "binding " << int(b.kind) << ' ' << b.key << ' ' << b.x << ' ' << b.y << ' ' << b.x2 << ' ' << b.y2 << ' '
                    << b.radius << '\n';
        }
        out << "end\n";
        auto fresh = file + L".new-" + std::to_wstring(GetCurrentProcessId());
        layoutPlain(file);
        layoutWrite(fresh, out.str());
        parseLayout(fresh);
        layoutNeed(MoveFileExW(fresh.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
                   "Couldn't publish the controls file.");
        report(i18n::text(L"Controls and game preset exported. No accounts or device IDs are included."), false);
        return true;
    } catch (const std::exception &e) {
        report(widen(e.what()), true);
        return false;
    }
}
LayoutInfo inspectLayout(const std::wstring &file) {
    LayoutInfo out;
    try {
        auto l = parseLayout(file);
        out.package = l.package;
        out.bindings = l.profiles[0].bindings.size() + l.profiles[1].bindings.size();
        out.valid = true;
    } catch (const std::exception &e) {
        out.error = widen(e.what());
    }
    return out;
}
bool importLayout(const std::wstring &file, const std::wstring &expectedPackage, const Report &report) {
    try {
        auto previous = layoutJournal();
        layoutNeed(previous.folder.empty() || previous.state != 0, "Finish or undo the interrupted controls import first.");
        auto layout = parseLayout(file);
        layoutNeed(layout.package == expectedPackage, "The controls file changed. Review it again before importing.");
        std::lock_guard<std::mutex> guard(lock);
        layoutNeed(!editingNow, "Close Control Studio before importing a layout.");
        cancel();
        auto token = std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(GetCurrentProcessId());
        auto history = dataFolder() + L"\\layout-history\\" + token;
        layoutPlain(history);
        createDirs(history);
        std::array<std::wstring, 3> targets = {dataFolder() + L"\\controls\\" + layout.package + L"-portrait.ini",
                                               dataFolder() + L"\\controls\\" + layout.package + L"-landscape.ini",
                                               dataFolder() + L"\\presets\\" + layout.package + L".ini"};
        std::array<std::string, 3> bytes = {controlsText(layout.profiles[0]), controlsText(layout.profiles[1]), presetText(layout.preset)};
        std::array<bool, 3> had{};
        for (int i = 0; i < 3; ++i) {
            layoutPlain(targets[i]);
            had[i] = exists(targets[i]);
            if (had[i])
                layoutNeed(CopyFileW(targets[i].c_str(), (history + L"\\" + std::to_wstring(i) + L".ini").c_str(), TRUE),
                           "Couldn't preserve the previous controls.");
            layoutWrite(targets[i] + L".import-new", bytes[i]);
        }
        LayoutJournal journal{token, layout.package, 0, 0};
        for (int i = 0; i < 3; ++i)
            if (had[i])
                journal.had |= 1u << i;
        saveLayoutJournal(journal);
        int changed = 0;
        try {
            for (int i = 0; i < 3; ++i) {
                layoutNeed(MoveFileExW((targets[i] + L".import-new").c_str(), targets[i].c_str(),
                                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
                           "Couldn't activate imported controls.");
                ++changed;
                WritePrivateProfileStringW(nullptr, nullptr, nullptr, targets[i].c_str());
            }
        } catch (...) {
            for (int i = 0; i < changed; ++i) {
                if (had[i])
                    CopyFileW((history + L"\\" + std::to_wstring(i) + L".ini").c_str(), targets[i].c_str(), FALSE);
                else
                    DeleteFileW(targets[i].c_str());
            }
            throw;
        }
        journal.state = 1;
        saveLayoutJournal(journal);
        presets::invalidate();
        if (current.package == layout.package)
            current = layout.profiles[current.landscape ? 1 : 0];
        report(i18n::text(L"Controls imported. Previous layouts are kept in layout-history."), false);
        return true;
    } catch (const std::exception &e) {
        report(widen(e.what()), true);
        return false;
    }
}
void pollPhone() {
    if (state != State::Running) {
        release();
        return;
    }
    std::string focus = adb(L"shell \"dumpsys window | grep -E 'mCurrentFocus|mRotation=[0-3]'\"", 5000);
    std::smatch p, r;
    static const std::regex package("mCurrentFocus[^\n]*? ([A-Za-z][A-Za-z0-9_.]*)/[A-Za-z0-9_.$]+"), rotation("mRotation=([0-3])");
    if (std::regex_search(focus, p, package)) {
        bool landscape = std::regex_search(focus, r, rotation) && (atoi(r[1].str().c_str()) % 2 != 0);
        selectProfile(widen(p[1]), landscape);
    }
}
void initialize(HWND owner) {
    try {
        auto j = layoutJournal();
        if (!j.folder.empty() && j.state == 0)
            revertLayoutJournal(j);
    } catch (const std::exception &e) {
        message = widen(e.what());
    }
    launcher = owner;
    order.clear();
    for (size_t i = 0; i < tools.size(); i++)
        order.push_back(static_cast<int>(i));
    showBar = GetPrivateProfileIntW(L"toolbar", L"visible", 1, ini().c_str()) != 0;
    leftDock = GetPrivateProfileIntW(L"toolbar", L"left", 0, ini().c_str()) != 0;
    collapsed = GetPrivateProfileIntW(L"toolbar", L"collapsed", 0, ini().c_str()) != 0;
    wchar_t list[256];
    GetPrivateProfileStringW(L"toolbar", L"order", L"", list, 256, ini().c_str());
    if (*list) {
        std::wstring s = list;
        std::replace(s.begin(), s.end(), L',', L' ');
        std::wistringstream in(s);
        std::vector<int> parsed;
        for (int i; in >> i;)
            if (i >= 0 && i < static_cast<int>(tools.size()) && std::find(parsed.begin(), parsed.end(), i) == parsed.end())
                parsed.push_back(i);
        for (int i : order)
            if (std::find(parsed.begin(), parsed.end(), i) == parsed.end())
                parsed.push_back(i);
        order = parsed;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpfnWndProc = barProc;
    wc.lpszClassName = L"VeloxToolbar";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = editorProc;
    wc.lpszClassName = L"VeloxControlStudio";
    RegisterClassExW(&wc);
    quitting = false;
    inputThread = std::thread(inputLoop);
}
void manageHooks(bool needed) {
    if (needed) {
        if (!keyboardHook)
            keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, keyProc, GetModuleHandleW(nullptr), 0);
        if (!mouseHook)
            mouseHook = SetWindowsHookExW(WH_MOUSE_LL, mouseProc, GetModuleHandleW(nullptr), 0);
    } else {
        if (keyboardHook)
            UnhookWindowsHookEx(keyboardHook);
        if (mouseHook)
            UnhookWindowsHookEx(mouseHook);
        keyboardHook = mouseHook = nullptr;
    }
}
void tick() {
    findWindows();
    focusEmu = emu;
    if (bar && (!IsWindow(bar) || GetWindow(bar, GW_OWNER) != emu)) {
        if (IsWindow(bar))
            DestroyWindow(bar);
        bar = nullptr;
        tip = nullptr;
    }
    if (editor && !IsWindow(editor)) {
        editor = nullptr;
        std::lock_guard<std::mutex> guard(lock);
        editingNow = false;
        cancel();
    }
    if (!display || !emu || state != State::Running || !IsWindowVisible(emu) || IsIconic(emu)) {
        if (bar)
            ShowWindow(bar, SW_HIDE);
        if (editor)
            PostMessageW(editor, WM_CLOSE, 0, 0);
        if (stockBar && IsWindowVisible(stockBar)) {
            stockWasVisible = true;
            ShowWindowAsync(stockBar, SW_HIDE);
        }
        manageHooks(false);
        release();
        return;
    }
    fitPhoneWindow();
    if (nativeControls && stockBar && !IsWindowVisible(stockBar))
        ShowWindowAsync(stockBar, SW_SHOWNOACTIVATE);
    RECT r;
    GetClientRect(display, &r);
    POINT origin{0, 0};
    ClientToScreen(display, &origin);
    OffsetRect(&r, origin.x, origin.y);
    {
        std::lock_guard<std::mutex> g(lock);
        viewport = r;
    }
    bool mapped = false;
    {
        std::lock_guard<std::mutex> guard(lock);
        mapped = current.enabled && !editingNow;
    }
    manageHooks(mapped);
    if (!showBar) {
        if (bar)
            ShowWindow(bar, SW_HIDE);
        if (stockBar && stockWasVisible)
            ShowWindowAsync(stockBar, SW_SHOWNOACTIVATE);
        return;
    }
    scale = GetDpiForWindow(emu) / 96.f;
    int width = static_cast<int>(54 * scale),
        height = collapsed ? static_cast<int>(86 * scale)
                           : std::min<int>(static_cast<int>(r.bottom - r.top), static_cast<int>((88 + tools.size() * 38) * scale));
    if (!bar) {
        bar = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"VeloxToolbar", i18n::text(L"Velox controls"), WS_POPUP, 0, 0, width,
                              height, emu, nullptr, GetModuleHandleW(nullptr), nullptr);
        BOOL dark = TRUE;
        DwmSetWindowAttribute(bar, 20, &dark, sizeof(dark));
        int corner = 2;
        DwmSetWindowAttribute(bar, 33, &corner, sizeof(corner));
    }
    RECT mainRect;
    GetWindowRect(emu, &mainRect);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(emu, MONITOR_DEFAULTTONEAREST), &mi);
    int x = leftDock ? mainRect.left - width - 7 : mainRect.right + 7;
    if (x + width > mi.rcWork.right)
        x = mainRect.left - width - 7;
    if (x < mi.rcWork.left)
        x = mi.rcWork.left;
    int y = std::clamp<int>(r.top, mi.rcWork.top, std::max<LONG>(mi.rcWork.top, mi.rcWork.bottom - height));
    if (nativeControls && stockBar && IsWindowVisible(stockBar)) {
        RECT native{}, proposed{x, y, x + width, y + height}, overlap{};
        if (GetWindowRect(stockBar, &native) && IntersectRect(&overlap, &native, &proposed)) {
            x = native.right + 7 + width <= mi.rcWork.right ? native.right + 7 : mainRect.left - width - 7;
            x = std::clamp<int>(x, mi.rcWork.left, std::max<LONG>(mi.rcWork.left, mi.rcWork.right - width));
        }
    }
    RECT previous{};
    GetWindowRect(bar, &previous);
    if (!IsWindowVisible(bar) || previous.left != x || previous.top != y || previous.right - previous.left != width ||
        previous.bottom - previous.top != height)
        SetWindowPos(bar, emu, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    static std::wstring tooltipLayout;
    std::wstring nextLayout = std::to_wstring(width) + L":" + std::to_wstring(height) + L":" + (collapsed ? L"1" : L"0");
    nextLayout += recording() ? L":recording" : L":idle";
    nextLayout += L":" + std::to_wstring(i18n::language());
    for (int i : order)
        nextLayout += L":" + std::to_wstring(i);
    if (!tip) {
        tooltipLayout.clear();
        tip = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0,
                              0, bar, nullptr, GetModuleHandleW(nullptr), nullptr);
        SetWindowTheme(tip, L"DarkMode_Explorer", nullptr);
        SendMessageW(tip, TTM_SETMAXTIPWIDTH, 0, 240);
    }
    if (nextLayout != tooltipLayout) {
        InvalidateRect(bar, nullptr, FALSE);
        tooltipLayout = nextLayout;
        for (UINT_PTR i = 1; i <= tools.size() + 2; i++) {
            TOOLINFOW t{};
            t.cbSize = sizeof(t);
            t.hwnd = bar;
            t.uId = i;
            SendMessageW(tip, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&t));
        }
        int n = visibleTools(static_cast<int>(height / scale));
        for (int i = -1; i <= n; i++) {
            TOOLINFOW t{};
            t.cbSize = sizeof(t);
            t.hwnd = bar;
            t.uId = i + 2;
            t.uFlags = TTF_SUBCLASS | TTF_TRANSPARENT;
            int top = i == -1 ? 0 : 48 + i * 38;
            t.rect = {0, static_cast<LONG>(top * scale), width, static_cast<LONG>((top + (i == -1 ? 46 : 36)) * scale)};
            t.lpszText = const_cast<wchar_t *>(i18n::text(i == -1  ? i18n::text(L"Click to collapse · Right-click to customize")
                                                          : i == n ? i18n::text(L"More controls")
                                                          : tools[order[i]].action == Record && recording() ? i18n::text(L"Stop recording")
                                                                                                            : tools[order[i]].name));
            SendMessageW(tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&t));
        }
    }
    if (stockBar && !nativeControls && IsWindowVisible(stockBar)) {
        stockWasVisible = true;
        ShowWindowAsync(stockBar, SW_HIDE);
    }
    if (editor) {
        RECT client{}, previous{};
        GetClientRect(display, &client);
        GetClientRect(editor, &previous);
        if (client.right != previous.right || client.bottom != previous.bottom)
            SetWindowPos(editor, HWND_TOP, 0, 0, client.right, client.bottom, SWP_NOACTIVATE);
    }
}
void toolbarVisible(bool on) {
    showBar = on;
    WritePrivateProfileStringW(L"toolbar", L"visible", on ? L"1" : L"0", ini().c_str());
}
bool toolbarVisible() {
    return showBar;
}
void edit(bool focus) {
    if (!display || !IsWindow(display) || state != State::Running)
        return;
    {
        std::lock_guard<std::mutex> g(lock);
        if (!safePackage(current.package) || current.package == L"app.velox.home")
            return;
        cancel();
        editingNow = true;
        editorTakesFocus = focus;
        selected = -1;
        choosingKey = choosingEnd = false;
    }
    if (editor) {
        if (focus)
            SetFocus(editor);
        return;
    }
    RECT r;
    GetClientRect(display, &r);
    auto before = SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(display));
    editor = CreateWindowExW(WS_EX_LAYERED, L"VeloxControlStudio", L"Velox Control Studio", WS_CHILD | WS_VISIBLE, 0, 0, r.right, r.bottom,
                             display, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetThreadDpiAwarenessContext(before);
    if (editor) {
        SetLayeredWindowAttributes(editor, RGB(1, 2, 3), 255, LWA_COLORKEY);
        if (focus)
            SetFocus(editor);
    } else {
        std::lock_guard<std::mutex> g(lock);
        editingNow = false;
    }
}
void focusPhone() {
    findWindows();
    if (emu) {
        ShowWindow(emu, SW_RESTORE);
        SetForegroundWindow(emu);
    }
}
bool controller(const Profile &desired) {
    std::lock_guard<std::mutex> guard(lock);
    if (!safePackage(current.package) || current.package != desired.package || current.landscape != desired.landscape)
        return false;
    cancel();
    current.padMode = std::clamp(desired.padMode, 0, 2);
    current.padDevice = desired.padDevice.size() < 100 ? desired.padDevice : L"";
    current.padLayout = std::clamp(desired.padLayout, 0, 3);
    current.deadzone = std::clamp(desired.deadzone, .05f, .4f);
    current.sensitivity = std::clamp(desired.sensitivity, .5f, 2.f);
    current.invertY = desired.invertY;
    persist();
    return true;
}
void setFullscreen(bool on) {
    if (on != isFullscreen)
        toggleFullscreen();
}
void toggleFullscreen() {
    if (!emu)
        return;
    if (isFullscreen) {
        SetWindowLongPtrW(emu, GWL_STYLE, oldStyle);
        SetWindowPlacement(emu, &oldPlacement);
        SetWindowPos(emu, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        isFullscreen = false;
        fitApplied = {};
    } else {
        oldStyle = GetWindowLongPtrW(emu, GWL_STYLE);
        GetWindowPlacement(emu, &oldPlacement);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(emu, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrW(emu, GWL_STYLE, oldStyle & ~(WS_CAPTION | WS_THICKFRAME));
        SetWindowPos(emu, nullptr, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
        isFullscreen = true;
    }
}
void shutdown() {
    if (editor)
        SendMessageW(editor, WM_CLOSE, 0, 0);
    release();
    if (keyboardHook)
        UnhookWindowsHookEx(keyboardHook);
    if (mouseHook)
        UnhookWindowsHookEx(mouseHook);
    keyboardHook = mouseHook = nullptr;
    quitting = true;
    if (inputThread.joinable())
        inputThread.join();
    if (isFullscreen && IsWindow(emu))
        toggleFullscreen();
    if (bar)
        DestroyWindow(bar);
    bar = nullptr;
    if (stockBar && stockWasVisible)
        ShowWindowAsync(stockBar, SW_SHOWNOACTIVATE);
}
void renderToolbar(gp::Graphics &gr, int width, int height, int hover, bool compact) {
    float s = width / 54.f;
    gr.Clear(gp::Color(C_SIDE));
    gr.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    gr.ScaleTransform(s, s);
    gp::SolidBrush background{gp::Color(0xff191c2c)};
    art::fillRound(gr, gp::RectF(1, 1, 52, height / s - 2), 15, background);
    art::logo(gr, 13, 9, 28);
    int n = compact ? 0 : visibleTools(static_cast<int>(height / s));
    gp::Font icon(L"Segoe MDL2 Assets", 18, gp::FontStyleRegular, gp::UnitPixel);
    gp::StringFormat fmt;
    fmt.SetAlignment(gp::StringAlignmentCenter);
    fmt.SetLineAlignment(gp::StringAlignmentCenter);
    gp::SolidBrush fg{gp::Color(0xffc8c8e0)};
    for (int i = 0; i < n; i++) {
        fg.SetColor(gp::Color(tools[order[i]].action == Record && recording() ? C_BAD : 0xffc8c8e0));
        gp::RectF rect(7, static_cast<float>(48 + i * 38), 40, 34);
        if (hover == i) {
            gp::LinearGradientBrush active(rect, gp::Color(0xff7253d9), gp::Color(0xff3c5fae), gp::LinearGradientModeHorizontal);
            art::fillRound(gr, rect, 10, active);
        }
        gr.DrawString(tools[order[i]].icon, -1, &icon, rect, &fmt, &fg);
    }
    drawText(gr, L"•••", gp::RectF(6, static_cast<float>(48 + n * 38), 42, 32), 16, C_DIM, true);
}
} // namespace phone::playtools
