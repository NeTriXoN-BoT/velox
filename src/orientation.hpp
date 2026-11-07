#pragma once

#include <cmath>
#include <cstdio>
#include <string>

namespace phone::orientation {

// Ignore task configurations and secondary displays: only DisplayRotation's numeric value
// describes how Android is actually presenting the phone's default display.
inline int displayRotation(const std::string& output) {
    const std::string heading = "Display: mDisplayId=";
    size_t begin = 0;
    while ((begin = output.find(heading, begin)) != std::string::npos) {
        size_t id = begin + heading.size();
        size_t end = output.find(heading, id);
        if (output.compare(id, 2, "0 ") == 0 || output.compare(id, 2, "0\n") == 0 ||
            output.compare(id, 2, "0\r") == 0) {
            size_t at = output.find("mRotation=", id);
            while (at != std::string::npos && (end == std::string::npos || at < end)) {
                at += 10;
                if (at + 1 < output.size() && output[at] >= '0' && output[at] <= '3' &&
                    (output[at + 1] == ' ' || output[at + 1] == '\r' || output[at + 1] == '\n'))
                    return output[at] - '0';
                at = output.find("mRotation=", at);
            }
            return -1;
        }
        begin = id;
    }
    return -1;
}

// Android rotations are counterclockwise from the natural portrait orientation. The
// emulator's gravity vector tells us the physical angle, independently of a game's lock.
inline int deviceRotation(const std::string& output) {
    size_t at = output.find("acceleration = ");
    double x = 0, y = 0, z = 0;
    if (at == std::string::npos || std::sscanf(output.c_str() + at + 15, "%lf:%lf:%lf", &x, &y, &z) != 3 ||
        !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return -1;
    if (std::abs(y) >= 7 && std::abs(x) < 4) return y > 0 ? 0 : 2;
    if (std::abs(x) >= 7 && std::abs(y) < 4) return x > 0 ? 1 : 3;
    return -1;  // flat or being tilted: don't guess and spin the window
}

inline int clockwiseSteps(int current, int target) { return (current - target + 4) % 4; }

}  // namespace phone::orientation
