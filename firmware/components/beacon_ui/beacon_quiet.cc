/* The quiet screen: what the device looks like when nothing needs you.
 *
 * This used to be the only 16-grey frame in the product. A 4bpp refresh is
 * ~8 s, starts by painting the panel white, and destroys the partial-refresh
 * base — so a clock on it made the whole desk flash every time the minute
 * (or, worse, the 5 s idle tick) moved. The same composition now draws in
 * 1bpp with ordered-dither glows. Entering quiet is one full flash; after
 * that the clock is a small partial, and an unchanged minute costs nothing.
 *
 * The picture is a night sky, not a radar: no range rings, no spokes. Time
 * sits as a poster on the left; the fleet is a scatter of stars on the
 * right, brightness by state, arrangement by a sunflower so it is stable
 * and never a clock face. */

#include <cstdio>
#include <cstring>
#include <new>

#include "beacon_fonts.h"
#include "beacon_gray.h"
#include "beacon_mem.h"
#include "beacon_ui.h"

namespace beacon {
namespace {

constexpr int kHubX = 258;
constexpr int kHubY = 142;

uint32_t Hash(const char* s) {
    uint32_t h = 2166136261u;
    for (; *s; ++s) {
        h ^= static_cast<uint8_t>(*s);
        h *= 16777619u;
    }
    return h;
}

uint32_t HashU(uint32_t n) {
    n ^= n >> 16;
    n *= 0x7feb352du;
    n ^= n >> 15;
    n *= 0x846ca68bu;
    n ^= n >> 16;
    return n;
}

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

int ISqrt(int n) {
    if (n <= 0) return 0;
    int x = n, y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + n / x) / 2;
    }
    return x;
}

struct StarLook {
    int radius;
    int core_ink;   /* 0-255 for 1bpp, mapped to 0-15 for 4bpp */
    int dot;
    int halo;
};

StarLook LookFor(Status s) {
    switch (s) {
        case Status::kBlocked: return {0, 255, 5, 22};
        case Status::kWorking: return {0, 220, 4, 16};
        case Status::kDone:    return {0, 140, 3, 11};
        case Status::kIdle:    return {0, 90,  3, 9};
        default:               return {0, 50,  2, 7};
    }
}

void PlaceStar(int i, int count, uint32_t h, int* x, int* y) {
    /* Golden-angle sunflower, with a few brads of per-id jitter so two
     * sessions never land on the same pixel and the field does not look
     * machine-laid. */
    const int spread = (i * 98) & 255;
    const int jitter = static_cast<int>((h >> 5) % 13) - 6;
    const int angle = (spread + jitter + 18) & 255;
    const int radius = 22 + ISqrt(i * 220 + 16);
    (void)count;
    *x = kHubX + (Cos1024(angle) * radius) / 1024;
    *y = kHubY + (Sin1024(angle) * radius) / 1024;
}

void SummaryLine(const Fleet& f, char* out, int cap) {
    if (f.n_blocked > 0) {
        std::snprintf(out, cap, "%u WAITING ON YOU", f.n_blocked);
    } else if (f.n_working > 0) {
        std::snprintf(out, cap, "%u WORKING  ·  %u QUIET",
                      f.n_working, f.n_idle + f.n_done);
    } else if (f.count > 0) {
        std::snprintf(out, cap, "ALL QUIET  ·  %u", f.count);
    } else {
        std::snprintf(out, cap, "NO SESSIONS");
    }
}

/* ---- 1bpp primitives -------------------------------------------------- */

void StampDisc(Canvas& c, int cx, int cy, int radius, Ink v) {
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            if (dx * dx + dy * dy <= radius * radius) c.Stamp(cx + dx, cy + dy, v);
        }
    }
}

void StampGlow(Canvas& c, int cx, int cy, int radius, Ink core) {
    if (radius <= 0) return;
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            const int r2 = dx * dx + dy * dy;
            if (r2 > radius * radius) continue;
            int d = 0;
            while ((d + 1) * (d + 1) <= r2) ++d;
            const Ink v = static_cast<Ink>((static_cast<int>(core) * (radius - d)) /
                                           radius);
            if (v > 0) c.Stamp(cx + dx, cy + dy, v);
        }
    }
}

