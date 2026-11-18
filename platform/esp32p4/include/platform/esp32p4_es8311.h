/*
 * Control I2C del codec ES8311 (0x18) — portado de Xinu device/audio/es8311.
 * Solo configuracion/volumen/mute/micro; el flujo PCM (I2S+DMA) es otra etapa.
 */
#pragma once

#include <platform/esp32p4_board.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define P4_ES8311_MCLK_HZ        12288000U
#define P4_ES8311_SAMPLE_RATE_HZ 48000U
#define P4_ES8311_MIC_GAIN_MAX   7U
#define P4_ES8311_MIC_PGA_MAX    10U
#define P4_ES8311_VOL_0DB_REG    191
#define P4_ES8311_VOL_STEP_DB10  5
#define P4_ES8311_VOL_MUTE_DB10  (-955)

bool p4_es8311_init(void);
bool p4_es8311_present(void);
int p4_es8311_set_volume(uint32_t percent);
int p4_es8311_get_volume(void);
int p4_es8311_get_volume_db(void);
bool p4_es8311_set_mute(bool mute);
int p4_es8311_get_mute(void);
bool p4_es8311_set_mic_gain(uint32_t gain);
int p4_es8311_get_mic_scale(void);
bool p4_es8311_start_mic(void);
bool p4_es8311_stop_mic(void);
bool p4_es8311_mic_running(void);
bool p4_es8311_set_mic_pga(uint32_t gain);
int p4_es8311_get_mic_pga(void);
int p4_es8311_get_mic_volume(void);
int p4_es8311_mic_status(void);
uint8_t p4_es8311_reg_read(uint8_t reg);
void p4_es8311_reg_write(uint8_t reg, uint8_t val);

#ifdef __cplusplus
}
#endif
