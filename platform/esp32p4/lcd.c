/*
 * ST7796 320x480 sobre GPSPI2 — subconjunto minimo de Xinu
 * device/lcd-esp32p4/{lcdSpi,lcdGpio,st7796}.  Sin DMA, PIO de 64 bytes.
 */
#include <platform/esp32p4_lcd.h>
#include <platform/esp32p4_board.h>
#include <platform/esp32p4.h>
#include <platform/time.h>
#include "font8x16.h"
#include <kernel/mutex.h>

#define LCD_SPI_HZ       80000000UL
#define SPI2_BASE        0x500D0000UL
#define LCD_W            320
#define LCD_H            480

/* GPSPI2 registers */
#define SPI_CMD      0x00UL
#define SPI_CTRL     0x08UL
#define SPI_CLOCK    0x0CUL
#define SPI_USER     0x10UL
#define SPI_MS_DLEN  0x1CUL
#define SPI_MISC     0x20UL
#define SPI_W0       0x98UL
#define SPI_CLK_GATE 0xE8UL
#define SPI_CMD_UPDATE (1UL << 23)
#define SPI_CMD_USR    (1UL << 24)
#define SPI_CLK_EQU_SYSCLK (1UL << 31)
#define SPI_CLK_CNT_N_MASK 0x3FUL
#define SPI_CLK_CNT_H_MASK 0x3FUL
#define SPI_CLK_CNT_L_MASK 0x3FUL
#define SPI_USER_CK_OUT_EDGE (1UL << 9)
#define SPI_USER_DOUTDIN (1UL << 0)
#define SPI_USER_USR_CMD (1UL << 31)
#define SPI_USER_USR_ADDR (1UL << 30)
#define SPI_USER_USR_DUMMY (1UL << 29)
#define SPI_USER_USR_MISO (1UL << 28)
#define SPI_USER_USR_MOSI (1UL << 27)
#define SPI_DLEN_MASK 0x3FFFFUL
#define SPI_MISC_CK_IDLE (1UL << 29)
#define SPI_GATE_CLK_EN (1UL << 0)
#define SPI_GATE_MST_ACTIVE (1UL << 1)
#define SPI_GATE_MST_SEL80 (1UL << 2)
#define SPI_MODULE_CLK 80000000UL
#define SPI_CHUNK 64U
#define SIG_SPI2_CK 53U
#define SIG_SPI2_D  55U

/* clock/reset */
#define CLKRST_SOC_CTRL1 0x18UL  /* wait: Xinu usa 0x00 para SOC_CTRL1 */
#define CLKRST_SOC_CTRL2 0x1CUL
#define CLKRST_PERI_CTRL116 0x80UL
#define CLK_GPSPI2_SYS  (1UL << 0)   /* SOC_CTRL1 (offset 0x00 en Xinu) */
#define CLK_GPSPI2_APB  (1UL << 19)
#define CLK_GPSPI2_HS   (1UL << 3)
#define CLK_GPSPI2_MST  (1UL << 20)

/* GPIO/IO_MUX */
#define GPIO_BASE_ 0x500E0000UL
#define IOMUX_BASE 0x500E1000UL
#define G_OUT_W1TS (GPIO_BASE_ + 0x08UL)
#define G_OUT_W1TC (GPIO_BASE_ + 0x0CUL)
#define G_EN_W1TS  (GPIO_BASE_ + 0x24UL)
#define G_OUT_SEL(p) (GPIO_BASE_ + 0x558UL + 4UL*(p))
#define G_MUX(p)   (IOMUX_BASE + 4UL + 4UL*(p))
#define G_SEL_MASK 0x1FFUL
#define G_OEN_SEL  (1UL << 10)
#define G_SEL_GPIO 256UL
#define IO_MCU_SEL_MASK (7UL << 12)
#define IO_MCU_SEL_GPIO (1UL << 12)
#define IO_FUN_IE (1UL << 9)
#define IO_DRV40  (3UL << 10)

