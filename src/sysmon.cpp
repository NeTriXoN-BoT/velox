#include "sysmon.hpp"

#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>

#include <atomic>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

#include "phone.hpp"

namespace phone::sysmon {
namespace {

std::atomic<bool> g_active{false}, g_started{false};
std::mutex g_lock;
std::deque<Sample> g_history;

uint64_t ft(const FILETIME &f) {
    return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}

// GPU engine and video memory counters, read through PDH like Task Manager does.
struct Gpu {
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER engines = nullptr, memory = nullptr;
    bool ok = false;
    Gpu() {
        if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS)
            return;
        ok = PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &engines) == ERROR_SUCCESS;
        PdhAddEnglishCounterW(query, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &memory);
        PdhCollectQueryData(query);
    }
    ~Gpu() {
        if (query)
            PdhCloseQuery(query);
    }
    template <typename F> void each(PDH_HCOUNTER c, F &&f) {
        DWORD size = 0, count = 0;
        if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &size, &count, nullptr) != static_cast<PDH_STATUS>(PDH_MORE_DATA))
            return;
        std::vector<BYTE> buf(size);
        auto *items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W *>(buf.data());
        if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &size, &count, items) != ERROR_SUCCESS)
            return;
        for (DWORD i = 0; i < count; i++)
            if (items[i].FmtValue.CStatus == ERROR_SUCCESS)
                f(items[i].szName, items[i].FmtValue.doubleValue);
    }
    // Like Task Manager: each engine's load is summed over all processes, and a GPU's load is
    // its busiest engine. Only the graphics card shown in the gauge counts (if it's known).
    void sample(DWORD phonePid, float &total, float &phone, float &vramGB) {
        if (!ok || PdhCollectQueryData(query) != ERROR_SUCCESS)
            return;
        std::wstring adapter = host().gpuLuid; // "luid_0x..._0x...", lower case
        std::map<std::wstring, double> engine, phoneEngine;
        std::wstring pidTag = L"pid_" + std::to_wstring(phonePid) + L"_";
        each(engines, [&](const wchar_t *name, double v) {
            std::wstring n = name;
            for (auto &ch : n)
                ch = static_cast<wchar_t>(towlower(ch));
            size_t at = n.find(L"luid_");
            if (at == std::wstring::npos)
                return;
            std::wstring key = n.substr(at);
            if (!adapter.empty() && key.rfind(adapter, 0) != 0)
                return;
            engine[key] += v;
            if (phonePid && n.rfind(pidTag, 0) == 0)
                phoneEngine[key] += v;
        });
        double best = 0, phoneBest = 0;
        for (auto &[k, v] : engine)
            best = std::max(best, v);
        for (auto &[k, v] : phoneEngine)
            phoneBest = std::max(phoneBest, v);
        total = static_cast<float>(std::min(100.0, best));
        phone = phonePid ? static_cast<float>(std::min(100.0, phoneBest)) : -1;
        double bytes = 0;
        bool any = false;
        if (memory)
            each(memory, [&](const wchar_t *name, double v) {
                std::wstring n = name;
                for (auto &ch : n)
                    ch = static_cast<wchar_t>(towlower(ch));
                if (!adapter.empty() && n.find(adapter) == std::wstring::npos)
                    return;
                bytes += v;
                any = true;
            });
        vramGB = any ? static_cast<float>(bytes / 1073741824.0) : -1;
    }
};

