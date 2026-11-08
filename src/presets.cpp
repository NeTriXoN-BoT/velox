#include "presets.hpp"
#include <map>
#include <regex>
#include <fstream>
#include <filesystem>
namespace phone::presets {
namespace {
std::mutex mutex;
std::map<std::wstring, Preset> cache;
bool safe(const std::wstring &s) {
    static const std::wregex r(L"[A-Za-z][A-Za-z0-9_]*(\\.[A-Za-z][A-Za-z0-9_]*)+");
    return s.size() < 200 && std::regex_match(s, r);
}
std::wstring path(const std::wstring &p) {
    return dataFolder() + L"\\presets\\" + p + L".ini";
}
uint64_t born(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
        return 0;
    FILETIME c{}, e{}, k{}, u{};
    bool ok = GetProcessTimes(h, &c, &e, &k, &u);
    CloseHandle(h);
    return ok ? (uint64_t(c.dwHighDateTime) << 32) | c.dwLowDateTime : 0;
}
} // namespace
Preset load(const std::wstring &p) {
    if (!safe(p))
        return {};
    std::lock_guard<std::mutex> guard(mutex);
    auto it = cache.find(p);
    if (it != cache.end())
        return it->second;
    auto f = path(p);
    auto get = [&](const wchar_t *k, int d) { return int(GetPrivateProfileIntW(L"preset", k, d, f.c_str())); };
    Preset v;
    v.enabled = get(L"enabled", 0) == 1;
    v.display = std::clamp(get(L"display", -1), -1, int(displays().size()) - 1);
    v.rate = get(L"rate", 0);
    if (v.rate && std::find(refreshRates().begin(), refreshRates().end(), v.rate) == refreshRates().end())
        v.rate = 0;
    v.cores = get(L"cores", -1);
    if (v.cores != -1 && !coreAllowed(v.cores))
        v.cores = -1;
    v.memory = get(L"memory", -1);
    if (v.memory != -1 && !memoryAllowed(v.memory))
        v.memory = -1;
    v.gpu = std::clamp(get(L"gpu", -1), -1, 2);
    v.fullscreen = std::clamp(get(L"fullscreen", -1), -1, 1);
    cache[p] = v;
    return v;
}
bool save(const std::wstring &p, const Preset &v) {
    if (!safe(p))
        return false;
    createDirs(dataFolder() + L"\\presets");
    auto f = path(p);
    std::ofstream out(std::filesystem::path(f + L".new"), std::ios::binary);
    out << "[preset]\nenabled=" << v.enabled << "\ndisplay=" << v.display << "\nrate=" << v.rate << "\ncores=" << v.cores
        << "\nmemory=" << v.memory << "\ngpu=" << v.gpu << "\nfullscreen=" << v.fullscreen << "\n";
    out.close();
    if (!out || !MoveFileExW((f + L".new").c_str(), f.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return false;
    std::lock_guard<std::mutex> guard(mutex);
    cache.erase(p);
    return true;
}
void invalidate() {
    std::lock_guard<std::mutex> guard(mutex);
    cache.clear();
}
Settings apply(const Settings &global, const std::wstring &p) {
    auto v = load(p);
    Settings s = global;
    if (!v.enabled)
        return s;
    if (v.display >= 0)
        s.display = v.display;
    if (v.rate)
        s.rate = v.rate;
    if (v.cores >= 0)
        s.cores = v.cores;
    if (v.memory >= 0)
        s.memory = v.memory;
    if (v.gpu >= 0)
        s.gpu = v.gpu;
    return s;
}
bool restartNeeded(const Settings &a, const Settings &b) {
    return a.display != b.display || a.rate != b.rate || coresFor(a) != coresFor(b) || memoryFor(a) != memoryFor(b) || a.gpu != b.gpu ||
           a.root != b.root || a.number != b.number || a.frame != b.frame;
}
std::wstring summary(const Settings &global, const std::wstring &p) {
    if (!load(p).enabled)
        return L"Uses your global settings";
    auto s = apply(global, p);
    return std::wstring(displays()[s.display].name) + L" · " + std::to_wstring(s.rate) + L" FPS · " + std::to_wstring(coresFor(s)) +
           i18n::text(L" cores · ") + memoryLabel(memoryFor(s)) + L" GB";
}
void remember(const std::wstring &p) {
    auto f = dataFolder() + L"\\session.ini";
    DWORD pid = emulatorPid();
    WritePrivateProfileStringW(L"game", L"package", safe(p) ? p.c_str() : L"", f.c_str());
    WritePrivateProfileStringW(L"game", L"pid", std::to_wstring(pid).c_str(), f.c_str());
    WritePrivateProfileStringW(L"game", L"born", std::to_wstring(born(pid)).c_str(), f.c_str());
}
std::wstring active() {
    auto f = dataFolder() + L"\\session.ini";
    DWORD pid = emulatorPid();
    if (!pid || DWORD(GetPrivateProfileIntW(L"game", L"pid", 0, f.c_str())) != pid)
        return {};
    wchar_t t[256]{}, b[32]{};
    GetPrivateProfileStringW(L"game", L"born", L"0", b, 32, f.c_str());
    if (_wcstoui64(b, nullptr, 10) != born(pid))
        return {};
    GetPrivateProfileStringW(L"game", L"package", L"", t, 256, f.c_str());
    return safe(t) ? std::wstring(t) : std::wstring();
}
} // namespace phone::presets
