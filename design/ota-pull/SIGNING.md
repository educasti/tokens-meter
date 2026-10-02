# Signed firmware updates (ECDSA P-256 + SHA-256)

Status: **contract for the P4 signed-update work.** It layers on
`design/ota-pull/DESIGN.md` without changing its transport, manifest format or
state machine. It replaces the *algorithm* proposed in DESIGN §13.3 (which
sketched Ed25519) with **ECDSA P-256 + SHA-256**, because the mbedTLS shipped
with ESP-IDF (`mbedtls_pk_verify`, `MBEDTLS_MD_SHA256`) supports P-256
reliably. Everything else in §13.3 stands.

Source of truth for the base design: `DESIGN.md` §6 (manifest, publish
guarantee) and §13.3 (future upgrade path). This document does not edit either;
read them together.

## 1. What this fixes

DESIGN §13.1 shows that TLS + SHA-256 proves **transport integrity**, not
**author authenticity**. The central residual risk (§13.2) is a compromised VM
or publish pipeline: whoever can write the firmware directory and obtain a
valid TLS certificate can serve a malicious manifest and a matching binary.
SHA-256 only proves the device got the bytes the manifest named.

A detached signature over the image, verified against a public key pinned in
firmware, closes that gap: the VM can still *serve* an update, but it cannot
*forge* one without the private key.

## 2. Algorithm

| | |
|---|---|
| Signature scheme | ECDSA |
| Curve | NIST P-256 (`prime256v1`, `secp256r1`) |
| Digest | SHA-256 |
| Signature encoding | ASN.1 DER (`EVP_*` / OpenSSL default) |
| Transport encoding | base64 (standard alphabet, single line) in the manifest |
| `sig_alg` value | `ecdsa-p256-sha256` |
| Firmware API | `ota_sig_verify_p256()` — `mbedtls_pk_parse_public_key` + `mbedtls_pk_verify(MBEDTLS_MD_SHA256)` |

P-256 is chosen over Ed25519 solely for ESP-IDF/mbedTLS availability; no other
property of the design depends on the curve.

## 3. What is signed

The signature is **detached** and covers the **exact bytes of the firmware
`.bin` file** (the same bytes `sha256` addresses). Concretely:

```
sig = ECDSA_P256_sign( SHA256(firmware.bin), private_key )
```

The device never has to buffer the 6 MB image: `st_download()` already streams
an mbedTLS SHA-256 over the bytes as they arrive (DESIGN §7.1/§7.5). The
32-byte digest that is compared against the manifest `sha256` is the same
digest fed to `mbedtls_pk_verify`, so signature verification costs one digest
comparison and one ECDSA verify — no second pass over the image.

Because `mbedtls_pk_verify()` takes the digest (not the raw message), the
firmware verifier's `msg`/`len` arguments are the **SHA-256 digest and 32**.
This is the only subtlety in the API; it is called out in `ota_sig.h`.

## 4. Manifest additions

DESIGN §6.1/§6.2 is unchanged except for three optional fields, emitted **only
when the publisher signs**:

```json
{
  "schema_version": 1,
  "board": "waveshare_amoled_216",
  "version": "0.2.0",
  "url": "clawdmeter-0.2.0.bin",
  "sha256": "<64 lowercase hex>",
  "size": 1287600,
  "mandatory": false,
  "released_at": "2026-10-02T12:00:00Z",
  "sig": "<base64 DER ECDSA P-256/SHA-256 signature>",
  "sig_alg": "ecdsa-p256-sha256",
  "key_id": "ef7cd0a16e487445"
}
```

| Field | Type | Required | Meaning / device rule |
|---|---|---|---|
| `sig` | string | with `key_id`/`sig_alg` | base64 DER signature over the `.bin` (see §3). |
| `sig_alg` | string | with `sig` | Must be exactly `ecdsa-p256-sha256`; any other value is rejected (`bad_signature`). |
| `key_id` | string | with `sig` | Opaque label identifying the pinned key that signed. Default: first 16 hex chars of the SHA-256 of the public key's DER `SubjectPublicKeyInfo`. Used for rotation/logging; it is **not** a key by itself. |

Rules:

1. The three fields are all-or-nothing. A manifest with some but not all is
   malformed (`bad_signature`).
2. An **unsigned** manifest (no `sig`) is the pre-P4 shape. During the
   transition the firmware accepts it exactly as before; once a build pins a
   key and turns verification on, a missing `sig` is `bad_signature` and no
   activation happens. Which behavior a given build has is a policy constant,
   not a manifest field.
