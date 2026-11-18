/*
 * Demo app: contador que sigue corriendo en background.
 * Swipe ↑ desde borde inferior en raíz -> background (miniatura) y vuelve launcher.
 * Reanudar desde Recientes retoma el estado/UI. Si launcher pide cerrar, sale.
 */
#include "../lib/lkui.h"

#define W 320
#define H 480
#define FB_PIXELS (W*H)
#define FB_BYTES  (FB_PIXELS*2)

#define C_BG 0x0010
#define C_TX 0xFFFF
#define C_AC 0x07E0
#define C_DIM 0x8410

static volatile int g_count;
static volatile int g_worker_run;

static int worker(void *arg) {
    (void)arg;
    while (g_worker_run) {
        g_count++;
        thread_sleep(1000);
    }
    return 0;
}

static void draw(gfx_surface *s, int count) {
    gfx_fillrect(s, 0, 0, W, H, C_BG);
    gfx_fillrect(s, 0, 0, W, 50, 0x0008);
    ui_draw_text(s, 10, 16, "Demo BG (contador)", C_TX);
    char buf[64];
    snprintf(buf, sizeof(buf), "Segundos: %d", count);
    ui_draw_text(s, 20, 120, buf, C_AC);
    ui_draw_text(s, 20, 160, "Sigue contando en background", C_DIM);
    ui_draw_text(s, 20, 180, "aunque abras otra app.", C_DIM);
    ui_draw_text(s, 20, 230, "Swipe ^ (borde inf) = bg", C_TX);
    ui_draw_text(s, 20, 250, "Reanuda desde Recientes", C_TX);
}

int main(int argc, char **argv) {
    const char *path = (argc > 0 && argv && argv[0]) ? argv[0] : "/sd/appslkx/demo.elf";
    int slot = appmgr_find_path(path);
    /* si launcher no registró, intentar registrar */
    if (slot < 0) slot = appmgr_register("Demo", path, 0x07E0, get_current_thread());

    g_count = 0;
    g_worker_run = 1;
    thread_t *wt = thread_create("demo_w", worker, NULL, DEFAULT_PRIORITY, DEFAULT_STACK_SIZE);
    if (wt) thread_resume(wt);

    uint16_t *fb = (uint16_t *)psram_malloc(FB_BYTES);
    if (!fb) { g_worker_run = 0; return -1; }
    gfx_surface *s = gfx_create_surface(fb, W, H, W, GFX_FORMAT_RGB_565);
    if (!s) { psram_free(fb); g_worker_run = 0; return -1; }

    ui_gesture_t gest;
    bool need_redraw = true;
    int last_drawn = -1;

    for (;;) {
        /* si estamos en foreground, tomar pantalla y dibujar/interactuar */
        if (appmgr_get_state(slot) == APP_RUNNING) {
            fbcon_p4_suspend();
            p4_touch_init();
            ui_gesture_init(&gest, W, H);
            need_redraw = true;
            while (appmgr_get_state(slot) == APP_RUNNING) {
                if (need_redraw || g_count != last_drawn) {
                    draw(s, g_count);
                    last_drawn = g_count;
                    arch_clean_cache_range((unsigned long)fb, FB_BYTES);
                    fbcon_p4_present(fb);
                    need_redraw = false;
                }
                ui_evt_t ev;
                while (ui_gesture_poll(&gest, &ev)) {
                    if (ev.type == UI_EVT_SWIPE && ev.edge == UI_EDGE_BOTTOM && ev.dir == UI_DIR_UP) {
                        /* raíz -> background cooperativo */
                        appmgr_suspend_background(slot, fb, W, H, 64, 96);
                        /* salir del bucle foreground, ir a esperar resume/close */
                        goto suspended;
                    }
                }
                thread_sleep(100);
            }
        }

suspended:
        if (appmgr_get_state(slot) == APP_CLOSING) break;
        if (appmgr_get_state(slot) == APP_FREE || appmgr_get_state(slot) == APP_EXITED) break;
        /* esperar reanudación o cierre */
        int ns = appmgr_wait_resume(slot);
        if (ns == APP_CLOSING || ns == APP_FREE || ns == APP_EXITED) break;
        /* ns == APP_RUNNING -> loop retomará y redibujará */
        need_redraw = true;
    }

    g_worker_run = 0;
    if (wt) thread_join(wt, NULL, 2000);
    gfx_surface_destroy(s);
    psram_free(fb);
    if (slot >= 0) appmgr_mark_exited(slot);
    return 0;
}
