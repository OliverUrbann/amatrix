/* Markdown -> HTML for sending (Matrix "org.matrix.custom.html").
 *
 * What Element understands and people type: **bold**, *italic* / _italic_,
 * ~~strike~~, `code`, ``` code blocks ```, "> " quotes, "- " / "* " lists,
 * "1. " lists, [text](url), headings "# ".  Web addresses are copied as
 * they are (no markup inside them), "_" inside words is no markup.
 * md_to_html returns NULL when the text has no markup: then plain text is
 * sent.  Input and output are UTF-8. */
#include <stdlib.h>
#include <string.h>
#include "markdown.h"

typedef struct { char *s; size_t n, cap; } Buf;

static void put(Buf *b, const char *t, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t c = (b->cap + n) * 2 + 64;
        char *x = realloc(b->s, c);
        if (!x) return;
        b->s = x;
        b->cap = c;
    }
    memcpy(b->s + b->n, t, n);
    b->n += n;
    b->s[b->n] = 0;
}
static void puts_(Buf *b, const char *t) { put(b, t, strlen(t)); }

static void put_esc(Buf *b, const char *t, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (t[i] == '<') puts_(b, "&lt;");
        else if (t[i] == '>') puts_(b, "&gt;");
        else if (t[i] == '&') puts_(b, "&amp;");
        else if (t[i] == '"') puts_(b, "&quot;");
        else put(b, t + i, 1);
    }
}

static int is_word(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (unsigned char)c >= 0x80; }

static size_t url_len(const char *p)
{
    size_t k;
    if (strncmp(p, "http://", 7) && strncmp(p, "https://", 8) && strncmp(p, "www.", 4)) return 0;
    k = strcspn(p, " \t\n<>\"");
    while (k > 0 && strchr(".,;:!?)'", p[k - 1])) k--;
    return k;
}

/* the inline markup of one line (n bytes) into b; counts what it changed */
static void inline_md(Buf *b, const char *t, size_t n, int *marks)
{
    size_t i = 0;
    while (i < n) {
        const char *p = t + i;
        size_t u = (i == 0 || !is_word(t[i - 1])) ? url_len(p) : 0, e;
        if (u && u <= n - i) { put_esc(b, p, u); i += u; continue; }
        if (*p == '\\' && i + 1 < n && strchr("\\`*_~[]#>-", p[1])) { put_esc(b, p + 1, 1); i += 2; (*marks)++; continue; }
        if (*p == '`') {                                       /* code: nothing else inside */
            const char *q = memchr(p + 1, '`', n - i - 1);
            if (q && q > p + 1) {
                puts_(b, "<code>");
                put_esc(b, p + 1, q - p - 1);
                puts_(b, "</code>");
                i = q - t + 1;
                (*marks)++;
                continue;
            }
        }
        if (*p == '[') {                                       /* [text](url) */
            const char *q = memchr(p, ']', n - i);
            if (q && q + 1 < t + n && q[1] == '(') {
                const char *r = memchr(q, ')', t + n - q);
                if (r && r > q + 2) {
                    puts_(b, "<a href=\"");
                    put_esc(b, q + 2, r - q - 2);
                    puts_(b, "\">");
                    inline_md(b, p + 1, q - p - 1, marks);
                    puts_(b, "</a>");
                    i = r - t + 1;
                    (*marks)++;
                    continue;
                }
            }
        }
        {   /* ** __ ~~ (double) and * _ (single), only at word edges */
            static const struct { const char *m; const char *tag; } pairs[] = {
                { "**", "strong" }, { "__", "strong" }, { "~~", "del" }, { "*", "em" }, { "_", "em" } };
            size_t k, done = 0;
            for (k = 0; k < sizeof pairs / sizeof pairs[0] && !done; k++) {
                size_t ml = strlen(pairs[k].m);
                const char *q;
                if (strncmp(p, pairs[k].m, ml) || i + ml >= n) continue;
                if (i > 0 && is_word(t[i - 1]) && pairs[k].m[0] == '_') continue;     /* snake_case */
                if (p[ml] == ' ' || p[ml] == pairs[k].m[0]) continue;
                for (q = p + ml; q + ml <= t + n; q++) {
                    if (strncmp(q, pairs[k].m, ml) || q[-1] == ' ') continue;
                    if (pairs[k].m[0] == '_' && q + ml < t + n && is_word(q[ml])) continue;
                    if (ml == 1 && q + 1 < t + n && q[1] == pairs[k].m[0]) { q++; continue; }
                    break;
                }
                if (q + ml > t + n || q == p + ml) continue;
                e = q - t;
                puts_(b, "<");
                puts_(b, pairs[k].tag);
                puts_(b, ">");
                inline_md(b, p + ml, e - i - ml, marks);
                puts_(b, "</");
                puts_(b, pairs[k].tag);
                puts_(b, ">");
                i = e + ml;
                (*marks)++;
                done = 1;
            }
            if (done) continue;
        }
        put_esc(b, p, 1);
        i++;
    }
}

