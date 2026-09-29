# 01 — OpenCode usage data sources

Researched 2026-09-28 on the user's Mac. OpenCode v2.0.18 (`~/.opencode/bin/opencode2`). No repo code was modified. Secrets were not recorded here. `service.json` holds a local service password and `auth.json` holds a provider key. Only the provider name (`opencode-go`) is listed below.

## 1. Local storage

OpenCode v2 stores everything in **one SQLite database** (WAL mode):

- `~/.local/share/opencode/opencode.db` (plus `-wal` and `-shm`), 22 MB.
- `~/.local/share/opencode/auth.json`, mode 0600. Its only provider key is `opencode-go`.
- `~/.local/share/opencode/log/opencode.log` is the server log (4 MB).
- `~/.config/opencode/` holds config only: `opencode.jsonc`, `cli.json`, `tui.jsonc`, `service.json` (background-service password, mode 0600), plugins and skills.
- `~/.opencode/` holds the binary and node_modules. There are no usage files there.
- There is no `~/Library/...` data.

### Tables that matter

- `session_v2` is the live v2 table, with 49 rows. The old `session` and `message` tables are v1 leftovers (1 and 2 rows, last touched 2026-09-18). Ignore them.
- `session_message` (1268 rows) holds the per-message rows the aggregations below use. Its `type` values are `assistant` (1019), `user` (115), `idle` (113), `synthetic`, `system`, `shell`, `agent-switched` and `model-switched`. `data` is JSON.
- `project` holds project names and worktree paths.
- `event` and `event_sequence` are an event log.

### `session_v2` columns

Cumulative per-session totals:

| Field | Type / meaning |
|---|---|
| `id`, `project_id`, `parent_id` | `parent_id` is set for subagents |
| `title`, `slug`, `directory`, `path` | |
| `agent`, `model` | |
| `cost` | real, USD |
| `tokens_input`, `tokens_output`, `tokens_reasoning`, `tokens_cache_read`, `tokens_cache_write` | integers |
| `time_created`, `time_updated`, `time_idle`, `time_viewed`, `time_archived`, `time_suspended` | epoch ms |
| `idle_outcome` | |

### Example `session_message` row (`type='assistant'`, text and snapshot content stripped)

```json
{
  "time":  {"created": 1790624356523, "streamed": 1790624363847, "completed": 1790624363883},
  "agent": "build",
  "model": {"id": "space-bunny-free", "providerID": "opencode-go", "variant": "default"},
  "finish": "stop",
  "cost": 0,
  "tokens": {"input": 925, "output": 464, "reasoning": 0,
             "cache": {"read": 55331, "write": 0}}
}
```

Fields available per assistant step:

- Tokens: input, output, reasoning, cache read, cache write.
- `cost` in USD, computed by OpenCode.
- Model: `model.id`, `model.providerID`, `model.variant`.
- Timestamps: `time.created`, `time.streamed`, `time.completed`, in epoch ms.

Session-level fields (title, directory, agent) come from `session_v2`.

### Example `session_v2` summary (via the API, redacted)

```json
{"id":"ses_…","agent":"build","model":{"id":"space-bunny-free","providerID":"opencode-go","variant":"default"},
 "cost":5.96e-05,
 "tokens":{"input":88335,"output":11099,"reasoning":6471,"cache":{"read":2213828,"write":0}},
 "outcome":"succeeded","time":{"created":1790619084339,"updated":1790624353653,"idle":1790624363886},
 "title":"…","location":{"directory":"/Users/…/clawd-meter"}}
```

## 2. Built-in CLI

`opencode2 stats` flags (from `--help`):

- `--days N`, where 0 means today.
- `--year`
- `--all`
- `--project`
- `--models`
- `--tools`
- `--cost`
- `--full`
- `--limit`
- **`--json`**

Real output of plain `opencode2 stats`:

```
opencode stats · 2026 so far · all projects
40 sessions · 9 subagents
106 prompts · 1k steps · 113.5m tokens
99.2% tool success · 5 active days · best streak 2 days
```

`opencode2 stats --models`:

