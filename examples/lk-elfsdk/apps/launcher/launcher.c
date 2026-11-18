/*
 * Launcher LK tipo Android (nativo, gfx/font/touch).
 * Home: grid de iconos, swipe ←/→ páginas. Recientes: tarjetas con miniatura.
 * Gestos: ↑ borde inferior alterna Home ↔ Recientes.
 */
#include "../lib/lkui.h"

#define W 320
#define H 480
#define FB_PIXELS (W*H)
#define FB_BYTES  (FB_PIXELS*2)

#define COLS 4
#define ROWS 4
#define APPS_PER_PAGE (COLS*ROWS)
#define MAX_APPS 48

/* Paleta Android-like */
#define C_BG      0xFFFF  /* blanco */
#define C_BG2     0xEF7D  /* gris muy claro */
#define C_BAR     0x18E3  /* barra oscura */
#define C_BAR_TX  0xFFFF
#define C_ACC     0x2D7F  /* azul Android */
#define C_TEXT    0x2104  /* texto oscuro */
#define C_SUB     0x8410
#define C_DOT_ON  0x2D7F
#define C_DOT_OFF 0xC618
#define C_WHITE   0xFFFF
#define C_REC_BG  0xE71C
#define C_CARD    0xFFFF
#define C_CARD_B  0xD69A
#define C_GREEN   0x07E0
#define C_WARN    0xFD20
#define C_RED     0xF800

#define VIEW_HOME    0
#define VIEW_RECENTS 1

typedef struct {
    char name[48];
    char path[128];
    char label[20];
    uint16_t color;
    char icon[4];
} app_info_t;

typedef struct {
    const char *path;
    const char *name;
    int slot;
} runarg_t;

static uint16_t *g_fb;
static gfx_surface *g_s;
static ui_gesture_t g_gest;
static app_info_t g_apps[MAX_APPS];
static int g_napps;
static int g_page;
static int g_view;

static unsigned name_hash(const char *s) {
    unsigned h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}
static bool is_elf_name(const char *n) {
    size_t l = strlen(n);
    if (l < 4) return false;
    const char *e = n + l - 4;
    return (e[0]=='.'&&(e[1]=='e'||e[1]=='E')&&(e[2]=='l'||e[2]=='L')&&(e[3]=='f'||e[3]=='F'));
}
static bool is_launcher_self(const char *n) {
    return strcmp(n, "launcher.elf")==0 || strcmp(n, "LAUNCHER.ELF")==0 || strcmp(n, "Launcher.elf")==0;
}
static bool str_contains_i(const char *s, const char *sub) {
    for (; *s; s++) {
        const char *a = s, *b = sub;
        while (*a && *b) {
            char ca=*a, cb=*b;
            if (ca>='A'&&ca<='Z') ca+=32;
            if (cb>='A'&&cb<='Z') cb+=32;
            if (ca!=cb) break; a++; b++;
        }
        if (!*b) return true;
    }
    return false;
}

