#ifndef BEACON_FONT_H_
#define BEACON_FONT_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One glyph in a baked face. Bitmaps are row-packed MSB-first into the
 * face's shared blob, stride = (w + 7) / 8, a set bit means ink. */
typedef struct {
    uint32_t offset;   /* byte offset into beacon_font_t::bits */
    uint8_t advance;   /* pen advance in pixels                */
    int8_t left;       /* x bearing from the pen               */
    int8_t top;        /* rows above the baseline              */
    uint8_t width;
    uint8_t height;
} beacon_glyph_t;

typedef struct {
    const char* name;
    uint8_t ascent;
    uint8_t descent;
    uint8_t line_height;
    uint16_t count;
    const uint16_t* codepoints; /* sorted-ish lookup table, parallel to glyphs */
    const beacon_glyph_t* glyphs;
    const uint8_t* bits;
} beacon_font_t;

/* Returns NULL when the face has no glyph for cp. */
const beacon_glyph_t* beacon_font_glyph(const beacon_font_t* font, uint32_t cp);

#ifdef __cplusplus
}
#endif

#endif  /* BEACON_FONT_H_ */
