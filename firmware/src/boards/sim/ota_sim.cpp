// Native-simulator stub for the hardware-only hybrid-OTA module (ota.cpp).
// The sim has no WiFi or BLE, so every entry point is a no-op and the shared
// code (main.cpp / ui.cpp) links against the same ota_* symbols as hardware.
// Mirrors the boards/sim/ble_sim.cpp split.
#include "../../ota.h"
#include <stdio.h>

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
