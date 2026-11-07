#include "phone.hpp"

#include <dxgi.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <shlobj.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <tlhelp32.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <regex>
#include <sstream>
#include <thread>

#include "art.hpp"
#include "net.hpp"
#include "orientation.hpp"
#include "instances.hpp"

namespace phone {

std::atomic<State> state{State::Stopped};
Settings running;
Boot boot;

namespace {

Paths g_paths;
HostInfo g_host;
bool g_hostReady = false;
std::atomic<int> g_cpuLimit{MAX_CPU_CORES};
std::wstring g_cpuCapabilityKey;
std::mutex g_hostLock;
HANDLE g_emuProcess = nullptr; // emulator.exe started by us
std::atomic<bool> g_recording{false};
std::wstring g_recordFile;
std::mutex g_recordLock;
DWORD g_recordPid = 0;
uint64_t g_recordStarted = 0;
std::atomic<int> g_session{0}; // counts phone starts
std::atomic<bool> g_autoRotation{true};
std::atomic<unsigned> g_rotationGeneration{0};
std::mutex g_rotationLock;
std::atomic<int> g_navigationMode{0}; // Back/Home/Recents, Recents/Home/Back, gestures
std::atomic<bool> g_navigationRestart{false};

// Magisk, pinned to the release root was tested with.
constexpr const wchar_t *MAGISK_URL = L"https://github.com/topjohnwu/Magisk/releases/download/v31.1/Magisk-v31.1.apk";
constexpr uint64_t MAGISK_SIZE = 10679283;
constexpr const char *MAGISK_SHA256 = "89cefac0a318afa39fb3880c45c87b036c05bc1ebf02a7a715b7f8d7c26ac6e7";

std::wstring q(const std::wstring &s) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (auto c : s) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        if (c == L'\"') {
            out.append(slashes * 2 + 1, L'\\');
            out += c;
        } else {
            out.append(slashes, L'\\');
            out += c;
        }
        slashes = 0;
    }
    out.append(slashes * 2, L'\\');
    return out + L"\"";
}

std::wstring lower(std::wstring s) {
    for (auto &c : s)
        c = static_cast<wchar_t>(towlower(c));
    return s;
}

} // namespace

std::wstring baseDataFolder() {
    static std::wstring folder;
    if (folder.empty()) {
        // Settings live in a "data" folder next to Velox.exe when it can be written. Windows gives
        // programs started from Microsoft Store apps their own private copy of AppData, so a folder
        // next to the exe keeps one set of settings however Velox is opened. Otherwise AppData.
        wchar_t dir[MAX_PATH] = L"";
        SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, dir);
        std::wstring appdata = std::wstring(dir) + L"\\Velox", portable = exeFolder() + L"data";
        createDirs(portable);
        std::wstring probe = portable + L"\\.write-test-" + std::to_wstring(GetCurrentProcessId());
        HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        bool canWrite = h != INVALID_HANDLE_VALUE;
        if (canWrite)
            CloseHandle(h);
        folder = canWrite ? portable : appdata;
        createDirs(folder);
        // Carry settings over from where earlier versions kept them.
        if (!exists(folder + L"\\settings.ini")) {
            for (const std::wstring &old : {appdata, std::wstring(dir) + L"\\GamingPhone"}) {
                if (old == folder || !exists(old + L"\\settings.ini"))
                    continue;
                for (const wchar_t *f : {L"\\settings.ini", L"\\preview.png"})
                    CopyFileW((old + f).c_str(), (folder + f).c_str(), TRUE);
                break;
            }
        }
    }
    return folder;
}

std::wstring dataFolder() {
    static const std::wstring folder = [] {
        auto path = instances::dataPath(baseDataFolder());
        createDirs(path);
        return path;
    }();
    return folder;
}

// ------------------------------------------------------------------------------------------
// Choices

const std::vector<Display> &displays() {
    static const std::vector<Display> list = {
        {L"hdplus", L"HD+", L"720 × 1600", 720, 1600, 280},
        {L"fhd", i18n::text(L"Full HD"), L"1080 × 1920", 1080, 1920, 420},
        {L"fhdplus", i18n::text(L"Full HD+"), L"1080 × 2400", 1080, 2400, 420},
        {L"qhdplus", i18n::text(L"Quad HD+"), L"1440 × 3200", 1440, 3200, 560},
        {L"fold", i18n::text(L"Foldable"), L"1840 × 2208", 1840, 2208, 420},
        {L"tablet", i18n::text(L"Tablet"), L"1600 × 2560", 1600, 2560, 320},
    };
    return list;
}
const std::vector<int> &refreshRates() {
    static const std::vector<int> r = {30, 60, 90, 120, 144};
    return r;
}
const std::vector<int> &coreChoices() {
    static const std::vector<int> c = {0, 2, 4, 6, 8, 12, 16, 24, 32, 48, MAX_CPU_CORES};
    return c;
}
const std::vector<int> &memoryChoices() {
    static const std::vector<int> m = {0, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, MAX_MEMORY_GB};
    return m;
}

// ------------------------------------------------------------------------------------------
// This PC

namespace {

std::wstring regString(HKEY root, const wchar_t *path, const wchar_t *name) {
    wchar_t buf[256] = L"";
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, path, name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return {};
    std::wstring s = buf;
    while (!s.empty() && s.back() == L' ')
        s.pop_back();
    while (!s.empty() && s.front() == L' ')
        s.erase(0, 1);
    return s;
}

bool hypervisorPresent() {
    HMODULE m = LoadLibraryExW(L"WinHvPlatform.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m)
        return false;
    using Fn = HRESULT(WINAPI *)(int, void *, UINT32, UINT32 *);
    auto fn = reinterpret_cast<Fn>(reinterpret_cast<void *>(GetProcAddress(m, "WHvGetCapability")));
    BOOL present = FALSE;
    UINT32 written = 0;
    bool ok = fn && SUCCEEDED(fn(0 /* WHvCapabilityCodeHypervisorPresent */, &present, sizeof(present), &written)) && present;
    FreeLibrary(m);
    return ok;
}

void detectGpu(HostInfo &h) {
    h.softwareGpu = true;
    IDXGIFactory1 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))))
        return;
    IDXGIAdapter1 *adapter = nullptr;
    for (UINT i = 0; SUCCEEDED(factory->EnumAdapters1(i, &adapter)); i++) {
        DXGI_ADAPTER_DESC1 d;
        if (SUCCEEDED(adapter->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            uint64_t vram = d.DedicatedVideoMemory >> 20;
            if (h.gpu.empty() || vram > h.vramMB) {
                h.gpu = d.Description;
                h.vramMB = vram;
                wchar_t luid[64];
                swprintf(luid, 64, L"luid_0x%08x_0x%08x", static_cast<unsigned>(d.AdapterLuid.HighPart),
                         static_cast<unsigned>(d.AdapterLuid.LowPart));
                h.gpuLuid = luid;
            }
        }
        adapter->Release();
    }
    factory->Release();
    h.softwareGpu = h.gpu.empty() || h.gpu.find(L"Basic Render") != std::wstring::npos;
    // Shorter names: "NVIDIA GeForce RTX 5070 Ti" -> "RTX 5070 Ti".
    for (const wchar_t *p : {L"NVIDIA GeForce ", L"NVIDIA ", L"AMD Radeon(TM) ", L"AMD ", L"Intel(R) "}) {
        if (h.gpu.rfind(p, 0) == 0) {
            h.gpu = h.gpu.substr(wcslen(p));
            break;
        }
    }
}

// Prefer the active display; the saved mode is only a fallback when Windows has no current mode.
void detectMonitor(HostInfo &h, HWND window = nullptr) {
    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &monitor))
        return;
    // Read the current mode of the screen containing Velox. A faster secondary
    // screen or a saved mode that is no longer active must not inflate the target.
    for (DWORD mode : {ENUM_CURRENT_SETTINGS, ENUM_REGISTRY_SETTINGS}) {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(monitor.szDevice, mode, &dm) && dm.dmDisplayFrequency > 1 && dm.dmPelsWidth >= 640 &&
            dm.dmPelsHeight >= 480) {
            h.monitorHz = int(dm.dmDisplayFrequency);
            h.monitorW = int(dm.dmPelsWidth);
            h.monitorH = int(dm.dmPelsHeight);
            break;
        }
    }
}

void detectDisk(HostInfo &h) {
    std::wstring where = g_paths.avdDir.empty() ? exeFolder() : g_paths.avdDir;
    ULARGE_INTEGER free{};
    if (GetDiskFreeSpaceExW(where.substr(0, 3).c_str(), &free, nullptr, nullptr))
        h.freeDiskGB = static_cast<int>(free.QuadPart >> 30);
}

} // namespace

HostInfo host() {
    std::lock_guard<std::mutex> l(g_hostLock);
    if (!g_hostReady) {
        HostInfo h;
        SYSTEM_INFO si;
        GetNativeSystemInfo(&si);
        h.threads = static_cast<int>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
        if (h.threads < 1)
            h.threads = static_cast<int>(si.dwNumberOfProcessors);
        DWORD len = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
        std::vector<BYTE> cpuInfo(len);
        if (len && GetLogicalProcessorInformationEx(RelationProcessorCore,
                                                    reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(cpuInfo.data()), &len)) {
            int cores = 0;
            for (DWORD off = 0; off < len; cores++)
                off += reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(cpuInfo.data() + off)->Size;
            h.cores = cores;
        }
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        GlobalMemoryStatusEx(&ms);
        h.ramGB = static_cast<int>(std::lround(ms.ullTotalPhys / 1073741824.0));
        h.cpu = regString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString");
        for (const wchar_t *p : {L"AMD ", L"Intel(R) Core(TM) ", L"Intel(R) "}) {
            if (h.cpu.rfind(p, 0) == 0)
                h.cpu = h.cpu.substr(wcslen(p));
        }
        size_t cut = h.cpu.find(L" 16-Core");
        for (const wchar_t *tail : {L"-Core Processor", L" Processor", L" CPU @"}) {
            size_t at = h.cpu.find(tail);
            if (at != std::wstring::npos) {
                size_t sp = h.cpu.rfind(L' ', at);
                cut = std::min(cut, std::wstring(tail) == L"-Core Processor" && sp != std::wstring::npos ? sp : at);
            }
        }
        if (cut != std::wstring::npos)
            h.cpu = h.cpu.substr(0, cut);
        h.hypervisor = hypervisorPresent();
        detectGpu(h);
        detectMonitor(h);
        detectDisk(h);
        g_host = h;
        g_hostReady = true;
    }
    return g_host;
}

void refreshHost(HWND window) {
    host();
    std::lock_guard<std::mutex> l(g_hostLock);
    detectMonitor(g_host, window);
    detectDisk(g_host);
}

std::wstring hostSummary() {
    const HostInfo &h = host();
    std::wstring s = h.cpu.empty() ? L"CPU" : h.cpu;
    s += L"  ·  " + std::to_wstring(h.cores) + i18n::text(L" cores / ") + std::to_wstring(h.threads) + i18n::text(L" threads  ·  ") +
         std::to_wstring(h.ramGB) + L" GB";
    s += L"  ·  " + (h.softwareGpu ? std::wstring(i18n::text(L"Software graphics")) : h.gpu);
    s += L"  ·  " + std::to_wstring(h.monitorW) + L" × " + std::to_wstring(h.monitorH) + L"  ·  " + std::to_wstring(h.monitorHz) + L" Hz";
    return s;
}

int cpuLimit() {
    return std::max(1, std::min({MAX_CPU_CORES, host().threads, g_cpuLimit.load()}));
}
int coresFor(const Settings &s) {
    int t = host().threads;
    if (s.cores > 0) {
        int resolved = 1, limit = std::max(1, std::min(cpuLimit(), s.cores));
        for (int n : coreChoices())
            if (n > 0 && n <= limit)
                resolved = n;
        return resolved;
    }
    return std::min(cpuLimit(), std::clamp(t / 2, std::min(2, std::max(1, t)), 6));
}

int memoryFor(const Settings &s) {
    int r = host().ramGB;
    if (s.memory > 0) {
        int resolved = 2, limit = std::min({MAX_MEMORY_GB, std::max(2, r - std::max(3, r / 4)), s.memory});
        for (int n : memoryChoices())
            if (n > 0 && n <= limit)
                resolved = n;
        return resolved;
    }
    return r <= 6 ? 2 : r <= 8 ? 3 : r <= 12 ? 4 : r <= 16 ? 6 : 8;
}

bool coreAllowed(int c) {
    return c == 0 || (std::find(coreChoices().begin(), coreChoices().end(), c) != coreChoices().end() && c <= cpuLimit());
}
bool memoryAllowed(int gb) {
    return gb == 0 || (std::find(memoryChoices().begin(), memoryChoices().end(), gb) != memoryChoices().end() &&
                       gb <= std::max(2, host().ramGB - std::max(3, host().ramGB / 4)));
}

