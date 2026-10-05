#pragma once
#include <stdbool.h>

// Runtime pairing + device-token storage (backend-wifi phase 2).
// Contract: design/backend-wifi/PHASE2-CONTRACT.md sections 2, 6.
//
// Transport-free header so shared code (main.cpp, ui.cpp, portal.cpp) can
// include it on every board. The implementation (usage_pair.cpp) is
// hardware-only; the native sim links no-op stubs in boards/sim/ota_sim.cpp.
//
// Threading: the pairing poll runs on a dedicated worker / the loop task via
// usage_pair_tick(); callers on the loop task may read the getters at any time.

// Load the stored device token from NVS (namespace "otah", key "dev_token").
// Call once in setup().
void usage_pair_init(void);

// Non-blocking: when unpaired, drives the pairing poll (WiFi + pinned cert).
// Call every loop().
void usage_pair_tick(void);

// True once a device token is stored (NVS).
bool usage_pair_has_token(void);

// The current pairing code (8 chars) to display, or "" once paired.
const char* usage_pair_code(void);

// "idle" | "pairing" | "paired" | "error" — for logs/UI.
const char* usage_pair_state(void);

// (Re)generate a code and begin polling; also used after a 401 (re-pair).
void usage_pair_start(void);

// Wipe the stored token and re-enter pairing.
void usage_pair_clear(void);
