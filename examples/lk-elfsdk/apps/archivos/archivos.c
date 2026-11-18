#include "../lib/lkui.h"

#define W 320
#define H 480
#define FB_PIXELS (W*H)
#define FB_BYTES  (FB_PIXELS*2)

#define MAX_ENT 64
#define NAME_MAX 64
#define PATH_MAX 256

typedef struct {
    char name[NAME_MAX];
    bool is_dir;
    uint64_t size;
} ent_t;

typedef enum { SCR_BROWSE, SCR_MENU, SCR_INFO, SCR_CONFIRM, SCR_INPUT, SCR_MSG } scr_t;
typedef enum { ACT_NONE, ACT_MKDIR, ACT_RENAME } in_act_t;

typedef struct {
    uint16_t *fb; gfx_surface *s; ui_gesture_t gest; int slot;
    char cwd[PATH_MAX];
    ent_t ents[MAX_ENT];
    int nent;
    int sel;
    int scroll;
    scr_t scr;
    scr_t prev_scr;
    /* menu */
    int menu_sel;
    /* confirm */
    char confirm_msg[80];
    void (*confirm_yes)(struct app*);
    /* input */
    ui_keyboard_t kb;
    in_act_t in_act;
    char input_target[NAME_MAX];
    /* clipboard */
    char clip_path[PATH_MAX];
    int clip_cut; /* 0=copy,1=cut */
    /* message */
    char msg[80];
} app_t;

static void set_msg(app_t *a, const char *m) { strlcpy(a->msg, m, sizeof(a->msg)); a->scr = SCR_MSG; }
static void path_join(char *out, int outsz, const char *dir, const char *name) {
    if (strcmp(dir, "/") == 0) snprintf(out, outsz, "/%s", name);
    else snprintf(out, outsz, "%s/%s", dir, name);
}
static char *xstrrchr(const char *s, int c) {
    const char *last = NULL;
    for (; *s; s++) if (*s == (char)c) last = s;
    return (char *)last;
}
static const char *basename_s(const char *p) {
    const char *s = xstrrchr(p, '/'); return s ? s+1 : p;
}

static void load_dir(app_t *a) {
    a->nent = 0; a->sel = 0; a->scroll = 0;
    dirhandle *dh = NULL;
    if (fs_open_dir(a->cwd, &dh) < 0) { set_msg(a, "No se pudo abrir dir"); return; }
    struct dirent de;
    while (a->nent < MAX_ENT && fs_read_dir(dh, &de) == 0) {
        if (strcmp(de.name, ".")==0 || strcmp(de.name, "..")==0) continue;
        ent_t *e = &a->ents[a->nent];
        strlcpy(e->name, de.name, sizeof(e->name));
        e->is_dir = false; e->size = 0;
        char full[PATH_MAX]; path_join(full, sizeof(full), a->cwd, de.name);
        /* detectar dir intentando abrirlo */
        dirhandle *td = NULL;
        if (fs_open_dir(full, &td) == 0) { e->is_dir = true; fs_close_dir(td); }
        else {
            filehandle *fh = NULL;
            if (fs_open_file(full, &fh) == 0) {
                struct file_stat st;
                if (fs_stat_file(fh, &st) == 0) e->size = st.size;
                fs_close_file(fh);
            }
        }
        a->nent++;
    }
    fs_close_dir(dh);
    /* ordenar: dirs primero, luego nombre simple */
    for (int i=0;i<a->nent;i++) for (int j=i+1;j<a->nent;j++) {
        bool sw = false;
        if (a->ents[i].is_dir != a->ents[j].is_dir) sw = (!a->ents[i].is_dir && a->ents[j].is_dir);
        else sw = (strcmp(a->ents[i].name, a->ents[j].name) > 0);
        if (sw) { ent_t t=a->ents[i]; a->ents[i]=a->ents[j]; a->ents[j]=t; }
    }
}

