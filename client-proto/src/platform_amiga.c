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

/* TEST ONLY.  A plain AmigaOS 3 machine has no entropy source.  This hashes
 * clock readings, the date stamp, free memory and a counter with SHA-256.
 * Good enough to get a working device for tests, NOT for real keys: the
 * AmiSSL build will use RAND_bytes() instead. */
int plat_random(void *buf, size_t n)
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
    return 0;
}

char *plat_read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *b;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc(n + 1);
    if (!b || fread(b, 1, n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    b[n] = 0;
    fclose(f);
    return b;
}

int plat_write_file(const char *path, const char *data)
{
    char *tmp = malloc(strlen(path) + 5);
    FILE *f;
    int ok;
    if (!tmp) return -1;
    sprintf(tmp, "%s.new", path);
    f = fopen(tmp, "wb");
    if (!f) { free(tmp); return -1; }
    ok = fwrite(data, 1, strlen(data), f) == strlen(data);
    ok = (fclose(f) == 0) && ok;
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
