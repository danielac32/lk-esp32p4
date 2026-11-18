/*
 * Servidor UDP de diagnostico y subida de firmware a SD para LK/ESP32-P4.
 *
 * Adaptado a la API de LK (minip udp_listen/udp_open/udp_send + lib/fs),
 * inspirado en xsh_udpserver.c de Xinu pero sin descriptores/devices de Xinu
 * ni APIs FAT/red propias de Xinu. Corre en el contexto del stack de red
 * (core0), como tftpd: los handlers no deben bloquearse/dormir.
 *
 * Comandos de texto (respuesta ASCII, CRLF):
 *   PING                      -> PONG <uptime_ms>
 *   INFO                      -> resumen (version, uptime, red, hostname)
 *   NET                       -> estado de red (ip/mask/gateway/dns/mac)
 *   UPTIME                    -> uptime en ms
 *   SD [INFO]                 -> espacio de /sd (fs_stat_fs)
 *   FW_STATUS                 -> estado de la subida / imagen en SD
 *   FW_BEGIN <size> [path]    -> inicia subida a <path> (def: /sd/fw.bin)
 *   FW_END                    -> cierra y verifica; deja imagen en SD
 *   FW_ABORT                  -> cancela subida
 *   HELP                      -> lista de comandos
 *
 * Subida binaria (misma semantica que Xinu):
 *   "FW_DATA <seq> <len>\n" seguido de exactamente <len> bytes en el mismo
 *   datagrama UDP. seq consecutivo desde 0; retransmisiones se confirman sin
 *   reescribir. Se escribe a un temporal y al FW_END se renombra/copia al
 *   destino. El "apply" real a flash no esta implementado en este puerto: la
 *   imagen queda en la SD.
 *
 * Consola: udpserver [port] | udpserver stop | udpserver status
 * Puerto por defecto: 5000.
 */
#include <app.h>
#include <lk/console_cmd.h>
#include <lk/debug.h>
#include <lk/err.h>
#include <kernel/mutex.h>
#include <kernel/thread.h>
#include <lib/fs.h>
#include <lib/minip.h>
#include <lib/minip/netif.h>
#include <platform.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define UDPSRV_DEFAULT_PORT 5000
#define UDPSRV_REPLY_MAX    1024
#define UDPSRV_LINE_MAX     256
#define FW_BLOCK_SIZE       960
#define FW_MAX_IMAGE        (16u * 1024u * 1024u)
#define FW_DEFAULT_PATH     "/sd/fw.bin"

extern void *wifi_netif_get(void);

static bool udpsrv_running;
static uint16_t udpsrv_port;
static mutex_t udpsrv_lock = MUTEX_INITIAL_VALUE(udpsrv_lock);

/* ---- estado de subida ---- */
struct fwstate {
    bool active;
    filehandle *fh;
    char temp_path[FS_MAX_PATH_LEN];
    char final_path[FS_MAX_PATH_LEN];
    uint32_t expected;
    uint32_t received;
    uint32_t next_seq;
    ipv4_addr_t client;
    uint16_t client_port;
};
static struct fwstate fw;

static uint16_t rd16_be(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void wr16_be(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)(v & 0xff); }

/* ---- salida acotada ---- */
struct outbuf {
    char *buf;
    size_t cap;
    size_t len;
};

static void out_init(struct outbuf *o, char *buf, size_t cap) {
    o->buf = buf; o->cap = cap; o->len = 0;
    if (cap) buf[0] = '\0';
}

