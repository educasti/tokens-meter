# BLE OTA design — Waveshare ESP32-S3-Touch-AMOLED-2.16

**Status:** design (not implemented).

**Companion:** [`design/single-board-216/PLAN.md`](../single-board-216/PLAN.md)
lays down the OTA-ready partition table this document assumes.

**Device:** ESP32-S3, 16 MB flash, 480×480 CO5300 AMOLED, NimBLE peripheral,
no WiFi stack. Host daemon (Python/bleak) already holds a bonded GATT
connection for the telemetry channel.

---

## 1. Why BLE OTA (not WiFi OTA)

The device is a BLE peripheral. There is no WiFi stack in the firmware, no
network credentials, and no provisioning UX. The daemon already maintains a
bonded, owner-locked connection to the custom data service, so the image can
ride a link that is already up and already authenticates the host.

### BLE OTA vs WiFi OTA

| Dimension | **BLE OTA (proposed)** | WiFi OTA |
|-----------|------------------------|----------|
| Firmware stack | None added — reuse NimBLE | WiFi + lwIP + TLS + HTTP server, tens of KB of RAM/flash and a much larger attack surface |
| Credentials / UX | Already solved: the daemon is bonded and owner-locked | Must add AP/SSID/password provisioning (BLE-assisted or captive portal) |
| Who talks to the device | The machine the user already paired | Any host on the network; needs its own auth |
| Security baseline | Link is bonded + encrypted (LE Secure Connections); ownership already enforced | Needs app-layer auth on top of the network |
| Throughput | Low: order 10s of KB/s effective | High: order MB/s |
| Time for a ~2.5–3 MB image | Minutes (single-digit worst case) | Seconds |
| Availability | Requires the daemon running and in range | Requires a network the board can join |
| Ports/interop | None (uses the existing GATT service) | New HTTP endpoint, version manifest, auth |
| Power/latency | BLE radio already active; no DHCP/association | WiFi association + DHCP; higher average current |
| Failure modes | Link drop / supervision timeout mid-transfer | Association failures, IP/routing, HTTP timeouts |

**Conclusion:** BLE OTA is the pragmatic default because the only thing that
must be added is application-level framing; the transport, bonding and
ownership already exist. WiFi OTA is a future option only if transfer time or
field-provisioning requirements justify a second radio stack.

## 2. Partition layout and sizing rationale

The single-board reorganisation replaces the stock `default_16MB.csv` with a
repo-owned, OTA-ready table.

```
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x5000,
otadata,  data, ota,     0xe000,   0x2000,
app0,     app,  ota_0,   0x10000,  0x600000,
app1,     app,  ota_1,   0x610000, 0x600000,
spiffs,   data, spiffs,  0xC10000, 0x3E0000,
coredump, data, coredump,0xFF0000, 0x10000,
```

Rationale:

- **Two 6 MiB app slots (`0x600000` each).** The firmware image is roughly
  **2.5–3 MB**, so a slot is ~2× the image. The extra headroom covers feature
  growth, debug builds, and the fact that an OTA image is written *alongside*
  the image currently running — both must fit at once. (The platform's
  `default_16MB.csv` gives a single ~6.5 MB app slot and a smaller SPIFFS;
  this layout trades SPIFFS for a second app slot.)
