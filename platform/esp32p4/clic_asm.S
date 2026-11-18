/*
 * ESP32-P4 CLIC vectored interrupt trampolines for LK.
 */
#include <lk/asm.h>
#include <arch/riscv.h>
#include <arch/riscv/asm.h>
#include <arch/riscv/iframe.h>

.section .text
.balign 4

/* Timer interrupt (IRQ 7) - vectored entry. */
FUNCTION(p4_clic_timer_entry)
    csrrw   sp, RISCV_CSR_XSCRATCH, sp
    bnez    sp, 1f
    csrrw   sp, RISCV_CSR_XSCRATCH, sp
    j       2f
1:
    /* came from user (unexpected); fall through to kernel save path for now */
2:
    addi    sp, sp, -RISCV_IFRAME_LEN
    STR     t6, RISCV_IFRAME_T(6)(sp)
    STR     t5, RISCV_IFRAME_T(5)(sp)
    STR     t4, RISCV_IFRAME_T(4)(sp)
    STR     t3, RISCV_IFRAME_T(3)(sp)
    STR     t2, RISCV_IFRAME_T(2)(sp)
    STR     t1, RISCV_IFRAME_T(1)(sp)
    STR     t0, RISCV_IFRAME_T(0)(sp)
    STR     a7, RISCV_IFRAME_A(7)(sp)
    STR     a6, RISCV_IFRAME_A(6)(sp)
    STR     a5, RISCV_IFRAME_A(5)(sp)
    STR     a4, RISCV_IFRAME_A(4)(sp)
    STR     a3, RISCV_IFRAME_A(3)(sp)
    STR     a2, RISCV_IFRAME_A(2)(sp)
    STR     a1, RISCV_IFRAME_A(1)(sp)
    STR     a0, RISCV_IFRAME_A(0)(sp)
    STR     ra, RISCV_IFRAME_RA(sp)
    STR     s0, RISCV_IFRAME_S0(sp)
    csrr    t0, RISCV_CSR_XSTATUS
    STR     t0, RISCV_IFRAME_STATUS(sp)
    csrr    a1, RISCV_CSR_XEPC
    STR     a1, RISCV_IFRAME_EPC(sp)

    li      a0, 0x80000007 /* M-mode timer interrupt cause */
    mv      a2, sp
    li      a3, 1          /* kernel */
    call    riscv_exception_handler

    LDR     t0, RISCV_IFRAME_EPC(sp)
    csrw    RISCV_CSR_XEPC, t0
    LDR     t0, RISCV_IFRAME_STATUS(sp)
    csrw    RISCV_CSR_XSTATUS, t0
    LDR     ra, RISCV_IFRAME_RA(sp)
    LDR     a0, RISCV_IFRAME_A(0)(sp)
    LDR     a1, RISCV_IFRAME_A(1)(sp)
    LDR     a2, RISCV_IFRAME_A(2)(sp)
    LDR     a3, RISCV_IFRAME_A(3)(sp)
    LDR     a4, RISCV_IFRAME_A(4)(sp)
    LDR     a5, RISCV_IFRAME_A(5)(sp)
    LDR     a6, RISCV_IFRAME_A(6)(sp)
    LDR     a7, RISCV_IFRAME_A(7)(sp)
    LDR     t0, RISCV_IFRAME_T(0)(sp)
    LDR     t1, RISCV_IFRAME_T(1)(sp)
    LDR     t2, RISCV_IFRAME_T(2)(sp)
    LDR     t3, RISCV_IFRAME_T(3)(sp)
    LDR     t4, RISCV_IFRAME_T(4)(sp)
    LDR     t5, RISCV_IFRAME_T(5)(sp)
    LDR     t6, RISCV_IFRAME_T(6)(sp)
    addi    sp, sp, RISCV_IFRAME_LEN
    RISCV_XRET
END_FUNCTION(p4_clic_timer_entry)

FUNCTION(p4_clic_default_entry)
    j       riscv_exception_entry
END_FUNCTION(p4_clic_default_entry)

/* CLIC hardware vector table (mtvt). Must be in flash/XIP. */
.section .rodata.p4_mtvt, "a", @progbits
.balign 64
.global p4_clic_vector_table
p4_clic_vector_table:
    .word p4_clic_default_entry /* 0 */
    .word p4_clic_default_entry /* 1 */
    .word p4_clic_default_entry /* 2 */
    .word p4_clic_default_entry /* 3 software */
    .word p4_clic_default_entry /* 4 */
    .word p4_clic_default_entry /* 5 */
    .word p4_clic_default_entry /* 6 */
    .word p4_clic_timer_entry   /* 7 timer */
    .word p4_clic_default_entry /* 8 */
    .word p4_clic_default_entry /* 9 */
    .word p4_clic_default_entry /* 10 */
    .word p4_clic_default_entry /* 11 ext */
    .word p4_clic_default_entry /* 12 */
    .word p4_clic_default_entry /* 13 */
    .word p4_clic_default_entry /* 14 */
    .word p4_clic_default_entry /* 15 */
    .word p4_clic_default_entry /* 16 */
    .word p4_clic_default_entry /* 17 */
    .word p4_clic_default_entry /* 18 */
    .word p4_clic_default_entry /* 19 */
    .word p4_clic_default_entry /* 20 */
    .word p4_clic_default_entry /* 21 */
    .word p4_clic_default_entry /* 22 */
    .word p4_clic_default_entry /* 23 */
    .word p4_clic_default_entry /* 24 */
    .word p4_clic_default_entry /* 25 */
    .word p4_clic_default_entry /* 26 */
    .word p4_clic_default_entry /* 27 */
    .word p4_clic_default_entry /* 28 */
    .word p4_clic_default_entry /* 29 */
    .word p4_clic_default_entry /* 30 */
    .word p4_clic_default_entry /* 31 */
    .word p4_clic_default_entry /* 32 */
    .word p4_clic_default_entry /* 33 */
    .word p4_clic_default_entry /* 34 */
    .word p4_clic_default_entry /* 35 */
    .word p4_clic_default_entry /* 36 */
    .word p4_clic_default_entry /* 37 */
    .word p4_clic_default_entry /* 38 */
    .word p4_clic_default_entry /* 39 */
    .word p4_clic_default_entry /* 40 */
    .word p4_clic_default_entry /* 41 */
    .word p4_clic_default_entry /* 42 */
    .word p4_clic_default_entry /* 43 */
    .word p4_clic_default_entry /* 44 */
    .word p4_clic_default_entry /* 45 */
    .word p4_clic_default_entry /* 46 */
    .word p4_clic_default_entry /* 47 */
