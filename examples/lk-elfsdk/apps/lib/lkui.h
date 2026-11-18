#pragma once
#include "lk_app.h"

#define UI_EVT_NONE       0
#define UI_EVT_TAP        1
#define UI_EVT_SWIPE      2
#define UI_EVT_LONG_PRESS 3

#define UI_DIR_UP    0
#define UI_DIR_DOWN  1
#define UI_DIR_LEFT  2
#define UI_DIR_RIGHT 3
#define UI_DIR_NONE  4

#define UI_EDGE_NONE   0
#define UI_EDGE_TOP    1
#define UI_EDGE_BOTTOM 2
#define UI_EDGE_LEFT   3
#define UI_EDGE_RIGHT  4

typedef struct {
    int type;
    int dir;
    int edge;
    int x0, y0;
    int x1, y1;
} ui_evt_t;

typedef struct {
    int screen_w, screen_h;
    int down;
    int start_x, start_y;
    int last_x, last_y;
    lk_time_t start_ms;
    lk_time_t last_ms;
    int edge;
} ui_gesture_t;

void ui_gesture_init(ui_gesture_t *g, int w, int h);
int  ui_gesture_poll(ui_gesture_t *g, ui_evt_t *out);

/* dibujo */
void ui_draw_text(gfx_surface *s, int x, int y, const char *str, uint32_t color);
void ui_draw_text_clip(gfx_surface *s, int x, int y, int maxw, const char *str, uint32_t color);
int  ui_text_width(const char *str);
void ui_draw_rect(gfx_surface *s, int x, int y, int w, int h, uint32_t color);
void ui_draw_rect_outline(gfx_surface *s, int x, int y, int w, int h, uint32_t color);
void ui_draw_button(gfx_surface *s, int x, int y, int w, int h, const char *label,
                    uint32_t bg, uint32_t fg, bool pressed);
void ui_draw_thumb(gfx_surface *dst, int dx, int dy, int dw, int dh,
                   const uint16_t *src, int sw, int sh);

/* teclado on-screen simple (qwerty + numeros/simbolos basicos) */
#define UI_KB_BUF_MAX 128
typedef struct {
    char buf[UI_KB_BUF_MAX];
    int len;
    int shift;
    int sym; /* 0=abc,1=123/sym */
    bool done;   /* enter/ok */
    bool cancel;
    const char *title;
    bool password_mode;
} ui_keyboard_t;

void ui_keyboard_init(ui_keyboard_t *kb, const char *title, const char *initial, bool password_mode);
/* dibuja teclado en parte inferior y campo de texto arriba; devuelve true si redibujar */
void ui_keyboard_draw(gfx_surface *s, ui_keyboard_t *kb, int screen_w, int screen_h);
/* procesa tap (x,y) en coordenadas de pantalla; devuelve true si estado cambió */
bool ui_keyboard_tap(ui_keyboard_t *kb, int x, int y, int screen_w, int screen_h);

/* scrollable list helper */
typedef struct {
    int count;
    int selected;
    int scroll;
    int visible;
    int item_h;
    int x, y, w, h;
} ui_list_t;

void ui_list_init(ui_list_t *l, int x, int y, int w, int h, int item_h);
void ui_list_set_count(ui_list_t *l, int count);
int  ui_list_hit(ui_list_t *l, int tx, int ty); /* devuelve indice o -1 */
void ui_list_ensure_visible(ui_list_t *l);

/* helpers de app: ciclo cooperativo básico */
int  ui_app_get_slot(int argc, char **argv, const char *default_name, uint32_t color);
bool ui_app_should_close(int slot);
bool ui_app_handle_sys_gesture(int slot, ui_evt_t *ev, uint16_t *fb, int w, int h);
/* si gesto es nav de sistema (borde inf ↑) en raíz: hace suspend+wait y retorna true cuando reanuda
 * (o false si debe cerrar). Caller debe redibujar tras true. */
typedef enum { UI_NAV_NONE=0, UI_NAV_BACK=1, UI_NAV_BACKGROUND=2, UI_NAV_CLOSE=3 } ui_nav_t;
