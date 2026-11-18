/*
 * Driver GPIO / IO_MUX / GPIO Matrix para ESP32-P4 (LK).
 * Basado en la lógica/registros verificados de Xinu (xinu-p4-smp/device/gpio).
 * Implementa la API estándar LK (dev/gpio.h) y extensiones ESP32-P4.
 */
#include <dev/gpio.h>
#include <platform/esp32p4.h>
#include <platform/esp32p4_gpio.h>
#include <stdint.h>
#include <stdbool.h>

/* Offsets GPIO (verificados SVD) */
#define GPIO_OUT_W1TS_REG     (P4_GPIO_BASE + 0x08)
#define GPIO_OUT_W1TC_REG     (P4_GPIO_BASE + 0x0C)
#define GPIO_OUT1_W1TS_REG    (P4_GPIO_BASE + 0x14)
#define GPIO_OUT1_W1TC_REG    (P4_GPIO_BASE + 0x18)
#define GPIO_ENABLE_W1TS_REG  (P4_GPIO_BASE + 0x24)
#define GPIO_ENABLE_W1TC_REG  (P4_GPIO_BASE + 0x28)
#define GPIO_ENABLE1_W1TS_REG (P4_GPIO_BASE + 0x30)
#define GPIO_ENABLE1_W1TC_REG (P4_GPIO_BASE + 0x34)
#define GPIO_IN_REG           (P4_GPIO_BASE + 0x3C)
#define GPIO_IN1_REG          (P4_GPIO_BASE + 0x40)
#define GPIO_CLOCK_GATE_REG   (P4_GPIO_BASE + 0x64C)

#define GPIO_OUT_SEL_CFG_REG(pin) (P4_GPIO_BASE + 0x558UL + 4UL * (uint32_t)(pin))
#define GPIO_IN_SEL_CFG_REG(sig)  (P4_GPIO_BASE + 0x158UL + 4UL * (uint32_t)(sig))
#define GPIO_PIN_REG(pin)         (P4_GPIO_BASE + 0x74UL  + 4UL * (uint32_t)(pin))

#define GPIO_OUT_SEL_MASK     0x1FFUL
#define GPIO_OUT_SEL_DEFAULT  256UL       /* OUT signal = GPIO_OUT_REG */
#define GPIO_OEN_SEL          (1UL << 10) /* OEN = GPIO_ENABLE_REG */
#define GPIO_PIN_PAD_DRIVER   (1UL << 2)

#define GPIO_MAT_OUT_SEL_MASK    0x1FFUL
#define GPIO_MAT_OUT_INV_SEL     (1UL << 9)
#define GPIO_MAT_OUT_OEN_SEL     (1UL << 10)
#define GPIO_MAT_OUT_OEN_INV_SEL (1UL << 11)

#define GPIO_MAT_IN_SEL_MASK     0x3FUL
#define GPIO_MAT_IN_INV_SEL      (1UL << 6)
#define GPIO_MAT_IN_SIG_IN_SEL   (1UL << 7)

#define GPIO_PIN_MAX   54U
#define IO_MUX_PIN_MAX 54U

static inline volatile uint32_t *reg32(uintptr_t addr) {
    return (volatile uint32_t *)addr;
}

void p4_gpio_init(void) {
    *reg32(GPIO_CLOCK_GATE_REG) = 1;
}

void p4_gpio_set_dir(unsigned pin, bool output) {
    if (pin > GPIO_PIN_MAX) return;

    if (pin <= IO_MUX_PIN_MAX) {
        volatile uint32_t *pad = reg32(P4_IO_MUX_PIN_REG(pin));
        uint32_t val = *pad;
        val &= ~(P4_IO_MCU_SEL_MASK | P4_IO_FUN_IE | P4_IO_FUN_DRV_MASK);
        val |= P4_IO_MCU_SEL_GPIO | P4_IO_FUN_DRV_20MA;
        if (!output) {
            val |= P4_IO_FUN_IE;
        }
        *pad = val;
    }

    if (output) {
        volatile uint32_t *out_sel = reg32(GPIO_OUT_SEL_CFG_REG(pin));
        *out_sel = (*out_sel & ~GPIO_OUT_SEL_MASK) | GPIO_OUT_SEL_DEFAULT | GPIO_OEN_SEL;
        if (pin < 32) {
            *reg32(GPIO_ENABLE_W1TS_REG) = 1UL << pin;
        } else {
            *reg32(GPIO_ENABLE1_W1TS_REG) = 1UL << (pin - 32);
        }
    } else {
        if (pin < 32) {
            *reg32(GPIO_ENABLE_W1TC_REG) = 1UL << pin;
        } else {
            *reg32(GPIO_ENABLE1_W1TC_REG) = 1UL << (pin - 32);
        }
    }
}

