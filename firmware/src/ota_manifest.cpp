// Update manifest parser: design/ota-pull/DESIGN.md section 6 and
// design/ota-pull/IMPL.md section 2.1.
//
// Pure logic only: ArduinoJson for the JSON, no WiFi/Arduino. The whole
// implementation is guarded on ArduinoJson being available so the host test
// (and CI, which has no ArduinoJson) can still compile this translation unit;
// the transport-free helpers below are always compiled and always tested.
#include "ota_manifest.h"
#include "ota_semver.h"

#include <stdio.h>
#include <string.h>

#if defined(__has_include)
#  if __has_include("ArduinoJson.h") || __has_include(<ArduinoJson.h>)
#    include <ArduinoJson.h>
#    define OTA_MANIFEST_HAVE_ARDUINOJSON 1
#  endif
#endif

// ---- pure helpers (no ArduinoJson) -----------------------------------------

static bool is_lower_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

// A relative URL must not carry an origin: no scheme ("https:"), no
// protocol-relative or absolute path (leading '/'), no traversal ("..") and no
// Windows-style separator. The TLS origin is pinned, so anything that could
// redirect the GET elsewhere is refused.
bool ota_url_is_relative_safe(const char* url) {
    if (!url || url[0] == '\0') return false;

    // Absolute path or protocol-relative "//host".
    if (url[0] == '/') return false;

    // No scheme: "scheme:" where scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ).
    const char* p = url;
    while (*p && *p != '/' && *p != '?' && *p != '#') {
        if (*p == ':') {
            char f = url[0];
            if (!((f >= 'a' && f <= 'z') || (f >= 'A' && f <= 'Z'))) return false;
            for (const char* q = url; q < p; q++) {
                char c = *q;
                bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
                if (!ok) return false;
            }
            return false;   // scheme present
        }
        p++;
    }

    // Reject path traversal and backslashes, walking segment by segment so
    // "a..b" is allowed but "..", "../x" and "a/../b" are not.
    const char* s = url;
    while (*s) {
        if (*s == '\\') return false;
        if (s[0] == '.' && s[1] == '.' && (s[2] == '\0' || s[2] == '/')) return false;
        while (*s && *s != '/') {
            if (*s == '\\') return false;
            s++;
        }
        if (*s == '/') s++;
    }
    return true;
}

bool ota_sha256_hex_eq(const char* a, const char* b) {
    if (!a || !b) return false;
    if (strlen(a) != OTA_SHA256_HEX_LEN || strlen(b) != OTA_SHA256_HEX_LEN) return false;

    // Fold every byte and every alphabet check into one accumulator so the
    // loop runs the full 64 chars regardless of where a mismatch is.
    unsigned diff = 0;
    for (size_t i = 0; i < OTA_SHA256_HEX_LEN; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        diff |= (unsigned)(ca ^ cb);
        if (!is_lower_hex((char)ca)) diff |= 1u;
        if (!is_lower_hex((char)cb)) diff |= 1u;
    }
    return diff == 0;
}

static bool iso_is_digit(char c) { return c >= '0' && c <= '9'; }

static int iso_d2(const char* s) { return (s[0] - '0') * 10 + (s[1] - '0'); }

static int iso_d4(const char* s) {
    return (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
}

static bool iso_is_leap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int iso_days_in_month(int y, int m) {
    static const int days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && iso_is_leap(y)) return 29;
    return days[m - 1];
}

// Howard Hinnant's days-from-civil; no timegm() dependency.
static long iso_days_from_civil(int y, int m, int d) {
    y -= (m <= 2) ? 1 : 0;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097L + (long)doe - 719468L;
}

// ---- base64 + signature policy (no ArduinoJson) ----------------------------

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool ota_base64_is_valid(const char* s) {
    if (!s || s[0] == '\0') return false;
    size_t n = strlen(s);
    if (n % 4 != 0) return false;   // standard base64 is a whole number of quads

    size_t pad = 0;
    if (s[n - 1] == '=') pad++;
    if (n >= 2 && s[n - 2] == '=') pad++;
    if (pad > 2) return false;

    // Every non-padding char must be in the alphabet; '=' is never in the
    // alphabet, so a stray '=' before the tail also fails here.
    for (size_t i = 0; i < n - pad; i++) {
        if (b64_val(s[i]) < 0) return false;
    }
    return true;
}

