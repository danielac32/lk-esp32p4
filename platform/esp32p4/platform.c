/*
 * ESP32-P4 platform init for LK.
 */
#include <lk/debug.h>
#include <lk/err.h>
#include <lk/init.h>
#include <lk/reg.h>
#include <platform.h>
#include <platform/debug.h>
#include <platform/esp32p4.h>
#include <stdint.h>

void esp32p4_platform_uart_init_early(void);

void platform_early_init(void) {
    /* Early UART (polled) for debug output */
    esp32p4_platform_uart_init_early();

    p4_clic_init();
    /* mtvec in CLIC mode (MODE=3) */
    extern void riscv_exception_entry(void);
    __asm__ volatile("csrw mtvec, %0" :: "r"(((uintptr_t)&riscv_exception_entry) | 3));
}

void platform_init(void) {
#if WITH_SMP && defined(PLATFORM_ESP32P4)
    extern void p4_cpu1_tick_init_primary(void);
    p4_cpu1_tick_init_primary();
#endif
}

void platform_halt(platform_halt_action suggested_action,
                   platform_halt_reason reason) {
    (void)suggested_action; (void)reason;
    __asm__ volatile("csrc mstatus, %0" :: "r"(1 << 3) : "memory");
    for (;;) {
        __asm__ volatile("wfi");
    }
}

#if WITH_SMP && defined(PLATFORM_ESP32P4)
#include <kernel/thread.h>
#include <platform/timer.h>
#include <platform/interrupts.h>
#include <arch/mp.h>

/* SYSTIMER-based tick for hart1 (mtimecmp1 has no HW IRQ delivery on ESP32-P4).
 * Uses SYSTIMER target 1 (unit 0), oneshot, routed to CLIC line 30 on core1.
 * Kernel timer callback for CPU1 is invoked from the IRQ handler so software
 * timers/preemption run on CPU1's timer queue. */
#define P4_SYSTIMER_BASE        0x500E2000UL
#define P4_SYSTIMER_CONF        (*(volatile uint32_t *)(P4_SYSTIMER_BASE + 0x00))
#define P4_SYSTIMER_U0_OP       (*(volatile uint32_t *)(P4_SYSTIMER_BASE + 0x04))
#define P4_SYSTIMER_TARGET1_CONF (*(volatile uint32_t *)(P4_SYSTIMER_BASE + 0x38))
#define P4_SYSTIMER_U0_VAL_HI   (*(volatile uint32_t *)(P4_SYSTIMER_BASE + 0x40))
#define P4_SYSTIMER_U0_VAL_LO   (*(volatile uint32_t *)(P4_SYSTIMER_BASE + 0x44))
#define P4_SYSTIMER_COMP1_LOAD  (*(volatile uint32_t *)(P4_SYSTIMER_BASE + 0x54))
#define P4_SYSTIMER_INT_ENA     (*(volatile uint32_t *)(P4_SYSTIMER_BASE + 0x64))
#define P4_SYSTIMER_INT_CLR     (*(volatile uint32_t *)(P4_SYSTIMER_BASE + 0x6C))
#define P4_SYSTIMER_TARGET1_HI  (*(volatile uint32_t *)(P4_SYSTIMER_BASE + 0x24))
#define P4_SYSTIMER_TARGET1_LO  (*(volatile uint32_t *)(P4_SYSTIMER_BASE + 0x28))

#define P4_SYSTIMER_UNIT0_WORK_EN       (1u << 30)
#define P4_SYSTIMER_TARGET1_WORK_EN     (1u << 23)
#define P4_SYSTIMER_CLK_EN              (1u << 31)
#define P4_SYSTIMER_TARGET1_INT_ENA_BIT (1u << 1)
#define P4_SYSTIMER_TARGET1_INT_CLR_BIT (1u << 1)

#define P4_CPU1_TICK_CLIC_LINE  30u
#define P4_SYSTIMER_TARGET1_SRC 54u  /* ETS_SYSTIMER_TARGET1_INTR_SOURCE */
#define P4_SYSTIMER_CLK_HZ      16000000u

enum handler_return riscv_timer_invoke_cb(uint cpu, lk_time_t now);

