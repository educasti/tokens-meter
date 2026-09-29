# 02 — OpenCode brand identity

Source repo: https://github.com/anomalyco/opencode (formerly `sst/opencode`; GitHub redirects). Branch `dev`. All raw files fetched via the GitHub contents API on 2026-09-28. Site: https://opencode.ai.

## 1. Logo files

Downloaded to `design/opencode-screen/research/assets/`. Base URL for all: `https://github.com/anomalyco/opencode/blob/dev/<path>`.

| Local file | Source path in repo | Notes |
|---|---|---|
| `opencode-logo-dark.svg` | `packages/console/app/src/asset/brand/` | 240×300 mark: hollow square-ish "o" glyph. Fills `#F1ECEC` (outer) + `#4B4646` (inner). For dark backgrounds. |
| `opencode-logo-light.svg` | same | Light-background variant. |
| `opencode-logo-dark-square.svg` / `-light-square.svg` | same | Mark on square canvas. |
| `opencode-wordmark-dark.svg` / `-light.svg` | same | 641×115 wordmark, two-tone (`#B7B1B1` + `#4B4646` on dark). |
| `opencode-wordmark-simple-dark.svg` / `-light.svg` | same | Single-colour (white) wordmark, no inner shadow. |
| `logo-ornate-dark.svg` / `-light.svg` | `packages/console/app/src/asset/` | 234×42 wordmark used in the GitHub README. |
| `favicon-v3.svg` | `packages/app/public/` | 512×512 icon: bg `#131010`, glyph white + `#5A5858` inner. |
| `opencode-theme.json` | `packages/tui/src/theme/assets/opencode.json` | Default TUI theme (see §2). |

PNG variants and the official zip also exist upstream (not downloaded): `packages/console/app/public/opencode-brand-assets.zip` (19 KB), plus `preview-*.png` in the `brand/` folder. The zip is what the site's `/brand` page offers.

The mark is a pixel-grid glyph (a rectangular "o" with a darker inner cell offset toward the bottom), i.e. it is built from square blocks on a 60-unit grid. It converts well to a low-res bitmap: 4×5 cells of 60 units → drawable as a 20×25 or 12×15 pixel sprite.

### TUI ASCII logo (exact source, `packages/tui/src/logo.ts`)

```ts
export const logo = {
  left: ["                   ", "█▀▀█ █▀▀█ █▀▀█ █▀▀▄", "█__█ █__█ █^^^ █__█", "▀▀▀▀ █▀▀▀ ▀▀▀▀ ▀~~▀"],
  right: ["             ▄     ", "█▀▀▀ █▀▀█ █▀▀█ █▀▀█", "█___ █__█ █__█ █^^^", "▀▀▀▀ ▀▀▀▀ ▀▀▀▀ ▀▀▀▀"],
}
export const go = {
  left: ["    ", "█▀▀▀", "█_^█", "▀▀▀▀"],
  right: ["    ", "█▀▀█", "█__█", "▀▀▀▀"],
}
export const marks = "_^~,"
```

Rendered: "open" (left, muted colour) + "code" (right, bold, `text` colour). Marker chars are substituted at render time (`packages/tui/src/component/logo.tsx`): `_` = space with shadow bg, `^` = `▀` with shadow bg, `~` = `▀` in shadow fg, `,` = `▄` in shadow fg. `shadow = tint(background, fg, 0.25)` (25% blend of fg into bg). Plain rendering (markers → shadow shading omitted):

```
                                  ▄
█▀▀█ █▀▀█ █▀▀█ █▀▀▄ █▀▀▀ █▀▀█ █▀▀█ █▀▀█
█  █ █  █ █▀▀▀ █  █ █    █  █ █  █ █▀▀▀
▀▀▀▀ █▀▀▀ ▀▀▀▀ ▀  ▀ ▀▀▀▀ ▀▀▀▀ ▀▀▀▀ ▀▀▀▀
```

(Approximation: I replaced `_` with space and `^`/`~` with the closest glyph for a monochrome view; use the source above for fidelity. Also present in `packages/opencode/src/cli/logo.ts`.)

## 2. Colors

### Default TUI theme `opencode` (dark / light) — `packages/tui/src/theme/assets/opencode.json`
Default active theme is `"opencode"` (`packages/tui/src/context/theme.tsx`).