void StampRing(Canvas& c, int cx, int cy, int radius, Ink v) {
    for (int a = 0; a < 256; ++a) {
        c.Stamp(cx + (Cos1024(a) * radius) / 1024,
                cy + (Sin1024(a) * radius) / 1024, v);
    }
}

}  // namespace

void Ui::DrawQuiet(Canvas& c, const Fleet& f, const Device& d) {
    c.Clear(ink::kPaper);

    /* Dust: a stable starfield so the panel never looks empty even with
     * one session. Skip the poster region on the left. */
    for (int i = 0; i < 86; ++i) {
        const uint32_t h = HashU(0x9e3779b9u ^ static_cast<uint32_t>(i * 2654435761u));
        const int x = 6 + static_cast<int>(h % 388);
        const int y = 8 + static_cast<int>((h >> 10) % 284);
        if (x < 168 && y < 96) continue;
        if (x < 168 && y > 268) continue;
        c.Stamp(x, y, (h & 3) ? ink::kWhisper : ink::kQuiet);
    }

    /* A soft meridian, hour of day — one brad per ~5.6 minutes, so a minute
     * tick only moves the clock digits, not a wedge of the panel. */
    const int minutes = f.hour * 60 + f.minute;
    const int beam = (minutes * 256) / 1440;
    const int ax = Cos1024(beam), ay = Sin1024(beam);
    constexpr int kHalfWidth = 210;
    constexpr int kReach = 210;
    for (int y = 0; y < kScreenH; ++y) {
        const int dy = y - kHubY;
        for (int x = 160; x < kScreenW; ++x) {
            const int dx = x - kHubX;
            const int along = (dx * ax + dy * ay) / 1024;
            if (along < 16 || along > kReach) continue;
            const int across = (dx * ay - dy * ax) / 1024;
            const int limit = (along * kHalfWidth) / 1024;
            const int off = across < 0 ? -across : across;
            if (off > limit) continue;
            const int edge = limit > 0 ? (off * 100) / limit : 0;
            const int reach = (along * 100) / kReach;
            int fade = (edge * edge) / 80 + (reach * reach) / 90;
            if (along < 56) {
                const int near = ((56 - along) * 100) / 56;
                fade += (near * near) / 80;
            }
            if (fade > 100) fade = 100;
            const int cover = 28 - (28 * fade) / 100;
            if (cover > 4) c.Stamp(x, y, static_cast<Ink>(cover));
        }
    }

    const bool linked = d.link == Link::kOnline;
    StampGlow(c, kHubX, kHubY, 26, linked ? 70 : 36);
    StampDisc(c, kHubX, kHubY, 3, linked ? ink::kSolid : ink::kMid);

    for (uint8_t i = 0; i < f.count; ++i) {
        const Agent& a = f.agents[i];
        const uint32_t h = Hash(a.id);
        const StarLook look = LookFor(a.status);
        int x, y;
        PlaceStar(i, f.count, h, &x, &y);
        StampGlow(c, x, y, look.halo, static_cast<Ink>(look.core_ink / 3));
        StampDisc(c, x, y, look.dot, static_cast<Ink>(look.core_ink));
        if (a.status == Status::kBlocked) {
            StampRing(c, x, y, look.dot + 5, ink::kStrong);
        }
    }

    char clock[8];
    std::snprintf(clock, sizeof(clock), "%02u:%02u", f.hour, f.minute);
    c.Text(18, 50, clock, beacon_font_ui40b, ink::kSolid);
    if (f.date_label[0] != '\0') {
        c.Text(20, 70, f.date_label, beacon_font_label, ink::kSolid);
    }
    c.HLine(18, 80, 96, ink::kWhisper);

    char summary[48];
    SummaryLine(f, summary, sizeof(summary));
    c.Text(18, 284, summary, beacon_font_mono11b, ink::kSolid);

    const int ww = c.TextWidth("BEACON", beacon_font_label);
    c.Text(kScreenW - 18 - ww, 284, "BEACON", beacon_font_label, ink::kSolid);
    c.DottedHLine(18, 270, kScreenW - 36, ink::kMid, 4);
}

