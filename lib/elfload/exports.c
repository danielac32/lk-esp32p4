/*
 * Tabla de simbolos exportados por el kernel LK a apps ELF (ET_REL).
 * Adaptada al entorno de LK: exporta APIs de LK (libc, fs, threads, gfx, etc.)
 * en lugar de las APIs de Xinu. Solo se exportan funciones/enlaces reales
 * presentes en la imagen; si una app necesita un simbolo no listado, el
 * cargador lo reporta y aborta antes de saltar.
 */
#include <lib/elf_exports.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <lib/fs.h>
#include <lib/gfx.h>
#include <lib/font.h>
#include <kernel/thread.h>
#include <kernel/mutex.h>
#include <platform.h>
#include <arch/ops.h>
#include <platform/esp32p4_lcd.h>
#include <platform/esp32p4_touch.h>
#include <platform/psram_heap.h>
#include <dev/fbcon_p4.h>
#include <lib/elfload.h>
#include <lib/appmgr.h>
#include <kernel/thread.h>
#include <dev/wifi/wifi.h>
#include <dev/wifi/esp_hosted.h>
#include <dev/wifi/esp_hosted_sdio.h>
#include <dev/wifi/esp_hosted_ctrl.h>

/* forward de funciones no declaradas en headers incluidos arriba */
extern int puts(const char *s);
extern int putchar(int c);
extern void *memmove(void *dest, const void *src, size_t n);
extern void *memset(void *s, int c, size_t n);
extern void *memcpy(void *dest, const void *src, size_t n);

static thread_t *lk_get_current_thread(void) { return get_current_thread(); }

