#include "beacon_display.h"

#include <algorithm>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace beacon {
namespace {

constexpr const char* kTag = "beacon.disp";

/* Partial refreshes leave a faint residue; this many in a row and the panel
 * gets a full flash to clean up, whether or not anything else changed. */
constexpr uint32_t kPartialsPerFull = 16;

/* Above this fraction of the panel, a "partial" update costs about what a
 * full one does but looks worse, so take the flash instead. */
constexpr int kFullThresholdPercent = 42;

}  // namespace

esp_err_t Display::Init() {
    zectrix_epd_config_t config;
    zectrix_epd_get_default_config(&config);
    esp_err_t err = zectrix_epd_new(&config, &epd_);
    if (err != ESP_OK) return err;
    err = zectrix_epd_power_on(epd_);
    if (err != ESP_OK) return err;

    previous_ = static_cast<uint8_t*>(
        heap_caps_malloc(kFrameBytes, MALLOC_CAP_SPIRAM));
    if (previous_ == nullptr) previous_ = static_cast<uint8_t*>(malloc(kFrameBytes));
    patch_ = static_cast<uint8_t*>(
        heap_caps_malloc(kFrameBytes, MALLOC_CAP_SPIRAM));
    if (patch_ == nullptr) patch_ = static_cast<uint8_t*>(malloc(kFrameBytes));
    if (previous_ == nullptr || patch_ == nullptr) return ESP_ERR_NO_MEM;
    memset(previous_, 0xFF, kFrameBytes);
    // E-ink retains its image without power. Each refresh wakes and
    // reinitializes the controller, so leave the external rail off at idle.
    return zectrix_epd_power_off(epd_);
}

void Display::Shutdown() {
    if (epd_ == nullptr) return;
    zectrix_epd_power_off(epd_);
    zectrix_epd_del(epd_);
    epd_ = nullptr;
}

/* Bounding box of every byte that differs from what is on the panel. Working
 * a byte at a time (rather than a pixel at a time) is both faster and exactly
 * the granularity the panel's partial window wants on X. */
bool Display::DiffRect(const Canvas& canvas, Rect* out) const {
    const uint8_t* now = canvas.data();
    int min_x = kStride, max_x = -1, min_y = kScreenH, max_y = -1;
    for (int y = 0; y < kScreenH; ++y) {
        const size_t row = static_cast<size_t>(y) * kStride;
        if (memcmp(&now[row], &previous_[row], kStride) == 0) continue;
        if (y < min_y) min_y = y;
        max_y = y;
        for (int b = 0; b < kStride; ++b) {
            if (now[row + b] != previous_[row + b]) {
                if (b < min_x) min_x = b;
                if (b > max_x) max_x = b;
            }
        }
    }
    if (max_y < 0) return false;
    *out = {min_x * 8, min_y, (max_x - min_x + 1) * 8, max_y - min_y + 1};
    return true;
}

esp_err_t Display::Present(Canvas& canvas, bool force_full) {
    if (epd_ == nullptr) return ESP_ERR_INVALID_STATE;
    const int64_t started = esp_timer_get_time();

    Rect diff{};
    const bool changed = DiffRect(canvas, &diff);
    if (!changed && !force_full && have_base_) {
        return ESP_OK;  // Nothing to say; say nothing.
    }

    const int area = diff.w * diff.h;
    const int percent = (area * 100) / (kScreenW * kScreenH);
    const bool need_full = force_full || !have_base_ ||
                           since_full_ >= kPartialsPerFull ||
                           percent >= kFullThresholdPercent;

    esp_err_t err = zectrix_epd_power_on(epd_);
    if (err != ESP_OK) return err;
    if (need_full) {
        err = zectrix_epd_refresh_full_1bpp(epd_, canvas.data(), kFrameBytes);
        if (err == ESP_OK) {
            have_base_ = true;
            have_gray_ = false;
            since_full_ = 0;
            ++full_count_;
        }
    } else {
        size_t length = 0;
        const Rect snapped =
            canvas.ExtractPatch(diff, patch_, kFrameBytes, &length);
        if (snapped.empty() || length == 0) return ESP_ERR_INVALID_SIZE;
        const zectrix_epd_rect_t window = {snapped.x, snapped.y, snapped.w,
                                           snapped.h};
        err = zectrix_epd_refresh_partial_1bpp(epd_, &window, patch_, length);
        if (err == ESP_OK) {
            ++since_full_;
            ++partial_count_;
        }
    }

    const esp_err_t power_err = zectrix_epd_power_off(epd_);
    if (err == ESP_OK) err = power_err;

    if (err == ESP_OK) {
        memcpy(previous_, canvas.data(), kFrameBytes);
    } else {
        // Do not record a frame the panel never took, or the next diff will be
        // computed against a lie.
        ESP_LOGW(kTag, "refresh failed: %s", esp_err_to_name(err));
        have_base_ = false;
    }
    last_refresh_us_ = esp_timer_get_time() - started;
    ESP_LOGI(kTag, "%s %dx%d (%d%%) in %lld ms",
             need_full ? "full" : "partial", diff.w, diff.h, percent,
             last_refresh_us_ / 1000);
    return err;
}

