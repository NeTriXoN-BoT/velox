#pragma once
#include "phone.hpp"
namespace phone::instances {
constexpr UINT CONTROL = WM_APP + 55;
enum class Command { Open = 1, Start, Stop, CloseControls };
struct Info {
    std::wstring id, name, data, sdk, error;
    int port = 5580, cores = 0, memory = 0;
    bool active = false, starting = false, current = false;
    HWND window = nullptr;
};
// A per-phone lease prevents a second launcher from starting during a cold copy.
class Maintenance {
    HANDLE handle = nullptr;
    HANDLE launcher = nullptr;

  public:
    bool acquired = false;
    std::wstring error;
    explicit Maintenance(const std::wstring &id);
    ~Maintenance();
    Maintenance(const Maintenance &) = delete;
    Maintenance &operator=(const Maintenance &) = delete;
};
struct Storage {
    uint64_t phone = 0, allocated = 0, reclaimable = 0, desktop = 0;
    std::wstring id, home, previous, error;
};
bool rename(const std::wstring &id, const std::wstring &name, const Report &report);
bool clone(const std::wstring &id, const std::wstring &name, bool full, const Report &report);
Storage storage(const std::wstring &id);
bool cleanup(const std::wstring &id, const Report &report);
bool relocate(const std::wstring &id, const std::wstring &parent, const Report &report);
bool undoRelocation(const std::wstring &id, const Report &report);
bool configure(std::wstring &error);
const std::wstring &id();
std::wstring name();
std::wstring windowClass();
std::wstring mutexName();
std::wstring dataPath(const std::wstring &primary);
int port();
Command initialCommand();
std::vector<Info> list();
bool create(const std::wstring &sdk, const Report &report);
bool recycle(const std::wstring &id, const Report &report);
bool command(const std::wstring &id, Command action, const Report &report);
HWND window(const std::wstring &id);
bool otherLaunchers();
bool ownsWrapper(DWORD wrapper);
bool reserve(const Settings &settings, std::wstring &error);
bool budget(const Settings &settings, std::wstring &error);
void failure(const std::wstring &message);
bool started(DWORD wrapper, const std::wstring &sdk, int actualPort);
void lowerCpuAllocation(int actual);
void stopped();
} // namespace phone::instances
