/*
 * Copyright (c) 2019 Elliot Berman
 * Copyright (c) 2020 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

#include <arch/riscv.h>

#include <lk/reg.h>
#include <lk/compiler.h>
#include <lk/debug.h>
#include <lk/trace.h>
#include <lk/err.h>
#include <lk/init.h>
#include <lk/main.h>
#include <stdlib.h>
#include <string.h>

#include <arch/atomic.h>
#include <arch/ops.h>
#include <arch/mp.h>
#include <arch/riscv/clint.h>

#include "riscv_priv.h"

#if WITH_SMP

#define LOCAL_TRACE 0

#ifdef PLATFORM_ESP32P4
#include <platform/esp32p4.h>
#include <platform/interrupts.h>
#define P4_HP_SYS_BASE             0x500E5000UL
#define P4_CPU_INT_FROM_CPU_0_REG  (*(volatile uint32_t *)(P4_HP_SYS_BASE + 0x10))
#define P4_CPU_INT_FROM_CPU_1_REG  (*(volatile uint32_t *)(P4_HP_SYS_BASE + 0x14))
/* Use external CLIC lines for IPIs (routed from FROM_CPU interrupt sources).
 * These are peripheral/external interrupt IDs in the 16-47 range. */
#define P4_IPI_IRQ_CPU0 28
#define P4_IPI_IRQ_CPU1 29
#define P4_ETS_FROM_CPU_INTR0_SOURCE 79
#define P4_ETS_FROM_CPU_INTR1_SOURCE 80

static inline void p4_ipi_send_hart(uint hart) {
    volatile uint32_t *reg = (hart == 1) ? &P4_CPU_INT_FROM_CPU_1_REG : &P4_CPU_INT_FROM_CPU_0_REG;
    /* Pulse: clear then set to ensure a new interrupt trigger */
    *reg = 0;
    __asm__ volatile("fence rw,rw" ::: "memory");
    *reg = 1;
    __asm__ volatile("fence rw,rw" ::: "memory");
}

static inline void p4_ipi_clear_current(void) {
    uint h = riscv_current_hart();
    if (h == 1) {
        P4_CPU_INT_FROM_CPU_1_REG = 0;
    } else {
        P4_CPU_INT_FROM_CPU_0_REG = 0;
    }
    __asm__ volatile("fence rw,rw" ::: "memory");
}
#endif

// mapping of cpu -> hart
static uint cpu_to_hart_map[SMP_MAX_CPUS];

// list of IPIs queued per cpu
static volatile int ipi_data[SMP_MAX_CPUS];

static spin_lock_t boot_cpu_lock = 1;

// list of cpus to boot, passed from platform
static uint harts_to_boot[SMP_MAX_CPUS];
static uint harts_to_boot_count = 0;

// modified in start.S to save the physical address of _start as the first cpu boots
uintptr_t _start_physical;

status_t arch_mp_send_ipi(mp_cpu_mask_t target, mp_ipi_t ipi) {
    LTRACEF("target 0x%x, ipi %u\n", target, ipi);

    mp_cpu_mask_t m = target;
    ulong hart_mask = 0;
    for (uint c = 0; c < SMP_MAX_CPUS && m; c++, m >>= 1) {
        if (m & 1) {
            int h = cpu_to_hart_map[c];
            LTRACEF("c %u h %d m %#x\n", c, h, m);

            // record a pending hart to notify
            hart_mask |= (1ul << h);

            // set the ipi_data based on the incoming ipi.
            // NOTE: indexed by cpu number, not hart id, since that is how the
            // receiving side (riscv_software_exception) reads it.
            atomic_or(&ipi_data[c], (1u << ipi));
        }
    }

    mb();
#ifdef PLATFORM_ESP32P4
    {
        ulong hm = hart_mask;
        for (uint h = 0; h < SMP_MAX_CPUS && hm; h++, hm >>= 1) {
            if (hm & 1) p4_ipi_send_hart(h);
        }
        /* also try CLINT MSIP */
        clint_send_ipis(&hart_mask);
    }
#elif RISCV_M_MODE
    clint_send_ipis(&hart_mask);
#else
    sbi_send_ipis(&hart_mask);
#endif

    return NO_ERROR;
}

