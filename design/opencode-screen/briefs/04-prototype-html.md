# Brief 04 — Build the interactive HTML prototype

You are a front-end subagent. Build ONE self-contained HTML file. Do not modify anything else in the repo.

**Output file:** `/Users/educasti/Projects/Personal/tokens-meter/design/opencode-screen/prototype.html`

## Read first (mandatory)
1. `/Users/educasti/Projects/Personal/tokens-meter/design/opencode-screen/SPEC.md` — the design spec. Sections 3 (tokens), 4 (layouts with pixel coordinates), 5 (states), 6 (navigation), 7 (payload) are what you implement. Follow its numbers exactly.
2. SVG logos to inline (copy their markup verbatim into the HTML, scale with width/height attributes; keep viewBox):
   - `design/opencode-screen/research/assets/opencode-logo-dark.svg` (the pixel "o" mark)
   - `design/opencode-screen/research/assets/opencode-wordmark-simple-dark.svg` (white "opencode" wordmark)
   If two inlined SVGs share `id`s (mask/clipPath ids), rename ids so they are unique per instance (or strip the mask/clip wrappers — the paths render fine without them).

## What the page is
A prototype of a small desk device (ESP32 AMOLED) that shows Claude Code usage and — new — an **OpenCode usage screen**. The page lets the user click through the device's screens and states to approve the design before firmware work. Page chrome text in **Spanish**; text *on the device screen* in **English** (like the real firmware).

## Page layout
- Page background dark neutral (`#111`), page text `#ddd`, font for page chrome: system-ui. Load **IBM Plex Mono** (weights 400, 500, 600) from Google Fonts: `https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;500;600&display=swap` — used ONLY for the device's OpenCode screen.
- Header: title "Clawdmeter — prototipo pantalla OpenCode", subtitle "v0.1 · ver SPEC.md · nada implementado aún".
- **Section A — Simulador interactivo** (two columns on wide screens, stacked below 900px):
  - Left: the device. A bezel (`#222`, 24px padding, 28px radius, subtle shadow) containing the screen `div` at the exact pixel size of the selected board (1:1 CSS px). Screen has `overflow:hidden; background:#000; position:relative; cursor:pointer`. Clicking the screen = a tap.
    Under the device: row of physical buttons: `BOOT (Space)`, `PWR`, and a caption showing current screen name + brightness level.
  - Right: control panel:
    - **Board**: segmented control — `2.16" 480×480` (default), `1.8" 368×448`, `1.54" 240×240`.
    - **Escenario** (radio list, see Scenarios below).
    - **Modo**: toggle `Límite Go` / `Solo consumo` (Solo consumo = remove `l`,`p5`,`c5`,`pw`,`cw` from the payload).
    - Toggles: `BLE conectado` (default on), `Daemon envía OpenCode` (default on; off = "never received OC data" state).
    - **Payload BLE**: `<pre>` with the current scenario's compact JSON (`JSON.stringify` no spaces) plus a byte counter "N / 240 bytes" (red if > 240).
    - **Navegación**: short explanation (from SPEC §6): tap cycles Splash → Claude → OpenCode → Splash; OpenCode skipped if daemon never sent data; PWR = brightness (or next animation on splash); BOOT = Space (no-op here, show a small toast "BOOT → Space (HID)").
- **Section B — Todas las variantes**: a grid rendering the OpenCode screen statically for every board size × these scenarios: `Sesión activa`, `Cerca del límite`, `Solo consumo`, `Datos viejos`. Each cell: small caption (board + scenario) above the screen. Use the SAME render function as the simulator. Grid wraps responsively; cells are 1:1 pixel size (no scaling), so the grid is wide — let it wrap.
- **Section C — Paleta y tipografía**: swatches for every token in SPEC §3 (name, hex) and a type specimen of IBM Plex Mono at 48/40/24/20/18/16/14/12 px.

