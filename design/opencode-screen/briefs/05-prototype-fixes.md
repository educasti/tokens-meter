# Brief 05 — Fix the OpenCode prototype (targeted edits)

You are a front-end subagent. Edit ONE existing file in place: `/Users/educasti/Projects/Personal/tokens-meter/design/opencode-screen/prototype.html`. Do not touch any other file. Read `/Users/educasti/Projects/Personal/tokens-meter/design/opencode-screen/SPEC.md` §3–§6 for reference (note §4.1: text `y` values are the TOP of the text; SVG baseline ≈ top + 0.8 × font size).

The screen is drawn as SVG inside `renderOpenCode(container, sizeKey, data, uiState)`. Layout constants live in the `layout` object per `sizeKey` (`grande` / `compacto` / `chico`). Fix these defects, all of them:

1. **JS crash (blocks everything after it).** In `drawPanel`, the bottom-line cost is `const window = isPanel1 ? data.c5 : data.c7;` — wrong and it crashes in Solo consumo mode (`c5` undefined → `formatMoney` calls `.toFixed` on undefined). Correct mapping:
   - Límite Go mode: panel 1 → `data.c5` of `l*0.2`; panel 2 → `data.cw` of `l*0.5`.
   - Solo consumo mode: panel 1 → `data.cd`; panel 2 → `data.c7`.
   Also rename the variable (`window` shadows the global). Make `formatMoney` / `formatTokens` return `'—'` for undefined/null instead of throwing.
   After this fix the variants grid must render all 12 cells (3 sizes × 4 scenarios) and the palette swatches must appear.
2. **Logo mark is a flat white rect.** Replace the placeholder with the real mark: append an SVG `<g transform="translate(x,y) scale(logoW/240)">` containing the two paths from `getLogoSVG` (`#4B4646` inner cell path first, then the `#F1ECEC` outer path with the hole). Position (margin, top) per SPEC: Grande (20,24) 40×50; Compacto (20,24) 32×40; Chico (8,6) 16×20.
3. **Wordmark is plain text.** Replace `addText('opencode', …)` with the 8 white paths from `getWordmarkSVG` inside `<g transform="translate(x,y) scale(wordmarkW/641)">`, horizontally centred (`x = (w − wordmarkW)/2`). Top y: Grande 32, Compacto 30, Chico 10. Widths: 200 / 150 / 100.
4. **Bar segments too tall.** Segment height must be fixed per size: Grande 20, Compacto 16, Chico 10 (not `panelH − 64 − 36`). Bar top inside panel: Grande `panelY+64`, Compacto `panelY+58`, Chico `panelY+40`. Segment width 10, gap 2 (Chico: width = floor((panelW − 16 − 17×2)/18)).
5. **Bottom line overflows the panel border and collides with the stats block.** Place its baseline at `panelY + panelH − 14` (Grande/Compacto) and `panelY + panelH − 8` (Chico). Hero baseline at `panelY + 12 + 0.8×heroSize`. Chip vertically aligned with the hero's cap height, right edge at `panelX + panelW − 16`.
6. **Stats block** (Grande/Compacto only): titles top at `statsY` (baseline `statsY + 0.8×size`), values top at `statsY + 22` (Grande) / `+20` (Compacto). Columns: Grande x = 20, 150, 370; Compacto x = 20, 110, 276. Titles in `text` `#eeeeee`, values in `muted` `#808080`. Model value = `m` truncated to 12 chars + `…` if longer, then ` ` + `ms` + `%`. Nothing may overlap panel 2 or the status line.
7. **Status line colors.** Only the blinking 10×10 square is `success` green (Chico: 6×6); the text `build · working` is `text` `#eeeeee`. Idle text `muted`; stale/offline text `error`. Baseline at `statusY + 0.8×statusSize`.
8. **Stale / offline dimming** must apply only to panels + stats (wrap them in a `<g opacity=".4">`), not the header or status line.
9. Hero color: keep `text` always (SPEC §5: only bar and chip turn `error` at ≥ 85%).

## Verify before finishing
Open nothing interactively. Re-read your edited `renderOpenCode` and `drawPanel` once to check there are no undefined references. Make sure every scenario (`real`, `activa`, `limite`, `consumo`, `viejo`) × every size renders without throwing (mentally trace `consumo`, which has no `l/p5/c5/pw/cw`).

When finished, reply with one line: `DONE 05` plus the file path.
