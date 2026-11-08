#pragma once
#include <algorithm>
#include <cmath>

namespace phone::scaling {
// Percentages describe physical pixels, independently of the monitor's DPI.
inline int preference(int value, int minimum, int maximum) {
    return value >= minimum && value <= maximum ? value : 0;
}
inline int fit(int width, int height, int contentWidth, int contentHeight, int desired) {
    if (width <= 0 || height <= 0 || contentWidth <= 0 || contentHeight <= 0)
        return 1;
    const double limit = std::min(double(width) / contentWidth, double(height) / contentHeight);
    return std::max(1, std::min(desired, int(std::floor(limit * 100))));
}
inline int automatic(int workHeight, unsigned dpi) {
    int readable = workHeight >= 1800 ? 150 : workHeight >= 950 ? 125 : 100;
    return std::max(readable, int(std::lround(std::clamp(dpi, 48u, 768u) * 100.0 / 96)));
}
inline int launcher(int width, int height, unsigned dpi, int requested, int frameWidth, int frameHeight) {
    return fit(width - frameWidth, height - frameHeight, 900, 640, requested ? requested : automatic(height, dpi));
}
inline int phone(int width, int height, int renderWidth, int renderHeight, int requested, unsigned dpi = 96) {
    // A landscape game should remain a comfortable desktop window rather than
    // growing to fill an ultrawide screen. Fullscreen and manual sizes are explicit.
    int comfortableWidth = std::min(width * 82 / 100, int(1440.0 * dpi / 96));
    int comfortableHeight = std::min(height * 82 / 100, int(960.0 * dpi / 96));
    return fit(width, height, renderWidth, renderHeight,
               requested ? requested : fit(comfortableWidth, comfortableHeight, renderWidth, renderHeight, 100));
}
} // namespace phone::scaling
