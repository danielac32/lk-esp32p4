/*
 * Copyright (c) 2008 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

#include <stdarg.h>
#include <lk/compiler.h>
#include <stddef.h>

__BEGIN_CDECLS

/* printf engine that parses the format string and generates output */

/* function pointer to pass the printf engine, called back during the formatting.
 * input is a string to output, length bytes to output,
 * return code is number of characters that would have been written, or error code (if negative)
 */
typedef int (*_printf_engine_output_func)(const char *str, size_t len, void *state);

int _printf_engine(_printf_engine_output_func out, void *state, const char *fmt, va_list ap);

/* Devuelve el log del kernel capturado hasta ahora (para volcar al fbcon). */
size_t printf_get_log(char *dst, size_t cap);

/* Redirige el printf de ESTE hilo a un putc propio (telnet).  0 = UART. */
typedef void (*thread_putc_t)(char);
void printf_set_putc(thread_putc_t fn);
thread_putc_t printf_get_putc(void);

/* Salida de un caracter con redireccion por hilo, fbcon y captura de log.
 * Requiere print_spin_lock tomado por el llamador. */
void _dputc(char c);

#if !WITH_NO_FP
int _printf_engine_float(_printf_engine_output_func out, void *state, const char *fmt, va_list ap);
#endif

__END_CDECLS

