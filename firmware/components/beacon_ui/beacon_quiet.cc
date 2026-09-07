/* The quiet screen: what the device looks like when nothing needs you.
 *
 * This is the only place the 16-grey mode is spent, because a 4bpp refresh is
 * slow and destroys the partial-refresh base image. That cost buys the one
 * thing 1bpp cannot do - soft light - so the screen is built entirely out of
 * glows: each session is a star, its brightness is its state, and the whole
 * fleet orbits the hub. The beam angle is driven by the clock, so a desk at
 * rest still shows the day moving. */

#include <initializer_list>
#include <new>
#include <cstdio>
#include <cstring>

#include "beacon_fonts.h"
#include "beacon_gray.h"
#include "beacon_mem.h"
#include "beacon_ui.h"

namespace beacon {
namespace {

constexpr int kHubX = 262;
constexpr int kHubY = 116;

/* Deterministic per-agent jitter so the constellation is stable between
 * refreshes but never looks like a clock face. */
uint32_t Hash(const char* s) {
    uint32_t h = 2166136261u;
    for (; *s; ++s) {
        h ^= static_cast<uint8_t>(*s);
        h *= 16777619u;
    }
    return h;
}

/* Fixed-point sin/cos over 256 brads, x1024. */
const int16_t kSin[64] = {
    0, 25, 50, 75, 100, 125, 150, 175, 199, 224, 248, 272, 296, 320, 343, 366,
    389, 412, 434, 456, 477, 499, 519, 540, 560, 579, 598, 616, 634, 651, 668,
    684, 700, 715, 730, 744, 757, 770, 782, 793, 804, 814, 824, 833, 841, 848,
    855, 861, 866, 871, 875, 878, 881, 883, 884, 885, 885, 884, 883, 881, 878,
    875, 871, 866,
};

int Sin1024(int a) {
    a &= 255;
    if (a < 64) return kSin[a];
    if (a < 128) return kSin[127 - a >= 0 ? 127 - a : 0];
    if (a < 192) return -kSin[a - 128];
    return -kSin[255 - a];
}
int Cos1024(int a) { return Sin1024(a + 64); }

}  // namespace

void Ui::RenderQuiet4bpp(uint8_t* gray, const Fleet& f, const Device& d) {
    /* 120 KB: far too big for a task stack, and too big for internal RAM on
     * this part. Allocated once from PSRAM - see beacon_mem.h for why this is
     * a runtime allocation rather than a placed static. */
    static GrayCanvas* canvas = nullptr;
    if (canvas == nullptr) {
        void* memory = BigAlloc(sizeof(GrayCanvas));
        if (memory == nullptr) return;
        canvas = new (memory) GrayCanvas();
    }
    GrayCanvas& g = *canvas;
    g.Clear(GrayCanvas::kPaper);

    /* ---- the beam --------------------------------------------------------
     * A soft cone sweeping from the hub, its bearing set by the time of day,
     * so a desk at rest still shows the day moving. Drawn per-pixel rather
     * than by stepping angles: stepping angles produced a fan of separating
     * rays as radius grew, which read as moire rather than light. Testing
     * every pixel of the panel costs ~120k integer ops, which is nothing next
     * to the 4bpp refresh that follows. */
    const int minutes = f.hour * 60 + f.minute;
    const int beam = (minutes * 256) / 1440;
    const int ax = Cos1024(beam), ay = Sin1024(beam);
    constexpr int kHalfWidth = 260;  /* sin(half-angle) x 1024 ~ 15 degrees  */
    constexpr int kReach = 230;

    for (int y = 0; y < GrayCanvas::kH; ++y) {
        const int dy = y - kHubY;
        for (int x = 0; x < GrayCanvas::kW; ++x) {
            const int dx = x - kHubX;
            const int along = (dx * ax + dy * ay) / 1024;   /* projection   */
            if (along < 22 || along > kReach) continue;
            const int across = (dx * ay - dy * ax) / 1024;  /* perpendicular */
            const int limit = (along * kHalfWidth) / 1024;
            const int off = across < 0 ? -across : across;
            if (off > limit) continue;
            /* Two falloffs: away from the axis, and away from the hub. */
            const int edge = limit > 0 ? (off * 100) / limit : 0;      /* 0..100 */
            const int reach = (along * 100) / kReach;                  /* 0..100 */
            /* Quadratic on both terms so the beam is a suggestion of light
             * rather than a searchlight: it has to sit behind the fleet, not
             * compete with it. */
            int fade = (edge * edge) / 100 + (reach * reach) / 100;
            /* Fade back in close to the hub too, so the cone emerges from the
             * glow instead of starting at a hard triangular point. */
            if (along < 70) {
                const int near = ((70 - along) * 100) / 70;
                fade += (near * near) / 100;
            }
            if (fade > 100) fade = 100;
            const uint8_t level = static_cast<uint8_t>(13 + (2 * fade) / 100);
            g.Darken(x, y, level > 15 ? 15 : level);
        }
    }

    /* ---- orbit rings ---------------------------------------------------- */
    for (int r : {46, 78, 106}) g.Ring(kHubX, kHubY, r, 1, 13);

    /* ---- hub ------------------------------------------------------------- */
    const bool linked = d.link == Link::kOnline;
    g.Glow(kHubX, kHubY, 20, linked ? 9 : 13);
    g.Disc(kHubX, kHubY, 5, linked ? 2 : 8);
    g.Ring(kHubX, kHubY, 10, 1, linked ? 6 : 12);

    /* ---- one star per session -------------------------------------------
     * Ring by state (urgent inward), angle by id hash so the arrangement is
     * memorable but not sorted. */
    for (uint8_t i = 0; i < f.count; ++i) {
        const Agent& a = f.agents[i];
        const uint32_t h = Hash(a.id);
        int radius, core, dot, halo;
        switch (a.status) {
            case Status::kBlocked: radius = 46;  core = 0; dot = 6; halo = 26; break;
            case Status::kWorking: radius = 78;  core = 3; dot = 4; halo = 18; break;
            case Status::kDone:    radius = 78;  core = 6; dot = 3; halo = 11; break;
            case Status::kIdle:    radius = 106; core = 8; dot = 3; halo = 11; break;
            default:               radius = 106; core = 11; dot = 2; halo = 8; break;
        }
        /* Even spread by position with a per-id jitter: pure hashing let two
         * sessions on the same ring land on top of each other, which made a
         * calm fleet look like a broken one. */
        const int spread = f.count > 0 ? (i * 256) / f.count : 0;
        const int jitter = static_cast<int>((h >> 7) % 21) - 10;
        const int angle = (spread + jitter + 200) & 255;
        const int x = kHubX + (Cos1024(angle) * radius) / 1024;
        const int y = kHubY + (Sin1024(angle) * radius) / 1024;

        g.Line(kHubX, kHubY, x, y, a.status == Status::kBlocked ? 9 : 13);
        if (halo > 0) g.Glow(x, y, halo, static_cast<uint8_t>(core + 4));
        g.Disc(x, y, dot, static_cast<uint8_t>(core));
        if (a.status == Status::kBlocked) {
            g.Ring(x, y, dot + 5, 1, 3);
            g.Ring(x, y, dot + 10, 1, 8);
        }
    }

    /* ---- the readout -----------------------------------------------------
     * Kept to the lower-left quadrant so the constellation stays uncluttered,
     * and set large enough to read from across a room. */
    char clock[8];
    std::snprintf(clock, sizeof(clock), "%02u:%02u", f.hour, f.minute);
    g.Text(22, 250, clock, beacon_font_ui40b, 1);

    if (f.date_label[0] != '\0') {
        g.Text(24, 268, f.date_label, beacon_font_label, 6);
    }

    char summary[48];
    if (f.n_blocked > 0) {
        std::snprintf(summary, sizeof(summary), "%u WAITING ON YOU",
                      f.n_blocked);
    } else if (f.n_working > 0) {
        std::snprintf(summary, sizeof(summary), "%u WORKING · %u IDLE",
                      f.n_working, f.n_idle);
    } else if (f.count > 0) {
        std::snprintf(summary, sizeof(summary), "ALL QUIET · %u SESSIONS",
                      f.count);
    } else {
        std::snprintf(summary, sizeof(summary), "NO SESSIONS");
    }
    g.Text(24, 200, summary, beacon_font_mono11b, 3);
    g.HLine(24, 210, 150, 10);

    // Wordmark, small, bottom-right: this is a product, not a screensaver.
    const int ww = g.TextWidth("BEACON", beacon_font_label);
    g.Text(GrayCanvas::kW - 24 - ww, 272, "BEACON", beacon_font_label, 8);

    g.PackInto(gray, static_cast<size_t>(GrayCanvas::kW) * GrayCanvas::kH / 2);
}

}  // namespace beacon
