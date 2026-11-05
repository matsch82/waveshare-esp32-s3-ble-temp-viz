#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
IDF_DIR="${HOME}/esp/esp-idf"
if [ ! -f "${IDF_DIR}/export.sh" ]; then
    echo "ESP-IDF not found. Run ./scripts/setup.sh first."
    exit 1
fi
. "${IDF_DIR}/export.sh"

find_port() {
    find /dev -maxdepth 1 \( -name 'cu.usbserial*' -o -name 'cu.usbmodem*' -o -name 'ttyACM*' \) 2>/dev/null | head -n1 || true
}

PORT="${1:-}"
if [ -z "${PORT}" ]; then
    PORT=$(find_port)
fi
if [ -z "${PORT}" ]; then
    # In the deep-sleep cycle the USB port only exists while the board is awake.
    echo "Waiting up to 310 s (one wake interval) for the board to wake up ..."
    for _ in $(seq 1 6200); do
        PORT=$(find_port)
        [ -n "${PORT}" ] && break
        sleep 0.05
    done
fi
if [ -z "${PORT}" ]; then
    echo "No serial port found. Pass it explicitly: ./scripts/monitor.sh /dev/cu.usbmodemXXXX"
    exit 1
fi

# --no-reset: a reset would restart the board into the setup window. The
# monitor reconnects on its own each time the port comes back after a wake.
idf.py -p "${PORT}" monitor --no-reset
