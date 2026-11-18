/*
 * Copyright (c) 2015 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#include <assert.h>
#include <lk/backtrace.h>
#include <lk/compiler.h>
#include <lk/err.h>
#include <lk/trace.h>
#include <arch/ops.h>
#include <arch/riscv.h>
#include <kernel/thread.h>
#include <platform.h>
#include <arch/riscv/iframe.h>
#if RISCV_MMU
#include <arch/riscv/mmu.h>
#include <kernel/vm.h>
#endif

#define LOCAL_TRACE 0

static const char *cause_to_string(long cause) {
#ifdef PLATFORM_ESP32P4
    long is_interrupt = (cause < 0);
    long code = cause & 0xfff;
    if (!is_interrupt) {
        switch (code) {
#else
    if (cause >= 0) {
        switch (cause) {
#endif
            case RISCV_EXCEPTION_IADDR_MISALIGN:
                return "Instruction address misaligned";
            case RISCV_EXCEPTION_IACCESS_FAULT:
                return "Instruction access fault";
            case RISCV_EXCEPTION_ILLEGAL_INS:
                return "Illegal instruction";
            case RISCV_EXCEPTION_BREAKPOINT:
                return "Breakpoint";
            case RISCV_EXCEPTION_LOAD_ADDR_MISALIGN:
                return "Load address misaligned";
            case RISCV_EXCEPTION_LOAD_ACCESS_FAULT:
                return "Load access fault";
            case RISCV_EXCEPTION_STORE_ADDR_MISALIGN:
                return "Store/AMO address misaligned";
            case RISCV_EXCEPTION_STORE_ACCESS_FAULT:
                return "Store/AMO access fault";
            case RISCV_EXCEPTION_ENV_CALL_U_MODE:
                return "Environment call from U-mode";
            case RISCV_EXCEPTION_ENV_CALL_S_MODE:
                return "Environment call from S-mode";
            case RISCV_EXCEPTION_ENV_CALL_M_MODE:
                return "Environment call from M-mode";
            case RISCV_EXCEPTION_INS_PAGE_FAULT:
                return "Instruction page fault";
            case RISCV_EXCEPTION_LOAD_PAGE_FAULT:
                return "Load page fault";
            case RISCV_EXCEPTION_STORE_PAGE_FAULT:
                return "Store/AMO page fault";
        }
        return "Unknown exception";
    } else {
#ifdef PLATFORM_ESP32P4
        switch (code) {
#else
        switch (cause & LONG_MAX) {
#endif
            case RISCV_INTERRUPT_USWI:
                return "User software interrupt";
            case RISCV_INTERRUPT_SSWI:
                return "Supervisor software interrupt";
            case RISCV_INTERRUPT_MSWI:
                return "Machine software interrupt";
            case RISCV_INTERRUPT_UTIM:
                return "User timer interrupt";
            case RISCV_INTERRUPT_STIM:
                return "Supervisor timer interrupt";
            case RISCV_INTERRUPT_MTIM:
                return "Machine timer interrupt";
            case RISCV_INTERRUPT_UEXT:
                return "User external interrupt";
            case RISCV_INTERRUPT_SEXT:
                return "Supervisor external interrupt";
            case RISCV_INTERRUPT_MEXT:
                return "Machine external interrupt";
        }
        return "Unknown interrupt";
    }
}

static void dump_iframe(struct riscv_short_iframe *frame, bool kernel) {
    printf("a0 0x%lx a1 0x%lx a2 0x%lx a3 0x%lx\n", frame->a0, frame->a1, frame->a2, frame->a3);
    printf("a4 0x%lx a5 0x%lx a6 0x%lx a7 0x%lx\n", frame->a4, frame->a5, frame->a6, frame->a7);
    printf("t0 0x%lx t1 0x%lx t2 0x%lx t3 0x%lx\n", frame->t0, frame->t1, frame->t2, frame->t3);
    printf("t5 0x%lx t6 0x%lx\n", frame->t5, frame->t6);
    if (!kernel) {
        // Old gp/tp/sp are saved when coming from user space in the iframe
        printf("gp 0x%lx tp 0x%lx sp 0x%lx\n", frame->gp, frame->tp, frame->sp);
    } else {
        ulong gp, tp;

        asm volatile("mv %0, gp" : "=r"(gp));
        asm volatile("mv %0, tp" : "=r"(tp));

        // gp/tp/sp are not modified when taking a kernel to kernel exception
        printf("gp 0x%lx tp 0x%lx\n", gp, tp);
    }
    printf("s0 0x%lx ra 0x%lx\n", frame->s0, frame->ra);

    backtrace_print(frame->epc, frame->s0);
}

__NO_RETURN __NO_INLINE
static void fatal_exception(long cause, ulong epc, struct riscv_short_iframe *frame, bool kernel) {
    if (cause < 0) {
        printf("unhandled interrupt cause 0x%lx (%s), epc 0x%lx, tval 0x%lx\n", cause,
              cause_to_string(cause), epc, riscv_csr_read(RISCV_CSR_XTVAL));
    } else {
        printf("unhandled exception cause 0x%lx (%s), epc 0x%lx, tval 0x%lx\n", cause,
              cause_to_string(cause), epc, riscv_csr_read(RISCV_CSR_XTVAL));
    }

    dump_iframe(frame, kernel);
    platform_halt(HALT_ACTION_HALT, HALT_REASON_SW_PANIC);
}

void riscv_syscall_unhandled(struct riscv_short_iframe *frame) {
    printf("unhandled syscall from user space in thread %s\n", get_current_thread()->name);
    dump_iframe(frame, false);
    thread_exit(ERR_NOT_SUPPORTED);
}

void riscv_user_exception_unhandled(long cause, ulong epc, struct riscv_short_iframe *frame) {
    printf("unhandled exception from user space in thread %s: cause 0x%lx (%s), epc 0x%lx, tval 0x%lx\n",
           get_current_thread()->name, cause, cause_to_string(cause), epc,
           riscv_csr_read(RISCV_CSR_XTVAL));
    dump_iframe(frame, false);
    thread_exit(ERR_FAULT);
}

// weak references, overridable by whatever hosts user space. Both run in
// trap context on the thread's kernel stack with interrupts disabled;
// returning resumes user space at frame->epc.
__WEAK
void riscv_syscall_handler(struct riscv_short_iframe *frame) {
    riscv_syscall_unhandled(frame);
}

__WEAK
void riscv_user_exception(long cause, ulong epc, struct riscv_short_iframe *frame) {
    riscv_user_exception_unhandled(cause, epc, frame);
}

#if RISCV_MMU
// A page fault the tables do not explain is a translation this hart cached
// before the entry was made valid: drop everything the hart holds and let the
// access run again. Returns false for a fault that is real.
//
// The same instruction faulting on the same address over and over is not
// stale whatever the tables say: after the fence the next walk is definite,
// and the window Svvptc allows is short. Past a small count the fault is
// reported as real, so a wrong verdict ends in a diagnosable fault rather than
// a hart that spins in the handler. One record per cpu; this runs with
// interrupts off.
struct page_fault_retry {
    ulong epc;
    vaddr_t addr;
    long cause;
    uint count;
};
static struct page_fault_retry page_fault_retries[SMP_MAX_CPUS];
#define PAGE_FAULT_RETRY_LIMIT 16

static bool riscv_page_fault_retry(long cause, ulong epc, const struct riscv_short_iframe *frame) {
    const vaddr_t addr = riscv_csr_read(RISCV_CSR_XTVAL);
    vmm_aspace_t *aspace = vaddr_to_aspace((void *)addr);
    if (!aspace || !riscv_mmu_fault_is_stale(&aspace->arch_aspace, addr, cause, frame->status)) {
        return false;
    }

    struct page_fault_retry *r = &page_fault_retries[arch_curr_cpu_num()];
    if (r->epc == epc && r->addr == addr && r->cause == cause) {
        if (++r->count > PAGE_FAULT_RETRY_LIMIT) {
            printf("page fault at 0x%lx (epc 0x%lx, cause %ld) still stale after %u retries, treating as real\n",
                   addr, epc, cause, r->count - 1);
            r->count = 0;
            return false;
        }
    } else {
        *r = (struct page_fault_retry){ .epc = epc, .addr = addr, .cause = cause, .count = 1 };
    }

    LTRACEF("stale translation for %#lx, cause %ld\n", addr, cause);
    // the everything form: a cached invalid link to a table is only dropped by that one
    riscv_tlb_flush_all();
    return true;
}
#endif

// called from assembly
void riscv_exception_handler(long cause, ulong epc, struct riscv_short_iframe *frame, bool kernel);
void riscv_exception_handler(long cause, ulong epc, struct riscv_short_iframe *frame, bool kernel) {
    LTRACEF("hart %u cause %#lx epc %#lx status %#lx kernel %d\n",
            riscv_current_hart(), cause, epc, frame->status, kernel);

    enum handler_return ret = INT_NO_RESCHEDULE;
#ifdef PLATFORM_ESP32P4
    /* ESP32-P4 CLIC: mcause can encode extra fields (mpil/mpp/mpie/minhv) in
     * upper bits; exccode/interrupt id is in low 12 bits. */
    long is_interrupt = (cause < 0);
    long code = cause & 0xfff;
