/*
 * Servidor HTTP minimalista sobre lib/minip para el ESP32-P4.
 *
 *  - GET /_archivo.txt  -> sirve archivos de /sd
 *  - GET /              -> pagina index embebida (o /sd/index.html si existe)
 *  - GET /api/files?path=/dir   -> lista un directorio en JSON
 *  - GET /api/download?path=/f  -> baja un archivo
 *  - PUT /api/file?path=/f      -> sube archivo (body crudo)
 *  - DELETE /api/file?path=/f   -> borra archivo
 *  - POST /api/mkdir?path=/d    -> crea directorio
 *  - POST /api/rmdir?path=/d    -> borra directorio
 *
 * Comandos: `httpd' (arranca, core0), `httpd stop', `httpd status'.
 * Diseno inspirado en xsh_httpd.c / httpapi.c de Xinu, adaptado a la API de
 * LK (lib/minip + lib/fs).  Un solo cliente a la vez, con accept con timeout
 * para no bloquear para siempre.
 */
#include <app.h>
#include <lk/err.h>
#include <lk/console_cmd.h>
#include <kernel/thread.h>
#include <kernel/mutex.h>
#include <lib/minip.h>
#include <lib/minip/netif.h>
#include <platform/esp32p4.h>
#include <lib/fs.h>

/* La IP sale de la netif WiFi (no hay minip_get_ip()).  wifi_netif_get()
 * devuelve el netif_t de wlan0; se declara aqui para no acoplar el modulo
 * al driver. */
extern void *wifi_netif_get(void);
static void httpd_print_url(void) {
    netif_t *n = (netif_t *)wifi_netif_get();
    if (n && n->ipv4_addr != IPV4_NONE) {
        printf("\n============================================================\n");
        printf(" httpd LISTO en http://%u.%u.%u.%u/  (core0)\n",
               IPV4_SPLIT(n->ipv4_addr));
        printf(" pagina de prueba: http://%u.%u.%u.%u/index.html\n",
               IPV4_SPLIT(n->ipv4_addr));
        printf(" archivos: http://%u.%u.%u.%u/api/files?path=/\n",
               IPV4_SPLIT(n->ipv4_addr));
        printf("============================================================\n\n");
    }
}
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HTTPD_PORT 80
/* 2048 bytes hacia incapaz de recibir un ELF de ~25KB: el body se truncaba y
 * el upload parecia funcionar pero escribia un fichero incompleto. */
#define HTTPD_RQMAX (64 * 1024)
#define HTTPD_STK 16384
#define HTTPD_ACCEPT_TIMEOUT 2000
#define ROOT "/sd"

static tcp_socket_t *listen_socket;
static thread_t *httpd_thread;
static bool httpd_running;
static mutex_t httpd_lock = MUTEX_INITIAL_VALUE(httpd_lock);

/* ---------- pagina por defecto ---------- */
static const char index_html[] =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<title>LK ESP32-P4</title></head><body>"
    "<h1 style='color:#0a0'>LK ESP32-P4 - httpd OK</h1>"
    "<p>Servidor HTTP sobre minip + FAT en /sd (core0).</p>"
    "<ul>"
    "<li><a href='/api/files?path=/'>Ver archivos (/sd)</a></li>"
    "<li><a href='/api/download?path=/wifi.txt'>Descargar wifi.txt</a></li>"
    "<li>Subir: <code>curl -X PUT --data-binary @f "
    "http://&lt;ip&gt;/api/file?path=/f</code></li>"
    "<li>Borrar: <code>curl -X DELETE "
    "http://&lt;ip&gt;/api/file?path=/f</code></li>"
    "</ul>"
    "<p style='color:#888'>Splash fbcon activo en la LCD.</p>"
    "</body></html>";

/* ---------- utilidades HTTP ---------- */
static const char *mime_type(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    dot++;
    if (!strcmp(dot, "html") || !strcmp(dot, "htm")) return "text/html";
    if (!strcmp(dot, "txt"))  return "text/plain";
    if (!strcmp(dot, "css"))  return "text/css";
    if (!strcmp(dot, "js"))   return "application/javascript";
    if (!strcmp(dot, "json")) return "application/json";
    if (!strcmp(dot, "bmp"))  return "image/bmp";
    if (!strcmp(dot, "png"))  return "image/png";
    if (!strcmp(dot, "jpg"))  return "image/jpeg";
    if (!strcmp(dot, "elf"))  return "application/octet-stream";
    return "application/octet-stream";
}

