/*
 * Driver minimo ST7796 (SPI2, 320x480) — portado de Xinu device/lcd-esp32p4.
 * init + fill de pantalla (sin framebuffer, sin fuentes, sin LVGL).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
bool p4_lcd_init(void);
bool p4_lcd_fill(uint16_t color);
bool p4_lcd_blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *pixels);
bool p4_lcd_blit_flipy(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *pixels);
void p4_lcd_text(const char *s, uint16_t x, uint16_t y, uint16_t fg, uint16_t bg);
void p4_lcd_set_rot(uint8_t rot);   /* 0=0x08 2=0xC8 4=0x48 6=0x88 */
bool p4_lcd_test(void);   /* 8 barras de colores */
#ifdef __cplusplus
}
#endif
