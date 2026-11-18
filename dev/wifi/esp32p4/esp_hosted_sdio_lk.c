/*
 * ESP-Hosted SDIO transport for LK on ESP32-P4 (slot 1 / ESP32-C6).
 * Adapted from Xinu p4-smp working driver, using LK SDMMC primitives.
 */
#include <dev/wifi/esp_hosted_sdio.h>
#include <dev/wifi/sdio.h>
#include <dev/wifi/esp_hosted.h>
#include "sdmmc_internal.h"

/* El C6 deja DAT0 afirmado mientras tiene un paquete C2H sin leer.  Con
 * CMD[13]=wait_complete el comando internal "update clock" queda inhibido y
 * el CMD_START se auto-limpia: el comando no sale y el CIU reporta RTO o
 * INHIBIT.  El propio sd_hw.c expone este flag para ese caso. */
extern uint32_t sd_xfer_no_wait;

#include <platform/esp32p4.h>
#include <platform/esp32p4_gpio.h>
#include <dev/gpio.h>
#include <kernel/thread.h>
#include <lk/debug.h>
#include <lk/err.h>
#include <platform.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#define SDIO_SLOT 1U
#define FUNC0 0U
#define FUNC1 1U
#define CMD0 0U
#define CMD3 3U
#define CMD5 5U
#define CMD7 7U
#define CMD52 52U
#define CMD53 53U

#define BLOCK_SIZE 512U
#define DMA_BUF_SIZE 4096U
#define FRAME_BUF_SIZE 2048U
#define TX_FRAME_BUF_SIZE 2048U
#define IO_READY_RETRIES 200U
#define ATTACH_RETRIES 3U
#define PROBE_CARD_DIV 50U /* SD_CARD_DIV_PROBE */
#define WORK_CARD_DIV  1U

#define R4_READY (1U<<31)
#define RCA_SHIFT 16U
#define CMD5_OCR_ARG 0x00FF8000U

#define INT_NEW_PACKET     (1U<<23)
#define INT_START_THROTTLE (1U<<7)
#define INT_STOP_THROTTLE  (1U<<6)

#define CCCR_FN_ENABLE   0x02U
#define CCCR_FN_READY    0x03U
#define CCCR_INT_ENABLE  0x04U
#define CCCR_BUS_WIDTH   0x07U
#define CCCR_BLKSIZEL    0x10U
#define CCCR_BLKSIZEH    0x11U
#define CCCR_BUS_WIDTH_4 0x02U
#define CCCR_FN1_EN      (1U<<1)
#define CCCR_INT_MASTER_EN (1U<<0)

#define SLAVE_BASE             0x3FF55000U
#define HOST_TO_SLAVE_INTR     (SLAVE_BASE + 0x8CU)
#define SLAVE_INT_RAW_REG      (SLAVE_BASE + 0x50U)
#define SLAVE_INT_CLR_REG      (SLAVE_BASE + 0xD4U)
#define SLAVE_PACKET_LEN_REG   (SLAVE_BASE + 0x60U)
#define SLAVE_TOKEN_RDATA      (SLAVE_BASE + 0x44U)
#define SLAVE_CMD53_END_ADDR   0x1F800U
#define HOST_INTR_OPEN_DATA    0U
#define TX_BUF_MAX             0x1000U
#define TX_BUF_MASK            0x0FFFU
#define RX_BUF_WINDOW          1536U
#define RX_BYTE_MAX            0x100000U
#define SLAVE_LEN_MASK         0xFFFFFU
#define ADDR_MASK              0x3FFU

#define LP_IOMUX_BASE 0x5012B000UL
#define LP_PAD_REG(n) (LP_IOMUX_BASE + 0x08UL + 4UL*(uint32_t)(n))
#define LP_MUX_SEL (1U<<4)
#define LAST_LP_PAD 15U

typedef struct __attribute__((packed)) {
    uint8_t if_type : 4;
    uint8_t if_num : 4;
    uint8_t flags;
    uint16_t len;
    uint16_t offset;
    uint16_t checksum;
    uint16_t seq_num;
    uint8_t throttle_cmd : 2;
    uint8_t reserved2 : 6;
    union { uint8_t reserved3; uint8_t hci_pkt_type; uint8_t priv_pkt_type; };
} payload_header_t;

