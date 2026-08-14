#!/usr/bin/env bash
set -euo pipefail
# Install ESP-IDF v5.5.2 on macOS if it is not present.
IDF_DIR="${HOME}/esp/esp-idf"
IDF_VERSION="v5.5.2"

if [ -d "${IDF_DIR}" ] && [ -f "${IDF_DIR}/export.sh" ]; then
    echo "ESP-IDF already installed at ${IDF_DIR}"
    exit 0
fi

echo "Installing ESP-IDF ${IDF_VERSION} into ${IDF_DIR} ..."
mkdir -p "${HOME}/esp"

git clone -b "${IDF_VERSION}" --recursive https://github.com/espressif/esp-idf.git "${IDF_DIR}"

# macOS dependencies
if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 is required. Please install it (e.g. brew install python3)."
    exit 1
fi

"${IDF_DIR}/install.sh" esp32s3

echo "ESP-IDF ${IDF_VERSION} installed. Run 'source ${IDF_DIR}/export.sh' before building."