void Ui::RenderQuiet4bpp(uint8_t* gray, const Fleet& f, const Device& d) {
    /* Kept as a hardware-acceptance path (`beacon-preview`) so the 16-grey
     * waveform cannot rot. The product quiet screen is DrawQuiet above. */
    static GrayCanvas* canvas = nullptr;
    if (canvas == nullptr) {
        void* memory = BigAlloc(sizeof(GrayCanvas));
        if (memory == nullptr) return;
        canvas = new (memory) GrayCanvas();
    }
    GrayCanvas& g = *canvas;
    g.Clear(GrayCanvas::kPaper);

    for (int i = 0; i < 86; ++i) {
        const uint32_t h = HashU(0x9e3779b9u ^ static_cast<uint32_t>(i * 2654435761u));
        const int x = 6 + static_cast<int>(h % 388);
        const int y = 8 + static_cast<int>((h >> 10) % 284);
        if (x < 168 && y < 96) continue;
        if (x < 168 && y > 268) continue;
        g.Darken(x, y, (h & 3) ? 13 : 11);
    }

    const int minutes = f.hour * 60 + f.minute;
    const int beam = (minutes * 256) / 1440;
    const int ax = Cos1024(beam), ay = Sin1024(beam);
    constexpr int kHalfWidth = 210;
    constexpr int kReach = 210;
    for (int y = 0; y < GrayCanvas::kH; ++y) {
        const int dy = y - kHubY;
        for (int x = 160; x < GrayCanvas::kW; ++x) {
            const int dx = x - kHubX;
            const int along = (dx * ax + dy * ay) / 1024;
            if (along < 16 || along > kReach) continue;
            const int across = (dx * ay - dy * ax) / 1024;
            const int limit = (along * kHalfWidth) / 1024;
            const int off = across < 0 ? -across : across;
            if (off > limit) continue;
            const int edge = limit > 0 ? (off * 100) / limit : 0;
            const int reach = (along * 100) / kReach;
            int fade = (edge * edge) / 80 + (reach * reach) / 90;
            if (along < 56) {
                const int near = ((56 - along) * 100) / 56;
                fade += (near * near) / 80;
            }
            if (fade > 100) fade = 100;
            const uint8_t level = static_cast<uint8_t>(13 + (2 * fade) / 100);
            g.Darken(x, y, level > 15 ? 15 : level);
        }
    }

    const bool linked = d.link == Link::kOnline;
    g.Glow(kHubX, kHubY, 26, linked ? 9 : 13);
    g.Disc(kHubX, kHubY, 3, linked ? 2 : 8);

    for (uint8_t i = 0; i < f.count; ++i) {
        const Agent& a = f.agents[i];
        const uint32_t h = Hash(a.id);
        const StarLook look = LookFor(a.status);
        int x, y;
        PlaceStar(i, f.count, h, &x, &y);
        const uint8_t core = static_cast<uint8_t>(15 - (look.core_ink * 15) / 255);
        g.Glow(x, y, look.halo, static_cast<uint8_t>(core + 4 > 15 ? 15 : core + 4));
        g.Disc(x, y, look.dot, core);
        if (a.status == Status::kBlocked) {
            g.Ring(x, y, look.dot + 5, 1, 3);
        }
    }

    char clock[8];
    std::snprintf(clock, sizeof(clock), "%02u:%02u", f.hour, f.minute);
    g.Text(18, 50, clock, beacon_font_ui40b, 1);
    if (f.date_label[0] != '\0') {
        g.Text(20, 70, f.date_label, beacon_font_label, 6);
    }
    g.HLine(18, 80, 96, 12);

    char summary[48];
    SummaryLine(f, summary, sizeof(summary));
    g.Text(18, 284, summary, beacon_font_mono11b, 3);

    const int ww = g.TextWidth("BEACON", beacon_font_label);
    g.Text(GrayCanvas::kW - 18 - ww, 284, "BEACON", beacon_font_label, 8);
    g.HLine(18, 270, GrayCanvas::kW - 36, 11);

    g.PackInto(gray, static_cast<size_t>(GrayCanvas::kW) * GrayCanvas::kH / 2);
}

}  // namespace beacon
