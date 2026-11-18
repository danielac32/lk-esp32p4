/*
 * AHB-DMA canal 0 (OUT->I2S0 TX, IN<-I2S0 RX), portado de Xinu ahbdma.c.
 * Sin interrupciones: todo se sondea por registros.
 */
#include <platform/esp32p4_ahbdma.h>
#include <platform/esp32p4.h>
#include <platform/time.h>
#include <stdint.h>

#define AHB_DMA_BASE 0x50085000UL
#define DMA_REG(o) (*(volatile uint32_t *)(AHB_DMA_BASE + (o)))

/* OUT (TX) */
#define CH_OUT_INT_RAW   0x30U
#define CH_OUT_INT_CLR   0x3CU
#define CH_OUT_CONF0     0xD0U
#define CH_OUT_CONF1     0xD4U
#define CH_OUT_LINK      0xE0U
#define CH_OUT_DSCR      0xF0U
#define CH_OUT_PRI       0xFCU
#define CH_OUT_PERI_SEL  0x100U
#define CH_OUT_LINK_ADDR 0x3B8U
#define OUT_RST          (1U << 0)
#define OUT_EOF_MODE     (1U << 3)
#define OUT_CHECK_OWNER  (1U << 12)
#define OUTLINK_STOP     (1U << 0)
#define OUTLINK_START    (1U << 1)
#define OUTLINK_PARK     (1U << 3)
#define OUT_INT_ALL      0x0FU
#define PERI_I2S0        3U
#define PERI_NONE        0x3FU

/* IN (RX) */
#define CH_IN_INT_RAW    0x00U
#define CH_IN_INT_ENA    0x08U
#define CH_IN_INT_CLR    0x0CU
#define CH_IN_CONF0      0x70U
#define CH_IN_CONF1      0x74U
#define CH_IN_LINK       0x80U
#define CH_IN_SUC_EOF    0x88U
#define CH_IN_DSCR       0x90U
#define CH_IN_PRI        0x9CU
#define CH_IN_PERI_SEL   0xA0U
#define CH_IN_LINK_ADDR  0x3ACU
#define CH_IN_FIFO_STS   0x78U
#define INLINK_AUTORET   (1U << 0)
#define INLINK_STOP      (1U << 1)
#define INLINK_START     (1U << 2)
#define INLINK_PARK      (1U << 4)
#define IN_RST           (1U << 0)
#define IN_INT_ALL       0x1FU

/* globales */
#define DMA_MISC_CONF    0x64U
#define DMA_INTR_MEM_START 0x3C4U
#define DMA_INTR_MEM_END   0x3C8U
#define DMA_REG_CLK_FORCE  (1U << 3)
#define DMA_MEM_START     0x40000000U
#define DMA_MEM_END       0x4FFC0000U
#define CACHE_L2_BASE     0x3FF10000UL
#define CACHE_L2_CTRL     0x270U
#define L2_SHUT_DMA       (1U << 4)
#define C1_AHB_PDMA_CLK_EN (1U << 3)
#define PARK_GUARD        100000U

/* Igual que I2S: tx_linked/in_linked viven bajo el mutex de audio (audio.c),
 * unico llamador concurrente.  El bus SDMMC ya tiene su propio mutex
 * (sdmmc_bus_lock) para las transferencias CMD53; aqui no se duplica. */
static bool tx_linked, in_linked;

static void dma_clocks_open(void) {
    *(volatile uint32_t *)(P4_HP_SYS_CLKRST_BASE + 0x18U) |= C1_AHB_PDMA_CLK_EN;
    *(volatile uint32_t *)(CACHE_L2_BASE + CACHE_L2_CTRL) &= ~L2_SHUT_DMA;
    DMA_REG(DMA_MISC_CONF) |= DMA_REG_CLK_FORCE;
    DMA_REG(DMA_INTR_MEM_START) = DMA_MEM_START;
    DMA_REG(DMA_INTR_MEM_END) = DMA_MEM_END;
}
static void wait_parked_tx(void) {
    for (uint32_t i = 0; i < PARK_GUARD && (DMA_REG(CH_OUT_LINK) & OUTLINK_PARK) == 0; i++) {}
}
static void wait_parked_in(void) {
    for (uint32_t i = 0; i < PARK_GUARD && (DMA_REG(CH_IN_LINK) & INLINK_PARK) == 0; i++) {}
}