/* ST7796 commands */
#define CMD_SLPOUT 0x11
#define CMD_DISPON 0x29
#define CMD_CASET  0x2A
#define CMD_RASET  0x2B
#define CMD_RAMWR  0x2C
#define CMD_MADCTL 0x36
#define CMD_COLMOD 0x3A
#define CMD_INVON  0x21

/* Serializa el bus SPI2 y el panel ST7796: hoy solo el comando `lcd`, pero
 * con fbcon el printf del kernel (ambos cores) tambien dibujara. */
static mutex_t lcd_lock = MUTEX_INITIAL_VALUE(lcd_lock);
static bool spi_ready, lcd_up;

#define SPIREG(o) (*(volatile uint32_t *)(SPI2_BASE + (o)))
#define SPIWORD(n) (*(volatile uint32_t *)(SPI2_BASE + SPI_W0 + 4UL*(n)))

static void pin_mux(uint8_t pin) {
    volatile uint32_t *p = (volatile uint32_t *)G_MUX(pin);
    uint32_t v = *p;
    v &= ~IO_MCU_SEL_MASK;
    v |= IO_MCU_SEL_GPIO | IO_FUN_IE | IO_DRV40;
    *p = v;
}
static void matrix_out(uint8_t pin, uint32_t sig, bool oen) {
    volatile uint32_t *r = (volatile uint32_t *)G_OUT_SEL(pin);
    uint32_t v = *r;
    v &= ~(G_SEL_MASK | G_OEN_SEL);
    v |= (sig & G_SEL_MASK);
    if (oen) v |= G_OEN_SEL;
    *r = v;
}
static void pin_setup(uint8_t pin, bool level) {
    uint32_t m = 1UL << (pin < 32U ? pin : pin - 32U);
    pin_mux(pin);
    matrix_out(pin, G_SEL_GPIO, true);
    *(volatile uint32_t *)(level ? G_OUT_W1TS : G_OUT_W1TC) = m;
    *(volatile uint32_t *)G_EN_W1TS = m;
}
static void pin_write(uint8_t pin, bool level) {
    uint32_t m = 1UL << (pin < 32U ? pin : pin - 32U);
    *(volatile uint32_t *)(level ? G_OUT_W1TS : G_OUT_W1TC) = m;
}

static bool spi_done(uint32_t bit) {
    lk_time_t t0 = current_time();
    while ((SPIREG(SPI_CMD) & bit) != 0)
        if (current_time() - t0 > 100U) return false;
    return true;
}
static size_t latched_len;
static bool spi_setlen(size_t len) {
    if (len == latched_len) return true;
    SPIREG(SPI_MS_DLEN) = (SPIREG(SPI_MS_DLEN) & ~SPI_DLEN_MASK) | (len * 8UL - 1UL);
    SPIREG(SPI_CMD) = SPI_CMD_UPDATE;
    if (!spi_done(SPI_CMD_UPDATE)) return false;
    latched_len = len;
    return true;
}
static bool spi_write_unlocked(const uint8_t *data, size_t len) {
    uint32_t w[16];
    lk_time_t t0 = current_time();
    if (!data || len == 0 || len > SPI_CHUNK) return false;
    for (int i = 0; i < 16; i++) w[i] = 0;
    for (size_t i = 0; i < len; i++) w[i/4] |= (uint32_t)data[i] << (8U*(i%4));
    if (!spi_setlen(len)) return false;
    uint32_t nw = (uint32_t)((len + 3U)/4U);
    for (uint32_t i = 0; i < nw; i++) SPIWORD(i) = w[i];
    SPIREG(SPI_CMD) = SPI_CMD_USR;
    while ((SPIREG(SPI_CMD) & SPI_CMD_USR) != 0)
        if (current_time() - t0 > 100U) return false;
    return true;
}
static void spi_init(void) {
    if (spi_ready) return;
    /* GPSPI2 clock gates */
    *(volatile uint32_t *)(P4_HP_SYS_CLKRST_BASE + 0x00UL) |= CLK_GPSPI2_SYS;
    *(volatile uint32_t *)(P4_HP_SYS_CLKRST_BASE + 0x1CUL) |= CLK_GPSPI2_APB;
    *(volatile uint32_t *)(P4_HP_SYS_CLKRST_BASE + 0x80UL) |= (CLK_GPSPI2_HS | CLK_GPSPI2_MST);
    pin_mux(P4_LCD_SCLK); pin_mux(P4_LCD_MOSI);
    matrix_out(P4_LCD_SCLK, SIG_SPI2_CK, false);
    matrix_out(P4_LCD_MOSI, SIG_SPI2_D, false);
    SPIREG(SPI_CLK_GATE) |= (SPI_GATE_CLK_EN | SPI_GATE_MST_ACTIVE | SPI_GATE_MST_SEL80);
    SPIREG(SPI_CLOCK) = SPI_CLK_EQU_SYSCLK;  /* 80 MHz directo */
    SPIREG(SPI_CMD) = SPI_CMD_UPDATE; spi_done(SPI_CMD_UPDATE);
    SPIREG(SPI_CTRL) = 0;
    SPIREG(SPI_MISC) |= SPI_MISC_CK_IDLE;     /* mode 3 */
    uint32_t u = SPIREG(SPI_USER);
    u &= ~(SPI_USER_CK_OUT_EDGE | SPI_USER_DOUTDIN | SPI_USER_USR_CMD |
           SPI_USER_USR_ADDR | SPI_USER_USR_DUMMY | SPI_USER_USR_MISO);
    u |= SPI_USER_USR_MOSI;
    SPIREG(SPI_USER) = u;
    SPIREG(SPI_CMD) = SPI_CMD_UPDATE; spi_done(SPI_CMD_UPDATE);
    spi_ready = true;
}

