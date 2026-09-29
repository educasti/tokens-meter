# 08 — OpenCode splash: official motion assets, logo pixel grid, animation concepts

Repo: https://github.com/anomalyco/opencode, branch `dev` (files read via GitHub contents API, 2026-09-28). Complements `02-opencode-branding.md`. Nothing in the firmware repo was modified.

## 0. Short answer

- **No mascot, no GIF, no Lottie, no animated SVG logo, no animated-logo start-up sequence exists.** Grep of the full `dev` tree (7,413 paths) finds no `.gif`/`.lottie`, and no logo animation code. OpenCode's official motion is entirely *functional UI motion*: (a) the TUI "Knight Rider" block scanner, (b) a braille spinner, (c) the web/desktop 4×4 pulsing-squares spinner, (d) a per-character text shimmer, (e) a typewriter with blinking cursor, (f) two product screen-recording MP4s on the website. All frame data is in source and is reproduced below, so frames can be rebuilt exactly.
- The logo mark and wordmark are exact integer pixel grids (units of 60 and 16.43 SVG px), so pixel-art conversion is lossless (§2).

## 1. Official animations found

### 1.1 TUI working indicator — "Knight Rider" block scanner (the signature one)
- Source: `packages/tui/src/ui/spinner.ts` (`createFrames`, `createColors`, `deriveTrailColors`), used in `packages/tui/src/component/prompt/index.tsx` (`<spinner … interval={40} />`, ~line 1322–1344, 1525) and `packages/opencode/src/cli/cmd/run/footer.view.tsx` (~line 259, 844).
- Config actually used: `createFrames({ color, style:"blocks", inactiveFactor:0.6, minAlpha:0.3 })`; defaults `width=8`, `holdStart=30`, `holdEnd=9`, `trailSteps=6`. `color` = the current **agent's colour** (`local.agent.color(agent.name)`) in the prompt, `theme().highlight` in the run footer. Agent colours come from theme; which hex the default agent gets is UNVERIFIED (theme primary `#fab283` is the likely value).
- Glyphs: active cell `■`, inactive cell `⬝` (style "blocks"; style "diamonds" would use `⬥ ◆ ⬩ ⬪` / `·`, not used).
- **Timing: 40 ms/frame, 54 frames → 2.16 s cycle** (= 8 forward + 9 hold-end + 7 backward + 30 hold-start).
- Motion: bidirectional sweep, head moves 0→7 (8 frames), holds at cell 7 while the trail drains (9 frames), sweeps back 6→0 (7 frames), holds at cell 0 while the trail drains (30 frames ≈ 1.2 s of "resting" dim dots). Trail = up to 6 cells behind the head (head + 5).
- **Exact glyph frames** (from a re-implementation of `calculateColorIndex`; ■ = lit, ⬝ = dim):

```
 f  cells        f  cells        f  cells
 0  ■⬝⬝⬝⬝⬝⬝⬝    9  ⬝⬝⬝■■■■■   18  ⬝⬝⬝⬝⬝■■■
 1  ■■⬝⬝⬝⬝⬝⬝   10  ⬝⬝⬝⬝■■■■   19  ⬝⬝⬝⬝■■■■
 2  ■■■⬝⬝⬝⬝⬝   11  ⬝⬝⬝⬝⬝■■■   20  ⬝⬝⬝■■■■■
 3  ■■■■⬝⬝⬝⬝   12  ⬝⬝⬝⬝⬝⬝■■   21  ⬝⬝■■■■■■
 4  ■■■■■⬝⬝⬝   13  ⬝⬝⬝⬝⬝⬝⬝■   22  ⬝■■■■■■⬝
 5  ■■■■■■⬝⬝   14  ⬝⬝⬝⬝⬝⬝⬝⬝   23  ■■■■■■⬝⬝
 6  ⬝■■■■■■⬝   15  ⬝⬝⬝⬝⬝⬝⬝⬝   24  ■■■■■■⬝⬝
 7  ⬝⬝■■■■■■   16  ⬝⬝⬝⬝⬝⬝⬝⬝   25  ■■■■■⬝⬝⬝
 8  ⬝⬝■■■■■■   17  ⬝⬝⬝⬝⬝⬝■■   26  ■■■■⬝⬝⬝⬝
                                27  ■■■⬝⬝⬝⬝⬝
                                28  ■■⬝⬝⬝⬝⬝⬝
                                29  ■⬝⬝⬝⬝⬝⬝⬝
                        30–53: ⬝⬝⬝⬝⬝⬝⬝⬝  (24 frames, all dim)
```
  (Frames 0–13 forward + hold/drain right; 14–16 empty; 17–29 backward + drain left; 30–53 rest.) Note hold-end drain: `index = distance + holdProgress`, so a lit cell disappears when index ≥ 6.
