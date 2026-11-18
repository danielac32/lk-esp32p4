/*
 * Driver del touch FT6336U (I2C0, 0x38) — portado de Xinu device/touch.
 */
#pragma once

#include <platform/esp32p4_board.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    P4_TOUCH_NONE = 0,
    P4_TOUCH_FT6336,
} p4_touch_type_t;

typedef struct {
    uint16_t x;
    uint16_t y;
    uint8_t id;
} p4_touch_point_t;

#define P4_TOUCH_MAX_POINTS 5

bool p4_touch_init(void);
p4_touch_type_t p4_touch_type(void);
int p4_touch_read(p4_touch_point_t *pts, int cap);

#ifdef __cplusplus
}
#endif