/* Decodifica %XX en dst (dst debe ser >= len+1). */
static void url_decode(const char *src, char *dst, size_t cap) {
    size_t j = 0;
    while (*src && j + 1 < cap) {
        if (*src == '%' && src[1] && src[2]) {
            char h[3] = { src[1], src[2], 0 };
            dst[j++] = (char)strtol(h, NULL, 16);
            src += 3;
        } else {
            dst[j++] = *src++;
        }
    }
    dst[j] = '\0';
}

/* Compone una ruta segura dentro de ROOT: /foo -> /sd/foo, sin ../ */
static void resolve_path(const char *url_path, char *out, size_t cap) {
    char decoded[256];
    url_decode(url_path, decoded, sizeof(decoded));
    /* normalizar barras y quitar '..' basico */
    if (!strcmp(decoded, "/") || decoded[0] == '\0') {
        snprintf(out, cap, "%s/index.html", ROOT);
        return;
    }
    /* rechazar '..' */
    if (strstr(decoded, "..")) { snprintf(out, cap, "%s/_invalid_", ROOT); return; }
    const char *p = decoded;
    if (p[0] == '/') p++;
    snprintf(out, cap, "%s/%s", ROOT, p);
}

static int send_resp(tcp_socket_t *s, int code, const char *reason,
                     const char *ctype, const void *body, size_t len) {
    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %u\r\n"
                      "Connection: close\r\n\r\n",
                      code, reason, ctype, (unsigned)len);
    if (tcp_write(s, hdr, hn) != hn) return -1;
    if (len && tcp_write(s, body, len) != (ssize_t)len) return -1;
    return 0;
}

static int send_err(tcp_socket_t *s, int code, const char *reason, const char *msg) {
    return send_resp(s, code, reason, "text/plain", msg, strlen(msg));
}

/* ---------- API REST ---------- */
static int api_files(tcp_socket_t *s, const char *qpath) {
    char dir[256];
    resolve_path(qpath && qpath[0] ? qpath : "/", dir, sizeof(dir));
    /* quitar index.html si vino de "/" -> queremos el directorio */
    char *slash = strrchr(dir, '/');
    if (slash && !strcmp(slash, "/index.html")) *slash = '\0';

    dirhandle *dh;
    if (fs_open_dir(dir, &dh) < 0)
        return send_err(s, 404, "Not Found", "dir not found\n");

    char out[2048];
    int n = snprintf(out, sizeof(out), "[\n");
    struct dirent ent;
    bool first = true;
    while (fs_read_dir(dh, &ent) == NO_ERROR && ent.name[0]) {
        int k = snprintf(out + n, sizeof(out) - n,
                         "%s  {\"name\":\"%s\"}", first ? "" : ",\n", ent.name);
        if (k <= 0 || n + k >= (int)sizeof(out)) break;
        n += k;
        first = false;
    }
    fs_close_dir(dh);
    n += snprintf(out + n, sizeof(out) - n, "\n]\n");
    return send_resp(s, 200, "OK", "application/json", out, n);
}

static int api_download(tcp_socket_t *s, const char *qpath) {
    if (!qpath || !qpath[0]) return send_err(s, 400, "Bad Request", "missing path\n");
    char path[256];
    resolve_path(qpath, path, sizeof(path));
    filehandle *fh;
    if (fs_open_file(path, &fh) < 0)
        return send_err(s, 404, "Not Found", "file not found\n");

    struct file_stat st;
    size_t size = 0;
    if (fs_stat_file(fh, &st) == NO_ERROR) size = st.size;
    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
                      "Content-Length: %u\r\nConnection: close\r\n\r\n",
                      mime_type(path), (unsigned)size);
    tcp_write(s, hdr, hn);

    char buf[512];
    off_t off = 0;
    ssize_t r;
    while ((r = fs_read_file(fh, buf, off, sizeof(buf))) > 0) {
        if (tcp_write(s, buf, r) != r) break;
        off += r;
    }
    fs_close_file(fh);
    return 0;
}

static int api_put(tcp_socket_t *s, const char *qpath, const char *body, size_t bodylen) {
    if (!qpath || !qpath[0]) return send_err(s, 400, "Bad Request", "missing path\n");
    char path[256];
    resolve_path(qpath, path, sizeof(path));
    /* FAT de este puerto no trunca: borrar primero si existe.  Crear con
     * longitud 0 y dejar que fs_write_file haga el truncate internamente. */
    fs_remove_file(path);
    filehandle *fh;
    status_t st = fs_create_file(path, &fh, 0);
    if (st < 0) {
        return send_err(s, 500, "Internal Error", "cannot create\n");
    }
    size_t off = 0;
    while (off < bodylen) {
        ssize_t w = fs_write_file(fh, body + off, off, bodylen - off);
        if (w <= 0) break;
        off += w;
    }
    fs_truncate_file(fh, off);
    fs_close_file(fh);
    return send_resp(s, 201, "Created", "text/plain", "ok\n", 3);
}