void loop() {
    Gpu gpu;
    FILETIME idle0, kern0, user0;
    GetSystemTimes(&idle0, &kern0, &user0);
    DWORD pid = 0;
    HANDLE proc = nullptr;
    uint64_t procTime0 = 0;
    ULONGLONG wall0 = GetTickCount64();
    int threads = std::max(1, host().threads);
    for (;;) {
        Sleep(1000);
        if (!g_active)
            continue;
        Sample s;
        FILETIME idle, kern, user;
        GetSystemTimes(&idle, &kern, &user);
        uint64_t di = ft(idle) - ft(idle0), dk = ft(kern) - ft(kern0), du = ft(user) - ft(user0);
        if (dk + du > 0)
            s.cpu = 100.f * static_cast<float>(dk + du - di) / static_cast<float>(dk + du);
        idle0 = idle;
        kern0 = kern;
        user0 = user;
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        GlobalMemoryStatusEx(&ms);
        s.ramTotalGB = static_cast<float>(ms.ullTotalPhys / 1073741824.0);
        s.ramUsedGB = static_cast<float>((ms.ullTotalPhys - ms.ullAvailPhys) / 1073741824.0);
        // The phone's own share.
        DWORD now = emulatorPid();
        ULONGLONG wall = GetTickCount64();
        if (now != pid) {
            if (proc)
                CloseHandle(proc);
            proc = now ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, now) : nullptr;
            pid = now;
            procTime0 = 0;
        }
        if (proc) {
            FILETIME c, e, k, u;
            if (GetProcessTimes(proc, &c, &e, &k, &u)) {
                uint64_t t = ft(k) + ft(u);
                if (procTime0 && wall > wall0)
                    s.phoneCpu = std::min(100.f, static_cast<float>((t - procTime0) / 10000.0 / (wall - wall0) / threads * 100.0));
                procTime0 = t;
            }
            PROCESS_MEMORY_COUNTERS pmc{};
            pmc.cb = sizeof(pmc);
            if (K32GetProcessMemoryInfo(proc, &pmc, sizeof(pmc)))
                s.phoneRamGB = static_cast<float>(pmc.WorkingSetSize / 1073741824.0);
        }
        wall0 = wall;
        gpu.sample(pid, s.gpu, s.phoneGpu, s.vramUsedGB);
        std::lock_guard<std::mutex> l(g_lock);
        g_history.push_back(s);
        while (g_history.size() > 60)
            g_history.pop_front();
    }
}

std::wstring readProperty(const std::wstring &file, const std::string &key) {
    std::ifstream in{std::filesystem::path(file)};
    for (std::string line; std::getline(in, line);) {
        if (line.rfind(key + "=", 0) == 0) {
            std::string v = line.substr(key.size() + 1);
            while (!v.empty() && (v.back() == '\r' || v.back() == ' '))
                v.pop_back();
            return widen(v);
        }
    }
    return {};
}

std::wstring gb(double v) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%.0f GB", v);
    return buf;
}

} // namespace

void setActive(bool on) {
    g_active = on;
    if (on && !g_started.exchange(true))
        std::thread(loop).detach();
}

Sample latest() {
    std::lock_guard<std::mutex> l(g_lock);
    return g_history.empty() ? Sample{} : g_history.back();
}

std::vector<Sample> history() {
    std::lock_guard<std::mutex> l(g_lock);
    return {g_history.begin(), g_history.end()};
}

Specs specs() {
    static Specs cached;
    static bool done = false;
    if (done)
        return cached;
    const HostInfo &h = host();
    Specs s;
    s.cpu = (h.cpu.empty() ? i18n::text(L"Processor") : h.cpu) + L"  ·  " + std::to_wstring(h.cores) + L"C / " +
            std::to_wstring(h.threads) + L"T";
    s.gpu = (h.gpu.empty() ? L"Basic display adapter" : h.gpu) + (h.vramMB ? L"  ·  " + gb(h.vramMB / 1024.0) : L"");
    s.memory = std::to_wstring(h.ramGB) + L" GB";
    s.display = std::to_wstring(h.monitorW) + L" × " + std::to_wstring(h.monitorH) + L"  ·  " + std::to_wstring(h.monitorHz) + L" Hz";
    // Windows version from the registry (the build number decides 10 vs 11).
    wchar_t ver[64] = L"", build[32] = L"";
    DWORD n = sizeof(ver);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"DisplayVersion", RRF_RT_REG_SZ, nullptr, ver,
                 &n);
    n = sizeof(build);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuild", RRF_RT_REG_SZ, nullptr, build,
                 &n);
    int b = _wtoi(build);
    s.windows = std::wstring(b >= 22000 ? L"Windows 11" : L"Windows 10") + (*ver ? L" " + std::wstring(ver) : L"") + L"  ·  build " + build;
    s.virtualization = h.hypervisor ? L"Hypervisor Platform  ·  on" : L"Hypervisor Platform  ·  off";
    const Paths &p = paths();
    std::wstring api =
        readProperty(p.sdk + L"\\system-images\\android-35\\google_apis_playstore\\x86_64\\source.properties", "AndroidVersion.ApiLevel");
    s.android = api.empty() ? i18n::text(L"Not installed") : L"Android 15  ·  Google Play";
    std::wstring emu = readProperty(p.sdk + L"\\emulator\\source.properties", "Pkg.Revision");
    s.emulator = emu.empty() ? i18n::text(L"Not installed") : L"Emulator " + emu;
    if (!p.sdk.empty())
        done = true;
    cached = s;
    return s;
}

} // namespace phone::sysmon
