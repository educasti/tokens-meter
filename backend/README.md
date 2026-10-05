# tokens-meter backend (phases 1-2)

A tiny Python 3 **stdlib-only** service that stores the latest Claude usage
reading and hands it to the device. The Mac publishes, the device pulls. Only
numbers are stored — never a Claude/OpenCode credential. Phase 2 adds runtime
pairing: the device shows a short code, the owner approves it, and the device
fetches its token once (so no secret is baked into the firmware).

```
Mac (collector)  --POST /api/usage-->  backend (SQLite)  <--GET /api/usage--  device
                 user API key                              device token (paired)
```

Contracts: `design/backend-wifi/PHASE1-CONTRACT.md` and
`PHASE2-CONTRACT.md` (sections 3-5).

## API

All errors are `{"ok":false,"err":"<code>"}`.

| Request | Auth | Result |
|---|---|---|
| `POST /api/usage` body `{"s","sr","w","wr","st"[,"t"]}` | `Bearer <user key>` | `200 {"ok":true,"updated_at":N}` · `401 unauthorized` · `400 bad_json` |
| `GET /api/usage` | `Bearer <device token>` | `200 {"ok":true,"s","sr","w","wr","st","updated_at"}` · `401 unauthorized` · `404 no_data` |
| `GET /api/health` | none | `200 {"ok":true,"version":"1"}` |
| `POST /api/pair/start` body `{"code":"ABCD2345","board":"..."}` | none (rate-limited) | `200 {"ok":true,"state":"pending","expires_in":N}` · `400 bad_code` · `400 bad_json` · `429 rate_limited` |
| `GET /api/pair/status?code=ABCD2345` | none (rate-limited) | `200 {"ok":true,"state":"pending"}` · `200 {"ok":true,"state":"paired","token":"<base64url 32B>"}` (once) · `200 {"ok":true,"state":"claimed"}` · `404 unknown_code` · `410 expired` · `400 bad_code` · `429 rate_limited` |

## Pairing (phase 2)

A code is **8 chars** from the unambiguous alphabet
`23456789ABCDEFGHJKMNPQRSTUVWXYZ` (no `0/O/1/I/L`) and expires **15 min** after
`pair/start` (re-starting the same code refreshes its clock, idempotently).
Both pairing endpoints are **unauthenticated** but rate-limited per client IP
(default 30 requests/minute, shared across the two paths; `X-Forwarded-For` is
honoured because Caddy sits in front).

The device polls `GET /api/pair/status`; once the owner runs `manage.py pair`,
the next poll returns the token **exactly once** and later polls say `claimed`.
The token is never printed by the CLI and is only stored as a SHA-256 hash in
`device`; the `pairing` row holds the plaintext token solely between approval
and that first fetch, and wipes it the moment it is claimed.

`s`/`w` are percent (numbers), `sr`/`wr` integer minutes to reset (`-1` =
unknown), `st` a non-empty status string (≤ 32 chars). `t` is validated if
present but `updated_at` is always the server's clock. Only the latest reading
is kept. Unknown paths answer `404 not_found`, wrong methods `405
method_not_allowed`. Bodies over 4 KiB are rejected with `400 bad_json`.

## Setup

```bash
cd backend
cp config.example.json config.json      # {"bind": "127.0.0.1:8080", "db": "data/backend.db"}
python3 -c 'import secrets; print(secrets.token_urlsafe(32))'   # generate a user key
python3 manage.py set-user-key          # paste it on stdin / hidden prompt
python3 manage.py mint-device --label desk   # prints the device token ONCE
python3 backend.py --config config.json
```

A relative `db` path resolves against the config file's directory, not the
cwd. The database is created with mode `0600`. Every `manage.py` command takes
`--config PATH` or `--db PATH` before the subcommand.

### Admin CLI (`manage.py`)