/* --- panel --- */
static bool spi_write(const uint8_t *data, size_t len) {
    bool ok;
    mutex_acquire(&lcd_lock);
    ok = spi_write_unlocked(data, len);
    mutex_release(&lcd_lock);
    return ok;
}

static bool panel_cmd(uint8_t cmd) {
    pin_write(P4_LCD_DC, false);
    pin_write(P4_LCD_CS, false);
    bool ok = spi_write(&cmd, 1);
    pin_write(P4_LCD_CS, true);
    return ok;
}
static bool panel_cmddata(uint8_t cmd, const uint8_t *d, size_t n) {
    if (!panel_cmd(cmd)) return false;
    if (!n) return true;
    pin_write(P4_LCD_DC, true);
    pin_write(P4_LCD_CS, false);
    bool ok = spi_write(d, n);
    pin_write(P4_LCD_CS, true);
    return ok;
}
static bool panel_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    uint8_t d[4];
    d[0]=x0>>8; d[1]=x0; d[2]=x1>>8; d[3]=x1;
    bool ok = panel_cmddata(CMD_CASET, d, 4);
    d[0]=y0>>8; d[1]=y0; d[2]=y1>>8; d[3]=y1;
    ok = panel_cmddata(CMD_RASET, d, 4) && ok;
    return panel_cmd(CMD_RAMWR) && ok;
}
/* Enviar bytes RGB565 crudos al area de pixel ya abierta (CS/DC gestionados
 * aqui).  Con color!=NULL se genera un patron constante; con color==NULL se
 * envian los datos de `src'. */
static bool panel_data(const uint8_t *src, uint16_t color, uint32_t count) {
    uint8_t chunk[SPI_CHUNK];
    if (src == NULL) {
        for (int i = 0; i < SPI_CHUNK; i += 2) { chunk[i]=color>>8; chunk[i+1]=color; }
    }
    pin_write(P4_LCD_DC, true);
    pin_write(P4_LCD_CS, false);
    bool ok = true;
    const uint8_t *p = src ? src : chunk;   /* unico puntero de lectura */
    size_t left = (size_t)count * 2U;
    while (left > 0 && ok) {
        size_t n = left > SPI_CHUNK ? SPI_CHUNK : left;
        if (!spi_write(p, n)) { ok = false; break; }
        p += n; left -= n;                  /* avanzar UNA vez */
    }
    pin_write(P4_LCD_CS, true);
    return ok;
}
static bool panel_push(uint16_t color, uint32_t count) {
    return panel_data(NULL, color, count);
}

