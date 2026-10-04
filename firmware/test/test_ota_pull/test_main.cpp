// Host unit test for the pull-OTA pure logic (IMPL.md section 4.4):
// ota_semver.h always runs; ota_manifest.{h,cpp} needs ArduinoJson, which CI
// does not have, so those checks are compiled and run only when the header is
// on the include path. With ArduinoJson absent the manifest section prints
// "SKIP (no ArduinoJson)" and the program still exits 0.
//
// Build & run (pure, no ArduinoJson):
//   cd firmware/test/test_ota_pull
//   g++ -std=c++17 -I ../../src test_main.cpp ../../src/ota_manifest.cpp -o /tmp/p1pure
//   /tmp/p1pure
//
// With ArduinoJson from the P0 build cache:
//   AJ=$(dirname "$(find ../../.pio/libdeps -name ArduinoJson.h | head -1)")
//   g++ -std=c++17 -I ../../src -I "$AJ" test_main.cpp ../../src/ota_manifest.cpp -o /tmp/p1full
//   /tmp/p1full

#include <stdio.h>
#include <string.h>
#include <limits.h>

#include "ota_semver.h"
#include "ota_manifest.h"

#if defined(__has_include)
#  if __has_include("ArduinoJson.h")
#    define TEST_HAVE_ARDUINOJSON 1
#  endif
#endif

static int checks = 0;
static int failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++checks;                                                              \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);             \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static void eq_int(const char* what, long got, long want) {
    ++checks;
    if (got == want) {
        printf("  ok   %-38s %ld\n", what, got);
    } else {
        printf("  FAIL %-38s got %ld want %ld\n", what, got, want);
        ++failures;
    }
}

// ---- semver (always runs) ---------------------------------------------------

static void test_semver_parse(void) {
    printf("semver_parse\n");
    int v[3];

    CHECK(semver_parse("0.1.0", v) && v[0] == 0 && v[1] == 1 && v[2] == 0);
    CHECK(semver_parse("v1.2.3", v) && v[0] == 1 && v[1] == 2 && v[2] == 3);
    CHECK(semver_parse("10.20.30", v) && v[0] == 10 && v[1] == 20 && v[2] == 30);
    CHECK(semver_parse("2147483647.0.0", v) && v[0] == INT_MAX);
    CHECK(semver_parse("0.0.0", v) && v[0] == 0 && v[1] == 0 && v[2] == 0);

    CHECK(!semver_parse("", v));            // empty
    CHECK(!semver_parse("v", v));           // only the prefix
    CHECK(!semver_parse("1.2", v));         // too few parts
    CHECK(!semver_parse("1.2.3.4", v));     // too many parts
    CHECK(!semver_parse("a.b.c", v));       // non-numeric
    CHECK(!semver_parse("1.2.3-rc1", v));   // pre-release metadata
    CHECK(!semver_parse("1.2.3+build", v)); // build metadata
    CHECK(!semver_parse("1..2", v));        // empty part
    CHECK(!semver_parse(".1.2", v));        // leading dot
    CHECK(!semver_parse("1.2.", v));        // trailing dot
    CHECK(!semver_parse("1.2.x", v));       // non-numeric part
    CHECK(!semver_parse("1.2.3 ", v));      // trailing space
    CHECK(!semver_parse(" 1.2.3", v));      // leading space
    CHECK(!semver_parse("2147483648.0.0", v));     // overflow major
    CHECK(!semver_parse("9999999999.0.0", v));     // overflow major
    CHECK(!semver_parse("1.2.2147483648", v));     // overflow patch
    CHECK(!semver_parse(NULL, v));          // null
}

