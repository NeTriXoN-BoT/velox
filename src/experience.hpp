#pragma once
#include "phone.hpp"
#include <optional>

namespace phone::experience {
struct AppInfo {
    std::wstring component, package, label, icon;
    bool game = false, favorite = false;
    uint64_t updated = 0, played = 0;
};
struct GuestPreferences {
    uint64_t revision = 0;
    int navigation = 0;
    bool rotate = true;
    int language = 0;
};
bool validComponent(const std::wstring &component);
void load();
std::vector<AppInfo> library();
void requestLibrary();
bool libraryBusy();
bool companionReady();
bool ensureCompanion(const Report &report);
std::optional<GuestPreferences> tick(const Report &report);
void hostPreferences(const Settings &settings);
bool launch(const std::wstring &component, const Report &report);
void favorite(const AppInfo &app, const Report &report);
void classify(const AppInfo &app, const Report &report);
void openHome(const Report &report, bool makeDefault = false);
void openSettings(const Report &report);
std::wstring inputCommand(bool observe = false);
bool framePolicy(const std::wstring &package = L"", int rate = 0);
} // namespace phone::experience
