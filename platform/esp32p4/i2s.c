/*
 * I2S0 para ESP32-P4, portado de Xinu device/audio/i2s.c.
 * Registros crudos.  Sin FIFO visible para CPU: los datos van por AHB-DMA.
 * Incluye la errata de doble division y la cadena de reloj RX propia.
 */
#include <platform/esp32p4_i2s.h>
#include <platform/esp32p4_es8311.h>
#include <platform/esp32p4.h>
#include <platform/esp32p4_gpio.h>
#include <dev/gpio.h>
#include <platform/time.h>

#define I2S0_BASE 0x500C6000UL
#define LP_AON_CLKRST_BASE 0x50111000UL  /* Xinu esp32p4.h: MCLK del pad I2S0 (+0x40 bit12) */
#define I2S0_REG(o)  (*(volatile uint32_t *)(I2S0_BASE + (o)))
#define I2S0_REGP(o) ((volatile uint32_t *)(I2S0_BASE + (o)))
#define CLKRST_REG(o) (*(volatile uint32_t *)(P4_HP_SYS_CLKRST_BASE + (o)))
#define LPCLK_REG(o)  (*(volatile uint32_t *)(LP_AON_CLKRST_BASE + (o)))

#define I2S_SLOT_BITS 16U
#define I2S_CHANNELS  2U
#define I2S_FRAME_BITS (I2S_SLOT_BITS * I2S_CHANNELS)
#define I2S_BCLK_HZ   (48000U * I2S_FRAME_BITS)
#define I2S_RX_EOF_BYTES 2048U
#define I2S_SRC_HZ    40000000U

/* I2S block offsets */
#define I2S_RX_CONF 0x20U
#define I2S_TX_CONF 0x24U
#define I2S_RX_CONF1 0x28U
#define I2S_TX_CONF1 0x2CU
#define I2S_TX_PCM2PDM_CONF 0x40U
#define I2S_RX_TDM_CTRL 0x50U
#define I2S_TX_TDM_CTRL 0x54U
#define I2S_LC_HUNG_CONF 0x60U
#define I2S_RXEOF_NUM 0x64U
#define I2S_CONF_SINGLE_DATA 0x68U
#define I2S_STATE 0x6CU
#define I2S_FIFO_CNT 0x74U
#define I2S_BCK_CNT 0x78U

/* TX/RX CONF bits */
#define TX_RESET (1U<<0)
#define TX_FIFO_RESET (1U<<1)
#define TX_START (1U<<2)
#define TX_SLAVE_MOD (1U<<3)
#define TX_UPDATE (1U<<8)
#define TX_MONO_FST_VLD (1U<<9)
#define TX_PCM_BYPASS (1U<<12)
#define TX_MSB_SHIFT (1U<<13)
#define TX_BCK_NO_DLY (1U<<14)
#define TX_TDM_EN (1U<<19)
#define TX_BCK_DIV_SHIFT 21U
#define TX_SIG_LOOPBACK (1U<<30)
#define RX_RESET (1U<<0)
#define RX_FIFO_RESET (1U<<1)
#define RX_START (1U<<2)
#define RX_SLAVE_MOD (1U<<3)
#define RX_UPDATE (1U<<8)
#define RX_MONO_FST_VLD (1U<<9)
#define RX_PCM_BYPASS (1U<<12)
#define RX_MSB_SHIFT (1U<<13)
#define RX_TDM_EN (1U<<19)
#define RX_BCK_DIV_SHIFT 21U
#define TX_IDLE (1U<<0)
#define LC_FIFO_TIMEOUT_ENA (1U<<11)

/* CONF1 field masks */
#define WS_WIDTH_MASK 0x1FFU
#define WS_WIDTH_SHIFT 0U
#define BITS_MOD_MASK (0x1FU<<14)
#define BITS_MOD_SHIFT 14U
#define HALF_SAMPLE_MASK (0x7FU<<19)
#define HALF_SAMPLE_SHIFT 19U
#define CHAN_BITS_MASK (0x1FU<<27)
#define CHAN_BITS_SHIFT 27U
#define TDM_CHAN_EN_MASK 0xFFFFU
#define TDM_CHAN_EN_SHIFT 0U
#define TDM_TOT_CHAN_MASK (0xFU<<16)
#define TDM_TOT_CHAN_SHIFT 16U