bool p4_lcd_fill(uint16_t color) {
    if (!lcd_up) return false;
    if (!panel_window(0, 0, LCD_W-1, LCD_H-1)) return false;
    return panel_push(color, (uint32_t)LCD_W * LCD_H);
}

static bool blit_internal(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                          const uint16_t *pixels, bool flipy) {
    if (!lcd_up || !pixels) return false;
    if (x >= LCD_W || y >= LCD_H) return false;
    if ((uint32_t)x + w > LCD_W) w = LCD_W - x;
    if ((uint32_t)y + h > LCD_H) h = LCD_H - y;
    if (!w || !h) return true;
    if (!panel_window(x, y, x + w - 1U, y + h - 1U)) return false;

    /* El framebuffer es little-endian (0x001f), pero el ST7796 espera el
     * byte alto primero (big-endian): convertimos por filas.  Con flipy leemos
     * las filas en orden inverso para compensar el bit MY del MADCTL (0x88):
     * fbcon dibuja la fila 0 arriba, pero el panel escanea la fila 0 abajo. */
    uint16_t row[LCD_W];
    for (uint16_t ry = 0; ry < h; ry++) {
        uint32_t sy = flipy ? (uint32_t)(h - 1U - ry) : (uint32_t)ry;
        for (uint16_t rx = 0; rx < w; rx++) {
            uint16_t c = pixels[sy * w + rx];
            row[rx] = (uint16_t)((c >> 8) | (c << 8));  /* swap16 */
        }
        if (!panel_data((const uint8_t *)row, 0, w)) return false;
    }
    return true;
}

bool p4_lcd_blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *pixels) {
    return blit_internal(x, y, w, h, pixels, false);
}

/*Como p4_lcd_blit pero invierte el eje Y (para framebuffers lineales con
 * fila 0 arriba cuando el panel usa MADCTL=0x88 / MY=1). */
bool p4_lcd_blit_flipy(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *pixels) {
    return blit_internal(x, y, w, h, pixels, true);
}

bool p4_lcd_init(void) {
    spi_init();
    pin_setup(P4_LCD_CS, true);
    pin_setup(P4_LCD_DC, false);
    pin_setup(P4_LCD_BL, false);
    pin_setup(P4_LCD_RST, true);

    spin(100000U);                    /* 100 ms */
    pin_write(P4_LCD_RST, false); spin(120000U);
    pin_write(P4_LCD_RST, true);  spin(120000U);

    /* Secuencia EXACTA de Xinu st7796.c (Arduino_GFX): gamma, vendor regs,
     * SLPOUT, NORSET, DISPON, INVON, MADCTL.  Sin la gamma el panel queda con
     * contraste/color erratico (ruido, fondo raro). */
    static const uint8_t gammaE0[14] = {
        0xF0,0x09,0x0B,0x06,0x04,0x15,0x2F,0x54,0x42,0x3C,0x17,0x14,0x18,0x1B
    };
    static const uint8_t gammaE1[14] = {
        0xE0,0x09,0x0B,0x06,0x04,0x03,0x2B,0x43,0x42,0x3B,0x16,0x14,0x17,0x1B
    };
    static const uint8_t b6[3] = { 0x80, 0x22, 0x3B };
    static const uint8_t e8[8] = { 0x40,0x8A,0x00,0x00,0x29,0x19,0xA5,0x33 };
    uint8_t v;
    v = 0x55; if (!panel_cmddata(CMD_COLMOD, &v, 1)) return false;
    v = 0xC3; if (!panel_cmddata(0xF0, &v, 1)) return false;
    v = 0x96; if (!panel_cmddata(0xF0, &v, 1)) return false;
    v = 0x01; if (!panel_cmddata(0xB4, &v, 1)) return false;
    if (!panel_cmddata(0xB6, b6, 3)) return false;
    if (!panel_cmddata(0xE8, e8, 8)) return false;
    v = 0x06; if (!panel_cmddata(0xC1, &v, 1)) return false;
    v = 0xA7; if (!panel_cmddata(0xC2, &v, 1)) return false;
    v = 0x18; if (!panel_cmddata(0xC5, &v, 1)) return false;
    if (!panel_cmddata(0xE0, gammaE0, 14)) return false;
    if (!panel_cmddata(0xE1, gammaE1, 14)) return false;
    v = 0x3C; if (!panel_cmddata(0xF0, &v, 1)) return false;
    v = 0x69; if (!panel_cmddata(0xF0, &v, 1)) return false;
    if (!panel_cmd(CMD_SLPOUT)) return false;
    spin(120000U);
    if (!panel_cmd(0x38)) return false;      /* NORSET */
    if (!panel_cmd(CMD_DISPON)) return false;
    spin(120000U);
    if (!panel_cmd(CMD_INVON)) return false;

    /* MADCTL por defecto: 0x88 (MY|BGR).  Es el que esta placa necesita para
     * que el panel lea correctamente (medido en HW).  MY invierte el eje Y del
     * controlador; el splash compensa escribiendo fb_y = PANEL_H-1-y, igual
     * que LVGL compensa en su pipeline.  Se puede cambiar con p4_lcd_set_rot(). */
    {
        uint8_t mad = 0x88;
        if (!panel_cmddata(CMD_MADCTL, &mad, 1)) return false;
    }

    lcd_up = true;
    p4_lcd_fill(0x0000);              /* pantalla negra */
    pin_write(P4_LCD_BL, true);       /* backlight ON */
    return true;
}

