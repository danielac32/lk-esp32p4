/*
 * printf/vprintf/snprintf para LK (ESP32-P4).
 * Núcleo de formato basado en la lógica de doprnt de Xinu (xinu-p4-smp/lib/libxc/doprnt.c),
 * adaptado a LK: salida por putc callback, soporte %z, y snprintf/vsnprintf funcionales.
 * vprintf/printf mantienen print_spin_lock + platform_dputc (sin romper LK).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <arch/ops.h>
#include <kernel/spinlock.h>
#include <kernel/thread.h>
#include <printf.h>

void platform_dputc(char c);
extern spin_lock_t print_spin_lock;

#define LONG_BITS (8 * sizeof(unsigned long long))

enum integer_size {
    SHORT_SHORT_SIZE,
    SHORT_SIZE,
    REGULAR_SIZE,
    LONG_SIZE,
    LONG_LONG_SIZE,
    SIZE_T_SIZE
};

static void ullong_to_string(unsigned long long num, char *str,
                             unsigned int base, bool alt_digits);

static int _doprnt(const char *fmt, va_list ap,
                   int (*putc_func)(int, uintptr_t), uintptr_t putc_arg) {
    int chars_written = 0;
    int i;
    char *str;
    char string[LONG_BITS + 1];

    bool leftjust;
    char pad_char;
    int fmin;
    int prec;
    enum integer_size size;
    char sign;

    long long larg;
    unsigned long long ularg;
    bool alt_digits;
    unsigned int base;

    int len_str;
    int num_zeroes;
    int len_nonpadding;
    int len_padding;

    const char *spec_start;

    while (*fmt != '\0') {
        if (*fmt == '%' && *++fmt != '%') {
            spec_start = fmt - 1;

            pad_char = ' ';
            leftjust = false;
            bool alt_form = false;
            for (;; fmt++) {
                switch (*fmt) {
                case '-': leftjust = true; break;
                case '0': pad_char = '0'; break;
                case '#': alt_form = true; break;
                default: goto flags_scanned;
                }
            }
        flags_scanned:

            fmin = 0;
            if (*fmt == '*') {
                fmin = va_arg(ap, int);
                if (fmin < 0) {
                    fmin = -fmin;
                    leftjust = true;
                }
                fmt++;
            } else {
                while ('0' <= *fmt && *fmt <= '9') {
                    fmin *= 10;
                    fmin += (*fmt - '0');
                    fmt++;
                }
            }
            if (leftjust) pad_char = ' ';

            prec = -1;
            if (*fmt == '.') {
                fmt++;
                if (*fmt == '*') {
                    prec = va_arg(ap, int);
                    fmt++;
                } else {
                    prec = 0;
                    while ('0' <= *fmt && *fmt <= '9') {
                        prec *= 10;
                        prec += (*fmt - '0');
                        fmt++;
                    }
                }
            }

            size = REGULAR_SIZE;
            if (*fmt == 'l') {
                size = LONG_SIZE;
                fmt++;
                if (*fmt == 'l') {
                    size = LONG_LONG_SIZE;
                    fmt++;
                }
            } else if (*fmt == 'h') {
                fmt++;
                if (*fmt == 'h') { size = SHORT_SHORT_SIZE; fmt++; }
                else { size = SHORT_SIZE; }
            } else if (*fmt == 'z') {
                size = SIZE_T_SIZE;
                fmt++;
            }

            base = 0;
            sign = '\0';
            str = string;
            alt_digits = false;
            const char *prefix = "";

            switch (*fmt) {
            case 'c':
                string[0] = (unsigned char)va_arg(ap, int);
                string[1] = '\0';
                break;
            case 's':
                str = va_arg(ap, char *);
                if (str == NULL) str = "(null)";
                break;
            case 'd':
            case 'i':
                base = 10;
                if (size == LONG_LONG_SIZE)
                    larg = va_arg(ap, long long);
                else if (size == LONG_SIZE)
                    larg = va_arg(ap, long);
                else if (size == SIZE_T_SIZE)
                    larg = (long long)(ssize_t)va_arg(ap, ssize_t);
                else
                    larg = va_arg(ap, int);
                ularg = (unsigned long long)larg;
                if (larg < 0) { sign = '-'; ularg = (unsigned long long)(-larg); }
                break;
            case 'u': base = 10; goto handle_unsigned;
            case 'o':
                base = 8;
                goto handle_unsigned;
            case 'x': base = 16; goto handle_unsigned;
            case 'X': base = 16; alt_digits = true; goto handle_unsigned;
            case 'p':
                base = 16;
                ularg = (unsigned long long)(uintptr_t)va_arg(ap, void *);
                prefix = "0x";
                break;
            case 'b': base = 2;  goto handle_unsigned;
            handle_unsigned:
                if (size == LONG_LONG_SIZE)
                    ularg = va_arg(ap, unsigned long long);
                else if (size == LONG_SIZE)
                    ularg = va_arg(ap, unsigned long);
                else if (size == SIZE_T_SIZE)
                    ularg = (unsigned long long)va_arg(ap, size_t);
                else
                    ularg = va_arg(ap, unsigned int);
                break;
            default:
                fmt = spec_start;
                goto literal;
            }

            fmt++;

            if (base != 0) {
                ullong_to_string(ularg, str, base, alt_digits);
            }

            if (alt_form && ularg != 0) {
                if (base == 16) {
                    prefix = alt_digits ? "0X" : "0x";
                } else if (base == 8 && str[0] != '0') {
                    prefix = "0";
                } else if (base == 2) {
                    prefix = alt_digits ? "0B" : "0b";
                }
            }
            int prefix_len = (int)strlen(prefix);

            num_zeroes = 0;
            len_str = (int)strlen(str);
            if (prec >= 0) {
                if (base == 0) {
                    if (prec < len_str) len_str = prec;
                } else {
                    if (prec > len_str) num_zeroes = prec - len_str;
                    pad_char = ' ';
                }
            }

            len_nonpadding = prefix_len + len_str + num_zeroes + (sign != '\0');
            len_padding = 0;
            if (len_nonpadding < fmin) len_padding = fmin - len_nonpadding;

            if (!leftjust && pad_char == '0') {
                num_zeroes = len_padding;
                len_padding = 0;
            }

            if (!leftjust) {
                for (i = 0; i < len_padding; i++) {
                    if (putc_func(' ', putc_arg) < 0) return -1;
                    chars_written++;
                }
            }
            if (sign != '\0') {
                if (putc_func(sign, putc_arg) < 0) return -1;
                chars_written++;
            }
            if (prefix_len) {
                for (i = 0; i < prefix_len; i++) {
                    if (putc_func((unsigned char)prefix[i], putc_arg) < 0) return -1;
                    chars_written++;
                }
            }
            for (i = 0; i < num_zeroes; i++) {
                if (putc_func('0', putc_arg) < 0) return -1;
                chars_written++;
            }
            for (i = 0; i < len_str; i++) {
                if (putc_func((unsigned char)str[i], putc_arg) < 0) return -1;
                chars_written++;
            }
            if (leftjust) {
                for (i = 0; i < len_padding; i++) {
                    if (putc_func(' ', putc_arg) < 0) return -1;
                    chars_written++;
                }
            }
        } else {
        literal:
            if (putc_func((unsigned char)*fmt, putc_arg) < 0) return -1;
            chars_written++;
            fmt++;
        }
    }
    return chars_written;
}

static const char digits_lc[16] = "0123456789abcdef";
static const char digits_uc[16] = "0123456789ABCDEF";
static const unsigned char base_to_nbits[17] = {
    [2] = 1, [4] = 2, [8] = 3, [16] = 4,
};

static void ullong_to_string(unsigned long long num, char *str,
                             unsigned int base, bool alt_digits) {
    const char *digits = alt_digits ? digits_uc : digits_lc;
    char temp[8 * sizeof(unsigned long long) + 1];
    int i = 1;
    temp[0] = '\0';

    if (base_to_nbits[base] == 0) {
        for (;;) {
            temp[i] = digits[num % base];
            num /= base;
            if (num == 0) break;
            i++;
        }
    } else {
        unsigned char shift = base_to_nbits[base];
        unsigned long long mask = (1ULL << shift) - 1ULL;
        for (;;) {
            temp[i] = digits[num & mask];
            num >>= shift;
            if (num == 0) break;
            i++;
        }
    }
    do {
        *str++ = temp[i--];
    } while (i >= 0);
}

/* --- captura del log para fbcon --- *
 * Se captura CADA caracter desde el primer printf del kernel, mucho antes de
 * que el LCD/fbcon esten listos (eso ocurre en LK_INIT_LEVEL_PLATFORM).  El
 * ring buffer vive en RAM y se vuelca al fbcon en cuanto este se inicializa,
 * para que la pantalla muestre el arranque completo, no solo desde PLATFORM.
 * Se escribe bajo print_spin_lock (que ya serializa todos los printf), por lo
 * que no necesita sincronizacion adicional. */
