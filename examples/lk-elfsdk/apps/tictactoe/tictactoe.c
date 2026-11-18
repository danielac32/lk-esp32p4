#include "../lib/lkui.h"

#define W 320
#define H 480
#define FB_PIXELS (W*H)
#define FB_BYTES  (FB_PIXELS*2)

#define C_BG 0x0861
#define C_GRID 0xFFFF
#define C_X 0xF800
#define C_O 0x07FF
#define C_TX 0xFFFF
#define C_WIN 0x07E0

typedef struct {
    uint16_t *fb; gfx_surface *s; ui_gesture_t gest; int slot;
    int board[9]; /* 0 empty,1 X,2 O */
    int turn; /* 1 or 2 */
    int winner; /* 0 none,1 X,2 O,3 draw */
} ttt_t;

static int check_win(int *b) {
    static const int lines[8][3] = {
        {0,1,2},{3,4,5},{6,7,8},{0,3,6},{1,4,7},{2,5,8},{0,4,8},{2,4,6}
    };
    for (int i=0;i<8;i++) {
        int a=lines[i][0], c=lines[i][1], d=lines[i][2];
        if (b[a] && b[a]==b[c] && b[a]==b[d]) return b[a];
    }
    for (int i=0;i<9;i++) if (b[i]==0) return 0;
    return 3;
}

static void reset(ttt_t *g) {
    memset(g->board,0,sizeof(g->board));
    g->turn=1; g->winner=0;
}

static void draw_x(gfx_surface *s, int x,int y,int sz,uint32_t col) {
    gfx_line(s, x+8, y+8, x+sz-8, y+sz-8, col);
    gfx_line(s, x+sz-8, y+8, x+8, y+sz-8, col);
    gfx_line(s, x+9, y+8, x+sz-7, y+sz-8, col);
    gfx_line(s, x+sz-9, y+8, x+7, y+sz-8, col);
}
static void draw_o(gfx_surface *s, int cx,int cy,int r,uint32_t col) {
    /* aproximar circulo con rects/pixels */
    for (int dy=-r; dy<=r; dy++) for (int dx=-r; dx<=r; dx++) {
        int d2 = dx*dx + dy*dy;
        if (d2 >= (r-3)*(r-3) && d2 <= r*r) gfx_putpixel(s, cx+dx, cy+dy, col);
    }
}

static void draw(ttt_t *g) {
    gfx_fillrect(g->s,0,0,W,H,C_BG);
    ui_draw_text(g->s, 10, 10, "Tres en raya", C_TX);
    char st[48];
    if (g->winner==1) strlcpy(st, "Gana X  (toca Reiniciar)", sizeof(st));
    else if (g->winner==2) strlcpy(st, "Gana O  (toca Reiniciar)", sizeof(st));
    else if (g->winner==3) strlcpy(st, "Empate  (toca Reiniciar)", sizeof(st));
    else snprintf(st, sizeof(st), "Turno: %c", g->turn==1?'X':'O');
    ui_draw_text(g->s, 10, 32, st, g->winner?C_WIN:C_TX);

    int gx=20, gy=70, gs=280, cs=gs/3;
    /* grid */
    for (int i=1;i<3;i++) {
        gfx_fillrect(g->s, gx + i*cs-1, gy, 3, gs, C_GRID);
        gfx_fillrect(g->s, gx, gy + i*cs-1, gs, 3, C_GRID);
    }
    ui_draw_rect_outline(g->s, gx, gy, gs, gs, C_GRID);

    for (int r=0;r<3;r++) for (int c0=0;c0<3;c0++) {
        int v = g->board[r*3+c0];
        int x = gx + c0*cs;
        int y = gy + r*cs;
        if (v==1) draw_x(g->s, x,y,cs,C_X);
        else if (v==2) draw_o(g->s, x+cs/2, y+cs/2, cs/2-10, C_O);
    }

    /* boton reiniciar */
    gfx_fillrect(g->s, 90, 370, 140, 44, 0x2945);
    ui_draw_rect_outline(g->s, 90, 370, 140, 44, C_TX);
    ui_draw_text(g->s, 120, 386, "Reiniciar", C_TX);
    ui_draw_text(g->s, 40, H-24, "^ borde inf = salir/bg", 0x8410);
}

int main(int argc, char **argv) {
    ttt_t g; memset(&g,0,sizeof(g));
    g.slot = ui_app_get_slot(argc, argv, "Tres en raya", 0x07FF);
    g.fb = (uint16_t*)psram_malloc(FB_BYTES);
    if (!g.fb) return -1;
    g.s = gfx_create_surface(g.fb,W,H,W,GFX_FORMAT_RGB_565);
    if (!g.s){psram_free(g.fb);return -1;}
    reset(&g);
    for(;;){
        if (appmgr_get_state(g.slot)==APP_RUNNING){
            fbcon_p4_suspend(); p4_touch_init(); ui_gesture_init(&g.gest,W,H);
            int dirty=1;
            while(appmgr_get_state(g.slot)==APP_RUNNING){
                if(dirty){ draw(&g); arch_clean_cache_range((unsigned long)g.fb,FB_BYTES); fbcon_p4_present(g.fb); dirty=0; }
                ui_evt_t ev;
                while(ui_gesture_poll(&g.gest,&ev)){
                    if(ev.type==UI_EVT_SWIPE && ev.edge==UI_EDGE_BOTTOM && ev.dir==UI_DIR_UP){
                        appmgr_suspend_background(g.slot,g.fb,W,H,64,96); goto suspended;
                    } else if(ev.type==UI_EVT_TAP){
                        if(ev.x1>=90 && ev.x1<=230 && ev.y1>=370 && ev.y1<=414){ reset(&g); dirty=1; continue; }
                        int gx=20,gy=70,gs=280,cs=gs/3;
                        if(!g.winner && ev.x1>=gx && ev.x1<gx+gs && ev.y1>=gy && ev.y1<gy+gs){
                            int c0=(ev.x1-gx)/cs, r=(ev.y1-gy)/cs;
                            int idx=r*3+c0;
                            if(idx>=0&&idx<9 && g.board[idx]==0){
                                g.board[idx]=g.turn; g.turn = (g.turn==1)?2:1; g.winner=check_win(g.board); dirty=1;
                            }
                        }
                    }
                }
                thread_sleep(30);
            }
        }
suspended:
        if(ui_app_should_close(g.slot)) break;
        int ns=appmgr_wait_resume(g.slot);
        if(ns==APP_CLOSING||ns==APP_FREE||ns==APP_EXITED) break;
    }
    gfx_surface_destroy(g.s); psram_free(g.fb); appmgr_mark_exited(g.slot); return 0;
}
