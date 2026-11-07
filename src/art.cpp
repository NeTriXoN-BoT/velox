#include "art.hpp"
#include "art_util.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace phone::art {
namespace {

using namespace util;

void addGrain(gp::Bitmap &bmp, int amount, uint32_t seed) {
    gp::Rect r(0, 0, static_cast<INT>(bmp.GetWidth()), static_cast<INT>(bmp.GetHeight()));
    gp::BitmapData d;
    if (bmp.LockBits(&r, gp::ImageLockModeRead | gp::ImageLockModeWrite, PixelFormat32bppARGB, &d) != gp::Ok)
        return;
    Rng rng(seed);
    for (int y = 0; y < r.Height; y++) {
        auto *px = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(d.Scan0) + y * d.Stride);
        for (int x = 0; x < r.Width; x++) {
            int n = static_cast<int>(rng.next() % (2 * amount + 1)) - amount;
            uint32_t c = px[x];
            int rr = std::clamp(static_cast<int>((c >> 16) & 255) + n, 0, 255);
            int gg = std::clamp(static_cast<int>((c >> 8) & 255) + n, 0, 255);
            int bb = std::clamp(static_cast<int>(c & 255) + n, 0, 255);
            px[x] = (c & 0xFF000000u) | (rr << 16) | (gg << 8) | bb;
        }
    }
    bmp.UnlockBits(&d);
}

// ---- the six wallpapers

void wpAurora(gp::Graphics &g, int w, int h) {
    float W = static_cast<float>(w), H = static_cast<float>(h);
    verticalGradient(g, W, H, 0xFF090A15, 0xFF0D0F2A);
    softLayer(g, w, h, 8, [&](gp::Graphics &s) {
        blob(s, W * 0.22f, H * 0.26f, W * 0.75f, H * 0.30f, 0xFF8B6CFF, 210);
        blob(s, W * 0.86f, H * 0.42f, W * 0.62f, H * 0.28f, 0xFF3D7BFF, 190);
        blob(s, W * 0.30f, H * 0.70f, W * 0.60f, H * 0.22f, 0xFF19C9C0, 120);
        blob(s, W * 0.90f, H * 0.10f, W * 0.40f, H * 0.16f, 0xFFFF5FA2, 120);
        blob(s, W * 0.60f, H * 0.92f, W * 0.70f, H * 0.16f, 0xFF5B3BD6, 140);
    });
    // A faint ribbon of light across the middle.
    gp::GraphicsPath ribbon;
    ribbon.AddBezier(gp::PointF(-W * 0.1f, H * 0.58f), gp::PointF(W * 0.3f, H * 0.40f), gp::PointF(W * 0.7f, H * 0.62f),
                     gp::PointF(W * 1.1f, H * 0.44f));
    for (int i = 0; i < 24; i++) {
        gp::Matrix m;
        m.Translate(0, H * 0.004f * i);
        gp::GraphicsPath p;
        p.AddPath(&ribbon, FALSE);
        p.Transform(&m);
        gp::Pen pen(argb(0xFFFFFFFF, 10), W * 0.0015f);
        g.DrawPath(&pen, &p);
    }
}

void wpNeonGrid(gp::Graphics &g, int w, int h) {
    float W = static_cast<float>(w), H = static_cast<float>(h), horizon = H * 0.60f;
    {
        gp::LinearGradientBrush sky(gp::PointF(0, -1), gp::PointF(0, horizon + 1), argb(0xFF080320), argb(0xFF3A1060));
        g.FillRectangle(&sky, 0.f, 0.f, W, horizon);
        gp::LinearGradientBrush ground(gp::PointF(0, horizon - 1), gp::PointF(0, H + 1), argb(0xFF14052B), argb(0xFF050109));
        g.FillRectangle(&ground, 0.f, horizon, W, H - horizon);
    }
    Rng rng(7);
    for (int i = 0; i < 120; i++) {
        float x = rng.unit() * W, y = rng.unit() * horizon * 0.8f, r = W * rng.range(0.0008f, 0.0024f);
        gp::SolidBrush b(argb(0xFFFFFFFF, static_cast<int>(rng.range(60, 200))));
        g.FillEllipse(&b, x - r, y - r, 2 * r, 2 * r);
    }
    float sunR = W * 0.30f, sunY = horizon - sunR * 0.55f;
    softLayer(g, w, h, 6, [&](gp::Graphics &s) { blob(s, W / 2, sunY, sunR * 2.2f, sunR * 1.9f, 0xFFFF4FA0, 150); });
    {
        gp::GraphicsPath sun;
        sun.AddEllipse(W / 2 - sunR, sunY - sunR, 2 * sunR, 2 * sunR);
        gp::Region region(&sun);
        // Stripes cut through the lower half of the sun, thicker towards the horizon.
        for (int i = 0; i < 7; i++) {
            float y = sunY + sunR * (0.05f + i * 0.14f), th = sunR * (0.02f + i * 0.012f);
            region.Exclude(gp::RectF(0, y, W, th));
        }
        region.Exclude(gp::RectF(0, horizon, W, H));
        g.SetClip(&region);
        gp::LinearGradientBrush b(gp::PointF(0, sunY - sunR), gp::PointF(0, sunY + sunR), argb(0xFFFFE07A), argb(0xFFFF3D9A));
        g.FillRectangle(&b, 0.f, sunY - sunR, W, 2 * sunR);
        g.ResetClip();
    }
    // Mountains on the horizon.
    gp::PointF hills[] = {{0, horizon},
                          {W * 0.08f, horizon - H * 0.035f},
                          {W * 0.18f, horizon - H * 0.012f},
                          {W * 0.30f, horizon - H * 0.05f},
                          {W * 0.42f, horizon - H * 0.01f},
                          {W * 0.58f, horizon - H * 0.008f},
                          {W * 0.70f, horizon - H * 0.055f},
                          {W * 0.82f, horizon - H * 0.02f},
                          {W * 0.92f, horizon - H * 0.04f},
                          {W, horizon - H * 0.015f},
                          {W, horizon}};
    gp::SolidBrush hillBrush(argb(0xFF12062A));
    g.FillPolygon(&hillBrush, hills, static_cast<INT>(std::size(hills)));
    gp::Pen hillPen(argb(0xFF41E2FF, 170), W * 0.0022f);
    g.DrawLines(&hillPen, hills, static_cast<INT>(std::size(hills)) - 1);
    // Perspective grid.
    gp::Pen grid(argb(0xFFFF4FD8, 170), W * 0.0024f);
    for (int i = -14; i <= 14; i++) {
        float xb = W / 2 + i * W * 0.16f;
        g.DrawLine(&grid, W / 2 + i * W * 0.012f, horizon, xb, H);
    }
    for (int i = 1; i < 18; i++) {
        float f = std::pow(i / 17.f, 2.2f), y = horizon + (H - horizon) * f;
        gp::Pen p(argb(0xFFFF4FD8, static_cast<int>(60 + 150 * f)), W * (0.0012f + 0.003f * f));
        g.DrawLine(&p, 0.f, y, W, y);
    }
    softLayer(g, w, h, 6, [&](gp::Graphics &s) { blob(s, W / 2, horizon, W * 0.9f, H * 0.05f, 0xFFFF6BE6, 130); });
}

void wpWaves(gp::Graphics &g, int w, int h) {
    float W = static_cast<float>(w), H = static_cast<float>(h);
    verticalGradient(g, W, H, 0xFF050813, 0xFF0B1230);
    Rng rng(11);
    for (int i = 0; i < 90; i++) {
        float x = rng.unit() * W, y = rng.unit() * H * 0.45f, r = W * rng.range(0.0007f, 0.002f);
        gp::SolidBrush b(argb(0xFFFFFFFF, static_cast<int>(rng.range(40, 170))));
        g.FillEllipse(&b, x - r, y - r, 2 * r, 2 * r);
    }
    softLayer(g, w, h, 8, [&](gp::Graphics &s) {
        blob(s, W * 0.5f, H * 0.55f, W * 0.9f, H * 0.25f, 0xFF3D7BFF, 120);
        blob(s, W * 0.2f, H * 0.8f, W * 0.6f, H * 0.2f, 0xFF8B6CFF, 120);
    });
    const gp::ARGB tops[] = {0xFF2C4BFF, 0xFF4A5BFF, 0xFF6A5CFF, 0xFF8B6CFF, 0xFF6F4FE0, 0xFF3E2CA8};
    for (int band = 0; band < 6; band++) {
        float base = H * (0.50f + band * 0.085f), amp = H * (0.022f + band * 0.004f);
        float freq = 1.6f + band * 0.35f, phase = band * 1.3f;
        std::vector<gp::PointF> pts;
        for (int i = 0; i <= 64; i++) {
            float x = W * i / 64.f;
            float y = base + amp * std::sin(freq * 2 * PI * i / 64.f + phase) + amp * 0.4f * std::sin(3.1f * PI * i / 64.f + band);
            pts.push_back({x, y});
        }
        gp::GraphicsPath p;
        p.AddCurve(pts.data(), static_cast<INT>(pts.size()));
        p.AddLine(W, H, 0.f, H);
        p.CloseFigure();
        gp::LinearGradientBrush fill(gp::PointF(0, base - amp), gp::PointF(0, H), argb(tops[band], 150), argb(0xFF05060F, 235));
        g.FillPath(&fill, &p);
        gp::Pen edge(argb(0xFFB9C4FF, 70), W * 0.0018f);
        g.DrawCurve(&edge, pts.data(), static_cast<INT>(pts.size()));
    }
}