Settings settingsForHardware(const HostInfo &h, int cpuBudget, int ramBudget, int availableGB) {
    Settings s;
    const int threads = std::max(1, h.threads);
    const int safeRam = std::max(2, h.ramGB - std::max(3, h.ramGB / 4));
    const int cpuLimit = std::clamp(cpuBudget, 1, std::min(MAX_CPU_CORES, threads));
    const int ramLimit = std::clamp(ramBudget, 2, std::min(MAX_MEMORY_GB, safeRam));
    int targetCpu = std::min(cpuLimit, std::clamp(threads / 2, std::min(2, threads), 6));
    int targetRam = h.ramGB <= 6 ? 2 : h.ramGB <= 8 ? 3 : h.ramGB <= 12 ? 4 : h.ramGB <= 16 ? 6 : 8;
    if (availableGB >= 0)
        targetRam = std::min(targetRam, std::max(2, availableGB - 1));
    targetRam = std::min(targetRam, ramLimit);
    // Integrated GPUs borrow system RAM. Leave room for both Windows and graphics.
    const bool dedicated = !h.softwareGpu && h.vramMB >= 2048;
    if (!dedicated)
        targetRam = std::min(targetRam, h.ramGB <= 12 ? 3 : 4);
    s.cores = targetCpu < 2 ? 0 : 2;
    for (int n : coreChoices())
        if (n > 0 && n <= targetCpu)
            s.cores = n;
    s.memory = 2;
    for (int n : memoryChoices())
        if (n > 0 && n <= targetRam)
            s.memory = n;
    const bool constrained = h.softwareGpu || threads <= 4 || h.ramGB <= 8 || targetCpu <= 2 || targetRam <= 3;
    const int shortEdge = std::min(h.monitorW, h.monitorH);
    s.display = constrained || shortEdge < 900 ? 0 : 2;
    int ceiling = h.softwareGpu ? 30 : constrained ? 60 : !dedicated ? 90 : h.vramMB < 6000 || threads < 12 || targetRam < 6 ? 120 : 144;
    // Never run above the display's current refresh rate. The 2 Hz tolerance
    // handles modes Windows reports as 59/119/143 Hz.
    s.rate = 30;
    for (int hz : refreshRates())
        if (hz <= ceiling && hz <= std::max(30, h.monitorHz) + 2)
            s.rate = hz;
    s.gpu = h.softwareGpu ? GPU_SOFTWARE : GPU_HOST;
    s.boost = threads >= 8 && cpuLimit >= 4;
    s.snappy = true;
    s.quickBoot = false;
    s.bootScreen = true;
    s.frame = true;
    return s;
}

Settings recommended() {
    const auto &h = host();
    int cpu = h.threads, ram = h.ramGB - std::max(3, h.ramGB / 4), currentRam = 0;
    for (const auto &i : instances::list())
        if (i.active) {
            if (i.current)
                currentRam = i.memory;
            else {
                cpu -= i.cores;
                ram -= i.memory;
            }
        }
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    int available = GlobalMemoryStatusEx(&ms) ? int(ms.ullAvailPhys >> 30) + currentRam : -1;
    return settingsForHardware(h, cpu, ram, available);
}

// ------------------------------------------------------------------------------------------
// Settings

std::wstring iniPath() {
    return dataFolder() + L"\\settings.ini";
}

namespace {

std::wstring readStr(const wchar_t *section, const wchar_t *key) {
    wchar_t buf[1024] = L"";
    GetPrivateProfileStringW(section, key, L"", buf, 1024, iniPath().c_str());
    return buf;
}
int readIntIn(const wchar_t *section, const wchar_t *key, int def) {
    return static_cast<int>(GetPrivateProfileIntW(section, key, def, iniPath().c_str()));
}
void writeStr(const wchar_t *section, const wchar_t *key, const std::wstring &v) {
    WritePrivateProfileStringW(section, key, v.c_str(), iniPath().c_str());
}

int pick(int v, const std::vector<int> &allowed, int def) {
    return std::find(allowed.begin(), allowed.end(), v) != allowed.end() ? v : def;
}

// A quick-boot snapshot is only resumed when Velox itself saved it cleanly and nothing has used
// the phone since. That is recorded next to the phone (shared by every copy of Velox and by
// Android Studio) with a fingerprint of its disk and snapshot files: if anything else runs the
// phone, the fingerprint changes and the next start does a normal full boot.
std::wstring trustFile() {
    return g_paths.avdDir + L"\\velox-snapshot.ini";
}

std::wstring fingerprint() {
    std::wstring fp;
    for (const wchar_t *f : {L"\\userdata-qemu.img.qcow2", L"\\sdcard.img.qcow2", L"\\snapshots\\default_boot\\snapshot.pb"}) {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (!GetFileAttributesExW((g_paths.avdDir + f).c_str(), GetFileExInfoStandard, &fa)) {
            fp += L"-;";
            continue;
        }
        uint64_t t = (static_cast<uint64_t>(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime;
        uint64_t n = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
        fp += std::to_wstring(t) + L":" + std::to_wstring(n) + L";";
    }
    return fp;
}

bool snapshotTrusted(const std::wstring &sig) {
    wchar_t a[512] = L"", b[512] = L"";
    GetPrivateProfileStringW(L"snapshot", L"signature", L"", a, 512, trustFile().c_str());
    GetPrivateProfileStringW(L"snapshot", L"fingerprint", L"", b, 512, trustFile().c_str());
    return *a && sig == a && fingerprint() == b;
}

void trustSnapshot(const std::wstring &sig) {
    std::wstring fp = fingerprint();
    WritePrivateProfileStringW(L"snapshot", L"signature", sig.c_str(), trustFile().c_str());
    WritePrivateProfileStringW(L"snapshot", L"fingerprint", fp.c_str(), trustFile().c_str());
}

bool validPhoneNumber(const std::wstring &number) {
    if (number.empty())
        return true;
    if (number.size() > 20 || number == L"+")
        return false;
    for (size_t i = 0; i < number.size(); i++)
        if ((number[i] < L'0' || number[i] > L'9') && !(i == 0 && number[i] == L'+'))
            return false;
    return true;
}

void loadFrom(Settings &s, const wchar_t *sec) {
    Settings d = recommended();
    auto rb = [&](const wchar_t *key, bool def) { return readIntIn(sec, key, def ? 1 : 0) != 0; };
    std::wstring key = readStr(sec, L"screen");
    s.display = d.display;
    for (size_t i = 0; i < displays().size(); i++)
        if (key == displays()[i].key)
            s.display = static_cast<int>(i);
    s.rate = pick(readIntIn(sec, L"hz", d.rate), refreshRates(), d.rate);
    s.cores = pick(readIntIn(sec, L"cpu", 0), coreChoices(), 0);
    s.memory = pick(readIntIn(sec, L"ram", 0), memoryChoices(), 0);
    s.gpu = std::clamp(readIntIn(sec, L"gpu", d.gpu), 0, 2);
    s.root = rb(L"root", false);
    s.snappy = rb(L"snappy", true);
    s.boost = rb(L"boost", d.boost);
    s.quickBoot = rb(L"quickboot", d.quickBoot);
    s.bootScreen = rb(L"bootscreen", true);
    s.frame = rb(L"frame", d.frame);
    s.autoUpdate = rb(L"autoupdate", true);
    s.autoStart = rb(L"autostart", false);
    s.showTouches = rb(L"touches", false);
    s.autoRotate = rb(L"autorotate", true);
    s.gestureNavigation = rb(L"gesturenavigation", false);
    s.reverseButtons = rb(L"reversebuttons", false);
    s.brand = std::clamp(readIntIn(sec, L"brand", 2), 0, 3);
    s.bootStyle = std::clamp(readIntIn(sec, L"bootstyle", 0), 0, 5);
    s.cursor = std::clamp(readIntIn(sec, L"cursor", 0), 0, static_cast<int>(art::cursorNames().size()) - 1);
    s.number = readStr(sec, L"number");
    s.language = readIntIn(sec, L"language", 0) == 1 ? 1 : 0;
    s.backgroundRate = readIntIn(sec, L"backgroundfps", 0);
    if (s.backgroundRate != 15 && s.backgroundRate != 30 && s.backgroundRate != 60)
        s.backgroundRate = 0;
    s.backgroundMute = rb(L"backgroundmute", false);
    if (!validPhoneNumber(s.number))
        s.number.clear();
}

void saveTo(const Settings &s, const wchar_t *sec) {
    auto b = [](bool v) { return std::wstring(v ? L"1" : L"0"); };
    writeStr(sec, L"configured", L"1");
    writeStr(sec, L"screen", displays()[s.display].key);
    writeStr(sec, L"hz", std::to_wstring(s.rate));
    writeStr(sec, L"cpu", std::to_wstring(s.cores));
    writeStr(sec, L"ram", std::to_wstring(s.memory));
    writeStr(sec, L"gpu", std::to_wstring(s.gpu));
    writeStr(sec, L"root", b(s.root));
    writeStr(sec, L"snappy", b(s.snappy));
    writeStr(sec, L"boost", b(s.boost));
    writeStr(sec, L"quickboot", b(s.quickBoot));
    writeStr(sec, L"bootscreen", b(s.bootScreen));
    writeStr(sec, L"frame", b(s.frame));
    writeStr(sec, L"autoupdate", b(s.autoUpdate));
    writeStr(sec, L"autostart", b(s.autoStart));
    writeStr(sec, L"touches", b(s.showTouches));
    writeStr(sec, L"autorotate", b(s.autoRotate));
    writeStr(sec, L"gesturenavigation", b(s.gestureNavigation));
    writeStr(sec, L"reversebuttons", b(s.reverseButtons));
    writeStr(sec, L"brand", std::to_wstring(s.brand));
    writeStr(sec, L"bootstyle", std::to_wstring(s.bootStyle));
    writeStr(sec, L"cursor", std::to_wstring(s.cursor));
    writeStr(sec, L"number", s.number);
    writeStr(sec, L"language", std::to_wstring(s.language));
    writeStr(sec, L"backgroundfps", std::to_wstring(s.backgroundRate));
    writeStr(sec, L"backgroundmute", b(s.backgroundMute));
}

} // namespace

void loadSettings(Settings &s, bool &firstRun) {
    firstRun = readIntIn(L"launcher", L"configured", 0) == 0;
    if (firstRun) {
        s = recommended();
        s.autoStart = true;
        s.language = readIntIn(L"launcher", L"language", PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_GERMAN ? 1 : 0) == 1 ? 1 : 0;
        // Keep what the older launcher already knew.
        s.number = readStr(L"phone", L"number");
        if (!validPhoneNumber(s.number))
            s.number.clear();
        s.root = readIntIn(L"phone", L"root", 0) == 1;
        return;
    }
    loadFrom(s, L"launcher");
}

void saveSettings(const Settings &s) {
    saveTo(s, L"launcher");
    backgroundPolicy(s.backgroundRate, s.backgroundMute, s.rate);
}

std::wstring gpuMode(const Settings &s) {
    if (s.gpu == GPU_SOFTWARE)
        return L"swiftshader";
    if (s.gpu == GPU_HOST)
        return host().softwareGpu ? L"swiftshader" : L"host";
    return host().softwareGpu ? L"swiftshader" : L"host";
}

std::wstring signature(const Settings &s) {
    const Display &d = displays()[s.display];
    return std::to_wstring(d.w) + L"x" + std::to_wstring(d.h) + L"@" + std::to_wstring(d.dpi) + L"," + std::to_wstring(s.rate) + L"hz," +
           std::to_wstring(coresFor(s)) + L"c," + std::to_wstring(memoryFor(s)) + L"g," + gpuMode(s) + L"," +
           (s.root ? L"root" : L"stock") + L"," + (s.frame ? L"frame" : L"plain") + L"," + s.number;
}

// ------------------------------------------------------------------------------------------
// Paths

const Paths &paths() {
    return g_paths;
}

namespace {

// True if `file` has a valid Authenticode signature from Google LLC. Velox only ever runs an
// emulator and adb that Google signed, wherever they were found.
bool signedByGoogle(const std::wstring &file) {
    WINTRUST_FILE_INFO fi{};
    fi.cbStruct = sizeof(fi);
    fi.pcwszFilePath = file.c_str();
    WINTRUST_DATA wd{};
    wd.cbStruct = sizeof(wd);
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    bool ok = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &wd) == ERROR_SUCCESS;
    if (ok) {
        ok = false;
        CRYPT_PROVIDER_DATA *prov = WTHelperProvDataFromStateData(wd.hWVTStateData);
        CRYPT_PROVIDER_SGNR *signer = prov ? WTHelperGetProvSignerFromChain(prov, 0, FALSE, 0) : nullptr;
        if (signer && signer->csCertChain && signer->pasCertChain && signer->pasCertChain[0].pCert) {
            wchar_t name[256] = L"";
            CertGetNameStringW(signer->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, 256);
            ok = wcscmp(name, L"Google LLC") == 0;
        }
    }
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &wd);
    return ok;
}

bool goodSdk(const std::wstring &dir) {
    std::wstring emu = dir + L"\\emulator\\emulator.exe", adbPath = dir + L"\\platform-tools\\adb.exe";
    return exists(emu) && exists(adbPath) && signedByGoogle(emu) && signedByGoogle(adbPath);
}

} // namespace

