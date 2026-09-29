# Brief 11 — Update the prototype to spec v0.2

You are a front-end subagent. Edit ONE existing file in place: `/Users/educasti/Projects/Personal/tokens-meter/design/opencode-screen/prototype.html`. Do NOT edit `oc-splash.js` (another agent owns it); only load and use it.

## Read first
1. `/Users/educasti/Projects/Personal/tokens-meter/design/opencode-screen/SPEC.md` (v0.2) — §4 (usage screen content), §5 (OpenCode splash), §6 (navigation), §7 (states), §8 (payload), Anexo A (coordinates — unchanged from the current renderer).
2. The current `prototype.html` — keep its structure, styling, `renderOpenCode` HTML-positioning approach and Anexo A coordinates. This is an evolution, not a rewrite.
3. `/Users/educasti/Projects/Personal/tokens-meter/design/opencode-screen/oc-splash.js` — if it exists, read its API comment. Contract (use exactly this):
   ```js
   const s = OCSplash.create(canvas);   // canvas sized W×H
   s.setMood(m);   // 'idle' | 'active' | 'busy' | 'near' | 'limited'
   s.nextScene();  // returns new scene name ('typeon'|'assemble'|'scanner')
   s.setScene(null); s.resize(); s.destroy();
   ```
   Load it with `<script src="oc-splash.js"></script>` before the main script. Guard: if `window.OCSplash` is undefined, draw a black canvas with muted text "oc-splash.js no cargado" (no crash).

## Changes

### A. Four-screen cycle
Screens in order: `clawd` (img `../../screenshots/splash.gif`), `claude` (img `../../screenshots/usage.png`), `ocsplash` (a `<canvas>` W×H driven by OCSplash), `ocusage` (`renderOpenCode`). Cyclic. If the "Daemon envía OC" toggle is off → only `clawd`, `claude`. Keep one OCSplash instance for the simulator; `destroy()` it when leaving `ocsplash` or when the board size changes, recreate on entering. Mood from the scenario (see D). Page indicator: one dot per screen in the cycle (4 or 2), current `#eeeeee`, others `#484848`, centred at Anexo A y, shown 1.5 s after each change.

### B. Physical buttons (per board, render under the device)
- 2.16" 480×480: `BOOT`, `PWR`, `SEC` (left→right).
- 1.8" 368×448: `BOOT`, `PWR` (no SEC).
- 1.54" 240×240: `BOOT`, `SEC`, `PWR`.
Each button shows a 2-line label: e.g. `BOOT` / `toque ◀ · mantener: Espacio`; `SEC` / `toque ▶ · mantener: Shift+Tab`; `PWR` / `brillo · splash: anim · 3 s: emparejar`. On 1.8 (no SEC) BOOT label reads `toque ▶ · mantener: Espacio`.
Use `pointerdown`/`pointerup` timing (also handle `pointerleave` as release):
- BOOT: < 300 ms → previous screen (next screen on boards without SEC). ≥ 300 ms → toast `Espacio mantenido 0.8 s → HID (dictado)` with the real held seconds. While held past 300 ms, show a small live pill above the device `⏺ Espacio` until release.
- SEC: < 300 ms → next screen. ≥ 300 ms → toast `Shift+Tab → HID (cambio de modo)`.
- PWR: < 3000 ms: on `clawd` → toast `PWR → siguiente animación Clawd`; on `ocsplash` → `splash.nextScene()` and toast `PWR → escena: <name>`; elsewhere → brightness cycle 64→128→200→255 (existing). ≥ 3000 ms → toast `PWR 3 s → modo emparejar`.
- Tap on the screen → next screen (all boards).
Replace the "Navegación" help box text with SPEC §6 in Spanish, including the note "Espacio y Shift+Tab salen ~300 ms después (hay que distinguir toque de mantener)".

### C. Usage screen content (`renderOpenCode`)
Per SPEC §4 and §7:
- Panel line (same position as the old cost line): `Resets in ${fmtMins(r)}` in `#808080`, where `fmtMins`: ≥1440 → `Xd Yh`; ≥60 → `Xh Ym`; else `Xm`. Panel 1 with `r5 === -1` → `No active window`.
- Chip: `5h` / `week`; append ` · est.` when `src === 'est'`; when `st === 'limited'` the panel whose pct ≥ 100 shows chip `limit`. Chip colour `#e06c75` if pct ≥ 85 or limited, else `#fab283`. Bar: filled `round(pct/100·N)` segments, red if pct ≥ 85.
- Stats col 3: title `month`, value `${pm}% · ${Math.round(rm/1440)}d`. In Solo consumo (`src === 'none'`): title `7d cost`, value `formatMoney(c7)`.
- Solo consumo (`src === 'none'`): heroes `formatTokens(tk)` / `formatTokens(t7)`, chips `today` / `7 days`, no bar, lines `${formatMoney(cd)} spent` / `${formatMoney(c7)} spent`.
- Status line: unchanged behaviour (active / idle / stale / offline).

### D. Scenarios (replace the old list and the old "Modo" control — delete the Modo toggle)
All have `k:"oc"`, `ok:true`. Mood rule for the splash: `limited` if `st==='limited'`; else `near` if max(p5,pw) ≥ 75; else `busy` if a ≥ 2; else `active` if a ≥ 1; else `idle`.
| id | Label | src | p5 | r5 | pw | rw | pm | rm | st | t7 | m | ms | a | ag | la | extra |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| real | Hoy real (default) | api | 0 | -1 | 0 | 9430 | 3 | 20354 | ok | 106969 | ds-v4.1-flash | 93 | 0 | build | 1260 | |
| activa | Sesión activa | api | 37 | 134 | 22 | 5040 | 5 | 20354 | ok | 112400 | ds-v4.1-flash | 88 | 1 | build | 5 | |
| varias | Varias sesiones | api | 58 | 71 | 41 | 5040 | 9 | 20354 | ok | 131700 | kimi-k3 | 61 | 2 | plan | 3 | |
| cerca | Cerca del límite | api | 91 | 42 | 78 | 2880 | 31 | 20354 | ok | 131700 | ds-v4.1-flash | 81 | 1 | plan | 12 | |
| limite | Límite alcanzado | api | 100 | 18 | 83 | 2880 | 33 | 20354 | limited | 131700 | ds-v4.1-flash | 81 | 0 | plan | 240 | |
| est | Estimado (sin red) | est | 12 | 250 | 8 | 9430 | 2 | 20354 | ok | 108200 | ds-v4.1-flash | 92 | 1 | build | 20 | |
| consumo | Solo consumo (sin Go) | none | — | — | — | — | — | — | — | 106969 | ds-v4.1-flash | 93 | 0 | build | 1260 | tk 3221, cd 0, c7 1.10 |
| viejo | Datos viejos | = real | | | | | | | | | | | | | | stale 12m (UI state) |
Payload box shows the compact JSON of the selected scenario (omit keys marked —) and the byte counter.

### E. Variants grid
Two sub-sections:
1. **Pantalla de uso** — 3 sizes × scenarios `activa`, `cerca`, `limite`, `est`, `consumo`, `viejo` (reuse `renderOpenCode`).
2. **Splash OpenCode** — 3 sizes × moods `idle`, `active`, `near`, `limited`: each cell a canvas with its own `OCSplash.create()` + `setMood()`; caption `tamaño — mood (escena)`.

## Constraints
Vanilla JS/CSS, works from `file://`, no console errors, only external resource the existing Google Fonts link. Keep code readable with section comments.

When finished, reply with one line: `DONE 11` plus the file path.