| Command | Effect |
|---|---|
| `set-user-key` | Set/replace the user API key. Read from stdin (hidden prompt on a tty), never argv; ≥ 32 chars. Stored hashed. |
| `mint-device --label TEXT` | New device token (32 random bytes, base64url). Token goes to **stdout once**, the id to stderr; only its SHA-256 is stored. |
| `pair --code CODE [--label TEXT]` | Approve a pending code: mint a device token bound to it and mark it `paired`. Prints the device id + label, **never the token** (the device fetches it once from `pair/status`). Fails on an unknown/expired/already-approved code. |
| `revoke-device ID` | `GET /api/usage` with that token returns 401 from then on. |
| `list-devices` | id / label / created / active-or-revoked. No tokens. |
| `list-pending` | Pending pairing codes with board, age and `pending`/`expired`. No tokens. |

Secrets are SHA-256 hashed at rest (both are 256-bit random, so no salt is
needed). The request log is one line per request (`METHOD /path STATUS`); the
`Authorization` header, tokens and query strings are never logged.

## curl round-trip

```bash
# server running on 127.0.0.1:8080 (or https://<vm-ip> behind Caddy, add -k / --cacert)
KEY=...      # the user key you set
TOKEN=...    # printed by `mint-device`

curl -s http://127.0.0.1:8080/api/health
# {"ok":true,"version":"1"}

curl -s -X POST http://127.0.0.1:8080/api/usage \
  -H "Authorization: Bearer $KEY" -H 'Content-Type: application/json' \
  -d '{"s":45.0,"sr":120,"w":28.0,"wr":7200,"st":"allowed","t":1791000000}'
# {"ok":true,"updated_at":1791214142}

curl -s http://127.0.0.1:8080/api/usage -H "Authorization: Bearer $TOKEN"
# {"ok":true,"s":45.0,"sr":120,"w":28.0,"wr":7200,"st":"allowed","updated_at":1791214142}

curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8080/api/usage -H "Authorization: Bearer wrong"
# 401
```

## Pairing round-trip (phase 2)

```bash
# device side: open a code, then poll it
curl -s -X POST http://127.0.0.1:8080/api/pair/start \
  -H 'Content-Type: application/json' \
  -d '{"code":"ABCD2345","board":"waveshare_amoled_216"}'
# {"ok":true,"state":"pending","expires_in":900}
curl -s 'http://127.0.0.1:8080/api/pair/status?code=ABCD2345'
# {"ok":true,"state":"pending"}

# owner side: approve it (prints the device id, never the token)
python3 manage.py pair --code ABCD2345 --label desk
python3 manage.py list-pending          # codes still waiting, with age

# device side: fetch the token once, then it is claimed
curl -s 'http://127.0.0.1:8080/api/pair/status?code=ABCD2345'
# {"ok":true,"state":"paired","token":"..."}
curl -s 'http://127.0.0.1:8080/api/pair/status?code=ABCD2345'
# {"ok":true,"state":"claimed"}
```

A fully scripted version of this (real server + real `manage.py` subprocess)
lives in `backend/tests/roundtrip_phase2.py`.

## Deploy on the VM

1. Copy `backend/` to `/opt/tokens-meter/backend` (a dedicated `tokens-meter`
   user owns `data/`), create `config.json`, set the user key and mint a
   device token as above.
2. `deploy/backend.service` is a systemd unit template (hardened, loopback
   only): copy to `/etc/systemd/system/tokens-meter-backend.service`, then
   `systemctl enable --now tokens-meter-backend`.
3. `deploy/Caddyfile.fragment` goes inside the existing site block **before**
   the catch-all `handle {}`. Caddy keeps terminating TLS with the
   self-signed certificate already pinned in the firmware; the backend itself
   speaks plain HTTP on loopback and must not be exposed on a public address.

## Tests

```bash
python3 -m pytest backend/tests -q                 # from the repo root; ephemeral port
python3 backend/tests/roundtrip_phase2.py          # scripted pairing round-trip (acceptance §9.1)
```
