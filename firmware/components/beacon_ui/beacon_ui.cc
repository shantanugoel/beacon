#include "beacon_ui.h"

#include <cstdio>
#include <cstring>

namespace beacon {

void Ui::Reset() {
    screen_ = Screen::kSplash;
    selected_id_[0] = '\0';
    action_cursor_ = 0;
    scroll_ = 0;
    force_full_ = true;
    ClearPending();
}

void Ui::GoTo(Screen s) {
    if (screen_ != s) force_full_ = true;
    screen_ = s;
    action_cursor_ = 0;
}

int Ui::SelectedIndex(const Fleet& f) const {
    if (f.count == 0) return -1;
    if (selected_id_[0] != '\0') {
        for (uint8_t i = 0; i < f.count; ++i) {
            if (std::strcmp(f.agents[i].id, selected_id_) == 0) return i;
        }
    }
    return 0;
}

const Agent* Ui::Selected(const Fleet& f) const {
    const int i = SelectedIndex(f);
    return i < 0 ? nullptr : &f.agents[i];
}

void Ui::SelectIndex(const Fleet& f, int index) {
    if (f.count == 0) return;
    if (index < 0) index = f.count - 1;
    if (index >= f.count) index = 0;
    std::snprintf(selected_id_, sizeof(selected_id_), "%s", f.agents[index].id);
}

void Ui::OnFleetUpdated(const Fleet& f) {
    /* An agent becoming blocked is the one event allowed to interrupt: it is
     * the difference between an ambient display and a pager. Recovering from
     * blocked does not re-arm until the count actually drops, so a fleet that
     * hovers at one blocked agent chirps once. */
    if (f.n_blocked > last_blocked_) attention_edge_ = true;
    last_blocked_ = f.n_blocked;

    // Keep the cursor pointing at something real.
    if (SelectedIndex(f) < 0 || selected_id_[0] == '\0') {
        SelectIndex(f, 0);
    } else {
        bool found = false;
        for (uint8_t i = 0; i < f.count; ++i) {
            if (std::strcmp(f.agents[i].id, selected_id_) == 0) found = true;
        }
        if (!found) SelectIndex(f, 0);
    }
    force_full_ = true;
}

bool Ui::TakeAttentionEdge() {
    const bool e = attention_edge_;
    attention_edge_ = false;
    return e;
}

void Ui::set_toast(const char* text, uint32_t ttl_ticks) {
    std::snprintf(toast_, sizeof(toast_), "%s", text ? text : "");
    toast_ttl_ = ttl_ticks;
}

void Ui::tick_toast() {
    if (toast_ttl_ > 0) --toast_ttl_;
}

void Ui::ClearPending() {
    pending_agent_id_[0] = '\0';
    pending_action_id_[0] = '\0';
}

Intent Ui::OnInput(Button b, Press p, const Fleet& f) {
    switch (screen_) {
        case Screen::kSplash:
            if (b == Button::kOk && p == Press::kHold) {
                GoTo(Screen::kSystem);
                return Intent::kRedrawFull;
            }
            return Intent::kNone;

        case Screen::kQuiet:
            // Any touch wakes the desk object back into an instrument.
            GoTo(Screen::kFleet);
            return Intent::kRedrawFull;

        case Screen::kSystem:
            if (b == Button::kOk) {
                GoTo(Screen::kFleet);
                return Intent::kRedrawFull;
            }
            return Intent::kNone;

        case Screen::kFleet: {
            if (b == Button::kOk && p == Press::kHold) {
                GoTo(Screen::kQuiet);
                return Intent::kRedrawFull;
            }
            if (b == Button::kOk) {
                if (f.count == 0) {
                    GoTo(Screen::kSystem);
                    return Intent::kRedrawFull;
                }
                GoTo(Screen::kAgent);
                return Intent::kRedrawFull;
            }
            if (f.count == 0) return Intent::kNone;
            const int i = SelectedIndex(f);
            SelectIndex(f, b == Button::kUp ? i - 1 : i + 1);
            return Intent::kRedrawFull;
        }

        case Screen::kAgent: {
            const Agent* a = Selected(f);
            if (b == Button::kOk && p == Press::kHold) {
                GoTo(Screen::kFleet);
                return Intent::kRedrawFull;
            }
            if (a == nullptr) {
                GoTo(Screen::kFleet);
                return Intent::kRedrawFull;
            }
            if (b == Button::kOk) {
                if (a->action_count == 0 || !a->actionable) {
                    set_toast("READ ONLY", 3);
                    return Intent::kRedrawFull;
                }
                std::snprintf(pending_agent_id_, sizeof(pending_agent_id_),
                              "%s", a->id);
                std::snprintf(pending_action_id_, sizeof(pending_action_id_),
                              "%s", a->actions[action_cursor_].id);
                return Intent::kFireAction;
            }
            if (a->action_count == 0) return Intent::kNone;
            if (b == Button::kUp) {
                action_cursor_ = action_cursor_ == 0
                                     ? static_cast<uint8_t>(a->action_count - 1)
                                     : static_cast<uint8_t>(action_cursor_ - 1);
            } else {
                action_cursor_ = static_cast<uint8_t>(
                    (action_cursor_ + 1) % a->action_count);
            }
            return Intent::kRedrawFull;
        }
    }
    return Intent::kNone;
}

Paint Ui::Render(Canvas& c, const Fleet& f, const Device& d) {
    switch (screen_) {
        case Screen::kFleet: DrawFleet(c, f, d); break;
        case Screen::kAgent: DrawAgent(c, f, d); break;
        case Screen::kSystem: DrawSystem(c, f, d); break;
        case Screen::kSplash: DrawSplash(c, f, d); break;
        case Screen::kQuiet: return Paint::kFull4bpp;
    }
    const bool full = force_full_;
    force_full_ = false;
    return full ? Paint::kFull1bpp : Paint::kPartial1bpp;
}

}  // namespace beacon
