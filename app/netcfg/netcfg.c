/*
 * Configuracion del kernel en /sd/kernel.json + auto-conexion WiFi.
 *
 * Estructura del archivo (se crea uno por defecto si no existe):
 * {
 *   "hostname": "lk-p4",
 *   "httpd_port": 80,
 *   "telnet_port": 23,
 *   "tftp_enabled": true,
 *   "wifi": [
 *     {"ssid": "wifi2.4Ghz", "pass": "ac32mqn422308"}
 *   ],
 *   "urls": {
 *     "update": "http://192.168.1.50/fw.bin"
 *   }
 * }
 *
 * Comandos: `netcfg` (muestra config), `netcfg reload`, `netcfg wifi`
 * (auto-conecta), `netcfg default` (reescribe el archivo por defecto).
 */
#include <app.h>
#include <lk/err.h>
#include <lk/init.h>
#include <lk/console_cmd.h>
#include <kernel/thread.h>
#include <kernel/mutex.h>
#include <lib/minip.h>
#include <lib/minip/netif.h>
#include <lib/fs.h>
#include <lib/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CFG_PATH "/sd/kernel.json"
#define CFG_MAX 8192
#define WIFI_MAX 8
#define SSID_MAX 33
#define PASS_MAX 65
#define URL_MAX 128
#define PATH_MAX_CFG 96

typedef struct {
    char ssid[SSID_MAX];
    char pass[PASS_MAX];
} wifi_net_t;

typedef struct {
    char hostname[32];
    uint16_t httpd_port;
    uint16_t telnet_port;
    bool tftp_enabled;
    wifi_net_t wifi[WIFI_MAX];
    int wifi_count;
    char url_update[URL_MAX];
    /* Interfaz grafica / lanzador de apps */
    bool gui_autostart;
    char apps_dir[PATH_MAX_CFG];
    char launcher_path[PATH_MAX_CFG];
    bool loaded;
} netcfg_t;

static netcfg_t cfg;
static mutex_t cfg_lock = MUTEX_INITIAL_VALUE(cfg_lock);
static char cfg_buf[CFG_MAX];
static jsmntok_t cfg_toks[256];

/* Declaraciones del driver WiFi (evitan acoplar modulo). */
extern void *wifi_netif_get(void);
int wifi_join(const char *ssid, const char *pass);
extern bool esp_hosted_wifi_sta_is_connected(void);

static const char default_json[] =
    "{\n"
    "  \"hostname\": \"lk-p4\",\n"
    "  \"httpd_port\": 80,\n"
    "  \"telnet_port\": 23,\n"
    "  \"tftp_enabled\": true,\n"
    "  \"gui\": {\n"
    "    \"autostart\": false,\n"
    "    \"apps_dir\": \"/sd/appslkx\",\n"
    "    \"launcher\": \"/sd/appslkx/launcher.elf\"\n"
    "  },\n"
    "  \"wifi\": [\n"
    "    {\"ssid\": \"PhMNet_2\", \"pass\": \"TofMagSim47\"},\n"
    "    {\"ssid\": \"wifi2.4Ghz\", \"pass\": \"ac32mqn422308\"},\n"
    "    {\"ssid\": \"SOP-TECNOLOGIA\", \"pass\": \"@@Sop-2026##\"}\n"
    "  ],\n"
    "  \"urls\": {\n"
    "    \"update\": \"http://192.168.1.50/fw.bin\"\n"
    "  }\n"
    "}\n";

static void cfg_defaults(void) {
    memset(&cfg, 0, sizeof(cfg));
    strlcpy(cfg.hostname, "lk-p4", sizeof(cfg.hostname));
    cfg.httpd_port = 80;
    cfg.telnet_port = 23;
    cfg.tftp_enabled = true;
    strlcpy(cfg.url_update, "http://192.168.1.50/fw.bin", sizeof(cfg.url_update));
    /* Desactivado temporalmente para aislar la caida de red bajo GUI:
     * con la GUI corriendo, rx_dropped crece y mueren httpd/telnet/UDP.
     * Reactivar con 'gui autostart' o poniendo autostart=true en kernel.json. */
    cfg.gui_autostart = false;
    strlcpy(cfg.apps_dir, "/sd/appslkx", sizeof(cfg.apps_dir));
    strlcpy(cfg.launcher_path, "/sd/appslkx/launcher.elf", sizeof(cfg.launcher_path));
}

/* Salta un token y todo su subarbol.  Devuelve el indice del siguiente token. */
static int jsmn_skip(const jsmntok_t *toks, int idx, int ntoks) {
    if (idx >= ntoks) return ntoks;
    int j = idx + 1;
    if (toks[idx].type == JSMN_ARRAY) {
        for (int n = 0; n < toks[idx].size && j < ntoks; n++)
            j = jsmn_skip(toks, j, ntoks);
    } else if (toks[idx].type == JSMN_OBJECT) {
        for (int n = 0; n < toks[idx].size && j < ntoks; n++) {
            j = jsmn_skip(toks, j, ntoks); /* key */
            j = jsmn_skip(toks, j, ntoks); /* value */
        }
    }
    return j;
}

