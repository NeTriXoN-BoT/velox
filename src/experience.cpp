#include "experience.hpp"
#include "companion_id.h"
#include "net.hpp"
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <wincrypt.h>

namespace phone::experience {
namespace {
std::mutex catalogLock, installLock;
std::vector<AppInfo> catalog;
std::atomic<bool> refreshWanted{true}, busy{false};
std::atomic<DWORD> readyPid{0};
std::atomic<int> hostLanguage{0};
std::atomic<int> hostNav{0};
std::atomic<bool> hostRotate{true};
std::wstring apkPath;
uint64_t seenRevision = 0;
std::wstring ini() {
    return dataFolder() + L"\\experience.ini";
}
std::wstring quote(const std::wstring &s) {
    return L"\"" + s + L"\"";
}
std::wstring shellArg(const std::wstring &s) {
    std::wstring r = L"'";
    for (auto c : s)
        r += c == L'\'' ? L"'\\''" : std::wstring(1, c);
    return r + L"'";
}
std::string shell(const std::wstring &s, DWORD timeout = 10000) {
    return adb(L"shell " + quote(s), timeout);
}
std::vector<std::string> split(const std::string &s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    for (std::string f; std::getline(in, f, '\t');)
        out.push_back(f);
    return out;
}
std::string decode(const std::string &s) {
    DWORD n = 0;
    if (!CryptStringToBinaryA(s.c_str(), static_cast<DWORD>(s.size()), CRYPT_STRING_BASE64, nullptr, &n, nullptr, nullptr))
        return {};
    std::string out(n, '\0');
    if (!CryptStringToBinaryA(s.c_str(), static_cast<DWORD>(s.size()), CRYPT_STRING_BASE64, reinterpret_cast<BYTE *>(out.data()), &n,
                              nullptr, nullptr))
        return {};
    out.resize(n);
    return out;
}
std::wstring iconName(const std::wstring &name, uint64_t updated) {
    uint64_t hash = 1469598103934665603ull;
    for (auto c : name) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return std::to_wstring(hash) + L"-" + std::to_wstring(updated) + L".png";
}
std::vector<AppInfo> parse(const std::string &text) {
    std::vector<AppInfo> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line) && out.size() < 2000) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        auto f = split(line);
        if (f.size() != 6)
            continue;
        AppInfo a;
        a.component = widen(f[0]);
        if (!validComponent(a.component))
            continue;
        a.package = a.component.substr(0, a.component.find('/'));
        a.label = widen(decode(f[1]));
        if (a.label.empty())
            a.label = a.package;
        a.game = f[2] == "1";
        a.favorite = f[3] == "1";
        a.updated = strtoull(f[4].c_str(), nullptr, 10);
        a.played = strtoull(f[5].c_str(), nullptr, 10);
        a.icon = dataFolder() + L"\\app-icons\\" + iconName(a.component, a.updated);
        out.push_back(std::move(a));
    }
    return out;
}
void refreshCatalog(const Report &report) {
    busy = true;
    std::string data = adb(L"exec-out content read --uri content://app.velox.home.bridge/apps", 15000);
    auto apps = parse(data);
    if (apps.empty()) {
        busy = false;
        report(L"The app library couldn't be read. Try Refresh when Android is ready.", true);
        return;
    }
    createDirs(dataFolder() + L"\\app-icons");
    for (auto &a : apps) {
        if (exists(a.icon))
            continue;
        std::wstring component = a.component;
        size_t slash = component.find('/');
        component.replace(slash, 1, L"%2F");
        std::string png =
            adb(L"exec-out content read --uri " + quote(L"content://app.velox.home.bridge/icon?component=" + component), 10000);
        if (png.size() > 8 && png.substr(1, 3) == "PNG") {
            std::ofstream f{std::filesystem::path(a.icon), std::ios::binary};
            f.write(png.data(), png.size());
        }
    }
    {
        std::ofstream file{std::filesystem::path(dataFolder() + L"\\library.tsv.new"), std::ios::binary};
        file.write(data.data(), data.size());
        if (file.good()) {
            file.close();
            MoveFileExW((dataFolder() + L"\\library.tsv.new").c_str(), (dataFolder() + L"\\library.tsv").c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        }
    }
    {
        std::lock_guard<std::mutex> l(catalogLock);
        catalog = std::move(apps);
    }
    busy = false;
}
} // namespace
bool validComponent(const std::wstring &c) {
    static const std::wregex pattern(L"[A-Za-z][A-Za-z0-9_]*(\\.[A-Za-z][A-Za-z0-9_]*)+/[A-Za-z0-9_.$]+");
    return c.size() < 400 && std::regex_match(c, pattern);
}
void load() {
    std::ifstream file{std::filesystem::path(dataFolder() + L"\\library.tsv"), std::ios::binary};
    std::string data((std::istreambuf_iterator<char>(file)), {});
    {
        std::lock_guard<std::mutex> l(catalogLock);
        catalog = parse(data);
    }
    wchar_t text[32];
    GetPrivateProfileStringW(L"sync", L"revision", L"0", text, 32, ini().c_str());
    seenRevision = _wcstoui64(text, nullptr, 10);
    if (seenRevision > INT64_MAX)
        seenRevision = 0;
}
std::vector<AppInfo> library() {
    std::lock_guard<std::mutex> l(catalogLock);
    return catalog;
}
void requestLibrary() {
    refreshWanted = true;
}
bool libraryBusy() {
    return busy || (refreshWanted && state == State::Running);
}
bool companionReady() {
    return state == State::Running && readyPid == emulatorPid();
}
bool ensureCompanion(const Report &report) {
    if (state != State::Running)
        return false;
    DWORD pid = emulatorPid();
    if (!pid)
        return false;
    if (readyPid == pid)
        return true;
    std::lock_guard<std::mutex> l(installLock);
    if (readyPid == pid)
        return true;
    readyPid = 0;
    apkPath.clear();
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(207), RT_RCDATA);
    HGLOBAL loaded = resource ? LoadResource(nullptr, resource) : nullptr;
    if (!loaded) {
        report(L"Velox Home is missing from this build.", true);
        return false;
    }
    const void *bytes = LockResource(loaded);
    DWORD size = SizeofResource(nullptr, resource);
    std::string expected = net::hashBytes(bytes, size, net::Hash::Sha256);
    auto installedPath = [] {
        std::string path = adb(L"shell pm path app.velox.home", 5000);
        if (path.rfind("package:/", 0) != 0 || path.find_first_of("\r\n\"' ") != std::string::npos)
            return std::wstring();
        return widen(path.substr(8));
    };
    auto matches = [&](const std::wstring &path) {
        if (path.empty() || expected.size() != 64)
            return false;
        std::string hash = shell(L"/system/bin/sha256sum " + shellArg(path), 10000);
        return hash.size() > 64 && hash.substr(0, 64) == expected && hash[64] == ' ';
    };
    std::wstring installed = installedPath();
    // A package name or a provider's claimed build ID cannot authenticate code
    // that will run as adb shell. Verify the installed APK against our own bytes.
    if (!matches(installed)) {
        report(L"Preparing Velox Home and game controls…", false);
        std::wstring path = dataFolder() + L"\\VeloxHome.apk";
        {
            std::ofstream file{std::filesystem::path(path), std::ios::binary};
            file.write(static_cast<const char *>(bytes), size);
            if (!file.good())
                return false;
        }
        std::string result = adb(L"install -r " + quote(path), 120000);
        DeleteFileW(path.c_str());
        if (result.find("Success") == std::string::npos) {
            report(L"Couldn't prepare the signed Velox Home app: " + widen(result), true);
            return false;
        }
        installed = installedPath();
        if (!matches(installed)) {
            report(L"Velox Home failed its integrity check. Game input remains disabled.", true);
            return false;
        }
    }
    auto hello = adb(L"shell content call --uri content://app.velox.home.bridge --method hello", 5000);
    if (hello.find(narrow(VELOX_COMPANION_ID)) == std::string::npos) {
        report(i18n::text(L"Velox Home isn't ready yet. Reconnecting shortly."), true);
        return false;
    }
    apkPath = installed;
    readyPid = pid;
    refreshWanted = true;
    return true;
}

