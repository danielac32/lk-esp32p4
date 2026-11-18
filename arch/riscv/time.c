/*
 * Copyright (c) 2018 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#include <lk/reg.h>
#include <lk/debug.h>
#include <lk/trace.h>
#include <lk/err.h>

#include <arch/riscv/feature.h>
#include <arch/riscv.h>
#include <arch/ops.h>

#include <platform.h>
#include <platform/timer.h>
#if WITH_SMP
#include <arch/mp.h>
#include <kernel/mp.h>
#endif

#define LOCAL_TRACE 0

static platform_timer_callback timer_cb[SMP_MAX_CPUS];
static void *timer_arg[SMP_MAX_CPUS];
volatile unsigned int p4_timer_set_count[2];
volatile uint64_t p4_timer_last_ticks[2];

#ifdef PLATFORM_ESP32P4
/* CPU1 uses external systimer (mtimecmp1 IRQ not delivered to hart1). */
void p4_cpu1_systimer_rearm(lk_time_t interval_ms);
#endif

status_t platform_set_oneshot_timer(platform_timer_callback callback, void *arg, lk_time_t interval) {
    LTRACEF("cb %p, arg %p, interval %u\n", callback, arg, interval);

    uint cpu = arch_curr_cpu_num();

    // disable timer
    riscv_csr_clear(RISCV_CSR_XIE, RISCV_CSR_XIE_TIE);

    timer_cb[cpu] = callback;
    timer_arg[cpu] = arg;

    // enable the timer
    riscv_csr_set(RISCV_CSR_XIE, RISCV_CSR_XIE_TIE);

    // convert interval to ticks (use 64-bit math: interval * 400MHz overflows 32 bits)
    uint64_t ticks = riscv_get_time() + (((uint64_t)interval * ARCH_RISCV_MTIME_RATE) / 1000u);
#ifdef PLATFORM_ESP32P4
    {
        uint h = riscv_current_hart();
        if (h < 2) { p4_timer_set_count[h]++; p4_timer_last_ticks[h] = ticks; }
    }
    if (cpu == 1) {
        /* No mtimecmp IRQ delivery on hart1: arm platform systimer instead. */
        p4_cpu1_systimer_rearm(interval);
        return NO_ERROR;
    }
#endif
#if RISCV_M_MODE
    clint_set_timer(ticks);
#elif RISCV_S_MODE
    if (riscv_feature_test(RISCV_FEAT_SSTC)) {
#if __riscv_xlen == 64
        riscv_csr_write(RISCV_CSR_STIMECMP, ticks);
#else
        riscv_csr_write(RISCV_CSR_STIMECMPH, ticks >> 32);
        riscv_csr_write(RISCV_CSR_STIMECMP, ticks);
#endif
    } else {
        sbi_set_timer(ticks);
    }
#endif

    return NO_ERROR;
}

lk_bigtime_t current_time_hires(void) {
#if ARCH_RISCV_MTIME_RATE < 10000000
    return current_time() * 1000llu; // hack to deal with slow clocks
#else
    return riscv_get_time() / (ARCH_RISCV_MTIME_RATE / 1000000u);
#endif
}

lk_time_t current_time(void) {
    return riscv_get_time() / (ARCH_RISCV_MTIME_RATE / 1000u);
}

void platform_stop_timer(void) {
    riscv_csr_clear(RISCV_CSR_XIE, RISCV_CSR_XIE_TIE);
}

enum handler_return riscv_timer_exception(void) {
    LTRACEF("tick\n");

    riscv_csr_clear(RISCV_CSR_XIE, RISCV_CSR_XIE_TIE);

    uint cpu = arch_curr_cpu_num();
    enum handler_return ret = INT_NO_RESCHEDULE;
    if (timer_cb[cpu]) {
        ret = timer_cb[cpu](timer_arg[cpu], current_time());
    }

    return ret;
}

#ifdef PLATFORM_ESP32P4
/* Called from platform CPU1 systimer IRQ handler. */
enum handler_return riscv_timer_invoke_cb(uint cpu, lk_time_t now) {
    if (cpu >= SMP_MAX_CPUS || !timer_cb[cpu]) return INT_NO_RESCHEDULE;
    return timer_cb[cpu](timer_arg[cpu], now);
}
#endif