static void app_style(app_info_t *a) {
    char *dot = NULL; for (char *p=a->name; *p; p++) if (*p=='.') dot=p;
    if (dot) { size_t n = (size_t)(dot - a->name); if (n>=sizeof(a->label)) n=sizeof(a->label)-1; memcpy(a->label,a->name,n); a->label[n]=0; }
    else strlcpy(a->label, a->name, sizeof(a->label));
    if (str_contains_i(a->name,"calc")) { a->color=0x4C66; strlcpy(a->icon,"+=",3); strlcpy(a->label,"Calculadora",sizeof(a->label)); }
    else if (str_contains_i(a->name,"tetris")) { a->color=0x9B3F; strlcpy(a->icon,"Te",3); strlcpy(a->label,"Tetris",sizeof(a->label)); }
    else if (str_contains_i(a->name,"tictac")||str_contains_i(a->name,"tres")||str_contains_i(a->name,"tateti")) { a->color=0xF986; strlcpy(a->icon,"XO",3); strlcpy(a->label,"3 en raya",sizeof(a->label)); }
    else if (str_contains_i(a->name,"archivo")||str_contains_i(a->name,"file")||str_contains_i(a->name,"gestor")) { a->color=0xFD20; strlcpy(a->icon,"Di",3); strlcpy(a->label,"Archivos",sizeof(a->label)); }
    else if (str_contains_i(a->name,"ajuste")||str_contains_i(a->name,"setting")||str_contains_i(a->name,"config")) { a->color=0x5ACF; strlcpy(a->icon,"Aj",3); strlcpy(a->label,"Ajustes",sizeof(a->label)); }
    else if (str_contains_i(a->name,"demo")) { a->color=0x07EF; strlcpy(a->icon,"Dm",3); strlcpy(a->label,"Demo",sizeof(a->label)); }
    else if (str_contains_i(a->name,"music")||str_contains_i(a->name,"audio")) { a->color=0xE2BF; strlcpy(a->icon,"Mu",3); strlcpy(a->label,"Musica",sizeof(a->label)); }
    else if (str_contains_i(a->name,"hello")||str_contains_i(a->name,"hola")) { a->color=0x2D7F; strlcpy(a->icon,"Hi",3); strlcpy(a->label,"Hola",sizeof(a->label)); }
    else {
        unsigned h = name_hash(a->name);
        static const uint16_t pal[] = {0x2D7F,0x4C66,0xF986,0xFD20,0x9B3F,0x07EF,0xE2BF,0x5ACF,0xE8A2,0x34DF};
        a->color = pal[h % 10];
        a->icon[0]=a->label[0]?a->label[0]:'A'; if(a->icon[0]>='a'&&a->icon[0]<='z') a->icon[0]-=32; a->icon[1]=0;
        if ((int)strlen(a->label)>11) a->label[11]=0;
    }
}

static void scan_apps(void) {
    g_napps = 0;
    const char *dirs[] = {"/sd/appslkx", "/sd/apps", NULL};
    for (int d = 0; dirs[d] && g_napps < MAX_APPS; d++) {
        fs_make_dir(dirs[d]);
        dirhandle *dh = NULL;
        if (fs_open_dir(dirs[d], &dh) < 0) continue;
        struct dirent ent;
        while (g_napps < MAX_APPS && fs_read_dir(dh, &ent) == 0) {
            if (ent.name[0] == '.') continue;
            if (!is_elf_name(ent.name)) continue;
            if (is_launcher_self(ent.name)) continue;
            char full[128]; snprintf(full,sizeof(full),"%s/%s",dirs[d],ent.name);
            bool dup=false; for(int i=0;i<g_napps;i++) if(strcmp(g_apps[i].path,full)==0){dup=true;break;}
            if (dup) continue;
            app_info_t *a = &g_apps[g_napps];
            strlcpy(a->name, ent.name, sizeof(a->name));
            strlcpy(a->path, full, sizeof(a->path));
            app_style(a);
            g_napps++;
        }
        fs_close_dir(dh);
    }
    int pages = (g_napps + APPS_PER_PAGE - 1) / APPS_PER_PAGE;
    if (pages < 1) pages = 1;
    if (g_page >= pages) g_page = pages - 1;
    if (g_page < 0) g_page = 0;
}

static void present(void) {
    arch_clean_cache_range((unsigned long)g_fb, FB_BYTES);
    fbcon_p4_present(g_fb);
}

static void draw_icon_tile(gfx_surface *s, int x, int y, int sz, uint16_t color, const char *glyph) {
    /* sombra suave */
    gfx_fillrect(s, x+3, y+4, sz, sz, 0xD69A);
    /* tile redondeado (aprox) */
    gfx_fillrect(s, x+4, y+0, sz-8, sz, color);
    gfx_fillrect(s, x+0, y+4, sz, sz-8, color);
    gfx_fillrect(s, x+2, y+2, sz-4, sz-4, color);
    /* highlight superior */
    gfx_fillrect(s, x+6, y+3, sz-12, 2, 0xFFFF);
    /* glyph grande (doble dibujando offset) */
    if (glyph && glyph[0]) {
        int tw = ui_text_width(glyph);
        int tx = x + (sz - tw)/2;
        int ty = y + (sz - FONT_Y)/2;
        ui_draw_text(s, tx+1, ty, glyph, 0x0000);
        ui_draw_text(s, tx, ty, glyph, 0xFFFF);
    }
}

