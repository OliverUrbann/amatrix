/* amxrun: test runner inside an emulated AmigaOS 4 / MorphOS (or OS 3).
 *
 *   amxrun HOST PORT
 *
 * Asks the host (tests/emu/remote.py) for jobs over plain HTTP and does them:
 *   GET /next   ->  "PUT <path> <len> <crc32>\n<bytes>"   write a file
 *                   (a wrong checksum is reported with POST /bad <path>; the
 *                   emulated network has been seen to corrupt data)
 *                   "RUN <command line>\n"         start it; its output goes to /result when done
 *                                                  (several commands: separated by a backslash-n)
 *                   "SH <command line>\n"          run a short command now, output to /sh
 *                   "FETCH <path>\n"               send the file back (/file, or /nofile)
 *                   "WAIT\n"                       nothing to do yet
 *                   "QUIT\n"                       end
 * The host stays the only side that knows the test plan; the emulated system
 * only needs a network connection to it (no shared drive needed). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#ifdef __amigaos4__
#include <dos/obsolete.h>     /* DeleteFile (OS 4 renamed it) */
#endif
#include <proto/exec.h>
#include <proto/dos.h>

#include "net.h"

#ifdef AMX_TLS
int net_init_socket_only(void);
#define NET_START() net_init_socket_only()
#else
#define NET_START() net_init()
#endif

#define OUT_FILE "RAM:amxrun.out"

static const char *host;
static int port;
static long n_dummy;

/* one HTTP/1.0 request; returns the body (malloc'd, 0-terminated) */
static char *request(const char *method, const char *path, const char *body, long blen, long *rlen)
{
    char err[200], head[256], *buf = NULL, *p;
    long n = 0, cap = 0, r;
    net_conn *c = net_connect(host, port, 0, err, sizeof err);
    if (!c) return NULL;
    snprintf(head, sizeof head, "%s %s HTTP/1.0\r\nContent-Length: %ld\r\n\r\n", method, path, blen);
    if (net_write(c, head, strlen(head)) < 0 || (blen && net_write(c, body, blen) < 0)) { net_close(c); return NULL; }
    for (;;) {
        if (n + 65536 > cap) {
            char *nb = realloc(buf, cap = cap ? cap * 2 : 262144);
            if (!nb) { free(buf); net_close(c); return NULL; }
            buf = nb;
        }
        r = net_read(c, buf + n, cap - n - 1);
        if (r <= 0) break;
        n += r;
    }
    net_close(c);
    if (!buf) return NULL;
    buf[n] = 0;
    if (!(p = strstr(buf, "\r\n\r\n"))) { free(buf); return NULL; }
    p += 4;
    *rlen = n - (p - buf);
    memmove(buf, p, *rlen + 1);
    return buf;
}

static unsigned long crc32(const unsigned char *p, long n)
{
    unsigned long c = 0xFFFFFFFFUL;
    int k;
    while (n-- > 0) {
        c ^= *p++;
        for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320UL & (0UL - (c & 1)));
    }
    return c ^ 0xFFFFFFFFUL;
}

static void put_file(const char *path, const char *data, long len)
{
    BPTR f = Open((STRPTR)path, MODE_NEWFILE);
    if (f) {
        Write(f, (APTR)data, len);
        Close(f);
        SetProtection((STRPTR)path, 0);          /* rwed */
    } else
        printf("amxrun: cannot write %s\n", path);
}

/* the whole output file plus a tail line, POSTed to path */
static void post_file(const char *file, const char *path, const char *tail)
{
    char *res = NULL;
    long n = 0, cap = 0, r, rl;
    BPTR f = Open((STRPTR)file, MODE_OLDFILE);
    if (f) {
        for (;;) {                                  /* read all (no Seek: renamed on OS 4) */
            if (n + 4096 + strlen(tail) + 1 > cap) {
                char *nr = realloc(res, cap = cap ? cap * 2 : 65536);
                if (!nr) break;
                res = nr;
            }
            if ((r = Read(f, res + n, 4096)) <= 0) break;
            n += r;
        }
        Close(f);
    }
    if (!res && !(res = malloc(strlen(tail) + 1))) return;
    strcpy(res + n, tail);
    free(request("POST", path, res, n + strlen(tail), &rl));
    free(res);
}

static int busy;              /* a test is running (asynchronously) */

