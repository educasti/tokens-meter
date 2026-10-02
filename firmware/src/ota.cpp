// Hybrid OTA — WiFi binary transfer over a BLE control channel.
//
// Frozen contract: design/ota-hybrid/DESIGN.md. Hardware-only module: the
// native sim links boards/sim/ota_sim.cpp instead (see the sim env's
// build_src_filter). The task split matters — ota_handle_ctrl() runs on the
// NimBLE host task and only parses/enqueues; every WiFi and ArduinoOTA call
// happens in ota_tick() on the Arduino loop task, whose stack can absorb a
// join and an mDNS start.
#include "ota.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

#include "ble.h"
#include "hal/board_caps.h"
#include "ota_pull.h"
#include "ota_wifi.h"

#ifndef FW_VERSION
#define FW_VERSION "dev"   // set per env in platformio.ini (-DFW_VERSION="...")
#endif
#ifndef FW_GIT_SHA
#define FW_GIT_SHA ""      // injected by scripts/version.py (P0 build stamp)
#endif
#ifndef FW_BUILD_DATE
#define FW_BUILD_DATE ""   // injected by scripts/version.py (P0 build stamp)
#endif

// NVS location and keys are frozen by the contract (§2 / §4).
#define OTAH_NS        "otah"
#define KEY_SSID       "ssid"
#define KEY_PASS       "pass"
#define KEY_BOOT_TRIES "boot_tries"

#define OTA_PORT       3232
#define WIFI_JOIN_MS   20000u   // bounded join; the helper has given up by then
#define HEALTHY_MS     60000u   // uptime that confirms a boot with no owner payload
#define MAX_BOOT_TRIES 3        // more than this many unconfirmed boots → roll back

enum ota_mode_t { OTA_MODE_IDLE, OTA_MODE_CONNECTING, OTA_MODE_READY };
enum ota_req_t   { REQ_NONE, REQ_START, REQ_STOP, REQ_REBOOT };

static ota_mode_t s_state = OTA_MODE_IDLE;

// One-slot request handoff from the BLE task to the loop task. Newest wins:
// the helper only ever has one control command in flight.
static ota_req_t     s_req = REQ_NONE;
static char          s_req_pass[64];
static portMUX_TYPE  s_mux = portMUX_INITIALIZER_UNLOCKED;

static char     s_ssid[64] = {0};
static char     s_pass[64] = {0};
static char     s_ota_pass[64] = {0};   // password for the in-flight OTA session
static bool     s_ota_pass_set = false;
static uint32_t s_connect_deadline = 0;
static uint32_t s_boot_ms = 0;
static bool     s_boot_confirmed = false;

const char* ota_version(void) { return FW_VERSION; }

// ---- NVS: WiFi credentials -------------------------------------------------

static void load_creds(void) {
    Preferences prefs;
    prefs.begin(OTAH_NS, true);
    String ssid = prefs.getString(KEY_SSID, "");
    String pass = prefs.getString(KEY_PASS, "");
    prefs.end();
    strlcpy(s_ssid, ssid.c_str(), sizeof(s_ssid));
    strlcpy(s_pass, pass.c_str(), sizeof(s_pass));
    Serial.printf("OTA: stored creds %s\n", s_ssid[0] ? s_ssid : "(none)");
}

void ota_set_wifi(const char* ssid, const char* pass) {
    strlcpy(s_ssid, ssid ? ssid : "", sizeof(s_ssid));
    strlcpy(s_pass, pass ? pass : "", sizeof(s_pass));
    Preferences prefs;
    prefs.begin(OTAH_NS, false);
    prefs.putString(KEY_SSID, s_ssid);
    prefs.putString(KEY_PASS, s_pass);
    prefs.end();
    Serial.printf("OTA: wifi creds stored (ssid=%s)\n", s_ssid);
}

void ota_wifi_clear(void) {
    s_ssid[0] = '\0';
    s_pass[0] = '\0';
    Preferences prefs;
    prefs.begin(OTAH_NS, false);
    prefs.remove(KEY_SSID);
    prefs.remove(KEY_PASS);
    prefs.end();
    Serial.println("OTA: wifi creds cleared");
}

// ---- NVS: boot-verify counter + application-level rollback (§4) ------------

static void boot_counter_reset(void) {
    Preferences prefs;
    prefs.begin(OTAH_NS, false);
    prefs.putUChar(KEY_BOOT_TRIES, 0);
    prefs.end();
}

// The prebuilt Arduino bootloader does not run with
// CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE verified, so rollback is application
// level: point otadata at the other app slot and reboot. Espressif's own
// sequence — switch partition, then clear the counter — prevents the good slot
// from immediately rolling back to the bad one (ping-pong).
static void rollback_and_restart(void) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* other   = esp_ota_get_next_update_partition(running);
    if (!other || other == running) {
        Serial.println("OTA: rollback: no alternate slot; keeping current image");
        boot_counter_reset();
        return;
    }
    Serial.printf("OTA: rollback %s -> %s\n",
                  running ? running->label : "?", other->label);
    if (esp_ota_set_boot_partition(other) != ESP_OK) {
        Serial.println("OTA: rollback: set_boot_partition failed; keeping current image");
        boot_counter_reset();
        return;
    }
    boot_counter_reset();
    delay(100);
    esp_restart();
}

