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

typedef struct net_conn net_conn;

int  net_init(void);                 /* once at startup; 0 = ok */
void net_cleanup(void);
net_conn *net_connect(const char *host, int port, int tls, char *err, size_t errlen);
int  net_write(net_conn *c, const void *buf, size_t len);   /* all or -1 */
long net_read(net_conn *c, void *buf, size_t len);          /* >0, 0 = EOF, -1 = error */
void net_close(net_conn *c);

#endif