#define LOG_CAPTURE_SZ 32768U
static char log_capture[LOG_CAPTURE_SZ];
static unsigned log_capture_head;

/* Hook debil: dev/fbcon lo redefine para reflejar printf en pantalla en vivo.
 * Mientras no este definido, el log sigue capturandose en log_capture. */
void fbcon_putc_hook(char c) __attribute__((weak));

/* Salida de printf por hilo (para telnet).  Cada hilo puede redirigir su
 * printf a un putc propio guardado en TLS_ENTRY_PUTCHAR.  0 = UART. */
typedef void (*thread_putc_t)(char);
void printf_set_putc(thread_putc_t fn) {
    tls_set(TLS_ENTRY_PUTCHAR, (uintptr_t)fn);
}
thread_putc_t printf_get_putc(void) {
    /* En early boot (antes de threading) no hay thread actual: no hay sink. */
    if (!get_current_thread()) return NULL;
    return (thread_putc_t)tls_get(TLS_ENTRY_PUTCHAR);
}

static void log_capture_putc(char c) {
    log_capture[log_capture_head] = c;
    log_capture_head = (log_capture_head + 1U) % LOG_CAPTURE_SZ;
}

/* Devuelve el log capturado en orden cronologico para volcarlo al fbcon. */
size_t printf_get_log(char *dst, size_t cap) {
    size_t n = log_capture_head < LOG_CAPTURE_SZ ? log_capture_head : LOG_CAPTURE_SZ;
    unsigned start = log_capture_head < LOG_CAPTURE_SZ ? 0 : log_capture_head;
    if (n > cap) { n = cap; start = (start + (log_capture_head - cap)) % LOG_CAPTURE_SZ; }
    for (size_t i = 0; i < n; i++) dst[i] = log_capture[(start + i) % LOG_CAPTURE_SZ];
    dst[n < cap ? n : cap - 1] = '\0';
    return n;
}

