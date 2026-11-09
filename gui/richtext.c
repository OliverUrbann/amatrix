/* richtext.c - Matrix HTML (formatted_body) to text with style marks.
 * Plain C, no Amiga calls: also built and tested on Linux
 * (tests/gui/richtext_test.c). */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "richtext.h"

#define MAX_LISTS 8

typedef struct {
    char *b;
    size_t n, cap;
    int oom;
    int bold, italic, under;      /* open style tags */
    int line_start;               /* nothing on the current line yet */
    int space;                    /* a blank is due before the next word */
    int quote;                    /* <blockquote> depth */
    int pre;                      /* inside <pre> */
    int lists[MAX_LISTS], n_lists;   /* 0 = <ul>, else the next number of an <ol> */
    int item;                     /* the next text starts a list item */
    int any;                      /* something was written at all */
} R;

static void raw(R *r, const char *s, size_t len)
{
    if (r->oom) return;
    if (r->n + len + 1 > r->cap) {
        size_t nc = (r->cap ? r->cap * 2 : 256) + len;
        char *t = realloc(r->b, nc);
        if (!t) { r->oom = 1; return; }
        r->b = t;
        r->cap = nc;
    }
    memcpy(r->b + r->n, s, len);
    r->n += len;
    r->b[r->n] = 0;
}

static void mark(R *r, char c)
{
    char m[2];
    m[0] = RICH_MARK;
    m[1] = c;
    raw(r, m, 2);
}

static void styles(R *r)
{
    if (r->bold) mark(r, 'b');
    if (r->italic) mark(r, 'i');
    if (r->under) mark(r, 'u');
}

/* the beginning of a line: quote bars, list indent and marker, open styles */
static void begin_line(R *r)
{
    int i;
    char num[16];
    if (!r->line_start) return;
    r->line_start = 0;
    r->space = 0;
    for (i = 0; i < r->quote; i++) raw(r, "| ", 2);
    if (r->pre) raw(r, "    ", 4);
    for (i = 1; i < r->n_lists; i++) raw(r, "  ", 2);
    if (r->n_lists) {
        if (r->item) {
            int *k = &r->lists[r->n_lists - 1];
            if (*k) { sprintf(num, "%d. ", (*k)++); raw(r, num, strlen(num)); }
            else raw(r, "- ", 2);
        } else raw(r, "  ", 2);
    }
    r->item = 0;
    styles(r);
    r->any = 1;
}

static void newline(R *r)
{
    raw(r, "\n", 1);
    r->line_start = 1;
    r->space = 0;
}

/* a block starts or ends: finish the current line (never an empty one) */
static void brk(R *r)
{
    if (!r->line_start) newline(r);
}

static void text(R *r, const char *s, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 27 || c == (unsigned char)RICH_MARK) continue;       /* no codes from senders */
        if (r->pre) {
            if (c == '\n') { if (r->line_start) begin_line(r); newline(r); continue; }
            if (c == '\r') continue;
            begin_line(r);
            raw(r, c == '\t' ? " " : (const char *)&s[i], 1);
            continue;
        }
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
            if (!r->line_start) r->space = 1;
            continue;
        }
        if (r->line_start) begin_line(r);
        else if (r->space) { raw(r, " ", 1); r->space = 0; }
        raw(r, (const char *)&s[i], 1);
    }
}

/* a style tag opened (d = 1) or closed (d = -1) */
static void style(R *r, int *which, int d)
{
    int was = *which > 0;
    *which += d;
    if (*which < 0) *which = 0;
    if ((*which > 0) == was || r->line_start) return;     /* a new line sets them itself */
    if (r->space) { raw(r, " ", 1); r->space = 0; }
    if (d > 0) mark(r, which == &r->bold ? 'b' : which == &r->italic ? 'i' : 'u');
    else { mark(r, 'n'); styles(r); }
}

static void utf8(R *r, unsigned long cp)
{
    char b[4];
    if (cp < 0x80) { b[0] = (char)cp; text(r, b, 1); }
    else if (cp < 0x800) { b[0] = (char)(0xc0 | cp >> 6); b[1] = (char)(0x80 | (cp & 0x3f)); text(r, b, 2); }
    else if (cp < 0x10000) {
        b[0] = (char)(0xe0 | cp >> 12); b[1] = (char)(0x80 | ((cp >> 6) & 0x3f)); b[2] = (char)(0x80 | (cp & 0x3f));
        text(r, b, 3);
    } else if (cp < 0x110000) {
        b[0] = (char)(0xf0 | cp >> 18); b[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3f)); b[3] = (char)(0x80 | (cp & 0x3f));
        text(r, b, 4);
    }
}