- **`otadata` (8 KB) drives slot selection and rollback.** With two `app`
  partitions of subtype `ota_0`/`ota_1`, the ESP-IDF bootloader reads
  `otadata` to decide which slot to boot, and the rollback state machine
  (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`) lives there too.
- **`spiffs` (~3.875 MiB)** is retained for runtime assets/settings; it is not
  used by OTA.
- **`coredump` (64 KB)** retains crash dumps across the reboot that ends an
  update, which is exactly when a bad image is most likely to be diagnosed.
- **`nvs` (20 KB)** holds the BLE owner address and settings; it is preserved
  across OTA because OTA never touches it.

Sizing check: `0x9000` (table start) → `0x1000000` (16 MB) is fully covered;
each app slot is `0x600000` = 6,291,456 bytes.

## 3. Protocol

### 3.1 Reuse and extend the existing custom GATT service

Existing characteristics (`firmware/src/ble.cpp`):

| Role | UUID | Props |
|------|------|-------|
| Service | `4c41555a-4465-7669-6365-000000000001` | — |
| **RX** (host → device, telemetry JSON) | `4c41555a-4465-7669-6365-000000000002` | WRITE / WRITE_NR |
| **TX** (device → host, ack/nack + version) | `4c41555a-4465-7669-6365-000000000003` | READ / NOTIFY |
| REQ (device → host, refresh nudge) | `4c41555a-4465-7669-6365-000000000004` | NOTIFY |

The telemetry RX path is a JSON FIFO (`RxCallbacks::onWrite` → `rx_buf` →
`ble_get_data()` → `parse_json()`); binary OTA frames must **never** enter that
FIFO. Add two characteristics to the *same* service, so ownership, bonding and
connection bookkeeping are shared:

| Role | UUID | Props | Purpose |
|------|------|-------|---------|
| OTA control/data (host → device) | `4c41555a-4465-7669-6365-000000000005` | WRITE / WRITE_NR | Opcode frames: BEGIN, CHUNK, END, ABORT, REBOOT |
| OTA status (device → host, notify) | `4c41555a-4465-7669-6365-000000000006` | READ / NOTIFY | READY, ACK, ERROR, OK + version/board-id |

Both are gated at the callback the same way `RxCallbacks` gates telemetry:
reject unless `info.isEncrypted()` and the peer matches the stored owner
(§5).

### 3.2 Hook point: `RxCallbacks::onWrite`

A new `OtaCallbacks : NimBLECharacteristicCallbacks` mirrors the existing
pattern. The existing callback (`ble.cpp`, ~line 287) does:

1. `info.isEncrypted()` gate, else drop.
2. First-encrypted-writer claims ownership; non-owner dropped.
3. `chr->getValue()` → memcpy into the 2-slot JSON RX FIFO.

`OtaCallbacks::onWrite` keeps steps 1–2 verbatim, then **replaces step 3**:
parse the first byte as an opcode and dispatch into the OTA state machine.
Chunk data is written straight to flash from the characteristic value buffer —
it is never copied into `rx_buf` and never parsed as JSON.

Frame format (little-endian where multi-byte):

```
Byte 0 : opcode (u8)
Bytes 1..N : opcode-specific payload
```

| Opcode | Name | Payload | Direction |
|--------|------|---------|-----------|
| `0x01` | BEGIN | ver(utf8,16) · board_id(utf8,16) · size(u32) · sha256(32) | host → dev |
| `0x02` | CHUNK | seq(u32) · image bytes (≤ chunk_size) | host → dev |
| `0x03` | END | sha256(32) — final | host → dev |
| `0x04` | ABORT | — | host → dev |
| `0x10` | READY | offset(u32) (0 = fresh, >0 = resume) | dev → host (notify) |
| `0x11` | ACK | seq(u32) · received(u32) | dev → host (notify) |
| `0x12` | ERROR | code(u8) · detail | dev → host (notify) |
| `0x13` | OK | state (image written / boot set) | dev → host (notify) |

### 3.3 State machine

```
IDLE
 └─ BEGIN(version, board_id, size, sha256)
      ├─ reject (board_id != this board, size > slot, unsupported version) → IDLE
      └─ esp_ota_begin(next inactive slot, size)
           → READY(offset = 0)
READY / RECEIVING
 └─ CHUNK(seq, bytes)
      ├─ seq != expected → ACK(last good) / ERROR(BAD_SEQ)   (idempotent)
      ├─ esp_ota_write(handle, bytes, len)
      └─ ACK(seq, received)                                  (flow control)
END
 └─ esp_ota_end(handle)  → verifies sha256 (and signature, phase 2)
      ├─ mismatch → ERROR(VERIFY)  → IDLE (running slot untouched)
      └─ ok → esp_ota_set_boot_partition(inactive slot)
              → OK(boot set)
REBOOT
 └─ esp_restart()   (after OK, on host request or a short grace timer)
```

`ABORT` at any point calls `esp_ota_abort(handle)` and returns to `IDLE`.

### 3.4 MTU negotiation and chunk size

- Request the largest ATT MTU at init: `NimBLEDevice::setMTU(517)` (NimBLE
  supports up to 517). The effective MTU is whichever the central negotiates
  down to; read it back with `server->getPeerMTU(conn_handle)`.
- Maximum ATT write payload is `MTU − 3` (1-byte opcode + 2-byte ATT handle,
  i.e. `MTU - 3` bytes of frame including the opcode).
- **Default chunk size = 244 bytes.** With a 251-byte Link-Layer PDU (Data
  Length Extension) the ATT MTU is 247, leaving 244 bytes of payload; this
  avoids L2CAP fragmentation and is the safest common denominator across
  macOS/Windows/Linux controllers. `chunk_size = min(negotiated_MTU - 3,
  OTA_MAX_CHUNK)` with `OTA_MAX_CHUNK = 512` (matches `BLE_BUF_SIZE`).
- Transfers use **WRITE_NR** (write without response) for throughput; flow
  control comes from the NOTIFY ack channel, not from ATT responses.

### 3.5 ASCII sequence diagram

```
  Daemon (bleak)                    Firmware (NimBLE)              Flash
        |                                  |                         |
        |-- BEGIN(ver,board,size,sha) ---->|                         |
        |                          validate board-id / version        |
        |                                  |-- esp_ota_begin(next)-> |
        |<-- NOTIFY READY(offset=0) -------|                         |
        |                                  |                         |
        |-- CHUNK(seq=0, 244 B) ---------->|-- esp_ota_write() ----> |
        |-- CHUNK(seq=1, 244 B) ---------->|-- esp_ota_write() ----> |
        |-- ... (window of K chunks) ----->|                         |
        |<-- NOTIFY ACK(seq=K-1, bytes) ---|   (flow control)        |
        |-- ... remaining chunks --------->|                         |
        |-- END(sha256) ------------------>|-- esp_ota_end() ------> |
        |                                  |   verify sha/signature  |
        |                                  |-- set_boot_partition -> |
        |<-- NOTIFY OK(boot set) ---------|                         |
        |-- REBOOT ----------------------->|  esp_restart()          |
        |                                  |                         |
        |   (reconnect after ~5 s)         |                         |
        |-- READ version (OTA status) ---->|                         |
        |<-- version == expected ----------|  mark_app_valid()       |
        |                                  |  (confirm on good boot) |
```

## 4. Firmware side

### 4.1 OTA write API

Prefer the ESP-IDF calls over the Arduino `Update` class so the target slot is
explicit and rollback is hand-controlled:

```c
const esp_partition_t* part = esp_ota_get_next_update_partition(NULL); // never the running slot
esp_ota_handle_t h;
esp_ota_begin(part, OTA_SIZE_UNKNOWN /* or known size */, &h);
esp_ota_write(h, chunk, len);
esp_ota_end(h);                     // verifies the image (and signature if enabled)
esp_ota_set_boot_partition(part);
esp_restart();
```

The Arduino `Update` path (`Update.begin(size)` / `Update.write()` /
`Update.end(true)`) is equivalent and may be used as a thin wrapper, but it
also picks the next update partition implicitly. Either way:

- **Always write the inactive slot.** `esp_ota_get_next_update_partition()`
  returns the slot that is *not* currently booted, so the running image can
  never be overwritten by construction.
- **`otadata` is flipped only after a successful `esp_ota_end()`.** A
  power cut mid-transfer leaves `otadata` pointing at the old, intact slot.

### 4.2 Boot-confirm / rollback

Enable the ESP-IDF rollback machinery:

- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` (via `sdkconfig` / build flag).
- On the first boot of a new image the bootloader marks it
  `ESP_OTA_IMG_PENDING_VERIFY`.
- The new firmware calls
  `esp_ota_mark_app_valid_cancel_rollback()` once it is demonstrably healthy —
  e.g. after `ble_init()` succeeds and the first telemetry beat or the UI is
  up, or after a N-second uptime timer.
- If the image crashes or resets before confirming, the bootloader boots the
  previous slot on the next reset. This is the anti-brick net.

### 4.3 No full-image buffering

Do **not** allocate or stage the ~2.5–3 MB image in RAM. `esp_ota_write()` is
called directly from the incoming characteristic value; the only buffers are
the NimBLE ATT buffer and, optionally, a small fixed internal-SRAM scratch
chunk. This preserves the project's internal-SRAM discipline (the existing
code deliberately avoids PSRAM for anything on hot paths and sizes LVGL
buffers for internal SRAM).

