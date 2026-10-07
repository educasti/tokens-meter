// Runtime pairing + device-token storage. Hardware-only module.
//
// Contract: design/backend-wifi/PHASE2-CONTRACT.md sections 2 and 6.
//
// Lifecycle:
//   * usage_pair_init() loads the token from NVS (namespace "otah", key
//     "dev_token"). Once a token is stored the module is a no-op and the usage
//     pull owns the radio.
//   * While unpaired, usage_pair_tick() (Arduino loop task, non-blocking)
//     schedules pairing cycles on a dedicated worker: generate an 8-char code,
//     POST /api/pair/start, then poll GET /api/pair/status?code=... until the
//     owner approves it with `manage.py pair`. The token is returned exactly
//     once, stored in NVS, and the code is dropped.
//   * usage_pair_clear() wipes the token and re-enters pairing (used on a
//     GET /api/usage 401: server-side revocation).
//
// Radio: reuses OTA_WIFI_PULL + the pinned certificate, exactly like
// usage_pull.cpp. Never concurrent with OTA or the usage pull (the callers are
// mutually exclusive on usage_pair_has_token()).
//
// Secrets: the device token is only ever written to NVS and placed in the
// Authorization header. Neither the token nor the full URL is logged. The code
// is a public identifier and is safe to show/log.

#include "usage_pair.h"

// Base backend URL up to (not including) "/usage" or "/pair", e.g.
// "https://<vm-ip>/api". Injected by scripts/gen_usage_config.py from the
// untracked firmware/certs/usage_backend.json (or the environment). Empty ->
// pairing (and the usage pull) compile to a no-op.
#ifndef USAGE_BACKEND_BASE
#define USAGE_BACKEND_BASE ""
#endif

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_random.h>
#include <time.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/portmacro.h>

#include "ota.h"
#include "ota_pull.h"
#include "ota_wifi.h"
#include "net_worker.h"
#include "hal/board_caps.h"
#include "certs/pinned_server_pem.h"

// sizeof() of a string literal is 1 for "", so this is a compile-time test.
static constexpr bool kEnabled = sizeof(USAGE_BACKEND_BASE) > 1;

#define FIRST_PAIR_MS     5000u     // first pairing attempt this long after boot
#define BUSY_RETRY_MS     15000u    // radio owned by OTA: look again shortly
#define WIFI_JOIN_MS      20000u
#define SNTP_TIMEOUT_MS   15000u
#define HTTP_TIMEOUT_MS   15000u
#define MIN_VALID_EPOCH   1700000000L   // sane-clock floor (cert validity check)
#define CODE_TTL_MS       (15u * 60u * 1000u)   // server TTL (contract 2)

// Poll fast while a human is standing in front of the pairing screen; back off
// on transient failures so a down backend does not keep the radio hot.
#define PAIR_POLL_MS      3000u
static const uint32_t kFailBackoffMs[] = { 10000, 30000, 60000 };

#define BODY_MAX          256           // the status payload is small
#define TOKEN_MAX         128           // base64url 32 B -> 43 chars

// ---- NVS (shared hybrid-OTA namespace) --------------------------------------
#define NVS_NS   "otah"
#define K_SSID   "ssid"
#define K_PASS   "pass"
#define K_TOKEN  "dev_token"

// Unambiguous alphabet (no 0/O/1/I/L), contract section 2.
static const char kAlphabet[] = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";
#define CODE_LEN 8

enum pair_result_t {
    PG_PAIRED,    // token received and stored
    PG_PENDING,   // code started / still waiting for approval
    PG_REGEN,     // code unknown/expired/claimed: mint a fresh one
    PG_BUSY,      // radio owned by OTA
    PG_NO_WIFI,   // no stored WiFi credentials
    PG_FAIL,
};

enum pair_state_t { ST_IDLE, ST_PAIRING, ST_PAIRED, ST_ERROR };

static volatile bool     s_active = false;   // a cycle is queued or running
static volatile int      s_result = -1;      // pair_result_t of the last cycle
static portMUX_TYPE      s_mux = portMUX_INITIALIZER_UNLOCKED;