typedef struct {
    esp_hosted_sdio_config_t config;
    bool attached;
    bool tx_throttled;
    uint16_t rca;
    uint32_t rx_byte_count;
    uint32_t tx_buf_count;
    uint8_t *frame_buf;
    uint8_t *tx_frame;
} sdio_ctx_t;

/* s_ctx: tx_frame/frame_buf y contadores se acceden desde el hilo poll (core 0,
 * pinneado en wifi_netif.c) y desde los comandos shell (tambien core 0).  El
 * bus SDMMC lleva su propio mutex (cmd52/cmd53), que serializa el hardware.
 * Mantener TODO el trafico ESP-Hosted en el core 0 es la sincronizacion; mover
 * un servidor al core 1 exigira un mutex explicito alrededor de s_ctx. */
static sdio_ctx_t s_ctx;
static volatile uint32_t g_cmd53_ok, g_cmd53_fail, g_last_hwerr;
static volatile uint32_t g_rx_parse_calls, g_rx_parse_ok, g_rx_parse_reject, g_rx_parse_pad;
static volatile uint32_t g_rx_last_tlen, g_rx_last_poff, g_rx_last_plen, g_rx_last_if, g_rx_last_in, g_rx_last_flags;

static void msleep(uint32_t ms) { thread_sleep(ms); }

static uint16_t cksum16(const uint8_t *buf, uint32_t len) {
    uint32_t c = 0; for (uint32_t i = 0; i < len; i++) c += buf[i]; return (uint16_t)c;
}

static bool cmd52_rw(uint8_t function, uint32_t reg, bool is_write, uint8_t *data) {
    if (!data) return false;
    uint32_t arg = ((uint32_t)is_write << 31) | ((uint32_t)function << 28)
                 | ((reg & 0x1FFFFU) << 9) | (uint32_t)(*data);
    uint32_t resp = 0;
    sdmmc_bus_lock();
    bool ok = sd_send_cmd_slot(SDIO_SLOT, CMD52, arg, 1, true, false, &resp);
    sdmmc_bus_unlock();
    if (ok) *data = (uint8_t)(resp & 0xFFU);
    return ok;
}

static bool cmd53_transfer(uint8_t function, uint32_t addr, bool is_write, bool block_mode,
                           uint8_t *data, uint32_t len) {
    if (!data || len == 0U || len > BLOCK_SIZE) return false;
    uint32_t count = block_mode ? (len / BLOCK_SIZE) : (len & 0x1FFU);
    if (!block_mode && len == BLOCK_SIZE) count = 0U;
    uint32_t arg = ((uint32_t)is_write << 31) | ((uint32_t)function << 28)
                 | ((uint32_t)block_mode << 27) | (1U << 26)
                 | ((addr & 0x1FFFFU) << 9) | (count & 0x1FFU);
    uint32_t blksiz = block_mode ? BLOCK_SIZE : len;
    sdmmc_bus_lock();
    if (is_write) sd_block_copy_in(data, len);
    bool ok = sd_transfer_slot(SDIO_SLOT, CMD53, arg, len, blksiz, is_write);
    if (ok && !is_write) sd_block_copy_out(data, len);
    sdmmc_bus_unlock();
    if (ok) g_cmd53_ok++;
    else { g_cmd53_fail++; g_last_hwerr = sd_hwerr; }
    return ok;
}

static bool read_bytes(uint8_t function, uint32_t addr, uint8_t *data, uint32_t len) {
    while (len >= BLOCK_SIZE) {
        if (!cmd53_transfer(function, addr, false, true, data, BLOCK_SIZE)) return false;
        addr += BLOCK_SIZE; data += BLOCK_SIZE; len -= BLOCK_SIZE;
    }
    if (len > 0U) {
        if (!cmd53_transfer(function, addr, false, false, data, len)) return false;
    }
    return true;
}

