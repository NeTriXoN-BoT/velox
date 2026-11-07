// The phone itself: settings, this PC's hardware, starting and stopping the emulator, and the
// everyday tools (install apps, send files, screenshots, recording, FPS, wallpapers, root setup).
#pragma once

#include <atomic>
#include <mutex>

#include "common.hpp"

namespace phone {

// ---- choices offered in the launcher

constexpr int MAX_CPU_CORES = 64, MAX_MEMORY_GB = 64;

struct Display {
    const wchar_t *key, *name, *detail;
    int w, h, dpi;
};
const std::vector<Display>& displays();
const std::vector<int>& refreshRates();
const std::vector<int>& coreChoices();    // 0 = automatic
const std::vector<int>& memoryChoices();  // GB, 0 = automatic
enum Gpu { GPU_AUTO = 0, GPU_HOST = 1, GPU_SOFTWARE = 2 };

// ---- this PC

struct HostInfo {
    int threads = 4, cores = 2, ramGB = 8, monitorHz = 60, monitorW = 1920, monitorH = 1080;
    std::wstring cpu, gpu, gpuLuid;  // gpuLuid: "luid_0x..._0x..." as PDH names it, lower case
    uint64_t vramMB = 0;
    bool softwareGpu = false, hypervisor = false;
    int freeDiskGB = 0;  // on the drive the phone lives on
};
HostInfo host();
void refreshHost(HWND window = nullptr);          // re-reads free disk space and the monitor
std::wstring hostSummary();  // "Ryzen 9 9950X3D · 32 threads · 47 GB · RTX 5070 Ti · 240 Hz"

// ---- settings (saved in this phone's portable data folder)

struct Settings {
    int display = 2, rate = 120, cores = 0, memory = 0, gpu = GPU_AUTO;
    bool root = false, snappy = true, boost = true, quickBoot = true, bootScreen = true, frame = false;
    bool autoUpdate = true, showTouches = false, autoRotate = true, autoStart = false;
    bool gestureNavigation = false, reverseButtons = false;
    int brand = 2;      // art::Brand drawn on wallpapers (Signature)
    int bootStyle = 0;  // art::bootStyleNames() index, or 5 = a different one each start
    int cursor = 0;
    std::wstring number;
    int language = 0; // 0 English, 1 Deutsch; per phone, independent of Android system language
};
Settings settingsForHardware(const HostInfo& hardware, int cpuBudget, int ramBudget, int availableGB = -1);
Settings recommended();  // the best settings for this PC
void loadSettings(Settings& s, bool& firstRun);
void saveSettings(const Settings& s);
std::wstring iniPath();
int cpuLimit(); // effective host/observed Android engine limit
int coresFor(const Settings& s);
int memoryFor(const Settings& s);
bool coreAllowed(int choice);
bool memoryAllowed(int gb);
std::wstring signature(const Settings& s);  // what a quick-boot snapshot depends on

// ---- where things are

struct Paths {
    std::wstring sdk, avdHome, avdDir, rootImage, log;
};
const Paths& paths();
void findPaths();
bool installed();  // the SDK and the phone were found
bool useSdkFolder(const std::wstring& dir);  // an existing Android SDK picked by the user (must be Google-signed)
bool rootReady();

// ---- running the phone

enum class State { Stopped, Starting, Running, Stopping };
extern std::atomic<State> state;
extern Settings running;  // the settings the running phone was started with (valid while running)
using Report = std::function<void(const std::wstring& message, bool error)>;

// Boot progress, read by the boot screen.
struct Boot {
    std::atomic<bool> active{false}, done{false}, resume{false};
    std::atomic<float> progress{0};
    std::mutex lock;
    std::wstring stage;
    void set(float p, const std::wstring& s) {
        std::lock_guard<std::mutex> l(lock);
        if (p > progress) progress = p;
        stage = s;
    }
    std::wstring text() {
        std::lock_guard<std::mutex> l(lock);
        return stage;
    }
};
extern Boot boot;

void start(const Settings& s, const Report& report);  // blocks until booted or failed
// Whether `running` really holds what the phone was started with. After Velox is reopened,
// adoptRunning() reloads it when it belongs to the same emulator process.
bool runningKnown();
bool adoptRunning();
void forgetRunning();
void distrustSnapshot();  // the phone ran outside Velox: never resume the old quick-boot snapshot
void stop(const Report& report);                     // blocks until closed
void onExitedOutside();                              // the emulator window was closed directly
bool emulatorRunning();
DWORD emulatorPid();       // qemu-system-x86_64.exe
void applyLive(const Settings& s);  // settings that change without a restart

// ---- tools (need a running phone)

void startAdbServer();
std::string adb(const std::wstring& args, DWORD timeoutMs = 15000);
std::wstring adbSerial();
bool installApk(const std::wstring& path, const Report& report);
bool appPackageFile(const std::wstring& path);
bool sendFile(const std::wstring& path, const Report& report);
bool screenshot(const Report& report);
bool toggleRecording(const Report& report);
bool recording();
void syncRecordingState();
void rotate();
void setAutoRotation(bool enabled);
void syncOrientation();  // background tick: follow Android's default display, even while minimized
void setGestureNavigation(bool enabled, bool reverseButtons = false);
enum class NavigationResult { Unchanged, Applied, NeedsRoot, RestartRequired, Unsupported };
NavigationResult syncNavigation();  // apply when Android is ready, including adopted phones
bool navigationNeedsRestart();
int sampleFps();  // frames shown in the last second, or -1
bool applyWallpaper(int index, int brand, const Report& report);
bool applyWallpaperFile(const std::wstring& image, int brand, const Report& report);
// Wallpaper packs: pictures in <Velox folder>\wallpapers (shipped) and <data>\wallpapers (added).
std::wstring wallpaperFolder();
std::vector<std::wstring> wallpaperFiles();
int addWallpapers(const std::vector<std::wstring>& files);
bool setupRoot(const Report& report);
std::wstring capturesFolder();

}  // namespace phone