/* clock/reset */
#define C2_I2S0_APB_CLK_EN (1U<<14)
#define LP_PAD_I2S0_MCLK_EN (1U<<12)
#define C11_RX_CLK_EN (1U<<24)
#define C11_RX_SRC_SEL_MASK (0x3U<<25)
#define C11_RX_SRC_SEL_SHIFT 25U
#define C12_RX_DIV_N_MASK 0xFFU
#define C12_RX_DIV_N_SHIFT 0U
#define C12_RX_DIV_X_MASK (0x1FFU<<8)
#define C12_RX_DIV_X_SHIFT 8U
#define C12_RX_DIV_Y_MASK (0x1FFU<<17)
#define C12_RX_DIV_Y_SHIFT 17U
#define C13_TX_CLK_EN (1U<<10)
#define C13_SRC_SEL_MASK (0x3U<<11)
#define C13_SRC_SEL_SHIFT 11U
#define C13_DIV_N_MASK (0xFFU<<13)
#define C13_DIV_N_SHIFT 13U
#define C13_DIV_X_MASK (0x1FFU<<21)
#define C13_DIV_X_SHIFT 21U
#define C13_RX_DIV_Z_MASK 0x1FFU
#define C13_RX_DIV_Z_SHIFT 0U
#define C13_RX_DIV_YN1 (1U<<9)
#define C14_DIV_Y_MASK 0x1FFU
#define C14_DIV_Y_SHIFT 0U
#define C14_DIV_Z_MASK (0x1FFU<<9)
#define C14_DIV_Z_SHIFT 9U
#define C14_DIV_YN1 (1U<<18)
#define C14_MST_CLK_SEL_TX (1U<<19)

#define I2S0_SIG_BCK 25U
#define I2S0_SIG_MCLK 26U
#define I2S0_SIG_WS 27U
#define I2S0_SIG_SD 28U
#define TX_SYNC_GUARD 200000U

/* Estado I2S: se accede siempre bajo el mutex de audio (audio.c) en el unico
 * camino concurrente (comandos de audio), y desde platform_init en elarranque
 * (un solo core).  No se anade otro lock aqui para evitar inversion con
 * audio_lock; si el I2S llega a usarse desde otro subsistema, tomar audio_lock. */
static bool started, rxStarted;

static void field_write(volatile uint32_t *r, uint32_t mask, uint32_t shift, uint32_t v) {
    uint32_t x = *r;
    x = (x & ~mask) | ((v << shift) & mask);
    *r = x;
}
static uint32_t gcd(uint32_t a, uint32_t b) { while (b) { uint32_t t = a % b; a = b; b = t; } return a; }

static bool mclk_divider(uint32_t *dn, uint32_t *x, uint32_t *y, uint32_t *z, bool *yn1) {
    uint32_t g = gcd(I2S_SRC_HZ, P4_ES8311_MCLK_HZ);
    uint32_t num = I2S_SRC_HZ / g, den = P4_ES8311_MCLK_HZ / g;
    uint32_t n = num / den, b = num % den, a = den;
    if (n < 2U || n > 255U || (b != 0U && a > 511U)) return false;
    *yn1 = (b * 2U > a);
    *z = *yn1 ? a - b : b;
    *x = b == 0U ? 0U : a / *z - 1U;
    *y = b == 0U ? 0U : a % *z;
    *dn = n;
    return true;
}
static bool bclk_divider(uint32_t *div) {
    if (P4_ES8311_MCLK_HZ % I2S_BCLK_HZ != 0U) return false;
    *div = P4_ES8311_MCLK_HZ / I2S_BCLK_HZ;
    return *div >= 2U && *div <= 64U;
}
static bool tx_sync(void) {
    volatile uint32_t *r = I2S0_REGP(I2S_TX_CONF);
    uint32_t guard = TX_SYNC_GUARD;
    *r |= TX_UPDATE;
    while ((*r & TX_UPDATE) != 0U) if (--guard == 0U) return false;
    return true;
}
static bool rx_sync(void) {
    volatile uint32_t *r = I2S0_REGP(I2S_RX_CONF);
    uint32_t guard = TX_SYNC_GUARD;
    *r |= RX_UPDATE;
    while ((*r & RX_UPDATE) != 0U) if (--guard == 0U) return false;
    return true;
}
static void pad_to_clock(uint8_t pin, uint32_t sig) {
    p4_gpio_set_dir(pin, true);
    p4_gpio_matrix_out(pin, sig, false, false);
}