static bool write_bytes(uint8_t function, uint32_t addr, const uint8_t *data, uint32_t len) {
    while (len >= BLOCK_SIZE) {
        if (!cmd53_transfer(function, addr, true, true, (uint8_t *)data, BLOCK_SIZE)) return false;
        addr += BLOCK_SIZE; data += BLOCK_SIZE; len -= BLOCK_SIZE;
    }
    if (len > 0U) {
        if (!cmd53_transfer(function, addr, true, false, (uint8_t *)data, len)) return false;
    }
    return true;
}

static bool read_reg_bytes(uint32_t reg, uint8_t *data, uint32_t len) {
    if (!data || len == 0U) return false;
    reg &= ADDR_MASK;
    if (len > 1U) {
        if (cmd53_transfer(FUNC1, reg, false, false, data, len)) return true;
    }
    for (uint32_t i = 0; i < len; i++) {
        uint8_t v = 0;
        if (!cmd52_rw(FUNC1, reg + i, false, &v)) return false;
        data[i] = v;
    }
    return true;
}

static bool write_reg_bytes(uint32_t reg, const uint8_t *data, uint32_t len) {
    if (!data || len == 0U) return false;
    reg &= ADDR_MASK;
    if (len > 1U) {
        if (cmd53_transfer(FUNC1, reg, true, false, (uint8_t *)data, len)) return true;
    }
    for (uint32_t i = 0; i < len; i++) {
        uint8_t v = data[i];
        if (!cmd52_rw(FUNC1, reg + i, true, &v)) return false;
    }
    return true;
}

static bool set_block_size(uint8_t function, uint16_t value) {
    uint32_t offset = (uint32_t)function * 0x100U;
    uint8_t lo = (uint8_t)(value & 0xFFU), hi = (uint8_t)(value >> 8);
    return cmd52_rw(FUNC0, offset + CCCR_BLKSIZEL, true, &lo)
        && cmd52_rw(FUNC0, offset + CCCR_BLKSIZEH, true, &hi);
}

static bool get_tx_buffer_num(uint32_t *tx_num) {
    uint32_t token = 0;
    if (!tx_num) return false;
    if (!read_reg_bytes(SLAVE_TOKEN_RDATA, (uint8_t *)&token, sizeof(token))) return false;
    token = (token >> 16) & TX_BUF_MASK;
    token = (token + TX_BUF_MAX - s_ctx.tx_buf_count) % TX_BUF_MAX;
    *tx_num = token; return true;
}
static bool get_len_from_slave(uint32_t *rx_size) {
    uint32_t len = 0, temp;
    if (!rx_size) return false;
    if (!read_reg_bytes(SLAVE_PACKET_LEN_REG, (uint8_t *)&len, sizeof(len))) return false;
    len &= SLAVE_LEN_MASK;
    if (len >= s_ctx.rx_byte_count) len = (len + RX_BYTE_MAX - s_ctx.rx_byte_count) % RX_BYTE_MAX;
    else { temp = RX_BYTE_MAX - s_ctx.rx_byte_count; len = temp + len; }
    *rx_size = len; return true;
}

static bool generate_slave_intr(uint8_t intr_no) {
    uint8_t mask = (uint8_t)(1U << intr_no);
    return write_reg_bytes(HOST_TO_SLAVE_INTR, &mask, 1);
}

static void setup_matrix_pin(int gpio_num, bool pullup) {
    if (gpio_num < 0) return;
    uint32_t g = (uint32_t)gpio_num;
    if (g <= LAST_LP_PAD) {
        volatile uint32_t *lp = (volatile uint32_t *)LP_PAD_REG(g);
        *lp &= ~LP_MUX_SEL;
    }
    if (g <= 54U) {
        volatile uint32_t *pad = (volatile uint32_t *)P4_IO_MUX_PIN_REG(g);
        uint32_t val = *pad;
        val &= ~(P4_IO_MCU_SEL_MASK | P4_IO_FUN_WPD | P4_IO_FUN_WPU | P4_IO_FUN_DRV_MASK);
        val |= P4_IO_MCU_SEL_GPIO | P4_IO_FUN_IE | P4_IO_FUN_DRV_40MA;
        if (pullup) val |= P4_IO_FUN_WPU;
        *pad = val;
    }
}

static void sdio_route(uint8_t pin, uint32_t sig, bool is_clk, bool pullup) {
    setup_matrix_pin((int)pin, pullup);
    p4_gpio_matrix_out(pin, sig, false, false);
    if (!is_clk) p4_gpio_matrix_in(pin, sig, false);
}

