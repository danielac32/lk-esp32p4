/*
 * Interfaz grafica / launcher de aplicaciones para LK (ESP32-P4).
 *
 * Usa las capacidades nativas de LK: lib/gfx + lib/font para dibujar,
 * touch FT6336U (p4_touch_*) para entrada, lib/fs para listar apps en SD.
 * Sin LVGL. Corre en core 1 para no competir con la red/minip en core 0.
 *
 * Flujo: toma la pantalla (suspende fbcon), muestra un lanzador que lista
 * /sd/apps (configurable en kernel.json) y permite seleccionar/ejecutar
 * archivos .elf.  Al salir restaura fbcon.
 *
 * Comandos:
 *   gui              - inicia la interfaz grafica
 *   gui autostart    - inicia solo si gui.autostart esta activo en config
 *   gui stop         - solicita salida de la interfaz
 */
#include <app.h>
#include <app/netcfg.h>
#include <lk/console_cmd.h>
#include <lk/debug.h>
#include <lk/err.h>
#include <kernel/thread.h>
#include <kernel/mutex.h>
#include <lib/fs.h>
#include <lib/gfx.h>
#include <lib/font.h>
#include <lib/elfload.h>
#include <platform.h>
#include <platform/esp32p4_lcd.h>
#include <platform/esp32p4_touch.h>
#include <platform/psram_heap.h>
#include <dev/fbcon_p4.h>
#include <dev/display.h>
#include <arch/ops.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Stub debil: este target no implementa dev/display; lib/gfx trae comandos
 * de consola que lo usan y el enlazador lo requiere aunque no se ejecuten. */
__WEAK status_t display_get_framebuffer(struct display_framebuffer *fb) {
    (void)fb;
    return ERR_NOT_SUPPORTED;
}

#define SCREEN_W 320
#define SCREEN_H 480
#define FB_PIXELS (SCREEN_W * SCREEN_H)
#define FB_BYTES  (FB_PIXELS * 2)

/* Colores RGB565 */
#define C_BG       0x1082  /* gris azulado oscuro */
#define C_HEADER   0x0014  /* azul oscuro */
#define C_ACCENT   0x07E0  /* verde */
#define C_TEXT     0xFFFF  /* blanco */
#define C_MUTED    0x8410  /* gris */
#define C_ITEM     0x2104  /* gris oscuro */
#define C_ITEM_SEL 0x03EF  /* azul brillante */
#define C_WARN     0xFD20  /* naranja */

#define MAX_APPS      32
#define APP_NAME_MAX  48
#define APP_PATH_MAX  128
#define ITEMS_PER_PAGE 6
#define ITEM_H        52
#define ITEM_Y0       110

typedef struct {
    char name[APP_NAME_MAX];
    char path[APP_PATH_MAX];
    bool is_dir;
} gui_app_entry_t;

static volatile bool gui_running;
static volatile bool gui_stop_req;
static thread_t *gui_thread;
static mutex_t gui_lock = MUTEX_INITIAL_VALUE(gui_lock);

static uint16_t *gui_fb;
static gfx_surface *gui_surf;

static gui_app_entry_t apps[MAX_APPS];
static int app_count;
static int sel_index;
static int scroll_top;

/* ---- utilidades de dibujo ---- */
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void draw_text(gfx_surface *s, int x, int y, const char *str, uint32_t color) {
    while (*str) {
        if (*str == '\n') break;
        font_draw_char(s, (unsigned char)*str, x, y, color);
        x += FONT_X;
        str++;
    }
}

static void draw_text_clipped(gfx_surface *s, int x, int y, int max_px, const char *str, uint32_t color) {
    int limit = x + max_px;
    while (*str && x + FONT_X <= limit) {
        if (*str == '\n') break;
        font_draw_char(s, (unsigned char)*str, x, y, color);
        x += FONT_X;
        str++;
    }
}

static void draw_centered(gfx_surface *s, int y, const char *str, uint32_t color) {
    int w = (int)strlen(str) * FONT_X;
    int x = (SCREEN_W - w) / 2;
    if (x < 0) x = 0;
    draw_text(s, x, y, str, color);
}