The OTA state machine must yield to the rest of the loop: `esp_ota_write()` can
block for tens of ms per chunk. Options, in order of preference:

1. Run OTA writes from the NimBLE callback/task (as `onWrite` already runs off
   the Arduino loop) and keep the loop rendering.
2. Gate telemetry parsing while `OTA_RECEIVING` so the loop is not competing
   for flash.
3. Show a dedicated OTA progress screen (`%` of `received/size`) and suspend
   the idle fade during the transfer.

## 5. Security and anti-brick

### 5.1 Link security

- The OTA characteristics **require a bonded, encrypted link**. Reject any
  write where `!info.isEncrypted()`, exactly as `RxCallbacks::onWrite` does for
  telemetry.
- Additionally require the peer to be the stored **owner** (the identity
  address persisted in NVS and checked by `onAuthenticationComplete` /
  `claim_owner`). A second machine that pairs is already un-bonded and
  dropped; OTA inherits that.
- Reject BEGIN unless the connection uses LE Secure Connections (the project
  already calls `NimBLEDevice::setSecurityAuth(true, false, true)`).

### 5.2 Image signing (optional, phase 2)

`Updater_Signing.h` is available in the Arduino core. Enable image signing so
`esp_ota_end()` refuses an image whose signature does not verify against the
embedded public key:

