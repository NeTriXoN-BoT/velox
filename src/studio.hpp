#pragma once
#include "experience.hpp"
namespace phone::studio {
constexpr UINT CHANGED = WM_APP + 44;
void game(HWND owner, const experience::AppInfo &app, const Settings &global);
void controller(HWND owner, const experience::AppInfo &app);
void confirm(HWND owner, const std::wstring &title, const std::wstring &message, const std::wstring &accept,
             std::function<void(bool)> callback);
void input(HWND owner, const std::wstring &title, const std::wstring &message, const std::wstring &initial, const std::wstring &accept,
           std::function<void(const std::wstring &)> callback);
void preview(gp::Graphics &graphics, int type, const experience::AppInfo &app, const Settings &global);
} // namespace phone::studio
