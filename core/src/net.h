/* net: one TCP connection, optionally TLS.  POSIX sockets + OpenSSL on Linux,
 * bsdsocket.library (+ AmiSSL later) on AmigaOS.  Same API on both. */
#ifndef AMX_NET_H
#define AMX_NET_H
#include <stddef.h>
#include <stdio.h>

/* Error output: on AmigaOS 3 the shell cannot redirect stderr, so errors go
 * to stdout there and end up in the same log file as everything else. */
#ifndef AMX_ERR
#ifdef __amigaos__
#define AMX_ERR stdout
#else
#define AMX_ERR stderr
#endif
#endif

/* TLS: AmiSSL (AmigaOS 3/4) or OpenSSL linked in (MorphOS, AMX_OPENSSL) */
#if defined(AMX_AMISSL) || defined(AMX_OPENSSL)
#define AMX_TLS
#endif

typedef struct net_conn net_conn;

int  net_init(void);                 /* once at startup; 0 = ok */
void net_cleanup(void);
net_conn *net_connect(const char *host, int port, int tls, char *err, size_t errlen);
int  net_write(net_conn *c, const void *buf, size_t len);   /* all or -1 */
long net_read(net_conn *c, void *buf, size_t len);          /* >0, 0 = EOF, -1 = error */
void net_close(net_conn *c);

/* Interruptible reads for the async layer (only the network process uses
 * the net layer).  net_set_wakeup(): Amiga: signal mask (it becomes the
 * bsdsocket break mask while interruptible); POSIX: fd that becomes
 * readable.  While net_interruptible(1), a wake-up makes net_read() return
 * -2.  net_wake_pending(): a wake-up is waiting (without consuming it). */
void net_set_wakeup(long fd_or_sigmask);
void net_interruptible(int on);
int  net_wake_pending(void);

#endif
