# 07 — OpenCode Go plan limits

Sources (fetched 2026-09-28):
- Docs, raw markdown: https://raw.githubusercontent.com/anomalyco/opencode/dev/packages/web/src/content/docs/go.mdx (rendered: https://opencode.ai/docs/go)
- Server source (`anomalyco/opencode`, branch `dev`):
  - `packages/console/core/src/subscription.ts` (window math)
  - `packages/console/core/src/util/date.ts` (week/month bounds)
  - `packages/console/app/src/routes/zen/util/handler.ts` (metering)
  - `packages/console/app/src/routes/zen/go/v1/usage.ts` (usage endpoint)
  - `packages/console/app/src/lib/lite-usage.ts` (console breakdown math)
- `opencode2 --help` / `opencode2 stats --help` (local CLI).

No secrets were read or printed. I did NOT call the live usage endpoint (see Q4).

## TL;DR

- **There is no single dollar limit.** Each model has its own monthly limit (Go: $15 / $30 / $60; Go Plus: $60–$240). Spend on a model counts as `cost / that model's monthly limit`.
- Windows: **5-hour = 20 %, weekly = 50 %, monthly = 100 %** of each model's monthly limit.
- The 5h window is **fixed-start**, not sliding. Weekly resets **Monday 00:00 UTC**. Monthly resets on the **subscription anniversary** (UTC).
- A **real endpoint exists**: `GET https://opencode.ai/zen/go/v1/usage` with `Authorization: Bearer <opencode-go key>` returns `percent` and `resetsAt` for all three windows. It's the best source; local estimation is the fallback.

## Q1 — Monthly dollar limit

Docs, "Usage limits":

> "Usage limits are defined as monthly dollar amounts. The table below shows the monthly limit for each plan and the token costs for each model. Token pricing is the same for Go and Go Plus."
> "Each model has the following usage limits: 5-hour — 20% of the monthly limit; weekly — 50%; and monthly — 100%."
> "Each model's monthly limit below determines how its usage counts toward those allowances."

Plans: **Go $10/month**, **Go Plus $40/month**. The docs give no single "$X of usage" number. Limits are per model (tables below). For Go, models sit in three tiers:
- **$60**: cheap models (Flash, MiniMax, LongCat, Kimi K2.x, Qwen3.7 Plus, Hy3, and so on).
- **$30**: Qwen3.8 Flash, DeepSeek V4 Flash.
- **$15**: expensive models (Kimi K3, GLM-5.3, Grok, GPT Luna, Qwen3.8 Max, DeepSeek Pro, MiMo Pro, V4 Flash Vision).

So heavy use of a $15 model exhausts the quota 4× faster than the same spend on a $60 model.

Server mechanics (`handler.ts` ~L1189): `quotaCost = round(cost * modelInfo.costMultiplier)` is added to the rolling, weekly and monthly counters. A single base limit `B` (`ZEN_LIMITS.lite.monthlyLimit`, a secret) applies to all models, and a model's limit is `B / costMultiplier` (`getModelQuotaLimit` in `lite-usage.ts`). The base `B` is **UNVERIFIED** (likely $60, the largest table value, so multiplier 1 for $60 models). It cancels out in the formula below, so the device doesn't need it.

### Go ($10/mo) per-model table (prices per 1M tokens)