static bool setup_slot1_pins(const esp_hosted_sdio_config_t *cfg) {
    if (cfg->pin_clk < 0 || cfg->pin_cmd < 0 || cfg->pin_d0 < 0) return false;
    /* slot-relative SDIO signal indices (as used by working sdio_probe.c) */
    const uint32_t SIG_CLK=0, SIG_CMD=1, SIG_D0=2, SIG_D1=3, SIG_D2=4, SIG_D3=5;
    sdio_route((uint8_t)cfg->pin_clk, SIG_CLK, true,  false);
    sdio_route((uint8_t)cfg->pin_cmd, SIG_CMD, false, cfg->use_internal_pullups);
    sdio_route((uint8_t)cfg->pin_d0,  SIG_D0,  false, cfg->use_internal_pullups);
    if (cfg->bus_width >= 4U) {
        if (cfg->pin_d1 < 0 || cfg->pin_d2 < 0 || cfg->pin_d3 < 0) return false;
        sdio_route((uint8_t)cfg->pin_d1, SIG_D1, false, cfg->use_internal_pullups);
        sdio_route((uint8_t)cfg->pin_d2, SIG_D2, false, cfg->use_internal_pullups);
        sdio_route((uint8_t)cfg->pin_d3, SIG_D3, false, cfg->use_internal_pullups);
    }
    return true;
}

static void reset_c6(const esp_hosted_sdio_config_t *cfg) {
    if (cfg->pin_reset < 0) return;
    unsigned pin = (unsigned)cfg->pin_reset;
    p4_gpio_init();
    p4_gpio_set_dir(pin, true);
    /* match sdio_probe: drive high, pulse low 100ms, high, wait for C6 boot */
    gpio_set(pin, 1);
    msleep(5);
    gpio_set(pin, 0);
    msleep(100);
    gpio_set(pin, 1);
    msleep(1500);
}

