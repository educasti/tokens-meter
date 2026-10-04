#pragma once
#include <stdbool.h>
#include <stddef.h>

// Update manifest contract: design/ota-pull/DESIGN.md section 6 and
// design/ota-pull/IMPL.md section 2.1. Pure/transport-free: this header and
// ota_manifest.cpp never touch WiFi or Arduino, so the host test can compile
// them with plain g++ (ota_manifest.cpp needs only ArduinoJson).
//
// A manifest is parsed once per check; every string is a fixed-size buffer so
// the struct can live on the pull task's stack with no heap churn.

#define OTA_MANIFEST_SCHEMA_VERSION 1
#define OTA_SHA256_HEX_LEN 64
#define OTA_URL_MAX 192
#define OTA_BOARD_MAX 32
#define OTA_VERSION_MAX 16

// Detached-signature manifest fields (design/ota-pull/SIGNING.md section 4).
// They are emitted only when the publisher signs; a pre-P4 (unsigned) manifest
// omits all three and still parses. `sig` is standard-alphabet base64 of the
// DER ECDSA P-256/SHA-256 signature; a 72-byte DER signature encodes to 96
// base64 chars, so 128 is a comfortable ceiling.
#define OTA_SIG_ALG_ECDSA_P256_SHA256 "ecdsa-p256-sha256"
#define OTA_SIG_ALG_MAX 24      // "ecdsa-p256-sha256" is 18 chars + NUL room
#define OTA_SIG_B64_MAX 128     // base64 DER signature, no NUL
#define OTA_KEY_ID_MAX 64       // opaque rotation label ("", when unsigned)

struct OtaManifest {
    int  schema_version;
    char board[OTA_BOARD_MAX];
    char version[OTA_VERSION_MAX];        // strict semver, no leading 'v'
    char url[OTA_URL_MAX];                // relative path, validated
    char sha256[OTA_SHA256_HEX_LEN + 1];  // 64 lowercase hex + NUL
    long size;                            // bytes, > 0
    char min_from[OTA_VERSION_MAX];       // "" when absent
    bool mandatory;                       // default false
    char released_at[24];                 // ISO-8601 UTC, "" when absent
    char notes[OTA_URL_MAX];              // "" when absent
    char sig[OTA_SIG_B64_MAX + 1];        // base64 DER, "" when unsigned
    char sig_alg[OTA_SIG_ALG_MAX];        // "ecdsa-p256-sha256" when signed
    char key_id[OTA_KEY_ID_MAX];          // opaque key label, "" when unsigned
};

enum ota_manifest_err {
    OTA_MF_OK = 0, OTA_MF_BAD_JSON, OTA_MF_BAD_SCHEMA, OTA_MF_BAD_BOARD,
    OTA_MF_BAD_VERSION, OTA_MF_BAD_URL, OTA_MF_BAD_SHA, OTA_MF_BAD_SIZE,
    OTA_MF_BAD_SIG,
};

// Parse `json` into `out`. On success every field is populated and OTA_MF_OK is
// returned. On any failure the matching error is returned and `out` is left
// untouched (it is not partially overwritten). Unknown fields are ignored.
ota_manifest_err ota_manifest_parse(const char* json, OtaManifest* out);

// True when `url` is a safe relative path: non-empty, no scheme, no leading
// '/', no '..' segment and no backslash. Absolute URLs and path traversal are
// rejected so the pinned TLS origin keeps its meaning.
bool ota_url_is_relative_safe(const char* url);

// Constant-time-ish equality for two 64-char lowercase hex SHA-256 strings.
// Length and alphabet are checked first; uppercase hex is not accepted.
bool ota_sha256_hex_eq(const char* a, const char* b);

// ISO-8601 UTC sanity check. `""` is sane (absent); a malformed timestamp or
// one more than 24 h in the future (relative to `now_epoch`) is not.
bool ota_released_at_sane(const char* iso, long now_epoch);

// Standard-alphabet base64 ("A-Za-z0-9+/"), zero, one or two trailing '='
// padding chars, and a length that is a positive multiple of four. Used to
// reject a malformed manifest `sig` as OTA_MF_BAD_SIG before any crypto runs.
bool ota_base64_is_valid(const char* s);

// Decode a base64 string already vetted by ota_base64_is_valid() into `out`
// (capacity `out_cap`). Returns the decoded byte count, or -1 when `s` is
// malformed or would overflow `out`. No NUL terminator is written.
int ota_base64_decode(const char* s, unsigned char* out, size_t out_cap);

// True when the manifest carries a detached signature (`sig` non-empty). The
// parser guarantees a present sig already passed the sig_alg / base64 checks.
bool ota_manifest_is_signed(const OtaManifest* mf);

// Verify-step policy gate for the signed-update flow (SIGNING.md section 4 and
// section 7): with no pinned key, an unsigned manifest is the pre-P4 shape and
// remains acceptable (returns true); with a pinned key, a signed manifest is
// required (returns ota_manifest_is_signed(mf)). A false return means the
// caller must report `bad_signature` and must not activate.
bool ota_manifest_sig_ok(const OtaManifest* mf, bool have_pinned_key);
