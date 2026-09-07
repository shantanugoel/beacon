#ifndef BEACON_CANVAS_H_
#define BEACON_CANVAS_H_

#include <stddef.h>
#include <stdint.h>

#include "beacon_font.h"

namespace beacon {

constexpr int kScreenW = 400;
constexpr int kScreenH = 300;
constexpr int kStride = kScreenW / 8;
constexpr size_t kFrameBytes = static_cast<size_t>(kStride) * kScreenH;

/* Ink is 8-bit coverage: 0 = paper (white), 255 = solid ink (black).
 * Anything between is resolved through an ordered Bayer matrix, which is how
 * a 1bpp panel gets a tonal range without leaving partial-refresh mode. */
using Ink = uint8_t;

namespace ink {
constexpr Ink kPaper = 0;
constexpr Ink kWhisper = 32;   /* 12.5% - hairline fills, disabled states   */
constexpr Ink kQuiet = 64;     /* 25%   - recessed rows, secondary surfaces */
constexpr Ink kMid = 128;      /* 50%   - dividers, meter tracks            */
constexpr Ink kStrong = 192;   /* 75%   - emphasis bands                    */
constexpr Ink kSolid = 255;    /* 100%  - text, rules, alerts               */
}  // namespace ink

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    constexpr int right() const { return x + w; }
    constexpr int bottom() const { return y + h; }
    constexpr bool empty() const { return w <= 0 || h <= 0; }
    Rect Inset(int d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
};

enum class Align : uint8_t { kLeft, kCenter, kRight };

/* A 1bpp sprite: row-packed MSB-first, stride = (w + 7) / 8, 1 = ink. */
struct Sprite {
    const uint8_t* bits;
    uint8_t w;
    uint8_t h;
};

class Canvas {
public:
    Canvas();

    void Clear(Ink fill = ink::kPaper);
    void SetClip(const Rect& r);
    void ResetClip();

    void Pixel(int x, int y, Ink v);
    /* Like Pixel, but never clears ink: overlapping glows and dust can only
     * darken the paper, they cannot punch holes in a star that is already
     * there. */
    void Stamp(int x, int y, Ink v);
    void FillRect(const Rect& r, Ink v);
    void StrokeRect(const Rect& r, Ink v, int thickness = 1);
    void RoundRect(const Rect& r, int radius, Ink v, bool filled);
    void HLine(int x, int y, int len, Ink v);
    void VLine(int x, int y, int len, Ink v);
    void Line(int x0, int y0, int x1, int y1, Ink v);
    /* A 1-in-`period` dotted rule; reads as a lighter divider than 25% ink
     * and, unlike dithered fills, stays crisp under partial refresh. */
    void DottedHLine(int x, int y, int len, Ink v, int period = 3);
    void Blit(const Sprite& s, int x, int y, Ink v);

    /* Text. `baseline` is the glyph baseline, not the box top. Returns the
     * advance width actually drawn. */
    int Text(int x, int baseline, const char* utf8, const beacon_font_t& f,
             Ink v = ink::kSolid);
    int TextAligned(const Rect& box, int baseline, const char* utf8,
                    const beacon_font_t& f, Align a, Ink v = ink::kSolid);
    /* Draws at most `max_w` px, appending an ellipsis when it does not fit. */
    int TextEllipsized(int x, int baseline, int max_w, const char* utf8,
                       const beacon_font_t& f, Ink v = ink::kSolid);
    int TextWidth(const char* utf8, const beacon_font_t& f) const;

    /* Union of everything touched since the last ResetDirty(). Drives which
     * region gets a partial refresh. */
    const Rect& dirty() const { return dirty_; }
    void ResetDirty();
    void MarkAllDirty();

    uint8_t* data() { return pixels_; }
    const uint8_t* data() const { return pixels_; }
    static constexpr size_t size() { return kFrameBytes; }

    /* Copies a sub-rectangle out tightly packed, for partial refresh. The
     * panel wants x and width byte-aligned; the returned rect is the snapped
     * one. */
    Rect ExtractPatch(const Rect& want, uint8_t* out, size_t out_cap,
                      size_t* out_len) const;

private:
    void Touch(int x, int y, int w, int h);
    bool ClipTest(int x, int y) const;

    uint8_t pixels_[kFrameBytes];
    Rect clip_{0, 0, kScreenW, kScreenH};
    Rect dirty_{};
    bool has_dirty_ = false;
};

/* Decodes one UTF-8 scalar, advancing *p. Returns 0 at end of string. */
uint32_t Utf8Next(const char** p);

}  // namespace beacon

#endif  // BEACON_CANVAS_H_
