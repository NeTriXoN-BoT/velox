#include "setup.hpp"

#include <shellapi.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

#include "net.hpp"
#include "phone.hpp"

namespace phone::setup {

Progress progress;

namespace {

// Exactly the packages the phone was tested with, with Google's published SHA-1 checksums
// (from https://dl.google.com/android/repository/repository2-3.xml and sys-img2-3.xml).
struct Package {
    const wchar_t *name, *url;
    uint64_t size;
    const char *sha1;
    const wchar_t *into, *check;
};
const Package PACKAGES[] = {
    {i18n::text(L"Android platform tools"), L"https://dl.google.com/android/repository/platform-tools_r37.0.1-win.zip", 8044989,
     "e03e78b1d80b396f1c3358e31251cb31740e1110", L"", L"platform-tools\\adb.exe"},
    {i18n::text(L"Android emulator"), L"https://dl.google.com/android/repository/emulator-windows_x64-16740729.zip", 455344678,
     "4d7268aa7787c640ad892844e6c9a5f159e91ad3", L"", L"emulator\\emulator.exe"},
    {i18n::text(L"Android 15 with Google Play"),
     L"https://dl.google.com/android/repository/sys-img/google_apis_playstore/x86_64-35_r09.zip", 1762061559,
     "2f0054868e6aab3c098acd3decba17a82aed4176", L"system-images\\android-35\\google_apis_playstore",
     L"system-images\\android-35\\google_apis_playstore\\x86_64\\system.img"},
};

const char *CONFIG_TEMPLATE =
    "AvdId=Gaming_Phone\r\navd.ini.displayname=Velox\r\navd.ini.encoding=UTF-8\r\nPlayStore.enabled=true\r\n"
    "abi.type=x86_64\r\nimage.sysdir.1=system-images\\android-35\\google_apis_playstore\\x86_64\\\r\ntag.display=Google Play\r\n"
    "tag.id=google_apis_playstore\r\nhw.cpu.arch=x86_64\r\nhw.cpu.ncore=4\r\nhw.ramSize=4096\r\nvm.heapSize=576\r\n"
    "hw.gpu.enabled=yes\r\nhw.gpu.mode=host\r\ndisk.dataPartition.size=32G\r\nsdcard.size=2048M\r\nhw.sdCard=yes\r\n"
    "hw.device.manufacturer=Google\r\nhw.device.name=pixel_7\r\nhw.lcd.width=1080\r\nhw.lcd.height=2400\r\nhw.lcd.density=420\r\n"
    "hw.initialOrientation=portrait\r\nhw.keyboard=yes\r\nhw.mainKeys=no\r\nhw.dPad=no\r\nhw.trackBall=no\r\nhw.accelerometer=yes\r\n"
    "hw.gyroscope=yes\r\nhw.sensors.orientation=yes\r\nhw.sensors.proximity=yes\r\nhw.audioInput=yes\r\nhw.audioOutput=yes\r\n"
    "hw.battery=yes\r\nhw.gps=yes\r\nhw.camera.back=emulated\r\nhw.camera.front=emulated\r\nhw.arc=false\r\n"
    "fastboot.forceColdBoot=no\r\nfastboot.forceFastBoot=yes\r\nruntime.network.latency=none\r\nruntime.network.speed=full\r\n"
    "showDeviceFrame=no\r\nhw.lcd.vsync=60\r\n";

bool writable(const std::wstring &dir) {
    createDirs(dir);
    std::wstring probe = dir + L"\\.write-test";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    CloseHandle(h);
    return true;
}

// Unpacking runs inside a job that ends with Velox, so closing Velox never leaves a half-written
// package behind that keeps changing.
HANDLE job() {
    static HANDLE j = [] {
        HANDLE h = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (h && !SetInformationJobObject(h, JobObjectExtendedLimitInformation, &li, sizeof(li))) {
            CloseHandle(h);
            h = nullptr;
        }
        return h;
    }();
    return j;
}

DWORD runWait(std::wstring cmd, DWORD timeoutMs) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi))
        return 1;
    if (!job() || !AssignProcessToJobObject(job(), pi.hProcess)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 1;
    }
    if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 1;
    }
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0)
        GetExitCodeProcess(pi.hProcess, &code);
    else
        TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

