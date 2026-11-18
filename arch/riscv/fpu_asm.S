/*
 * Copyright (c) 2022 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#include <lk/asm.h>
#include <arch/riscv.h>
#include <arch/riscv/asm.h>

#if RISCV_FPU

// Configurar ISA para instrucciones FP disponibles
#if defined(__riscv_flen) && __riscv_flen == 64
  #if __riscv_xlen == 32
    .attribute arch, "rv32imafdc"
  #elif __riscv_xlen == 64
    .attribute arch, "rv64imafdc"
  #endif
  #define FP_W d
  #define FP_S 8
  #define FP_FSCR_OFF 256
#elif defined(__riscv_flen) && __riscv_flen == 32
  #if __riscv_xlen == 32
    .attribute arch, "rv32imafc"
  #elif __riscv_xlen == 64
    .attribute arch, "rv64imafc"
  #endif
  #define FP_W s
  #define FP_S 4
  #define FP_FSCR_OFF 128
#else
  #error RISCV_FPU enabled but __riscv_flen not supported
#endif

.macro ZERO_FPU_REG reg
#if defined(__riscv_flen) && __riscv_flen == 64
  #if __riscv_xlen == 32
    fcvt.d.w \reg, zero
  #else
    fmv.d.x \reg, zero
  #endif
#else
    fcvt.s.w \reg, zero
#endif
.endm

FUNCTION(riscv_fpu_zero)
    csrw fcsr, zero
    ZERO_FPU_REG f0
    ZERO_FPU_REG f1
    ZERO_FPU_REG f2
    ZERO_FPU_REG f3
    ZERO_FPU_REG f4
    ZERO_FPU_REG f5
    ZERO_FPU_REG f6
    ZERO_FPU_REG f7
    ZERO_FPU_REG f8
    ZERO_FPU_REG f9
    ZERO_FPU_REG f10
    ZERO_FPU_REG f11
    ZERO_FPU_REG f12
    ZERO_FPU_REG f13
    ZERO_FPU_REG f14
    ZERO_FPU_REG f15
    ZERO_FPU_REG f16
    ZERO_FPU_REG f17
    ZERO_FPU_REG f18
    ZERO_FPU_REG f19
    ZERO_FPU_REG f20
    ZERO_FPU_REG f21
    ZERO_FPU_REG f22
    ZERO_FPU_REG f23
    ZERO_FPU_REG f24
    ZERO_FPU_REG f25
    ZERO_FPU_REG f26
    ZERO_FPU_REG f27
    ZERO_FPU_REG f28
    ZERO_FPU_REG f29
    ZERO_FPU_REG f30
    ZERO_FPU_REG f31

    li  a0, (1 << 13)
    csrs RISCV_CSR_XSTATUS, a0
    li  a0, (1 << 14)
    csrc RISCV_CSR_XSTATUS, a0
    ret
END_FUNCTION(riscv_fpu_zero)

FUNCTION(riscv_fpu_save)
#if FP_S == 8
    fsd  f0,  0*8(a0)
    fsd  f1,  1*8(a0)
    fsd  f2,  2*8(a0)
    fsd  f3,  3*8(a0)
    fsd  f4,  4*8(a0)
    fsd  f5,  5*8(a0)
    fsd  f6,  6*8(a0)
    fsd  f7,  7*8(a0)
    fsd  f8,  8*8(a0)
    fsd  f9,  9*8(a0)
    fsd  f10, 10*8(a0)
    fsd  f11, 11*8(a0)
    fsd  f12, 12*8(a0)
    fsd  f13, 13*8(a0)
    fsd  f14, 14*8(a0)
    fsd  f15, 15*8(a0)
    fsd  f16, 16*8(a0)
    fsd  f17, 17*8(a0)
    fsd  f18, 18*8(a0)
    fsd  f19, 19*8(a0)
    fsd  f20, 20*8(a0)
    fsd  f21, 21*8(a0)
    fsd  f22, 22*8(a0)
    fsd  f23, 23*8(a0)
    fsd  f24, 24*8(a0)
    fsd  f25, 25*8(a0)
    fsd  f26, 26*8(a0)
    fsd  f27, 27*8(a0)
    fsd  f28, 28*8(a0)
    fsd  f29, 29*8(a0)
    fsd  f30, 30*8(a0)
    fsd  f31, 31*8(a0)
#else
    fsw  f0,  0*4(a0)
    fsw  f1,  1*4(a0)
    fsw  f2,  2*4(a0)
    fsw  f3,  3*4(a0)
    fsw  f4,  4*4(a0)
    fsw  f5,  5*4(a0)
    fsw  f6,  6*4(a0)
    fsw  f7,  7*4(a0)
    fsw  f8,  8*4(a0)
    fsw  f9,  9*4(a0)
    fsw  f10, 10*4(a0)
    fsw  f11, 11*4(a0)
    fsw  f12, 12*4(a0)
    fsw  f13, 13*4(a0)
    fsw  f14, 14*4(a0)
    fsw  f15, 15*4(a0)
    fsw  f16, 16*4(a0)
    fsw  f17, 17*4(a0)
    fsw  f18, 18*4(a0)
    fsw  f19, 19*4(a0)
    fsw  f20, 20*4(a0)
    fsw  f21, 21*4(a0)
    fsw  f22, 22*4(a0)
    fsw  f23, 23*4(a0)
    fsw  f24, 24*4(a0)
    fsw  f25, 25*4(a0)
    fsw  f26, 26*4(a0)
    fsw  f27, 27*4(a0)
    fsw  f28, 28*4(a0)
    fsw  f29, 29*4(a0)
    fsw  f30, 30*4(a0)
    fsw  f31, 31*4(a0)
#endif
    csrr a1, fcsr
    sw   a1, FP_FSCR_OFF(a0)
    ret
END_FUNCTION(riscv_fpu_save)

FUNCTION(riscv_fpu_restore)
#if FP_S == 8
    fld  f0,  0*8(a0)
    fld  f1,  1*8(a0)
    fld  f2,  2*8(a0)
    fld  f3,  3*8(a0)
    fld  f4,  4*8(a0)
    fld  f5,  5*8(a0)
    fld  f6,  6*8(a0)
    fld  f7,  7*8(a0)
    fld  f8,  8*8(a0)
    fld  f9,  9*8(a0)
    fld  f10, 10*8(a0)
    fld  f11, 11*8(a0)
    fld  f12, 12*8(a0)
    fld  f13, 13*8(a0)
    fld  f14, 14*8(a0)
    fld  f15, 15*8(a0)
    fld  f16, 16*8(a0)
    fld  f17, 17*8(a0)
    fld  f18, 18*8(a0)
    fld  f19, 19*8(a0)
    fld  f20, 20*8(a0)
    fld  f21, 21*8(a0)
    fld  f22, 22*8(a0)
    fld  f23, 23*8(a0)
    fld  f24, 24*8(a0)
    fld  f25, 25*8(a0)
    fld  f26, 26*8(a0)
    fld  f27, 27*8(a0)
    fld  f28, 28*8(a0)
    fld  f29, 29*8(a0)
    fld  f30, 30*8(a0)
    fld  f31, 31*8(a0)
#else
    flw  f0,  0*4(a0)
    flw  f1,  1*4(a0)
    flw  f2,  2*4(a0)
    flw  f3,  3*4(a0)
    flw  f4,  4*4(a0)
    flw  f5,  5*4(a0)
    flw  f6,  6*4(a0)
    flw  f7,  7*4(a0)
    flw  f8,  8*4(a0)
    flw  f9,  9*4(a0)
    flw  f10, 10*4(a0)
    flw  f11, 11*4(a0)
    flw  f12, 12*4(a0)
    flw  f13, 13*4(a0)
    flw  f14, 14*4(a0)
    flw  f15, 15*4(a0)
    flw  f16, 16*4(a0)
    flw  f17, 17*4(a0)
    flw  f18, 18*4(a0)
    flw  f19, 19*4(a0)
    flw  f20, 20*4(a0)
    flw  f21, 21*4(a0)
    flw  f22, 22*4(a0)
    flw  f23, 23*4(a0)
    flw  f24, 24*4(a0)
    flw  f25, 25*4(a0)
    flw  f26, 26*4(a0)
    flw  f27, 27*4(a0)
    flw  f28, 28*4(a0)
    flw  f29, 29*4(a0)
    flw  f30, 30*4(a0)
    flw  f31, 31*4(a0)
#endif
    lw   a1, FP_FSCR_OFF(a0)
    csrw fcsr, a1
    ret
END_FUNCTION(riscv_fpu_restore)
#endif // RISCV_FPU