static bool card_init(sdio_ctx_t *ctx) {
    uint32_t resp = 0;
    uint8_t ioe=0, ior=0, ie=0, busw=0;

    printf("[sdio] CMD5 probe\n");
    sdmmc_bus_lock();
    (void)sd_send_cmd_slot(SDIO_SLOT, CMD5, 0, 1, false, false, &resp);
    for (int i = 0; i < IO_READY_RETRIES; i++) {
        if (sd_send_cmd_slot(SDIO_SLOT, CMD5, CMD5_OCR_ARG, 1, false, false, &resp) && (resp & R4_READY)) break;
        sdmmc_bus_unlock(); msleep(1); sdmmc_bus_lock();
        if (i == IO_READY_RETRIES - 1) { sdmmc_bus_unlock(); printf("[sdio] CMD5 timeout resp=0x%x\n",(unsigned)resp); return false; }
    }
    printf("[sdio] CMD5 ok resp=0x%x\n",(unsigned)resp);
    printf("[sdio] CMD3\n");
    if (!sd_send_cmd_slot(SDIO_SLOT, CMD3, 0, 1, true, false, &resp)) { sdmmc_bus_unlock(); printf("[sdio] CMD3 fail\n"); return false; }
    ctx->rca = (uint16_t)(resp >> RCA_SHIFT);
    if (!ctx->rca) { sdmmc_bus_unlock(); printf("[sdio] bad rca\n"); return false; }
    printf("[sdio] CMD7 rca=0x%x\n", (unsigned)ctx->rca);
    if (!sd_send_cmd_slot(SDIO_SLOT, CMD7, ((uint32_t)ctx->rca) << RCA_SHIFT, 1, true, false, NULL)) { sdmmc_bus_unlock(); printf("[sdio] CMD7 fail\n"); return false; }
    sdmmc_bus_unlock();

    printf("[sdio] fn enable\n");
    if (!cmd52_rw(FUNC0, CCCR_FN_ENABLE, false, &ioe)) { printf("[sdio] rd fn_enable fail\n"); return false; }
    ioe |= CCCR_FN1_EN;
    if (!cmd52_rw(FUNC0, CCCR_FN_ENABLE, true, &ioe)) { printf("[sdio] wr fn_enable fail\n"); return false; }
    (void)cmd52_rw(FUNC0, CCCR_FN_ENABLE, false, &ioe);

    for (int i = 0; i < IO_READY_RETRIES; i++) {
        ior = 0;
        if (cmd52_rw(FUNC0, CCCR_FN_READY, false, &ior) && (ior & CCCR_FN1_EN)) break;
        msleep(10);
        if (i == IO_READY_RETRIES - 1) { printf("[sdio] fn1 ready timeout ior=0x%x\n",(unsigned)ior); return false; }
    }
    printf("[sdio] fn1 ready ior=0x%x\n",(unsigned)ior);

    if (!cmd52_rw(FUNC0, CCCR_INT_ENABLE, false, &ie)) return false;
    ie |= CCCR_INT_MASTER_EN | CCCR_FN1_EN;
    if (!cmd52_rw(FUNC0, CCCR_INT_ENABLE, true, &ie)) return false;

    (void)cmd52_rw(FUNC0, CCCR_BUS_WIDTH, false, &busw);
    if (ctx->config.bus_width >= 4U) {
        busw = (uint8_t)((busw & ~0x3U) | CCCR_BUS_WIDTH_4);
        if (!cmd52_rw(FUNC0, CCCR_BUS_WIDTH, true, &busw)) return false;
        sdmmc_bus_lock();
        volatile uint32_t *hreg = sd_host_reg_base();
        hreg[0x018/4] |= (1UL << SDIO_SLOT); /* 4-bit for slot 1 */
        sdmmc_bus_unlock();
    } else {
        busw = (uint8_t)(busw & ~0x3U);
        (void)cmd52_rw(FUNC0, CCCR_BUS_WIDTH, true, &busw);
    }

    printf("[sdio] set block sizes\n");
    if (!set_block_size(FUNC0, BLOCK_SIZE) || !set_block_size(FUNC1, BLOCK_SIZE)) { printf("[sdio] blksz fail\n"); return false; }

    printf("[sdio] set work clk\n");
    sdmmc_bus_lock();
    bool clk_ok = sd_set_clock_slot(SDIO_SLOT, WORK_CARD_DIV);
    sdmmc_bus_unlock();
    printf("[sdio] work clk ok=%d\n", clk_ok);
    return clk_ok;
}

static void submit_rx_frame(const payload_header_t *h, const uint8_t *payload, uint16_t len) {
    esp_hosted_frame_info_t info = {0};
    info.if_type = (esp_hosted_if_t)h->if_type; info.if_num = h->if_num; info.flags = h->flags;
    info.payload_len = len; info.offset = h->offset; info.checksum = h->checksum; info.seq_num = h->seq_num;
    info.throttle_cmd = h->throttle_cmd; info.hci_pkt_type = h->hci_pkt_type; info.priv_pkt_type = h->priv_pkt_type;
    (void)esp_hosted_rx_submit(&info, payload, len);
}

static bool is_zero_pad(const uint8_t *d, uint32_t len) { for (uint32_t i=0;i<len;i++) if(d[i]) return false; return true; }

static void parse_rx_frames(uint8_t *data, uint32_t len) {
    uint32_t cursor = 0;
    g_rx_parse_calls++;
    g_rx_last_tlen = len;
    while (cursor + sizeof(payload_header_t) <= len) {
        payload_header_t *h = (payload_header_t *)(data + cursor);
        uint32_t poff = h->offset, plen = h->len, flen = poff + plen;
        g_rx_last_poff = poff; g_rx_last_plen = plen; g_rx_last_if = h->if_type;
        g_rx_last_in = h->if_num; g_rx_last_flags = h->flags;
        if (plen == 0U && is_zero_pad(data + cursor, len - cursor)) { g_rx_parse_pad++; return; }
        if (flen == 0U || poff != sizeof(payload_header_t) || cursor + flen > len || plen > ESP_HOSTED_RX_PAYLOAD_MAX) { g_rx_parse_reject++; return; }
        submit_rx_frame(h, data + cursor + poff, (uint16_t)plen);
        g_rx_parse_ok++;
        cursor += flen;
    }
}