static int api_delete(tcp_socket_t *s, const char *qpath) {
    if (!qpath || !qpath[0]) return send_err(s, 400, "Bad Request", "missing path\n");
    char path[256];
    resolve_path(qpath, path, sizeof(path));
    if (fs_remove_file(path) < 0)
        return send_err(s, 404, "Not Found", "cannot delete\n");
    return send_resp(s, 200, "OK", "text/plain", "deleted\n", 8);
}

static int api_mkdir(tcp_socket_t *s, const char *qpath, bool rm) {
    if (!qpath || !qpath[0]) return send_err(s, 400, "Bad Request", "missing path\n");
    char path[256];
    resolve_path(qpath, path, sizeof(path));
    status_t st = rm ? fs_remove_dir(path) : fs_make_dir(path);
    if (st < 0) return send_err(s, 500, "Internal Error", "mkdir/rmdir failed\n");
    return send_resp(s, 200, "OK", "text/plain", "ok\n", 3);
}

/* ---------- request ---------- */
struct request {
    char method[8];
    char path[256];
    char query[256];
};

static int parse_request(char *rq, struct request *r) {
    char *sp1 = strchr(rq, ' ');
    if (!sp1) return -1;
    *sp1 = '\0';
    char *url = sp1 + 1;
    char *sp2 = strchr(url, ' ');
    if (!sp2) return -1;
    *sp2 = '\0';
    strlcpy(r->method, rq, sizeof(r->method));
    r->query[0] = '\0';
    char *q = strchr(url, '?');
    if (q) {
        *q = '\0';
        strlcpy(r->query, q + 1, sizeof(r->query));
    }
    strlcpy(r->path, url, sizeof(r->path));
    return 0;
}

/* Extrae el valor del parametro 'name' de una query string. */
static const char *query_value(const char *query, const char *name) {
    static char val[256];
    char tmp[512];
    strlcpy(tmp, query, sizeof(tmp));
    char *tok = strtok(tmp, "&");
    size_t nlen = strlen(name);
    while (tok) {
        if (!strncmp(tok, name, nlen) && tok[nlen] == '=') {
            url_decode(tok + nlen + 1, val, sizeof(val));
            return val;
        }
        tok = strtok(NULL, "&");
    }
    val[0] = '\0';
    return val;
}

static int serve_static(tcp_socket_t *s, const char *path) {
    char full[256];
    resolve_path(path, full, sizeof(full));
    /* "/" e "/index.html" -> SIEMPRE la pagina embebida (pequena).  El
     * index.html de la SD puede ser enorme y por SPI PIO el navegador hace
     * timeout; se sirve bajo /api/download si se quiere. */
    if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
        return send_resp(s, 200, "OK", "text/html", index_html, strlen(index_html));
    }
    filehandle *fh;
    if (fs_open_file(full, &fh) < 0)
        return send_err(s, 404, "Not Found", "404 Not Found\n");
    struct file_stat st;
    size_t size = 0;
    if (fs_stat_file(fh, &st) == NO_ERROR) size = st.size;
    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
                      "Content-Length: %u\r\nConnection: close\r\n\r\n",
                      mime_type(full), (unsigned)size);
    tcp_write(s, hdr, hn);
    char buf[512];
    off_t off = 0;
    ssize_t r;
    while ((r = fs_read_file(fh, buf, off, sizeof(buf))) > 0) {
        if (tcp_write(s, buf, r) != r) break;
        off += r;
    }
    fs_close_file(fh);
    return 0;
}