void wpEmber(gp::Graphics &g, int w, int h) {
    float W = static_cast<float>(w), H = static_cast<float>(h);
    verticalGradient(g, W, H, 0xFF0F0606, 0xFF1A0810);
    softLayer(g, w, h, 8, [&](gp::Graphics &s) {
        blob(s, W * 0.75f, H * 0.30f, W * 0.70f, H * 0.28f, 0xFFFF6A2B, 190);
        blob(s, W * 0.20f, H * 0.52f, W * 0.65f, H * 0.26f, 0xFFFF2E5B, 170);
        blob(s, W * 0.60f, H * 0.82f, W * 0.70f, H * 0.22f, 0xFFB52C8F, 150);
        blob(s, W * 0.10f, H * 0.12f, W * 0.40f, H * 0.14f, 0xFFFFB347, 90);
    });
    // Silky light: many thin curves next to each other.
    for (int i = 0; i < 60; i++) {
        float t = i / 60.f;
        gp::PointF a(-W * 0.2f, H * (0.30f + t * 0.25f)), b(W * 0.35f, H * (0.10f + t * 0.35f)), c(W * 0.65f, H * (0.75f - t * 0.2f)),
            d(W * 1.2f, H * (0.45f + t * 0.2f));
        gp::Pen pen(argb(0xFFFFD2B0, static_cast<int>(10 + 22 * std::sin(t * PI))), W * 0.0016f);
        g.DrawBezier(&pen, a, b, c, d);
    }
}

void wpCarbon(gp::Graphics &g, int w, int h) {
    float W = static_cast<float>(w), H = static_cast<float>(h);
    g.Clear(argb(0xFF0B0C0F));
    // Twill weave: small cells with alternating light direction.
    float cell = W / 46.f;
    gp::SolidBrush a(argb(0xFF16181E)), b(argb(0xFF1D2027)), hi(argb(0xFFFFFFFF, 9));
    for (int y = 0; y * cell < H + cell; y++) {
        for (int x = 0; x * cell < W + cell; x++) {
            bool on = ((x + y) / 2) % 2 == 0;
            gp::RectF r(x * cell, y * cell, cell - W * 0.0012f, cell - W * 0.0012f);
            g.FillRectangle(on ? &a : &b, r);
            if (on)
                g.FillRectangle(&hi, r.X, r.Y, r.Width, r.Height * 0.35f);
        }
    }
    softLayer(g, w, h, 8, [&](gp::Graphics &s) {
        s.TranslateTransform(W / 2, H / 2);
        s.RotateTransform(-32);
        blob(s, 0, 0, W * 1.1f, H * 0.09f, 0xFF8B6CFF, 200);
        blob(s, W * 0.2f, H * 0.02f, W * 0.7f, H * 0.05f, 0xFF3D7BFF, 200);
    });
    // Vignette.
    gp::GraphicsPath v;
    v.AddEllipse(-W * 0.4f, -H * 0.15f, W * 1.8f, H * 1.3f);
    gp::PathGradientBrush vb(&v);
    vb.SetCenterColor(argb(0xFF000000, 0));
    gp::Color edge = argb(0xFF000000, 200);
    int n = 1;
    vb.SetSurroundColors(&edge, &n);
    g.FillRectangle(&vb, 0.f, 0.f, W, H);
}

void wpPrism(gp::Graphics &g, int w, int h) {
    float W = static_cast<float>(w), H = static_cast<float>(h);
    const int cols = 7, rows = 15;
    std::vector<gp::PointF> pts((cols + 1) * (rows + 1));
    Rng rng(23);
    for (int y = 0; y <= rows; y++) {
        for (int x = 0; x <= cols; x++) {
            float jx = (x == 0 || x == cols) ? 0 : rng.range(-0.38f, 0.38f), jy = (y == 0 || y == rows) ? 0 : rng.range(-0.38f, 0.38f);
            pts[y * (cols + 1) + x] = {W * (x + jx) / cols, H * (y + jy) / rows};
        }
    }
    gp::Pen edge(argb(0xFFFFFFFF, 14), W * 0.0012f);
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            gp::PointF p00 = pts[y * (cols + 1) + x], p10 = pts[y * (cols + 1) + x + 1], p01 = pts[(y + 1) * (cols + 1) + x],
                       p11 = pts[(y + 1) * (cols + 1) + x + 1];
            bool flip = (x + y) % 2 == 0;
            gp::PointF tris[2][3] = {{p00, p10, flip ? p11 : p01}, {flip ? p00 : p10, p11, p01}};
            for (auto &t : tris) {
                float cx = (t[0].X + t[1].X + t[2].X) / 3 / W, cy = (t[0].Y + t[1].Y + t[2].Y) / 3 / H;
                float d = cx * 0.45f + cy * 0.55f;
                gp::ARGB c = d < 0.5f ? mix(0xFF6B4FE8, 0xFF2F64F0, d * 2) : mix(0xFF2F64F0, 0xFF13A5C9, (d - 0.5f) * 2);
                float shade = 0.42f + 0.30f * rng.unit() - 0.20f * cy;
                c = mix(0xFF05060C, c, shade);
                gp::SolidBrush b(argb(c));
                g.FillPolygon(&b, t, 3);
                g.DrawPolygon(&edge, t, 3);
            }
        }
    }
}

} // namespace

// ------------------------------------------------------------------------------------------

void roundRectPath(gp::GraphicsPath &path, const gp::RectF &r, float rad) {
    float d = std::min(rad * 2, std::min(r.Width, r.Height));
    if (d <= 0.5f) {
        path.AddRectangle(r);
        return;
    }
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
}

void fillRound(gp::Graphics &g, const gp::RectF &r, float rad, const gp::Brush &b) {
    gp::GraphicsPath path;
    roundRectPath(path, r, rad);
    g.FillPath(&b, &path);
}

// The Velox mark: a faceted "V" on a glossy violet-to-blue tile (same shape as tools/VeloxIcon.java).
void logo(gp::Graphics &g, float x, float y, float size) {
    gp::GraphicsState st = g.Save();
    g.TranslateTransform(x, y);
    g.ScaleTransform(size / 100.f, size / 100.f);
    // Dark tile with a glow behind the V and two speed streaks.
    gp::LinearGradientBrush tile(gp::PointF(0, -1), gp::PointF(0, 101), argb(0xFF1E1840), argb(0xFF0A0C1A));
    fillRound(g, gp::RectF(0, 0, 100, 100), 24, tile);
    gp::GraphicsPath tilePath;
    roundRectPath(tilePath, gp::RectF(0, 0, 100, 100), 24);
    gp::GraphicsState clipState = g.Save(); // keeps the caller's own clip intact
    g.SetClip(&tilePath, gp::CombineModeIntersect);
    blob(g, 52, 50, 46, 46, C_ACCENT, 150);
    gp::PointF s1[] = {{9, 30}, {14, 30}, {25, 58}, {20, 58}}, s2[] = {{6, 44}, {10, 44}, {19, 66}, {15, 66}};
    gp::SolidBrush streak1(argb(0xFF7FA6FF, 170)), streak2(argb(0xFF7FA6FF, 110));
    g.FillPolygon(&streak1, s1, 4);
    g.FillPolygon(&streak2, s2, 4);
    g.Restore(clipState);
    // Neon rim.
    gp::LinearGradientBrush rimBrush(gp::PointF(0, 0), gp::PointF(100, 100), argb(0xFFB39BFF), argb(0xFF2F6BFF));
    gp::Pen rim(&rimBrush, 4.5f);
    gp::GraphicsPath rimPath;
    roundRectPath(rimPath, gp::RectF(2.25f, 2.25f, 95.5f, 95.5f), 22);
    g.DrawPath(&rim, &rimPath);
    // The V: violet left facet, bright right facet.
    gp::PointF left[] = {{22, 24}, {38, 24}, {51, 56}, {51, 77}, {44, 77}};
    gp::PointF right[] = {{64, 24}, {80, 24}, {58, 77}, {51, 77}, {51, 56}};
    gp::LinearGradientBrush lb(gp::PointF(20, 23), gp::PointF(50, 78), argb(0xFFC9B8FF), argb(0xFF7A5CFF));
    gp::LinearGradientBrush rb(gp::PointF(80, 23), gp::PointF(50, 78), argb(0xFFFFFFFF), argb(0xFFB9CCFF));
    g.FillPolygon(&lb, left, 5);
    g.FillPolygon(&rb, right, 5);
    g.Restore(st);
}

