/*
 * Declaraciones minimas para apps ELF de LK.
 * Las implementaciones reales las provee el kernel via tabla de exports.
 */
#ifndef LK_APP_H
#define LK_APP_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

/* stdio/libc */
int printf(const char *fmt, ...);
int sprintf(char *str, const char *fmt, ...);
int snprintf(char *str, size_t size, const char *fmt, ...);
int puts(const char *s);
int putchar(int c);

void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t n, size_t size);
void *realloc(void *ptr, size_t size);
void *psram_malloc(size_t size);
void psram_free(void *ptr);
void *psram_calloc(size_t n, size_t size);
void *psram_realloc(void *ptr, size_t size);

void *memset(void *s, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t maxlen);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
char *strcat(char *dest, const char *src);
char *strncat(char *dest, const char *src, size_t n);
char *strchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
size_t strlcpy(char *dst, const char *src, size_t size);
size_t strlcat(char *dst, const char *src, size_t size);

int atoi(const char *nptr);
long atol(const char *nptr);
long strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);

/* tiempo / hilos */
typedef unsigned long lk_time_t;
typedef int (*thread_start_routine)(void *arg);
typedef struct thread thread_t;
void thread_sleep(lk_time_t delay);
void thread_yield(void);
thread_t *thread_create(const char *name, thread_start_routine entry, void *arg, int priority, size_t stack_size);
int thread_resume(thread_t *t);
void thread_exit(int retcode);
int thread_join(thread_t *t, int *retcode, lk_time_t timeout);
int thread_detach(thread_t *t);
void thread_set_pinned_cpu(thread_t *t, int cpu);
void thread_set_priority(int priority);
lk_time_t current_time(void);
thread_t *get_current_thread(void);

#define DEFAULT_PRIORITY 16
#define DEFAULT_STACK_SIZE 8192
#define INFINITE_TIME 0xFFFFFFFFu

/* FS (LK lib/fs) */
typedef struct filehandle filehandle;
typedef struct dirhandle dirhandle;
struct file_stat { bool is_dir; uint64_t size; uint64_t capacity; };
struct dirent { char name[64]; };

int fs_open_file(const char *path, filehandle **handle);
int fs_create_file(const char *path, filehandle **handle, uint64_t len);
ssize_t fs_read_file(filehandle *handle, void *buf, long long offset, size_t len);
ssize_t fs_write_file(filehandle *handle, const void *buf, long long offset, size_t len);
int fs_close_file(filehandle *handle);
int fs_stat_file(filehandle *handle, struct file_stat *stat);
int fs_truncate_file(filehandle *handle, uint64_t len);
int fs_remove_file(const char *path);
int fs_make_dir(const char *path);
int fs_remove_dir(const char *path);
int fs_open_dir(const char *path, dirhandle **handle);
int fs_read_dir(dirhandle *handle, struct dirent *ent);
int fs_close_dir(dirhandle *handle);

/* touch */
typedef struct { uint16_t x; uint16_t y; uint8_t id; } p4_touch_point_t;
bool p4_touch_init(void);
int p4_touch_read(p4_touch_point_t *pts, int cap);

/* display/fbcon control */
bool fbcon_p4_is_ready(void);
void fbcon_p4_suspend(void);
void fbcon_p4_resume(void);
void fbcon_p4_present(const uint16_t *pixels);
void arch_clean_cache_range(unsigned long start, unsigned long len);

/* gfx/font */
typedef enum { GFX_FORMAT_RGB_565 = 1 } gfx_format;
typedef struct gfx_surface gfx_surface;
gfx_surface *gfx_create_surface(void *ptr, unsigned width, unsigned height, unsigned stride, gfx_format format);
void gfx_surface_destroy(gfx_surface *surface);
void gfx_fillrect(gfx_surface *surface, unsigned x, unsigned y, unsigned w, unsigned h, unsigned color);
void gfx_putpixel(gfx_surface *surface, unsigned x, unsigned y, unsigned color);
void gfx_line(gfx_surface *surface, unsigned x1, unsigned y1, unsigned x2, unsigned y2, unsigned color);
void gfx_flush(gfx_surface *surface);
void font_draw_char(gfx_surface *surface, unsigned char c, int x, int y, uint32_t color);
#define FONT_X 6
#define FONT_Y 12

/* elf */
int elf_run(const char *ruta, int argc, char *argv[]);

/* exit es provisto por el loader (no declarar aqui para evitar conflictos) */
void exit(int code);

#endif

/* ---- app manager (cooperativo, estilo Android/launcher) ---- */
#define APPMGR_MAX 16
#define APP_FREE       0
#define APP_RUNNING    1
#define APP_BACKGROUND 2
#define APP_CLOSING    3
#define APP_EXITED     4
#define APPMGR_NAME_LEN 24
#define APPMGR_PATH_LEN 128

typedef struct {
    char     name[APPMGR_NAME_LEN];
    char     path[APPMGR_PATH_LEN];
    uint32_t color;
    int      state;
    thread_t *thread;
    int      info;
    uint16_t *thumb;
    int       thumb_w;
    int       thumb_h;
} appmgr_entry_t;

void appmgr_init(void);
appmgr_entry_t *appmgr_table(void);
int appmgr_count(void);
int appmgr_register(const char *name, const char *path, uint32_t color, thread_t *t);
int appmgr_unregister(int slot);
int appmgr_find_path(const char *path);
int appmgr_find_thread(thread_t *t);
int  appmgr_get_state(int slot);
void appmgr_set_state(int slot, int state);
void appmgr_request_close(int slot);
int  appmgr_take_foreground(int slot);
void appmgr_set_info(int slot, int info);
int  appmgr_get_info(int slot);
int  appmgr_set_thumbnail(int slot, const uint16_t *src, int src_w, int src_h, int tw, int th);
void appmgr_clear_thumbnail(int slot);
int  appmgr_suspend_background(int slot, const uint16_t *fb, int fb_w, int fb_h, int tw, int th);
int  appmgr_wait_resume(int slot);
int  appmgr_resume_app(int slot);
void appmgr_mark_exited(int slot);

/* ---- WiFi (para ajustes) ---- */
#define WIFI_SCAN_SSID_MAX 33
#define WIFI_SCAN_MAX_RESULTS 32

typedef struct {
    char ssid[WIFI_SCAN_SSID_MAX];
    uint8_t bssid[6];
    int32_t rssi;
    uint32_t channel;
    uint32_t authmode;
} wifi_scan_result_t;

typedef enum {
    LK_WIFI_IF_STA = 0,
    LK_WIFI_IF_AP = 1,
} lk_wifi_interface_t;

int wifi_scan(wifi_scan_result_t *out, int max, int *out_count);
int wifi_attach_and_up(void); /* status_t */
int wifi_join(const char *ssid, const char *pass); /* status_t */
bool wifi_is_attached(void);
bool esp_hosted_wifi_sta_is_connected(void);
int esp_hosted_wifi_disconnect(uint32_t timeout_ms);
bool esp_hosted_sdio_is_attached(void);
bool esp_hosted_is_initialized(void);
bool esp_hosted_ctrl_is_ready(void);
int esp_hosted_wifi_get_mac(lk_wifi_interface_t iface, uint8_t mac[6], uint32_t timeout_ms);
