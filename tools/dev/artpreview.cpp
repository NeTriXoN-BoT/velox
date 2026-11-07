// Renders wallpapers and a cursor sheet to PNG files, for checking the artwork.
//   artpreview <outdir> [index...]
#include "art.hpp"

namespace phone {
std::wstring dataFolder() { return L"."; }
}

int wmain(int argc, wchar_t** argv) {
    gp::GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    gp::GdiplusStartup(&token, &gsi, nullptr);
    std::wstring out = argc > 1 ? argv[1] : L".";
    std::vector<int> which;
    for (int i = 2; i < argc; i++) which.push_back(_wtoi(argv[i]));
    if (which.empty())
        for (int i = 0; i < 3; i++) which.push_back(i);
    for (int i : which) {
        if (i < 0) continue;
        auto bmp = phone::art::renderWallpaper(i, 1080, 2400);
        phone::art::savePng(*bmp, out + L"\\wp" + std::to_wstring(i) + L".png");
    }
    // Cursor sheet: every style at 64 px on a dark and a light strip.
    int n = static_cast<int>(phone::art::cursorNames().size());
    gp::Bitmap sheet(n * 80, 160, PixelFormat32bppARGB);
    {
        gp::Graphics g(&sheet);
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        gp::SolidBrush dark(gp::Color(0xFF1A1D26)), light(gp::Color(0xFFE8E8EE));
        g.FillRectangle(&dark, 0, 0, n * 80, 80);
        g.FillRectangle(&light, 0, 80, n * 80, 80);
        for (int i = 0; i < n; i++) {
            phone::art::drawCursorPreview(g, i, gp::RectF(i * 80.f + 8, 8, 64, 64));
            phone::art::drawCursorPreview(g, i, gp::RectF(i * 80.f + 8, 88, 64, 64));
        }
    }
    phone::art::savePng(sheet, out + L"\\cursors.png");
    gp::GdiplusShutdown(token);
    return 0;
}
