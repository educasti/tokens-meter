# Backend WiFi — Phase 2 "lifecycle" — frozen contract

Status: **frozen for implementation.** Scope: `design/backend-wifi/ROADMAP.md`
phase 2 (pairing code, token issuance, revocation). Builds on phase 1
(`design/backend-wifi/PHASE1-CONTRACT.md`), which is implemented and verified.

## 1. Goal

The device gets its **device token at runtime** (stored in NVS) instead of
baking it into the firmware, via a **short pairing code** the owner approves on
the backend. Revocation is server-side. After this phase the firmware carries
**no secret** and can be published/OTA'd normally.

## 2. Pairing flow

```
Device (no token)                Backend                      Owner (VM)
generates code, shows it   ──►  POST /api/pair/start {code,board}
polls GET /api/pair/status?code=…  (pending)   ◄──  manage.py pair --code CODE --label X
                                  (mints token, marks code paired)
receives token once        ◄──  {state:"paired","token":…}
stores in NVS, uses it for GET /api/usage
```

- **Code**: 8 chars from the unambiguous alphabet
  `23456789ABCDEFGHJKMNPQRSTUVWXYZ` (no 0/O/1/I/L). Shown on the device screen
  and in the captive portal page.
- **TTL**: a code expires 15 min after `pair/start`.
- The token is returned **once**; a later status poll returns
  `{state:"claimed"}`.
- Only the owner's `manage.py pair` can approve a code (manual), so an attacker
  cannot self-approve.

## 3. HTTP API additions (version 1, unauthenticated)

### 3.1 `POST /api/pair/start`
Body `{"code":"ABCD2345","board":"waveshare_amoled_216"}`.
- `200 {"ok":true,"state":"pending","expires_in":<s>}`
- Re-`start` with the same code refreshes `created_at` (idempotent).
- `400 bad_json` / `400 bad_code` (not 8 chars from the alphabet).

### 3.2 `GET /api/pair/status?code=ABCD2345`
- `200 {"ok":true,"state":"pending"}`
- `200 {"ok":true,"state":"paired","token":"<base64url 32B>"}` — **once only**
- `200 {"ok":true,"state":"claimed"}` — the token was already handed out
- `404 {"ok":false,"err":"unknown_code"}` — never started / expired
- `410 {"ok":false,"err":"expired"}` — code expired before approval

Rate-limit both endpoints: at most N requests per IP per minute (simple in-memory
counter; answer `429 {"ok":false,"err":"rate_limited"}`).

## 4. Admin CLI additions (`backend/manage.py`)

- `pair --code CODE [--label TEXT]` — approve a pending code: mint a device
  token bound to it and mark it `paired`. Prints the device id + label (never the
  token). Fails if the code is unknown/expired.
- Existing `revoke-device ID`, `list-devices` unchanged; `list-devices` gains a
  `PAIRING` column or a separate `list-pending` (pending codes with age).

## 5. Storage additions

```sql
CREATE TABLE pairing (
    code       TEXT PRIMARY KEY,     -- the 8-char code
    board      TEXT,
    created_at INTEGER NOT NULL,
    paired_at  INTEGER,              -- NULL until approved
    claimed_at INTEGER,              -- NULL until the token was fetched
    device_id  INTEGER REFERENCES device(id)
);
```

## 6. Firmware

- New `firmware/src/usage_pair.{h,cpp}` (transport-free header):
  ```c
  void usage_pair_init(void);        // setup(): load the token from NVS
  void usage_pair_tick(void);        // loop(): pairing poll when unpaired
  bool usage_pair_has_token(void);   // true once a token is stored
  const char* usage_pair_code(void); // current code, or "" when paired
  const char* usage_pair_state(void);// "idle"|"pairing"|"paired"|"error"
  void usage_pair_start(void);       // (re)generate a code and begin polling
  void usage_pair_clear(void);       // wipe the token, re-enter pairing
  ```
- NVS namespace `otah` (shared): key `dev_token` (string).
- `USAGE_BACKEND_BASE` build macro (e.g. `https://<vm-ip>/api`); the token is
  **not** a build macro anymore. `usage_pull` reads `usage_pair_has_token()` and
  the token from NVS; if unpaired it skips the pull and the pairing poll runs
  instead. The URL stays a build macro (not secret).
- Radio: reuse `OTA_WIFI_PULL` + the pinned cert, exactly like `usage_pull`
  (mutual exclusion with OTA; never concurrent).
- On `GET /api/usage` → `401`: call `usage_pair_clear()` (re-pair). One log line.
- `main.cpp`: `usage_pair_init()` in setup, `usage_pair_tick()` in loop.
- Sim: no-op stubs in `boards/sim/ota_sim.cpp`; exclude `usage_pair.cpp` in the
  sim `build_src_filter`.

## 7. UI and portal

- **Screen**: while unpaired, show the pairing code and a short hint
  (`Pair: ABCD-2345` or similar) on the usage screen (or a small overlay); once
  paired, the normal usage view. Use `usage_pair_state()` / `usage_pair_code()`.
- **Portal** (`portal.cpp`): the captive-portal page shows the pairing code and
  a one-line explanation ("enter this code with the owner tool to pair").
- No new screen is required; keep it minimal and consistent with the existing UI.

## 8. Out of scope

Battery-adaptive polling (phase 4), OpenCode/portfolio over the backend, history,
remote wipe (best-effort revocation only: 401 → re-pair).

## 9. Acceptance (phase 2)

1. Backend tests green; a scripted round-trip: `pair/start` → `manage.py pair` →
   `pair/status` returns the token once → `claimed` → `GET /api/usage` works with
   it → `revoke-device` → `401`.
2. Firmware builds (`-e waveshare_amoled_216`) with `USAGE_BACKEND_BASE` set and
   **no token macro**; with no base URL it stays a no-op.
3. A device with no token shows a code, and after `manage.py pair` it stores the
   token and starts pulling (verified on hardware).
4. No secret is compiled into the firmware; the token lives only in NVS.
