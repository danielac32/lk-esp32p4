/*
 * AHB-DMA (canal 0, mitades OUT/IN) para I2S0 — portado de Xinu device/audio/ahbdma.
 * La construccion de descriptores la hace el llamador (audio driver).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

bool p4_ahbdma_tx_link(uint32_t first_desc);
void p4_ahbdma_tx_stop(void);
bool p4_ahbdma_tx_running(void);
uint32_t p4_ahbdma_tx_raw(void);
void p4_ahbdma_tx_clear(uint32_t mask);
uint32_t p4_ahbdma_tx_current_desc(void);

bool p4_ahbdma_in_link(uint32_t first_desc);
void p4_ahbdma_in_stop(void);
bool p4_ahbdma_in_running(void);
uint32_t p4_ahbdma_in_raw(void);
void p4_ahbdma_in_clear(uint32_t mask);
uint32_t p4_ahbdma_in_current_desc(void);
uint32_t p4_ahbdma_in_eof_desc(void);
void p4_ahbdma_in_clear_eof(void);
uint32_t p4_ahbdma_in_fifo_words(void);

#ifdef __cplusplus
}
#endif
