#pragma once
#include "common.hpp"
#include "display_scale.hpp"
#include <cmath>

namespace phone::scaling {
constexpr const wchar_t *PROPERTY = L"VeloxUiPercent", *REQUESTED = L"VeloxUiRequestedPercent";
struct Desktop {
    RECT work{};
    UINT dpi = 96;
    int padding = 12;
    int width() const { return std::max(1L, work.right - work.left - padding * 2); }
    int height() const { return std::max(1L, work.bottom - work.top - padding * 2); }
};
inline Desktop desktop(HWND window, const RECT *anchor = nullptr) {
    Desktop result;
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    auto target = anchor ? MonitorFromRect(anchor, MONITOR_DEFAULTTONEAREST) : MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    if (GetMonitorInfoW(target, &monitor))
        result.work = monitor.rcWork;
    else
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &result.work, 0);
    result.dpi = window ? GetDpiForWindow(window) : GetDpiForSystem();
    result.padding = MulDiv(12, result.dpi, 96);
    return result;
}
inline RECT outer(HWND window, int width, int height, UINT dpi) {
    RECT result{0, 0, width, height};
    AdjustWindowRectExForDpi(&result, GetWindowLongW(window, GWL_STYLE), FALSE, GetWindowLongW(window, GWL_EXSTYLE), dpi);
    return result;
}
inline POINT position(const RECT &anchor, const Desktop &screen, int width, int height) {
    return {std::clamp<LONG>(anchor.left, screen.work.left + screen.padding,
                             std::max<LONG>(screen.work.left + screen.padding, screen.work.right - screen.padding - width)),
            std::clamp<LONG>(anchor.top, screen.work.top + screen.padding,
                             std::max<LONG>(screen.work.top + screen.padding, screen.work.bottom - screen.padding - height))};
}
inline float ownerScale(HWND owner, UINT dpi, int workHeight) {
    if (!GetPropW(owner, PROPERTY))
        return dpi / 96.f;
    auto requested = reinterpret_cast<UINT_PTR>(GetPropW(owner, REQUESTED));
    int percent = requested >= 75 && requested <= 300 ? int(requested) : automatic(workHeight, dpi);
    return percent / 100.f;
}
} // namespace phone::scaling