bool unzip(const std::wstring &zip, const std::wstring &dest) {
    createDirs(dest);
    std::wstring tar = envVar(L"SystemRoot") + L"\\System32\\tar.exe";
    if (exists(tar) && runWait(L"\"" + tar + L"\" -xf \"" + zip + L"\" -C \"" + dest + L"\"", 30 * 60 * 1000) == 0)
        return true;
    // Older Windows without tar: PowerShell can unpack zips too (slower).
    std::wstring ps = envVar(L"SystemRoot") + L"\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
    auto literal = [](const std::wstring &text) {
        std::wstring out = L"'";
        for (auto c : text)
            out += c == L'\'' ? L"''" : std::wstring(1, c);
        return out + L"'";
    };
    return runWait(L"\"" + ps + L"\" -NoProfile -NonInteractive -Command \"Expand-Archive -LiteralPath " + literal(zip) +
                       L" -DestinationPath " + literal(dest) + L" -Force\"",
                   60 * 60 * 1000) == 0;
}

void fail(const std::wstring &msg) {
    {
        std::lock_guard<std::mutex> l(progress.lock);
        progress.error = msg;
    }
    progress.failed = true;
    progress.running = false;
}

std::wstring mb(uint64_t b) {
    return std::to_wstring((b + (1 << 19)) >> 20) + L" MB";
}

void worker() {
    std::wstring base = home(), sdk = base + L"\\Sdk", avdHome = base + L"\\avd", downloads = base + L"\\downloads";
    if (!writable(base))
        return fail(i18n::text(L"Can't write to ") + base + i18n::text(L". Pick another folder."));
    createDirs(sdk);
    createDirs(downloads);
    // A package counts as installed only once its unpacking was confirmed, which is when its
    // downloaded zip gets deleted. A zip that is still there means the last unpack didn't finish.
    auto zipOf = [&](const Package &p) {
        std::wstring url = p.url;
        return downloads + L"\\" + url.substr(url.find_last_of(L'/') + 1);
    };
    auto have = [&](const Package &p) { return exists(sdk + L"\\" + p.check) && !exists(zipOf(p)); };
    uint64_t todo = 0;
    for (auto &p : PACKAGES)
        if (!have(p))
            todo += p.size;
    if (static_cast<uint64_t>(freeGB(base)) * (1ull << 30) < todo * 2 + (6ull << 30))
        return fail(i18n::text(L"Not enough free space on that drive. About ") + std::to_wstring((todo * 2 >> 30) + 6) +
                    i18n::text(L" GB are needed."));
    uint64_t doneBefore = 0;
    for (auto &p : PACKAGES) {
        if (have(p))
            continue;
        if (progress.cancel)
            return fail(i18n::text(L"Setup was cancelled."));
        std::wstring url = p.url, zip = zipOf(p);
        std::wstring err;
        bool ok = net::download(
            url, zip, p.size, p.sha1, net::Hash::Sha1,
            [&](uint64_t done, uint64_t total) {
                float f = todo ? static_cast<float>(doneBefore + done) / static_cast<float>(todo) : 1.f;
                progress.set(0.02f + 0.83f * f,
                             i18n::text(L"Downloading ") + std::wstring(p.name) + L"  ·  " + mb(done) + i18n::text(L" of ") + mb(total));
            },
            &progress.cancel, err);
        if (!ok)
            return fail(L"Download of " + std::wstring(p.name) + L" failed: " + err +
                        i18n::text(L". Try again to continue where it stopped."));
        doneBefore += p.size;
        progress.set(progress.fraction, L"Unpacking " + std::wstring(p.name) + L"…");
        std::wstring dest = *p.into ? sdk + L"\\" + p.into : sdk;
        if (!unzip(zip, dest) || !exists(sdk + L"\\" + p.check))
            return fail(i18n::text(L"Couldn't unpack ") + std::wstring(p.name) + L".");
        DeleteFileW(zip.c_str());
    }
    RemoveDirectoryW(downloads.c_str());
    if (!paths().avdDir.empty()) {
        // The phone already exists (only Android was missing): keep it, don't create a second one.
        setHome(base);
        progress.set(1.f, L"Done");
        progress.done = true;
        progress.running = false;
        return;
    }
    progress.set(0.92f, i18n::text(L"Creating your phone…"));
    std::wstring avdDir = avdHome + L"\\" + AVD_NAME + L".avd";
    createDirs(avdDir);
    if (!exists(avdDir + L"\\config.ini")) {
        std::ofstream c(std::filesystem::path(avdDir + L"\\config.ini"), std::ios::binary);
        c << CONFIG_TEMPLATE;
        if (!c)
            return fail(i18n::text(L"Couldn't create the phone in ") + avdDir + L".");
    }
    {
        std::ofstream ini(std::filesystem::path(avdHome + L"\\" + AVD_NAME + L".ini"), std::ios::binary | std::ios::trunc);
        ini << "avd.ini.encoding=UTF-8\r\npath=" << narrow(avdDir) << "\r\npath.rel=avd\\" << narrow(AVD_NAME)
            << ".avd\r\ntarget=android-35\r\n";
    }
    setHome(base);
    // The caller refreshes discovery on its UI thread after observing done. Publishing
    // g_paths from this worker would race painting and timers reading its strings.
    progress.set(1.f, L"Done");
    progress.done = true;
    progress.running = false;
}

} // namespace

