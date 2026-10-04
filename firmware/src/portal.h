#pragma once
#include <stdbool.h>

// SoftAP + captive portal WiFi provisioning. Contract:
// design/backend-wifi/ROADMAP.md (D6 and section 7).
//
// Transport-free header so shared code (main.cpp) can include it on every
// board. The hardware implementation lives in portal.cpp; the native sim links
// the no-op stubs in boards/sim/ota_sim.cpp instead (platformio.ini's sim
// build_src_filter excludes portal.cpp), mirroring ota.cpp / ota_pull.cpp.
//
// Threading: the AP, DNS and HTTP servers are owned by the Arduino loop task.
// portal_start()/portal_stop() are safe to call from the NimBLE host task
// (ota_handle_ctrl) because they only set a request flag; portal_tick()
// performs the actual radio bring-up/teardown on the loop task. WiFi must never
// be touched from the NimBLE host task (same rule as hybrid OTA / pull OTA).

// One-time init; call once in setup(), after ota_init()/ota_pull_init().
void portal_init(void);

// Advance the portal state machine and service the DNS + HTTP servers.
// Call every loop().
void portal_tick(void);

// True while the SoftAP is up.
bool portal_is_active(void);

// Request the SoftAP captive portal to start / stop. Enqueue-only: the radio
// work happens in portal_tick().
void portal_start(void);
void portal_stop(void);

// The advertised SoftAP SSID ("Clawdmeter-XXXX", XXXX = last two bytes of the
// WiFi MAC, upper-case). Stable across boots and valid before the AP is up so
// the CTRL reply can name it.
const char* portal_ssid(void);

// Read-only check: is an SSID already stored in NVS? Used by main.cpp to
// decide whether to auto-start provisioning. Credentials are never written
// here — ota_set_wifi() owns the write path.
bool portal_has_creds(void);
