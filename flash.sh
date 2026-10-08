#!/bin/bash
# Compila y flashea el firmware de Clawdmeter en Linux.
# Uso:
#   ./flash.sh [board] [port]
#
# [board] es el nombre del entorno de PlatformIO (por omisión: waveshare_amoled_216).
# [port]  es el puerto serie USB (por omisión: /dev/ttyACM0).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BOARD="${1:-waveshare_amoled_216}"
PORT="${2:-/dev/ttyACM0}"

echo "=== Flasheando Clawdmeter ==="
echo "Placa: $BOARD"
echo "Puerto:  $PORT"
echo ""

cd "$SCRIPT_DIR/firmware"
~/.platformio/penv/bin/pio run -e "$BOARD" -t upload --upload-port "$PORT"

echo ""
echo "=== ¡Listo! ==="
