# Pull-based automatic OTA: implementation plan

Design source of truth: `design/ota-pull/DESIGN.md` (frozen contract).
Baseline for the existing system: `design/ota-hybrid/DESIGN.md`,
`firmware/src/ota.cpp` / `ota.h`, `firmware/src/ble.cpp`, `firmware/src/main.cpp`,
`firmware/platformio.ini`, `firmware/partitions/waveshare_amoled_216.csv`,
`daemon/ota_flash.py`, `daemon/config.example`, `README.md` §"Firmware updates
(hybrid OTA)".

Repo rules that apply to every package (from `CLAUDE.md`):
- **No `#ifdef BOARD_*` in shared code.** Board differences come from
  `board_caps()` (`firmware/src/hal/board_caps.h`). `#ifdef BOARD_HAS_PSRAM` and
  `BOARD_SIM` are the only sanctioned compile-time gates.
- **Hardware-only modules are excluded from the sim** and stubbed instead
  (`platformio.ini` lines 84-87; `boards/sim/ota_sim.cpp`). `ota_pull.cpp` must
  follow `ota.cpp`.
- **Transport-free headers.** `ota.h` is deliberately include-able by shared code
  on every board (`ota.h` lines 7-10); `ota_pull.h` must keep the same shape.
- **Threading contract.** BLE-task callbacks only parse/enqueue; WiFi and flash
  work happens on the loop task or a dedicated task (`ota.h` lines 12-14;
  `ota.cpp` lines 5-8).
- **Match the surrounding code style**: comment density, `snake_case` functions,
  module prefixes, Arduino/ESP-IDF idioms.
- Do not commit. Do not flash hardware. Do not touch the user's installed
  daemon or LaunchAgent.

## 1. Work packages and file ownership

| WP | Owner files (create or edit ONLY these) | Depends on |
|---|---|---|
| **P0 Version + hosting + manifest** | `firmware/scripts/version.py` (new), `firmware/platformio.ini`, `design/ota-pull/deploy/ota-publish.sh`, `design/ota-pull/deploy/Caddyfile.example`, `design/ota-pull/manifest.example.json` | — |
| **P1 Firmware pull updater** | `firmware/src/ota_pull.{h,cpp}` (new), `firmware/src/ota_semver.h` (new, pure), `firmware/src/ota_manifest.{h,cpp}` (new, pure parse + constants), `firmware/src/ota_wifi.{h,cpp}` (new, hardware), `firmware/src/certs/ (pinned server cert)` (new), `firmware/src/ota.{h,cpp}`, `firmware/src/ble.cpp`, `firmware/src/main.cpp`, `firmware/src/boards/sim/ota_sim.cpp`, `firmware/src/boards/sim/shim/Preferences.h`, `firmware/platformio.ini` | P0 |
| **P2 CI publish automation** | `.github/workflows/ota-publish.yml` (new), `design/ota-pull/deploy/ota-publish.sh` (reuse), `README.md` (new section only) | P0, P1 |
| **P3 UI + telemetry** | `firmware/src/ui.{h,cpp}`, `firmware/src/ota_pull.{h,cpp}`, `firmware/src/main.cpp` | P1 |
| **P4 Optional signing** | `tools/sign_firmware.py` (new), `firmware/src/ota_manifest.{h,cpp}`, `firmware/src/ota_pull.cpp`, `firmware/src/certs/` (public key), `design/ota-pull/deploy/ota-publish.sh` | P1, P2 |

Suggested branch: `feat/ota-pull`. Each package is independently reviewable and
compiles at every step (P1 must keep `-e sim` green, which forces the stub work
to land in the same package).

## 2. Interfaces (contracts; implement exactly)

### 2.1 Pure logic — host-testable, no Arduino

`firmware/src/ota_semver.h` (header-only, `static inline`):

```c
// Parse "X.Y.Z" (optional leading 'v') into 3 non-negative ints. false on any
// non-numeric or out-of-range part. No pre-release/build metadata (DESIGN §5.1).
bool semver_parse(const char* s, int out[3]);

// -1 / 0 / 1. Parse failure of either side -> SEMVER_INVALID (negative sentinel),
// so callers must reject, never treat as equal.
int semver_cmp(const char* a, const char* b);
```

`firmware/src/ota_manifest.h` + `.cpp` (ArduinoJson only, no WiFi):

