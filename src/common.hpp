// Shared includes and small helpers for the Velox launcher.
#pragma once

#undef _WIN32_WINNT
#undef WINVER
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>

#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "version.h"
#include "localization.hpp"

namespace gp = Gdiplus;

namespace phone {

constexpr const wchar_t* AVD_NAME = L"Gaming_Phone";

// Colours shared by the launcher window, the boot screen and the generated art.
constexpr gp::ARGB C_BG = 0xFF0E1015, C_SIDE = 0xFF13151C, C_CARD = 0xFF1A1D26, C_CHIP = 0xFF242834,
                   C_CHIP_HOVER = 0xFF2E3341, C_LINE = 0xFF262A36, C_ACCENT = 0xFF8B6CFF, C_ACCENT2 = 0xFF3D7BFF,
                   C_TEXT = 0xFFF2F3F7, C_DIM = 0xFF9AA0AE, C_FAINT = 0xFF5D6372, C_GOOD = 0xFF3DD68C,
                   C_WARN = 0xFFFFC15E, C_BAD = 0xFFFF7B7B;

inline bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

inline bool isDir(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

inline std::wstring envVar(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2] = L"";
    DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    return n && n < std::size(buf) ? std::wstring(buf) : std::wstring();
}

inline std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

inline std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// The folder Velox.exe is in, with a trailing backslash.
inline std::wstring exeFolder() {
    wchar_t path[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s = path;
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}

inline std::wstring exePath() {
    wchar_t path[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

// %APPDATA%\Velox, created on first use.
std::wstring dataFolder();
std::wstring baseDataFolder();

inline std::wstring fileName(const std::wstring& path) { return path.substr(path.find_last_of(L"\\/") + 1); }

inline void createDirs(const std::wstring& dir) {
    for (size_t i = 3; i <= dir.size(); i++) {
        if (i == dir.size() || dir[i] == L'\\' || dir[i] == L'/') CreateDirectoryW(dir.substr(0, i).c_str(), nullptr);
    }
}

}  // namespace phone