static void go_up(app_t *a) {
    if (strcmp(a->cwd, "/sd") == 0 || strcmp(a->cwd, "/") == 0) return;
    char *sl = xstrrchr(a->cwd, '/');
    if (sl == a->cwd) a->cwd[1] = 0;
    else if (sl) *sl = 0;
    load_dir(a);
}

static int do_copy_file(const char *src, const char *dst) {
    filehandle *in=NULL, *out=NULL;
    if (fs_open_file(src, &in) < 0) return -1;
    fs_remove_file(dst);
    if (fs_create_file(dst, &out, 0) < 0) { fs_close_file(in); return -1; }
    uint8_t buf[512]; ssize_t r; off_t off=0;
    int ok=0;
    while ((r = fs_read_file(in, buf, off, sizeof(buf))) > 0) {
        ssize_t w = fs_write_file(out, buf, off, (size_t)r);
        if (w != r) { ok=-1; break; }
        off += r;
    }
    fs_close_file(in);
    fs_truncate_file(out, (uint64_t)off);
    fs_close_file(out);
    return ok;
}

static void do_paste(app_t *a) {
    if (a->clip_path[0] == 0) { set_msg(a, "Nada para pegar"); return; }
    char dst[PATH_MAX];
    path_join(dst, sizeof(dst), a->cwd, basename_s(a->clip_path));
    if (strcmp(dst, a->clip_path) == 0) { set_msg(a, "Mismo destino"); return; }
    /* solo copiar archivos por simplicidad; mover = copiar+borrar */
    dirhandle *td=NULL;
    if (fs_open_dir(a->clip_path, &td)==0) { fs_close_dir(td); set_msg(a, "Copiar dir no soportado"); return; }
    if (do_copy_file(a->clip_path, dst) == 0) {
        if (a->clip_cut) { fs_remove_file(a->clip_path); a->clip_path[0]=0; a->clip_cut=0; }
        set_msg(a, "Pegado OK");
        load_dir(a);
    } else set_msg(a, "Error al pegar");
}

static void do_delete(app_t *a) {
    if (a->sel < 0 || a->sel >= a->nent) return;
    char full[PATH_MAX]; path_join(full,sizeof(full),a->cwd,a->ents[a->sel].name);
    int rc;
    if (a->ents[a->sel].is_dir) rc = fs_remove_dir(full);
    else rc = fs_remove_file(full);
    if (rc == 0) { load_dir(a); a->scr = SCR_BROWSE; }
    else { set_msg(a, "No se pudo borrar (vacio?)"); }
}

static void draw_header(app_t *a, const char *title) {
    gfx_fillrect(a->s,0,0,W,56,0x0016);
    gfx_fillrect(a->s,0,54,W,2,0x07FF);
    ui_draw_text(a->s,8,6,title,0xFFFF);
    ui_draw_text_clip(a->s,8,26,W-16,a->cwd,0x9CD3);
    gfx_fillrect(a->s,0,H-24,W,24,0x0016);
    ui_draw_text(a->s,6,H-18,"^ borde inf = atras/bg",0x8410);
}

