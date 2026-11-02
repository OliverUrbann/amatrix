/* amxdiag: find which part of amatrix-recv upsets the system on exit.
 *
 *   amxdiag <step> [url]
 *     socket   open + close bsdsocket.library
 *     amissl   + open AmiSSL (OpenAmiSSLTags), SSL_CTX, close again
 *     random   + 1 KB from RAND_bytes (key material path)
 *     https    + one https GET <url>/_matrix/client/versions (TLS handshake)
 *     https5   + five of them (amatrix-recv opens one connection per request)
 *     file     write/replace/read/delete a file in the current drawer
 *
 * Every step prints what it does before and after, so if a requester pops
 * up the last line on screen names the culprit.  Each step also cleans up
 * completely and exits; run them one after another (amxdiag-run script).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"
#include "net.h"
#include "platform.h"

#define SAY(...) do { printf(__VA_ARGS__); putchar('\n'); fflush(stdout); } while (0)

#ifdef AMX_AMISSL
int net_random(void *buf, size_t n);
#endif

int main(int argc, char **argv)
{
    const char *step = argc > 1 ? argv[1] : "";
    const char *url = argc > 2 ? argv[2] : "https://matrix.org";
    char full[512];
    int i, n = 1;

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!strcmp(step, "file")) {
        char *r;
        SAY("[file] write amxdiag.tmp");
        if (plat_write_file("amxdiag.tmp", "first\n")) { SAY("[file] write failed"); return 5; }
        SAY("[file] replace amxdiag.tmp");
        if (plat_write_file("amxdiag.tmp", "second, longer content\n")) { SAY("[file] replace failed"); return 5; }
        r = plat_read_file("amxdiag.tmp");
        SAY("[file] read back: %s", r && !strcmp(r, "second, longer content\n") ? "ok" : "WRONG");
        free(r);
        remove("amxdiag.tmp");
        SAY("[file] exiting");
        return 0;
    }
    if (strcmp(step, "socket") && strcmp(step, "amissl") && strcmp(step, "random") &&
        strcmp(step, "https") && strcmp(step, "https5")) {
        fprintf(stderr, "usage: amxdiag socket|amissl|random|https|https5|file [server-url]\n");
        return 10;
    }

    SAY("[%s] opening network (bsdsocket%s)", step,
#ifdef AMX_AMISSL
        strcmp(step, "socket") ? " + AmiSSL" : ""
#else
        ""
#endif
    );
#ifdef AMX_AMISSL
    if (!strcmp(step, "socket")) {
        /* bsdsocket only: bypass net_init's AmiSSL part */
        extern int net_init_socket_only(void);
        if (net_init_socket_only()) { SAY("[socket] open failed"); return 5; }
    } else
#endif
    if (http_init()) { SAY("[%s] network init failed", step); return 5; }
    SAY("[%s] network open", step);

#ifdef AMX_AMISSL
    if (!strcmp(step, "random")) {
        unsigned char buf[1024];
        SAY("[random] RAND_bytes(1024) -> %s", net_random(buf, sizeof buf) == 0 ? "ok" : "FAILED");
    }
#endif
    if (!strcmp(step, "https5")) n = 5;
    if (!strncmp(step, "https", 5)) {
        snprintf(full, sizeof full, "%s/_matrix/client/versions", url);
        for (i = 0; i < n; i++) {
            HttpResponse r;
            SAY("[%s] GET %s (%d/%d)", step, full, i + 1, n);
            if (http_request("GET", full, NULL, NULL, 30, &r)) { SAY("[%s] request failed", step); continue; }
            SAY("[%s] HTTP %ld, %lu bytes", step, r.status, (unsigned long)r.body_len);
            http_response_free(&r);
        }
    }
    SAY("[%s] closing network", step);
    http_cleanup();
    SAY("[%s] network closed, exiting", step);
    return 0;
}
