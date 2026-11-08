#pragma once
#include "common.hpp"

namespace painting {
// Keep one surface per window. Dragging and uncovering a window only need a
// compositor copy until its content or pixel size actually changes.
class Buffer {
    HDC memory = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    int width = 0, height = 0;

  public:
    bool dirty = true;
    Buffer() = default;
    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;
    ~Buffer() { reset(); }
    void reset() {
        if (memory && previous)
            SelectObject(memory, previous);
        if (bitmap)
            DeleteObject(bitmap);
        if (memory)
            DeleteDC(memory);
        memory = nullptr;
        bitmap = nullptr;
        previous = nullptr;
        width = height = 0;
        dirty = true;
    }
    HDC get(HDC target, int w, int h) {
        if (w <= 0 || h <= 0)
            return nullptr;
        if (!memory || width != w || height != h) {
            reset();
            memory = CreateCompatibleDC(target);
            bitmap = CreateCompatibleBitmap(target, w, h);
            if (!memory || !bitmap) {
                reset();
                return nullptr;
            }
            previous = SelectObject(memory, bitmap);
            if (!previous || previous == HGDI_ERROR) {
                previous = nullptr;
                reset();
                return nullptr;
            }
            width = w;
            height = h;
        }
        return memory;
    }
};
} // namespace painting
