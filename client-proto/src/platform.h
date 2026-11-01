/* The few OS services the client needs, kept separate for the Amiga port. */
#ifndef AMATRIX_PLATFORM_H
#define AMATRIX_PLATFORM_H

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

int   plat_random(void *buf, size_t n);         /* 0 on success */
char *plat_read_file(const char *path);         /* malloc'd, NUL-terminated; NULL if missing */
int   plat_write_file(const char *path, const char *data); /* atomic replace; 0 on success */
int   plat_mkdir(const char *path);             /* 0 if it exists afterwards */
void  plat_sleep(int seconds);
/* Text for the console: UTF-8 as is on the host; on the Amiga converted to
 * ISO-8859-1 (characters outside Latin-1 become '?').  Caller frees. */
char *plat_display(const char *utf8);

#endif