static inline uint64_t p4_systimer_unit0_now(void) {
    P4_SYSTIMER_U0_OP = (1u << 30); /* update */
    for (int i = 0; i < 10000; i++) {
        if (P4_SYSTIMER_U0_OP & (1u << 29)) break; /* valid */
    }
    return ((uint64_t)(P4_SYSTIMER_U0_VAL_HI & 0xFFFFFu) << 32) | (uint64_t)P4_SYSTIMER_U0_VAL_LO;
}

static void p4_systimer_target1_arm_ticks(uint64_t delta_ticks) {
    if (delta_ticks == 0) delta_ticks = 1;
    uint64_t target = p4_systimer_unit0_now() + delta_ticks;
    P4_SYSTIMER_TARGET1_HI = (uint32_t)((target >> 32) & 0xFFFFFu);
    P4_SYSTIMER_TARGET1_LO = (uint32_t)(target & 0xFFFFFFFFu);
    P4_SYSTIMER_COMP1_LOAD = 1;
}

/* Called by arch/riscv/time.c when CPU1 needs a oneshot timer programmed. */
void p4_cpu1_systimer_rearm(lk_time_t interval_ms) {
    uint64_t ticks = ((uint64_t)interval_ms * P4_SYSTIMER_CLK_HZ) / 1000u;
    if (ticks == 0) ticks = 1;
    p4_systimer_target1_arm_ticks(ticks);
}

static enum handler_return p4_cpu1_systimer_tick(void *arg) {
    (void)arg;
    P4_SYSTIMER_INT_CLR = P4_SYSTIMER_TARGET1_INT_CLR_BIT;
    /* Run kernel timer callback for CPU1 (handles timer queue + preemption). */
    return riscv_timer_invoke_cb(1, current_time());
}

/* Called on CPU1 (secondary) to enable the routed systimer tick interrupt line. */
void p4_cpu1_tick_init_secondary(void) {
    p4_clic_route_irq(P4_SYSTIMER_TARGET1_SRC, P4_CPU1_TICK_CLIC_LINE, 1);
    unmask_interrupt(P4_CPU1_TICK_CLIC_LINE);
    volatile uint8_t *clic = (volatile uint8_t *)0x20801000UL; /* self on hart1 */
    clic[P4_CPU1_TICK_CLIC_LINE * 4 + 2] = 0x00; /* non-vectored */
    clic[P4_CPU1_TICK_CLIC_LINE * 4 + 3] = 0xff;
    clic[P4_CPU1_TICK_CLIC_LINE * 4 + 1] = 1;
}

/* Called once on CPU0 to program systimer target1 for CPU1. */
void p4_cpu1_tick_init_primary(void) {
    volatile uint32_t *hp_clk2 = (volatile uint32_t *)(0x500E6000UL + 0x1c);
    volatile uint32_t *hp_pclk21 = (volatile uint32_t *)(0x500E6000UL + 0x98);
    *hp_clk2 |= (1u << 23);   /* systimer APB clk en */
    *hp_pclk21 |= (1u << 30); /* systimer clk en */

    P4_SYSTIMER_CONF |= P4_SYSTIMER_CLK_EN | P4_SYSTIMER_UNIT0_WORK_EN | P4_SYSTIMER_TARGET1_WORK_EN;

    P4_SYSTIMER_INT_CLR = P4_SYSTIMER_TARGET1_INT_CLR_BIT;
    P4_SYSTIMER_INT_ENA &= ~P4_SYSTIMER_TARGET1_INT_ENA_BIT;

    /* Target1: oneshot, unit0 */
    P4_SYSTIMER_TARGET1_CONF = 0;
    P4_SYSTIMER_COMP1_LOAD = 1;

    register_int_handler(P4_CPU1_TICK_CLIC_LINE, p4_cpu1_systimer_tick, NULL);

    P4_SYSTIMER_INT_CLR = P4_SYSTIMER_TARGET1_INT_CLR_BIT;
    P4_SYSTIMER_INT_ENA |= P4_SYSTIMER_TARGET1_INT_ENA_BIT;
}
#endif
