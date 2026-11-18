/*
 * Internal shared definitions for ESP32-P4 SDMMC host + SD card (LK port).
 * Register-level logic adapted from xinu-p4-smp/device/sdmmc-esp32p4 and sdblock-esp32p4.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define SD_BLOCK_SIZE 512
#define SD_SLOT 0U
#define SD_CARD_DIV_PROBE 50U
#define SD_CARD_DIV_WORK  1U

/* SD protocol commands */
#define SD_CMD0_GO_IDLE            0
#define SD_CMD2_ALL_SEND_CID       2
#define SD_CMD3_SEND_RCA           3
#define SD_CMD7_SELECT             7
#define SD_CMD8_SEND_IF_COND       8
#define SD_CMD9_SEND_CSD           9
#define SD_CMD16_SET_BLOCKLEN      16
#define SD_CMD17_READ_SINGLE       17
#define SD_CMD24_WRITE_SINGLE      24
#define SD_CMD55_APP_CMD           55
#define SD_ACMD6_SET_BUS_WIDTH     6
#define SD_ACMD41_SEND_OP_COND     41

#define SD_R1_ERR_MASK             0xFEF80000UL

/* Shared bus lock (controller shared by SD slot 0 and SDIO slot 1 / C6) */
void sdmmc_bus_init(void);
void sdmmc_bus_lock(void);
void sdmmc_bus_unlock(void);

/* Card lock (protects card state/cursor) */
void sd_card_lock_init(void);
void sd_card_lock(void);
void sd_card_unlock(void);

/* Time helpers */
uint32_t sd_now_ms(void);
bool sd_timed_out_ms(uint32_t start, uint32_t ms);
void sd_udelay(uint32_t us);

/* Host (slot 0, microSD) */
bool sd_pads_init(void);
bool sd_controller_init(void);
bool sd_set_width(uint32_t width);
bool sd_set_clock_slot(uint32_t slot, uint8_t card_div);
bool sd_set_clock_slot_no_wait(uint32_t slot, uint8_t card_div);
bool sd_set_host_div(uint32_t div);
bool sd_block_reset(void);

bool sd_send_cmd(uint32_t idx, uint32_t arg, uint32_t resp, bool crc, uint32_t *r0);
bool sd_send_cmd_slot(uint32_t slot, uint32_t idx, uint32_t arg, uint32_t resp, bool crc, bool send_init, uint32_t *r0);
void sd_get_long_resp(uint32_t *dst);

bool sd_read_block(uint32_t lba);
bool sd_write_block(uint32_t lba);
void sd_block_copy_out(void *dst, uint32_t bytes);
void sd_block_copy_in(const void *src, uint32_t bytes);

bool sd_transfer_slot(uint32_t slot, uint32_t idx, uint32_t arg, uint32_t bytcnt,
                      uint32_t blksiz, bool write);

volatile uint32_t *sd_host_reg_base(void);
void sd_bus_state(const char *tag, uint32_t slot);

/* Card */
bool sd_identify(void);

extern bool sd_present;
extern bool sd_hc;
extern uint32_t sd_rca;
extern uint32_t sd_blocks;
extern uint8_t sd_width_card;
extern uint32_t sd_ocr;
extern uint32_t sd_cid[4];
extern uint32_t sd_csd[4];
extern uint32_t sd_lba;
extern const char *sd_fail;

/* Host state */
extern uint32_t sd_hwerr;
extern uint32_t sd_clk_khz;
extern uint32_t sd_host_khz;
extern uint8_t sd_host_width;
extern bool sd_host_ready;
extern bool sd_host_hc;
extern uint32_t sd_host_lba;

/* BIO registration / init hook */
void esp32p4_sdmmc_init(void);

bool sdTransferPio(uint32_t slot, unsigned int idx, uint32_t arg, uint8_t *buf, uint32_t bytcnt, uint32_t blksiz, bool write);
