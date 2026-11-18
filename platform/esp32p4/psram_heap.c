/*
 * Segundo heap (dlmalloc mspace) sobre PSRAM del ESP32-P4.
 * No sustituye al dlmalloc principal (L2MEM); provee un heap separado
 * en la región de PSRAM ya inicializada/mapeada por el SBL.
 */
#include <kernel/spinlock.h>
#define MSPACES 1
#include <lib/dlmalloc.h>
#include <lk/compiler.h>
#include <lk/debug.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <platform/psram_heap.h>

/* Región PSRAM para heap: desde fin de .sdshare (reservas DMA SDMMC en linker)
 * hasta el final seguro de PSRAM (antes de los 32K del core LP). */
extern uint8_t _sd_block_page[];
extern uint8_t _psram_free_start[];
extern uint8_t _psram_free_end[];

static mspace psram_msp;
static spin_lock_t psram_heap_lock = SPIN_LOCK_INITIAL_VALUE;

void psram_heap_init(void) {
    if (psram_msp) {
        return;
    }
    uintptr_t base = (uintptr_t)_psram_free_start;
    uintptr_t end = (uintptr_t)_psram_free_end;
    if (end <= base) {
        dprintf(CRITICAL, "psram_heap: invalid range\n");
        return;
    }
    size_t size = end - base;
    /* locked=0: gestionamos la exclusión con spinlock SMP propio. */
    psram_msp = create_mspace_with_base((void *)base, size, 0);
    if (!psram_msp) {
        dprintf(CRITICAL, "psram_heap: create_mspace_with_base failed\n");
    } else {
        dprintf(INFO, "psram_heap: mspace @ 0x%08x size 0x%08x\n",
                (unsigned)base, (unsigned)size);
    }
}

static inline bool psram_ready(void) {
    return psram_msp != NULL;
}

void *psram_malloc(size_t size) {
    if (!psram_ready() || size == 0) {
        return NULL;
    }
    arch_interrupt_saved_state_t state = spin_lock_irqsave(&psram_heap_lock);
    void *p = mspace_malloc(psram_msp, size);
    spin_unlock_irqrestore(&psram_heap_lock, state);
    return p;
}

void *psram_calloc(size_t n, size_t size) {
    if (!psram_ready() || n == 0 || size == 0) {
        return NULL;
    }
    arch_interrupt_saved_state_t state = spin_lock_irqsave(&psram_heap_lock);
    void *p = mspace_calloc(psram_msp, n, size);
    spin_unlock_irqrestore(&psram_heap_lock, state);
    return p;
}

void *psram_realloc(void *ptr, size_t size) {
    if (!psram_ready()) {
        return NULL;
    }
    arch_interrupt_saved_state_t state = spin_lock_irqsave(&psram_heap_lock);
    void *p = mspace_realloc(psram_msp, ptr, size);
    spin_unlock_irqrestore(&psram_heap_lock, state);
    return p;
}

void *psram_memalign(size_t alignment, size_t size) {
    if (!psram_ready() || size == 0) {
        return NULL;
    }
    arch_interrupt_saved_state_t state = spin_lock_irqsave(&psram_heap_lock);
    void *p = mspace_memalign(psram_msp, alignment, size);
    spin_unlock_irqrestore(&psram_heap_lock, state);
    return p;
}

void psram_free(void *ptr) {
    if (!ptr || !psram_ready()) {
        return;
    }
    arch_interrupt_saved_state_t state = spin_lock_irqsave(&psram_heap_lock);
    mspace_free(psram_msp, ptr);
    spin_unlock_irqrestore(&psram_heap_lock, state);
}

#if !NO_MALLINFO
struct mallinfo psram_mallinfo(void) {
    struct mallinfo mi = {0};
    if (!psram_ready()) {
        return mi;
    }
    arch_interrupt_saved_state_t state = spin_lock_irqsave(&psram_heap_lock);
    mi = mspace_mallinfo(psram_msp);
    spin_unlock_irqrestore(&psram_heap_lock, state);
    return mi;
}
#endif