namespace {
int cpuListCount(const std::string &text) {
    if (text.empty() || text.size() > 512 || text.back() == ',')
        return 0;
    std::istringstream list(text);
    std::string part;
    int count = 0, previous = -1;
    while (std::getline(list, part, ',')) {
        int first = -1, last = -1;
        char extra = 0;
        if (sscanf(part.c_str(), "%d-%d%c", &first, &last, &extra) == 2) {
        } else if (sscanf(part.c_str(), "%d%c", &first, &extra) == 1)
            last = first;
        else
            return 0;
        if (first <= previous || last < first || last >= MAX_CPU_CORES)
            return 0;
        count += last - first + 1;
        previous = last;
    }
    return count;
}
void loadCpuCapability() {
    g_cpuLimit = MAX_CPU_CORES;
    g_cpuCapabilityKey.clear();
    auto kernel = g_paths.sdk + L"\\system-images\\android-35\\google_apis_playstore\\x86_64\\kernel-ranchu";
    auto digest = net::hashFile(kernel, net::Hash::Sha256);
    if (digest.empty())
        return;
    std::ifstream properties{std::filesystem::path(g_paths.sdk + L"\\emulator\\source.properties")};
    char bytes[4096]{};
    properties.read(bytes, sizeof(bytes));
    std::string identity =
        digest + "|" + narrow(g_paths.sdk) + "|" + std::to_string(host().threads) + "|" + std::string(bytes, size_t(properties.gcount()));
    g_cpuCapabilityKey = widen(net::hashBytes(identity.data(), identity.size(), net::Hash::Sha256));
    auto file = dataFolder() + L"\\hardware.ini";
    wchar_t key[80]{};
    GetPrivateProfileStringW(L"android", L"identity", L"", key, 80, file.c_str());
    int limit = int(GetPrivateProfileIntW(L"android", L"cpu-limit", 0, file.c_str()));
    if (g_cpuCapabilityKey == key && limit >= 1 && limit <= MAX_CPU_CORES)
        g_cpuLimit = limit;
}
void observeCpuCapability(int requested, int present) {
    if (present < 1 || present >= requested || g_cpuCapabilityKey.empty())
        return;
    g_cpuLimit = std::min(g_cpuLimit.load(), present);
    auto file = dataFolder() + L"\\hardware.ini";
    std::ofstream out{std::filesystem::path(file + L".new"), std::ios::binary | std::ios::trunc};
    out << "[android]\nidentity=" << narrow(g_cpuCapabilityKey) << "\ncpu-limit=" << g_cpuLimit.load() << "\n";
    out.close();
    if (out)
        MoveFileExW((file + L".new").c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    instances::lowerCpuAllocation(present);
}
} // namespace

void findPaths() {
    Paths p;
    std::wstring explicitSdk = readStr(L"launcher", L"sdk"), home = readStr(L"launcher", L"home");
    std::vector<std::wstring> homes;
    if (!home.empty())
        homes.push_back(home + L"\\avd");
    if (instances::id().empty()) {
        homes.push_back(exeFolder() + L"Android\\avd");
        homes.push_back(envVar(L"ANDROID_AVD_HOME"));
        homes.push_back(envVar(L"USERPROFILE") + L"\\.android\\avd");
    }
    for (auto &hh : homes) {
        if (!hh.empty() && exists(hh + L"\\" + AVD_NAME + L".avd\\config.ini")) {
            p.avdHome = hh;
            p.avdDir = hh + L"\\" + AVD_NAME + L".avd";
            break;
        }
    }
    // An SDK chosen explicitly, or installed by Velox's own setup, comes first. Only folders the
    // user's own programs write to are searched, never other apps' private (sandboxed) folders.
    std::vector<std::wstring> sdks = {explicitSdk};
    if (!home.empty())
        sdks.push_back(home + L"\\Sdk");
    sdks.push_back(exeFolder() + L"Android\\Sdk");
    for (const wchar_t *v : {L"ANDROID_SDK_ROOT", L"ANDROID_HOME"})
        sdks.push_back(envVar(v));
    sdks.push_back(envVar(L"LOCALAPPDATA") + L"\\Android\\Sdk");
    // The emulator records the SDK it last ran from in the phone's hardware-qemu.ini.
    if (!p.avdDir.empty()) {
        std::ifstream hw{std::filesystem::path(p.avdDir + L"\\hardware-qemu.ini")};
        for (std::string line; std::getline(hw, line);) {
            if (line.rfind("android.sdk.root", 0) != 0)
                continue;
            size_t eq = line.find('=');
            std::string v = eq == std::string::npos ? "" : line.substr(eq + 1);
            while (!v.empty() && (v.front() == ' '))
                v.erase(0, 1);
            while (!v.empty() && (v.back() == ' ' || v.back() == '\r' || v.back() == '\\' || v.back() == '/'))
                v.pop_back();
            sdks.push_back(widen(v));
            break;
        }
    }
    for (auto &c : sdks) {
        if (!c.empty() && goodSdk(c)) {
            p.sdk = c;
            if (c != explicitSdk)
                writeStr(L"launcher", L"sdk", c); // found the slow way: remember it
            break;
        }
    }
    // The root image: where root setup saved it, else next to the exe, else in the data folder.
    p.rootImage = readStr(L"launcher", L"rootimage");
    for (const std::wstring &c : {exeFolder() + L"root\\ramdisk-root.img", dataFolder() + L"\\root\\ramdisk-root.img"})
        if (p.rootImage.empty() || !exists(p.rootImage))
            p.rootImage = c;
    p.log = dataFolder() + L"\\emulator.log";
    g_paths = p;
    loadCpuCapability();
}

bool useSdkFolder(const std::wstring &dir) {
    std::wstring d = dir;
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/'))
        d.pop_back();
    if (!goodSdk(d))
        return false;
    writeStr(L"launcher", L"sdk", d);
    findPaths();
    return true;
}

bool installed() {
    return !g_paths.sdk.empty() && !g_paths.avdDir.empty();
}
bool rootReady() {
    return exists(g_paths.rootImage);
}

// ------------------------------------------------------------------------------------------
// Processes

namespace {

// Starts a hidden process that inherits exactly its three std handles and nothing else, so no
// child ever holds on to files other threads have open (downloads, logs, pipes).
bool spawn(std::wstring cmd, HANDLE in, HANDLE out, HANDLE err, const wchar_t *dir, PROCESS_INFORMATION &pi) {
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in;
    si.StartupInfo.hStdOutput = out;
    si.StartupInfo.hStdError = err;
    std::vector<HANDLE> list;
    for (HANDLE h : {in, out, err})
        if (h && h != INVALID_HANDLE_VALUE && std::find(list.begin(), list.end(), h) == list.end())
            list.push_back(h);
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<BYTE> buf(size);
    auto *attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buf.data());
    bool initialized = !list.empty() && InitializeProcThreadAttributeList(attrs, 1, 0, &size);
    if (!initialized)
        return false;
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list.data(), list.size() * sizeof(HANDLE), nullptr,
                                   nullptr)) {
        DeleteProcThreadAttributeList(attrs);
        return false;
    }
    si.lpAttributeList = attrs;
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, dir,
                             &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    return ok != 0;
}

// Runs a command with no window. Output goes to `out` (or the file `toFile`); the command is
// stopped if it takes longer than `timeoutMs`, so a stuck adb can never hang the launcher.
DWORD run(std::wstring cmd, std::string *out, DWORD timeoutMs, HANDLE toFile = nullptr, bool *timedOut = nullptr) {
    if (out)
        out->clear();
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (out && !CreatePipe(&rd, &wr, &sa, 0))
        return 1;
    if (rd)
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    PROCESS_INFORMATION pi{};
    bool ok = spawn(cmd, nul, out ? wr : (toFile ? toFile : nul), out ? wr : nul, nullptr, pi);
    if (wr)
        CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE)
        CloseHandle(nul);
    if (!ok) {
        if (rd)
            CloseHandle(rd);
        return 1;
    }
    bool outputOverflow = false;
    auto drain = [&] {
        DWORD avail = 0;
        char buf[8192];
        for (int chunks = 0; chunks < 128 && rd && PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail; chunks++) {
            DWORD n = 0;
            if (!ReadFile(rd, buf, std::min<DWORD>(avail, sizeof(buf)), &n, nullptr) || !n)
                break;
            if (out->size() + n > (64ull << 20)) {
                outputOverflow = true;
                break;
            }
            out->append(buf, n);
        }
    };
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    bool late = false;
    for (;;) {
        drain();
        if (outputOverflow) {
            TerminateProcess(pi.hProcess, 1);
            break;
        }
        if (WaitForSingleObject(pi.hProcess, 10) == WAIT_OBJECT_0) {
            drain();
            break;
        }
        if (GetTickCount64() > deadline) {
            TerminateProcess(pi.hProcess, 1);
            late = true;
            break;
        }
    }
    if (timedOut)
        *timedOut = late;
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (rd)
        CloseHandle(rd);
    return late || outputOverflow ? 1 : code;
}

std::wstring adbExe() {
    return q(g_paths.sdk + L"\\platform-tools\\adb.exe");
}

// The phone's emulator console port. New phones get 5580 so they never take the port Android
// Studio's emulators use first (5554); an already running phone keeps the port it has.
constexpr int DEFAULT_PORT = 5580;
std::atomic<int> g_port{DEFAULT_PORT};
std::wstring serial() {
    return L"emulator-" + std::to_wstring(g_port.load());
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    while (!s.empty() && (s.front() == '\n' || s.front() == '\r' || s.front() == ' '))
        s.erase(0, 1);
    return s;
}

struct Proc {
    DWORD pid, parent;
    std::wstring exe;
};

std::vector<Proc> processList() {
    std::vector<Proc> list;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return list;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            list.push_back({pe.th32ProcessID, pe.th32ParentProcessID, lower(pe.szExeFile)});
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return list;
}

uint64_t creationTime(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
        return 0;
    FILETIME c, e, k, u;
    uint64_t t = GetProcessTimes(h, &c, &e, &k, &u) ? (static_cast<uint64_t>(c.dwHighDateTime) << 32 | c.dwLowDateTime) : 0;
    CloseHandle(h);
    return t;
}

// Exactly our phone: the qemu process whose window is "Android Emulator - Gaming_Phone:<port>",
// or, before that window exists, the qemu started by the emulator.exe Velox launched. Other
// emulators (Android Studio's, other AVDs) are never matched.
struct Found {
    DWORD qemu = 0, wrapper = 0;
    int port = 0;
};
std::mutex g_scanLock;
Found g_found;
ULONGLONG g_scanTime = 0;
DWORD g_launchedPid = 0; // emulator.exe started by this Velox
DWORD g_wrapperPid = 0;  // the emulator.exe wrapper of our phone, for cleanup
uint64_t g_wrapperBorn = 0;

struct ScanCtx {
    Found *f;
    const std::vector<Proc> *procs;
    int port;
};

std::wstring exeOf(const std::vector<Proc> &procs, DWORD pid) {
    for (auto &p : procs)
        if (p.pid == pid)
            return p.exe;
    return {};
}

