/*
 * jsmn: parser JSON minimalista para embebidos (dominio publico / MIT).
 * No asigna memoria: tokeniza sobre el buffer de entrada.  Suficiente para
 * leer kernel.json (objetos, arrays, strings, numeros, true/false/null).
 */
#pragma once

#include <stddef.h>

typedef enum {
    JSMN_UNDEFINED = 0,
    JSMN_OBJECT = 1,
    JSMN_ARRAY = 2,
    JSMN_STRING = 3,
    JSMN_PRIMITIVE = 4,
} jsmntype_t;

typedef struct {
    jsmntype_t type;
    int start;
    int end;
    int size;
    int parent;
} jsmntok_t;

typedef struct {
    unsigned pos;
    unsigned toknext;
    int toksuper;
} jsmn_parser;

void jsmn_init(jsmn_parser *parser);
/* Devuelve numero de tokens, o negativo: -1 sin memoria, -2 JSON invalido. */
int jsmn_parse(jsmn_parser *parser, const char *js, size_t len,
               jsmntok_t *tokens, unsigned num_tokens);

/* Compara el contenido de un token string con una cadena (js es el JSON). */
int jsmn_streq(const char *js, const jsmntok_t *tok, const char *s);
/* Copia el contenido de un token string/primitive a dst. */
void jsmn_strcpy(const char *js, const jsmntok_t *tok, char *dst, size_t cap);
/* Parsea un token primitive como entero. */
int jsmn_int(const char *js, const jsmntok_t *tok);
/* true si el token primitive es "true". */
int jsmn_strcmp_true(const char *js, const jsmntok_t *tok);
