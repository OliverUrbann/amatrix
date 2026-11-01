#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __amigaos__
/* ------------------------------------------------------------------ AmigaOS */
#include <errno.h>
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/socket.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#ifdef AMX_AMISSL
#include <libraries/amisslmaster.h>
#include <libraries/amissl.h>
#include <amissl/amissl.h>
#include <proto/amisslmaster.h>
#include <proto/amissl.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/x509v3.h>
struct Library *AmiSSLMasterBase = NULL, *AmiSSLBase = NULL, *AmiSSLExtBase = NULL;
static SSL_CTX *ctx;
static int amissl_open;
#endif

struct Library *SocketBase = NULL;

struct net_conn {
    long fd;
#ifdef AMX_AMISSL
    SSL *ssl;
#endif
};

int net_init(void)
{
    SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 4);
    if (!SocketBase) {
        fprintf(AMX_ERR, "net: cannot open bsdsocket.library (TCP/IP stack running?)\n");
        return -1;
    }
#ifdef AMX_AMISSL
    AmiSSLMasterBase = OpenLibrary((STRPTR)"amisslmaster.library", AMISSLMASTER_MIN_VERSION);
    if (!AmiSSLMasterBase) {
        fprintf(AMX_ERR, "net: cannot open amisslmaster.library (AmiSSL 5 installed?)\n");
        return -1;
    }
    if (OpenAmiSSLTags(AMISSL_CURRENT_VERSION,
                       AmiSSL_UsesOpenSSLStructs, FALSE,
                       AmiSSL_GetAmiSSLBase, (ULONG)&AmiSSLBase,
                       AmiSSL_GetAmiSSLExtBase, (ULONG)&AmiSSLExtBase,
                       AmiSSL_SocketBase, (ULONG)SocketBase,
                       AmiSSL_ErrNoPtr, (ULONG)&errno,
                       TAG_DONE) != 0) {
        fprintf(AMX_ERR, "net: OpenAmiSSLTags failed (AmiSSL too old?)\n");
        return -1;
    }
    amissl_open = 1;
    ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) { fprintf(AMX_ERR, "net: SSL_CTX_new failed\n"); return -1; }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_default_verify_paths(ctx);          /* AmiSSL:Certs */
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
#endif
    return 0;
}

void net_cleanup(void)
{
#ifdef AMX_AMISSL
    if (ctx) { SSL_CTX_free(ctx); ctx = NULL; }
    if (amissl_open) { CloseAmiSSL(); amissl_open = 0; }
    if (AmiSSLMasterBase) { CloseLibrary(AmiSSLMasterBase); AmiSSLMasterBase = NULL; }
#endif
    if (SocketBase) { CloseLibrary(SocketBase); SocketBase = NULL; }
}

#ifdef AMX_AMISSL
/* used by platform_amiga.c for key material */
int net_random(void *buf, size_t n)
{
    if (!amissl_open) return -1;
    return RAND_bytes(buf, (int)n) == 1 ? 0 : -1;
}
void net_random_seed(const void *buf, size_t n)
{
    if (amissl_open) RAND_add(buf, (int)n, 0.0);
}
#endif

net_conn *net_connect(const char *host, int port, int tls, char *err, size_t errlen)
{
    struct hostent *he;
    struct sockaddr_in sa;
    net_conn *c;
    long fd;

#ifndef AMX_AMISSL
    if (tls) { snprintf(err, errlen, "https needs the AmiSSL build"); return NULL; }
#endif
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
#ifdef AMX_AMISSL
    if (tls) {
        long vr;
        c->ssl = SSL_new(ctx);
        SSL_set_fd(c->ssl, (int)fd);
        SSL_set_tlsext_host_name(c->ssl, host);
        SSL_set1_host(c->ssl, host);
        if (SSL_connect(c->ssl) != 1) {
            vr = SSL_get_verify_result(c->ssl);
            if (vr != X509_V_OK)
                snprintf(err, errlen, "TLS: certificate of %s not accepted: %s", host,
                         X509_verify_cert_error_string(vr));
            else
                snprintf(err, errlen, "TLS handshake with %s failed: %s", host,
                         ERR_reason_error_string(ERR_get_error()));
            net_close(c);
            return NULL;
        }
    }
#endif
    return c;
}

int net_write(net_conn *c, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        long n;
#ifdef AMX_AMISSL
        if (c->ssl) n = SSL_write(c->ssl, p, (int)len); else
#endif
        n = send(c->fd, (APTR)p, len, 0);
        if (n <= 0) return -1;
        p += n; len -= n;
    }
    return 0;
}

long net_read(net_conn *c, void *buf, size_t len)
{
    long n;
#ifdef AMX_AMISSL
    if (c->ssl) {
        n = SSL_read(c->ssl, buf, (int)len);
        if (n <= 0) {
            int e = SSL_get_error(c->ssl, (int)n);
            /* many servers just close the TCP connection: treat as EOF */
            if (e == SSL_ERROR_ZERO_RETURN || e == SSL_ERROR_SYSCALL || e == SSL_ERROR_SSL) return 0;
            return -1;
        }
        return n;
    }
#endif
    n = recv(c->fd, buf, len, 0);
    return n < 0 ? -1 : n;
}

void net_close(net_conn *c)
{
    if (!c) return;
#ifdef AMX_AMISSL
    if (c->ssl) { SSL_shutdown(c->ssl); SSL_free(c->ssl); }
#endif
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
