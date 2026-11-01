/* Matrix canonical JSON (spec: Appendices / Canonical JSON):
 * keys sorted by Unicode code point (= byte order for UTF-8), no
 * insignificant whitespace, UTF-8 output without \u escapes except for
 * control characters, integers only. Used for signing device and one-time
 * keys. */
#include "canonjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *p; size_t len, cap; int err; } SB;

static void sb_put(SB *s, const char *d, size_t n)
{
    if (s->err) return;
    if (s->len + n + 1 > s->cap) {
        size_t nc = s->cap ? s->cap : 256;
        char *np;
        while (nc < s->len + n + 1) nc *= 2;
        np = realloc(s->p, nc);
        if (!np) { s->err = 1; return; }
        s->p = np;
        s->cap = nc;
    }
    memcpy(s->p + s->len, d, n);
    s->len += n;
    s->p[s->len] = 0;
}
static void sb_c(SB *s, char c) { sb_put(s, &c, 1); }

static void put_string(SB *s, const char *str)
{
    const unsigned char *u = (const unsigned char *)str;
    sb_c(s, '"');
    for (; *u; u++) {
        char esc[8];
        switch (*u) {
        case '"':  sb_put(s, "\\\"", 2); break;
        case '\\': sb_put(s, "\\\\", 2); break;
        case '\b': sb_put(s, "\\b", 2); break;
        case '\f': sb_put(s, "\\f", 2); break;
        case '\n': sb_put(s, "\\n", 2); break;
        case '\r': sb_put(s, "\\r", 2); break;
        case '\t': sb_put(s, "\\t", 2); break;
        default:
            if (*u < 0x20) {
                sprintf(esc, "\\u%04x", *u);
                sb_put(s, esc, 6);
            } else {
                sb_c(s, (char)*u);
            }
        }
    }
    sb_c(s, '"');
}

static int cmp_items(const void *a, const void *b)
{
    const cJSON *x = *(const cJSON * const *)a, *y = *(const cJSON * const *)b;
    return strcmp(x->string, y->string);
}

static void put_value(SB *s, const cJSON *v)
{
    char num[32];
    if (cJSON_IsNull(v)) sb_put(s, "null", 4);
    else if (cJSON_IsTrue(v)) sb_put(s, "true", 4);
    else if (cJSON_IsFalse(v)) sb_put(s, "false", 5);
    else if (cJSON_IsNumber(v)) {
        /* canonical JSON only allows integers in [-(2^53)+1, 2^53-1] */
        sprintf(num, "%.0f", v->valuedouble);
        sb_put(s, num, strlen(num));
    } else if (cJSON_IsString(v)) put_string(s, v->valuestring);
    else if (cJSON_IsArray(v)) {
        const cJSON *c;
        int first = 1;
        sb_c(s, '[');
        cJSON_ArrayForEach(c, v) {
            if (!first) sb_c(s, ',');
            first = 0;
            put_value(s, c);
        }
        sb_c(s, ']');
    } else if (cJSON_IsObject(v)) {
        int n = cJSON_GetArraySize(v), i = 0;
        const cJSON **items, *c;
        sb_c(s, '{');
        if (n > 0) {
            items = malloc(sizeof *items * n);
            if (!items) { s->err = 1; return; }
            cJSON_ArrayForEach(c, v) items[i++] = c;
            qsort(items, n, sizeof *items, cmp_items);
            for (i = 0; i < n; i++) {
                if (i) sb_c(s, ',');
                put_string(s, items[i]->string);
                sb_c(s, ':');
                put_value(s, items[i]);
            }
            free(items);
        }
        sb_c(s, '}');
    } else {
        s->err = 1;
    }
}

char *canonical_json(const cJSON *v)
{
    SB s = { NULL, 0, 0, 0 };
    put_value(&s, v);
    if (s.err) { free(s.p); return NULL; }
    return s.p;
}

char *canonical_json_for_signing(const cJSON *obj)
{
    cJSON *copy = cJSON_Duplicate(obj, 1);
    char *r;
    if (!copy) return NULL;
    cJSON_DeleteItemFromObjectCaseSensitive(copy, "signatures");
    cJSON_DeleteItemFromObjectCaseSensitive(copy, "unsigned");
    r = canonical_json(copy);
    cJSON_Delete(copy);
    return r;
}
