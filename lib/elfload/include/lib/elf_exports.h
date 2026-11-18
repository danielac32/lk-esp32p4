#pragma once

struct lk_elf_export {
    const char *name;
    const void *addr;
};

/* Resuelve un simbolo por nombre en la tabla de exports del kernel.
 * Devuelve NULL si no existe. */
void *elf_export_resolve(const char *name);
unsigned int elf_export_count(void);
const char *elf_export_name(unsigned int i);
