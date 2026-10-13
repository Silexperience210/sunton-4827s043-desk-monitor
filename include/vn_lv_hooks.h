// Crochets LVGL (mémoire + horloge) — implémentés par la plateforme :
// src/platform/platform_esp32.cpp (PSRAM, millis) ou sim/platform_sim.cpp.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void *   vn_lv_malloc(size_t size);
void     vn_lv_free(void * p);
void *   vn_lv_realloc(void * p, size_t size);
uint32_t vn_lv_tick(void);

#ifdef __cplusplus
}
#endif