const std::vector<WallpaperInfo> &wallpapers() {
    static const std::vector<WallpaperInfo> list = {
        {L"Aurora", L"aurora"}, {L"Neon Grid", L"neon-grid"}, {L"Midnight", L"midnight"},
        {L"Ember", L"ember"},   {L"Carbon", L"carbon"},       {L"Prism", L"prism"},
    };
    return list;
}

std::unique_ptr<gp::Bitmap> renderWallpaper(int index, int w, int h) {
    auto bmp = std::make_unique<gp::Bitmap>(w, h, PixelFormat32bppARGB);
    {
        gp::Graphics g(bmp.get());
        setQuality(g);
        switch (index) {
        case 0:
            wpAurora(g, w, h);
            break;
        case 1:
            wpNeonGrid(g, w, h);
            break;
        case 2:
            wpWaves(g, w, h);
            break;
        case 3:
            wpEmber(g, w, h);
            break;
        case 4:
            wpCarbon(g, w, h);
            break;
        default:
            wpPrism(g, w, h);
            break;
        }
    }
    if (w >= 400)
        addGrain(*bmp, 3, 1234u + index);
    return bmp;
}

bool savePng(gp::Bitmap &bmp, const std::wstring &path) {
    UINT num = 0, size = 0;
    gp::GetImageEncodersSize(&num, &size);
    if (!size)
        return false;
    std::vector<BYTE> buf(size);
    auto *enc = reinterpret_cast<gp::ImageCodecInfo *>(buf.data());
    gp::GetImageEncoders(num, size, enc);
    for (UINT i = 0; i < num; i++) {
        if (wcscmp(enc[i].MimeType, L"image/png") == 0)
            return bmp.Save(path.c_str(), &enc[i].Clsid, nullptr) == gp::Ok;
    }
    return false;
}

namespace {

// Draws text with extra space between the letters; returns the width it took.
float spaced(gp::Graphics &g, const wchar_t *text, const gp::Font &f, float x, float y, float spacing, const gp::Brush &b,
             bool draw = true) {
    gp::StringFormat fmt(gp::StringFormat::GenericTypographic());
    float cx = x;
    for (const wchar_t *c = text; *c; c++) {
        gp::RectF box;
        g.MeasureString(c, 1, &f, gp::PointF(0, 0), &fmt, &box);
        if (draw)
            g.DrawString(c, 1, &f, gp::PointF(cx, y), &fmt, &b);
        cx += (*c == L' ' ? f.GetSize() * 0.32f : box.Width) + spacing;
    }
    return cx - x - spacing;
}

} // namespace

const std::vector<const wchar_t *> &brandNames() {
    static const std::vector<const wchar_t *> names = {L"None", L"Badge", L"Signature", L"Poster"};
    return names;
}

void drawBranding(gp::Graphics &g, int w, int h, int brand) {
    if (brand <= BRAND_NONE)
        return;
    float W = static_cast<float>(w), H = static_cast<float>(h);
    g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(gp::TextRenderingHintAntiAlias);
    gp::FontFamily fam(L"Segoe UI");
    gp::FontFamily blackFam(L"Segoe UI Black");
    gp::FontFamily *heavy = blackFam.IsAvailable() ? &blackFam : &fam;
    gp::SolidBrush white(argb(0xFFFFFFFF)), lavender(argb(0xFFCFC4FF, 235)), shadow(argb(0xFF000000, 120));
    if (brand == BRAND_BADGE) {
        // A small glass pill with the logo and name, under the status bar on the right.
        float ph = W * 0.075f, pw = W * 0.30f;
        gp::RectF pill(W - pw - W * 0.05f, H * 0.065f, pw, ph);
        gp::SolidBrush glass(argb(0xFF0B0C14, 150));
        fillRound(g, pill, ph / 2, glass);
        gp::Pen edge(argb(0xFFB49CFF, 160), W * 0.002f);
        gp::GraphicsPath pp;
        roundRectPath(pp, pill, ph / 2);
        g.DrawPath(&edge, &pp);
        logo(g, pill.X + ph * 0.14f, pill.Y + ph * 0.14f, ph * 0.72f);
        gp::Font f(&fam, ph * 0.42f, gp::FontStyleBold, gp::UnitPixel);
        gp::StringFormat fmt;
        fmt.SetLineAlignment(gp::StringAlignmentCenter);
        g.DrawString(L"VELOX", -1, &f, gp::RectF(pill.X + ph * 1.0f, pill.Y, pw - ph, ph), &fmt, &white);
    } else if (brand == BRAND_SIGNATURE) {
        // Logo, name and tagline just above the app icons, like a distro wallpaper's signature.
        float s = W * 0.13f, x = W * 0.08f, cy = H * 0.545f;
        blob(g, x + W * 0.28f, cy, W * 0.48f, s * 1.15f, 0xFF000000, 110);
        logo(g, x, cy - s / 2, s);
        gp::Font word(heavy, s * 0.66f, gp::FontStyleBold, gp::UnitPixel);
        gp::Font tag(&fam, s * 0.17f, gp::FontStyleBold, gp::UnitPixel);
        float tx = x + s * 1.22f;
        spaced(g, L"VELOX", word, tx + W * 0.003f, cy - s * 0.56f + W * 0.003f, s * 0.07f, shadow);
        spaced(g, L"VELOX", word, tx, cy - s * 0.56f, s * 0.07f, white);
        spaced(g, L"ANDROID GAMING PHONE", tag, tx + s * 0.03f, cy + s * 0.20f, s * 0.05f, lavender);
    } else {
        // Poster: huge see-through lettering across the top, with the logo and tagline under it.
        gp::GraphicsPath word;
        gp::StringFormat centre;
        centre.SetAlignment(gp::StringAlignmentCenter);
        float top = H * 0.115f;
        word.AddString(L"VELOX", -1, heavy, gp::FontStyleBold, W * 0.285f, gp::RectF(-W * 0.1f, top, W * 1.2f, W * 0.42f), &centre);
        gp::Pen glow(argb(0xFF8B6CFF, 80), W * 0.014f);
        glow.SetLineJoin(gp::LineJoinRound);
        g.DrawPath(&glow, &word);
        gp::LinearGradientBrush fill(gp::PointF(0, top), gp::PointF(W, top + W * 0.3f), argb(0xFFB49CFF, 70), argb(0xFF3D7BFF, 70));
        g.FillPath(&fill, &word);
        gp::Pen outline(argb(0xFFFFFFFF, 210), W * 0.0038f);
        outline.SetLineJoin(gp::LineJoinRound);
        g.DrawPath(&outline, &word);
        float s = W * 0.085f, ly = top + W * 0.36f;
        logo(g, W / 2 - s / 2, ly, s);
        gp::Font tag(&fam, W * 0.026f, gp::FontStyleBold, gp::UnitPixel);
        float tw = spaced(g, L"ANDROID GAMING PHONE", tag, 0, 0, W * 0.008f, white, false);
        spaced(g, L"ANDROID GAMING PHONE", tag, W / 2 - tw / 2 + W * 0.002f, ly + s * 1.2f + W * 0.002f, W * 0.008f, shadow);
        spaced(g, L"ANDROID GAMING PHONE", tag, W / 2 - tw / 2, ly + s * 1.2f, W * 0.008f, lavender);
    }
}

