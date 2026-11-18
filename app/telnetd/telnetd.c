/*
 * Servidor telnet sobre minip, inspirado en el telnetserver de Xinu.
 *
 * Por cada cliente se lanza un hilo con un sink de printf propio
 * (printf_set_putc) que envia toda la salida al socket TCP.  El contexto
 * (socket, buffer de salida) es por cliente y se localiza por hilo para que
 * varias sesiones no se pisen (el putc corre bajo print_spin_lock con IRQs
 * off, asi que no puede hacer tcp_write ni tomar mutex).
 *
 * Maneja negociacion IAC basica (WILL/WONT/DO/DONT, SB..SE) y filtra los
 * bytes de control del flujo de entrada, como hace el device telnet de Xinu.
 *
 * Comandos: `telnetd' (arranca, core0), `telnetd stop', `telnetd status'.
 */
#include <app.h>
#include <lk/err.h>
#include <lk/console_cmd.h>
#include <kernel/thread.h>
#include <kernel/mutex.h>
#include <lib/minip.h>
#include <lib/console.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define TELNET_PORT 23
#define TELNET_STK 16384
#define TELNET_ACCEPT_TIMEOUT 2000
#define LINE_MAX 256
#define OUTBUF_MAX 8192
#define MAX_CLIENTS 4

/* Codigos IAC (RFC 854) */
#define IAC  255
#define WILL 251
#define WONT 252
#define DO   253
#define DONT 254
#define SB   250
#define SE   240
#define GA   249
#define NOP  241
#define DM   242
#define BRK  243
#define IP   244
#define AO   245
#define AYT  246
#define EC   247
#define EL   248

#define OPT_ECHO        1
#define OPT_SUPPRESS_GA 3

typedef struct {
    bool used;
    thread_t *thread;
    tcp_socket_t *sock;
    char outbuf[OUTBUF_MAX];
    size_t outlen;
} telnet_client_ctx_t;

static tcp_socket_t *listen_socket;
static thread_t *telnetd_thread;
static bool telnetd_running;
static mutex_t telnetd_lock = MUTEX_INITIAL_VALUE(telnetd_lock);

static telnet_client_ctx_t clients[MAX_CLIENTS];
static spin_lock_t clients_lock = SPIN_LOCK_INITIAL_VALUE;

static telnet_client_ctx_t *client_alloc(thread_t *t, tcp_socket_t *s) {
    arch_interrupt_saved_state_t state = spin_lock_irqsave(&clients_lock);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].used) {
            clients[i].used = true;
            clients[i].thread = t;
            clients[i].sock = s;
            clients[i].outlen = 0;
            spin_unlock_irqrestore(&clients_lock, state);
            return &clients[i];
        }
    }
    spin_unlock_irqrestore(&clients_lock, state);
    return NULL;
}

static void client_free(telnet_client_ctx_t *ctx) {
    if (!ctx) return;
    arch_interrupt_saved_state_t state = spin_lock_irqsave(&clients_lock);
    ctx->used = false;
    ctx->thread = NULL;
    ctx->sock = NULL;
    ctx->outlen = 0;
    spin_unlock_irqrestore(&clients_lock, state);
}

/* Busca el contexto del hilo actual.  Puede llamarse bajo print_spin_lock
 * (IRQs off) porque solo lee campos estables durante la vida del cliente. */
static telnet_client_ctx_t *client_current(void) {
    thread_t *t = get_current_thread();
    if (!t) return NULL;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].used && clients[i].thread == t)
            return &clients[i];
    }
    return NULL;
}

/* putc por hilo: corre bajo print_spin_lock con IRQs off.  Solo acumula;
 * nunca llama a tcp_write. */
static void client_putc(char c) {
    telnet_client_ctx_t *ctx = client_current();
    if (!ctx || !ctx->sock) return;
    if (ctx->outlen + 2 >= OUTBUF_MAX) {
        /* Buffer lleno: descartar el resto de esta rafaga.  Se vacia al
         * volver de cada comando (client_flush). */
        return;
    }
    if (c == '\n') ctx->outbuf[ctx->outlen++] = '\r';
    ctx->outbuf[ctx->outlen++] = c;
}

