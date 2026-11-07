/* Generated with sfdc from openurl_lib.sfd of libopenurl
 * (https://github.com/jens-maus/libopenurl, LGPL); only the call macros. */
/* Automatically generated header (sfdc 1.13)! Do not edit! */

#ifndef _INLINE_OPENURL_H
#define _INLINE_OPENURL_H

#ifndef _SFDC_VARARG_DEFINED
#define _SFDC_VARARG_DEFINED
#ifdef __HAVE_IPTR_ATTR__
typedef APTR _sfdc_vararg __attribute__((iptr));
#else
typedef ULONG _sfdc_vararg;
#endif /* __HAVE_IPTR_ATTR__ */
#endif /* _SFDC_VARARG_DEFINED */

#ifndef __INLINE_MACROS_H
#include <inline/macros.h>
#endif /* !__INLINE_MACROS_H */

#ifndef OPENURL_BASE_NAME
#define OPENURL_BASE_NAME OpenURLBase
#endif /* !OPENURL_BASE_NAME */

#define URL_OpenA(___url, ___tags) \
      LP2(0x1e, ULONG, URL_OpenA , STRPTR, ___url, a0, struct TagItem *, ___tags, a1,\
      , OPENURL_BASE_NAME)

#ifndef NO_INLINE_STDARG
#define URL_Open(___url, ___last, ...) \
    ({_sfdc_vararg _tags[] = { ___last, __VA_ARGS__ }; URL_OpenA((___url), (struct TagItem *) _tags); })
#endif /* !NO_INLINE_STDARG */

#define URL_OldGetPrefs() \
      LP0(0x24, struct URL_Prefs *, URL_OldGetPrefs ,\
      , OPENURL_BASE_NAME)

#define URL_OldFreePrefs(___up) \
      LP1NR(0x2a, URL_OldFreePrefs , struct URL_Prefs *, ___up, a0,\
      , OPENURL_BASE_NAME)

#define URL_OldSetPrefs(___up, ___permanent) \
      LP2(0x30, ULONG, URL_OldSetPrefs , struct URL_Prefs *, ___up, a0, BOOL, ___permanent, d0,\
      , OPENURL_BASE_NAME)

#define URL_OldGetDefaultPrefs() \
      LP0(0x36, struct URL_Prefs *, URL_OldGetDefaultPrefs ,\
      , OPENURL_BASE_NAME)

#define URL_OldLaunchPrefsApp() \
      LP0(0x3c, ULONG, URL_OldLaunchPrefsApp ,\
      , OPENURL_BASE_NAME)

#define URL_GetPrefsA(___tags) \
      LP1(0x48, struct URL_Prefs *, URL_GetPrefsA , struct TagItem *, ___tags, a0,\
      , OPENURL_BASE_NAME)

#ifndef NO_INLINE_STDARG
#define URL_GetPrefs(___last, ...) \
    ({_sfdc_vararg _tags[] = { ___last, __VA_ARGS__ }; URL_GetPrefsA((struct TagItem *) _tags); })
#endif /* !NO_INLINE_STDARG */

#define URL_FreePrefsA(___prefs, ___tags) \
      LP2NR(0x4e, URL_FreePrefsA , struct URL_Prefs *, ___prefs, a0, struct TagItem *, ___tags, a1,\
      , OPENURL_BASE_NAME)

#ifndef NO_INLINE_STDARG
#define URL_FreePrefs(___prefs, ___last, ...) \
    ({_sfdc_vararg _tags[] = { ___last, __VA_ARGS__ }; URL_FreePrefsA((___prefs), (struct TagItem *) _tags); })
#endif /* !NO_INLINE_STDARG */

#define URL_SetPrefsA(___up, ___tags) \
      LP2(0x54, ULONG, URL_SetPrefsA , struct URL_Prefs *, ___up, a0, struct TagItem *, ___tags, a1,\
      , OPENURL_BASE_NAME)

#ifndef NO_INLINE_STDARG
#define URL_SetPrefs(___up, ___last, ...) \
    ({_sfdc_vararg _tags[] = { ___last, __VA_ARGS__ }; URL_SetPrefsA((___up), (struct TagItem *) _tags); })
#endif /* !NO_INLINE_STDARG */

#define URL_LaunchPrefsAppA(___tags) \
      LP1(0x5a, ULONG, URL_LaunchPrefsAppA , struct TagItem *, ___tags, a0,\
      , OPENURL_BASE_NAME)

#ifndef NO_INLINE_STDARG
#define URL_LaunchPrefsApp(___last, ...) \
    ({_sfdc_vararg _tags[] = { ___last, __VA_ARGS__ }; URL_LaunchPrefsAppA((struct TagItem *) _tags); })
#endif /* !NO_INLINE_STDARG */

#define URL_GetAttr(___attr, ___storage) \
      LP2(0x60, ULONG, URL_GetAttr , ULONG, ___attr, d0, ULONG *, ___storage, a0,\
      , OPENURL_BASE_NAME)

#endif /* !_INLINE_OPENURL_H */
