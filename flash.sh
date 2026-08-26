#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Zhanghan Wang

# Build the sketch and push it to the device in one step.
#
#   ./flash.sh                 build, then upload over the air to esp32.local
#   ./flash.sh /dev/ttyACM0    build, then upload over USB (needed for the first
#                              flash, since OTA only exists once it is running)
#
# Env overrides:
#   OTA_HOST   device address, hostname or IP (default esp32.local)
#   OTA_PASS   matches the "ota_pass" key in /sdcard/esp32.json (unset = none)
#   VERBOSE=1  full compiler command lines
#
# Every command is echoed and its output goes straight to your terminal --
# nothing here captures or filters it.
set -euo pipefail

FQBN="esp32:esp32:esp32s3:PSRAM=enabled,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB"
SKETCH_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SKETCH_DIR/build"
BIN="$BUILD_DIR/$(basename "$SKETCH_DIR").ino.bin"

run() { printf '+ %s\n' "$*" >&2; "$@"; }

run arduino-cli compile --fqbn "$FQBN" --build-path "$BUILD_DIR" \
    ${VERBOSE:+-v} "$SKETCH_DIR"

if [ $# -gt 0 ]; then
  run arduino-cli upload --fqbn "$FQBN" -p "$1" --input-dir "$BUILD_DIR" "$SKETCH_DIR"
  exit
fi

# espota.py is driven directly rather than through `arduino-cli upload --protocol
# network`: the CLI only accepts network ports its mDNS discovery already found,
# which fails outright ("port not found") whenever the device isn't advertising
# _arduino._tcp at that moment. espota just needs the address.
ESPOTA="$(ls -1 "$HOME"/.arduino15/packages/esp32/hardware/esp32/*/tools/espota.py | tail -1)"
set -- -r -i "${OTA_HOST:-esp32.local}" -p 3232 -f "$BIN"
[ -n "${OTA_PASS:-}" ] && set -- "$@" -a "$OTA_PASS"
run python3 "$ESPOTA" "$@"