// Guarded by s_mux (s_state is a word-sized enum, safe to read unlocked).
static volatile bool     s_has_token = false;
static char              s_code[CODE_LEN + 1] = { 0 };
static volatile uint32_t s_code_ms = 0;      // when the current code was started
static volatile uint32_t s_ttl_ms = CODE_TTL_MS;
static volatile int      s_state = ST_IDLE;

// Loop-task-only scheduling.
static uint32_t s_next_ms = 0;
static uint8_t  s_fail_n = 0;

// ---- NVS helpers ------------------------------------------------------------

static bool load_token(char* out, size_t n) {
    out[0] = '\0';
    Preferences prefs;
    if (!prefs.begin(NVS_NS, true)) return false;
    String t = prefs.getString(K_TOKEN, "");
    prefs.end();
    strlcpy(out, t.c_str(), n);
    return out[0] != '\0';
}

static bool save_token(const char* token) {
    Preferences prefs;
    if (!prefs.begin(NVS_NS, false)) return false;
    size_t n = prefs.putString(K_TOKEN, token);
    prefs.end();
    return n > 0;
}

static void wipe_token(void) {
    Preferences prefs;
    if (!prefs.begin(NVS_NS, false)) return;
    prefs.remove(K_TOKEN);
    prefs.end();
}

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

// ---- Code + clock -----------------------------------------------------------

// 8 chars from the unambiguous alphabet, using the hardware RNG.
static void gen_code(char* out) {
    const uint32_t n = sizeof(kAlphabet) - 1;
    for (int i = 0; i < CODE_LEN; i++) {
        out[i] = kAlphabet[esp_random() % n];
    }
    out[CODE_LEN] = '\0';
}

static bool valid_token(const char* tok) {
    size_t n = strlen(tok);
    if (n < 16 || n >= TOKEN_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = tok[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                  c == '.' || c == '~' || c == '+' || c == '/';
        if (!ok) return false;
    }
    return true;
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

// The pinned cert has a validity window, so mbedTLS needs a real clock.
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

// ---- HTTP -------------------------------------------------------------------

static pair_result_t post_start(const char* code) {
    char url[192];
    snprintf(url, sizeof(url), "%s/pair/start", USAGE_BACKEND_BASE);

    WiFiClientSecure client;
    client.setCACert(PINNED_SERVER_PEM);   // pinned self-signed cert; never insecure
    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(client, url)) {
        http.end();
        return PG_FAIL;
    }
    char body[96];
    snprintf(body, sizeof(body), "{\"code\":\"%s\",\"board\":\"%s\"}", code,
             board_caps().id);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Accept", "application/json");

    int status = http.POST((uint8_t*)body, strlen(body));
    uint32_t ttl = CODE_TTL_MS;
    if (status == 200) {
        String resp = http.getString();
        JsonDocument doc;
        if (!deserializeJson(doc, resp)) {
            long exp = doc["expires_in"] | 0L;
            if (exp > 0) ttl = (uint32_t)exp * 1000u;
        }
    }
    http.end();

    if (status != 200) {
        // 400 bad_code should never happen with our alphabet; retry anyway.
        Serial.printf("PAIR: start failed (%d)\n", status);
        return PG_FAIL;
    }
    portENTER_CRITICAL(&s_mux);
    s_ttl_ms = ttl;
    portEXIT_CRITICAL(&s_mux);
    return PG_PENDING;
}

// One status poll. On PG_PAIRED, token_out holds the freshly minted token.
static pair_result_t get_status(const char* code, char* token_out, size_t token_n,
                                int* http_code) {
    token_out[0] = '\0';
    *http_code = 0;

    char url[256];
    snprintf(url, sizeof(url), "%s/pair/status?code=%s", USAGE_BACKEND_BASE, code);

    WiFiClientSecure client;
    client.setCACert(PINNED_SERVER_PEM);
    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(client, url)) {
        http.end();
        return PG_FAIL;
    }
    http.addHeader("Accept", "application/json");
    int status = http.GET();
    *http_code = status;

    if (status != 200) {
        http.end();
        // unknown_code (404) / expired (410): the code is unusable, mint a new
        // one. Anything else (429/5xx/-1) is a transient failure.
        return (status == 404 || status == 410) ? PG_REGEN : PG_FAIL;
    }

    String resp = http.getString();
    http.end();
    if (resp.length() == 0 || resp.length() > BODY_MAX) return PG_FAIL;

    JsonDocument doc;
    if (deserializeJson(doc, resp)) return PG_FAIL;
    const char* st = doc["state"] | "";
    if (strcmp(st, "paired") == 0) {
        const char* tok = doc["token"] | "";
        if (!valid_token(tok)) {
            Serial.println("PAIR: paired but token looks invalid");
            return PG_FAIL;
        }
        strlcpy(token_out, tok, token_n);
        return PG_PAIRED;
    }
    if (strcmp(st, "pending") == 0) return PG_PENDING;
    if (strcmp(st, "claimed") == 0) return PG_REGEN;   // token already handed out
    return PG_FAIL;
}

