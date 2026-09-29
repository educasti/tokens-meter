# Brief 10 — OpenCode splash animation module (JS, canvas)

You are a front-end subagent. Create ONE new file and touch nothing else:
**`/Users/educasti/Projects/Personal/tokens-meter/design/opencode-screen/oc-splash.js`**

It will be loaded by an existing prototype page (`prototype.html`, which another agent is editing — do NOT edit it) via `<script src="oc-splash.js"></script>`, from `file://`. Plain browser JS (no modules/imports, no build), attaching one global `window.OCSplash`.

## Read first
`/Users/educasti/Projects/Personal/tokens-meter/design/opencode-screen/research/08-opencode-splash-assets.md` — §1.1 (Knight Rider scanner: frames, 40 ms, trail alphas), §1.5 (typewriter + cursor blink timing), §2.3 (pixel grids of the mark and the wordmark, with colours), §4 (concepts + palette). Use its data exactly.

## What it is
A pixel-art splash for an ESP32 AMOLED desk display, in OpenCode's official branding, drawn on a **60×60-cell stage**: `cell = Math.floor(Math.min(W,H)/60)` (8 px at 480, 6 at 368/448, 4 at 240), stage centred in the canvas, background `#000000`. Crisp pixels only: `fillRect` per cell, integer coordinates, `imageSmoothingEnabled=false`. No text rendering on the canvas.

## API
```js
const s = OCSplash.create(canvas);   // canvas already sized W×H by the caller
s.setMood(mood);     // 'idle' | 'active' | 'busy' | 'near' | 'limited'
s.setScene(name);    // 'typeon' | 'assemble' | 'scanner' | null (null = auto from mood)
s.nextScene();       // manual cycle typeon → assemble → scanner → typeon (sets a manual override); returns the new scene name
s.resize();          // re-read canvas size and recompute cell/offsets
s.destroy();         // stop the animation loop
```
Auto scene from mood: idle → `typeon`; active/busy/near/limited → `scanner`. Whenever the scene changes, play that scene's intro, then loop. One `requestAnimationFrame` loop per instance using accumulated elapsed ms (frame-hold timing, not per-rAF steps). Several instances may run at once (the prototype shows a grid of them) — no shared mutable globals besides constants.

## Pixel data (from 08 §2.3)
Mark (4×5), `O` = `#F1ECEC`, `i` = `#4B4646`, `.` = none:
```
OOOO
O..O
OiiO
OiiO
OOOO
```
Wordmark (39×7), `B` = `#B7B1B1`, `C` = `#F1ECEC`, `A` = `#4B4646`:
```
.................................C.....
BBBB.BBBB.BBBB.BBB..CCCC.CCCC.CCCC.CCCC
B..B.B..B.B..B.B..B.C....C..C.C..C.C..C
BAAB.BAAB.BBBB.BAAB.CAAA.CAAC.CAAC.CCCC
BAAB.BAAB.BAAA.BAAB.CAAA.CAAC.CAAC.CAAA
BBBB.BBBB.BBBB.BAAB.CCCC.CCCC.CCCC.CCCC
.....B.................................
```
Letters occupy columns o=0–3, p=5–8, e=10–13, n=15–18, c=20–23, o=25–28, d=30–33, e=35–38.

## Scenes
1. **typeon** (idle). Wordmark at 1 stage-cell per grid cell, placed at stage col 10, row 26. Intro: 200 ms blank, then letters appear one per step in order o,p,e,n,c,o,d,e with fixed delays [60, 60, 60, 150, 60, 60, 250, 60] ms (deterministic). A cursor = a 1×5 block of `#fab283` at 1 column right of the last drawn letter's right edge, rows 27–31 (the letter body rows), solid during typing. Loop: all letters shown, cursor blinks 500 ms on / 500 ms off.
2. **assemble** (reset/boot). Mark scaled ×6 (each mark cell = 6×6 stage cells → 24×30), centred (stage col 18, row 15). Intro: the 14 `O` cells appear one by one, clockwise starting top-left (row0 col0→col3, down the right side, bottom row right→left, up the left side), 40 ms each; then the 4 `i` cells appear together in `#4B4646`, held 120 ms, then loop. Loop: inner cells alternate `#4B4646` ↔ `#5A5858` every 800 ms.
3. **scanner** (active and above). Mark ×4 (16×20 stage cells) at stage col 22, row 10. Below it an 8-block strip: each block 3×3 stage cells, 1-cell gap → 31 cells wide, at stage col 14, row 38. Animate exactly the 54-frame sequence of 08 §1.1 (frames 0–53; implement `calculateColorIndex` equivalently or hard-code the lit patterns from the table), with per-cell colour: head = base colour α1.0; index 1 = base ×1.15 brightness α0.9; index i≥2 = base α 0.65^(i−1); inactive = base α (0.6 × fade), fade = 0.3→1 during movement, 1→0.3 during holds. Pre-blend alpha over black (no canvas globalAlpha needed, but either is fine). Intro: mark appears (single frame, 120 ms), then strip starts.
   Mood variants of scanner:
   - active: base `#fab283`, 40 ms/frame, full 54 frames.
   - busy: base `#fab283`, head uses `#ffc09f`, 20 ms/frame, frames 0–29 only (no rest tail).
   - near: base `#f5a742`, 40 ms/frame, full 54.
   - limited: base `#e06c75`, strip frozen on frame 30 (all dim), and the mark's 4 `i` cells blink `#4B4646` ↔ `#e06c75` at 500 ms.

When `setScene()` is set manually, that scene plays with the current mood's colours (typeon's cursor uses the mood base colour: idle → `#fab283`).

## Quality bar
- No console errors; works when several canvases animate simultaneously; `destroy()` stops the loop.
- At the end of the file, add a small comment block documenting the API.

When finished, reply with one line: `DONE 10` plus the file path.
