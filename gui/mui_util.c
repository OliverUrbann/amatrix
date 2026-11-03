#include "mui_util.h"

/* m68k passes all arguments on the stack: the tags follow tag1 in memory.
 * noinline/noipa keep gcc from optimising that layout away. */
__attribute__((noinline, noipa))
Object *MUI_NewObject(CONST_STRPTR cl, Tag tag1, ...)
{
    return MUI_NewObjectA(cl, (struct TagItem *)&tag1);
}

__attribute__((noinline, noipa))
LONG MUI_Request(Object *app, Object *win, ULONG flags, CONST_STRPTR title, CONST_STRPTR gadgets,
                 CONST_STRPTR format, ULONG tag1, ...)
{
    return MUI_RequestA(app, win, flags, title, gadgets, format, (APTR)&tag1);
}
