#!/bin/bash
# Flash solo kernel LK en ESP32-P4 @ 0x5000 (SBL/ULP ya presentes)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${PORT:-/dev/ttyACM0}"
BAUD="${BAUD:-460800}"
ESPTOOL="${ESPTOOL:-/home/daniel/esp-idf-py/bin/esptool}"
CHIP="esp32p4"
LK_BIN="${LK_BIN:-$ROOT/build-esp32p4-test/lk.bin}"

if [[ ! -f "$LK_BIN" ]]; then
  echo "Error: no se encontró LK en $LK_BIN (compila LK primero)" >&2
  exit 1
fi

echo "==> Flasheando solo LK @ 0x5000 (port=$PORT)"
"$ESPTOOL" --chip "$CHIP" -p "$PORT" -b "$BAUD" \
  --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x5000 "$LK_BIN"
echo "==> Flash LK OK"