/* &name; at p: writes it, returns its length (0: not an entity) */
static size_t entity(R *r, const char *p)
{
    static const struct { const char *name; unsigned long cp; } ents[] = {
        { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xa0 },
        { "auml", 0xe4 }, { "ouml", 0xf6 }, { "uuml", 0xfc }, { "Auml", 0xc4 }, { "Ouml", 0xd6 }, { "Uuml", 0xdc },
        { "szlig", 0xdf }, { "euro", 0x20ac }, { "hellip", 0x2026 }, { "ndash", 0x2013 }, { "mdash", 0x2014 },
        { "copy", 0xa9 }, { "laquo", 0xab }, { "raquo", 0xbb } };
    const char *semi = strchr(p, ';');
    size_t len, i;
    if (!semi || semi - p > 10 || semi == p + 1) return 0;
    len = (size_t)(semi - p) - 1;
    if (p[1] == '#') {
        unsigned long cp = p[2] == 'x' || p[2] == 'X' ? strtoul(p + 3, NULL, 16) : strtoul(p + 2, NULL, 10);
        if (!cp) return 0;
        if (cp == 0xa0) cp = ' ';
        utf8(r, cp);
        return len + 2;
    }
    for (i = 0; i < sizeof ents / sizeof ents[0]; i++)
        if (strlen(ents[i].name) == len && !strncmp(ents[i].name, p + 1, len)) {
            if (ents[i].cp == 0xa0) { begin_line(r); if (r->space) { raw(r, " ", 1); r->space = 0; } raw(r, " ", 1); }
            else utf8(r, ents[i].cp);
            return len + 2;
        }
    return 0;
}

/* the value of attribute name in a tag's text (malloc()ed, entities &amp; decoded), or NULL */
static char *attr(const char *tag, size_t len, const char *name)
{
    size_t nl = strlen(name), i;
    for (i = 0; i + nl + 1 < len; i++) {
        const char *p = tag + i;
        char q, *v, *w;
        const char *e;
        if (!isspace((unsigned char)*p) || strncmp(p + 1, name, nl) || p[1 + nl] != '=') continue;
        p += nl + 2;
        q = *p == '"' || *p == '\'' ? *p++ : 0;
        for (e = p; e < tag + len && (q ? *e != q : !isspace((unsigned char)*e)); e++) ;
        if (!(v = malloc((size_t)(e - p) + 1))) return NULL;
        for (w = v; p < e; p++) {
            if (!strncmp(p, "&amp;", 5) && p + 5 <= e) { *w++ = '&'; p += 4; }
            else if ((unsigned char)*p >= 32) *w++ = *p;
        }
        *w = 0;
        return v;
    }
    return NULL;
}

/* skips to after the closing tag of name (case-insensitive); the rest if none */
static const char *skip_element(const char *p, const char *name)
{
    size_t nl = strlen(name);
    for (; *p; p++)
        if (p[0] == '<' && p[1] == '/') {
            size_t i;
            for (i = 0; i < nl && tolower((unsigned char)p[2 + i]) == name[i]; i++) ;
            if (i == nl && (p[2 + nl] == '>' || isspace((unsigned char)p[2 + nl]))) {
                const char *e = strchr(p, '>');
                return e ? e + 1 : p + strlen(p);
            }
        }
    return p;
}

