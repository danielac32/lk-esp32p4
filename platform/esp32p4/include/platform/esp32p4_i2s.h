/*
 * I2S0 master (48kHz, 16-bit, MCLK 12.288MHz) — portado de Xinu device/audio/i2s.
 * El PCM lo mueve el AHB-DMA; aqui solo relojes/config/estado.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
bool p4_i2s_clocks_start(void);
bool p4_i2s_clocks_stop(void);
bool p4_i2s_clocks_running(void);
uint32_t p4_i2s_bclk_measure(uint32_t us);
uint32_t p4_i2s_tx_sync_count(void);
void p4_i2s_tx_sync_reset(void);
bool p4_i2s_rx_start(void);
bool p4_i2s_rx_stop(void);
bool p4_i2s_rx_running(void);
#ifdef __cplusplus
}
#endif
