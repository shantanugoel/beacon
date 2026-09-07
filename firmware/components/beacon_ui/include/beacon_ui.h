#ifndef BEACON_UI_H_
#define BEACON_UI_H_

#include "beacon_canvas.h"
#include "beacon_model.h"

namespace beacon {

enum class Screen : uint8_t {
    kFleet,    /* the ambient home: attention band + fleet spine */
    kAgent,    /* one session in full, plus its actions          */
    kQuiet,    /* 1bpp night sky, for a desk at rest             */
    kSystem,   /* radio, hub, battery, panel wear                */
    kSplash,   /* boot / link states                             */
};

enum class Button : uint8_t { kUp, kDown, kOk };
enum class Press : uint8_t { kClick, kHold };

/* What the UI wants the shell to do after handling input. */
enum class Intent : uint8_t {
    kNone,
    kRedrawPartial,
    kRedrawFull,
    kFireAction,   /* pending_action_* is filled in */
};

/* Which refresh the renderer asked for. */
enum class Paint : uint8_t { kFull1bpp, kPartial1bpp, kFull4bpp };

class Ui {
public:
    void Reset();

    Screen screen() const { return screen_; }
    void GoTo(Screen s);

    /* Selection survives fleet reshuffles by tracking the agent id. */
    void OnFleetUpdated(const Fleet& fleet);

    Intent OnInput(Button b, Press p, const Fleet& fleet);

    /* Draws the current screen. Returns which refresh mode the caller should
     * use; for kPartial1bpp, canvas.dirty() is the region to push. */
    Paint Render(Canvas& canvas, const Fleet& fleet, const Device& dev);

    /* 16-grey twin of the quiet screen. Product quiet is 1bpp;
     * this exists so beacon-preview can still exercise the grey waveform. */
    void RenderQuiet4bpp(uint8_t* gray, const Fleet& fleet, const Device& dev);

    const char* pending_agent_id() const { return pending_agent_id_; }
    const char* pending_action_id() const { return pending_action_id_; }
    void ClearPending();

    /* True when the fleet gained a blocked agent since the last look; the
     * shell uses this to interrupt quiet mode and chirp. */
    bool TakeAttentionEdge();

    void set_toast(const char* text, uint32_t ttl_ticks);
    void tick_toast();

private:
    const Agent* Selected(const Fleet& fleet) const;
    int SelectedIndex(const Fleet& fleet) const;
    void SelectIndex(const Fleet& fleet, int index);

    void DrawFleet(Canvas& c, const Fleet& f, const Device& d);
    void DrawAgent(Canvas& c, const Fleet& f, const Device& d);
    void DrawSystem(Canvas& c, const Fleet& f, const Device& d);
    void DrawSplash(Canvas& c, const Fleet& f, const Device& d);
    void DrawQuiet(Canvas& c, const Fleet& f, const Device& d);

    Screen screen_ = Screen::kSplash;
    char selected_id_[28] = {};
    uint8_t action_cursor_ = 0;
    uint8_t last_blocked_ = 0;
    bool attention_edge_ = false;
    bool force_full_ = true;
    uint8_t scroll_ = 0;

    char pending_agent_id_[28] = {};
    char pending_action_id_[16] = {};

    char toast_[40] = {};
    uint32_t toast_ttl_ = 0;
};

/* ---- shared chrome, exposed so the simulator can proof pieces ---- */
void DrawHeader(Canvas& c, const char* title, const Fleet& f, const Device& d);
void DrawFooter(Canvas& c, const char* left, const char* mid,
                const char* right);
void DrawStatusMark(Canvas& c, int cx, int cy, Status s, bool on_dark);
void DrawBattery(Canvas& c, int x, int y, const Device& d, bool on_dark);
void DrawSignal(Canvas& c, int x, int y, const Device& d, bool on_dark);
void DrawWordmark(Canvas& c, int x, int baseline, bool on_dark);

}  // namespace beacon

#endif  // BEACON_UI_H_
