#pragma once
#include <bit>
#include "sysmon.hpp"
#include "phone.hpp"
#include <wbemidl.h>
#include <cmath>
#include <cstring>
#include <map>

namespace phone::sysmon::detail {
inline std::wstring lower(std::wstring value) {
    for (auto &c : value)
        c = static_cast<wchar_t>(towlower(c));
    return value;
}
inline bool reading(float value, float maximum) { return std::isfinite(value) && value >= 0 && value <= maximum; }

// Optional read-only driver API. Never search the working directory or ship a
// monitoring driver: the installed display driver owns this library and device.
class Nvidia {
    HMODULE library = nullptr;
    void *device = nullptr;
    bool initialized = false;
    using Simple = int (*)();
    using Count = int (*)(unsigned *);
    using Device = int (*)(unsigned, void **);
    using Luid = int (*)(void *, char *, unsigned *);
    using Name = int (*)(void *, char *, unsigned);
    using Number = int (*)(void *, unsigned *);
    using Indexed = int (*)(void *, unsigned, unsigned *);
    struct Memory { unsigned long long total, free, used; };
    using GetMemory = int (*)(void *, Memory *);
    template <typename F> F symbol(const char *name) {
        // GetProcAddress uses a generic FARPROC; the declarations below give each
        // optional NVML entry its actual ABI without a generic function call.
        return std::bit_cast<F>(GetProcAddress(library, name));
    }

  public:
    Nvidia() {
        wchar_t directory[MAX_PATH]{};
        UINT count = GetSystemDirectoryW(directory, MAX_PATH);
        if (!count || count >= MAX_PATH)
            return;
        auto path = std::wstring(directory, count) + L"\\nvml.dll";
        library = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library)
            return;
        auto init = symbol<Simple>("nvmlInit_v2");
        auto getCount = symbol<Count>("nvmlDeviceGetCount_v2");
        auto getDevice = symbol<Device>("nvmlDeviceGetHandleByIndex_v2");
        auto getLuid = symbol<Luid>("nvmlDeviceGetLuid");
        auto getName = symbol<Name>("nvmlDeviceGetName");
        if (!init || !getCount || !getDevice || init() != 0)
            return;
        initialized = true;
        unsigned devices = 0;
        if (getCount(&devices) != 0 || devices > 32)
            return;
        auto selected = host();
        for (unsigned index = 0; index < devices; ++index) {
            void *candidate = nullptr;
            if (getDevice(index, &candidate) != 0)
                continue;
            char luid[8]{};
            unsigned mask = 0;
            bool matches = false;
            if (getLuid && getLuid(candidate, luid, &mask) == 0) {
                unsigned lo = 0, hi = 0;
                std::memcpy(&lo, luid, 4);
                std::memcpy(&hi, luid + 4, 4);
                wchar_t value[64];
                swprintf(value, 64, L"luid_0x%08x_0x%08x", hi, lo);
                matches = selected.gpuLuid == value;
            } else if (devices == 1 && getName && !selected.gpu.empty()) {
                char name[256]{};
                if (getName(candidate, name, unsigned(std::size(name))) == 0)
                    matches = lower(widen(name)).find(lower(selected.gpu)) != std::wstring::npos;
            }
            if (matches) {
                device = candidate;
                break;
            }
        }
    }
    ~Nvidia() {
        if (initialized)
            if (auto close = symbol<Simple>("nvmlShutdown"))
                close();
        if (library)
            FreeLibrary(library);
    }
    Nvidia(const Nvidia &) = delete;
    void sample(Sample &value) {
        if (!device)
            return;
        auto indexed = [&](const char *name, unsigned index, float &result, float maximum) {
            unsigned number = 0;
            auto function = symbol<Indexed>(name);
            if (function && function(device, index, &number) == 0 && number <= maximum)
                result = float(number);
        };
        indexed("nvmlDeviceGetTemperature", 0, value.gpuTemperature, 130);
        indexed("nvmlDeviceGetClockInfo", 0, value.gpuClockMHz, 20000);
        auto number = [&](const char *name, float &result, float divisor, float maximum) {
            unsigned resultValue = 0;
            auto function = symbol<Number>(name);
            if (function && function(device, &resultValue) == 0 && reading(resultValue / divisor, maximum))
                result = resultValue / divisor;
        };
        number("nvmlDeviceGetPowerUsage", value.gpuPowerW, 1000, 2000);
        number("nvmlDeviceGetFanSpeed", value.gpuFanPercent, 1, 100);
        Memory memory{};
        auto getMemory = symbol<GetMemory>("nvmlDeviceGetMemoryInfo");
        if (getMemory && getMemory(device, &memory) == 0 && memory.total && memory.used <= memory.total && memory.total <= (1ull << 42)) {
            value.vramTotalGB = float(memory.total / 1073741824.0);
            value.vramUsedGB = float(memory.used / 1073741824.0);
        }
    }
};