static const struct lk_elf_export exports[] = {
    /* stdio / formato */
    { "printf",   (void *)printf },
    { "sprintf",  (void *)sprintf },
    { "snprintf", (void *)snprintf },
    { "puts",     (void *)puts },
    { "putchar",  (void *)putchar },

    /* memoria */
    { "malloc",  (void *)malloc },
    { "free",    (void *)free },
    { "calloc",  (void *)calloc },
    { "realloc", (void *)realloc },
    { "psram_malloc",  (void *)psram_malloc },
    { "psram_free",    (void *)psram_free },
    { "psram_calloc",  (void *)psram_calloc },
    { "psram_realloc", (void *)psram_realloc },

    /* strings/memoria */
    { "memset",  (void *)memset },
    { "memcpy",  (void *)memcpy },
    { "memmove", (void *)memmove },
    { "memcmp",  (void *)memcmp },
    { "strlen",  (void *)strlen },
    { "strnlen", (void *)strnlen },
    { "strcmp",  (void *)strcmp },
    { "strncmp", (void *)strncmp },
    { "strcpy",  (void *)strcpy },
    { "strncpy", (void *)strncpy },
    { "strcat",  (void *)strcat },
    { "strncat", (void *)strncat },
    { "strchr",  (void *)strchr },
    { "strstr",  (void *)strstr },
    { "strlcpy", (void *)strlcpy },
    { "strlcat", (void *)strlcat },

    /* conversion / util */
    { "atoi",   (void *)atoi },
    { "atol",   (void *)atol },
    { "strtol", (void *)strtol },
    { "strtoul",(void *)strtoul },

    /* hilos / tiempo */
    { "thread_create",     (void *)thread_create },
    { "thread_resume",     (void *)thread_resume },
    { "thread_exit",       (void *)thread_exit },
    { "thread_sleep",      (void *)thread_sleep },
    { "thread_yield",      (void *)thread_yield },
    { "thread_join",       (void *)thread_join },
    { "thread_detach",     (void *)thread_detach },
    { "thread_set_pinned_cpu", (void *)thread_set_pinned_cpu },
    { "thread_set_priority",   (void *)thread_set_priority },
    { "current_time",      (void *)current_time },
    { "get_current_thread", (void *)lk_get_current_thread },

    /* sistema de ficheros (LK) */
    { "fs_open_file",   (void *)fs_open_file },
    { "fs_create_file", (void *)fs_create_file },
    { "fs_read_file",   (void *)fs_read_file },
    { "fs_write_file",  (void *)fs_write_file },
    { "fs_close_file",  (void *)fs_close_file },
    { "fs_stat_file",   (void *)fs_stat_file },
    { "fs_truncate_file",(void *)fs_truncate_file },
    { "fs_remove_file", (void *)fs_remove_file },
    { "fs_make_dir",    (void *)fs_make_dir },
    { "fs_remove_dir",  (void *)fs_remove_dir },
    { "fs_open_dir",    (void *)fs_open_dir },
    { "fs_read_dir",    (void *)fs_read_dir },
    { "fs_close_dir",   (void *)fs_close_dir },

    /* graficos / display / touch (para launcher.elf nativo LK) */
    { "gfx_create_surface",  (void *)gfx_create_surface },
    { "gfx_surface_destroy", (void *)gfx_surface_destroy },
    { "gfx_fillrect",        (void *)gfx_fillrect },
    { "gfx_putpixel",        (void *)gfx_putpixel },
    { "gfx_line",            (void *)gfx_line },
    { "gfx_copyrect",        (void *)gfx_copyrect },
    { "gfx_flush",           (void *)gfx_flush },
    { "font_draw_char",      (void *)font_draw_char },
    { "fbcon_p4_suspend",    (void *)fbcon_p4_suspend },
    { "fbcon_p4_resume",     (void *)fbcon_p4_resume },
    { "fbcon_p4_present",    (void *)fbcon_p4_present },
    { "fbcon_p4_is_ready",   (void *)fbcon_p4_is_ready },
    { "p4_touch_init",       (void *)p4_touch_init },
    { "p4_touch_read",       (void *)p4_touch_read },
    { "arch_clean_cache_range", (void *)arch_clean_cache_range },

    /* cargador / gestor de apps */
    { "elf_run", (void *)elf_run },
    { "appmgr_init", (void *)appmgr_init },
    { "appmgr_register", (void *)appmgr_register },
    { "appmgr_unregister", (void *)appmgr_unregister },
    { "appmgr_find_path", (void *)appmgr_find_path },
    { "appmgr_find_thread", (void *)appmgr_find_thread },
    { "appmgr_get_state", (void *)appmgr_get_state },
    { "appmgr_set_state", (void *)appmgr_set_state },
    { "appmgr_request_close", (void *)appmgr_request_close },
    { "appmgr_take_foreground", (void *)appmgr_take_foreground },
    { "appmgr_set_info", (void *)appmgr_set_info },
    { "appmgr_get_info", (void *)appmgr_get_info },
    { "appmgr_set_thumbnail", (void *)appmgr_set_thumbnail },
    { "appmgr_clear_thumbnail", (void *)appmgr_clear_thumbnail },
    { "appmgr_table", (void *)appmgr_table },
    { "appmgr_count", (void *)appmgr_count },
    { "appmgr_suspend_background", (void *)appmgr_suspend_background },
    { "appmgr_wait_resume", (void *)appmgr_wait_resume },
    { "appmgr_resume_app", (void *)appmgr_resume_app },
    { "appmgr_mark_exited", (void *)appmgr_mark_exited },

    /* wifi / red (para apps de ajustes) */
    { "wifi_scan", (void *)wifi_scan },
    { "wifi_attach_and_up", (void *)wifi_attach_and_up },
    { "wifi_join", (void *)wifi_join },
    { "wifi_is_attached", (void *)wifi_is_attached },
    { "esp_hosted_wifi_sta_is_connected", (void *)esp_hosted_wifi_sta_is_connected },
    { "esp_hosted_wifi_disconnect", (void *)esp_hosted_wifi_disconnect },
    { "esp_hosted_sdio_is_attached", (void *)esp_hosted_sdio_is_attached },
    { "esp_hosted_is_initialized", (void *)esp_hosted_is_initialized },
    { "esp_hosted_ctrl_is_ready", (void *)esp_hosted_ctrl_is_ready },
    { "esp_hosted_wifi_get_mac", (void *)esp_hosted_wifi_get_mac },
};

void *elf_export_resolve(const char *name) {
    if (!name || name[0] == '\0') return NULL;
    unsigned int n = sizeof(exports) / sizeof(exports[0]);
    for (unsigned int i = 0; i < n; i++) {
        if (strcmp(exports[i].name, name) == 0) return (void *)exports[i].addr;
    }
    return NULL;
}

unsigned int elf_export_count(void) {
    return (unsigned int)(sizeof(exports) / sizeof(exports[0]));
}

const char *elf_export_name(unsigned int i) {
    unsigned int n = sizeof(exports) / sizeof(exports[0]);
    if (i >= n) return NULL;
    return exports[i].name;
}
