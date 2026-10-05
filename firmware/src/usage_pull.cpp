// Backend-WiFi usage pull. Contracts: design/backend-wifi/PHASE1-CONTRACT.md
// section 7 and PHASE2-CONTRACT.md section 6. Hardware-only: the native sim
// links boards/sim/ota_sim.cpp instead (the sim env's build_src_filter excludes
// this file).
//
// Config is injected at build time by scripts/gen_usage_config.py from the
// untracked firmware/certs/usage_backend.json (or env vars): USAGE_BACKEND_BASE
// is the URL up to "/usage" (e.g. https://<vm-ip>/api). The device token is NOT
// a build macro anymore -- it is read from NVS (namespace "otah", key
// "dev_token"), written by usage_pair. With no base URL the whole module is a
// no-op and nothing touches the radio; while unpaired it skips the pull and
// lets usage_pair run the pairing poll.
//
// Threading:
//   * usage_pull_tick() runs on the Arduino loop task, is non-blocking, owns
//     the schedule, and is the only place ui_update() is called.
//   * usage_pull_task (12 KB internal stack, core 0) does the blocking
//     JOIN -> SNTP -> GET sequence, like ota_pull's worker, and stages the
//     parsed UsageData for the loop task under a spinlock.
//
// Trust: same pinned self-signed certificate as pull-OTA
// (certs/pinned_server_pem.h). Verification is always ON: setInsecure() is
// never called. The TLS host must be the bare IP literal in the cert's SAN,
// exactly as documented in ota_pull.cpp.
//
// Secrets: the token is only ever placed in the Authorization header. It is
// never logged, and neither is the URL's query/path beyond the host.

#include "usage_pull.h"

#ifndef USAGE_BACKEND_BASE
#define USAGE_BACKEND_BASE ""
#endif
#ifndef USAGE_POLL_S
#define USAGE_POLL_S 300
#endif

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/portmacro.h>

#include "data.h"
#include "ui.h"
#include "ota.h"
#include "ota_pull.h"
#include "ota_wifi.h"
#include "usage_pair.h"
#include "certs/pinned_server_pem.h"

// sizeof() of a string literal is 1 for "", so this is a compile-time test.
static constexpr bool kEnabled = sizeof(USAGE_BACKEND_BASE) > 1;

#define USAGE_TASK_STACK   12288
#define USAGE_TASK_PRIO    1
#define USAGE_TASK_CORE    0

#define FIRST_PULL_MS      10000u    // first pull this long after boot
#define BUSY_RETRY_MS      30000u    // radio owned by OTA / portal: look again
#define WIFI_JOIN_MS       20000u
#define SNTP_TIMEOUT_MS    15000u
#define HTTP_TIMEOUT_MS    15000u
#define MIN_VALID_EPOCH    1700000000L   // sane-clock floor (cert validity check)
#define BODY_MAX           512           // the payload is ~100 bytes

// Failed pulls retry sooner than the poll interval, then fall back to it.
static const uint32_t kFailBackoffMs[] = { 60000, 120000, 240000 };

// ---- NVS (WiFi creds + device token live in the hybrid-OTA namespace) -------
#define NVS_NS   "otah"
#define K_SSID   "ssid"
#define K_PASS   "pass"
#define K_TOKEN  "dev_token"

enum pull_result_t { PR_OK, PR_BUSY, PR_NO_WIFI, PR_FAIL, PR_NO_DATA, PR_UNPAIRED };

static SemaphoreHandle_t s_run_sem = nullptr;
static volatile bool     s_active = false;    // a cycle is queued or running
static volatile int      s_result = -1;       // pull_result_t of the last cycle, -1 = none yet
static portMUX_TYPE      s_mux = portMUX_INITIALIZER_UNLOCKED;
static UsageData         s_staged;            // guarded by s_mux
static volatile bool     s_staged_ready = false;

static uint32_t s_next_ms = 0;
static uint8_t  s_fail_n  = 0;

// ---- Worker -----------------------------------------------------------------

static bool load_creds(char* ssid, size_t sn, char* pass, size_t pn) {
    ssid[0] = '\0';
    pass[0] = '\0';
    Preferences prefs;
    if (!prefs.begin(NVS_NS, true)) return false;
    String s = prefs.getString(K_SSID, "");
    String p = prefs.getString(K_PASS, "");
    prefs.end();
    strlcpy(ssid, s.c_str(), sn);
    strlcpy(pass, p.c_str(), pn);
    return ssid[0] != '\0';
}

