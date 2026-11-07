#include "controllers.hpp"
#include <xinput.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <cmath>
#include <mutex>
#include <sstream>
namespace phone::controllers {
namespace {
std::mutex stateLock;
Snapshot latest;
} // namespace
const wchar_t *buttonName(int i) {
    static const wchar_t *names[] = {L"A / Cross",    L"B / Circle",      L"X / Square", L"Y / Triangle", L"LB / L1",  L"RB / R1",
                                     L"Back / Share", L"Start / Options", L"Left stick", L"Right stick",  L"D-pad up", L"D-pad down",
                                     L"D-pad left",   L"D-pad right",     L"LT / L2",    L"RT / R2"};
    return i >= 0 && i < Count ? names[i] : L"Controller";
}
Snapshot snapshot() {
    std::lock_guard<std::mutex> g(stateLock);
    return latest;
}
Frame filtered(Frame f, float dz, float sensitivity, bool invert) {
    dz = std::isfinite(dz) ? std::clamp(dz, .05f, .4f) : .18f;
    sensitivity = std::isfinite(sensitivity) ? std::clamp(sensitivity, .5f, 2.f) : 1.f;
    auto stick = [&](float &x, float &y) {
        if (!std::isfinite(x) || !std::isfinite(y)) {
            x = y = 0;
            return;
        }
        float length = std::hypot(x, y);
        if (length <= dz) {
            x = y = 0;
            return;
        }
        float m = std::min(1.f, (std::min(1.f, length) - dz) / (1 - dz) * sensitivity);
        x = x / length * m;
        y = y / length * m;
    };
    stick(f.lx, f.ly);
    stick(f.rx, f.ry);
    if (invert)
        f.ry = -f.ry;
    f.lt = std::isfinite(f.lt) ? std::clamp((f.lt - .05f) / .95f, 0.f, 1.f) : 0;
    f.rt = std::isfinite(f.rt) ? std::clamp((f.rt - .05f) / .95f, 0.f, 1.f) : 0;
    f.buttons &= ~((1u << LT) | (1u << RT));
    if (f.lt > .12f)
        f.buttons |= 1u << LT;
    if (f.rt > .12f)
        f.buttons |= 1u << RT;
    return f;
}
struct Reader::Impl {
    using GetState = DWORD(WINAPI *)(DWORD, XINPUT_STATE *);
    HMODULE module = nullptr;
    GetState get = nullptr;
    IDirectInput8W *di = nullptr;
    bool com = false;
    HWND owner = nullptr;
    bool publish = true;
    Snapshot seen;
    ULONGLONG refreshed = 0;
    struct Direct {
        Device info;
        IDirectInputDevice8W *device = nullptr;
        bool sony = false;
    };
    std::vector<Direct> direct;
    std::vector<Device> list;
    Impl(HWND window, bool shared) : owner(window), publish(shared) {
        com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        module = LoadLibraryExW(L"xinput1_4.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module)
            module = LoadLibraryExW(L"xinput9_1_0.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module)
            get = reinterpret_cast<GetState>(reinterpret_cast<void *>(GetProcAddress(module, "XInputGetState")));
        DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W, reinterpret_cast<void **>(&di), nullptr);
    }
    void clear() {
        for (auto &d : direct) {
            d.device->Unacquire();
            d.device->Release();
        }
        direct.clear();
    }
    ~Impl() {
        clear();
        if (di)
            di->Release();
        if (module)
            FreeLibrary(module);
        if (com)
            CoUninitialize();
    }
    static BOOL CALLBACK axis(const DIDEVICEOBJECTINSTANCEW *obj, VOID *context) {
        auto *device = static_cast<IDirectInputDevice8W *>(context);
        DIPROPRANGE range{};
        range.diph.dwSize = sizeof(range);
        range.diph.dwHeaderSize = sizeof(range.diph);
        range.diph.dwHow = DIPH_BYID;
        range.diph.dwObj = obj->dwType;
        range.lMin = -32768;
        range.lMax = 32767;
        device->SetProperty(DIPROP_RANGE, &range.diph);
        return DIENUM_CONTINUE;
    }
    static BOOL CALLBACK device(const DIDEVICEINSTANCEW *instance, VOID *context) {
        auto &self = *static_cast<Impl *>(context);
        if (self.direct.size() >= 16)
            return DIENUM_STOP;
        IDirectInputDevice8W *device = nullptr;
        if (FAILED(self.di->CreateDevice(instance->guidInstance, &device, nullptr)))
            return DIENUM_CONTINUE;
        if (FAILED(device->SetDataFormat(&c_dfDIJoystick2)) ||
            FAILED(device->SetCooperativeLevel(self.owner, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE))) {
            device->Release();
            return DIENUM_CONTINUE;
        }
        device->EnumObjects(axis, device, DIDFT_AXIS);
        device->Acquire();
        wchar_t guid[48]{};
        StringFromGUID2(instance->guidInstance, guid, 48);
        std::wstring name = instance->tszProductName;
        name.resize(std::min<size_t>(128, name.size()));
        std::wstring lower = name;
        for (auto &c : lower)
            c = static_cast<wchar_t>(towlower(c));
        self.direct.push_back({{L"direct:" + std::wstring(guid), name, false},
                               device,
                               lower.find(L"wireless controller") != std::wstring::npos || lower.find(L"dualsense") != std::wstring::npos ||
                                   lower.find(L"dualshock") != std::wstring::npos});
        return DIENUM_CONTINUE;
    }
    void enumerate() {
        clear();
        list.clear();
        if (get)
            for (DWORD i = 0; i < 4; i++) {
                XINPUT_STATE state{};
                if (get(i, &state) == ERROR_SUCCESS)
                    list.push_back({L"xinput:" + std::to_wstring(i), L"Xbox-compatible controller " + std::to_wstring(i + 1), true});
            }
        if (di)
            di->EnumDevices(DI8DEVCLASS_GAMECTRL, device, this, DIEDFL_ATTACHEDONLY);
        for (auto &d : direct)
            list.push_back(d.info);
        refreshed = GetTickCount64();
    }
    Frame read(const std::wstring &requested, int layout) {
        if (!refreshed || GetTickCount64() - refreshed > 2000)
            enumerate();
        std::wstring id = requested;
        if (id.empty() && !list.empty())
            id = list.front().id;
        Frame f;
        if (id.rfind(L"xinput:", 0) == 0 && get && id.size() == 8 && id.back() >= L'0' && id.back() <= L'3') {
            XINPUT_STATE state{};
            if (get(id.back() - L'0', &state) == ERROR_SUCCESS) {
                f.connected = true;
                auto &p = state.Gamepad;
                const WORD mask[] = {XINPUT_GAMEPAD_A,
                                     XINPUT_GAMEPAD_B,
                                     XINPUT_GAMEPAD_X,
                                     XINPUT_GAMEPAD_Y,
                                     XINPUT_GAMEPAD_LEFT_SHOULDER,
                                     XINPUT_GAMEPAD_RIGHT_SHOULDER,
                                     XINPUT_GAMEPAD_BACK,
                                     XINPUT_GAMEPAD_START,
                                     XINPUT_GAMEPAD_LEFT_THUMB,
                                     XINPUT_GAMEPAD_RIGHT_THUMB,
                                     XINPUT_GAMEPAD_DPAD_UP,
                                     XINPUT_GAMEPAD_DPAD_DOWN,
                                     XINPUT_GAMEPAD_DPAD_LEFT,
                                     XINPUT_GAMEPAD_DPAD_RIGHT};
                for (int b = 0; b < 14; b++)
                    if (p.wButtons & mask[b])
                        f.buttons |= 1u << b;
                f.lx = p.sThumbLX / 32768.f;
                f.ly = -p.sThumbLY / 32768.f;
                f.rx = p.sThumbRX / 32768.f;
                f.ry = -p.sThumbRY / 32768.f;
                f.lt = p.bLeftTrigger / 255.f;
                f.rt = p.bRightTrigger / 255.f;
            }
        } else
            for (auto &d : direct)
                if (d.info.id == id) {
                    DIJOYSTATE2 p{};
                    if (FAILED(d.device->Poll()))
                        d.device->Acquire();
                    if (SUCCEEDED(d.device->GetDeviceState(sizeof(p), &p))) {
                        f.connected = true;
                        bool sony = layout == 2 || (layout == 0 && d.sony);
                        int indices[] = {sony ? 1 : 0, sony ? 2 : 1,  sony ? 0 : 2, 3, 4, 5, sony ? 8 : 6,
                                         sony ? 9 : 7, sony ? 10 : 8, sony ? 11 : 9};
                        for (int b = 0; b < 10; b++)
                            if (p.rgbButtons[indices[b]] & 0x80)
                                f.buttons |= 1u << b;
                        DWORD pov = p.rgdwPOV[0];
                        if (LOWORD(pov) != 0xffff) {
                            if (pov >= 31500 || pov <= 4500)
                                f.buttons |= 1u << Up;
                            if (pov >= 4500 && pov <= 13500)
                                f.buttons |= 1u << Right;
                            if (pov >= 13500 && pov <= 22500)
                                f.buttons |= 1u << Down;
                            if (pov >= 22500 && pov <= 31500)
                                f.buttons |= 1u << Left;
                        }
                        f.lx = p.lX / 32768.f;
                        f.ly = p.lY / 32768.f;
                        f.rx = (sony ? p.lZ : p.lRx) / 32768.f;
                        f.ry = (sony ? p.lRz : p.lRy) / 32768.f;
                        if (sony) {
                            f.lt = (p.lRx + 32768.f) / 65535;
                            f.rt = (p.lRy + 32768.f) / 65535;
                        } else {
                            f.lt = std::max(0.f, p.lZ / 32768.f);
                            f.rt = std::max(0.f, -p.lZ / 32768.f);
                        }
                    }
                    break;
                }
        seen = {list, f, id};
        if (publish) {
            std::lock_guard<std::mutex> g(stateLock);
            latest = seen;
        }
        return f;
    }
};
Reader::Reader(HWND window, bool publish) : impl(std::make_unique<Impl>(window, publish)) {
}
Reader::~Reader() = default;
Frame Reader::read(const std::wstring &id, int layout) {
    return impl->read(id, layout);
}
Snapshot Reader::state() const {
    return impl->seen;
}
std::string registration() {
    static const int descriptor[] = {5,   1,   9,   5,   161, 1,   5,   9,   25, 1,   41,  16,  21, 0,   37, 1,   117, 1,   149, 16,
                                     129, 2,   5,   1,   9,   57,  21,  0,   37, 7,   53,  0,   70, 59,  1,  101, 20,  117, 4,   149,
                                     1,   129, 66,  117, 4,   149, 1,   129, 3,  101, 0,   9,   48, 9,   49, 9,   50,  9,   53,  22,
                                     1,   128, 38,  255, 127, 54,  1,   128, 70, 255, 127, 117, 16, 149, 4,  129, 2,   5,   2,   9,
                                     197, 9,   196, 21,  0,   38,  255, 0,   53, 0,   70,  255, 0,  117, 8,  149, 2,   129, 2,   192};
    std::ostringstream o;
    o << "{\"id\":1,\"command\":\"register\",\"name\":\"Velox Controller\",\"vid\":4617,\"pid\":61441,\"bus\":\"usb\",\"descriptor\":[";
    bool first = true;
    for (int b : descriptor) {
        if (!first)
            o << ',';
        first = false;
        o << b;
    }
    return o.str() + "]}\n";
}
std::string report(const Frame &f) {
    unsigned buttons = 0;
    const int bit[] = {0, 1, 3, 4, 6, 7, 10, 11, 13, 14, -1, -1, -1, -1, 8, 9};
    for (int i = 0; i < Count; i++)
        if (bit[i] >= 0 && (f.buttons & (1u << i)))
            buttons |= 1u << bit[i];
    int x = int((f.buttons >> Right) & 1) - int((f.buttons >> Left) & 1), y = int((f.buttons >> Down) & 1) - int((f.buttons >> Up) & 1),
        hat = 8;
    if (x || y) {
        if (y < 0)
            hat = x < 0 ? 7 : x > 0 ? 1 : 0;
        else if (y > 0)
            hat = x < 0 ? 5 : x > 0 ? 3 : 4;
        else
            hat = x < 0 ? 6 : 2;
    }
    auto axis = [](float v) { return uint16_t(int16_t(std::lround((std::isfinite(v) ? std::clamp(v, -1.f, 1.f) : 0) * 32767))); };
    std::ostringstream o;
    o << "{\"id\":1,\"command\":\"report\",\"report\":[" << (buttons & 255) << ',' << (buttons >> 8) << ',' << hat;
    for (float value : {f.lx, f.ly, f.rx, f.ry}) {
        auto encoded = axis(value);
        o << ',' << (encoded & 255) << ',' << (encoded >> 8);
    }
    o << ',' << std::lround(std::clamp(f.lt, 0.f, 1.f) * 255) << ',' << std::lround(std::clamp(f.rt, 0.f, 1.f) * 255) << "]}\n";
    return o.str();
}
} // namespace phone::controllers