std::unique_ptr<gp::Bitmap> fitImage(gp::Bitmap &src, int w, int h, int brand) {
    auto out = std::make_unique<gp::Bitmap>(w, h, PixelFormat32bppARGB);
    gp::Graphics g(out.get());
    setQuality(g);
    g.Clear(gp::Color(0xFF000000));
    float sw = static_cast<float>(src.GetWidth()), sh = static_cast<float>(src.GetHeight());
    if (sw > 0 && sh > 0) {
        float sc = std::max(w / sw, h / sh); // fill the screen, cropping the sides or top/bottom
        gp::ImageAttributes ia;
        ia.SetWrapMode(gp::WrapModeTileFlipXY);
        g.DrawImage(&src, gp::RectF((w - sw * sc) / 2, (h - sh * sc) / 2, sw * sc, sh * sc), 0, 0, sw, sh, gp::UnitPixel, &ia);
    }
    drawBranding(g, w, h, brand);
    return out;
}

// ---- cursors

const std::vector<const wchar_t *> &cursorNames() {
    static const std::vector<const wchar_t *> names = {L"Windows", L"Neon", L"Crosshair", L"Touch",  L"Blade",
                                                       L"Comet",   L"Ring", L"Pixel",     L"Sakura", L"Scope"};
    return names;
}

namespace {

void arrowPath(gp::GraphicsPath &p, float x, float y, float s) {
    gp::PointF pts[] = {{x + 0.10f * s, y + 0.06f * s}, {x + 0.10f * s, y + 0.78f * s}, {x + 0.27f * s, y + 0.62f * s},
                        {x + 0.39f * s, y + 0.90f * s}, {x + 0.51f * s, y + 0.85f * s}, {x + 0.39f * s, y + 0.58f * s},
                        {x + 0.62f * s, y + 0.58f * s}};
    p.AddPolygon(pts, static_cast<INT>(std::size(pts)));
}

void drawCursor(gp::Graphics &g, int style, float x, float y, float s) {
    switch (style) {
    case 0: { // Like the normal Windows arrow, for the preview.
        gp::GraphicsPath p;
        arrowPath(p, x, y, s);
        gp::SolidBrush white(argb(0xFFFFFFFF));
        gp::Pen black(argb(0xFF000000), s * 0.05f);
        black.SetLineJoin(gp::LineJoinRound);
        g.FillPath(&white, &p);
        g.DrawPath(&black, &p);
        break;
    }
    case 1: { // Neon: gradient arrow with a glow.
        gp::GraphicsPath p;
        arrowPath(p, x, y, s);
        gp::Pen glow(argb(C_ACCENT, 70), s * 0.16f);
        glow.SetLineJoin(gp::LineJoinRound);
        g.DrawPath(&glow, &p);
        gp::LinearGradientBrush fill(gp::PointF(x, y), gp::PointF(x + 0.6f * s, y + 0.9f * s), argb(0xFFB49CFF), argb(C_ACCENT2));
        g.FillPath(&fill, &p);
        gp::Pen rim(argb(0xFFFFFFFF), s * 0.05f);
        rim.SetLineJoin(gp::LineJoinRound);
        g.DrawPath(&rim, &p);
        break;
    }
    case 2: { // Crosshair for shooters.
        float c = s / 2, r = s * 0.24f;
        gp::Pen dark(argb(0xFF000000, 170), s * 0.11f), light(argb(0xFFFFFFFF), s * 0.05f);
        dark.SetStartCap(gp::LineCapRound);
        dark.SetEndCap(gp::LineCapRound);
        light.SetStartCap(gp::LineCapRound);
        light.SetEndCap(gp::LineCapRound);
        for (gp::Pen *pen : {&dark, &light}) {
            g.DrawEllipse(pen, x + c - r, y + c - r, 2 * r, 2 * r);
            g.DrawLine(pen, x + c, y + s * 0.06f, x + c, y + c - r * 0.55f);
            g.DrawLine(pen, x + c, y + c + r * 0.55f, x + c, y + s * 0.94f);
            g.DrawLine(pen, x + s * 0.06f, y + c, x + c - r * 0.55f, y + c);
            g.DrawLine(pen, x + c + r * 0.55f, y + c, x + s * 0.94f, y + c);
        }
        gp::SolidBrush dot(argb(0xFFFF4F7A));
        float d = s * 0.05f;
        g.FillEllipse(&dot, x + c - d, y + c - d, 2 * d, 2 * d);
        break;
    }
    case 3: { // Touch: a soft fingertip circle.
        float c = s / 2, r = s * 0.30f;
        gp::SolidBrush fill(argb(0xFFFFFFFF, 80));
        g.FillEllipse(&fill, x + c - r, y + c - r, 2 * r, 2 * r);
        gp::Pen ring(argb(0xFFFFFFFF, 235), s * 0.045f), outer(argb(C_ACCENT, 200), s * 0.03f);
        g.DrawEllipse(&ring, x + c - r, y + c - r, 2 * r, 2 * r);
        float r2 = r + s * 0.05f;
        g.DrawEllipse(&outer, x + c - r2, y + c - r2, 2 * r2, 2 * r2);
        break;
    }
    case 4: { // Blade: a slim, sharp two-tone arrow like the logo's facets.
        gp::PointF left[] = {{x + 0.10f * s, y + 0.06f * s}, {x + 0.24f * s, y + 0.86f * s}, {x + 0.33f * s, y + 0.55f * s}};
        gp::PointF right[] = {{x + 0.10f * s, y + 0.06f * s}, {x + 0.33f * s, y + 0.55f * s}, {x + 0.66f * s, y + 0.62f * s}};
        gp::PointF all[] = {left[0], left[1], left[2], right[2]};
        gp::Pen edge(argb(0xFF0B0C14), s * 0.07f);
        edge.SetLineJoin(gp::LineJoinMiter);
        g.DrawPolygon(&edge, all, 4);
        gp::LinearGradientBrush lb(left[0], left[1], argb(0xFFE3DAFF), argb(0xFF8B6CFF));
        gp::LinearGradientBrush rb(right[0], right[2], argb(0xFFFFFFFF), argb(0xFFB9CCFF));
        g.FillPolygon(&lb, left, 3);
        g.FillPolygon(&rb, right, 3);
        break;
    }
    case 5: { // Comet: the neon arrow with speed streaks behind it.
        gp::Pen streak(argb(C_ACCENT2, 190), s * 0.05f);
        streak.SetStartCap(gp::LineCapRound);
        streak.SetEndCap(gp::LineCapRound);
        g.DrawLine(&streak, x + 0.55f * s, y + 0.70f * s, x + 0.80f * s, y + 0.95f * s);
        gp::Pen streak2(argb(C_ACCENT, 140), s * 0.04f);
        streak2.SetStartCap(gp::LineCapRound);
        streak2.SetEndCap(gp::LineCapRound);
        g.DrawLine(&streak2, x + 0.66f * s, y + 0.60f * s, x + 0.92f * s, y + 0.82f * s);
        g.DrawLine(&streak2, x + 0.42f * s, y + 0.80f * s, x + 0.60f * s, y + 0.97f * s);
        drawCursor(g, 1, x, y, s * 0.86f);
        break;
    }
    case 6: { // Ring: a hollow game ring with a dot, easy to see on any background.
        float c = s / 2, r = s * 0.28f;
        gp::Pen dark(argb(0xFF000000, 150), s * 0.12f), ring(argb(0xFFFFFFFF), s * 0.06f);
        g.DrawEllipse(&dark, x + c - r, y + c - r, 2 * r, 2 * r);
        gp::LinearGradientBrush rg(gp::PointF(x, y), gp::PointF(x + s, y + s), argb(0xFFC9B8FF), argb(0xFF5E9BFF));
        gp::Pen glow(&rg, s * 0.06f);
        g.DrawEllipse(&glow, x + c - r, y + c - r, 2 * r, 2 * r);
        gp::SolidBrush dot(argb(0xFFFFFFFF)), dotEdge(argb(0xFF000000, 160));
        float d = s * 0.07f;
        g.FillEllipse(&dotEdge, x + c - d - s * 0.02f, y + c - d - s * 0.02f, 2 * d + s * 0.04f, 2 * d + s * 0.04f);
        g.FillEllipse(&dot, x + c - d, y + c - d, 2 * d, 2 * d);
        break;
    }
    case 7: { // Pixel: a retro 8-bit arrow.
        static const char *rows[] = {"#",        "##",       "#o#",       "#oo#",       "#ooo#",       "#oooo#",
                                     "#ooooo#",  "#oooooo#", "#ooooooo#", "#oooooooo#", "#ooooo#####", "#oo#oo#",
                                     "#o# #oo#", "##  #oo#", "#    #oo#", "     #oo#",  "      ##"};
        float px = s / 18.f;
        gp::SolidBrush ink(argb(0xFF14101F)), fill(argb(0xFFFFFFFF)), shade(argb(0xFFB49CFF));
        for (int r = 0; r < 17; r++) {
            for (int c = 0; rows[r][c]; c++) {
                char ch = rows[r][c];
                if (ch == ' ')
                    continue;
                gp::Brush *b = ch == '#' ? static_cast<gp::Brush *>(&ink) : (c == 1 && r > 1 ? static_cast<gp::Brush *>(&shade) : &fill);
                g.FillRectangle(b, x + s * 0.06f + c * px, y + s * 0.04f + r * px, px + 0.5f, px + 0.5f);
            }
        }
        break;
    }
    case 8: { // Sakura: a soft pink arrow with a little star.
        gp::GraphicsPath p;
        arrowPath(p, x, y, s);
        gp::Pen glow(argb(0xFFFF7AC8, 80), s * 0.14f);
        glow.SetLineJoin(gp::LineJoinRound);
        g.DrawPath(&glow, &p);
        gp::LinearGradientBrush fill(gp::PointF(x, y), gp::PointF(x + 0.6f * s, y + 0.9f * s), argb(0xFFFFC2E6), argb(0xFFB06BFF));
        g.FillPath(&fill, &p);
        gp::Pen rim(argb(0xFFFFFFFF), s * 0.05f);
        rim.SetLineJoin(gp::LineJoinRound);
        g.DrawPath(&rim, &p);
        // Four-point sparkle.
        float cx = x + 0.78f * s, cy = y + 0.22f * s, a = s * 0.16f, b = s * 0.04f;
        gp::PointF star[] = {{cx, cy - a}, {cx + b, cy - b}, {cx + a, cy}, {cx + b, cy + b},
                             {cx, cy + a}, {cx - b, cy + b}, {cx - a, cy}, {cx - b, cy - b}};
        gp::SolidBrush sb(argb(0xFFFFFFFF));
        g.FillPolygon(&sb, star, 8);
        break;
    }
    default: { // Scope: a sniper reticle with mil dots.
        float c = s / 2, r = s * 0.40f;
        gp::Pen dark(argb(0xFF000000, 140), s * 0.07f), thin(argb(0xFFFFFFFF, 235), s * 0.03f);
        for (gp::Pen *pen : {&dark, &thin}) {
            g.DrawEllipse(pen, x + c - r, y + c - r, 2 * r, 2 * r);
            g.DrawLine(pen, x + c - r * 1.08f, y + c, x + c - r * 0.18f, y + c);
            g.DrawLine(pen, x + c + r * 0.18f, y + c, x + c + r * 1.08f, y + c);
            g.DrawLine(pen, x + c, y + c - r * 1.08f, x + c, y + c - r * 0.18f);
            g.DrawLine(pen, x + c, y + c + r * 0.18f, x + c, y + c + r * 1.08f);
        }
        gp::SolidBrush mil(argb(0xFFFFFFFF, 220)), red(argb(0xFFFF3B5C));
        for (int i = 1; i <= 2; i++) {
            float o = r * 0.3f * i, d = s * 0.022f;
            for (auto [dx, dy] : {std::pair{o, 0.f}, std::pair{-o, 0.f}, std::pair{0.f, o}, std::pair{0.f, -o}})
                g.FillEllipse(&mil, x + c + dx - d, y + c + dy - d, 2 * d, 2 * d);
        }
        float d = s * 0.035f;
        g.FillEllipse(&red, x + c - d, y + c - d, 2 * d, 2 * d);
        break;
    }
    }
}

} // namespace

