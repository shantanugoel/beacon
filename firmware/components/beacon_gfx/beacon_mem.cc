#include "beacon_mem.h"

#include <cstdlib>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

namespace beacon {

void* BigAlloc(size_t bytes) {
#ifdef ESP_PLATFORM
    void* memory = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (memory != nullptr) return memory;
    // No PSRAM (or it is full): internal RAM is better than not drawing.
    return heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
#else
    return std::malloc(bytes);
#endif
}

}  // namespace beacon
