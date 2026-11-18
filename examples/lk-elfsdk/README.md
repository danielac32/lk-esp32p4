# LK ELF SDK (ESP32-P4)

SDK mínimo para compilar aplicaciones ELF (ET_REL, `ld -r`) cargables por `elfload` de LK.

- Arquitectura: RV32IMAF (sin C), `ilp32f`, sin PIC, sin relax, sin small-data.
- Las apps se enlazan con `ld -r` y resuelven símbolos contra la tabla de exports del kernel LK (`elfexports`).
- Punto de entrada: `_start` (en `apps/lib/start.S`) llama a `main(argc, argv)` y luego a `exit()`.
- `argv[0]` es la ruta del ELF; el resto son argumentos pasados al comando `elf`.

Uso típico:
```
cd apps/hello
make
# copiar hello.elf a /sd/appslkx/ y ejecutar: elf /sd/appslkx/hello.elf
```

APIs disponibles: las exportadas por el kernel (`elfexports` en consola). Incluye libc básica, `fs_*`, hilos, `psram_*`, y gráficos/touch (`lib/gfx`, `font_draw_char`, `p4_touch_*`, `fbcon_p4_*`) para apps con UI nativa de LK.
