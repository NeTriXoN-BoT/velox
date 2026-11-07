#pragma once
#include "phone.hpp"
#include <optional>
namespace phone::vault {
struct Entry {
    std::wstring path, name, created;
    uint64_t bytes = 0;
    size_t files = 0;
    bool valid = false;
};
struct Progress {
    std::atomic<bool> running{false}, cancel{false};
    std::atomic<float> fraction{0};
    std::mutex lock;
    std::wstring step;
    void set(float f, const std::wstring &s) {
        fraction = f;
        std::lock_guard<std::mutex> g(lock);
        step = s;
    }
    std::wstring text() {
        std::lock_guard<std::mutex> g(lock);
        return step;
    }
};
extern Progress progress;
struct Schedule {
    int days = 0, keep = 3, limitGB = 25;
    uint64_t last = 0;
    std::wstring status;
    bool paused = false;
};
Schedule schedule();
bool schedule(const Schedule &value);
bool scheduleDue();
bool runScheduled(const Report &report);
std::wstring folder();
bool setFolder(const std::wstring &folder);
std::vector<Entry> list();
Entry inspect(const std::wstring &backup);
bool create(const Report &report);
// Destination must be new. Copies only cold phone disks, verifies, then rebases them.
bool copyPhone(const Paths &source, const std::wstring &destination, const Report &report);
bool verify(const std::wstring &backup, const Report &report);
std::optional<std::wstring> prepare(const std::wstring &backup, const Report &report);
bool activate(const std::wstring &prepared, const Report &report);
bool undo(const Report &report);
bool canUndo();
bool blocked();
bool recoverInterrupted(const Report &report);
std::wstring diagnostics();
} // namespace phone::vault