| Model | Input | Output | Cached read | Cached write | Monthly limit |
|---|---|---|---|---|---|
| GLM-5.3-Flash | $0.15 | $0.50 | $0.03 | - | $60 |
| GLM-5.3 | $1.40 | $4.40 | $0.26 | - | $15 |
| GLM-5.2 | $1.40 | $4.40 | $0.26 | - | $60 |
| Kimi K3 | $3.00 | $15.00 | $0.30 | - | $15 |
| Kimi K2.7 Code | $0.95 | $4.00 | $0.19 | - | $60 |
| Kimi K2.6 | $0.95 | $4.00 | $0.16 | - | $60 |
| LongCat-2.0 | $0.30 | $1.20 | $0.006 | - | $60 |
| LongCat 2.5 Preview Free | Free | Free | Free | - | Unlimited (limited time) |
| MiMo-V2.6-Flash | $0.14 | $0.28 | $0.0028 | - | $60 |
| MiMo-V2.6-Pro | $0.435 | $0.87 | $0.003625 | - | $15 |
| MiMo-V2.5 | $0.14 | $0.28 | $0.0028 | - | $60 |
| MiMo-V2.5-Pro | $0.435 | $0.87 | $0.003625 | - | $15 |
| MiniMax M3 | $0.30 | $1.20 | $0.06 | - | $60 |
| MiniMax M2.7 | $0.30 | $1.20 | $0.06 | $0.375 | $60 |
| Muse Spark 1.3 Contributor | $0.10 | $0.20 | $0.002 | - | $60 |
| Muse Spark 1.2 Contributor | $0.10 | $0.20 | $0.002 | - | $60 |
| Qwen3.8 Max | $2.00 | $6.00 | $0.25 | $2.50 | $15 |
| Qwen3.8 Flash | $0.15 | $0.47 | $0.016 | $0.20 | $30 |
| Qwen3.7 Plus (≤256K) | $0.40 | $1.60 | $0.04 | $0.50 | $60 |
| Qwen3.7 Plus (>256K) | $1.20 | $4.80 | $0.12 | $1.50 | $60 |
| DeepSeek V4.1 Flash (Off-Peak) | $0.15 | $0.60 | $0.003 | - | $60 |
| DeepSeek V4.1 Flash (Peak) | $0.30 | $1.20 | $0.006 | - | $60 |
| DeepSeek V4 Pro (Off-Peak) | $0.66 | $1.98 | $0.022 | - | $15 |
| DeepSeek V4 Pro (Peak) | $1.32 | $3.96 | $0.044 | - | $15 |
| DeepSeek V4 Flash (Off-Peak) | $0.15 | $0.60 | $0.003 | - | $30 |
| DeepSeek V4 Flash (Peak) | $0.30 | $1.20 | $0.006 | - | $30 |
| DeepSeek V4 Flash Vision Exp (Off-Peak) | $0.15 | $0.60 | $0.003 | - | $15 |
| DeepSeek V4 Flash Vision Exp (Peak) | $0.30 | $1.20 | $0.006 | - | $15 |
| Hy4 preview | $0.834 | $2.501 | $0.042 | - | $30 |
| Hy3 | $0.14 | $0.58 | $0.035 | - | $60 |
| Space Bunny Free | Free | Free | Free | - | Unlimited (limited time) |
| Grok 4.7 (≤200K) | $2.00 | $6.00 | $0.50 | - | $15 |
| Grok 4.7 (>200K) | $4.00 | $12.00 | $1.00 | - | $15 |
| Grok 4.6 (≤200K) | $2.00 | $6.00 | $0.50 | - | $15 |
| Grok 4.6 (>200K) | $4.00 | $12.00 | $1.00 | - | $15 |
| GPT 6 Luna (≤272K) | $0.10 | $0.50 | $0.01 | $0.125 | $15 |
| GPT 6 Luna (>272K) | $0.20 | $0.75 | $0.02 | $0.25 | $15 |
| GPT 5.6 Luna (≤272K) | $0.20 | $1.20 | $0.02 | $0.25 | $15 |

(The docs table continues with GPT 5.6 Luna >272K: $0.40 / $1.80 / $0.04 / $0.50, $15. Peak hours for DeepSeek: 01:00–04:00 and 06:00–10:00 UTC, Mon–Fri.)

### Go Plus ($40/mo), for reference

Token prices are identical to Go. Monthly limits per model:

| Go Plus limit | Models (Go limit in parentheses) |
|---|---|
| $240 | Kimi K2.6 ($60), LongCat-2.0 ($60), MiniMax M2.7 ($60), Hy3 ($60) |
| $180 | GLM-5.3-Flash, GLM-5.2, Kimi K2.7 Code, MiniMax M3, Qwen3.7 Plus (all $60) |
| $120 | GLM-5.3 ($15), MiMo-V2.6-Flash and V2.5 ($60), Muse Spark 1.2/1.3 ($60), DeepSeek V4.1 Flash ($60), DeepSeek V4 Flash ($30), Hy4 preview ($30) |
| $90 | Qwen3.8 Flash ($30) |
| $60 | Kimi K3, Grok 4.x, GPT 6/5.6 Luna, Qwen3.8 Max, MiMo Pro models, DeepSeek V4 Pro, V4 Flash Vision (all $15) |

Go Plus is not a uniform multiple of Go. The user is on Go, so use the Go column.

## Q2 — Rolling or fixed windows? Reset times

From `subscription.ts` and the SQL in `handler.ts` (~L1200–1235):

- **5-hour ("rolling") window: fixed-start.** The counter's timestamp (`timeRollingUpdated`) is kept while `now − timeRollingUpdated < 5h`, and usage accumulates. Once it's older than 5 h, the next request resets usage to that request's cost and stamps a new start. `resetsAt = timeRollingUpdated + 5h`. If the window is expired or idle, usage reads 0 % and reset is "in 5h" (not active). The name "rolling" is misleading: it does not slide per request. `rollingWindow` is a secret config value; the docs say 5 h.
- **Weekly: fixed calendar week, Monday 00:00 UTC → next Monday 00:00 UTC** (`getWeekBounds`: `(getUTCDay()+6)%7` offset).
- **Monthly: anchored to subscription date**, not the calendar month (`getMonthlyBounds(now, timeSubscribed)`). It uses the UTC day, hour, minute and second of `LiteTable.timeCreated`, clamped to the last day of shorter months. Reset = next anniversary. The device can't know `timeCreated` locally. **UNVERIFIED** how it relates to Stripe billing renewal (likely the same date).

