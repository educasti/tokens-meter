# Standalone device over WiFi — backend roadmap

Status: **planned, not implemented.** This is the forward-looking design for the
next phases. Hybrid OTA (`design/ota-hybrid/DESIGN.md`) is the only piece of this
direction that is already built.

## 1. Goal

Make the device work **anywhere with WiFi**, without depending on the nearby
Mac's Bluetooth. Today the daemon on the Mac pushes usage to the device over BLE,
which requires physical proximity.

## 2. Decisions

| # | Decision | Why |
|---|---|---|
| D1 | The **Mac stays the collector** | The Claude OAuth token, the OpenCode DB and the portfolio files only exist on the Mac. What changes is the transport, not who collects. |
| D2 | Only **usage numbers** leave the Mac (`s, sr, w, wr`) | The Claude/OpenCode credentials never leave the Mac. |
| D3 | The backend is the owner's **always-on VM** | No new hosting; reachable from any WiFi. |
| D4 | **Bearer token per device** | Simple, works over plain HTTPS, and revocation is deleting a record. mTLS rejected: cert lifecycle and TLS RAM for a single-owner setup. |
| D5 | **Server-side revocation** is the authority | Immediate and independent of the device. A best-effort remote wipe is defence in depth only. |
| D6 | WiFi provisioning via **SoftAP + captive portal** | Works on iOS and Android with no app (iOS has no Web Bluetooth). Token issued via a short pairing code. |
| D7 | **Timer polling**, radio off between fetches | A persistent connection (MQTT/WS) keeps the radio associated and drains the battery. |
| D8 | **Minimal self-hosted backend** | Small service + SQLite with hashed tokens; Caddy for TLS. |
| D9 | Two credentials: **user API key** (Mac writes) and **device token** (device reads) | Least privilege; no single all-powerful credential. |

## 3. Architecture

```
   Mac (collector)                 VM (backend)                  Device
   ───────────────                 ────────────                  ──────
   reads Claude token       POST /usage            GET /usage
   reads OpenCode DB   ───► (user API key)   ───►  (device token)  ───►  display
   reads portfolio              SQLite: latest value per user
```

- The Mac **pushes** when it has data; the VM stores the latest value.
- The device **pulls** on its own timer and shows the last value, marked stale if
  old.

## 4. Credentials and trust model

- **User API key** (Mac → VM): **write-only**. Stored in the daemon config with
  `chmod 600` (Keychain on macOS).
- **Device bearer token** (VM → device): **read-only**, unique per device, stored
  in the device's NVS, sent as `Authorization: Bearer …` over HTTPS, and **hashed
  at rest** on the VM. Never in a URL (URLs reach logs).
- The device validates the server certificate against the bundled root CA — never
  "accept any certificate".
- A stolen device leaks that device's numbers (already on its screen); a stolen
  Mac key can only write numbers.

## 5. Proposed endpoints

| Method | Path | Auth | Purpose |
|---|---|---|---|
| POST | `/usage` | user API key | Mac publishes the latest numbers |
| GET | `/usage` | device token | Device reads the latest numbers for its owner |
| GET | `/health` | none | Uptime check |
| (admin) | device tokens | user API key | Issue / revoke a device token |

## 6. Device lifecycle

1. **Pairing.** The device shows a short **pairing code**; the owner registers it
   in the backend; the backend issues the device token, which the device receives.
2. **Steady state.** The device polls `GET /usage` with its token.
3. **Loss.** Revoke the token on the VM → next request is `401`. Optional
   best-effort wipe (token + WiFi creds + BLE bonds) the next time the device
   connects. Also remove the BLE bond on the Mac.

## 7. WiFi provisioning

- **SoftAP + captive portal**, triggered automatically when no credentials are
  stored (and on demand afterwards). The device advertises `Clawdmeter-XXXX` and
  serves a page to pick the network and enter the password.
- The portal also relays the pairing code.
- iOS does not reliably auto-open the portal; the device screen must show the
  manual `http://192.168.4.1` fallback.
- **SNTP is mandatory before TLS**: the ESP32 needs a correct clock to validate
  the server certificate.

## 8. Power

- WiFi on for ~2–3 s per cycle (associate + one HTTPS GET), then
  `esp_wifi_stop()` / `WIFI_OFF` for the rest of the interval.
- Default interval ~5 min on battery; ~60 s while charging (AXP2101 tells us).
- A screen tap or a button press triggers an immediate fetch.
- The 90 s freshness window from the BLE era is replaced by one aligned to the
  poll interval (2–3× the period).

## 9. Fallback

- No WiFi or backend down → show the **last value + `stale`**; never a blank
  screen.
- **Provisioning and OTA do not depend on the VM**: provisioning is local SoftAP,
  OTA is LAN WiFi + BLE trigger.

## 10. Operational notes (Oracle Cloud)

- **Reserve a static public IP.** Oracle's ephemeral public IPs change across
  stop/start, which breaks both the device and TLS.
- Open **443 in both layers**: the VPC Security List **and** the image's
  `iptables` (Oracle images block by default).
- **Hostname + Let's Encrypt** (Caddy does this automatically). Let's Encrypt
  will not issue conveniently for a bare IP — use a domain or DuckDNS.
- **Always Free idle reclaim.** Oracle can reclaim idle Always Free instances;
  know whether the policy applies to this instance.
- **Back up the SQLite file** nightly: it holds the `token → device` map, so
  losing it means re-provisioning every device. Tokens are hashed, so a DB leak
  does not leak tokens.

## 11. Phasing

1. **The pipe.** Mac `POST /usage` with the user API key → VM stores → device
   `GET /usage` with the bearer token. No pairing flow, no portal, no battery
   tuning.
2. **Lifecycle.** Pairing code, token issuance, revocation.
3. **Convenience.** SoftAP + captive portal.
4. **Battery.** Adaptive polling and real measurement.

If phase 1 is not useful, the rest is not worth building.

## 12. Open questions

- Is the VM's public IP reserved yet? Which hostname for Let's Encrypt?
- Retention: latest value only, or history?
- Do the OpenCode and portfolio screens need to work standalone too? That means
  the Mac must publish those numbers as well.
- Real battery budget at the chosen interval — measure before fixing it.

## 13. Relationship to hybrid OTA

`design/ota-hybrid/DESIGN.md` defines the BLE CTRL `{"cmd":"info"}` reply
(`board`, `fw`, `id`). It is deliberately shaped to grow into this roadmap's
device identity + token, so the identity work does not have to be redone. OTA and
provisioning stay independent of the backend.
