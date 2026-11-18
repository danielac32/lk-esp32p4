/*
 * Driver del AXP2101 (PMIC de la placa Waveshare) — solo lecturas.
 * Portado de Xinu device/power/power.h.  Direccion I2C fija 0x34.
 */
#pragma once

#include <platform/esp32p4_board.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t percent;     /* 0xA4: carga de bateria, 0..100 */
    uint16_t vbat_mv;    /* ADC canal 0: 1 mV por LSB */
    bool vbus_good;      /* 0x00[5]: VBUS valida */
    bool battery;        /* 0x00[3]: bateria conectada */
    bool system_on;      /* 0x01[4]: sistema encendido */
    uint8_t charge;      /* 0x01[2:0]: estado del cargador */
    uint8_t current;     /* 0x01[6:5]: direccion de corriente */
} p4_power_info_t;

#define P4_POWER_CHG_TRICKLE  0
#define P4_POWER_CHG_FAST     2
#define P4_POWER_CHG_ABSORB   3
#define P4_POWER_CHG_DONE     4
#define P4_POWER_CHG_NONE     5

#define P4_POWER_CUR_STANDBY  0
#define P4_POWER_CUR_CHARGE   1
#define P4_POWER_CUR_DISCHARGE 2

bool p4_power_init(void);
bool p4_power_present(void);
int p4_power_percent(void);
bool p4_power_info(p4_power_info_t *info);

#ifdef __cplusplus
}
#endif
