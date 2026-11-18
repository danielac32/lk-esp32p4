#include "../lib/lkui.h"
#include <stdlib.h>

#define W 320
#define H 480
#define FB_PIXELS (W*H)
#define FB_BYTES  (FB_PIXELS*2)

#define COLS 10
#define ROWS 20
#define CELL 18
#define OX 10
#define OY 40

#define C_BG 0x0000
#define C_GRID 0x2104
#define C_TX 0xFFFF

static const uint16_t piece_colors[7] = {0x07FF,0x001F,0xFD20,0xFFE0,0x07E0,0x781F,0xF800};
/* 4x4 shapes, 4 rotations each */
static const uint16_t shapes[7][4] = {
 {0x0F00,0x2222,0x00F0,0x4444}, /* I */
 {0x8E00,0x6440,0x0E20,0x44C0}, /* J */
 {0x2E00,0x4460,0x0E80,0xC440}, /* L */
 {0x6600,0x6600,0x6600,0x6600}, /* O */
 {0x6C00,0x4620,0x06C0,0x8C40}, /* S */
 {0x4E00,0x4640,0x0E40,0x4C40}, /* T */
 {0xC600,0x2640,0x0C60,0x4C80}  /* Z */
};

typedef struct {
    uint16_t *fb; gfx_surface *s; ui_gesture_t gest; int slot;
    uint8_t board[ROWS][COLS];
    int px,py,pt,pr; /* pos, type, rot */
    int score, lines, level;
    int gameover;
    lk_time_t last_drop;
    unsigned seed;
} tet_t;

static int rnd(tet_t *t){ t->seed = t->seed*1664525u + 1013904223u; return (t->seed>>16)&0x7fff; }

static int cell_at(int type,int rot,int r,int c){
    return (shapes[type][rot] & (0x8000 >> (r*4+c))) != 0;
}

static int collides(tet_t *t,int nx,int ny,int nr){
    for(int r=0;r<4;r++) for(int c=0;c<4;c++) if(cell_at(t->pt,nr,r,c)){
        int bx=nx+c, by=ny+r;
        if(bx<0||bx>=COLS||by>=ROWS) return 1;
        if(by>=0 && t->board[by][bx]) return 1;
    }
    return 0;
}

static void spawn(tet_t *t){
    t->pt = rnd(t)%7; t->pr=0; t->px=COLS/2-2; t->py=-1;
    if(collides(t,t->px,t->py,t->pr)) t->gameover=1;
}

static void lock_piece(tet_t *t){
    for(int r=0;r<4;r++) for(int c=0;c<4;c++) if(cell_at(t->pt,t->pr,r,c)){
        int bx=t->px+c, by=t->py+r;
        if(by>=0 && by<ROWS && bx>=0 && bx<COLS) t->board[by][bx]=t->pt+1;
    }
    int cleared=0;
    for(int r=ROWS-1;r>=0;r--){
        int full=1; for(int c=0;c<COLS;c++) if(!t->board[r][c]){full=0;break;}
        if(full){
            cleared++;
            for(int rr=r; rr>0; rr--) for(int c=0;c<COLS;c++) t->board[rr][c]=t->board[rr-1][c];
            for(int c=0;c<COLS;c++) t->board[0][c]=0;
            r++; /* recheck row */
        }
    }
    if(cleared){
        t->lines += cleared;
        static const int sc[5]={0,100,300,500,800};
        t->score += sc[cleared] * (t->level+1);
        t->level = t->lines/10;
    }
    spawn(t);
}

static int drop(tet_t *t){
    if(!collides(t,t->px,t->py+1,t->pr)){ t->py++; return 1; }
    lock_piece(t); return 0;
}

static void hard_drop(tet_t *t){ while(drop(t)); }

static void reset(tet_t *t){
    memset(t->board,0,sizeof(t->board));
    t->score=0; t->lines=0; t->level=0; t->gameover=0; t->seed=(unsigned)current_time()^0x12345u;
    spawn(t); t->last_drop=current_time();
}