static void draw_browse(app_t *a) {
    gfx_fillrect(a->s,0,0,W,H,0x1082);
    draw_header(a, "Archivos");
    int y0=60, ih=34, vis=(H-24-y0-44)/ih;
    if (a->sel < a->scroll) a->scroll = a->sel;
    if (a->sel >= a->scroll + vis) a->scroll = a->sel - vis + 1;
    if (a->scroll < 0) a->scroll = 0;
    for (int i=0;i<vis && a->scroll+i < a->nent; i++) {
        int idx = a->scroll + i;
        int y = y0 + i*ih;
        bool sel = (idx==a->sel);
        gfx_fillrect(a->s,6,y,W-12,ih-2, sel?0x2945:0x18C3);
        if (sel) ui_draw_rect_outline(a->s,6,y,W-12,ih-2,0x07FF);
        ent_t *e = &a->ents[idx];
        ui_draw_text(a->s,10,y+10, e->is_dir?"[D]":"[F]", e->is_dir?0xFFE0:0x07E0);
        ui_draw_text_clip(a->s,46,y+10,W-120,e->name,0xFFFF);
        if (!e->is_dir) {
            char sz[16]; snprintf(sz,sizeof(sz),"%lu",(unsigned long)e->size);
            int tw=ui_text_width(sz); ui_draw_text(a->s, W-14-tw, y+10, sz, 0x9CD3);
        }
    }
    if (a->nent==0) ui_draw_text(a->s,20,120,"(vacio)",0x9CD3);
    /* botones abajo */
    int by=H-24-40; int bw=(W-20)/4;
    const char *bl[4]={"Arriba","Menu","Mkdir","Pegar"};
    for(int i=0;i<4;i++){
        gfx_fillrect(a->s,6+i*bw,by,bw-4,34,0x2945);
        ui_draw_text(a->s,10+i*bw,by+11,bl[i],0xFFFF);
    }
    gfx_fillrect(a->s,0,H-24,W,24,0x0016);
    ui_draw_text(a->s,6,H-18,"^ borde inf = salir/bg",0x8410);
}

static void draw_menu(app_t *a) {
    gfx_fillrect(a->s,0,0,W,H,0x1082);
    draw_header(a, "Acciones");
    if (a->sel>=0 && a->sel<a->nent)
        ui_draw_text_clip(a->s,10,66,W-20,a->ents[a->sel].name,0xFFE0);
    const char *opts[]={"Abrir","Info","Renombrar","Copiar","Cortar","Borrar","Volver"};
    int n=7; int y=96;
    for(int i=0;i<n;i++){
        bool sel=(i==a->menu_sel);
        gfx_fillrect(a->s,20,y,W-40,40, sel?0x07FF:0x2945);
        ui_draw_text(a->s,30,y+14,opts[i], sel?0x0000:0xFFFF);
        y+=46;
    }
}

static void draw_info(app_t *a) {
    gfx_fillrect(a->s,0,0,W,H,0x1082);
    draw_header(a, "Info");
    if (a->sel>=0 && a->sel<a->nent) {
        ent_t *e=&a->ents[a->sel];
        char line[80];
        ui_draw_text(a->s,12,70,"Nombre:",0x07FF); ui_draw_text_clip(a->s,12,88,W-24,e->name,0xFFFF);
        snprintf(line,sizeof(line),"Tipo: %s", e->is_dir?"Directorio":"Archivo");
        ui_draw_text(a->s,12,116,line,0xFFFF);
        snprintf(line,sizeof(line),"Tamano: %lu bytes",(unsigned long)e->size);
        ui_draw_text(a->s,12,136,line,0xFFFF);
        char full[PATH_MAX]; path_join(full,sizeof(full),a->cwd,e->name);
        ui_draw_text(a->s,12,164,"Ruta:",0x07FF);
        /* mostrar ruta en lineas */
        ui_draw_text_clip(a->s,12,182,W-24,full,0x9CD3);
    }
    ui_draw_text(a->s,12,240,"^ atras para volver",0x8410);
}

static void draw_confirm(app_t *a) {
    gfx_fillrect(a->s,0,0,W,H,0x1082);
    draw_header(a, "Confirmar");
    ui_draw_text_clip(a->s,20,120,W-40,a->confirm_msg,0xFFFF);
    gfx_fillrect(a->s,40,220,100,44,0x07E0); ui_draw_text(a->s,70,236,"Si",0x0000);
    gfx_fillrect(a->s,180,220,100,44,0xF800); ui_draw_text(a->s,210,236,"No",0xFFFF);
}

static void draw_msg(app_t *a) {
    gfx_fillrect(a->s,0,0,W,H,0x1082);
    draw_header(a, "Mensaje");
    ui_draw_text_clip(a->s,20,140,W-40,a->msg,0xFFFF);
    ui_draw_text(a->s,20,200,"Toca para continuar",0x07FF);
}