static void test_semver_cmp(void) {
    printf("semver_cmp\n");

    eq_int("0.1.0 < 0.2.0", semver_cmp("0.1.0", "0.2.0"), -1);
    eq_int("0.2.0 > 0.1.0", semver_cmp("0.2.0", "0.1.0"), 1);
    eq_int("0.2.0 == 0.2.0", semver_cmp("0.2.0", "0.2.0"), 0);
    eq_int("v0.2.0 == 0.2.0", semver_cmp("v0.2.0", "0.2.0"), 0);
    eq_int("v0.2.0 < v0.3.0", semver_cmp("v0.2.0", "v0.3.0"), -1);
    eq_int("1.0.0 > 0.9.9", semver_cmp("1.0.0", "0.9.9"), 1);
    eq_int("0.10.0 > 0.9.0", semver_cmp("0.10.0", "0.9.0"), 1);
    eq_int("0.1.10 > 0.1.9", semver_cmp("0.1.10", "0.1.9"), 1);
    eq_int("0.1.0 < 1.0.0", semver_cmp("0.1.0", "1.0.0"), -1);

    // Parse failure is never equal to anything.
    eq_int("1.2 vs 1.2.3 invalid", semver_cmp("1.2", "1.2.3"), SEMVER_INVALID);
    eq_int("bad vs bad invalid", semver_cmp("bad", "bad"), SEMVER_INVALID);
    CHECK(semver_cmp("1.2.3", "") < 0);
    CHECK(semver_cmp("", "") < 0);
    CHECK(semver_cmp("1.2.3", "1.2.3") != SEMVER_INVALID);
}

// min_from-style comparisons used by the COMPARE state (DESIGN section 5.4).
static void test_semver_min_from(void) {
    printf("semver min_from comparisons\n");
    const char* running = "0.2.0";
    CHECK(semver_cmp(running, "0.1.0") > 0);    // running newer than min_from
    CHECK(semver_cmp(running, "0.2.0") == 0);   // exactly at min_from
    CHECK(semver_cmp(running, "0.3.0") < 0);    // running too old
    CHECK(semver_cmp("0.0.0-dev", "0.1.0") == SEMVER_INVALID);  // dev is unparsable
}

// ---- URL / SHA / released_at (always runs) ---------------------------------

static void test_url_safe(void) {
    printf("ota_url_is_relative_safe\n");
    CHECK(ota_url_is_relative_safe("clawdmeter-0.2.0.bin"));
    CHECK(ota_url_is_relative_safe("sub/dir/fw.bin"));
    CHECK(ota_url_is_relative_safe("fw.bin?v=1"));
    CHECK(ota_url_is_relative_safe("a..b.bin"));   // dots inside a name are fine

    CHECK(!ota_url_is_relative_safe(""));
    CHECK(!ota_url_is_relative_safe(NULL));
    CHECK(!ota_url_is_relative_safe("/abs/fw.bin"));       // leading '/'
    CHECK(!ota_url_is_relative_safe("https://evil/fw.bin"));  // scheme
    CHECK(!ota_url_is_relative_safe("http://evil/fw.bin"));
    CHECK(!ota_url_is_relative_safe("ftp://evil/fw.bin"));
    CHECK(!ota_url_is_relative_safe("file:/etc/passwd"));
    CHECK(!ota_url_is_relative_safe("//evil/fw.bin"));     // protocol-relative
    CHECK(!ota_url_is_relative_safe("../fw.bin"));         // traversal
    CHECK(!ota_url_is_relative_safe("a/../fw.bin"));
    CHECK(!ota_url_is_relative_safe("a/.."));
    CHECK(!ota_url_is_relative_safe("a\\b.bin"));          // backslash
    CHECK(!ota_url_is_relative_safe("..\\fw.bin"));
}

#define SHA64  "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define SHA_B  "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdee"
#define SHA_UP "0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef"
#define SHA_63 "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcde"
#define SHA_65 "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0"
#define SHA_HEX "g123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

