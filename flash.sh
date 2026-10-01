#!/bin/bash
# Build and flash Clawdmeter firmware on Linux.
# Usage:
#   ./flash.sh [board] [port]
#
# [board] is the PlatformIO env name (default: waveshare_amoled_216).
# [port]  is the USB serial port (default: /dev/ttyACM0).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BOARD="${1:-waveshare_amoled_216}"
PORT="${2:-/dev/ttyACM0}"

echo "=== Flashing Clawdmeter ==="
echo "Board: $BOARD"
echo "Port:  $PORT"
echo ""

cd "$SCRIPT_DIR/firmware"
~/.platformio/penv/bin/pio run -e "$BOARD" -t upload --upload-port "$PORT"

echo ""
echo "=== Done! ==="
