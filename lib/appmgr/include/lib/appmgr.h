/*
 * App manager para LK - registro compartido launcher <-> apps ELF.
 * Modelo cooperativo: 1 hilo por app. Solo foreground pinta/atiende input.
 */
#pragma once

#include <kernel/thread.h>
#include <stdint.h>
#include <stdbool.h>

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

/* lifecycle cooperativo */
int  appmgr_suspend_background(int slot, const uint16_t *fb, int fb_w, int fb_h, int tw, int th);
int  appmgr_wait_resume(int slot);
int  appmgr_resume_app(int slot);

/* helpers para launcher: marcar salida del hilo app */
void appmgr_mark_exited(int slot);
