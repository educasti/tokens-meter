#pragma once
#include <stdbool.h>

// Backend-WiFi usage pull (design/backend-wifi/PHASE1-CONTRACT.md section 7).
//
// Periodically GETs USAGE_BACKEND_URL with the USAGE_DEVICE_TOKEN bearer token
// over the pinned-certificate TLS channel and feeds the numbers to ui_update().
// Both are build macros injected from untracked files by
// scripts/gen_usage_config.py; when either is empty the whole module is a no-op
// and the BLE path is untouched.
//
// Transport-free header so shared code (main.cpp) can include it on every
// board. The hardware implementation lives in usage_pull.cpp; the native sim
// links no-op stubs in boards/sim/ota_sim.cpp instead (platformio.ini's sim
// build_src_filter excludes usage_pull.cpp), like ota_pull.
//
// Threading: the blocking WiFi/TLS work runs on a dedicated worker task; the
// result is handed to the Arduino loop task, which is the only caller of
// ui_update() (LVGL is not thread-safe).

// Read the config and start the worker. Call once from setup(), after
// ota_init() / ota_pull_init().
void usage_pull_init(void);

// Non-blocking: applies the schedule (first pull ~10 s after boot, then every
// USAGE_POLL_S) and applies a finished result to the UI. Call every loop().
void usage_pull_tick(void);

// True while a pull is in flight (the radio is in use).
bool usage_pull_active(void);
