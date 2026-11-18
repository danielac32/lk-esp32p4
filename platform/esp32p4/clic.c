/*
 * ESP32-P4 CLIC (Core-Local Interrupt Controller) for LK.
 * SMP-capable: per-CPU init for timer/mtvt, per-core interrupt matrix routing.
 */
#include <lk/debug.h>
#include <lk/err.h>
#include <lk/reg.h>
#include <lk/trace.h>
#include <arch/ops.h>
#include <platform/interrupts.h>
#include <platform/esp32p4.h>
#include <stdint.h>

#if WITH_SMP
#include <arch/mp.h>
#endif

#define LOCAL_TRACE 0

#define P4_NIRQ         48
#define P4_EXT_OFFSET   16
#define CLIC_HART_OFF   0x10000UL

typedef struct {
    volatile uint8_t ip;
    volatile uint8_t ie;
    volatile uint8_t attr;
    volatile uint8_t ctl;
} clicint_t;

#define CLIC_BASE P4_CLIC_BASE
#define CLIC_CFG      (*(volatile uint32_t *)(CLIC_BASE + 0x00))
#define CLIC_INFO     (*(volatile uint32_t *)(CLIC_BASE + 0x04))
#define INTMTX0(n)    (*(volatile uint32_t *)(P4_INTERRUPT_CORE0_BASE + 4UL * (n)))
#define INTMTX1(n)    (*(volatile uint32_t *)(P4_INTERRUPT_CORE1_BASE + 4UL * (n)))

static inline uint32_t p4_hart(void) {
#if WITH_SMP
    return arch_curr_cpu_num();
#else
    return 0;
#endif
}
/* Per-hart CLIC register windows.
 * ESP32-P4: each hart accesses its own CLIC at 0x20801000 (local/self alias)
 * and the other hart's CLIC at 0x20801000 + 0x10000 (remote) from any hart.
 * So for "self" config always use 0x20801000; use HART views for cross-hart access. */
#define CLIC_HART0_CTRL 0x20801000UL
#define CLIC_HART1_CTRL 0x20811000UL
#define CLIC_CTRL_FOR_HART(h) (((uint32_t)(h) == 0) ? CLIC_HART0_CTRL : CLIC_HART1_CTRL)
#define CLIC_INT_FOR_HART(h,i) ((volatile clicint_t *)(CLIC_CTRL_FOR_HART(h) + (i) * 4))
#define CLIC_MTH_FOR_HART(h)   (*(volatile uint32_t *)(CLIC_BASE + 0x08UL + (uint32_t)(h) * CLIC_HART_OFF))

/* Local (self) CLIC window - same address from each hart, HW aliases to current hart */
#define CLIC_LOCAL_CTRL 0x20801000UL
#define CLIC_LOCAL_INT(i)   ((volatile clicint_t *)(CLIC_LOCAL_CTRL + (i) * 4))
#define CLIC_LOCAL_MTH      (*(volatile uint32_t *)(CLIC_BASE + 0x08UL)) /* self threshold */

struct int_handlers {
    int_handler handler;
    void *arg;
};

static struct int_handlers handlers[P4_NIRQ];

void p4_clic_route_irq(uint32_t source, uint32_t line, uint32_t cpu) {
    if (cpu == 1) {
        INTMTX1(source) = line;
    } else {
        INTMTX0(source) = line;
    }
}

/* Called from arch riscv exception path for external/platform interrupts. */
enum handler_return p4_clic_dispatch(unsigned int irq) {
    unsigned int irq_id = irq & 0xFFu;
    if (irq_id >= P4_NIRQ) {
        return INT_NO_RESCHEDULE;
    }
    if (handlers[irq_id].handler) {
        return handlers[irq_id].handler(handlers[irq_id].arg);
    }
    LTRACEF("unhandled irq %u\n", irq_id);
    return INT_NO_RESCHEDULE;
}

status_t mask_interrupt(unsigned int vector) {
    if (vector >= P4_NIRQ) return ERR_INVALID_ARGS;
    CLIC_LOCAL_INT(vector)->ie = 0;
    return NO_ERROR;
}

