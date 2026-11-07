// Renders one frame of every boot screen style side by side.  bootpreview <out.png> <t>
#include "art.hpp"

namespace phone {
std::wstring dataFolder() { return L"."; }
}

int wmain(int argc, wchar_t** argv) {
    gp::GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    gp::GdiplusStartup(&token, &gsi, nullptr);
    float t = argc > 2 ? static_cast<float>(_wtof(argv[2])) : 2.2f;
    const int W = 432, H = 960, N = static_cast<int>(phone::art::bootStyleNames().size());
    gp::Bitmap sheet(W * N + 8 * (N - 1), H, PixelFormat32bppARGB);
    {
        gp::Graphics g(&sheet);
        g.Clear(gp::Color(0xFF303030));
        for (int s = 0; s < N; s++) {
            phone::art::BootScreen bs(W, H, s);
            gp::Bitmap frame(W, H, PixelFormat32bppPARGB);
            {
                gp::Graphics fg(&frame);
                bs.draw(fg, t, 0.55f, L"Loading apps and services   55%", L"FULL HD+  ·  144 HZ  ·  ROOT");
            }
            g.DrawImage(&frame, s * (W + 8), 0, W, H);
        }
    }
    phone::art::savePng(sheet, argv[1]);
    gp::GdiplusShutdown(token);
    return 0;
}
