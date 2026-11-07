// Renders a sheet of the branding styles over a built-in design and a picture, for checking.
//   brandpreview <picture> <out.png>
#include "art.hpp"

namespace phone {
std::wstring dataFolder() { return L"."; }
}

int wmain(int argc, wchar_t** argv) {
    gp::GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    gp::GdiplusStartup(&token, &gsi, nullptr);
    const int W = 540, H = 1200;
    gp::Bitmap sheet(W * 4, H * 2, PixelFormat32bppARGB);
    {
        gp::Graphics g(&sheet);
        gp::Bitmap pic(argv[1]);
        for (int brand = 0; brand < 4; brand++) {
            auto a = phone::art::renderWallpaper(1, W, H);  // Neon Grid
            {
                gp::Graphics ag(a.get());
                phone::art::drawBranding(ag, W, H, brand);
            }
            g.DrawImage(a.get(), brand * W, 0, W, H);
            auto b = phone::art::fitImage(pic, W, H, brand);
            g.DrawImage(b.get(), brand * W, H, W, H);
        }
    }
    phone::art::savePng(sheet, argv[2]);
    gp::GdiplusShutdown(token);
    return 0;
}