Found scan(bool fresh = false) {
    std::lock_guard<std::mutex> l(g_scanLock);
    ULONGLONG now = GetTickCount64();
    if (!fresh && now - g_scanTime < 250)
        return g_found;
    Found f;
    std::vector<Proc> procs = processList();
    ScanCtx ctx{&f, &procs, instances::port()};
    EnumWindows(
        [](HWND w, LPARAM lp) -> BOOL {
            auto *c = reinterpret_cast<ScanCtx *>(lp);
            // Check the owner first: reading the title of one of Velox's own windows would send a
            // message to the UI thread, which may be waiting for this scan (a deadlock).
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            if (pid == GetCurrentProcessId() || exeOf(*c->procs, pid) != L"qemu-system-x86_64.exe")
                return TRUE;
            wchar_t title[160] = L"";
            if (!GetWindowTextW(w, title, 160))
                return TRUE;
            std::wstring prefix = std::wstring(L"Android Emulator - ") + AVD_NAME + L":";
            if (wcsncmp(title, prefix.c_str(), prefix.size()) != 0)
                return TRUE;
            if (_wtoi(title + prefix.size()) != c->port)
                return TRUE;
            DWORD parent = 0;
            for (const auto &process : *c->procs)
                if (process.pid == pid) {
                    parent = process.parent;
                    break;
                }
            bool launched = parent && parent == g_launchedPid && g_emuProcess && WaitForSingleObject(g_emuProcess, 0) == WAIT_TIMEOUT;
            bool recorded =
                DWORD(readIntIn(L"running", L"pid", 0)) == pid && readStr(L"running", L"born") == std::to_wstring(creationTime(pid));
            if (!launched && !recorded && !instances::ownsWrapper(parent))
                return TRUE;
            c->f->qemu = pid;
            c->f->port = _wtoi(title + prefix.size());
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    if (!f.qemu && g_launchedPid && g_emuProcess && WaitForSingleObject(g_emuProcess, 0) == WAIT_TIMEOUT) {
        for (auto &p : procs) {
            if (p.parent == g_launchedPid && p.exe == L"qemu-system-x86_64.exe") {
                f.qemu = p.pid;
                f.port = g_port;
            }
        }
    }
    if (f.qemu) {
        for (auto &p : procs) {
            if (p.pid == f.qemu && exeOf(procs, p.parent) == L"emulator.exe") {
                f.wrapper = p.parent;
                if (g_wrapperPid != p.parent) {
                    g_wrapperPid = p.parent;
                    g_wrapperBorn = creationTime(p.parent);
                }
            }
        }
        if (f.port > 0)
            g_port = f.port;
    }
    g_found = f;
    g_scanTime = now;
    return f;
}

void terminatePid(DWORD pid, const wchar_t *expectExe, uint64_t born = 0) {
    if (!pid)
        return;
    HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!h)
        return;
    wchar_t path[MAX_PATH * 2]{};
    DWORD n = static_cast<DWORD>(std::size(path));
    bool nameOk = false;
    if (QueryFullProcessImageNameW(h, 0, path, &n) && n < std::size(path)) {
        path[n] = L'\0';
        const wchar_t *leaf = wcsrchr(path, L'\\');
        nameOk = _wcsicmp(leaf ? leaf + 1 : path, expectExe) == 0;
    }
    FILETIME c, e, k, u;
    bool timeOk =
        !born || (GetProcessTimes(h, &c, &e, &k, &u) && (static_cast<uint64_t>(c.dwHighDateTime) << 32 | c.dwLowDateTime) == born);
    if (nameOk && timeOk) {
        TerminateProcess(h, 1);
        WaitForSingleObject(h, 5000);
    }
    CloseHandle(h);
}

// Closes our phone's emulator.exe wrapper if it is still around after the phone has closed.
// (The other emulator.exe processes seen after a shutdown are the emulator's own watchdogs,
// "emulator -kill <pid> -sleep 20"; they end by themselves and don't block a new start.)
void closeWrapper() {
    if (g_emuProcess && WaitForSingleObject(g_emuProcess, 0) == WAIT_TIMEOUT) {
        TerminateProcess(g_emuProcess, 1);
        WaitForSingleObject(g_emuProcess, 5000);
    } else if (g_wrapperPid) {
        terminatePid(g_wrapperPid, L"emulator.exe", g_wrapperBorn);
    }
    g_wrapperPid = 0;
    g_wrapperBorn = 0;
}

bool wrapperAlive() {
    if (g_emuProcess)
        return WaitForSingleObject(g_emuProcess, 0) == WAIT_TIMEOUT;
    return g_wrapperPid && g_wrapperBorn && creationTime(g_wrapperPid) == g_wrapperBorn;
}

void hardKill() {
    Found f = scan(true);
    if (f.qemu) {
        for (auto &p : processList())
            if (p.parent == f.qemu && p.exe == L"netsimd.exe")
                terminatePid(p.pid, L"netsimd.exe");
        terminatePid(f.qemu, L"qemu-system-x86_64.exe");
    }
    closeWrapper();
    if (g_emuProcess) {
        CloseHandle(g_emuProcess);
        g_emuProcess = nullptr;
    }
    g_launchedPid = 0;
    scan(true);
}

std::string readLog() {
    std::ifstream in{std::filesystem::path(g_paths.log), std::ios::binary};
    in.seekg(0, std::ios::end);
    auto size = in.tellg();
    if (size <= 0)
        return {};
    auto count = static_cast<std::streamoff>(std::min<std::streamoff>(size, 8 << 20));
    in.seekg(size - count);
    std::string text(static_cast<size_t>(count), '\0');
    in.read(text.data(), count);
    text.resize(static_cast<size_t>(in.gcount()));
    return text;
}

// The last line the emulator flagged as a problem, made readable.
std::wstring logProblem() {
    std::string log = readLog();
    std::string best;
    size_t pos = 0;
    while (pos < log.size()) {
        size_t end = log.find('\n', pos);
        if (end == std::string::npos)
            end = log.size();
        std::string line = trim(log.substr(pos, end - pos));
        pos = end + 1;
        if (line.find("FATAL") != std::string::npos || line.find("PANIC") != std::string::npos ||
            (line.find("ERROR") != std::string::npos && line.find("adb") == std::string::npos))
            best = line;
    }
    size_t bar = best.find('|');
    if (bar != std::string::npos)
        best = trim(best.substr(bar + 1));
    if (best.size() > 140)
        best = best.substr(0, 140) + "…";
    return widen(best);
}

// True only if the emulator really saved its state on the way out. After a Vulkan app (most
// games) it can't, logs "Not saving state", and leaves a stub that must never be resumed.
bool logSavedSnapshot() {
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(g_paths.log.c_str(), GetFileExInfoStandard, &attributes) || attributes.nFileSizeHigh ||
        attributes.nFileSizeLow > (8u << 20))
        return false;
    std::string log = readLog();
    return log.find("Saving snapshot 'default_boot' took") != std::string::npos && log.find("Not saving state") == std::string::npos;
}

void defaultEmulatorTheme() {
    // Qt stores this application preference in HKCU. Seed Dark only on first use;
    // the emulator's own Settings page remains free to change it later.
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Android Open Source Project\\Emulator\\set", 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    DWORD size = 0;
    if (RegQueryValueExW(key, L"theme", nullptr, nullptr, nullptr, &size) == ERROR_FILE_NOT_FOUND) {
        const DWORD dark = 1;
        RegSetValueExW(key, L"theme", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&dark), sizeof(dark));
    }
    RegCloseKey(key);
}

// Writes the chosen hardware into the AVD's config.ini, keeping everything else.
bool writeConfig(const Settings &s) {
    std::wstring path = g_paths.avdDir + L"\\config.ini";
    std::ifstream in{std::filesystem::path(path)};
    if (!in)
        return false;
    std::vector<std::string> lines;
    for (std::string l; std::getline(in, l);) {
        if (!l.empty() && l.back() == '\r')
            l.pop_back();
        lines.push_back(l);
    }
    in.close();
    const Display &d = displays()[s.display];
    std::string skinName = std::to_string(d.w) + "x" + std::to_string(d.h), skinPath = "_no_skin";
    if (s.frame) {
        std::wstring dir = dataFolder() + L"\\skins\\frame_" + std::to_wstring(d.w) + L"x" + std::to_wstring(d.h);
        if (exists(dir + L"\\layout") || art::writeDeviceFrame(dir, d.w, d.h)) {
            skinName = "frame_" + skinName;
            skinPath = narrow(dir);
        }
    }
    const std::pair<std::string, std::string> values[] = {
        {"hw.lcd.width", std::to_string(d.w)},
        {"hw.lcd.height", std::to_string(d.h)},
        {"hw.lcd.density", std::to_string(d.dpi)},
        {"hw.lcd.vsync", std::to_string(s.rate)},
        {"hw.cpu.ncore", std::to_string(coresFor(s))},
        {"hw.ramSize", std::to_string(memoryFor(s) * 1024)},
        {"hw.gpu.enabled", "yes"},
        {"hw.gpu.mode", narrow(gpuMode(s))},
        {"hw.camera.back", "emulated"}, // the 3D "virtual scene" camera is slow to load
        {"hw.keyboard", "yes"},
        {"skin.name", skinName},
        {"skin.path", skinPath},
        {"skin.dynamic", "no"},
        {"showDeviceFrame", s.frame && skinPath != "_no_skin" ? "yes" : "no"},
        {"fastboot.forceColdBoot", "no"},
        {"fastboot.forceFastBoot", "yes"},
    };
    for (auto &[key, value] : values) {
        bool found = false;
        for (auto &l : lines) {
            size_t eq = l.find('=');
            if (eq == std::string::npos)
                continue;
            std::string k = l.substr(0, eq);
            while (!k.empty() && k.back() == ' ')
                k.pop_back();
            if (k == key) {
                l = key + "=" + value;
                found = true;
            }
        }
        if (!found)
            lines.push_back(key + "=" + value);
    }
    std::wstring tmp = path + L".new";
    {
        std::ofstream o(std::filesystem::path(tmp), std::ios::binary | std::ios::trunc);
        for (auto &l : lines)
            o << l << "\r\n";
        if (!o)
            return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

void setPriority(bool boost) {
    DWORD pid = scan().qemu;
    if (!pid)
        return;
    if (HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid)) {
        SetPriorityClass(h, boost ? ABOVE_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);
        CloseHandle(h);
    }
}

int lastBootSeconds() {
    return std::clamp(readIntIn(L"state", L"lastboot", 25), 8, 120);
}

// What the running phone was started with is kept in [running], so a relaunched Velox knows it.
// It is tied to the exact qemu process (pid + start time); anything else counts as unknown.
std::atomic<bool> g_runningKnown{false};

void rememberRunning(const Settings &s) {
    saveTo(s, L"running");
    DWORD pid = scan(true).qemu;
    writeStr(L"running", L"pid", std::to_wstring(pid));
    writeStr(L"running", L"born", std::to_wstring(creationTime(pid)));
    writeStr(L"running", L"port", std::to_wstring(g_port.load()));
    g_runningKnown = true;
}

} // namespace

bool runningKnown() {
    return g_runningKnown;
}

void distrustSnapshot() {
    if (!g_paths.avdDir.empty())
        DeleteFileW(trustFile().c_str());
}

bool adoptRunning() {
    Found f = scan(true);
    g_runningKnown = false;
    if (!f.qemu || readIntIn(L"running", L"configured", 0) != 1)
        return false;
    if (static_cast<DWORD>(readIntIn(L"running", L"pid", 0)) != f.qemu)
        return false;
    if (readStr(L"running", L"born") != std::to_wstring(creationTime(f.qemu)))
        return false;
    loadFrom(running, L"running");
    g_runningKnown = true;
    return true;
}

void forgetRunning() {
    g_runningKnown = false;
    WritePrivateProfileStringW(L"running", nullptr, nullptr, iniPath().c_str());
}

void startAdbServer() {
    if (g_paths.sdk.empty())
        return;
    run(adbExe() + L" start-server", nullptr, 20000);
}

std::string adb(const std::wstring &args, DWORD timeoutMs) {
    std::string out;
    run(adbExe() + L" -s " + serial() + L" " + args, &out, timeoutMs);
    return trim(out);
}

std::wstring adbSerial() {
    return serial();
}

bool emulatorRunning() {
    return scan().qemu != 0;
}

DWORD emulatorPid() {
    return scan().qemu;
}

void applyLive(const Settings &s) {
    static std::mutex order; // one at a time, so the newest settings always land last
    std::lock_guard<std::mutex> l(order);
    setPriority(s.boost);
    const wchar_t *scale = s.snappy ? L"0.5" : L"1.0";
    std::wstring cmd = L"shell \"";
    for (const wchar_t *key : {L"window_animation_scale", L"transition_animation_scale", L"animator_duration_scale"})
        cmd += std::wstring(L"settings put global ") + key + L" " + scale + L"; ";
    // Android's "smooth display" settings decide what apps draw at; pin both ends to the rate.
    cmd += L"settings put system peak_refresh_rate " + std::to_wstring(s.rate) + L"; ";
    cmd += L"settings put system min_refresh_rate " + std::to_wstring(s.rate) + L"; ";
    cmd += L"settings put system show_touches " + std::wstring(s.showTouches ? L"1" : L"0") + L"\"";
    adb(cmd, 15000);
    backgroundPolicy(s.backgroundRate, s.backgroundMute, s.rate);
}

namespace {
bool prepareAndroidConfig(std::wstring &error) {
    std::wstring user = envVar(L"ANDROID_USER_HOME");
    if (user.empty()) {
        std::wstring parent = envVar(L"ANDROID_SDK_HOME");
        if (parent.empty())
            parent = envVar(L"USERPROFILE");
        if (parent.empty()) {
            error = i18n::text(L"The Android user profile folder couldn't be found.");
            return false;
        }
        user = parent + L"\\.android";
    }
    std::wstring emulator = envVar(L"ANDROID_EMULATOR_HOME");
    if (emulator.empty())
        emulator = user;
    for (const auto &directory : {user, emulator}) {
        createDirs(directory);
        std::wstring probe =
            directory + L"\\.velox-start-check-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            error =
                i18n::text(L"Can't write Android configuration in ") + directory + i18n::text(L". Choose a writable Android user folder.");
            return false;
        }
        CloseHandle(file);
    }
    if (!SetEnvironmentVariableW(L"ANDROID_USER_HOME", user.c_str()) ||
        !SetEnvironmentVariableW(L"ANDROID_EMULATOR_HOME", emulator.c_str())) {
        error = i18n::text(L"The Android configuration folders couldn't be selected.");
        return false;
    }
    return true;
}
} // namespace

