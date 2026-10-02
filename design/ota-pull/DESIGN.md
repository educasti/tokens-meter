# Pull-based automatic OTA — Waveshare ESP32-S3-Touch-AMOLED-2.16

Status: **frozen contract for implementation (design phase, not built).** This
document is **additive** to `design/ota-hybrid/DESIGN.md`; it does not change the
hybrid contract or the BLE protocol that is already frozen there.

## 1. Summary

Today the device can only be updated when a host on the same LAN runs
`daemon/ota_flash.py`: BLE is the trigger, WiFi is the transfer, and the host
pushes the binary with `espota` (`README.md` "Firmware updates (hybrid OTA)",
lines 345-379; `design/ota-hybrid/DESIGN.md` §1-§3). The device never learns
that a new version exists on its own.

Pull-based automatic OTA closes that gap. When the owner publishes a new
firmware version to the always-on VM (`design/backend-wifi/ROADMAP.md` D3, D8),
the device discovers it and updates itself over WiFi:

- The device checks on **boot** and every **24 h** while it has stored WiFi
  credentials.
- It fetches a small JSON **manifest** over HTTPS, compares versions, downloads
  the binary, verifies its SHA-256, flashes the **inactive** slot, reboots, and
  lets the existing application-level rollback confirm the boot.
- No host push and **no same-LAN requirement**: the device only needs ordinary
  internet access to the VM.

### 1.1 Goal

1. A device in the field reaches a newly published version with no host action.
2. The update path is safe: it can never brick the device, never writes the
   running slot, and aborts before activating on any integrity failure.
3. The host-pushed hybrid path keeps working unchanged as the manual / BLE
   fallback.
4. The design reuses the infrastructure already decided for the backend: the
   owner's always-on Oracle Cloud VM, Caddy, Let's Encrypt, static files.

### 1.2 Non-goals (this phase)

- No dynamic backend, database, device token, or per-device authorisation for
  OTA (contrast the usage backend in `design/backend-wifi/ROADMAP.md` §4-§5).
  Firmware is public-by-obscurity static content, protected by TLS and a hash.
- No static image signing, Secure Boot, or flash encryption. Their residual
  threat and the future upgrade path are in §13.
- No host-side push, no `espota`, no mDNS, no same-LAN dependency.
- No change to the partition table: `firmware/partitions/waveshare_amoled_216.csv`
  already has dual 6 MB app slots (`app0` / `app1`) plus `otadata`.
- No change to the frozen BLE commands or their reply shapes; new commands are
  additive (§9).

## 2. Relationship to hybrid OTA

Pull is a **second user of the same radio and the same NVS namespace**, not a
replacement.

| Aspect | Hybrid (existing) | Pull (this document) |
|---|---|---|
| Trigger | Host over BLE CTRL `…0005` | Device timer (boot + 24 h) or BLE `{"cmd":"update"}` |
| Binary source | Host on the LAN (`espota` push) | VM over HTTPS (device GET) |
| Same-LAN needed | Yes (`README.md` lines 372-373) | No |
| Host helper | `daemon/ota_flash.py` (kept as-is) | none |
| WiFi creds | NVS `otah` / `ssid` + `pass` (`ota.cpp` lines 30-33) | same keys |
| Rollback | App-level `boot_tries` (`ota.cpp` §4, lines 96-151) | same counter, same `ota_confirm()` |
| Radio ownership | `ota.cpp` `s_state` + `WiFi.mode()` | shares a single WiFi owner lock |

Rules:

1. **Additive.** `daemon/ota_flash.py`, the CTRL commands, and the `espota`
   upload path are unchanged. Pull adds a new source and a new trigger.
2. **Mutual exclusion.** Hybrid and pull must never run at the same time. Both
   bring up `WIFI_STA`, and both call `WiFi.disconnect(true); WiFi.mode(WIFI_OFF)`
   on teardown (`ota.cpp` lines 252-260). The implementation introduces a single
   WiFi owner (see §7.4): pull refuses to start while hybrid OTA is
   `OTA_MODE_CONNECTING` / `OTA_MODE_READY`, and a hybrid `{"cmd":"ota","mode":"on"}`
   is answered `{"ok":false,"err":"busy"}` while a pull check/update is running.
   This mirrors the existing `busy` reply for a double hybrid start
   (`ota.cpp` lines 342-343).
3. **Shared credentials.** Pull requires the same stored WiFi credentials the
   hybrid path provisions. If none are stored, pull does nothing (exactly as
   `{"cmd":"ota","mode":"on"}` answers `no_wifi`, `ota.cpp` lines 301-302).
4. **Shared confirmation.** A successful pull update is confirmed by the same
   `ota_confirm()` call sites (`main.cpp` lines 452, 460, 482) and the same
   `HEALTHY_MS` 60 s rule (`ota.cpp` lines 37, 327-329). The pull path adds no
   second rollback mechanism.

## 3. Decisions

Owner decisions are encoded verbatim; derived decisions are numbered D5+.

