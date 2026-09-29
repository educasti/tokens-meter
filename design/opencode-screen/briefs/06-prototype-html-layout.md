# Brief 06 — Rewrite the OpenCode screen renderer with HTML positioning

You are a front-end subagent. Edit ONE existing file in place: `/Users/educasti/Projects/Personal/tokens-meter/design/opencode-screen/prototype.html`. Touch nothing else.

## Why
`renderOpenCode(container, sizeKey, data, uiState)` currently draws with SVG `<text>`, and baseline math keeps misplacing text (hero overlaps the header, chip text sits outside its background, compact bar overflows the panel, stats columns collide). Replace the SVG drawing with **absolutely positioned HTML `div`s**, positioned by their TOP edge.

## Source of truth
Read **Anexo A** at the end of `/Users/educasti/Projects/Personal/tokens-meter/design/opencode-screen/SPEC.md` — a table with every coordinate for the three sizes (`grande` 480×480, `compacto` 368×448, `chico` 240×240). Use those numbers exactly, as a per-size `L` constants object. Colors/states: SPEC §3 and §5.

## Rules for the new `renderOpenCode`
- Keep the same function name and signature; keep every caller (simulator + variants grid) working. Keep the existing helpers (`formatTokens`, `formatMoney`, `formatAgo`-style helper, scenario data, brightness, page indicator, tap handler).
- Root: `container.innerHTML = ''`, then one `div.oc` with `position:relative; width:Wpx; height:Hpx; background:#000; overflow:hidden; font-family:'IBM Plex Mono',monospace; filter:brightness(b/255)`.
- Every child: `position:absolute; left; top; line-height:1; white-space:nowrap; margin:0`.
- **Mark and wordmark:** keep them as small inline `<svg>` elements (these already render correctly — reuse the path data from `getLogoSVG` / `getWordmarkSVG`), placed as absolutely positioned elements at the Anexo A x/y/width. Wordmark height = width × 115/641. Make any `id`s unique or remove them.
- **Battery:** div with 1px `#eeeeee` border at Anexo A rect, inner fill div 70% width, 2px inset, plus a 2×(h/2) nub on the right.
- **Panels:** div at Anexo A rect, `box-sizing:border-box; border:1px solid #3c3c3c; border-radius:0`. Children positioned relative to the panel using the panel-relative numbers (hero top, chip top, bar top, cost-line top) and the panel padding as `left` (chip uses `right: padding`).
  - Hero: `font-weight:500`, color `#eeeeee` always.
  - Chip: `height` and `line-height` = chip height, font size from table, `padding:0 Xpx`, background `#1e1e1e`, color `#fab283` (or `#e06c75` when the panel's pct ≥ 85). Text `5h · est.` / `week · est.` (Go) or `today` / `7 days` (Solo consumo).
  - Bar (Go mode only): a flex row (`gap:2px`) of N segment divs with the table's width×height; filled = round(pct/100 × N) in `#fab283` (or `#e06c75` if pct ≥ 85), rest `#1e1e1e`.
  - Cost line in `#808080`: Go → panel 1 `$c5 of $(l×0.2) spent`, panel 2 `$cw of $(l×0.5) spent`; Solo consumo → panel 1 `$cd spent`, panel 2 `$c7 spent`. Hero in Solo consumo: panel 1 `formatTokens(tk)`, panel 2 `formatTokens(t7)`.
- **Stats** (grande/compacto only): titles `7d tokens`, `top model`, `7d cost` in `#eeeeee`; values `formatTokens(t7)`, `truncate(m, N) + ' ' + ms + '%'` (N from table; append `…` only when truncated), `formatMoney(c7)` in `#808080`.
- **Dimming:** wrap panels + stats in one div with `opacity:.4` when `uiState.stale` or `uiState.offline`; header and status line stay at full opacity.
- **Status line** at table top/size: active (`a ≥ 1`) → square (table size) `#7fd88f` with the existing 1 Hz blink animation, vertically centred on the text, then text `${ag} · working` in `#eeeeee` at the table's text x. Idle → `idle · last activity ${ago(la)}` `#808080` starting at the square's x (no square). Stale → `○ stale · updated 12m ago` `#e06c75`. Offline (takes precedence) → `○ bluetooth disconnected` `#e06c75`.
- Page indicator: position its centre at the table's y for the current size.
- Delete the now-unused SVG helper code (`addText`, `addRect`, `addLine`) if nothing else uses it.

## Self-check before finishing
Trace these and confirm nothing overlaps or leaves its panel (all numbers come from the table, which was designed to fit): grande/compacto/chico × scenarios `real`, `activa`, `limite`, `consumo`, `viejo`. Confirm no reference to undefined fields in `consumo` (it has no `l/p5/c5/pw/cw`). Make sure the simulator still cycles Splash → Claude → OpenCode on click, the board selector re-renders at the new size, and the Solo consumo toggle works.

When finished, reply with one line: `DONE 06` plus the file path.
