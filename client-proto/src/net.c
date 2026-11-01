#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __amigaos__
/* ------------------------------------------------------------------ AmigaOS */
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/socket.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

struct Library *SocketBase = NULL;

struct net_conn { long fd; int tls; };

int net_init(void)
{
    SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 4);
    return SocketBase ? 0 : -1;
}

void net_cleanup(void)
{
    if (SocketBase) { CloseLibrary(SocketBase); SocketBase = NULL; }
}

net_conn *net_connect(const char *host, int port, int tls, char *err, size_t errlen)
{
    struct hostent *he;
    struct sockaddr_in sa;
    net_conn *c;
    long fd;

    if (tls) { snprintf(err, errlen, "TLS not built in yet"); return NULL; }
    he = gethostbyname((STRPTR)host);
    if (!he) { snprintf(err, errlen, "cannot resolve %s", host); return NULL; }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { snprintf(err, errlen, "socket() failed"); return NULL; }
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    memcpy(&sa.sin_addr, he->h_addr, he->h_length);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        snprintf(err, errlen, "connect to %s:%d failed (errno %ld)", host, port, (long)Errno());
        CloseSocket(fd);
        return NULL;
    }
    c = calloc(1, sizeof *c);
    c->fd = fd;
    c->tls = tls;
    return c;
}

int net_write(net_conn *c, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        long n = send(c->fd, (APTR)p, len, 0);
        if (n <= 0) return -1;
        p += n; len -= n;
    }
    return 0;
}

long net_read(net_conn *c, void *buf, size_t len)
{
    long n = recv(c->fd, buf, len, 0);
    return n < 0 ? -1 : n;
}

void net_close(net_conn *c)
{
    if (!c) return;
    CloseSocket(c->fd);
    free(c);
}

#else
/* -------------------------------------------------------------- POSIX/Linux */
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

struct net_conn { int fd; SSL *ssl; };
static SSL_CTX *ctx;

int net_init(void)
{
    ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return -1;
    SSL_CTX_set_default_verify_paths(ctx);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    return 0;
}

void net_cleanup(void) { if (ctx) SSL_CTX_free(ctx); ctx = NULL; }

net_conn *net_connect(const char *host, int port, int tls, char *err, size_t errlen)
{
    struct addrinfo hints, *res, *ai;
    char ports[16];
    int fd = -1, r;
    net_conn *c;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(ports, sizeof ports, "%d", port);
    if ((r = getaddrinfo(host, ports, &hints, &res)) != 0) {
        snprintf(err, errlen, "cannot resolve %s: %s", host, gai_strerror(r));
        return NULL;
    }
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) { snprintf(err, errlen, "connect to %s:%d failed: %s", host, port, strerror(errno)); return NULL; }

    c = calloc(1, sizeof *c);
    c->fd = fd;
    if (tls) {
        c->ssl = SSL_new(ctx);
        SSL_set_fd(c->ssl, fd);
        SSL_set_tlsext_host_name(c->ssl, host);
        SSL_set1_host(c->ssl, host);
        if (SSL_connect(c->ssl) != 1) {
            snprintf(err, errlen, "TLS handshake with %s failed: %s", host,
                     ERR_reason_error_string(ERR_get_error()));
            net_close(c);
            return NULL;
        }
    }
    return c;
}

int net_write(net_conn *c, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        long n = c->ssl ? SSL_write(c->ssl, p, (int)len) : write(c->fd, p, len);
        if (n <= 0) return -1;
        p += n; len -= n;
    }
    return 0;
}

long net_read(net_conn *c, void *buf, size_t len)
{
    long n = c->ssl ? SSL_read(c->ssl, buf, (int)len) : read(c->fd, buf, len);
    if (n < 0 && c->ssl && SSL_get_error(c->ssl, (int)n) == SSL_ERROR_ZERO_RETURN) return 0;
    return n < 0 ? -1 : n;
}

void net_close(net_conn *c)
{
    if (!c) return;
    if (c->ssl) { SSL_shutdown(c->ssl); SSL_free(c->ssl); }
    close(c->fd);
    free(c);
}
#endif
