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