status_t unmask_interrupt(unsigned int vector) {
    if (vector >= P4_NIRQ) return ERR_INVALID_ARGS;
    uint32_t h = p4_hart();
    /* external interrupts: identity route source->line on current core */
    if (vector >= P4_EXT_OFFSET) {
        p4_clic_route_irq(vector, vector, h);
    }
    CLIC_LOCAL_INT(vector)->ctl = 0xff;
    CLIC_LOCAL_INT(vector)->ie = 1;
    return NO_ERROR;
}

void register_int_handler(unsigned int vector, int_handler handler, void *arg) {
    if (vector >= P4_NIRQ) return;
    handlers[vector].handler = handler;
    handlers[vector].arg = arg;
}

void register_int_handler_msi(unsigned int vector, int_handler handler, void *arg, bool edge) {
    (void)edge;
    register_int_handler(vector, handler, arg);
}

extern void p4_clic_vector_table(void);

void p4_clic_init_percpu(void) {
    /* Set mtvt to our CLIC vector table (CSR 0x307) for this hart */
    __asm__ volatile("csrw 0x307, %0" :: "r"(p4_clic_vector_table));

    /* Threshold 0: allow all priorities (MMIO, both views for safety) */
    *(volatile uint32_t *)(CLIC_BASE + 0x08UL) = 0;
    *(volatile uint32_t *)(CLIC_BASE + 0x08UL + CLIC_HART_OFF) = 0;
    __asm__ volatile("csrw 0x345, %0" :: "r"(0)); /* mnxti (clear) best-effort */
    __asm__ volatile("csrw 0x348, %0" :: "r"(0)); /* mscratchcsw best-effort */

    /* Timer interrupt: hw-vectored (SHV=1) to p4_clic_timer_entry via mtvt. */
    CLIC_LOCAL_INT(P4_IRQ_M_TIMER)->attr = 0x01; /* SHV */
    CLIC_LOCAL_INT(P4_IRQ_M_TIMER)->ctl = 0xff;
    CLIC_LOCAL_INT(P4_IRQ_M_TIMER)->ie = 1;

    /* Software interrupt line (non-vectored, dispatched via exception path) */
    CLIC_LOCAL_INT(P4_IRQ_M_SOFT)->attr = 0x00;
    CLIC_LOCAL_INT(P4_IRQ_M_SOFT)->ctl = 0xff;
    CLIC_LOCAL_INT(P4_IRQ_M_SOFT)->ie = 1;

    /* Ensure global interrupt enables in mie for this hart (M-mode) */
    __asm__ volatile("csrs mie, %0" :: "r"((1UL<<11)|(1UL<<7)|(1UL<<3)));
}

void p4_clic_init(void) {
    /* Keep CLIC_CFG at reset/default; nlbits affects ctl interpretation */

    for (unsigned i = 0; i < P4_NIRQ; i++) {
        handlers[i].handler = NULL;
        handlers[i].arg = NULL;
        /* hart0 view: 0x20801000 = hart0 (self), 0x20811000 = hart1 (remote) */
        CLIC_INT_FOR_HART(0, i)->ip = 0;
        CLIC_INT_FOR_HART(0, i)->ie = 0;
        CLIC_INT_FOR_HART(0, i)->attr = 0;
        CLIC_INT_FOR_HART(0, i)->ctl = 0x80;
        CLIC_INT_FOR_HART(1, i)->ip = 0;
        CLIC_INT_FOR_HART(1, i)->ie = 0;
        CLIC_INT_FOR_HART(1, i)->attr = 0;
        CLIC_INT_FOR_HART(1, i)->ctl = 0x80;
    }
    CLIC_MTH_FOR_HART(0) = 0;
    CLIC_MTH_FOR_HART(1) = 0;

    /* CLINT mtime control (best-effort) */
    P4_MTIMECTL |= (3UL << 4);

    /* Per-CPU setup for boot CPU */
    p4_clic_init_percpu();
}

/* Platform dispatch hook for riscv external/platform IRQs. */
enum handler_return riscv_platform_irq(void) {
    return INT_NO_RESCHEDULE;
}