namespace {
std::atomic<bool> cancelStart{false};
std::atomic<int> backgroundFps{0};
std::atomic<int> foregroundFps{120};
std::atomic<bool> muteBackground{false};
std::atomic<unsigned> backgroundGeneration{1};
unsigned limitedGeneration = 0;
std::mutex backgroundMutex;
int limitedRate = 0;
DWORD limitedPid = 0;
std::map<std::wstring, BOOL> mutedSessions;
} // namespace
void cancelStartup() {
    if (state == State::Starting)
        cancelStart = true;
}
void backgroundPolicy(int rate, bool mute, int foregroundRate) {
    backgroundFps = rate == 15 || rate == 30 || rate == 60 ? rate : 0;
    muteBackground = mute;
    if (foregroundRate > 0)
        foregroundFps = foregroundRate;
    ++backgroundGeneration;
}
void backgroundTick(HWND launcher, bool restore) {
    std::lock_guard<std::mutex> guard(backgroundMutex);
    if (state == State::Stopped && mutedSessions.empty()) {
        limitedRate = 0;
        limitedPid = 0;
        return;
    }
    auto pid = emulatorPid();
    struct Process {
        HANDLE handle = nullptr;
        ~Process() {
            if (handle)
                CloseHandle(handle);
        }
    } process{pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr};
    DWORD focus = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &focus);
    bool inactive = !restore && state == State::Running && pid && focus != pid && focus != GetCurrentProcessId();
    int normalRate = foregroundFps.load();
    int wanted = inactive && backgroundFps ? std::min(backgroundFps.load(), normalRate) : 0;
    auto generation = backgroundGeneration.load();
    if (state == State::Running && (wanted || limitedRate) &&
        (limitedRate != wanted || limitedPid != pid || generation != limitedGeneration)) {
        int rate = wanted ? wanted : normalRate;
        adb(L"shell \"settings put system peak_refresh_rate " + std::to_wstring(rate) + L"; settings put system min_refresh_rate " +
                std::to_wstring(rate) + L"\"",
            3000);
        limitedRate = wanted;
        limitedPid = pid;
        limitedGeneration = generation;
    }
    if (state == State::Stopped) {
        limitedRate = 0;
        limitedPid = 0;
    }
    bool mute = inactive && muteBackground;
    if (!mute && mutedSessions.empty())
        return;
    // Session-level muting preserves Android's volume and every other Windows app.
    HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *devices = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void **>(&devices)))) {
        IMMDeviceCollection *collection = nullptr;
        if (SUCCEEDED(devices->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) {
            UINT count = 0;
            collection->GetCount(&count);
            for (UINT d = 0; d < count; ++d) {
                IMMDevice *device = nullptr;
                IAudioSessionManager2 *manager = nullptr;
                IAudioSessionEnumerator *sessions = nullptr;
                if (SUCCEEDED(collection->Item(d, &device)) &&
                    SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_INPROC_SERVER, nullptr,
                                               reinterpret_cast<void **>(&manager))) &&
                    SUCCEEDED(manager->GetSessionEnumerator(&sessions))) {
                    int n = 0;
                    sessions->GetCount(&n);
                    for (int i = 0; i < n; ++i) {
                        IAudioSessionControl *control = nullptr;
                        IAudioSessionControl2 *identity = nullptr;
                        ISimpleAudioVolume *volume = nullptr;
                        if (SUCCEEDED(sessions->GetSession(i, &control)) &&
                            SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void **>(&identity))) &&
                            SUCCEEDED(control->QueryInterface(__uuidof(ISimpleAudioVolume), reinterpret_cast<void **>(&volume)))) {
                            DWORD owner = 0;
                            LPWSTR raw = nullptr;
                            identity->GetProcessId(&owner);
                            if (SUCCEEDED(identity->GetSessionInstanceIdentifier(&raw)) && raw) {
                                std::wstring key(raw);
                                auto saved = mutedSessions.find(key);
                                if (mute && owner == pid && process.handle && WaitForSingleObject(process.handle, 0) == WAIT_TIMEOUT) {
                                    if (saved == mutedSessions.end()) {
                                        BOOL before = FALSE;
                                        if (SUCCEEDED(volume->GetMute(&before)) && SUCCEEDED(volume->SetMute(TRUE, nullptr)))
                                            mutedSessions[key] = before;
                                    }
                                } else if (saved != mutedSessions.end() && SUCCEEDED(volume->SetMute(saved->second, nullptr)))
                                    mutedSessions.erase(saved);
                                CoTaskMemFree(raw);
                            }
                        }
                        if (volume)
                            volume->Release();
                        if (identity)
                            identity->Release();
                        if (control)
                            control->Release();
                    }
                }
                if (sessions)
                    sessions->Release();
                if (manager)
                    manager->Release();
                if (device)
                    device->Release();
            }
            collection->Release();
        }
        devices->Release();
    }
    if (SUCCEEDED(apartment))
        CoUninitialize();
    (void)launcher;
}

bool repairStartup(const Settings &settings, const Report &report) {
    instances::Maintenance lease(instances::id());
    if (!lease.acquired) {
        report(lease.error, true);
        return false;
    }
    if (!goodSdk(paths().sdk)) {
        report(i18n::text(L"Android tools could not be verified. Choose a Google-signed SDK on the Phone page."), true);
        return false;
    }
    if (!exists(paths().avdDir + L"\\userdata-qemu.img") && !exists(paths().avdDir + L"\\userdata-qemu.img.qcow2")) {
        report(i18n::text(L"Phone disks are missing. Restore a verified Vault backup before starting."), true);
        return false;
    }
    if (!writeConfig(settings)) {
        report(i18n::text(L"Couldn't save the phone's settings (config.ini)."), true);
        return false;
    }
    distrustSnapshot();
    instances::failure(L"");
    report(i18n::text(L"Startup repaired. The next start uses a full boot; apps and saves are kept."), false);
    return true;
}
void start(const Settings &s, const Report &report) {
    struct ResetCancel {
        ~ResetCancel() { cancelStart = false; }
    } resetCancel;
    backgroundPolicy(s.backgroundRate, s.backgroundMute, s.rate);
    setAutoRotation(s.autoRotate);
    setGestureNavigation(s.gestureNavigation, s.reverseButtons);
    g_navigationRestart = false;
    state = State::Starting;
    running = s;
    g_session++;
    boot.progress = 0;
    boot.done = false;
    boot.set(0.02f, i18n::text(L"Starting the emulator"));
    boot.active = true;
    auto fail = [&](const std::wstring &msg) {
        boot.active = false;
        state = emulatorRunning() ? State::Running : State::Stopped;
        if (state == State::Stopped) {
            forgetRunning();
            instances::stopped();
        }
        report(msg, true);
        instances::failure(msg);
    };
    auto cancelled = [&] {
        if (!cancelStart)
            return false;
        if (g_launchedPid && instances::ownsWrapper(g_launchedPid))
            hardKill();
        distrustSnapshot();
        boot.active = false;
        state = emulatorRunning() ? State::Running : State::Stopped;
        if (state == State::Stopped) {
            forgetRunning();
            instances::stopped();
        }
        report(i18n::text(L"Startup cancelled. Your phone data is kept."), false);
        return true;
    };
    if (cancelled())
        return;
    if (!validPhoneNumber(s.number))
        return fail(i18n::text(L"The phone number must contain digits and an optional leading + (up to 20 characters)."));
    std::wstring configurationError;
    if (!prepareAndroidConfig(configurationError))
        return fail(configurationError);
    if (!writeConfig(s))
        return fail(i18n::text(L"Couldn't save the phone's settings (config.ini)."));
    if (s.root && !rootReady())
        return fail(i18n::text(L"Root isn't set up yet. Start the phone without root and use Set up root."));
    std::wstring sig = signature(s);
    bool resume = s.quickBoot && snapshotTrusted(sig) && exists(g_paths.avdDir + L"\\snapshots\\default_boot\\snapshot.pb");
    // Until the emulator saves cleanly at the end, the snapshot can't be trusted: if the session
    // crashes, the next start does a full boot and keeps everything written in between.
    distrustSnapshot();
    forgetRunning();
    if (!emulatorRunning())
        closeWrapper(); // a wrapper left from our last session
    startAdbServer();
    defaultEmulatorTheme();

    for (int attempt = 0; attempt < 2; attempt++) {
        if (cancelled())
            return;
        std::wstring allocationError;
        if (!instances::reserve(s, allocationError))
            return fail(allocationError);
        boot.resume = resume;
        g_port = instances::port();
        std::wstring cmd = q(g_paths.sdk + L"\\emulator\\emulator.exe") + L" -avd " + AVD_NAME + L" -port " +
                           std::to_wstring(instances::port()) + L" -gpu " + gpuMode(s) + L" -no-boot-anim -vsync-rate " +
                           std::to_wstring(s.rate) + L" -cores " + std::to_wstring(std::min(6, coresFor(s))) + L" -memory " +
                           std::to_wstring(memoryFor(s) * 1024) + L" -no-metrics -crash-report-mode disabled";
        if (s.root)
            cmd += L" -ramdisk " + q(g_paths.rootImage);
        if (!s.number.empty())
            cmd += L" -phone-number " + s.number; // validated here as well as in settings and the UI
        if (!s.quickBoot)
            cmd += L" -no-snapshot";
        else if (!resume)
            cmd += L" -no-snapshot-load";
        // The Android launcher clamps -cores to six. Its documented QEMU pass-through
        // accepts the full x86 topology; Android's online CPU list is validated in QA.
        if (coresFor(s) > 6)
            cmd += L" -qemu -smp cores=" + std::to_wstring(coresFor(s));

        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE logFile = CreateFileW(g_paths.log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        SetEnvironmentVariableW(L"ANDROID_SDK_ROOT", g_paths.sdk.c_str());
        SetEnvironmentVariableW(L"ANDROID_AVD_HOME", g_paths.avdHome.c_str());
        PROCESS_INFORMATION pi{};
        std::wstring workDir = g_paths.sdk + L"\\emulator";
        HANDLE outH = logFile != INVALID_HANDLE_VALUE ? logFile : nul;
        bool ok = spawn(cmd, nul, outH, outH, workDir.c_str(), pi);
        if (logFile != INVALID_HANDLE_VALUE)
            CloseHandle(logFile);
        if (nul != INVALID_HANDLE_VALUE)
            CloseHandle(nul);
        if (!ok)
            return fail(i18n::text(L"Couldn't start the emulator."));
        CloseHandle(pi.hThread);
        if (g_emuProcess)
            CloseHandle(g_emuProcess);
        g_emuProcess = pi.hProcess;
        g_launchedPid = pi.dwProcessId;
        if (!instances::started(pi.dwProcessId, g_paths.sdk, g_port.load())) {
            hardKill();
            return fail(i18n::text(L"Couldn't record this phone's ownership. Free disk space and check the Velox data folder."));
        }
        report(resume ? i18n::text(L"Resuming where you left off…") : i18n::text(L"Starting Android (full boot)…"), false);

        ULONGLONG t0 = GetTickCount64();
        float expected = resume ? 5.f : static_cast<float>(lastBootSeconds());
        bool online = false, booted = false, stuck = false;
        for (int tick = 0; !booted; tick++) {
            Sleep(250);
            if (cancelled())
                return;
            float el = (GetTickCount64() - t0) / 1000.f;
            WIN32_FILE_ATTRIBUTE_DATA logSize{};
            if (GetFileAttributesExW(g_paths.log.c_str(), GetFileExInfoStandard, &logSize) &&
                (logSize.nFileSizeHigh || logSize.nFileSizeLow > (16u << 20))) {
                hardKill();
                return fail(
                    i18n::text(L"The emulator repeatedly failed while starting and was stopped to limit its log. Check emulator.log."));
            }
            if (WaitForSingleObject(g_emuProcess, 0) == WAIT_OBJECT_0) {
                std::wstring why = logProblem();
                return fail(why.empty() ? i18n::text(L"The emulator closed while starting.") : i18n::text(L"The emulator stopped: ") + why);
            }
            float timePart = 1.f - std::exp(-2.2f * el / expected);
            if (!online)
                boot.set(0.04f + 0.36f * timePart, resume ? i18n::text(L"Loading your phone") : i18n::text(L"Starting Android"));
            else
                boot.set(std::max(0.45f, 0.06f + 0.88f * timePart), resume ? L"Waking up" : i18n::text(L"Loading apps and services"));
            if (tick % 3 == 0 && el > 1.f) {
                std::string st = adb(L"get-state", 2500);
                online = st == "device";
                if (online)
                    booted = adb(L"shell getprop sys.boot_completed", 3000) == "1";
            }
            if (resume && !booted && el > 25.f) {
                stuck = true;
                break;
            }
            if (!resume && el > 240.f) {
                hardKill();
                return fail(i18n::text(L"The phone took too long to start and was stopped. Use Startup repair in Recovery."));
            }
        }
        if (stuck) {
            // A snapshot that doesn't wake up: throw it away and boot normally.
            report(i18n::text(L"Instant start got stuck, doing a full boot instead…"), false);
            hardKill();
            resume = false;
            boot.progress = 0;
            boot.set(0.02f, i18n::text(L"Starting Android"));
            continue;
        }
        if (cancelled())
            return;
        float took = (GetTickCount64() - t0) / 1000.f;
        boot.set(0.97f, i18n::text(L"Applying your settings"));
        const int requestedCpu = coresFor(s);
        const int presentCpu = cpuListCount(adb(L"shell cat /sys/devices/system/cpu/present", 3000));
        observeCpuCapability(requestedCpu, presentCpu);
        rememberRunning(s);
        applyLive(s);
        if (cancelled())
            return;
        if (!resume)
            writeStr(L"state", L"lastboot", std::to_wstring(static_cast<int>(took + 0.5f)));
        state = State::Running;
        boot.set(1.f, L"Ready");
        boot.done = true;
        wchar_t buf[128];
        swprintf(buf, std::size(buf), i18n::text(L"Ready in %.1f s"), took);
        if (presentCpu > 0 && presentCpu < requestedCpu)
            report(i18n::text(L"Android limited this phone to ") + std::to_wstring(presentCpu) +
                       i18n::text(L" CPU cores. Unsupported choices are now disabled."),
                   false);
        else
            report(buf, false);
        return;
    }
    fail(i18n::text(L"The phone couldn't start."));
}

namespace {

void finishStop(bool cleanSave) {
    instances::stopped();
    // The saved snapshot is only trusted when we know exactly which settings it was made with.
    if (cleanSave && g_runningKnown && running.quickBoot && logSavedSnapshot())
        trustSnapshot(signature(running));
    else
        distrustSnapshot();
    forgetRunning();
    if (g_emuProcess) {
        CloseHandle(g_emuProcess);
        g_emuProcess = nullptr;
    }
    g_launchedPid = 0;
    g_recording = false;
    DeleteFileW((dataFolder() + L"\\recording.ini").c_str());
    state = State::Stopped;
}

} // namespace

void stop(const Report &report) {
    state = State::Stopping;
    bool saving = running.quickBoot;
    report(saving ? i18n::text(L"Saving your phone so it starts instantly next time…") : i18n::text(L"Closing…"), false);
    Found f = scan(true);
    // Flush guest writes before the emulator closes its writable disk overlays.
    // This also makes a subsequent cold backup include recently saved app data.
    adb(L"shell sync", 8000);
    adb(L"emu kill", 8000);
    ULONGLONG t0 = GetTickCount64(), phoneGone = 0;
    bool asked = false;
    for (;;) {
        Sleep(250);
        ULONGLONG now = GetTickCount64();
        int el = static_cast<int>((now - t0) / 1000);
        if (!scan(true).qemu) {
            // The phone has saved and closed; give its wrapper a moment to exit, then close it.
            if (!phoneGone)
                phoneGone = now;
            if (!wrapperAlive() || now - phoneGone > 1500)
                break;
            continue;
        }
        if (el >= 20 && !asked) {
            // adb couldn't reach the emulator: ask its window to close, which also saves.
            asked = true;
            EnumWindows(
                [](HWND w, LPARAM p) -> BOOL {
                    DWORD owner = 0;
                    GetWindowThreadProcessId(w, &owner);
                    if (owner == static_cast<DWORD>(p) && IsWindowVisible(w))
                        PostMessageW(w, WM_CLOSE, 0, 0);
                    return TRUE;
                },
                static_cast<LPARAM>(f.qemu));
        }
        if (el >= 150) {
            report(i18n::text(L"The emulator isn't closing. Close its window, or restart the PC if it stays stuck."), true);
            state = State::Running;
            return;
        }
        if (el % 2 == 0 && el > 2 && saving)
            report(i18n::text(L"Saving your phone… ") + std::to_wstring(el) + L" s", false);
    }
    closeWrapper();
    finishStop(true);
    report(L"", false);
}

void onExitedOutside() {
    // Remember exactly which wrapper belonged to the phone that closed, so the delayed cleanup can
    // never hit a phone started again in the meantime.
    DWORD wpid = g_emuProcess ? GetProcessId(g_emuProcess) : g_wrapperPid;
    uint64_t born = g_emuProcess ? creationTime(wpid) : g_wrapperBorn;
    finishStop(true);
    g_wrapperPid = 0;
    g_wrapperBorn = 0;
    if (wpid && born) {
        std::thread([wpid, born] {
            Sleep(1500);
            terminatePid(wpid, L"emulator.exe", born);
        }).detach();
    }
}

// ------------------------------------------------------------------------------------------
// Tools

std::wstring capturesFolder() {
    wchar_t pics[MAX_PATH] = L"";
    SHGetFolderPathW(nullptr, CSIDL_MYPICTURES, nullptr, 0, pics);
    std::wstring folder = std::wstring(pics) + L"\\Velox";
    if (!instances::id().empty())
        folder += L"\\" + instances::id();
    createDirs(folder);
    return folder;
}

namespace {

std::wstring stamp(const wchar_t *prefix, const wchar_t *ext) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[96];
    swprintf(name, std::size(name), L"%ls_%04d%02d%02d_%02d%02d%02d_%03d.%ls", prefix, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond, t.wMilliseconds, ext);
    return name;
}

// Writes a resource built into the exe to a file.
bool extractResource(int id, const std::wstring &path) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL data = res ? LoadResource(nullptr, res) : nullptr;
    if (!data)
        return false;
    std::ofstream o(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    o.write(static_cast<const char *>(LockResource(data)), SizeofResource(nullptr, res));
    return static_cast<bool>(o);
}

// The helper app (see gaming-phone/helper) that sets wallpapers; installed the first time.
bool ensureHelper() {
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(201), RT_RCDATA);
    HGLOBAL loaded = resource ? LoadResource(nullptr, resource) : nullptr;
    if (!loaded)
        return false;
    std::string expected = net::hashBytes(LockResource(loaded), SizeofResource(nullptr, resource), net::Hash::Sha256);
    auto verified = [&] {
        std::string path = adb(L"shell pm path app.velox.helper", 5000);
        if (path.rfind("package:/", 0) != 0)
            return false;
        path = path.substr(8);
        if (path.find_first_not_of("/ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._~+=-") != std::string::npos)
            return false;
        std::string hash = adb(L"shell /system/bin/sha256sum " + widen(path), 10000);
        return expected.size() == 64 && hash.size() > 64 && hash.substr(0, 64) == expected && hash[64] == ' ';
    };
    if (verified())
        return true;
    std::wstring apk = dataFolder() + L"\\VeloxHelper.apk";
    if (!extractResource(201, apk))
        return false;
    std::string out;
    DWORD code = run(adbExe() + L" -s " + serial() + L" install -r " + q(apk), &out, 120000);
    DeleteFileW(apk.c_str());
    return code == 0 && verified();
}