```c
#define OTA_MANIFEST_SCHEMA_VERSION 1
#define OTA_SHA256_HEX_LEN 64
#define OTA_URL_MAX 192
#define OTA_BOARD_MAX 32
#define OTA_VERSION_MAX 16

struct OtaManifest {
    int  schema_version;
    char board[OTA_BOARD_MAX];
    char version[OTA_VERSION_MAX];
    char url[OTA_URL_MAX];       // relative, validated
    char sha256[OTA_SHA256_HEX_LEN + 1];
    long size;
    char min_from[OTA_VERSION_MAX];   // "" when absent
    bool mandatory;
    char released_at[24];             // "" when absent
    char notes[OTA_URL_MAX];          // "" when absent
};

enum ota_manifest_err {
    OTA_MF_OK = 0, OTA_MF_BAD_JSON, OTA_MF_BAD_SCHEMA, OTA_MF_BAD_BOARD,
    OTA_MF_BAD_VERSION, OTA_MF_BAD_URL, OTA_MF_BAD_SHA, OTA_MF_BAD_SIZE,
};

ota_manifest_err ota_manifest_parse(const char* json, OtaManifest* out);
bool ota_url_is_relative_safe(const char* url);   // no scheme, no "..", no leading '/'
bool ota_sha256_hex_eq(const char* a, const char* b);  // length + constant-time compare
bool ota_released_at_sane(const char* iso, long now_epoch);  // reject >24h future
```

`ota_manifest_parse` uses ArduinoJson 7 (`JsonDocument`, matching
`ota.cpp` lines 266-272) but never touches WiFi, so the host test can compile
`ota_manifest.cpp` against the sim shim's Arduino/ArduinoJson or a tiny stub.

### 2.2 WiFi owner — hardware-only, shared by hybrid and pull

`firmware/src/ota_wifi.h` / `.cpp` (DESIGN §7.4):

```c
typedef enum { OTA_WIFI_NONE, OTA_WIFI_HYBRID, OTA_WIFI_PULL } ota_wifi_user_t;

// Returns false if another owner holds the radio. Idempotent for the same owner.
bool ota_wifi_acquire(ota_wifi_user_t who);
void ota_wifi_release(void);              // disconnect(true) + WiFi.mode(WIFI_OFF)
bool ota_wifi_is_held_by(ota_wifi_user_t who);
bool ota_wifi_is_held(void);
```

`ota.cpp`'s `wifi_begin()` (lines 216-222) and `ota_stop_now()` (lines 252-260)
are refactored to acquire/release through this module; no behaviour change.

### 2.3 Pull engine — hardware-only

`firmware/src/ota_pull.h` (transport-free, included by `main.cpp` / `ui.cpp`):

```c
void ota_pull_init(void);                       // setup(), after ota_init()
void ota_pull_tick(void);                       // loop(), non-blocking
void ota_pull_handle_ctrl(const char* json);    // {"cmd":"update",...} from CTRL
bool ota_pull_is_active(void);                  // UI badge
const char* ota_pull_state_name(void);          // "idle"|"join"|... for logs/UI
```

`firmware/src/ota_pull.cpp` owns the DESIGN §7 state machine and the
`ota_pull_task` (12 KB, core 0). It never runs on the NimBLE task; the CTRL
handler only enqueues a request (same one-slot handoff pattern as
`ota.cpp` lines 45-49, 155-172).

### 2.4 CTRL additions (DESIGN §9)

| Request | Handler | Replies on TX `…0003` |
|---|---|---|
| `{"cmd":"update"}` | `ota_pull_handle_ctrl` | `checking`, `up_to_date`/`available`, progress, `rebooting`, or `{"ok":false,"err":...}` |
| `{"cmd":"update","mode":"check"}` | same | `checking` → `up_to_date`/`available` / error |
| `{"cmd":"update","force":true,"to":"X.Y.Z"}` | same | as `update`, with `target_mismatch` if `to` differs |

`ota_handle_ctrl` (`ota.cpp` lines 264-321) routes `cmd == "update"` to
`ota_pull_handle_ctrl` and otherwise keeps its exact current behaviour. New
`err` values are listed in DESIGN §9.2.

### 2.5 `info` reply growth (DESIGN §5.3)

`ota.cpp`'s `info` branch (lines 277-284) gains `sha` and `build`:

