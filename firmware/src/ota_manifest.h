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
};

enum ota_manifest_err {
    OTA_MF_OK = 0, OTA_MF_BAD_JSON, OTA_MF_BAD_SCHEMA, OTA_MF_BAD_BOARD,
    OTA_MF_BAD_VERSION, OTA_MF_BAD_URL, OTA_MF_BAD_SHA, OTA_MF_BAD_SIZE,
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