// Quotes a path for Android's shell.
std::wstring shQuote(const std::wstring &s) {
    std::wstring r = L"'";
    for (wchar_t c : s) {
        if (c == L'\'')
            r += L"'\\''";
        else if (c == L'"')
            r += L"\\\"";
        else
            r += c;
    }
    return r + L"'";
}

std::vector<std::string> fields(const std::string &text, char separator) {
    std::vector<std::string> result;
    size_t begin = 0;
    for (;;) {
        size_t end = text.find(separator, begin);
        result.push_back(text.substr(begin, end == std::string::npos ? end : end - begin));
        if (end == std::string::npos)
            return result;
        begin = end + 1;
    }
}

bool androidPackage(const std::wstring &name) {
    static const std::wregex pattern(L"[A-Za-z][A-Za-z0-9_]*(\\.[A-Za-z][A-Za-z0-9_]*)+");
    return std::regex_match(name, pattern);
}

bool sendExpansion(const std::wstring &path, std::wstring package, const Report &report) {
    std::wstring name = fileName(path);
    static const std::wregex pattern(L"(?:main|patch)\\.[0-9]+\\.([A-Za-z][A-Za-z0-9_.]*)\\.obb", std::regex::icase);
    std::wsmatch match;
    if (std::regex_match(name, match, pattern)) {
        if (package.empty())
            package = match[1].str();
        else if (package != match[1].str()) {
            report(i18n::text(L"This OBB file belongs to a different app than the bundle."), true);
            return false;
        }
    }
    if (!androidPackage(package)) {
        report(i18n::text(L"OBB files need a name like main.123.com.example.game.obb, or must come with their app bundle."), true);
        return false;
    }
    std::wstring directory = L"/sdcard/Android/obb/" + package;
    report(L"Copying " + name + L" to the game's expansion folder…", false);
    adb(L"shell mkdir -p " + directory, 10000);
    std::string output;
    DWORD exitCode = run(adbExe() + L" -s " + serial() + L" push " + q(path) + L" " + q(directory + L"/" + name), &output, 600000);
    if (exitCode != 0) {
        report(i18n::text(L"Couldn't copy ") + name + L" to Android/obb/" + package +
                   i18n::text(L". Check the phone's free space and storage access."),
               true);
        return false;
    }
    report(L"Copied " + name + L" to Android/obb/" + package, false);
    return true;
}

struct ImportFolder {
    std::filesystem::path parent, directory;
    ImportFolder() {
        parent = std::filesystem::path(dataFolder()) / L"imports";
        std::filesystem::create_directories(parent);
        GUID id{};
        if (FAILED(CoCreateGuid(&id)))
            return;
        wchar_t text[40];
        StringFromGUID2(id, text, 40);
        directory = parent / text;
        std::filesystem::create_directory(directory);
    }
    ~ImportFolder() {
        std::error_code error;
        if (!directory.empty() &&
            std::filesystem::weakly_canonical(directory, error).parent_path() == std::filesystem::weakly_canonical(parent, error))
            std::filesystem::remove_all(directory, error);
    }
};

bool installBundle(const std::wstring &path, const Report &report) {
    try {
        ImportFolder temporary;
        if (temporary.directory.empty())
            return false;
        std::wstring script = (temporary.directory / L"import.ps1").wstring();
        if (!extractResource(204, script)) {
            report(i18n::text(L"The package importer is missing from this build."), true);
            return false;
        }
        report(L"Opening " + fileName(path) + L"…", false);
        std::string output;
        std::wstring powershell = envVar(L"SystemRoot") + L"\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
        DWORD code = run(q(powershell) + L" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " + q(script) + L" -Archive " +
                             q(path) + L" -Destination " + q(temporary.directory.wstring()),
                         &output, 600000);
        if (code != 0 || output.find("velox-import-ok") == std::string::npos) {
            report(i18n::text(L"Couldn't open this app bundle. ") + widen(trim(output)), true);
            return false;
        }
        struct Apk {
            std::wstring file;
            std::string package, split;
            int minSdk;
            std::vector<std::string> abis;
        };
        std::vector<Apk> apks;
        std::vector<std::wstring> obbs;
        std::ifstream metadata(temporary.directory / L"packages.tsv");
        for (std::string line; std::getline(metadata, line);) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            auto part = fields(line, '\t');
            if (part.size() >= 6 && part[0] == "APK")
                apks.push_back(
                    {(temporary.directory / widen(part[1])).wstring(), part[2], part[3], atoi(part[4].c_str()), fields(part[5], ',')});
            else if (part.size() >= 2 && part[0] == "OBB")
                obbs.push_back((temporary.directory / widen(part[1])).wstring());
        }
        if (apks.empty()) {
            report(i18n::text(L"This bundle contains no installable APK files."), true);
            return false;
        }
        auto supported = fields(adb(L"shell getprop ro.product.cpu.abilist", 5000), ',');
        int api = atoi(adb(L"shell getprop ro.build.version.sdk", 5000).c_str());
        if (api <= 0 || supported.empty()) {
            report(i18n::text(L"Android isn't ready to install apps yet."), true);
            return false;
        }
        auto compatible = [&](const Apk &apk) {
            if (apk.minSdk > api)
                return false;
            if (apk.abis.empty() || apk.abis[0].empty())
                return true;
            for (auto &abi : apk.abis)
                if (std::find(supported.begin(), supported.end(), abi) != supported.end())
                    return true;
            return false;
        };
        int base = -1, bestPriority = -1;
        bool ambiguous = false;
        for (size_t i = 0; i < apks.size(); ++i) {
            auto &apk = apks[i];
            if (!apk.split.empty() || !compatible(apk))
                continue;
            std::wstring name = lower(fileName(apk.file));
            int priority = name == L"universal.apk" ? 3 : (name == L"base.apk" || name == L"base-master.apk") ? 2 : 1;
            if (priority > bestPriority) {
                base = static_cast<int>(i);
                bestPriority = priority;
                ambiguous = false;
            } else if (priority == bestPriority)
                ambiguous = true;
        }
        if (base < 0) {
            report(i18n::text(L"This bundle has no base APK compatible with this phone's Android version and CPU."), true);
            return false;
        }
        if (ambiguous) {
            report(i18n::text(L"This bundle contains several standalone app variants. Use a universal APK or a split APK bundle."), true);
            return false;
        }
        std::string preferredAbi;
        for (auto &abi : supported) {
            for (auto &apk : apks)
                if (std::find(apk.abis.begin(), apk.abis.end(), abi) != apk.abis.end()) {
                    preferredAbi = abi;
                    break;
                }
            if (!preferredAbi.empty())
                break;
        }
        int density = displays()[running.display].dpi;
        std::string densityText = adb(L"shell wm density", 5000);
        std::regex densityPattern(": *([0-9]+)");
        for (std::sregex_iterator it(densityText.begin(), densityText.end(), densityPattern), end; it != end; ++it)
            density = atoi((*it)[1].str().c_str());
        const std::map<std::string, int> densities{{"ldpi", 120},  {"mdpi", 160},   {"tvdpi", 213},  {"hdpi", 240},
                                                   {"xhdpi", 320}, {"xxhdpi", 480}, {"xxxhdpi", 640}};
        std::map<std::string, std::pair<size_t, double>> densitySplits;
        std::vector<size_t> chosen{static_cast<size_t>(base)};
        bool standalone = bestPriority == 3 || lower(fileName(apks[base].file)).find(L"standalone") != std::wstring::npos;
        if (!standalone)
            for (size_t i = 0; i < apks.size(); ++i) {
                auto &apk = apks[i];
                if (i == static_cast<size_t>(base) || apk.split.empty() || !compatible(apk))
                    continue;
                if (apk.package != apks[base].package) {
                    report(i18n::text(L"The bundle mixes files from different apps."), true);
                    return false;
                }
                if (!preferredAbi.empty() && !apk.abis.empty() && !apk.abis[0].empty() &&
                    std::find(apk.abis.begin(), apk.abis.end(), preferredAbi) == apk.abis.end())
                    continue;
                size_t dot = apk.split.find_last_of('.');
                std::string suffix = apk.split.substr(dot == std::string::npos ? 0 : dot + 1);
                auto dpi = densities.find(suffix);
                if (dpi != densities.end()) {
                    std::string group = apk.split.substr(0, dot == std::string::npos ? 0 : dot + 1);
                    double distance = std::abs(std::log(static_cast<double>(dpi->second) / std::max(1, density)));
                    auto old = densitySplits.find(group);
                    if (old == densitySplits.end() || distance < old->second.second)
                        densitySplits[group] = {i, distance};
                } else
                    chosen.push_back(i);
            }
        for (auto &entry : densitySplits)
            chosen.push_back(entry.second.first);
        report(L"Installing " + fileName(path) + L" (" + std::to_wstring(chosen.size()) + L" APK files)…", false);
        std::string sessionOutput = adb(L"shell pm install-create -r --user current", 15000);
        size_t open = sessionOutput.find('['), close = sessionOutput.find(']', open);
        std::string session =
            open != std::string::npos && close != std::string::npos ? sessionOutput.substr(open + 1, close - open - 1) : "";
        if (session.empty() || session.find_first_not_of("0123456789") != std::string::npos) {
            report(i18n::text(L"Android couldn't start the installation: ") + widen(sessionOutput), true);
            return false;
        }
        std::wstring remote = L"/data/local/tmp/velox-install-" + widen(session);
        adb(L"shell mkdir -p " + remote);
        auto packageCommand = [&](const std::wstring &command, DWORD timeout) {
            DWORD status = run(adbExe() + L" -s " + serial() + L" shell " + command, &output, timeout);
            if (status != 0)
                return false;
            std::istringstream lines(output);
            for (std::string line; std::getline(lines, line);) {
                line = trim(line);
                if (line == "Success" || line.rfind("Success:", 0) == 0)
                    return true;
            }
            return false;
        };
        bool installed = false;
        for (size_t item = 0; item < chosen.size(); ++item) {
            auto &apk = apks[chosen[item]];
            std::wstring name = std::to_wstring(item) + L".apk", target = remote + L"/" + name;
            code = run(adbExe() + L" -s " + serial() + L" push " + q(apk.file) + L" " + q(target), &output, 600000);
            if (code != 0)
                break;
            if (!packageCommand(L"pm install-write -S " + std::to_wstring(std::filesystem::file_size(apk.file)) + L" " + widen(session) +
                                    L" " + name + L" " + target,
                                120000))
                break;
            if (item + 1 == chosen.size())
                installed = packageCommand(L"pm install-commit " + widen(session), 120000);
        }
        if (!installed)
            adb(L"shell pm install-abandon " + widen(session));
        adb(L"shell rm -rf " + remote);
        if (!installed) {
            report(i18n::text(L"Couldn't install ") + fileName(path) + L": " + widen(trim(output)), true);
            return false;
        }
        for (auto &obb : obbs)
            if (!sendExpansion(obb, widen(apks[base].package), report)) {
                report(i18n::text(L"The app installed, but its expansion data couldn't be copied."), true);
                return false;
            }
        report(L"Installed " + fileName(path) + (obbs.empty() ? L"" : L" with its game data"), false);
        return true;
    } catch (const std::exception &error) {
        report(i18n::text(L"Couldn't import the app bundle: ") + widen(error.what()), true);
        return false;
    }
}

} // namespace

