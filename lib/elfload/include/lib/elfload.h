/*
 * Cargador de apps ELF (ET_REL RV32, ld -r) para LK/ESP32-P4.
 * Adaptado del elfload de Xinu: carga desde /sd (lib/fs), coloca en PSRAM,
 * resuelve simbolos contra la tabla de exports del kernel, aplica relocaciones
 * RISC-V y ejecuta _start(argc,argv) en el hilo llamador.
 *
 * Modelo: el app corre en el hilo que llama a elf_run(). Si main() retorna,
 * elf_run() libera la imagen y devuelve el codigo. Si el app llama a exit()
 * (export del kernel), se libera la imagen y se termina el hilo.
 */
#pragma once

#include <stdint.h>

#define ELF_MAX_SECC    256
#define ELF_MAX_SIMB    2048
#define ELF_MAX_NOMBRE  64
#define ELF_MAX_ARGS    32

/* Carga y ejecuta un ELF ET_REL desde ruta (FS de LK, ej. /sd/appslkx/app.elf).
 * argc/argv son los argumentos adicionales; argv[0] del app sera ruta.
 * Retorna el codigo de retorno del app si main() retorna, o negativo en error. */
int elf_run(const char *ruta, int argc, char *argv[]);

/* Nombre de la imagen ELF en ejecucion en este hilo (o NULL). */
const char *elf_current_name(void);
