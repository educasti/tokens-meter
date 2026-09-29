# Brief 09 — Verify the OpenCode Go usage endpoint live (exploration only)

You are a research subagent. Do not modify any repo code. **Never print, echo, log, or write the API key anywhere** (not in the terminal, not in files, not in command lines visible in `ps`). Pass it via a Python script that reads the file and sets the header in-process.

## Context
Research (`/Users/educasti/Projects/Personal/tokens-meter/design/opencode-screen/research/07-go-plan-limits.md`, section Q4) found in the OpenCode source a usage endpoint:
`GET https://opencode.ai/zen/go/v1/usage` with header `Authorization: Bearer <OpenCode Go API key>`, returning `{"usage":{"rolling":{status,percent,resetsAt},"weekly":{...},"monthly":{...}}}`. It was not called live. The user's Go key is in `~/.local/share/opencode/auth.json` under the provider id `opencode-go` (inspect only the JSON *structure/keys* to find the field name holding the key — e.g. `{"opencode-go":{"type":"api","key":"…"}}` — never print values).

## Tasks
1. Write a throwaway Python script in `/tmp` (not in the repo) that loads auth.json, extracts the opencode-go key into a variable, calls the endpoint with `urllib.request` (timeout 10 s, a normal User-Agent), and prints ONLY: HTTP status, response headers of interest (content-type, any rate-limit headers), and the JSON body. Before printing the body, scrub any string that equals the key. Delete the script afterwards.
2. If 404/401/403 or HTML is returned, try the obvious variants once each (`https://opencode.ai/zen/go/v1/usage/`, `https://api.opencode.ai/zen/go/v1/usage`) and report. Do not hammer: at most 5 requests total.
3. Cross-check: using read-only SQLite (`file:~/.local/share/opencode/opencode.db?mode=ro`, table `session_message`, `type='assistant'`, JSON field `data`), compute this machine's `opencode-go` spend in the current 5h window / current week (since Monday 00:00 UTC) / current month, weighted by each model's Go monthly limit from the table in 07 (cost / L_model; free models skipped), and compare with the endpoint's percents. Report both.
4. Measure latency of the endpoint call (ms).

## Deliverable
Write `/Users/educasti/Projects/Personal/tokens-meter/design/opencode-screen/research/09-go-endpoint-live.md` with: exact request used (key redacted as `<KEY>`), status, the real JSON response (scrubbed), latency, the local-vs-server comparison table, and a recommendation (endpoint as primary? local estimate as fallback?). Reply `DONE 09`.
