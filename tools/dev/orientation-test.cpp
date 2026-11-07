// g++ -std=c++20 -I gaming-phone/src gaming-phone/tools/dev/orientation-test.cpp -o <temp>/orientation-test.exe
#include "orientation.hpp"
#include <cassert>
#include <iostream>

using namespace phone::orientation;

int main() {
    // Captured Android 15 display output contains nested task rotations as well as the
    // actual DisplayRotation value; other displays must never steer the phone window.
    std::string display = "Display: mDisplayId=0 (organized)\n"
        " overrideConfig={mRotation=ROTATION_90}\n task {mRotation=undefined}\n"
        " DisplayRotation\n  mRotation=1 mDeferredRotationPauseCount=0\n"
        "Display: mDisplayId=2\n mRotation=3\n";
    assert(displayRotation(display) == 1);
    assert(displayRotation("Display: mDisplayId=2\n mRotation=3\n" + display) == 1);
    assert(displayRotation("Display: mDisplayId=10\n mRotation=3\n" + display) == 1);
    assert(displayRotation("Display: mDisplayId=0\r\n mRotation=2\r\n") == 2);
    assert(displayRotation("Display: mDisplayId=0\n mRotation=0\n") == 0);
    assert(displayRotation("Display: mDisplayId=0\n mRotation=3\n") == 3);
    assert(displayRotation("Display: mDisplayId=0\n mRotation=ROTATION_90\nDisplay: mDisplayId=2\n mRotation=1\n") == -1);
    assert(displayRotation("Display: mDisplayId=0\n mRotation=90\n") == -1);
    assert(displayRotation("Display: mDisplayId=0\n mRotation=4\n") == -1);
    assert(displayRotation("error: device offline") == -1);
    assert(displayRotation("") == -1);
    assert(deviceRotation("acceleration = 0:9.77631:0.812349\r\r\nOK") == 0);
    assert(deviceRotation("acceleration = 9.81:0:0\nOK") == 1);
    assert(deviceRotation("acceleration = 0:-9.81:0\nOK") == 2);
    assert(deviceRotation("acceleration = -9.81:-1.90735e-06:0\nOK") == 3);
    assert(deviceRotation("acceleration = 0:0:9.81\nOK") == -1);
    assert(deviceRotation("acceleration = 6.9:6.9:0\nOK") == -1);
    assert(deviceRotation("acceleration = nan:9.81:0\nOK") == -1);
    assert(deviceRotation("acceleration = 9.81:0:inf\nOK") == -1);
    assert(deviceRotation("acceleration = 9.81:0\nOK") == -1);
    assert(deviceRotation("KO: unknown sensor") == -1);
    assert(deviceRotation("") == -1);
    // The console rotates clockwise: portrait -> reverse landscape -> upside down -> landscape.
    const int expected[4][4] = {{0, 3, 2, 1}, {1, 0, 3, 2}, {2, 1, 0, 3}, {3, 2, 1, 0}};
    for (int from = 0; from < 4; ++from)
        for (int to = 0; to < 4; ++to)
            assert(clockwiseSteps(from, to) == expected[from][to]);
    std::cout << "Orientation tests passed (37 cases).\n";
}