| Role | Dark | Light | Used for |
|---|---|---|---|
| background | `#0a0a0a` | `#ffffff` | main bg |
| backgroundPanel | `#141414` | `#fafafa` | panels/sidebar |
| backgroundElement | `#1e1e1e` | `#f5f5f5` | inputs, chips |
| borderSubtle | `#3c3c3c` | `#d4d4d4` | dividers |
| border | `#484848` | `#b8b8b8` | box borders |
| borderActive | `#606060` | `#a0a0a0` | focused border |
| text | `#eeeeee` | `#1a1a1a` | body |
| textMuted | `#808080` | `#8a8a8a` | secondary, token/cost lines |
| **primary** | **`#fab283`** (peach/orange) | `#3b7dd8` (blue) | links, function names, list bullets, brand accent |
| primary (step10, hover) | `#ffc09f` | `#2968c3` | — |
| secondary | `#5c9cf5` (blue) | `#7b5bb6` | — |
| accent | `#9d7cd8` (violet) | `#d68c27` | markdown headings, keywords |
| success | `#7fd88f` | `#3d9a57` | ok / strings |
| warning | `#f5a742` | `#d68c27` | warnings, numbers |
| error | `#e06c75` | `#d1383d` | errors |
| info | `#56b6c2` (cyan) | `#318795` | operators, list enumeration |
| yellow (syntax type) | `#e5c07b` | `#b0851f` | — |
| diffAdded / diffRemoved | `#4fd6be` / `#c53b53` | `#1e725c` / `#c53b53` | diffs |
| diffAddedBg / RemovedBg | `#20303b` / `#37222c` | `#d5e5d5` / `#f7d8db` | diffs |

Dark greys form a 12-step ramp: `#0a0a0a #141414 #1e1e1e #282828 #323232 #3c3c3c #484848 #606060 (…) #808080 #eeeeee`.

### Brand-asset colors (logos)
| Hex | Where |
|---|---|
| `#131010` | favicon background (warm near-black) |
| `#F1ECEC` | mark outer (dark variant) |
| `#B7B1B1` | wordmark/ornate outer glyph (dark variant) |
| `#4B4646` | inner cell of glyph (dark variant) |
| `#5A5858` | inner cell in favicon |
| `#FFFFFF` | favicon glyph, simple wordmark |

Warm greys (a slight red/brown cast), unlike the neutral TUI ramp.

### Website CSS tokens (`packages/console/app/src/style/token/color.css`)
Note: this is the console/billing app's generic token file, Apple-like and **not** the peach brand accent — treat as secondary. Dark mode: bg `#0c0c0e`, surface `#161618`, elevated `#1c1c1f`, text `#ffffff`, secondary `#c7c7cc`, muted `#a1a1a6`, disabled `#68686f`, accent `#007aff`, success `#30d158`, warning `#ff9f0a`, danger `#ff453a`, border `#38383a`, border-muted `#2c2c2e`. Light: bg `#ffffff`, surface `#f5f5f7`, text `#1d1d1f`, border `#d2d2d7`. The opencode.ai landing page's own CSS was **UNVERIFIED** (not inspected); the TUI theme + logos are the safest brand source.

## 3. Typography

| Context | Stack | Source |
|---|---|---|
| Website / console | `"Berkeley Mono", "IBM Plex Mono", ui-monospace, SFMono-Regular, Menlo, …, monospace`; `--font-sans` = same mono stack (everything is monospace) | `packages/console/app/src/style/token/font.css` |
| App / desktop UI (`packages/ui`) | mono: system `ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, …`; sans: system-ui. Bundled `Inter.ttf` and `JetBrainsMonoNerdFontMono-Regular.woff2` in `packages/ui/src/assets/fonts/` | `packages/ui/src/styles/theme.css` |
| Email templates | IBM Plex Mono 400/500/600/700, JetBrains Mono 400/500, Rubik | `packages/console/mail/.../static/` |
| TUI | User's terminal font (no font control) | — |

Sizes on the site: 11–13 px small, 15 px body, up to 128 px display. UI weights: 400 regular, 500 medium (`--font-weight-*`).

Licenses:
- **Berkeley Mono — commercial, not redistributable.** Do NOT embed. UNVERIFIED that opencode ships the font files (none found in the repo listing; site presumably licensed it).
- **IBM Plex Mono — SIL OFL 1.1**, redistributable; it is the site's own declared fallback, so it is the **closest faithful open alternative**.
- JetBrains Mono — OFL 1.1 (also acceptable). Inter — OFL 1.1.
- Nothing downloaded: no font from the repo is needed; for firmware, generate LVGL bitmap fonts from IBM Plex Mono (see `docs/fonts.md` in this repo for the lv_font_conv recipe).

## 4. Iconography and visual language

