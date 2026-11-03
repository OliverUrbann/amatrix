/* http.h on plain sockets (net.c): no libcurl.  This is the implementation
 * the Amiga uses (bsdsocket.library, AmiSSL for https); on Linux it runs on
 * POSIX sockets + OpenSSL, so the same code can be tested there first.
 * One connection per request (Connection: close). */
#include "http.h"
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static char last_err[200];
const char *http_last_error(void) { return last_err; }
static void seterr(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_err, sizeof last_err, fmt, ap);
    va_end(ap);
}

typedef struct { char *d; size_t len, cap; } Buf;

static int buf_add(Buf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 16384;
        char *nd;
        while (nc < b->len + n + 1) nc *= 2;
        nd = realloc(b->d, nc);
        if (!nd) return -1;
        b->d = nd;
        b->cap = nc;
    }
    memcpy(b->d + b->len, s, n);
    b->len += n;
    b->d[b->len] = 0;
    return 0;
}

static int lower_eq(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return 1;
}

static int header_value(const char *hdrs, const char *name, char *out, size_t ol)
{
    size_t nl = strlen(name);
    const char *p = hdrs;
    while ((p = strchr(p, '\n')) != NULL) {
        p++;
        if (lower_eq(p, name, nl) && p[nl] == ':') {
            const char *v = p + nl + 1;
            size_t n;
            while (*v == ' ') v++;
            n = strcspn(v, "\r\n");
            if (n >= ol) n = ol - 1;
            memcpy(out, v, n);
            out[n] = 0;
            return 1;
        }
    }
    return 0;
}

/* chunked transfer encoding, decoded in place; new length or -1 */
static long dechunk(char *s, size_t len)
{
    char *in = s, *out = s, *end = s + len;
    for (;;) {
        unsigned long n = strtoul(in, NULL, 16);
        char *nl = memchr(in, '\n', end - in);
        if (!nl) return -1;
        in = nl + 1;
        if (n == 0) break;
        if (in + n > end) return -1;
        memmove(out, in, n);
        out += n;
        in += n;
        if (in < end && *in == '\r') in++;
        if (in < end && *in == '\n') in++;
    }
    *out = 0;
    return out - s;
}

static int interruptible;
void http_interruptible(int on) { interruptible = on; net_interruptible(on); }

int http_init(void) { return net_init(); }
void http_cleanup(void) { net_cleanup(); }

int http_request(const char *method, const char *url, const char *bearer,
                 const char *body, long timeout_s, HttpResponse *out)
{
    char host[256], err[160], tmp[8192], val[64];
    const char *p, *path;
    char *hdr, *sep;
    int port, tls;
    size_t n, blen = body ? strlen(body) : 0;
    long r;
    net_conn *c;
    Buf b = { NULL, 0, 0 };

    (void)timeout_s;   /* the server bounds /sync with its own timeout */
    memset(out, 0, sizeof *out);
    last_err[0] = 0;
    if (!strncmp(url, "https://", 8)) { tls = 1; port = 443; p = url + 8; }
    else if (!strncmp(url, "http://", 7)) { tls = 0; port = 80; p = url + 7; }
    else { seterr("bad URL %s", url); return -1; }
    n = strcspn(p, ":/");
    if (n == 0 || n >= sizeof host) return -1;
    memcpy(host, p, n);
    host[n] = 0;
    if (p[n] == ':') port = atoi(p + n + 1);
    path = strchr(p, '/');
    if (!path) path = "/";

    if (net_wake_pending() && interruptible) { seterr("interrupted"); return -2; }
    c = net_connect(host, port, tls, err, sizeof err);
    if (!c) {
        seterr("%s", err);
        return interruptible && net_wake_pending() ? -2 : -1;
    }

    hdr = malloc(strlen(path) + strlen(host) + (bearer ? strlen(bearer) : 0) + 400);
    if (!hdr) { net_close(c); return -1; }
    n = sprintf(hdr, "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Amatrix/0.1\r\n"
                     "Accept: application/json\r\nConnection: close\r\n", method, path, host);
    if (bearer) n += sprintf(hdr + n, "Authorization: Bearer %s\r\n", bearer);
    if (body)   n += sprintf(hdr + n, "Content-Type: application/json\r\nContent-Length: %lu\r\n",
                             (unsigned long)blen);
    else if (strcmp(method, "GET")) n += sprintf(hdr + n, "Content-Length: 0\r\n");
    n += sprintf(hdr + n, "\r\n");
    if (net_write(c, hdr, n) || (blen && net_write(c, body, blen))) {
        seterr("send failed");
        free(hdr);
        net_close(c);
        return -1;
    }
    free(hdr);
    while ((r = net_read(c, tmp, sizeof tmp)) > 0)
        if (buf_add(&b, tmp, r)) { seterr("out of memory"); r = -1; break; }
    net_close(c);
    if (r == -2) { seterr("interrupted"); free(b.d); return -2; }

    if (!b.d || !(sep = strstr(b.d, "\r\n\r\n"))) {
        seterr("no response (%s)", r < 0 ? "read error" : "connection closed");
        free(b.d);
        return -1;
    }
    *sep = 0;
    p = strchr(b.d, ' ');
    out->status = p ? atol(p + 1) : 0;
    {
        char *bp = sep + 4;
        size_t bl = b.len - (bp - b.d);
        if (header_value(b.d, "Transfer-Encoding", val, sizeof val) && strstr(val, "chunked")) {
            long dl = dechunk(bp, bl);
            if (dl < 0) { seterr("bad chunked body"); free(b.d); return -1; }
            bl = dl;
        }
        out->body = malloc(bl + 1);
        if (!out->body) { free(b.d); return -1; }
        memcpy(out->body, bp, bl);
        out->body[bl] = 0;
        out->body_len = bl;
    }
    free(b.d);
    return 0;
}

void http_response_free(HttpResponse *r)
{
    free(r->body);
    r->body = NULL;
}

char *http_escape(const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    char *o = malloc(strlen(s) * 3 + 1), *q = o;
    if (!o) return NULL;
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~')
            *q++ = ch;
        else { *q++ = '%'; *q++ = hex[ch >> 4]; *q++ = hex[ch & 15]; }
    }
    *q = 0;
    return o;
}
