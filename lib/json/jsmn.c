/*
 * jsmn minimal JSON parser (MIT), basado en jsmn de Serge Zaitsev.
 * Adaptado a LK: siempre mantiene parent, tipos LK, API compatible.
 */
#include <lib/json.h>
#include <string.h>
#include <stdlib.h>

void jsmn_init(jsmn_parser *parser) {
    parser->pos = 0;
    parser->toknext = 0;
    parser->toksuper = -1;
}

static jsmntok_t *jsmn_alloc_token(jsmn_parser *parser, jsmntok_t *tokens,
                                   unsigned num_tokens) {
    jsmntok_t *tok;
    if (parser->toknext >= num_tokens) return NULL;
    tok = &tokens[parser->toknext++];
    tok->start = tok->end = -1;
    tok->size = 0;
    tok->parent = -1;
    return tok;
}

static void jsmn_fill_token(jsmntok_t *token, jsmntype_t type,
                            int start, int end) {
    token->type = type;
    token->start = start;
    token->end = end;
    token->size = 0;
}

static int jsmn_parse_primitive(jsmn_parser *parser, const char *js,
                                size_t len, jsmntok_t *tokens,
                                unsigned num_tokens) {
    int start = (int)parser->pos;
    for (; parser->pos < len; parser->pos++) {
        char c = js[parser->pos];
        switch (c) {
            case ':': case '\t': case '\r': case '\n': case ' ':
            case ',': case ']': case '}':
                goto found;
            default:
                break;
        }
        if ((unsigned char)c < 32) {
            parser->pos = (unsigned)start;
            return -2;
        }
    }
found:
    if (!tokens) { parser->pos--; return 0; }
    jsmntok_t *token = jsmn_alloc_token(parser, tokens, num_tokens);
    if (!token) { parser->pos = (unsigned)start; return -1; }
    jsmn_fill_token(token, JSMN_PRIMITIVE, start, (int)parser->pos);
    token->parent = parser->toksuper;
    parser->pos--;
    return 0;
}

static int jsmn_parse_string(jsmn_parser *parser, const char *js,
                             size_t len, jsmntok_t *tokens,
                             unsigned num_tokens) {
    int start = (int)parser->pos;
    parser->pos++; /* skip opening quote */
    for (; parser->pos < len; parser->pos++) {
        char c = js[parser->pos];
        if (c == '"') {
            if (!tokens) return 0;
            jsmntok_t *token = jsmn_alloc_token(parser, tokens, num_tokens);
            if (!token) { parser->pos = (unsigned)start; return -1; }
            jsmn_fill_token(token, JSMN_STRING, start + 1, (int)parser->pos);
            token->parent = parser->toksuper;
            return 0;
        }
        if (c == '\\' && parser->pos + 1 < len) {
            parser->pos++;
            switch (js[parser->pos]) {
                case '"': case '/': case '\\':
                case 'b': case 'f': case 'r': case 'n': case 't':
                    break;
                case 'u':
                    parser->pos++;
                    for (int i = 0; i < 4 && parser->pos < len; i++, parser->pos++) {
                        char h = js[parser->pos];
                        if (!((h >= '0' && h <= '9') || (h >= 'A' && h <= 'F') ||
                              (h >= 'a' && h <= 'f'))) {
                            parser->pos = (unsigned)start;
                            return -2;
                        }
                    }
                    parser->pos--;
                    break;
                default:
                    parser->pos = (unsigned)start;
                    return -2;
            }
        }
    }
    parser->pos = (unsigned)start;
    return -3; /* partial */
}