char *amx_rich_from_html(const char *html)
{
    R r;
    const char *p = html ? html : "";
    char *href = NULL;                   /* of the open <a>, if it is to be shown */
    size_t href_at = 0;
    int pill = 0;

    memset(&r, 0, sizeof r);
    r.line_start = 1;
    raw(&r, "", 0);
    while (*p && !r.oom) {
        if (*p == '&') {
            size_t n = entity(&r, p);
            if (n) { p += n; continue; }
            text(&r, p++, 1);
            continue;
        }
        if (*p != '<') {
            const char *e = p + strcspn(p, "<&");
            text(&r, p, (size_t)(e - p));
            p = e;
            continue;
        }
        {   /* a tag */
            const char *e = strchr(p, '>'), *t = p + 1;
            char name[16];
            size_t k = 0, tl;
            int close = 0;
            if (!e) { text(&r, p, strlen(p)); break; }
            if (!strncmp(p, "<!--", 4)) { const char *c = strstr(p, "-->"); p = c ? c + 3 : p + strlen(p); continue; }
            if (*t == '/') { close = 1; t++; }
            while (t < e && k < sizeof name - 1 && (isalnum((unsigned char)*t) || *t == '-')) name[k++] = (char)tolower((unsigned char)*t++);
            name[k] = 0;
            tl = (size_t)(e - t);
            p = e + 1;
            if (!k) continue;

            if (!strcmp(name, "mx-reply") && !close) { p = skip_element(p, "mx-reply"); continue; }
            if (!strcmp(name, "span") && !close) {
                const char *sp = strstr(t, "data-mx-spoiler");
                if (sp && sp < e) {
                    text(&r, "[Spoiler]", 9);
                    p = skip_element(p, "span");
                }
                continue;
            }
            if (!strcmp(name, "b") || !strcmp(name, "strong")) style(&r, &r.bold, close ? -1 : 1);
            else if (!strcmp(name, "i") || !strcmp(name, "em")) style(&r, &r.italic, close ? -1 : 1);
            else if (!strcmp(name, "u") || !strcmp(name, "ins")) style(&r, &r.under, close ? -1 : 1);
            else if (!strcmp(name, "del") || !strcmp(name, "s") || !strcmp(name, "strike")) text(&r, "~", 1);
            else if (!strcmp(name, "code")) { if (!r.pre) text(&r, "`", 1); }
            else if (!strcmp(name, "br")) { begin_line(&r); newline(&r); }
            else if (!strcmp(name, "p") || !strcmp(name, "div")) brk(&r);
            else if (!strcmp(name, "hr")) { brk(&r); text(&r, "----", 4); brk(&r); }
            else if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
                if (!close) { brk(&r); style(&r, &r.bold, 1); }
                else { style(&r, &r.bold, -1); brk(&r); }
            } else if (!strcmp(name, "blockquote")) {
                brk(&r);
                if (!close) r.quote++;
                else if (r.quote) r.quote--;
            } else if (!strcmp(name, "pre")) {
                brk(&r);
                r.pre = !close;
                if (!close && *p == '\n') p++;
            } else if (!strcmp(name, "ul") || !strcmp(name, "ol")) {
                brk(&r);
                if (!close) {
                    if (r.n_lists < MAX_LISTS) {
                        int start = 1;
                        char *s = name[0] == 'o' ? attr(t, tl, "start") : NULL;
                        if (s) { start = atoi(s) > 0 ? atoi(s) : 1; free(s); }
                        r.lists[r.n_lists++] = name[0] == 'o' ? start : 0;
                    }
                } else if (r.n_lists) r.n_lists--;
                r.item = 0;
            } else if (!strcmp(name, "li")) {
                brk(&r);
                if (!close) r.item = 1;
            } else if (!strcmp(name, "img")) {
                char *alt = attr(t, tl, "alt");
                if (!alt) alt = attr(t, tl, "title");
                if (alt) { text(&r, alt, strlen(alt)); free(alt); }
            } else if (!strcmp(name, "a")) {
                if (!close) {
                    free(href);
                    href = attr(t, tl, "href");
                    pill = href && !strncmp(href, "https://matrix.to/#/", 20);
                    if (pill) { free(href); href = NULL; style(&r, &r.bold, 1); }
                    else {                           /* remember where the link's text starts */
                        if (r.line_start) begin_line(&r);
                        else if (r.space) { raw(&r, " ", 1); r.space = 0; }
                        href_at = r.n;
                    }
                } else if (pill) { style(&r, &r.bold, -1); pill = 0; }
                else if (href) {
                    /* the URL too, unless the text is the URL already */
                    size_t hl = strlen(href);
                    int same = r.n >= href_at && r.b && strstr(r.b + href_at, "://") != NULL;
                    if (!same && hl && r.n > href_at
                        && (!strncmp(href, "http://", 7) || !strncmp(href, "https://", 8) || !strncmp(href, "mailto:", 7))) {
                        text(&r, " (", 2);
                        text(&r, href, hl);
                        text(&r, ")", 1);
                    } else if (r.n == href_at) text(&r, href, hl);
                    free(href);
                    href = NULL;
                }
            }
            /* everything else (span, font, table parts ...): just its text */
        }
    }
    free(href);
    if (r.oom) { free(r.b); return NULL; }
    while (r.n && (r.b[r.n - 1] == '\n' || r.b[r.n - 1] == ' ')) r.b[--r.n] = 0;
    return r.b;
}
