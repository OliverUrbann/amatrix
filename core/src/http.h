/* Minimal HTTP transport used by the Matrix client.
 *
 * The client only ever talks JSON to one homeserver, so the interface is
 * deliberately tiny. The Linux build implements it with libcurl
 * (http_curl.c); the Amiga build will implement the same functions on top of
 * bsdsocket.library + AmiSSL.
 */
#ifndef AMATRIX_HTTP_H
#define AMATRIX_HTTP_H

#include <stddef.h>

typedef struct {
    long status;      /* HTTP status, 0 on transport error */
    char *body;       /* NUL-terminated response body (malloc'd), may be NULL */
    size_t body_len;
} HttpResponse;

int  http_init(void);
void http_cleanup(void);

/* method: "GET", "POST", "PUT". url: absolute. bearer: access token or NULL.
 * body: request body (JSON) or NULL. timeout_s: overall timeout.
 * Returns 0 on success (any HTTP status), -1 on transport failure. */
int  http_request(const char *method, const char *url, const char *bearer,
                  const char *body, long timeout_s, HttpResponse *out);
void http_response_free(HttpResponse *r);

/* Why the last http_request() returned -1 (connect, TLS, ...). */
const char *http_last_error(void);

/* Percent-encode a path segment (room IDs, txn IDs). Caller frees. */
char *http_escape(const char *s);

#endif