bool appPackageFile(const std::wstring &path) {
    std::wstring extension = lower(std::filesystem::path(path).extension().wstring());
    return extension == L".apk" || extension == L".apks" || extension == L".xapk" || extension == L".apkm" || extension == L".aab";
}

bool installApk(const std::wstring &path, const Report &report) {
    std::wstring extension = lower(std::filesystem::path(path).extension().wstring());
    if (extension == L".aab") {
        report(i18n::text(L"AAB is a developer bundle. Export an APK or APKS package before installing it."), true);
        return false;
    }
    if (extension != L".apk")
        return installBundle(path, report);
    std::wstring name = fileName(path);
    report(L"Installing " + name + L"…", false);
    std::string out;
    DWORD installed = run(adbExe() + L" -s " + serial() + L" install -r " + q(path), &out, 600000);
    std::string result = trim(out);
    bool ok = installed == 0 && (result == "Success" || result.ends_with("\nSuccess"));
    if (ok) {
        report(L"Installed " + name, false);
        return true;
    }
    size_t at = out.find("Failure [");
    std::wstring why = at != std::string::npos ? widen(out.substr(at + 9, out.find(']', at) - at - 9)) : L"";
    report(i18n::text(L"Couldn't install ") + name + (why.empty() ? L"" : L" (" + why + L")"), true);
    return false;
}

bool sendFile(const std::wstring &path, const Report &report) {
    if (lower(std::filesystem::path(path).extension().wstring()) == L".obb")
        return sendExpansion(path, L"", report);
    std::wstring name = fileName(path);
    report(L"Sending " + name + L"…", false);
    std::wstring remote = L"/sdcard/Download/" + name;
    std::string out;
    DWORD sent = run(adbExe() + L" -s " + serial() + L" push " + q(path) + L" " + q(remote), &out, 600000);
    if (sent != 0) {
        report(i18n::text(L"Couldn't send ") + name, true);
        return false;
    }
    adb(L"shell \"am broadcast -a android.intent.action.MEDIA_SCANNER_SCAN_FILE -d " + shQuote(L"file://" + remote) + L" >/dev/null\"");
    report(L"Sent " + name + L" to the phone's Download folder", false);
    return true;
}

bool screenshot(const Report &report) {
    std::wstring file = capturesFolder() + L"\\" + stamp(i18n::text(L"Screenshot"), L"png");
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE f = CreateFileW(file.c_str(), GENERIC_WRITE, 0, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        report(i18n::text(L"Couldn't save the screenshot."), true);
        return false;
    }
    DWORD code = run(adbExe() + L" -s " + serial() + L" exec-out screencap -p", nullptr, 20000, f);
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    CloseHandle(f);
    if (code != 0 || size.QuadPart < 100) {
        DeleteFileW(file.c_str());
        report(i18n::text(L"Couldn't take the screenshot."), true);
        return false;
    }
    report(L"Screenshot saved to Pictures\\Velox", false);
    return true;
}

bool recording() {
    return g_recording;
}

void syncRecordingState() {
    std::unique_lock<std::mutex> guard(g_recordLock, std::try_to_lock);
    if (!guard.owns_lock())
        return;
    DWORD pid = state == State::Running ? emulatorPid() : 0;
    std::wstring marker = dataFolder() + L"\\recording.ini";
    if (pid != g_recordPid) {
        g_recordPid = pid;
        g_recording = false;
        g_recordFile.clear();
        wchar_t born[32], started[32], path[MAX_PATH];
        GetPrivateProfileStringW(L"recording", L"born", L"0", born, 32, marker.c_str());
        GetPrivateProfileStringW(L"recording", L"started", L"0", started, 32, marker.c_str());
        GetPrivateProfileStringW(L"recording", L"file", L"", path, MAX_PATH, marker.c_str());
        g_recordStarted = _wcstoui64(started, nullptr, 10);
        if (pid && GetPrivateProfileIntW(L"recording", L"pid", 0, marker.c_str()) == pid &&
            _wcstoui64(born, nullptr, 10) == creationTime(pid) && *path) {
            g_recordFile = path;
            g_recording = true;
        }
    }
    if (g_recording && (GetTickCount64() < g_recordStarted || GetTickCount64() - g_recordStarted > 3605000)) {
        g_recording = false;
        DeleteFileW(marker.c_str());
    }
}

bool toggleRecording(const Report &report) {
    syncRecordingState();
    if (!g_recording) {
        DWORD pid = emulatorPid();
        if (!pid)
            return false;
        std::wstring file = capturesFolder() + L"\\" + stamp(L"Recording", L"webm");
        std::wostringstream consoleFile;
        consoleFile << std::quoted(file);
        // The emulator records on the PC side, so the phone's own speed isn't affected.
        std::string out = adb(L"emu screenrecord start --bit-rate 12M --time-limit 3600 " + q(consoleFile.str()), 8000);
        if (out.find("OK") == std::string::npos || out.find("KO:") != std::string::npos) {
            report(i18n::text(L"Couldn't start recording. ") + widen(out), true);
            return false;
        }
        if (state != State::Running || emulatorPid() != pid)
            return false;
        uint64_t started = GetTickCount64();
        {
            std::lock_guard<std::mutex> guard(g_recordLock);
            g_recordFile = file;
            g_recordPid = pid;
            g_recordStarted = started;
            g_recording = true;
        }
        std::wstring marker = dataFolder() + L"\\recording.ini";
        WritePrivateProfileStringW(L"recording", L"pid", std::to_wstring(pid).c_str(), marker.c_str());
        WritePrivateProfileStringW(L"recording", L"born", std::to_wstring(creationTime(pid)).c_str(), marker.c_str());
        WritePrivateProfileStringW(L"recording", L"started", std::to_wstring(started).c_str(), marker.c_str());
        WritePrivateProfileStringW(L"recording", L"file", file.c_str(), marker.c_str());
        report(i18n::text(L"Recording the screen (up to 1 hour). Press Stop recording when done."), false);
        return true;
    }
    std::wstring file;
    {
        std::lock_guard<std::mutex> guard(g_recordLock);
        file = g_recordFile;
    }
    std::string stopped = adb(L"emu screenrecord stop", 15000);
    if (stopped.find("OK") == std::string::npos || stopped.find("KO:") != std::string::npos) {
        report(i18n::text(L"Couldn't stop recording yet. Try again or use Android Emulator controls."), true);
        return false;
    }
    g_recording = false;
    DeleteFileW((dataFolder() + L"\\recording.ini").c_str());
    for (int i = 0; i < 40 && !exists(file); i++)
        Sleep(250);
    report(exists(file) ? L"Recording saved to Pictures\\Velox" : i18n::text(L"The recording couldn't be saved."), !exists(file));
    return exists(file);
}

void rotate() {
    std::lock_guard<std::mutex> l(g_rotationLock);
    adb(L"emu rotate", 5000);
}

void setAutoRotation(bool enabled) {
    if (g_autoRotation.exchange(enabled) != enabled)
        ++g_rotationGeneration;
}

void syncOrientation() {
    std::lock_guard<std::mutex> l(g_rotationLock);
    if (!g_autoRotation || state != State::Running)
        return;
    DWORD pid = emulatorPid();
    if (!pid)
        return;
    static DWORD watchedPid = 0;
    static unsigned watchedGeneration = 0;
    static int followed = -1;
    unsigned generation = g_rotationGeneration;
    if (pid != watchedPid || generation != watchedGeneration) {
        watchedPid = pid;
        watchedGeneration = generation;
        followed = -1;
    }
    int target =
        orientation::displayRotation(adb(L"shell \"dumpsys window displays | grep -E 'Display: mDisplayId=|mRotation=[0-3]'\"", 3000));
    if (target < 0)
        return;
    if (target == followed)
        return; // leave a manual rotation alone until Android changes orientation
    int current = orientation::deviceRotation(adb(L"emu sensor get acceleration", 2000));
    if (current < 0)
        return;
    int steps = orientation::clockwiseSteps(current, target);
    if (steps > 0) {
        if (!g_autoRotation || generation != g_rotationGeneration || state != State::Running || emulatorPid() != pid)
            return;
        // One console transaction queues the final angle before the next UI paint,
        // avoiding separate adb launches and visible pauses at intermediate angles.
        std::wstring commands = L"rotate";
        for (int i = 1; i < steps; ++i)
            commands += L"\nrotate";
        std::string result = adb(L"emu " + q(commands), 2000);
        if (result.find("OK") == std::string::npos || result.find("KO:") != std::string::npos)
            return;
    }
    // A failed or unsupported rotation is retried later, never recorded as successful.
    if (steps == 0 || orientation::deviceRotation(adb(L"emu sensor get acceleration", 2000)) == target)
        followed = target;
}

void setGestureNavigation(bool enabled, bool reverseButtons) {
    g_navigationMode = enabled ? 2 : reverseButtons ? 1 : 0;
}

bool navigationNeedsRestart() {
    return g_navigationRestart;
}

