#pragma once
#include "phone.hpp"

namespace phone::playtools {
constexpr UINT ACTION_MESSAGE = WM_APP + 40;
enum Action {
    Back = 1,
    Home,
    Recents,
    VolumeUp,
    VolumeDown,
    Rotate,
    RotationLock,
    Fullscreen,
    Screenshot,
    Record,
    Library,
    Editor,
    Settings,
    Power,
    EnableMapping,
    FocusPhone,
    ScreenResolution
};
enum class Kind { Tap, Stick, Swipe, Aim, PadLeft, PadRight };
struct Binding {
    Kind kind = Kind::Tap;
    int key = VK_SPACE;
    float x = .5f, y = .5f, x2 = .7f, y2 = .5f, radius = .12f;
};
struct Profile {
    std::wstring package;
    bool landscape = false, enabled = false;
    std::vector<Binding> bindings;
    int padMode = 0, padLayout = 0;
    std::wstring padDevice;
    float deadzone = .18f, sensitivity = 1;
    bool invertY = false;
};
void initialize(HWND launcher);
void tick();
void shutdown();
void pollPhone();
void selectProfile(const std::wstring &package, bool landscape);
Profile profile();
void enabled(bool on);
void edit(bool focus = true);
bool editing();
bool mouseCaptured();
void release();
std::wstring status();
void toggleFullscreen();
void setFullscreen(bool enabled);
bool controller(const Profile& desired);
void focusPhone();
void toolbarVisible(bool on);
bool toolbarVisible();
std::wstring keyName(int key);
// Rendering is shared with the offline preview and the live phone toolbar.
void renderToolbar(gp::Graphics &gr, int width, int height, int hover, bool collapsed);
} // namespace phone::playtools