static void draw_cell(gfx_surface *s,int bx,int by,uint16_t col){
    int x=OX+bx*CELL, y=OY+by*CELL;
    if(by<0) return;
    gfx_fillrect(s,x+1,y+1,CELL-2,CELL-2,col);
    ui_draw_rect_outline(s,x+1,y+1,CELL-2,CELL-2,0xFFFF);
}
static void draw_board(tet_t *t){
    gfx_fillrect(t->s,0,0,W,H,C_BG);
    ui_draw_text(t->s,10,8,"Tetris",C_TX);
    char buf[48]; snprintf(buf,sizeof(buf),"Pts:%d Lin:%d Nv:%d",t->score,t->lines,t->level);
    ui_draw_text(t->s,90,8,buf,0x07E0);
    gfx_fillrect(t->s,OX,OY,COLS*CELL,ROWS*CELL,0x0841);
    ui_draw_rect_outline(t->s,OX-1,OY-1,COLS*CELL+2,ROWS*CELL+2,0x8410);
    for(int r=0;r<ROWS;r++) for(int c=0;c<COLS;c++) if(t->board[r][c])
        draw_cell(t->s,c,r,piece_colors[t->board[r][c]-1]);
    /* pieza actual */
    if(!t->gameover){
        for(int r=0;r<4;r++) for(int c=0;c<4;c++) if(cell_at(t->pt,t->pr,r,c))
            draw_cell(t->s,t->px+c,t->py+r,piece_colors[t->pt]);
    }
    /* controles */
    int by=OY+ROWS*CELL+8;
    gfx_fillrect(t->s,10,by,60,40,0x2945); ui_draw_text(t->s,30,by+14,"<",C_TX);
    gfx_fillrect(t->s,75,by,60,40,0x2945); ui_draw_text(t->s,88,by+14,"ROT",C_TX);
    gfx_fillrect(t->s,140,by,60,40,0x2945); ui_draw_text(t->s,162,by+14,">",C_TX);
    gfx_fillrect(t->s,205,by,50,40,0x2945); ui_draw_text(t->s,220,by+14,"v",C_TX);
    gfx_fillrect(t->s,260,by,50,40,0x7800); ui_draw_text(t->s,268,by+14,"DROP",C_TX);
    gfx_fillrect(t->s,10,by+46,300,30,0x4208); ui_draw_text(t->s,120,by+54,"Reiniciar",C_TX);
    if(t->gameover){
        gfx_fillrect(t->s,40,200,240,60,0xF800);
        ui_draw_text(t->s,110,210,"GAME OVER",0xFFFF);
        ui_draw_text(t->s,70,232,"Toca Reiniciar",0xFFFF);
    }
    ui_draw_text(t->s,6,H-18,"^ borde inf = salir/bg",0x8410);
}

static void handle_tap(tet_t *t,int tx,int ty){
    int by=OY+ROWS*CELL+8;
    if(ty>=by+46 && ty<by+76 && tx>=10 && tx<=310){ reset(t); return; }
    if(t->gameover) return;
    if(ty>=by && ty<by+40){
        if(tx>=10&&tx<70){ if(!collides(t,t->px-1,t->py,t->pr)) t->px--; }
        else if(tx>=75&&tx<135){ int nr=(t->pr+1)&3; if(!collides(t,t->px,t->py,nr)) t->pr=nr; }
        else if(tx>=140&&tx<200){ if(!collides(t,t->px+1,t->py,t->pr)) t->px++; }
        else if(tx>=205&&tx<255){ drop(t); }
        else if(tx>=260&&tx<310){ hard_drop(t); }
    }
}

int main(int argc,char **argv){
    tet_t t; memset(&t,0,sizeof(t));
    t.slot = ui_app_get_slot(argc,argv,"Tetris",0x07FF);
    t.fb=(uint16_t*)psram_malloc(FB_BYTES);
    if(!t.fb) return -1;
    t.s = gfx_create_surface(t.fb,W,H,W,GFX_FORMAT_RGB_565);
    if(!t.s){psram_free(t.fb);return -1;}
    reset(&t);
    for(;;){
        if(appmgr_get_state(t.slot)==APP_RUNNING){
            fbcon_p4_suspend(); p4_touch_init(); ui_gesture_init(&t.gest,W,H);
            t.last_drop=current_time();
            int dirty=1;
            while(appmgr_get_state(t.slot)==APP_RUNNING){
                lk_time_t now=current_time();
                int interval = 600 - t.level*40; if(interval<120) interval=120;
                if(!t.gameover && now - t.last_drop >= (lk_time_t)interval){ drop(&t); t.last_drop=now; dirty=1; }
                if(dirty){ draw_board(&t); arch_clean_cache_range((unsigned long)t.fb,FB_BYTES); fbcon_p4_present(t.fb); dirty=0; }
                ui_evt_t ev;
                while(ui_gesture_poll(&t.gest,&ev)){
                    if(ev.type==UI_EVT_SWIPE && ev.edge==UI_EDGE_BOTTOM && ev.dir==UI_DIR_UP){
                        appmgr_suspend_background(t.slot,t.fb,W,H,64,96); goto suspended;
                    } else if(ev.type==UI_EVT_TAP){ handle_tap(&t,ev.x1,ev.y1); dirty=1; }
                }
                thread_sleep(30);
            }
        }
suspended:
        if(ui_app_should_close(t.slot)) break;
        int ns=appmgr_wait_resume(t.slot);
        if(ns==APP_CLOSING||ns==APP_FREE||ns==APP_EXITED) break;
    }
    gfx_surface_destroy(t.s); psram_free(t.fb); appmgr_mark_exited(t.slot); return 0;
}