static void client_flush(telnet_client_ctx_t *ctx) {
    if (!ctx || !ctx->sock || ctx->outlen == 0) return;
    /* Lectura/escritura de outlen/outbuf: el productor (putc) corre en este
     * mismo hilo bajo print_spin_lock; aqui estamos fuera del lock, asi que
     * no hay carrera con putc en el mismo hilo. */
    tcp_write(ctx->sock, ctx->outbuf, ctx->outlen);
    ctx->outlen = 0;
}

static void telnet_send_iac(tcp_socket_t *s, unsigned char cmd, unsigned char opt) {
    unsigned char buf[3] = { IAC, cmd, opt };
    tcp_write(s, buf, 3);
}

/* Lee una linea del socket, filtrando secuencias IAC.  Hace eco local
 * (servidor) de los caracteres tecleados.  Devuelve false si el peer cierra
 * o hay error. */
static bool read_line(tcp_socket_t *s, char *buf, size_t cap) {
    size_t pos = 0;
    for (;;) {
        unsigned char c;
        ssize_t n = tcp_read(s, &c, 1);
        if (n <= 0) return false;

        if (c == IAC) {
            unsigned char cmd;
            if (tcp_read(s, &cmd, 1) <= 0) return false;
            if (cmd == IAC) {
                if (pos < cap - 1) { buf[pos++] = (char)0xFF; tcp_write(s, "\xff", 1); }
                continue;
            }
            if (cmd == WILL || cmd == WONT || cmd == DO || cmd == DONT) {
                unsigned char opt;
                if (tcp_read(s, &opt, 1) <= 0) return false;
                if (cmd == WILL) {
                    telnet_send_iac(s, (opt == OPT_SUPPRESS_GA) ? DO : DONT, opt);
                } else if (cmd == DO) {
                    telnet_send_iac(s, (opt == OPT_ECHO || opt == OPT_SUPPRESS_GA) ? WILL : WONT, opt);
                }
                continue;
            }
            if (cmd == SB) {
                unsigned char prev = 0, sb;
                for (;;) {
                    if (tcp_read(s, &sb, 1) <= 0) return false;
                    if (prev == IAC && sb == SE) break;
                    prev = sb;
                }
                continue;
            }
            /* GA, NOP, DM, BRK, IP, AO, AYT, EC, EL... ignorar */
            continue;
        }

        if (c == '\r') {
            /* CR NUL o CR LF terminan la linea (RFC 854 / NVT). */
            unsigned char nxt;
            if (tcp_read(s, &nxt, 1) <= 0) return false;
            (void)nxt; /* consumir LF o NUL */
            tcp_write(s, "\r\n", 2);
            buf[pos] = '\0';
            return true;
        }
        if (c == '\n') {
            tcp_write(s, "\r\n", 2);
            buf[pos] = '\0';
            return true;
        }
        if (c == '\b' || c == 0x7f) {
            if (pos > 0) {
                pos--;
                tcp_write(s, "\b \b", 3);
            }
            continue;
        }
        if (c == 0x04) { /* ^D */
            buf[0] = '\0';
            return false;
        }
        if (pos < cap - 1 && (isprint(c) || c == '\t')) {
            buf[pos++] = (char)c;
            tcp_write(s, &c, 1);
        }
    }
}

static const char banner[] =
    "\r\n=== LK ESP32-P4 telnet ===\r\n"
    "Escribe 'help' para ver comandos, 'exit' para salir.\r\n";

