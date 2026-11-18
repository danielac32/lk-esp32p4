/*
 * ESP32-P4 platform definitions for LK.
 */
#pragma once

#include <stdint.h>

#define P4_UART0_BASE      0x500CA000UL
#define P4_CLIC_BASE       0x20800000UL
#define P4_CLINT_BASE      0x20000000UL /* mtime/mtimecmp window */
#define P4_INTERRUPT_CORE0_BASE 0x500D6000UL
#define P4_INTERRUPT_CORE1_BASE 0x500D6800UL
#define P4_HP_SYS_CLKRST_BASE   0x500E6000UL

/* GPIO / IO_MUX */
#define P4_GPIO_BASE       0x500E0000UL
#define P4_GPIO_SD_BASE    0x500E0F00UL
#define P4_IO_MUX_BASE     0x500E1000UL

#define P4_CLIC_NIRQ       48
#define P4_IRQ_M_SOFT      3
#define P4_IRQ_M_TIMER     7
#define P4_IRQ_M_EXT       11

void p4_clic_init(void);
enum handler_return p4_clic_dispatch(unsigned int irq);
void p4_clic_route_irq(uint32_t source, uint32_t line, uint32_t cpu);
void p4_clic_init_percpu(void);
void p4_ipi_init_percpu(void);

/* Timer registers (CLINT-style, ESP32-P4) */
#define P4_MTIME_LO   (*(volatile uint32_t *)(P4_CLINT_BASE + 0xBFF8UL))
#define P4_MTIME_HI   (*(volatile uint32_t *)(P4_CLINT_BASE + 0xBFFCUL))
#define P4_MTIMECTL   (*(volatile uint32_t *)(P4_CLINT_BASE + 0x4010UL))
