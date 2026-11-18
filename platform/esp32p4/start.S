#include <lk/asm.h>
#include <arch/defines.h>

.section .text.boot
FUNCTION(_start)
    /* Enable UART0 clocks and configure for 115200 */
    li      t4, 0x500CA000
    li      t0, 0x500E6000
    lw      t1, 0x18(t0)
    li      t2, (1 << 18)
    or      t1, t1, t2
    sw      t1, 0x18(t0)
    lw      t1, 0x1C(t0)
    li      t2, (1 << 7)
    or      t1, t1, t2
    sw      t1, 0x1C(t0)
    lw      t1, 0x128(t0)
    li      t2, (1 << 26)
    or      t1, t1, t2
    sw      t1, 0x128(t0)
    lw      t1, 0x88(t4)
    li      t2, ((1 << 24) | (1 << 25))
    or      t1, t1, t2
    sw      t1, 0x88(t4)
    lw      t1, 0x20(t4)
    li      t2, ~((3 << 2))
    and     t1, t1, t2
    li      t2, ((3 << 2) | (1 << 20))
    or      t1, t1, t2
    sw      t1, 0x20(t4)
    li      t1, 347
    sw      t1, 0x14(t4)
    lw      t1, 0x24(t4)
    li      t2, 0xFFFF0000
    and     t1, t1, t2
    li      t2, (1 | (1 << 21))
    or      t1, t1, t2
    sw      t1, 0x24(t4)

    csrr    t0, mhartid
    bnez    t0, .Lsecondary_wait

    /* Core 0 */
    li      t0, (1 << 3)
    csrc    mstatus, t0
    li      t0, 0x2000
    csrs    mstatus, t0
    csrw    fcsr, zero

.option push
.option norelax
    la      gp, __global_pointer$
.option pop

    /* Copy .data from flash (ROM) to RAM */
    la      t0, __data_start
    la      t1, __data_end
    la      t2, __data_start_rom
1:  bgeu    t0, t1, 2f
    lw      t3, 0(t2)
    sw      t3, 0(t0)
    addi    t0, t0, 4
    addi    t2, t2, 4
    j       1b
2:
    /* Clear .bss */
    la      t0, __bss_start
    la      t1, __bss_end
3:  bgeu    t0, t1, 4f
    sw      zero, 0(t0)
    addi    t0, t0, 4
    j       3b
4:
    /* Core 0 stack: leave 8KB at top for core 1 */
    la      sp, _end_of_ram
    li      t0, 8192
    sub     sp, sp, t0
    addi    sp, sp, -16
    andi    sp, sp, -16

    /* percpu for cpu 0, hart 0 */
    li      a0, 0
    li      a1, 0
    li      a2, 0
    call    riscv_configure_percpu_early

    /* Register secondary hart 1 -> cpu 1 */
    la      a0, _p4_secondary_harts
    li      a1, 1
    call    riscv_set_secondary_harts_to_start

    li      a0, 0
    li      a1, 0
    li      a2, 0
    li      a3, 0
    call    lk_main

    j       .
END_FUNCTION(_start)

/* Secondary hart (core 1): wait for release, then enter LK SMP path */
.Lsecondary_wait:
    li      t0, (1 << 3)
    csrc    mstatus, t0

    /* PC-relative load of release flag (gp not set yet, no relaxation) */
.option push
.option norelax
1:  auipc   t1, %pcrel_hi(_p4_core1_release)
    lw      t0, %pcrel_lo(1b)(t1)
    beqz    t0, 1b
    fence   rw, rw

    /* debug marker: released */
2:  auipc   t4, %pcrel_hi(_p4_core1_debug)
    li      t5, 1
    sw      t5, %pcrel_lo(2b)(t4)
    fence   rw, rw
.option pop

    li      t0, 0x2000
    csrs    mstatus, t0
    csrw    fcsr, zero

.option push
.option norelax
    la      gp, __global_pointer$
.option pop

    /* debug marker: gp set */
    la      t4, _p4_core1_debug
    li      t5, 2
    sw      t5, 0(t4)

    /* Core 1 stack: top of RAM */
    la      sp, _end_of_ram
    addi    sp, sp, -16
    andi    sp, sp, -16

    /* percpu for cpu 1, hart 1: (hart_id, unused, cpu_num) */
    li      a0, 1
    li      a1, 0
    li      a2, 1
    call    riscv_configure_percpu_early

    /* debug marker: percpu done */
    la      t4, _p4_core1_debug
    li      t5, 3
    sw      t5, 0(t4)

    /* Enter LK secondary CPU path */
    li      a0, 1
    li      a1, 0
    li      a2, 1
    call    riscv_secondary_entry

    j       .

.section .data
.align 2
_p4_secondary_harts:
    .word   1

.section .bss
.align 4
.global _p4_core1_release
_p4_core1_release:
    .word   0
.global _p4_core1_debug
_p4_core1_debug:
    .word   0