| # | Decision | Why / source |
|---|---|---|
| **D1** | Update source is the owner's **always-on Oracle Cloud VM**, served by **Caddy with Let's Encrypt HTTPS** as static files. No dynamic backend for OTA. | Owner; `design/backend-wifi/ROADMAP.md` D3 (line 19), D8 (line 24), §10 (lines 98-110) |
| **D2** | Check on **boot** and every **24 h** while stored WiFi credentials exist. Optionally skip on battery below a threshold (recommended 20 %, open question Q3). | Owner |
| **D3** | Integrity is **HTTPS against a bundled CA root** plus **SHA-256 of the binary checked against the manifest**. Static signing / Secure Boot / flash encryption are out of scope; residual threat and future path in §13. | Owner |
| **D4** | **Design only** this phase; implementation in a later phase (`IMPL.md`). | Owner |
| **D5** | Transport is `WiFiClientSecure` + `HTTPClient`; flashing is raw ESP-IDF `esp_ota_begin/write/end` + `esp_ota_set_boot_partition`; hashing is mbedtls SHA-256. `esp_https_ota` is **rejected**. | §7.5 — it must be possible to compare the manifest's SHA-256 *before* activating; `esp_https_ota` fuses download+verify+activate and does not expose the raw-file digest. |
| **D6** | Canonical URL layout is **per board**: `/firmware/<board>/manifest.json` and `/firmware/<board>/clawdmeter-<version>.bin`. A flat `/firmware/…` alias is kept for the single-board convenience path. | §6.4 — the device knows `board_caps().id`; a second board must not collide. |
| **D7** | Version is **semver `MAJOR.MINOR.PATCH`**, driven by a git tag `vX.Y.Z`, injected at build time with a PlatformIO pre-build script. | §5 |
| **D8** | Updates apply only when `manifest.version` is **strictly greater** than the running version. Downgrade is possible only through an owner-only BLE `force` with an explicit target. | §5.4 |
| **D9** | The pull engine runs as a **dedicated FreeRTOS task** (12 KB stack, core 0), not on the Arduino loop task. `ota_tick()` stays non-blocking and owns the UI. | §8.3 |
| **D10** | A check is a **short, radio-on window**; on success or failure the radio is turned off again (`WiFi.mode(WIFI_OFF)`), matching the existing posture (`ota.cpp` lines 252-260; `ROADMAP.md` §8). | §10 |

## 4. Actors and architecture

Three actors:

1. **Build / CI (repo).** `pio run` produces `firmware/.pio/build/<env>/firmware.bin`
   (`daemon/ota_flash.py` usage header). A publish step computes SHA-256 + size
   and writes the manifest (`deploy/ota-publish.sh`, `deploy/Caddyfile.example`).
2. **VM (always-on).** The owner's Oracle Cloud VM already chosen for the usage
   backend (`design/backend-wifi/ROADMAP.md` D3). Caddy terminates TLS with a
   Let's Encrypt certificate and serves a static firmware directory. No dynamic
   OTA service.
3. **Device firmware.** A new pull engine (state machine §7) that uses the
   existing NVS credentials (`otah`) and the existing inactive slot / rollback
   machinery (`ota.cpp` §4).

```
  Build / CI (repo)                  Oracle Cloud VM (always-on)            Device (ESP32-S3)
  ─────────────────                  ───────────────────────────            ─────────────────
  pio run → firmware.bin             Caddy :443 (Let's Encrypt TLS)         ota_pull task
  deploy/ota-publish.sh              /var/www/firmware/<board>/             ├─ WiFi STA (on demand)
    sha256 + size + version            manifest.json                        ├─ SNTP (clock for TLS)
    tmp upload + atomic mv             clawdmeter-<version>.bin  ◄─ HTTPS ──┤ TLS vs pinned ISRG roots
    keep last N versions                                        GET        ├─ SHA-256 vs manifest
                                                                          └─ esp_ota_* → inactive slot
```

The device is a **pull-only, read-only** client. It never writes to the VM, and
the VM needs no per-device state for OTA. This is deliberately simpler than the
usage backend's bearer-token model (`ROADMAP.md` §4-§5).

## 5. Versioning

### 5.1 Format

- `FW_VERSION` is `MAJOR.MINOR.PATCH`, e.g. `0.2.0`. Strict form:
  `^[0-9]+\.[0-9]+\.[0-9]+$` after stripping an optional leading `v`.
- Pre-release / build-metadata suffixes (e.g. `-rc1`) are **not** supported in
  v1. A manifest whose version does not match the strict form is rejected
  (`{"err":"bad_version"}`). This keeps the comparator small and unambiguous.
- The manifest version and the binary's `FW_VERSION` must be equal; a mismatch is
  caught because the flashed image's `{"cmd":"info"}` will report a different
  `fw` after reboot (the publish script guarantees equality by construction,
  §6.5).

### 5.2 Build-time injection

Today `FW_VERSION` is a hand-edited build flag: `-DFW_VERSION=\"0.1.0\"` in
`firmware/platformio.ini` line 20, with a `"dev"` fallback in `ota.cpp`
lines 25-27. Replace the hand-edited value with a generated one.

Recommended: a PlatformIO **pre-build extra script** for the hardware env:

```ini
; firmware/platformio.ini (env:waveshare_amoled_216)
extra_scripts = pre:scripts/version.py
```

`scripts/version.py` runs `git` in the project dir and appends build flags:

| Macro | Source | Example |
|---|---|---|
| `FW_VERSION` | `git describe --tags --match 'v*' --abbrev=0` → strip `v`; `0.0.0-dev` if no tag / not a git tree | `0.2.0` |
| `FW_GIT_SHA` | `git rev-parse --short=7 HEAD` | `abcdef0` |
| `FW_BUILD_DATE` | UTC now, ISO-8601 seconds | `2026-10-02T12:00:00Z` |

Rules for the script:

- Never fail the build. Outside a git checkout (CI tarball, release zip) fall
  back to `FW_VERSION="0.0.0-dev"`, empty SHA, and the current UTC date.
- If HEAD is dirty (`--dirty`), still use the nearest tag for `FW_VERSION` but
  leave the SHA as-is; the publish step refuses to publish a dirty tree unless
  `--allow-dirty` is passed (`deploy/ota-publish.sh`).
- The exact commit is carried in `FW_GIT_SHA`, not in `FW_VERSION`, so the
  version stays semver-clean.

Alternative (documented, not preferred): CI passes
`PLATFORMIO_BUILD_FLAGS="-DFW_VERSION=0.2.0 -DFW_GIT_SHA=... -DFW_BUILD_DATE=..."`.
The pre-build script is preferred because a local `pio run` and CI produce
identical stamps with no duplicated logic.

### 5.3 `{"cmd":"info"}` grows

The frozen identity reply (`design/ota-hybrid/DESIGN.md` §2; `ota.cpp`
lines 277-284) gains two additive fields:

```json
{"ok":true,"board":"waveshare_amoled_216","fw":"0.2.0","sha":"abcdef0",
 "build":"2026-10-02T12:00:00Z","id":"AA:BB:CC:DD:EE:FF"}
```

- `fw`, `board`, `id` keep their exact current meaning and are unchanged.
- `sha` and `build` are new and optional to consumers.
- Backwards compatibility: `daemon/ota_flash.py` reads only `board` (via
  `assert_board`, lines 150-160), logs `fw` (line 570), and otherwise ignores
  unknown fields, so the larger reply is safe. The existing 192-byte buffer
  (`ota.cpp` line 278) still fits: the reply above is ~140 bytes. If a future
  field pushes past the buffer, the build date is shortened to `YYYY-MM-DD`
  first.

### 5.4 Monotonic rules, downgrade and replay

Let `running` be `ota_version()` (`ota.cpp` line 59) and `m` the manifest.

1. **Apply only if `semver_cmp(m.version, running) > 0`.** Equal or lower is a
   terminal `no_update` (or `{"err":"no_update"}` for a BLE check).
2. **`min_from` (optional, string).** If `semver_cmp(running, m.min_from) < 0`,
   the device must not apply this manifest directly; it reports `too_old` and
   waits for an intermediate release. This protects migrations that assume a
   newer NVS schema. `min_from` is ignored when absent.
3. **`force` (optional, bool).** A manifest with `"force":true` means the update
   is a security fix; it bypasses the battery/charging gate and `min_from`, but
   **never** the SHA-256 check, the board check, or the version-strictly-greater
   rule.
4. **BLE owner force / downgrade.** The only way to apply a version equal to or
   lower than the running one is an explicit owner command on CTRL `…0005`:
   `{"cmd":"update","force":true,"to":"X.Y.Z"}`. The `to` value must equal the
   manifest version; otherwise `{"err":"target_mismatch"}`. The command is only
   accepted on a bonded+encrypted owner link (already enforced by `WRITE_ENC` and
   the owner check, `ble.cpp` lines 338-357). This is the rollback path for a bad
   release, and it still requires a valid SHA-256.
5. **Replay / downgrade protection.** Because activation requires a strictly
   greater version, replaying an older manifest — a stale CDN edge, a
   rollback of the VM directory, or a captured response — cannot downgrade a
   device. TLS prevents a network attacker from substituting any manifest at
   all. An attacker who controls the VM *and* the TLS private key could publish
   an older version, but `min_from` and the strictly-greater rule still force
   monotonicity unless they also set `force:true`; `force` is honored for
   auto-updates only when the version is greater (it only bypasses gates, never
   the direction). See §13.

### 5.5 Semver comparator

```
semver_cmp(a, b) -> -1 | 0 | 1
  parse a and b as three unsigned integers (reject on any non-numeric part)
  compare MAJOR, then MINOR, then PATCH
```

Pure, transport-free, unit-tested on the host (`IMPL.md` P1 test plan).

## 6. Update manifest

### 6.1 Schema

```json
{
  "schema_version": 1,
  "board": "waveshare_amoled_216",
  "version": "0.2.0",
  "url": "clawdmeter-0.2.0.bin",
  "sha256": "<64 lowercase hex chars>",
  "size": 1287600,
  "min_from": "0.1.0",
  "mandatory": false,
  "released_at": "2026-10-02T12:00:00Z",
  "notes": "Fix WiFi reconnect on captive portals."
}
```

### 6.2 Field table

| Field | Type | Required | Meaning / device rule |
|---|---|---|---|
| `schema_version` | integer | yes | Manifest format version. Device accepts `1`; anything else → `{"err":"bad_schema"}`. |
| `board` | string | yes | Must equal `board_caps().id` (`board_caps.h` line 12), currently `waveshare_amoled_216`. Mismatch → `{"err":"board_mismatch"}`. Never flash a foreign board. |
| `version` | string | yes | semver `X.Y.Z` (§5.1). Drives the comparison. |
| `url` | string | yes | **Relative** path to the binary, resolved against the manifest's directory. Absolute URLs, other origins, or `..` → `{"err":"bad_url"}` (keeps the pinned TLS origin meaningful). |
| `sha256` | string | yes | 64 lowercase hex chars; SHA-256 of the exact `.bin` file. Compared after download (§7.3). |
| `size` | integer | yes | Exact byte length; must match the HTTP `Content-Length` and the bytes flashed, and must fit the inactive slot. |
| `min_from` | string | no | Oldest running version allowed to apply this manifest directly (§5.4). |
| `mandatory` | boolean | no (default `false`) | UI/priority hint and a battery-gate bypass (not a security bypass). |
| `released_at` | string | no | ISO-8601 UTC. Sanity check: reject more than 24 h in the future (clock/rollback guard). |
| `notes` | string | no | Human-readable; shown in logs, never parsed for control flow. |

Unknown fields are ignored (forward compatibility).

### 6.3 Caching

| Resource | Header | Reason |
|---|---|---|
| `manifest.json` | `Cache-Control: no-cache` (or `max-age=60, must-revalidate`) + `ETag: "<first 16 hex of sha256>"` | The device always wants the current pointer; the ETag lets it send `If-None-Match` and treat `304 Not Modified` as "no update". |
| `clawdmeter-<version>.bin` | `Cache-Control: public, max-age=31536000, immutable` | Filename is content-addressed by version; safe to cache forever. |

The device sends `If-None-Match` only if it stored the last ETag; it is a
bandwidth optimisation, not a correctness requirement. Caddy's `file_server`
sends `ETag` and `Last-Modified` automatically; the explicit `Cache-Control`
comes from the site block (`deploy/Caddyfile.example`).

### 6.4 URL layout and per-board handling

Canonical (D6):

```
https://<VM_HOST>/firmware/<board>/manifest.json
https://<VM_HOST>/firmware/<board>/clawdmeter-<version>.bin
```

`<board>` is `board_caps().id` (`waveshare_amoled_216`). The device requests
only its own directory, so adding a second board later is a matter of publishing
a second directory — no firmware change, no manifest multiplexing.