/* Busca una clave en un objeto.  Devuelve el token valor o NULL. */
static const jsmntok_t *find_key(const char *js, int obj_idx, int ntoks,
                                 const char *key) {
    if (obj_idx >= ntoks || cfg_toks[obj_idx].type != JSMN_OBJECT) return NULL;
    int i = obj_idx + 1;
    for (int n = 0; n < cfg_toks[obj_idx].size && i < ntoks; n++) {
        int k = i;
        int v = jsmn_skip(cfg_toks, k, ntoks);
        if (v < ntoks && jsmn_streq(js, &cfg_toks[k], key))
            return &cfg_toks[v];
        i = jsmn_skip(cfg_toks, v, ntoks);
    }
    return NULL;
}

static void parse_wifi_array(const char *js, int arr_idx, int ntoks) {
    cfg.wifi_count = 0;
    if (arr_idx >= ntoks || cfg_toks[arr_idx].type != JSMN_ARRAY) return;
    int i = arr_idx + 1;
    for (int n = 0; n < cfg_toks[arr_idx].size && i < ntoks && cfg.wifi_count < WIFI_MAX; n++) {
        if (cfg_toks[i].type == JSMN_OBJECT) {
            const jsmntok_t *ssid = find_key(js, i, ntoks, "ssid");
            const jsmntok_t *pass = find_key(js, i, ntoks, "pass");
            if (ssid && pass) {
                jsmn_strcpy(js, ssid, cfg.wifi[cfg.wifi_count].ssid, SSID_MAX);
                jsmn_strcpy(js, pass, cfg.wifi[cfg.wifi_count].pass, PASS_MAX);
                cfg.wifi_count++;
            }
        }
        i = jsmn_skip(cfg_toks, i, ntoks);
    }
}

static status_t cfg_parse(const char *js, size_t len) {
    jsmn_parser p;
    jsmn_init(&p);
    int n = jsmn_parse(&p, js, len, cfg_toks, 256);
    if (n < 1 || cfg_toks[0].type != JSMN_OBJECT) return ERR_INVALID_ARGS;

    const jsmntok_t *t;
    t = find_key(js, 0, n, "hostname");
    if (t && t->type == JSMN_STRING) jsmn_strcpy(js, t, cfg.hostname, sizeof(cfg.hostname));
    t = find_key(js, 0, n, "httpd_port");
    if (t) cfg.httpd_port = (uint16_t)jsmn_int(js, t);
    t = find_key(js, 0, n, "telnet_port");
    if (t) cfg.telnet_port = (uint16_t)jsmn_int(js, t);
    t = find_key(js, 0, n, "tftp_enabled");
    if (t) cfg.tftp_enabled = (jsmn_strcmp_true(js, t));

    t = find_key(js, 0, n, "wifi");
    if (t && t->type == JSMN_ARRAY) {
        int arr_idx = (int)(t - cfg_toks);
        parse_wifi_array(js, arr_idx, n);
    }
    t = find_key(js, 0, n, "urls");
    if (t && t->type == JSMN_OBJECT) {
        const jsmntok_t *u = find_key(js, (int)(t - cfg_toks), n, "update");
        if (u && u->type == JSMN_STRING) jsmn_strcpy(js, u, cfg.url_update, URL_MAX);
    }
    t = find_key(js, 0, n, "gui");
    if (t && t->type == JSMN_OBJECT) {
        int gidx = (int)(t - cfg_toks);
        const jsmntok_t *g = find_key(js, gidx, n, "autostart");
        if (g) cfg.gui_autostart = jsmn_strcmp_true(js, g);
        g = find_key(js, gidx, n, "apps_dir");
        if (g && g->type == JSMN_STRING) jsmn_strcpy(js, g, cfg.apps_dir, sizeof(cfg.apps_dir));
        g = find_key(js, gidx, n, "launcher");
        if (g && g->type == JSMN_STRING) jsmn_strcpy(js, g, cfg.launcher_path, sizeof(cfg.launcher_path));
    }
    cfg.loaded = true;
    return NO_ERROR;
}

/* Escribe el JSON por defecto en /sd (crear con 0 y escribir). */
static status_t write_default(void) {
    fs_remove_file(CFG_PATH);
    filehandle *fh;
    status_t st = fs_create_file(CFG_PATH, &fh, 0);
    if (st < 0) return st;
    ssize_t w = fs_write_file(fh, default_json, 0, strlen(default_json));
    fs_truncate_file(fh, (uint64_t)(w > 0 ? w : 0));
    fs_close_file(fh);
    return NO_ERROR;
}

