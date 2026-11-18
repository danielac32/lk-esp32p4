/*
 * I2C0 maestro para ESP32-P4 (portado de Xinu device/i2c).
 * Pines por defecto: SDA=7, SCL=8 (matriz GPIO, senales 69/68).
 * Bus a 100 kHz, polling con timeouts (sin IRQ, sin DMA).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <platform/esp32p4_board.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pines del bus definidos en esp32p4_board.h (P4_I2C_SDA / P4_I2C_SCL) */

/* Motivo del ultimo error de transaccion */
#define P4_I2C_ERR_NONE          0
#define P4_I2C_ERR_NACK          1
#define P4_I2C_ERR_BUS_TIMEOUT   2
#define P4_I2C_ERR_ARBITRATION   3
#define P4_I2C_ERR_STUCK         4

status_t p4_i2c_init(void);
bool p4_i2c_probe(uint8_t dev);
bool p4_i2c_write(uint8_t dev, const uint8_t *data, size_t len);
bool p4_i2c_read(uint8_t dev, uint8_t *data, size_t len);
bool p4_i2c_write_reg8(uint8_t dev, uint8_t reg, uint8_t val);
bool p4_i2c_read_regs(uint8_t dev, uint8_t reg, uint8_t *buf, size_t len);
bool p4_i2c_write_reg16(uint8_t dev, uint16_t reg, uint8_t val);
bool p4_i2c_read_regs16(uint8_t dev, uint16_t reg, uint8_t *buf, size_t len);
int p4_i2c_last_error(void);

#ifdef __cplusplus
}
#endif
