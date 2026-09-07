/* The screens. All layout constants live here as named values rather than
 * magic numbers scattered through the drawing code, because the whole point
 * of the host simulator is that these get nudged a lot.
 *
 * One rule earned the hard way, on the very first simulator render: on a
 * 1bpp panel, *dither is for areas, never for type*. Grey text at 9-12 px
 * dissolves into noise. So every string on every screen is drawn at full
 * ink (or full paper when reversed), and hierarchy is carried entirely by
 * size, weight and space. The Ink scale is reserved for fills and bands. */

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "beacon_fonts.h"
#include "beacon_ui.h"

namespace beacon {
namespace {

constexpr int kHeaderH = 28;
constexpr int kFooterH = 22;
constexpr int kMarginX = 14;
constexpr int kContentR = kScreenW - kMarginX;
constexpr int kSpineX = 30;          /* the vertical bus the fleet hangs off */
constexpr int kRowTextX = 50;
constexpr int kRowH = 34;
constexpr int kRowsBottom = kScreenH - kFooterH - 6;
constexpr int kAttentionH = 76;

/* Greedy word wrap. Returns the number of lines drawn (or measured when
 * canvas is null), never exceeding max_lines; the last line is ellipsized. */
int DrawWrapped(Canvas* c, int x, int first_baseline, int max_w,
                const char* text, const beacon_font_t& f, Ink v,
                int max_lines, int line_step) {
    if (text == nullptr || *text == '\0') return 0;
    int line = 0;
    const char* cursor = text;
    while (*cursor != '\0' && line < max_lines) {
        // Find the longest prefix that fits, breaking on the last space.
        const char* probe = cursor;
        const char* last_break = nullptr;
        int width = 0;
        const char* fit_end = cursor;
        while (*probe != '\0') {
            const char* before = probe;
            const uint32_t cp = Utf8Next(&probe);
            const beacon_glyph_t* g = beacon_font_glyph(&f, cp);
            const int adv = g ? g->advance : 0;
            if (width + adv > max_w) break;
            width += adv;
            fit_end = probe;
            if (cp == ' ') last_break = before;
            (void)before;
        }
        const bool truncated = (*fit_end != '\0');
        const char* end = fit_end;
        if (truncated && last_break != nullptr && line + 1 < max_lines) {
            end = last_break;
        }
        char buf[160];
        size_t n = std::min(sizeof(buf) - 1, static_cast<size_t>(end - cursor));
        std::memcpy(buf, cursor, n);
        buf[n] = '\0';
        if (c != nullptr) {
            const bool last = (line + 1 == max_lines) && truncated;
            if (last) {
                c->TextEllipsized(x, first_baseline + line * line_step, max_w,
                                  cursor, f, v);
            } else {
                c->Text(x, first_baseline + line * line_step, buf, f, v);
            }
        }
        ++line;
        cursor = end;
        while (*cursor == ' ') ++cursor;
        if (end == fit_end && !truncated) break;
    }
    return line;
}

void DrawSectionRule(Canvas& c, int y, const char* label, const char* right) {
    c.Text(kMarginX, y, label, beacon_font_label, ink::kSolid);
    const int lw = c.TextWidth(label, beacon_font_label);
    int rw = 0;
    if (right != nullptr) {
        rw = c.TextWidth(right, beacon_font_label);
        c.Text(kContentR - rw, y, right, beacon_font_label, ink::kSolid);
    }
    const int x0 = kMarginX + lw + 8;
    const int x1 = kContentR - (rw ? rw + 8 : 0);
    if (x1 > x0) c.DottedHLine(x0, y - 4, x1 - x0, ink::kSolid, 3);
}

/* A key/value pair in the detail grid: small mono key over a larger value. */
void DrawStat(Canvas& c, int x, int y, int w, const char* key,
              const char* value, const beacon_font_t& vf) {
    c.Text(x, y, key, beacon_font_label, ink::kSolid);
    c.TextEllipsized(x, y + 17, w, value, vf, ink::kSolid);
}

}  // namespace

void Ui::DrawFleet(Canvas& c, const Fleet& f, const Device& d) {
    c.Clear(ink::kPaper);
    DrawHeader(c, nullptr, f, d);

    int y = kHeaderH + 6;

    /* ---- attention band -------------------------------------------------
     * The pager moment. Exactly one blocked agent is promoted - the one that
     * has waited longest - because two competing alarms is the same as none.
     * The promoted agent is then *removed* from the list below: showing it
     * twice was the first thing that looked wrong in the simulator. The band
     * is itself selectable, so navigation stays uniform. */
    int promoted = -1;
    for (uint8_t i = 0; i < f.count; ++i) {
        if (f.agents[i].status != Status::kBlocked) continue;
        if (promoted < 0 ||
            f.agents[i].status_age_s > f.agents[promoted].status_age_s) {
            promoted = i;
        }
    }

    const int sel = SelectedIndex(f);

    if (promoted >= 0) {
        const Agent& a = f.agents[promoted];
        const bool on = (sel == promoted);
        const Rect band{kMarginX, y, kContentR - kMarginX, kAttentionH};

        /* Selection is shown by border weight and a caret, not by inverting
         * the whole band: a 76 px black slab directly under the black header
         * turned the top half of the screen into one mass, and e-ink pays for
         * every black pixel twice - in refresh time and in ghosting. */
        c.FillRect(band, ink::kPaper);
        c.StrokeRect(band, ink::kSolid, on ? 3 : 1);
        c.FillRect({band.x, band.y, on ? 8 : 5, band.h}, ink::kSolid);

        const char* tag = "NEEDS YOU";
        const int tw = c.TextWidth(tag, beacon_font_label);
        c.FillRect({band.x + 15, band.y + 9, tw + 12, 15}, ink::kSolid);
        c.Text(band.x + 21, band.y + 20, tag, beacon_font_label, ink::kPaper);

        const int lx = band.x + 15 + tw + 12 + 9;
        if (f.n_blocked > 1) {
            char more[16];
            snprintf(more, sizeof(more), "+%u MORE", f.n_blocked - 1);
            c.Text(lx, band.y + 20, more, beacon_font_label, ink::kSolid);
        }

        char age[10];
        FormatDuration(a.status_age_s, age, sizeof(age));
        char waited[24];
        snprintf(waited, sizeof(waited), "WAITING %s", age);
        const int aw = c.TextWidth(waited, beacon_font_mono11b);
        c.Text(band.right() - 14 - aw, band.y + 21, waited,
               beacon_font_mono11b, ink::kSolid);

        const int text_x = band.x + 16;
        const int text_w = band.w - 32;
        c.TextEllipsized(text_x, band.y + 42, text_w, a.title,
                         beacon_font_ui14b, ink::kSolid);
        DrawWrapped(&c, text_x, band.y + 57, text_w,
                    a.question[0] ? a.question : a.activity,
                    beacon_font_ui12, ink::kSolid, 2, 14);

        if (on) {
            // Armed caret inside the left bar, pointing into the band.
            for (int k = 0; k < 5; ++k) {
                c.VLine(band.x + 1 + k, band.y + band.h / 2 - 5 + k,
                        11 - 2 * k, ink::kPaper);
            }
        }

        y += kAttentionH + 8;
    }

    /* ---- fleet list ----------------------------------------------------- */
    char counts[40];
    if (f.count == 0) {
        snprintf(counts, sizeof(counts), "NONE");
    } else {
        snprintf(counts, sizeof(counts), "%u WORK · %u IDLE · %u DONE",
                 f.n_working, f.n_idle, f.n_done);
    }
    char title[24];
    snprintf(title, sizeof(title), "FLEET %u", f.count);
    DrawSectionRule(c, y + 9, title, counts);
    y += 16;

    /* Rows, with the promoted agent skipped. `order` maps visible slots back
     * to fleet indices so scrolling and selection stay honest. */
    uint8_t order[kMaxAgents];
    int n = 0;
    for (uint8_t i = 0; i < f.count; ++i) {
        if (static_cast<int>(i) != promoted) order[n++] = i;
    }

    const int rows_top = y;
    const int capacity = std::max(1, (kRowsBottom - rows_top) / kRowH);

    int sel_slot = -1;
    for (int k = 0; k < n; ++k) {
        if (order[k] == sel) sel_slot = k;
    }
    int first = scroll_;
    if (sel_slot >= 0) {
        if (sel_slot < first) first = sel_slot;
        if (sel_slot >= first + capacity) first = sel_slot - capacity + 1;
    }
    first = std::max(0, std::min(first, std::max(0, n - capacity)));
    scroll_ = static_cast<uint8_t>(first);

    const int shown = std::min(capacity, n - first);

    /* The spine: a dotted hairline the whole list hangs off. Drawn with real
     * pixels rather than a dithered fill, because a 1px dithered line is just
     * noise on this panel. */
    for (int py = rows_top + 4; py < rows_top + shown * kRowH - 6; py += 3) {
        c.Pixel(kSpineX, py, ink::kSolid);
    }

    if (f.count == 0) {
        c.Text(kMarginX, rows_top + 30, "No sessions reporting.",
               beacon_font_ui14b, ink::kSolid);
        c.Text(kMarginX, rows_top + 50,
               d.link == Link::kOnline
                   ? "The hub is up but nothing is running."
                   : "Waiting for the hub.",
               beacon_font_ui12, ink::kSolid);
    }

    for (int k = first; k < n && k - first < capacity; ++k) {
        const Agent& a = f.agents[order[k]];
        const int ry = rows_top + (k - first) * kRowH;
        const bool selected = (order[k] == sel);
        const Rect row{kMarginX, ry, kContentR - kMarginX, kRowH - 3};

        if (selected) {
            c.FillRect(row, ink::kSolid);
        } else if (a.status == Status::kBlocked) {
            // Unpromoted blocked agents still outrank an idle row.
            c.FillRect(row, ink::kWhisper);
            c.FillRect({row.x, row.y, 3, row.h}, ink::kSolid);
        }

        DrawStatusMark(c, kSpineX, ry + 15, a.status, selected);

        const Ink fg = selected ? ink::kPaper : ink::kSolid;

        char age[10];
        FormatDuration(a.status_age_s, age, sizeof(age));
        const int age_w = c.TextWidth(age, beacon_font_mono11b);
        c.Text(kContentR - 8 - age_w, ry + 15, age, beacon_font_mono11b, fg);

        const char* word = StatusWord(a.status);
        const int word_w = c.TextWidth(word, beacon_font_label);
        c.Text(kContentR - 8 - word_w, ry + 28, word, beacon_font_label, fg);

        const int right_col = std::max(age_w, word_w) + 18;
        const int text_w = kContentR - kRowTextX - right_col;

        c.TextEllipsized(kRowTextX, ry + 15, text_w, a.title,
                         beacon_font_ui14b, fg);

        char meta[192];  // machine + project + branch + activity
        if (a.activity[0] != '\0' &&
            (a.status == Status::kWorking || a.status == Status::kBlocked)) {
            snprintf(meta, sizeof(meta), "%s · %s", a.machine, a.activity);
        } else if (a.branch[0] != '\0') {
            snprintf(meta, sizeof(meta), "%s · %s · %s", a.machine, a.project,
                     a.branch);
        } else {
            snprintf(meta, sizeof(meta), "%s · %s", a.machine, a.project);
        }
        c.TextEllipsized(kRowTextX, ry + 28, text_w, meta, beacon_font_ui10,
                         fg);

        if (a.focused) {
            // Solid caret beside the mark: "this is the pane on your screen
            // right now" - the one row you do not need the device for.
            const Ink fg2 = selected ? ink::kPaper : ink::kSolid;
            for (int k = 0; k < 4; ++k) {
                c.VLine(kSpineX - 16 + k, ry + 11 + k, 9 - 2 * k, fg2);
            }
        }
    }

    if (n > first + capacity || first > 0) {
        // Overflow lives on the spine, where the eye already is, rather than
        // as a caption competing with the footer rail.
        const int fy = rows_top + shown * kRowH - 4;
        if (n > first + capacity) {
            for (int r = 0; r < 5; ++r) {
                c.HLine(kSpineX - 4 + r, fy + r, 9 - 2 * r, ink::kSolid);
            }
            char more[16];
            snprintf(more, sizeof(more), "+%d", n - first - capacity);
            c.Text(kSpineX + 10, fy + 8, more, beacon_font_label, ink::kSolid);
        }
        if (first > 0) {
            for (int r = 0; r < 5; ++r) {
                c.HLine(kSpineX - r, rows_top - 9 + r, 1 + 2 * r, ink::kSolid);
            }
        }
    }

    DrawFooter(c, "SELECT", "OPEN", "QUIET");
}

void Ui::DrawAgent(Canvas& c, const Fleet& f, const Device& d) {
    const Agent* a = Selected(f);
    if (a == nullptr) {
        GoTo(Screen::kFleet);
        DrawFleet(c, f, d);
        return;
    }

    c.Clear(ink::kPaper);
    DrawHeader(c, "SESSION", f, d);

    /* Strict top-down flow with an explicit budget. The first version
     * bottom-anchored the action list and let the middle grow into it; on a
     * fixed 300 px panel that is a collision waiting to happen, so instead
     * everything flows down and the *least* important block (the stat row) is
     * the one that gets dropped when the page runs out of room. */
    const int content_w = kContentR - kMarginX;
    int y = kHeaderH + 10;

    // -- status strip -----------------------------------------------------
    DrawStatusMark(c, kMarginX + 7, y + 8, a->status, false);
    c.Text(kMarginX + 22, y + 13, StatusWord(a->status), beacon_font_mono11b,
           ink::kSolid);
    char age[10];
    FormatDuration(a->status_age_s, age, sizeof(age));
    char forline[24];
    snprintf(forline, sizeof(forline), "FOR %s", age);
    const int fw = c.TextWidth(forline, beacon_font_mono11b);
    c.Text(kContentR - fw, y + 13, forline, beacon_font_mono11b, ink::kSolid);
    y += 18;
    c.HLine(kMarginX, y, content_w, ink::kSolid);
    y += 6;

    // -- title (measure first, then place) --------------------------------
    const int title_lines =
        DrawWrapped(nullptr, kMarginX, 0, content_w, a->title,
                    beacon_font_ui18b, ink::kSolid, 2, 21);
    DrawWrapped(&c, kMarginX, y + 15, content_w, a->title, beacon_font_ui18b,
                ink::kSolid, 2, 21);
    y += 15 + (title_lines - 1) * 21 + 18;  // clear the 18 px descenders

    // -- where it lives ---------------------------------------------------
    int branch_w = 0;
    if (a->branch[0] != '\0') {
        branch_w = c.TextWidth(a->branch, beacon_font_label) + 12;
        c.StrokeRect({kContentR - branch_w, y - 11, branch_w, 15},
                     ink::kSolid);
        c.Text(kContentR - branch_w + 6, y, a->branch, beacon_font_label,
               ink::kSolid);
    }
    char where[96];
    snprintf(where, sizeof(where), "%s : %s", a->machine, a->project);
    c.TextEllipsized(kMarginX, y, content_w - branch_w - 10, where,
                     beacon_font_ui12, ink::kSolid);
    y += 14;

    // -- what it is asking / doing ----------------------------------------
    const char* body = a->question[0] ? a->question : a->activity;
    if (body[0] != '\0') {
        const bool asking = a->question[0] != '\0';
        const int inner_w = content_w - 28;
        const int body_lines =
            DrawWrapped(nullptr, 0, 0, inner_w, body, beacon_font_ui12,
                        ink::kSolid, 3, 15);
        const Rect card{kMarginX, y, content_w, 22 + body_lines * 15};
        c.FillRect(card, asking ? ink::kPaper : ink::kWhisper);
        if (asking) {
            c.StrokeRect(card, ink::kSolid);
            c.FillRect({card.x, card.y, 5, card.h}, ink::kSolid);
        }
        c.Text(card.x + 14, card.y + 13, asking ? "ASKING" : "ACTIVITY",
               beacon_font_label, ink::kSolid);
        DrawWrapped(&c, card.x + 14, card.y + 29, inner_w, body,
                    beacon_font_ui12, ink::kSolid, 3, 15);
        y += card.h + 10;
    }

    // -- budget the rest ---------------------------------------------------
    const int action_block =
        a->action_count > 0 ? 16 + a->action_count * 26 : 0;
    const bool have_stats =
        a->tokens > 0 || a->cost_milli > 0 || a->lines_added != 0;
    const int stat_block = 34;
    int room = kRowsBottom - y;

    if (have_stats && room >= action_block + stat_block) {
        char tokens[16], cost[16], diff[24];
        if (a->tokens >= 1000) {
            snprintf(tokens, sizeof(tokens), "%u.%uk",
                     static_cast<unsigned>(a->tokens / 1000),
                     static_cast<unsigned>((a->tokens % 1000) / 100));
        } else {
            snprintf(tokens, sizeof(tokens), "%u",
                     static_cast<unsigned>(a->tokens));
        }
        snprintf(cost, sizeof(cost), "$%u.%02u",
                 static_cast<unsigned>(a->cost_milli / 1000),
                 static_cast<unsigned>((a->cost_milli % 1000) / 10));
        snprintf(diff, sizeof(diff), "+%d/-%d",
                 static_cast<int>(a->lines_added),
                 static_cast<int>(a->lines_removed));
        const int col = content_w / 3;
        DrawStat(c, kMarginX, y + 8, col - 8, "TOKENS", tokens,
                 beacon_font_ui14b);
        DrawStat(c, kMarginX + col, y + 8, col - 8, "COST", cost,
                 beacon_font_ui14b);
        DrawStat(c, kMarginX + col * 2, y + 8, col - 8, "DIFF", diff,
                 beacon_font_ui14b);
        y += stat_block;
        room = kRowsBottom - y;
    }

    // -- actions ----------------------------------------------------------
    if (a->action_count > 0) {
        const int fits = std::max(1, (kRowsBottom - y - 16) / 26);
        char rule_right[32] = {};
        if (fits < a->action_count) {
            snprintf(rule_right, sizeof(rule_right), "%d OF %d SHOWN", fits,
                     static_cast<int>(a->action_count));
        }
        DrawSectionRule(c, y + 9, a->actionable ? "RESPOND" : "VIEW ONLY",
                        rule_right[0] ? rule_right : nullptr);
        y += 16;
        int first = 0;
        if (action_cursor_ >= fits) first = action_cursor_ - fits + 1;
        if (first > a->action_count - fits) first = a->action_count - fits;
        if (first < 0) first = 0;
        for (uint8_t i = first;
             i < a->action_count && i - first < fits; ++i) {
            const Rect r{kMarginX, y + (i - first) * 26, content_w, 24};
            const bool on = (i == action_cursor_);
            if (on) {
                c.FillRect(r, ink::kSolid);
            } else {
                c.StrokeRect(r, ink::kSolid);
            }
            c.Text(r.x + 12, r.y + 17, a->actions[i].label, beacon_font_ui14b,
                   on ? ink::kPaper : ink::kSolid);
            if (on) {
                for (int k = 0; k < 5; ++k) {
                    c.VLine(r.right() - 18 + k, r.y + 7 + k, 11 - 2 * k,
                            ink::kPaper);
                }
            }
        }
        // More actions than fit: mark the edge rather than silently hiding.
    }

    if (toast_ttl_ > 0 && toast_[0] != '\0') {
        const int w = c.TextWidth(toast_, beacon_font_mono11b) + 28;
        const Rect t{(kScreenW - w) / 2, kRowsBottom - 32, w, 28};
        c.FillRect(t.Inset(-3), ink::kPaper);
        c.FillRect(t, ink::kSolid);
        c.TextAligned(t, t.y + 19, toast_, beacon_font_mono11b, Align::kCenter,
                      ink::kPaper);
    }

    DrawFooter(c, a->action_count > 1 ? "ACTION" : nullptr,
               a->action_count > 0 ? "SEND" : nullptr, "BACK");
}

void Ui::DrawSystem(Canvas& c, const Fleet& f, const Device& d) {
    c.Clear(ink::kPaper);
    DrawHeader(c, "SYSTEM", f, d);

    int y = kHeaderH + 20;
    const int col = (kContentR - kMarginX) / 2;

    const char* link = "—";
    switch (d.link) {
        case Link::kBooting: link = "BOOTING"; break;
        case Link::kWifiConnecting: link = "WI-FI…"; break;
        case Link::kHubConnecting: link = "HUB…"; break;
        case Link::kOnline: link = "ONLINE"; break;
        case Link::kDegraded: link = "DEGRADED"; break;
        case Link::kOffline: link = "OFFLINE"; break;
    }

    DrawSectionRule(c, y, "LINK", link);
    y += 18;
    char rssi[16], sync[16], batt[16], up[16], refresh[24];
    snprintf(rssi, sizeof(rssi), "%d dBm", d.rssi);
    FormatDuration(d.last_sync_age_s, sync, sizeof(sync));
    snprintf(batt, sizeof(batt), "%u%%  %u mV", d.battery_percent,
             d.battery_mv);
    FormatDuration(d.uptime_s, up, sizeof(up));
    snprintf(refresh, sizeof(refresh), "%u full / %u part",
             static_cast<unsigned>(d.full_refreshes),
             static_cast<unsigned>(d.partial_refreshes));

    DrawStat(c, kMarginX, y, col - 10, "NETWORK", d.ssid[0] ? d.ssid : "—",
             beacon_font_ui14b);
    DrawStat(c, kMarginX + col, y, col - 10, "SIGNAL", rssi,
             beacon_font_ui14b);
    y += 34;
    DrawStat(c, kMarginX, y, col - 10, "ADDRESS", d.ip[0] ? d.ip : "—",
             beacon_font_ui14b);
    DrawStat(c, kMarginX + col, y, col - 10, "LAST SYNC", sync,
             beacon_font_ui14b);
    y += 34;
    c.Text(kMarginX, y, "HUB", beacon_font_label, ink::kSolid);
    c.TextEllipsized(kMarginX, y + 16, kContentR - kMarginX,
                     d.hub[0] ? d.hub : "—", beacon_font_mono11,
                     ink::kSolid);
    y += 36;

    DrawSectionRule(c, y, "DEVICE", d.firmware);
    y += 18;
    DrawStat(c, kMarginX, y, col - 10, "BATTERY",
             d.battery_valid ? batt : "USB", beacon_font_ui14b);
    DrawStat(c, kMarginX + col, y, col - 10, "UPTIME", up, beacon_font_ui14b);
    y += 34;
    DrawStat(c, kMarginX, y, kContentR - kMarginX, "PANEL REFRESHES", refresh,
             beacon_font_ui14b);

    DrawFooter(c, nullptr, nullptr, "BACK");
}

void Ui::DrawSplash(Canvas& c, const Fleet& f, const Device& d) {
    c.Clear(ink::kPaper);

    // Corner registration ticks - the instrument frame.
    const int t = 10, m = 16;
    for (int i = 0; i < 2; ++i) {
        const int x = i ? kScreenW - m - t : m;
        for (int j = 0; j < 2; ++j) {
            const int yy = j ? kScreenH - m : m;
            c.HLine(x, yy, t, ink::kSolid);
            c.VLine(i ? x + t - 1 : x, j ? yy - t + 1 : yy, t, ink::kSolid);
        }
    }

    const int cx = kScreenW / 2;
    DrawWordmark(c, cx - 52, 132, false);
    // The wordmark at splash scale: redraw the lettering larger beneath it.
    c.FillRect({cx - 60, 146, 120, 2}, ink::kSolid);

    const char* msg = "STARTING";
    switch (d.link) {
        case Link::kWifiConnecting: msg = "JOINING NETWORK"; break;
        case Link::kHubConnecting: msg = "REACHING HUB"; break;
        case Link::kOffline: msg = "NO LINK"; break;
        case Link::kDegraded: msg = "HUB UNREACHABLE"; break;
        default: break;
    }
    c.TextAligned({0, 0, kScreenW, 0}, 176, msg, beacon_font_mono11b,
                  Align::kCenter, ink::kSolid);
    c.TextAligned({0, 0, kScreenW, 0}, 196, "AGENT MISSION CONTROL",
                  beacon_font_label, Align::kCenter, ink::kSolid);

    if (d.link == Link::kOffline || d.link == Link::kDegraded) {
        c.TextAligned({0, 0, kScreenW, 0}, 224,
                      d.ssid[0] ? d.ssid : "no network configured",
                      beacon_font_ui12, Align::kCenter, ink::kSolid);
        DrawFooter(c, nullptr, "RETRY", "SYSTEM");
    }
    (void)f;
}

}  // namespace beacon
