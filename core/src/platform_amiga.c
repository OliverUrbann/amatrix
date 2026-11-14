/* AmigaOS implementation of platform.h (libnix). */
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "olm/olm.h"

#ifdef AMX_AMISSL
int  net_random(void *buf, size_t n);              /* net.c, AmiSSL RAND_bytes */
void net_random_seed(const void *buf, size_t n);
#endif

/* Weak fallback mixer: hashes clock readings, the date stamp, free memory and
 * a counter with SHA-256.  In the AmiSSL build this only adds to AmiSSL's
 * pool and the bytes come from RAND_bytes(); without AmiSSL (plain HTTP test
 * build) it is the only source and NOT fit for real keys. */
static void weak_random(void *buf, size_t n)
{
    static unsigned long counter;
    static OlmUtility *u;
    unsigned char *o = buf;
    struct {
        struct DateStamp ds;
        clock_t c;
        unsigned long cnt, chip, fast;
        void *sp;
        unsigned char prev[32];
    } seed;
    char hash[64];   /* unpadded base64 of the 32-byte digest */

    if (!u) u = olm_utility(malloc(olm_utility_size()));
    memset(&seed, 0, sizeof seed);
    while (n) {
        size_t k;
        DateStamp(&seed.ds);
        seed.c = clock();
        seed.cnt = ++counter;
        seed.chip = AvailMem(MEMF_CHIP);
        seed.fast = AvailMem(MEMF_FAST);
        seed.sp = &seed;
        olm_sha256(u, &seed, sizeof seed, hash, sizeof hash);
        for (k = 0; k < 32; k++) seed.prev[k] = (unsigned char)hash[k];
        for (k = 0; k < 16 && n; k++, n--)
            *o++ = (unsigned char)((hash[2 * k] << 4) ^ hash[2 * k + 1] ^ hash[k + 32]);
    }
}

int plat_random(void *buf, size_t n)
{
#ifdef AMX_AMISSL
    unsigned char extra[32];
    weak_random(extra, sizeof extra);
    net_random_seed(extra, sizeof extra);
    if (net_random(buf, n) == 0) return 0;
    fprintf(AMX_ERR, "AmiSSL random source not available\n");
    return -1;
#else
    static int warned;
    if (!warned) { fprintf(AMX_ERR, "WARNING: test build without AmiSSL, keys use a weak random source\n"); warned = 1; }
    weak_random(buf, n);
    return 0;
#endif
}

/* Files through dos.library, not stdio: libnix keeps one list of open FILEs
 * for the whole program, and the network process and the GUI work on files
 * at the same time - stdio from both left handles open (and files locked). */
int plat_read_bin(const char *path, unsigned char **out, long *len, long max)
{
    BPTR f = Open((STRPTR)path, MODE_OLDFILE);
    LONG n;
    unsigned char *b;
    *out = NULL;
    *len = 0;
    if (!f) return -1;
    if (Seek(f, 0, OFFSET_END) < 0 || (n = Seek(f, 0, OFFSET_BEGINNING)) < 0) { Close(f); return -3; }
    if (max > 0 && n > max) { Close(f); return -2; }
    if (!(b = malloc(n + 1))) { Close(f); return -3; }
    if (n && Read(f, b, n) != n) { free(b); Close(f); return -3; }
    Close(f);
    b[n] = 0;
    *out = b;
    *len = n;
    return 0;
}

int plat_write_bin(const char *path, const void *data, size_t n)
{
    BPTR f = Open((STRPTR)path, MODE_NEWFILE);
    int ok;
    if (!f) return -1;
    ok = !n || Write(f, (APTR)data, (LONG)n) == (LONG)n;
    if (!Close(f)) ok = 0;
    return ok ? 0 : -1;
}

char *plat_read_file(const char *path)
{
    unsigned char *b;
    long n;
    return plat_read_bin(path, &b, &n, 0) == 0 ? (char *)b : NULL;
}

int plat_write_file(const char *path, const char *data)
{
    char *tmp = malloc(strlen(path) + 5);
    int ok;
    if (!tmp) return -1;
    sprintf(tmp, "%s.new", path);
    ok = plat_write_bin(tmp, data, strlen(data)) == 0;
    if (ok) {
        DeleteFile((STRPTR)path);                 /* Rename() does not replace */
        ok = Rename((STRPTR)tmp, (STRPTR)path) != 0;
    }
    free(tmp);
    return ok ? 0 : -1;
}

int plat_mkdir(const char *path)
{
    BPTR l = Lock((STRPTR)path, ACCESS_READ);
    if (l) { UnLock(l); return 0; }
    l = CreateDir((STRPTR)path);
    if (!l) return -1;
    UnLock(l);
    return 0;
}

void plat_sleep(int seconds) { Delay(seconds * 50); }

/* dos.library directly, no stdio buffer: the line is on disk when we return */
void plat_append_line(const char *path, const char *line)
{
    BPTR f = Open((STRPTR)path, MODE_READWRITE);
    if (!f) return;
    Seek(f, 0, OFFSET_END);
    Write(f, (APTR)line, strlen(line));
    Write(f, (APTR)"\n", 1);
    Close(f);
}

char *plat_display(const char *utf8)
{
    const unsigned char *s = (const unsigned char *)utf8;
    char *r = malloc(strlen(utf8) + 1), *o = r;
    if (!r) return NULL;
    while (*s) {
        unsigned long cp;
        int extra;
        if (*s < 0x80) { *o++ = *s++; continue; }
        if ((*s & 0xE0) == 0xC0) { cp = *s & 0x1F; extra = 1; }
        else if ((*s & 0xF0) == 0xE0) { cp = *s & 0x0F; extra = 2; }
        else if ((*s & 0xF8) == 0xF0) { cp = *s & 0x07; extra = 3; }
        else { *o++ = '?'; s++; continue; }
        s++;
        while (extra-- && (*s & 0xC0) == 0x80) cp = (cp << 6) | (*s++ & 0x3F);
        if (cp == 0x2013 || cp == 0x2014) *o++ = '-';          /* dashes */
        else if (cp == 0x2018 || cp == 0x2019) *o++ = '\'';    /* quotes */
        else if (cp == 0x201C || cp == 0x201D) *o++ = '"';
        else if (cp == 0x2026) { *o++ = '.'; }                  /* ellipsis */
        else *o++ = cp < 0x100 ? (char)cp : '?';
    }
    *o = 0;
    return r;
}

char *plat_from_local(const char *local)
{
    const unsigned char *s = (const unsigned char *)local;
    char *r = malloc(strlen(local) * 2 + 1), *o = r;
    if (!r) return NULL;
    for (; *s; s++) {
        if (*s < 0x80) *o++ = *s;
        else { *o++ = (char)(0xC0 | (*s >> 6)); *o++ = (char)(0x80 | (*s & 0x3F)); }
    }
    *o = 0;
    return r;
}
