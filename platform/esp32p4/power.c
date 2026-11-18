/*
 * AXP2101 (X-Powers) en I2C0, direccion fija 0x34 — solo lecturas.
 * Portado de Xinu device/power/power.c.  Sin escrituras: los registros 0x10,
 * 0x80/0x90/0x91 pueden apagar la placa si se escriben mal.
 */
#include <platform/esp32p4_power.h>
#include <platform/esp32p4_i2c.h>
#include <stddef.h>

#define AXP_REG_STATUS1     0x00U
#define AXP_REG_STATUS2     0x01U
#define AXP_REG_CHIPID      0x03U
#define AXP_REG_VBAT_H      0x34U
#define AXP_REG_VBAT_L      0x35U
#define AXP_REG_SOC         0xA4U

#define AXP_CHIPID_VALUE    0x4AU
#define AXP_SOC_MAX         100U

static bool pmic;

static bool axp_read(uint8_t reg, uint8_t *buf, size_t len) {
    return p4_i2c_read_regs(P4_AXP2101_ADDR, reg, buf, len);
}

bool p4_power_init(void) {
    uint8_t id = 0;
    static bool i2c_ready = false;
    if (!i2c_ready) {
        p4_i2c_init();
        i2c_ready = true;
    }
    pmic = false;
    /* Leer el ChipID directamente: el probe previo deja el FSM del I2C en un
     * estado en el que la lectura siguiente falla (medido en placa). */
    if (!axp_read(AXP_REG_CHIPID, &id, 1) || AXP_CHIPID_VALUE != id) return false;
    pmic = true;
    return true;
}

bool p4_power_present(void) { return pmic; }

int p4_power_percent(void) {
    uint8_t soc;
    if (!pmic || !axp_read(AXP_REG_SOC, &soc, 1)) return -1;
    return (soc > AXP_SOC_MAX) ? (int)AXP_SOC_MAX : (int)soc;
}

bool p4_power_info(p4_power_info_t *info) {
    uint8_t status[2];
    uint8_t adc[2];
    uint8_t soc;
    if (!info || !pmic) return false;
    if (!axp_read(AXP_REG_STATUS1, status, sizeof(status)) ||
        !axp_read(AXP_REG_VBAT_H, adc, sizeof(adc)) ||
        !axp_read(AXP_REG_SOC, &soc, 1))
        return false;

    info->vbat_mv = (uint16_t)((((uint16_t)(adc[0] & 0x3FU)) << 8) | adc[1]);
    info->percent = (soc > AXP_SOC_MAX) ? AXP_SOC_MAX : soc;
    info->vbus_good = (status[0] & 0x20U) != 0;
    info->battery = (status[0] & 0x08U) != 0;
    info->system_on = (status[1] & 0x10U) != 0;
    info->charge = status[1] & 0x07U;
    info->current = (uint8_t)((status[1] >> 5) & 0x03U);
    return true;
}
