/*
 * FT6336U (touch capacitivo) en I2C0 0x38 — portado de Xinu device/touch.
 * Bloque de reporte compatible FT5x06: TD_STATUS 0x02 (bajo 4 bits = #puntos),
 * cada punto 6 bytes desde 0x03 (XH XL YH YL WEIGHT MISC).
 */
#include <platform/esp32p4_touch.h>
#include <platform/esp32p4_i2c.h>
#include <platform/esp32p4_gpio.h>
#include <dev/gpio.h>
#include <platform/time.h>

#define FT_TD_STATUS        0x02U
#define FT_POINT_STRIDE     6U
#define FT_RST_HOLD_US      (20U * 1000U)
#define FT_RST_SETTLE_US    (50U * 1000U)

static p4_touch_type_t touchCtrl = P4_TOUCH_NONE;
static uint8_t touchAddr;

bool p4_touch_init(void) {
    static bool i2c_ready = false;
    if (!i2c_ready) { p4_i2c_init(); i2c_ready = true; }

    touchCtrl = P4_TOUCH_NONE;
    touchAddr = 0;

    /* El modulo no trae pull-up en RST: tras un reset el pad queda bajo y el
     * FT6336 no responde en 0x38 hasta pulsar RST (medido en Xinu). */
    p4_gpio_set_dir(P4_TP_RST, true);
    gpio_set(P4_TP_RST, 0);
    spin(FT_RST_HOLD_US);
    gpio_set(P4_TP_RST, 1);
    spin(FT_RST_SETTLE_US);

    /* Leer ChipID-like register 0x00 del FT6336 (debe responder en 0x38).
     * No usamos p4_i2c_probe() porque deja el FSM en mal estado (medido). */
    uint8_t id = 0;
    if (p4_i2c_read_regs(P4_TOUCH_ADDR_FT6336, 0x00, &id, 1)) {
        touchAddr = P4_TOUCH_ADDR_FT6336;
        touchCtrl = P4_TOUCH_FT6336;
        return true;
    }
    return false;
}

p4_touch_type_t p4_touch_type(void) { return touchCtrl; }

int p4_touch_read(p4_touch_point_t *pts, int cap) {
    uint8_t raw[1 + P4_TOUCH_MAX_POINTS * FT_POINT_STRIDE];
    uint8_t reported, want;
    if (!pts || cap <= 0 || touchCtrl == P4_TOUCH_NONE) return 0;
    want = (cap > P4_TOUCH_MAX_POINTS) ? P4_TOUCH_MAX_POINTS : (uint8_t)cap;
    if (!p4_i2c_read_regs(touchAddr, FT_TD_STATUS, raw, 1U + FT_POINT_STRIDE * want)) return 0;

    reported = raw[0] & 0x0FU;
    if (reported > want) reported = want;
    for (uint8_t i = 0; i < reported; i++) {
        const uint8_t *p = &raw[1 + FT_POINT_STRIDE * i];
        pts[i].x = (uint16_t)(((p[0] & 0x0FU) << 8) | p[1]);
        pts[i].y = (uint16_t)(((p[2] & 0x0FU) << 8) | p[3]);
        pts[i].id = (uint8_t)((p[2] >> 4) & 0x0FU);
    }
    return reported;
}
