// Pull-based automatic OTA — device-initiated HTTPS update engine.
//
// Frozen contract: design/ota-pull/DESIGN.md section 7 (state machine) and
// design/ota-pull/IMPL.md section 2.3. Hardware-only module: the native sim
// links boards/sim/ota_sim.cpp instead (the sim env's build_src_filter excludes
// this file), exactly like ota.cpp.
//
// Threading contract:
//   * ota_pull_handle_ctrl() runs on the NimBLE host task and only parses /
//     enqueues a one-slot request; it never touches WiFi or flash.
//   * ota_pull_tick() runs on the Arduino loop task and is non-blocking: it
//     applies the boot / 24 h schedule, arbitrates the radio with the hybrid
//     path, and wakes the worker.
//   * the blocking JOIN → … → REBOOT sequence runs on the shared network worker
//     (net_worker.cpp: one 12 KB internal-RAM stack, core 0). It owns the only
//     long stack in the pull path so the loop task is never inflated by a TLS
//     handshake, and the stack is shared with the usage pull / pairing workers
//     instead of each module keeping its own resident 12 KB stack (which had
//     starved the internal heap and broken the manifest TLS handshake).
//
// Anti-brick invariants (§7.2, §11):
//   * never write the running slot — the target is asserted != running;
//   * abort before activation on any hash / image / signature failure;
//   * activate (esp_ota_set_boot_partition) is the single commit step and runs
//     only after the manifest SHA-256, the detached signature (when a signing
//     key is pinned; SIGNING.md §7) and esp_ota_end() all pass.

#include "ota_pull.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <string.h>

#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/portmacro.h>

#include "ota.h"
#include "ui.h"
#include "ble.h"
#include "ota_manifest.h"
#include "ota_semver.h"
#include "ota_sig.h"
#include "ota_wifi.h"
#include "net_worker.h"
#include "hal/board_caps.h"
#include "hal/power_hal.h"
#include "certs/pinned_server_pem.h"
#include "certs/signing_pubkey_pem.h"

// ---- TLS trust model (DESIGN §8) -------------------------------------------
// The update origin is a bare public IP, so there is no DNS name to chain to
// Let's Encrypt: the firmware pins the server's SELF-SIGNED certificate
// instead of the ISRG roots. `PINNED_SERVER_PEM` is generated at build time
// from the untracked `certs/pinned_server.pem` (scripts/gen_pinned_cert.py)
// and passed to WiFiClientSecure::setCACert() for both the manifest fetch and
// the binary download. Verification is always ON:
//   * `setInsecure()` is never called, and the CA slot is never left NULL, so
//     `ssl_client.cpp` configures MBEDTLS_SSL_VERIFY_REQUIRED;
//   * a missing cert yields a non-parseable placeholder -> the CA parse fails
//     and the handshake fails closed rather than trusting anything.
//
// Hostname check vs. an IP-address SAN: mbedTLS 3.6.x (arduino-esp32 3.3.8)
// DOES verify IPAddress Subject Alternative Names. It works only when the TLS
// host string is the bare IP literal (`x509_crt_check_san_ip()` runs the host
// through inet_pton and byte-compares it against the SAN). HTTPClient feeds
// the URL host to `set_hostname()`, so the requirement is:
//   * `-DOTA_PULL_MANIFEST_URL` must be `https://<same IP literal>/...` --
//     byte-for-byte the address in the cert's SAN, no DNS name, no trailing
//     dot, IPv6 without URL brackets;
//   * because a cert that carries a subjectAltName is matched against the SAN
//     ONLY (CN is ignored), the IP MUST be in the SAN, not just the CN.
// If a future toolchain drops IP-SAN support, do NOT relax verification: add a
// DNS name to the cert as a dNSName SAN and point the URL at that name. The
// pinned cert is the trust anchor either way.

// Base URL of the static firmware directory (DESIGN §6.4, §14.2). The real VM
// host is injected per build via -DOTA_PULL_MANIFEST_URL="https://<host>/firmware";
// the fallback is the reserved, non-routable .invalid TLD so no real host is
// ever committed to source.
#ifndef OTA_PULL_MANIFEST_URL
#define OTA_PULL_MANIFEST_URL "https://ota.invalid/firmware"
#endif

// ---- Tuning (DESIGN §7.1, §7.3, §10) ---------------------------------------
// The blocking worker is the shared net_worker task (one 12 KB internal-RAM
// stack, core 0, created once in setup); this module only submits jobs to it.
#define WIFI_JOIN_MS         20000u    // §7.1 WIFI_JOIN timeout
#define SNTP_TIMEOUT_MS      15000u    // §7.1 SNTP_TIME_SYNC timeout
#define MANIFEST_TIMEOUT_MS  15000u    // §7.1 FETCH_MANIFEST timeout
#define DOWNLOAD_IDLE_MS     15000u    // §7.1 DOWNLOAD idle-read timeout
#define DOWNLOAD_TOTAL_MS    300000u   // §7.1 DOWNLOAD total timeout
#define REBOOT_DELAY_MS      300u

#define HEALTHY_MS           60000u    // matches ota.cpp: boot is confirmed first
#define CHECK_INTERVAL_S     (24u * 60u * 60u)   // 24 h cadence (§10.1)
#define MIN_VALID_EPOCH      1700000000L         // ≈2023-11; sane-clock floor (§8.2)

