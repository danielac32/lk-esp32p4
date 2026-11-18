#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SDIO_SLOT 1U

/* Pines C6 (Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5) */
#define C6_SDIO_CLK 18
#define C6_SDIO_CMD 19
#define C6_SDIO_D0  14
#define C6_SDIO_D1  15
#define C6_SDIO_D2  16
#define C6_SDIO_D3  17
#define C6_RESET    54

struct sdio_info {
    bool up;
    uint32_t ocr;
    uint32_t rca;
    uint8_t cio_enable;
    uint8_t cio_ready;
    uint8_t sdio_ver;
    uint8_t fn0_info[8];
    uint16_t blksize;
    uint32_t clk_khz;
};

status_t sdio_enumerate(struct sdio_info *out);
void sdio_probe_report(const struct sdio_info *info);

#ifdef __cplusplus
}
#endif