int ota_base64_decode(const char* s, unsigned char* out, size_t out_cap) {
    if (!s || !out) return -1;
    if (!ota_base64_is_valid(s)) return -1;

    size_t n = strlen(s);
    size_t o = 0;
    for (size_t i = 0; i < n; i += 4) {
        int v0 = b64_val(s[i]);
        int v1 = b64_val(s[i + 1]);
        bool p2 = (s[i + 2] == '=');
        bool p3 = (s[i + 3] == '=');
        int v2 = p2 ? 0 : b64_val(s[i + 2]);
        int v3 = p3 ? 0 : b64_val(s[i + 3]);
        if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0) return -1;

        if (o >= out_cap) return -1;
        out[o++] = (unsigned char)((v0 << 2) | (v1 >> 4));
        if (!p2) {
            if (o >= out_cap) return -1;
            out[o++] = (unsigned char)(((v1 & 0x0f) << 4) | (v2 >> 2));
        }
        if (!p3) {
            if (o >= out_cap) return -1;
            out[o++] = (unsigned char)(((v2 & 0x03) << 6) | v3);
        }
    }
    return (int)o;
}

bool ota_manifest_is_signed(const OtaManifest* mf) {
    return mf != NULL && mf->sig[0] != '\0';
}

bool ota_manifest_sig_ok(const OtaManifest* mf, bool have_pinned_key) {
    if (mf == NULL) return false;
    if (!have_pinned_key) return true;   // hash-only build: unsigned is fine
    return ota_manifest_is_signed(mf);
}

bool ota_released_at_sane(const char* iso, long now_epoch) {
    if (!iso) return false;
    if (iso[0] == '\0') return true;   // absent timestamp is sane

    // Exactly "YYYY-MM-DDThh:mm:ssZ" (20 chars).
    if (strlen(iso) != 20) return false;
    if (iso[4] != '-' || iso[7] != '-' || iso[13] != ':' || iso[16] != ':') return false;
    if (iso[10] != 'T' && iso[10] != 't') return false;
    if (iso[19] != 'Z' && iso[19] != 'z') return false;
    const int digit_pos[14] = {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18};
    for (int i = 0; i < 14; i++) {
        if (!iso_is_digit(iso[digit_pos[i]])) return false;
    }

    int year = iso_d4(iso);
    int month = iso_d2(iso + 5);
    int day = iso_d2(iso + 8);
    int hour = iso_d2(iso + 11);
    int minute = iso_d2(iso + 14);
    int second = iso_d2(iso + 17);

    if (month < 1 || month > 12) return false;
    if (day < 1 || day > iso_days_in_month(year, month)) return false;
    if (hour > 23 || minute > 59 || second > 60) return false;

    long epoch = iso_days_from_civil(year, month, day) * 86400L +
                 (long)hour * 3600L + (long)minute * 60L + (long)second;

    // Reject more than 24 h in the future; exactly +24 h is still accepted.
    if (epoch > now_epoch + 86400L) return false;
    return true;
}

// ---- JSON parser (ArduinoJson 7) -------------------------------------------

#ifdef OTA_MANIFEST_HAVE_ARDUINOJSON

static bool sha_is_valid_lower_hex(const char* s) {
    if (!s || strlen(s) != OTA_SHA256_HEX_LEN) return false;
    for (size_t i = 0; i < OTA_SHA256_HEX_LEN; i++) {
        if (!is_lower_hex(s[i])) return false;
    }
    return true;
}