static void draw(app_t *a) {
    switch(a->scr){
        case SCR_BROWSE: draw_browse(a); break;
        case SCR_MENU: draw_menu(a); break;
        case SCR_INFO: draw_info(a); break;
        case SCR_CONFIRM: draw_confirm(a); break;
        case SCR_INPUT:
            gfx_fillrect(a->s,0,0,W,H,0x1082);
            ui_keyboard_draw(a->s,&a->kb,W,H);
            gfx_fillrect(a->s,0,H-24,W,24,0x0016);
            ui_draw_text(a->s,6,H-18,"^ borde inf = atras",0x8410);
            break;
        case SCR_MSG: draw_msg(a); break;
    }
}

static void present(app_t *a){ arch_clean_cache_range((unsigned long)a->fb,FB_BYTES); fbcon_p4_present(a->fb); }

static void menu_action(app_t *a) {
    if (a->sel<0||a->sel>=a->nent){ a->scr=SCR_BROWSE; return; }
    ent_t *e=&a->ents[a->sel];
    char full[PATH_MAX]; path_join(full,sizeof(full),a->cwd,e->name);
    switch(a->menu_sel){
        case 0: /* abrir */
            if(e->is_dir){ strlcpy(a->cwd,full,sizeof(a->cwd)); load_dir(a); a->scr=SCR_BROWSE; }
            else { a->scr=SCR_INFO; }
            break;
        case 1: a->scr=SCR_INFO; break;
        case 2: /* renombrar */
            strlcpy(a->input_target,e->name,sizeof(a->input_target));
            ui_keyboard_init(&a->kb,"Renombrar:", e->name, false);
            a->in_act=ACT_RENAME; a->scr=SCR_INPUT; break;
        case 3: strlcpy(a->clip_path,full,sizeof(a->clip_path)); a->clip_cut=0; set_msg(a,"Copiado (portapapeles)"); break;
        case 4: strlcpy(a->clip_path,full,sizeof(a->clip_path)); a->clip_cut=1; set_msg(a,"Cortado (portapapeles)"); break;
        case 5:
            snprintf(a->confirm_msg,sizeof(a->confirm_msg),"Borrar '%s'?", e->name);
            a->scr=SCR_CONFIRM; break;
        default: a->scr=SCR_BROWSE; break;
    }
}

static void apply_input(app_t *a) {
    if (a->in_act==ACT_MKDIR) {
        if (a->kb.len>0) {
            char full[PATH_MAX]; path_join(full,sizeof(full),a->cwd,a->kb.buf);
            if (fs_make_dir(full)==0) { load_dir(a); a->scr=SCR_BROWSE; }
            else set_msg(a,"Error creando dir");
        } else a->scr=SCR_BROWSE;
    } else if (a->in_act==ACT_RENAME) {
        if (a->kb.len>0) {
            char src[PATH_MAX], dst[PATH_MAX];
            path_join(src,sizeof(src),a->cwd,a->input_target);
            path_join(dst,sizeof(dst),a->cwd,a->kb.buf);
            /* rename: si es archivo intentar copiar+borrar; fs no tiene rename API expuesta */
            dirhandle *td=NULL;
            if (fs_open_dir(src,&td)==0){ fs_close_dir(td); set_msg(a,"Renombrar dir no soportado"); }
            else {
                if (do_copy_file(src,dst)==0){ fs_remove_file(src); load_dir(a); a->scr=SCR_BROWSE; }
                else set_msg(a,"Error al renombrar");
            }
        } else a->scr=SCR_BROWSE;
    } else a->scr=SCR_BROWSE;
    a->in_act=ACT_NONE;
}