```
model                                 tokens   steps   cost
opencode-go/deepseek-v4.1-flash#h…    105.8m     863   $1.20
opencode-go/space-bunny-free#defa…      7.7m     153   $0.00
opencode/big-pickle#default             8.5k       1   $0.00
console-anthropic/claude-haiku-4-…         0       2   $0.00
```

**A machine-readable form exists.** `opencode2 stats --days 7 --json` returns:

- `range{from,to}`, `sessions`, `subagents`, `prompts`, `steps`
- `tokens{input,output,reasoning,cache{read,write}}`, `cost`
- `tools{...}`, `activeDays`, `streak`, `activity[{date,steps}]`
- `models[{model{id,providerID,variant},steps,tokens{…},cost}]`

Caveats:

- It has no "last 5h" window, only whole days.
- It talks to the background service, so it starts or needs that service.
- It is slower than a direct SQLite read.

## 3. Server / API

- OpenCode v2 runs a **background service** (`opencode2 service start|status|stop|restart`).
  - Here `service status` reports `http://127.0.0.1:49374`.
  - It is up even with no TUI open.
  - The `opencode2 serve` command starts a standalone v2 API + web server.
- Auth is HTTP Basic auth. The username is `opencode`; the password is in `~/.config/opencode/service.json` (mode 0600). A daemon on the same user account could read it.
- `GET /openapi.json` returns an OpenAPI 3.1 spec titled "opencode HttpApi", described as "Experimental".
- Endpoints relevant to usage:
  - `GET /api/session` returns session summaries with `cost`, `tokens{…}`, `model` and `time{created,updated,idle}`.
  - `GET /api/session/active`
  - `GET /api/experimental/session/stats`
  - `GET /api/session/{id}/message`
  - `GET /api/session/{id}/context`
- The CLI wrapper is `opencode2 api <operationId | METHOD path>` (`--server URL`, `--param k=v`).
- The `/global/health`, `/session` and `/doc` paths returned the SPA HTML instead of JSON.
- The API is **experimental and v2-only**, so it can change between releases. The daemon would also need a running service.

## 4. Limits / quota

There is no per-account "session %" endpoint that I found. I did not find a usage-limit or balance endpoint in the OpenAPI paths or CLI. The `--json` output has no `remaining` or `limit` fields. UNVERIFIED: I did not read the `sst/opencode` source, so a balance endpoint on the Zen/Go web console cannot be ruled out.