/* Common fractional divider setup (TX part).  Handle the double-divider errata. */
static bool clocks_common(uint32_t dn, uint32_t x, uint32_t y, uint32_t z, bool yn1) {
    CLKRST_REG(0x1CU) |= C2_I2S0_APB_CLK_EN;
    LPCLK_REG(0x40U) |= LP_PAD_I2S0_MCLK_EN;
    CLKRST_REG(0x4CU) |= C13_TX_CLK_EN;
    field_write(&CLKRST_REG(0x4CU), C13_SRC_SEL_MASK, C13_SRC_SEL_SHIFT, 0U);
    CLKRST_REG(0x50U) |= C14_MST_CLK_SEL_TX;
    /* errata: short divider first */
    field_write(&CLKRST_REG(0x4CU), C13_DIV_N_MASK, C13_DIV_N_SHIFT, 2U);
    CLKRST_REG(0x50U) &= ~C14_DIV_YN1;
    field_write(&CLKRST_REG(0x50U), C14_DIV_Y_MASK, C14_DIV_Y_SHIFT, 1U);
    field_write(&CLKRST_REG(0x50U), C14_DIV_Z_MASK, C14_DIV_Z_SHIFT, 0U);
    field_write(&CLKRST_REG(0x4CU), C13_DIV_X_MASK, C13_DIV_X_SHIFT, 0U);
    /* target divider */
    if (yn1) CLKRST_REG(0x50U) |= C14_DIV_YN1; else CLKRST_REG(0x50U) &= ~C14_DIV_YN1;
    field_write(&CLKRST_REG(0x50U), C14_DIV_Z_MASK, C14_DIV_Z_SHIFT, z);
    field_write(&CLKRST_REG(0x50U), C14_DIV_Y_MASK, C14_DIV_Y_SHIFT, y);
    field_write(&CLKRST_REG(0x4CU), C13_DIV_X_MASK, C13_DIV_X_SHIFT, x);
    field_write(&CLKRST_REG(0x4CU), C13_DIV_N_MASK, C13_DIV_N_SHIFT, dn);
    return true;
}

bool p4_i2s_clocks_start(void) {
    uint32_t dn, x, y, z, bdiv;
    bool yn1;
    if (started) return true;
    if (!mclk_divider(&dn, &x, &y, &z, &yn1) || !bclk_divider(&bdiv)) return false;
    (void)clocks_common(dn, x, y, z, yn1);

    I2S0_REG(I2S_TX_CONF) = TX_MSB_SHIFT | TX_BCK_NO_DLY | TX_MONO_FST_VLD |
                            TX_TDM_EN | TX_PCM_BYPASS | ((bdiv - 1U) << TX_BCK_DIV_SHIFT);
    I2S0_REG(I2S_LC_HUNG_CONF) &= ~LC_FIFO_TIMEOUT_ENA;
    field_write(I2S0_REGP(I2S_TX_CONF1), WS_WIDTH_MASK, WS_WIDTH_SHIFT, I2S_FRAME_BITS - 1U);
    field_write(I2S0_REGP(I2S_TX_CONF1), BITS_MOD_MASK, BITS_MOD_SHIFT, I2S_SLOT_BITS - 1U);
    field_write(I2S0_REGP(I2S_TX_CONF1), HALF_SAMPLE_MASK, HALF_SAMPLE_SHIFT, I2S_FRAME_BITS - 1U);
    field_write(I2S0_REGP(I2S_TX_CONF1), CHAN_BITS_MASK, CHAN_BITS_SHIFT, I2S_SLOT_BITS - 1U);
    field_write(I2S0_REGP(I2S_TX_TDM_CTRL), TDM_CHAN_EN_MASK, TDM_CHAN_EN_SHIFT, 0x3U);
    field_write(I2S0_REGP(I2S_TX_TDM_CTRL), TDM_TOT_CHAN_MASK, TDM_TOT_CHAN_SHIFT, I2S_CHANNELS - 1U);
    I2S0_REG(I2S_TX_PCM2PDM_CONF) = 0U;
    I2S0_REG(I2S_CONF_SINGLE_DATA) = 0U;

    pad_to_clock(P4_I2S_MCK, I2S0_SIG_MCLK);
    pad_to_clock(P4_I2S_BCK, I2S0_SIG_BCK);
    pad_to_clock(P4_I2S_WS, I2S0_SIG_WS);
    pad_to_clock(P4_I2S_DOUT, I2S0_SIG_SD);

    I2S0_REG(I2S_TX_CONF) |= TX_FIFO_RESET;
    I2S0_REG(I2S_TX_CONF) &= ~TX_FIFO_RESET;
    if (!tx_sync()) { (void)p4_i2s_clocks_stop(); return false; }
    I2S0_REG(I2S_TX_CONF) |= TX_START;
    started = true;
    return true;
}

