#pragma once
#include <stdbool.h>

// Single owner of the WiFi radio, shared by hybrid OTA (ota.cpp) and the pull
// updater (ota_pull.cpp). Contract: design/ota-pull/DESIGN.md section 7.4 and
// design/ota-pull/IMPL.md section 2.2.
//
// Hardware-only: the implementation lives in ota_wifi.cpp, which is excluded
// from the native sim (platformio.ini) exactly like ota.cpp. Nothing in shared
// code (main.cpp / ui.cpp) may call this header.
//
// Threading: ota_wifi_acquire()/ota_wifi_release() are called from the Arduino
// loop task or the pull task, never from the NimBLE host task.

typedef enum { OTA_WIFI_NONE, OTA_WIFI_HYBRID, OTA_WIFI_PULL } ota_wifi_user_t;

// Take the radio for `who`. Returns false if another owner already holds it;
// idempotent (returns true) when `who` already holds it. Bringing the radio up
// (WiFi.mode(WIFI_STA)) is the caller's job; this only arbitrates ownership.
bool ota_wifi_acquire(ota_wifi_user_t who);

// Release the radio: WiFi.disconnect(true) + WiFi.mode(WIFI_OFF). Safe to call
// when nobody holds it.
void ota_wifi_release(void);

bool ota_wifi_is_held_by(ota_wifi_user_t who);
bool ota_wifi_is_held(void);
