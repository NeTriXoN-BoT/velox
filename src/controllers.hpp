#pragma once
#include "phone.hpp"
#include <array>
#include <memory>
namespace phone::controllers {
enum Button { A, B, X, Y, LB, RB, Back, Start, LS, RS, Up, Down, Left, Right, LT, RT, Count };
struct Frame {
    bool connected = false;
    uint32_t buttons = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
};
struct Device {
    std::wstring id, name;
    bool xinput = false;
};
struct Snapshot {
    std::vector<Device> devices;
    Frame frame;
    std::wstring selected;
};
class Reader {
  public:
    explicit Reader(HWND window = nullptr, bool publish = true);
    ~Reader();
    Frame read(const std::wstring &id, int layout = 0);
    Snapshot state() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
Snapshot snapshot();
Frame filtered(Frame frame, float deadzone, float sensitivity, bool invertY);
std::string registration();
std::string report(const Frame &frame);
const wchar_t *buttonName(int button);
} // namespace phone::controllers
