#pragma once
// Force-included (see firmware/scripts/lvgl_psram.py) so LVGL's builtin TLSF
// pool is allocated in PSRAM instead of LV_MEM_SIZE bytes of internal RAM.
//
// LVGL's lv_mem_core_builtin.c calls LV_MEM_POOL_ALLOC(LV_MEM_SIZE) when the
// macro is defined, so this keeps the whole pool out of .bss and frees the
// internal RAM the pull-OTA WiFi + TLS path needs.
#include <esp_heap_caps.h>

#ifndef LV_MEM_POOL_ALLOC
#define LV_MEM_POOL_ALLOC(size) heap_caps_malloc((size), MALLOC_CAP_SPIRAM)
#endif