static void test_sha_eq(void) {
    printf("ota_sha256_hex_eq\n");
    CHECK(ota_sha256_hex_eq(SHA64, SHA64));
    CHECK(!ota_sha256_hex_eq(SHA64, SHA_B));      // near-miss, last char
    CHECK(!ota_sha256_hex_eq(SHA64, SHA_UP));     // uppercase rejected
    CHECK(!ota_sha256_hex_eq(SHA64, SHA_63));     // too short
    CHECK(!ota_sha256_hex_eq(SHA64, SHA_65));     // too long
    CHECK(!ota_sha256_hex_eq(SHA_63, SHA64));     // short on the left
    CHECK(!ota_sha256_hex_eq(SHA64, SHA_HEX));    // non-hex alphabet
    CHECK(!ota_sha256_hex_eq(SHA64, ""));
    CHECK(!ota_sha256_hex_eq(NULL, SHA64));
    CHECK(!ota_sha256_hex_eq(SHA64, NULL));
}

static void test_released_at(void) {
    printf("ota_released_at_sane\n");
    const long now = 1790942400L;   // 2026-10-02T12:00:00Z

    CHECK(ota_released_at_sane("", now));                       // absent is sane
    CHECK(!ota_released_at_sane(NULL, now));
    CHECK(ota_released_at_sane("2026-10-02T12:00:00Z", now));   // now
    CHECK(ota_released_at_sane("2026-10-02T11:59:59Z", now));   // past
    CHECK(ota_released_at_sane("2020-01-01T00:00:00Z", now));   // long past
    CHECK(ota_released_at_sane("2024-02-29T00:00:00Z", now));   // valid leap day
    CHECK(ota_released_at_sane("2026-10-03T12:00:00Z", now));   // exactly +24 h
    CHECK(!ota_released_at_sane("2026-10-03T12:00:01Z", now));  // +24 h + 1 s
    CHECK(!ota_released_at_sane("2026-10-04T12:00:00Z", now));  // +48 h

    CHECK(!ota_released_at_sane("2026-10-02", now));            // date only
    CHECK(!ota_released_at_sane("2026-10-02T12:00:00", now));   // no zone
    CHECK(!ota_released_at_sane("not-a-date", now));
    CHECK(!ota_released_at_sane("2026-13-01T00:00:00Z", now));  // bad month
    CHECK(!ota_released_at_sane("2026-00-01T00:00:00Z", now));
    CHECK(!ota_released_at_sane("2026-10-32T00:00:00Z", now));  // bad day
    CHECK(!ota_released_at_sane("2026-02-30T00:00:00Z", now));  // non-leap Feb
    CHECK(!ota_released_at_sane("2026-10-02T25:00:00Z", now));  // bad hour
    CHECK(!ota_released_at_sane("2026-10-02T12:60:00Z", now));  // bad minute
    CHECK(!ota_released_at_sane("26-10-02T12:00:00Z", now));    // short year
}

// ---- base64 + signature policy (always runs) -------------------------------

static void test_base64(void) {
    printf("ota_base64_is_valid / ota_base64_decode\n");
    unsigned char out[8];

    CHECK(ota_base64_is_valid("TWFu"));            // "Man"
    CHECK(ota_base64_is_valid("TWE="));            // "Ma"
    CHECK(ota_base64_is_valid("TQ=="));            // "M"
    CHECK(ota_base64_is_valid("MTIzNDU2Nzg="));    // 8 opaque bytes

    CHECK(!ota_base64_is_valid(""));
    CHECK(!ota_base64_is_valid(NULL));
    CHECK(!ota_base64_is_valid("TWF"));            // length not a multiple of 4
    CHECK(!ota_base64_is_valid("TWFu "));          // trailing space
    CHECK(!ota_base64_is_valid("TW Fu"));          // embedded space
    CHECK(!ota_base64_is_valid("TWFu!"));          // bad alphabet
    CHECK(!ota_base64_is_valid("===="));           // padding only
    CHECK(!ota_base64_is_valid("T==="));           // too much padding
    CHECK(!ota_base64_is_valid("TW=u"));           // '=' not at the end

    CHECK(ota_base64_decode("TWFu", out, sizeof(out)) == 3);
    CHECK(out[0] == 'M' && out[1] == 'a' && out[2] == 'n');
    CHECK(ota_base64_decode("TQ==", out, sizeof(out)) == 1);
    CHECK(out[0] == 'M');
    CHECK(ota_base64_decode("TWE=", out, sizeof(out)) == 2);
    CHECK(out[0] == 'M' && out[1] == 'a');
    CHECK(ota_base64_decode("", out, sizeof(out)) == -1);
    CHECK(ota_base64_decode(NULL, out, sizeof(out)) == -1);
    CHECK(ota_base64_decode("TWFu", out, 2) == -1);   // output overflow
}

