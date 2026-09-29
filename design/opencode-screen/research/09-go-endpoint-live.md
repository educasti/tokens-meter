# 09 — OpenCode Go usage endpoint, live verification

Run 2026-09-28 ~20:50 UTC. Throwaway script in `/tmp` (deleted). Key read in-process from `auth.json` (`opencode-go.key`), never printed; output scrubbed of the key.

## Request used

```
GET https://opencode.ai/zen/go/v1/usage/          <-- NOTE trailing slash
Authorization: Bearer <KEY>
User-Agent: Mozilla/5.0 tokens-meter-verify
Accept: application/json
```

Requests made: 2 of the 5 allowed.

| # | URL | Status | Body |
|---|---|---|---|
| 1 | `https://opencode.ai/zen/go/v1/usage` (no slash) | **401** | `{"type":"error","error":{"type":"AuthError","message":"Unauthorized"}}` |
| 2 | `https://opencode.ai/zen/go/v1/usage/` (slash) | **200** | see below |

`api.opencode.ai` variant not needed. The key is valid. The endpoint 401s without the trailing slash, so the daemon MUST use the trailing-slash URL. The docs and source do not say this.

Response headers of interest: `Content-Type: application/json`. No rate-limit headers.

## Real response (scrubbed; nothing to scrub in body)

```json
{"usage":{
  "rolling":{"status":"ok","percent":0,"resetsAt":"2026-09-29T01:49:59.249Z"},
  "weekly": {"status":"ok","percent":0,"resetsAt":"2026-10-05T00:00:00.249Z"},
  "monthly":{"status":"ok","percent":3,"resetsAt":"2026-10-12T17:04:24.249Z"}}}
```

Observations:
- Shape matches the source-derived spec from 07. `percent` is an integer (floored).
- Rolling: 0 % and `resetsAt` = now + 5 h exactly (20:49:59 + 5 h). This is the idle placeholder, not a real window end. When percent is 0, the daemon should show "no active window".
- Weekly resets Mon 2026-10-05 00:00 UTC, as predicted (`.249Z` ms offset is server-side noise).
- Monthly is anchored to the subscription anniversary: the window is 2026-09-12 17:04:24 UTC to 2026-10-12 17:04:24 UTC. This confirms the anchored-month behaviour (not a calendar month).

## Latency

512 ms (200 response); 672 ms (the 401). Fine for a 60 s poll.

## Local vs server

Local: read-only SQLite, `session_message`, `type='assistant'`, `model.providerID='opencode-go'`, cost / model monthly limit. Only paid Go model used locally is `deepseek-v4.1-flash` (863 msgs, $1.2013 total, limit $60, last used 2026-09-25 00:03 UTC). `space-bunny-free` (153 msgs) is free and skipped.

| Window | Local estimate | Server `percent` | Note |
|---|---|---|---|
| 5 h (last 5 h from now) | 0 % | 0 % | match (no local Go spend since 09-25) |
| Weekly (since Mon 2026-09-28 00:00 UTC) | 0 % | 0 % | match |
| Monthly (anchored 2026-09-12 17:04 UTC) | 2.00 % ($1.2013 / $60) | 3 % | server is higher by ≥1 pt |
| Monthly (calendar, since 09-01) | 2.00 % | 3 % | same spend, so the window choice does not matter here |

Monthly gap (local 2.0 % vs server 3 %, floored so the true value is in [3, 4)): the server counts more than this machine saw. Plausible causes: usage from other machines or clients on the same key, local `cost` under-reporting vs server pricing (peak-hours tier for DeepSeek), or a different `costMultiplier` base than assumed. This data cannot tell which. The 5 h and weekly windows carry no signal because there was no recent activity, so those are not validated by this run beyond "both 0".

## Recommendation

1. **Endpoint as primary.** It works, is fast (~0.5 s), returns exact percents and reset times for all three windows, and requires no pricing table. Poll every 60 s (or slower; monthly moves slowly). Use the **trailing-slash URL**.
2. **Local estimate as fallback only** (offline, 401/403/5xx, key missing). Label it as approximate on the device. It under-reports by at least 1 pt monthly here, and it cannot know the monthly anchor, the 5 h window start, or other-machine use.
3. Daemon handling: rolling `percent == 0` means idle (ignore its `resetsAt`); `status != "ok"` means rate-limited, show 100 %. Never log the key or the `Authorization` header. Treat 401 as "key invalid / re-auth", 403 as "no Go subscription".
4. Follow-up worth doing when there is active usage: re-run during a session to check that the 5 h and weekly percents track the local numbers.
