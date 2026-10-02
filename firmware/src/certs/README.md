# `firmware/src/certs/` — pinned TLS trust anchors

Pull-OTA pins the update server's **self-signed** certificate because the
origin is a bare public IP (no DNS name to chain to Let's Encrypt). This
replaces the old ISRG Root X1/X2 bundle. See `design/ota-pull/DESIGN.md`
section 8 and the TLS comment block in `../ota_pull.cpp`.

## Files

- `pinned_server_pem.h` — **generated, not committed.** `ota_pull.cpp` includes
  it and passes `PINNED_SERVER_PEM` to
  `WiFiClientSecure::setCACert()` (verification stays ON; `setInsecure()` is
  never called).

## How it is produced

`firmware/scripts/gen_pinned_cert.py` runs as a PlatformIO pre-build extra
script (`firmware/platformio.ini`) and as a standalone CLI. It reads the real,
untracked cert:

```
firmware/certs/pinned_server.pem      # frozen; SAN carries the update IP
```

and writes `pinned_server_pem.h`. On a clean checkout / CI the input is
absent, so the script emits a **clearly-marked placeholder** that cannot be
parsed as a certificate: the build still succeeds, but every TLS handshake
fails **closed** until the real cert is installed. Verification is never
silently disabled.

## Hostname matching

The cert must carry the update host's address as an **iPAddress SAN** (a cert
with a SAN is matched against the SAN only — the CN is ignored). The URL host
in `-DOTA_PULL_MANIFEST_URL` must be the same bare IP literal. mbedTLS 3.6.x
(arduino-esp32 3.3.8) supports IPAddress SAN verification.