class MonitorProvider {
    IWbemServices *service = nullptr;
    ULONGLONG retryAt = 0;
    std::wstring gpuParent;
    struct String {
        BSTR value;
        explicit String(const wchar_t *text) : value(SysAllocString(text)) {}
        ~String() { SysFreeString(value); }
        operator BSTR() const { return value; }
    };
    static std::wstring string(IWbemClassObject *row, const wchar_t *key) {
        VARIANT value{};
        std::wstring result;
        if (SUCCEEDED(row->Get(key, 0, &value, nullptr, nullptr)) && value.vt == VT_BSTR && value.bstrVal && SysStringLen(value.bstrVal) <= 512)
            result.assign(value.bstrVal, SysStringLen(value.bstrVal));
        VariantClear(&value);
        return result;
    }
    template <typename F> bool query(const wchar_t *statement, F receive) {
        String language(L"WQL"), sql(statement);
        IEnumWbemClassObject *rows = nullptr;
        if (FAILED(service->ExecQuery(language, sql, WBEM_FLAG_RETURN_IMMEDIATELY | WBEM_FLAG_FORWARD_ONLY, nullptr, &rows)))
            return false;
        bool ok = true;
        auto deadline = GetTickCount64() + 120;
        for (int index = 0; index < 2048; ++index) {
            IWbemClassObject *row = nullptr;
            ULONG count = 0;
            HRESULT result = rows->Next(15, 1, &row, &count);
            if (count && row) {
                receive(row);
                row->Release();
            }
            if (result == WBEM_S_FALSE)
                break;
            if (FAILED(result) || result == WBEM_S_TIMEDOUT || index == 2047 || GetTickCount64() >= deadline) {
                ok = false;
                break;
            }
        }
        rows->Release();
        return ok;
    }
    void disconnect() {
        if (service)
            service->Release();
        service = nullptr;
        gpuParent.clear();
        retryAt = GetTickCount64() + 60000;
    }
    bool connect() {
        if (service)
            return true;
        if (GetTickCount64() < retryAt)
            return false;
        IWbemLocator *locator = nullptr;
        if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator)))) {
            disconnect();
            return false;
        }
        for (auto name : {L"ROOT\\LibreHardwareMonitor", L"ROOT\\OpenHardwareMonitor"}) {
            String path(name);
            if (SUCCEEDED(locator->ConnectServer(path, nullptr, nullptr, nullptr, WBEM_FLAG_CONNECT_USE_MAX_WAIT, nullptr, nullptr, &service)))
                break;
        }
        locator->Release();
        if (!service || FAILED(CoSetProxyBlanket(service, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                                                 RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE))) {
            disconnect();
            return false;
        }
        int matches = 0;
        auto selected = lower(host().gpu);
        if (!query(L"SELECT Identifier,Name FROM Hardware", [&](IWbemClassObject *row) {
                auto id = lower(string(row, L"Identifier")), name = lower(string(row, L"Name"));
                if (!selected.empty() && id.find(L"gpu") != std::wstring::npos && name.find(selected) != std::wstring::npos) {
                    gpuParent = id;
                    ++matches;
                }
            })) {
            disconnect();
            return false;
        }
        if (matches != 1)
            gpuParent.clear();
        return true;
    }

  public:
    ~MonitorProvider() { disconnect(); }
    MonitorProvider() = default;
    MonitorProvider(const MonitorProvider &) = delete;
    void sample(Sample &sample) {
        if (!connect())
            return;
        bool ok = query(L"SELECT Parent,Name,SensorType,Value FROM Sensor WHERE SensorType='Temperature' OR SensorType='Clock' OR SensorType='Power' OR SensorType='Fan'", [&](IWbemClassObject *row) {
            auto parent = lower(string(row, L"Parent")), name = lower(string(row, L"Name")), type = string(row, L"SensorType");
            bool cpu = parent.rfind(L"/intelcpu/", 0) == 0 || parent.rfind(L"/amdcpu/", 0) == 0 || parent.rfind(L"/cpu/", 0) == 0;
            bool gpu = !gpuParent.empty() && parent == gpuParent;
            VARIANT raw{}, converted{};
            float value = -1;
            if (SUCCEEDED(row->Get(L"Value", 0, &raw, nullptr, nullptr)) && raw.vt != VT_NULL && raw.vt != VT_EMPTY &&
                SUCCEEDED(VariantChangeType(&converted, &raw, 0, VT_R4)))
                value = converted.fltVal;
            VariantClear(&raw);
            VariantClear(&converted);
            if (type == L"Temperature" && reading(value, 130)) {
                if (cpu)
                    sample.cpuTemperature = std::max(sample.cpuTemperature, value);
                else if (gpu && name.find(L"memory") != std::wstring::npos)
                    sample.vramTemperature = std::max(sample.vramTemperature, value);
                else if (gpu && (name == L"gpu core" || name == L"gpu temperature"))
                    sample.gpuTemperature = value;
            } else if (type == L"Clock" && reading(value, 20000) && cpu && name.find(L"core") != std::wstring::npos)
                sample.cpuClockMHz = std::max(sample.cpuClockMHz, value);
            else if (type == L"Power" && reading(value, 2000) && cpu && name.find(L"package") != std::wstring::npos)
                sample.cpuPowerW = value;
            else if (type == L"Fan" && reading(value, 30000)) {
                if (gpu)
                    sample.gpuFanRPM = std::max(sample.gpuFanRPM, value);
                else if (name == L"cpu fan" || name == L"cpu")
                    sample.cpuFanRPM = value;
            }
        });
        if (!ok)
            disconnect();
    }
};
} // namespace phone::sysmon::detail
