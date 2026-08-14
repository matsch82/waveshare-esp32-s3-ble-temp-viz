#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
IDF_DIR="${HOME}/esp/esp-idf"
if [ ! -f "${IDF_DIR}/export.sh" ]; then
    echo "ESP-IDF not found. Run ./scripts/setup.sh first."
    exit 1
fi
. "${IDF_DIR}/export.sh"

if [ ! -d build ] || [ ! -f build/sdkconfig ]; then
    idf.py set-target esp32s3
fi
idf.py build
