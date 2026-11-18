/*
 * Consola de framebuffer (fbcon) sobre el ST7796 (320x480) del ESP32-P4.
 *
 * El log del kernel se captura desde el primer printf (lib/libc/printf.c) en
 * un ring buffer y se vuelca aqui en cuanto el LCD esta listo; desde entonces
 * el hook fbcon_putc_hook() alimenta fbcon_putc() en vivo.  fbcon.c dibuja
 * 5x12 con scroll sobre un framebuffer lineal en PSRAM; un thread de refresh
 * hace un blit con flip-Y (compensa el MY del MADCTL=0x88) cuando hay cambios.
 */
#include <dev/fbcon.h>
#include <platform/esp32p4_lcd.h>
#include <platform/psram_heap.h>
#include <platform/esp32p4.h>
#include <lk/init.h>
#include <kernel/thread.h>
#include <kernel/mutex.h>
#include <stdio.h>
#include <string.h>

#define PANEL_W 320U
#define PANEL_H 480U
#define FB_SZ   (PANEL_W * PANEL_H * 2U)
#define PSRAM_DIRECT 0x40000000UL   /* 0x48.. cacheado -> 0x88.. directo */

/* Terminal clasico: fondo negro, texto verde claro. */
#define C_BG 0x0000U
#define C_FG 0x07E0U

static uint16_t *fb;        /* alias directo 0x88 */
static uint16_t *fb_alloc;  /* alias original 0x48 */
static mutex_t blit_lock = MUTEX_INITIAL_VALUE(blit_lock);
static volatile bool fb_dirty;
static bool fb_ready;
static volatile bool fb_suspended;
static char log_tmp[32768];  /* estatico: no cabe en el stack del init hook */

/* update callbacks de fbcon: solo marcan sucio (se llaman bajo print lock,
 * con IRQs off: nothing heavy here).  update_done debe devolver != 0 siempre
 * para que fbcon_flush() no gire en bucle. */
static void fbcon_update_start(void) { if (!fb_suspended) fb_dirty = true; }
static int fbcon_update_done(void) { return 1; }

static struct fbcon_config fbconfig = {
    .base    = NULL,
    .width   = PANEL_W,
    .height  = PANEL_H,
    .stride  = PANEL_W,
    .bpp     = 16,
    .format  = FB_FORMAT_RGB565,
    .update_start = fbcon_update_start,
    .update_done  = fbcon_update_done,
};

/* Hook fuerte (el debil esta en printf.c): refleja cada printf en el fbcon.
 * Solo escribe memoria del framebuffer (bajo print_spin_lock), sin SPI.
 * Cuando la GUI toma la pantalla, no se dibuja en el fb para no corromperla. */
void fbcon_putc_hook(char c) {
    if (fb_suspended) return;
    fbcon_putc(c);   /* fbcon ignora llamadas antes de fbcon_setup() */
}

/* Volcar el log capturado antes de que existiera el fbcon. */
static void dump_captured_log(void) {
    size_t n = printf_get_log(log_tmp, sizeof(log_tmp));
    for (size_t i = 0; i < n; i++) fbcon_putc(log_tmp[i]);
    fb_dirty = true;
}

/* Enviar el framebuffer al panel.  Se llama fuera del print lock (thread de
 * refresh o comando), asi que puede tomar blit_lock sin riesgo. */
static void fbcon_blit(void) {
    if (!fb_ready) return;
    mutex_acquire(&blit_lock);
    /*fbcon pinta fila 0 arriba; el MY de 0x88 la pondria abajo -> flipy */
    p4_lcd_blit_flipy(0, 0, PANEL_W, PANEL_H, fb);
    fb_dirty = false;
    mutex_release(&blit_lock);
}

/* Refresco periodico: el log sigue creciendo (wifi, FAT, comandos...). */
static int fbcon_refresh_thread(void *arg) {
    (void)arg;
    for (;;) {
        thread_sleep(150);
        if (fb_dirty && !fb_suspended) fbcon_blit();
    }
    return 0;
}

/* --- API para que la GUI tome la pantalla --- */
bool fbcon_p4_is_ready(void) { return fb_ready; }
uint16_t *fbcon_p4_framebuffer(void) { return fb; }
unsigned fbcon_p4_width(void) { return PANEL_W; }
unsigned fbcon_p4_height(void) { return PANEL_H; }

void fbcon_p4_suspend(void) {
    fb_suspended = true;
    fb_dirty = false;
}

void fbcon_p4_resume(void) {
    fb_suspended = false;
    fb_dirty = true;
}

void fbcon_p4_present(const uint16_t *pixels) {
    if (!fb_ready) return;
    mutex_acquire(&blit_lock);
    p4_lcd_blit_flipy(0, 0, PANEL_W, PANEL_H, pixels ? pixels : fb);
    mutex_release(&blit_lock);
}

/* Llamado por el comando `fbshow': fuerza un re-blit inmediato (el log ya
 * esta en el framebuffer, no hay que volcarlo otra vez). */
void p4_fbcon_show(void) {
    if (!fb_ready) return;
    fb_dirty = true;
    fbcon_blit();
}

static void fbcon_p4_init(uint level) {
    (void)level;
    psram_heap_init();
    fb_alloc = psram_malloc(FB_SZ);
    if (!fb_alloc) return;
    fb = (uint16_t *)((uintptr_t)fb_alloc + PSRAM_DIRECT);
    if (!p4_lcd_init()) return;

    fbconfig.base = fb;
    fbcon_setup(&fbconfig);             /* limpia con RGB565_BLUE/WHITE */
    fbcon_set_colors(C_BG, C_FG);       /* fondo negro, texto verde */
    for (uint32_t i = 0; i < PANEL_W * PANEL_H; i++) fb[i] = C_BG;

    fb_ready = true;
    dump_captured_log();                /* muestra todo el log de arranque */
    fbcon_blit();

    /* Refresco en vivo para printf posteriores.  Prioridad baja, en core 1
     * para no competir con la red/minip (core 0) y dar fluidez a la UI. */
    thread_t *t = thread_create("fbcon", fbcon_refresh_thread, NULL,
                                LOW_PRIORITY, 2048);
    if (t) {
        thread_set_pinned_cpu(t, 1);
        thread_resume(t);
    }
}

LK_INIT_HOOK(fbcon_p4, fbcon_p4_init, LK_INIT_LEVEL_PLATFORM);

/* Re-volcar el log completo al final del arranque.  El volcado de PLATFORM
 * solo alcanzaba hasta psram_heap; los mensajes de SD/wifi/httpd/telnet/udp
 * llegaban despues y el hook en vivo no los reflejaba todos.  Aqui se vuelca
 * todo lo capturado (hasta 32KB) y se fuerza el blit para que la pantalla
 * muestre el boot completo como antes de la GUI. */
static void fbcon_p4_late(uint level) {
    (void)level;
    if (!fb_ready || fb_suspended) return;
    fbcon_clear();
    dump_captured_log();
    fbcon_blit();
}
LK_INIT_HOOK(fbcon_p4_late, fbcon_p4_late, LK_INIT_LEVEL_APPS);

/* API publica: volcar el log completo cuando lo pida el shell (tras autorun). */
void fbcon_p4_dump_full_log(void) {
    if (!fb_ready || fb_suspended) return;
    fbcon_clear();
    dump_captured_log();
    fbcon_blit();
}