Single-board convenience alias (matches the owner's example):

```
https://<VM_HOST>/firmware/manifest.json
https://<VM_HOST>/firmware/clawdmeter-<version>.bin
```

The alias is produced by the publish script (copy or Caddy `handle`/`rewrite`)
and is for humans, tools, and the currently single supported board. The device
uses the canonical per-board path. If the canonical manifest is absent
(`404`), the device does **not** fall back to the flat path; a missing manifest
is just `no_update` for that cycle (this avoids silently serving the wrong
board).

### 6.5 Publish guarantee

`deploy/ota-publish.sh` is the only writer. It:

1. computes `sha256sum` and `stat -c %s` on the built `.bin`;
2. writes the manifest with the version taken from `git describe` (or an
   explicit `--version`), which is the same value the pre-build script injected
   into `FW_VERSION` (§5.2), so `manifest.version == info.fw`;
3. uploads to a temp path over `ssh`/`rsync` and `mv`s into place (atomic
   rename), so a device never sees a half-written manifest or binary;
4. keeps the last N versions and prunes older ones.

## 7. Device update flow

### 7.1 State machine

```
IDLE → WIFI_JOIN → SNTP_TIME_SYNC → FETCH_MANIFEST → COMPARE
     → DOWNLOAD → VERIFY_SHA256 → FLASH_INACTIVE_SLOT → REBOOT → CONFIRM
```

`CONFIRM` is not owned by the pull task: it is the existing post-reboot
confirmation path (`ota_init()` arms `boot_tries`, `ota_confirm()` clears it,
`ota.cpp` lines 96-151 and `main.cpp` lines 452/460/482). It is listed to make
the end-to-end loop explicit.

| State | Work | Timeout | Success → | Failure → | Retries |
|---|---|---|---|---|---|
| `IDLE` | Gate: credentials stored, hybrid idle, schedule due, battery ok | — | `WIFI_JOIN` | stay `IDLE` | — |
| `WIFI_JOIN` | `WiFi.mode(WIFI_STA); WiFi.begin(ssid,pass)` | 20 s (`WIFI_JOIN_MS`, `ota.cpp` line 36) | `SNTP_TIME_SYNC` | `WIFI_FAIL` | 3× backoff 5/15/45 s |
| `SNTP_TIME_SYNC` | `configTime()`; wait for a valid clock | 10 s | `FETCH_MANIFEST` | `SNTP_FAIL` | 2× (re-request) |
| `FETCH_MANIFEST` | HTTPS GET `manifest.json` (`If-None-Match`) | 15 s | `COMPARE` (or `NO_UPDATE` on `304`) | `HTTP_FAIL` | 3× backoff 2/4/8 s |
| `COMPARE` | `semver_cmp`, `board`, `min_from`, `schema_version` | instant | `DOWNLOAD` | `NO_UPDATE` / `REJECT` | — |
| `DOWNLOAD` | HTTPS GET binary; stream to inactive slot; incremental SHA-256; progress notify | 15 s idle-read; 300 s total | `VERIFY_SHA256` | `DOWNLOAD_FAIL` → `esp_ota_abort()` | 2× whole download |
| `VERIFY_SHA256` | finish digest; `esp_ota_end()` image validation; compare hex | instant | `FLASH_INACTIVE_SLOT` | `HASH_MISMATCH` / `BAD_IMAGE` | none |
| `FLASH_INACTIVE_SLOT` | activation: `esp_ota_set_boot_partition(inactive)` writes `otadata` | 1 s | `REBOOT` | `ACTIVATE_FAIL` | none |
| `REBOOT` | delay 300 ms; `esp_restart()` | — | `CONFIRM` (next boot) | — | — |
| `CONFIRM` | existing `ota_init()` / `ota_confirm()` / rollback | 60 s (`HEALTHY_MS`) | counter → 0 | after `MAX_BOOT_TRIES` → rollback | — |

Physical note on the ordering. A 6 MB binary cannot be buffered in RAM. The
bytes land in the inactive slot **during `DOWNLOAD`** (`esp_ota_begin` +
`esp_ota_write`), while a streaming mbedtls SHA-256 is updated. `VERIFY_SHA256`
then finishes the digest and calls `esp_ota_end()` (which validates the image),
and only if both pass does `FLASH_INACTIVE_SLOT` perform the **activation**
(`esp_ota_set_boot_partition`). The name is kept from the task's requested
sequence; functionally it is the commit step, and it is the only step that makes
the new image bootable. Nothing is activated before verification.

### 7.2 Interaction with the existing rollback

- The running slot is never touched: the target is
  `esp_ota_get_next_update_partition(NULL)`, and the code asserts the target is
  not `esp_ota_get_running_partition()` (the same guard the rollback uses,
  `ota.cpp` lines 111-113; `Updater.cpp:232` does the same for `Update`).
- After `FLASH_INACTIVE_SLOT`, `otadata` points at the new slot. On the next
  boot `ota_init()` increments `boot_tries` (`ota.cpp` lines 141-146). If the new
  image fails to confirm after `MAX_BOOT_TRIES` (3), `rollback_and_restart()`
  points `otadata` back at the last-known-good slot and reboots
  (`ota.cpp` lines 110-128).
- The pull task additionally writes the activated version to NVS `pend_ver`
  before rebooting; `ota_confirm()` clears it on confirmation (an implementation
  detail for `IMPL.md`; the counter semantics do not change).

### 7.3 Retry, backoff and failure classification

- Transient failures (join timeout, DNS/TLS/connect, 5xx, read timeout) retry
  with exponential backoff. Recommended schedule per check: 5 s → 15 s → 45 s →
  2 min → 10 min, at most 5 attempts, then give up until the next 24 h tick.
- `429 Too Many Requests`: honor `Retry-After` when present and ≤ 24 h.
- Non-retryable: `board_mismatch`, `bad_schema`, `bad_version`, `bad_url`,
  `hash_mismatch`, `bad_image`, `target_mismatch`, `too_old`, and HTTP 4xx other
  than 429.
- `defer_until` is stored in NVS (epoch) so a reboot mid-backoff does not hammer
  the VM. `chk_fail` counts consecutive failed checks; after 5 it resets and
  waits for the next cadence.
- Every terminal outcome turns the radio off (`WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF)`, as `ota.cpp` lines 254-255) and releases the WiFi owner.

### 7.4 WiFi ownership and mutual exclusion

A single owner of `WIFI_STA` is introduced:

```c
typedef enum { WIFI_OWNER_NONE, WIFI_OWNER_HYBRID, WIFI_OWNER_PULL } wifi_owner_t;
bool ota_wifi_acquire(wifi_owner_t who);  // false if held by someone else
void ota_wifi_release(void);              // disconnect + WiFi.mode(WIFI_OFF)
```

`ota.cpp`'s `wifi_begin()` / `ota_stop_now()` (lines 216-260) and the pull
engine both route through it. Pull refuses to start while
`ota_is_active()` is true (`ota.cpp` line 176) or `s_state != OTA_MODE_IDLE`;
hybrid `REQ_START` answers `{"ok":false,"err":"busy"}` while a pull check runs.
This is the "must never run at the same time" rule from §2.

### 7.5 API choice (D5)

**Recommendation:** `WiFiClientSecure` + `HTTPClient` for the transport, and the
raw ESP-IDF OTA API for flashing:

| Step | API |
|---|---|
| TLS GET | `WiFiClientSecure` (`libraries/NetworkClientSecure/src/WiFiClientSecure.h`, a typedef of `NetworkClientSecure`) with `setCACert(pem)`; `HTTPClient` for status/headers/body streaming. |
| Select slot | `esp_ota_get_next_update_partition(NULL)`; assert `!= esp_ota_get_running_partition()`. |
| Flash | `esp_ota_begin(part, OTA_SIZE_UNKNOWN, &handle)`, `esp_ota_write(handle, buf, n)`, `esp_ota_end(handle)`. |
| Hash | `mbedtls/sha256.h`: `mbedtls_sha256_starts/update/finish` (IDF 5.x also offers `psa_hash_*`). |
| Activate | `esp_ota_set_boot_partition(part)` — the same call the rollback uses (`ota.cpp` line 120). |

Why not `esp_https_ota`: it fuses download, image verification and activation
into one call and does not expose the raw `.bin` SHA-256 needed to compare
against the manifest (D3). Its Arduino wrapper `HttpsOTAUpdate` also defaults
`skip_cert_common_name_check=true` (`libraries/Update/src/HttpsOTAUpdate.h`),
which violates D3. `esp_https_ota` remains the documented fallback only if the
manifest hash is ever dropped.

Why not the Arduino `Update` class: `Update.end()` implicitly calls
`esp_ota_set_boot_partition()` (`Updater.cpp:671`), so it cannot express
"verify the manifest hash *before* activating" as a separate gate. `Update` is
fine for the hybrid `espota` path (which does not carry a manifest); the pull
path needs the lower-level split.

## 8. HTTPS / TLS

### 8.1 Bundled root CA

- Pin a PEM bundle in firmware containing **ISRG Root X1** (RSA) and
  **ISRG Root X2** (ECDSA), the two trust anchors. Optionally include the
  **ISRG Root X1 cross-sign** (X1 signed by IdenTrust DST Root CA X3, now
  retired) for legacy trust stores; it is not required by a device that carries
  its own trust anchor, so it may be omitted.
- Let's Encrypt's current hierarchy routes new issuance through the
  cross-signed **Root YE** (ECDSA) and **Root YR** (RSA) roots, which chain up
  to ISRG Root X2 / X1. Pinning X1 + X2 is sufficient because the server
  presents those cross-signed intermediates; the device builds
  `leaf ← YR1 ← Root YR ← ISRG Root X1` (RSA) or
  `leaf ← YE1 ← Root YE ← ISRG Root X2` (ECDSA). Track this in Q5, because
  Root YE/YR are not yet in general trust stores.
- Load with `WiFiClientSecure::setCACert(ca_pem)` (a `const char[]` in a new
  `firmware/src/certs/isrg_roots.pem`). Alternative: `setCACertBundle()` uses
  IDF's `x509_crt_bundle`; the pinned PEM is preferred because it is explicit,
  smaller, and immune to bundle churn.
- **Never** `setInsecure()`, never skip common-name verification. The hostname
  in the URL must match the certificate.
- Pinning roots rather than the leaf or an intermediate means Caddy's automatic
  90-day Let's Encrypt renewal *and* intermediate rotation (YE1/YR1 are valid
  until 2028-09-02) are transparent to the device.

