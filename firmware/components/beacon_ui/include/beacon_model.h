#ifndef BEACON_MODEL_H_
#define BEACON_MODEL_H_

#include <stddef.h>
#include <stdint.h>

namespace beacon {

/* Truncating copy into a fixed display field.
 *
 * Every string in this model is sized for a column on a 400 px panel, so
 * losing the tail of an over-long title is the intended behaviour, not a bug.
 * Saying that explicitly keeps -Wformat-truncation (which the ESP-IDF build
 * treats as an error) meaningful for the cases where truncation *is* a bug. */
inline void CopyField(char* dst, size_t cap, const char* src) {
    if (dst == nullptr || cap == 0) return;
    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }
    size_t i = 0;
    for (; i + 1 < cap && src[i] != '\0'; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

/* Mirrors herdr's AgentStatus so no translation is needed at the edge, plus
 * kStale for records the hub has stopped hearing about. */
enum class Status : uint8_t {
    kUnknown = 0,
    kIdle,      /* alive, waiting for a human prompt        */
    kWorking,   /* actively producing                       */
    kBlocked,   /* wants an answer from you right now       */
    kDone,      /* finished a turn, result unread           */
    kStale,     /* collector has gone quiet on this record  */
};

constexpr int kMaxAgents = 16;
constexpr int kMaxActions = 4;

/* A quick action the device can fire back at an agent. `send` is opaque to
 * the firmware: the hub decides whether it becomes send_keys or a prompt. */
struct Action {
    char label[20];
    char id[16];
};

struct Agent {
    char id[28];        /* stable "machine:pane" key                     */
    char title[64];     /* AI-generated session title, the primary label */
    char project[32];   /* basename of cwd                               */
    char branch[24];
    char machine[16];
    char activity[80];  /* one line of what it is doing right now        */
    char question[112]; /* when blocked: what it is asking for           */

    Status status = Status::kUnknown;
    uint32_t status_age_s = 0;  /* seconds held in the current status */
    uint32_t tokens = 0;        /* context tokens consumed this session */
    uint32_t cost_milli = 0;    /* USD * 1000 */
    int32_t lines_added = 0;
    int32_t lines_removed = 0;

    bool focused = false;    /* this pane has the human's attention */
    bool actionable = false; /* the hub can write back to it        */

    Action actions[kMaxActions];
    uint8_t action_count = 0;
};

/* Everything the device knows, refreshed as a unit from the hub. */
struct Fleet {
    Agent agents[kMaxAgents];
    uint8_t count = 0;
    uint32_t rev = 0;

    /* Denormalised counters so screens do not rescan. */
    uint8_t n_working = 0;
    uint8_t n_blocked = 0;
    uint8_t n_done = 0;
    uint8_t n_idle = 0;
    uint8_t n_machines = 0;

    /* Wall clock, from the hub (authoritative) or the on-board RTC. */
    uint8_t hour = 0, minute = 0;
    char date_label[16] = {};  /* "MON 07 SEP" */
};

enum class Link : uint8_t {
    kBooting,
    kWifiConnecting,
    kHubConnecting,
    kOnline,
    kDegraded,  /* had state once, lost the hub since */
    kOffline,
};

struct Device {
    Link link = Link::kBooting;
    int8_t rssi = 0;
    uint8_t battery_percent = 0;
    uint16_t battery_mv = 0;
    bool charging = false;
    bool battery_valid = false;
    uint32_t uptime_s = 0;
    uint32_t last_sync_age_s = 0;
    uint32_t full_refreshes = 0;
    uint32_t partial_refreshes = 0;
    char ssid[32] = {};
    char ip[16] = {};
    char hub[48] = {};
    char firmware[16] = {};
};

const char* StatusWord(Status s);
/* "12s" / "4m12s" / "1h04m" / "3d" - always <= 6 chars so columns stay put. */
void FormatDuration(uint32_t seconds, char* out, int cap);

}  // namespace beacon

#endif  // BEACON_MODEL_H_