ota_manifest_err ota_manifest_parse(const char* json, OtaManifest* out) {
    if (!json || !out) return OTA_MF_BAD_JSON;

    JsonDocument doc;
    if (deserializeJson(doc, json)) return OTA_MF_BAD_JSON;

    // Required fields (DESIGN section 6.2). Unknown fields are ignored.
    if (!doc["schema_version"].is<int>() ||
        doc["schema_version"].as<int>() != OTA_MANIFEST_SCHEMA_VERSION) {
        return OTA_MF_BAD_SCHEMA;
    }

    const char* board = doc["board"] | "";
    if (board[0] == '\0' || strlen(board) >= OTA_BOARD_MAX) return OTA_MF_BAD_BOARD;

    const char* version = doc["version"] | "";
    int vparts[3];
    if (version[0] == '\0' || strlen(version) >= OTA_VERSION_MAX ||
        !semver_parse(version, vparts)) {
        return OTA_MF_BAD_VERSION;
    }

    const char* url = doc["url"] | "";
    if (!ota_url_is_relative_safe(url) || strlen(url) >= OTA_URL_MAX) return OTA_MF_BAD_URL;

    const char* sha = doc["sha256"] | "";
    if (!sha_is_valid_lower_hex(sha)) return OTA_MF_BAD_SHA;

    if (!doc["size"].is<long>()) return OTA_MF_BAD_SIZE;
    long size = doc["size"].as<long>();
    if (size <= 0) return OTA_MF_BAD_SIZE;

    // Optional fields.
    const char* min_from = doc["min_from"] | "";
    if (min_from[0] != '\0' && strlen(min_from) >= OTA_VERSION_MAX) return OTA_MF_BAD_VERSION;

    const char* released_at = doc["released_at"] | "";
    if (released_at[0] != '\0' && strlen(released_at) >= sizeof(((OtaManifest*)0)->released_at)) {
        return OTA_MF_BAD_JSON;
    }

    const char* notes = doc["notes"] | "";
    bool mandatory = doc["mandatory"] | false;

    // Optional detached signature (SIGNING.md section 4): the three fields are
    // all-or-nothing, and a present sig must name the one supported algorithm
    // with syntactically valid base64. An absent sig is allowed here -- whether
    // it is *accepted* is the pinned-key policy in the verify step.
    const char* sig = doc["sig"] | "";
    const char* sig_alg = doc["sig_alg"] | "";
    const char* key_id = doc["key_id"] | "";
    const bool any_sig = sig[0] || sig_alg[0] || key_id[0];
    if (any_sig) {
        // All three, or the manifest is malformed.
        if (!sig[0] || !sig_alg[0] || !key_id[0]) return OTA_MF_BAD_SIG;
        if (strcmp(sig_alg, OTA_SIG_ALG_ECDSA_P256_SHA256) != 0) return OTA_MF_BAD_SIG;
        if (strlen(sig) > OTA_SIG_B64_MAX || !ota_base64_is_valid(sig)) return OTA_MF_BAD_SIG;
        if (strlen(sig_alg) >= OTA_SIG_ALG_MAX) return OTA_MF_BAD_SIG;
        if (strlen(key_id) >= OTA_KEY_ID_MAX) return OTA_MF_BAD_SIG;
    }

    // Validate everything before touching *out, so a failure never leaves a
    // half-populated manifest behind.
    OtaManifest m;
    memset(&m, 0, sizeof(m));
    m.schema_version = OTA_MANIFEST_SCHEMA_VERSION;
    snprintf(m.board, sizeof(m.board), "%s", board);
    snprintf(m.version, sizeof(m.version), "%s", version);
    snprintf(m.url, sizeof(m.url), "%s", url);
    snprintf(m.sha256, sizeof(m.sha256), "%s", sha);
    m.size = size;
    if (min_from[0] != '\0') snprintf(m.min_from, sizeof(m.min_from), "%s", min_from);
    m.mandatory = mandatory;
    if (released_at[0] != '\0') snprintf(m.released_at, sizeof(m.released_at), "%s", released_at);
    if (notes[0] != '\0') snprintf(m.notes, sizeof(m.notes), "%s", notes);
    if (any_sig) {
        snprintf(m.sig, sizeof(m.sig), "%s", sig);
        snprintf(m.sig_alg, sizeof(m.sig_alg), "%s", sig_alg);
        snprintf(m.key_id, sizeof(m.key_id), "%s", key_id);
    }

    *out = m;
    return OTA_MF_OK;
}

#endif  // OTA_MANIFEST_HAVE_ARDUINOJSON
