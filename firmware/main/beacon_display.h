#ifndef BEACON_DISPLAY_H_
#define BEACON_DISPLAY_H_

#include "beacon_canvas.h"
#include "beacon_ui.h"
#include "esp_err.h"
#include "zectrix_epd.h"

namespace beacon {

/* Owns the panel and every decision about how to put pixels on it.
 *
 * The interesting part is Present(): the UI always renders a whole frame, and
 * this class works out whether that frame can be pushed as a small partial
 * update or needs a full flash, by diffing against the previous frame. That
 * keeps the screens simple - no per-widget invalidation - while still giving
 * flash-free updates for the common case of one timer ticking over. */
class Display {
public:
    esp_err_t Init();
    void Shutdown();

    /* Pushes `canvas` using the cheapest refresh that will look right.
     * `force_full` overrides the diff, for screen changes and periodic
     * de-ghosting. */
    esp_err_t Present(Canvas& canvas, bool force_full);

    /* 16-grey full frame. Invalidates the partial-refresh base, so the next
     * Present() after this is forced to a full 1bpp refresh. Skips the panel
     * entirely when the packed frame has not changed. */
    esp_err_t PresentGray(const uint8_t* packed);

    esp_err_t Clear();

    uint32_t full_count() const { return full_count_; }
    uint32_t partial_count() const { return partial_count_; }
    int64_t last_refresh_us() const { return last_refresh_us_; }

private:
    bool DiffRect(const Canvas& canvas, Rect* out) const;

    zectrix_epd_handle_t epd_ = nullptr;
    uint8_t* previous_ = nullptr;   /* last 1bpp frame actually on the panel */
    uint8_t* previous_gray_ = nullptr; /* last 4bpp packed frame, if any   */
    uint8_t* patch_ = nullptr;      /* scratch for the partial window   */
    bool have_base_ = false;
    bool have_gray_ = false;
    uint32_t full_count_ = 0;
    uint32_t partial_count_ = 0;
    uint32_t since_full_ = 0;
    int64_t last_refresh_us_ = 0;
};

}  // namespace beacon

#endif  // BEACON_DISPLAY_H_
