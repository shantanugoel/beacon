/* Shared chrome: the header band, the footer hint rail, and the small
 * instrument glyphs. Everything here is drawn geometrically rather than
 * blitted from sprite sheets so it stays crisp at any ink level and can be
 * reversed out over the black header without a second asset. */

#include <cstdio>
#include <cstring>

#include "beacon_fonts.h"
#include "beacon_ui.h"

namespace beacon {

const char* StatusWord(Status s) {
    switch (s) {
        case Status::kIdle: return "IDLE";
        case Status::kWorking: return "WORKING";
        case Status::kBlocked: return "BLOCKED";
        case Status::kDone: return "DONE";
        case Status::kStale: return "STALE";
        default: return "—";
    }
}

/* Resolution deliberately matches how often the panel actually redraws.
 *
 * The measured cost of a partial refresh on this panel is ~760 ms, so the
 * device updates roughly once a minute, not once a second. Printing "4m12s"
 * would render a seconds field that is stale almost all the time - precision
 * the display cannot honour. Above a minute the value is therefore shown to
 * the minute; below a minute, where a fresh block genuinely does want
 * seconds, it is shown exactly. */
void FormatDuration(uint32_t seconds, char* out, int cap) {
    if (cap <= 0) return;
    if (seconds < 60) {
        snprintf(out, cap, "%us", static_cast<unsigned>(seconds));
    } else if (seconds < 3600) {
        snprintf(out, cap, "%um", static_cast<unsigned>(seconds / 60));
    } else if (seconds < 86400) {
        snprintf(out, cap, "%uh%02um", static_cast<unsigned>(seconds / 3600),
                 static_cast<unsigned>((seconds % 3600) / 60));
    } else {
        snprintf(out, cap, "%ud%02uh", static_cast<unsigned>(seconds / 86400),
                 static_cast<unsigned>((seconds % 86400) / 3600));
    }
}

/* The mark: a solid lamp with two beam arcs sweeping right. Reads as a
 * lighthouse at 18px and as a broadcast dot at a glance. */
void DrawWordmark(Canvas& c, int x, int baseline, bool on_dark) {
    const Ink fg = on_dark ? ink::kPaper : ink::kSolid;
    const int cy = baseline - 5;

    c.FillRect({x, cy - 2, 5, 5}, fg);
    // Two arcs, drawn as quarter circles opening to the right.
    for (int r = 5; r <= 8; r += 3) {
        for (int dy = -r; dy <= r; ++dy) {
            const int dx2 = r * r - dy * dy;
            if (dx2 < 0) continue;
            int dx = 0;
            while ((dx + 1) * (dx + 1) <= dx2) ++dx;
            if (dx * 3 < r * 2) continue;  // keep only the right-hand sweep
            c.Pixel(x + 2 + dx, cy + dy, fg);
        }
    }
    c.Text(x + 15, baseline, "BEACON", beacon_font_ui14b, fg);
}

void DrawSignal(Canvas& c, int x, int y, const Device& d, bool on_dark) {
    const Ink fg = on_dark ? ink::kPaper : ink::kSolid;
    const Ink dim = on_dark ? ink::kQuiet : ink::kWhisper;
    // Four bars; RSSI buckets chosen so a usable link shows at least two.
    int bars = 0;
    if (d.link == Link::kOnline || d.link == Link::kDegraded) {
        if (d.rssi > -55) bars = 4;
        else if (d.rssi > -67) bars = 3;
        else if (d.rssi > -78) bars = 2;
        else bars = 1;
    }
    for (int i = 0; i < 4; ++i) {
        const int h = 3 + i * 2;
        c.FillRect({x + i * 4, y + 9 - h, 3, h}, i < bars ? fg : dim);
    }
}

void DrawBattery(Canvas& c, int x, int y, const Device& d, bool on_dark) {
    const Ink fg = on_dark ? ink::kPaper : ink::kSolid;
    const Ink dim = on_dark ? ink::kQuiet : ink::kWhisper;
    const Rect body{x, y, 22, 11};
    c.StrokeRect(body, fg);
    c.FillRect({x + 22, y + 3, 2, 5}, fg);
    const int inner = 18;
    const int fill = d.battery_valid ? (inner * d.battery_percent) / 100 : 0;
    c.FillRect({x + 2, y + 2, inner, 7}, dim);
    if (fill > 0) c.FillRect({x + 2, y + 2, fill, 7}, fg);
    if (d.charging) {
        // A bolt notched out of the fill so it reads at either charge level.
        const int bx = x + 9, by = y + 1;
        for (int i = 0; i < 5; ++i) c.Pixel(bx + 2 - i / 2, by + i, on_dark ? ink::kSolid : ink::kPaper);
        for (int i = 0; i < 5; ++i) c.Pixel(bx + 3 - i / 2, by + 4 + i, on_dark ? ink::kSolid : ink::kPaper);
    }
}

/* The status mark is the single most-read element on the screen, so each
 * state gets a distinct silhouette rather than a distinct shade: a filled
 * disc reads the same as a ring from two metres away, but a triangle never
 * reads as a circle. */
void DrawStatusMark(Canvas& c, int cx, int cy, Status s, bool on_dark) {
    const Ink fg = on_dark ? ink::kPaper : ink::kSolid;
    switch (s) {
        case Status::kWorking: {
            // Filled disc with a detached outer ring: "energy radiating".
            for (int dy = -4; dy <= 4; ++dy)
                for (int dx = -4; dx <= 4; ++dx)
                    if (dx * dx + dy * dy <= 17) c.Pixel(cx + dx, cy + dy, fg);
            for (int dy = -7; dy <= 7; ++dy)
                for (int dx = -7; dx <= 7; ++dx) {
                    const int r2 = dx * dx + dy * dy;
                    if (r2 <= 50 && r2 >= 36) c.Pixel(cx + dx, cy + dy, fg);
                }
            break;
        }
        case Status::kBlocked: {
            // Upward triangle - the only pointed silhouette in the set.
            for (int row = 0; row < 12; ++row) {
                const int half = (row * 6) / 11;
                c.HLine(cx - half, cy - 6 + row, half * 2 + 1, fg);
            }
            break;
        }
        case Status::kDone: {
            // Check mark, drawn with 2px strokes so it survives dithering.
            for (int i = 0; i < 4; ++i) {
                c.FillRect({cx - 6 + i, cy - 1 + i, 2, 2}, fg);
            }
            for (int i = 0; i < 7; ++i) {
                c.FillRect({cx - 2 + i, cy + 3 - i, 2, 2}, fg);
            }
            break;
        }
        case Status::kIdle: {
            // Hollow ring: present, but nothing happening.
            for (int dy = -5; dy <= 5; ++dy)
                for (int dx = -5; dx <= 5; ++dx) {
                    const int r2 = dx * dx + dy * dy;
                    if (r2 <= 27 && r2 >= 12) c.Pixel(cx + dx, cy + dy, fg);
                }
            break;
        }
        case Status::kStale: {
            // Dashed ring - the shape is there but the signal is not.
            for (int i = 0; i < 12; ++i) {
                if (i % 2) continue;
                static const int kSin[12] = {0, 2, 4, 5, 4, 2, 0, -2, -4, -5, -4, -2};
                const int dx = kSin[(i + 3) % 12];
                const int dy = kSin[i];
                c.FillRect({cx + dx, cy + dy, 2, 2}, fg);
            }
            break;
        }
        default:
            c.FillRect({cx - 4, cy - 1, 9, 2}, fg);
            break;
    }
}

void DrawHeader(Canvas& c, const char* title, const Fleet& f,
                const Device& d) {
    c.FillRect({0, 0, kScreenW, 28}, ink::kSolid);

    if (title == nullptr) {
        DrawWordmark(c, 12, 19, true);
    } else {
        DrawWordmark(c, 12, 19, true);
        // Breadcrumb: a hairline pip, then the screen name in mono.
        const int bx = 12 + 15 + c.TextWidth("BEACON", beacon_font_ui14b) + 9;
        c.FillRect({bx, 12, 2, 2}, ink::kPaper);
        c.Text(bx + 7, 18, title, beacon_font_label, ink::kPaper);
    }

    /* The date already has a prominent home in quiet mode. Keeping it out of
     * the working chrome leaves the right rail as a quick instrument read:
     * time, radio, battery. */
    int x = kScreenW - 12;
    x -= 24;
    DrawBattery(c, x, 9, d, true);
    x -= 8;
    x -= 15;
    DrawSignal(c, x, 9, d, true);

    char clock[8];
    snprintf(clock, sizeof(clock), "%02u:%02u", d.link == Link::kOffline ? 0 : f.hour,
             d.link == Link::kOffline ? 0 : f.minute);
    x -= 12;
    const int cw = c.TextWidth(clock, beacon_font_mono11b);
    c.Text(x - cw, 19, clock, beacon_font_mono11b, ink::kPaper);
    x -= cw + 10;
    (void)f;
}

/* The footer is a hint rail, not a status bar: it always says what the three
 * buttons do here, because there is no other affordance on the device. */
void DrawFooter(Canvas& c, const char* left, const char* mid,
                const char* right) {
    const int top = kScreenH - 22;
    c.DottedHLine(0, top, kScreenW, ink::kSolid, 3);

    const int base = kScreenH - 7;
    /* Three fixed zones mirror the three physical interactions. Labels stay
     * put as screens change, so the rail reads as hardware rather than prose. */
    int x = 12;
    if (left != nullptr) {
        // Up/down chevrons drawn as glyphs the font does not carry.
        for (int r = 0; r < 4; ++r) c.HLine(x + 3 - r, base - 9 + r, r * 2 + 1, ink::kSolid);
        for (int r = 0; r < 4; ++r) c.HLine(x + 3 - (3 - r), base - 4 + r, (3 - r) * 2 + 1, ink::kSolid);
        x += 12;
        c.Text(x, base, left, beacon_font_label, ink::kSolid);
    }
    if (mid != nullptr) {
        const int w = c.TextWidth(mid, beacon_font_label);
        const int tx = kScreenW / 2 - (w + 11) / 2 + 5;
        c.FillRect({tx - 11, base - 7, 6, 6}, ink::kSolid);
        c.Text(tx, base, mid, beacon_font_label, ink::kSolid);
    }
    if (right != nullptr) {
        const int w = c.TextWidth(right, beacon_font_label);
        c.FillRect({kScreenW - 12 - w - 11, base - 7, 6, 6}, ink::kSolid);
        c.StrokeRect({kScreenW - 12 - w - 14, base - 10, 12, 12}, ink::kSolid);
        c.Text(kScreenW - 12 - w, base, right, beacon_font_label, ink::kSolid);
    }
}

}  // namespace beacon
