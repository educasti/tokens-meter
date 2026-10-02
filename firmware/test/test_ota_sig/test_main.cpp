// Host test for the P-256/SHA-256 firmware signature verifier (P4; contract in
// design/ota-pull/SIGNING.md). It exercises a fixed P-256 vector — a SHA-256
// digest and its DER signature under a throwaway public key — when mbedTLS is
// on the host include path. With mbedTLS absent (the usual CI/Linux case) the
// harness still compile-checks ota_sig.h, prints SKIP and exits 0.
//
// Build & run: ../test_ota_sig/run_test.sh
//   with mbedTLS: g++ -std=c++17 -I ../../src test_main.cpp ../../src/ota_sig.cpp \
//                     -lmbedtls -lmbedcrypto -lmbedx509 -o /tmp/test_ota_sig
//   without:      g++ -std=c++17 -I ../../src test_main.cpp -o /tmp/test_ota_sig
//
// The vector's private key was generated with OpenSSL, used once and discarded;
// only the public key, the digest and the signature are embedded here.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ota_sig.h"

#if !defined(TEST_OTA_SIG_FORCE_SKIP)
#  if defined(__has_include)
#    if __has_include(<mbedtls/pk.h>)
#      define TEST_HAVE_MBEDTLS 1
#    endif
#  endif
#endif

// A P-256 public key (SubjectPublicKeyInfo PEM), pinned from firmware.
static const char* kPubKeyPem =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEMWssdHHaLs2GbZefTSLcgSsYROgT\n"
    "+rG2GtibqV0HoHWCRlnXmOh60RJrKuRY8K7RJDtl14hRsfyZExRlrqw4Ww==\n"
    "-----END PUBLIC KEY-----\n";

// A different P-256 key, used to prove the verifier binds to the pinned key.
static const char* kOtherPubKeyPem =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAExZ7nVtaHhGsbuWqYkkMBq/21FkKn\n"
    "1PcdpikQvLNV3VMQHYWm6qE202Dyw+G3T6ws1fHyLZY8qJfLvG5mwJluBg==\n"
    "-----END PUBLIC KEY-----\n";

// SHA-256("clawdmeter ota P-256 test vector\n"), the digest the signature covers.
static const char* kDigestHex =
    "127559a6d0f914eeb52c765f4641b502721c6f8d54fbe72da887bcffb8a13df8";

// DER ECDSA P-256 signature over that digest (71 bytes).
static const char* kSigDerHex =
    "3045022100be4be3ba11eeaa3fa1402864ab4141eedb8ce1f6bb89eeba"
    "1cff2b0e5971e2970220015f98670a52f4af0961c4b11174945709a457"
    "93b944ea04c7699e6fdf1e53e4";

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

#ifdef TEST_HAVE_MBEDTLS

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t hex2bin(const char* hex, unsigned char* out, size_t out_cap) {
    size_t n = 0;
    for (const char* p = hex; p[0] != '\0' && p[1] != '\0'; p += 2) {
        int hi = hex_nibble(p[0]);
        int lo = hex_nibble(p[1]);
        if (hi < 0 || lo < 0 || n >= out_cap) return 0;
        out[n++] = (unsigned char)((hi << 4) | lo);
    }
    return n;
}

static void test_ota_sig_vector(void) {
    printf("ota_sig_verify_p256: known P-256 vector\n");

    unsigned char digest[32];
    unsigned char sig[80];
    size_t dlen = hex2bin(kDigestHex, digest, sizeof(digest));
    size_t slen = hex2bin(kSigDerHex, sig, sizeof(sig));
    CHECK(dlen == 32);
    CHECK(slen == 71);

    // The pinned vector verifies.
    CHECK(ota_sig_verify_p256(digest, dlen, sig, slen, kPubKeyPem));

    // A modified digest must not verify.
    digest[0] ^= 0x01;
    CHECK(!ota_sig_verify_p256(digest, dlen, sig, slen, kPubKeyPem));
    digest[0] ^= 0x01;

    // A modified signature must not verify.
    sig[slen / 2] ^= 0x01;
    CHECK(!ota_sig_verify_p256(digest, dlen, sig, slen, kPubKeyPem));
    sig[slen / 2] ^= 0x01;

    // The signature is bound to the pinned key.
    CHECK(!ota_sig_verify_p256(digest, dlen, sig, slen, kOtherPubKeyPem));

    // Wrong digest length / malformed key / null inputs are refused.
    CHECK(!ota_sig_verify_p256(digest, dlen - 1, sig, slen, kPubKeyPem));
    CHECK(!ota_sig_verify_p256(digest, dlen, sig, slen, "not a pem"));
    CHECK(!ota_sig_verify_p256(NULL, dlen, sig, slen, kPubKeyPem));
    CHECK(!ota_sig_verify_p256(digest, dlen, NULL, slen, kPubKeyPem));
    CHECK(!ota_sig_verify_p256(digest, dlen, sig, slen, NULL));
}

#else  // !TEST_HAVE_MBEDTLS

static void test_ota_sig_vector(void) {
    printf("ota_sig_verify_p256: SKIP (no mbedTLS on the host include path)\n");
}

#endif  // TEST_HAVE_MBEDTLS

int main(void) {
    printf("== ota_sig P-256/SHA-256 verifier ==\n");
    test_ota_sig_vector();

    printf("\n%d check(s), %d failure(s)\n", checks, failures);
    if (failures) {
        printf("SOME CHECKS FAILED\n");
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
