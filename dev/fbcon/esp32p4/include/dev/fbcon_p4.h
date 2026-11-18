/*
 * Control del fbcon del ESP32-P4 (ST7796 320x480 RGB565).
 * Permite a la interfaz grafica tomar la pantalla y presentarle frames.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

bool fbcon_p4_is_ready(void);
uint16_t *fbcon_p4_framebuffer(void);
unsigned fbcon_p4_width(void);
unsigned fbcon_p4_height(void);

/* Suspende el refresco del fbcon y el eco de printf a pantalla. */
/* Vuelca todo el log de arranque capturado al framebuffer y refresca el LCD.
 * Se llama tras el autorun para que la pantalla muestre el boot completo. */
void fbcon_p4_dump_full_log(void);
void fbcon_p4_suspend(void);
/* Reanuda el fbcon y fuerza un repintado. */
void fbcon_p4_resume(void);
/* Presenta un buffer RGB565 (320x480) con flip-Y; NULL = framebuffer del fbcon. */
void fbcon_p4_present(const uint16_t *pixels);
