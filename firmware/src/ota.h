#pragma once
#include <stdbool.h>

// Hybrid OTA — WiFi binary transfer driven by a BLE control channel.
// Frozen contract: design/ota-hybrid/DESIGN.md.
//
// This header is deliberately transport-free so shared code (main.cpp, ui.cpp)
// can include it on every board. The hardware implementation lives in ota.cpp;
// the native sim links boards/sim/ota_sim.cpp (same symbols, no-ops) instead —
// see the sim env's build_src_filter in platformio.ini.
//
// Threading: ota_handle_ctrl() runs on the NimBLE host task and only parses /
// enqueues; ota_tick(), ota_start(), ota_stop() and ota_confirm() are called
// from the Arduino loop task.

// Load WiFi credentials from NVS and arm the boot-verify counter (DESIGN.md §4).
// Must be called once, early in setup(); it can reboot the device to roll back
// a slot that has failed to confirm too many times.
void ota_init(void);

// Non-blocking state machine: drives the WiFi join, ArduinoOTA, timeouts and
// the deferred TX replies. Call every loop().
void ota_tick(void);

// Parse one CTRL (…0005) JSON command and reply on the TX (…0003) notify
// channel. Ownership/bonding is enforced by the BLE layer before this runs.
void ota_handle_ctrl(const char* json);

// Store / erase the WiFi credentials in NVS (namespace "otah").
void ota_set_wifi(const char* ssid, const char* pass);
void ota_wifi_clear(void);

// Bring WiFi up and start ArduinoOTA (asynchronous: the ready/error TX reply
// is emitted from ota_tick()), or stop it and drop WiFi.
void ota_start(const char* pass);
void ota_stop(void);
bool ota_is_active(void);

// Confirm the running slot: resets the boot-verify counter to 0. Called when a
// valid owner usage payload lands, or after 60 s of healthy uptime.
void ota_confirm(void);

// Firmware version string baked in at build time (-DFW_VERSION="...").
const char* ota_version(void);
