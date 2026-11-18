#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <dev/wifi/esp_hosted.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t slot;
    uint8_t bus_width;
    uint32_t clock_khz;
    int pin_clk, pin_cmd, pin_d0, pin_d1, pin_d2, pin_d3, pin_reset;
    bool reset_active_low;
    uint16_t reset_pulse_ms;
    uint16_t post_reset_delay_ms;
    bool use_internal_pullups;
} esp_hosted_sdio_config_t;

void esp_hosted_sdio_get_default_config(esp_hosted_sdio_config_t *config);
bool esp_hosted_sdio_attach(const esp_hosted_sdio_config_t *config);
void esp_hosted_sdio_detach(void);
bool esp_hosted_sdio_is_attached(void);

#ifdef __cplusplus
}
#endif
