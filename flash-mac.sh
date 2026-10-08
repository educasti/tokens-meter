#!/bin/bash
# Compila y flashea el firmware de Clawdmeter en macOS.
# Uso:
#   ./flash-mac.sh [board] [port]
#
# [board] es el nombre del entorno de PlatformIO (por omisión: waveshare_amoled_216).
# [port]  es el puerto serie USB (por omisión: detección automática /dev/cu.usbmodem*).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BOARD="${1:-waveshare_amoled_216}"
PORT="$2"

if [ -z "$PORT" ]; then
    PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
    if [ -z "$PORT" ]; then
        echo "Error: no se encontró ningún dispositivo /dev/cu.usbmodem*. Conéctalo por USB-C."
        exit 1
    fi
fi

if ! command -v pio >/dev/null; then
    echo "Error: no se encontró 'pio'. Instálalo con:"
    echo "  brew install platformio"
    exit 1
fi

echo "=== Flasheando Clawdmeter ==="
echo "Placa: $BOARD"
echo "Puerto:  $PORT"
echo ""

cd "$SCRIPT_DIR/firmware"
pio run -e "$BOARD" -t upload --upload-port "$PORT"

echo ""
echo "=== Listo ==="
echo "Monitor serie con: pio device monitor -p $PORT -b 115200"
