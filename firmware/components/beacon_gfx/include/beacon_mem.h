#ifndef BEACON_MEM_H_
#define BEACON_MEM_H_

#include <stddef.h>

namespace beacon {

/* Allocator for the few objects too big for internal RAM.
 *
 * The 400x300 greyscale scratch surface is 120 KB; the ESP32-S3 has ~330 KB of
 * DRAM and ESP-IDF has already spent most of it. These go to the 8 MB of octal
 * PSRAM instead.
 *
 * Why a runtime allocation rather than EXT_RAM_BSS_ATTR: placing .bss in PSRAM
 * (CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY) coincided with a hard crash
 * inside the Wi-Fi driver's power-management timer path as soon as the radio
 * associated. heap_caps_malloc(MALLOC_CAP_SPIRAM) is the pattern ESP-IDF
 * actually supports alongside Wi-Fi, and it leaves the BSS segment alone.
 * See docs/RESEARCH_LOG.md §9.
 *
 * Never returns memory for something touched from an ISR, and never used for
 * the 1bpp canvas, which is a SPI DMA source and the hot path for drawing.
 *
 * Falls back to plain malloc, so the host simulator builds unchanged.
 */
void* BigAlloc(size_t bytes);

}  // namespace beacon

#endif  /* BEACON_MEM_H_ */
