#include "lkui.h"

#define SWIPE_MIN_PX    45
#define EDGE_ZONE       40
#define TAP_MAX_MOVE    18
#define LONG_PRESS_MS   600

static int abs_i(int v) { return v < 0 ? -v : v; }

void ui_gesture_init(ui_gesture_t *g, int w, int h) {
    memset(g, 0, sizeof(*g));
    g->screen_w = w;
    g->screen_h = h;
}

static int detect_edge(ui_gesture_t *g, int x, int y) {
    (void)x;
    if (y >= g->screen_h - EDGE_ZONE) return UI_EDGE_BOTTOM;
    if (y <= EDGE_ZONE) return UI_EDGE_TOP;
    if (x <= EDGE_ZONE) return UI_EDGE_LEFT;
    if (x >= g->screen_w - EDGE_ZONE) return UI_EDGE_RIGHT;
    return UI_EDGE_NONE;
}

static int dominant_dir(int dx, int dy) {
    int ax = abs_i(dx), ay = abs_i(dy);
    if (ax < SWIPE_MIN_PX && ay < SWIPE_MIN_PX) return UI_DIR_NONE;
    if (ay >= ax) return (dy < 0) ? UI_DIR_UP : UI_DIR_DOWN;
    return (dx < 0) ? UI_DIR_LEFT : UI_DIR_RIGHT;
}

int ui_gesture_poll(ui_gesture_t *g, ui_evt_t *out) {
    if (!g || !out) return 0;
    memset(out, 0, sizeof(*out));
    out->type = UI_EVT_NONE;

    p4_touch_point_t pts[1];
    int n = p4_touch_read(pts, 1);
    lk_time_t now = current_time();

    if (n > 0) {
        int x = pts[0].x;
        int y = pts[0].y;
        if (!g->down) {
            g->down = 1;
            g->start_x = g->last_x = x;
            g->start_y = g->last_y = y;
            g->start_ms = g->last_ms = now;
            g->edge = detect_edge(g, x, y);
        } else {
            g->last_x = x;
            g->last_y = y;
            g->last_ms = now;
        }
        return 0;
    }

    if (!g->down) return 0;
    g->down = 0;
    int dx = g->last_x - g->start_x;
    int dy = g->last_y - g->start_y;
    int adx = abs_i(dx), ady = abs_i(dy);
    lk_time_t dur = now - g->start_ms;

    out->x0 = g->start_x; out->y0 = g->start_y;
    out->x1 = g->last_x;  out->y1 = g->last_y;
    out->edge = g->edge;
    out->dir = dominant_dir(dx, dy);

    if (out->dir != UI_DIR_NONE && (adx >= SWIPE_MIN_PX || ady >= SWIPE_MIN_PX)) {
        out->type = UI_EVT_SWIPE;
        return 1;
    }
    if (adx <= TAP_MAX_MOVE && ady <= TAP_MAX_MOVE) {
        if (dur >= LONG_PRESS_MS) out->type = UI_EVT_LONG_PRESS;
        else out->type = UI_EVT_TAP;
        return 1;
    }
    out->type = UI_EVT_TAP;
    out->dir = UI_DIR_NONE;
    return 1;
}

void ui_draw_text(gfx_surface *s, int x, int y, const char *str, uint32_t color) {
    if (!str) return;
    while (*str) {
        if (*str == '\n') break;
        font_draw_char(s, (unsigned char)*str, x, y, color);
        x += FONT_X;
        str++;
    }
}

void ui_draw_text_clip(gfx_surface *s, int x, int y, int maxw, const char *str, uint32_t color) {
    if (!str) return;
    int endx = x + maxw;
    while (*str && x + FONT_X <= endx) {
        font_draw_char(s, (unsigned char)*str, x, y, color);
        x += FONT_X;
        str++;
    }
}

int ui_text_width(const char *str) {
    if (!str) return 0;
    int n = 0; while (str[n] && str[n] != '\n') n++;
    return n * FONT_X;
}

void ui_draw_rect(gfx_surface *s, int x, int y, int w, int h, uint32_t color) {
    gfx_fillrect(s, x, y, w, h, color);
}

void ui_draw_rect_outline(gfx_surface *s, int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    gfx_fillrect(s, x, y, w, 1, color);
    gfx_fillrect(s, x, y + h - 1, w, 1, color);
    gfx_fillrect(s, x, y, 1, h, color);
    gfx_fillrect(s, x + w - 1, y, 1, h, color);
}

void ui_draw_button(gfx_surface *s, int x, int y, int w, int h, const char *label,
                    uint32_t bg, uint32_t fg, bool pressed) {
    uint32_t c = pressed ? (bg ^ 0x39E7) : bg;
    gfx_fillrect(s, x, y, w, h, c);
    ui_draw_rect_outline(s, x, y, w, h, 0xFFFF);
    if (label) {
        int tw = ui_text_width(label);
        int tx = x + (w - tw) / 2;
        int ty = y + (h - FONT_Y) / 2;
        ui_draw_text(s, tx, ty, label, fg);
    }
}