bool createPhoneAt(const std::wstring &base, std::wstring &error, const std::wstring &registeredFolder) {
    auto avdHome = base + L"\\avd", avdDir = avdHome + L"\\" + AVD_NAME + L".avd";
    createDirs(avdDir);
    if (!exists(avdDir + L"\\config.ini")) {
        std::ofstream config(std::filesystem::path(avdDir + L"\\config.ini"), std::ios::binary);
        config << CONFIG_TEMPLATE;
        config.close();
        if (!config.good()) {
            error = i18n::text(L"Couldn't create the phone in ") + avdDir;
            return false;
        }
    }
    auto registered = registeredFolder.empty() ? base : registeredFolder;
    std::ofstream ini(std::filesystem::path(avdHome + L"\\" + AVD_NAME + L".ini"), std::ios::binary | std::ios::trunc);
    ini << "avd.ini.encoding=UTF-8\r\npath=" << narrow(registered + L"\\avd\\" + AVD_NAME + L".avd") << "\r\npath.rel=avd\\"
        << narrow(AVD_NAME) << ".avd\r\ntarget=android-35\r\n";
    ini.close();
    if (!ini.good()) {
        error = i18n::text(L"Couldn't register the new phone.");
        return false;
    }
    return true;
}

std::wstring defaultHome() {
    std::wstring next = exeFolder() + L"Android";
    if (writable(exeFolder()))
        return next;
    return envVar(L"LOCALAPPDATA") + L"\\Velox\\Android";
}

std::wstring home() {
    wchar_t buf[MAX_PATH] = L"";
    GetPrivateProfileStringW(L"launcher", L"home", L"", buf, MAX_PATH, iniPath().c_str());
    return *buf ? std::wstring(buf) : defaultHome();
}

void setHome(const std::wstring &folder) {
    WritePrivateProfileStringW(L"launcher", L"home", folder.c_str(), iniPath().c_str());
}

uint64_t downloadBytes() {
    uint64_t n = 0;
    for (auto &p : PACKAGES)
        n += p.size;
    return n;
}

uint64_t installedBytes() {
    return 7ull << 30;
}

int freeGB(const std::wstring &folder) {
    ULARGE_INTEGER free{};
    std::wstring root = folder.size() >= 3 ? folder.substr(0, 3) : folder;
    return GetDiskFreeSpaceExW(root.c_str(), &free, nullptr, nullptr) ? static_cast<int>(free.QuadPart >> 30) : 0;
}

bool fetchLicense(std::wstring &text, std::wstring &error) {
    std::string xml;
    if (!net::getText(L"https://dl.google.com/android/repository/repository2-3.xml", 8 << 20, xml, error))
        return false;
    size_t at = xml.find("<license id=\"android-sdk-license\"");
    size_t open = at == std::string::npos ? at : xml.find('>', at);
    size_t close = open == std::string::npos ? open : xml.find("</license>", open);
    if (close == std::string::npos) {
        error = L"the license text wasn't found";
        return false;
    }
    std::string s = xml.substr(open + 1, close - open - 1);
    for (auto [from, to] :
         {std::pair{"&quot;", "\""}, std::pair{"&apos;", "'"}, std::pair{"&lt;", "<"}, std::pair{"&gt;", ">"}, std::pair{"&amp;", "&"}}) {
        for (size_t p; (p = s.find(from)) != std::string::npos;)
            s.replace(p, strlen(from), to);
    }
    std::wstring w = widen(s), out;
    for (wchar_t c : w) {
        if (c == L'\n')
            out += L"\r\n";
        else if (c != L'\r')
            out += c;
    }
    text = out;
    return true;
}

void start() {
    if (progress.running.exchange(true))
        return;
    progress.done = false;
    progress.failed = false;
    progress.cancel = false;
    progress.set(0.01f, L"Preparing…");
    std::thread(worker).detach();
}

bool enableHypervisor() {
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    wchar_t system[MAX_PATH]{};
    if (!GetSystemDirectoryW(system, MAX_PATH))
        return false;
    std::wstring dism = std::wstring(system) + L"\\dism.exe";
    sei.lpFile = dism.c_str();
    sei.lpParameters = L"/online /enable-feature /featurename:HypervisorPlatform /all /norestart";
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei) || !sei.hProcess)
        return false;
    WaitForSingleObject(sei.hProcess, 15 * 60 * 1000);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return code == 0 || code == 3010; // 3010: done, restart needed
}

} // namespace phone::setup