HCURSOR makeCursor(int style, int size) {
    if (style <= 0)
        return nullptr;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void *bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO *>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color)
        return nullptr;
    memset(bits, 0, static_cast<size_t>(size) * size * 4);
    {
        gp::Bitmap canvas(size, size, size * 4, PixelFormat32bppARGB, static_cast<BYTE *>(bits));
        gp::Graphics g(&canvas);
        setQuality(g);
        drawCursor(g, style, 0, 0, static_cast<float>(size));
    }
    std::vector<BYTE> zeros(static_cast<size_t>((size + 15) / 16 * 2) * size, 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, zeros.data());
    ICONINFO ii{};
    ii.fIcon = FALSE;
    bool centred = style == 2 || style == 3 || style == 6 || style >= 9;
    ii.xHotspot = centred ? size / 2 : static_cast<DWORD>(0.10f * size);
    ii.yHotspot = centred ? size / 2 : static_cast<DWORD>(0.06f * size);
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HCURSOR cur = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return cur;
}

void drawCursorPreview(gp::Graphics &g, int style, const gp::RectF &r) {
    float s = std::min(r.Width, r.Height);
    drawCursor(g, style, r.X + (r.Width - s) / 2, r.Y + (r.Height - s) / 2, s);
}

// ---- boot screens

const std::vector<const wchar_t *> &bootStyleNames() {
    static const std::vector<const wchar_t *> names = {L"Orbit", L"Hyperdrive", L"Neon Grid", L"Terminal", L"Minimal"};
    return names;
}

BootScreen::BootScreen(int w, int h, int style) : w_(w), h_(h), style_(std::clamp(style, 0, 4)) {
    float W = static_cast<float>(w), H = static_cast<float>(h);
    back_ = std::make_unique<gp::Bitmap>(w, h, PixelFormat32bppPARGB);
    {
        gp::Graphics g(back_.get());
        setQuality(g);
        Rng rng(5);
        auto stars = [&](int n, float maxY, int lo, int hi) {
            for (int i = 0; i < n; i++) {
                float x = rng.unit() * W, y = rng.unit() * maxY, r = W * rng.range(0.0008f, 0.0022f);
                gp::SolidBrush b(argb(0xFFFFFFFF, static_cast<int>(rng.range(static_cast<float>(lo), static_cast<float>(hi)))));
                g.FillEllipse(&b, x - r, y - r, 2 * r, 2 * r);
            }
        };
        if (style_ == 0) { // Orbit
            verticalGradient(g, W, H, 0xFF06070C, 0xFF0C0E1A);
            stars(70, H, 25, 110);
        } else if (style_ == 1) { // Hyperdrive
            verticalGradient(g, W, H, 0xFF020208, 0xFF070A1C);
            stars(40, H, 20, 80);
        } else if (style_ == 2) { // Neon Grid: sky, sun, hills and the grid's fixed lines
            float horizon = H * 0.58f;
            gp::LinearGradientBrush sky(gp::PointF(0, -1), gp::PointF(0, horizon + 1), argb(0xFF07031A), argb(0xFF34105C));
            g.FillRectangle(&sky, 0.f, 0.f, W, horizon);
            gp::LinearGradientBrush ground(gp::PointF(0, horizon - 1), gp::PointF(0, H + 1), argb(0xFF16062E), argb(0xFF040109));
            g.FillRectangle(&ground, 0.f, horizon, W, H - horizon);
            stars(90, horizon * 0.7f, 40, 200);
            float sunR = W * 0.24f, sunY = horizon - sunR * 0.45f;
            softLayer(g, w, h, 6, [&](gp::Graphics &s) { blob(s, W / 2, sunY, sunR * 2.2f, sunR * 1.8f, 0xFFFF4FA0, 150); });
            gp::GraphicsPath sun;
            sun.AddEllipse(W / 2 - sunR, sunY - sunR, 2 * sunR, 2 * sunR);
            gp::Region region(&sun);
            for (int i = 0; i < 6; i++)
                region.Exclude(gp::RectF(0, sunY + sunR * (0.08f + i * 0.15f), W, sunR * (0.025f + i * 0.014f)));
            region.Exclude(gp::RectF(0, horizon, W, H));
            g.SetClip(&region);
            gp::LinearGradientBrush sb(gp::PointF(0, sunY - sunR), gp::PointF(0, sunY + sunR), argb(0xFFFFE07A), argb(0xFFFF3D9A));
            g.FillRectangle(&sb, 0.f, sunY - sunR, W, 2 * sunR);
            g.ResetClip();
            gp::PointF hills[] = {{0, horizon},
                                  {W * 0.1f, horizon - H * 0.03f},
                                  {W * 0.22f, horizon - H * 0.012f},
                                  {W * 0.34f, horizon - H * 0.045f},
                                  {W * 0.5f, horizon - H * 0.008f},
                                  {W * 0.66f, horizon - H * 0.05f},
                                  {W * 0.8f, horizon - H * 0.018f},
                                  {W * 0.92f, horizon - H * 0.038f},
                                  {W, horizon - H * 0.012f},
                                  {W, horizon}};
            gp::SolidBrush hill(argb(0xFF13062B));
            g.FillPolygon(&hill, hills, static_cast<INT>(std::size(hills)));
            gp::Pen hillEdge(argb(0xFF41E2FF, 160), W * 0.003f);
            g.DrawLines(&hillEdge, hills, static_cast<INT>(std::size(hills)) - 1);
            gp::Pen grid(argb(0xFFFF4FD8, 150), W * 0.003f);
            for (int i = -12; i <= 12; i++)
                g.DrawLine(&grid, W / 2 + i * W * 0.015f, horizon, W / 2 + i * W * 0.17f, H);
        } else if (style_ == 3) { // Terminal: black glass with faint scan lines
            g.Clear(argb(0xFF040506));
            gp::SolidBrush scan(argb(0xFFFFFFFF, 7));
            for (float y = 0; y < H; y += 3)
                g.FillRectangle(&scan, 0.f, y, W, 1.f);
        } else { // Minimal
            g.Clear(argb(0xFF000000));
        }
    }
    int gw = std::max(16, w / 6), gh = std::max(16, h / 6);
    glow_ = std::make_unique<gp::Bitmap>(gw, gh, PixelFormat32bppPARGB);
    {
        gp::Graphics g(glow_.get());
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        g.Clear(gp::Color(0, 0, 0, 0));
        float gW = static_cast<float>(gw), gH = static_cast<float>(gh);
        blob(g, gW * 0.30f, gH * 0.34f, gW * 0.75f, gH * 0.26f, C_ACCENT, 150);
        blob(g, gW * 0.75f, gH * 0.48f, gW * 0.65f, gH * 0.24f, C_ACCENT2, 140);
        blob(g, gW * 0.45f, gH * 0.80f, gW * 0.70f, gH * 0.20f, 0xFF19C9C0, 70);
    }
    family_ = std::make_unique<gp::FontFamily>(L"Segoe UI");
    mono_ = std::make_unique<gp::FontFamily>(L"Consolas");
    if (!mono_->IsAvailable())
        mono_ = std::make_unique<gp::FontFamily>(L"Courier New");
    word_ = std::make_unique<gp::Font>(family_.get(), W * 0.105f, gp::FontStyleBold, gp::UnitPixel);
    small_ = std::make_unique<gp::Font>(family_.get(), W * 0.034f, gp::FontStyleRegular, gp::UnitPixel);
    tiny_ = std::make_unique<gp::Font>(family_.get(), W * 0.028f, gp::FontStyleBold, gp::UnitPixel);
    code_ = std::make_unique<gp::Font>(mono_.get(), W * 0.031f, gp::FontStyleRegular, gp::UnitPixel);
    // Letter widths of the name, measured once.
    gp::Bitmap probe(1, 1, PixelFormat32bppPARGB);
    gp::Graphics pg(&probe);
    pg.SetTextRenderingHint(gp::TextRenderingHintAntiAlias);
    gp::StringFormat fmt(gp::StringFormat::GenericTypographic());
    float spacing = W * 0.05f;
    for (const wchar_t *c = L"VELOX"; *c; c++) {
        gp::RectF box;
        pg.MeasureString(c, 1, word_.get(), gp::PointF(0, 0), &fmt, &box);
        advances_.push_back(box.Width);
        wordWidth_ += box.Width + spacing;
    }
    wordWidth_ -= spacing;
}