- Build signed images; the device verifies the signature as part of
  `esp_ota_end()`.
- Keep the signing private key off the device and off the daemon host;
  the daemon streams the already-signed `.bin`.
- Signing is defence-in-depth on top of the encrypted/bonded link, not a
  substitute for it.

### 5.3 Anti-brick rules

1. **Wrong-board protection.** BEGIN carries `board_id` (compile-time
   constant, e.g. `"WS-AMOLED-216"`) and a firmware `version`. The device
   refuses a mismatched `board_id` before touching flash. This is the single
   most important guard: it makes it impossible to flash a 1.8/C6/LCD image
   onto this board.
2. **Never overwrite the running slot.** The target is always
   `esp_ota_get_next_update_partition()`.
3. **Verify before commit.** `size` and `sha256` are checked; `esp_ota_end()`
   validates the image. `otadata` flips only after success.
4. **Rollback window.** Until `esp_ota_mark_app_valid_cancel_rollback()`, the
   previous slot remains bootable.
5. **Power guard.** Only accept BEGIN when the battery is charged enough (or
   on USB), and abort cleanly on a low-battery IRQ rather than brown out
   mid-write.
6. **Idempotent chunks.** CHUNK seq is monotonic; duplicates/retries are
   ACKed against the last good offset so a dropped link can resume without
   rewriting the whole image.

## 6. Daemon side (Python / bleak)

The daemon already discovers the system-connected peripheral by the custom
service UUID and holds an encrypted session, so the uploader is a mode on top
of the existing `Session`:

1. **Prepare.** Read the `.bin`, compute `sha256`, read the expected
   `version` / `board_id` from build metadata.