- **Icon set:** custom, in `packages/ui/src/assets/icons/` (file-type icons `file-types/*.svg` are a Material-style icon pack; the rest is a custom 20 px line icon set). Lucide use is **UNVERIFIED** (not confirmed). For the device, simple pixel glyphs are more on-brand than any icon set.
- **Motifs:** terminal-first; all-monospace type, even on the website; square corners (no rounded radii in the TUI, box borders in the `border` grey); block characters `█ ▀ ▄` for the logo with a 25% tinted "shadow" giving a 2-tone pseudo-3D pixel look; the mark itself is a pixel-block glyph. Restrained palette: greys plus one warm peach accent (`#fab283`); colour is reserved for semantic status.
- **Mascot:** none found.
- **Usage/cost patterns** (`packages/tui/src/feature-plugins/sidebar/context.tsx`): a "Context" section with a bold `text`-colour title, then three plain `textMuted` lines:
  ```
  Context
  12,345 tokens
  37% used
  $0.42 spent
  ```
  - tokens = last assistant message's input + output + reasoning + cache.read + cache.write, formatted with `toLocaleString()` (thousands separators).
  - percent = `round(tokens / model.limit.context * 100)`, `0` when the limit is unknown.
  - cost = `Intl.NumberFormat("en-US", currency USD)`, suffix "spent".
  - **No progress bar** in the TUI context block — numbers only. A bar would be a device-side addition; keep it a flat block bar in the accent colour.
  - Other sidebar sections follow the same "bold title + muted lines" pattern (MCP: "N active, M errors"; files: `+adds -dels`).

## 5. Brand usage / license

- Repo license: **MIT**, "Copyright (c) 2025 opencode" (`LICENSE`). MIT covers the repo contents; it does **not** contain any explicit statement about logo or trademark rights, and there is no separate trademark policy in the repo. Whether the brand assets carry additional restrictions is **UNVERIFIED**; MIT text technically grants use of the files, but trademark law is separate.
- A `/brand` page exists (`packages/console/app/src/routes/brand/`) offering the asset zip; source contains no guideline text (no "usage", "guideline", "trademark" strings found).
- Practical guidance for this personal fork: use the logo unmodified (recolouring only between the shipped dark/light variants), don't imply endorsement, and mention "OpenCode is a trademark of its owners" in ATTRIBUTION.md. Confirm on opencode.ai/brand before any redistribution (this fork has no license anyway; see the repo's `ATTRIBUTION.md`).

## 6. Design tokens for a 480×480 black AMOLED screen

| Token | Value | Notes |
|---|---|---|
| `bg` | `#000000` | True black for AMOLED (theme's `#0a0a0a` is imperceptible difference but wastes the pixel-off benefit) |
| `panel` | `#141414` | theme backgroundPanel; use sparingly for cards |
| `element` | `#1e1e1e` | bar tracks, chips |
| `border` | `#484848` | 1–2 px square, no radius |
| `border_subtle` | `#3c3c3c` | dividers |
| `accent` (primary) | `#fab283` | brand peach; bar fill, key numbers, logo highlight |
| `accent_hi` | `#ffc09f` | pressed / peak |
| `text` | `#eeeeee` | main values |
| `muted` | `#808080` | labels ("tokens", "spent", "% used") |
| `logo_inner` | `#4B4646` | logo inner cell |
| `logo_outer` | `#F1ECEC` | logo outer (mark on black) |
| `success` | `#7fd88f` | under ~60% context |
| `warn` | `#f5a742` | 60–85% |
| `error` | `#e06c75` | >85% / disconnected |
| `info` | `#56b6c2` | BLE/link status (optional) |
| `secondary` / `violet` | `#5c9cf5` / `#9d7cd8` | optional second series |

Fonts (generate as LVGL 9 bitmap fonts from **IBM Plex Mono**, OFL; Berkeley Mono is commercial, so do not use it):
- Hero number (tokens/percent): Plex Mono Medium **56 px**
- Cost / secondary value: Plex Mono Medium **34 px**
- Section title (bold "Context"): Plex Mono SemiBold **24 px**
- Labels / muted lines: Plex Mono Regular **18 px** (min **14 px** for footers)
- Compact boards (240×240): scale to 34 / 20 / 14 / 12.

Layout guidance: monospace everything, left-aligned text blocks mimicking the sidebar (`Title` / `12,345 tokens` / `37% used` / `$0.42 spent`), thousands separators, square-cornered block progress bar (accent fill on `element` track, colour shifts via success → warn → error), logo mark top-left drawn as a pixel sprite from the 60-unit grid (cell = 4 px at 480 → ~48×60 px) or the block-letter wordmark rendered in a monospace font using `█▀▄` (needs those glyphs included in the LVGL font subset).

Sources: raw files under https://github.com/anomalyco/opencode/tree/dev (paths above); https://opencode.ai; https://opencode.ai/theme.json (schema URL referenced by the theme, not fetched).
