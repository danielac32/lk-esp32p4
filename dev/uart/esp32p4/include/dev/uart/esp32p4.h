/*
 * ESP32-P4 UART driver (LK dev/uart style).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Route RX chars into global console input buffer (lib/io console). */
#define ESP32P4_UART_FLAG_DEBUG_UART (1u << 0)

struct esp32p4_uart_config {
    uintptr_t base;
    uint32_t irq_source; /* peripheral interrupt source (INTMTX) */
    uint32_t irq_line;   /* CLIC line/vector */
    uint32_t baud;
    uint32_t flags;
};

/* Early init: clocks/pins/baud, TX enabled (polled). Safe before scheduler. */
void esp32p4_uart_init_early(int port, const struct esp32p4_uart_config *config);
/* Full init: RX buffer + IRQ handler via CLIC/INTMTX. */
void esp32p4_uart_init(int port);
