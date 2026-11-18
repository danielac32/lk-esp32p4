/*
 * Servidor TFTP (RFC 1350) sobre minip/UDP para el ESP32-P4.
 *
 *  - WRQ (escritura): guarda el archivo en /sd
 *  - RRQ (lectura):   envia el archivo desde /sd
 *  - 8-bit ASCII / octet (binario), bloques de 512 bytes.
 *
 * Se registra un unico callback UDP en el puerto 69 (udp_listen).  Cada
 * transferencia usa un socket UDP efimero al puerto que el cliente elige:
 * se abre con udp_open(host, sport, dport) y se cierra al terminar.  Todo
 * corre en el hilo del stack (core0), como exige la API de minip.
 *
 * Comandos: `tftpd' (arranca), `tftpd stop', `tftpd status'.
 */
#include <app.h>
#include <lk/err.h>
#include <lk/console_cmd.h>
#include <kernel/mutex.h>
#include <lib/minip.h>
#include <lib/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TFTP_PORT 69
#define BLOCK 512
#define ROOT "/sd"

#define OP_RRQ 1
#define OP_WRQ 2
#define OP_DATA 3
#define OP_ACK 4
#define OP_ERROR 5

static bool tftpd_running;
static mutex_t tftpd_lock = MUTEX_INITIAL_VALUE(tftpd_lock);

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xff; }

/* Parsea los dos strings nulos del RRQ/WRQ: filename y mode. */
static int parse_req(const uint8_t *data, size_t len, char *file, size_t fcap) {
    if (len < 4) return -1;
    size_t i = 2;
    size_t j = 0;
    while (i < len && data[i] && j + 1 < fcap) file[j++] = data[i++];
    file[j] = '\0';
    while (i < len && data[i]) i++;  /* saltar mode */
    return (i < len) ? 0 : -1;
}

static void resolve(const char *name, char *out, size_t cap) {
    const char *p = name;
    if (p[0] == '/') p++;
    if (strstr(p, "..")) { snprintf(out, cap, "%s/_invalid_", ROOT); return; }
    snprintf(out, cap, "%s/%s", ROOT, p);
}

static void send_error(udp_socket_t *sock, uint16_t code, const char *msg) {
    uint8_t pkt[64];
    wr16(pkt, OP_ERROR);
    wr16(pkt + 2, code);
    size_t n = strlen(msg);
    if (n > sizeof(pkt) - 4) n = sizeof(pkt) - 4;
    memcpy(pkt + 4, msg, n);
    pkt[4 + n] = 0;
    udp_send(pkt, 4 + n + 1, sock);
}

/* ---- Maquina de estados por transferencia (en el callback UDP) ---- */
typedef struct {
    bool used;
    uint32_t src;
    uint16_t sport;
    char path[160];
    filehandle *fh;
    bool writing;
    uint16_t next_block;
    size_t read_off;
    size_t file_size;
    bool read_done;
} xfer_t;

#define MAX_XFER 4
static xfer_t xfers[MAX_XFER];

static xfer_t *xfer_find_or_alloc(uint32_t src, uint16_t sport) {
    for (int i = 0; i < MAX_XFER; i++)
        if (xfers[i].used && xfers[i].src == src && xfers[i].sport == sport)
            return &xfers[i];
    for (int i = 0; i < MAX_XFER; i++)
        if (!xfers[i].used) {
            memset(&xfers[i], 0, sizeof(xfers[i]));
            xfers[i].used = true;
            xfers[i].src = src;
            xfers[i].sport = sport;
            return &xfers[i];
        }
    return NULL;
}

