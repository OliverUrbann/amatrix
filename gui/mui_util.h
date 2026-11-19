/* Helpers around the MUI headers for m68k-amigaos-gcc. */
#ifndef AMX_MUI_UTIL_H
#define AMX_MUI_UTIL_H
#include <proto/intuition.h>
#include <proto/muimaster.h>

/* The generated inline header makes MUI_NewObject a function-like macro,
 * which cannot work with the classic "XxxObject, ..., End" macros (the
 * argument list is only closed by End).  Use a real varargs function. */
#undef MUI_NewObject
Object *MUI_NewObject(CONST_STRPTR cl, Tag tag1, ...);
#undef MUI_Request
LONG MUI_Request(Object *app, Object *win, ULONG flags, CONST_STRPTR title, CONST_STRPTR gadgets,
                CONST_STRPTR format, ULONG tag1, ...);
#ifndef __amigaos4__
#include <proto/alib.h>             /* DoMethod */
#endif

#ifndef MAKE_ID
#define MAKE_ID(a,b,c,d) ((ULONG)(a)<<24 | (ULONG)(b)<<16 | (ULONG)(c)<<8 | (ULONG)(d))
#endif

static inline ULONG xget(Object *obj, ULONG attr)
{
    ULONG v = 0;
    GetAttr(attr, obj, &v);
    return v;
}
#endif