static void handle_client(tcp_socket_t *s) {
    static char rq[HTTPD_RQMAX];
    ssize_t n = tcp_read(s, rq, sizeof(rq) - 1);
    if (n <= 0) { tcp_close(s); return; }
    rq[n] = '\0';

    /* curl/envian headers y body en segmentos distintos: si el request es un
     * PUT con Content-Length, seguimos leyendo hasta tener todo el body. */
    /* Calcular body ANTES de parse_request (que pone \0 en rq). */
    char *hend0 = strstr(rq, "\r\n\r\n");
    size_t hlen0 = hend0 ? (size_t)(hend0 + 4 - rq) : 0;
    const char *body_saved = hend0 ? hend0 + 4 : "";
    if (hend0) {
        char *cl = strstr(rq, "Content-Length:");
        if (cl) {
            cl += 15;
            while (*cl == ' ') cl++;
            long need = strtol(cl, NULL, 10);
            size_t have = (size_t)n - hlen0;
            while (need > 0 && (long)have < need && (size_t)n < sizeof(rq) - 1) {
                ssize_t m = tcp_read(s, rq + n, sizeof(rq) - 1 - n);
                if (m <= 0) break;
                n += m;
                rq[n] = '\0';
                have = (size_t)n - hlen0;
            }
        }
    }

    struct request r;
    if (parse_request(rq, &r) < 0) {
        send_err(s, 400, "Bad Request", "bad request\n");
        tcp_close(s);
        return;
    }

    /* body calculado antes de parse_request (rq ya fue modificado).
     * IMPORTANTE: usar 'n' (actualizado tras leer el body completo), no el
     * tamano del primer segmento, o el upload se truncaba. */
    const char *body = body_saved;
    size_t bodylen = hend0 ? (size_t)n - hlen0 : 0;

    if (!strcmp(r.path, "/api/files") && !strcmp(r.method, "GET")) {
        api_files(s, query_value(r.query, "path"));
    } else if (!strcmp(r.path, "/api/download") && !strcmp(r.method, "GET")) {
        api_download(s, query_value(r.query, "path"));
    } else if (!strcmp(r.path, "/api/file") && !strcmp(r.method, "PUT")) {
        api_put(s, query_value(r.query, "path"), body, bodylen);
    } else if (!strcmp(r.path, "/api/file") && !strcmp(r.method, "DELETE")) {
        api_delete(s, query_value(r.query, "path"));
    } else if (!strcmp(r.path, "/api/mkdir") && !strcmp(r.method, "POST")) {
        api_mkdir(s, query_value(r.query, "path"), false);
    } else if (!strcmp(r.path, "/api/rmdir") && !strcmp(r.method, "POST")) {
        api_mkdir(s, query_value(r.query, "path"), true);
    } else if (!strcmp(r.method, "GET")) {
        serve_static(s, r.path);
    } else {
        send_err(s, 405, "Method Not Allowed", "405\n");
    }
    tcp_close(s);
}

static int httpd_entry(void *arg) {
    (void)arg;
    /* Esperar a que DHCP haya asignado IP (hasta 15s) antes de anunciar la
     * URL.  El listener ya esta abierto; solo es para el printf informativo. */
    minip_wait_for_configured(15000);
    httpd_print_url();

    tcp_socket_t *client;
    while (httpd_running) {
        status_t st = tcp_accept_timeout(listen_socket, &client, HTTPD_ACCEPT_TIMEOUT);
        if (st != NO_ERROR) continue;
        handle_client(client);
    }
    return 0;
}

static int cmd_httpd(int argc, const console_cmd_args *argv) {
    mutex_acquire(&httpd_lock);
    if (argc < 2 || !strcmp(argv[1].str, "start")) {
        if (httpd_running) {
            printf("httpd: ya esta corriendo\n");
        } else if (tcp_open_listen(&listen_socket, HTTPD_PORT) == NO_ERROR) {
            httpd_running = true;
            httpd_thread = thread_create("httpd", httpd_entry, NULL,
                                         DEFAULT_PRIORITY, HTTPD_STK);
            if (httpd_thread) {
                /* El stack de minip corre en core0; el socket/event del accept
                 * no despierta bien desde otro core, asi que el httpd va en
                 * core0 (medido en HW). */
                thread_set_pinned_cpu(httpd_thread, 0);
                thread_resume(httpd_thread);
                printf("httpd: escuchando en puerto %d (core0)\n", HTTPD_PORT);
            } else {
                httpd_running = false;
                tcp_close(listen_socket);
            }
        } else {
            printf("httpd: no se pudo abrir puerto %d\n", HTTPD_PORT);
        }
    } else if (!strcmp(argv[1].str, "stop")) {
        httpd_running = false;
        if (listen_socket) { tcp_close(listen_socket); listen_socket = NULL; }
        printf("httpd: detenido\n");
    } else if (!strcmp(argv[1].str, "status")) {
        printf("httpd: %s en puerto %d\n", httpd_running ? "activo" : "detenido", HTTPD_PORT);
    } else {
        printf("uso: httpd [start|stop|status]\n");
    }
    mutex_release(&httpd_lock);
    return 0;
}

STATIC_COMMAND_START
STATIC_COMMAND("httpd", "servidor HTTP sobre minip (start|stop|status)", &cmd_httpd)
STATIC_COMMAND_END(httpd);