int jsmn_parse(jsmn_parser *parser, const char *js, size_t len,
               jsmntok_t *tokens, unsigned num_tokens) {
    int r;
    int count = (int)parser->toknext;

    for (; parser->pos < len; parser->pos++) {
        char c = js[parser->pos];
        jsmntok_t *token;
        switch (c) {
            case '{':
            case '[':
                count++;
                if (!tokens) break;
                token = jsmn_alloc_token(parser, tokens, num_tokens);
                if (!token) return -1;
                if (parser->toksuper != -1) {
                    tokens[parser->toksuper].size++;
                    token->parent = parser->toksuper;
                }
                token->type = (c == '{') ? JSMN_OBJECT : JSMN_ARRAY;
                token->start = (int)parser->pos;
                parser->toksuper = (int)parser->toknext - 1;
                break;
            case '}':
            case ']': {
                if (!tokens) break;
                jsmntype_t type = (c == '}') ? JSMN_OBJECT : JSMN_ARRAY;
                /* find the open container to close */
                int i;
                for (i = (int)parser->toknext - 1; i >= 0; i--) {
                    token = &tokens[i];
                    if (token->start != -1 && token->end == -1) {
                        if (token->type != type) return -2;
                        token->end = (int)parser->pos + 1;
                        parser->toksuper = token->parent;
                        break;
                    }
                }
                if (i == -1) return -2; /* unmatched */
                /* if closing an object key's context, toksuper is already set
                 * correctly via parent. For nested containers inside object
                 * values, we need toksuper to point to the enclosing container
                 * if it's still open. */
                /* After closing, if parent container is still open and was a
                 * key context in an object (toksuper set by ':'), restore to
                 * the open container. */
                if (parser->toksuper != -1) {
                    jsmntok_t *pt = &tokens[parser->toksuper];
                    if (pt->type != JSMN_OBJECT && pt->type != JSMN_ARRAY) {
                        /* toksuper points to a key string; go up to its parent */
                        parser->toksuper = pt->parent;
                    }
                }
                break;
            }
            case '"':
                r = jsmn_parse_string(parser, js, len, tokens, num_tokens);
                if (r < 0) return r;
                count++;
                if (parser->toksuper != -1 && tokens)
                    tokens[parser->toksuper].size++;
                break;
            case '\t': case '\r': case '\n': case ' ':
                break;
            case ':':
                parser->toksuper = (int)parser->toknext - 1; /* key token */
                break;
            case ',':
                if (tokens && parser->toksuper != -1) {
                    jsmntok_t *t = &tokens[parser->toksuper];
                    if (t->type != JSMN_ARRAY && t->type != JSMN_OBJECT) {
                        /* we were in a key/value context; go to parent container */
                        parser->toksuper = t->parent;
                    }
                }
                break;
            default:
                r = jsmn_parse_primitive(parser, js, len, tokens, num_tokens);
                if (r < 0) return r;
                count++;
                if (parser->toksuper != -1 && tokens)
                    tokens[parser->toksuper].size++;
                break;
        }
    }
    /* check for unclosed containers */
    if (tokens) {
        for (int i = (int)parser->toknext - 1; i >= 0; i--) {
            if (tokens[i].start != -1 && tokens[i].end == -1)
                return -3;
        }
    }
    return count;
}

int jsmn_streq(const char *js, const jsmntok_t *tok, const char *s) {
    if (!tok || tok->type != JSMN_STRING) return 0;
    int n = tok->end - tok->start;
    return ((int)strlen(s) == n) && !strncmp(js + tok->start, s, n);
}

void jsmn_strcpy(const char *js, const jsmntok_t *tok, char *dst, size_t cap) {
    if (!tok || tok->start < 0 || tok->end < tok->start || cap == 0) {
        if (cap) dst[0] = '\0';
        return;
    }
    size_t n = (size_t)(tok->end - tok->start);
    if (n >= cap) n = cap - 1;
    memcpy(dst, js + tok->start, n);
    dst[n] = '\0';
}

int jsmn_int(const char *js, const jsmntok_t *tok) {
    if (!tok || tok->start < 0) return 0;
    char tmp[32];
    jsmn_strcpy(js, tok, tmp, sizeof(tmp));
    return (int)strtol(tmp, NULL, 10);
}

int jsmn_strcmp_true(const char *js, const jsmntok_t *tok) {
    if (!tok || tok->start < 0) return 0;
    int n = tok->end - tok->start;
    return (n == 4) && !strncmp(js + tok->start, "true", 4);
}
