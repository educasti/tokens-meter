# Backend WiFi — Phase 1 "the pipe" — frozen contract

Status: **frozen for implementation.** Scope: `design/backend-wifi/ROADMAP.md`
phase 1 only. No pairing flow, no revocation UI, no battery tuning (phases 2-4).

## 1. Goal

The Mac (collector) publishes the latest Claude usage numbers to the owner's
always-on VM; the device pulls them over HTTPS from anywhere on WiFi. Only
numbers leave the Mac — never a Claude/OpenCode credential (ROADMAP D2).

```
Mac (collector)                VM (backend)                Device
POST /api/usage  ───────────►  SQLite: latest per owner  ◄───  GET /api/usage
(user API key)                 (numbers only)                 (device token)
```

## 2. Transport and trust

- The VM already serves HTTPS with a **self-signed certificate pinned in the
  firmware** (see `design/ota-pull/DESIGN.md` §8). The backend reuses it: the
  device pins the same certificate for `/api/*`.
- The backend is a **local HTTP service on `127.0.0.1:8080`**; Caddy terminates
  TLS and reverse-proxies `/api/*` to it. No new certificate, no ACME.
- The Mac verifies the same certificate (config `backend_ca` = path to the PEM).

## 3. HTTP API (version 1)

Base: `https://<vm-ip>/api`. JSON only. All errors are
`{"ok":false,"err":"<code>"}`.

### 3.1 `POST /api/usage` — Mac publishes

- Auth: `Authorization: Bearer <user_api_key>`.
- Body:
  ```json
  {"s": 45.0, "sr": 120, "w": 28.0, "wr": 7200, "st": "allowed", "t": 1791000000}
  ```
  `s`/`w` floats (percent), `sr`/`wr` ints (minutes to reset, -1 unknown),
  `st` status string, `t` optional epoch seconds of the reading.
- `200` → `{"ok":true,"updated_at":<epoch>}`
- `401` → `{"ok":false,"err":"unauthorized"}` (missing/wrong user key)
- `400` → `{"ok":false,"err":"bad_json"}`

### 3.2 `GET /api/usage` — device pulls

- Auth: `Authorization: Bearer <device_token>`.
- `200` → `{"ok":true,"s":45.0,"sr":120,"w":28.0,"wr":7200,"st":"allowed",
  "updated_at":<epoch>}`
- `401` → `{"ok":false,"err":"unauthorized"}`
- `404` → `{"ok":false,"err":"no_data"}` (no reading stored yet)

### 3.3 `GET /api/health` — no auth

- `200` → `{"ok":true,"version":"1"}`

### 3.4 Admin (local CLI, not HTTP in phase 1)

`backend/manage.py`:
- `set-user-key` — set/replace the user API key (stored **hashed**); prints
  nothing secret.
- `mint-device --label <text>` — create a device token (32 random bytes,
  base64url); prints the token **once**; stores only its SHA-256 hash.
- `revoke-device <id>` — mark a device token revoked (GET returns 401).
- `list-devices` — ids/labels/created/revoked (no tokens).

## 4. Storage (SQLite)

`backend/data/backend.db` (path configurable). SHA-256 for token/user-key
hashes (both are 256-bit random, so a plain hash is sufficient; no salting
needed for high-entropy secrets).

```sql
CREATE TABLE owner (id INTEGER PRIMARY KEY CHECK (id=1),
                    user_key_hash TEXT NOT NULL);
CREATE TABLE device (id INTEGER PRIMARY KEY AUTOINCREMENT,
                     token_hash TEXT NOT NULL UNIQUE, label TEXT,
                     created_at INTEGER NOT NULL, revoked INTEGER NOT NULL DEFAULT 0);
CREATE TABLE usage  (owner_id INTEGER PRIMARY KEY REFERENCES owner(id),
                     s REAL, sr INTEGER, w REAL, wr INTEGER, st TEXT,
                     updated_at INTEGER NOT NULL);
```

## 5. Backend service

- **Python 3 stdlib only** (`http.server.ThreadingHTTPServer`), no third-party
  deps — the VM must run it with no pip install.