/* Salida de un caracter respetando el sink por hilo (telnet), fbcon y log.
 * Debe llamarse con print_spin_lock tomado (o en contexto donde la
 * serializacion este garantizada), igual que console_putc_cb. */
void _dputc(char c) {
    thread_putc_t sink = printf_get_putc();
    if (sink) {
        /* Hilo remoto (telnet): toda la salida va al socket, no a UART/fbcon. */
        sink(c);
    } else {
        platform_dputc(c);
        if (fbcon_putc_hook) fbcon_putc_hook(c);
    }
    log_capture_putc(c);
}

/* --- salida a consola (LK) --- */
static int console_putc_cb(int c, uintptr_t arg) {
    (void)arg;
    _dputc((char)c);
    return (unsigned char)c;
}

int vprintf(const char *fmt, va_list ap) {
    arch_interrupt_saved_state_t state = spin_lock_irqsave(&print_spin_lock);
    int r = _doprnt(fmt, ap, console_putc_cb, 0);
    spin_unlock_irqrestore(&print_spin_lock, state);
    return r;
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

int vfprintf(FILE *fp, const char *fmt, va_list ap) {
    (void)fp;
    return vprintf(fmt, ap);
}

int fprintf(FILE *fp, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(fp, fmt, ap);
    va_end(ap);
    return r;
}

/* --- salida a buffer (sprintf/snprintf), lógica estilo Xinu sprntf/snprntf --- */
static int str_putc_cb(int c, uintptr_t arg) {
    char **sptr = (char **)arg;
    *(*sptr)++ = (char)c;
    return (unsigned char)c;
}

struct sn_state { char *s; char *end; };

static int sn_putc_cb(int c, uintptr_t arg) {
    struct sn_state *st = (struct sn_state *)arg;
    if (st->s < st->end) {
        *st->s++ = (char)c;
    }
    return (unsigned char)c;
}

int vsprintf(char *str, const char *fmt, va_list ap) {
    char *s = str;
    int n = _doprnt(fmt, ap, str_putc_cb, (uintptr_t)&s);
    *s = '\0';
    return n;
}

int sprintf(char *str, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsprintf(str, fmt, ap);
    va_end(ap);
    return n;
}

int vsnprintf(char *str, size_t size, const char *fmt, va_list ap) {
    struct sn_state st;
    if (str == NULL) return -1;
    st.s = str;
    st.end = str + (size > 0 ? size - 1 : 0);
    int n = _doprnt(fmt, ap, sn_putc_cb, (uintptr_t)&st);
    if (size > 0) *st.s = '\0';
    return n;
}

int snprintf(char *str, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(str, size, fmt, ap);
    va_end(ap);
    return n;
}

/* Adaptador para la API de LK _printf_engine (la usan lib/unittest y otros).
 * _doprnt escribe caracter individual vía callback; aqui lo acumulamos en un
 * buffer y lo entregamos en bloques al out_func de LK. */
struct _pe_state {
    _printf_engine_output_func out;
    void *state;
    char buf[256];
    size_t used;
    int total;
};

static int _pe_putc(int c, uintptr_t arg) {
    struct _pe_state *s = (struct _pe_state *)arg;
    if (s->used >= sizeof(s->buf)) {
        s->out(s->buf, s->used, s->state);
        s->used = 0;
    }
    s->buf[s->used++] = (char)c;
    s->total++;
    return c;
}

int _printf_engine(_printf_engine_output_func out, void *state, const char *fmt, va_list ap) {
    struct _pe_state s = { .out = out, .state = state, .used = 0, .total = 0 };
    _doprnt(fmt, ap, _pe_putc, (uintptr_t)&s);
    if (s.used) s.out(s.buf, s.used, s.state);
    return s.total;
}

