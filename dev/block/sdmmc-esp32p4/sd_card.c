/*
 * SD card protocol (slot 0) for LK, adapted from Xinu sdblock-esp32p4/sdCard.c.
 * Uses shared SDMMC host (sd_hw.c) and bus lock.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <kernel/mutex.h>
#include "sdmmc_internal.h"

static mutex_t sd_card_mutex;
static bool sd_card_mutex_inited;

void sd_card_lock_init(void) {
    if (!sd_card_mutex_inited) {
        mutex_init(&sd_card_mutex);
        sd_card_mutex_inited = true;
    }
}
void sd_card_lock(void) { if (sd_card_mutex_inited) mutex_acquire(&sd_card_mutex); }
void sd_card_unlock(void) { if (sd_card_mutex_inited) mutex_release(&sd_card_mutex); }

#define SD_ID_FAIL(stage) do { sd_fail = (stage); return false; } while (0)

static bool sd_cmd(uint32_t idx, uint32_t arg, uint32_t resp, bool crc, uint32_t *r0) {
    return sd_send_cmd(idx, arg, resp, crc, r0);
}

static bool sd_cmd_r1(uint32_t idx, uint32_t arg, bool ignore_errors, uint32_t *r0) {
    uint32_t r;
    if (!sd_cmd(idx, arg, 1, true, &r)) return false;
    if ((r & SD_R1_ERR_MASK) != 0 && !ignore_errors) return false;
    if (r0) *r0 = r;
    return true;
}

static bool sd_app_cmd(uint32_t idx, uint32_t arg, uint32_t resp, bool crc, uint32_t *r0) {
    if (!sd_cmd(SD_CMD55_APP_CMD, sd_rca << 16, 1, true, NULL)) return false;
    return sd_cmd(idx, arg, resp, crc, r0);
}

static bool sd_long_response(uint32_t *dst) {
    sd_get_long_resp(dst);
    return true;
}

static bool sd_decode_csd(void) {
    uint32_t c3 = sd_csd[0];
    uint32_t c2 = sd_csd[1];
    uint32_t c1 = sd_csd[2];
    if (((c3 >> 30) & 0x3U) == 1U) {
        uint32_t csize = ((c2 & 0x3FU) << 16) | ((c1 >> 16) & 0xFFFFU);
        sd_blocks = (csize + 1U) * 1024U;
    } else {
        uint32_t csize = ((c2 & 0x3FFU) << 2) | ((c1 >> 30) & 0x3U);
        uint32_t mult = (c1 >> 15) & 0x7U;
        uint32_t blen = (c2 >> 16) & 0xFU;
        uint32_t shift = mult + blen;
        if (shift < 7) return false;
        sd_blocks = (csize + 1U) << (shift - 7U);
    }
    return true;
}

static bool sd_identify_inner(void) {
    uint32_t r;
    uint32_t t0;
    bool v2 = false;

    sd_present = false; sd_hc = false; sd_rca = 0; sd_blocks = 0; sd_width_card = 1;

    if (!sd_set_clock_slot(SD_SLOT, SD_CARD_DIV_PROBE)) SD_ID_FAIL("probe clock");
    sd_udelay(5000);

    if (!sd_cmd(SD_CMD0_GO_IDLE, 0, 0, false, NULL)) SD_ID_FAIL("CMD0");
    sd_udelay(2000);

    if (sd_cmd(SD_CMD8_SEND_IF_COND, 0x1AA, 1, false, &r) && ((r & 0xFFFU) == 0x1AAU)) v2 = true;

    t0 = sd_now_ms();
    for (;;) {
        uint32_t arg = v2 ? 0x40FF8000UL : 0x00FF8000UL;
        if (!sd_app_cmd(SD_ACMD41_SEND_OP_COND, arg, 1, false, &r)) SD_ID_FAIL(v2 ? "ACMD41" : "ACMD41 (CMD8 unanswered)");
        if (r & (1UL << 31)) break;
        if (sd_timed_out_ms(t0, 2000)) SD_ID_FAIL("ACMD41 busy");
        sd_udelay(10000);
    }
    sd_ocr = r;
    sd_hc = (r & (1UL << 30)) != 0;
    sd_host_hc = sd_hc;

    if (!sd_cmd(SD_CMD2_ALL_SEND_CID, 0, 2, false, NULL)) SD_ID_FAIL("CMD2");
    if (!sd_long_response(sd_cid)) SD_ID_FAIL("CMD2 response");

    if (!sd_cmd(SD_CMD3_SEND_RCA, 0, 1, true, &r)) SD_ID_FAIL("CMD3");
    sd_rca = (r >> 16) & 0xFFFFU;
    if (sd_rca == 0) SD_ID_FAIL("CMD3 rca");

    if (!sd_cmd(SD_CMD9_SEND_CSD, sd_rca << 16, 2, false, NULL)) SD_ID_FAIL("CMD9");
    if (!sd_long_response(sd_csd)) SD_ID_FAIL("CMD9 response");
    if (!sd_decode_csd()) SD_ID_FAIL("CSD");

    if (!sd_cmd_r1(SD_CMD7_SELECT, sd_rca << 16, false, NULL)) SD_ID_FAIL("CMD7");

    if (!sd_hc) {
        if (!sd_cmd_r1(SD_CMD16_SET_BLOCKLEN, SD_BLOCK_SIZE, false, NULL)) SD_ID_FAIL("CMD16");
    }

    if (v2) {
        if (!sd_app_cmd(SD_ACMD6_SET_BUS_WIDTH, 2, 1, true, NULL)) SD_ID_FAIL("ACMD6");
        if (!sd_set_width(4)) SD_ID_FAIL("bus width");
        sd_width_card = 4;
    }

    if (!sd_set_clock_slot(SD_SLOT, SD_CARD_DIV_WORK)) SD_ID_FAIL("work clock");

    sd_host_lba = 0;
    sd_lba = 0;
    sd_present = true;
    sd_fail = "ok";
    return true;
}

bool sd_identify(void) {
    bool ok;
    sd_card_lock();
    sdmmc_bus_lock();
    ok = sd_identify_inner();
    sdmmc_bus_unlock();
    sd_card_unlock();
    return ok;
}
