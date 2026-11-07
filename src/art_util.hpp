// Small drawing helpers shared by the generated artwork (art.cpp).
#pragma once

#include <cmath>

#include "common.hpp"

namespace phone::art::util {

constexpr float PI = 3.14159265f;

inline gp::Color argb(gp::ARGB c, int alpha = -1) {
    gp::Color k(c);
    return alpha < 0 ? k : gp::Color(static_cast<BYTE>(std::clamp(alpha, 0, 255)), k.GetR(), k.GetG(), k.GetB());
}

// Small deterministic random numbers, so every picture looks the same each time.
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 1) {}
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float unit() { return (next() & 0xFFFFFF) / 16777216.f; }
    float range(float a, float b) { return a + (b - a) * unit(); }
};

inline float hash(int i, int salt = 0) {
    uint32_t x = static_cast<uint32_t>(i) * 2654435761u ^ static_cast<uint32_t>(salt) * 40503u;
    x ^= x >> 15;
    x *= 2246822519u;
    x ^= x >> 13;
    return (x & 0xFFFFFF) / 16777216.f;
}

inline gp::ARGB mix(gp::ARGB a, gp::ARGB b, float t) {
    t = std::clamp(t, 0.f, 1.f);
    auto ch = [&](int s) { return static_cast<uint32_t>(((a >> s) & 255) * (1 - t) + ((b >> s) & 255) * t) << s; };
    return 0xFF000000u | ch(16) | ch(8) | ch(0);
}

// A soft round glow that fades to nothing at its edge.
inline void blob(gp::Graphics& g, float cx, float cy, float rx, float ry, gp::ARGB color, int alpha) {
    gp::GraphicsPath p;
    p.AddEllipse(cx - rx, cy - ry, 2 * rx, 2 * ry);
    gp::PathGradientBrush b(&p);
    b.SetCenterColor(argb(color, alpha));
    gp::Color edge = argb(color, 0);
    int n = 1;
    b.SetSurroundColors(&edge, &n);
    b.SetBlendBellShape(1.f, 1.f);
    g.FillPath(&b, &p);
}

inline void verticalGradient(gp::Graphics& g, float w, float h, gp::ARGB top, gp::ARGB bottom) {
    gp::LinearGradientBrush b(gp::PointF(0, -1), gp::PointF(0, h + 1), argb(top), argb(bottom));
    g.FillRectangle(&b, 0.f, 0.f, w, h);
}

inline void setQuality(gp::Graphics& g) {
    g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
    g.SetCompositingQuality(gp::CompositingQualityHighQuality);
    g.SetInterpolationMode(gp::InterpolationModeHighQualityBicubic);
}

// Draws soft glows into a small bitmap and stretches it over the target, which blurs them nicely.
template <typename F>
void softLayer(gp::Graphics& g, int w, int h, int shrink, F&& paint) {
    int sw = std::max(8, w / shrink), sh = std::max(8, h / shrink);
    gp::Bitmap small(sw, sh, PixelFormat32bppPARGB);
    {
        gp::Graphics sg(&small);
        sg.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        sg.Clear(gp::Color(0, 0, 0, 0));
        sg.ScaleTransform(static_cast<float>(sw) / w, static_cast<float>(sh) / h);
        paint(sg);
    }
    gp::ImageAttributes ia;
    ia.SetWrapMode(gp::WrapModeTileFlipXY);
    g.DrawImage(&small, gp::Rect(0, 0, w, h), 0, 0, sw, sh, gp::UnitPixel, &ia);
}

}  // namespace phone::art::util