#define CHECK_FLOOR_PCT      20        // §10.2 check floor
#define APPLY_FLOOR_PCT      50        // §10.2 apply floor when not charging
#define BATTERY_DEFER_S      1800      // back-off after a battery gate refusal

// ---- NVS (namespace "otah", shared with the hybrid path; DESIGN §14.1) ------
#define PULL_NS        "otah"
#define K_SSID         "ssid"
#define K_PASS         "pass"
#define K_AUTO         "auto"
#define K_LAST_CHK     "last_chk"
#define K_DEFER        "defer"
#define K_CHK_FAIL     "chk_fail"
#define K_PEND_VER     "pend_ver"
#define K_ETAG         "etag"
#define ETAG_MAX       40

// ---- Retry schedules (DESIGN §7.1 / §7.3) ----------------------------------
static const uint32_t kJoinBackoffMs[]     = { 5000, 15000, 45000 };  // after attempts 1,2
static const uint32_t kManifestBackoffMs[] = { 2000, 4000, 8000 };    // after attempts 1,2
static const uint32_t kCheckBackoffS[]     = { 5, 15, 45, 120, 600 }; // per failed check

// ---- State -----------------------------------------------------------------
enum pull_state_t {
    PS_IDLE,
    PS_JOIN,
    PS_SNTP,
    PS_MANIFEST,
    PS_COMPARE,
    PS_DOWNLOAD,
    PS_VERIFY,
    PS_ACTIVATE,
    PS_REBOOT,
};

// A queued check/update request. `apply == false` is `mode:"check"`.
struct pull_req_t {
    bool apply;
    bool force;
    bool from_ble;
    char to[OTA_VERSION_MAX];
};

static volatile pull_state_t s_state = PS_IDLE;

// One-slot handoff, BLE task → loop task (same pattern as ota.cpp lines 45-49).
static pull_req_t    s_ctrl_req;
static volatile bool s_ctrl_pending = false;
static portMUX_TYPE  s_ctrl_mux = portMUX_INITIALIZER_UNLOCKED;

// One-slot handoff, loop task → worker task. The loop only fills this while the
// worker is idle; net_worker_submit() failing is the synchronisation backstop.
static pull_req_t       s_run_req;

// Scheduling state (DESIGN §10.1).
static bool     s_auto = true;
static bool     s_boot_checked = false;
static uint32_t s_boot_ms = 0;
static uint32_t s_last_chk = 0;   // epoch of the last completed check
static uint32_t s_defer = 0;      // epoch before which no auto-check runs
static uint8_t  s_chk_fail = 0;   // consecutive failed checks
static char     s_etag[ETAG_MAX + 1] = { 0 };
static uint32_t s_next_sched_ms = 0;   // throttle for the NVS/time schedule poll

// ---- Small helpers ---------------------------------------------------------

static void pull_send(const char* json) {
    Serial.printf("OTA: pull TX %s\n", json);
    ble_notify_status(json);
}

static void pull_send_err(const char* err) {
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"err\":\"%s\"}", err ? err : "unknown");
    pull_send(buf);
}

static void pull_send_checking(void) {
    pull_send("{\"ok\":true,\"cmd\":\"update\",\"state\":\"checking\"}");
}

static void pull_send_up_to_date(const char* ver) {
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"cmd\":\"update\",\"state\":\"up_to_date\",\"version\":\"%s\"}", ver);
    pull_send(buf);
}

static void pull_send_available(const char* ver, long size) {
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"cmd\":\"update\",\"state\":\"available\",\"version\":\"%s\",\"size\":%ld}",
             ver, size);
    pull_send(buf);
}

// "Update <ver>" on the transient UI line when a newer build is available
// (DESIGN §12). Purely additive: the BLE reply above is the source of truth.
static void pull_ui_available(const char* ver) {
    char line[OTA_VERSION_MAX + 8];
    snprintf(line, sizeof(line), "Update %s", ver);
    ui_ota_status(line, -1);
}

static void pull_send_downloading(int pct) {
    ui_ota_status("Updating", pct);   // transient line; never blocks the worker
    char buf[96];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"cmd\":\"update\",\"state\":\"downloading\",\"pct\":%d}", pct);
    pull_send(buf);
}

static void pull_send_verifying(void) {
    pull_send("{\"ok\":true,\"cmd\":\"update\",\"state\":\"verifying\"}");
}

static void pull_send_rebooting(const char* ver) {
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"cmd\":\"update\",\"state\":\"rebooting\",\"version\":\"%s\"}", ver);
    pull_send(buf);
}

static bool pull_load_creds(char* ssid, size_t sn, char* pass, size_t pn) {
    ssid[0] = '\0';
    pass[0] = '\0';
    Preferences prefs;
    if (!prefs.begin(PULL_NS, true)) return false;
    String s = prefs.getString(K_SSID, "");
    String p = prefs.getString(K_PASS, "");
    prefs.end();
    strlcpy(ssid, s.c_str(), sn);
    strlcpy(pass, p.c_str(), pn);
    return ssid[0] != '\0';
}

static void pull_nvs_u32(const char* key, uint32_t value) {
    Preferences prefs;
    if (!prefs.begin(PULL_NS, false)) return;
    prefs.putUInt(key, value);
    prefs.end();
}