static void fill_roundish_rect(gfx_surface *s, int x, int y, int w, int h, uint32_t color) {
    /* rectangulo simple (sin esquinas redondeadas reales para simplicidad) */
    gfx_fillrect(s, x, y, w, h, color);
}

static void draw_button(gfx_surface *s, int x, int y, int w, int h,
                        const char *label, bool selected, bool is_dir) {
    uint32_t bg = selected ? C_ITEM_SEL : C_ITEM;
    fill_roundish_rect(s, x, y, w, h, bg);
    /* borde superior claro si seleccionado */
    if (selected) {
        gfx_fillrect(s, x, y, w, 2, C_ACCENT);
        gfx_fillrect(s, x, y + h - 2, w, 2, C_ACCENT);
    }
    const char *prefix = is_dir ? "[DIR] " : "[ELF] ";
    uint32_t pcol = is_dir ? C_WARN : C_ACCENT;
    draw_text(s, x + 8, y + 8, prefix, pcol);
    int tx = x + 8 + (int)strlen(prefix) * FONT_X;
    draw_text_clipped(s, tx, y + 8, w - (tx - x) - 8, label, C_TEXT);
    /* ruta/ayuda pequena */
    char hint[32];
    snprintf(hint, sizeof(hint), "%s", is_dir ? "carpeta" : "tocar para ejecutar");
    draw_text_clipped(s, x + 8, y + 28, w - 16, hint, C_MUTED);
}

