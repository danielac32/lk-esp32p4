/*
 * Audio PCM: anillos de reproduccion/captura sobre AHB-DMA + I2S0.
 * Portado de Xinu device/audio/audioOut.c + audioIn.c.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Reproduccion (stream) */
bool p4_audio_tone(uint32_t hz, uint32_t ms);       /* tono de prueba */
bool p4_audio_stream_start(void);
uint32_t p4_audio_stream_write(const uint8_t *buf, uint32_t len);
bool p4_audio_stream_drain(void);
uint32_t p4_audio_stream_queued(void);
void p4_audio_stream_stop(void);

/* Reproduccion de un buffer PCM ya en memoria (48kHz/16-bit estereo) */
bool p4_audio_play(const uint8_t *pcm, uint32_t len);

/* Captura */
bool p4_audio_in_start(void);
uint32_t p4_audio_in_read(uint8_t *buf, uint32_t len);
void p4_audio_in_stop(void);
uint32_t p4_audio_in_dropped(void);

/* estado */
uint32_t p4_audio_out_sync_count(void);
bool p4_audio_clocks_running(void);
void p4_audio_status(void);

#ifdef __cplusplus
}
#endif
