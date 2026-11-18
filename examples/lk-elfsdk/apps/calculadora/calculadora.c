#include "../lib/lkui.h"
#include <stdint.h>

#define W 320
#define H 480
#define FB_PIXELS (W*H)
#define FB_BYTES  (FB_PIXELS*2)

#define C_BG   0x1082
#define C_DISP 0x0000
#define C_TXT  0xFFFF
#define C_BTN  0x2945
#define C_OP   0xFD20
#define C_EQ   0x07E0
#define C_SP   0x5ACB
#define C_ACC  0x001F

typedef struct {
    uint16_t *fb;
    gfx_surface *s;
    ui_gesture_t gest;
    int slot;
    char expr[64];
    char display[64];
    double acc;
    double cur;
    int has_acc;
    char op; /* 0,+,-,*,/ */
    int new_entry;
    int error;
} calc_t;

static void fmt_double(double v, char *out, int outsz) {
    if (v != v) { strlcpy(out, "Error", outsz); return; }
    /* formato simple: entero si cabe, sino hasta 8 decimales recortando ceros */
    long long iv = (long long)v;
    double frac = v - (double)iv;
    if (frac < 0) frac = -frac;
    if (frac < 1e-9 && v > -1e12 && v < 1e12) {
        snprintf(out, outsz, "%lld", iv);
        return;
    }
    /* usar snprintf si soporta %f, si no fallback manual */
    int n = snprintf(out, outsz, "%.8f", v);
    if (n <= 0) { strlcpy(out, "0", outsz); return; }
    /* recortar ceros finales */
    char *p = out + strlen(out) - 1;
    while (p > out && *p == '0') { *p-- = 0; }
    if (*p == '.') *p = 0;
}

static void update_display(calc_t *c) {
    if (c->error) { strlcpy(c->display, "Error", sizeof(c->display)); return; }
    fmt_double(c->cur, c->display, sizeof(c->display));
}

static void clear_all(calc_t *c) {
    c->acc = 0; c->cur = 0; c->has_acc = 0; c->op = 0; c->new_entry = 1; c->error = 0;
    strlcpy(c->expr, "", sizeof(c->expr));
    strlcpy(c->display, "0", sizeof(c->display));
}

static void input_digit(calc_t *c, char d) {
    if (c->error) clear_all(c);
    if (c->new_entry) { strlcpy(c->display, "0", sizeof(c->display)); c->new_entry = 0; }
    if (strcmp(c->display, "0") == 0 && d != '.') c->display[0] = 0;
    if (d == '.') {
        /* solo un punto */
        for (char *p = c->display; *p; p++) if (*p == '.') return;
        if (c->display[0] == 0) strlcpy(c->display, "0", sizeof(c->display));
    }
    int len = (int)strlen(c->display);
    if (len < 18) { c->display[len] = d; c->display[len+1] = 0; }
    /* parse manual */
    {
        double val = 0, sign = 1, base = 0.1;
        int i = 0, in_frac = 0;
        char *s = c->display;
        if (s[0]=='-') { sign=-1; i++; }
        for (; s[i]; i++) {
            if (s[i]=='.') { in_frac=1; continue; }
            if (s[i]>='0' && s[i]<='9') {
                if (!in_frac) val = val*10 + (s[i]-'0');
                else { val += (s[i]-'0')*base; base*=0.1; }
            }
        }
        c->cur = sign*val;
    }
}

static void apply_op(calc_t *c, char op) {
    if (c->error) return;
    if (c->has_acc && !c->new_entry) {
        double r = c->acc;
        if (c->op == '+') r = c->acc + c->cur;
        else if (c->op == '-') r = c->acc - c->cur;
        else if (c->op == '*') r = c->acc * c->cur;
        else if (c->op == '/') {
            if (c->cur == 0.0) { c->error = 1; update_display(c); return; }
            r = c->acc / c->cur;
        } else r = c->cur;
        c->acc = r; c->cur = r;
    } else {
        c->acc = c->cur;
    }
    c->has_acc = 1;
    c->op = op;
    c->new_entry = 1;
    fmt_double(c->acc, c->display, sizeof(c->display));
    snprintf(c->expr, sizeof(c->expr), "%s %c", c->display, op?op:'=');
}

static void do_equals(calc_t *c) {
    if (!c->has_acc || c->op == 0) { update_display(c); return; }
    apply_op(c, 0);
    c->has_acc = 0; c->op = 0; c->new_entry = 1;
    c->expr[0] = 0;
}

static void do_back(calc_t *c) {
    if (c->error || c->new_entry) return;
    int len = (int)strlen(c->display);
    if (len > 1) c->display[len-1] = 0;
    else strlcpy(c->display, "0", sizeof(c->display));
    /* reparse */
    double val = 0, sign = 1, base = 0.1; int i=0, in_frac=0; char *s=c->display;
    if (s[0]=='-'){sign=-1;i++;}
    for(;s[i];i++){ if(s[i]=='.'){in_frac=1;continue;} if(s[i]>='0'&&s[i]<='9'){ if(!in_frac) val=val*10+(s[i]-'0'); else {val+=(s[i]-'0')*base; base*=0.1;} } }
    c->cur = sign*val;
}

static void do_sign(calc_t *c) {
    if (c->error) return;
    c->cur = -c->cur;
    fmt_double(c->cur, c->display, sizeof(c->display));
}

static const char *labels[5][4] = {
    {"C", "+/-", "<-", "/"},
    {"7", "8", "9", "*"},
    {"4", "5", "6", "-"},
    {"1", "2", "3", "+"},
    {"0", ".", "=", "="}
};

