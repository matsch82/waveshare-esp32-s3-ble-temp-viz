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

find_port() {
    find /dev -maxdepth 1 \( -name 'cu.usbserial*' -o -name 'cu.usbmodem*' -o -name 'ttyACM*' \) 2>/dev/null | head -n1 || true
}

if [ -z "${PORT}" ]; then
    PORT=$(find_port)
fi
if [ -z "${PORT}" ]; then
    # In the deep-sleep cycle the USB port only exists for 2-3 s per wake.
    echo "Waiting up to 70 s for the board to wake up ..."
    for _ in $(seq 1 1400); do
        PORT=$(find_port)
        [ -n "${PORT}" ] && break
        sleep 0.05
    done
fi
if [ -z "${PORT}" ]; then
    echo "No ESP32 USB port found. Hold BOOT and plug the board, or pass the port explicitly:"
    echo "  ./scripts/flash.sh /dev/cu.usbmodemXXXX"
    exit 1
fi

echo "Flashing to ${PORT} ..."
if [ -f build/flash_args ]; then
    # esptool directly: it catches a short wake window and puts the chip
    # into download mode before it goes back to sleep.
    (cd build && python -m esptool --chip esp32s3 -p "${PORT}" -b 460800 \
        --before default_reset --after hard_reset write_flash "@flash_args")
else
    idf.py -p "${PORT}" flash
fi
if [ "${MONITOR}" = "1" ]; then
    idf.py -p "${PORT}" monitor --no-reset
fi
