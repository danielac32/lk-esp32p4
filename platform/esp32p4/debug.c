/*
 * ESP32-P4 debug/console glue for LK.
 * Early: polled UART0. After dev/uart/esp32p4 init: uses uart_* with cbuf/IRQ RX.
 */
#include <lk/reg.h>
#include <lk/init.h>
#include <platform/debug.h>
#include <platform/esp32p4.h>
#include <dev/uart.h>
#include <dev/uart/esp32p4.h>
#include <arch/ops.h>
#include <kernel/spinlock.h>
#include <stdbool.h>
#include <stdint.h>

#define UART0_BASE P4_UART0_BASE
#define UART_FIFO_REG          (UART0_BASE + 0x00)
#define UART_STATUS_REG        (UART0_BASE + 0x1C)

static bool uart_driver_ready;
static spin_lock_t debug_tx_lock = SPIN_LOCK_INITIAL_VALUE;

static const struct esp32p4_uart_config uart0_cfg = {
    .base = P4_UART0_BASE,
    .irq_source = 31, /* UART0 peripheral interrupt source */
    .irq_line = 31,   /* CLIC line (identity-mapped by unmask_interrupt) */
    .baud = 115200,
    .flags = ESP32P4_UART_FLAG_DEBUG_UART,
};

void esp32p4_platform_uart_init_early(void) {
    esp32p4_uart_init_early(0, &uart0_cfg);
}

void esp32p4_platform_uart_init(void) {
    esp32p4_uart_init(0);
    uart_driver_ready = true;
}

void platform_dputc(char c) {
    if (uart_driver_ready) {
        if (c == '\n') uart_putc(0, '\r');
        uart_putc(0, c);
        return;
    }
    arch_interrupt_saved_state_t state = spin_lock_irqsave(&debug_tx_lock);
    if (c == '\n') {
        uint32_t st;
        do { st = *REG32(UART_STATUS_REG); } while (((st >> 16) & 0xFFu) >= 120u);
        *REG32(UART_FIFO_REG) = (uint32_t)'\r';
    }
    {
        uint32_t st;
        do { st = *REG32(UART_STATUS_REG); } while (((st >> 16) & 0xFFu) >= 120u);
        *REG32(UART_FIFO_REG) = (uint32_t)c;
    }
    spin_unlock_irqrestore(&debug_tx_lock, state);
}

int platform_dgetc(char *c, bool wait) {
    if (uart_driver_ready) {
        int r = uart_getc(0, wait);
        if (r < 0) return -1;
        *c = (char)r;
        return 0;
    }
    for (;;) {
        uint32_t st = *REG32(UART_STATUS_REG);
        uint32_t rxcount = st & 0x3FFu;
        if (rxcount != 0) {
            *c = (char)(*REG32(UART_FIFO_REG) & 0xFFu);
            return 0;
        }
        if (!wait) return -1;
    }
}

void platform_pputc(char c) {
    if (c == '\n') uart_pputc(0, '\r');
    uart_pputc(0, c);
}

int platform_pgetc(char *c, bool wait) {
    (void)wait;
    int r = uart_pgetc(0);
    if (r < 0) return -1;
    *c = (char)r;
    return 0;
}

/* Enable buffered/IRQ RX once threading/interrupts are ready enough. */
static void esp32p4_uart_late_init(uint level) {
    (void)level;
    esp32p4_platform_uart_init();
}

LK_INIT_HOOK(esp32p4_uart, esp32p4_uart_late_init, LK_INIT_LEVEL_PLATFORM);