void ota_confirm(void) {
    if (s_boot_confirmed) return;
    s_boot_confirmed = true;
    boot_counter_reset();
    Serial.println("OTA: boot confirmed (boot_tries=0)");
}

void ota_init(void) {
    s_boot_ms = millis();
    load_creds();

    Preferences prefs;
    prefs.begin(OTAH_NS, false);
    uint8_t tries = prefs.getUChar(KEY_BOOT_TRIES, 0) + 1;
    prefs.putUChar(KEY_BOOT_TRIES, tries);
    prefs.end();
    Serial.printf("OTA: boot verify armed, boot_tries=%u\n", tries);

    if (tries > MAX_BOOT_TRIES) rollback_and_restart();
    // If the rollback cannot proceed we fall through and keep running; the
    // 60 s / payload confirmation below will settle the counter.
}

// ---- Request handoff (BLE task → loop task) --------------------------------

static void enqueue(ota_req_t r, const char* pass) {
    portENTER_CRITICAL(&s_mux);
    s_req = r;
    if (r == REQ_START) strlcpy(s_req_pass, pass ? pass : "", sizeof(s_req_pass));
    portEXIT_CRITICAL(&s_mux);
}

static ota_req_t dequeue(char* pass_out, size_t n) {
    portENTER_CRITICAL(&s_mux);
    ota_req_t r = s_req;
    s_req = REQ_NONE;
    if (r == REQ_START) {
        strlcpy(pass_out, s_req_pass, n);
        s_req_pass[0] = '\0';
    }
    portEXIT_CRITICAL(&s_mux);
    return r;
}

void ota_start(const char* pass) { enqueue(REQ_START, pass); }
void ota_stop(void)              { enqueue(REQ_STOP, nullptr); }
bool ota_is_active(void)         { return s_state != OTA_MODE_IDLE; }

// ---- TX status notifications (§2) ------------------------------------------

static void send_status(const char* json) {
    Serial.printf("OTA: TX %s\n", json);
    ble_notify_status(json);
}

static void send_ready(void) {
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"cmd\":\"ota\",\"state\":\"ready\",\"ip\":\"%s\",\"port\":%d}",
             WiFi.localIP().toString().c_str(), OTA_PORT);
    send_status(buf);
}

static void send_off(void) {
    send_status("{\"ok\":true,\"cmd\":\"ota\",\"state\":\"off\"}");
}

// ---- WiFi / ArduinoOTA -----------------------------------------------------

// clawdmeter-<last6ofmac> (§3), lower-case hex. The MAC string from ble.cpp is
// upper-case "AA:BB:CC:DD:EE:FF".
static void make_hostname(char* out, size_t n) {
    const char* mac = ble_get_mac_address();
    char hex[13];
    int j = 0;
    for (int i = 0; mac && mac[i] && j < 12; i++) {
        char c = mac[i];
        if (c == ':') continue;
        if (c >= 'A' && c <= 'F') c += 'a' - 'A';
        hex[j++] = c;
    }
    hex[j] = '\0';
    const char* tail = (j >= 6) ? hex + j - 6 : hex;
    snprintf(out, n, "clawdmeter-%s", tail);
}

static bool wifi_begin(void) {
    // The radio has a single owner (DESIGN §7.4): refuse to start if the pull
    // engine already holds it, mirroring the "busy" reply for a double start.
    if (!ota_wifi_acquire(OTA_WIFI_HYBRID)) {
        Serial.println("OTA: wifi busy (held by another owner)");
        send_status("{\"ok\":false,\"err\":\"busy\"}");
        return false;
    }
    Serial.printf("OTA: joining WiFi ssid=%s\n", s_ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(s_ssid, s_pass);
    s_connect_deadline = millis() + WIFI_JOIN_MS;
    s_state = OTA_MODE_CONNECTING;
    return true;
}

static void ota_server_begin(const char* pass) {
    char host[32];
    make_hostname(host, sizeof(host));
    ArduinoOTA.setPort(OTA_PORT);
    ArduinoOTA.setHostname(host);
    // setPassword() SHA256-hashes its argument; an empty string would hash to a
    // real 64-char hash (i.e. still require auth), so an unauthenticated
    // session never calls it. (ArduinoOTA only offers set/ overwrite, so a
    // session change from "password" to "none" within one boot keeps the old
    // hash — the helper is consistent across its staged reboots, so this is a
    // non-issue in practice.)
    if (pass && pass[0]) ArduinoOTA.setPassword(pass);
    ArduinoOTA.setRebootOnSuccess(true);
    ArduinoOTA.onStart([]() { Serial.println("OTA: transfer started"); });
    ArduinoOTA.onEnd([]() { Serial.println("OTA: transfer ended"); });
    ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
        static unsigned int last = 0;
        if (total && (done - last) * 100 / total >= 10) {
            last = done;
            Serial.printf("OTA: transfer %u%%\n", done * 100 / total);
        }
    });
    ArduinoOTA.onError([](ota_error_t e) { Serial.printf("OTA: error %u\n", (unsigned)e); });
    ArduinoOTA.begin();
    Serial.printf("OTA: server ready at %s.local:%d ip=%s\n",
                  host, OTA_PORT, WiFi.localIP().toString().c_str());
}

