#include "../lib/lkui.h"

#define W 320
#define H 480
#define FB_PIXELS (W*H)
#define FB_BYTES  (FB_PIXELS*2)

#define MAX_NETS 24

typedef enum { SCR_MAIN, SCR_WIFI, SCR_PASS, SCR_CONNECT, SCR_MSG } scr_t;

typedef struct {
    uint16_t *fb; gfx_surface *s; ui_gesture_t gest; int slot;
    scr_t scr;
    int main_sel;
    /* wifi */
    wifi_scan_result_t nets[MAX_NETS];
    int nnets;
    int net_sel;
    int net_scroll;
    int scanning;
    char scan_msg[48];
    /* pass */
    ui_keyboard_t kb;
    char sel_ssid[33];
    /* msg */
    char msg[96];
    char msg2[96];
    /* connect state */
    int conn_state; /* 0 idle,1 working,2 ok,3 fail */
    char conn_ssid[33];
    char conn_pass[65];
} app_t;

static void draw_header(app_t *a, const char *title, const char *sub) {
    gfx_fillrect(a->s,0,0,W,56,0x0016);
    gfx_fillrect(a->s,0,54,W,2,0x07FF);
    ui_draw_text(a->s,8,6,title,0xFFFF);
    if(sub) ui_draw_text_clip(a->s,8,26,W-16,sub,0x9CD3);
    gfx_fillrect(a->s,0,H-24,W,24,0x0016);
    ui_draw_text(a->s,6,H-18,"^ borde inf = atras/bg",0x8410);
}

static void save_wifi_config(const char *ssid, const char *pass) {
    /* genera kernel.json con la red seleccionada primero */
    char buf[1024];
    int n = snprintf(buf, sizeof(buf),
        "{\n"
        "  \"hostname\": \"lk-p4\",\n"
        "  \"httpd_port\": 80,\n"
        "  \"telnet_port\": 23,\n"
        "  \"tftp_enabled\": true,\n"
        "  \"gui\": {\n"
        "    \"autostart\": true,\n"
        "    \"apps_dir\": \"/sd/appslkx\",\n"
        "    \"launcher\": \"/sd/appslkx/launcher.elf\"\n"
        "  },\n"
        "  \"wifi\": [\n"
        "    {\"ssid\": \"%s\", \"pass\": \"%s\"}\n"
        "  ],\n"
        "  \"urls\": {\n"
        "    \"update\": \"http://192.168.1.50/fw.bin\"\n"
        "  }\n"
        "}\n", ssid, pass);
    if (n <= 0) return;
    fs_remove_file("/sd/kernel.json");
    filehandle *fh=NULL;
    if (fs_create_file("/sd/kernel.json", &fh, 0) == 0) {
        fs_write_file(fh, buf, 0, (size_t)n);
        fs_truncate_file(fh, (uint64_t)n);
        fs_close_file(fh);
    }
}

static int do_scan(app_t *a) {
    a->scanning = 1;
    strlcpy(a->scan_msg, "Escaneando...", sizeof(a->scan_msg));
    int count = 0;
    int rc = wifi_scan(a->nets, MAX_NETS, &count);
    a->nnets = count;
    if (a->nnets > MAX_NETS) a->nnets = MAX_NETS;
    a->scanning = 0;
    if (rc == 0) snprintf(a->scan_msg, sizeof(a->scan_msg), "Redes: %d", count);
    else snprintf(a->scan_msg, sizeof(a->scan_msg), "Error scan (%d)", rc);
    a->net_sel = 0; a->net_scroll = 0;
    return rc;
}

static void draw_main(app_t *a) {
    gfx_fillrect(a->s,0,0,W,H,0x1082);
    draw_header(a, "Ajustes", "Configuracion del sistema");
    const char *opts[] = {"WiFi", "Informacion"};
    for(int i=0;i<2;i++){
        int y=80+i*56;
        bool sel=(i==a->main_sel);
        gfx_fillrect(a->s,20,y,W-40,46, sel?0x07FF:0x2945);
        ui_draw_text(a->s,30,y+16,opts[i], sel?0x0000:0xFFFF);
    }
    /* estado wifi rapido */
    bool conn = esp_hosted_wifi_sta_is_connected();
    ui_draw_text(a->s,20,220, conn?"WiFi: conectado":"WiFi: no conectado", conn?0x07E0:0xFD20);
}

