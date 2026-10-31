#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
# Keep host binaries out of build/: idf.py refuses a build dir it did not create.
OUT=build_host
mkdir -p "${OUT}"
cc -std=c99 -Wall -Wextra -I main -o "${OUT}/test_govee_decode" \
    test/test_govee_decode.c main/govee_decode.c -lm
cc -std=c99 -Wall -Wextra -I main -o "${OUT}/test_history" \
    test/test_history.c main/history.c
"./${OUT}/test_govee_decode"
"./${OUT}/test_history"