static void ota_stop_now(void) {
    if (s_state == OTA_MODE_READY) ArduinoOTA.end();
    ota_wifi_release();   // disconnect(true) + WiFi.mode(WIFI_OFF)
    s_state = OTA_MODE_IDLE;
    s_ota_pass_set = false;
    s_ota_pass[0] = '\0';
    Serial.println("OTA: stopped, WiFi down");
}

// ---- CTRL command parser (§2) ----------------------------------------------

void ota_handle_ctrl(const char* json) {
    if (!json) return;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("OTA: ctrl bad json: %s\n", err.c_str());
        send_status("{\"ok\":false,\"err\":\"bad_json\"}");
        return;
    }

    const char* cmd = doc["cmd"] | "";
    Serial.printf("OTA: ctrl cmd=%s\n", cmd);

    if (strcmp(cmd, "info") == 0) {
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "{\"ok\":true,\"board\":\"%s\",\"fw\":\"%s\",\"sha\":\"%s\",\"build\":\"%s\",\"id\":\"%s\"}",
                 board_caps().id, ota_version(), FW_GIT_SHA, FW_BUILD_DATE,
                 ble_get_mac_address());
        send_status(buf);
        return;
    }

    if (strcmp(cmd, "wifi") == 0) {
        ota_set_wifi(doc["ssid"] | "", doc["pass"] | "");
        send_status("{\"ok\":true,\"cmd\":\"wifi\"}");
        return;
    }

    if (strcmp(cmd, "wifi_clear") == 0) {
        ota_wifi_clear();
        send_status("{\"ok\":true,\"cmd\":\"wifi_clear\"}");
        return;
    }

    if (strcmp(cmd, "ota") == 0) {
        const char* mode = doc["mode"] | "";
        if (strcmp(mode, "on") == 0) {
            if (!s_ssid[0]) {
                send_status("{\"ok\":false,\"err\":\"no_wifi\"}");
            } else {
                ota_start(doc["pass"] | "");   // ready/error reply comes from ota_tick()
            }
        } else if (strcmp(mode, "off") == 0) {
            ota_stop();                        // "off" reply comes from ota_tick()
        } else {
            send_status("{\"ok\":false,\"err\":\"bad_mode\"}");
        }
        return;
    }

    if (strcmp(cmd, "update") == 0) {
        // Pull-engine commands (DESIGN §9): parse/enqueue only; the BLE owner
        // check was already enforced before ota_handle_ctrl() ran.
        ota_pull_handle_ctrl(json);
        return;
    }

    if (strcmp(cmd, "reboot") == 0) {
        // The link drops on reboot, so there is deliberately no TX reply (§2).
        enqueue(REQ_REBOOT, nullptr);
        return;
    }

    send_status("{\"ok\":false,\"err\":\"unknown_cmd\"}");
}

// ---- State machine ---------------------------------------------------------

void ota_tick(void) {
    // §4: 60 s of healthy uptime confirms the boot even without a payload.
    if (!s_boot_confirmed && (uint32_t)(millis() - s_boot_ms) >= HEALTHY_MS) {
        ota_confirm();
    }

    char pass[64] = {0};
    switch (dequeue(pass, sizeof(pass))) {
    case REQ_REBOOT:
        Serial.println("OTA: reboot requested");
        delay(50);
        esp_restart();
        break;

    case REQ_START:
        if (s_state == OTA_MODE_READY) {
            send_ready();   // already up — re-answer with the current address
        } else if (s_state == OTA_MODE_CONNECTING) {
            send_status("{\"ok\":false,\"err\":\"busy\"}");
        } else {
            s_ota_pass_set = (pass[0] != '\0');
            strlcpy(s_ota_pass, pass, sizeof(s_ota_pass));
            wifi_begin();
        }
        break;

    case REQ_STOP:
        ota_stop_now();
        send_off();
        break;

    case REQ_NONE:
    default:
        break;
    }

    if (s_state == OTA_MODE_CONNECTING) {
        if (WiFi.status() == WL_CONNECTED) {
            ota_server_begin(s_ota_pass_set ? s_ota_pass : nullptr);
            s_state = OTA_MODE_READY;
            send_ready();
        } else if ((int32_t)(millis() - s_connect_deadline) >= 0) {
            Serial.printf("OTA: wifi join failed (status=%d)\n", (int)WiFi.status());
            ota_stop_now();
            send_status("{\"ok\":false,\"err\":\"wifi_timeout\"}");
        }
    }

    if (s_state == OTA_MODE_READY) ArduinoOTA.handle();
}
