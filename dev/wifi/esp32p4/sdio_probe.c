/*
 * SDIO probe/enumeración del ESP32-C6 (slot 1) sobre el SDHOST compartido.
 * Portado/adaptado desde xinu-p4-smp/device/sdio-esp32p4/sdioProbe.c a LK.
 */
#include <dev/wifi/sdio.h>
#include "sdmmc_internal.h"

#include <platform/esp32p4.h>
#include <platform/esp32p4_gpio.h>
#include <dev/gpio.h>
#include <lk/debug.h>
#include <lk/err.h>
#include <stdio.h>
#include <string.h>
#include <kernel/thread.h>
#include <platform.h>
/* SDHOST register offsets (subset) */
#define H_CLKENA    0x010U
#define H_CTYPE     0x018U
#define H_CLKDIV    0x008U
#define H_CLKSRC    0x00CU
#define H_RINTSTS   0x044U
#define H_STATUS    0x048U

#define SDIO_SIG_CLK 0U
#define SDIO_SIG_CMD 1U
#define SDIO_SIG_D0  2U
#define SDIO_SIG_D1  3U
#define SDIO_SIG_D2  4U
#define SDIO_SIG_D3  5U

#define SDIO_CMD0_GO_IDLE      0U
#define SDIO_CMD3_SET_RCA      3U
#define SDIO_CMD5_IO_SEND      5U
#define SDIO_CMD7_SELECT       7U
#define SDIO_CMD52_RDWR_DIRECT 52U

#define SDIO_CMD5_ARG    0x00FF8000UL
#define SDIO_R4_IO_READY (1UL << 31)

#define CCCR_IO_ENABLE   0x02U
#define CCCR_IO_READY    0x03U
#define CCCR_CARD_INFO   0x08U
#define CCCR_FN0_INFO    0x0CU
#define CCCR_BLKSIZE_L   0x10U
#define CCCR_BLKSIZE_H   0x11U
#define CCCR_FN1_EN      (1U << 1)

#define F_CTYPE_W4_CARD1 (1UL << SDIO_SLOT)

static void delay_ms(unsigned ms) { thread_sleep(ms); }

static void sdio_pad_to_matrix(uint8_t pin, bool pullup) {
    if (pin <= 15U) {
        volatile uint32_t *lp = (volatile uint32_t *)(P4_IO_MUX_BASE - 0x1000UL /* LP_IOMUX_BASE? */);
        /* LP_IOMUX_BASE 0x5012B000, pad n at +0x08+4*n, MUX_SEL bit 4 */
        volatile uint32_t *lpreg = (volatile uint32_t *)(0x5012B000UL + 0x08UL + 4UL * pin);
        *lpreg &= ~(1UL << 4);
        (void)lp;
    }
    volatile uint32_t *pad = (volatile uint32_t *)(P4_IO_MUX_BASE + 4UL + 4UL * pin);
    uint32_t val = *pad;
    val &= ~(P4_IO_MCU_SEL_MASK | P4_IO_FUN_WPD | P4_IO_FUN_WPU | P4_IO_FUN_DRV_MASK);
    val |= P4_IO_MCU_SEL_GPIO | P4_IO_FUN_IE | P4_IO_FUN_DRV_40MA;
    if (pullup) val |= P4_IO_FUN_WPU;
    *pad = val;
}

static void sdio_route(uint8_t pin, uint32_t sig, bool isClk, bool pullup) {
    sdio_pad_to_matrix(pin, pullup);
    p4_gpio_matrix_out(pin, sig, false, false);
    if (!isClk) {
        p4_gpio_matrix_in(pin, sig, false);
    }
}

static void sdio_pads_init(void) {
    sdio_route(C6_SDIO_CLK, SDIO_SIG_CLK, true,  false);
    sdio_route(C6_SDIO_CMD, SDIO_SIG_CMD, false, true);
    sdio_route(C6_SDIO_D0,  SDIO_SIG_D0,  false, true);
    sdio_route(C6_SDIO_D1,  SDIO_SIG_D1,  false, true);
    sdio_route(C6_SDIO_D2,  SDIO_SIG_D2,  false, true);
    sdio_route(C6_SDIO_D3,  SDIO_SIG_D3,  false, true);
}

static void sdio_reset_c6(void) {
    p4_gpio_init();
    p4_gpio_set_dir(C6_RESET, true);
    gpio_set(C6_RESET, 1);
    delay_ms(5);
    gpio_set(C6_RESET, 0);
    delay_ms(100);
    gpio_set(C6_RESET, 1);
    delay_ms(1500);
}

static bool sdio_cmd52(uint8_t fn, uint32_t reg, bool write, uint8_t *data) {
    uint32_t arg = ((uint32_t)write << 31) | ((uint32_t)fn << 28)
                 | ((reg & 0x1FFFFUL) << 9) | (uint32_t)(*data);
    uint32_t resp = 0;
    if (!sd_send_cmd_slot(SDIO_SLOT, SDIO_CMD52_RDWR_DIRECT, arg, 1, true, false, &resp))
        return false;
    *data = (uint8_t)(resp & 0xFFU);
    return true;
}