A "resets in X" countdown is possible:
- **Weekly:** exact from local UTC clock alone.
- **5h:** locally, use the timestamp of the first local step after the previous window expired, +5 h. Approximate, because requests from other machines or clients also count.
- **Monthly:** needs the subscription day. Ask the user for it once, or use the endpoint.
- **Exact for all three:** use the usage endpoint (Q4).

## Q3 — Does local `cost` match server accounting? Free models?

- **Free models are excluded.** `handler.ts` ~L896: `if (Object.values(modelInfo.cost).every((price) => price === 0)) return "lite"`. They return before any counter update, so `space-bunny-free` (cost 0) counts for nothing. The docs also say: "If you reach the usage limit, you can continue using the free models."
- **Cost formula server-side** (`calculateCost`): `input·inputTokens + output·outputTokens + cacheRead·cacheReadTokens + cacheWrite5m·… + cacheWrite1h·…`, with:
  - a peak/off-peak price table (DeepSeek), and
  - a >200K/256K/272K context tier price switch (based on input + cache tokens per request).
  These are the same per-1M prices as the doc table.
- **Local `cost`** is computed by OpenCode from its models.dev price metadata. It **should** match when the local prices equal the doc table, but it may not apply peak vs off-peak or the long-context tier, and it may include reasoning tokens differently. **UNVERIFIED**: I did not diff the local DB `cost` against the table.
- **Recommended check:** for a handful of local steps, recompute `tokens × table price` and compare to `data.cost`. If they diverge for the DeepSeek or long-context models, correct locally.
- Also: local DB only sees this machine. Usage from other machines or clients on the same key counts on the server but is invisible locally. **UNVERIFIED** that the user uses the key elsewhere.

## Q4 — Real balance/usage API?

**Yes.** `packages/console/app/src/routes/zen/go/v1/usage.ts` implements:

```
GET https://opencode.ai/zen/go/v1/usage
Authorization: Bearer <OpenCode Go API key>
```

Response shape (from the source):

```json
{ "usage": {
    "rolling": { "status": "ok|rate-limited", "percent": 0-100, "resetsAt": "ISO-8601" },
    "weekly":  { "status": "...", "percent": ..., "resetsAt": "..." },
    "monthly": { "status": "...", "percent": ..., "resetsAt": "..." } } }
```

Errors: 401 (missing or invalid key), 403 (`OpenCode Go subscription required`).

- `percent` is the server's own aggregate (`floor(usage/limit·100)`), already weighted across models by `costMultiplier`. `resetsAt` gives the exact "resets in".
- The host domain and path are inferred from the docs' `https://opencode.ai/zen/go/v1/models` and the route path. **UNVERIFIED live**: I did not call it. The key lives in the user's `auth.json`; a daemon or the device can read it and must never log it.
- The console (https://opencode.ai, workspace → Go) shows the same numbers, including a per-model breakdown of the meter.
- `opencode2` has no Go/balance subcommand: `stats` (`--cost --models --json --days N`) only aggregates local data. There is no `opencode2 go …`.

## What the device should compute

**Preferred (exact):** the host daemon calls `GET /zen/go/v1/usage` every 60 s with the Go key from `auth.json` and forwards `{rolling, weekly, monthly}.{percent, resetsAt}`. The screen shows the 5h and weekly bars, with monthly as a footnote and a countdown to each `resetsAt`. A status of `rate-limited` means 100 %.

**Fallback (local estimate from `session_message`):** for every step with `providerID = "opencode-go"` and `cost > 0`, let `L_m` be the model's monthly limit from the table above (Go column):

```
window_pct(W) = 100 * Σ_steps( cost_step / L_model ) / f_W        f_5h = 0.20, f_week = 0.50, f_month = 1.00
```

The base limit `B` cancels: `quotaCost/B = cost·(B/L)/B = cost/L`. Sum over steps in each window:
- **5h:** steps since `t0`, where `t0` is the earliest step after the last idle gap of ≥5 h. The window ends at `t0 + 5h`. If `now > t0 + 5h`, show 0 %.
- **Weekly:** steps since the most recent Monday 00:00 UTC. Ends next Monday 00:00 UTC.
- **Monthly:** steps since the last subscription anniversary in UTC (needs a configured day/time; **UNVERIFIED** default: fall back to the 1st of the month, which will be off).
- Clamp to 100. Skip zero-cost models (`space-bunny-free`, `longcat-2.5-preview-free`).
- Map model IDs to `L_m`. Unknown models: assume $60 and flag as approximate.

Example: $1.50 spent on Kimi K3 ($15 limit) in the current 5h window = 1.5/15 = 10 % of monthly, so 50 % of the 5h allowance. The same $1.50 on Kimi K2.6 ($60) is 12.5 % of the 5h allowance.

"Resets in" countdown: exact with the endpoint. Locally, weekly is exact, 5h is approximate (`t0 + 5h`), and monthly needs the subscription day.

**Caveats:** limits "may change as we learn from early usage" (docs). The local estimate ignores other devices, DeepSeek peak pricing (unless modeled) and long-context tiers.