// The name in big spaced letters with a light sweep; `chrome` gives it a synthwave chrome look.
void BootScreen::wordmark(gp::Graphics &g, float cx, float ty, float t, bool chrome) {
    float W = static_cast<float>(w_);
    g.SetTextRenderingHint(gp::TextRenderingHintAntiAlias);
    const wchar_t *title = L"VELOX";
    float spacing = W * 0.05f, total = wordWidth_;
    gp::StringFormat fmt(gp::StringFormat::GenericTypographic());
    float tx = cx - total / 2;
    gp::SolidBrush white(argb(0xFFFFFFFF, 240));
    gp::LinearGradientBrush chromeFill(gp::PointF(0, ty), gp::PointF(0, ty + W * 0.13f), argb(0xFFFFFFFF), argb(0xFFFF8BE0));
    float sweepX = cx - total + std::fmod(t * 0.55f, 1.8f) * total * 1.3f;
    gp::LinearGradientBrush shine(gp::RectF(sweepX - total * 1.5f, ty, total * 3.f, W * 0.1f), argb(0xFFFFFFFF, 0), argb(0xFFFFFFFF, 0),
                                  gp::LinearGradientModeHorizontal);
    gp::Color colors[] = {argb(0xFFB49CFF, 0), argb(0xFFB49CFF, 0), argb(0xFFD7CCFF, 255), argb(0xFFB49CFF, 0), argb(0xFFB49CFF, 0)};
    float pos[] = {0.f, 0.44f, 0.5f, 0.56f, 1.f};
    shine.SetInterpolationColors(colors, pos, 5);
    gp::SolidBrush neon(argb(0xFFFF4FD8, 120));
    float x = tx;
    for (int i = 0; title[i]; i++) {
        if (chrome)
            g.DrawString(&title[i], 1, word_.get(), gp::PointF(x + W * 0.006f, ty + W * 0.006f), &fmt, &neon);
        g.DrawString(&title[i], 1, word_.get(), gp::PointF(x, ty), &fmt, chrome ? static_cast<gp::Brush *>(&chromeFill) : &white);
        g.DrawString(&title[i], 1, word_.get(), gp::PointF(x, ty), &fmt, &shine);
        x += advances_[i] + spacing;
    }
}

// Progress bar with the boot stage under it.
void BootScreen::footer(gp::Graphics &g, float progress, const std::wstring &status, float y) {
    float W = static_cast<float>(w_);
    float bw = W * 0.46f, bh = std::max(3.f, W * 0.0085f), bx = W / 2 - bw / 2;
    gp::SolidBrush trackFill(argb(0xFFFFFFFF, 26));
    fillRound(g, gp::RectF(bx, y, bw, bh), bh / 2, trackFill);
    float fw = std::max(bh, bw * std::clamp(progress, 0.f, 1.f));
    gp::LinearGradientBrush fill(gp::RectF(bx, y, bw, bh), argb(C_ACCENT), argb(C_ACCENT2), gp::LinearGradientModeHorizontal);
    fillRound(g, gp::RectF(bx, y, fw, bh), bh / 2, fill);
    blob(g, bx + fw, y + bh / 2, W * 0.05f, W * 0.03f, 0xFFC9B8FF, 150);
    gp::StringFormat centre;
    centre.SetAlignment(gp::StringAlignmentCenter);
    gp::SolidBrush dim(argb(0xFFB4B9C8, 230));
    g.DrawString(status.c_str(), -1, small_.get(), gp::RectF(0, y + bh + W * 0.035f, W, W * 0.06f), &centre, &dim);
}