#else
    long is_interrupt = (cause < 0);
    long code = cause & LONG_MAX;
#endif

    // top bit of the cause register determines if it's an interrupt or not
    if (is_interrupt) {
        switch (code) {
#if WITH_SMP
            case RISCV_INTERRUPT_XSWI: // machine software interrupt
                ret = riscv_software_exception();
                break;
#endif
            case RISCV_INTERRUPT_XTIM: // machine timer interrupt
                ret = riscv_timer_exception();
                break;
            case RISCV_INTERRUPT_XEXT: // machine external interrupt
                ret = riscv_platform_irq();
                break;
            default:
#ifdef PLATFORM_ESP32P4
                if (code >= 16) {
                    extern enum handler_return p4_clic_dispatch(unsigned int irq);
                    ret = p4_clic_dispatch((unsigned int)code);
                    break;
                }
#endif
                fatal_exception(cause, epc, frame, kernel);
        }
    } else {
        // all synchronous traps go here
        switch (code) {
            case RISCV_EXCEPTION_ENV_CALL_U_MODE: // ecall from user mode
                // resume past the ecall, which is always 4 bytes even with the C
                // extension; the handler may still move epc elsewhere
                frame->epc += 4;
                riscv_syscall_handler(frame);
                break;
#if RISCV_MMU
            case RISCV_EXCEPTION_INS_PAGE_FAULT:
            case RISCV_EXCEPTION_LOAD_PAGE_FAULT:
            case RISCV_EXCEPTION_STORE_PAGE_FAULT:
                if (riscv_page_fault_retry((long)code, epc, frame)) {
                    break;
                }
                if (!kernel) {
                    riscv_user_exception((long)code, epc, frame);
                } else {
                    fatal_exception(cause, epc, frame, kernel);
                }
                break;
#endif
            default:
                // anything else user space did is its own problem, not the kernel's
                if (!kernel) {
                    riscv_user_exception((long)code, epc, frame);
                } else {
                    fatal_exception(cause, epc, frame, kernel);
                }
        }
    }

    if (ret == INT_RESCHEDULE) {
        thread_preempt();
    }
}
