#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
cc -std=c99 -Wall -Wextra -I main -o build/test_govee_decode \
    test/test_govee_decode.c main/govee_decode.c -lm
./build/test_govee_decode