static bool point_in(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

/* ---- escaneo de apps ---- */
static bool has_suffix_ci(const char *s, const char *suffix) {
    size_t ls = strlen(s), lf = strlen(suffix);
    if (lf > ls) return false;
    const char *p = s + ls - lf;
    for (size_t i = 0; i < lf; i++) {
        char a = p[i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

static void scan_apps(void) {
    app_count = 0;
    const char *dir = netcfg_apps_dir();
    if (!dir || !dir[0]) dir = "/sd/apps";

    /* crear directorio si no existe */
    fs_make_dir(dir);

    dirhandle *dh = NULL;
    status_t st = fs_open_dir(dir, &dh);
    if (st < 0) return;

    struct dirent ent;
    while (app_count < MAX_APPS && fs_read_dir(dh, &ent) == NO_ERROR) {
        if (ent.name[0] == '.') continue;
        /* aceptar .elf y directorios como entradas navegables/listables */
        bool elf = has_suffix_ci(ent.name, ".elf");
        /* no podemos distinguir dir facilmente sin stat por path completo;
         * tratamos todo lo que no termina en .elf como posible dir solo si
         * no tiene extension, pero para simplicidad listamos .elf y tambien
         * cualquier entrada (la ejecucion fallara si no es elf ejecutable). */
        gui_app_entry_t *a = &apps[app_count];
        strlcpy(a->name, ent.name, sizeof(a->name));
        snprintf(a->path, sizeof(a->path), "%s/%s", dir, ent.name);
        a->is_dir = !elf; /* heuristica: .elf = archivo ejecutable, resto = dir/otro */
        /* solo incluir .elf en la lista principal para ser claro */
        if (elf) {
            app_count++;
        }
    }
    fs_close_dir(dh);

    /* si no hay .elf, hacer un segundo pase para mostrar contenido */
    if (app_count == 0) {
        dh = NULL;
        if (fs_open_dir(dir, &dh) == NO_ERROR) {
            struct dirent ent2;
            while (app_count < MAX_APPS && fs_read_dir(dh, &ent2) == NO_ERROR) {
                if (ent2.name[0] == '.') continue;
                gui_app_entry_t *a = &apps[app_count];
                strlcpy(a->name, ent2.name, sizeof(a->name));
                snprintf(a->path, sizeof(a->path), "%s/%s", dir, ent2.name);
                a->is_dir = !has_suffix_ci(ent2.name, ".elf");
                app_count++;
            }
            fs_close_dir(dh);
        }
    }

    if (sel_index >= app_count) sel_index = app_count > 0 ? app_count - 1 : 0;
    if (scroll_top > sel_index) scroll_top = sel_index;
}

/* ---- ejecucion de app ELF (basico) ---- */
static void show_message(const char *l1, const char *l2, uint32_t color) {
    gfx_fillrect(gui_surf, 20, 180, 280, 100, C_ITEM);
    gfx_fillrect(gui_surf, 20, 180, 280, 3, color);
    draw_centered(gui_surf, 205, l1, C_TEXT);
    draw_centered(gui_surf, 230, l2, C_MUTED);
    draw_centered(gui_surf, 255, "Toca para volver", C_ACCENT);
    arch_clean_cache_range((addr_t)gui_fb, FB_BYTES);
    fbcon_p4_present(gui_fb);
}

static void wait_tap_release(void) {
    /* esperar a que no haya toque */
    for (int i = 0; i < 50 && !gui_stop_req; i++) {
        p4_touch_point_t pts[1];
        if (p4_touch_read(pts, 1) <= 0) return;
        thread_sleep(20);
    }
}

static void wait_tap_any(void) {
    /* espera release y luego press+release */
    wait_tap_release();
    bool was_down = false;
    while (!gui_stop_req) {
        p4_touch_point_t pts[1];
        int n = p4_touch_read(pts, 1);
        if (n > 0) {
            was_down = true;
        } else if (was_down) {
            return;
        }
        thread_sleep(30);
    }
}

/* Intenta cargar y ejecutar un ELF desde SD.
 * Soporte inicial: ELF con segmentos PT_LOAD (estilo LK/lib/elf) cargado en
 * memoria propia. Los ELFs ET_REL estilo Xinu (con relocaciones/resolucion de
 * simbolos del kernel) requieren un cargador+tabla de exports especifica de LK
 * que se puede ampliar; por ahora se informa si el formato no es soportado. */
static int run_app_elf(const char *path) {
    filehandle *fh = NULL;
    status_t st = fs_open_file(path, &fh);
    if (st < 0) return -1;

    struct file_stat fst;
    if (fs_stat_file(fh, &fst) < 0 || fst.size == 0 || fst.size > 4u * 1024u * 1024u) {
        fs_close_file(fh);
        return -2;
    }

    size_t sz = (size_t)fst.size;
    uint8_t *buf = malloc(sz);
    if (!buf) { fs_close_file(fh); return -3; }

    ssize_t rd = fs_read_file(fh, buf, 0, sz);
    fs_close_file(fh);
    if (rd != (ssize_t)sz) { free(buf); return -4; }

    /* detectar tipo ELF por e_type en cabecera (offset 16, 2 bytes LE) */
    if (sz < 20 || buf[0] != 0x7f || buf[1] != 'E' || buf[2] != 'L' || buf[3] != 'F') {
        free(buf); return -5;
    }
    uint16_t etype = (uint16_t)buf[16] | ((uint16_t)buf[17] << 8);

    if (etype == 1 /* ET_REL */) {
        /* Requiere cargador reubicable con exports del kernel (estilo Xinu).
         * Infraestructura pendiente de portado completo a LK; por ahora no
         * ejecutamos para no corromper memoria. */
        free(buf);
        return -10;
    }

    /* ET_EXEC/ET_DYN con PT_LOAD: intentar con lib/elf de LK */
    /* Nota: lib/elf carga a las direcciones virtuales del ELF (p_vaddr).
     * Solo es seguro si el ELF fue enlazado para una region libre (p.ej. PSRAM).
     * El loader integrado de LK (app/loader) usa esta via; aqui hacemos un
     * intento best-effort. */
    /* Para no duplicar mucha logica aqui y evitar enlazar fuerte con lib/elf
     * API en este modulo sin declararla, reportamos que se requiere un ELF
     * enlazado para este entorno. */
    free(buf);
    return -11; /* formato no ejecutable directamente aun */
}

static void launch_selected(void) {
    if (sel_index < 0 || sel_index >= app_count) return;
    gui_app_entry_t *a = &apps[sel_index];
    if (a->is_dir) {
        show_message("Es una carpeta", a->name, C_WARN);
        wait_tap_any();
        return;
    }

    char msg[64];
    snprintf(msg, sizeof(msg), "%s", a->name);
    show_message("Ejecutando...", msg, C_ACCENT);
    thread_sleep(200);

    int rc = run_app_elf(a->path);
    if (rc == 0) {
        /* si la app tomo control y volvio, refrescar */
        scan_apps();
        return;
    }

    const char *reason = "error al cargar";
    if (rc == -1) reason = "no se pudo abrir";
    else if (rc == -2) reason = "tamano invalido";
    else if (rc == -3) reason = "sin memoria";
    else if (rc == -4) reason = "error de lectura";
    else if (rc == -5) reason = "no es ELF";
    else if (rc == -10) reason = "ET_REL: requiere loader+exports";
    else if (rc == -11) reason = "ELF PT_LOAD: enlazado no soportado";

    char l2[64];
    snprintf(l2, sizeof(l2), "%s (%d)", reason, rc);
    show_message("No se pudo ejecutar", l2, C_WARN);
    wait_tap_any();
}

/* ---- render ---- */
static void draw_ui(void) {
    /* fondo */
    gfx_fillrect(gui_surf, 0, 0, SCREEN_W, SCREEN_H, C_BG);

    /* cabecera */
    gfx_fillrect(gui_surf, 0, 0, SCREEN_W, 60, C_HEADER);
    gfx_fillrect(gui_surf, 0, 60, SCREEN_W, 3, C_ACCENT);
    draw_text(gui_surf, 12, 10, "LK Launcher", C_TEXT);
    draw_text(gui_surf, 12, 28, "Interfaz grafica (core 1)", C_MUTED);
    draw_text(gui_surf, 12, 44, netcfg_apps_dir(), C_ACCENT);

    /* info */
    char info[64];
    snprintf(info, sizeof(info), "Apps: %d  [toca para abrir]", app_count);
    draw_text(gui_surf, 12, 72, info, C_MUTED);

    /* botones de control */
    fill_roundish_rect(gui_surf, 230, 68, 80, 28, 0x3000);
    draw_text(gui_surf, 238, 76, "Recargar", C_TEXT);

    /* lista */
    if (app_count == 0) {
        draw_centered(gui_surf, 180, "No hay apps en", C_MUTED);
        draw_centered(gui_surf, 200, netcfg_apps_dir(), C_TEXT);
        draw_centered(gui_surf, 230, "Coloca archivos .elf en la SD", C_MUTED);
        draw_centered(gui_surf, 250, "y toca Recargar", C_MUTED);
    } else {
        int visible = ITEMS_PER_PAGE;
        if (scroll_top < 0) scroll_top = 0;
        if (sel_index < scroll_top) scroll_top = sel_index;
        if (sel_index >= scroll_top + visible) scroll_top = sel_index - visible + 1;
        if (scroll_top < 0) scroll_top = 0;

        for (int i = 0; i < visible; i++) {
            int idx = scroll_top + i;
            if (idx >= app_count) break;
            int y = ITEM_Y0 + i * (ITEM_H + 6);
            draw_button(gui_surf, 10, y, SCREEN_W - 20, ITEM_H,
                        apps[idx].name, idx == sel_index, apps[idx].is_dir);
        }

        /* indicadores de scroll */
        if (scroll_top > 0)
            draw_centered(gui_surf, ITEM_Y0 - 14, "^ mas ^", C_MUTED);
        if (scroll_top + visible < app_count)
            draw_centered(gui_surf, ITEM_Y0 + visible * (ITEM_H + 6) + 2, "v mas v", C_MUTED);
    }

    /* pie */
    gfx_fillrect(gui_surf, 0, SCREEN_H - 28, SCREEN_W, 28, C_HEADER);
    draw_text(gui_surf, 8, SCREEN_H - 20, "Touch activo  |  'gui stop' para salir", C_MUTED);

    arch_clean_cache_range((addr_t)gui_fb, FB_BYTES);
    fbcon_p4_present(gui_fb);
}

/* ---- bucle principal ---- */
static int gui_main_loop(void) {
    p4_touch_init();
    /* Escanear apps de la SD al entrar en la GUI. */
    scan_apps();
    draw_ui();

    bool prev_down = false;
    p4_touch_point_t last_pt = {0, 0, 0};
    lk_time_t last_action = 0;

    while (!gui_stop_req) {
        p4_touch_point_t pts[2];
        int n = p4_touch_read(pts, 2);
        bool down = (n > 0);

        if (down) {
            last_pt = pts[0];
            prev_down = true;
        } else if (prev_down) {
            /* tap/release */
            prev_down = false;
            lk_time_t now = current_time();
            if (now - last_action < 250) { thread_sleep(20); continue; }
            last_action = now;

            int tx = last_pt.x, ty = last_pt.y;

            /* boton recargar */
            if (point_in(tx, ty, 230, 68, 80, 28)) {
                scan_apps();
                draw_ui();
                continue;
            }

            /* items */
            bool handled = false;
            int visible = ITEMS_PER_PAGE;
            for (int i = 0; i < visible; i++) {
                int idx = scroll_top + i;
                if (idx >= app_count) break;
                int y = ITEM_Y0 + i * (ITEM_H + 6);
                if (point_in(tx, ty, 10, y, SCREEN_W - 20, ITEM_H)) {
                    if (sel_index == idx) {
                        launch_selected();
                        draw_ui();
                    } else {
                        sel_index = idx;
                        draw_ui();
                    }
                    handled = true;
                    break;
                }
            }
            if (!handled) {
                /* toque arriba/abajo para scroll */
                if (ty < ITEM_Y0 && scroll_top > 0) {
                    scroll_top--;
                    sel_index = scroll_top;
                    draw_ui();
                } else if (ty > ITEM_Y0 + visible * (ITEM_H + 6) && scroll_top + visible < app_count) {
                    scroll_top++;
                    sel_index = scroll_top + visible - 1;
                    if (sel_index >= app_count) sel_index = app_count - 1;
                    draw_ui();
                }
            }
        }

        thread_sleep(30);
    }
    return 0;
}

static int gui_thread_entry(void *arg) {
    (void)arg;

    if (!fbcon_p4_is_ready()) {
        printf("gui: fbcon/lcd no esta listo\n");
        gui_running = false;
        return -1;
    }

    psram_heap_init();
    gui_fb = psram_malloc(FB_BYTES);
    if (!gui_fb) {
        printf("gui: sin memoria para framebuffer (%u bytes)\n", (unsigned)FB_BYTES);
        gui_running = false;
        return -1;
    }
    memset(gui_fb, 0, FB_BYTES);

    gui_surf = gfx_create_surface(gui_fb, SCREEN_W, SCREEN_H, SCREEN_W, GFX_FORMAT_RGB_565);
    if (!gui_surf) {
        printf("gui: no se pudo crear superficie gfx\n");
        free(gui_fb);
        gui_fb = NULL;
        gui_running = false;
        return -1;
    }

    /* tomar la pantalla */
    fbcon_p4_suspend();
    /* pantalla de inicio */
    gfx_fillrect(gui_surf, 0, 0, SCREEN_W, SCREEN_H, C_BG);
    draw_centered(gui_surf, 200, "Iniciando interfaz grafica...", C_TEXT);
    draw_centered(gui_surf, 230, "Touch: FT6336U", C_MUTED);
    arch_clean_cache_range((addr_t)gui_fb, FB_BYTES);
    fbcon_p4_present(gui_fb);

    gui_main_loop();

    /* restaurar consola */
    gfx_surface_destroy(gui_surf);
    gui_surf = NULL;
    free(gui_fb);
    gui_fb = NULL;

    fbcon_p4_resume();
    printf("gui: interfaz grafica detenida\n");

    gui_running = false;
    return 0;
}

struct gui_elf_ctx {
    char path[128];
};

static int gui_elf_launcher_thread(void *arg) {
    struct gui_elf_ctx *ctx = (struct gui_elf_ctx *)arg;
    char path[128];
    strlcpy(path, ctx->path, sizeof(path));
    free(ctx);
    char *argv0 = path;
    printf("gui: lanzando launcher ELF %s\n", path);
    int rc = elf_run(path, 1, &argv0);
    printf("gui: launcher ELF finalizo (rc=%d)\n", rc);
    return rc;
}

static int gui_start_external_launcher(const char *path) {
    if (!path || !path[0]) return -1;
    struct gui_elf_ctx *ctx = malloc(sizeof(*ctx));
    if (!ctx) return -1;
    strlcpy(ctx->path, path, sizeof(ctx->path));
    thread_t *t = thread_create("gui_elf", gui_elf_launcher_thread, ctx, DEFAULT_PRIORITY, DEFAULT_STACK_SIZE * 4);
    if (!t) { free(ctx); return -1; }
    /* CRITICO: fijar a core 1.  En core 0 el launcher (prio 16) preempta al
     * hilo wifi_poll (prio 8), el C6 deja de bombearse y mueren ARP/httpd/
     * telnet/UDP ~16s despues del boot (medido en HW).  Core 0 queda para
     * netstack + wifi_poll + servidores. */
    thread_set_pinned_cpu(t, 1);
    thread_detach(t);
    thread_resume(t);
    return 0;
}

static int gui_start(bool autostart_mode) {
    if (autostart_mode && !netcfg_gui_autostart()) {
        printf("gui: autostart deshabilitado en kernel.json\n");
        return 0;
    }

    /* Si hay un launcher ELF configurado, usarlo (especialmente en autostart) */
    const char *lp = netcfg_launcher_path();
    if (autostart_mode && lp && lp[0]) {
        if (gui_start_external_launcher(lp) == 0) {
            printf("gui: autostart launcher ELF '%s'\n", lp);
            return 0;
        }
        printf("gui: no se pudo lanzar '%s', usando GUI embebida\n", lp);
    }

    mutex_acquire(&gui_lock);
    if (gui_running) {
        mutex_release(&gui_lock);
        printf("gui: ya esta en ejecucion\n");
        return 0;
    }
    gui_stop_req = false;
    gui_running = true;
    gui_thread = thread_create("gui", gui_thread_entry, NULL, DEFAULT_PRIORITY, 8192);
    if (!gui_thread) {
        gui_running = false;
        mutex_release(&gui_lock);
        printf("gui: no se pudo crear hilo\n");
        return -1;
    }
    thread_set_pinned_cpu(gui_thread, 1);
    thread_resume(gui_thread);
    mutex_release(&gui_lock);

    if (!autostart_mode) {
        printf("gui: iniciada (core 1). Usa 'gui stop' para salir.\n");
    } else {
        printf("gui: iniciada por autostart (core 1)\n");
    }
    return 0;
}

static int cmd_gui(int argc, const console_cmd_args *argv) {
    if (argc < 2) {
        return gui_start(false);
    }
    if (!strcmp(argv[1].str, "autostart")) {
        return gui_start(true);
    } else if (!strcmp(argv[1].str, "start")) {
        return gui_start(false);
    } else if (!strcmp(argv[1].str, "stop")) {
        gui_stop_req = true;
        printf("gui: solicitando salida...\n");
        return 0;
    } else if (!strcmp(argv[1].str, "status")) {
        printf("gui: %s (apps_dir=%s)\n", gui_running ? "ejecutando" : "detenida",
               netcfg_apps_dir());
        return 0;
    }
    printf("uso: gui [autostart|start|stop|status]\n");
    return -1;
}

STATIC_COMMAND_START
STATIC_COMMAND("gui", "interfaz grafica / launcher (touch, core 1)", &cmd_gui)
STATIC_COMMAND("launcher", "alias de gui", &cmd_gui)
STATIC_COMMAND_END(gui);