void BootScreen::draw(gp::Graphics &g, float t, float progress, const std::wstring &status, const std::wstring &subtitle) {
    float W = static_cast<float>(w_), H = static_cast<float>(h_), cx = W / 2;
    g.SetCompositingQuality(gp::CompositingQualityHighSpeed);
    g.SetInterpolationMode(gp::InterpolationModeNearestNeighbor);
    g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
    g.DrawImage(back_.get(), 0, 0, w_, h_);
    g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(gp::TextRenderingHintAntiAlias);
    gp::StringFormat mid;
    mid.SetAlignment(gp::StringAlignmentCenter);
    gp::SolidBrush accentText(argb(0xFFA996FF, 220));
    float pulse = 0.5f + 0.5f * std::sin(t * 2.4f);

    if (style_ == 0) { // ---- Orbit: drifting colour, rising sparks, a spinning ring round the logo
        g.SetInterpolationMode(gp::InterpolationModeBilinear);
        float dx = W * 0.05f * std::sin(t * 0.55f), dy = H * 0.03f * std::cos(t * 0.4f);
        gp::ImageAttributes ia;
        ia.SetWrapMode(gp::WrapModeTileFlipXY);
        g.DrawImage(glow_.get(), gp::RectF(-W * 0.08f + dx, -H * 0.06f + dy, W * 1.16f, H * 1.12f), 0, 0,
                    static_cast<float>(glow_->GetWidth()), static_cast<float>(glow_->GetHeight()), gp::UnitPixel, &ia);
        for (int i = 0; i < 34; i++) {
            float speed = H * (0.03f + 0.06f * hash(i, 1));
            float y = H * 1.05f - std::fmod(t * speed + hash(i, 2) * H * 1.1f, H * 1.1f);
            float x = hash(i, 3) * W + W * 0.02f * std::sin(t * (0.6f + hash(i, 4)) + i);
            float a = 0.5f + 0.5f * std::sin(t * (1.5f + 2 * hash(i, 5)) + i * 1.7f);
            float r = W * (0.0025f + 0.004f * hash(i, 6));
            gp::SolidBrush b(argb(hash(i, 7) > 0.5f ? 0xFFB9A8FF : 0xFF8FB4FF, static_cast<int>(40 + 150 * a)));
            g.FillEllipse(&b, x - r, y - r, 2 * r, 2 * r);
        }
        float cy = H * 0.40f, size = W * 0.24f * (1.f + 0.025f * std::sin(t * 2.4f));
        blob(g, cx, cy, size * 1.25f, size * 1.25f, C_ACCENT, static_cast<int>(70 + 60 * pulse));
        float ringR = W * 0.205f;
        gp::Pen track(argb(0xFFFFFFFF, 22), W * 0.006f);
        g.DrawEllipse(&track, cx - ringR, cy - ringR, 2 * ringR, 2 * ringR);
        gp::LinearGradientBrush arcBrush(gp::RectF(cx - ringR, cy - ringR, 2 * ringR, 2 * ringR), argb(0xFFC9B8FF), argb(C_ACCENT2),
                                         t * 240.f);
        gp::Pen arc(&arcBrush, W * 0.009f);
        arc.SetStartCap(gp::LineCapRound);
        arc.SetEndCap(gp::LineCapRound);
        float start = std::fmod(t * 260.f, 360.f), sweep = 70.f + 50.f * std::sin(t * 1.7f);
        g.DrawArc(&arc, cx - ringR, cy - ringR, 2 * ringR, 2 * ringR, start, sweep);
        g.DrawArc(&arc, cx - ringR, cy - ringR, 2 * ringR, 2 * ringR, start + 180.f, sweep * 0.5f);
        logo(g, cx - size / 2, cy - size / 2, size);
        float ty = H * 0.565f;
        wordmark(g, cx, ty, t, false);
        g.DrawString(subtitle.c_str(), -1, tiny_.get(), gp::RectF(0, ty + W * 0.16f, W, W * 0.05f), &mid, &accentText);
        footer(g, progress, status, H * 0.80f);
    } else if (style_ == 1) { // ---- Hyperdrive: warp streaks rushing out of the logo
        float cy = H * 0.40f, R = std::sqrt(W * W + H * H) * 0.62f;
        for (int k = 0; k < 3; k++) {
            float u = std::fmod(t * 0.45f + k / 3.f, 1.f), rr = W * 0.14f + u * W * 0.75f;
            gp::Pen ring(argb(C_ACCENT, static_cast<int>(110 * (1 - u))), W * 0.004f);
            g.DrawEllipse(&ring, cx - rr, cy - rr, 2 * rr, 2 * rr);
        }
        for (int i = 0; i < 170; i++) {
            float a = hash(i, 1) * 2 * util::PI, sp = 0.35f + 0.9f * hash(i, 2);
            float u = std::fmod(hash(i, 3) + t * sp * 0.42f, 1.f);
            float r0 = R * std::pow(u, 2.2f), r1 = R * std::pow(std::min(1.f, u + 0.03f + 0.12f * u), 2.2f);
            gp::ARGB c = hash(i, 4) > 0.66f ? 0xFFC9B8FF : hash(i, 4) > 0.33f ? 0xFF9CC2FF : 0xFFFFFFFF;
            gp::Pen streak(argb(c, static_cast<int>(30 + 225 * u)), W * (0.0015f + 0.005f * u));
            streak.SetStartCap(gp::LineCapRound);
            streak.SetEndCap(gp::LineCapRound);
            float ca = std::cos(a), sa = std::sin(a);
            g.DrawLine(&streak, cx + ca * r0, cy + sa * r0, cx + ca * r1, cy + sa * r1);
        }
        blob(g, cx, cy, W * 0.38f, W * 0.38f, C_ACCENT2, static_cast<int>(70 + 50 * pulse));
        blob(g, cx, cy, W * 0.20f, W * 0.20f, 0xFFFFFFFF, static_cast<int>(30 + 30 * pulse));
        float zoom = t < 0.7f ? 0.35f + 0.65f * (1 - std::pow(1 - t / 0.7f, 3.f)) : 1.f + 0.02f * std::sin(t * 3.f);
        float size = W * 0.24f * zoom;
        logo(g, cx - size / 2, cy - size / 2, size);
        float ty = H * 0.565f;
        // Dark veils keep the name and status readable over the streaks.
        blob(g, cx, ty + W * 0.1f, W * 0.62f, W * 0.17f, 0xFF000000, 200);
        blob(g, cx, H * 0.80f + W * 0.04f, W * 0.48f, W * 0.08f, 0xFF000000, 200);
        wordmark(g, cx, ty, t, false);
        g.DrawString(subtitle.c_str(), -1, tiny_.get(), gp::RectF(0, ty + W * 0.16f, W, W * 0.05f), &mid, &accentText);
        footer(g, progress, status, H * 0.80f);
    } else if (style_ == 2) { // ---- Neon Grid: the grid floor scrolls towards you under a synthwave sun
        float horizon = H * 0.58f;
        for (int k = 0; k < 16; k++) {
            float f = std::fmod(k / 16.f + t * 0.22f, 1.f), e = std::pow(f, 2.2f), y = horizon + (H - horizon) * e;
            gp::Pen line(argb(0xFFFF4FD8, static_cast<int>(50 + 190 * e)), W * (0.0012f + 0.004f * e));
            g.DrawLine(&line, 0.f, y, W, y);
        }
        blob(g, cx, horizon, W * 0.9f, H * 0.035f, 0xFFFF6BE6, 120);
        float ly = H * 0.17f, size = W * 0.20f * (1.f + 0.02f * std::sin(t * 2.4f));
        blob(g, cx, ly + size / 2, size, size, 0xFFFF4FD8, static_cast<int>(50 + 50 * pulse));
        logo(g, cx - size / 2, ly, size);
        float ty = ly + size + W * 0.05f;
        wordmark(g, cx, ty, t, true);
        gp::SolidBrush cyan(argb(0xFF41E2FF, 230));
        g.DrawString(subtitle.c_str(), -1, tiny_.get(), gp::RectF(0, ty + W * 0.155f, W, W * 0.05f), &mid, &cyan);
        blob(g, cx, H * 0.84f + W * 0.04f, W * 0.48f, W * 0.08f, 0xFF050109, 230);
        footer(g, progress, status, H * 0.84f);
    } else if (style_ == 3) { // ---- Terminal: a boot log typing itself out
        float x0 = W * 0.06f, lineH = code_->GetSize() * 1.5f;
        logo(g, x0, H * 0.05f, W * 0.09f);
        gp::Font head(mono_.get(), W * 0.04f, gp::FontStyleBold, gp::UnitPixel);
        gp::SolidBrush ink(argb(0xFFE6E8F0)), grey(argb(0xFF8A90A0)), ok(argb(0xFF3DFF9A)), violet(argb(0xFFB49CFF));
        g.DrawString(L"VELOX 2.0", -1, &head, gp::PointF(x0 + W * 0.12f, H * 0.05f), &ink);
        g.DrawString(L"android 15 · x86_64 · gaming phone", -1, code_.get(), gp::PointF(x0 + W * 0.12f, H * 0.05f + W * 0.05f), &grey);
        std::vector<std::wstring> lines = {L"Velox boot manager ready",
                                           L"Hypervisor: Windows Hypervisor Platform",
                                           L"Loading kernel 6.6 (x86_64)",
                                           L"Mounting /system and /vendor",
                                           L"Starting init",
                                           L"Starting servicemanager",
                                           L"Starting surfaceflinger",
                                           L"Profile: " + subtitle,
                                           L"Starting zygote64",
                                           L"Preloading classes",
                                           L"Starting system_server",
                                           L"Starting package manager",
                                           L"Starting window manager",
                                           L"Starting input",
                                           L"Bringing up Wi-Fi",
                                           L"Starting Google Play services",
                                           L"Starting launcher"};
        int shown = std::min(static_cast<int>(lines.size()), static_cast<int>(t / 0.3f) + 1);
        int maxRows = static_cast<int>((H * 0.70f) / lineH) - 2, first = std::max(0, shown - maxRows);
        float y = H * 0.14f;
        for (int i = first; i < shown; i++, y += lineH) {
            std::wstring text = lines[i];
            if (i == shown - 1 && shown < static_cast<int>(lines.size())) {
                size_t chars = static_cast<size_t>(std::max(0.f, (t - i * 0.3f) / 0.012f));
                text = text.substr(0, std::min(text.size(), chars));
            }
            g.DrawString(L"[  OK  ]", -1, code_.get(), gp::PointF(x0, y), &ok);
            g.DrawString(text.c_str(), -1, code_.get(), gp::PointF(x0 + code_->GetSize() * 5.2f, y), &ink);
        }
        y += lineH * 0.5f;
        std::wstring live = L"> " + status + (std::fmod(t, 1.f) < 0.55f ? L"_" : L" ");
        g.DrawString(live.c_str(), -1, code_.get(), gp::PointF(x0, y), &violet);
        int cells = 24, filled = static_cast<int>(std::clamp(progress, 0.f, 1.f) * cells + 0.5f);
        std::wstring bar = L"[" + std::wstring(filled, L'#') + std::wstring(cells - filled, L'-') + L"] " +
                           std::to_wstring(static_cast<int>(std::clamp(progress, 0.f, 1.f) * 100 + 0.5f)) + L"%";
        g.DrawString(bar.c_str(), -1, code_.get(), gp::PointF(x0, H * 0.88f), &violet);
    } else { // ---- Minimal: the logo fades in on black, a light sweep, a hairline of progress
        float size = W * 0.26f, ly = H * 0.40f - size / 2;
        logo(g, cx - size / 2, ly, size);
        gp::GraphicsPath tile;
        roundRectPath(tile, gp::RectF(cx - size / 2, ly, size, size), size * 0.24f);
        gp::GraphicsState st = g.Save();
        g.SetClip(&tile, gp::CombineModeIntersect);
        float sx = cx - size + std::fmod(t, 2.6f) / 2.6f * size * 3.f;
        gp::LinearGradientBrush sweep(gp::RectF(sx - size, ly, size * 2, size), argb(0xFFFFFFFF, 0), argb(0xFFFFFFFF, 0), 35.f);
        gp::Color sc[] = {argb(0xFFFFFFFF, 0), argb(0xFFFFFFFF, 0), argb(0xFFFFFFFF, 110), argb(0xFFFFFFFF, 0), argb(0xFFFFFFFF, 0)};
        float sp[] = {0.f, 0.42f, 0.5f, 0.58f, 1.f};
        sweep.SetInterpolationColors(sc, sp, 5);
        g.FillRectangle(&sweep, cx - size / 2, ly, size, size);
        g.Restore(st);
        if (t < 0.9f) {
            gp::SolidBrush fade(argb(0xFF000000, static_cast<int>(255 * (1 - t / 0.9f))));
            g.FillRectangle(&fade, cx - size, ly - size * 0.2f, size * 2, size * 1.4f);
        }
        gp::Font name(family_.get(), W * 0.034f, gp::FontStyleBold, gp::UnitPixel);
        gp::SolidBrush grey(argb(0xFFB0B4C0, 220));
        float nw = 0;
        {
            gp::StringFormat tf(gp::StringFormat::GenericTypographic());
            for (const wchar_t *c = L"VELOX"; *c; c++) {
                gp::RectF box;
                g.MeasureString(c, 1, &name, gp::PointF(0, 0), &tf, &box);
                nw += box.Width + W * 0.025f;
            }
            nw -= W * 0.025f;
            float nx = cx - nw / 2;
            for (const wchar_t *c = L"VELOX"; *c; c++) {
                gp::RectF box;
                g.MeasureString(c, 1, &name, gp::PointF(0, 0), &tf, &box);
                g.DrawString(c, 1, &name, gp::PointF(nx, ly + size + W * 0.08f), &tf, &grey);
                nx += box.Width + W * 0.025f;
            }
        }
        float bw = W * 0.30f, bh = std::max(1.5f, W * 0.003f), bx = cx - bw / 2, by = H * 0.86f;
        gp::SolidBrush track(argb(0xFFFFFFFF, 30)), fill(argb(0xFFFFFFFF, 210));
        g.FillRectangle(&track, bx, by, bw, bh);
        g.FillRectangle(&fill, bx, by, bw * std::clamp(progress, 0.f, 1.f), bh);
        gp::SolidBrush dim(argb(0xFF80848F, 230));
        g.DrawString(status.c_str(), -1, small_.get(), gp::RectF(0, by + W * 0.03f, W, W * 0.06f), &mid, &dim);
    }
}

