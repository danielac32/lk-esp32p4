/*
 * Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 — mapa de pines.
 * Portado de Xinu include/board.h.  No son registros: solo el cableado de
 * la placa, verificado contra el esquema oficial y ESP-IDF v5.5.
 */
#pragma once

/* ==================== LCD (ST7796, 320x480, SPI2) ==================== */
#define P4_LCD_MOSI          20
#define P4_LCD_SCLK          21
#define P4_LCD_CS            23
#define P4_LCD_DC            26
#define P4_LCD_RST           27
#define P4_LCD_BL            28
#define P4_LCD_WIDTH         320
#define P4_LCD_HEIGHT        480
#define P4_LCD_PIXEL_CLOCK   80000000UL

/* ============ I2C (GPIO 7/8) — touch + audio + PMIC ============ */
#define P4_I2C_SDA           7
#define P4_I2C_SCL           8

#define P4_TOUCH_ADDR_FT6336 0x38U
#define P4_TOUCH_ADDR_GT911_A 0x5DU
#define P4_TOUCH_ADDR_GT911_B 0x14U
#define P4_ES8311_ADDR0      0x18U   /* CE low  */
#define P4_ES8311_ADDR1      0x19U   /* CE high */
#define P4_AXP2101_ADDR      0x34U   /* PMIC    */

/* Touch FPC: TP_RST activo bajo, sin pull-up: el kernel debe ponerlo en alto
 * para que el FT6336 responda en 0x38. */
#define P4_TP_RST            29
#define P4_TP_INT            50

/* ================= SD card (SDMMC SLOT0, IOMUX) ================= */
#define P4_SD_D0             39
#define P4_SD_D1             40
#define P4_SD_D2             41
#define P4_SD_D3             42
#define P4_SD_CMD            44
#define P4_SD_CLK            43
#define P4_SD_CD             P4_SD_D3

/* ================= I2S (audio ES8311 + amp + mic) ================= */
#define P4_I2S_DOUT          9
#define P4_I2S_WS            10
#define P4_I2S_DIN           11
#define P4_I2S_BCK           12
#define P4_I2S_MCK           13
#define P4_I2S_AMP_POWER     53

/* ============ ESP32-C6 (WiFi6, SDMMC SLOT1 por matriz) ============ */
#define P4_C6_SDIO_CLK       18
#define P4_C6_SDIO_CMD       19
#define P4_C6_SDIO_D0        14
#define P4_C6_SDIO_D1        15
#define P4_C6_SDIO_D2        16
#define P4_C6_SDIO_D3        17
#define P4_C6_RESET          54
#define P4_C6_IO2_STRAP      6

/* ==================== USB / sistema ==================== */
#define P4_USB_DM            45
#define P4_USB_DP            46
#define P4_BUTTON_BOOT       0
#define P4_PWR_KEY           49