status_t netcfg_load(void) {
    mutex_acquire(&cfg_lock);
    cfg_defaults();
    filehandle *fh;
    status_t st = fs_open_file(CFG_PATH, &fh);
    if (st < 0) {
        printf("netcfg: no hay %s, creando uno por defecto\n", CFG_PATH);
        write_default();
        mutex_release(&cfg_lock);
        return ERR_NOT_FOUND;   /* uso defaults ya cargados */
    }
    struct file_stat fst;
    size_t size = CFG_MAX - 1;
    if (fs_stat_file(fh, &fst) == NO_ERROR && fst.size < CFG_MAX) size = fst.size;
    ssize_t r = fs_read_file(fh, cfg_buf, 0, size);
    fs_close_file(fh);
    if (r <= 0) { mutex_release(&cfg_lock); return ERR_IO; }
    cfg_buf[r] = '\0';
    st = cfg_parse(cfg_buf, (size_t)r);
    if (st < 0) printf("netcfg: JSON invalido (err %d), uso defaults\n", st);
    mutex_release(&cfg_lock);
    return st;
}

netcfg_t *netcfg_get(void) { return &cfg; }

bool netcfg_gui_autostart(void) { return cfg.gui_autostart; }
const char *netcfg_apps_dir(void) { return cfg.apps_dir[0] ? cfg.apps_dir : "/sd/apps"; }
const char *netcfg_launcher_path(void) { return cfg.launcher_path; }

/* Intenta conectar con cada red guardada, en orden.  Devuelve 0 si conecto. */
static int netcfg_connect(void) {
    if (esp_hosted_wifi_sta_is_connected()) {
        printf("netcfg: wifi ya conectada\n");
        return 0;
    }
    for (int i = 0; i < cfg.wifi_count; i++) {
        printf("netcfg: probando '%s' ...\n", cfg.wifi[i].ssid);
        if (wifi_join(cfg.wifi[i].ssid, cfg.wifi[i].pass) == NO_ERROR &&
            esp_hosted_wifi_sta_is_connected()) {
            printf("netcfg: conectado a '%s'\n", cfg.wifi[i].ssid);
            return 0;
        }
    }
    printf("netcfg: ninguna red guardada conecto\n");
    return -1;
}

/* Init hook: solo carga la config.  NO lanzamos supervisor de DHCP:
 * netif_register() ya arranca DHCP una vez por interfaz; los supervisores y
 * `wifi dhcp' crean threads DHCP multiples que compiten y dejan la IP a 0
 * (medido en HW).  El autorun espera a DHCP con `netwait'. */
static void netcfg_init(uint level) {
    (void)level;
    netcfg_load();
}

LK_INIT_HOOK(netcfg, netcfg_init, LK_INIT_LEVEL_APPS);

static int cmd_netcfg(int argc, const console_cmd_args *argv) {
    if (argc < 2) {
        printf("netcfg: hostname=%s httpd=%u telnet=%u tftp=%d\n",
               cfg.hostname, cfg.httpd_port, cfg.telnet_port, cfg.tftp_enabled);
        printf("  gui autostart=%d apps_dir=%s launcher='%s'\n",
               cfg.gui_autostart, cfg.apps_dir, cfg.launcher_path);
        printf("  wifi guardadas: %d\n", cfg.wifi_count);
        for (int i = 0; i < cfg.wifi_count; i++)
            printf("    [%d] %s\n", i, cfg.wifi[i].ssid);
        printf("  url update: %s\n", cfg.url_update);
        return 0;
    }
    if (!strcmp(argv[1].str, "wait")) {
        /* Como `netup' de Xinu: pedir DHCP SOLO despues de que el join haya
         * asociado.  El auto-DHCP de netif_register se desactivo en `netcfg
         * reload', asi que aqui no hay otro thread compitiendo. */
        netif_t *n0 = (netif_t *)wifi_netif_get();
        if (n0) minip_start_dhcp(n0);
        status_t st = minip_wait_for_configured(20000);
        netif_t *n = (netif_t *)wifi_netif_get();
        if (st < 0 || !n || n->ipv4_addr == 0 || n->ipv4_addr == IPV4_NONE) {
            printf("netwait: DHCP no asigno IP (st=%d)\n", st);
            return -1;
        }
        printf("netwait: IP %u.%u.%u.%u lista\n", IPV4_SPLIT(n->ipv4_addr));
        return 0;
    } else if (!strcmp(argv[1].str, "reload")) {
        /* Desactivar DHCP automatico: se pedira en `netcfg wait', despues del
         * join (orden wifi join -> DHCP de Xinu). */
        minip_set_auto_dhcp(false);
        return netcfg_load();
    } else if (!strcmp(argv[1].str, "wifi")) {
        return netcfg_connect();
    } else if (!strcmp(argv[1].str, "default")) {
        return write_default();
    }
    printf("uso: netcfg [reload|wifi|default]\n");
    return -1;
}

STATIC_COMMAND_START
STATIC_COMMAND("netcfg", "config de red (/sd/kernel.json) y auto-wifi", &cmd_netcfg)
STATIC_COMMAND_END(netcfg);
