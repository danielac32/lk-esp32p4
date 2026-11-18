/*
 * Main entry point to the OS.
 */
#include <lk/main.h>

#include <app.h>
#include <arch.h>
#include <kernel/init.h>
#include <kernel/thread.h>
#include <lib/heap.h>
#include <lk/compiler.h>
#include <lk/debug.h>
#include <lk/init.h>
#include <platform.h>
#include <stdio.h>
#include <target.h>

/* saved boot arguments from whoever loaded the system */
ulong lk_boot_args[4];

extern void (*__ctor_list[])(void);
extern void (*__ctor_end[])(void);

static int bootstrap2(void *arg);

static void call_constructors(void) {
    void (**ctor)(void) = __ctor_list;
    while (ctor != __ctor_end) {
        (*ctor)();
        ctor++;
    }
}

void lk_main(ulong arg0, ulong arg1, ulong arg2, ulong arg3) {
    lk_boot_args[0] = arg0;
    lk_boot_args[1] = arg1;
    lk_boot_args[2] = arg2;
    lk_boot_args[3] = arg3;

    kernel_init_early();

    lk_primary_cpu_init_level(LK_INIT_LEVEL_EARLIEST, LK_INIT_LEVEL_ARCH_EARLY - 1);
    arch_early_init();

    lk_primary_cpu_init_level(LK_INIT_LEVEL_ARCH_EARLY, LK_INIT_LEVEL_PLATFORM_EARLY - 1);
    platform_early_init();

    lk_primary_cpu_init_level(LK_INIT_LEVEL_PLATFORM_EARLY, LK_INIT_LEVEL_TARGET_EARLY - 1);
    target_early_init();

#if WITH_SMP
    printf("\nwelcome to lk/MP\n\n");
#else
    printf("\nwelcome to lk\n\n");
#endif

    lk_primary_cpu_init_level(LK_INIT_LEVEL_TARGET_EARLY, LK_INIT_LEVEL_HEAP - 1);
    heap_init();

    call_constructors();

    lk_primary_cpu_init_level(LK_INIT_LEVEL_HEAP, LK_INIT_LEVEL_KERNEL - 1);
    kernel_init();

    lk_primary_cpu_init_level(LK_INIT_LEVEL_KERNEL, LK_INIT_LEVEL_THREADING - 1);

    thread_t *t = thread_create("bootstrap2", &bootstrap2, NULL, DEFAULT_PRIORITY, DEFAULT_STACK_SIZE);
    if (t) {
        thread_detach(t);
        thread_resume(t);
    }

    thread_become_idle();
}

static int bootstrap2(void *arg) {
    lk_primary_cpu_init_level(LK_INIT_LEVEL_THREADING, LK_INIT_LEVEL_ARCH - 1);
    arch_init();

    lk_primary_cpu_init_level(LK_INIT_LEVEL_ARCH, LK_INIT_LEVEL_PLATFORM - 1);
    platform_init();

    lk_primary_cpu_init_level(LK_INIT_LEVEL_PLATFORM, LK_INIT_LEVEL_TARGET - 1);
    target_init();

    lk_primary_cpu_init_level(LK_INIT_LEVEL_TARGET, LK_INIT_LEVEL_APPS - 1);
    apps_init();

    lk_primary_cpu_init_level(LK_INIT_LEVEL_APPS, LK_INIT_LEVEL_LAST);
    return 0;
}
