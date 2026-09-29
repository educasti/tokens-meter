# Brief 08 — OpenCode animations / motion assets for a splash screen (exploration only)

You are a research subagent. Do not modify any repo code. Output: the findings file below plus any downloaded assets.

## Context
Repo `/Users/educasti/Projects/Personal/tokens-meter` ("Clawdmeter", ESP32 AMOLED desk display). Its splash screen plays Anthropic's official pixel-art "Clawd" animations (see `research/clawd-official/` and `firmware/src/splash.cpp`, `tools/convert_official_clawd.js`: frames on a grid, ≤16-colour palette per animation, per-frame hold ms, intro→loop→outro). We now want an **OpenCode splash** with OpenCode's official branding. Existing brand research: `design/opencode-screen/research/02-opencode-branding.md` (logo SVGs in `design/opencode-screen/research/assets/`, TUI block logo in `packages/tui/src/logo.ts`, palette: peach `#fab283`, greys, logo `#F1ECEC`/`#4B4646`).

## Questions
1. Does OpenCode have **official animations**? Search the `anomalyco/opencode` repo (GitHub, branch `dev`) and opencode.ai for: animated logo/loader/spinner in the TUI (e.g. start-up logo animation, "thinking"/working spinners — find the exact frames/characters and timings in source), animated SVG/Lottie/GIF/video on the website or desktop app (`packages/app`, `packages/desktop`, `packages/web`, `packages/console`), og images, the `/brand` page, a mascot or character. Give file paths, frame data, timings, and download any GIF/Lottie/SVG animation into `design/opencode-screen/research/assets/motion/` (note source URLs).
2. The TUI logo shading trick (markers `_ ^ ~ ,` → shadow colours, `shadow = tint(bg, fg, 0.25)`): document precisely how it renders so it could be redrawn as pixel art; produce an exact cell grid (text art) of the full "opencode" block logo including shadow cells.
3. Any motion in the desktop app or website hero (typing effect, cursor blink, shimmer)? Describe precisely (durations, easing) if found.
4. Propose 3–5 splash animation concepts that use ONLY official material (logo mark, block wordmark, TUI spinner frames, palette), each describable as frames on a pixel grid (e.g. 60×60 stage like Clawd), e.g.: block wordmark typing in letter by letter with a blinking cursor; the "o" mark assembling from pixels; TUI spinner around the mark; mood variants for idle / active / near-limit.

## Deliverable
Write `/Users/educasti/Projects/Personal/tokens-meter/design/opencode-screen/research/08-opencode-splash-assets.md` with sources (URLs/paths), exact frame data, the pixel grid of the logo, and the concepts. Mark uncertain items UNVERIFIED. Reply `DONE 08` when finished.