int main(int argc,char **argv){
    app_t a; memset(&a,0,sizeof(a));
    a.slot=ui_app_get_slot(argc,argv,"Archivos",0x07E0);
    a.fb=(uint16_t*)psram_malloc(FB_BYTES);
    if(!a.fb) return -1;
    a.s=gfx_create_surface(a.fb,W,H,W,GFX_FORMAT_RGB_565);
    if(!a.s){psram_free(a.fb);return -1;}
    strlcpy(a.cwd,"/sd",sizeof(a.cwd));
    a.scr=SCR_BROWSE;
    load_dir(&a);
    for(;;){
        if(appmgr_get_state(a.slot)==APP_RUNNING){
            fbcon_p4_suspend(); p4_touch_init(); ui_gesture_init(&a.gest,W,H);
            int dirty=1;
            while(appmgr_get_state(a.slot)==APP_RUNNING){
                if(dirty){ draw(&a); present(&a); dirty=0; }
                ui_evt_t ev;
                while(ui_gesture_poll(&a.gest,&ev)){
                    /* nav sistema: atras o background segun pantalla */
                    if(ev.type==UI_EVT_SWIPE && ev.edge==UI_EDGE_BOTTOM && ev.dir==UI_DIR_UP){
                        if(a.scr==SCR_BROWSE){
                            appmgr_suspend_background(a.slot,a.fb,W,H,64,96); goto suspended;
                        } else {
                            /* atras: volver a browse o pantalla previa */
                            a.scr=SCR_BROWSE; a.in_act=ACT_NONE; dirty=1; continue;
                        }
                    }
                    if(ev.type==UI_EVT_TAP){
                        int tx=ev.x1, ty=ev.y1;
                        if(a.scr==SCR_BROWSE){
                            int y0=60, ih=34, vis=(H-24-y0-44)/ih;
                            if(ty>=y0 && ty<y0+vis*ih){
                                int idx=a.scroll + (ty-y0)/ih;
                                if(idx>=0 && idx<a.nent){
                                    if(a.sel==idx){ a.menu_sel=0; a.scr=SCR_MENU; }
                                    else a.sel=idx;
                                    dirty=1; continue;
                                }
                            }
                            int by=H-24-40; int bw=(W-20)/4;
                            if(ty>=by && ty<by+34){
                                int bi=(tx-6)/bw;
                                if(bi==0){ go_up(&a); }
                                else if(bi==1 && a.nent>0){ a.menu_sel=0; a.scr=SCR_MENU; }
                                else if(bi==2){ ui_keyboard_init(&a.kb,"Nueva carpeta:","",false); a.in_act=ACT_MKDIR; a.scr=SCR_INPUT; }
                                else if(bi==3){ do_paste(&a); }
                                dirty=1; continue;
                            }
                        } else if(a.scr==SCR_MENU){
                            int y=96; for(int i=0;i<7;i++){
                                if(tx>=20&&tx<=W-20 && ty>=y && ty<y+40){ a.menu_sel=i; menu_action(&a); dirty=1; break; }
                                y+=46;
                            }
                        } else if(a.scr==SCR_INFO){
                            /* cualquier tap o swipe atras vuelve; swipe ya manejado */
                            a.scr=SCR_BROWSE; dirty=1;
                        } else if(a.scr==SCR_CONFIRM){
                            if(tx>=40&&tx<=140&&ty>=220&&ty<=264){ do_delete(&a); dirty=1; }
                            else if(tx>=180&&tx<=280&&ty>=220&&ty<=264){ a.scr=SCR_BROWSE; dirty=1; }
                        } else if(a.scr==SCR_INPUT){
                            if(ui_keyboard_tap(&a.kb,tx,ty,W,H)){
                                if(a.kb.cancel){ a.scr=SCR_BROWSE; a.in_act=ACT_NONE; }
                                else if(a.kb.done){ apply_input(&a); }
                                dirty=1;
                            }
                        } else if(a.scr==SCR_MSG){
                            a.scr=SCR_BROWSE; dirty=1;
                        }
                    }
                }
                thread_sleep(30);
            }
        }
suspended:
        if(ui_app_should_close(a.slot)) break;
        int ns=appmgr_wait_resume(a.slot);
        if(ns==APP_CLOSING||ns==APP_FREE||ns==APP_EXITED) break;
    }
    gfx_surface_destroy(a.s); psram_free(a.fb); appmgr_mark_exited(a.slot); return 0;
}
