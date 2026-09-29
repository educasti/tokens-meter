# Brief 02 — OpenCode brand identity (exploration only)

You are a research subagent. **Do not modify any code in this repo.** Your outputs are the findings file and downloaded assets described below.

## Context
Repo: `/Users/educasti/Projects/Personal/tokens-meter` — "Clawdmeter", an ESP32 AMOLED desk display (screens 480×480, 368×448, 410×502, 240×240) that currently shows Claude Code usage with Anthropic branding. We want a **new screen for OpenCode usage** (OpenCode = open-source AI coding agent, https://opencode.ai, GitHub `sst/opencode` / `anomalyco/opencode`) that uses **OpenCode's own branding**: fonts, logo, icons, colors, visual language.

## Questions to answer
1. **Logo / wordmark / mark**: find the official OpenCode logo files (SVG preferred). Look in the GitHub repo (search for `logo`, `brand`, `favicon`, `.svg` under `packages/web`, `packages/console`, `packages/ui`, `packages/desktop`, etc.), on opencode.ai (favicon, og-image, `/brand` or press page), and the TUI's ASCII-art logo (the big block-letter "opencode" shown on TUI start — find its exact source text in the repo). Download the actual SVG/PNG files into `design/opencode-screen/research/assets/` (keep original filenames, note source URLs).
2. **Colors**: exact hex values for the brand palette and the **default TUI theme** (the `opencode` theme JSON in the repo — find the file, list background, text, primary/accent, success/warning/error, borders, dark and light variants). Also the website's CSS color tokens.
3. **Typography**: which fonts opencode.ai and the desktop/console apps use (check CSS/`@font-face`/tailwind config in the repo). Font names, weights, and **license** (OFL? commercial like Berkeley Mono?). If a font is commercial/non-redistributable, propose the closest open alternative (e.g. JetBrains Mono, IBM Plex Mono, Geist Mono) and say so explicitly. Download font files only if they are openly licensed and served from the repo or Google Fonts; note license.
4. **Iconography & visual language**: icon set used (Lucide? custom?), UI motifs (square corners, terminal/monospace aesthetic, block characters, pixel art, borders, spacing), any mascot. Describe how the TUI renders things like model name, token counts, cost, progress/context usage — these are good patterns to mimic on a small screen.
5. **Brand usage rules / license**: is there a brand guideline page or trademark note? What's the repo license (MIT?) and does it cover the logo?

## Deliverable
Write **`/Users/educasti/Projects/Personal/tokens-meter/design/opencode-screen/research/02-opencode-branding.md`** with: palette table (name → hex → where used), typography (font, weights, license, fallback), logo files list (local path + source URL + license), inline copy of the TUI ASCII logo, visual-language notes, and a final **"Design tokens for a 480×480 black AMOLED screen"** section (background should stay true black #000 for AMOLED; propose accent, text, muted, success/warn/error colors and font sizes). Cite URLs. Mark uncertain items UNVERIFIED.

When finished, reply with one line: `DONE 02` plus the file path.
