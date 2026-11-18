# machine/ (ESP32-P4) — integrado en LK

Copiado desde `xinu-p4-smp/machine`. Contiene las imágenes de arranque que no forman parte del binario del kernel LK:

- **SBL** (`sbl/`, imagen `build/sbl.bin`): bootloader de 2ª etapa en HP TCM. Inicializa WDT, clocks, **PSRAM**, MMU y arranca el ULP; salta al kernel en `__kernel_entry_point` (XIP `0x40005000`, flash offset `0x5000`). Se genera con `esptool elf2image` (lleva cabecera). Depende de `sbl/libpsram.a` (precompilada).
- **ULP** (`ulp/`, imagen `build/ulp.bin`): firmware del coprocesador LP (RV32IMC), binario crudo flasheado en `0x400000`.

LK se carga por el SBL ya presente en placa (cadena de arranque Xinu) en `0x5000`. Este directorio permite reconstruir/modificar SBL y ULP en el futuro.

## Compilar SBL/ULP

Requiere `riscv32-unknown-elf-*` en PATH y `esptool` (o `ESPTOOL=...`):

```sh
make -C machine          # genera machine/build/sbl.bin y machine/build/ulp.bin
make -C machine sbl      # solo SBL
make -C machine ulp      # solo ULP
make -C machine clean
```

## Flasheo (ESP32-P4, 16 MB)

Desde la raíz de LK:

```sh
# Solo kernel LK @ 0x5000 (SBL/ULP ya presentes)
scripts/flash-esp32p4-lk.sh
# o: PORT=/dev/ttyACM0 LK_BIN=build-esp32p4-test/lk.bin scripts/flash-esp32p4-lk.sh

# Todo: SBL @ 0x2000, LK @ 0x5000, ULP @ 0x400000
scripts/flash-esp32p4-all.sh
# Recompilar SBL/ULP antes: BUILD_MACHINE=1 scripts/flash-esp32p4-all.sh
```

Variables: `PORT` (def `/dev/ttyACM0`), `BAUD` (460800), `ESPTOOL` (def `/home/daniel/esp-idf-py/bin/esptool`), `LK_BIN` (def `build-esp32p4-test/lk.bin`).

## Mapa de flash (referencia Xinu)

- `0x00002000` SBL (`sbl.bin`)
- `0x00005000` Kernel LK (`lk.bin`)
- `0x00400000` ULP (`ulp.bin`)