- **Per-cell colour/alpha** (`deriveTrailColors(color, 6)`): index 0 (head) = colour, α 1.0; index 1 = colour ×1.15 brightness (clamped), α 0.9 ("bloom"); index i≥2 = colour, α = 0.65^(i−1) → 0.65, 0.4225, 0.2746, 0.1785. Inactive cells: colour with α = `inactiveFactor` 0.6 × fade, where fade goes from `minAlpha` 0.3 → 1 linearly across a movement pass and 1 → 0.3 across a hold (so the whole strip dims while resting).
- Reduced-motion fallback: if KV `animations_enabled` is false, static `[⋯]` (prompt) — `packages/tui/src/component/prompt/index.tsx:1524`.

### 1.2 TUI braille spinner (`Spinner` component, start-up and generic loading)
- Source: `packages/tui/src/component/spinner.tsx`.
- `SPINNER_FRAMES = ["⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"]`, `interval={80}` (ms) → 10 frames, **800 ms** per turn. Colour default `theme.textMuted` (`#808080`). Disabled fallback text: `⋯ <label>`.
- Used by `packages/tui/src/component/startup-loading.tsx`: label "Loading plugins…" → "Finishing startup…"; appears only if loading exceeds **500 ms**, then stays at least **3000 ms** once shown; bottom-centre chip on `backgroundPanel`. This is the closest thing to a "start-up animation"; there is no animated logo.

### 1.3 Web / desktop app spinner — 4×4 pulsing squares
- Source: `packages/ui/src/components/spinner.tsx`, `spinner.css`, keyframes in `packages/ui/src/styles/animations.css`.
- SVG `viewBox 0 0 15 15`, 16 rects 3×3 (rx=1) on a 4 px pitch (x,y ∈ {0,4,8,12}), rendered 18 px wide. **4 corners are hidden (opacity 0)**, giving a rounded-square 12-cell ring+core shape. 8 "outer" cells (indices 1,2,4,7,8,11,13,14) animate `pulse-opacity-dim` (opacity 0.15↔0.35); the 4 centre cells (5,6,9,10) animate `pulse-opacity` (0.4↔1). Each cell: `ease-in-out infinite`, **random** duration 1–2 s, random delay 0–1.5 s (so no fixed frame sequence; it is stochastic shimmering).
- Also `pulse-scale` (scale 1↔0.6667, 1.2 s) and `pulse` (2 s) tokens; `fadeUp` 0.4 s ease-out, 5 px rise, stagger 0.1 s (`animations.css`).

### 1.4 Text shimmer (web/desktop "thinking" labels)
- Source: `packages/ui/src/components/text-shimmer.css` (+ `v2/components/text-shimmer-v2.css`).
- Per-character linear-gradient sweep: `--step 45ms` delay per character index (negative delay → wave), `--duration 1200ms`, `linear`, infinite, `--spread 5.2ch`, gradient `transparent → peak → transparent` at 90°, `background-size: 360%`; base colour `--text-weak`, peak `--text-strong` (v2: `text-muted` → `text-base`). Cross-fade in/out `--swap 220ms ease-out`. Honors `prefers-reduced-motion`.

### 1.5 Typewriter + blinking cursor
- Source: `packages/ui/src/components/typewriter.tsx`, `typewriter.css`.
- Start delay 200 ms. Per-character delay is randomised: 5 % → 150–250 ms (long pause), 10 % → 80–140 ms, 85 % → 30–80 ms. Cursor glyph `│` shown while typing (solid), then `.blinking-cursor { animation: blink 1s step-end infinite }` (opacity 1 for 0–50 %, 0 for 51–100 % → **500 ms on / 500 ms off**); the cursor is removed 2000 ms after typing ends.