// The pure half of the SIGNING.md section 7 decision: with no pinned key the
// hash-only path stands; with a pinned key an unsigned manifest is rejected.
static void test_sig_policy(void) {
    printf("ota_manifest_sig_ok (pinned-key policy)\n");
    OtaManifest m;
    memset(&m, 0, sizeof(m));

    CHECK(!ota_manifest_is_signed(&m));
    CHECK(ota_manifest_sig_ok(&m, false));   // no key + unsigned -> hash-only ok
    CHECK(!ota_manifest_sig_ok(&m, true));   // pinned key + unsigned -> reject

    strcpy(m.sig, "MTIzNDU2Nzg=");
    CHECK(ota_manifest_is_signed(&m));
    CHECK(ota_manifest_sig_ok(&m, true));    // pinned key + signed -> ok
    CHECK(ota_manifest_sig_ok(&m, false));   // no key + signed -> still ok

    CHECK(!ota_manifest_sig_ok(NULL, false));
    CHECK(!ota_manifest_sig_ok(NULL, true));
}

// ---- manifest (needs ArduinoJson) ------------------------------------------

#ifdef TEST_HAVE_ARDUINOJSON

static ota_manifest_err parse(const char* json) {
    OtaManifest m;
    return ota_manifest_parse(json, &m);
}

static void test_manifest_valid(void) {
    printf("ota_manifest_parse: valid + optional fields\n");
    static const char* J =
        "{\"schema_version\":1,\"board\":\"waveshare_amoled_216\","
        "\"version\":\"0.2.0\",\"url\":\"clawdmeter-0.2.0.bin\","
        "\"sha256\":\"" SHA64 "\",\"size\":1287600,"
        "\"min_from\":\"0.1.0\",\"mandatory\":false,"
        "\"released_at\":\"2026-10-02T12:00:00Z\",\"notes\":\"Fix WiFi.\"}";
    OtaManifest m;
    memset(&m, 0, sizeof(m));
    CHECK(ota_manifest_parse(J, &m) == OTA_MF_OK);
    CHECK(m.schema_version == OTA_MANIFEST_SCHEMA_VERSION);
    CHECK(strcmp(m.board, "waveshare_amoled_216") == 0);
    CHECK(strcmp(m.version, "0.2.0") == 0);
    CHECK(strcmp(m.url, "clawdmeter-0.2.0.bin") == 0);
    CHECK(strcmp(m.sha256, SHA64) == 0);
    CHECK(m.size == 1287600);
    CHECK(strcmp(m.min_from, "0.1.0") == 0);
    CHECK(m.mandatory == false);
    CHECK(strcmp(m.released_at, "2026-10-02T12:00:00Z") == 0);
    CHECK(strcmp(m.notes, "Fix WiFi.") == 0);

    static const char* J_MAND =
        "{\"schema_version\":1,\"board\":\"b\",\"version\":\"1.0.0\",\"url\":\"fw.bin\","
        "\"sha256\":\"" SHA64 "\",\"size\":1,\"mandatory\":true}";
    CHECK(ota_manifest_parse(J_MAND, &m) == OTA_MF_OK);
    CHECK(m.mandatory == true);
}

