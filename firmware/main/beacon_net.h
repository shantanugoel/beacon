#ifndef BEACON_NET_H_
#define BEACON_NET_H_

#include "beacon_config.h"
#include "beacon_model.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace beacon {

/* Owns the radio and the hub conversation. Runs its own task; the UI reads the
 * fleet under a mutex and never blocks on the network. */
class Net {
public:
    esp_err_t Start(const Config& config);

    /* Copies the current fleet out. Returns false if nothing has arrived yet. */
    bool CopyFleet(Fleet* out);

    Link link() const { return link_; }
    int8_t rssi() const { return rssi_; }
    const char* ip() const { return ip_; }
    uint32_t last_sync_age_s() const;
    uint32_t rev() const { return rev_; }

    /* Queues an action for the hub. Non-blocking; the network task sends it
     * on its next pass, so a button press never waits on the radio. */
    bool PostAction(const char* agent_id, const char* action_id);

    /* Set when a new fleet has landed and the UI should redraw. */
    bool TakeFleetChanged();

private:
    static void TaskEntry(void* arg);
    void Task();
    esp_err_t StartWifi();
    bool PollOnce();
    void SendPending();
    bool ParseState(const char* body, int length);

    Config config_{};
    /* Net is a file-scope object in app_main, so this 8.5 KB member lands in
     * .bss rather than on a stack - but it still has to be placed in PSRAM,
     * which is why the instance itself carries BEACON_BIG_BSS. */
    Fleet fleet_{};
    SemaphoreHandle_t lock_ = nullptr;
    volatile Link link_ = Link::kBooting;
    volatile int8_t rssi_ = 0;
    char ip_[16] = {};
    volatile uint32_t rev_ = 0;
    volatile int64_t last_sync_us_ = 0;
    volatile bool fleet_changed_ = false;
    volatile bool have_fleet_ = false;

    char pending_agent_[28] = {};
    char pending_action_[16] = {};
    volatile bool pending_ = false;
};

}  // namespace beacon

#endif  // BEACON_NET_H_
