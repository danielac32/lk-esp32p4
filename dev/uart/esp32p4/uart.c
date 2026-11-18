/*
 * ESP32-P4 UART driver for LK.
 * Uses cbuf for RX, register_int_handler + CLIC/INTMTX for IRQ RX.
 * HW register programming based on ESP32-P4 UART (Xinu bring-up reference).
 */
#include <dev/uart/esp32p4.h>
#include <dev/uart.h>

#include <assert.h>
#include <lk/debug.h>
#include <lk/reg.h>
#include <lib/cbuf.h>
#include <platform/interrupts.h>
#include <platform/esp32p4.h>
#include <arch/ops.h>
#include <kernel/spinlock.h>

#if CONSOLE_HAS_INPUT_BUFFER
#include <lib/io.h>
#endif

#define NUM_UART 1
#define RXBUF_SIZE 128

/* ESP32-P4 UART register offsets (UART0 base 0x500CA000) */
#define UART_FIFO_REG       0x00
#define UART_INT_RAW_REG    0x04
#define UART_INT_ST_REG     0x08
#define UART_INT_ENA_REG    0x0C
#define UART_INT_CLR_REG    0x10
#define UART_CLKDIV_REG     0x14
#define UART_STATUS_REG     0x1C
#define UART_CONF0_REG      0x20
#define UART_CONF1_REG      0x24
#define UART_CLK_CONF_REG   0x88
#define UART_TOUT_CONF_REG  0x64
#define UART_REG_UPDATE_REG 0x98

/* INT bits */
#define UART_RXFIFO_FULL_INT  (1u << 0)
#define UART_RXFIFO_TOUT_INT  (1u << 8)
#define UART_RXFIFO_OVF_INT   (1u << 4)
#define UART_BRK_DET_INT      (1u << 7)

/* STATUS fields */
#define UART_RXFIFO_CNT_MASK  0x3FFu
#define UART_TXFIFO_CNT_SHIFT 16
#define UART_TXFIFO_CNT_MASK  0x3FFu

/* Clock/reset and pinmux (ESP32-P4) */
#define SYS_CLKRST_BASE        0x500E6000u
#define HP_SYS_CLKRST_SOC_CLK_CTRL2_REG   (SYS_CLKRST_BASE + 0x18)
#define HP_SYS_CLKRST_SOC_CLK_CTRL3_REG   (SYS_CLKRST_BASE + 0x1C)
#define HP_SYS_CLKRST_PERI_CLK_CTRL25_REG (SYS_CLKRST_BASE + 0x128)

#define P4_GPIO_BASE        0x500E0000UL
#define P4_IO_MUX_BASE      0x500E1000UL
#define P4_UART0_RXD_IN_IDX 10
#define P4_UART0_RXD_PAD    38
#define P4_GPIO_IN_SEL(i)   (*(volatile uint32_t *)(P4_GPIO_BASE + 0x158UL + 4UL * (i)))
#define P4_IOMUX_PAD(n)     (*(volatile uint32_t *)(P4_IO_MUX_BASE + 4UL + 4UL * (n)))

struct esp32p4_uart {
    bool initialized;
    bool irq_enabled;
    struct esp32p4_uart_config config;
    cbuf_t rxbuf;
};

static struct esp32p4_uart uarts[NUM_UART];
static spin_lock_t uart_tx_lock = SPIN_LOCK_INITIAL_VALUE;

static inline uint32_t rd(uintptr_t base, uint32_t off) { return *REG32(base + off); }
static inline void wr(uintptr_t base, uint32_t off, uint32_t v) { *REG32(base + off) = v; }

static enum handler_return esp32p4_uart_irq(void *arg) {
    struct esp32p4_uart *u = (struct esp32p4_uart *)arg;
    uintptr_t base = u->config.base;
    uint32_t st = rd(base, UART_INT_ST_REG);
    bool resched = false;

    if (st == 0) {
        return INT_NO_RESCHEDULE;
    }

    /* Drenar siempre el FIFO RX si hay datos pendientes (robustez ante timeout/flags) */
    while ((rd(base, UART_STATUS_REG) & UART_RXFIFO_CNT_MASK) != 0) {
        char c = (char)(rd(base, UART_FIFO_REG) & 0xFFu);
#if CONSOLE_HAS_INPUT_BUFFER
        if (u->config.flags & ESP32P4_UART_FLAG_DEBUG_UART) {
            unsigned char uc = (unsigned char)c;
            /* drop NUL/0xFF (break/noise) que ensucian la línea de consola */
            if (uc != 0x00 && uc != 0xFF) {
                cbuf_write_char(&console_input_cbuf, c, false);
            }
        } else
#endif
        {
            if (cbuf_space_avail(&u->rxbuf) != 0) {
                cbuf_write_char(&u->rxbuf, c, false);
            }
        }
        resched = true;
    }

    /* Limpiar todas las interrupciones reportadas */
    wr(base, UART_INT_CLR_REG, st);
    return resched ? INT_RESCHEDULE : INT_NO_RESCHEDULE;
}