static int list_item(const char *l, size_t n, int *ordered)
{
    size_t i = 0;
    if (n >= 2 && (l[0] == '-' || l[0] == '*' || l[0] == '+') && l[1] == ' ') { *ordered = 0; return 2; }
    while (i < n && l[i] >= '0' && l[i] <= '9') i++;
    if (i > 0 && i < 4 && i + 1 < n && l[i] == '.' && l[i + 1] == ' ') { *ordered = 1; return (int)i + 2; }
    return 0;
}

char *md_to_html(const char *text)
{
    Buf b = { NULL, 0, 0 };
    const char *p = text;
    int marks = 0, in_list = -1, in_quote = 0, first = 1;
    if (!text) return NULL;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        int ordered = 0, li;
        if (n >= 3 && !strncmp(p, "```", 3)) {                 /* a code block up to the next ``` */
            const char *end = strstr(p + 3, "\n```"), *body = nl ? nl + 1 : p + n;
            if (end && nl) {
                if (in_list >= 0) { puts_(&b, in_list ? "</ol>" : "</ul>"); in_list = -1; }
                if (in_quote) { puts_(&b, "</blockquote>"); in_quote = 0; }
                puts_(&b, "<pre><code>");
                if (end + 1 > body) put_esc(&b, body, end + 1 - body);
                puts_(&b, "</code></pre>");
                marks++;
                p = end + 4;
                p += strcspn(p, "\n");
                if (*p == '\n') p++;
                first = 1;
                continue;
            }
        }
        li = list_item(p, n, &ordered);
        if (in_list >= 0 && (!li || ordered != in_list)) { puts_(&b, in_list ? "</ol>" : "</ul>"); in_list = -1; first = 1; }
        if (in_quote && !(n >= 1 && p[0] == '>')) { puts_(&b, "</blockquote>"); in_quote = 0; first = 1; }
        if (li) {
            if (in_list < 0) { puts_(&b, ordered ? "<ol>" : "<ul>"); in_list = ordered; }
            puts_(&b, "<li>");
            inline_md(&b, p + li, n - li, &marks);
            puts_(&b, "</li>");
            marks++;
        } else if (n >= 1 && p[0] == '>') {
            size_t s = n >= 2 && p[1] == ' ' ? 2 : 1;
            if (!in_quote) { puts_(&b, "<blockquote>"); in_quote = 1; }
            else puts_(&b, "<br>");
            inline_md(&b, p + s, n - s, &marks);
            marks++;
        } else if (n >= 2 && p[0] == '#') {
            size_t h = 0;
            while (h < n && h < 6 && p[h] == '#') h++;
            if (h < n && p[h] == ' ') {
                char tag[8];
                tag[0] = '<'; tag[1] = 'h'; tag[2] = (char)('0' + h); tag[3] = '>'; tag[4] = 0;
                puts_(&b, tag);
                inline_md(&b, p + h + 1, n - h - 1, &marks);
                tag[1] = '/'; tag[2] = 'h'; tag[3] = (char)('0' + h); tag[4] = '>'; tag[5] = 0;
                puts_(&b, tag);
                marks++;
                first = 1;
            } else {
                if (!first) puts_(&b, "<br>");
                inline_md(&b, p, n, &marks);
                first = 0;
            }
        } else {
            if (!first) puts_(&b, "<br>");
            inline_md(&b, p, n, &marks);
            first = 0;
        }
        p += n;
        if (*p == '\n') p++;
    }
    if (in_list >= 0) puts_(&b, in_list ? "</ol>" : "</ul>");
    if (in_quote) puts_(&b, "</blockquote>");
    if (!marks) { free(b.s); return NULL; }
    return b.s ? b.s : calloc(1, 1);
}

/* HTML with the first occurrence of each mention's name made a link
 * (https://matrix.to/#/@user:server), as Element shows them as pills */
char *md_mentions(const char *html, const char *const *names, const char *const *ids, int n)
{
    Buf b = { NULL, 0, 0 };
    const char *p = html;
    int i, *done = calloc(n > 0 ? n : 1, sizeof *done);
    if (!done) return NULL;
    while (*p) {
        int hit = -1;
        if (*p == '<') {                                   /* tags as they are (no names inside) */
            const char *e = strchr(p, '>');
            size_t k = e ? (size_t)(e - p + 1) : strlen(p);
            put(&b, p, k);
            p += k;
            continue;
        }
        for (i = 0; i < n; i++)
            if (!done[i] && names[i][0] && !strncmp(p, names[i], strlen(names[i]))) { hit = i; break; }
        if (hit >= 0) {
            puts_(&b, "<a href=\"https://matrix.to/#/");
            puts_(&b, ids[hit]);
            puts_(&b, "\">");
            puts_(&b, names[hit]);
            puts_(&b, "</a>");
            p += strlen(names[hit]);
            done[hit] = 1;
            continue;
        }
        put(&b, p, 1);
        p++;
    }
    free(done);
    return b.s ? b.s : calloc(1, 1);
}

/* plain text as HTML (escaped, line breaks) - for mentions without markup */
char *md_escape(const char *text)
{
    Buf b = { NULL, 0, 0 };
    const char *p;
    for (p = text; *p; p++) {
        if (*p == '\n') puts_(&b, "<br>");
        else put_esc(&b, p, 1);
    }
    return b.s ? b.s : calloc(1, 1);
}
