#include "display_scale.hpp"
#include <cassert>
#include <iostream>

int main() {
    using namespace phone::scaling;
    assert(launcher(1896, 1016, 96, 0, 16, 39) == 125);
    assert(launcher(2536, 1376, 96, 0, 16, 39) == 125);
    assert(launcher(3816, 2096, 144, 0, 24, 58) == 150);
    assert(phone::scaling::phone(5000, 1400, 2400, 1080, 0, 96) == 60);
    assert(launcher(1342, 704, 120, 0, 20, 49) == 102);
    assert(preference(-1, 75, 300) == 0 && preference(301, 75, 300) == 0);
    assert(preference(125, 75, 300) == 125);
    assert(preference(9, 10, 100) == 0 && preference(40, 10, 100) == 40);
    unsigned long cases = 0;
    for (int width : {640, 800, 1024, 1280, 1366, 1920, 2560, 3440, 3840, 5120, 7680})
        for (int height : {480, 600, 720, 768, 1080, 1440, 1600, 2160, 4320})
            for (unsigned dpi : {96u, 120u, 144u, 168u, 192u, 240u, 288u, 384u})
                for (int requested : {0, 75, 90, 100, 110, 125, 150, 175, 200, 250, 300}) {
                    int frameW = int(dpi / 6), frameH = int(dpi * 39 / 96);
                    int percent = launcher(width - 24, height - 24, dpi, requested, frameW, frameH);
                    assert(900 * percent / 100.0 + frameW <= width - 24);
                    assert(640 * percent / 100.0 + frameH <= height - 24);
                    assert(!requested || percent <= requested);
                    int toolbar = int(61 * dpi / 96);
                    for (bool landscape : {false, true}) {
                        int renderW = landscape ? 2400 : 1080, renderH = landscape ? 1080 : 2400;
                        int phonePercent = phone::scaling::phone(width - toolbar - 24, height - 24, renderW, renderH, 0);
                        assert(renderW * phonePercent / 100.0 + toolbar <= width - 24);
                        assert(renderH * phonePercent / 100.0 <= height - 24);
                        int previous = 0;
                        for (int overridePercent : {10, 25, 33, 40, 50, 67, 75, 100}) {
                            int fitted = phone::scaling::phone(width - toolbar - 24, height - 24, renderW, renderH, overridePercent);
                            assert(fitted >= previous && fitted <= overridePercent);
                            assert(renderW * fitted / 100.0 + toolbar <= width - 24);
                            assert(renderH * fitted / 100.0 <= height - 24);
                            previous = fitted;
                            ++cases;
                        }
                    }
                    ++cases;
                }
    std::cout << "PASS: " << cases << " scaling, DPI, work-area, orientation and override cases\n";
}
