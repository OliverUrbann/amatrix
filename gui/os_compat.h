/* Per-system definitions for the GUI, so the code itself stays free of #ifdef.
 * AmigaOS 3 (m68k, libnix) is the reference; MorphOS and AmigaOS 4 differ
 * in a few names and headers. */
#ifndef AMX_OS_COMPAT_H
#define AMX_OS_COMPAT_H

#include <exec/types.h>
#include <dos/dos.h>

/* type of LocaleBase as proto/locale.h declares it */
#if defined(__MORPHOS__) || defined(__amigaos4__)
#define LOCALEBASE_T struct Library
#else
#define LOCALEBASE_T struct LocaleBase
#endif

#ifdef __amigaos4__
#include <dos/obsolete.h>     /* ACCESS_READ, Seek, DeleteFile ... (OS 4 renamed them) */
/* OS 4 calls libraries through interfaces; libauto opens the standard ones,
 * muimaster.library is opened by us */
struct MUIMasterIFace;
#define MUI_IFACE_DEFINE struct MUIMasterIFace *IMUIMaster
#define MUI_IFACE_GET()  (IMUIMaster = (struct MUIMasterIFace *)GetInterface(MUIMasterBase, "main", 1, NULL))
#else
#define MUI_IFACE_DEFINE extern int amx_no_iface
#define MUI_IFACE_GET()  ((void)0)
#endif

/* MUIA_List_Quiet: on MorphOS (MUI 4) a List switched quiet and back stays
 * empty on screen (rows there, nothing drawn), so it is left out there */
#ifdef __MORPHOS__
#define LIST_QUIET(list, on) ((void)(list))
#else
#define LIST_QUIET(list, on) set(list, MUIA_List_Quiet, on)
#endif

#endif
