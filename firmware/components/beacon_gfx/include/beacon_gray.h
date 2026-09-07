#ifndef BEACON_GRAY_H_
#define BEACON_GRAY_H_

#include <stdint.h>

#include "beacon_canvas.h"

namespace beacon {

/* 16-level greyscale surface for the ambient screen. Stored one byte per
 * pixel while drawing (cheap compositing, and PSRAM is plentiful), then
 * packed to the panel's two-pixels-per-byte layout on the way out.
 * 0 = black, 15 = paper, matching the SSD2683 4bpp convention. */
class GrayCanvas {
public:
    static constexpr int kW = kScreenW;
    static constexpr int kH = kScreenH;
    static constexpr uint8_t kBlack = 0;
    static constexpr uint8_t kPaper = 15;

    void Clear(uint8_t level = kPaper);
    void Pixel(int x, int y, uint8_t level);
    uint8_t At(int x, int y) const;
    /* Darkens toward `level` only - used for glows so overlapping halos do
     * not brighten each other back out. */
    void Darken(int x, int y, uint8_t level);

    void FillRect(const Rect& r, uint8_t level);
    void HLine(int x, int y, int len, uint8_t level);
    void VLine(int x, int y, int len, uint8_t level);
    void Line(int x0, int y0, int x1, int y1, uint8_t level);
    void Disc(int cx, int cy, int radius, uint8_t level);
    /* Radial falloff from `core` at the centre to paper at `radius`. */
    void Glow(int cx, int cy, int radius, uint8_t core);
    void Ring(int cx, int cy, int radius, int thickness, uint8_t level);

    int Text(int x, int baseline, const char* utf8, const beacon_font_t& f,
             uint8_t level);
    int TextWidth(const char* utf8, const beacon_font_t& f) const;

    void PackInto(uint8_t* out, size_t cap) const;

private:
    uint8_t px_[kW * kH];
};

}  // namespace beacon

#endif  // BEACON_GRAY_H_
