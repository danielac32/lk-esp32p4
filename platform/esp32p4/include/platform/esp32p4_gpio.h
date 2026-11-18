/*
 * Extensiones GPIO para ESP32-P4 (IO_MUX, matrix, pull, open-drain).
 * Complementa dev/include/dev/gpio.h (API LK estándar).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Direcciones de registro IO_MUX (pin n) */
#define P4_IO_MUX_PIN_REG(pin)   (P4_IO_MUX_BASE + 4UL + 4UL * (uint32_t)(pin))

/* Bits IO_MUX */
#define P4_IO_FUN_WPD            (1UL << 7)
#define P4_IO_FUN_WPU            (1UL << 8)
#define P4_IO_FUN_IE             (1UL << 9)
#define P4_IO_FUN_DRV_MASK       (3UL << 10)
#define P4_IO_FUN_DRV_20MA       (2UL << 10)
#define P4_IO_FUN_DRV_40MA       (3UL << 10)
#define P4_IO_MCU_SEL_MASK       (7UL << 12)
#define P4_IO_MCU_SEL_GPIO       (1UL << 12) /* PIN_FUNC_GPIO */
#define P4_IO_MCU_SEL_DIRECT     (0UL << 12) /* función IOMUX directa */

/* GPIO matrix constantes */
#define P4_GPIO_MAT_CONST_ZERO   0x3EU
#define P4_GPIO_MAT_CONST_ONE    0x3FU

void p4_gpio_init(void);

void p4_gpio_set_dir(unsigned pin, bool output);
void p4_gpio_set_pull(unsigned pin, bool pullup, bool pulldown);
void p4_gpio_set_open_drain(unsigned pin, bool enable);

void p4_gpio_matrix_out(unsigned pin, uint32_t sig, bool out_inv, bool oen_inv);
void p4_gpio_matrix_in(unsigned pin, uint32_t sig, bool inv);

#ifdef __cplusplus
}
#endif