// Runs on the pairing worker with the radio held.
static pair_result_t pair_http(void) {
    bool fresh;
    char code[CODE_LEN + 1];
    uint32_t ttl;

    portENTER_CRITICAL(&s_mux);
    ttl = s_ttl_ms;
    fresh = (s_code[0] == '\0') ||
            ((uint32_t)(millis() - s_code_ms) >= ttl);
    if (!fresh) strlcpy(code, s_code, sizeof(code));
    portEXIT_CRITICAL(&s_mux);

    if (fresh) {
        char next[CODE_LEN + 1];
        gen_code(next);
        pair_result_t r = post_start(next);
        if (r != PG_PENDING) return r;
        portENTER_CRITICAL(&s_mux);
        strlcpy(s_code, next, sizeof(s_code));
        s_code_ms = millis();
        s_state = ST_PAIRING;
        portEXIT_CRITICAL(&s_mux);
        Serial.printf("PAIR: code %s - approve with the owner tool\n", next);
        return PG_PENDING;
    }

    char token[TOKEN_MAX];
    int http_code = 0;
    pair_result_t r = get_status(code, token, sizeof(token), &http_code);
    if (r == PG_PAIRED) {
        if (!save_token(token)) {
            Serial.println("PAIR: could not store the token in NVS");
            return PG_FAIL;
        }
        portENTER_CRITICAL(&s_mux);
        s_has_token = true;
        s_code[0] = '\0';
        s_state = ST_PAIRED;
        portEXIT_CRITICAL(&s_mux);
        Serial.println("PAIR: token stored - device paired");
        return PG_PAIRED;
    }
    if (r == PG_REGEN) {
        portENTER_CRITICAL(&s_mux);
        s_code[0] = '\0';   // next cycle mints a fresh code
        portEXIT_CRITICAL(&s_mux);
        Serial.printf("PAIR: code no longer valid (%d) - regenerating\n", http_code);
    }
    return r;
}

static pair_result_t run_cycle(void) {
    if (!ota_wifi_acquire(OTA_WIFI_PULL)) return PG_BUSY;   // OTA / hybrid owns the radio
    pair_result_t r;
    char ssid[64], pass[64];
    if (!load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        r = PG_NO_WIFI;
    } else if (!wifi_join(ssid, pass)) {
        Serial.println("PAIR: WiFi join failed");
        r = PG_FAIL;
    } else if (!clock_sync()) {
        Serial.println("PAIR: clock sync failed");
        r = PG_FAIL;
    } else {
        r = pair_http();
    }
    ota_wifi_release();
    return r;
}

// Job body for the shared network worker. One blocking pairing cycle; clears
// s_active when it returns so usage_pair_tick() may schedule the next one.
static void usage_pair_job(void* arg) {
    (void)arg;
    s_result = (int)run_cycle();
    s_active = false;
}