```json
{"ok":true,"board":"waveshare_amoled_216","fw":"0.2.0","sha":"abcdef0",
 "build":"2026-10-02T12:00:00Z","id":"AA:BB:CC:DD:EE:FF"}
```

Keep the buffer at ≥192 bytes (`ota.cpp` line 278); the reply is ~140 bytes.

## 3. P0 — Version stamping + VM static hosting + manifest

### 3.1 Files

- **Add** `firmware/scripts/version.py`.
- **Change** `firmware/platformio.ini`: replace the hand-edited
  `-DFW_VERSION=\"0.1.0\"` (line 20) with `extra_scripts = pre:scripts/version.py`
  for `env:waveshare_amoled_216`; keep the `sim` env untouched.
- **Add/refine** `design/ota-pull/deploy/ota-publish.sh`,
  `design/ota-pull/deploy/Caddyfile.example`,
  `design/ota-pull/manifest.example.json` (deliverables in this phase).

### 3.2 Function-level task list

1. `firmware/scripts/version.py`
   - `def git(*args) -> str | None` — run git in `env["PROJECT_DIR"]`, swallow
     errors.
   - `def stamped_version() -> tuple[str, str, str]` — return
     `(version, sha, build_date)` per DESIGN §5.2; `0.0.0-dev` / `""` outside a
     git tree.
   - `def generate_manifest_defaults() -> dict` — expose the same version to the
     publish script via a `--print-version` mode (so P0 tooling and firmware
     cannot drift).
   - `Import("env")`; `env.Append(BUILD_FLAGS=[...])` with
     `-DFW_VERSION=\"...\"`, `-DFW_GIT_SHA=\"...\"`, `-DFW_BUILD_DATE=\"...\"`.
2. `ota.cpp`: make the `#ifndef FW_VERSION` fallback (lines 25-27) also define
   `FW_GIT_SHA` / `FW_BUILD_DATE` as `""`, so shared code always links.
3. `deploy/ota-publish.sh` (sketch, DESIGN §6.5): `--bin`, `--version`,
   `--board`, `--dest user@host:/var/www/firmware`, `--keep N`; computes
   `sha256sum` + `stat`, renders the manifest from `manifest.example.json`,
   `rsync` to a temp dir, `ssh mv` atomically, prunes to N.
4. `deploy/Caddyfile.example`: site block, TLS with the explicit self-signed certificate (no ACME), `file_server`,
   explicit `Content-Type` for `.bin`, `Cache-Control` per DESIGN §6.3, and a
   `handle /firmware/manifest.json` alias to the canonical per-board path.

### 3.3 Acceptance criteria

- `pio run -d firmware -e waveshare_amoled_216` on tag `v0.2.0` produces an image
  whose `info` reports `fw:"0.2.0"` with a non-empty `sha`/`build`.
- On a dirty tree or no tag, the build still succeeds with `0.0.0-dev`.
- `ota-publish.sh` on a built `.bin` writes a manifest whose `sha256`/`size`
  match `sha256sum`/`stat`, whose `version` equals `info.fw`, and publishes both
  files atomically; a re-run keeps the last N versions.
- `caddy validate` accepts `Caddyfile.example`; a local `curl -I` shows the
  expected `Content-Type` and `Cache-Control` for `.json` and `.bin`.

### 3.4 Test plan

