#ifndef BEACON_MEM_H_
#define BEACON_MEM_H_

/* Placement for the few objects that are too big for internal RAM.
 *
 * The 400x300 greyscale scratch surface is 120 KB and a Fleet is 8.5 KB; the
 * ESP32-S3 has ~330 KB of DRAM total and ESP-IDF has already spent most of it.
 * These live in the 8 MB of octal PSRAM instead. The host simulator has no
 * such distinction, so the attribute compiles away there.
 *
 * Note what is deliberately *not* marked: the 1bpp canvas stays in internal
 * RAM because it is handed straight to SPI DMA and is written a pixel at a
 * time by every draw call.
 */
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define BEACON_BIG_BSS EXT_RAM_BSS_ATTR
#else
#define BEACON_BIG_BSS
#endif

#endif  /* BEACON_MEM_H_ */