NavigationResult syncNavigation() {
    if (state != State::Running)
        return NavigationResult::Unchanged;
    DWORD pid = emulatorPid();
    if (!pid)
        return NavigationResult::Unchanged;
    static DWORD appliedPid = 0;
    static int appliedMode = -1;
    static ULONGLONG retryAt = 0;
    int mode = g_navigationMode;
    if ((pid == appliedPid && mode == appliedMode) || GetTickCount64() < retryAt)
        return NavigationResult::Unchanged;
    retryAt = GetTickCount64() + 1500;
    const wchar_t *orderOverlay = L"app.velox.navorder";
    std::string overlays = adb(L"shell cmd overlay list com.google.android.apps.nexuslauncher", 3000);
    bool orderExists = overlays.find("app.velox.navorder") != std::string::npos;
    bool orderEnabled = overlays.find("[x] app.velox.navorder") != std::string::npos;
    bool pending = g_navigationRestart;
    if (mode != 1 || orderExists)
        g_navigationRestart = false;
    if (mode == 1 && !orderExists) {
        if (pending)
            return NavigationResult::RestartRequired;
        if (adb(L"shell su -c id", 5000).find("uid=0(") == std::string::npos)
            return NavigationResult::NeedsRoot;
        // Pixel Launcher owns Android 15's unified phone navbar. Only its button
        // container is reversed; the home screen and contextual controls retain LTR.
        HRSRC expected = FindResourceW(nullptr, MAKEINTRESOURCEW(205), RT_RCDATA);
        HGLOBAL loaded = expected ? LoadResource(nullptr, expected) : nullptr;
        if (!loaded)
            return NavigationResult::Unsupported;
        std::string apkPath = adb(L"shell pm path com.google.android.apps.nexuslauncher", 3000);
        if (apkPath.rfind("package:/", 0) != 0 || apkPath.find_first_of("\r\n\"' ") != std::string::npos)
            return NavigationResult::Unsupported;
        std::string actual = adb(L"exec-out unzip -p " + widen(apkPath.substr(8)) + L" res/layout/taskbar.xml", 5000);
        std::string original(static_cast<const char *>(LockResource(loaded)), SizeofResource(nullptr, expected));
        if (actual != original)
            return NavigationResult::Unsupported;
        std::wstring localApk = dataFolder() + L"\\VeloxNavOrder.apk", localProp = dataFolder() + L"\\velox-navigation.prop",
                     localGuard = dataFolder() + L"\\velox-navigation.sh";
        if (!extractResource(203, localApk) || !extractResource(206, localGuard))
            return NavigationResult::Unsupported;
        {
            std::ofstream prop{std::filesystem::path(localProp), std::ios::binary | std::ios::trunc};
            prop << "id=velox-navigation\nname=Velox native navigation\nversion=1.0\nversionCode=1\nauthor=Velox\n"
                    "description=Optional native button order for Velox Android 15.\n";
        }
        std::string ignored;
        DWORD pushed =
            run(adbExe() + L" -s " + serial() + L" push " + q(localApk) + L" /data/local/tmp/VeloxNavOrder.apk", &ignored, 15000);
        if (pushed == 0)
            pushed =
                run(adbExe() + L" -s " + serial() + L" push " + q(localProp) + L" /data/local/tmp/velox-navigation.prop", &ignored, 15000);
        if (pushed == 0)
            pushed =
                run(adbExe() + L" -s " + serial() + L" push " + q(localGuard) + L" /data/local/tmp/velox-navigation.sh", &ignored, 15000);
        DeleteFileW(localApk.c_str());
        DeleteFileW(localProp.c_str());
        DeleteFileW(localGuard.c_str());
        if (pushed != 0)
            return NavigationResult::Unchanged;
        std::string prepared =
            adb(L"shell \"su -c 'mkdir -p /data/adb/modules/velox-navigation/system/product/overlay && "
                L"cp /data/local/tmp/VeloxNavOrder.apk /data/adb/modules/velox-navigation/VeloxNavOrder.apk && "
                L"cp /data/local/tmp/VeloxNavOrder.apk /data/adb/modules/velox-navigation/system/product/overlay/VeloxNavOrder.apk && "
                L"cp /data/local/tmp/velox-navigation.prop /data/adb/modules/velox-navigation/module.prop && "
                L"cp /data/local/tmp/velox-navigation.sh /data/adb/modules/velox-navigation/post-fs-data.sh && "
                L"chmod 755 /data/adb/modules/velox-navigation/post-fs-data.sh && "
                L"chmod 644 /data/adb/modules/velox-navigation/module.prop "
                L"/data/adb/modules/velox-navigation/system/product/overlay/VeloxNavOrder.apk && "
                L"chcon u:object_r:system_file:s0 /data/adb/modules/velox-navigation/system/product/overlay/VeloxNavOrder.apk && echo "
                L"velox-nav-prepared'\"",
                5000);
        adb(L"shell rm -f /data/local/tmp/VeloxNavOrder.apk /data/local/tmp/velox-navigation.prop /data/local/tmp/velox-navigation.sh");
        if (prepared.find("velox-nav-prepared") == std::string::npos)
            return NavigationResult::NeedsRoot;
        g_navigationRestart = true;
        return NavigationResult::RestartRequired;
    }
    bool refreshOrder = orderExists && orderEnabled != (mode == 1);
    // Check before changing the overlay: without a launcher refresh Android can
    // report the new preference while its visible buttons still use the old order.
    if (refreshOrder && adb(L"shell su -c id", 5000).find("uid=0(") == std::string::npos)
        return NavigationResult::NeedsRoot;
    const wchar_t *package =
        mode == 2 ? L"com.android.internal.systemui.navbar.gestural" : L"com.android.internal.systemui.navbar.threebutton";
    std::wstring command = std::wstring(L"shell \"cmd overlay enable-exclusive --user current --category ") + package;
    if (orderExists)
        command += std::wstring(L" && cmd overlay ") + (mode == 1 ? L"enable" : L"disable") + L" --user current " + orderOverlay;
    std::string result = adb(command + L" && echo velox-nav-ok\"", 3000);
    if (result.find("velox-nav-ok") != std::string::npos) {
        if (refreshOrder) {
            bool home = adb(L"shell \"dumpsys window | grep -m1 mCurrentFocus\"", 3000).find("com.google.android.apps.nexuslauncher/") !=
                        std::string::npos;
            std::string refreshed = adb(L"shell \"su -c 'killall com.google.android.apps.nexuslauncher; echo velox-nav-refreshed'\"", 5000);
            if (refreshed.find("velox-nav-refreshed") == std::string::npos)
                return NavigationResult::NeedsRoot;
            if (home)
                adb(L"shell input keyevent KEYCODE_HOME", 3000);
        }
        appliedPid = pid;
        appliedMode = mode;
        retryAt = 0;
        return NavigationResult::Applied;
    }
    return NavigationResult::Unchanged;
}

// Frames the phone actually put on screen, from SurfaceFlinger's own statistics (which adb may
// read without root). An idle screen shows few frames; a game shows its real frame rate.
int sampleFps() {
    static ULONGLONG lastTime = 0;
    static int lastSession = -1;
    ULONGLONG now = GetTickCount64();
    if (lastSession != g_session || now - lastTime > 10000) {
        adb(L"shell dumpsys SurfaceFlinger --timestats -enable -clear", 4000);
        lastSession = g_session;
        lastTime = now;
        return -1;
    }
    std::string out = adb(L"shell \"dumpsys SurfaceFlinger --timestats -dump -clear | grep -m1 totalFrames\"", 4000);
    size_t at = out.find("totalFrames = ");
    if (at == std::string::npos)
        return -1;
    long frames = strtol(out.c_str() + at + 14, nullptr, 10);
    double secs = (now - lastTime) / 1000.0;
    lastTime = now;
    return secs > 0.2 ? static_cast<int>(std::lround(frames / secs)) : -1;
}

namespace {

// Sends a finished picture to the phone and sets it on the home and lock screen.
bool pushWallpaper(gp::Bitmap &bmp, const std::wstring &name, const Report &report) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring local = std::wstring(tmp) + L"Velox-wallpaper-" + std::to_wstring(GetCurrentProcessId()) + L".png";
    if (!art::savePng(bmp, local)) {
        report(i18n::text(L"Couldn't create the wallpaper."), true);
        return false;
    }
    if (!ensureHelper()) {
        DeleteFileW(local.c_str());
        report(i18n::text(L"Couldn't install the small helper app that sets wallpapers."), true);
        return false;
    }
    const std::wstring tmpRemote = L"/data/local/tmp/velox_wallpaper.png";
    std::string out;
    run(adbExe() + L" -s " + serial() + L" push " + q(local) + L" " + tmpRemote, &out, 60000);
    DeleteFileW(local.c_str());
    if (out.find("pushed") == std::string::npos) {
        report(i18n::text(L"Couldn't copy the wallpaper to the phone."), true);
        return false;
    }
    adb(L"shell \"content write --uri content://app.velox.helper/wallpaper < " + tmpRemote + L"\"", 30000);
    std::string res = adb(L"shell content call --uri content://app.velox.helper --method set", 30000);
    adb(L"shell rm -f " + tmpRemote);
    if (res.find("ok=true") == std::string::npos) {
        report(i18n::text(L"The phone didn't accept the wallpaper."), true);
        return false;
    }
    report(name + L" wallpaper set", false);
    return true;
}

} // namespace

bool applyWallpaper(int index, int brand, const Report &report) {
    const auto &wp = art::wallpapers();
    if (index < 0 || index >= static_cast<int>(wp.size()))
        return false;
    const Display &d = displays()[running.display];
    report(L"Drawing the " + std::wstring(wp[index].name) + L" wallpaper at " + d.detail + L"…", false);
    auto bmp = art::renderWallpaper(index, d.w, d.h);
    {
        gp::Graphics g(bmp.get());
        art::drawBranding(g, d.w, d.h, brand);
    }
    return pushWallpaper(*bmp, wp[index].name, report);
}

bool applyWallpaperFile(const std::wstring &image, int brand, const Report &report) {
    gp::Bitmap src(image.c_str());
    if (src.GetLastStatus() != gp::Ok || src.GetWidth() == 0) {
        report(i18n::text(L"That picture couldn't be opened."), true);
        return false;
    }
    const Display &d = displays()[running.display];
    report(L"Fitting " + fileName(image) + L" to the phone screen…", false);
    auto bmp = art::fitImage(src, d.w, d.h, brand);
    std::wstring name = fileName(image);
    name = name.substr(0, name.find_last_of(L'.'));
    return pushWallpaper(*bmp, L"The " + name, report);
}

std::wstring wallpaperFolder() {
    std::wstring dir = dataFolder() + L"\\wallpapers";
    createDirs(dir);
    return dir;
}

namespace {

bool isPicture(const std::wstring &name) {
    std::wstring l = lower(name);
    for (const wchar_t *ext : {L".png", L".jpg", L".jpeg", L".bmp", L".gif"})
        if (l.size() > wcslen(ext) && l.compare(l.size() - wcslen(ext), wcslen(ext), ext) == 0)
            return true;
    return false;
}

} // namespace

std::vector<std::wstring> wallpaperFiles() {
    std::vector<std::wstring> files;
    std::vector<std::wstring> dirs = {exeFolder() + L"wallpapers", wallpaperFolder()};
    if (lower(dirs[0]) == lower(dirs[1]))
        dirs.pop_back();
    for (auto &dir : dirs) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        std::vector<std::wstring> here;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && isPicture(fd.cFileName))
                here.push_back(dir + L"\\" + fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        std::sort(here.begin(), here.end());
        files.insert(files.end(), here.begin(), here.end());
    }
    return files;
}

int addWallpapers(const std::vector<std::wstring> &files) {
    std::wstring dir = wallpaperFolder();
    int added = 0;
    for (auto &f : files) {
        if (!isPicture(f))
            continue;
        std::wstring name = fileName(f), target = dir + L"\\" + name;
        std::wstring stem = name.substr(0, name.find_last_of(L'.')), ext = name.substr(name.find_last_of(L'.'));
        for (int i = 2; exists(target) && i < 1000; i++)
            target = dir + L"\\" + stem + L" (" + std::to_wstring(i) + L")" + ext;
        if (CopyFileW(f.c_str(), target.c_str(), TRUE))
            added++;
    }
    return added;
}

// Patches the phone's boot ramdisk with Magisk inside the running (stock) phone, using rootAVD.
// The SDK's own image is never changed; the patched copy is used only when Root is turned on.
bool setupRoot(const Report &report) {
    if (state != State::Running || running.root) {
        report(i18n::text(L"Start the phone without root first."), true);
        return false;
    }
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(200), RT_RCDATA);
    HGLOBAL data = res ? LoadResource(nullptr, res) : nullptr;
    if (!data) {
        report(i18n::text(L"The root tools are missing from this build."), true);
        return false;
    }
    std::wstring work = dataFolder() + L"\\root";
    createDirs(work);
    std::wstring script = work + L"\\rootAVD.sh", apk = work + L"\\Magisk-v31.1.apk";
    {
        std::ofstream o(std::filesystem::path(script), std::ios::binary | std::ios::trunc);
        o.write(static_cast<const char *>(LockResource(data)), SizeofResource(nullptr, res));
    }
    report(i18n::text(L"Downloading Magisk v31.1 from GitHub…"), false);
    std::wstring err;
    if (!net::download(MAGISK_URL, apk, MAGISK_SIZE, MAGISK_SHA256, net::Hash::Sha256, nullptr, nullptr, err)) {
        report(i18n::text(L"Couldn't download Magisk: ") + err, true);
        return false;
    }
    std::wstring stock = g_paths.sdk + L"\\system-images\\android-35\\google_apis_playstore\\x86_64\\ramdisk.img";
    if (!exists(stock)) {
        report(i18n::text(L"The phone's ramdisk wasn't found."), true);
        return false;
    }
    report(i18n::text(L"Patching the phone with Magisk…"), false);
    const std::wstring W = L"/data/data/com.android.shell/Magisk";
    adb(L"shell rm -rf " + W);
    adb(L"shell mkdir " + W);
    std::string out;
    bool ok = true;
    for (auto [from, to] :
         {std::pair{apk, W + L"/Magisk.zip"}, std::pair{stock, W + L"/ramdisk.img"}, std::pair{script, W + L"/rootAVD.sh"}}) {
        out.clear();
        run(adbExe() + L" -s " + serial() + L" push " + q(from) + L" " + to, &out, 120000);
        ok = ok && out.find("pushed") != std::string::npos;
    }
    if (!ok) {
        report(i18n::text(L"Couldn't copy the root tools to the phone."), true);
        return false;
    }
    out = adb(L"shell \"cd " + W + L" && MAGISKVERCHOOSEN=false sh " + W +
                  L"/rootAVD.sh system-images/android-35/google_apis_playstore/x86_64/ramdisk.img 2>&1\"",
              300000);
    std::wstring target = exeFolder() + L"root";
    createDirs(target);
    std::wstring tmpImg = target + L"\\ramdisk-root.img.new";
    out.clear();
    run(adbExe() + L" -s " + serial() + L" pull " + W + L"/ramdiskpatched4AVD.img " + q(tmpImg), &out, 120000);
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(tmpImg.c_str(), GetFileExInfoStandard, &fa) || fa.nFileSizeLow < 100000) {
        // The exe's folder may not be writable; keep the image in AppData instead.
        target = dataFolder() + L"\\root";
        tmpImg = target + L"\\ramdisk-root.img.new";
        out.clear();
        run(adbExe() + L" -s " + serial() + L" pull " + W + L"/ramdiskpatched4AVD.img " + q(tmpImg), &out, 120000);
        if (!GetFileAttributesExW(tmpImg.c_str(), GetFileExInfoStandard, &fa) || fa.nFileSizeLow < 100000) {
            report(L"Patching didn't work. Details are in the phone's " + W + L" folder.", true);
            return false;
        }
    }
    std::wstring img = target + L"\\ramdisk-root.img";
    MoveFileExW(tmpImg.c_str(), img.c_str(), MOVEFILE_REPLACE_EXISTING);
    report(i18n::text(L"Installing the Magisk app…"), false);
    out.clear();
    run(adbExe() + L" -s " + serial() + L" install -r " + q(apk), &out, 120000);
    adb(L"shell rm -rf " + W);
    g_paths.rootImage = img;
    writeStr(L"launcher", L"rootimage", img);
    report(i18n::text(L"Root is ready. Turn on Root and restart the phone."), false);
    return true;
}

} // namespace phone