**OpenCode Go limits are documented as dollar amounts** (https://opencode.ai/docs/go):

- Plans:
  - Go, $10/month.
  - Go Plus, $40/month.
- "Usage limits are defined as monthly dollar amounts."
- "5-hour — 20% of the monthly limit; weekly — 50%; and monthly — 100%."
- Per-model token prices and a per-model "monthly limit" column are in a table on that page. The Go and Go Plus column split was garbled by my HTML-to-text pass, so I do not know which plan gets which limit for `deepseek-v4.1-flash`. UNVERIFIED.

**These limits are computable locally.** The cost per step is already in the DB, so a daemon can estimate 5h, weekly and monthly percent as `sum(cost over window) / (limit × fraction)`. The limit dollar amount depends on the plan and on OpenCode's cost accounting matching the server's. Both are UNVERIFIED. The Zen pricing page is https://opencode.ai/docs/zen.

**Providers and models actually used** (assistant steps, tokens = input + output + reasoning + cache):

| Window | Model | Tokens | Cost |
|---|---|---|---|
| 7d | `opencode-go/deepseek-v4.1-flash` | 99.3M (93%) | $1.104 |
| 7d | `opencode-go/space-bunny-free` | 7.7M (7%) | $0 |
| 30d | `opencode-go/deepseek-v4.1-flash` | 105.8M (93%) | $1.201 |
| 30d | `opencode-go/space-bunny-free` | 7.7M (7%) | $0 |
| 30d | `opencode/big-pickle` | 8.5k | $0 |

`stats --models` also lists `console-anthropic/claude-haiku-…` with 2 steps and 0 tokens. It does not show up in the `session_message` aggregation. Total cost is about $1.20, so the user is far below any plausible monthly cap.

## 5. Aggregations (computed from `session_message`, read-only)

Run 2026-09-28 about 15:00 local. "Tokens" is total including cache; "non-cache" is input + output + reasoning. Cache reads dominate (about 95% of tokens).

| Window | Steps | Tokens (total) | Tokens (non-cache) | Cost |
|---|---|---|---|---|
| Today | 80 | 3,220,770 | 211,276 | $0.00 |
| Last 5h | 80 | 3,220,770 | 211,276 | $0.00 |
| Last 7d | 920 | 106,968,984 | 4,033,503 | $1.104 |
| Last 30d | 1015 | 113,491,371 | 4,316,402 | $1.201 |

- Today and last 5h are the same because all activity happened after 09:00.
- Today's spend is all on the free model, so cost is $0.
- Lifetime, from `session_v2`: 49 sessions (including 9 subagents per `stats`), cost $1.2057, about 113.5M tokens.
- Last activity: 2026-09-28 14:39:23 local.
- Active sessions: 0 with an update in the last 10 minutes at the time of the run. One session has `time_idle IS NULL AND time_archived IS NULL`.
- `stats --days 7 --json` cross-check: 9 sessions, 176 steps, 22 prompts. Its `steps` differs from my 920 because `--days 7` counts only calendar days and `steps` there may exclude something. UNVERIFIED which definition it uses.

A daemon can get all of this with a few read-only SQL queries against `session_message` (indexed on `time_created`) or with one `/api/session` call. It does not need the CLI.

## 6. Write frequency

- `opencode.db-wal` was modified minutes before the queries, and `opencode.db` itself only at checkpoint time.
- Each assistant `session_message` row carries `time.created`, `streamed` and `completed`, so a row appears at least by step end.
- The `session_v2` totals appear to be updated per step (`time.updated` moves during a session). UNVERIFIED whether they are updated mid-stream.
- **Conclusion:** usage is visible per step, not only when a session ends. Reading the DB while OpenCode has it open is safe if you open it read-only (`?mode=ro`) and let SQLite read the WAL. I did this without problems.

## Recommended data model for the device

Existing Claude payload for reference: `{"s":45,"sr":120,"w":28,"wr":7200,"st":"allowed","ok":true}`.

Proposed OpenCode payload (~150 bytes):

```json
{"t5":80,"c5":0.0,"t7":107000,"c7":1.10,"t30":113500,"c30":1.20,"m":"ds-v4.1-flash","ms":93,"n":0,"la":600,"ok":true}
```

| Key | Meaning |
|---|---|
| `t5`, `t7`, `t30` | Tokens (thousands, total incl. cache) for last 5h / 7d / 30d |
| `c5`, `c7`, `c30` | USD cost for the same windows, as a float |
| `m`, `ms` | Top model (short name) and its percent share of 7d tokens |
| `n` | Active sessions (updated in the last 10 min) |
| `la` | Seconds since last activity |
| `ok` | Data valid |

Optional extras if the Go plan is confirmed: `p5`, `pw` for percent of the 5h and weekly Go limits, computed as `cost / (monthly_limit × 0.2 or 0.5)`. They need the plan's dollar limit in a daemon config (`opencode_monthly_limit`) and should be marked estimated on screen.

Design notes:

- Use a daemon poll of 60 s with a read-only SQLite query. This mirrors the Claude daemon's heartbeat behavior.
- Cost is tiny for this user (about $1.20 over 30 days), so the screen should lead with tokens and activity, not dollars.
- Show non-cache tokens as a secondary number, since cache reads (about 95%) make the total look inflated.

## Sources

- https://opencode.ai/docs/go (limits and plans)
- https://opencode.ai/docs/zen (Zen pricing)
- https://opencode.ai/docs/cli (`stats` flags)
- Local runs: `opencode2 --help`, `stats`, `stats --models`, `stats --days 7 --json`, `service status`, `GET /openapi.json`, `GET /api/session`, and read-only SQL on `opencode.db`.