static bool load_token(char* out, size_t n) {
    out[0] = '\0';
    Preferences prefs;
    if (!prefs.begin(NVS_NS, true)) return false;
    String t = prefs.getString(K_TOKEN, "");
    prefs.end();
    strlcpy(out, t.c_str(), n);
    return out[0] != '\0';
}

static bool wifi_join(const char* ssid, const char* pass) {
    for (int attempt = 0; attempt < 2; attempt++) {
        WiFi.persistent(false);
        if (!WiFi.mode(WIFI_STA)) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        WiFi.begin(ssid, pass);
        uint32_t deadline = millis() + WIFI_JOIN_MS;
        while ((int32_t)(millis() - deadline) < 0) {
            if (WiFi.status() == WL_CONNECTED) return true;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        WiFi.disconnect(true);
    }
    return false;
}

// The pinned cert has a validity window, so mbedTLS needs a real clock. Skip
// the sync when it is already set (e.g. a previous cycle did it).
static bool clock_sync(void) {
    if ((long)time(nullptr) > MIN_VALID_EPOCH) return true;
    static const char* const srv[] = { "216.239.35.0", "162.159.200.1", "pool.ntp.org" };
    for (int attempt = 0; attempt < 3; attempt++) {
        configTime(0, 0, srv[attempt % 3], srv[(attempt + 1) % 3], srv[(attempt + 2) % 3]);
        uint32_t deadline = millis() + SNTP_TIMEOUT_MS;
        while ((int32_t)(millis() - deadline) < 0) {
            if ((long)time(nullptr) > MIN_VALID_EPOCH) return true;
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    return false;
}

// Parse the GET /api/usage body (contract 3.2) into a UsageData the same way
// main.cpp's parse_json() does for the BLE payload. Fields the backend does not
// carry (enterprise, clock, chime) keep their "absent" defaults.
static bool parse_body(const char* body, UsageData* out) {
    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    if (!(doc["ok"] | false)) return false;

    memset(out, 0, sizeof(*out));
    out->session_pct = doc["s"] | 0.0f;
    out->session_reset_mins = doc["sr"] | -1;
    out->weekly_pct = doc["w"] | 0.0f;
    out->weekly_reset_mins = doc["wr"] | -1;
    strlcpy(out->status, doc["st"] | "unknown", sizeof(out->status));
    out->chime = false;
    out->enterprise = false;
    out->period_days = 30;
    out->clock_epoch = 0;
    out->clock_fmt = 24;
    out->ok = true;
    out->valid = true;
    return true;
}

static pull_result_t do_get(void) {
    // The token lives in NVS (written by usage_pair), not in a build macro.
    char token[128];
    if (!load_token(token, sizeof(token))) return PR_UNPAIRED;

    char url[192];
    snprintf(url, sizeof(url), "%s/usage", USAGE_BACKEND_BASE);

    WiFiClientSecure client;
    client.setCACert(PINNED_SERVER_PEM);   // pinned self-signed cert; never insecure
    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(client, url)) {
        http.end();
        return PR_FAIL;
    }
    char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %s", token);
    http.addHeader("Authorization", auth);
    http.addHeader("Accept", "application/json");

    int code = http.GET();
    if (code == 404) {                     // {"ok":false,"err":"no_data"} (contract 3.2)
        http.end();
        Serial.println("USAGE: backend has no reading yet");
        return PR_NO_DATA;
    }
    if (code != 200) {
        http.end();
        if (code == 401) {                 // revoked server-side -> re-pair (phase 2)
            Serial.println("USAGE: 401 - device token revoked, clearing and re-pairing");
            usage_pair_clear();
            return PR_UNPAIRED;
        }
        Serial.printf("USAGE: GET failed (%d)\n", code);
        return PR_FAIL;
    }
    int len = http.getSize();
    if (len > BODY_MAX) {
        http.end();
        Serial.println("USAGE: response too large");
        return PR_FAIL;
    }
    String body = http.getString();
    http.end();
    if (body.length() == 0 || body.length() > BODY_MAX) return PR_FAIL;

    UsageData parsed;
    if (!parse_body(body.c_str(), &parsed)) {
        Serial.println("USAGE: bad response body");
        return PR_FAIL;
    }
    portENTER_CRITICAL(&s_mux);
    s_staged = parsed;
    s_staged_ready = true;
    portEXIT_CRITICAL(&s_mux);
    return PR_OK;
}

static pull_result_t run_cycle(void) {
    // Unpaired: usage_pair owns the radio for the pairing poll (phase 2).
    if (!usage_pair_has_token()) return PR_UNPAIRED;
    if (!ota_wifi_acquire(OTA_WIFI_PULL)) return PR_BUSY;   // OTA / hybrid owns the radio
    pull_result_t r;
    char ssid[64], pass[64];
    if (!load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        r = PR_NO_WIFI;
    } else if (!wifi_join(ssid, pass)) {
        Serial.println("USAGE: WiFi join failed");
        r = PR_FAIL;
    } else if (!clock_sync()) {
        Serial.println("USAGE: clock sync failed");
        r = PR_FAIL;
    } else {
        r = do_get();
    }
    ota_wifi_release();
    return r;
}

static void usage_pull_task(void* arg) {
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(s_run_sem, portMAX_DELAY) == pdTRUE) {
            s_result = (int)run_cycle();
            s_active = false;
        }
    }
}

