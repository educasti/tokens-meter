# Asset tools

## Splash animations

```bash
node convert_official_clawd.js
node convert_official_clawd.js --verify /tmp/verify   # + per-animation PNGs
```

Converts the official Anthropic Clawd animations archived in
`research/clawd-official/` (GIFs decoded via ImageMagick, the Laptop and
Soccer Lottie exports read directly) into a single
`firmware/src/splash_animations.h`:

- frames as bounding-box crops on the shared 55×37 art stage, one byte per
  cell into a per-animation ≤16-color RGB565 palette (index 0 = background)
- per-frame hold in ms, with consecutive duplicate frames collapsed
- a detected loop region per animation (gait cycles, scene middles) that the
  engine can hold or release for walk-to-target and timed scenes
- the eyes — transparent holes in the source GIFs — inked as `#141413`
- contrast recolors (trumpet notes → ivory, magnifier fedora → gray) and the
  sailing-loop cross-match that defines the sailing scene's loop window

See `research/clawd-official/CLAUDE.md` for asset provenance and the format
details, and `--in` / `--out` to override paths. Rebuild firmware after
running.

## Icons

```bash
node png_to_lvgl.js input.png symbol_name [W_MACRO] [H_MACRO] [--tint=RRGGBB | --no-tint]
```

Converts an alpha PNG to an LVGL RGB565A8 C array. Default tint is white —
Lucide PNGs ship black-on-transparent and would render invisible without it.
Paste the output into `firmware/src/icons.h`.

## OpenCode logo

```bash
node gen_oc_logo.js [--out FILE]
```

Generates `firmware/src/oc_logo.h` — the OpenCode mark and wordmark as LVGL
image descriptors. Unlike the icon converter, no SVG or PNG input is needed: the
art lives in the script as two small pixel grids (from
`design/opencode-screen/IMPL.md`, themselves redrawn from the official SVGs in
[anomalyco/opencode](https://github.com/anomalyco/opencode), MIT), and each cell
is expanded to a square block. That keeps every size pixel-crisp instead of
resampled.

Emits six descriptors at the three layout breakpoints — `oc_mark_{l,m,s}` at
10/8/4 px per cell (40×50, 32×40, 16×20) and `oc_wordmark_{l,m,s}` at 5/4/3
(195×35, 156×28, 117×21) — as `LV_COLOR_FORMAT_RGB565` on an opaque black
background, with `*_W` / `*_H` macros. They are `static const` in a header,
like `logo.h`. The file is generated: edit the grids in the script, not the
output. Rebuild the firmware afterwards.

Provenance and trademark note: `ATTRIBUTION.md`. The type that goes with these
screens, IBM Plex Mono, is generated separately with `lv_font_conv` — see
`docs/fonts.md`.