void esp32p4_uart_init_early(int port, const struct esp32p4_uart_config *config) {
    if (port < 0 || port >= NUM_UART || !config) return;
    struct esp32p4_uart *u = &uarts[port];
    u->config = *config;
    uintptr_t base = u->config.base;

    /* System/APB clocks for UART0 */
    *REG32(HP_SYS_CLKRST_SOC_CLK_CTRL2_REG) |= (1u << 18);
    *REG32(HP_SYS_CLKRST_SOC_CLK_CTRL3_REG) |= (1u << 7);
    *REG32(HP_SYS_CLKRST_PERI_CLK_CTRL25_REG) |= (1u << 26);

    /* Enable TX/RX sclk in UART clock domain */
    wr(base, UART_CLK_CONF_REG, rd(base, UART_CLK_CONF_REG) | (1u << 24) | (1u << 25));

    /* CONF0: 8N1 + mem_clk_en */
    uint32_t conf0 = rd(base, UART_CONF0_REG);
    conf0 = (conf0 & ~(3u << 2)) | (3u << 2) | (1u << 20);
    wr(base, UART_CONF0_REG, conf0);

    /* Baud: 115200 @ ~40MHz source (matches existing bring-up: div 347) */
    wr(base, UART_CLKDIV_REG, 347);

    /* CONF1: RX full threshold + clk_en */
    uint32_t conf1 = rd(base, UART_CONF1_REG);
    conf1 = (conf1 & ~0xFFFFu) | 8u | (1u << 21);
    wr(base, UART_CONF1_REG, conf1);

    /* RX timeout */
    wr(base, UART_TOUT_CONF_REG, (10u << 2) | 1u);
    wr(base, UART_REG_UPDATE_REG, 1u);

    /* RX pinmux: GPIO38 -> UART0 RX */
    uint32_t pad = P4_IOMUX_PAD(P4_UART0_RXD_PAD);
    pad &= ~((7u << 12) | (1u << 15));
    pad |= (1u << 9); /* input enable */
    P4_IOMUX_PAD(P4_UART0_RXD_PAD) = pad;
    P4_GPIO_IN_SEL(P4_UART0_RXD_IN_IDX) = P4_UART0_RXD_PAD;

    wr(base, UART_INT_ENA_REG, 0);
    wr(base, UART_INT_CLR_REG, 0xFFFFFFFFu);

    u->initialized = true;
}

void esp32p4_uart_init(int port) {
    if (port < 0 || port >= NUM_UART) return;
    struct esp32p4_uart *u = &uarts[port];
    if (!u->initialized) return;

    cbuf_initialize(&u->rxbuf, RXBUF_SIZE);

    uintptr_t base = u->config.base;
    wr(base, UART_INT_CLR_REG, 0xFFFFFFFFu);

    register_int_handler(u->config.irq_line, esp32p4_uart_irq, u);
    /* Route peripheral source to CLIC line (CPU0) and unmask */
    p4_clic_route_irq(u->config.irq_source, u->config.irq_line, 0);
    unmask_interrupt(u->config.irq_line);

    /* Enable RX interrupts */
    uint32_t ena = UART_RXFIFO_FULL_INT | UART_RXFIFO_TOUT_INT | UART_RXFIFO_OVF_INT | UART_BRK_DET_INT;
    wr(base, UART_INT_ENA_REG, ena);

    u->irq_enabled = true;
}

int uart_putc(int port, char c) {
    if (port < 0 || port >= NUM_UART || !uarts[port].initialized) return -1;
    uintptr_t base = uarts[port].config.base;
    arch_interrupt_saved_state_t state = spin_lock_irqsave(&uart_tx_lock);
    while (((rd(base, UART_STATUS_REG) >> UART_TXFIFO_CNT_SHIFT) & UART_TXFIFO_CNT_MASK) >= 120u) {}
    wr(base, UART_FIFO_REG, (uint32_t)(unsigned char)c);
    spin_unlock_irqrestore(&uart_tx_lock, state);
    return 1;
}

int uart_getc(int port, bool wait) {
    if (port < 0 || port >= NUM_UART || !uarts[port].initialized) return -1;
    struct esp32p4_uart *u = &uarts[port];
    char c;
    if (u->irq_enabled) {
        if (cbuf_read_char(&u->rxbuf, &c, wait) == 1) return (int)(unsigned char)c;
        return -1;
    }
    /* polled fallback */
    for (;;) {
        if ((rd(u->config.base, UART_STATUS_REG) & UART_RXFIFO_CNT_MASK) != 0) {
            c = (char)(rd(u->config.base, UART_FIFO_REG) & 0xFFu);
            return (int)(unsigned char)c;
        }
        if (!wait) return -1;
    }
}

int uart_pputc(int port, char c) {
    if (port < 0 || port >= NUM_UART || !uarts[port].initialized) return -1;
    uintptr_t base = uarts[port].config.base;
    while (((rd(base, UART_STATUS_REG) >> UART_TXFIFO_CNT_SHIFT) & UART_TXFIFO_CNT_MASK) >= 120u) {}
    wr(base, UART_FIFO_REG, (uint32_t)(unsigned char)c);
    return 1;
}

int uart_pgetc(int port) {
    if (port < 0 || port >= NUM_UART || !uarts[port].initialized) return -1;
    uintptr_t base = uarts[port].config.base;
    if ((rd(base, UART_STATUS_REG) & UART_RXFIFO_CNT_MASK) == 0) return -1;
    return (int)(rd(base, UART_FIFO_REG) & 0xFFu);
}

void uart_flush_tx(int port) { (void)port; }
void uart_flush_rx(int port) { (void)port; }

void uart_init_port(int port, uint baud) { (void)port; (void)baud; }
void uart_init(void) {}
void uart_init_early(void) {}