// software triggered exceptions, used for cross-cpu calls
enum handler_return riscv_software_exception(void) {
    uint curr_cpu = arch_curr_cpu_num();

#ifdef PLATFORM_ESP32P4
    p4_ipi_clear_current();
    /* Clear CLIC pending for IPI line */
    {
        uint irq = (curr_cpu == 1) ? 29u : 28u;
        volatile uint8_t *clic = (volatile uint8_t *)0x20801000UL; /* self */
        clic[irq * 4 + 0] = 0;
    }
    { uint ch = riscv_current_hart(); clint_ipi_clear(ch); }
#elif RISCV_M_MODE
    uint ch = riscv_current_hart();
    clint_ipi_clear(ch);
#else
    sbi_clear_ipi();
#endif

    // Clear the pending bit before taking the reasons, or a reason and its IPI
    // landing in between are both lost. The clear is device output (an MMIO
    // store to the CLINT, or a CSR write), so this needs the o -> rw fence.
    mb();
    int reason = atomic_swap(&ipi_data[curr_cpu], 0);
    LTRACEF("cpu %u reason %#x\n", curr_cpu, reason);

    enum handler_return ret = INT_NO_RESCHEDULE;
    if (reason & (1u << MP_IPI_RESCHEDULE)) {
        ret = mp_mbx_reschedule_irq();
        reason &= ~(1u << MP_IPI_RESCHEDULE);
    }
    if (reason & (1u << MP_IPI_GENERIC)) {
        if (mp_mbx_generic_irq() == INT_RESCHEDULE) {
            ret = INT_RESCHEDULE;
        }
        reason &= ~(1u << MP_IPI_GENERIC);
    }

    if (unlikely(reason)) {
        TRACEF("unhandled ipi cause %#x, cpu %u\n", reason, curr_cpu);
        panic("stopping");
    }

    return ret;
}

#ifdef PLATFORM_ESP32P4
static enum handler_return p4_ipi_irq_handler(void *arg) {
    (void)arg;
    return riscv_software_exception();
}

void p4_ipi_init_percpu(void) {
    uint cpu = arch_curr_cpu_num();
    uint irq = (cpu == 1) ? P4_IPI_IRQ_CPU1 : P4_IPI_IRQ_CPU0;
    uint src = (cpu == 1) ? P4_ETS_FROM_CPU_INTR1_SOURCE : P4_ETS_FROM_CPU_INTR0_SOURCE;
    extern void p4_clic_route_irq(uint32_t source, uint32_t line, uint32_t cpu);
    register_int_handler(irq, &p4_ipi_irq_handler, NULL);
    /* Route FROM_CPU source to this CLIC line for this CPU, then enable line */
    p4_clic_route_irq(src, irq, cpu);
    unmask_interrupt(irq);
    /* Ensure priority/enabled for this hart's local CLIC (self alias), non-vectored */
    volatile uint8_t *clic = (volatile uint8_t *)0x20801000UL;
    clic[irq*4 + 2] = 0x00; /* attr: non-vectored */
    clic[irq*4 + 3] = 0xff; /* ctl/priority */
    clic[irq*4 + 1] = 1;    /* ie */
    /* Also enable MEIE/MSIE/MTIE in mie */
    __asm__ volatile("csrs mie, %0" :: "r"((1UL<<11)|(1UL<<7)|(1UL<<3)));
    __asm__ volatile("fence rw,rw" ::: "memory");
}
#endif

// called in very early percpu bringup
void riscv_configure_percpu_mp_early(uint hart_id, uint cpu_num) {
    cpu_to_hart_map[cpu_num] = hart_id;
    wmb();
}

