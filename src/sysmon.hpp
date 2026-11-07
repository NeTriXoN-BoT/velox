// Live usage of this PC and of the phone, from Windows' own counters (no extra tools).
#pragma once

#include "common.hpp"

namespace phone::sysmon {

struct Sample {
    float cpu = -1, gpu = -1;          // whole PC, percent
    float ramUsedGB = 0, ramTotalGB = 0;
    float vramUsedGB = -1;             // dedicated video memory in use
    float phoneCpu = -1, phoneGpu = -1;  // the emulator's share, percent of the whole PC
    float phoneRamGB = 0;
};

void setActive(bool on);  // sample once a second while the System page is open
Sample latest();
std::vector<Sample> history();  // up to the last 60 samples, oldest first

struct Specs {
    std::wstring cpu, gpu, memory, display, windows, virtualization, android, emulator;
};
Specs specs();

}  // namespace phone::sysmon