### 1.6 Website hero / video
- Home page `packages/console/app/src/routes/index.tsx`: `<video autoplay playsinline loop muted preload="auto" poster=opencode-poster.png>` of `asset/lander/opencode-min.mp4` (10.4 MB). Other product screen recordings: `opencode-comparison-min.mp4` (16.9 MB), `desktop-tabs-landscape.mp4` (14.9 MB), `packages/app/src/assets/help/introducing-tabs.mp4`. All are **recordings of the product UI**, not brand animation → not downloaded (large, no reuse value for a pixel splash). Posters/stills downloaded (below). Videos can be fetched at `https://github.com/anomalyco/opencode/raw/dev/packages/console/app/src/asset/lander/opencode-min.mp4`.
- Also `artifacts/glm52-rise-video/out/*.mp4`: marketing-style renders unrelated to brand identity (UNVERIFIED content, not opened).
- No hero typing effect/shimmer found in the home route (UNVERIFIED for landing CSS; `Typewriter` exists in the shared UI library and is used by the app, not confirmed on the hero).
- Mascot/character: **none** (checked repo tree for mascot/character names; `/brand` route only serves logo files).

### 1.7 Downloaded to `assets/motion/`
| File | Source URL |
|---|---|
| `opencode-poster.png` (13.7 KB, hero video poster) | https://github.com/anomalyco/opencode/blob/dev/packages/console/app/src/asset/lander/opencode-poster.png |
| `screenshot-splash.png` (133 KB, product screenshot of the TUI start screen) | https://github.com/anomalyco/opencode/blob/dev/packages/console/app/src/asset/lander/screenshot-splash.png |

No GIF/Lottie/animated SVG exist to download. The rest of the "motion" is reproduced as data in this file.

### 1.8 Run-mode splash (static, but shows the mini logo)
`packages/opencode/src/cli/cmd/run/splash.ts`: entry banner = 3-row "go" mark (`go.right.slice(1)`) at left, then bold "OpenCode" and detail; exit banner shows `Session <title>` / `Continue opencode --mini -s <id>`. Static scrollback, no animation.

## 2. Logo pixel grids

### 2.1 Shading rules of the TUI block logo (`packages/tui/src/logo.ts`, `component/logo.tsx`, `cli/cmd/run/splash.ts`)
Each terminal cell is 1 column × 2 vertical half-pixels (top/bottom). Two colours per half-logo: `fg` = `theme.textMuted` `#808080` for "open" (left, normal weight), `theme.text` `#eeeeee` for "code" (right, bold); `shadow = tint(background, fg, 0.25)` where `tint = base + (overlay − base) × α` per channel, rounded (`packages/tui/src/theme/index.ts:346`). With bg `#0a0a0a`: **shadow_left = `#282828`**, **shadow_right = `#434343`** (computed, ±1 rounding).

| Char | top half | bottom half | Renderer |
|---|---|---|---|
| `█` | fg | fg | plain |
| `▀` | fg | (bg) | plain |
| `▄` | (bg) | fg | plain |
| `_` | shadow | shadow | space, bg=shadow |
| `^` | fg | shadow | `▀` fg=fg bg=shadow |
| `~` | shadow | (bg) | `▀` fg=shadow, no bg |
| `,` | (bg) | shadow | `▄` fg=shadow, no bg (in `marks` but unused by `logo`) |
| ` ` | bg | bg | — |

"open" and "code" are separated by 1 blank column (`gap={1}`).

### 2.2 Exact cell grid of the full "opencode" TUI logo (39 × 8 half-pixels)
Legend: `F` = fg (left `#808080`, right `#eeeeee`), `S` = shadow (left `#282828`, right `#434343`), `.` = background. Columns 0–18 = "open" (fg muted), col 19 = gap, columns 20–38 = "code" (fg text). Rows come in pairs (top/bottom half) per text line 0–3:

```
        0         1         2         3
        012345678901234567890123456789012345678
t0      .......................................
b0      .................................F.....     <- the "d" ascender ▄ at col 33
t1      FFFF.FFFF.FFFF.FFF..FFFF.FFFF.FFFF.FFFF
b1      F..F.F..F.F..F.F..F.F....F..F.F..F.F..F
t2      FSSF.FSSF.FFFF.FSSF.FSSS.FSSF.FSSF.FFFF
b2      FSSF.FSSF.FSSS.FSSF.FSSS.FSSF.FSSF.FSSS
t3      FFFF.FFFF.FFFF.FSSF.FFFF.FFFF.FFFF.FFFF
b3      .....F.................................     <- the "p" descender ▀ at col 5
```
(Row order top→bottom: t0,b0,t1,b1,t2,b2,t3,b3; pair tN/bN = text line N.) Letters: o=0–3, p=5–8, e=10–13, n=15–18, c=20–23, o=25–28, d=30–33, e=35–38. Minor artefact: "n" (cols 15–18) top-right corner is missing (`FFF.`), exactly as in the SVG wordmark.