static size_t transport_tx(const esp_hosted_frame_info_t *info, const void *payload, size_t len, void *ctx) {
    sdio_ctx_t *sdio = (sdio_ctx_t *)ctx;
    if (!sdio || !info || !sdio->attached || (!payload && len) || len > ESP_HOSTED_RX_PAYLOAD_MAX) return 0;
    if (sdio->tx_throttled && (info->if_type==ESP_HOSTED_IF_STA||info->if_type==ESP_HOSTED_IF_AP||info->if_type==ESP_HOSTED_IF_ETH)) return 0;

    payload_header_t *h = (payload_header_t *)sdio->tx_frame;
    memset(h, 0, sizeof(*h));
    h->if_type = (uint8_t)info->if_type; h->if_num = info->if_num; h->flags = info->flags;
    h->len = (uint16_t)len; h->offset = (uint16_t)sizeof(*h); h->seq_num = info->seq_num; h->throttle_cmd = info->throttle_cmd;
    if (info->if_type == ESP_HOSTED_IF_HCI) h->hci_pkt_type = info->hci_pkt_type;
    if (info->if_type == ESP_HOSTED_IF_PRIV) h->priv_pkt_type = info->priv_pkt_type;
    if (len) memcpy(sdio->tx_frame + sizeof(*h), payload, len);
    uint32_t frame_len = (uint32_t)(sizeof(*h) + len);
    if (frame_len > TX_FRAME_BUF_SIZE) return 0;
    h->checksum = cksum16(sdio->tx_frame, frame_len);

    uint32_t tx_buf_num = 0, buf_needed = (frame_len + RX_BUF_WINDOW - 1U) / RX_BUF_WINDOW;
    if (!get_tx_buffer_num(&tx_buf_num) || tx_buf_num < buf_needed) return 0;

    uint32_t padded = ((frame_len + BLOCK_SIZE - 1U) / BLOCK_SIZE) * BLOCK_SIZE;
    if (padded > TX_FRAME_BUF_SIZE) return 0;
    if (padded != frame_len) memset(sdio->tx_frame + frame_len, 0, padded - frame_len);
    uint32_t addr = SLAVE_CMD53_END_ADDR - frame_len;
    if (!write_bytes(FUNC1, addr, sdio->tx_frame, padded)) return 0;

    sdio->tx_buf_count = (sdio->tx_buf_count + buf_needed) % TX_BUF_MAX;
    return len;
}

static void transport_poll(void *ctx) {
    sdio_ctx_t *sdio = (sdio_ctx_t *)ctx;
    if (!sdio || !sdio->attached) return;
    for (int budget = 0; budget < 4; budget++) {
        uint32_t intr = 0, rxlen = 0;
        if (!read_reg_bytes(SLAVE_INT_RAW_REG, (uint8_t *)&intr, sizeof(intr))) return;
        if (intr != 0U) {
            (void)write_reg_bytes(SLAVE_INT_CLR_REG, (const uint8_t *)&intr, sizeof(intr));
            if (intr & INT_START_THROTTLE) sdio->tx_throttled = true;
            if (intr & INT_STOP_THROTTLE) sdio->tx_throttled = false;
        }
        if (!get_len_from_slave(&rxlen)) return;
        if (rxlen == 0U) {
            if (intr == 0U) return;
            continue;
        }
        /* El esclavo solo atiende CMD53 block-mode: leer bloques de 512
         * desde END_ADDR - rxlen.  Hay que leer ceil(rxlen/512)*512 bytes
         * (padded) para que frame_buf contenga todos los rxlen bytes; leer
         * solo 1 bloque deja el resto como datos viejos y corrompe tramas
         * grandes (>512 B).  Xinu lee len_from_slave completo en bloques. */
        if (rxlen > FRAME_BUF_SIZE) {
            /* No deberia ocurrir (FRAME_BUF_SIZE=2048 >= max trama), drenar. */
            uint32_t addr = SLAVE_CMD53_END_ADDR - rxlen, left = rxlen;
            while (left > 0U) {
                uint32_t chunk = left > FRAME_BUF_SIZE ? FRAME_BUF_SIZE : left;
                uint32_t rd = ((chunk + BLOCK_SIZE - 1U) / BLOCK_SIZE) * BLOCK_SIZE;
                if (rd > FRAME_BUF_SIZE) rd = FRAME_BUF_SIZE;
                if (!cmd53_transfer(FUNC1, addr, false, true, sdio->frame_buf, rd)) return;
                addr += chunk; left -= chunk;
            }
            sdio->rx_byte_count = (sdio->rx_byte_count + rxlen) % RX_BYTE_MAX;
            continue;
        }
        {
            uint32_t padded = ((rxlen + BLOCK_SIZE - 1U) / BLOCK_SIZE) * BLOCK_SIZE;
            uint32_t addr = SLAVE_CMD53_END_ADDR - rxlen;
            if (padded > FRAME_BUF_SIZE) padded = FRAME_BUF_SIZE;
            for (uint32_t off = 0; off < padded; off += BLOCK_SIZE) {
                if (!cmd53_transfer(FUNC1, addr + off, false, true,
                                    sdio->frame_buf + off, BLOCK_SIZE)) return;
            }
        }
        sdio->rx_byte_count = (sdio->rx_byte_count + rxlen) % RX_BYTE_MAX;
        parse_rx_frames(sdio->frame_buf, rxlen);
    }
}