bool p4_ahbdma_tx_link(uint32_t first_desc) {
    if (first_desc < DMA_MEM_START || first_desc >= DMA_MEM_END || (first_desc & 3U) != 0U)
        return false;
    dma_clocks_open();
    DMA_REG(CH_OUT_LINK) = OUTLINK_STOP;
    wait_parked_tx();
    DMA_REG(CH_OUT_CONF0) = OUT_RST;
    DMA_REG(CH_OUT_CONF0) = OUT_EOF_MODE;
    DMA_REG(CH_OUT_CONF1) = OUT_CHECK_OWNER;
    DMA_REG(CH_OUT_PRI) = 1U;
    DMA_REG(CH_OUT_INT_CLR) = OUT_INT_ALL;
    DMA_REG(CH_OUT_PERI_SEL) = PERI_I2S0;
    DMA_REG(CH_OUT_LINK_ADDR) = first_desc;
    DMA_REG(CH_OUT_LINK) = OUTLINK_START;
    tx_linked = true;
    return true;
}
void p4_ahbdma_tx_stop(void) {
    if (!tx_linked) return;
    DMA_REG(CH_OUT_LINK) = OUTLINK_STOP;
    wait_parked_tx();
    /* channelReset de Xinu: pulso de reset y VOLVER a OUT_EOF_MODE.  Dejar
     * OUT_RST puesto deja el motor TX parado -> el 2º play no suena. */
    DMA_REG(CH_OUT_CONF0) = OUT_RST;
    DMA_REG(CH_OUT_CONF0) = OUT_EOF_MODE;
    DMA_REG(CH_OUT_PERI_SEL) = PERI_NONE;
    DMA_REG(CH_OUT_INT_CLR) = OUT_INT_ALL;
    tx_linked = false;
}
bool p4_ahbdma_tx_running(void) {
    return tx_linked && (DMA_REG(CH_OUT_LINK) & OUTLINK_PARK) == 0;
}
uint32_t p4_ahbdma_tx_raw(void) { return DMA_REG(CH_OUT_INT_RAW) & OUT_INT_ALL; }
void p4_ahbdma_tx_clear(uint32_t mask) { DMA_REG(CH_OUT_INT_CLR) = mask & OUT_INT_ALL; }
uint32_t p4_ahbdma_tx_current_desc(void) { return DMA_REG(CH_OUT_DSCR); }

bool p4_ahbdma_in_link(uint32_t first_desc) {
    if (first_desc < DMA_MEM_START || first_desc >= DMA_MEM_END || (first_desc & 3U) != 0U)
        return false;
    dma_clocks_open();
    DMA_REG(CH_IN_LINK) = INLINK_STOP;
    wait_parked_in();
    DMA_REG(CH_IN_CONF0) = IN_RST;
    DMA_REG(CH_IN_CONF0) = 0U;  /* soltar el reset (inChannelReset de Xinu) */
    DMA_REG(CH_IN_CONF1) = 0U;  /* sin check_owner (medido en Xinu) */
    DMA_REG(CH_IN_INT_ENA) = 0U;
    DMA_REG(CH_IN_PRI) = 1U;
    DMA_REG(CH_IN_INT_CLR) = IN_INT_ALL;
    DMA_REG(CH_IN_PERI_SEL) = PERI_I2S0;
    DMA_REG(CH_IN_LINK_ADDR) = first_desc;
    DMA_REG(CH_IN_LINK) = INLINK_START;
    in_linked = true;
    return true;
}
void p4_ahbdma_in_stop(void) {
    if (!in_linked) return;
    DMA_REG(CH_IN_LINK) = INLINK_STOP;
    wait_parked_in();
    /* inChannelReset de Xinu: pulso de reset y SOLTARLO (CONF0=0).  Dejarlo
     * en IN_RST deja el motor parado para siempre -> el 2º rec no graba. */
    DMA_REG(CH_IN_CONF0) = IN_RST;
    DMA_REG(CH_IN_CONF0) = 0U;
    DMA_REG(CH_IN_PERI_SEL) = PERI_NONE;
    DMA_REG(CH_IN_INT_CLR) = IN_INT_ALL;
    in_linked = false;
}
bool p4_ahbdma_in_running(void) {
    return in_linked && (DMA_REG(CH_IN_LINK) & INLINK_PARK) == 0;
}
uint32_t p4_ahbdma_in_raw(void) { return DMA_REG(CH_IN_INT_RAW) & IN_INT_ALL; }
void p4_ahbdma_in_clear(uint32_t mask) { DMA_REG(CH_IN_INT_CLR) = mask & IN_INT_ALL; }
uint32_t p4_ahbdma_in_current_desc(void) { return DMA_REG(CH_IN_DSCR); }
uint32_t p4_ahbdma_in_eof_desc(void) { return DMA_REG(CH_IN_SUC_EOF); }
void p4_ahbdma_in_clear_eof(void) { DMA_REG(CH_IN_SUC_EOF) = 0U; }
uint32_t p4_ahbdma_in_fifo_words(void) { return (DMA_REG(CH_IN_FIFO_STS) & (0x3FU << 2)) >> 2; }
