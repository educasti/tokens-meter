// Native-simulator stub for the hardware-only hybrid-OTA module (ota.cpp).
// The sim has no WiFi or BLE, so every entry point is a no-op and the shared
// code (main.cpp / ui.cpp) links against the same ota_* symbols as hardware.
// Mirrors the boards/sim/ble_sim.cpp split.
#include "../../ota.h"
#include "../../ota_pull.h"
#include "../../portal.h"
#include "../../usage_pull.h"
#include "../../usage_pair.h"
#include <stdio.h>
#include <string.h>

void ota_init(void) {}
void ota_tick(void) {}
void ota_handle_ctrl(const char* json) { (void)json; }
void ota_set_wifi(const char* ssid, const char* pass) { (void)ssid; (void)pass; }
void ota_wifi_clear(void) {}
void ota_start(const char* pass) {
    (void)pass;
    printf("[sim] ota start ignored (no WiFi in the simulator)\n");
}
void ota_stop(void) {}
bool ota_is_active(void) { return false; }
void ota_confirm(void) {}
const char* ota_version(void) { return "sim"; }
const char* ota_stored_ssid(void) { return ""; }
bool ota_wifi_connected(void) { return false; }
int ota_wifi_rssi(void) { return 0; }

// Pull-OTA stubs: ota_pull.cpp is hardware-only (excluded from the sim build),
// so the shared main.cpp links these no-ops instead.
void ota_pull_init(void) {}
void ota_pull_tick(void) {}
void ota_pull_handle_ctrl(const char* json) { (void)json; }
bool ota_pull_is_active(void) { return false; }
const char* ota_pull_state_name(void) { return "idle"; }
void ota_pull_ui_snapshot(OtaUiSnapshot* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
}
void ota_pull_request_check(void) {}
void ota_pull_request_apply(void) {}

// SoftAP captive portal stubs: portal.cpp is hardware-only (excluded from the
// sim build), so main.cpp links these no-ops instead.
void portal_init(void) {}
void portal_tick(void) {}
bool portal_is_active(void) { return false; }
void portal_start(void) {}
void portal_stop(void) {}
const char* portal_ssid(void) { return "Clawdmeter-SIM0"; }
bool portal_has_creds(void) { return false; }

// Backend usage-pull stubs: usage_pull.cpp is hardware-only (excluded from the
// sim build), so main.cpp links these no-ops instead.
void usage_pull_init(void) {}
void usage_pull_tick(void) {}
bool usage_pull_active(void) { return false; }

// Runtime pairing stubs: usage_pair.cpp is hardware-only (excluded from the sim
// build). The sim has no token and never pairs.
void usage_pair_init(void) {}
void usage_pair_tick(void) {}
bool usage_pair_has_token(void) { return false; }
const char* usage_pair_code(void) { return ""; }
const char* usage_pair_state(void) { return "idle"; }
void usage_pair_start(void) {}
void usage_pair_clear(void) {}
