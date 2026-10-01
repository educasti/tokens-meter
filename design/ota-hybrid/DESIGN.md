# Hybrid OTA design — Waveshare ESP32-S3-Touch-AMOLED-2.16

Status: **frozen contract for implementation.** This supersedes the earlier
BLE-only chunked-transfer draft (the transfer is now WiFi, not BLE).

## 1. Summary

OTA is **hybrid**: the BLE link stays the control path (trigger, provisioning,
status) and WiFi is brought up **only for the transfer**, then switched off. The
device keeps its "offline by default" posture and gains a fast update path
(seconds instead of BLE's minutes).

Three actors:

1. **Device firmware** — receives BLE control commands, stores WiFi credentials
   in NVS, enters an "OTA mode" that runs `ArduinoOTA`, reboots into the new
   slot when done.
2. **Host OTA helper** (`daemon/ota_flash.py`) — stops the daemon (freeing the
   BLE connection), provisions credentials + triggers OTA over BLE, uploads the
   binary over WiFi, restarts the daemon.
3. **PlatformIO build** — produces `firmware.bin`, which the helper feeds to
   `espota`.

The partition layout (dual 6 MB app slots + `otadata`) already supports this and
does not change.

## 2. BLE control protocol (FROZEN)

Same custom service, two new endpoints. UUIDs:

| Char | UUID | Direction | Properties |
|---|---|---|---|
| Service | `4c41555a-4465-7669-6365-000000000001` | — | — |
| RX (usage payload) | `…0002` | host → device | WRITE / WRITE_NR |
| TX (ack/status notify) | `…0003` | device → host | READ / NOTIFY |
| REQ (refresh request) | `…0004` | device → host | NOTIFY |
| **CTRL (OTA control)** | `…0005` | host → device | WRITE, **requires bonding/encryption** |

`CTRL` writes carry UTF-8 JSON, same shape as the RX payload. Every command
produces exactly one notification on **TX (`…0003`)**, so the host has a single
place to wait for results. Unknown `cmd` → `{"err":"unknown_cmd"}`.

### Commands (host → device, on CTRL `…0005`)

| JSON | Effect | TX notification |
|---|---|---|
| `{"cmd":"info"}` | Report identity | `{"ok":true,"board":"waveshare_amoled_216","fw":"<ver>","id":"<mac>"}` |
| `{"cmd":"wifi","ssid":"S","pass":"P"}` | Store credentials in NVS | `{"ok":true,"cmd":"wifi"}` |
| `{"cmd":"wifi_clear"}` | Erase stored credentials | `{"ok":true,"cmd":"wifi_clear"}` |
| `{"cmd":"ota","mode":"on"}` | Connect WiFi + start `ArduinoOTA` | `{"ok":true,"cmd":"ota","state":"ready","ip":"<ip>","port":3232}` or `{"ok":false,"err":"<reason>"}` |
| `{"cmd":"ota","mode":"off"}` | Stop `ArduinoOTA`, WiFi down | `{"ok":true,"cmd":"ota","state":"off"}` |
| `{"cmd":"reboot"}` | Reboot | (link drops) |

Rules:

- `wifi` / `wifi_clear` / `ota` / `reboot` only accepted on a **bonded and
  encrypted** link (same ownership check as the RX payload). Otherwise
  `{"ok":false,"err":"not_owner"}`.
- `{"cmd":"ota","mode":"on"}` with no stored credentials →
  `{"ok":false,"err":"no_wifi"}`.
- While `ota` mode is on, the usage RX path keeps working; OTA mode is additive.
- Firmware identifies as `board = "waveshare_amoled_216"`. The helper MUST abort
  if it does not match the artifact it is about to flash.

Backwards compatibility: the existing daemon that never writes `…0005` and never
subscribes `…0003` is unaffected.

## 3. WiFi + transfer

- `ArduinoOTA` in station mode, hostname `clawdmeter-<last6ofmac>`, port 3232.
- Optional shared password, passed in the `ota` command:
  `{"cmd":"ota","mode":"on","pass":"<pw>"}`. When present, `ArduinoOTA` uses it.
- The helper uploads with `espota.py -i <ip> -p 3232 -f firmware.bin [-a <pw>]`.
  `espota.py` ships with PlatformIO (`~/.platformio/packages/framework-*/…` or
  `tool-esptoolpy`); the helper resolves it and falls back to
  `pio run -t upload --upload-port <ip>`.
- Windows uploads to the same `espota` path.

## 4. Safety: application-level rollback

The prebuilt Arduino bootloader in this toolchain does **not** run with
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` verified, so rollback is implemented in
the application instead of relying on the bootloader:

- On boot, firmware increments an NVS counter `boot_tries` (namespace `otah`).
- A boot is **confirmed** — counter reset to 0 — when either a valid usage
  payload is written by the owner over BLE, or 60 s of healthy uptime elapse.
- At `esptool`-style reset time: if `boot_tries > 3`, firmware calls
  `esp_ota_set_boot_partition(<the other slot>)` and reboots, returning to the
  last known-good image.

Limitations (documented, accepted): this catches crash loops, not a hang with no
reset. USB re-flash is always the final fallback.

Additional anti-brick rules:

- Never write the running slot (`esp_ota_get_running_partition()`).
- The helper verifies `board` before uploading.
- OTA runs only on an authenticated (bonded) BLE trigger.

## 5. Firmware work

New module `firmware/src/ota.cpp` / `ota.h`:

- `ota_init()` — load WiFi creds, arm the boot-verify counter.
- `ota_tick()` — called from `loop()`; drives connect/timeout/ArduinoOTA.
- `ota_handle_ctrl(const char* json)` — invoked from a new `CtrlCallbacks` in
  `ble.cpp`; parses commands, replies on TX.
- `ota_set_wifi(ssid, pass)`, `ota_start()`, `ota_stop()`, `ota_confirm()`.

`ble.cpp` changes:

- Create CTRL char `…0005` with `NIMBLE_PROPERTY::WRITE | WRITE_NR`, and require
  encryption (NimBLE `setAccessPermissions` / the existing owner check).
- New `CtrlCallbacks::onWrite` → forward to `ota_handle_ctrl()`.
- Expose `ble_notify_status(const char* json)` that writes to the TX char.

`main.cpp`:

- `ota_init()` in `setup()`, `ota_tick()` in `loop()`, and
  `ota_confirm()` where a valid payload is accepted.

`platformio.ini` (216 env): nothing new should be required — `WiFi`,
`ArduinoOTA` and `Update` ship with the framework. If enabling WiFi changes the
BLE/NimBLE coupling flags, keep peripheral roles as-is and add the coexistence
flags only if the build demands them.

UI (minimal, no new screen): show an "OTA" badge/indicator while OTA mode is
on, reusing the existing status area; the serial log prints `OTA: ...` lines for
debugging.

## 6. Daemon work

New `daemon/ota_flash.py` (macOS/Linux; Windows path documented) with a CLI:

```
python daemon/ota_flash.py --firmware firmware/.pio/build/waveshare_amoled_216/firmware.bin \
    [--ssid S --pass P] [--ota-password PW] [--keep-daemon]
```

Flow:

1. Stop the daemon service (`launchctl unload …` / `systemctl --user stop …`) so
   the BLE connection is free; remember to restart it in a `finally`.
2. `BleakClient` connect (same discovery/owner logic as the daemon).
3. Subscribe TX `…0003`; send `{"cmd":"info"}` and assert `board`.
4. If `--ssid`, send `{"cmd":"wifi",…}`.
5. Send `{"cmd":"ota","mode":"on","pass":…}`; wait for `state:"ready"`; read
   `ip`.
6. Run `espota.py` against `ip:3232`.
7. On success send `{"cmd":"ota","mode":"off"}` (best effort; the device may
   already be rebooting).
8. Restart the daemon.

Config (optional, `daemon/config.example`): `ota_ssid`, `ota_password`,
`ota_wifi_password` — but the CLI flags override.

Windows: `daemon/claude_usage_daemon_windows.py` keeps its tray app; the helper
stops it via the existing autostart/quit path (or the user quits from the tray
prompt) before flashing.

## 7. Verification

- `pio run -d firmware -e waveshare_amoled_216` and `-e sim` succeed; sim stubs
  the OTA module (no WiFi) behind `BOARD_SIM`.
- Fake-BLE unit test: `ota_handle_ctrl` parsing table (info/wifi/ota/mode/err).
- `daemon/tests` gain a protocol test for the JSON shapes.
- Hardware smoke test (needs WiFi credentials, provided by the operator):
  pair → `info` → `wifi` → `ota on` → `espota` → device reboots into the new
  slot → `info` still answers → rollback counter resets.

## 8. Out of scope (next phases)

- Public backend so the device works without a local host (decided model: the
  host uploads usage, the device downloads it; tokens never leave the host).
- Push instead of polling; battery-aware scheduling.
- The `{"cmd":"info"}` payload is deliberately shaped to grow into the future
  backend's `device-id + token`, so this phase does not have to be redone.
