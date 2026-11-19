#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "mui_util.h"

/* The tag list is copied out of the va_list: works on every ABI (m68k
 * passes varargs on the stack, PowerPC partly in registers). */
Object *MUI_NewObject(CONST_STRPTR cl, Tag tag1, ...)
{
    struct TagItem buf[64], *t = buf, *big = NULL;
    Object *o;
    va_list ap;
    int n = 0, max = 64;
    Tag tag = tag1;

    va_start(ap, tag1);
    for (;;) {
        if (n == max - 1) {                       /* long lists (big windows): grow */
            struct TagItem *nt = malloc(sizeof *nt * max * 2);
            if (!nt) break;
            memcpy(nt, t, sizeof *nt * n);
            free(big);
            t = big = nt;
            max *= 2;
        }
        t[n].ti_Tag = tag;
        if (tag == TAG_DONE) break;
        t[n].ti_Data = va_arg(ap, ULONG);
        n++;
        tag = va_arg(ap, Tag);
    }
    va_end(ap);
    t[n].ti_Tag = TAG_DONE;
    o = MUI_NewObjectA(cl, t);
    free(big);
    return o;
}

LONG MUI_Request(Object *app, Object *win, ULONG flags, CONST_STRPTR title, CONST_STRPTR gadgets,
                 CONST_STRPTR format, ULONG tag1, ...)
{
    ULONG args[16];
    const char *p;
    int n = 1, i;
    va_list ap;

    for (p = format; p && *p; p++)                 /* one argument per % (not %%) */
        if (*p == '%') { if (p[1] == '%') p++; else n++; }
    if (n > 16) n = 16;
    args[0] = tag1;
    va_start(ap, tag1);
    for (i = 1; i < n; i++) args[i] = va_arg(ap, ULONG);
    va_end(ap);
    return MUI_RequestA(app, win, flags, title, gadgets, format, args);
}
