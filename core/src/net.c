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
#include <dos/dos.h>
#include <sys/ioctl.h>
#include <sys/filio.h>

#ifdef AMX_AMISSL
#include <libraries/amisslmaster.h>
#include <libraries/amissl.h>
#include <amissl/amissl.h>
#include <proto/amisslmaster.h>
#include <proto/amissl.h>
struct Library *AmiSSLMasterBase = NULL, *AmiSSLBase = NULL, *AmiSSLExtBase = NULL;
#endif
#ifdef AMX_TLS                  /* AmiSSL, or OpenSSL linked in (MorphOS) */
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/x509v3.h>
static SSL_CTX *ctx;
static int tls_open;
#endif

struct Library *SocketBase = NULL;

/* AmigaOS 4 calls libraries through interfaces, fetched here after opening
 * (libauto's ISocket stays empty: we open bsdsocket.library ourselves) */
#ifdef __amigaos4__
#define SOCKET_IFACE_GET() (ISocket = (struct SocketIFace *)GetInterface(SocketBase, "main", 1, NULL))
#else
#define SOCKET_IFACE_GET() ((void)0)
#endif
#if defined(AMX_AMISSL) && defined(__amigaos4__)
struct AmiSSLMasterIFace *IAmiSSLMaster;
struct AmiSSLIFace *IAmiSSL;
#define MASTER_IFACE_GET() (IAmiSSLMaster = (struct AmiSSLMasterIFace *)GetInterface(AmiSSLMasterBase, "main", 1, NULL))
#define AMISSL_OS_TAGS     AmiSSL_ISocket, (ULONG)ISocket, AmiSSL_GetIAmiSSL, (ULONG)&IAmiSSL
#else
#define MASTER_IFACE_GET() ((void)0)
#define AMISSL_OS_TAGS     AmiSSL_SocketBase, (ULONG)SocketBase
#endif
static ULONG wake_mask;
static int intr_on;

void net_set_wakeup(long m) { wake_mask = (ULONG)m; }
void net_interruptible(int on) { intr_on = on && wake_mask; }
int net_wake_pending(void) { return wake_mask && (SetSignal(0, 0) & wake_mask); }

struct net_conn {
    long fd;
#ifdef AMX_TLS
    SSL *ssl;
#endif
};

int net_init(void)
{
#ifdef AMX_AMISSL
    LONG rc;
#endif
    SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 4);
    if (!SocketBase) {
        fprintf(AMX_ERR, "net: cannot open bsdsocket.library (TCP/IP stack running?)\n");
        return -1;
    }
    SOCKET_IFACE_GET();
#ifdef AMX_AMISSL
    AmiSSLMasterBase = OpenLibrary((STRPTR)"amisslmaster.library", AMISSLMASTER_MIN_VERSION);
    if (!AmiSSLMasterBase) {
        fprintf(AMX_ERR, "net: cannot open amisslmaster.library (AmiSSL 5 installed?)\n");
        return -1;
    }
    MASTER_IFACE_GET();
    if ((rc = OpenAmiSSLTags(AMISSL_CURRENT_VERSION,
                       AmiSSL_UsesOpenSSLStructs, FALSE,
                       AmiSSL_GetAmiSSLBase, (ULONG)&AmiSSLBase,
                       AmiSSL_GetAmiSSLExtBase, (ULONG)&AmiSSLExtBase,
                       AMISSL_OS_TAGS,
                       AmiSSL_ErrNoPtr, (ULONG)&errno,
                       TAG_DONE)) != 0) {
        fprintf(AMX_ERR, "net: OpenAmiSSLTags failed (%ld, AmiSSL too old?)\n", (long)rc);
        return -1;
    }
#endif
#ifdef AMX_TLS
    tls_open = 1;
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
#ifdef AMX_TLS
    if (ctx) { SSL_CTX_free(ctx); ctx = NULL; }
#endif
#ifdef AMX_AMISSL
    if (tls_open) CloseAmiSSL();
    if (AmiSSLMasterBase) { CloseLibrary(AmiSSLMasterBase); AmiSSLMasterBase = NULL; }
#endif
#ifdef AMX_TLS
    tls_open = 0;
#endif
    if (SocketBase) { CloseLibrary(SocketBase); SocketBase = NULL; }
}

#ifdef AMX_TLS
/* amxdiag: bsdsocket only, no AmiSSL */
int net_init_socket_only(void)
{
    SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 4);
    if (SocketBase) SOCKET_IFACE_GET();
    return SocketBase ? 0 : -1;
}

