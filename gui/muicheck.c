/* muicheck.c - checks the MUI toolchain and runtime: a window with an NList
 * whose rows show a generated avatar (Pixmap.mui, ARGB) next to a name, and a
 * TextEditor below.  "muicheck QUIT" closes it after 6 seconds (for tests). */
#include <stdio.h>
#include <string.h>
#include <exec/types.h>
#include <libraries/mui.h>
#include <mui/NList_mcc.h>
#include <mui/NListview_mcc.h>
#include <mui/TextEditor_mcc.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/muimaster.h>
#include <proto/utility.h>
#include <SDI_hook.h>
#include "mui_util.h"
#include "os_compat.h"

struct Library *MUIMasterBase;
MUI_IFACE_DEFINE;
unsigned long __stack = 32 * 1024;

#define AV 28
static ULONG avatar[3][AV * AV];

static void make_avatar(ULONG *px, ULONG rgb)
{
    int x, y;
    for (y = 0; y < AV; y++)
        for (x = 0; x < AV; x++) {
            int dx = 2 * x - AV + 1, dy = 2 * y - AV + 1;
            px[y * AV + x] = (dx * dx + dy * dy <= AV * AV) ? (0xff000000UL | rgb) : 0;
        }
}

static const char *names[] = { "Alice", "Retro Corner", "Kaffeeklatsch" };

HOOKPROTONHNO(disp_func, LONG, struct NList_DisplayMessage *m)
{
    static char buf[3][64];
    LONG i = (LONG)m->entry;
    if (!m->entry) { m->strings[0] = ""; return 0; }
    i--;
    snprintf(buf[i], sizeof buf[i], "\33o[%ld] %s", (long)i, names[i]);
    m->strings[0] = buf[i];
    return 0;
}
MakeStaticHook(disp_hook, disp_func);

int main(int argc, char **argv)
{
    Object *app, *win, *list, *ed;
    ULONG sigs = 0;
    int i, quit_after = argc > 1 && !strcmp(argv[1], "QUIT");
    static const ULONG colors[3] = { 0xd04040, 0x40a040, 0x4060d0 };

    if (!(MUIMasterBase = OpenLibrary("muimaster.library", 19))) {
        printf("muicheck: no muimaster.library V19+\n");
        return 20;
    }
    MUI_IFACE_GET();
    printf("muicheck: muimaster.library %d.%d\n", MUIMasterBase->lib_Version, MUIMasterBase->lib_Revision);
    for (i = 0; i < 3; i++) make_avatar(avatar[i], colors[i]);

    app = ApplicationObject,
        MUIA_Application_Title, "muicheck",
        MUIA_Application_Base, "MUICHECK",
        SubWindow, win = WindowObject,
            MUIA_Window_Title, "muicheck",
            MUIA_Window_ID, MAKE_ID('C','H','K','1'),
            WindowContents, VGroup,
                Child, NListviewObject,
                    MUIA_NListview_NList, list = NListObject,
                        MUIA_NList_DisplayHook2, &disp_hook,
                        MUIA_NList_MinLineHeight, AV + 2,
                    End,
                End,
                Child, ed = TextEditorObject, MUIA_TextEditor_Contents, "TextEditor.mcc ok", End,
            End,
        End,
    End;
    if (!app) {
        printf("muicheck: application failed (NList/TextEditor missing?)\n");
        CloseLibrary(MUIMasterBase);
        return 20;
    }
    for (i = 0; i < 3; i++) {
        Object *img = MUI_NewObject(MUIC_Pixmap,
            MUIA_Pixmap_Width, AV, MUIA_Pixmap_Height, AV,
            MUIA_Pixmap_Format, MUIV_Pixmap_Format_ARGB32,
            MUIA_Pixmap_Data, avatar[i], TAG_DONE);
        DoMethod(list, MUIM_NList_UseImage, img, i, 0);
        DoMethod(list, MUIM_NList_InsertSingle, (APTR)(LONG)(i + 1), MUIV_NList_Insert_Bottom);
    }
    DoMethod(win, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
             app, 2, MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);
    set(win, MUIA_Window_Open, TRUE);
    printf("muicheck: window %s\n", xget(win, MUIA_Window_Open) ? "open" : "NOT open");

    {
        ULONG t0 = 0;
        struct DateStamp ds;
        DateStamp(&ds); t0 = ds.ds_Minute * 3000 + ds.ds_Tick;
        while (DoMethod(app, MUIM_Application_NewInput, &sigs) != MUIV_Application_ReturnID_Quit) {
            if (quit_after) {
                DateStamp(&ds);
                if (ds.ds_Minute * 3000 + ds.ds_Tick - t0 > 6 * 50) break;
                Delay(5);
                continue;
            }
            if (sigs && !(sigs = Wait(sigs | SIGBREAKF_CTRL_C)) ) break;
            if (sigs & SIGBREAKF_CTRL_C) break;
        }
    }
    MUI_DisposeObject(app);
    CloseLibrary(MUIMasterBase);
    printf("muicheck: done\n");
    return 0;
}