Simplified 1-bit silhouette (F and S both lit, for very small panels): every `S` in the interior of o/p/e/n... is the darker *inside* of the letter (the "inner shadow" of each glyph = hollow counter), matching the SVG's `#4B4646` inner cells.

### 2.3 Official SVG mark and wordmark as pixel grids (lossless; colours from SVG)
Rasterised from `assets/opencode-logo-dark.svg` and `opencode-wordmark-dark.svg` by sampling cell centres (path coordinates are exact multiples of the unit).

**Mark** (240×300, unit 60 → 4 × 5 cells). `O` = `#F1ECEC` outer, `i` = `#4B4646` inner, `.` = transparent:
```
OOOO
O..O
OiiO
OiiO
OOOO
```
Light variant: swap colours per `opencode-logo-light.svg`.

**Wordmark** (641×115, unit 16.4286 → 39 × 7 cells). `B` = `#B7B1B1` ("open"), `C` = `#F1ECEC` ("code"), `A` = `#4B4646` inner:
```
.................................C.....
BBBB.BBBB.BBBB.BBB..CCCC.CCCC.CCCC.CCCC
B..B.B..B.B..B.B..B.C....C..C.C..C.C..C
BAAB.BAAB.BBBB.BAAB.CAAA.CAAC.CAAC.CCCC
BAAB.BAAB.BAAA.BAAB.CAAA.CAAC.CAAC.CAAA
BBBB.BBBB.BBBB.BAAB.CCCC.CCCC.CCCC.CCCC
.....B.................................
```
(In the SVG letters 5–8, "code", use `#F1ECEC`; all 8 letters are 4 cells wide, 5 tall, 1-cell gap, "p" has a 1-cell descender, "d" a 1-cell ascender.) Compare with §2.2: the SVG wordmark is a 1:1 cell version of the TUI logo where each TUI text row = 1 SVG row and shading `S` ≈ `A`. Use §2.3 as the canonical pixel art; §2.2 if the "muted vs bright" half-and-half look of the TUI is wanted.

Palette summary for the splash: bg `#000000`, wordmark open `#B7B1B1`, code `#F1ECEC`, inner `#4B4646`, accent `#fab283`, TUI greys `#808080/#eeeeee/#282828/#434343`, status `#7fd88f` / `#f5a742` / `#e06c75` (see 02 §2).

## 3. Motion inventory summary (durations / easing)
| Motion | Where | Timing |
|---|---|---|
| Knight Rider scanner (8 cells) | TUI prompt, run footer | 40 ms/frame, 54-frame loop (2.16 s), trail 6, α falloff ×0.65 |
| Braille spinner (10 frames) | TUI loading | 80 ms/frame, 0.8 s loop |
| 4×4 pulsing squares | web/desktop | random 1–2 s, delay 0–1.5 s, ease-in-out, opacity 0.15–0.35 / 0.4–1 |
| Text shimmer | web/desktop | 1200 ms linear loop, 45 ms per-char stagger, 220 ms ease-out swap |
| Typewriter | web/desktop | 30–80 ms/char (+ rare 80–250 ms pauses), 200 ms start delay |
| Cursor blink | web/desktop | `step-end` 1 s (500 ms on/off), removed after 2 s idle |
| Fade-up | web/desktop | 0.4 s ease-out, 5 px, 0.1 s stagger |
| Startup loading chip | TUI | shown after 500 ms, ≥3 s min display |

## 4. Splash concepts using only official material
Assume a Clawd-style stage: **60×60 grid, cell = min(W,H)/60** (8 px @480, 6 px @368, 4 px @240), black background, ≤16-colour palette per animation, intro → loop → outro, per-frame hold in ms. Wordmark = 39 cells wide → fits a 60 stage at 1×, with 10-cell margins each side; mark (4×5) can be scaled ×3–×4 (12×15 / 16×20 cells).

Palette (≤16): `0 bg #000000`, `1 #F1ECEC`, `2 #B7B1B1`, `3 #4B4646`, `4 #fab283` (accent), `5 #ffc09f`, `6..9` = peach trail alphas pre-blended on black (α 0.9/0.65/0.42/0.27/0.18 → `#e6a079 #a67254 #6c4a36 #452e22 #2d1e16`), `10 #808080`, `11 #282828`, `12 #7fd88f`, `13 #f5a742`, `14 #e06c75`.

