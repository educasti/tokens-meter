# Brief 07 — OpenCode Go $10 plan: exact usage limits (exploration only)

You are a research subagent. Do not modify any repo code. Output only the findings file below.

## Context
We are building an ESP32 desk display screen that shows the user's **OpenCode Go** usage as a % of the plan's limits, estimated locally from the per-step `cost` (USD) that OpenCode writes into `~/.local/share/opencode/opencode.db` (table `session_message`, JSON `data.cost`, `data.model.providerID = "opencode-go"`). The user is on **OpenCode Go, $10/month**. A previous pass read https://opencode.ai/docs/go and found: "Usage limits are defined as monthly dollar amounts", "5-hour — 20% of the monthly limit; weekly — 50%; monthly — 100%", but the per-plan table was garbled, so the actual dollar limit of the $10 plan is UNKNOWN.

## Questions
1. The exact **monthly dollar limit** of the Go $10 plan (is it $10 of usage, or more/less?). Also Go Plus for reference.
2. Are the 5-hour and weekly windows **rolling** or **fixed** (reset at a set time)? If fixed, when do they reset (e.g. 5h from first request, weekly on a weekday/time, monthly on billing date)? Anything a device could use to show "resets in X".
3. Are limits counted with the same per-token prices OpenCode computes locally as `cost` (i.e. does local `cost` match server accounting)? Is the free model (`space-bunny-free`, cost 0) excluded from limits?
4. Is there any API/endpoint/CLI (`opencode2 --help`, `opencode2 go …`, console at opencode.ai) that returns the **real** remaining balance/usage for Go? Check `opencode2` subcommands and the `anomalyco/opencode` source (search e.g. `packages/console`, `packages/opencode/src` for "limit", "usage", "go", "quota", "balance"). Do NOT print or record secrets (auth.json, service.json contents).
Sources: fetch https://opencode.ai/docs/go (raw HTML and parse the table carefully — try `curl -s` and look at the table markup directly), https://opencode.ai/go if it exists, the GitHub repo source/docs (`packages/web/src/content/docs/go.mdx` or similar — the raw markdown is the most reliable).

## Deliverable
Write `/Users/educasti/Projects/Personal/tokens-meter/design/opencode-screen/research/07-go-plan-limits.md`: direct answers with quotes + URLs, the per-model table as clean markdown, and a final "What the device should compute" section (formulas, window semantics, whether a "resets in" countdown is possible). Mark uncertain items UNVERIFIED. Reply `DONE 07` when finished.