static const esp_hosted_transport_t s_transport = { .tx = transport_tx, .poll = transport_poll, .ctx = &s_ctx };

void esp_hosted_sdio_get_default_config(esp_hosted_sdio_config_t *config) {
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->slot = 1;
    config->bus_width = 4;
    config->clock_khz = 20000;
    config->pin_clk = 18; config->pin_cmd = 19; config->pin_d0 = 14;
    config->pin_d1 = 15; config->pin_d2 = 16; config->pin_d3 = 17;
    config->pin_reset = 54; config->reset_active_low = true;
    config->reset_pulse_ms = 5; config->post_reset_delay_ms = 50;
    config->use_internal_pullups = true;
}

bool esp_hosted_sdio_attach(const esp_hosted_sdio_config_t *config) {
    esp_hosted_sdio_config_t cfg;
    if (!config) { esp_hosted_sdio_get_default_config(&cfg); config = &cfg; }
    if (s_ctx.attached) esp_hosted_sdio_detach();
    sd_xfer_no_wait = 1U;
    if (s_ctx.frame_buf) { free(s_ctx.frame_buf); s_ctx.frame_buf = NULL; }
    if (s_ctx.tx_frame) { free(s_ctx.tx_frame); s_ctx.tx_frame = NULL; }
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.frame_buf = (uint8_t *)calloc(1, FRAME_BUF_SIZE);
    s_ctx.tx_frame = (uint8_t *)calloc(1, TX_FRAME_BUF_SIZE);
    if (!s_ctx.frame_buf || !s_ctx.tx_frame) {
        if (s_ctx.frame_buf) free(s_ctx.frame_buf);
        if (s_ctx.tx_frame) free(s_ctx.tx_frame);
        s_ctx.frame_buf = s_ctx.tx_frame = NULL;
        printf("[wifi] sdio buffer alloc failed\n");
        return false;
    }
    s_ctx.config = *config;
    if (s_ctx.config.bus_width != 1U && s_ctx.config.bus_width != 4U) s_ctx.config.bus_width = 4U;

    for (int attempt = 0; attempt < ATTACH_RETRIES; attempt++) {
        printf("[sdio] attempt %d\n", attempt);
        if (!setup_slot1_pins(&s_ctx.config)) { printf("[wifi] pin setup failed\n"); return false; }
        printf("[sdio] pins ok, reset\n");
        reset_c6(&s_ctx.config);
        printf("[sdio] reset done, set clk\n");
        sdmmc_bus_lock();
        volatile uint32_t *hreg = sd_host_reg_base();
        hreg[0x018/4] &= ~(1UL << SDIO_SLOT); /* CTYPE: 1-bit for slot 1 initially */
        bool clk = sd_set_clock_slot_no_wait(SDIO_SLOT, PROBE_CARD_DIV);
        sdmmc_bus_unlock();
        printf("[sdio] clk=%d\n", clk);
        if (!clk) { msleep(20); continue; }
        printf("[sdio] card_init...\n");
        if (card_init(&s_ctx)) goto ok;
        printf("[sdio] card_init fail\n");
        msleep(20);
    }
    printf("[wifi] esp_hosted sdio attach failed\n");
    return false;
ok:
    if (!generate_slave_intr(HOST_INTR_OPEN_DATA)) {
        printf("[wifi] failed to open slave data path\n");
    }
    s_ctx.attached = true;
    if (esp_hosted_is_initialized()) esp_hosted_set_transport(&s_transport);
    else esp_hosted_init(&s_transport);
    printf("[wifi] esp_hosted sdio attached (slot %u, %u-bit, rca=0x%x)\n",
           (unsigned)s_ctx.config.slot, (unsigned)s_ctx.config.bus_width, (unsigned)s_ctx.rca);
    return true;
}