// ---- device frame

bool writeDeviceFrame(const std::wstring &dir, int w, int h) {
    createDirs(dir);
    float W = static_cast<float>(w);
    int bezel = static_cast<int>(W * 0.034f), body = static_cast<int>(W * 0.012f); // screen border, rim
    int side = static_cast<int>(W * 0.022f);                                       // room for the side buttons
    int fw = w + 2 * (bezel + body) + side, fh = h + 2 * (bezel + body);
    float screenR = W * 0.075f, bodyR = screenR + bezel + body;
    gp::Bitmap frame(fw, fh, PixelFormat32bppARGB);
    {
        gp::Graphics g(&frame);
        setQuality(g);
        g.Clear(gp::Color(0, 0, 0, 0));
        float ox = static_cast<float>(body), oy = static_cast<float>(body);
        gp::RectF outer(1, 1, static_cast<float>(fw - side) - 2, static_cast<float>(fh) - 2);
        // Side buttons (power and volume) on the right edge.
        gp::LinearGradientBrush btn(gp::PointF(outer.GetRight() - 4, 0), gp::PointF(static_cast<float>(fw), 0), argb(0xFF2A2D35),
                                    argb(0xFF15171C));
        float bx = outer.GetRight() - side * 0.5f, bwid = side * 1.2f;
        fillRound(g, gp::RectF(bx, fh * 0.20f, bwid, fh * 0.07f), side * 0.5f, btn);
        fillRound(g, gp::RectF(bx, fh * 0.30f, bwid, fh * 0.13f), side * 0.5f, btn);
        // Body: graphite with a soft metallic rim.
        gp::LinearGradientBrush rim(outer, argb(0xFF4A4E5A), argb(0xFF1A1C22), gp::LinearGradientModeForwardDiagonal);
        fillRound(g, outer, bodyR, rim);
        gp::RectF inner(outer.X + body * 0.7f, outer.Y + body * 0.7f, outer.Width - body * 1.4f, outer.Height - body * 1.4f);
        gp::LinearGradientBrush bodyFill(inner, argb(0xFF17191F), argb(0xFF0B0C10), gp::LinearGradientModeVertical);
        fillRound(g, inner, bodyR - body * 0.7f, bodyFill);
        // The screen area is cut out so the emulator's display shows through.
        gp::GraphicsPath screen;
        roundRectPath(screen, gp::RectF(ox + bezel, oy + bezel, W, static_cast<float>(h)), screenR);
        g.SetCompositingMode(gp::CompositingModeSourceCopy);
        gp::SolidBrush clear(gp::Color(0, 0, 0, 0));
        g.FillPath(&clear, &screen);
    }
    // Mask: opaque outside the rounded screen, which rounds off the display's corners.
    gp::Bitmap mask(w, h, PixelFormat32bppARGB);
    {
        gp::Graphics g(&mask);
        setQuality(g);
        g.Clear(argb(0xFF0B0C10));
        gp::GraphicsPath screen;
        roundRectPath(screen, gp::RectF(0, 0, W, static_cast<float>(h)), screenR);
        g.SetCompositingMode(gp::CompositingModeSourceCopy);
        gp::SolidBrush clear(gp::Color(0, 0, 0, 0));
        g.FillPath(&clear, &screen);
    }
    if (!savePng(frame, dir + L"\\back.png") || !savePng(mask, dir + L"\\mask.png"))
        return false;
    int dx = body + bezel, dy = body + bezel;
    char layout[1024];
    snprintf(layout, sizeof(layout),
             "parts {\n    device {\n        display {\n            width %d\n            height %d\n            x 0\n            y 0\n"
             "        }\n    }\n    portrait {\n        background {\n            image back.png\n        }\n        foreground {\n"
             "            mask mask.png\n        }\n    }\n}\nlayouts {\n    portrait {\n        width %d\n        height %d\n"
             "        event EV_SW:0:1\n        part1 {\n            name portrait\n            x 0\n            y 0\n        }\n"
             "        part2 {\n            name device\n            x %d\n            y %d\n        }\n    }\n}\n",
             w, h, fw, fh, dx, dy);
    std::ofstream o(std::filesystem::path(dir + L"\\layout"), std::ios::binary | std::ios::trunc);
    o << layout;
    return static_cast<bool>(o);
}

} // namespace phone::art