2. **Connect.** Reuse the existing bonded connection (the daemon never scans
   for a stranger; it targets the peripheral macOS/Windows/Linux already
   holds).
3. **Handshake.** `start_notify(OTA_STATUS_UUID, on_status)`, then write
   `BEGIN(version, board_id, size, sha256)`; wait for `READY(offset)`.
4. **Stream with flow control.**
   - Read the file in `chunk_size` (≤ 244) blocks.
   - Write `CHUNK(seq, bytes)` with `write_gatt_char(..., response=False)`.
   - Maintain a window of K in-flight chunks; after K, **await an `ACK`**
     (`asyncio.Event` + timeout). On timeout, resend from the last ACKed seq.
   - Guard every await with a timeout so a supervision-timeout disconnect
     surfaces as a retry, not a hang (the daemon already bounds
     `start_notify` for exactly this reason).
5. **Finish.** Write `END(sha256)`; wait for `OK`. Send `REBOOT` (or let the
   device auto-reboot after a grace timer).
6. **Verify after reboot.** Reconnect (retry with backoff for ~30 s) and read
   the version from the OTA status characteristic (READ), comparing it to the
   image just sent. Only then report success; otherwise the old version is
   still running and the transfer failed safely.
7. **Throttle.** Never start an OTA while telemetry beats are due; pause the
   60 s poll / heartbeat for the duration, and restore it after.

## 7. Phasing

| Phase | Deliverable | Risk | Notes |
|-------|-------------|------|-------|
| **0 — Identity handshake** | Read-only `version` + `board_id` characteristic; daemon reads it and refuses to flash a mismatch. | Very low | Ships independently; no flash writes. Proves the extended service and the encryption gate. |
| **1 — Chunked OTA** | BEGIN/CHUNK/END, WRITE_NR + NOTIFY ACK flow control, `esp_ota_*` into the inactive slot, progress UI. No signing, manual/auto rollback. | Medium | First end-to-end update. Test with a deliberately corrupt image to confirm `esp_ota_end()` rejection. |
| **2 — Rollback + signing** | `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` + `esp_ota_mark_app_valid_cancel_rollback()`; signed images via `Updater_Signing.h`; resume-on-reconnect; low-battery guard. | Higher | Turns OTA into a field-safe operation. |

## 8. Risks

| Risk | Impact | Mitigation |
|------|--------|------------|
| **BLE throughput** | A ~2.5–3 MB image at an effective tens of KB/s is **minutes**, possibly 5–15 min worst case; slower on busy 2.4 GHz. | WRITE_NR + large MTU + windowed acks; chunk 244 B; accept that OTA is an occasional, deliberate action, not interactive. |
| Supervision-timeout mid-transfer | Link drop restarts the transfer. | Resume from last ACKed offset instead of restarting; bounded awaits; retry with backoff. |
| MTU differences per OS | Small chunks on macOS vs Windows. | Negotiate up to 517, cap chunk at `min(MTU-3, 244)` for the common case, never exceed the ATT buffer. |
| Power loss / low battery | Bricked slot if it happened during commit — but it cannot, because `otadata` flips last. | Inactive-slot writes + rollback + a charging/battery gate on BEGIN. |
| Competing with HID + telemetry | Two-connection limit; telemetry beats interleave. | Pause telemetry during OTA; run OTA on the daemon's existing connection. |
| Wrong image | Unbootable or wrong-hardware flash. | `board_id` + `version` handshake (phase 0); hash/size check; signed images (phase 2). |
| Flash wear | Negligible: one write per slot per update. | — |

## 9. Open questions

- Resume semantics: is a partial CHUNK window resent or skipped by seq? (Leaning
  to: device stores last good seq, daemon resumes at `offset` from READY.)
- Should OTA be exposed on a separate GATT service (vs. more characteristics on
  the data service) to keep the telemetry service's discovery surface small?
- Signing key management and rotation policy for phase 2.
- OTA trigger UX: a daemon CLI subcommand vs. an opt-in flag on the tray app.
