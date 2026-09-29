# Brief 01 — OpenCode usage data sources (exploration only)

You are a research subagent. **Do not modify any code in this repo.** Your only output is the findings file below.

## Context
Repo: `/Users/educasti/Projects/Personal/tokens-meter` — "Clawdmeter", an ESP32 desk display showing Claude Code usage (session % / weekly %, reset countdowns). A host daemon (macOS: `daemon/claude_usage_daemon.py`) polls Anthropic and pushes JSON over BLE to the device. We want to add a **new screen showing OpenCode usage** (OpenCode = the open-source AI coding agent, https://opencode.ai, repo `sst/opencode` / `anomalyco/opencode`). The user runs OpenCode on this Mac: binary `/Users/educasti/.opencode/bin/opencode2`, version v2.0.18 (shell alias `opencode` → `opencode2`).

## Questions to answer
1. **Where OpenCode stores usage locally** on macOS: find its data dir(s) (e.g. `~/.local/share/opencode/`, `~/.opencode/`, `~/.config/opencode/`, `~/Library/...`). Describe the storage format (JSON files per session/message? SQLite?) and the exact fields available: input/output/reasoning/cache-read/cache-write tokens, cost (USD), model id, provider id, timestamps, session id/title, project/cwd. Show 1–2 **redacted** example records (strip prompt/response text — only keep structure and numeric fields). Never print API keys or auth tokens; do not read `auth.json` contents beyond listing which provider keys exist.
2. **Built-in CLI commands** for stats: run `opencode2 --help`, and try `opencode2 stats` (and any `--days`, `--json`, `--models` flags). Paste the real output (redacted if needed). Say whether a machine-readable (JSON) output exists.
3. **Server/API**: does OpenCode expose a local HTTP server/SDK (`opencode serve`, `/session`, `/event`) that reports token/cost usage? Is it usable by a background daemon when no TUI is running?
4. **Limits / quota**: OpenCode is multi-provider, so there's no single "session %". Find what quota-like signals exist: OpenCode Zen / "opencode go" subscription limits or balances, per-provider rate limits, any usage/limit endpoint. Check the official docs (opencode.ai/docs, especially pages on Zen, providers, stats/usage) and the GitHub repo source. If the user's data shows which providers/models they actually use, list them (names + rough share of tokens/cost over the last 7 and 30 days).
5. **Aggregations a daemon could compute cheaply**: today / last 5h / last 7 days / 30 days tokens and cost, per-model breakdown, active session count, last activity timestamp. Actually compute these from the real local data (a quick python3 or jq script run from a temp dir; do not save scripts inside the repo) and report the numbers — they'll seed the prototype with realistic values.
6. How often files are written (is usage visible live while a session runs, or only at message end?).

## Deliverable
Write **`/Users/educasti/Projects/Personal/tokens-meter/design/opencode-screen/research/01-opencode-usage-data.md`** with sections per question, concrete paths, field names, commands, real computed numbers, and a final **"Recommended data model for the device"** section: a compact JSON payload proposal (short keys, like the existing Claude payload `{"s":45,"sr":120,"w":28,"wr":7200,"st":"allowed","ok":true}`) that stays well under ~200 bytes. Cite URLs for anything taken from the web. Mark anything uncertain as UNVERIFIED.

When finished, reply with one line: `DONE 01` plus the file path.
