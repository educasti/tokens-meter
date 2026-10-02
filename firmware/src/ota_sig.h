#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Detached-signature verification for pull OTA (design/ota-pull/SIGNING.md).
//
// ECDSA P-256 + SHA-256: `msg` is the 32-byte SHA-256 digest of the exact
// firmware `.bin` (mbedTLS's pk_verify takes the digest, not the raw bytes),
// `sig_der` is the DER-encoded signature from the manifest's base64 `sig`, and
// `pubkey_pem` is the pinned SubjectPublicKeyInfo PEM. Returns true only on a
// valid P-256 signature. The update flow will call this before activation
// (follow-up wiring).
bool ota_sig_verify_p256(const uint8_t* msg, size_t len,
                         const uint8_t* sig_der, size_t sig_len,
                         const char* pubkey_pem);
