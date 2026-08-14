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
if [ -z "${PORT}" ]; then
    PORT=$(find /dev -maxdepth 1 -name 'cu.usbserial*' -o -name 'cu.usbmodem*' -o -name 'ttyACM*' 2>/dev/null | head -n1 || true)
fi
if [ -z "${PORT}" ]; then
    echo "No serial port found. Pass it explicitly: ./scripts/monitor.sh /dev/cu.usbmodemXXXX"
    exit 1
fi

idf.py -p "${PORT}" monitor