/* used by platform_amiga.c for key material */
int net_random(void *buf, size_t n)
{
    if (!tls_open) return -1;
    return RAND_bytes(buf, (int)n) == 1 ? 0 : -1;
}
void net_random_seed(const void *buf, size_t n)
{
    if (tls_open) RAND_add(buf, (int)n, 0.0);
}
#endif

net_conn *net_connect(const char *host, int port, int tls, char *err, size_t errlen)
{
    struct hostent *he;
    struct sockaddr_in sa;
    net_conn *c;
    long fd;

#ifndef AMX_TLS
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
#ifdef AMX_TLS
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
#ifdef AMX_TLS
        if (c->ssl) n = SSL_write(c->ssl, p, (int)len); else
#endif
        n = send(c->fd, (APTR)p, len, 0);
        if (n <= 0) return -1;
        p += n; len -= n;
    }
    return 0;
}

/* interruptible read: WaitSelect() on the socket and the wake-up signal,
 * non-blocking socket (TLS records without data must not block SSL_read) */
static long read_intr(net_conn *c, void *buf, size_t len)
{
    long one = 1, n;
    IoctlSocket(c->fd, FIONBIO, (char *)&one);
    for (;;) {
#ifdef AMX_TLS
        if (!(c->ssl && SSL_pending(c->ssl)))
#endif
        {
            fd_set rd;
            ULONG sig = wake_mask;
            long r;
            FD_ZERO(&rd);
            FD_SET(c->fd, &rd);
            r = WaitSelect(c->fd + 1, &rd, NULL, NULL, NULL, &sig);
            if (r == 0 && (sig & wake_mask)) return -2;
            if (r < 0) return -1;
        }
#ifdef AMX_TLS
        if (c->ssl) {
            n = SSL_read(c->ssl, buf, (int)len);
            if (n > 0) return n;
            switch (SSL_get_error(c->ssl, (int)n)) {
            case SSL_ERROR_WANT_READ: case SSL_ERROR_WANT_WRITE: continue;
            case SSL_ERROR_ZERO_RETURN: case SSL_ERROR_SYSCALL: case SSL_ERROR_SSL: return 0;
            default: return -1;
            }
        }
#endif
        n = recv(c->fd, buf, len, 0);
        if (n >= 0) return n;
        if (Errno() == EWOULDBLOCK || Errno() == EINTR) continue;
        return -1;
    }
}

long net_read(net_conn *c, void *buf, size_t len)
{
    long n;
    if (intr_on) return read_intr(c, buf, len);
#ifdef AMX_TLS
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
#ifdef AMX_TLS
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

#include <poll.h>
#include <fcntl.h>
struct net_conn { int fd; SSL *ssl; };
static SSL_CTX *ctx;
static int wake_fd = -1, intr_on;

void net_set_wakeup(long fd) { wake_fd = (int)fd; }
void net_interruptible(int on) { intr_on = on && wake_fd >= 0; }
int net_wake_pending(void)
{
    struct pollfd p;
    if (wake_fd < 0) return 0;
    p.fd = wake_fd; p.events = POLLIN; p.revents = 0;
    return poll(&p, 1, 0) > 0 && (p.revents & POLLIN);
}

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
    long n;
    if (intr_on) {
        /* non-blocking, so that TLS records without data (session tickets)
         * cannot leave us stuck in SSL_read() */
        fcntl(c->fd, F_SETFL, fcntl(c->fd, F_GETFL) | O_NONBLOCK);
        for (;;) {
            if (!(c->ssl && SSL_pending(c->ssl))) {
                struct pollfd p[2];
                p[0].fd = c->fd; p[0].events = POLLIN; p[0].revents = 0;
                p[1].fd = wake_fd; p[1].events = POLLIN; p[1].revents = 0;
                while (poll(p, 2, -1) < 0 && errno == EINTR) ;
                if (p[1].revents & POLLIN) return -2;
            }
            n = c->ssl ? SSL_read(c->ssl, buf, (int)len) : read(c->fd, buf, len);
            if (n > 0) return n;
            if (c->ssl) {
                int e = SSL_get_error(c->ssl, (int)n);
                if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) continue;
                return e == SSL_ERROR_ZERO_RETURN || n == 0 ? 0 : -1;
            }
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            return n < 0 ? -1 : 0;
        }
    }
    n = c->ssl ? SSL_read(c->ssl, buf, (int)len) : read(c->fd, buf, len);
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
