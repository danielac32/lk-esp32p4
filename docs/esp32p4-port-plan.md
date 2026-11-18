# Plan de port/migración LK para ESP32-P4 (base: `xinu-p4-smp`)

Objetivo: usar **Little Kernel (LK)** como base funcional en **ESP32-P4 (RISC-V, rv32imafc)** con **SMP (2 harts)** y portar/adaptar el código ya desarrollado en `xinu-p4-smp` (drivers, red, UI/LVGL, audio, apps/ELF, etc.).

> Nota: LK y Xinu tienen modelos distintos (hilos/scheduler, sincronización, modelo de dispositivos, memoria, VFS/red, API de apps). La migración es principalmente **portar lógica/drivers y re-integrarlos a las APIs de LK**, no un reemplazo drop-in.

## Estado actual (bring-up HW, commit `86074c4`)
- [x] Imagen LK flasheable en `0x5000` y arranque bajo SBL existente (XIP flash `0x40005000`, datos en L2MEM).
- [x] UART0 polled TX/RX básico (`platform/esp32p4/debug.c`) y salida serie operativa.
- [x] `lk_main` avanza (heap, ctors, kernel init, arch/platform/target init) y llega a init de apps (`starting app: shell`).
- [ ] Consola/shell interactiva funcional (bloqueada; dependiente de hilos/timer/consola).
- [ ] Timer/IRQ/CLIC reales y scheduler/preempción operativos.
- [ ] SMP (2º hart).
- [ ] Drivers/periféricos/apps portados.

## Fase 1 — Estabilizar bring-up (kernel funcional mononúcleo)
**Meta:** boot limpio, consola serie interactiva, hilos y temporización funcionando.

- [ ] **P1.1 Revertir hacks de bring-up y restaurar rutas normales**
  - Quitar marcadores de depuración y la ejecución en línea de bootstrap/shell (`top/main.c`, `app/app.c`, `top/init.c`, `kernel/init.c`, instrumentación en `arch/riscv/*`).
  - Restaurar `lib/libc/printf.c`/`stdio.c` y `lib/heap/heap_wrapper.c` al comportamiento de LK (salida por consola/stdio de LK) una vez que la consola sea estable.
  - Criterio: arranque sin trazas de debug forzadas y con `dprintf/printf` normales.
- [ ] **P1.2 Interrupciones y CLIC (ESP32-P4)**
  - Implementar soporte CLIC: init, enable/disable, prioridad/threshold, vector hw y dispatch de IRQ externas; integrar con `arch/riscv` (reemplazar/ajustar supuestos CLINT/mtvec).
  - Implementar `riscv_platform_irq()` real y ack/EOI según CLIC; mapear IRQs (UART, timer, SDIO, I2S, GPIO, etc.).
  - Referencia Xinu: `system/platforms/esp32p4/clic.c`, `system/arch/riscv/trap.c`, `include/esp32p4.h`.
  - Criterio: IRQ externa de prueba (p.ej. UART RX) atendida correctamente.
- [ ] **P1.3 Timer del sistema**
  - Programar timer (mtime/mtimecmp o timer CLIC según HW) y entregar tick al scheduler de LK; exponer `current_time()`/delays coherentes.
  - Referencia Xinu: `system/platforms/esp32p4/riscvclk.c`, `include/esp32p4.h` (CLINT/mtime).
  - Criterio: `thread_sleep`, timeouts y preempción funcionan.
- [ ] **P1.4 Consola/shell interactiva por UART0**
  - Habilitar RX por IRQ (o polling fiable integrado) y entrada de consola; validar `app/shell` interactivo (prompt, `help`, `version`).
  - Asegurar init de pines/clock UART0 (GPIO37 TX / RX mux) como en Xinu.
  - Referencia Xinu: `device/uart-esp32p4/*`, `device/tty/*`.
  - Criterio: shell LK usable por `/dev/ttyACM0`.
- [ ] **P1.5 Excepciones/traps y diagnóstico**
  - Handler de traps/excepciones robusto (mcause/mepc/mtval/ra, backtrace si es posible), panic legible; validar FPU (rv32imafc, F-only) en contexto de hilos.
  - Referencia Xinu: `system/arch/riscv/trap.c`, `system/platforms/esp32p4/crashscreen.c` (concepto).

## Fase 2 — SMP (2 harts)
**Meta:** scheduler SMP de LK operativo en ambos núcleos del ESP32-P4.

- [ ] **P2.1 Arranque del 2º hart**
  - Secuencia de wake/entry del hart 1 (registros de arranque ESP32-P4), stack/percpu y salto a `secondary_cpu_entry` de LK.
  - Referencia Xinu: `system/platforms/esp32p4/core1smp.c`, `smpcore.c`, `loader/platforms/esp32p4/start.S`.
- [ ] **P2.2 IPIs y coherencia MP en CLIC/CLINT**
  - Implementar `arch_mp_send_ipi`/ack para ESP32-P4 (CLINT msip o mecanismo CLIC equivalente) y habilitar `WITH_SMP`.
  - Referencia LK: `arch/riscv/mp.c`; Xinu: `core1smp.c`.
- [ ] **P2.3 Afinidad, locks y validación SMP**
  - Configurar afinidad/pinning según necesidad de drivers; pruebas de concurrencia/estrés.
  - Criterio: ambos harts ejecutan hilos, IPIs/reschedule correctos, sin deadlocks/races evidentes.