void esp_hosted_sdio_detach(void) {
    if (s_ctx.attached) {
        esp_hosted_set_transport(NULL);
        s_ctx.attached = false;
    }
    if (s_ctx.frame_buf) { free(s_ctx.frame_buf); s_ctx.frame_buf = NULL; }
    if (s_ctx.tx_frame) { free(s_ctx.tx_frame); s_ctx.tx_frame = NULL; }
}
bool esp_hosted_sdio_is_attached(void) { return s_ctx.attached; }

void esp_hosted_sdio_debug_dump(void) {
    if (!s_ctx.attached) { printf("sdio: not attached\n"); return; }
    uint32_t intr = 0, pktlen = 0, token = 0;
    bool r1 = read_reg_bytes(SLAVE_INT_RAW_REG, (uint8_t *)&intr, sizeof(intr));
    bool r2 = read_reg_bytes(SLAVE_PACKET_LEN_REG, (uint8_t *)&pktlen, sizeof(pktlen));
    bool r3 = read_reg_bytes(SLAVE_TOKEN_RDATA, (uint8_t *)&token, sizeof(token));
    uint32_t raw = pktlen & SLAVE_LEN_MASK;
    uint32_t rxlen = 0;
    if (raw >= s_ctx.rx_byte_count) rxlen = (raw - s_ctx.rx_byte_count) % RX_BYTE_MAX;
    else rxlen = (RX_BYTE_MAX - s_ctx.rx_byte_count) + raw;
    printf("int=0x%x(%d) pktlen=0x%x raw=%u(%d) token=0x%x(%d)\n",
           (unsigned)intr, r1, (unsigned)pktlen, (unsigned)raw, r2, (unsigned)token, r3);
    printf("rx_bc=%u tx_bc=%u rxlen=%u thr=%d\n",
           (unsigned)s_ctx.rx_byte_count, (unsigned)s_ctx.tx_buf_count, (unsigned)rxlen, s_ctx.tx_throttled);
    printf("parse calls=%u ok=%u rej=%u pad=%u last tlen=%u poff=%u plen=%u if=%u in=%u flags=0x%x\n",
           (unsigned)g_rx_parse_calls, (unsigned)g_rx_parse_ok, (unsigned)g_rx_parse_reject, (unsigned)g_rx_parse_pad,
           (unsigned)g_rx_last_tlen, (unsigned)g_rx_last_poff, (unsigned)g_rx_last_plen,
           (unsigned)g_rx_last_if, (unsigned)g_rx_last_in, (unsigned)g_rx_last_flags);
    printf("cmd53 ok=%u fail=%u last_hwerr=0x%x\n",
           (unsigned)g_cmd53_ok, (unsigned)g_cmd53_fail, (unsigned)g_last_hwerr);
}

int esp_hosted_sdio_debug_last_tx_err(void) { return 0; }
uint32_t esp_hosted_sdio_debug_tx_ok(void) { return 0; }
uint32_t esp_hosted_sdio_debug_tx_fail(void) { return 0; }
uint32_t esp_hosted_sdio_debug_rx_pkts(void) { return 0; }
uint32_t esp_hosted_sdio_debug_last_hwerr(void) { return 0; }
uint32_t esp_hosted_sdio_debug_last_len(void) { return 0; }
uint32_t esp_hosted_sdio_debug_last_addr(void) { return 0; }
