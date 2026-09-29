# Recompiling fonts

The `firmware/src/font_*.c` files are pre-compiled LVGL bitmap fonts.

```bash
npm install -g lv_font_conv
```

Generate each one (one at a time — `lv_font_conv` doesn't like loop-driven
invocations) with `--no-compress` (required for LVGL 9):

```bash
# Tiempos Text (titles, 56px)
lv_font_conv --font assets/TiemposText-400-Regular.otf -r 0x20-0x7E \
  --size 56 --format lvgl --bpp 4 --no-compress \
  -o firmware/src/font_tiempos_56.c --lv-include "lvgl.h"

# Styrene B (large numbers 48, panel labels 28, small text 24, minimal 20)
for size in 48 28 24 20; do
  lv_font_conv --font assets/StyreneB-Regular.otf -r 0x20-0x7E \
    --size $size --format lvgl --bpp 4 --no-compress \
    -o firmware/src/font_styrene_${size}.c --lv-include "lvgl.h"
done

# DejaVu Sans Mono (32px, with spinner Unicode chars)
lv_font_conv --font assets/DejaVuSansMono.ttf \
  -r 0x20-0x7E,0xB7,0x2026,0x2722,0x2733,0x2736,0x273B,0x273D \
  --size 32 --format lvgl --bpp 4 --no-compress \
  -o firmware/src/font_mono_32.c --lv-include "lvgl.h"
```

**Important:** `lv_font_conv` v1.5.3 outputs LVGL 8 format. Each generated
file must be patched for LVGL 9 compatibility:

1. Remove `#if LVGL_VERSION_MAJOR >= 8` guards around `font_dsc` and the font struct
2. Remove the `.cache` field from `font_dsc`
3. Add `.release_glyph = NULL`, `.kerning = 0`, `.static_bitmap = 0` to the font struct
4. Add `.fallback = NULL`, `.user_data = NULL` to the font struct

Without these patches, fonts compile but render as invisible.

## IBM Plex Mono (OpenCode screens)

`font_plex_{48,40,24,18,16,12}.c` are the type family of the OpenCode
screens: 48/40/24 from **Medium** (big numbers, headings), 18/16/12 from
**Regular** (labels, values, status lines). Sources are the complete TTFs from
the IBM Plex repo, vendored under `assets/fonts/` next to the SIL OFL 1.1
license (`assets/fonts/OFL.txt`):

```bash
curl -sSL -o assets/fonts/IBMPlexMono-Regular.ttf \
  https://raw.githubusercontent.com/IBM/plex/master/packages/plex-mono/fonts/complete/ttf/IBMPlexMono-Regular.ttf
curl -sSL -o assets/fonts/IBMPlexMono-Medium.ttf \
  https://raw.githubusercontent.com/IBM/plex/master/packages/plex-mono/fonts/complete/ttf/IBMPlexMono-Medium.ttf
curl -sSL -o assets/fonts/OFL.txt \
  https://raw.githubusercontent.com/IBM/plex/master/LICENSE.txt
```

Generate them with the same flags as the fonts above, then apply the LVGL 9
patch (the four steps at the top of this file) to each one:

```bash
for size in 48 40 24; do
  lv_font_conv --font assets/fonts/IBMPlexMono-Medium.ttf -r 0x20-0x7E,0xB7,0x2026 \
    --size $size --format lvgl --bpp 4 --no-compress \
    -o firmware/src/font_plex_${size}.c --lv-include "lvgl.h"
done

for size in 18 16 12; do
  lv_font_conv --font assets/fonts/IBMPlexMono-Regular.ttf -r 0x20-0x7E,0xB7,0x2026 \
    --size $size --format lvgl --bpp 4 --no-compress \
    -o firmware/src/font_plex_${size}.c --lv-include "lvgl.h"
done
```

The glyph range is `0x20-0x7E` (ASCII) plus `·` (U+00B7) and `…` (U+2026).
**U+25CB `○` is not in IBM Plex Mono** — `lv_font_conv` rejects the range with
`Font "assets/fonts/IBMPlexMono-Regular.ttf" doesn't have any characters
included in range 0x25cb-0x25cb`, so it is not in the generated fonts and the
UI must fall back to a lowercase `o` wherever a ring glyph was planned.

Use them from C with the usual declaration:

```c
LV_FONT_DECLARE(font_plex_24);
```