/* Texto 8x16 directo al panel, sin framebuffer: copia exacta de Xinu
 * lcdPanelText().  Cada caracter se expande a RGB565 MSB-primero y se envia
 * en una ventana de 8x16 con panelData.  Asi no hay swap ni caché de por
 * medio. */
#define FT_W 8U
#define FT_H 16U
void p4_lcd_text(const char *s, uint16_t x, uint16_t y, uint16_t fg, uint16_t bg) {
    uint8_t glyph[FT_W * FT_H * 2];
    if (!s) return;
    while (*s) {
        unsigned char ch = (unsigned char)*s++;
        if (ch < 32 || ch > 126) ch = '?';
        unsigned gi = 0;
        for (unsigned row = 0; row < FT_H; row++) {
            uint8_t bits = font8x16[ch - 32][row];
            for (unsigned col = 0; col < FT_W; col++) {
                uint16_t c = (bits & (0x80U >> col)) ? fg : bg;
                glyph[gi++] = (uint8_t)(c >> 8);
                glyph[gi++] = (uint8_t)c;
            }
        }
        if ((uint32_t)x + FT_W <= LCD_W && (uint32_t)y + FT_H <= LCD_H) {
            if (!panel_window(x, y, x + FT_W - 1U, y + FT_H - 1U)) return;
            if (!panel_data(glyph, 0, FT_W * FT_H)) return;
        }
        x += FT_W;
        if ((uint32_t)x + FT_W > LCD_W) { x = 0; y += FT_H; }
    }
}

void p4_lcd_set_rot(uint8_t rot) {
    static const uint8_t madctl[4] = { 0x08, 0xC8, 0x48, 0x88 };
    if ((rot & 1U) || rot > 6U) return;
    panel_cmddata(CMD_MADCTL, &madctl[rot >> 1], 1);
}

bool p4_lcd_test(void) {
    if (!lcd_up && !p4_lcd_init()) return false;
    static const uint16_t colors[8] = {
        0xF800, 0x07E0, 0x001F, 0xFFFF, 0xFFE0, 0xF81F, 0x07FF, 0x0000
    };
    for (int i = 0; i < 8; i++) {
        if (!panel_window(0, (uint16_t)(i*60), LCD_W-1, (uint16_t)(i*60+59))) return false;
        if (!panel_push(colors[i], (uint32_t)LCD_W * 60U)) return false;
    }
    return true;
}