static void pull_nvs_u8(const char* key, uint8_t value) {
    Preferences prefs;
    if (!prefs.begin(PULL_NS, false)) return;
    prefs.putUChar(key, value);
    prefs.end();
}

// A completed, successful check.
static void pull_record_ok(void) {
    time_t now = time(nullptr);
    if ((long)now > MIN_VALID_EPOCH) s_last_chk = (uint32_t)now;
    s_defer = 0;
    s_chk_fail = 0;
    pull_nvs_u32(K_LAST_CHK, s_last_chk);
    pull_nvs_u32(K_DEFER, 0);
    pull_nvs_u8(K_CHK_FAIL, 0);
    s_boot_checked = true;
}

// A completed check with a permanently bad manifest/image — do not retry it.
static void pull_record_terminal(void) {
    ui_ota_status("Update failed", -1);   // short transient line, DESIGN §12
    time_t now = time(nullptr);
    if ((long)now > MIN_VALID_EPOCH) s_last_chk = (uint32_t)now;
    s_defer = 0;
    s_chk_fail = 0;
    pull_nvs_u32(K_LAST_CHK, s_last_chk);
    pull_nvs_u32(K_DEFER, 0);
    pull_nvs_u8(K_CHK_FAIL, 0);
    s_boot_checked = true;
}

// A transient failure: exponential back-off, persist `defer` so a reboot
// mid-backoff does not hammer the VM (§7.3).
static void pull_record_transient(void) {
    time_t now = time(nullptr);
    uint32_t epoch = ((long)now > MIN_VALID_EPOCH) ? (uint32_t)now : 0;
    if (s_chk_fail < 255) s_chk_fail++;
    if (s_chk_fail >= 5) {
        // Give up until the next cadence.
        s_chk_fail = 0;
        if (epoch) s_last_chk = epoch;
        s_defer = 0;
    } else if (epoch) {
        s_defer = epoch + kCheckBackoffS[s_chk_fail - 1];
    }
    pull_nvs_u32(K_LAST_CHK, s_last_chk);
    pull_nvs_u32(K_DEFER, s_defer);
    pull_nvs_u8(K_CHK_FAIL, s_chk_fail);
    s_boot_checked = true;
}

static void pull_defer_battery(void) {
    time_t now = time(nullptr);
    if ((long)now > MIN_VALID_EPOCH) {
        s_defer = (uint32_t)now + BATTERY_DEFER_S;
        pull_nvs_u32(K_DEFER, s_defer);
    }
}

static void pull_fail_transient(const pull_req_t& req, const char* err) {
    pull_record_transient();
    if (req.from_ble) {
        pull_send_err(err);
    } else {
        Serial.printf("OTA: pull check failed (%s), deferred\n", err ? err : "unknown");
    }
}

// Compile-time board check against the manifest, but with the running version
// parsed leniently: a dev build ("0.0.0-dev") is not strict semver, so it is
// treated as 0.0.0 for comparison instead of poisoning every result.
static int pull_version_cmp(const char* newer, const char* running) {
    int parsed[3];
    if (!semver_parse(running, parsed)) return semver_cmp(newer, "0.0.0");
    return semver_cmp(newer, running);
}

// Battery / charging gate (DESIGN §10.2). `for_apply` selects the higher floor;
// `bypass` (owner force or manifest mandatory) lowers it to the check floor.
static bool pull_battery_ok(bool for_apply, bool bypass) {
    if (!board_caps().has_battery) return true;
    int pct = power_hal_battery_pct();
    if (pct < 0) return true;  // unknown — do not block on a missing reading
    if (power_hal_is_charging() || power_hal_is_vbus_in()) return true;
    int floor = (for_apply && !bypass) ? APPLY_FLOOR_PCT : CHECK_FLOOR_PCT;
    return pct >= floor;
}

static const char* pull_manifest_err_name(int e) {
    switch (e) {
    case OTA_MF_BAD_SCHEMA:  return "bad_schema";
    case OTA_MF_BAD_BOARD:   return "board_mismatch";
    case OTA_MF_BAD_VERSION: return "bad_version";
    case OTA_MF_BAD_URL:     return "bad_url";
    case OTA_MF_BAD_SHA:     return "bad_schema";
    case OTA_MF_BAD_SIZE:    return "bad_schema";
    case OTA_MF_BAD_SIG:     return "bad_signature";
    case OTA_MF_BAD_JSON:    return "bad_schema";
    default:                 return "bad_schema";
    }
}

// ---- WiFi join / SNTP ------------------------------------------------------

