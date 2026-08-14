#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
IDF_DIR="${HOME}/esp/esp-idf"
if [ ! -f "${IDF_DIR}/export.sh" ]; then
    echo "ESP-IDF not found. Run ./scripts/setup.sh first."
    exit 1
fi
. "${IDF_DIR}/export.sh"

PORT="${1:-}"
MONITOR=0
if [ "${PORT}" = "monitor" ]; then
    MONITOR=1
    PORT=""
fi
if [ "${2:-}" = "monitor" ]; then
    MONITOR=1
fi
if [ -z "${PORT}" ]; then
    PORT=$(find /dev -maxdepth 1 -name 'cu.usbserial*' -o -name 'cu.usbmodem*' -o -name 'ttyACM*' 2>/dev/null | head -n1 || true)
fi
if [ -z "${PORT}" ]; then
    echo "No ESP32 USB port found. Hold BOOT and plug the board, or pass the port explicitly:"
    echo "  ./scripts/flash.sh /dev/cu.usbmodemXXXX"
    exit 1
fi

echo "Flashing to ${PORT} ..."
if [ "${MONITOR}" = "1" ]; then
    idf.py -p "${PORT}" flash monitor
else
    idf.py -p "${PORT}" flash
fi
