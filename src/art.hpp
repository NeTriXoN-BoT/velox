// Generated artwork: the logo, phone wallpapers, mouse cursors, the boot screen and the device frame.
// Everything is drawn at runtime at the exact size needed, so nothing has to ship next to the exe.
#pragma once

#include <memory>

#include "common.hpp"

namespace phone::art {

// --- drawing helpers shared with the launcher window
void roundRectPath(gp::GraphicsPath& path, const gp::RectF& r, float rad);
void fillRound(gp::Graphics& g, const gp::RectF& r, float rad, const gp::Brush& b);
void logo(gp::Graphics& g, float x, float y, float size);

// --- wallpapers
struct WallpaperInfo {
    const wchar_t* name;
    const wchar_t* file;  // file name without extension
};
const std::vector<WallpaperInfo>& wallpapers();
std::unique_ptr<gp::Bitmap> renderWallpaper(int index, int w, int h);
// Velox branding drawn on wallpapers, in the style of Linux distro wallpapers.
enum Brand { BRAND_NONE = 0, BRAND_BADGE = 1, BRAND_SIGNATURE = 2, BRAND_POSTER = 3 };
const std::vector<const wchar_t*>& brandNames();
void drawBranding(gp::Graphics& g, int w, int h, int brand);
// A picture cropped to fill w x h, with the chosen branding on top.
std::unique_ptr<gp::Bitmap> fitImage(gp::Bitmap& src, int w, int h, int brand);
bool savePng(gp::Bitmap& bmp, const std::wstring& path);

// --- cursors (0 is the normal Windows cursor)
const std::vector<const wchar_t*>& cursorNames();
HCURSOR makeCursor(int style, int size);
void drawCursorPreview(gp::Graphics& g, int style, const gp::RectF& r);

// --- boot screens
const std::vector<const wchar_t*>& bootStyleNames();  // Orbit, Hyperdrive, Neon Grid, Terminal, Minimal

// One boot screen style, with the parts that don't move cached for one size.
class BootScreen {
public:
    BootScreen(int w, int h, int style = 0);
    // t: seconds since the boot screen appeared; progress: 0..1; status: one short line;
    // subtitle: what the phone is set to.
    void draw(gp::Graphics& g, float t, float progress, const std::wstring& status, const std::wstring& subtitle);

private:
    void wordmark(gp::Graphics& g, float cx, float ty, float t, bool chrome);
    void footer(gp::Graphics& g, float progress, const std::wstring& status, float y);
    int w_, h_, style_;
    std::unique_ptr<gp::Bitmap> back_, glow_;
    std::unique_ptr<gp::FontFamily> family_, mono_;
    std::unique_ptr<gp::Font> word_, small_, tiny_, code_;
    std::vector<float> advances_;
    float wordWidth_ = 0;
};

// --- device frame skin for the emulator (a sleek graphite phone body around the screen)
// Writes the skin files into `dir` for a screen of w x h pixels. Returns false on failure.
bool writeDeviceFrame(const std::wstring& dir, int w, int h);

}  // namespace phone::art