static bool pull_wifi_join(const char* ssid, const char* pass) {
    for (int attempt = 0; attempt < 3; attempt++) {
        Serial.printf("OTA: pull join attempt %d heap=%u max=%u\n",
                      attempt + 1, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
        WiFi.persistent(false);
        if (!WiFi.mode(WIFI_STA)) {
            Serial.println("OTA: WiFi.mode(WIFI_STA) failed (radio/heap)");
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        WiFi.begin(ssid, pass);
        uint32_t deadline = millis() + WIFI_JOIN_MS;
        while ((int32_t)(millis() - deadline) < 0) {
            if (WiFi.status() == WL_CONNECTED) {
                Serial.printf("OTA: pull WiFi up (%s)\n", WiFi.localIP().toString().c_str());
                return true;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        Serial.printf("OTA: pull join attempt %d failed (status=%d)\n",
                      attempt + 1, (int)WiFi.status());
        WiFi.disconnect(true);
        if (attempt < 2) vTaskDelay(pdMS_TO_TICKS(kJoinBackoffMs[attempt]));
    }
    return false;
}

static bool pull_sntp(void) {
    // Use NTP server IPs, not names: on some LANs DNS is slow or the first
    // configTime() has nothing to resolve, so the sync times out. Three
    // well-known anycast IPs (Google / Cloudflare) plus a named fallback.
    static const char* const srv[] = {
        "216.239.35.0",     // time.google.com
        "162.159.200.1",    // time.cloudflare.com
        "pool.ntp.org",
    };
    for (int attempt = 0; attempt < 4; attempt++) {
        configTime(0, 0, srv[attempt % 3], srv[(attempt + 1) % 3], srv[(attempt + 2) % 3]);
        uint32_t deadline = millis() + SNTP_TIMEOUT_MS;
        while ((int32_t)(millis() - deadline) < 0) {
            if ((long)time(nullptr) > MIN_VALID_EPOCH) {
                Serial.printf("OTA: pull clock synced (%ld)\n", (long)time(nullptr));
                return true;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        Serial.printf("OTA: pull SNTP attempt %d timed out\n", attempt + 1);
    }
    return false;
}

// ---- Manifest fetch --------------------------------------------------------

enum fetch_res_t { FETCH_OK, FETCH_304, FETCH_TLS, FETCH_HTTP, FETCH_PARSE };

static fetch_res_t pull_fetch_manifest(OtaManifest* mf, int* http_code, int* parse_err) {
    *http_code = 0;
    *parse_err = OTA_MF_OK;

    char url[OTA_URL_MAX + 64];
    snprintf(url, sizeof(url), "%s/%s/manifest.json",
             OTA_PULL_MANIFEST_URL, board_caps().id);
    Serial.printf("OTA: pull GET %s\n", url);

    WiFiClientSecure client;
    client.setCACert(PINNED_SERVER_PEM);   // pinned self-signed cert, §8
    HTTPClient http;
    http.setTimeout(MANIFEST_TIMEOUT_MS);
    if (!http.begin(client, url)) {
        http.end();
        return FETCH_TLS;
    }
    if (s_etag[0]) http.addHeader("If-None-Match", s_etag);
    int code = http.GET();
    *http_code = code;

    if (code == 304) {           // ETag still valid → nothing to update (§6.3)
        http.end();
        return FETCH_304;
    }
    if (code != 200) {
        Serial.printf("OTA: pull manifest HTTP %d\n", code);
        http.end();
        return (code < 0) ? FETCH_TLS : FETCH_HTTP;
    }

    String body = http.getString();
    String etag = http.header("ETag");
    http.end();

    if (body.length() == 0) return FETCH_HTTP;
    ota_manifest_err pe = ota_manifest_parse(body.c_str(), mf);
    if (pe != OTA_MF_OK) {
        *parse_err = (int)pe;
        Serial.printf("OTA: pull manifest parse err %d\n", (int)pe);
        return FETCH_PARSE;
    }
    if (etag.length() > 0) {     // remember for If-None-Match
        strlcpy(s_etag, etag.c_str(), sizeof(s_etag));
        Preferences prefs;
        if (prefs.begin(PULL_NS, false)) {
            prefs.putString(K_ETAG, s_etag);
            prefs.end();
        }
    }
    return FETCH_OK;
}

// ---- Compare ---------------------------------------------------------------

enum compare_res_t { CR_APPLY, CR_CHECK_ONLY, CR_UP_TO_DATE, CR_BATTERY_LOW, CR_REJECT };

static compare_res_t pull_compare(const pull_req_t& req, const OtaManifest& mf,
                                  const char** err_out) {
    // Never flash a foreign board (DESIGN §6.2).
    if (strcmp(mf.board, board_caps().id) != 0) {
        *err_out = "board_mismatch";
        return CR_REJECT;
    }
    // Owner force must name the manifest version exactly (§5.4).
    if (req.force && req.to[0] && strcmp(req.to, mf.version) != 0) {
        *err_out = "target_mismatch";
        return CR_REJECT;
    }
    // released_at sanity: reject a manifest more than 24 h in the future (§6.2).
    time_t now = time(nullptr);
    if (mf.released_at[0] && (long)now > MIN_VALID_EPOCH &&
        !ota_released_at_sane(mf.released_at, (long)now)) {
        *err_out = "bad_schema";
        return CR_REJECT;
    }

    int cmp = pull_version_cmp(mf.version, ota_version());
    if (cmp <= 0 && !req.force) {
        return CR_UP_TO_DATE;   // equal or older is a terminal no-update (§5.4)
    }
    // min_from is never bypassed by force.
    if (!req.force && mf.min_from[0] &&
        pull_version_cmp(mf.min_from, ota_version()) > 0) {
        *err_out = "too_old";
        return CR_REJECT;
    }

    // Early signature policy gate (SIGNING.md §4 rule 2), decided HERE in
    // COMPARE rather than after the download: a pinned key with no `sig` in the
    // manifest can never activate, so reject it before the ~3 MB transfer
    // (CR_REJECT -> pull_send_err("bad_signature") + pull_record_terminal(),
    // the same terminal class as `hash_mismatch`). A present-but-invalid `sig`
    // is still caught by the cryptographic check in pull_verify(). With no key
    // pinned (SIGNING_PUBKEY_PEM empty) this is skipped: hash-only, unchanged.
    if (SIGNING_PUBKEY_PEM[0] != '\0' && mf.sig[0] == '\0') {
        *err_out = "bad_signature";
        return CR_REJECT;
    }

    // A check is only reporting: it never needs the heavier apply floor.
    if (!req.apply) return CR_CHECK_ONLY;

    bool bypass = req.force || mf.mandatory;   // battery/priority gates only
    if (!pull_battery_ok(true, bypass)) return CR_BATTERY_LOW;

    return CR_APPLY;
}

// ---- Download + verify -----------------------------------------------------

struct dl_ctx {
    const esp_partition_t* part;
    esp_ota_handle_t       handle;
    mbedtls_sha256_context sha;
    bool                   sha_open;
    bool                   ota_open;
    bool                   force_bypass;
    long                   total;
    char                   err[24];
};

static void dl_set_err(dl_ctx* c, const char* e) {
    strlcpy(c->err, e ? e : "timeout", sizeof(c->err));
}

static void pull_download_cleanup(dl_ctx* c) {
    if (c->sha_open) {
        mbedtls_sha256_free(&c->sha);
        c->sha_open = false;
    }
    if (c->ota_open) {
        esp_ota_abort(c->handle);
        c->ota_open = false;
    }
}

// One attempt: GET the binary and stream it into the inactive slot while
// updating a streaming SHA-256. Leaves the OpenOTA handle and digest open for
// pull_verify() on success.
static bool pull_download_once(dl_ctx* c, const OtaManifest& mf) {
    const esp_partition_t* part = esp_ota_get_next_update_partition(NULL);
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!part || part == running) {   // anti-brick invariant §7.2
        Serial.println("OTA: pull no valid inactive slot");
        dl_set_err(c, "bad_image");
        return false;
    }
    c->part = part;

    char url[OTA_URL_MAX + 64];
    snprintf(url, sizeof(url), "%s/%s/%s",
             OTA_PULL_MANIFEST_URL, board_caps().id, mf.url);
    Serial.printf("OTA: pull GET %s (%ld bytes)\n", url, mf.size);

    WiFiClientSecure client;
    client.setCACert(PINNED_SERVER_PEM);   // pinned self-signed cert, §8
    HTTPClient http;
    http.setTimeout(DOWNLOAD_IDLE_MS);
    if (!http.begin(client, url)) {
        http.end();
        dl_set_err(c, "tls_fail");
        return false;
    }
    int code = http.GET();
    if (code != 200) {
        Serial.printf("OTA: pull bin HTTP %d\n", code);
        http.end();
        dl_set_err(c, (code == 404) ? "http_404" : (code < 0 ? "tls_fail" : "timeout"));
        return false;
    }
    int clen = http.getSize();
    if (clen >= 0 && (long)clen != mf.size) {
        Serial.printf("OTA: pull size mismatch clen=%d manifest=%ld\n", clen, mf.size);
        http.end();
        dl_set_err(c, "bad_image");
        return false;
    }

    if (esp_ota_begin(part, OTA_SIZE_UNKNOWN, &c->handle) != ESP_OK) {
        Serial.println("OTA: pull esp_ota_begin failed");
        http.end();
        dl_set_err(c, "bad_image");
        return false;
    }
    c->ota_open = true;

    mbedtls_sha256_init(&c->sha);
    mbedtls_sha256_starts(&c->sha, 0);
    c->sha_open = true;

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[4096];
    long total = 0;
    int last_pct = -1;
    uint32_t started = millis();
    uint32_t last_data = started;
    bool failed = false;

    while (total < mf.size) {
        if ((uint32_t)(millis() - started) > DOWNLOAD_TOTAL_MS) { dl_set_err(c, "timeout"); failed = true; break; }
        if ((uint32_t)(millis() - last_data) > DOWNLOAD_IDLE_MS) { dl_set_err(c, "timeout"); failed = true; break; }
        if (!pull_battery_ok(true, c->force_bypass)) { dl_set_err(c, "battery_low"); failed = true; break; }

        size_t want = sizeof(buf);
        if ((long)want > mf.size - total) want = (size_t)(mf.size - total);
        size_t n = stream->readBytes(buf, want);
        if (n == 0) {
            if (!http.connected()) { dl_set_err(c, "timeout"); failed = true; break; }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        last_data = millis();
        if (esp_ota_write(c->handle, buf, n) != ESP_OK) { dl_set_err(c, "bad_image"); failed = true; break; }
        mbedtls_sha256_update(&c->sha, buf, n);
        total += (long)n;

        int pct = (int)((total * 100) / (mf.size > 0 ? mf.size : 1));
        if (pct != last_pct && (pct == 100 || pct - last_pct >= 5)) {
            last_pct = pct;
            pull_send_downloading(pct);
        }
    }
    http.end();

    if (failed) return false;
    if (total != mf.size) {   // short body without an explicit stream error
        dl_set_err(c, "timeout");
        return false;
    }
    c->total = total;
    return true;
}

static bool pull_download(dl_ctx* c, const OtaManifest& mf, bool bypass) {
    memset(c, 0, sizeof(*c));
    c->force_bypass = bypass;

    for (int attempt = 0; attempt < 2; attempt++) {   // §7.1: 2× whole download
        if (pull_download_once(c, mf)) return true;
        bool battery_low = (strcmp(c->err, "battery_low") == 0);
        char saved[sizeof(c->err)];
        strlcpy(saved, c->err, sizeof(saved));
        pull_download_cleanup(c);
        memset(c, 0, sizeof(*c));
        c->force_bypass = bypass;
        strlcpy(c->err, saved, sizeof(c->err));

        if (battery_low || attempt >= 1) return false;
        Serial.printf("OTA: pull download attempt %d failed (%s), retrying\n",
                      attempt + 1, c->err);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    return false;
}

// Detached-signature check (design/ota-pull/SIGNING.md section 7). Runs over
// the SAME 32-byte SHA-256 digest the hash check just validated: the signature
// authenticates the digest, the digest authenticates the bytes.
//
// Policy (SIGNING.md section 4 rule 2):
//   * no key pinned (SIGNING_PUBKEY_PEM empty): pre-P4 hash-only build; the
//     check is skipped and logged.
//   * key pinned, manifest unsigned: rejected as `bad_signature`. This case is
//     normally decided earlier, at COMPARE (pull_compare), so no download is
//     started; the check below stays as a defensive backstop for any future
//     caller that reaches VERIFY without passing through COMPARE.
//   * key pinned, manifest signed: `ota_sig_verify_p256()` must accept it.
// On any rejection the caller aborts the OTA handle and never activates.
static bool pull_verify_signature(const OtaManifest& mf, const uint8_t* digest,
                                  const char** err) {
    const bool have_key = (SIGNING_PUBKEY_PEM[0] != '\0');

    // Pure policy gate shared with the host test: no key -> always ok. The
    // "pinned key but no sig" case is normally already rejected at COMPARE
    // (before the download); reaching it here would mean a caller bypassed that
    // gate, so it still hard-fails rather than falling through to crypto.
    if (!ota_manifest_sig_ok(&mf, have_key)) {
        Serial.println("OTA: pull manifest unsigned but a signing key is pinned, rejecting");
        *err = "bad_signature";
        return false;
    }
    if (!have_key) {
        Serial.println("OTA: pull signing not configured, hash-only verification");
        return true;
    }

    // sig_alg and base64 syntax were enforced by the parser (OTA_MF_BAD_SIG);
    // decode to DER here. A 128-byte buffer covers the largest P-256 DER sig.
    uint8_t sig_der[OTA_SIG_B64_MAX];
    int sig_len = ota_base64_decode(mf.sig, sig_der, sizeof(sig_der));
    if (sig_len <= 0) {
        Serial.println("OTA: pull manifest sig base64 decode failed");
        *err = "bad_signature";
        return false;
    }

    if (!ota_sig_verify_p256(digest, 32, sig_der, (size_t)sig_len, SIGNING_PUBKEY_PEM)) {
        Serial.printf("OTA: pull signature verify failed (key_id=%s)\n", mf.key_id);
        *err = "bad_signature";
        return false;
    }
    Serial.printf("OTA: pull signature OK (alg=%s key_id=%s)\n", mf.sig_alg, mf.key_id);
    return true;
}

// Finish the digest, compare against the manifest, verify the detached
// signature (when pinned), then validate the image. The hash and signature are
// checked before esp_ota_end() so a failure can esp_ota_abort() the handle
// (DESIGN §7.1 / §11, SIGNING.md §7) — nothing is activated either way.
static bool pull_verify(dl_ctx* c, const OtaManifest& mf, const char** err) {
    uint8_t digest[32];
    mbedtls_sha256_finish(&c->sha, digest);
    mbedtls_sha256_free(&c->sha);
    c->sha_open = false;

    char hex[OTA_SHA256_HEX_LEN + 1];
    for (int i = 0; i < 32; i++) {
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    hex[OTA_SHA256_HEX_LEN] = '\0';

    if (!ota_sha256_hex_eq(hex, mf.sha256)) {
        Serial.println("OTA: pull SHA-256 mismatch, aborting");
        esp_ota_abort(c->handle);
        c->ota_open = false;
        *err = "hash_mismatch";
        return false;
    }

    // Signature check: after SHA-256, before esp_ota_end()/activation. A
    // failure is terminal (`bad_signature`), same class as `hash_mismatch`.
    if (!pull_verify_signature(mf, digest, err)) {
        esp_ota_abort(c->handle);
        c->ota_open = false;
        return false;
    }

    esp_err_t e = esp_ota_end(c->handle);
    c->ota_open = false;
    if (e != ESP_OK) {
        Serial.printf("OTA: pull esp_ota_end failed (%d)\n", (int)e);
        *err = "bad_image";
        return false;
    }
    return true;
}

// ---- Activate --------------------------------------------------------------

static bool pull_activate(const OtaManifest& mf, const esp_partition_t* part) {
    // Remember the pending version before touching otadata; ota_confirm()
    // clears it once the new image boots (DESIGN §7.2).
    Preferences prefs;
    if (prefs.begin(PULL_NS, false)) {
        prefs.putString(K_PEND_VER, mf.version);
        prefs.end();
    }
    esp_err_t e = esp_ota_set_boot_partition(part);
    if (e != ESP_OK) {
        Serial.printf("OTA: pull set_boot_partition failed (%d)\n", (int)e);
        return false;
    }
    return true;
}

// ---- Worker ----------------------------------------------------------------

// Runs on the OTA pull worker (never the loop or NimBLE task). The
// ui_ota_status() calls below are the only UI interaction: each one just
// stashes a short line for the loop task (DESIGN §12), so they are
// non-blocking and a dark or sleeping panel never affects control flow.
static void pull_cycle_inner(const pull_req_t& req) {
    char ssid[64] = { 0 }, pass[64] = { 0 };
    if (!pull_load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        pull_send_err("no_wifi");
        return;
    }

    s_state = PS_JOIN;
    ui_ota_status("Checking", -1);
    if (!pull_wifi_join(ssid, pass)) { pull_fail_transient(req, "timeout"); return; }

    s_state = PS_SNTP;
    ui_ota_status("Checking", -1);
    if (!pull_sntp()) { pull_fail_transient(req, "timeout"); return; }

    OtaManifest mf;
    memset(&mf, 0, sizeof(mf));
    int http_code = 0, parse_err = OTA_MF_OK;
    s_state = PS_MANIFEST;
    ui_ota_status("Checking", -1);
    fetch_res_t fr = pull_fetch_manifest(&mf, &http_code, &parse_err);
    if (fr == FETCH_304) {
        pull_send_up_to_date(ota_version());
        pull_record_ok();
        return;
    }
    if (fr != FETCH_OK) {
        const char* e = "timeout";
        bool terminal = false;
        if (fr == FETCH_PARSE) {
            e = pull_manifest_err_name(parse_err);
            terminal = true;
        } else if (fr == FETCH_HTTP) {
            if (http_code == 404) { e = "http_404"; terminal = true; }
        } else {  // FETCH_TLS
            e = "tls_fail";
        }
        if (terminal) { pull_send_err(e); pull_record_terminal(); }
        else          { pull_fail_transient(req, e); }
        return;
    }

    s_state = PS_COMPARE;
    const char* cerr = "no_update";
    compare_res_t cr = pull_compare(req, mf, &cerr);
    if (cr == CR_UP_TO_DATE) {
        pull_send_up_to_date(ota_version());   // report what is actually running
        pull_record_ok();
        return;
    }
    if (cr == CR_REJECT) {
        pull_send_err(cerr);
        pull_record_terminal();
        return;
    }
    if (cr == CR_BATTERY_LOW) {
        if (req.from_ble && req.apply) pull_send_err("battery_low");
        else {
            pull_send_available(mf.version, mf.size);
            pull_ui_available(mf.version);
        }
        pull_defer_battery();
        return;
    }
    pull_send_available(mf.version, mf.size);
    pull_ui_available(mf.version);
    if (cr == CR_CHECK_ONLY) {
        pull_record_ok();
        return;
    }

    dl_ctx dl;
    s_state = PS_DOWNLOAD;
    ui_ota_status("Updating", -1);
    if (!pull_download(&dl, mf, req.force || mf.mandatory)) {
        const char* e = dl.err[0] ? dl.err : "timeout";
        bool battery_low = (strcmp(e, "battery_low") == 0);
        if (battery_low) {
            if (req.from_ble) pull_send_err("battery_low");
            else {
                pull_send_available(mf.version, mf.size);
                pull_ui_available(mf.version);
            }
            pull_defer_battery();
        } else {
            pull_fail_transient(req, e);
        }
        return;
    }

    s_state = PS_VERIFY;
    ui_ota_status("Verifying", -1);
    pull_send_verifying();
    const char* verr = "bad_image";
    if (!pull_verify(&dl, mf, &verr)) {
        // hash_mismatch / bad_signature / bad_image are non-retryable (§7.3).
        pull_send_err(verr);
        pull_record_terminal();
        return;
    }

    s_state = PS_ACTIVATE;
    if (!pull_activate(mf, dl.part)) {
        pull_send_err("activate_fail");
        pull_record_terminal();
        return;
    }

    s_state = PS_REBOOT;
    ui_ota_status("Restarting", -1);
    pull_send_rebooting(mf.version);
    pull_record_ok();
    ota_wifi_release();                 // radio off before the reset (§10.3)
    vTaskDelay(pdMS_TO_TICKS(REBOOT_DELAY_MS));
    Serial.printf("OTA: pull rebooting into %s\n", mf.version);
    esp_restart();
}

static void pull_run_cycle(void) {
    pull_req_t req = s_run_req;   // worker is idle; the loop wrote this before waking us
    if (!ota_wifi_acquire(OTA_WIFI_PULL)) {
        pull_send_err("busy");
        s_state = PS_IDLE;
        return;
    }
    pull_cycle_inner(req);
    ota_wifi_release();
    s_state = PS_IDLE;
}

// Job body for the shared network worker (net_worker.cpp). Runs with the one
// shared 12 KB internal-RAM stack; s_state returns to PS_IDLE when it returns.
static void ota_pull_job(void* arg) {
    (void)arg;
    pull_run_cycle();
}

// ---- Public API ------------------------------------------------------------

void ota_pull_init(void) {
    s_boot_ms = millis();

    Preferences prefs;
    if (prefs.begin(PULL_NS, true)) {
        s_auto = prefs.getUChar(K_AUTO, 1) != 0;
        s_last_chk = prefs.getUInt(K_LAST_CHK, 0);
        s_defer = prefs.getUInt(K_DEFER, 0);
        s_chk_fail = prefs.getUChar(K_CHK_FAIL, 0);
        String etag = prefs.getString(K_ETAG, "");
        strlcpy(s_etag, etag.c_str(), sizeof(s_etag));
        prefs.end();
    }

    // No per-module task: the blocking worker is the shared net_worker task,
    // created once in setup(). This keeps only ONE 12 KB internal-RAM stack
    // resident for all three pull paths.

    Serial.printf("OTA: pull init auto=%d last_chk=%lu defer=%lu chk_fail=%u etag=%s\n",
                  (int)s_auto, (unsigned long)s_last_chk, (unsigned long)s_defer,
                  (unsigned)s_chk_fail, s_etag[0] ? s_etag : "-");
}

void ota_pull_tick(void) {
    if (s_state != PS_IDLE || ota_is_active()) return;   // busy or hybrid owns the radio
    // Do not consume a one-shot BLE request while the shared worker is still
    // busy with another pull; the request stays queued in s_ctrl_pending.
    if (net_worker_busy()) return;

    pull_req_t req = {};
    bool have = false;
    bool is_ble = false;

    // A queued BLE request takes priority over the schedule and is answered
    // promptly; the schedule only needs second resolution.
    portENTER_CRITICAL(&s_ctrl_mux);
    if (s_ctrl_pending) {
        req = s_ctrl_req;
        s_ctrl_pending = false;
        have = true;
        is_ble = true;
    }
    portEXIT_CRITICAL(&s_ctrl_mux);

    if (!have) {
        if (!s_auto) return;
        if ((int32_t)(millis() - s_next_sched_ms) < 0) return;
        s_next_sched_ms = millis() + 5000;

        if ((uint32_t)(millis() - s_boot_ms) < HEALTHY_MS) return;   // boot confirmed first

        time_t now = time(nullptr);
        bool clock_ok = (long)now > MIN_VALID_EPOCH;
        if (s_defer && clock_ok && (uint32_t)now < s_defer) return;  // backoff window

        char ssid[64], pass[64];
        if (!pull_load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) return;  // no creds

        bool boot_due = !s_boot_checked;
        bool timer_due = clock_ok &&
                         (s_last_chk == 0 ||
                          ((uint32_t)now - s_last_chk) >= CHECK_INTERVAL_S);
        if (!boot_due && !timer_due) return;

        req.apply = true;
        req.force = false;
        req.from_ble = false;
        req.to[0] = '\0';
        have = true;
    }
    if (!have) return;

    // Gates run here on the loop task, never on the NimBLE task. The floor is
    // the cheap check floor (20 %); the heavier apply floor (50 %) is applied
    // later at COMPARE, so a check can still report `available` (§10.2).
    char ssid[64], pass[64];
    if (!pull_load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        if (is_ble) pull_send_err("no_wifi");
        return;
    }
    bool applies = req.from_ble ? req.apply : true;   // BLE checks bypass the floor
    if (applies && !pull_battery_ok(false, req.force)) {
        if (is_ble) {
            pull_send_err("battery_low");
        } else {
            pull_defer_battery();
            Serial.println("OTA: pull auto check deferred (battery low)");
        }
        s_boot_checked = true;
        return;
    }

    bool had_boot_check = s_boot_checked;
    s_run_req = req;
    s_boot_checked = true;
    s_state = PS_JOIN;   // expose "active" to the UI before the worker runs
    if (!net_worker_submit(ota_pull_job, nullptr)) {
        // Slot occupied (defensive: net_worker_busy() gated above). Revert so
        // the next tick retries rather than dropping the check.
        s_boot_checked = had_boot_check;
        s_state = PS_IDLE;
        s_next_sched_ms = millis() + 1000;
    }
}

void ota_pull_handle_ctrl(const char* json) {
    if (!json) return;

    // NimBLE host task: parse and enqueue only. Every gate that touches NVS or
    // the PMU runs on the loop task in ota_pull_tick() (DESIGN §9, IMPL §2.3).
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        pull_send_err("bad_json");
        return;
    }

    if (ota_pull_is_active() || ota_is_active()) {
        pull_send_err("busy");
        return;
    }

    const char* mode = doc["mode"] | "";
    bool apply = !(mode[0] && strcmp(mode, "check") == 0);

    pull_req_t req = {};
    req.apply = apply;
    req.force = doc["force"] | false;
    req.from_ble = true;
    strlcpy(req.to, doc["to"] | "", sizeof(req.to));

    portENTER_CRITICAL(&s_ctrl_mux);
    s_ctrl_req = req;
    s_ctrl_pending = true;
    portEXIT_CRITICAL(&s_ctrl_mux);

    pull_send_checking();
}

bool ota_pull_is_active(void) {
    return s_state != PS_IDLE;
}

const char* ota_pull_state_name(void) {
    switch (s_state) {
    case PS_JOIN:     return "join";
    case PS_SNTP:     return "sntp";
    case PS_MANIFEST: return "manifest";
    case PS_COMPARE:  return "manifest";
    case PS_DOWNLOAD: return "download";
    case PS_VERIFY:   return "verify";
    case PS_ACTIVATE: return "activate";
    case PS_REBOOT:   return "reboot";
    case PS_IDLE:
    default:          return "idle";
    }
}