## Fase 3 — Memoria y layout de plataforma
**Meta:** uso correcto de L2MEM + PSRAM y layout compatible con SBL.

- [ ] **P3.1 PSRAM como heap/arena adicional**
  - Detectar/mapear PSRAM y registrarla como arena de heap (novm/dlmalloc/physmem) para buffers grandes.
  - Referencia Xinu: `device/psram/*`, `system/stkget.c`, `include/memory.h`.
- [ ] **P3.2 Pilas y asignaciones grandes en PSRAM**
  - Política de stacks/allocs grandes (p.ej. LVGL, audio, buffers) en PSRAM con fallback.
- [ ] **P3.3 Revisar linker/start vs flujo SBL real**
  - Alinear secciones, símbolos y (si aplica) XIP/cache con el cargador SBL; garantizar `.data` LMA/VMA correcto para imágenes mayores.

## Fase 4 — Periféricos núcleo (HAL/driver LK)
**Meta:** buses y GPIO base para soportar el resto de drivers.

- [ ] **P4.1 GPIO e IO_MUX** (ref. `device/gpio`, `include/gpio.h`).
- [ ] **P4.2 I2C0 master** (ref. `device/i2c`).
- [ ] **P4.3 SPI master (para LCD)** (ref. `device/lcd-esp32p4`).
- [ ] **P4.4 UART driver completo** (IRQ, buffers) integrable con consola/dispositivos (ref. `device/uart-esp32p4`, `device/tty`).

## Fase 5 — Almacenamiento y sistema de ficheros
- [ ] **P5.1 SDMMC + tarjeta SD** (ref. `device/sdmmc-esp32p4`, `device/sdblock-esp32p4`).
- [ ] **P5.2 FAT**: integrar `fatfs`/capa existente a VFS/bio de LK o exponer API equivalente (ref. `device/fat`, `fatfs/`).

## Fase 6 — Pantalla, táctil y UI (LVGL)
- [ ] **P6.1 LCD ST7796 (SPI)** + flush/blit (ref. `device/lcd-esp32p4`).
- [ ] **P6.2 Táctil FT6336 (I2C)** con filtrado de eventos/coords (ref. `device/touch`, fixes de ghost touches).
- [ ] **P6.3 LVGL v8 integrado en LK** (display/touch indev, buffers en PSRAM, tick/handler en hilo/afinidad adecuados) (ref. `device/lvgl`, `system/lvgl_*` si aplica).
- [ ] **P6.4 UI/launcher**: portar/adaptar `gestos`, `launcher`, `appmgr` UI a LK+LVGL.

## Fase 7 — Audio
- [ ] **P7.1 I2S + códec (ES8311) + DMA** in/out (ref. `device/audio`).
- [ ] **P7.2 Rutas de reproducción/captura y control volumen/mic**; validar con apps (`musica`, `micdemo`).

## Fase 8 — Red y WiFi
- [ ] **P8.1 Transporte SDIO + esp-hosted (ESP32-C6)** (ref. `device/sdio-esp32p4`).
- [ ] **P8.2 Interfaz Ethernet/WiFi y pila de red**: integrar lwIP (LK suele integrarlo) o portar la pila usada, DHCP/ARP/IP/ICMP/UDP/TCP.
- [ ] **P8.3 Servicios**: httpd/servicios de Xinu sobre la nueva pila (ref. `shell/xsh_httpd.c`, `network/`).

## Fase 9 — Cargador de apps, runtime y paridad de apps
- [ ] **P9.1 Formato y carga de apps/ELF**: decidir compatibilidad con ELFs de Xinu (`elfload`, syscalls/exports) o ABI nueva; loader y aislamiento (ref. `system/elfload.c`, `system/exports.c`, `examples/p4-elfsdk`).
- [ ] **P9.2 Gestor de apps/procesos** (ref. `system/appmgr.c`).
- [ ] **P9.3 Portar apps existentes** (`examples/`, `apps/`) a la nueva base/APIs.
- [ ] **P9.4 Manual/recursos** servidos desde el nuevo sistema.

## Fase 10 — Integración, robustez y paridad
- [ ] **P10.1 Pruebas de humo/SMP/estrés** (equivalentes a `ktest`, `smp stress`).
- [ ] **P10.2 Gestión de fallos/panic** (pantalla/logs) y estabilidad.
- [ ] **P10.3 Rendimiento/contención** (afinidad hilos críticos, audio/SD/UI).
- [ ] **P10.4 Documentación** del port LK y mapeo de APIs Xinu→LK.

## Dependencias y orden recomendado
1. F1 (CLIC/timer/consola/hilos) → 2. F2 (SMP) → 3. F3 (PSRAM) → 4. F4/F5 (buses/SD/FAT) → 5. F6 (display/touch/LVGL) → 6. F7/F8 en paralelo según necesidad → 7. F9/F10.

## Criterios de “funcional” (mínimo usable)
- Boot estable + shell serie interactiva + timer/preempción + SMP 2 harts.
- Almacenamiento (SD/FAT) y pantalla+táctil+LVGL operativos.
- Al menos un subconjunto representativo de apps/drivers de Xinu ejecutando sobre LK.