static bool sdio_go_idle_ready(uint32_t *ocr) {
    uint32_t resp = 0;
    for (int tries = 0; tries < 5; tries++) {
        if (sd_send_cmd_slot(SDIO_SLOT, SDIO_CMD0_GO_IDLE, 0, 0, false, true, NULL)) break;
        delay_ms(2);
    }
    (void)sd_send_cmd_slot(SDIO_SLOT, SDIO_CMD5_IO_SEND, 0, 1, false, false, &resp);

    lk_time_t t0 = current_time();
    do {
        if (sd_send_cmd_slot(SDIO_SLOT, SDIO_CMD5_IO_SEND, SDIO_CMD5_ARG, 1, false, false, &resp)
            && (resp & SDIO_R4_IO_READY) != 0) {
            *ocr = resp;
            return true;
        }
        delay_ms(1);
    } while ((current_time() - t0) < 1000);
    return false;
}

static bool sdio_set_rca_select(uint32_t *rca) {
    uint32_t resp = 0;
    if (!sd_send_cmd_slot(SDIO_SLOT, SDIO_CMD3_SET_RCA, 0, 1, true, false, &resp)) return false;
    *rca = (resp >> 16) & 0xFFFFUL;
    if (*rca == 0) return false;
    return sd_send_cmd_slot(SDIO_SLOT, SDIO_CMD7_SELECT, (*rca << 16), 1, true, false, NULL);
}

static bool sdio_enable_fn1(uint8_t *cio_enable, uint8_t *cio_ready) {
    uint8_t val = 0;
    if (!sdio_cmd52(0, CCCR_IO_ENABLE, false, &val)) return false;
    val |= CCCR_FN1_EN;
    if (!sdio_cmd52(0, CCCR_IO_ENABLE, true, &val)) return false;
    val = 0;
    (void)sdio_cmd52(0, CCCR_IO_ENABLE, false, &val);
    *cio_enable = val;

    lk_time_t t0 = current_time();
    do {
        val = 0;
        if (sdio_cmd52(0, CCCR_IO_READY, false, &val) && (val & CCCR_FN1_EN)) {
            *cio_ready = val;
            return true;
        }
        delay_ms(10);
    } while ((current_time() - t0) < 2500);
    *cio_ready = val;
    return false;
}

static void sdio_read_ids(struct sdio_info *info) {
    uint8_t b = 0, lo = 0, hi = 0;
    (void)sdio_cmd52(0, CCCR_CARD_INFO, false, &b); info->sdio_ver = b;
    for (int i = 0; i < 8; i++) {
        b = 0; (void)sdio_cmd52(0, (uint32_t)(CCCR_FN0_INFO + i), false, &b);
        info->fn0_info[i] = b;
    }
    (void)sdio_cmd52(0, CCCR_BLKSIZE_L, false, &lo);
    (void)sdio_cmd52(0, CCCR_BLKSIZE_H, false, &hi);
    info->blksize = (uint16_t)lo | ((uint16_t)hi << 8);
}

status_t sdio_enumerate(struct sdio_info *out) {
    if (!out) return ERR_INVALID_ARGS;
    memset(out, 0, sizeof(*out));
    if (!sd_host_ready) return ERR_NOT_READY;

    sdmmc_bus_lock();
    volatile uint32_t *reg = sd_host_reg_base();
    reg[H_CTYPE / 4] &= ~F_CTYPE_W4_CARD1;

    sdio_pads_init();
    sdio_reset_c6();
    sd_bus_state("1 pads+reset", SDIO_SLOT);

    if (!sd_set_clock_slot_no_wait(SDIO_SLOT, SD_CARD_DIV_PROBE)) goto fail;
    sd_bus_state("2 reloj", SDIO_SLOT);

    if (!sdio_go_idle_ready(&out->ocr)) goto fail;
    sd_bus_state("3 CMD0+CMD5", SDIO_SLOT);

    if (!sdio_set_rca_select(&out->rca)) goto fail;
    sd_bus_state("4 CMD3+CMD7", SDIO_SLOT);

    if (!sdio_enable_fn1(&out->cio_enable, &out->cio_ready)) goto fail;
    sd_bus_state("5 fn1", SDIO_SLOT);

    sdio_read_ids(out);
    out->clk_khz = sd_clk_khz;
    out->up = true;
    sdmmc_bus_unlock();
    return NO_ERROR;

fail:
    out->clk_khz = sd_clk_khz;
    sdmmc_bus_unlock();
    return ERR_IO;
}

void sdio_probe_report(const struct sdio_info *info) {
    if (!info) return;
    printf("SDHOST base = 0x%08x (slot1 C6)\n", (unsigned)(uintptr_t)sd_host_reg_base());
    if (!info->up) {
        printf("C6 (slot 1): SIN RESPUESTA (hwerr=0x%08x)\n", (unsigned)sd_hwerr);
        return;
    }
    printf("C6 (slot 1): enumerado OK\n");
    printf("  OCR(R4)=0x%08x  RCA=0x%04x  clk=%u kHz\n", (unsigned)info->ocr, (unsigned)info->rca, (unsigned)info->clk_khz);
    printf("  io_enable=0x%02x  io_ready=0x%02x  sdio_ver=0x%02x\n", info->cio_enable, info->cio_ready, info->sdio_ver);
    printf("  fn0 blksize=%u  CCCR 0x0C..0x13: %02x %02x %02x %02x %02x %02x %02x %02x\n",
           info->blksize,
           info->fn0_info[0], info->fn0_info[1], info->fn0_info[2], info->fn0_info[3],
           info->fn0_info[4], info->fn0_info[5], info->fn0_info[6], info->fn0_info[7]);
}
