#!/usr/bin/env bash
# QA for the Spanish translation: glyph coverage, label widths and a build.
#
# The fonts are bitmaps with a fixed glyph set, so a translated string can
# either render as a blank box or overflow its label without failing to compile.
# This checks both, then compiles the firmware.
#
# Usage: tools/verify_spanish.sh [--no-build]
set -euo pipefail
cd "$(dirname "$0")/.."

SRC="firmware/src/ui.cpp firmware/src/ui_opencode.cpp firmware/src/ui_portfolio.cpp \
     firmware/src/splash.cpp firmware/src/oc_splash.cpp firmware/src/ota.cpp \
     firmware/src/ota_pull.cpp firmware/src/portal.cpp"

echo "== 1/3 juego de caracteres (solo ASCII + áéíóúüñÁÉÍÓÚÜÑ¡¿ · …)"
python3 tools/check_label_widths.py --charset $SRC

echo
echo "== 2/3 ancho de etiquetas"
python3 tools/check_label_widths.py --audit $SRC

echo
echo "== 3/3 build del firmware"
if [ "${1:-}" = "--no-build" ]; then
    echo "  (omitido)"
else
    ~/.platformio/penv/bin/pio run -d firmware -e waveshare_amoled_216
fi
