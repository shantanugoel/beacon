#include "beacon_gray.h"

#include <algorithm>
#include <cstring>

namespace beacon {

void GrayCanvas::Clear(uint8_t level) {
    std::memset(px_, level & 0x0F, sizeof(px_));
}

void GrayCanvas::Pixel(int x, int y, uint8_t level) {
    if (x < 0 || x >= kW || y < 0 || y >= kH) return;
    px_[static_cast<size_t>(y) * kW + x] = level & 0x0F;
}

uint8_t GrayCanvas::At(int x, int y) const {
    if (x < 0 || x >= kW || y < 0 || y >= kH) return kPaper;
    return px_[static_cast<size_t>(y) * kW + x];
}

void GrayCanvas::Darken(int x, int y, uint8_t level) {
    if (x < 0 || x >= kW || y < 0 || y >= kH) return;
    uint8_t& p = px_[static_cast<size_t>(y) * kW + x];
    const uint8_t v = level & 0x0F;
    if (v < p) p = v;
}

void GrayCanvas::FillRect(const Rect& r, uint8_t level) {
    const int x0 = std::max(0, r.x), y0 = std::max(0, r.y);
    const int x1 = std::min(kW, r.right()), y1 = std::min(kH, r.bottom());
    for (int y = y0; y < y1; ++y) {
        std::memset(&px_[static_cast<size_t>(y) * kW + x0], level & 0x0F,
                    static_cast<size_t>(x1 - x0));
    }
}

void GrayCanvas::HLine(int x, int y, int len, uint8_t l) {
    FillRect({x, y, len, 1}, l);
}
void GrayCanvas::VLine(int x, int y, int len, uint8_t l) {
    FillRect({x, y, 1, len}, l);
}

void GrayCanvas::Line(int x0, int y0, int x1, int y1, uint8_t l) {
    const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        Darken(x0, y0, l);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = err * 2;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void GrayCanvas::Disc(int cx, int cy, int radius, uint8_t level) {
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            if (dx * dx + dy * dy <= radius * radius) Pixel(cx + dx, cy + dy, level);
        }
    }
}

void GrayCanvas::Ring(int cx, int cy, int radius, int thickness,
                      uint8_t level) {
    const int inner = radius - thickness;
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            const int r2 = dx * dx + dy * dy;
            if (r2 <= radius * radius && r2 >= inner * inner) {
                Darken(cx + dx, cy + dy, level);
            }
        }
    }
}

void GrayCanvas::Glow(int cx, int cy, int radius, uint8_t core) {
    if (radius <= 0) return;
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            const int r2 = dx * dx + dy * dy;
            if (r2 > radius * radius) continue;
            // Integer distance is fine here; the panel only has 16 steps.
            int d = 0;
            while ((d + 1) * (d + 1) <= r2) ++d;
            const int span = kPaper - core;
            const uint8_t level =
                static_cast<uint8_t>(core + (span * d) / radius);
            Darken(cx + dx, cy + dy, level);
        }
    }
}

int GrayCanvas::Text(int x, int baseline, const char* utf8,
                     const beacon_font_t& f, uint8_t level) {
    if (utf8 == nullptr) return 0;
    const int start = x;
    const char* p = utf8;
    for (uint32_t cp = Utf8Next(&p); cp != 0; cp = Utf8Next(&p)) {
        const beacon_glyph_t* g = beacon_font_glyph(&f, cp);
        if (g == nullptr) continue;
        const int stride = (g->width + 7) / 8;
        for (int row = 0; row < g->height; ++row) {
            const uint8_t* line = &f.bits[g->offset + row * stride];
            for (int col = 0; col < g->width; ++col) {
                if (line[col >> 3] & (0x80u >> (col & 7))) {
                    Pixel(x + g->left + col, baseline - g->top + row, level);
                }
            }
        }
        x += g->advance;
    }
    return x - start;
}

int GrayCanvas::TextWidth(const char* utf8, const beacon_font_t& f) const {
    if (utf8 == nullptr) return 0;
    int w = 0;
    const char* p = utf8;
    for (uint32_t cp = Utf8Next(&p); cp != 0; cp = Utf8Next(&p)) {
        const beacon_glyph_t* g = beacon_font_glyph(&f, cp);
        if (g != nullptr) w += g->advance;
    }
    return w;
}

void GrayCanvas::PackInto(uint8_t* out, size_t cap) const {
    const size_t need = static_cast<size_t>(kW) * kH / 2;
    if (out == nullptr || cap < need) return;
    for (size_t i = 0, o = 0; i < static_cast<size_t>(kW) * kH; i += 2, ++o) {
        out[o] = static_cast<uint8_t>((px_[i] << 4) | px_[i + 1]);
    }
}

}  // namespace beacon