### 8.2 SNTP is mandatory

The ESP32 has no RTC battery; certificate validity checks and
`released_at` sanity need a correct clock (`design/backend-wifi/ROADMAP.md` §7,
"SNTP is mandatory before TLS").

- `configTime(0, 0, "pool.ntp.org", "time.cloudflare.com")` — primary and
  fallback. (Public NTP infrastructure; not the owner's VM.)
- Wait until `time(nullptr)` is after a sane floor (e.g. `1_700_000_000`,
  ≈ 2023-11) with a 10 s timeout, then proceed. On timeout, abort the check
  (`SNTP_FAIL`); do **not** proceed to TLS.
- Consider setting a `sntp_set_time_sync_notification_cb` callback or polling
  `sntp_get_sync_status()`.

### 8.3 Memory and stack

- The pull engine runs in a dedicated task `ota_pull_task` (D9), recommended
  **12 KB stack, core 0**. `ota_tick()` on the Arduino loop task only advances
  state and drives the UI indicator, so the loop stack is not raised globally.
  Rationale: the existing module deliberately keeps WiFi/ArduinoOTA work on the
  loop task because "its stack can absorb a join and an mDNS start"
  (`ota.cpp` lines 5-8); a TLS handshake plus mbedtls plus HTTP parsing needs
  more than that, and the pull path should not inflate the loop task for every
  boot.
- TLS buffers: mbedtls defaults are `MBEDTLS_SSL_IN_CONTENT_LEN` /
  `OUT_CONTENT_LEN` = 16 KB each (internal RAM). `WiFiClientSecure::setBufferSizes(rx, tx)`
  can shrink them if heap is tight; do not go below what the certificate chain
  needs. `-DBOARD_HAS_PSRAM` is set (`platformio.ini` line 22), but mbedtls
  allocations are internal by default — measure before trimming.
- No 6 MB RAM buffer: the image streams into flash (`app0`/`app1` are 6 MB each,
  `firmware/partitions/waveshare_amoled_216.csv` lines 4-5). Peak RAM is the TLS
  session + a 4 KB read buffer.
- The exact stack/heap budget must be measured on hardware (open question Q6).

### 8.4 Certificate rotation and expiry

- Let's Encrypt leaf certificates are short-lived; Caddy renews automatically.
  Pinning the ISRG **roots** avoids a firmware update on every renewal.
- Root expiry (as of the current Let's Encrypt chain-of-trust page, July 2026):
  ISRG Root X1 is trusted until 2030-06-04 (its self-signed `notAfter` is
  2035-06-04); ISRG Root X2 until 2035-09-04. Track these in Q5.
- The newer Root YE / Root YR generation (generated 2025-09-03) is not yet in
  general trust stores. When Let's Encrypt retires X1/X2, the device must
  already carry the new roots, or it must trust the cross-sign path through the
  pinned X1/X2. If Let's Encrypt ever serves a chain that does not terminate at
  a pinned root, the new root must be shipped in a firmware release *before* the
  VM switches chains. Keep both old and new roots in the bundle across the
  transition.
- This is a chicken-and-egg concern: the device must be able to fetch the update
  that adds a new root using the old root. Mitigations: (a) keep the previous
  root for one release cycle; (b) have Caddy serve the old chain until every
  device has updated (tracked operationally); (c) treat the root bundle as part
  of the release runbook, not an afterthought.

## 9. BLE control additions

All new commands arrive on CTRL `…0005`
(`4c41555a-4465-7669-6365-000000000005`), which is already `WRITE | WRITE_NR |
WRITE_ENC` and owner-checked (`ble.cpp` lines 433-438, 338-357). Every command
replies on TX `…0003` (`ble_notify_status`, `ble.cpp` lines 531-536), preserving
the "one notification per command" rule (`design/ota-hybrid/DESIGN.md` §2).

### 9.1 Commands

| JSON (host → device) | Effect | TX notification(s) (device → host) |
|---|---|---|
| `{"cmd":"update"}` | Check now and apply if available | `checking` → (`available` → progress → `rebooting`) or `up_to_date` / error |
| `{"cmd":"update","mode":"check"}` | Check only; do not download | `checking` → `available`/`up_to_date` / error |
| `{"cmd":"update","force":true,"to":"X.Y.Z"}` | Owner-only: apply even if not strictly newer; `to` must equal the manifest version | same as `update`, then apply |

### 9.2 Status notifications (frozen shapes)

Success/progress (one line per transition; `pct` only while downloading):

```json
{"ok":true,"cmd":"update","state":"checking"}
{"ok":true,"cmd":"update","state":"up_to_date","version":"0.1.0"}
{"ok":true,"cmd":"update","state":"available","version":"0.2.0","size":1287600}
{"ok":true,"cmd":"update","state":"downloading","pct":42}
{"ok":true,"cmd":"update","state":"verifying"}
{"ok":true,"cmd":"update","state":"rebooting","version":"0.2.0"}
```

Errors reuse the frozen `{"ok":false,"err":"…"}` shape
(`design/ota-hybrid/DESIGN.md` §2). New `err` values:

| `err` | Meaning |
|---|---|
| `no_wifi` | No stored credentials (same value as the hybrid path). |
| `busy` | Hybrid OTA or another pull is in progress. |
| `battery_low` | Auto gate refused the update (BLE check may still report `available`). |
| `timeout` | A state timed out after its retries. |
| `tls_fail` | Certificate/handshake failure. |
| `http_404` | Manifest or binary missing. |
| `board_mismatch` | Manifest `board` ≠ this device. |
| `bad_schema` / `bad_version` / `bad_url` | Manifest failed validation. |
| `too_old` | Running version < `min_from`. |
| `target_mismatch` | `force.to` ≠ manifest version. |
| `hash_mismatch` | SHA-256 of the downloaded binary ≠ manifest. |
| `bad_image` | `esp_ota_end()` rejected the image. |
| `no_update` | `force` requested a version that is not in the manifest / no manifest. |

### 9.3 Ownership

- `{"cmd":"update","mode":"check"}` is allowed on any bonded+encrypted owner
  link; it is read-only and reveals only the public version.
- `{"cmd":"update"}` and the `force` form require the owner link (already the
  only link the CTRL handler accepts). `force` is the only downgrade path and
  additionally requires `to` to match, so it cannot be triggered by a bare
  retry loop.

### 9.4 Backwards compatibility

- The existing commands (`info`, `wifi`, `wifi_clear`, `ota`, `reboot`) and
  their reply shapes are unchanged (`ota.cpp` lines 264-321).
- `daemon/ota_flash.py` writes only those commands and reads only the frozen
  fields, so it is unaffected (its frames and guards: lines 92-160, 537-624).
- Unknown commands still answer `{"ok":false,"err":"unknown_cmd"}`.

## 10. Power, battery and scheduling

### 10.1 Cadence

| Trigger | Condition | Action |
|---|---|---|
| Boot | ≥ 60 s after boot (after `HEALTHY_MS`, so the boot is confirmed first) and creds stored | check |
| Timer | Every 24 h (86 400 000 ms) since the last completed check | check |
| BLE | `{"cmd":"update"}` | check (+apply) now |
| Screen tap / button | — | **not** an OTA trigger (unlike the usage poll, `ROADMAP.md` §8); OTA is not user-facing enough to be worth a manual trigger |

`last_chk` (epoch) and `defer_until` are persisted so a reboot does not restart
the clock or hammer a failing VM.

### 10.2 Battery and charging gate

- **Check** is cheap: WiFi on for a few seconds (`ROADMAP.md` §8 says 2-3 s for
  an HTTPS GET; the manifest is a few hundred bytes). Allowed on battery down to
  a threshold; recommended **20 %** (`power_hal_battery_pct()`,
  `power_hal.h` line 14). Below it, skip and defer.
- **Apply** (download + flash) is power-heavy. Recommended: only while charging
  (`power_hal_is_charging()`, line 15) or battery ≥ **50 %**; `mandatory:true`
  or an owner BLE `force` lowers the floor to the 20 % check threshold. These
  numbers are recommendations — Q3.
- If the battery drops below the floor mid-download, abort safely
  (`esp_ota_abort()`), never activate, turn the radio off, and defer.
- `power_hal_is_vbus_in()` (line 16) distinguishes "USB present" from
  "charging"; treat USB-present as charging for the gate.

### 10.3 Radio on only during the check

- The pull task owns `WIFI_STA` only between `WIFI_JOIN` and the terminal state,
  then calls the shared `ota_wifi_release()` → `WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF)` (same as `ota.cpp` lines 252-260). A check that finds no
  update keeps the radio on for a few seconds total.

### 10.4 The 90 s BLE freshness window is irrelevant

The firmware's `DATA_FRESH_MS` (90 s) staleness rule applies to the **usage
payload** path (`daemon/config.example` lines 20-23; `ROADMAP.md` §8 replaces it
with a poll-aligned window). OTA does not touch the usage display, so the OTA
schedule and the freshness window are independent. A pull check does not
`ota_confirm()` unless it activates an image, and does not affect "stale" state.

### 10.5 Idle / sleep interaction

- OTA is a background task; it does not call `idle_note_activity()` and does not
  wake the panel. `idle_is_asleep()` (`idle.h` line 22) does not gate OTA.
- The panel may be dark throughout an update. The UI indicator (§12) appears if
  the panel is awake; if it is asleep, the update proceeds silently and the
  reboot shows the new version.
- The pull task must not be starved by the loop: it runs on core 0 with its own
  stack (D9), so the LVGL/idle loop keeps running.

## 11. Failure modes and fallback

| Failure | Behaviour | Device state |
|---|---|---|
| No WiFi creds | Do nothing (no check) | unchanged |
| WiFi join fails | Retry/backoff, radio off, defer | unchanged |
| SNTP fails | Abort before TLS | unchanged |
| Manifest 404 / VM down | `no_update` / `http_404`, retry per §7.3 | unchanged |
| Manifest malformed / wrong board | Reject, never download | unchanged |
| Version not newer | `no_update` | unchanged |
| Download interrupted | `esp_ota_abort()`, inactive slot left dirty, retry | running slot untouched |
| SHA-256 mismatch | `esp_ota_abort()`; **never** activate | running slot untouched |
| `esp_ota_end()` invalid image | `bad_image`; never activate | running slot untouched |
| Activation fails | `activate_fail`; reboot to running slot | unchanged |
| New image crash-loops | existing `boot_tries` > 3 → rollback to last-known-good | back on old version |
| Persistent failure | give up until next 24 h; USB reflash always available | unchanged |

Anti-brick invariants:

1. **Never write the running slot** — target is
   `esp_ota_get_next_update_partition(NULL)`, asserted `!= running`
   (`ota.cpp` lines 111-113).
2. **Abort on hash mismatch** before activation (§7.1).
3. **Keep the last-known-good** — the running slot is only replaced by
   `otadata` after a verified image is written; the app-level rollback returns to
   it after 3 unconfirmed boots (`ota.cpp` lines 110-128).
4. **USB reflash is the final fallback** (`design/ota-hybrid/DESIGN.md` §4;
   `README.md` lines 373-374). The pull path never removes it.

## 12. UI

No new screen (consistent with `design/ota-hybrid/DESIGN.md` §5, lines 128-130).
The pull engine borrows the existing animated status line, which already shows a
transient "OTA…" badge when `ota_is_active()` (`ui.cpp` lines 845-855).

- Add a transport-free UI hook in `ui.h`, e.g.
  `void ui_ota_status(const char* text, int pct);` — a transient label that
  reuses the same status area/fonts and auto-clears after a few seconds.
- States shown: `Checking…` (brief), `Update 0.2.0` when available,
  `Updating… 42%` while downloading, `Verifying…`, `Restarting…`.
- Error: a short `Update failed` line for ~3 s (optionally with the `err`
  code on the serial log only, not on screen). The screen never blocks the
  update; the panel may be asleep.
- The native sim must remain safe: `ui_ota_status` is a no-op (or draws
  normally) behind `BOARD_SIM`, and `ota_pull.cpp` is excluded from the sim like
  `ota.cpp` (`platformio.ini` lines 84-87) with no-op stubs added to
  `boards/sim/ota_sim.cpp`.

## 13. Security model and threat analysis

### 13.1 What TLS + SHA-256 protects

- **Passive network eavesdropper:** sees only encrypted bytes. Firmware is
  public anyway, but the manifest and binary are not modifiable in transit.
- **Active MITM / DNS spoofing:** cannot present a certificate chaining to the
  pinned ISRG roots, so cannot substitute a manifest or binary. Certificate
  hostname verification is enforced.
- **Corrupted or partial download:** SHA-256 mismatch aborts before activation.
- **Replay of an old manifest (CDN, VM rollback, captured response):** the
  strictly-greater version rule refuses to downgrade (§5.4).
- **Wrong-board flash:** `board` must match `board_caps().id`; a foreign image is
  never activated.
- **Unbootable image:** `esp_ota_end()` image validation plus the existing
  `boot_tries` rollback keep the last-known-good bootable.

### 13.2 What it does **not** protect (residual threat)

- **A compromised VM or compromised publish pipeline.** Whoever can write the
  firmware directory and obtain a valid TLS certificate (i.e. controls the VM or
  its DNS/ACME account) can serve a malicious manifest and binary whose SHA-256
  matches. TLS+SHA-256 proves *transport* integrity, not *author* authenticity.
  This is the central residual risk of D3.
- **A compromised CA or the owner's TLS private key.** Same outcome.
- **A malicious firmware author.** An insider with publish access.
- **Offline device.** A device with no internet never updates and keeps running
  its current (possibly vulnerable) version; `mandatory` cannot reach it.
- **Physical attacker.** No Secure Boot / flash encryption means a physical
  attacker can read/replace flash. Out of scope by D3.
- **Downgrade via `force`.** An owner who is tricked into issuing a force
  downgrade can install an older, vulnerable-but-validly-hashed image. The
  `to`-must-match rule limits accidents but not social engineering.

### 13.3 Future upgrade path (signed images)

Concrete next step, layered on top of this design without changing the transport:

1. **Sign the image.** Produce a detached Ed25519 signature over the binary
   (or sign the manifest's `sha256`), and add `"sig"` + `"sig_alg":"ed25519"` +
   `"key_id"` to the manifest.
2. **Pin the public key** in firmware (separate from the TLS roots, so a
   compromised VM/TLS cannot forge an image).
3. **Verify before activation** in `VERIFY_SHA256`: the signature is checked
   against the pinned key; only then does `FLASH_INACTIVE_SLOT` run.
4. **Optionally enable Secure Boot v2 + flash encryption** so the device only
   boots signed images, closing the physical-attacker and boot-chain gaps. This
   is an irreversible eFuse step and a separate project.
5. The hybrid `espota` path would need the same signing story if it is kept as a
   production path (today it is the trusted manual path).

Until then, the security of an update is only as good as the security of the
VM, its ACME account, and the owner's SSH keys.

## 14. NVS and build-time symbols

### 14.1 NVS namespace `otah` (existing, `ota.cpp` lines 30-33)

| Key | Type | Status | Meaning |
|---|---|---|---|
| `ssid` | string | existing | WiFi SSID (shared with hybrid). |
| `pass` | string | existing | WiFi password (shared with hybrid). |
| `boot_tries` | u8 | existing | Unconfirmed-boot counter (rollback). |
| `auto` | u8 | new | Auto-pull enabled (default 1). |
| `last_chk` | u32 | new | Epoch of the last completed check. |
| `defer` | u32 | new | Epoch before which no auto-check runs (backoff). |
| `chk_fail` | u8 | new | Consecutive failed checks. |
| `pend_ver` | string ≤16 | new | Version activated but not yet confirmed; cleared by `ota_confirm()`. |
| `etag` | string ≤40 | new (optional) | Last manifest ETag for `If-None-Match`. |

Adding keys is additive; existing devices keep working. `ota_wifi_clear()`
(`ota.cpp` lines 85-94) must not delete the new scheduling keys, or it resets
the cadence; recommend it clears only `ssid`/`pass` (unchanged behaviour).

### 14.2 Build-time macros

| Macro | Status | Meaning |
|---|---|---|
| `FW_VERSION` | existing (`platformio.ini` line 20) | semver, now generated (§5.2). |
| `FW_GIT_SHA` | new | 7-hex commit, exposed in `info`. |
| `FW_BUILD_DATE` | new | ISO-8601 UTC, exposed in `info`. |
| `OTA_PULL_MANIFEST_URL` | new (optional) | Compile-time base URL `https://<VM_HOST>/firmware`; if unset, use a build default. Keep the host out of source by injecting it per build (local `platformio_override.ini` / CI secret). |

The VM hostname is **not** hard-coded in the repository; it is injected as a
build macro (`OTA_PULL_MANIFEST_URL`) so no real hostname or credential is
committed.

## 15. Out of scope

- Signing / Secure Boot / flash encryption (§13.3, phase P4).
- A dynamic OTA backend, auth tokens, or per-device manifests.
- Delta/patch updates; full-image OTA only.
- A/B metadata beyond `otadata`; no version pinning server-side.
- Updating anything other than the app image (no SPIFFS/asset OTA).

## 16. Open questions

| # | Question | Recommendation / note |
|---|---|---|
| Q1 | Is the VM's public IP reserved and is the OTA hostname decided? | Required before P0; `ROADMAP.md` §12 already asks this. |
| Q2 | Single-board flat manifest vs per-board directories? | Recommend per-board canonical + flat alias (D6); revisit if a second board ships. |
| Q3 | Exact battery floors: check at 20 %, apply only when charging or ≥ 50 %? | Recommended values above; measure real draw on hardware. |
| Q4 | Retry schedule and 24 h cadence: is 24 h right, or 12 h for security fixes? | `mandatory` could shorten the interval; keep 24 h in v1. |
| Q5 | Who tracks ISRG root expiry / rotation (X1 trusted to 2030-06-04, X2 to 2035-09-04, new Root YE/YR not yet widely trusted), and when is the new root shipped? | Operational checklist; add to the release runbook before the VM chain changes. |
| Q6 | Measured peak internal RAM and stack for TLS + flash? | Verify on hardware before trimming buffers (D9). |
| Q7 | Should `{"cmd":"update","mode":"check"}` be allowed before pairing (non-owner)? | Recommend owner-only for now; it is already owner-gated. |
| Q8 | Does the publish pipeline run in GitHub Actions or manually on the VM? | P2 `IMPL.md` sketches CI; the owner may prefer a manual signed publish. |
| Q9 | Should `mandatory` auto-apply without the charging gate? | Recommend no: `mandatory` bypasses only the battery/priority gates, never safety. |
| Q10 | Keep the flat `/firmware/manifest.json` writable by the same script? | Recommend yes, as an alias generated atomically with the canonical one. |