/* RUN: the test runs in its own shell, we keep serving jobs (so the host can
 * still look around when it hangs or crashes); a script reports the end */
static void run(const char *cmd)
{
    BPTR in, out, f = Open((STRPTR)"RAM:amxrun.job", MODE_NEWFILE);
    const char *p = cmd, *q;
    int first = 1;
    if (!f) return;
    for (;; first = 0) {                            /* "\n" (backslash n) separates commands */
        char part[1024];
        size_t n;
        q = strstr(p, "\\n");
        n = q ? (size_t)(q - p) : strlen(p);
        if (n >= sizeof part) n = sizeof part - 1;
        memcpy(part, p, n);
        part[n] = 0;
        FPrintf(f, (STRPTR)"%s >%s" OUT_FILE "\n", (LONG)part, (LONG)(first ? "" : ">"));
        if (!q) break;
        p = q + 2;
    }
    FPrintf(f, (STRPTR)"Echo \"# rc=$RC\" >>" OUT_FILE "\nEcho done >RAM:amxrun.done\n");
    Close(f);
    DeleteFile((STRPTR)"RAM:amxrun.done");
    in = Open((STRPTR)"NIL:", MODE_OLDFILE);
    out = Open((STRPTR)"NIL:", MODE_NEWFILE);
    if (SystemTags((STRPTR)"Execute RAM:amxrun.job", SYS_Input, in, SYS_Output, out, SYS_Asynch, TRUE,
                   NP_StackSize, 262144, TAG_DONE) == -1) {
        Close(in); Close(out);
        free(request("POST", "/result", "amxrun: could not start the test\n# rc=-1\n", 42, &n_dummy));
        return;
    }
    busy = 1;
}

static void check_done(void)
{
    BPTR l = Lock((STRPTR)"RAM:amxrun.done", SHARED_LOCK);
    if (!l) return;
    UnLock(l);
    DeleteFile((STRPTR)"RAM:amxrun.done");
    busy = 0;
    post_file(OUT_FILE, "/result", "");
}

/* SH: a short command, synchronously, output to /sh */
static void sh(const char *cmd)
{
    BPTR in = Open((STRPTR)"NIL:", MODE_OLDFILE), out = Open((STRPTR)"RAM:amxrun.sh", MODE_NEWFILE);
    LONG rc = SystemTags((STRPTR)cmd, SYS_Input, in, SYS_Output, out, NP_StackSize, 65536, TAG_DONE);
    char tail[40];
    Close(in);
    Close(out);
    snprintf(tail, sizeof tail, "\n# rc=%ld\n", (long)rc);
    post_file("RAM:amxrun.sh", "/sh", tail);
}

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: amxrun HOST PORT\n"); return 10; }
    host = argv[1];
    port = atoi(argv[2]);
    if (NET_START()) { printf("amxrun: no network\n"); return 20; }
    for (;;) {
        long len;
        char *job, *nl;
        if (busy) check_done();                     /* first: a finished test is reported at once */
        job = request("GET", "/next", NULL, 0, &len);
        if (!job) { Delay(100); continue; }
        nl = strchr(job, '\n');
        if (nl) *nl = 0;
        if (!strncmp(job, "PUT ", 4)) {
            char path[256];
            long sz = 0;
            unsigned long crc = 0;
            if (sscanf(job + 4, "%255s %ld %lx", path, &sz, &crc) == 3 && nl && sz <= len - (nl + 1 - job)
                && crc32((unsigned char *)nl + 1, sz) == crc)
                put_file(path, nl + 1, sz);
            else
                free(request("POST", "/bad", path, strlen(path), &n_dummy));
        } else if (!strncmp(job, "RUN ", 4))
            run(job + 4);
        else if (!strncmp(job, "SH ", 3))
            sh(job + 3);
        else if (!strncmp(job, "FETCH ", 6)) {
            BPTR l = Lock((STRPTR)job + 6, SHARED_LOCK);
            if (l) { UnLock(l); post_file(job + 6, "/file", ""); }
            else free(request("POST", "/nofile", "", 0, &n_dummy));
        }
        else if (!strcmp(job, "QUIT")) { free(job); break; }
        else Delay(busy ? 10 : 50);                 /* a test runs: look again soon */
        free(job);
    }
    net_cleanup();
    return 0;
}