## Screens inside the device
1. **Splash**: `<img src="../../screenshots/splash.gif">` scaled with `object-fit:contain` (true 480×480 capture; on other sizes it's scaled — add a tiny page caption under the device "Splash/Claude: capturas reales 480×480 escaladas").
2. **Claude**: `<img src="../../screenshots/usage.png">` same treatment. When BLE toggle is off, overlay nothing (keep it simple).
3. **OpenCode**: rendered with absolutely positioned HTML per SPEC §4, per board size. This is the main deliverable — be precise.

### OpenCode render details (Grande 480×480 — scale per SPEC §4.2/4.3 for other sizes)
Tokens: bg `#000`, element `#1e1e1e`, border `#3c3c3c`, text `#eeeeee`, muted `#808080`, primary `#fab283`, error `#e06c75`, success `#7fd88f`. All text IBM Plex Mono.
- Header: mark SVG at (20,24) size 40×50; wordmark SVG width 200 (height auto ≈ 36) horizontally centered, top 30. A battery indicator top-right at (412,30): draw a simple outlined battery 40×20 with 70% fill in `#eeeeee` (placeholder for the firmware's icon).
- Panel 1 at (20,96) 440×132; Panel 2 at (20,240) 440×132. `border:1px solid #3c3c3c; border-radius:0; background:transparent`. Inner padding 16.
  - Hero (top-left, y+10): Plex Mono 500, 48px, `text`. Límite Go mode: `37%`. Solo consumo: tokens formatted (`3.2M`).
  - Chip top-right (y+14): background `element`, text `primary` (or `error` when value ≥ 85), 18px, padding 2px 8px, square corners. Text: `5h · est.` / `week · est.` (Go) or `today` / `7 days` (Solo consumo).
  - Segmented bar (Go mode only) at y+64 inside panel: 34 squares of 10×20 with 2px gap (Compact: 27 segs 10×16; Small: 18 segs 10×10 w/ 2px gap... fit width). Filled count = round(pct/100 × segs). Filled = `primary` (or `error` if pct ≥ 85); empty = `element`.
  - Bottom line (y+100): 18px `muted`: Go → `$0.74 of $2.00 spent` (window limit = l×0.2 for 5h, l×0.5 for week; money with 2 decimals). Solo consumo → `$0.00 spent`.
- Stats block (Grande/Compacto only): three columns at y=388 (titles, 18px `text`): `7d tokens`, `top model`, `7d cost`; values at y=410 (18px `muted`): `107.0M`, `ds-v4.1-flash 93%`, `$1.10`. Columns at x=20, 170, 360 (Grande). Compact: 16px, x=20,130,280 and truncate model to 12 chars with `…` if needed.
- Status line at y=448 (Grande), 18px:
  - active (`a ≥ 1`): a 10×10 `success` square blinking at 1 Hz (CSS animation) + ` build · working` in `text` (use `ag` value).
  - idle (`a = 0`): `idle · last activity 21m ago` in `muted` (format `la` seconds → `Ns`/`Nm`/`Nh Nm`).
  - stale: `○ stale · updated 12m ago` in `error`; panels + stats at `opacity:.4`.
  - BLE off: `○ bluetooth disconnected` in `error`; panels + stats at `opacity:.4`.
- Token formatting helper (input in thousands): `<1000` → `812k`; `<1_000_000` → `3.2M` (one decimal) e.g. 106969 → `107.0M`; ≥ 1e6 → `1.2B`.
- Compacto (368×448) and Chico (240×240): use exactly the coordinates/sizes listed in SPEC §4.2 and §4.3 (Small: no stats block, status at y=216 12px, hero 24px, chip 12px, line 12px, mark 16×20 at (8,6), wordmark width 100 centered at top 10, battery 24×12 at right 8/top 8).

### Page indicator
After every screen change (in the simulator only), show 3 dots (6px circles, gap 8px) centered at bottom `y = H − 8` for 1.5 s then fade out: current screen dot `#eeeeee`, others `#484848`. If OpenCode is skipped (daemon toggle off) show only 2 dots.

### Brightness
PWR on non-splash screens cycles levels 64 → 128 → 200 → 255 (default 255) and applies `filter: brightness(level/255)` to the screen; on splash, PWR shows toast "PWR → siguiente animación". Toast: small pill that fades after 1.2 s, positioned under the device.

## Scenarios (values for the payload; `k:"oc"`, `ok:true` always)
| Escenario | l | p5 | c5 | pw | cw | tk | cd | t7 | c7 | m | ms | a | ag | la | extra |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Hoy real (modelo gratis) — default | 10 | 0 | 0.00 | 22 | 1.10 | 3221 | 0.00 | 106969 | 1.10 | ds-v4.1-flash | 93 | 0 | build | 1260 | — |
| Sesión activa | 10 | 37 | 0.74 | 45 | 2.25 | 5840 | 0.81 | 112400 | 2.25 | ds-v4.1-flash | 88 | 1 | build | 5 | — |
| Cerca del límite | 10 | 91 | 1.82 | 78 | 3.90 | 18200 | 1.82 | 131700 | 3.90 | ds-v4.1-flash | 81 | 2 | plan | 12 | — |
| Solo consumo | — | — | — | — | — | 3221 | 0.00 | 106969 | 1.10 | ds-v4.1-flash | 93 | 0 | build | 1260 | forces Solo consumo mode |
| Datos viejos | 10 | 0 | 0.00 | 22 | 1.10 | 3221 | 0.00 | 106969 | 1.10 | ds-v4.1-flash | 93 | 0 | build | 1260 | stale 12m (UI state, not in payload) |

(`la` = seconds since last activity. In Solo consumo panel 1 hero uses `tk`, cost `cd`; panel 2 hero uses `t7`, cost `c7`.)

## Technical constraints
- Single file, vanilla JS + CSS, no frameworks, no build step. Only external resource: the Google Fonts stylesheet above (plus the two relative screenshot image paths).
- One function `renderOpenCode(container, sizeKey, data, uiState)` used by both the simulator and the variants grid.
- Must work opened directly from disk (`file://`).
- Keep code readable; section comments. No console errors.

## Done
When finished, open nothing; just reply with one line: `DONE 04` plus the file path and its size in KB.