static void test_manifest_defaults(void) {
    printf("ota_manifest_parse: optional fields default\n");
    static const char* J =
        "{\"schema_version\":1,\"board\":\"waveshare_amoled_216\","
        "\"version\":\"1.0.0\",\"url\":\"fw.bin\","
        "\"sha256\":\"" SHA64 "\",\"size\":10}";
    OtaManifest m;
    memset(&m, 0xff, sizeof(m));
    CHECK(ota_manifest_parse(J, &m) == OTA_MF_OK);
    CHECK(m.min_from[0] == '\0');
    CHECK(m.released_at[0] == '\0');
    CHECK(m.notes[0] == '\0');
    CHECK(m.mandatory == false);
}

static void test_manifest_unknown_fields(void) {
    printf("ota_manifest_parse: unknown fields ignored\n");
    static const char* J =
        "{\"schema_version\":1,\"board\":\"b\",\"version\":\"1.0.0\",\"url\":\"fw.bin\","
        "\"sha256\":\"" SHA64 "\",\"size\":10,"
        "\"extra\":{\"nested\":[1,2,3]},\"future_field\":true}";
    OtaManifest m;
    CHECK(ota_manifest_parse(J, &m) == OTA_MF_OK);
    CHECK(strcmp(m.url, "fw.bin") == 0);
}