void ui_draw_thumb(gfx_surface *dst, int dx, int dy, int dw, int dh,
                   const uint16_t *src, int sw, int sh) {
    if (!dst || !src || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    for (int y = 0; y < dh; y++) {
        int sy = y * sh / dh;
        if (sy >= sh) sy = sh - 1;
        for (int x = 0; x < dw; x++) {
            int sx = x * sw / dw;
            if (sx >= sw) sx = sw - 1;
            uint16_t c = src[sy * sw + sx];
            gfx_putpixel(dst, dx + x, dy + y, c);
        }
    }
}

/* ---- Teclado ---- */
static const char *kb_rows_abc[] = {
    "qwertyuiop",
    "asdfghjkl",
    "zxcvbnm",
    NULL
};
static const char *kb_rows_sym[] = {
    "1234567890",
    "-_/.:;()@",
    "!?&+#*=\"'",
    NULL
};

void ui_keyboard_init(ui_keyboard_t *kb, const char *title, const char *initial, bool password_mode) {
    memset(kb, 0, sizeof(*kb));
    kb->title = title ? title : "Entrada";
    kb->password_mode = password_mode;
    if (initial) {
        strlcpy(kb->buf, initial, sizeof(kb->buf));
        kb->len = (int)strlen(kb->buf);
    }
}

static void kb_draw_field(gfx_surface *s, ui_keyboard_t *kb, int w) {
    gfx_fillrect(s, 0, 0, w, 80, 0x1082);
    ui_draw_text(s, 8, 6, kb->title, 0xFFFF);
    gfx_fillrect(s, 8, 26, w - 16, 28, 0xFFFF);
    ui_draw_rect_outline(s, 8, 26, w - 16, 28, 0x0000);
    char disp[UI_KB_BUF_MAX];
    if (kb->password_mode) {
        int n = kb->len; if (n > 40) n = 40;
        for (int i = 0; i < n; i++) disp[i] = '*';
        disp[n] = 0;
    } else {
        strlcpy(disp, kb->buf, sizeof(disp));
    }
    /* mostrar cola si largo */
    int maxc = (w - 24) / FONT_X;
    int dlen = (int)strlen(disp);
    const char *show = disp;
    if (dlen > maxc) show = disp + (dlen - maxc);
    ui_draw_text_clip(s, 12, 34, w - 24, show, 0x0000);
    ui_draw_text(s, 8, 60, "OK=Enter  Cancel=X  <=Borrar", 0x9CD3);
}

void ui_keyboard_draw(gfx_surface *s, ui_keyboard_t *kb, int screen_w, int screen_h) {
    const char **rows = kb->sym ? kb_rows_sym : kb_rows_abc;
    int key_h = 44;
    int y0 = screen_h - key_h * 5 - 4;
    gfx_fillrect(s, 0, y0, screen_w, screen_h - y0, 0x2104);
    kb_draw_field(s, kb, screen_w);

    int y = y0 + 4;
    for (int r = 0; rows[r]; r++) {
        const char *row = rows[r];
        int n = (int)strlen(row);
        int kw = (screen_w - 8) / (n > 0 ? n : 1);
        for (int i = 0; i < n; i++) {
            int kx = 4 + i * kw;
            char lab[2] = { row[i], 0 };
            if (!kb->sym && kb->shift && lab[0] >= 'a' && lab[0] <= 'z') lab[0] -= 32;
            gfx_fillrect(s, kx+1, y+1, kw-2, key_h-2, 0x39E7);
            ui_draw_rect_outline(s, kx+1, y+1, kw-2, key_h-2, 0x0000);
            int tx = kx + (kw - FONT_X)/2;
            int ty = y + (key_h - FONT_Y)/2;
            ui_draw_text(s, tx, ty, lab, 0xFFFF);
        }
        y += key_h;
    }
    /* fila especial: shift/sym, space, back, cancel, ok */
    int kw4 = (screen_w - 8) / 5;
    int yb = y;
    struct { const char *lab; int idx; } specs[5] = {
        { kb->sym?"ABC":"123", 0 }, { "SPACE", 1 }, { "<-", 2 }, { "X", 3 }, { "OK", 4 }
    };
    for (int i = 0; i < 5; i++) {
        int kx = 4 + i * kw4;
        uint32_t bg = (i==4)?0x07E0:((i==3)?0xF800:0x2945);
        gfx_fillrect(s, kx+1, yb+1, kw4-2, key_h-2, bg);
        ui_draw_rect_outline(s, kx+1, yb+1, kw4-2, key_h-2, 0x0000);
        const char *lab = specs[i].lab;
        if (i==0 && !kb->sym) lab = kb->shift?"abc":"ABC";
        int tw = ui_text_width(lab);
        ui_draw_text(s, kx + (kw4-tw)/2, yb + (key_h-FONT_Y)/2, lab, (i>=3)?0xFFFF:0xFFFF);
    }
}

bool ui_keyboard_tap(ui_keyboard_t *kb, int x, int y, int screen_w, int screen_h) {
    const char **rows = kb->sym ? kb_rows_sym : kb_rows_abc;
    int key_h = 44;
    int y0 = screen_h - key_h * 5 - 4;
    if (y < y0) return false;
    int ry = y0 + 4;
    for (int r = 0; rows[r]; r++) {
        if (y >= ry && y < ry + key_h) {
            const char *row = rows[r];
            int n = (int)strlen(row);
            int kw = (screen_w - 8) / (n > 0 ? n : 1);
            int i = (x - 4) / kw;
            if (i >= 0 && i < n && kb->len < UI_KB_BUF_MAX - 1) {
                char c = row[i];
                if (!kb->sym && kb->shift && c >= 'a' && c <= 'z') c -= 32;
                kb->buf[kb->len++] = c;
                kb->buf[kb->len] = 0;
                if (!kb->sym && kb->shift) kb->shift = 0; /* one-shot feel optional */
                return true;
            }
            return false;
        }
        ry += key_h;
    }
    int yb = ry;
    if (y >= yb && y < yb + key_h) {
        int kw4 = (screen_w - 8) / 5;
        int i = (x - 4) / kw4;
        if (i == 0) {
            if (kb->sym) { kb->sym = 0; kb->shift = 0; }
            else kb->shift = !kb->shift;
            return true;
        } else if (i == 1) {
            if (kb->len < UI_KB_BUF_MAX - 1) { kb->buf[kb->len++] = ' '; kb->buf[kb->len] = 0; }
            return true;
        } else if (i == 2) {
            if (kb->len > 0) { kb->len--; kb->buf[kb->len] = 0; }
            return true;
        } else if (i == 3) {
            kb->cancel = true; return true;
        } else if (i == 4) {
            kb->done = true; return true;
        }
    }
    return false;
}

/* ---- List helper ---- */
void ui_list_init(ui_list_t *l, int x, int y, int w, int h, int item_h) {
    memset(l, 0, sizeof(*l));
    l->x=x; l->y=y; l->w=w; l->h=h; l->item_h=item_h;
    l->visible = item_h > 0 ? h / item_h : 0;
}
void ui_list_set_count(ui_list_t *l, int count) {
    l->count = count;
    if (l->selected >= count) l->selected = count > 0 ? count - 1 : 0;
    if (l->selected < 0) l->selected = 0;
    ui_list_ensure_visible(l);
}
int ui_list_hit(ui_list_t *l, int tx, int ty) {
    if (tx < l->x || tx >= l->x+l->w || ty < l->y || ty >= l->y+l->h) return -1;
    int idx = l->scroll + (ty - l->y) / l->item_h;
    if (idx < 0 || idx >= l->count) return -1;
    return idx;
}
void ui_list_ensure_visible(ui_list_t *l) {
    if (l->visible <= 0) return;
    if (l->selected < l->scroll) l->scroll = l->selected;
    if (l->selected >= l->scroll + l->visible) l->scroll = l->selected - l->visible + 1;
    if (l->scroll < 0) l->scroll = 0;
    int maxs = l->count - l->visible; if (maxs < 0) maxs = 0;
    if (l->scroll > maxs) l->scroll = maxs;
}

/* ---- App lifecycle helpers ---- */
int ui_app_get_slot(int argc, char **argv, const char *default_name, uint32_t color) {
    const char *path = (argc > 0 && argv && argv[0]) ? argv[0] : NULL;
    int slot = -1;
    if (path) slot = appmgr_find_path(path);
    if (slot < 0) {
        slot = appmgr_register(default_name ? default_name : "app", path ? path : "", color, get_current_thread());
    }
    return slot;
}

bool ui_app_should_close(int slot) {
    int st = appmgr_get_state(slot);
    return (st == APP_CLOSING || st == APP_FREE || st == APP_EXITED);
}

bool ui_app_handle_sys_gesture(int slot, ui_evt_t *ev, uint16_t *fb, int w, int h) {
    if (!ev || ev->type != UI_EVT_SWIPE) return false;
    if (ev->edge != UI_EDGE_BOTTOM || ev->dir != UI_DIR_UP) return false;
    /* en raíz: ir a background */
    appmgr_suspend_background(slot, fb, w, h, 64, 96);
    int ns = appmgr_wait_resume(slot);
    if (ns == APP_CLOSING || ns == APP_FREE || ns == APP_EXITED) return false;
    /* reanudado */
    fbcon_p4_suspend();
    p4_touch_init();
    return true; /* caller debe redibujar/recuperar estado UI */
}