// called from assembly
void riscv_secondary_entry(uint hart_id, uint __unused, uint cpu_id);
void riscv_secondary_entry(uint hart_id, uint __unused, uint cpu_id) {

    // basic bootstrapping of this cpu
    riscv_early_init_percpu();

    if (unlikely(arch_curr_cpu_num() >= SMP_MAX_CPUS)) {
        while (1) {
            arch_idle();
        }
    }

    // spin here waiting for the main cpu to release us
    spin_lock(&boot_cpu_lock);
    spin_unlock(&boot_cpu_lock);

#if RISCV_MMU
    // let the mmu code configure per cpu bits
    riscv_mmu_init_secondaries();
#endif

    // Get us into thread context and run the initial secondary cpu init routines
    lk_secondary_cpu_entry_early();


    // run threading level initialization on this cpu
    riscv_init_percpu();
#ifdef PLATFORM_ESP32P4
    extern void p4_clic_init_percpu(void);
    extern void p4_ipi_init_percpu(void);
    extern void p4_cpu1_tick_init_secondary(void);
    p4_clic_init_percpu();
    p4_ipi_init_percpu();
    if (arch_curr_cpu_num() == 1) {
        p4_cpu1_tick_init_secondary();
    }
#endif

    dprintf(INFO, "RISCV: secondary hart started: cpu %u, mvendorid %#lx marchid %#lx mimpid %#lx mhartid %#x\n",
            arch_curr_cpu_num(),
            riscv_get_mvendorid(), riscv_get_marchid(),
            riscv_get_mimpid(), riscv_current_hart());


    // Finish secondary cpu initialization and enter the scheduler
    lk_secondary_cpu_entry();
}

// platform hands the arch layer a list of harts to start, these will be
// started later in riscv_boot_secondaries()
void riscv_set_secondary_harts_to_start(const uint *harts, size_t count) {
    harts_to_boot_count = MIN(count, SMP_MAX_CPUS);
    memcpy(harts_to_boot, harts, harts_to_boot_count * sizeof(harts[0]));
}

// start any secondary cpus we are set to start. called on the boot processor
void riscv_boot_secondaries(void) {
    // if theres nothing to start, abort here
    if (harts_to_boot_count == 0) {
        dprintf(INFO, "RISCV: No secondary harts to start\n");
        return;
    }

    lk_init_secondary_cpus(harts_to_boot_count);

#ifdef PLATFORM_ESP32P4
    /* init IPI receive path on boot CPU as well */
    extern void p4_ipi_init_percpu(void);
    p4_ipi_init_percpu();

    /* ESP32-P4: program the SYSTIMER target1 that drives the core1 tick BEFORE
     * releasing hart1, so the routed CLIC line already has a timer armed when the
     * secondary hart starts running its timer handler. */
    extern void p4_cpu1_tick_init_primary(void);
    p4_cpu1_tick_init_primary();
#endif

#if RISCV_M_MODE
    dprintf(INFO, "RISCV: Releasing all secondary harts from purgatory\n");

#ifdef PLATFORM_ESP32P4
    /* ESP32-P4: core1 waits in platform start.S on this flag */
    {
        extern volatile unsigned int _p4_core1_release;
        _p4_core1_release = 1;
        __asm__ volatile("fence rw,rw" ::: "memory");
    }
#endif
    // For machine mode, we simply unblock all the trapped cpus in start.S by releasing
    // the boot_cpu_lock. Via some mechanism or other, all of the cpus should have already entered
    // start.S via the main entry point for this to work. This is the default behavior when
    // running on QEMU, for example.
    spin_unlock(&boot_cpu_lock);
#else
    dprintf(INFO, "RISCV: Going to try to start %d secondary harts\n", harts_to_boot_count);

    // May as well release the boot cpu lock now, since there's no reason to hold back secondaries we
    // haven't started yet.
    spin_unlock(&boot_cpu_lock);

    // use SBI HSM to boot the secondaries
    uint boot_hart = riscv_current_hart();
    for (uint i = 0; i < harts_to_boot_count; i++) {
        // skip the boot hart
        if (harts_to_boot[i] != boot_hart) {
            dprintf(INFO, "RISCV: using SBI to start hart %u at %#lx\n", harts_to_boot[i], _start_physical);
            sbi_boot_hart(harts_to_boot[i], _start_physical, 0);

            // Pause a little bit here to give the secondary an opportunity to start. Not strictly
            // needed but helps the boot output flow without much of a boot penalty. Remove
            // if it's a problem.
            thread_sleep(50);
        }
    }
#endif
}


#endif
