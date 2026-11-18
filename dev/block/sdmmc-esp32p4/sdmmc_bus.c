/*
 * Shared SDMMC bus lock (SD slot 0 + SDIO C6 slot 1).
 * Adapted from Xinu system/sdmmcbus.c to LK primitives.
 * Uses a mutex; if reentrancy is needed by a future caller it can be extended.
 */
#include <kernel/mutex.h>
#include <stdbool.h>
#include "sdmmc_internal.h"

static mutex_t sdmmc_bus_mutex;
static bool sdmmc_bus_inited;

void sdmmc_bus_init(void) {
    if (!sdmmc_bus_inited) {
        mutex_init(&sdmmc_bus_mutex);
        sdmmc_bus_inited = true;
    }
}

void sdmmc_bus_lock(void) {
    if (sdmmc_bus_inited) {
        mutex_acquire(&sdmmc_bus_mutex);
    }
}

void sdmmc_bus_unlock(void) {
    if (sdmmc_bus_inited) {
        mutex_release(&sdmmc_bus_mutex);
    }
}