static void draw_wifi(app_t *a) {
    gfx_fillrect(a->s,0,0,W,H,0x1082);
    draw_header(a, "WiFi", a->scan_msg);
    /* boton escanear */
    gfx_fillrect(a->s,W-110,60,100,34,0x07E0);
    ui_draw_text(a->s,W-100,70,"Escanear",0x0000);
    bool conn = esp_hosted_wifi_sta_is_connected();
    ui_draw_text(a->s,10,68, conn?"Estado: conectado":"Estado: desconectado", conn?0x07E0:0xFD20);

    int y0=104, ih=36, vis=(H-24-y0)/ih;
    if(a->net_sel < a->net_scroll) a->net_scroll=a->net_sel;
    if(a->net_sel >= a->net_scroll+vis) a->net_scroll=a->net_sel-vis+1;
    if(a->net_scroll<0) a->net_scroll=0;
    for(int i=0;i<vis && a->net_scroll+i < a->nnets; i++){
        int idx=a->net_scroll+i;
        int y=y0+i*ih;
        bool sel=(idx==a->net_sel);
        gfx_fillrect(a->s,8,y,W-16,ih-2, sel?0x2945:0x18C3);
        if(sel) ui_draw_rect_outline(a->s,8,y,W-16,ih-2,0x07FF);
        wifi_scan_result_t *n=&a->nets[idx];
        ui_draw_text_clip(a->s,12,y+6,190,n->ssid[0]?n->ssid:"(oculta)",0xFFFF);
        char info[32]; snprintf(info,sizeof(info),"ch%u %ddBm", (unsigned)n->channel, (int)n->rssi);
        ui_draw_text(a->s,12,y+20,info,0x9CD3);
        const char *lock = (n->authmode==0)?"abierta":"segura";
        ui_draw_text(a->s,W-70,y+12,lock, n->authmode?0xFD20:0x07E0);
    }
    if(a->nnets==0 && !a->scanning) ui_draw_text(a->s,20,150,"Toca Escanear",0x9CD3);
}

static void draw_connect(app_t *a) {
    gfx_fillrect(a->s,0,0,W,H,0x1082);
    draw_header(a, "WiFi", "Conexion");
    ui_draw_text(a->s,20,100,"Red:",0x9CD3);
    ui_draw_text_clip(a->s,20,120,W-40,a->conn_ssid,0xFFFF);
    if(a->conn_state==1){
        ui_draw_text(a->s,20,170,"Conectando...",0xFD20);
    } else if(a->conn_state==2){
        ui_draw_text(a->s,20,170,"Conectado!",0x07E0);
        ui_draw_text(a->s,20,200,"Config guardada en",0x9CD3);
        ui_draw_text(a->s,20,218,"/sd/kernel.json",0x9CD3);
        ui_draw_text(a->s,20,260,"Toca para volver",0x07FF);
    } else if(a->conn_state==3){
        ui_draw_text(a->s,20,170,"No se pudo conectar",0xF800);
        ui_draw_text(a->s,20,200,"Verifica clave/senal",0x9CD3);
        ui_draw_text(a->s,20,240,"Toca para volver",0x07FF);
    }
}

static void draw_msg(app_t *a) {
    gfx_fillrect(a->s,0,0,W,H,0x1082);
    draw_header(a, "Ajustes", NULL);
    ui_draw_text_clip(a->s,20,120,W-40,a->msg,0xFFFF);
    ui_draw_text_clip(a->s,20,140,W-40,a->msg2,0x9CD3);
    ui_draw_text(a->s,20,200,"Toca para continuar",0x07FF);
}

static void draw_info(app_t *a) {
    gfx_fillrect(a->s,0,0,W,H,0x1082);
    draw_header(a, "Informacion", "Sistema");
    ui_draw_text(a->s,20,80,"LK en ESP32-P4",0xFFFF);
    ui_draw_text(a->s,20,104,"Launcher ELF + apps",0x9CD3);
    ui_draw_text(a->s,20,128,"Apps: /sd/appslkx",0x9CD3);
    bool conn = esp_hosted_wifi_sta_is_connected();
    ui_draw_text(a->s,20,160, conn?"WiFi: conectado":"WiFi: no conectado", conn?0x07E0:0xFD20);
    ui_draw_text(a->s,20,210,"^ atras para volver",0x8410);
}

static void present(app_t *a){ arch_clean_cache_range((unsigned long)a->fb,FB_BYTES); fbcon_p4_present(a->fb); }

static void do_connect(app_t *a) {
    a->conn_state = 1; a->scr = SCR_CONNECT;
    /* dibujar estado */
    draw_connect(a); present(a);
    /* intentar conectar */
    wifi_attach_and_up();
    thread_sleep(200);
    int rc = wifi_join(a->conn_ssid, a->conn_pass);
    bool ok = (rc == 0) || esp_hosted_wifi_sta_is_connected();
    if (ok) {
        a->conn_state = 2;
        save_wifi_config(a->conn_ssid, a->conn_pass);
    } else {
        a->conn_state = 3;
    }
}

