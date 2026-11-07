#pragma once
#include "phone.hpp"
namespace phone::presets {
struct Preset {
    bool enabled = false;
    int display = -1, rate = 0, cores = -1, memory = -1, gpu = -1, fullscreen = -1;
};
Preset load(const std::wstring &package);
bool save(const std::wstring &package, const Preset &preset);
void invalidate();
Settings apply(const Settings &global, const std::wstring &package);
bool restartNeeded(const Settings &wanted, const Settings &actual);
std::wstring summary(const Settings &global, const std::wstring &package);
void remember(const std::wstring &package);
std::wstring active();
} // namespace phone::presets
