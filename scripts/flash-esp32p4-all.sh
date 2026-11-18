#!/bin/bash
# Flash completo ESP32-P4: SBL @ 0x2000, LK @ 0x5000, ULP @ 0x400000
# Construye machine (SBL/ULP) si es necesario. LK debe estar compilado.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${PORT:-/dev/ttyACM0}"
BAUD="${BAUD:-460800}"
ESPTOOL="${ESPTOOL:-/home/daniel/esp-idf-py/bin/esptool}"
CHIP="esp32p4"
LK_BIN="${LK_BIN:-$ROOT/build-esp32p4-test/lk.bin}"
SBL_BIN="$ROOT/machine/build/sbl.bin"
ULP_BIN="$ROOT/machine/build/ulp.bin"

# Construir SBL/ULP si faltan o si se pide
if [[ "${BUILD_MACHINE:-0}" == "1" ]] || [[ ! -f "$SBL_BIN" ]] || [[ ! -f "$ULP_BIN" ]]; then
  echo "==> Construyendo machine (SBL/ULP)..."
  make -C "$ROOT/machine"
fi

if [[ ! -f "$LK_BIN" ]]; then
  echo "Error: no se encontró LK en $LK_BIN (compila LK primero)" >&2
  exit 1
fi

echo "==> Flasheando TODO: SBL@0x2000 LK@0x5000 ULP@0x400000 (port=$PORT)"
"$ESPTOOL" --chip "$CHIP" -p "$PORT" -b "$BAUD" \
  --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x2000 "$SBL_BIN" 0x5000 "$LK_BIN" 0x400000 "$ULP_BIN"
echo "==> Flash completo OK"