__attribute__((format(printf, 2, 3)))
static void out_append(struct outbuf *o, const char *fmt, ...) {
    if (o->len + 1 >= o->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->buf + o->len, o->cap - o->len, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    size_t add = (size_t)n;
    if (add >= o->cap - o->len) add = o->cap - o->len - 1;
    o->len += add;
    o->buf[o->len] = '\0';
}

static void udp_reply_src(ipv4_addr_t src, uint16_t srcport, const char *text) {
    if (!text) return;
    size_t len = strlen(text);
    if (len == 0) return;
    udp_socket_t *sock = NULL;
    /* Responder desde el puerto del servidor hacia el puerto origen del cliente. */
    if (udp_open(src, udpsrv_port, srcport, &sock) != NO_ERROR)
        return;
    udp_send((void *)text, len, sock);
    udp_close(sock);
}

/* ---- utilidades ---- */
static bool fw_client_matches(ipv4_addr_t src, uint16_t srcport) {
    return fw.active && fw.client == src && fw.client_port == srcport;
}

static void fw_reset(bool remove_temp) {
    if (fw.fh) {
        fs_close_file(fw.fh);
        fw.fh = NULL;
    }
    if (remove_temp && fw.temp_path[0])
        fs_remove_file(fw.temp_path);
    memset(&fw, 0, sizeof(fw));
}

static bool valid_sd_path(const char *path) {
    if (!path || !*path) return false;
    if (strncmp(path, "/sd", 3) != 0) return false;
    if (strstr(path, "..")) return false;
    return true;
}

static void make_temp_path(const char *finalp, char *tmp, size_t cap) {
    snprintf(tmp, cap, "%s.tmp", finalp);
    tmp[cap - 1] = '\0';
}

static void append_net(struct outbuf *o) {
    netif_t *n = (netif_t *)wifi_netif_get();
    if (!n) {
        out_append(o, "net: no interface\r\n");
        return;
    }
    ipv4_addr_t mask = netif_get_netmask_ipv4(n);
    out_append(o, "ip: %u.%u.%u.%u\r\n", IPV4_SPLIT(n->ipv4_addr));
    out_append(o, "netmask: %u.%u.%u.%u\r\n", IPV4_SPLIT(mask));
    out_append(o, "gateway: %u.%u.%u.%u\r\n", IPV4_SPLIT(minip_get_gateway()));
    out_append(o, "dns: %u.%u.%u.%u\r\n", IPV4_SPLIT(minip_get_dns_server()));
    out_append(o, "hostname: %s\r\n", minip_get_hostname() ? minip_get_hostname() : "");
    out_append(o, "mac: %02x:%02x:%02x:%02x:%02x:%02x\r\n",
               n->mac_address[0], n->mac_address[1], n->mac_address[2],
               n->mac_address[3], n->mac_address[4], n->mac_address[5]);
}

/* ---- comandos FW ---- */
static void fw_begin(struct outbuf *o, unsigned long size, const char *path,
                     ipv4_addr_t src, uint16_t srcport) {
    if (size == 0 || size > FW_MAX_IMAGE) {
        out_append(o, "ERR FW_SIZE max=%u\r\n", FW_MAX_IMAGE);
        return;
    }
    const char *finalp = (path && path[0]) ? path : FW_DEFAULT_PATH;
    if (!valid_sd_path(finalp)) {
        out_append(o, "ERR FW_PATH (use /sd/...)\r\n");
        return;
    }
    fw_reset(true);

    strlcpy(fw.final_path, finalp, sizeof(fw.final_path));
    make_temp_path(fw.final_path, fw.temp_path, sizeof(fw.temp_path));

    fs_remove_file(fw.temp_path);
    if (fs_create_file(fw.temp_path, &fw.fh, 0) < 0) {
        out_append(o, "ERR FW_OPEN temp=%s\r\n", fw.temp_path);
        memset(&fw, 0, sizeof(fw));
        return;
    }
    fw.active = true;
    fw.expected = (uint32_t)size;
    fw.received = 0;
    fw.next_seq = 0;
    fw.client = src;
    fw.client_port = srcport;
    out_append(o, "OK FW_BEGIN size=%lu block=%u temp=%s final=%s\r\n",
               (unsigned long)fw.expected, (unsigned)FW_BLOCK_SIZE,
               fw.temp_path, fw.final_path);
}

static void fw_status(struct outbuf *o) {
    if (!fw.active) {
        out_append(o, "FW_STATUS IDLE\r\n");
        return;
    }
    out_append(o, "FW_STATUS ACTIVE size=%lu received=%lu next=%lu block=%u final=%s\r\n",
               (unsigned long)fw.expected, (unsigned long)fw.received,
               (unsigned long)fw.next_seq, (unsigned)FW_BLOCK_SIZE, fw.final_path);
}

static void fw_abort(struct outbuf *o, ipv4_addr_t src, uint16_t srcport) {
    if (fw.active && !fw_client_matches(src, srcport)) {
        out_append(o, "ERR FW_CLIENT\r\n");
        return;
    }
    fw_reset(true);
    out_append(o, "OK FW_ABORT\r\n");
}

static void fw_end(struct outbuf *o, ipv4_addr_t src, uint16_t srcport) {
    if (!fw.active || !fw.fh) {
        out_append(o, "ERR FW_IDLE\r\n");
        return;
    }
    if (!fw_client_matches(src, srcport)) {
        out_append(o, "ERR FW_CLIENT\r\n");
        return;
    }
    if (fw.received != fw.expected) {
        out_append(o, "ERR FW_SIZE expected=%lu received=%lu\r\n",
                   (unsigned long)fw.expected, (unsigned long)fw.received);
        return;
    }
    uint32_t size = fw.expected;
    fs_close_file(fw.fh);
    fw.fh = NULL;

    /* Reemplazar destino: borrar y mover/copiarenombre temporal.  FAT puede no
     * tener rename atomico expuesto; intentamos rename via fs (no disponible),
     * asi que hacemos copia por lectura/escritura y borramos temp.  Para
     * imagenes grandes esto corre en el hilo de red (como tftpd): aceptable. */
    fs_remove_file(fw.final_path);

    /* Intento simple: abrir temp y copiar a final. */
    filehandle *srcfh = NULL;
    filehandle *dstfh = NULL;
    status_t st = fs_open_file(fw.temp_path, &srcfh);
    if (st < 0) { fw_reset(true); out_append(o, "ERR FW_VERIFY\r\n"); return; }

    /* verificar tamaño del temporal */
    struct file_stat fst;
    if (fs_stat_file(srcfh, &fst) < 0 || fst.size != size) {
        fs_close_file(srcfh); fw_reset(true); out_append(o, "ERR FW_VERIFY\r\n"); return;
    }

    if (fs_create_file(fw.final_path, &dstfh, 0) < 0) {
        fs_close_file(srcfh); fw_reset(true); out_append(o, "ERR FW_FINALIZE\r\n"); return;
    }

    char buf[512];
    off_t off = 0;
    bool ok = true;
    while ((uint32_t)off < size) {
        size_t chunk = sizeof(buf);
        if (size - (uint32_t)off < chunk) chunk = size - (uint32_t)off;
        ssize_t r = fs_read_file(srcfh, buf, off, chunk);
        if (r <= 0) { ok = false; break; }
        ssize_t w = fs_write_file(dstfh, buf, off, (size_t)r);
        if (w != r) { ok = false; break; }
        off += r;
    }
    if (ok) {
        fs_truncate_file(dstfh, size);
    }
    if (dstfh) fs_close_file(dstfh);
    if (srcfh) fs_close_file(srcfh);

    if (!ok) {
        fs_remove_file(fw.final_path);
        fw_reset(true);
        out_append(o, "ERR FW_FINALIZE\r\n");
        return;
    }

    fs_remove_file(fw.temp_path);
    fw.active = false;
    fw.expected = 0;
    fw.received = 0;
    fw.next_seq = 0;

    out_append(o, "OK FW_END size=%lu staged=%s apply=blocked\r\n",
               (unsigned long)size, fw.final_path);
    /* apply bloqueado: no hay escritura de flash / salto bl2->flash en este puerto */
}

static void fw_handle_data(const uint8_t *data, size_t total, ipv4_addr_t src, uint16_t srcport) {
    char reply[192];
    struct outbuf o;
    out_init(&o, reply, sizeof(reply));

    if (!fw.active || !fw.fh) {
        out_append(&o, "ERR FW_IDLE\r\n");
        udp_reply_src(src, srcport, reply);
        return;
    }
    if (!fw_client_matches(src, srcport)) {
        out_append(&o, "ERR FW_CLIENT\r\n");
        udp_reply_src(src, srcport, reply);
        return;
    }

    /* Cabecera ASCII hasta '\n': "FW_DATA <seq> <len>\n" */
    size_t hlen = 0;
    while (hlen < total && hlen < 64 && data[hlen] != '\n') hlen++;
    if (hlen >= total || hlen >= 64) {
        out_append(&o, "ERR FW_HEADER\r\n");
        udp_reply_src(src, srcport, reply);
        return;
    }
    char hdr[72];
    memcpy(hdr, data, hlen);
    hdr[hlen] = '\0';
    if (hlen > 0 && hdr[hlen - 1] == '\r') hdr[hlen - 1] = '\0';

    unsigned long seq = 0;
    unsigned int blen = 0;
    /* formato: FW_DATA <seq> <len> */
    if (strncmp(hdr, "FW_DATA ", 8) != 0) {
        out_append(&o, "ERR FW_USAGE\r\n");
        udp_reply_src(src, srcport, reply);
        return;
    }
    char *pseq = hdr + 8;
    char *endp = NULL;
    seq = strtoul(pseq, &endp, 10);
    if (endp == pseq) {
        out_append(&o, "ERR FW_USAGE\r\n");
        udp_reply_src(src, srcport, reply);
        return;
    }
    while (*endp == ' ' || *endp == '\t') endp++;
    char *endp2 = NULL;
    blen = (unsigned int)strtoul(endp, &endp2, 10);
    if (endp2 == endp) {
        out_append(&o, "ERR FW_USAGE\r\n");
        udp_reply_src(src, srcport, reply);
        return;
    }

    const uint8_t *body = data + hlen + 1;
    size_t body_len = total - hlen - 1;
    if (body_len != blen) {
        out_append(&o, "ERR FW_LEN header=%u actual=%u\r\n", blen, (unsigned)body_len);
        udp_reply_src(src, srcport, reply);
        return;
    }
    if (blen == 0 || blen > FW_BLOCK_SIZE) {
        out_append(&o, "ERR FW_BLOCK max=%u\r\n", (unsigned)FW_BLOCK_SIZE);
        udp_reply_src(src, srcport, reply);
        return;
    }
    if ((uint32_t)seq > fw.next_seq) {
        out_append(&o, "ERR FW_SEQ expected=%lu got=%lu\r\n",
                   (unsigned long)fw.next_seq, seq);
        udp_reply_src(src, srcport, reply);
        return;
    }
    if ((uint32_t)seq < fw.next_seq) {
        /* retransmision: confirmar sin reescribir */
        out_append(&o, "OK FW_DATA %lu duplicate %lu/%lu\r\n", seq,
                   (unsigned long)fw.received, (unsigned long)fw.expected);
        udp_reply_src(src, srcport, reply);
        return;
    }
    if (fw.received > fw.expected || blen > (fw.expected - fw.received)) {
        out_append(&o, "ERR FW_OVERFLOW received=%lu len=%u size=%lu\r\n",
                   (unsigned long)fw.received, blen, (unsigned long)fw.expected);
        udp_reply_src(src, srcport, reply);
        return;
    }

    off_t off = (off_t)fw.received;
    ssize_t w = fs_write_file(fw.fh, body, off, blen);
    if (w != (ssize_t)blen) {
        fw_reset(true);
        out_append(&o, "ERR FW_WRITE\r\n");
        udp_reply_src(src, srcport, reply);
        return;
    }
    fw.received += blen;
    fw.next_seq++;
    out_append(&o, "OK FW_DATA %lu %lu/%lu%s\r\n", seq,
               (unsigned long)fw.received, (unsigned long)fw.expected,
               (fw.received == fw.expected) ? " complete" : "");
    udp_reply_src(src, srcport, reply);
}

/* ---- dispatch ---- */
static void handle_text(const char *data, size_t len, ipv4_addr_t src, uint16_t srcport) {
    char line[UDPSRV_LINE_MAX];
    if (len >= sizeof(line)) len = sizeof(line) - 1;
    memcpy(line, data, len);
    line[len] = '\0';
    /* trim final */
    while (len > 0 && (line[len-1]=='\r' || line[len-1]=='\n' || line[len-1]==' ' || line[len-1]=='\t'))
        line[--len] = '\0';
    /* tokenizar */
    char *argv[4] = {0};
    int argc = 0;
    char *p = line;
    while (*p && argc < 4) {
        while (*p==' '||*p=='\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p!=' ' && *p!='\t') p++;
        if (*p) *p++ = '\0';
    }
    if (argc == 0) return;

    char reply[UDPSRV_REPLY_MAX];
    struct outbuf o;
    out_init(&o, reply, sizeof(reply));

    if (!strcmp(argv[0], "PING")) {
        out_append(&o, "PONG %lu\r\n", (unsigned long)current_time());
    } else if (!strcmp(argv[0], "UPTIME")) {
        out_append(&o, "uptime_ms: %lu\r\n", (unsigned long)current_time());
    } else if (!strcmp(argv[0], "INFO")) {
        out_append(&o, "LK ESP32-P4\r\n");
        out_append(&o, "uptime_ms: %lu\r\n", (unsigned long)current_time());
        append_net(&o);
    } else if (!strcmp(argv[0], "NET")) {
        append_net(&o);
    } else if (!strcmp(argv[0], "SD")) {
        /* Evitar operaciones de FS en el callback del stack de red. */
        out_append(&o, "SD: usar telnet/TFTP/HTTP para acceder a /sd\r\n");
    } else if (!strcmp(argv[0], "FW_BEGIN")) {
        if (argc < 2) {
            out_append(&o, "ERR usage: FW_BEGIN <size> [path]\r\n");
        } else {
            unsigned long size = strtoul(argv[1], NULL, 10);
            const char *path = (argc >= 3) ? argv[2] : NULL;
            fw_begin(&o, size, path, src, srcport);
        }
    } else if (!strcmp(argv[0], "FW_END")) {
        fw_end(&o, src, srcport);
    } else if (!strcmp(argv[0], "FW_ABORT")) {
        fw_abort(&o, src, srcport);
    } else if (!strcmp(argv[0], "FW_STATUS")) {
        fw_status(&o);
    } else if (!strcmp(argv[0], "HELP")) {
        out_append(&o,
            "Commands: PING INFO NET UPTIME SD FW_BEGIN FW_DATA FW_END FW_ABORT FW_STATUS HELP\r\n"
            "FW_BEGIN <size> [path]  path defecto: /sd/fw.bin\r\n"
            "FW_DATA <seq> <len>\\n<bytes>  (binario, en el mismo datagrama)\r\n"
            "FW_END: deja imagen en SD (apply a flash no implementado)\r\n");
    } else {
        out_append(&o, "ERR unknown command '%s'\r\n", argv[0]);
    }

    udp_reply_src(src, srcport, reply);
}

static void udpsrv_cb(void *data, size_t len, uint32_t srcaddr, uint16_t srcport, void *arg) {
    (void)arg;
    if (!data || len == 0) return;
    const uint8_t *d = (const uint8_t *)data;
    /* FW_DATA: cabecera ASCII seguida de bytes binarios */
    if (len >= 8 && memcmp(d, "FW_DATA ", 8) == 0) {
        fw_handle_data(d, len, (ipv4_addr_t)srcaddr, srcport);
        return;
    }
    handle_text((const char *)d, len, (ipv4_addr_t)srcaddr, srcport);
}

static int cmd_udpserver(int argc, const console_cmd_args *argv) {
    mutex_acquire(&udpsrv_lock);
    if (argc >= 2 && !strcmp(argv[1].str, "stop")) {
        if (udpsrv_running) {
            udp_listen(udpsrv_port, NULL, NULL);
            udpsrv_running = false;
            fw_reset(false);
            printf("udpserver: detenido (UDP %u)\n", udpsrv_port);
        } else {
            printf("udpserver: no esta corriendo\n");
        }
        mutex_release(&udpsrv_lock);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1].str, "status")) {
        if (udpsrv_running)
            printf("udpserver: activo en UDP %u\n", udpsrv_port);
        else
            printf("udpserver: detenido\n");
        mutex_release(&udpsrv_lock);
        return 0;
    }

    uint16_t port = UDPSRV_DEFAULT_PORT;
    if (argc >= 2) {
        int p = atoi(argv[1].str);
        if (p <= 0 || p > 65535) {
            printf("udpserver: puerto invalido\n");
            mutex_release(&udpsrv_lock);
            return -1;
        }
        port = (uint16_t)p;
    }
    if (udpsrv_running) {
        printf("udpserver: ya esta activo en UDP %u\n", udpsrv_port);
        mutex_release(&udpsrv_lock);
        return 0;
    }
    if (udp_listen(port, udpsrv_cb, NULL) == 0) {
        udpsrv_running = true;
        udpsrv_port = port;
        printf("udpserver: escuchando UDP %u (core0, minip)\n", port);
    } else {
        printf("udpserver: no se pudo registrar UDP %u\n", port);
    }
    mutex_release(&udpsrv_lock);
    return 0;
}

STATIC_COMMAND_START
STATIC_COMMAND("udpserver", "servidor UDP diagnostico/FW a SD (port|stop|status)", &cmd_udpserver)
STATIC_COMMAND_END(udpserver);