static void test_manifest_required_errors(void) {
    printf("ota_manifest_parse: missing/invalid required fields\n");
    // Missing schema_version.
    CHECK(parse("{\"board\":\"b\",\"version\":\"1.0.0\",\"url\":\"fw.bin\","
                "\"sha256\":\"" SHA64 "\",\"size\":10}") == OTA_MF_BAD_SCHEMA);
    // Wrong schema_version.
    CHECK(parse("{\"schema_version\":2,\"board\":\"b\",\"version\":\"1.0.0\","
                "\"url\":\"fw.bin\",\"sha256\":\"" SHA64 "\",\"size\":10}") == OTA_MF_BAD_SCHEMA);
    // schema_version as a string is not an integer.
    CHECK(parse("{\"schema_version\":\"1\",\"board\":\"b\",\"version\":\"1.0.0\","
                "\"url\":\"fw.bin\",\"sha256\":\"" SHA64 "\",\"size\":10}") == OTA_MF_BAD_SCHEMA);

    // Missing / empty board.
    CHECK(parse("{\"schema_version\":1,\"version\":\"1.0.0\",\"url\":\"fw.bin\","
                "\"sha256\":\"" SHA64 "\",\"size\":10}") == OTA_MF_BAD_BOARD);
    CHECK(parse("{\"schema_version\":1,\"board\":\"\",\"version\":\"1.0.0\",\"url\":\"fw.bin\","
                "\"sha256\":\"" SHA64 "\",\"size\":10}") == OTA_MF_BAD_BOARD);

    // Missing / malformed version.
    CHECK(parse("{\"schema_version\":1,\"board\":\"b\",\"url\":\"fw.bin\","
                "\"sha256\":\"" SHA64 "\",\"size\":10}") == OTA_MF_BAD_VERSION);
    CHECK(parse("{\"schema_version\":1,\"board\":\"b\",\"version\":\"1.2\",\"url\":\"fw.bin\","
                "\"sha256\":\"" SHA64 "\",\"size\":10}") == OTA_MF_BAD_VERSION);
    CHECK(parse("{\"schema_version\":1,\"board\":\"b\",\"version\":\"1.2.3-rc1\","
                "\"url\":\"fw.bin\",\"sha256\":\"" SHA64 "\",\"size\":10}") == OTA_MF_BAD_VERSION);

    // Missing / bad url.
    CHECK(parse("{\"schema_version\":1,\"board\":\"b\",\"version\":\"1.0.0\","
                "\"sha256\":\"" SHA64 "\",\"size\":10}") == OTA_MF_BAD_URL);
    CHECK(parse("{\"schema_version\":1,\"board\":\"b\",\"version\":\"1.0.0\","
                "\"url\":\"https://evil/fw.bin\",\"sha256\":\"" SHA64 "\",\"size\":10}") == OTA_MF_BAD_URL);
    CHECK(parse("{\"schema_version\":1,\"board\":\"b\",\"version\":\"1.0.0\","
                "\"url\":\"/fw.bin\",\"sha256\":\"" SHA64 "\",\"size\":10}") == OTA_MF_BAD_URL);
    CHECK(parse("{\"schema_version\":1,\"board\":\"b\",\"version\":\"1.0.0\","
                "\"url\":\"../fw.bin\",\"sha256\":\"" SHA64 "\",\"size\":10}") == OTA_MF_BAD_URL);
    CHECK(parse("{\"schema_version\":1,\"board\":\"b\",\"version\":\"1.0.0\","
                "\"url\":\"a/../fw.bin\",\"sha256\":\"" SHA64 "\",\"size\":10}") == OTA_MF_BAD_URL);

    // Missing / short / uppercase / non-hex sha256.
    CHECK(parse("{\"schema_version\":1,\"board\":\"b\",\"version\":\"1.0.0\","
                "\"url\":\"fw.bin\",\"size\":10}") == OTA_MF_BAD_SHA);
    CHECK(parse("{\"schema_version\":1,\"board\":\"b\",\"version\":\"1.0.0\","
                "\"url\":\"fw.bin\",\"sha256\":\"" SHA_63 "\",\"size\":10}") == OTA_MF_BAD_SHA);
    CHECK(parse("{\"schema_version\":1,\"board\":\"b\",\"version\":\"1.0.0\","
                "\"url\":\"fw.bin\",\"sha256\":\"" SHA_UP "\",\"size\":10}") == OTA_MF_BAD_SHA);
    CHECK(parse("{\"schema_version\":1,\"board\":\"b\",\"version\":\"1.0.0\","
                "\"url\":\"fw.bin\",\"sha256\":\"" SHA_HEX "\",\"size\":10}") == OTA_MF_BAD_SHA);

    // Missing / zero / negative / non-numeric size.
    CHECK(parse("{\"schema_version\":1,\"board\":\"b\",\"version\":\"1.0.0\","
                "\"url\":\"fw.bin\",\"sha256\":\"" SHA64 "\"}") == OTA_MF_BAD_SIZE);
    CHECK(parse("{\"schema_version\":1,\"board\":\"b\",\"version\":\"1.0.0\","
                "\"url\":\"fw.bin\",\"sha256\":\"" SHA64 "\",\"size\":0}") == OTA_MF_BAD_SIZE);
    CHECK(parse("{\"schema_version\":1,\"board\":\"b\",\"version\":\"1.0.0\","
                "\"url\":\"fw.bin\",\"sha256\":\"" SHA64 "\",\"size\":-5}") == OTA_MF_BAD_SIZE);
    CHECK(parse("{\"schema_version\":1,\"board\":\"b\",\"version\":\"1.0.0\","
                "\"url\":\"fw.bin\",\"sha256\":\"" SHA64 "\",\"size\":\"10\"}") == OTA_MF_BAD_SIZE);

    // Malformed JSON.
    CHECK(parse("not json") == OTA_MF_BAD_JSON);
    CHECK(parse("") == OTA_MF_BAD_JSON);
    CHECK(ota_manifest_parse(NULL, NULL) == OTA_MF_BAD_JSON);
}

static void test_manifest_failure_leaves_out(void) {
    printf("ota_manifest_parse: failure leaves out untouched\n");
    OtaManifest m;
    memset(&m, 0, sizeof(m));
    m.schema_version = -99;
    m.size = 4242;
    CHECK(ota_manifest_parse("{\"schema_version\":2}", &m) == OTA_MF_BAD_SCHEMA);
    CHECK(m.schema_version == -99);
    CHECK(m.size == 4242);
}