- **Python unit test** (`daemon/tests/test_ota_publish.py`, new; pytest is the
  repo's host-test runner per `CLAUDE.md` §Tests): invoke the publish script's
  pure helpers (hash, size, manifest render) and assert the generated manifest
  round-trips through `json.loads`, `sha256` equals `hashlib.sha256(bin).hexdigest()`,
  and version formatting matches the tag.
- **Shell test** (bash, like `daemon/tests/test_bash_heartbeat.sh`): a dry-run
  publish to a temp dir exercises tmp+`mv` and `--keep N` pruning without ssh.
- **Build test**: `pio run -d firmware -e waveshare_amoled_216` and `-e sim`
  both green.

## 4. P1 — Firmware pull updater

### 4.1 Files

- **Add** `firmware/src/ota_pull.{h,cpp}`, `ota_semver.h`,
  `ota_manifest.{h,cpp}`, `ota_wifi.{h,cpp}`, `firmware/src/certs/ (pinned server cert)`.
- **Change** `firmware/src/ota.{h,cpp}` (route `update`, add `sha`/`build`, route
  WiFi through `ota_wifi`), `firmware/src/ble.cpp` (no change expected; CTRL
  already routes to `ota_handle_ctrl`), `firmware/src/main.cpp`
  (`ota_pull_init()` in `setup()` after `ota_init()`, `ota_pull_tick()` in
  `loop()`), `firmware/src/boards/sim/ota_sim.cpp` (stub new symbols),
  `firmware/src/boards/sim/shim/Preferences.h` (add `getString`/`putString`/`getUInt`/
  `putUInt`/`remove` if the pure test links pull code; keep the sim's OTA path
  stubbed), `firmware/platformio.ini` (add `-<ota_pull.cpp>` and
  `-<ota_wifi.cpp>` to the sim `build_src_filter`; add the certs include path if
  needed).
- **Change** NVS keys per DESIGN §14.1.

### 4.2 Function-level task list

`ota_wifi.cpp`:
- `ota_wifi_acquire(who)` / `ota_wifi_release()` / `ota_wifi_is_held_by(who)`.
- Refactor `ota.cpp::wifi_begin` and `ota_stop_now` to call these.

`ota_manifest.cpp`:
- `ota_manifest_parse`, `ota_url_is_relative_safe`,
  `ota_sha256_hex_eq`, `ota_released_at_sane` (§2.1).

`ota_pull.cpp` (state machine per DESIGN §7):
- `ota_pull_init()` — load `auto`, `last_chk`, `defer`, `chk_fail`; create the
  task.
- `ota_pull_tick()` — non-blocking: compute `due` (boot ≥ `HEALTHY_MS`, or 24 h
  since `last_chk`, or a queued BLE request), gate on credentials/hybrid/battery,
  start the task; expose `ota_pull_state_name()`.
- `ota_pull_task(void*)` — the blocking worker:
  `st_wifi_join()` → `st_sntp()` → `st_fetch_manifest()` → `st_compare()` →
  `st_download()` → `st_verify()` → `st_activate()` → `st_reboot()`.
- `ota_pull_handle_ctrl(json)` — parse `mode`/`force`/`to`, enqueue; owner gate is
  already enforced by `ble.cpp` lines 338-357.
- `pull_notify(state, pct)` — build the DESIGN §9.2 JSON and call
  `ble_notify_status` (`ble.cpp` lines 531-536).
- `st_download()` uses `WiFiClientSecure` + `HTTPClient` + `esp_ota_begin/write`
  and `mbedtls_sha256_*`; `st_verify()` finishes the digest, calls
  `esp_ota_end`, compares; `st_activate()` calls `esp_ota_set_boot_partition`
  (DESIGN §7.5). Every failure path calls `esp_ota_abort()` and releases WiFi.
- Timeouts/backoff exactly as DESIGN §7.1/§7.3; persist `last_chk`/`defer`/
  `chk_fail`/`pend_ver`.

`ota.cpp`:
- Route `cmd=="update"` → `ota_pull_handle_ctrl`.
- Add `sha`/`build` to the `info` reply.
- `ota_confirm()` (lines 130-135) additionally clears `pend_ver`.

`main.cpp`:
- `ota_pull_init()` right after `ota_init()` (line 207); `ota_pull_tick()` next
  to `ota_tick()` (line 311).

`boards/sim/ota_sim.cpp`:
- Add no-op `ota_pull_init`, `ota_pull_tick`, `ota_pull_handle_ctrl`,
  `ota_pull_is_active`, `ota_pull_state_name`; `ota_pull_is_active()` returns
  false (mirrors the existing `ota_is_active()` comment in `ui.cpp` lines 847-848).

### 4.3 Acceptance criteria

- `pio run -d firmware -e waveshare_amoled_216` and `-e sim` both build.
- On hardware with stored credentials and a reachable VM: boot + 60 s triggers a
  check; a newer manifest downloads, verifies, flashes the inactive slot,
  reboots, and `info` reports the new `fw`; the old slot remains bootable.
- With a manifest whose `sha256` is deliberately wrong, the device logs
  `hash_mismatch`, calls `esp_ota_abort()`, and **does not** reboot; `otadata`
  is unchanged.
- With a manifest version ≤ running, the device reports `no_update` and does not
  download.
- With `board` mismatched, the device reports `board_mismatch` and does not
  download.
- BLE `{"cmd":"update","mode":"check"}` returns `available`/`up_to_date`; a
  concurrent hybrid `{"cmd":"ota","mode":"on"}` returns `busy` (and vice versa).
- `info` includes `sha`/`build`; `daemon/ota_flash.py` still works unchanged.

### 4.4 Test plan

- **Host unit tests** (plain `g++`, the repo pattern in
  `.github/workflows/ci.yml` lines 30-34 and `CLAUDE.md` §Tests), new
  `firmware/test/test_ota_pull/`:
  - `semver_cmp`: ordering, equal, leading `v`, rejects `1.2`, `1.2.3.4`,
    `a.b.c`, empty; `min_from` comparisons.
  - `ota_manifest_parse`: valid example; missing required fields; wrong
    `schema_version`; bad `board`; absolute/`..`/scheme `url`; short/non-hex
    `sha256`; non-positive `size`; unknown fields ignored.
  - `ota_sha256_hex_eq`: equal, case, length mismatch, near-miss.
  - `ota_released_at_sane`: past, now, +25 h, malformed.
  These compile `ota_manifest.cpp` + `ota_semver.h` only (no Arduino/WiFi).
- **Python daemon test** (`daemon/tests/test_ota_flash.py` extended, or a new
  `test_ota_update_frames.py`): the new CTRL frames and TX status/error shapes
  round-trip; `parse_tx` handles the progress notifications; the existing
  `assert_board` still accepts the grown `info` reply.
- **Sim stub**: `pio run -e sim` links with the new stubs; `ui_ota_status` is a
  no-op under `BOARD_SIM`.
- **Hardware smoke matrix** (operator-provided credentials; documented in a new
  `design/ota-pull/hw-matrix.md` or `IMPL` appendix):

  | Case | Setup | Expect |
  |---|---|---|
  | happy path | valid manifest, newer version | downloads, flashes, reboots, `fw` bumps, `boot_tries` resets |
  | no update | manifest == running | `up_to_date`, radio off, no flash |
  | bad hash | corrupt `.bin`, manifest hash kept | `hash_mismatch`, no reboot, old slot boots |
  | bad board | manifest `board` mismatch | `board_mismatch`, no download |
  | too old | running < `min_from` | `too_old`, no download |
  | VM down | stop Caddy / wrong host | retry/backoff, radio off, no flash |
  | bad image | truncate `.bin`, fix hash+size | `esp_ota_end` → `bad_image`, no activate |
  | crash loop | force a bad image that boots then panics | rollback to old slot after 3 tries |
  | battery low | battery < 20 %, no charging | check skipped/deferred |
  | hybrid busy | hybrid `ota on` during pull | `busy`, no radio contention |
  | cert failure | serve with a cert that does not match the pin | `tls_fail`, no download |
  | SNTP failure | block NTP | `timeout` before TLS, no download |

## 5. P2 — CI publish automation

### 5.1 Files

- **Add** `.github/workflows/ota-publish.yml`.
- **Reuse** `design/ota-pull/deploy/ota-publish.sh`.
- **Change** `README.md` (new "Automatic updates (pull OTA)" subsection only).

### 5.2 Task list

1. Workflow trigger: `push: tags: ['v*']` (the existing CI only runs on
   `main` / PR, `.github/workflows/ci.yml` lines 3-7).
2. Job steps: checkout (full history for `git describe`), install PlatformIO,
   `pio run -d firmware -e waveshare_amoled_216`, run `ota-publish.sh` with the
   tag-derived version, `ssh`/`rsync` to the VM using a deploy key in Actions
   secrets (never a password).
3. Guard: refuse to publish when the built `FW_VERSION` and the manifest
   version differ (call `version.py --print-version` and compare), and when the
   tree is dirty.
4. Record the published manifest URL + SHA in the workflow summary.

### 5.3 Acceptance criteria

- Pushing tag `v0.2.0` builds, publishes `manifest.json` + binary atomically,
  and the live `manifest.json` has `version:"0.2.0"` and the correct hash.
- A second tag publishes alongside the first; the last N are retained.
- A failed build or a version mismatch aborts before touching the VM.

### 5.4 Test plan

- Dry-run the workflow with `act` or a manual `workflow_dispatch` to a staging
  directory; assert the manifest content.
- Fetch the published manifest and binary from a machine on the public internet
  and verify `sha256sum` matches.
- Confirm the device check against the staging URL (P1 hardware matrix, happy
  path).

## 6. P3 — UI + telemetry

### 6.1 Files

- **Change** `firmware/src/ui.{h,cpp}`, `firmware/src/ota_pull.{h,cpp}`,
  `firmware/src/main.cpp`.

### 6.2 Task list

1. `ui.h`: `void ui_ota_status(const char* text, int pct);` (DESIGN §12).
2. `ui.cpp`: draw the transient line in the existing animated status area; auto
   clear after ~3 s; no new screen. Extend the existing OTA branch
   (`ui.cpp` lines 845-855) to also light up while `ota_pull_is_active()`.
3. `ota_pull.cpp`: call `ui_ota_status` on each state transition; keep the
   panel-independent path so a dark screen does not block the update.
4. Telemetry: persist the last result (`last_chk`, last `err`) in NVS and log
   `OTA: pull ...` lines on serial. Optional (open question Q7): a
   `{"cmd":"update","mode":"status"}` reply carrying `last_chk` + last error.
5. `ota_sim.cpp` stub for `ui_ota_status` if the UI hook is hardware-gated; keep
   `-e sim` green.

### 6.3 Acceptance criteria

- A running check shows `Checking…`; a download shows `Updating… <pct>%`; an
  error shows `Update failed` for ~3 s; the usage screen is not replaced.
- With the panel asleep, the update still completes and the next wake shows the
  new version.
- `-e sim` still builds and shows no OTA indicator (no WiFi in the sim).

### 6.4 Test plan

- Sim headless screenshot with a forced `ui_ota_status` call to verify layout
  (`SDL_VIDEODRIVER=dummy SIM_AUTOSHOT_MS=...`, `CLAUDE.md` §Desktop simulator).
- Hardware: watch the indicator across a real update; confirm it disappears
  after the reboot.

## 7. P4 — Optional signing (future)

### 7.1 Files

- **Add** `tools/sign_firmware.py`, a pinned public key under
  `firmware/src/certs/`.
- **Change** `ota_manifest.{h,cpp}` (add `sig` / `sig_alg` / `key_id`),
  `ota_pull.cpp` (`st_verify` checks the signature before activation),
  `ota-publish.sh` (sign after hashing).

### 7.2 Task list

1. `tools/sign_firmware.py` — Ed25519 sign the `.bin` (or the manifest hash);
   write `sig` into the manifest.
2. Firmware: pin the Ed25519 public key; verify in `VERIFY_SHA256`; on failure
   `{"err":"bad_signature"}` and abort (DESIGN §13.3).
3. Publish: sign with a key held only in the publish environment (CI secret or
   the VM's HSM/keystore), never in the repo.
4. Optional follow-on project: Secure Boot v2 + flash encryption (irreversible
   eFuse step; explicitly separate).

### 7.3 Acceptance criteria

- An unsigned manifest is rejected by a signed-firmware build.
- A manifest signed by the wrong key is rejected before activation.
- A correctly signed manifest updates normally.
- The hybrid `espota` path's trust story is documented if it remains a
  production path.

### 7.4 Test plan

- Host unit test for the signature verifier with known Ed25519 test vectors.
- Hardware smoke: signed good/bad images; confirm `bad_signature` never
  activates.

## 8. Verification (reviewer checklist)

1. `pio run -d firmware -e waveshare_amoled_216` and `-e sim` both succeed.
2. `python -m pytest daemon/tests -q` passes, including the new manifest/publish
   and CTRL-frame tests.
3. Host C++ tests: `(cd firmware/test/test_ota_pull && g++ -std=c++17 -I ../../src <sources> -o /tmp/otapull && /tmp/otapull)`.
4. `caddy validate --config design/ota-pull/deploy/Caddyfile.example`.
5. Hardware smoke matrix (P1 §4.4) signed off by the operator.
6. No live config references a real VM IP or credential: the host is
   injected via `OTA_PULL_MANIFEST_URL` (DESIGN §14.2).
7. `README.md` gains a pull-OTA subsection that links `design/ota-pull/DESIGN.md`
   and states the same-LAN requirement is lifted for pull.

## 9. Rollback of this work

- Firmware: revert the P1 commits; the hybrid path is untouched because it was
  only refactored to route through `ota_wifi`, which can be reverted
  independently.
- Publish: stop the workflow / remove the VM directory; devices keep running
  their current version (no update is forced).
- No partition or eFuse change is made in P0-P3, so nothing here is
  irreversible. P4's Secure Boot is the only irreversible step and is a separate
  project.
