/* POSIX implementation of platform.h (Linux/macOS host build). */
#include "platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int plat_random(void *buf, size_t n)
{
    FILE *f = fopen("/dev/urandom", "rb");
    size_t got;
    if (!f) return -1;
    got = fread(buf, 1, n, f);
    fclose(f);
    return got == n ? 0 : -1;
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
    size_t len = strlen(path);
    char *tmp = malloc(len + 5);
    FILE *f;
    int ok;
    if (!tmp) return -1;
    sprintf(tmp, "%s.new", path);
    f = fopen(tmp, "wb");
    if (!f) { free(tmp); return -1; }
    ok = fwrite(data, 1, strlen(data), f) == strlen(data);
    ok = (fclose(f) == 0) && ok;
    if (ok) ok = rename(tmp, path) == 0;
    free(tmp);
    return ok ? 0 : -1;
}

int plat_mkdir(const char *path)
{
    if (mkdir(path, 0700) == 0 || errno == EEXIST) return 0;
    return -1;
}

void plat_sleep(int seconds) { sleep(seconds); }

char *plat_display(const char *utf8)
{
    char *r = malloc(strlen(utf8) + 1);
    if (r) strcpy(r, utf8);
    return r;
}