static void draw_home(void) {
    gfx_fillrect(g_s, 0, 0, W, H, C_BG);
    /* barra superior */
    gfx_fillrect(g_s, 0, 0, W, 30, C_BAR);
    ui_draw_text(g_s, 8, 9, "LK Tablet", C_BAR_TX);
    char info[32]; snprintf(info,sizeof(info),"%d apps", g_napps);
    int tw = ui_text_width(info);
    ui_draw_text(g_s, W-tw-8, 9, info, 0xA7FF);

    /* franja decorativa */
    gfx_fillrect(g_s, 0, 30, W, 4, C_ACC);

    int grid_top = 44;
    int grid_h = 360;
    int cell_w = W / COLS;
    int cell_h = grid_h / ROWS;

    if (g_napps == 0) {
        gfx_fillrect(g_s, 20, 120, W-40, 120, C_BG2);
        ui_draw_rect_outline(g_s, 20, 120, W-40, 120, C_CARD_B);
        ui_draw_text(g_s, 36, 145, "Sin aplicaciones", C_TEXT);
        ui_draw_text(g_s, 36, 168, "No hay .elf en", C_SUB);
        ui_draw_text(g_s, 36, 186, "/sd/appslkx", C_ACC);
        ui_draw_text(g_s, 36, 208, "Sube apps y toca refrescar", C_SUB);
        /* boton refrescar */
        gfx_fillrect(g_s, W/2-50, 260, 100, 36, C_ACC);
        ui_draw_text(g_s, W/2-30, 272, "Refrescar", C_WHITE);
    } else {
        int start = g_page * APPS_PER_PAGE;
        for (int r = 0; r < ROWS; r++) {
            for (int c = 0; c < COLS; c++) {
                int idx = start + r * COLS + c;
                if (idx >= g_napps) break;
                app_info_t *a = &g_apps[idx];
                int cx = c * cell_w;
                int cy = grid_top + r * cell_h;
                int icon_sz = 54;
                int ix = cx + (cell_w - icon_sz) / 2;
                int iy = cy + 6;
                draw_icon_tile(g_s, ix, iy, icon_sz, a->color, a->icon);
                int lw = ui_text_width(a->label);
                int lx = cx + (cell_w - lw) / 2;
                if (lx < cx + 2) lx = cx + 2;
                ui_draw_text_clip(g_s, lx, iy + icon_sz + 8, cell_w - 4, a->label, C_TEXT);
            }
        }
    }

    /* dots */
    int pages = (g_napps + APPS_PER_PAGE - 1) / APPS_PER_PAGE;
    if (pages < 1) pages = 1;
    if (pages > 1) {
        int dots_w = pages * 14;
        int dx0 = (W - dots_w) / 2;
        for (int i = 0; i < pages; i++) {
            int px = dx0 + i * 14 + 4;
            gfx_fillrect(g_s, px, 414, 8, 8, (i == g_page) ? C_DOT_ON : C_DOT_OFF);
        }
    }

    /* dock inferior */
    gfx_fillrect(g_s, 10, H-44, W-20, 36, C_BG2);
    ui_draw_rect_outline(g_s, 10, H-44, W-20, 36, C_CARD_B);
    ui_draw_text(g_s, 22, H-32, "< pagina", C_SUB);
    int mtw = ui_text_width("^ recientes");
    ui_draw_text(g_s, W-22-mtw, H-32, "^ recientes", C_ACC);
    gfx_fillrect(g_s, W/2 - 30, H-6, 60, 4, 0xC618);
    present();
}

