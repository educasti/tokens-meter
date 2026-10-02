// ECDSA P-256 / SHA-256 verifier for a detached firmware signature
// (design/ota-pull/SIGNING.md). Small and focused: parse the pinned public
// key, insist it is P-256, and verify the DER signature over the SHA-256
// digest. The update flow (ota_pull.cpp st_verify) will call this before
// activation in a follow-up; nothing here touches the flash or the network.
#include "ota_sig.h"

#include <string.h>

// The native simulator has no mbedTLS, and a non-IDF host may not either. The
// hardware build always has it. Keep the sim build green (and the host
// compile-check meaningful) with a stub that never accepts a signature.
#if defined(BOARD_SIM)
#  define OTA_SIG_STUB 1
#elif defined(__has_include)
#  if !__has_include(<mbedtls/pk.h>)
#    define OTA_SIG_STUB 1
#  endif
#else
#  define OTA_SIG_STUB 1
#endif

#ifndef OTA_SIG_STUB

#include <mbedtls/ecp.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>

bool ota_sig_verify_p256(const uint8_t* msg, size_t len,
                         const uint8_t* sig_der, size_t sig_len,
                         const char* pubkey_pem) {
    if (msg == NULL || sig_der == NULL || pubkey_pem == NULL) return false;
    if (len != 32) return false;      // mbedTLS pk_verify wants the SHA-256 digest
    if (sig_len == 0) return false;

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);

    int rc = mbedtls_pk_parse_public_key(
        &pk, (const unsigned char*)pubkey_pem, strlen(pubkey_pem) + 1);
    if (rc != 0) {
        mbedtls_pk_free(&pk);
        return false;
    }

    // Pin the algorithm as well as the key: a P-384 or RSA PEM must not pass
    // just because the signature happens to verify under it.
    if (mbedtls_pk_get_type(&pk) != MBEDTLS_PK_ECKEY) {
        mbedtls_pk_free(&pk);
        return false;
    }
    mbedtls_ecp_keypair* ec = mbedtls_pk_ec(pk);
    if (ec == NULL ||
        mbedtls_ecp_keypair_get_group_id(ec) != MBEDTLS_ECP_DP_SECP256R1) {
        mbedtls_pk_free(&pk);
        return false;
    }

    rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, msg, len, sig_der, sig_len);
    mbedtls_pk_free(&pk);
    return rc == 0;
}

#else  // OTA_SIG_STUB

bool ota_sig_verify_p256(const uint8_t* msg, size_t len,
                         const uint8_t* sig_der, size_t sig_len,
                         const char* pubkey_pem) {
    (void)msg;
    (void)len;
    (void)sig_der;
    (void)sig_len;
    (void)pubkey_pem;
    return false;
}

#endif  // OTA_SIG_STUB
