#include "beacon_canvas.h"

#include <algorithm>
#include <cstring>

namespace beacon {
namespace {

/* Ordered 8x8 Bayer thresholds, scaled to 0..255. A pixel takes ink when the
 * requested coverage exceeds the cell threshold, which gives stable, tileable
 * greys that survive partial refresh without shimmering between frames. */
constexpr uint8_t kBayer8[8][8] = {
    {  0, 128,  32, 160,   8, 136,  40, 168},
    {192,  64, 224,  96, 200,  72, 232, 104},
    { 48, 176,  16, 144,  56, 184,  24, 152},
    {240, 112, 208,  80, 248, 120, 216,  88},
    { 12, 140,  44, 172,   4, 132,  36, 164},
    {204,  76, 236, 108, 196,  68, 228, 100},
    { 60, 188,  28, 156,  52, 180,  20, 148},
    {252, 124, 220,  92, 244, 116, 212,  84},
};

inline bool InkAt(int x, int y, Ink v) {
    if (v == 0) return false;
    if (v == 255) return true;
    return v > kBayer8[y & 7][x & 7];
}

}  // namespace

uint32_t Utf8Next(const char** p) {
    const unsigned char* s = reinterpret_cast<const unsigned char*>(*p);
    if (*s == 0) return 0;
    uint32_t cp;
    int extra;
    if (*s < 0x80) {
        cp = *s;
        extra = 0;
    } else if ((*s & 0xE0) == 0xC0) {
        cp = *s & 0x1F;
        extra = 1;
    } else if ((*s & 0xF0) == 0xE0) {
        cp = *s & 0x0F;
        extra = 2;
    } else if ((*s & 0xF8) == 0xF0) {
        cp = *s & 0x07;
        extra = 3;
    } else {  // Stray continuation byte: skip it rather than desync.
        *p = reinterpret_cast<const char*>(s + 1);
        return '?';
    }
    ++s;
    for (int i = 0; i < extra; ++i) {
        if ((*s & 0xC0) != 0x80) {  // Truncated sequence.
            *p = reinterpret_cast<const char*>(s);
            return '?';
        }
        cp = (cp << 6) | (*s & 0x3F);
        ++s;
    }
    *p = reinterpret_cast<const char*>(s);
    return cp;
}

const beacon_glyph_t* beacon_font_glyph_impl(const beacon_font_t* f,
                                             uint32_t cp) {
    if (f == nullptr) return nullptr;
    // ASCII is the dense common case and sits at the head of the table.
    if (cp >= 0x20 && cp <= 0x7E) {
        const uint16_t index = static_cast<uint16_t>(cp - 0x20);
        if (index < f->count && f->codepoints[index] == cp) {
            return &f->glyphs[index];
        }
    }
    for (uint16_t i = 0; i < f->count; ++i) {
        if (f->codepoints[i] == cp) return &f->glyphs[i];
    }
    return nullptr;
}

Canvas::Canvas() { Clear(); }

void Canvas::Clear(Ink fill) {
    if (fill == ink::kPaper) {
        std::memset(pixels_, 0xFF, sizeof(pixels_));
    } else if (fill == ink::kSolid) {
        std::memset(pixels_, 0x00, sizeof(pixels_));
    } else {
        Rect all{0, 0, kScreenW, kScreenH};
        const Rect saved = clip_;
        clip_ = all;
        FillRect(all, fill);
        clip_ = saved;
    }
    MarkAllDirty();
}

void Canvas::SetClip(const Rect& r) {
    const int x0 = std::max(0, r.x);
    const int y0 = std::max(0, r.y);
    const int x1 = std::min(kScreenW, r.right());
    const int y1 = std::min(kScreenH, r.bottom());
    clip_ = {x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
}

void Canvas::ResetClip() { clip_ = {0, 0, kScreenW, kScreenH}; }

bool Canvas::ClipTest(int x, int y) const {
    return x >= clip_.x && x < clip_.right() && y >= clip_.y &&
           y < clip_.bottom();
}

void Canvas::Touch(int x, int y, int w, int h) {
    const int x0 = std::max(x, clip_.x);
    const int y0 = std::max(y, clip_.y);
    const int x1 = std::min(x + w, clip_.right());
    const int y1 = std::min(y + h, clip_.bottom());
    if (x1 <= x0 || y1 <= y0) return;
    if (!has_dirty_) {
        dirty_ = {x0, y0, x1 - x0, y1 - y0};
        has_dirty_ = true;
        return;
    }
    const int nx = std::min(dirty_.x, x0);
    const int ny = std::min(dirty_.y, y0);
    dirty_ = {nx, ny, std::max(dirty_.right(), x1) - nx,
              std::max(dirty_.bottom(), y1) - ny};
}

void Canvas::ResetDirty() {
    dirty_ = {};
    has_dirty_ = false;
}

void Canvas::MarkAllDirty() {
    dirty_ = {0, 0, kScreenW, kScreenH};
    has_dirty_ = true;
}

void Canvas::Pixel(int x, int y, Ink v) {
    if (!ClipTest(x, y)) return;
    uint8_t& byte = pixels_[static_cast<size_t>(y) * kStride + (x >> 3)];
    const uint8_t mask = static_cast<uint8_t>(0x80u >> (x & 7));
    if (InkAt(x, y, v)) {
        byte &= static_cast<uint8_t>(~mask);
    } else {
        byte |= mask;
    }
    Touch(x, y, 1, 1);
}

void Canvas::FillRect(const Rect& r, Ink v) {
    const int x0 = std::max(r.x, clip_.x);
    const int y0 = std::max(r.y, clip_.y);
    const int x1 = std::min(r.right(), clip_.right());
    const int y1 = std::min(r.bottom(), clip_.bottom());
    if (x1 <= x0 || y1 <= y0) return;

    if (v == ink::kPaper || v == ink::kSolid) {
        // Byte-aligned interior can be memset; only the ragged ends need the
        // per-pixel path.
        const uint8_t fill = (v == ink::kPaper) ? 0xFF : 0x00;
        for (int y = y0; y < y1; ++y) {
            uint8_t* row = &pixels_[static_cast<size_t>(y) * kStride];
            int x = x0;
            while (x < x1 && (x & 7) != 0) {
                const uint8_t m = static_cast<uint8_t>(0x80u >> (x & 7));
                if (fill) row[x >> 3] |= m; else row[x >> 3] &= ~m;
                ++x;
            }
            const int bytes = (x1 - x) >> 3;
            if (bytes > 0) {
                std::memset(&row[x >> 3], fill, static_cast<size_t>(bytes));
                x += bytes * 8;
            }
            while (x < x1) {
                const uint8_t m = static_cast<uint8_t>(0x80u >> (x & 7));
                if (fill) row[x >> 3] |= m; else row[x >> 3] &= ~m;
                ++x;
            }
        }
    } else {
        for (int y = y0; y < y1; ++y) {
            uint8_t* row = &pixels_[static_cast<size_t>(y) * kStride];
            for (int x = x0; x < x1; ++x) {
                const uint8_t m = static_cast<uint8_t>(0x80u >> (x & 7));
                if (InkAt(x, y, v)) {
                    row[x >> 3] &= static_cast<uint8_t>(~m);
                } else {
                    row[x >> 3] |= m;
                }
            }
        }
    }
    Touch(x0, y0, x1 - x0, y1 - y0);
}

void Canvas::StrokeRect(const Rect& r, Ink v, int thickness) {
    for (int i = 0; i < thickness; ++i) {
        const Rect e = r.Inset(i);
        if (e.empty()) return;
        HLine(e.x, e.y, e.w, v);
        HLine(e.x, e.bottom() - 1, e.w, v);
        VLine(e.x, e.y, e.h, v);
        VLine(e.right() - 1, e.y, e.h, v);
    }
}

void Canvas::RoundRect(const Rect& r, int radius, Ink v, bool filled) {
    const int rad = std::min(radius, std::min(r.w, r.h) / 2);
    if (rad <= 0) {
        if (filled) FillRect(r, v); else StrokeRect(r, v);
        return;
    }
    if (filled) {
        FillRect({r.x + rad, r.y, r.w - 2 * rad, r.h}, v);
        FillRect({r.x, r.y + rad, rad, r.h - 2 * rad}, v);
        FillRect({r.right() - rad, r.y + rad, rad, r.h - 2 * rad}, v);
    } else {
        HLine(r.x + rad, r.y, r.w - 2 * rad, v);
        HLine(r.x + rad, r.bottom() - 1, r.w - 2 * rad, v);
        VLine(r.x, r.y + rad, r.h - 2 * rad, v);
        VLine(r.right() - 1, r.y + rad, r.h - 2 * rad, v);
    }
    // Midpoint circle over the four corners.
    int x = rad, y = 0, err = 1 - rad;
    while (x >= y) {
        const int cxl = r.x + rad, cxr = r.right() - 1 - rad;
        const int cyt = r.y + rad, cyb = r.bottom() - 1 - rad;
        if (filled) {
            HLine(cxl - x, cyt - y, (cxr + x) - (cxl - x) + 1, v);
            HLine(cxl - y, cyt - x, (cxr + y) - (cxl - y) + 1, v);
            HLine(cxl - x, cyb + y, (cxr + x) - (cxl - x) + 1, v);
            HLine(cxl - y, cyb + x, (cxr + y) - (cxl - y) + 1, v);
        } else {
            Pixel(cxr + x, cyb + y, v); Pixel(cxr + y, cyb + x, v);
            Pixel(cxl - x, cyb + y, v); Pixel(cxl - y, cyb + x, v);
            Pixel(cxr + x, cyt - y, v); Pixel(cxr + y, cyt - x, v);
            Pixel(cxl - x, cyt - y, v); Pixel(cxl - y, cyt - x, v);
        }
        ++y;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            --x;
            err += 2 * (y - x) + 1;
        }
    }
}

void Canvas::HLine(int x, int y, int len, Ink v) {
    if (len > 0) FillRect({x, y, len, 1}, v);
}

void Canvas::VLine(int x, int y, int len, Ink v) {
    if (len > 0) FillRect({x, y, 1, len}, v);
}

void Canvas::DottedHLine(int x, int y, int len, Ink v, int period) {
    if (period < 1) period = 1;
    for (int i = 0; i < len; i += period) Pixel(x + i, y, v);
}

void Canvas::Line(int x0, int y0, int x1, int y1, Ink v) {
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        Pixel(x0, y0, v);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = err * 2;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void Canvas::Blit(const Sprite& s, int x, int y, Ink v) {
    if (s.bits == nullptr) return;
    const int stride = (s.w + 7) / 8;
    for (int row = 0; row < s.h; ++row) {
        const uint8_t* line = &s.bits[static_cast<size_t>(row) * stride];
        for (int col = 0; col < s.w; ++col) {
            if (line[col >> 3] & (0x80u >> (col & 7))) {
                Pixel(x + col, y + row, v);
            }
        }
    }
}

int Canvas::Text(int x, int baseline, const char* utf8,
                 const beacon_font_t& f, Ink v) {
    if (utf8 == nullptr) return 0;
    const int start = x;
    const char* p = utf8;
    for (uint32_t cp = Utf8Next(&p); cp != 0; cp = Utf8Next(&p)) {
        const beacon_glyph_t* g = beacon_font_glyph_impl(&f, cp);
        if (g == nullptr) g = beacon_font_glyph_impl(&f, '?');
        if (g == nullptr) continue;
        if (g->width != 0 && g->height != 0) {
            const Sprite s{&f.bits[g->offset], g->width, g->height};
            Blit(s, x + g->left, baseline - g->top, v);
        }
        x += g->advance;
    }
    return x - start;
}

int Canvas::TextWidth(const char* utf8, const beacon_font_t& f) const {
    if (utf8 == nullptr) return 0;
    int w = 0;
    const char* p = utf8;
    for (uint32_t cp = Utf8Next(&p); cp != 0; cp = Utf8Next(&p)) {
        const beacon_glyph_t* g = beacon_font_glyph_impl(&f, cp);
        if (g == nullptr) g = beacon_font_glyph_impl(&f, '?');
        if (g != nullptr) w += g->advance;
    }
    return w;
}

int Canvas::TextAligned(const Rect& box, int baseline, const char* utf8,
                        const beacon_font_t& f, Align a, Ink v) {
    const int w = TextWidth(utf8, f);
    int x = box.x;
    if (a == Align::kCenter) x = box.x + (box.w - w) / 2;
    else if (a == Align::kRight) x = box.right() - w;
    return Text(x, baseline, utf8, f, v);
}

int Canvas::TextEllipsized(int x, int baseline, int max_w, const char* utf8,
                           const beacon_font_t& f, Ink v) {
    if (utf8 == nullptr || max_w <= 0) return 0;
    if (TextWidth(utf8, f) <= max_w) {
        return Text(x, baseline, utf8, f, v);
    }
    const beacon_glyph_t* dots = beacon_font_glyph_impl(&f, 0x2026);
    const int dots_w = dots ? dots->advance : TextWidth("...", f);
    const int budget = max_w - dots_w;

    const int start = x;
    const char* p = utf8;
    for (uint32_t cp = Utf8Next(&p); cp != 0; cp = Utf8Next(&p)) {
        const beacon_glyph_t* g = beacon_font_glyph_impl(&f, cp);
        if (g == nullptr) g = beacon_font_glyph_impl(&f, '?');
        if (g == nullptr) continue;
        if (x + g->advance - start > budget) break;
        if (g->width != 0 && g->height != 0) {
            const Sprite s{&f.bits[g->offset], g->width, g->height};
            Blit(s, x + g->left, baseline - g->top, v);
        }
        x += g->advance;
    }
    if (dots != nullptr) {
        if (dots->width != 0) {
            const Sprite s{&f.bits[dots->offset], dots->width, dots->height};
            Blit(s, x + dots->left, baseline - dots->top, v);
        }
        x += dots->advance;
    } else {
        x += Text(x, baseline, "...", f, v);
    }
    return x - start;
}

Rect Canvas::ExtractPatch(const Rect& want, uint8_t* out, size_t out_cap,
                          size_t* out_len) const {
    // The panel addresses partial windows in whole bytes on X.
    int x0 = std::max(0, want.x) & ~7;
    int x1 = std::min(kScreenW, (want.right() + 7) & ~7);
    int y0 = std::max(0, want.y);
    int y1 = std::min(kScreenH, want.bottom());
    if (x1 <= x0 || y1 <= y0) {
        if (out_len) *out_len = 0;
        return {};
    }
    const int w = x1 - x0, h = y1 - y0;
    const size_t patch_stride = static_cast<size_t>(w) / 8;
    const size_t need = patch_stride * static_cast<size_t>(h);
    if (need > out_cap) {
        if (out_len) *out_len = 0;
        return {};
    }
    for (int y = 0; y < h; ++y) {
        std::memcpy(&out[static_cast<size_t>(y) * patch_stride],
                    &pixels_[static_cast<size_t>(y0 + y) * kStride + (x0 / 8)],
                    patch_stride);
    }
    if (out_len) *out_len = need;
    return {x0, y0, w, h};
}

}  // namespace beacon

extern "C" const beacon_glyph_t* beacon_font_glyph(const beacon_font_t* font,
                                                   uint32_t cp) {
    return beacon::beacon_font_glyph_impl(font, cp);
}
