#pragma once

#include <stddef.h>

#if !NO_MALLINFO
#include <lib/dlmalloc.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Inicializa el heap de PSRAM (mspace dlmalloc) sobre la región segura
 * de PSRAM (0x48000000..0x49FF8000). Idempotente. */
void psram_heap_init(void);

void *psram_malloc(size_t size);
void *psram_calloc(size_t n, size_t size);
void *psram_realloc(void *ptr, size_t size);
void *psram_memalign(size_t alignment, size_t size);
void  psram_free(void *ptr);

#if !NO_MALLINFO
struct mallinfo psram_mallinfo(void);
#endif

#ifdef __cplusplus
}
#endif