static void tftp_cb(void *data, size_t len, uint32_t src, uint16_t sport, void *arg) {
    (void)arg;
    if (len < 2) return;
    uint16_t op = rd16(data);
    char name[128];
    char path[160];
    udp_socket_t *sock = NULL;

    if (op == OP_RRQ || op == OP_WRQ) {
        if (parse_req(data, len, name, sizeof(name)) < 0) return;
        resolve(name, path, sizeof(path));
        xfer_t *x = xfer_find_or_alloc(src, sport);
        if (!x) return;
        strlcpy(x->path, path, sizeof(x->path));
        x->next_block = 1;
        x->read_off = 0;
        x->read_done = false;
        x->writing = (op == OP_WRQ);
        if (udp_open(src, TFTP_PORT, sport, &sock) != NO_ERROR) {
            x->used = false;
            return;
        }
        if (x->writing) {
            fs_remove_file(path);
            if (fs_create_file(path, &x->fh, 0) < 0) {
                send_error(sock, 2, "cannot create");
                udp_close(sock); x->used = false; return;
            }
            uint8_t ack[4];
            wr16(ack, OP_ACK); wr16(ack + 2, 0);
            udp_send(ack, 4, sock);
            printf("tftpd: WRQ %s\n", path);
        } else {
            x->fh = NULL;
            if (fs_open_file(path, &x->fh) < 0) {
                send_error(sock, 1, "not found");
                udp_close(sock); x->used = false; return;
            }
            struct file_stat st;
            x->file_size = 0;
            if (fs_stat_file(x->fh, &st) == NO_ERROR) x->file_size = st.size;
            printf("tftpd: RRQ %s (%u bytes)\n", path, (unsigned)x->file_size);
            /* enviar primer bloque */
            uint8_t pkt[BLOCK + 4];
            wr16(pkt, OP_DATA); wr16(pkt + 2, x->next_block);
            ssize_t r = fs_read_file(x->fh, pkt + 4, x->read_off, BLOCK);
            if (r < 0) r = 0;
            x->read_off += r;
            if ((size_t)r < BLOCK) x->read_done = true;
            udp_send(pkt, 4 + r, sock);
        }
        udp_close(sock);
        return;
    }

    xfer_t *x = xfer_find_or_alloc(src, sport);
    if (!x || !x->used) return;

    if (op == OP_DATA && x->writing) {
        uint16_t blk = rd16(data + 2);
        if (blk == x->next_block) {
            size_t n = len - 4;
            fs_write_file(x->fh, data + 4, (off_t)((blk - 1) * BLOCK), n);
            x->next_block++;
            if (udp_open(src, TFTP_PORT, sport, &sock) == NO_ERROR) {
                uint8_t ack[4];
                wr16(ack, OP_ACK); wr16(ack + 2, blk);
                udp_send(ack, 4, sock);
                udp_close(sock);
            }
            if (n < BLOCK) {   /* ultimo bloque */
                fs_truncate_file(x->fh, (uint64_t)((blk - 1) * BLOCK + n));
                fs_close_file(x->fh);
                printf("tftpd: WRQ %s completado\n", x->path);
                x->used = false;
            }
        }
        return;
    }

    if (op == OP_ACK && !x->writing) {
        uint16_t blk = rd16(data + 2);
        if (blk == x->next_block) {
            x->next_block++;
            if (udp_open(src, TFTP_PORT, sport, &sock) != NO_ERROR) return;
            uint8_t pkt[BLOCK + 4];
            wr16(pkt, OP_DATA); wr16(pkt + 2, x->next_block);
            ssize_t r = 0;
            if (!x->read_done)
                r = fs_read_file(x->fh, pkt + 4, x->read_off, BLOCK);
            if (r < 0) r = 0;
            x->read_off += r;
            if ((size_t)r < BLOCK) x->read_done = true;
            udp_send(pkt, 4 + r, sock);
            udp_close(sock);
            if (x->read_done && (size_t)r < BLOCK) {
                fs_close_file(x->fh);
                printf("tftpd: RRQ %s completado\n", x->path);
                x->used = false;
            }
        }
        return;
    }
}

static int cmd_tftpd(int argc, const console_cmd_args *argv) {
    mutex_acquire(&tftpd_lock);
    if (argc < 2 || !strcmp(argv[1].str, "start")) {
        if (tftpd_running) {
            printf("tftpd: ya esta corriendo\n");
        } else if (udp_listen(TFTP_PORT, tftp_cb, NULL) == 0) {
            tftpd_running = true;
            printf("tftpd: escuchando UDP %d (WRQ->/sd, RRQ<-/sd)\n", TFTP_PORT);
        } else {
            printf("tftpd: no se pudo registrar UDP %d\n", TFTP_PORT);
        }
    } else if (!strcmp(argv[1].str, "stop")) {
        udp_listen(TFTP_PORT, NULL, NULL);
        tftpd_running = false;
        printf("tftpd: detenido\n");
    } else if (!strcmp(argv[1].str, "status")) {
        printf("tftpd: %s en UDP %d\n", tftpd_running ? "activo" : "detenido", TFTP_PORT);
    } else {
        printf("uso: tftpd [start|stop|status]\n");
    }
    mutex_release(&tftpd_lock);
    return 0;
}

STATIC_COMMAND_START
STATIC_COMMAND("tftpd", "servidor TFTP sobre minip (start|stop|status)", &cmd_tftpd)
STATIC_COMMAND_END(tftpd);