bool p4_i2s_clocks_stop(void) {
    I2S0_REG(I2S_TX_CONF) &= ~TX_START;
    I2S0_REG(I2S_TX_CONF) |= TX_RESET;
    I2S0_REG(I2S_TX_CONF) &= ~TX_RESET;
    LPCLK_REG(0x40U) &= ~LP_PAD_I2S0_MCLK_EN;
    CLKRST_REG(0x4CU) &= ~C13_TX_CLK_EN;
    started = false;
    return true;
}
bool p4_i2s_clocks_running(void) {
    return started && (I2S0_REG(I2S_TX_CONF) & TX_START) && !(I2S0_REG(I2S_STATE) & TX_IDLE);
}
uint32_t p4_i2s_bclk_measure(uint32_t us) {
    uint32_t t0, t1, cnt;
    if (!started || us == 0U || us > 200000U) return 0U;
    I2S0_REG(I2S_BCK_CNT) = (1UL << 31);  /* reset */
    I2S0_REG(I2S_BCK_CNT) = 0U;
    t0 = (uint32_t)current_time_hires();
    spin(us);
    t1 = (uint32_t)current_time_hires();
    cnt = I2S0_REG(I2S_BCK_CNT) & 0x7FFFFFFFUL;
    if (t1 <= t0 || cnt == 0U) return 0U;
    /* BCLK ~ cnt / (t1-t0) en us; devuelvo el conteo crudo para diagnostico */
    return cnt;
}
uint32_t p4_i2s_tx_sync_count(void) { return I2S0_REG(I2S_FIFO_CNT) & 0x7FFFFFFFUL; }
void p4_i2s_tx_sync_reset(void) {
    I2S0_REG(I2S_FIFO_CNT) |= (1UL << 31);
    I2S0_REG(I2S_FIFO_CNT) &= ~(1UL << 31);
}