void hostPreferences(const Settings &s) {
    hostNav = s.gestureNavigation ? 2 : s.reverseButtons ? 1 : 0;
    hostRotate = s.autoRotate;
    hostLanguage = s.language;
}
std::optional<GuestPreferences> tick(const Report &report) {
    if (!ensureCompanion(report))
        return {};
    std::optional<GuestPreferences> changed;
    auto fields = split(adb(L"exec-out content read --uri content://app.velox.home.bridge/state", 5000));
    if (fields.size() == 4) {
        GuestPreferences p;
        p.revision = strtoull(fields[0].c_str(), nullptr, 10);
        p.navigation = atoi(fields[1].c_str());
        p.rotate = atoi(fields[2].c_str()) != 0;
        p.language = atoi(fields[3].c_str()) == 1 ? 1 : 0;
        if (p.navigation >= 0 && p.navigation <= 2 && p.revision > seenRevision) {
            seenRevision = p.revision;
            WritePrivateProfileStringW(L"sync", L"revision", std::to_wstring(seenRevision).c_str(), ini().c_str());
            hostNav = p.navigation;
            hostRotate = p.rotate;
            hostLanguage = p.language;
            changed = p;
        }
    }
    // Also serves as a heartbeat for the in-phone settings connection indicator.
    shell(L"content call --uri content://app.velox.home.bridge --method sync --arg " +
              shellArg(std::to_wstring(hostNav) + L"," + (hostRotate ? L"1" : L"0") + L"," + std::to_wstring(seenRevision) + L"," +
                       std::to_wstring(hostLanguage)),
          5000);
    static ULONGLONG lastLibrary = 0;
    if (refreshWanted.exchange(false) || GetTickCount64() - lastLibrary > 30000) {
        refreshCatalog(report);
        lastLibrary = GetTickCount64();
    }
    return changed;
}
bool launch(const std::wstring &component, const Report &report) {
    if (!validComponent(component) || !ensureCompanion(report))
        return false;
    // Android 15 restricts background app launches from providers. The already
    // authorized shell starts the activity; Home itself uses normal user clicks.
    std::string r = shell(L"am start -W --user current -a android.intent.action.MAIN -c android.intent.category.LAUNCHER -f 270532608 -n " +
                              shellArg(component),
                          30000);
    bool ok = r.find("Status: ok") != std::string::npos;
    if (ok)
        shell(L"content call --uri content://app.velox.home.bridge --method played --arg " + shellArg(component));
    if (!ok)
        report(i18n::text(L"Couldn't open this app. Refresh the library and try again."), true);
    requestLibrary();
    return ok;
}
void favorite(const AppInfo &app, const Report &report) {
    if (!ensureCompanion(report))
        return;
    shell(L"content call --uri content://app.velox.home.bridge --method favorite --arg " +
          shellArg(app.component + (app.favorite ? L"|0" : L"|1")));
    requestLibrary();
}
void classify(const AppInfo &app, const Report &report) {
    if (!ensureCompanion(report))
        return;
    shell(L"content call --uri content://app.velox.home.bridge --method game --arg " +
          shellArg(app.component + (app.game ? L"|0" : L"|1")));
    requestLibrary();
}
void openHome(const Report &report, bool makeDefault) {
    if (!ensureCompanion(report))
        return;
    if (makeDefault) {
        auto r = shell(L"cmd package set-home-activity --user current app.velox.home/.HomeActivity");
        if (r.find("Success") == std::string::npos) {
            report(L"Choose Velox Home in Android's Default Home setting.", true);
            return;
        }
    }
    shell(L"am start -n app.velox.home/.HomeActivity");
}
void openSettings(const Report &report) {
    if (ensureCompanion(report))
        shell(L"am start -n app.velox.home/.SettingsActivity");
}
std::wstring inputCommand() {
    std::lock_guard<std::mutex> l(installLock);
    if (apkPath.empty() || state != State::Running || readyPid != emulatorPid())
        return {};
    return quote(paths().sdk + L"\\platform-tools\\adb.exe") + L" -s " + adbSerial() + L" shell " +
           quote(L"CLASSPATH=" + shellArg(apkPath) + L" app_process /system/bin app.velox.home.InputBridge");
}
} // namespace phone::experience
