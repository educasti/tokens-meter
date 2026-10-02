#pragma once
#include <stdbool.h>

// Pull-based automatic OTA engine. Contract: design/ota-pull/DESIGN.md section
// 7 (state machine) and design/ota-pull/IMPL.md section 2.3.
//
// Transport-free header so shared code (main.cpp) can include it on every
// board. The hardware implementation lives in ota_pull.cpp; the native sim
// links no-op stubs in boards/sim/ota_sim.cpp instead (platformio.ini's sim
// build_src_filter excludes ota_pull.cpp), mirroring how ota.cpp is handled.
//
// Threading: ota_pull_handle_ctrl() runs on the NimBLE host task and only
// parses/enqueues; the WiFi/TLS/flash work happens on a dedicated pull task
// (or the loop task) via ota_pull_tick().

// Load the scheduling state and (lazily) prepare the worker. Call once, early
// in setup(), after ota_init().
void ota_pull_init(void);

// Non-blocking: advances the state machine, applies the boot/24 h schedule and
// exposes the UI state. Call every loop().
void ota_pull_tick(void);

// Parse one `{"cmd":"update",...}` CTRL (…0005) command and enqueue it. The
// owner/bonding check is enforced by the BLE layer before this runs.
void ota_pull_handle_ctrl(const char* json);

// True while a check or update is in progress (UI badge).
bool ota_pull_is_active(void);

// State name for logs/UI: "idle" | "join" | "sntp" | "manifest" | "download" |
// "verify" | "activate" | "reboot".
const char* ota_pull_state_name(void);
