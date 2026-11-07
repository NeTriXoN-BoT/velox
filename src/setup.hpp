// First-run setup on a PC without the phone: downloads Google's official Android emulator,
// platform tools and the Android 15 (Google Play) system image, checks every file against a
// pinned checksum, unpacks them and creates the phone. Nothing from Google is shipped with the
// launcher itself.
#pragma once

#include <atomic>
#include <mutex>

#include "common.hpp"

namespace phone::setup {

struct Progress {
    std::atomic<bool> running{false}, done{false}, failed{false}, cancel{false};
    std::atomic<float> fraction{0};
    std::mutex lock;
    std::wstring step, error;
    void set(float f, const std::wstring& s) {
        std::lock_guard<std::mutex> l(lock);
        fraction = f;
        step = s;
    }
    std::wstring text() {
        std::lock_guard<std::mutex> l(lock);
        return step;
    }
};
extern Progress progress;

std::wstring defaultHome();  // next to the launcher if that folder is writable, else in AppData
std::wstring home();         // the folder chosen for this PC
void setHome(const std::wstring& folder);
bool createPhoneAt(const std::wstring& folder, std::wstring& error, const std::wstring& registeredFolder = L"");
uint64_t downloadBytes();
uint64_t installedBytes();
int freeGB(const std::wstring& folder);

// The Android SDK License Agreement, read from Google's own package list.
bool fetchLicense(std::wstring& text, std::wstring& error);
void start();  // runs in the background; see progress
bool enableHypervisor();  // asks Windows (with an admin prompt) to turn on Windows Hypervisor Platform

}  // namespace phone::setup