bool p4_i2s_rx_start(void) {
    uint32_t dn, x, y, z, bdiv;
    bool yn1;
    if (!mclk_divider(&dn, &x, &y, &z, &yn1) || !bclk_divider(&bdiv)) return false;
    if (!started && !p4_i2s_clocks_start()) return false;

    /* RX clock chain (own divider): sin esto el RX daba 8 kB/s en vez de 192 kB/s */
    CLKRST_REG(0x44U) |= C11_RX_CLK_EN;
    field_write(&CLKRST_REG(0x44U), C11_RX_SRC_SEL_MASK, C11_RX_SRC_SEL_SHIFT, 0U);
    /* errata de doble division (igual que TX): primero la corta, luego la objetivo */
    field_write(&CLKRST_REG(0x48U), C12_RX_DIV_N_MASK, C12_RX_DIV_N_SHIFT, 2U);
    CLKRST_REG(0x4CU) &= ~C13_RX_DIV_YN1;
    field_write(&CLKRST_REG(0x48U), C12_RX_DIV_Y_MASK, C12_RX_DIV_Y_SHIFT, 1U);
    field_write(&CLKRST_REG(0x4CU), C13_RX_DIV_Z_MASK, C13_RX_DIV_Z_SHIFT, 0U);
    field_write(&CLKRST_REG(0x48U), C12_RX_DIV_X_MASK, C12_RX_DIV_X_SHIFT, 0U);
    if (yn1) CLKRST_REG(0x4CU) |= C13_RX_DIV_YN1; else CLKRST_REG(0x4CU) &= ~C13_RX_DIV_YN1;
    field_write(&CLKRST_REG(0x4CU), C13_RX_DIV_Z_MASK, C13_RX_DIV_Z_SHIFT, z);
    field_write(&CLKRST_REG(0x48U), C12_RX_DIV_Y_MASK, C12_RX_DIV_Y_SHIFT, y);
    field_write(&CLKRST_REG(0x48U), C12_RX_DIV_X_MASK, C12_RX_DIV_X_SHIFT, x);
    field_write(&CLKRST_REG(0x48U), C12_RX_DIV_N_MASK, C12_RX_DIV_N_SHIFT, dn);

    p4_gpio_set_dir(P4_I2S_DIN, false);
    p4_gpio_matrix_in(P4_I2S_DIN, I2S0_SIG_SD, false);

    I2S0_REG(I2S_TX_CONF) |= TX_SIG_LOOPBACK;
    (void)tx_sync();

    I2S0_REG(I2S_RX_CONF) = RX_SLAVE_MOD | RX_MSB_SHIFT | RX_MONO_FST_VLD |
                            RX_TDM_EN | RX_PCM_BYPASS | ((bdiv - 1U) << RX_BCK_DIV_SHIFT);
    field_write(I2S0_REGP(I2S_RX_CONF1), WS_WIDTH_MASK, WS_WIDTH_SHIFT, I2S_FRAME_BITS - 1U);
    field_write(I2S0_REGP(I2S_RX_CONF1), BITS_MOD_MASK, BITS_MOD_SHIFT, I2S_SLOT_BITS - 1U);
    field_write(I2S0_REGP(I2S_RX_CONF1), HALF_SAMPLE_MASK, HALF_SAMPLE_SHIFT, I2S_FRAME_BITS - 1U);
    field_write(I2S0_REGP(I2S_RX_CONF1), CHAN_BITS_MASK, CHAN_BITS_SHIFT, I2S_SLOT_BITS - 1U);
    field_write(I2S0_REGP(I2S_RX_TDM_CTRL), TDM_CHAN_EN_MASK, TDM_CHAN_EN_SHIFT, 0x3U);
    field_write(I2S0_REGP(I2S_RX_TDM_CTRL), TDM_TOT_CHAN_MASK, TDM_TOT_CHAN_SHIFT, I2S_CHANNELS - 1U);
    I2S0_REG(I2S_RXEOF_NUM) = I2S_RX_EOF_BYTES - 1U;

    I2S0_REG(I2S_RX_CONF) |= RX_FIFO_RESET;
    I2S0_REG(I2S_RX_CONF) &= ~RX_FIFO_RESET;
    if (!rx_sync()) return false;
    I2S0_REG(I2S_RX_CONF) |= RX_START;
    rxStarted = true;
    return true;
}
bool p4_i2s_rx_stop(void) {
    I2S0_REG(I2S_RX_CONF) &= ~RX_START;
    I2S0_REG(I2S_RX_CONF) |= RX_RESET;
    I2S0_REG(I2S_RX_CONF) &= ~RX_RESET;
    I2S0_REG(I2S_TX_CONF) &= ~TX_SIG_LOOPBACK;
    (void)tx_sync();
    CLKRST_REG(0x44U) &= ~C11_RX_CLK_EN;
    rxStarted = false;
    return true;
}
bool p4_i2s_rx_running(void) {
    return rxStarted && (I2S0_REG(I2S_RX_CONF) & RX_START);
}