int main(int argc,char **argv){
    app_t a; memset(&a,0,sizeof(a));
    a.slot=ui_app_get_slot(argc,argv,"Ajustes",0x001F);
    a.fb=(uint16_t*)psram_malloc(FB_BYTES);
    if(!a.fb) return -1;
    a.s=gfx_create_surface(a.fb,W,H,W,GFX_FORMAT_RGB_565);
    if(!a.s){psram_free(a.fb);return -1;}
    a.scr=SCR_MAIN; a.main_sel=0; a.conn_state=0;
    strlcpy(a.scan_msg,"Pulsa Escanear",sizeof(a.scan_msg));

    for(;;){
        if(appmgr_get_state(a.slot)==APP_RUNNING){
            fbcon_p4_suspend(); p4_touch_init(); ui_gesture_init(&a.gest,W,H);
            int dirty=1;
            while(appmgr_get_state(a.slot)==APP_RUNNING){
                if(dirty){
                    if(a.scr==SCR_MAIN) draw_main(&a);
                    else if(a.scr==SCR_WIFI) draw_wifi(&a);
                    else if(a.scr==SCR_PASS){ gfx_fillrect(a.s,0,0,W,H,0x1082); ui_keyboard_draw(a.s,&a.kb,W,H); draw_header(&a,"Clave WiFi",a.sel_ssid); }
                    else if(a.scr==SCR_CONNECT) draw_connect(&a);
                    else if(a.scr==SCR_MSG) draw_msg(&a);
                    present(&a); dirty=0;
                }
                ui_evt_t ev;
                while(ui_gesture_poll(&a.gest,&ev)){
                    if(ev.type==UI_EVT_SWIPE && ev.edge==UI_EDGE_BOTTOM && ev.dir==UI_DIR_UP){
                        if(a.scr==SCR_MAIN){ appmgr_suspend_background(a.slot,a.fb,W,H,64,96); goto suspended; }
                        else if(a.scr==SCR_MSG || a.scr==SCR_CONNECT){ a.scr=SCR_WIFI; dirty=1; }
                        else if(a.scr==SCR_PASS){ a.scr=SCR_WIFI; dirty=1; }
                        else { a.scr=SCR_MAIN; dirty=1; }
                        continue;
                    }
                    if(ev.type==UI_EVT_TAP){
                        int tx=ev.x1, ty=ev.y1;
                        if(a.scr==SCR_MAIN){
                            if(tx>=20&&tx<=W-20){ if(ty>=80&&ty<126){ a.scr=SCR_WIFI; dirty=1; } else if(ty>=136&&ty<182){ /* info */ gfx_fillrect(a.s,0,0,W,H,0x1082); draw_info(&a); present(&a); /* esperar tap atras via gesto */ } }
                        } else if(a.scr==SCR_WIFI){
                            if(tx>=W-110&&tx<=W-10&&ty>=60&&ty<=94){ draw_header(&a,"WiFi","Escaneando..."); ui_draw_text(a.s,20,150,"Buscando redes...",0xFD20); present(&a); do_scan(&a); dirty=1; continue; }
                            int y0=104, ih=36, vis=(H-24-y0)/ih;
                            if(ty>=y0 && ty<y0+vis*ih){
                                int idx=a.net_scroll + (ty-y0)/ih;
                                if(idx>=0 && idx<a.nnets){
                                    a.net_sel=idx;
                                    strlcpy(a.sel_ssid,a.nets[idx].ssid,sizeof(a.sel_ssid));
                                    strlcpy(a.conn_ssid,a.nets[idx].ssid,sizeof(a.conn_ssid));
                                    ui_keyboard_init(&a.kb,"Clave WiFi:","",false);
                                    a.scr=SCR_PASS; dirty=1; continue;
                                }
                            }
                        } else if(a.scr==SCR_PASS){
                            if(ui_keyboard_tap(&a.kb,tx,ty,W,H)){
                                if(a.kb.cancel){ a.scr=SCR_WIFI; }
                                else if(a.kb.done){
                                    strlcpy(a.conn_pass,a.kb.buf,sizeof(a.conn_pass));
                                    do_connect(&a);
                                }
                                dirty=1;
                            }
                        } else if(a.scr==SCR_CONNECT){
                            if(a.conn_state==2 || a.conn_state==3){ a.scr=SCR_WIFI; a.conn_state=0; dirty=1; }
                        } else if(a.scr==SCR_MSG){
                            a.scr=SCR_MAIN; dirty=1;
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
