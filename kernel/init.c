/*
 * Copyright (c) 2013 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#include <kernel/init.h>

#include <kernel/debug.h>
#include <kernel/mp.h>
#include <kernel/port.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <lk/compiler.h>
#include <lk/debug.h>

void kernel_init_early(void) {
    // get us into some sort of thread context
    thread_init_early();
}

void kernel_init(void) {
    kernel_evlog_init();
#if WITH_SMP
    mp_init();
#endif
    thread_init();
    timer_init();
    port_init();
}

