#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
# Keep host binaries out of build/: idf.py refuses a build dir it did not create.
OUT=build_host
mkdir -p "${OUT}"
CFLAGS=(-std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -I main)
cc "${CFLAGS[@]}" -o "${OUT}/test_govee_decode" test/test_govee_decode.c main/govee_decode.c -lm
cc "${CFLAGS[@]}" -o "${OUT}/test_history" test/test_history.c main/history.c
cc "${CFLAGS[@]}" -o "${OUT}/test_civil_time" test/test_civil_time.c main/civil_time.c
cc "${CFLAGS[@]}" -o "${OUT}/test_sdlog" test/test_sdlog.c main/sdlog_format.c
cc "${CFLAGS[@]}" -o "${OUT}/test_sensor_math" test/test_sensor_math.c main/sensor_math.c -lm
"./${OUT}/test_govee_decode"
"./${OUT}/test_history"
"./${OUT}/test_civil_time"
"./${OUT}/test_sdlog"
"./${OUT}/test_sensor_math"