static void draw_recents(void) {
    gfx_fillrect(g_s, 0, 0, W, H, C_REC_BG);
    gfx_fillrect(g_s, 0, 0, W, 30, C_BAR);
    ui_draw_text(g_s, 8, 9, "Recientes", C_BAR_TX);
    ui_draw_text(g_s, W-70, 9, "^ Inicio", 0xA7FF);
    gfx_fillrect(g_s, 0, 30, W, 4, C_ACC);

    int y = 42;
    int shown = 0;
    appmgr_entry_t *tab = appmgr_table();
    int cnt = appmgr_count();
    for (int i = 0; i < cnt; i++) {
        if (tab[i].state != APP_RUNNING && tab[i].state != APP_BACKGROUND) continue;
        if (y > H - 96) break;
        int card_h = 84;
        gfx_fillrect(g_s, 10, y+2, W-20, card_h, 0xC618);
        gfx_fillrect(g_s, 10, y, W-20, card_h, C_CARD);
        ui_draw_rect_outline(g_s, 10, y, W-20, card_h, C_CARD_B);
        int tx=16, ty=y+8, twi=44, thi=66;
        gfx_fillrect(g_s, tx, ty, twi, thi, 0x2104);
        if (tab[i].thumb && tab[i].thumb_w>0 && tab[i].thumb_h>0)
            ui_draw_thumb(g_s, tx+1, ty+1, twi-2, thi-2, tab[i].thumb, tab[i].thumb_w, tab[i].thumb_h);
        const char *nm = tab[i].name[0]?tab[i].name:tab[i].path;
        char label[24]; strlcpy(label,nm,sizeof(label));
        char *dot=NULL; for(char*p=label;*p;p++) if(*p=='.') dot=p; if(dot)*dot=0;
        ui_draw_text_clip(g_s, 68, y+10, 170, label, C_TEXT);
        const char *st = (tab[i].state==APP_RUNNING)?"En uso":"En segundo plano";
        ui_draw_text(g_s, 68, y+32, st, (tab[i].state==APP_RUNNING)?0x03E0:0xFD20);
        ui_draw_text_clip(g_s, 68, y+52, 170, tab[i].path, C_SUB);
        gfx_fillrect(g_s, W-44, y+26, 28, 28, C_RED);
        ui_draw_text(g_s, W-35, y+34, "X", C_WHITE);
        y += card_h + 8; shown++;
    }
    if (shown==0) {
        gfx_fillrect(g_s, 20, 140, W-40, 110, C_CARD);
        ui_draw_rect_outline(g_s, 20, 140, W-40, 110, C_CARD_B);
        ui_draw_text(g_s, 36, 165, "No hay apps recientes", C_TEXT);
        ui_draw_text(g_s, 36, 188, "Abre una app y sal con", C_SUB);
        ui_draw_text(g_s, 36, 206, "swipe ^ desde abajo", C_ACC);
    }
    gfx_fillrect(g_s, 10, H-44, W-20, 36, C_BG2);
    ui_draw_text(g_s, 22, H-32, "^ volver al inicio", C_ACC);
    gfx_fillrect(g_s, W/2 - 30, H-6, 60, 4, 0xC618);
    present();
}

static void draw_current(void) { if (g_view==VIEW_RECENTS) draw_recents(); else draw_home(); }

static void wait_until_not_running(int slot) {
    for (;;) {
        int st = appmgr_get_state(slot);
        if (st==APP_BACKGROUND||st==APP_EXITED||st==APP_FREE||st==APP_CLOSING) {
            if (st==APP_CLOSING) { thread_sleep(50); continue; }
            return;
        }
        thread_sleep(40);
    }
}

static int runner_thread(void *arg) {
    runarg_t *ra = (runarg_t *)arg;
    char path[128]; int slot = ra->slot;
    strlcpy(path, ra->path, sizeof(path)); free(ra);
    char *argv0 = path; (void)elf_run(path, 1, &argv0);
    int st = appmgr_get_state(slot);
    if (st != APP_FREE) { appmgr_mark_exited(slot); appmgr_unregister(slot); }
    return 0;
}

static void resume_or_launch(int app_index) {
    if (app_index < 0 || app_index >= g_napps) return;
    app_info_t *ai = &g_apps[app_index];
    int slot = appmgr_find_path(ai->path);
    if (slot >= 0) {
        int st = appmgr_get_state(slot);
        if (st==APP_BACKGROUND||st==APP_RUNNING) {
            appmgr_resume_app(slot); wait_until_not_running(slot);
            fbcon_p4_suspend(); p4_touch_init(); draw_current(); return;
        }
        if (st==APP_EXITED) { appmgr_unregister(slot); slot=-1; }
    }
    slot = appmgr_register(ai->label[0]?ai->label:ai->name, ai->path, ai->color, NULL);
    if (slot < 0) return;
    runarg_t *ra2 = (runarg_t*)malloc(sizeof(runarg_t)); if(!ra2){appmgr_unregister(slot);return;}
    ra2->path=ai->path; ra2->name=ai->name; ra2->slot=slot;
    appmgr_set_state(slot, APP_RUNNING);
    thread_t *th = thread_create("app", runner_thread, ra2, DEFAULT_PRIORITY, DEFAULT_STACK_SIZE*4);
    if(!th){free(ra2);appmgr_unregister(slot);return;}
    /* apps en core 1: no preemptar wifi_poll/netstack en core 0 */
    thread_set_pinned_cpu(th, 1);
    appmgr_table()[slot].thread = th; thread_resume(th);
    wait_until_not_running(slot);
    fbcon_p4_suspend(); p4_touch_init(); draw_current();
}