void p4_gpio_set_pull(unsigned pin, bool pullup, bool pulldown) {
    if (pin > IO_MUX_PIN_MAX) return;
    volatile uint32_t *pad = reg32(P4_IO_MUX_PIN_REG(pin));
    uint32_t val = *pad & ~(P4_IO_FUN_WPU | P4_IO_FUN_WPD);
    if (pullup) val |= P4_IO_FUN_WPU;
    if (pulldown) val |= P4_IO_FUN_WPD;
    *pad = val;
}

void p4_gpio_set_open_drain(unsigned pin, bool enable) {
    if (pin > GPIO_PIN_MAX) return;
    volatile uint32_t *cfg = reg32(GPIO_PIN_REG(pin));
    uint32_t v = *cfg;
    if (enable) v |= GPIO_PIN_PAD_DRIVER;
    else v &= ~GPIO_PIN_PAD_DRIVER;
    *cfg = v;
}

void p4_gpio_matrix_out(unsigned pin, uint32_t sig, bool out_inv, bool oen_inv) {
    if (pin > GPIO_PIN_MAX) return;
    volatile uint32_t *r = reg32(GPIO_OUT_SEL_CFG_REG(pin));
    uint32_t v = *r;
    v &= ~(GPIO_MAT_OUT_SEL_MASK | GPIO_MAT_OUT_INV_SEL |
           GPIO_MAT_OUT_OEN_SEL  | GPIO_MAT_OUT_OEN_INV_SEL);
    v |= (sig & GPIO_MAT_OUT_SEL_MASK);
    if (out_inv) v |= GPIO_MAT_OUT_INV_SEL;
    if (oen_inv) v |= GPIO_MAT_OUT_OEN_INV_SEL;
    *r = v;
}

void p4_gpio_matrix_in(unsigned pin, uint32_t sig, bool inv) {
    volatile uint32_t *r = reg32(GPIO_IN_SEL_CFG_REG(sig));
    uint32_t v;
    if (pin == P4_GPIO_MAT_CONST_ZERO) {
        v = GPIO_MAT_IN_SIG_IN_SEL;
    } else if (pin == P4_GPIO_MAT_CONST_ONE) {
        v = GPIO_MAT_IN_SIG_IN_SEL | 1UL;
    } else {
        v = GPIO_MAT_IN_SIG_IN_SEL | ((uint32_t)pin & GPIO_MAT_IN_SEL_MASK);
        if (inv) v |= GPIO_MAT_IN_INV_SEL;
    }
    *r = v;
}

/* --- API estándar LK (dev/gpio.h) --- */

int gpio_config(unsigned nr, unsigned flags) {
    if (nr > GPIO_PIN_MAX) return -1;
    p4_gpio_init();

    bool output = (flags & GPIO_OUTPUT) != 0;
    p4_gpio_set_dir(nr, output);
    p4_gpio_set_pull(nr, (flags & GPIO_PULLUP) != 0, (flags & GPIO_PULLDOWN) != 0);
    return 0;
}

void gpio_set(unsigned nr, unsigned on) {
    if (nr > GPIO_PIN_MAX) return;
    if (nr < 32) {
        *reg32(on ? GPIO_OUT_W1TS_REG : GPIO_OUT_W1TC_REG) = 1UL << nr;
    } else {
        *reg32(on ? GPIO_OUT1_W1TS_REG : GPIO_OUT1_W1TC_REG) = 1UL << (nr - 32);
    }
}

int gpio_get(unsigned nr) {
    if (nr > GPIO_PIN_MAX) return -1;
    uint32_t reg = (nr < 32) ? *reg32(GPIO_IN_REG) : *reg32(GPIO_IN1_REG);
    return (int)((reg >> (nr & 31)) & 1U);
}