- Bind `127.0.0.1:8080` (configurable). Behind Caddy.
- Config file `backend/config.json` (path via `--config`): `{"bind":"127.0.0.1:8080",
  "db":"data/backend.db"}`. The user key lives **only** in the DB (hashed).
- Logging: one line per request (method, path, status); never log the
  Authorization header or a token.
- Files (new): `backend/backend.py` (server), `backend/manage.py` (CLI),
  `backend/config.example.json`, `backend/tests/test_backend.py` (pytest, spins
  the server on an ephemeral port), `backend/deploy/Caddyfile.fragment`,
  `backend/deploy/backend.service` (systemd unit template), `backend/README.md`.
- Caddy fragment (added before the existing catch-all `handle {}`):
  ```
  handle /api/* {
      reverse_proxy 127.0.0.1:8080
  }
  ```

## 6. Mac daemon publisher

- Config keys (in `~/.config/claude-usage-monitor/config`, see
  `daemon/config.example`): `backend_url` (e.g. `https://<vm-ip>/api/usage`),
  `backend_user_key`, optional `backend_ca` (path to the pinned PEM).
- When `backend_url` is set: after each successful Claude poll, POST the same
  numbers (`s,sr,w,wr,st`, plus `t` = now) with `Authorization: Bearer
  <backend_user_key>`. Use `httpx` (already a dependency). Verify TLS against
  `backend_ca` when given; otherwise default system CAs. Never block the BLE
  loop: send via `asyncio.to_thread`/a bounded timeout (5 s); a failure logs one
  generic line and does not affect the BLE path.
- Off by default (no `backend_url` → no network call).
- Files: `daemon/claude_usage_daemon.py` (add the publish), `daemon/config.example`
  (document the keys), `daemon/tests/test_backend_publish.py` (mock httpx).

## 7. Firmware pull

- New module `firmware/src/usage_pull.{h,cpp}`, transport-free header:
  ```c
  void usage_pull_init(void);   // setup(): read config
  void usage_pull_tick(void);   // loop(): schedule + non-blocking state
  bool usage_pull_active(void);
  ```
- Config is injected at build (untracked, like the OTA pin), never committed:
  - `USAGE_BACKEND_URL` (e.g. `https://<vm-ip>/api/usage`) — build macro.
  - `USAGE_DEVICE_TOKEN` — build macro, from an untracked file via a pre-build
    script (mirror `firmware/scripts/gen_pinned_cert.py`).
  - Both empty → the module is a **no-op** (feature off), so the existing BLE
    path is untouched.
- Poll: on boot (after ~10 s) and every `USAGE_POLL_S` (default 300 s).
- Radio: reuse the existing WiFi owner + pinned cert — acquire
  `OTA_WIFI_PULL` (from `ota_wifi.h`) for the GET, `WiFiClientSecure` +
  `HTTPClient`, `setCACert(PINNED_SERVER_PEM)`, release afterwards. Never run
  while `ota_pull`/hybrid OTA is active (the acquire returns false → skip).
- Parse the JSON into the existing `UsageData` and call `ui_update(&usage)`.
  The ack/chime path is not involved (no BLE). Reuse the numeric fields exactly
  as the BLE payload does.
- Sim: `boards/sim/ota_sim.cpp` stubs `usage_pull_*`; `platformio.ini` sim
  `build_src_filter` excludes `usage_pull.cpp`.
- Files: `firmware/src/usage_pull.{h,cpp}`, `firmware/scripts/gen_usage_config.py`,
  `firmware/src/main.cpp` (init/tick), `firmware/src/boards/sim/ota_sim.cpp`,
  `firmware/platformio.ini`.

## 8. Out of scope (later phases)

Pairing code / token issuance over the portal, revocation UI, OpenCode/portfolio
over the backend, battery-adaptive polling, history (latest value only).

## 9. Acceptance (phase 1)

1. `backend/tests` green; a manual `curl` round-trip works:
   `POST /api/usage` with the user key then `GET /api/usage` with a minted
   device token returns the same numbers.
2. `daemon/tests` green, including the publisher test.
3. Firmware builds (`-e waveshare_amoled_216` and, if SDL2 is present, `-e sim`);
   with empty macros it is a no-op; with macros set it polls and updates the
   usage screen.
4. No credential is ever logged; tokens are stored hashed.
