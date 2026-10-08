#!/usr/bin/env bash
# Regenerate every firmware/src/font_*.c with the accented glyphs the Spanish UI
# needs, keeping the repo's LVGL 9 hand-patch (see tools/patch_lvgl_font.py).
#
# The committed fonts only cover 0x20-0x7E, so Spanish letters (ñ, á, é, í, ó,
# ú, ü) and the inverted marks (¿ ¡) would render as blank boxes. This adds
# exactly the 16 codepoints the Spanish UI uses — no more — so the flash cost
# stays small.
#
# Usage:
#   tools/regen_fonts_spanish.sh [--old-range]   # --old-range: verify mode,
#                                                # regenerate with the previous
#                                                # 0x20-0x7E range to diff
#                                                # against the committed files
set -euo pipefail
cd "$(dirname "$0")/.."

FONT_CONV="npx --yes lv_font_conv@1.5.3"

# 0x20-0x7E + the accented glyphs the Spanish strings actually use:
#   á é í ó ú ü ñ   Á É Í Ó Ú Ü Ñ   ¡ ¿
ACCENTS="0xA1,0xBF,0xC1,0xC9,0xCD,0xD1,0xD3,0xDA,0xDC,0xE1,0xE9,0xED,0xF1,0xF3,0xFA,0xFC"
PLEX_EXTRA="0xB7,0x2026"                                  # · …
MONO_EXTRA="0xB7,0x2026,0x2722,0x2733,0x2736,0x273B,0x273D"  # · … + spinner symbols

OLD_RANGE=0
[ "${1:-}" = "--old-range" ] && OLD_RANGE=1

gen() {  # gen <source.ttf> <symbol> <size> [extra range]
    local src="$1" symbol="$2" size="$3" extra="${4:-}"
    local range="0x20-0x7E"
    [ "$OLD_RANGE" -eq 0 ] && range="$range,$ACCENTS"
    [ -n "$extra" ] && range="$range,$extra"
    local tmp
    tmp="$(mktemp /tmp/fontgen-XXXXXX.c)"
    $FONT_CONV --font "$src" --range "$range" --size "$size" --format lvgl \
        --bpp 4 --no-compress -o "$tmp" --lv-include "lvgl.h" >/dev/null
    python3 tools/patch_lvgl_font.py "$tmp" "firmware/src/${symbol}.c" "$symbol"
    rm -f "$tmp"
    echo "  ${symbol}.c  size=${size}  range=${range}"
}

echo "Regenerating fonts (old-range=$OLD_RANGE)..."
gen assets/TiemposText-400-Regular.otf          font_tiempos_56 56
gen assets/TiemposText-400-Regular.otf          font_tiempos_34 34
gen assets/StyreneB-Regular.otf                 font_styrene_48 48
gen assets/StyreneB-Regular.otf                 font_styrene_28 28
gen assets/StyreneB-Regular.otf                 font_styrene_24 24
gen assets/StyreneB-Regular.otf                 font_styrene_20 20
gen assets/StyreneB-Regular.otf                 font_styrene_16 16
gen assets/StyreneB-Regular.otf                 font_styrene_14 14
gen assets/StyreneB-Regular.otf                 font_styrene_12 12
gen assets/fonts/IBMPlexMono-Medium.ttf         font_plex_48 48 "$PLEX_EXTRA"
gen assets/fonts/IBMPlexMono-Medium.ttf         font_plex_40 40 "$PLEX_EXTRA"
gen assets/fonts/IBMPlexMono-Medium.ttf         font_plex_24 24 "$PLEX_EXTRA"
gen assets/fonts/IBMPlexMono-Regular.ttf        font_plex_18 18 "$PLEX_EXTRA"
gen assets/fonts/IBMPlexMono-Regular.ttf        font_plex_16 16 "$PLEX_EXTRA"
gen assets/fonts/IBMPlexMono-Regular.ttf        font_plex_12 12 "$PLEX_EXTRA"
gen assets/DejaVuSansMono.ttf                   font_mono_32 32 "$MONO_EXTRA"
gen assets/DejaVuSansMono.ttf                   font_mono_18 18 "$MONO_EXTRA"
echo "Done."