// ---- Public API -------------------------------------------------------------

void usage_pull_init(void) {
    if (!kEnabled) return;                 // feature off: no task, no radio
    s_next_ms = millis() + FIRST_PULL_MS;
    s_run_sem = xSemaphoreCreateBinary();
    if (!s_run_sem) {
        Serial.println("USAGE: semaphore alloc failed");
        return;
    }
    // Stack MUST be in internal RAM (NVS/flash access from a PSRAM stack asserts).
    xTaskCreatePinnedToCore(usage_pull_task, "usage_pull", USAGE_TASK_STACK, nullptr,
                            USAGE_TASK_PRIO, nullptr, USAGE_TASK_CORE);
    Serial.printf("USAGE: pull enabled, every %us\n", (unsigned)USAGE_POLL_S);
}

void usage_pull_tick(void) {
    if (!kEnabled || !s_run_sem) return;

    // Apply a finished result on the loop task (LVGL is single-threaded).
    if (s_staged_ready) {
        UsageData d;
        portENTER_CRITICAL(&s_mux);
        d = s_staged;
        s_staged_ready = false;
        portEXIT_CRITICAL(&s_mux);
        ui_update(&d);
    }

    if (s_active) return;

    // Schedule the next pull from the outcome of the one that just finished.
    int r = s_result;
    if (r >= 0) {
        s_result = -1;
        uint32_t wait_ms;
        if (r == PR_OK || r == PR_NO_DATA) {
            s_fail_n = 0;
            wait_ms = (uint32_t)USAGE_POLL_S * 1000u;
        } else if (r == PR_BUSY) {
            wait_ms = BUSY_RETRY_MS;
        } else if (r == PR_UNPAIRED) {
            wait_ms = BUSY_RETRY_MS;   // pairing owns the radio; look again shortly
        } else {
            // Failure: retry sooner, then fall back to the poll interval. With
            // no stored WiFi there is nothing to retry until it is provisioned.
            wait_ms = (uint32_t)USAGE_POLL_S * 1000u;
            if (r == PR_FAIL && s_fail_n < sizeof(kFailBackoffMs) / sizeof(kFailBackoffMs[0])) {
                wait_ms = kFailBackoffMs[s_fail_n++];
            }
        }
        s_next_ms = millis() + wait_ms;
        return;
    }

    // Unpaired: usage_pair drives the pairing poll and owns the radio; do not
    // schedule a usage pull until a token is stored (phase 2 contract section 6).
    if (!usage_pair_has_token()) {
        s_next_ms = millis() + BUSY_RETRY_MS;
        return;
    }

    if ((int32_t)(millis() - s_next_ms) < 0) return;

    // Never contend for the radio: leave it to OTA, and look again shortly.
    if (ota_is_active() || ota_pull_is_active() || ota_wifi_is_held()) {
        s_next_ms = millis() + BUSY_RETRY_MS;
        return;
    }
    s_active = true;
    xSemaphoreGive(s_run_sem);
}

bool usage_pull_active(void) {
    return kEnabled && s_active;
}