// Signature fields (design/ota-pull/SIGNING.md section 4). The base64 blob is
// opaque to the parser; it only checks the alphabet/shape.
#define SIG_VALID "MTIzNDU2Nzg="
#define SIG_BAD   "not base64!!"

static void test_manifest_sig(void) {
    printf("ota_manifest_parse: signature fields\n");
    OtaManifest m;
    memset(&m, 0, sizeof(m));

    // Absent sig is the pre-P4 shape: parse succeeds and all three stay empty.
    static const char* J_UNSIGNED =
        "{\"schema_version\":1,\"board\":\"b\",\"version\":\"1.0.0\",\"url\":\"fw.bin\","
        "\"sha256\":\"" SHA64 "\",\"size\":10}";
    CHECK(ota_manifest_parse(J_UNSIGNED, &m) == OTA_MF_OK);
    CHECK(!ota_manifest_is_signed(&m));
    CHECK(m.sig[0] == '\0' && m.sig_alg[0] == '\0' && m.key_id[0] == '\0');

    // A complete signed manifest parses and round-trips every field.
    static const char* J_SIGNED =
        "{\"schema_version\":1,\"board\":\"b\",\"version\":\"1.0.0\",\"url\":\"fw.bin\","
        "\"sha256\":\"" SHA64 "\",\"size\":10,"
        "\"sig\":\"" SIG_VALID "\",\"sig_alg\":\"ecdsa-p256-sha256\","
        "\"key_id\":\"ef7cd0a16e487445\"}";
    CHECK(ota_manifest_parse(J_SIGNED, &m) == OTA_MF_OK);
    CHECK(ota_manifest_is_signed(&m));
    CHECK(strcmp(m.sig, SIG_VALID) == 0);
    CHECK(strcmp(m.sig_alg, OTA_SIG_ALG_ECDSA_P256_SHA256) == 0);
    CHECK(strcmp(m.key_id, "ef7cd0a16e487445") == 0);

    // All-or-nothing: some but not all of the three fields is malformed.
    CHECK(parse("{\"schema_version\":1,\"board\":\"b\",\"version\":\"1.0.0\",\"url\":\"fw.bin\","
                "\"sha256\":\"" SHA64 "\",\"size\":10,\"sig\":\"" SIG_VALID "\","
                "\"sig_alg\":\"ecdsa-p256-sha256\"}") == OTA_MF_BAD_SIG);   // no key_id
    CHECK(parse("{\"schema_version\":1,\"board\":\"b\",\"version\":\"1.0.0\",\"url\":\"fw.bin\","
                "\"sha256\":\"" SHA64 "\",\"size\":10,\"sig\":\"" SIG_VALID "\","
                "\"key_id\":\"k\"}") == OTA_MF_BAD_SIG);                    // no sig_alg
    CHECK(parse("{\"schema_version\":1,\"board\":\"b\",\"version\":\"1.0.0\",\"url\":\"fw.bin\","
                "\"sha256\":\"" SHA64 "\",\"size\":10,\"sig_alg\":\"ecdsa-p256-sha256\","
                "\"key_id\":\"k\"}") == OTA_MF_BAD_SIG);                    // no sig

    // Wrong algorithm (only ecdsa-p256-sha256 is accepted).
    CHECK(parse("{\"schema_version\":1,\"board\":\"b\",\"version\":\"1.0.0\",\"url\":\"fw.bin\","
                "\"sha256\":\"" SHA64 "\",\"size\":10,\"sig\":\"" SIG_VALID "\","
                "\"sig_alg\":\"rsa-sha256\",\"key_id\":\"k\"}") == OTA_MF_BAD_SIG);
    CHECK(parse("{\"schema_version\":1,\"board\":\"b\",\"version\":\"1.0.0\",\"url\":\"fw.bin\","
                "\"sha256\":\"" SHA64 "\",\"size\":10,\"sig\":\"" SIG_VALID "\","
                "\"sig_alg\":\"ecdsa-p256-sha256 \",\"key_id\":\"k\"}") == OTA_MF_BAD_SIG);

    // Bad base64: bad alphabet, bad length, stray '=', empty.
    CHECK(parse("{\"schema_version\":1,\"board\":\"b\",\"version\":\"1.0.0\",\"url\":\"fw.bin\","
                "\"sha256\":\"" SHA64 "\",\"size\":10,\"sig\":\"" SIG_BAD "\","
                "\"sig_alg\":\"ecdsa-p256-sha256\",\"key_id\":\"k\"}") == OTA_MF_BAD_SIG);
    CHECK(parse("{\"schema_version\":1,\"board\":\"b\",\"version\":\"1.0.0\",\"url\":\"fw.bin\","
                "\"sha256\":\"" SHA64 "\",\"size\":10,\"sig\":\"TWF\","
                "\"sig_alg\":\"ecdsa-p256-sha256\",\"key_id\":\"k\"}") == OTA_MF_BAD_SIG);
    CHECK(parse("{\"schema_version\":1,\"board\":\"b\",\"version\":\"1.0.0\",\"url\":\"fw.bin\","
                "\"sha256\":\"" SHA64 "\",\"size\":10,\"sig\":\"====\","
                "\"sig_alg\":\"ecdsa-p256-sha256\",\"key_id\":\"k\"}") == OTA_MF_BAD_SIG);
    CHECK(parse("{\"schema_version\":1,\"board\":\"b\",\"version\":\"1.0.0\",\"url\":\"fw.bin\","
                "\"sha256\":\"" SHA64 "\",\"size\":10,\"sig\":\"\","
                "\"sig_alg\":\"ecdsa-p256-sha256\",\"key_id\":\"k\"}") == OTA_MF_BAD_SIG);

    // Too long for the fixed buffer (132 'A' chars; base64-shaped but > 128).
    {
        char json[512];
        char big[140];
        memset(big, 'A', sizeof(big));
        big[sizeof(big) - 1] = '\0';
        snprintf(json, sizeof(json),
                 "{\"schema_version\":1,\"board\":\"b\",\"version\":\"1.0.0\","
                 "\"url\":\"fw.bin\",\"sha256\":\"" SHA64 "\",\"size\":10,"
                 "\"sig\":\"%s\",\"sig_alg\":\"ecdsa-p256-sha256\",\"key_id\":\"k\"}",
                 big);
        CHECK(parse(json) == OTA_MF_BAD_SIG);
    }

    // A signed manifest whose required core fields are bad still fails on the
    // core error first (signature parsing does not mask it).
    CHECK(parse("{\"schema_version\":1,\"board\":\"b\",\"version\":\"1.2\",\"url\":\"fw.bin\","
                "\"sha256\":\"" SHA64 "\",\"size\":10,\"sig\":\"" SIG_VALID "\","
                "\"sig_alg\":\"ecdsa-p256-sha256\",\"key_id\":\"k\"}") == OTA_MF_BAD_VERSION);
}

static void test_manifest(void) {
    printf("manifest: running (ArduinoJson present)\n");
    test_manifest_valid();
    test_manifest_defaults();
    test_manifest_unknown_fields();
    test_manifest_required_errors();
    test_manifest_failure_leaves_out();
    test_manifest_sig();
}

#else  // !TEST_HAVE_ARDUINOJSON

static void test_manifest(void) {
    printf("manifest: SKIP (no ArduinoJson)\n");
}

#endif

int main(void) {
    printf("== pull OTA pure logic ==\n");
    test_semver_parse();
    test_semver_cmp();
    test_semver_min_from();
    test_url_safe();
    test_sha_eq();
    test_released_at();
    test_base64();
    test_sig_policy();
    test_manifest();

    printf("\n%d check(s), %d failure(s)\n", checks, failures);
    if (failures) {
        printf("SOME CHECKS FAILED\n");
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