static void recents_tap(int tx, int ty) {
    appmgr_entry_t *tab = appmgr_table(); int cnt = appmgr_count();
    int y = 42;
    for (int i=0;i<cnt;i++) {
        if (tab[i].state!=APP_RUNNING && tab[i].state!=APP_BACKGROUND) continue;
        if (y > H-96) break;
        int card_h = 84;
        if (ty>=y && ty<y+card_h) {
            if (tx>=W-44 && tx<=W-16 && ty>=y+26 && ty<=y+54) {
                appmgr_request_close(i); thread_sleep(100); draw_current(); return;
            }
            if (tab[i].state==APP_BACKGROUND||tab[i].state==APP_RUNNING) {
                appmgr_resume_app(i); wait_until_not_running(i); fbcon_p4_suspend(); p4_touch_init();
            }
            draw_current(); return;
        }
        y += card_h + 8;
    }
}

static void home_tap(int tx, int ty) {
    int start = g_page * APPS_PER_PAGE;
    int grid_top=44, grid_h=360, cell_w=W/COLS, cell_h=grid_h/ROWS;
    /* refrescar si no hay apps */
    if (g_napps==0 && tx>=W/2-50 && tx<=W/2+50 && ty>=260 && ty<=296) { scan_apps(); draw_current(); return; }
    for (int r=0;r<ROWS;r++) for (int c=0;c<COLS;c++) {
        int idx = start + r*COLS + c; if (idx>=g_napps) break;
        int cx=c*cell_w, cy=grid_top + r*cell_h;
        if (tx>=cx && tx<cx+cell_w && ty>=cy && ty<cy+cell_h) { resume_or_launch(idx); return; }
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!fbcon_p4_is_ready()) { printf("launcher: pantalla no disponible\n"); return -1; }
    appmgr_init();
    g_fb = (uint16_t*)psram_malloc(FB_BYTES);
    if (!g_fb) { printf("launcher: sin fb\n"); return -1; }
    memset(g_fb,0,FB_BYTES);
    g_s = gfx_create_surface(g_fb,W,H,W,GFX_FORMAT_RGB_565);
    if (!g_s) { psram_free(g_fb); return -1; }
    fbcon_p4_suspend(); p4_touch_init(); ui_gesture_init(&g_gest,W,H);
    g_view=VIEW_HOME; g_page=0; scan_apps(); draw_current();
    for (;;) {
        ui_evt_t ev;
        while (ui_gesture_poll(&g_gest,&ev)) {
            if (ev.type==UI_EVT_SWIPE) {
                if (ev.edge==UI_EDGE_BOTTOM && ev.dir==UI_DIR_UP) {
                    g_view = (g_view==VIEW_HOME)?VIEW_RECENTS:VIEW_HOME; draw_current(); continue;
                }
                if (g_view==VIEW_HOME && (ev.dir==UI_DIR_LEFT||ev.dir==UI_DIR_RIGHT)) {
                    int pages=(g_napps+APPS_PER_PAGE-1)/APPS_PER_PAGE; if(pages<1) pages=1;
                    if (ev.dir==UI_DIR_LEFT && g_page<pages-1) g_page++;
                    else if (ev.dir==UI_DIR_RIGHT && g_page>0) g_page--;
                    draw_current(); continue;
                }
            } else if (ev.type==UI_EVT_TAP) {
                if (g_view==VIEW_RECENTS) recents_tap(ev.x1,ev.y1); else home_tap(ev.x1,ev.y1);
            }
        }
        thread_sleep(30);
    }
    return 0;
}
