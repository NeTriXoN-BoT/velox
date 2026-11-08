#pragma once
#include "phone.hpp"
#include "instances.hpp"
#include <propsys.h>
#include <propkey.h>
#include <shellapi.h>
#include <array>

namespace phone::shell {
class Identity {
    HWND window = nullptr;
    DWORD pid = 0;
    uint64_t birth = 0;
    std::array<PROPVARIANT, 4> previous{};
    std::array<std::wstring, 4> applied{};
    HICON oldSmall = nullptr, oldBig = nullptr, small = nullptr, big = nullptr;
    bool iconsApplied = false;
    inline static const std::array<PROPERTYKEY, 4> keys{PKEY_AppUserModel_RelaunchCommand, PKEY_AppUserModel_RelaunchDisplayNameResource,
                                                      PKEY_AppUserModel_RelaunchIconResource, PKEY_AppUserModel_ID};
    static uint64_t born(DWORD id) {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id);
        FILETIME c{}, e{}, k{}, u{};
        bool ok = process && GetProcessTimes(process, &c, &e, &k, &u);
        if (process)
            CloseHandle(process);
        return ok ? (uint64_t(c.dwHighDateTime) << 32) | c.dwLowDateTime : 0;
    }
    bool same() const {
        DWORD owner = 0;
        return window && IsWindow(window) && GetWindowThreadProcessId(window, &owner) && owner == pid && birth && born(pid) == birth;
    }

  public:
    ~Identity() { reset(); }
    void reset() {
        IPropertyStore *store = nullptr;
        if (same() && SUCCEEDED(SHGetPropertyStoreForWindow(window, IID_PPV_ARGS(&store)))) {
            for (size_t i = 0; i < keys.size(); ++i) {
                PROPVARIANT current{};
                if (SUCCEEDED(store->GetValue(keys[i], &current)) && current.vt == VT_LPWSTR && current.pwszVal &&
                    applied[i] == current.pwszVal)
                    store->SetValue(keys[i], previous[i]);
                PropVariantClear(&current);
            }
            store->Release();
            if (iconsApplied) {
                PostMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(oldSmall));
                PostMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(oldBig));
            }
        }
        for (auto &value : previous)
            PropVariantClear(&value);
        window = nullptr;
        pid = 0;
        birth = 0;
        iconsApplied = false;
    }
    bool bind(HWND target, DWORD verifiedPid) {
        if (window == target && pid == verifiedPid && same())
            return true;
        reset();
        DWORD owner = 0;
        if (!target || !verifiedPid || !GetWindowThreadProcessId(target, &owner) || owner != verifiedPid ||
            cachedEmulatorPid() != verifiedPid)
            return false;
        IPropertyStore *store = nullptr;
        if (FAILED(SHGetPropertyStoreForWindow(target, IID_PPV_ARGS(&store))))
            return false;
        window = target;
        pid = verifiedPid;
        birth = born(pid);
        wchar_t path[32768]{};
        DWORD count = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
        bool ok = birth && count && count < std::size(path);
        std::wstring executable(path, count);
        applied = {L"\"" + executable + L"\"" + (instances::id().empty() ? L"" : L" --instance " + instances::id()) + L" --start-phone",
                   L"Velox · " + instances::name(), executable + L",0",
                   L"Velox.Phone." + (instances::id().empty() ? L"Main" : instances::id())};
        for (size_t i = 0; i < keys.size() && ok; ++i) {
            ok = SUCCEEDED(store->GetValue(keys[i], &previous[i]));
            PROPVARIANT value{};
            value.vt = VT_LPWSTR;
            value.pwszVal = applied[i].data();
            ok = ok && SUCCEEDED(store->SetValue(keys[i], value));
        }
        store->Release();
        if (!ok) {
            reset();
            return false;
        }
        oldSmall = reinterpret_cast<HICON>(GetClassLongPtrW(window, GCLP_HICONSM));
        oldBig = reinterpret_cast<HICON>(GetClassLongPtrW(window, GCLP_HICON));
        UINT dpi = GetDpiForWindow(window);
        small = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
                                             GetSystemMetricsForDpi(SM_CXSMICON, dpi), GetSystemMetricsForDpi(SM_CYSMICON, dpi), LR_SHARED));
        big = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
                                           GetSystemMetricsForDpi(SM_CXICON, dpi), GetSystemMetricsForDpi(SM_CYICON, dpi), LR_SHARED));
        if (small)
            PostMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small));
        if (big)
            PostMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
        iconsApplied = small || big;
        return true;
    }
};
} // namespace phone::shell
