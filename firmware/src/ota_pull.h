#pragma once
#include <stdbool.h>
#include <stdint.h>

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

// Command Center UI snapshot (design/command-center/SPEC.md section 7).
// Transport-free: shared UI code includes this header on every board.
enum ota_ui_result_t {
    OTA_UI_UNKNOWN,
    OTA_UI_UP_TO_DATE,
    OTA_UI_AVAILABLE,
    OTA_UI_ERROR,
    OTA_UI_REBOOTING,
};

struct OtaUiSnapshot {
    uint8_t  result;      // ota_ui_result_t
    char     ver[16];     // available version, "" if none
    long     size;        // image bytes
    int      pct;         // last %, -1 if none
    uint32_t rate_bps;    // last measured rate, 0 unknown
    uint32_t eta_s;       // seconds remaining, 0 unknown
    char     err[16];     // short code, "" if none
    uint32_t ms;          // millis() of last update, 0 = never
};

// Copy the snapshot under the mux. Call from the loop task.
void ota_pull_ui_snapshot(OtaUiSnapshot* out);

// Enqueue a check / apply with from_ble=false. Call from the loop task.
void ota_pull_request_check(void);
void ota_pull_request_apply(void);