1. **"Type-on wordmark + cursor"** (default idle/boot). Stage: wordmark (§2.3) centred at rows ~26–32, x 10–48. Intro: after 200 ms, letters appear one at a time (o,p,e,n,c,o,d,e) using typewriter timing: mostly 60 ms/char, with 150 ms holds after "n" and 250 ms once at random (match 5 %/10 % pauses; keep deterministic in firmware); a solid `│`-style cursor cell (1×5 cells, `#fab283`) sits after the last drawn letter. Loop: cursor blinks 500 ms on / 500 ms off (`step-end`). Outro (on screen change): cursor stays, letters fade to `#4B4646` in 2 steps of 100 ms then disappear. ~10 frames intro, 2 frames loop (hold 500 ms each).
2. **"Mark assembling"** (boot/pairing). The 4×5 mark scaled ×4 (16×20 cells) centred. Intro: the 14 outer cells (border ring: 14 = 4+4+2+2+... all `O` cells) pop in clockwise from the top-left in 40 ms steps (matches the TUI 40 ms interval), then the two inner rows (`i`, 2×2 blocks) fade in `#4B4646` (2 frames × 120 ms). Loop: inner block alternates `#4B4646` ↔ `#5A5858` (favicon inner colour) 800 ms — a soft "breathing"; optional single shine cell travelling the ring at 40 ms/cell (54-frame ping-pong from §1.1). Outro: reverse of intro.
3. **"Scanner under the mark"** (active / generating tokens). Mark (12×15 cells) top-centre; below it an **8-cell Knight Rider strip** exactly per §1.1: each block = 3×3 stage cells with 1-cell gap (8×3 + 7 = 31 cells wide), lit cell = peach `#fab283` head, trail colours 6–9, inactive cells = peach at 0.6×fade (~`#2d1e16`–`#6c4a36`). Loop = the 54 frames at 40 ms (≈ 2.16 s), including the 1.2 s resting dim tail; can be sped up ×2 (20 ms) for "high rate". Outro: strip fades out over 3 frames. Pixel budget: fits ≤16 colours; only ~30 unique frames if the 24 all-dim rest frames are collapsed into one 1 s hold (per-frame ms supports this).
4. **"Braille spinner + label"** (loading / waiting for BLE data). Mark small (8×10) with the braille spinner glyphs redrawn as 5×5-pixel dot patterns (10 frames × 80 ms, `#808080`) to its right, next to the text "Loading…" in the usual UI font (from the device font set, not pixel art). Loop = 800 ms. Purely official (`SPINNER_FRAMES`, `interval 80`). ⚠ the braille dot layouts (⠋ = dots 1,2,4,5? etc.) should be transcribed from Unicode; not encoded here (UNVERIFIED pixel mapping).
5. **Mood variants driven by usage rate / limit** (same base scene = concept 1 or 3, colour and tempo change; maps 1:1 onto the existing `usage_rate_group()` idea):
   - *Idle* (rate ≈ 0): wordmark static, cursor blink 500 ms (concept 1 loop), all accents `#808080/#B7B1B1`.
   - *Active* (rate > 0): mark + scanner (concept 3) at 40 ms/frame, accent `#fab283`.
   - *Busy* (high rate): scanner at 20 ms/frame, no rest tail (only frames 0–29), head colour `#ffc09f` bloom.
   - *Near limit* (>60 % / >85 % context or session): scanner colour switches head + trail to `#f5a742` (warning) then `#e06c75` (error); at >85 % the mark's inner cell `#4B4646` blinks to the error colour at 500 ms (blink-cursor timing), and the scanner is held in the "rest" (all-dim) state — i.e. a stopped scanner reads as "blocked".
   - *Reset/refill event*: replay concept 2 intro once (assembling mark) then return to idle.
   Colour/state thresholds mirror the ones already proposed in 02 §6 (`success`/`warn`).

Notes for implementation (not done here): all concepts stay inside "official material only" (logo mark, block wordmark, TUI spinner frames, palette). The wordmark grid is exactly 39 cells wide, so on the 240×240 board (cell 4 px) it takes 156 px and on 368-wide 234 px — comfortable margins everywhere.

## 5. Open / UNVERIFIED items
- Default agent colour used by the scanner (`local.agent.color`) — UNVERIFIED (likely theme `primary` `#fab283`).
- Whether opencode.ai hero uses Typewriter/Shimmer — UNVERIFIED (only the video hero was confirmed).
- Video contents of `artifacts/glm52-rise-video/*.mp4` — not opened.
- Braille-glyph dot layouts for concept 4 — not transcribed.
- Shadow colours are computed by hand from `tint` (±1 per channel from rounding).
- Brand/trademark reuse terms unchanged from 02 §5 (UNVERIFIED).
