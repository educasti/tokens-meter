// Single owner of the WiFi radio, shared by hybrid OTA (ota.cpp) and the pull
// updater (ota_pull.cpp). Contract: design/ota-pull/DESIGN.md section 7.4 and
// design/ota-pull/IMPL.md section 2.2.
//
// Hardware-only module: the native sim excludes it (platformio.ini) exactly
// like ota.cpp. This only arbitrates ownership; bringing the radio up
// (WiFi.mode(WIFI_STA)) stays the caller's job.
#include "ota_wifi.h"

#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

static ota_wifi_user_t s_owner = OTA_WIFI_NONE;
static portMUX_TYPE     s_mux   = portMUX_INITIALIZER_UNLOCKED;

bool ota_wifi_acquire(ota_wifi_user_t who) {
    portENTER_CRITICAL(&s_mux);
    bool ok = (s_owner == OTA_WIFI_NONE) || (s_owner == who);
    ota_wifi_user_t holder = s_owner;
    if (ok) s_owner = who;
    portEXIT_CRITICAL(&s_mux);

    if (!ok) {
        Serial.printf("OTA: wifi acquire refused (held by %d, asked by %d)\n",
                      (int)holder, (int)who);
    }
    return ok;
}

void ota_wifi_release(void) {
    portENTER_CRITICAL(&s_mux);
    ota_wifi_user_t prev = s_owner;
    s_owner = OTA_WIFI_NONE;
    portEXIT_CRITICAL(&s_mux);

    WiFi.disconnect(true);   // radio off, keep the AP config
    WiFi.mode(WIFI_OFF);
    if (prev != OTA_WIFI_NONE) {
        Serial.printf("OTA: wifi owner %d released, radio off\n", (int)prev);
    }
}

bool ota_wifi_is_held_by(ota_wifi_user_t who) {
    portENTER_CRITICAL(&s_mux);
    bool held = (s_owner != OTA_WIFI_NONE) && (s_owner == who);
    portEXIT_CRITICAL(&s_mux);
    return held;
}

bool ota_wifi_is_held(void) {
    portENTER_CRITICAL(&s_mux);
    bool held = (s_owner != OTA_WIFI_NONE);
    portEXIT_CRITICAL(&s_mux);
    return held;
}
