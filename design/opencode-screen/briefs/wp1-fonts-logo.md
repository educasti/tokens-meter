# WP1 — IBM Plex Mono LVGL fonts + OpenCode logo images

Repo: /Users/educasti/Projects/Personal/tokens-meter (branch feat/opencode-screens). Read first: `design/opencode-screen/IMPL.md` (section WP1 + repo rules), `docs/fonts.md` (the exact lv_font_conv recipe and the MANDATORY LVGL 9 patch), and one existing font file (e.g. `firmware/src/font_styrene_24.c`) to copy its patched structure exactly.

Edit/create ONLY: `assets/fonts/IBMPlexMono-Regular.ttf`, `assets/fonts/IBMPlexMono-Medium.ttf`, `assets/fonts/OFL.txt`, `firmware/src/font_plex_{48,40,24,18,16,12}.c`, `tools/gen_oc_logo.js`, `firmware/src/oc_logo.h`, and append a "IBM Plex Mono (OpenCode screens)" section to `docs/fonts.md`.

Steps
1. Download IBM Plex Mono TTFs (Regular, Medium) + the OFL license from the official IBM/plex GitHub releases or raw repo (e.g. https://github.com/IBM/plex — packages/plex-mono/fonts/complete/ttf/). Verify they are real TTF files (`file` command).
2. Generate with `npx lv_font_conv` (4 bpp, format lvgl, `--no-compress`, same flags as docs/fonts.md): 48/40/24 from Medium, 18/16/12 from Regular. Range `0x20-0x7E,0xB7,0x2026` plus `0x25CB` only if the font contains it (check with a quick python fontTools or lv_font_conv error; report the result). Symbol names `font_plex_48` … `font_plex_12`.
3. Apply the LVGL 9 patch from docs/fonts.md to every generated file (remove version guards, drop `.cache`, add `.release_glyph`, `.kerning`, `.static_bitmap`, `.fallback`, `.user_data` — mirror an existing font file exactly).
4. Write `tools/gen_oc_logo.js` (plain Node, no deps) that turns the two pixel grids in IMPL.md into `firmware/src/oc_logo.h`: RGB565 little-endian byte arrays + `static const lv_image_dsc_t` for `oc_mark_l/m/s` (cells 10/8/4) and `oc_wordmark_l/m/s` (cells 5/4/3), black background, `LV_COLOR_FORMAT_RGB565`, `LV_IMAGE_HEADER_MAGIC`, stride = w*2 — mirror how `firmware/src/logo.h` declares its descriptors. Run it to produce the header. Add a header comment with provenance (OpenCode logo, MIT, github.com/anomalyco/opencode).
5. Verify it compiles: `pio run -d firmware -e sim` (fonts are compiled even if unused; must build clean). Report the per-font `.c` sizes.

When done reply exactly: `DONE WP1` + one line summary (glyph ○ present? yes/no).
