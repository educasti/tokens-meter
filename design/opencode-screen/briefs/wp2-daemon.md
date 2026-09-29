# WP2 — macOS daemon: OpenCode collector

Repo: /Users/educasti/Projects/Personal/tokens-meter (branch feat/opencode-screens). Read first: `design/opencode-screen/SPEC.md` §2, §8, §9; `design/opencode-screen/research/07-go-plan-limits.md` (model limit table, window semantics); `research/09-go-endpoint-live.md` (verified endpoint — TRAILING SLASH REQUIRED); `research/01-opencode-usage-data.md` (SQLite schema); and `daemon/claude_usage_daemon.py` (config readers `read_chime_setting`/`read_clock_setting`, `Session.write_payload`, `connect_and_run` poll branch).

Edit/create ONLY: `daemon/opencode_collector.py` (new), `daemon/claude_usage_daemon.py`, `daemon/config.example`, `daemon/tests/test_opencode_collector.py` (new).

Implement
- `opencode_collector.py` with a sync `collect(now=None, db_path=None, auth_path=None, fetch=None) -> dict | None` returning the SPEC §8 payload dict (`k:"oc"`, `ok:true`, `src`, `p5 r5 pw rw pm rm st`, `t7`, `m ms`, `a ag la`; with `src:"none"` also `tk cd c7`), or None if OpenCode isn't installed.
  - Go usage: read the `opencode-go` key from `~/.local/share/opencode/auth.json` (field `key` under `opencode-go`), `GET https://opencode.ai/zen/go/v1/usage/` (trailing slash!) with `Authorization: Bearer`, urllib, timeout 10 s. Map rolling/weekly/monthly → p5/pw/pm and resetsAt → minutes (r5 = -1 when rolling percent == 0). st = "limited" if any status != "ok". **Never log or print the key or the Authorization header** (also not in exceptions — catch and log a generic message).
  - Fallback `src:"est"`: local estimate per research/07 "What the device should compute" (cost / model monthly limit, Go column; skip zero-cost models; unknown model → $60). Weekly = since Monday 00:00 UTC; 5h = fixed window from first step after a ≥5 h gap; monthly = since the 1st of the month UTC (approximate).
  - No key → `src:"none"` with tk/cd (today, local midnight) and c7.
  - Local activity from `~/.local/share/opencode/opencode.db` opened read-only (`file:...?mode=ro`, uri=True): t7 (thousands, all tokens incl. cache), top model + share (short name: strip provider, `deepseek-` → `ds-`, ASCII, ≤14 chars), a = sessions in `session_v2` with time_updated within 10 min, ag = agent of the most recent one, la = seconds since last assistant step.
  - Keep payload ≤ 240 bytes (compact JSON); assert in tests.
- In `claude_usage_daemon.py`: `read_opencode_setting()` (`opencode = on|off`, default off; same parser pattern), and in `connect_and_run` poll every 60 s (independent of the Claude token/dead branch), run `collect` via `asyncio.to_thread`, then `await asyncio.sleep(0.25)` and `session.write_payload(oc)` after the Claude write. Log a short line (never the key).
- `config.example`: document `opencode = off` (+ optional `opencode_db`, `opencode_auth`).
- Tests (`pytest`, run from repo root; root conftest adds sys.path): fixture SQLite DB built in a tmp dir with `session_v2` + `session_message` rows; fake fetch returning the real response shape from research/09; cases: api ok, rolling 0 → r5 -1, limited, http error → est fallback, no key → none, payload ≤ 240 bytes, key never appears in any log output (caplog).
- Verify: `python -m pytest daemon/tests -q` passes (create a venv in /tmp if needed: pytest bleak httpx Pillow). Do NOT touch the user's installed LaunchAgent or run the daemon against the real device.

When done reply exactly: `DONE WP2` + test count.
