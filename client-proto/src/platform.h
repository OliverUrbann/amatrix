/* The few OS services the client needs, kept separate for the Amiga port. */
#ifndef AMATRIX_PLATFORM_H
#define AMATRIX_PLATFORM_H

#include <stddef.h>

int   plat_random(void *buf, size_t n);         /* 0 on success */
char *plat_read_file(const char *path);         /* malloc'd, NUL-terminated; NULL if missing */
int   plat_write_file(const char *path, const char *data); /* atomic replace; 0 on success */
int   plat_mkdir(const char *path);             /* 0 if it exists afterwards */
void  plat_sleep(int seconds);

#endif