3. Unknown fields remain ignored (DESIGN §6.2), so a signed manifest is
   backwards compatible with a pre-P4 device (which simply does not verify).
4. `sha256` remains mandatory and is verified independently of the signature:
   the signature authenticates the digest, the digest authenticates the bytes.

## 5. Pinned public key in firmware

- The public key is shipped with the firmware (planned location:
  `firmware/src/certs/`, alongside the TLS roots), as a
  `SubjectPublicKeyInfo` PEM. It is passed to
  `ota_sig_verify_p256(..., pubkey_pem)`.
- Pinning it **separately from the TLS roots** is the whole point: TLS anchors
  (DESIGN §8.1) prove the server; this key proves the *author*. A compromised
  VM or a re-issued server certificate cannot substitute it.
- The key is generated as a P-256 key; only the public half is committed.
  `tools/sign_firmware.py --key <key> --pubkey` prints the PEM to pin.
- `key_id` names the pinned key. In this phase firmware pins one key, so
  `key_id` is informational; during rotation (§9) the device may pin the old
  and new keys and pick by `key_id`.

## 6. Publish step (signs after hashing)

`design/ota-pull/deploy/ota-publish.sh` remains the only writer (DESIGN §6.5).
The frozen CLI is unchanged; two options are added:

```bash
design/ota-pull/deploy/ota-publish.sh \
    --bin firmware/.pio/build/waveshare_amoled_216/firmware.bin \
    --version 0.2.0 \
    --sign-key /run/secrets/ota-p256.pem \
    [--key-id field-key-2026] \
    [--dest user@vm.example.com:/srv/firmware]
```

Order of operations (DESIGN §6.5, extended):

1. compute `sha256` + `size` of the built `.bin`;
2. **sign the exact same `.bin` bytes** with the private key via
   `tools/sign_firmware.py` (openssl `dgst -sha256 -sign`), producing base64
   DER;
3. determine `key_id` — `--key-id` if given, otherwise the public-key
   fingerprint (§4);
4. render the manifest with `sig` / `sig_alg` / `key_id` added;
5. upload binary then manifest atomically, prune, alias (unchanged).

Guarantees:

- The private key **never enters the repository**. It is supplied by path at
  publish time (a CI secret or the VM's keystore/HSM) and only the public key
  is pinned in firmware. CI wiring (P2) injects it as a secret.
- `--dry-run` keeps its stdout contract: the rendered JSON is the only stdout
  content, logged lines stay on stderr, and signing happens before the manifest
  is emitted.
- Without `--sign-key`, the manifest is byte-for-byte the pre-P4 output — no
  `sig` fields, no openssl requirement.
- The publish script refuses a non-P-256 key and a missing key file before it
  writes anything.

## 7. Device verification before activation

The pull state machine (DESIGN §7.1) gains the signature check inside
`VERIFY_SHA256`, after `esp_ota_end()` image validation and **before**
`FLASH_INACTIVE_SLOT` (the `esp_ota_set_boot_partition()` activation):

```
DOWNLOAD → VERIFY_SHA256:
    finish streaming SHA-256; esp_ota_end()
    compare digest == manifest.sha256            # existing D3 check
    ota_sig_verify_p256(digest, 32,
                        base64_decode(manifest.sig),
                        manifest.sig,
                        PINNED_PUBKEY_PEM)        # new
      ├─ ok    → FLASH_INACTIVE_SLOT (activate)
      └─ false → esp_ota_abort(); err = "bad_signature"; radio off; no reboot
```

Invariants:

- **Nothing is activated before both checks pass.** The running slot is never
  touched; `otadata` is unchanged on failure.
- A missing, malformed, wrong-algorithm or wrong-key signature is
  `bad_signature`, treated as non-retryable in the same class as
  `hash_mismatch` (DESIGN §7.3) — it is not a transient error.
- The new `err` value `bad_signature` belongs to the frozen
  `{"ok":false,"err":…}` family (DESIGN §9.2); it never activates.

> This phase only **lands the verifier + tooling + contract**. Wiring the call
> into `ota_pull.cpp`, the manifest parser fields, the pinned PEM and the
> `bad_signature` error string is a follow-up that owns those files. Until it
> lands, no behavior changes for the device.

## 8. Threat model delta

Adds, on top of DESIGN §13.1:

- **Compromised VM / publish pipeline** (the §13.2 headline risk): the attacker
  can serve a manifest and a binary whose `sha256` matches, but cannot produce a
  `sig` that verifies under the pinned public key. The device downloads,
  rejects at `bad_signature`, aborts and keeps running the old image.
- **Compromised CA / TLS private key**: same — TLS is no longer the only thing
  standing between an attacker and an activation.
- **Manifest tampering in transit**: already covered by TLS; the signature adds
  authenticity even if TLS is broken.

Residual trust (what signing does **not** fix):

- **Whoever holds the private key can sign a malicious image.** The key is the
  new root of trust; its custody (CI secret store, VM permissions, operator
  workstation) is the security boundary. A leaked key is equivalent to the old
  compromise until the key is rotated and the device updated.
- **A publish host that can read the key at signing time** can exfiltrate it.
  Prefer a short-lived key, a hardware keystore/HSM, or an offline signing step.
- **Whoever can replace the pinned public key in a firmware update** can
  re-anchor trust — but that update itself must be signed by the currently
  pinned key, so an attacker without the key cannot rotate to their own.
- **Physical attacker / boot chain**: not addressed. No Secure Boot v2, no
  flash encryption, so a physical attacker can still replace flash directly
  (DESIGN §13.2, §13.3 item 4). That is an irreversible eFuse step and remains
  a separate project.
- **Hybrid `espota` path**: still unsigned and trusted as the manual path
  (DESIGN §13.3 item 5). It must not be treated as equivalent to the signed
  pull path.
- **No per-device authorization**: firmware is still public static content;
  signing authenticates the author, not the requesting device (DESIGN §1.2).

## 9. Key management and rotation

- Generate: `openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out ota-p256.pem`.
- Pin: `tools/sign_firmware.py --key ota-p256.pem --pubkey` → commit the PEM
  under `firmware/src/certs/`.
- Sign at publish: `--sign-key /secure/path/ota-p256.pem` (never committed).
- The private key file must be readable only by the publish identity
  (`chmod 600`, CI secret, or keystore).
- **Rotation**: add the new public key to the firmware alongside the old one
  and ship that build (signed by the old key), then start signing with the new
  key and a new `key_id`. The device accepts a signature from any pinned key
  whose `key_id` matches; once every device has the new anchor, drop the old.
- Compromise response: publish a signed firmware update that pins a fresh key
  and revokes the old one, using the still-trusted key if available; otherwise
  USB reflash is the final fallback (DESIGN §11).

## 10. Tooling reference

```bash
# one-time: generate the key (out of repo)
openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out ota-p256.pem

# print the public key to pin in firmware
tools/sign_firmware.py --key ota-p256.pem --pubkey

# sign a built image (base64 DER on stdout, key_id derived by the caller)
tools/sign_firmware.py --bin firmware.bin --key ota-p256.pem

# full publish with signing
design/ota-pull/deploy/ota-publish.sh \
    --bin firmware/.pio/build/waveshare_amoled_216/firmware.bin \
    --version 0.2.0 --sign-key ota-p256.pem
```

`tools/sign_firmware.py` refuses RSA/Ed25519/P-384 keys so a mis-signed
manifest cannot be produced silently.

## 11. Verification and tests

- **Host (Python)**: `daemon/tests/test_sign_firmware.py` generates a throwaway
  P-256 key in a temp dir, signs a temp `.bin`, verifies the base64 DER with
  `openssl dgst -sha256 -verify`, checks `--pubkey` against
  `openssl pkey -pubout`, and confirms tampered input / wrong key / non-P-256
  key are refused. It also drives `ota-publish.sh --dry-run --sign-key` and
  verifies the manifest `sig` end to end.
- **Host (C++)**: `firmware/test/test_ota_sig/run_test.sh` compiles
  `ota_sig.cpp` against host mbedTLS and runs a fixed P-256 vector when
  `<mbedtls/pk.h>` is available; otherwise it compile-checks the header and
  prints `SKIP`. No network.
- **Hardware (follow-up)**: signed good/bad images must show the correct/`bad_signature`
  outcome; a wrong-key signature and an unsigned manifest are both rejected
  before activation. Extends the P4 acceptance criteria in `IMPL.md` §7.3.

## 12. References

- `design/ota-pull/DESIGN.md` §6 (manifest, publish guarantee) and §13.3
  (signed-image upgrade path; Ed25519 sketch superseded here by P-256).
- `design/ota-pull/IMPL.md` §7 (P4 work package).
- `firmware/src/ota_sig.h` / `ota_sig.cpp` — the verifier.
- `tools/sign_firmware.py`, `design/ota-pull/deploy/ota-publish.sh` — signing
  and publish.