// ---- Public API -------------------------------------------------------------

void usage_pair_init(void) {
    // Load the token regardless of the build config so usage_pull can gate on
    // usage_pair_has_token() even on a no-op build.
    char token[TOKEN_MAX];
    load_token(token, sizeof(token));
    portENTER_CRITICAL(&s_mux);
    s_has_token = token[0] != '\0';
    portEXIT_CRITICAL(&s_mux);
    s_state = s_has_token ? ST_PAIRED : ST_IDLE;

    if (!kEnabled) {
        Serial.println("PAIR: disabled (no USAGE_BACKEND_BASE)");
        return;
    }

    s_next_ms = millis() + FIRST_PAIR_MS;
    // No per-module task: the blocking cycle runs on the shared net_worker task
    // (one 12 KB internal-RAM stack), created once in setup().
    Serial.printf("PAIR: %s\n", s_has_token ? "token loaded" : "unpaired, will pair");
}

void usage_pair_tick(void) {
    if (!kEnabled) return;
    if (s_has_token) return;              // paired: the usage pull owns the radio
    if (s_active) return;

    int r = s_result;
    if (r >= 0) {
        s_result = -1;
        if (r == PG_PAIRED) return;       // usage_pull takes over from here
        uint32_t wait_ms;
        if (r == PG_PENDING) {
            s_fail_n = 0;
            wait_ms = PAIR_POLL_MS;
        } else if (r == PG_REGEN) {
            // The code expired / was claimed; a fresh one is minted next cycle.
            s_fail_n = 0;
            wait_ms = PAIR_POLL_MS;
        } else if (r == PG_BUSY) {
            wait_ms = BUSY_RETRY_MS;
        } else if (r == PG_NO_WIFI) {
            wait_ms = 15000u;             // wait for provisioning (portal)
        } else {
            size_t n = sizeof(kFailBackoffMs) / sizeof(kFailBackoffMs[0]);
            wait_ms = kFailBackoffMs[s_fail_n < n ? s_fail_n : n - 1];
            if (s_fail_n < n) s_fail_n++;
            s_state = ST_ERROR;
        }
        s_next_ms = millis() + wait_ms;
        return;
    }

    if ((int32_t)(millis() - s_next_ms) < 0) return;

    // Never contend for the radio: leave it to OTA, and look again shortly.
    if (ota_is_active() || ota_pull_is_active() || ota_wifi_is_held() || net_worker_busy()) {
        s_next_ms = millis() + BUSY_RETRY_MS;
        return;
    }
    s_active = true;
    if (!net_worker_submit(usage_pair_job, nullptr)) s_active = false;
}

bool usage_pair_has_token(void) {
    return s_has_token;
}

const char* usage_pair_code(void) {
    static char out[CODE_LEN + 1];
    portENTER_CRITICAL(&s_mux);
    strlcpy(out, s_code, sizeof(out));
    portEXIT_CRITICAL(&s_mux);
    return out;
}

const char* usage_pair_state(void) {
    switch (s_state) {
    case ST_PAIRED:  return "paired";
    case ST_PAIRING: return "pairing";
    case ST_ERROR:   return "error";
    case ST_IDLE:
    default:         return "idle";
    }
}

void usage_pair_start(void) {
    if (!kEnabled) return;
    portENTER_CRITICAL(&s_mux);
    s_code[0] = '\0';
    s_state = s_has_token ? ST_PAIRED : ST_IDLE;
    portEXIT_CRITICAL(&s_mux);
    s_next_ms = millis();
    s_fail_n = 0;
}

void usage_pair_clear(void) {
    if (!kEnabled) return;
    wipe_token();
    portENTER_CRITICAL(&s_mux);
    s_has_token = false;
    s_code[0] = '\0';
    s_state = ST_IDLE;
    portEXIT_CRITICAL(&s_mux);
    s_next_ms = millis();
    s_fail_n = 0;
    Serial.println("PAIR: token cleared - re-pairing");
}