static int telnet_client(void *arg) {
    tcp_socket_t *s = (tcp_socket_t *)arg;
    telnet_client_ctx_t *ctx = client_alloc(get_current_thread(), s);
    if (!ctx) {
        tcp_close(s);
        return 0;
    }

    /* Activar sink de printf para ESTE hilo (TLS).  client_putc localiza el
     * contexto por hilo, asi que varias sesiones no comparten buffer. */
    printf_set_putc(client_putc);

    console_t *con = console_create(false);
    if (con) console_set_current(con);

    /* Negociacion inicial (como telnetServer de Xinu): queremos eco local del
     * servidor y suprimir GA. */
    telnet_send_iac(s, WILL, OPT_ECHO);
    telnet_send_iac(s, DO, OPT_SUPPRESS_GA);

    tcp_write(s, banner, strlen(banner));

    char line[LINE_MAX];
    for (;;) {
        tcp_write(s, "> ", 2);
        if (!read_line(s, line, sizeof(line))) break;
        if (!strcmp(line, "exit") || !strcmp(line, "quit")) break;
        if (!line[0]) continue;

        /* Tokenizar y despachar como hace la consola local. */
        char *tok[16];
        int argc = 0;
        char *p = line;
        while (*p && argc < 16) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            tok[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = '\0';
        }
        if (argc > 0) {
            console_cmd_func handler = console_get_command_handler(tok[0]);
            if (handler) {
                console_cmd_args args[16];
                for (int i = 0; i < argc; i++) {
                    memset(&args[i], 0, sizeof(args[i]));
                    args[i].str = tok[i];
                    args[i].u = strtoul(tok[i], NULL, 0);
                    args[i].i = (long)args[i].u;
                    args[i].ull = args[i].u;
                    args[i].b = (args[i].u != 0);
                }
                handler(argc, args);
            } else {
                printf("command not found: %s\n", tok[0]);
            }
        }
        printf("\n");
        client_flush(ctx);
    }

    client_flush(ctx);
    printf_set_putc(NULL);
    client_free(ctx);
    tcp_close(s);
    return 0;
}

static int telnetd_entry(void *arg) {
    (void)arg;
    tcp_socket_t *client;
    while (telnetd_running) {
        status_t st = tcp_accept_timeout(listen_socket, &client, TELNET_ACCEPT_TIMEOUT);
        if (st != NO_ERROR) continue;
        thread_t *t = thread_create("telnet-sh", telnet_client, client,
                                    DEFAULT_PRIORITY, TELNET_STK);
        if (t) {
            /* Sesion de telnet en core0 (afinidad estable con minip). */
            thread_set_pinned_cpu(t, 0);
            thread_resume(t);
        } else {
            tcp_close(client);
        }
    }
    return 0;
}

static int cmd_telnetd(int argc, const console_cmd_args *argv) {
    mutex_acquire(&telnetd_lock);
    if (argc < 2 || !strcmp(argv[1].str, "start")) {
        if (telnetd_running) {
            printf("telnetd: ya esta corriendo\n");
        } else if (tcp_open_listen(&listen_socket, TELNET_PORT) == NO_ERROR) {
            telnetd_running = true;
            telnetd_thread = thread_create("telnetd", telnetd_entry, NULL,
                                           DEFAULT_PRIORITY, 4096);
            if (telnetd_thread) {
                thread_set_pinned_cpu(telnetd_thread, 0);
                thread_resume(telnetd_thread);
                printf("telnetd: escuchando en puerto %d (core0)\n", TELNET_PORT);
            } else {
                telnetd_running = false;
                tcp_close(listen_socket);
            }
        } else {
            printf("telnetd: no se pudo abrir puerto %d\n", TELNET_PORT);
        }
    } else if (!strcmp(argv[1].str, "stop")) {
        telnetd_running = false;
        if (listen_socket) { tcp_close(listen_socket); listen_socket = NULL; }
        printf("telnetd: detenido\n");
    } else if (!strcmp(argv[1].str, "status")) {
        printf("telnetd: %s en puerto %d\n", telnetd_running ? "activo" : "detenido", TELNET_PORT);
    } else {
        printf("uso: telnetd [start|stop|status]\n");
    }
    mutex_release(&telnetd_lock);
    return 0;
}

STATIC_COMMAND_START
STATIC_COMMAND("telnetd", "servidor telnet sobre minip (start|stop|status)", &cmd_telnetd)
STATIC_COMMAND_END(telnetd);