static void draw(calc_t *c) {
    gfx_fillrect(c->s, 0, 0, W, H, C_BG);
    gfx_fillrect(c->s, 0, 0, W, 28, 0x0010);
    ui_draw_text(c->s, 8, 8, "Calculadora", C_TXT);
    gfx_fillrect(c->s, 8, 36, W-16, 70, C_DISP);
    ui_draw_rect_outline(c->s, 8, 36, W-16, 70, 0x39E7);
    ui_draw_text_clip(c->s, 12, 42, W-24, c->expr, 0x07E0);
    /* display alineado a la derecha */
    int tw = ui_text_width(c->display);
    int dx = W - 16 - tw; if (dx < 12) dx = 12;
    ui_draw_text(c->s, dx, 72, c->display, C_TXT);

    int bx=8, by=116, bw=(W-16-9)/4, bh=64, gap=3;
    for (int r=0;r<5;r++) for (int col=0;col<4;col++) {
        if (r==4 && col==3) continue; /* '=' ocupa 2 */
        int x = bx + col*(bw+gap);
        int y = by + r*(bh+gap);
        int ww = bw, hh = bh;
        const char *lab = labels[r][col];
        if (r==4 && col==2) { ww = bw*2+gap; lab = "="; }
        uint32_t bg = C_BTN;
        if (lab[0]=='C' || (lab[0]=='+' && lab[1]=='/') || (lab[0]=='<' ) ) bg = C_SP;
        if (lab[0]=='/'||lab[0]=='*'||(lab[0]=='-'&&lab[1]==0)||(lab[0]=='+'&&lab[1]==0)) bg = C_OP;
        if (lab[0]=='=' ) bg = C_EQ;
        gfx_fillrect(c->s, x, y, ww, hh, bg);
        ui_draw_rect_outline(c->s, x, y, ww, hh, 0x0000);
        int ltw = ui_text_width(lab);
        ui_draw_text(c->s, x + (ww-ltw)/2, y + (hh-FONT_Y)/2, lab, (bg==C_EQ)?0x0000:0xFFFF);
    }
    gfx_fillrect(c->s, 0, H-24, W, 24, 0x0010);
    ui_draw_text(c->s, 6, H-18, "^ borde inf = salir/bg", 0x8410);
}

static void handle_tap(calc_t *c, int tx, int ty) {
    int bx=8, by=116, bw=(W-16-9)/4, bh=64, gap=3;
    for (int r=0;r<5;r++) for (int col=0;col<4;col++) {
        if (r==4 && col==3) continue;
        int x = bx + col*(bw+gap);
        int y = by + r*(bh+gap);
        int ww = bw, hh = bh;
        if (r==4 && col==2) ww = bw*2+gap;
        if (tx>=x && tx<x+ww && ty>=y && ty<y+hh) {
            const char *lab = labels[r][col];
            if (r==4 && col==2) lab = "=";
            if (lab[0]>='0' && lab[0]<='9' && lab[1]==0) input_digit(c, lab[0]);
            else if (lab[0]=='.' && lab[1]==0) input_digit(c, '.');
            else if (lab[0]=='C' && lab[1]==0) clear_all(c);
            else if (lab[0]=='<' ) do_back(c);
            else if (lab[0]=='+' && lab[1]=='/') do_sign(c);
            else if (lab[0]=='+' && lab[1]==0) apply_op(c, '+');
            else if (lab[0]=='-' && lab[1]==0) apply_op(c, '-');
            else if (lab[0]=='*') apply_op(c, '*');
            else if (lab[0]=='/') apply_op(c, '/');
            else if (lab[0]=='=') do_equals(c);
            return;
        }
    }
}

int main(int argc, char **argv) {
    calc_t c; memset(&c,0,sizeof(c));
    c.slot = ui_app_get_slot(argc, argv, "Calculadora", 0xFD20);
    c.fb = (uint16_t*)psram_malloc(FB_BYTES);
    if (!c.fb) return -1;
    c.s = gfx_create_surface(c.fb, W, H, W, GFX_FORMAT_RGB_565);
    if (!c.s) { psram_free(c.fb); return -1; }
    clear_all(&c);

    for (;;) {
        if (appmgr_get_state(c.slot) == APP_RUNNING) {
            fbcon_p4_suspend();
            p4_touch_init();
            ui_gesture_init(&c.gest, W, H);
            int dirty = 1;
            while (appmgr_get_state(c.slot) == APP_RUNNING) {
                if (dirty) {
                    draw(&c);
                    arch_clean_cache_range((unsigned long)c.fb, FB_BYTES);
                    fbcon_p4_present(c.fb);
                    dirty = 0;
                }
                ui_evt_t ev;
                while (ui_gesture_poll(&c.gest, &ev)) {
                    if (ev.type == UI_EVT_SWIPE && ev.edge == UI_EDGE_BOTTOM && ev.dir == UI_DIR_UP) {
                        appmgr_suspend_background(c.slot, c.fb, W, H, 64, 96);
                        goto suspended;
                    } else if (ev.type == UI_EVT_TAP) {
                        handle_tap(&c, ev.x1, ev.y1);
                        dirty = 1;
                    }
                }
                thread_sleep(30);
            }
        }
suspended:
        if (ui_app_should_close(c.slot)) break;
        int ns = appmgr_wait_resume(c.slot);
        if (ns == APP_CLOSING || ns == APP_FREE || ns == APP_EXITED) break;
    }

    gfx_surface_destroy(c.s);
    psram_free(c.fb);
    appmgr_mark_exited(c.slot);
    return 0;
}