esp_err_t Display::PresentGray(const uint8_t* packed) {
    if (epd_ == nullptr) return ESP_ERR_INVALID_STATE;
    if (packed == nullptr) return ESP_ERR_INVALID_ARG;

    if (previous_gray_ == nullptr) {
        previous_gray_ = static_cast<uint8_t*>(
            heap_caps_malloc(ZECTRIX_EPD_4BPP_FRAME_BYTES, MALLOC_CAP_SPIRAM));
        if (previous_gray_ == nullptr) {
            previous_gray_ = static_cast<uint8_t*>(
                malloc(ZECTRIX_EPD_4BPP_FRAME_BYTES));
        }
        if (previous_gray_ == nullptr) return ESP_ERR_NO_MEM;
    }
    if (have_gray_ &&
        memcmp(previous_gray_, packed, ZECTRIX_EPD_4BPP_FRAME_BYTES) == 0) {
        ESP_LOGI(kTag, "4bpp unchanged, skip");
        return ESP_OK;
    }

    const int64_t started = esp_timer_get_time();
    /* The vendor 4bpp path already paints a white base internally
     * (DisplayOtpWhiteBase). A second full 1bpp white here doubled the flash
     * the user sees and added ~1 s for no extra cleanliness. */
    esp_err_t err = zectrix_epd_power_on(epd_);
    if (err == ESP_OK) {
        err = zectrix_epd_refresh_full_4bpp(
            epd_, packed, ZECTRIX_EPD_4BPP_FRAME_BYTES);
    }
    const esp_err_t power_err = zectrix_epd_power_off(epd_);
    if (err == ESP_OK) err = power_err;
    if (err == ESP_OK) {
        memcpy(previous_gray_, packed, ZECTRIX_EPD_4BPP_FRAME_BYTES);
        have_gray_ = true;
        ++full_count_;
        // 4bpp leaves no usable base for partial updates.
        have_base_ = false;
        since_full_ = kPartialsPerFull;
    } else {
        ESP_LOGW(kTag, "4bpp refresh failed: %s", esp_err_to_name(err));
        have_gray_ = false;
        have_base_ = false;
    }
    last_refresh_us_ = esp_timer_get_time() - started;
    ESP_LOGI(kTag, "4bpp frame in %lld ms", last_refresh_us_ / 1000);
    return err;
}

esp_err_t Display::Clear() {
    if (epd_ == nullptr) return ESP_ERR_INVALID_STATE;
    static uint8_t* white = nullptr;
    if (white == nullptr) {
        white = static_cast<uint8_t*>(
            heap_caps_malloc(kFrameBytes, MALLOC_CAP_SPIRAM));
        if (white == nullptr) return ESP_ERR_NO_MEM;
    }
    memset(white, 0xFF, kFrameBytes);
    esp_err_t err = zectrix_epd_power_on(epd_);
    if (err == ESP_OK) {
        err = zectrix_epd_refresh_full_1bpp(epd_, white, kFrameBytes);
    }
    const esp_err_t power_err = zectrix_epd_power_off(epd_);
    if (err == ESP_OK) err = power_err;
    if (err == ESP_OK && previous_ != nullptr) {
        memset(previous_, 0xFF, kFrameBytes);
        have_base_ = true;
        since_full_ = 0;
    }
    return err;
}

}  // namespace beacon
