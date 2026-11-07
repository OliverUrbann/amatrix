/* Amatrix - a small Matrix client for AmigaOS with MUI 5.
 *
 * In the spirit of the old ICQ: a narrow main window with the list of chats
 * and rooms (picture, name, unread count), buttons for a new chat and a new
 * room; a double-click opens a chat window (history, input line, Return
 * sends).  All networking and crypto run in the network process of the core
 * (amatrix_async.h); this file is only the GUI.
 *
 *   Amatrix [SERVER=<url>] [USER=<name>] [PASSWORD=<pw>] [DEBUG]
 *
 * DEBUG writes a detailed log to PROGDIR:Amatrix.log (line by line, so it
 * survives a crash).
 *
 * Without arguments the last account is used (the login is kept in the
 * store, the password is never saved); the first time a login window opens.
 * ARexx port AMATRIX.1: OPENCHAT, SAY, ACCEPT, NEWCHAT, NEWROOM, INVITE, STATUS,
 * CHATLINES, UNREADABLE, OLDER, FIRSTLINE, UNREAD, ROOMLINE, RECOVERY, UNLOCK, STOREPASS,
 * CHATSTATUS, CLOSECHAT, QUIT (see rexx_cmds below) - used by the emulator tests.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <libraries/gadtools.h>
#include <libraries/mui.h>
#include <mui/NList_mcc.h>
#include <mui/NListview_mcc.h>
#include <mui/BetterString_mcc.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/muimaster.h>
#include <SDI_hook.h>
#include "mui_util.h"
#include "amatrix_async.h"

struct Library *MUIMasterBase;
unsigned long __stack = 64 * 1024;
static const char version[] __attribute__((used)) = "$VER: Amatrix 0.1 (03.11.2026)";

#define AV 24                     /* avatar size in the room list */
#define N_PLACEHOLDER 8
#define FIRST_PAGE 30             /* messages loaded when a chat window opens */
#define OLDER_PAGE 20             /* per click on "20 ältere Nachrichten laden" */
#define OLDER_HELP "20 ältere Nachrichten laden"
#define OLDER_HELP_END "Anfang des Chats - keine älteren Nachrichten"
#define INVITE_HELP "Jemanden in diesen Raum einladen"
#define LEAVE_HELP "Diesen Raum verlassen"
#define REACT_HELP "Reaktion auf die markierte Nachricht\n(ohne Markierung: auf die neueste)"

enum {
    ID_LOGIN = 1, ID_NEWCHAT, ID_NEWCHAT_GO, ID_NEWROOM, ID_NEWROOM_GO, ID_ROOM_DCLICK,
    ID_JOINWIN, ID_JOIN_SEARCH, ID_JOIN_GO, ID_LOGOUT, ID_ABOUT, ID_INVITE_GO, ID_RECOVERY, ID_RECOVERY_GO, ID_UNLOCK_GO, ID_STOREPASS, ID_STOREPASS_SET, ID_STOREPASS_DEL, ID_LEAVE
};
enum { TAG_LOGIN = 1, TAG_NEWCHAT, TAG_NEWROOM, TAG_JOIN, TAG_SEND, TAG_HIST, TAG_AVATAR,
       TAG_READ, TAG_SEARCH, TAG_ACCEPT, TAG_LEAVE, TAG_LOGOUT, TAG_INVITE, TAG_RECOVERY, TAG_UNLOCK, TAG_STOREPASS,
       TAG_REACT };

/* ------------------------------------------------------------------ state */

typedef struct {
    char *id, *name, *dm_user, *inviter, *avatar_url;   /* name/inviter: local charset */
    char *topic, *last_sender, *last_body;              /* local charset */
    int invited, unread, highlights, direct, encrypted, members, last_own;
    double last_ts;
} Room;

#define N_EMO_MAX 16

typedef struct React {
    char *id, *target, *key, *sender;   /* key: UTF-8 */
    int own;
} React;

typedef struct ChatWin {
    struct ChatWin *next;
    char *room_id;
    Object *win, *list, *input, *older, *invite, *leave, *reactb;
    long next_serial;
    char **seen;                  /* event IDs shown (history and live overlap) */
    int n_seen, cap_seen;
    int n_undec;                  /* placeholder lines for undecryptable messages */
    int at_start, loading, close_req, lines;
    char *last_event;             /* newest message shown: for the read marker */
    double last_ts;
    char *marked;                 /* event the read marker was last sent for */
    int mark_pending;             /* marker sent, room list not refreshed since */
    char *title;
    void **all;                   /* every Line we created, each once: NList returns
                                     the same entry for each wrapped part, so the
                                     list itself cannot tell us what to free */
    int n_all, cap_all;
    Object *busy;                 /* status text in the toolbar */
    struct React *react;          /* reactions seen in this room */
    int n_react, cap_react;
    Object *emo[N_EMO_MAX];       /* the reaction pictures, as NList images (ESC o[n]) */
    int sending, inviting, loading_older;
    char err[160];                /* last error, shown until the next action */
} ChatWin;

/* a picture for the room list: raw ARGB that MUI 5 draws itself (ESC A) */
typedef struct {
    struct MUI_AlphaData ad;
    ULONG px[AV * AV];
} Alpha;

typedef struct {
    char *url;
    Alpha *img;                   /* NULL until loaded */
} Img;

static AmxAsync *A;
static int debug;                 /* DEBUG: log to PROGDIR:Amatrix.log */
static Object *app, *win_main, *lv_rooms, *lst_rooms, *txt_status;
static Object *win_login, *str_server, *str_user, *str_pass, *txt_login;
static Object *win_newchat, *str_chatuser;
static Object *win_newroom, *str_roomname, *chk_enc, *chk_pub;
static Object *win_invite, *str_inviteuser, *txt_inviteroom;
static char *invite_room;          /* room the invite window is for */
static char *pending_password;     /* login password while the store is being unlocked */
static Object *win_recovery, *str_recovery, *txt_recovery, *b_recovery;
static int verified;               /* this device signed (this session) */
static Object *win_unlock, *str_unlock, *txt_unlock, *b_unlock;
static Object *win_storepass, *str_sp1, *str_sp2, *txt_sp, *b_sp_set, *b_sp_del;
static int store_protected;        /* the store has a passphrase */
static int store_protected_removing;
static Object *win_join, *str_search, *lst_pub;
static Room *rooms;
static int n_rooms;
static ChatWin *chats;
static Img *imgs;
static int n_imgs;
static Alpha placeholder[N_PLACEHOLDER];
static char *my_user, *cfg_server, *cfg_user;
static char *pending_open;        /* room to open as soon as it is in the list */
static char prog_dir[256];
static char status_buf[300];
static AmxPublicRoom *pub;
static int n_pub;

/* ---------------------------------------------------------------- helpers */

static char *xstrdup(const char *s) { return s ? strdup(s) : NULL; }

static char *local(const char *utf8)       /* UTF-8 -> Latin-1, ESC removed (NList codes) */
{
    char *l = amx_to_local(utf8 ? utf8 : ""), *p;
    for (p = l; *p; p++) if (*p == 27) *p = ' ';
    return l;
}

static int same_ci(const char *a, const char *b)
{
    while (*a && *b) {
        int x = (unsigned char)*a++, y = (unsigned char)*b++;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return *a == *b;
}

static unsigned long hash(const char *s)
{
    unsigned long h = 5381;
    while (*s) h = h * 33 + (unsigned char)*s++;
    return h;
}

static void set_status(const char *fmt, const char *a, const char *b)
{
    snprintf(status_buf, sizeof status_buf, fmt, a ? a : "", b ? b : "");
    set(txt_status, MUIA_Text_Contents, status_buf);
}

/* "Online - user (verifiziert)": the one place for it, so no path forgets the suffix */
static void set_online(void)
{
    set_status(verified ? "Online - %s (verifiziert)" : "Online - %s", my_user, NULL);
}

static Room *room_by_id(const char *id)
{
    int i;
    for (i = 0; id && i < n_rooms; i++) if (!strcmp(rooms[i].id, id)) return &rooms[i];
    return NULL;
}

static Room *room_by_name(const char *name)
{
    int i;
    Room *r = room_by_id(name);
    for (i = 0; !r && i < n_rooms; i++) if (same_ci(rooms[i].name, name)) r = &rooms[i];
    return r;
}

static ChatWin *chat_by_room(const char *id)
{
    ChatWin *w;
    for (w = chats; w; w = w->next) if (!strcmp(w->room_id, id)) return w;
    return NULL;
}

/* the status text in a chat window's toolbar, and the busy pointer while
 * something runs (the window stays usable) */
static void update_status(ChatWin *w)
{
    static char t[200];
    const Room *r = room_by_id(w->room_id);
    struct Window *iw;
    int busy = w->loading || w->sending || w->inviting;
    if (w->loading) snprintf(t, sizeof t, w->loading_older ? "Lade ältere Nachrichten ..." : "Lade Verlauf ...");
    else if (w->sending) snprintf(t, sizeof t, w->sending > 1 ? "Sende %d Nachrichten ..." : "Sende ...", w->sending);
    else if (w->inviting) snprintf(t, sizeof t, "Lade ein ...");
    else if (w->err[0]) snprintf(t, sizeof t, "\33b%s", w->err);
    else snprintf(t, sizeof t, "%s", !r ? "" : r->encrypted ? "Ende-zu-Ende-verschlüsselt" : "nicht verschlüsselt");
    set(w->busy, MUIA_Text_Contents, t);
    if ((iw = (struct Window *)xget(w->win, MUIA_Window_Window)) != NULL)
        SetWindowPointer(iw, WA_BusyPointer, busy, WA_PointerDelay, busy, TAG_DONE);
}

/* results come back in the order the commands were sent: which room each
 * pending send/invite belongs to */
static char *fifo_send[64], *fifo_invite[16];
static int n_send, n_invite;

static void fifo_push(char **q, int *n, int max, const char *room)
{
    if (*n < max) q[(*n)++] = strdup(room);
}

static char *fifo_pop(char **q, int *n)
{
    char *r;
    int i;
    if (!*n) return NULL;
    r = q[0];
    for (i = 1; i < *n; i++) q[i - 1] = q[i];
    (*n)--;
    return r;
}

/* config: PROGDIR:Amatrix.prefs, two lines (server, user) */
static void load_prefs(void)
{
    char path[300], line[256];
    FILE *f;
    snprintf(path, sizeof path, "%sAmatrix.prefs", prog_dir);
    if (!(f = fopen(path, "r"))) return;
    if (fgets(line, sizeof line, f)) { line[strcspn(line, "\r\n")] = 0; cfg_server = xstrdup(line); }
    if (fgets(line, sizeof line, f)) { line[strcspn(line, "\r\n")] = 0; cfg_user = xstrdup(line); }
    fclose(f);
    if (cfg_user && !*cfg_user) { free(cfg_user); cfg_user = NULL; }
}

static void save_prefs(void)
{
    char path[300];
    FILE *f;
    snprintf(path, sizeof path, "%sAmatrix.prefs", prog_dir);
    if (!(f = fopen(path, "w"))) return;
    fprintf(f, "%s\n%s\n", cfg_server ? cfg_server : "", cfg_user ? cfg_user : "");
    fclose(f);
}

/* the store: one drawer per account */
static void store_dir(char *buf, size_t n, const char *server, const char *user)
{
    char key[600];
    snprintf(key, sizeof key, "%s|%s", server, user);
    snprintf(buf, n, "%sAmatrix-%08lx", prog_dir, hash(key) & 0xffffffffUL);
}

/* ----------------------------------------------------------------- avatars */

/* toolbar icons, 16x16, in the MagicWB style: its palette plus a darker
 * blue for the shadow side, light from the top left, black outlines and a
 * drop shadow.  '.' = transparent */
#define IC 16
static Alpha icon_up, icon_invite, icon_leave, icon_react;
static const char *const art_up[IC] = {
    "................",
    ".......kk.......",
    "......kwbk......",
    ".....kwbbBk.....",
    "....kwbbbbBk....",
    "...kwbbbbbbBk...",
    "..kwbbbbbbbbBk..",
    ".kkkkkwbbBkkkkkd",
    "..dddkwbbBkddddd",
    ".....kwbbBkd....",
    ".....kwbbBkd....",
    ".....kwbbBkd....",
    ".....kwbbBkd....",
    ".....kwbbBkd....",
    ".....kBBBBkd....",
    "......kkkkdd...." };
static const char *const art_invite[IC] = {
    "....kkkkk.......",
    "...kwwfffk......",
    "..kwffffffk.....",
    "..kwfffffek.....",
    "..kffffffekd....",
    "..kfffffeekd....",
    "...kffeeekd.....",
    "....kkkkkd.kkk..",
    "..kkkkkkkkkkwk..",
    ".kwbbbbbbkkkwkkk",
    "kwbbbbbbbkwwwwwk",
    "kbbbbbbbbkkkwkkk",
    "kbbbbbbbbbbkwk..",
    "kbbbbbbbbbbkkk..",
    "kBBBBBBBBBBBkd..",
    "kkkkkkkkkkkkkd.." };

static const char *const art_leave[IC] = {
    "kkkkkkkkk.......",
    "kwwwwwwwk.......",
    "kwkkkkkwk.......",
    "kwknnnnkk.......",
    "kwknnnnNk...k...",
    "kwknnnnNk...kk..",
    "kwknnnnNkkkkkrk.",
    "kwknnnkNkrrrrrRk",
    "kwknnnnNkrrrrRkd",
    "kwknnnnNkkkkkRkd",
    "kwknnnnNk.ddkkdd",
    "kwknnnnNk...kdd.",
    "kwkNNNNNk....d..",
    "kwkkkkkkk.......",
    "kkkkkkkkkd......",
    ".ddddddddd......" };

static const char *const art_react[IC] = {
    "...kkkkk........",
    ".kkyyyyykk......",
    ".kywyyyyYk......",
    "kywyyyyyyYk.....",
    "kyykyyykyYk.....",
    "kyykyyykyYk.....",
    "kyyyyyyyyYk.....",
    "kykyyyyykYk.....",
    "kyykkkkkYYk.....",
    ".kYyyyyYYk.kkk..",
    "..kkYYYkk..kbk..",
    "....kkk..kkkbkkk",
    ".........kbbbbbk",
    ".........kkkbkkk",
    "...........kbk..",
    "...........kkk.." };

static ULONG wb_colour(char ch)
{
    switch (ch) {
    case 'k': return 0xff000000UL;            /* black */
    case 'w': return 0xffffffffUL;            /* white */
    case 'b': return 0xff3b67a2UL;            /* MagicWB blue */
    case 'B': return 0xff244070UL;            /* its shadow side */
    case 'd': return 0x806e6e6eUL;            /* drop shadow, half transparent */
    case 'l': return 0xffafafafUL;            /* light grey */
    case 'g': return 0xff959595UL;            /* MagicWB grey */
    case 'f': return 0xffffa997UL;            /* skin */
    case 'e': return 0xffd2826eUL;            /* skin, shadow side */
    case 'n': return 0xffaa907cUL;            /* MagicWB beige (door) */
    case 'N': return 0xff786050UL;            /* its shadow side */
    case 'y': return 0xffffd648UL;            /* face yellow */
    case 'Y': return 0xffd6961eUL;            /* its shadow side */
    case 'r': return 0xffe22828UL;            /* red */
    case 'R': return 0xff961010UL;            /* dark red */
    case 'G': return 0xff5aaa5aUL;            /* green */
    case 'p': return 0xffff78aaUL;            /* pink */
    case 'c': return 0xff6ebef0UL;            /* light blue (tears, confetti) */
    default:  return 0;
    }
}

static void make_icon(Alpha *a, const char *const *art)
{
    int x, y;
    a->ad.width = a->ad.height = IC;
    for (y = 0; y < IC; y++)
        for (x = 0; x < IC; x++) a->px[y * IC + x] = wb_colour(art[y][x]);
}

/* reactions: the common ones as 12x12 pictures in the same style (Element's
 * quick reactions plus "tears of joy"); keys are UTF-8 */
#define EM 12
enum { EMO_THUMBSUP, EMO_THUMBSDOWN, EMO_HEART, EMO_SMILE, EMO_JOY, EMO_CONFUSED, EMO_EYES, EMO_TADA, EMO_ROCKET, EMO_UNKNOWN, N_EMO };
static Alpha emo_img[N_EMO], emo_big[N_EMO];   /* 12x12 in text, 24x24 in the picker */
static const char *const emo_art[N_EMO][EM] = {
    { /* thumbsup */
      "............",
      ".....kk.....",
      "....kffk....",
      "....kfek....",
      "...kffek....",
      ".kkkffffkkk.",
      "kwkkfffffffk",
      "kbkkffffffek",
      "kbkkffffffek",
      "kbkkfffffeek",
      "kBkkkeeeeek.",
      ".kk.kkkkkk.." },
    { /* thumbsdown */
      ".kk.kkkkkk..",
      "kBkkkeeeeek.",
      "kbkkfffffeek",
      "kbkkffffffek",
      "kbkkffffffek",
      "kwkkfffffffk",
      ".kkkffffkkk.",
      "...kffek....",
      "....kfek....",
      "....kffk....",
      ".....kk.....",
      "............" },
    { /* heart */
      "............",
      ".kkk...kkk..",
      "krwrk.krrrk.",
      "kwrrrkrrrrRk",
      "krrrrrrrrrRk",
      "krrrrrrrrrRk",
      ".krrrrrrrRk.",
      "..krrrrrRk..",
      "...krrrRk...",
      "....krRk....",
      ".....kk.....",
      "............" },
    { /* smile */
      "....kkkk....",
      "..kkyyyykk..",
      ".kyyyyyyyYk.",
      ".kyykyykyYk.",
      "kyyykyykyyYk",
      "kyyyyyyyyyYk",
      "kyykyyyykyYk",
      "kyyykkkkyyYk",
      ".kyyyyyyyYk.",
      ".kYyyyyyYYk.",
      "..kkYYYYkk..",
      "....kkkk...." },
    { /* joy */
      "....kkkk....",
      "..kkyyyykk..",
      ".kyyyyyyyYk.",
      ".kykkyykkyYk",
      "kcyyyyyyyycY",
      "kcyyyyyyyycY",
      "kyykkkkkkyYk",
      "kyykwwwwkyYk",
      ".kyykrrkyYk.",
      ".kYyykkyYYk.",
      "..kkYYYYkk..",
      "....kkkk...." },
    { /* confused */
      "....kkkk....",
      "..kkyyyykk..",
      ".kyyyyyyyYk.",
      ".kyykyykyYk.",
      "kyyykyykyyYk",
      "kyyyyyyyyyYk",
      "kyyyyyykkyYk",
      "kyykkkkyyyYk",
      ".kyyyyyyyYk.",
      ".kYyyyyyYYk.",
      "..kkYYYYkk..",
      "....kkkk...." },
    { /* eyes */
      "............",
      "............",
      ".kkkk..kkkk.",
      "kwwwwkkwwwwk",
      "kwwkkwkwwkkw",
      "kwwkkwkwwkkw",
      "kwwwwkkwwwwk",
      ".kkkk..kkkk.",
      "............",
      "............",
      "............",
      "............" },
    { /* tada */
      "..r...c..p..",
      "......k.....",
      ".p..y.....G.",
      "......c..r..",
      "..kk.....c..",
      "..kYk..y....",
      "..kyYk....p.",
      ".kyyyYk.....",
      ".kyyyyYk..G.",
      "kyyyyyyYk...",
      "kYyyyyyyYk..",
      "kkkkkkkkkkk." },
    { /* rocket */
      "........kkkk",
      ".......kwwck",
      "......kwwcBk",
      ".....kwwwBk.",
      "..kkkwwwBk..",
      ".krkwwwBkk..",
      "..kwwwBkrk..",
      ".kkwwBk.k...",
      "kyrkBk......",
      "kyrkk.......",
      ".kyk........",
      "............" },
    { /* unknown: an emoji we have no picture for */
      "....kkkk....",
      "..kkyyyykk..",
      ".kyywyyyyYk.",
      ".kywkkkyyYk.",
      "kyyyyyykyyYk",
      "kyyyyyykyyYk",
      "kyyyykkyyyYk",
      "kyyyykyyyyYk",
      ".kyyyyyyyYk.",
      ".kYyykyyYYk.",
      "..kkYYYYkk..",
      "....kkkk...." },
};
/* key (without variation selector / skin tone) -> picture; similar emoji
 * share a picture and are counted together (all laughing faces, all hearts...).
 * The first key of a picture is the one we send. */
static const struct { const char *key; int img; } emo_keys[] = {
    { "\xf0\x9f\x91\x8d", EMO_THUMBSUP },   /* U+1F44D */
    { "\xf0\x9f\x91\x8c", EMO_THUMBSUP },   /* U+1F44C */
    { "\xe2\x9c\x85", EMO_THUMBSUP },   /* U+2705 */
    { "\xe2\x9c\x94", EMO_THUMBSUP },   /* U+2714 */
    { "\xf0\x9f\x86\x97", EMO_THUMBSUP },   /* U+1F197 */
    { "\xf0\x9f\x91\x8e", EMO_THUMBSDOWN },   /* U+1F44E */
    { "\xe2\x9d\xa4", EMO_HEART },   /* U+2764 */
    { "\xe2\x99\xa5", EMO_HEART },   /* U+2665 */
    { "\xf0\x9f\x92\x95", EMO_HEART },   /* U+1F495 */
    { "\xf0\x9f\x92\x96", EMO_HEART },   /* U+1F496 */
    { "\xf0\x9f\x92\x97", EMO_HEART },   /* U+1F497 */
    { "\xf0\x9f\x92\x93", EMO_HEART },   /* U+1F493 */
    { "\xf0\x9f\x92\x9e", EMO_HEART },   /* U+1F49E */
    { "\xf0\x9f\x92\x98", EMO_HEART },   /* U+1F498 */
    { "\xf0\x9f\x92\x9d", EMO_HEART },   /* U+1F49D */
    { "\xf0\x9f\xa7\xa1", EMO_HEART },   /* U+1F9E1 */
    { "\xf0\x9f\x92\x9b", EMO_HEART },   /* U+1F49B */
    { "\xf0\x9f\x92\x9a", EMO_HEART },   /* U+1F49A */
    { "\xf0\x9f\x92\x99", EMO_HEART },   /* U+1F499 */
    { "\xf0\x9f\x92\x9c", EMO_HEART },   /* U+1F49C */
    { "\xf0\x9f\x96\xa4", EMO_HEART },   /* U+1F5A4 */
    { "\xf0\x9f\xa4\x8d", EMO_HEART },   /* U+1F90D */
    { "\xf0\x9f\xa4\x8e", EMO_HEART },   /* U+1F90E */
    { "\xf0\x9f\x98\x8d", EMO_HEART },   /* U+1F60D */
    { "\xf0\x9f\xa5\xb0", EMO_HEART },   /* U+1F970 */
    { "\xf0\x9f\x98\xbb", EMO_HEART },   /* U+1F63B */
    { "\xf0\x9f\x98\x98", EMO_HEART },   /* U+1F618 */
    { "\xf0\x9f\x98\x97", EMO_HEART },   /* U+1F617 */
    { "\xf0\x9f\x98\x99", EMO_HEART },   /* U+1F619 */
    { "\xf0\x9f\x98\x9a", EMO_HEART },   /* U+1F61A */
    { "\xf0\x9f\x92\x8b", EMO_HEART },   /* U+1F48B */
    { "\xf0\x9f\x98\xbd", EMO_HEART },   /* U+1F63D */
    { "\xe2\x9d\xa3", EMO_HEART },   /* U+2763 */
    { "\xf0\x9f\x92\x9f", EMO_HEART },   /* U+1F49F */
    { "\xf0\x9f\xab\xb6", EMO_HEART },   /* U+1FAF6 */
    { "\xf0\x9f\x98\x84", EMO_SMILE },   /* U+1F604 */
    { "\xf0\x9f\x98\x80", EMO_SMILE },   /* U+1F600 */
    { "\xf0\x9f\x98\x83", EMO_SMILE },   /* U+1F603 */
    { "\xf0\x9f\x98\x81", EMO_SMILE },   /* U+1F601 */
    { "\xf0\x9f\x99\x82", EMO_SMILE },   /* U+1F642 */
    { "\xf0\x9f\x98\x8a", EMO_SMILE },   /* U+1F60A */
    { "\xe2\x98\xba", EMO_SMILE },   /* U+263A */
    { "\xf0\x9f\x98\xba", EMO_SMILE },   /* U+1F63A */
    { "\xf0\x9f\x98\xb8", EMO_SMILE },   /* U+1F638 */
    { "\xf0\x9f\x98\x89", EMO_SMILE },   /* U+1F609 */
    { "\xf0\x9f\x98\x8c", EMO_SMILE },   /* U+1F60C */
    { "\xf0\x9f\x98\x82", EMO_JOY },   /* U+1F602 */
    { "\xf0\x9f\xa4\xa3", EMO_JOY },   /* U+1F923 */
    { "\xf0\x9f\x98\x86", EMO_JOY },   /* U+1F606 */
    { "\xf0\x9f\x98\xb9", EMO_JOY },   /* U+1F639 */
    { "\xf0\x9f\x98\x85", EMO_JOY },   /* U+1F605 */
    { "\xf0\x9f\x98\x95", EMO_CONFUSED },   /* U+1F615 */
    { "\xf0\x9f\x98\x9f", EMO_CONFUSED },   /* U+1F61F */
    { "\xf0\x9f\x99\x81", EMO_CONFUSED },   /* U+1F641 */
    { "\xe2\x98\xb9", EMO_CONFUSED },   /* U+2639 */
    { "\xf0\x9f\x98\x90", EMO_CONFUSED },   /* U+1F610 */
    { "\xf0\x9f\xa4\x94", EMO_CONFUSED },   /* U+1F914 */
    { "\xf0\x9f\xab\xa4", EMO_CONFUSED },   /* U+1FAE4 */
    { "\xf0\x9f\x91\x80", EMO_EYES },   /* U+1F440 */
    { "\xf0\x9f\x91\x81", EMO_EYES },   /* U+1F441 */
    { "\xf0\x9f\x8e\x89", EMO_TADA },   /* U+1F389 */
    { "\xf0\x9f\x8e\x8a", EMO_TADA },   /* U+1F38A */
    { "\xf0\x9f\xa5\xb3", EMO_TADA },   /* U+1F973 */
    { "\xf0\x9f\x9a\x80", EMO_ROCKET },   /* U+1F680 */
};

/* the key without variation selector and skin tone: same picture, same group */
static void norm_key(const char *key, char *k, size_t size)
{
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)key;
    while (*p && n < size - 1) {
        if (p[0] == 0xef && p[1] == 0xb8 && p[2] == 0x8f) { p += 3; continue; }      /* U+FE0F */
        if (p[0] == 0xf0 && p[1] == 0x9f && p[2] == 0x8f && p[3] >= 0xbb && p[3] <= 0xbf) { p += 4; continue; }
        k[n++] = (char)*p++;
    }
    k[n] = 0;
}

static int emo_index(const char *key);

/* one group: the same picture, or (no picture) the same emoji */
static int same_key(const char *a, const char *b)
{
    char x[64], y[64];
    int ia = emo_index(a), ib = emo_index(b);
    if (ia >= 0 || ib >= 0) return ia == ib;
    norm_key(a, x, sizeof x);
    norm_key(b, y, sizeof y);
    return !strcmp(x, y);
}

static int emo_index(const char *key)
{
    char k[64];
    unsigned i;
    norm_key(key, k, sizeof k);
    for (i = 0; i < sizeof emo_keys / sizeof emo_keys[0]; i++)
        if (!strcmp(emo_keys[i].key, k)) return emo_keys[i].img;
    return -1;
}

static void make_emoji(void)
{
    int i, x, y;
    for (i = 0; i < N_EMO; i++) {
        emo_img[i].ad.width = emo_img[i].ad.height = EM;
        for (y = 0; y < EM; y++)
            for (x = 0; x < EM; x++) emo_img[i].px[y * EM + x] = wb_colour(emo_art[i][y][x]);
        emo_big[i].ad.width = emo_big[i].ad.height = 2 * EM;
        for (y = 0; y < 2 * EM; y++)
            for (x = 0; x < 2 * EM; x++) emo_big[i].px[y * 2 * EM + x] = emo_img[i].px[(y / 2) * EM + x / 2];
    }
}

/* ---- emoji in message text: a picture (ESC o[n]) instead of "?" */

static unsigned long utf8_cp(const unsigned char *p, int *len)
{
    if (p[0] < 0x80) { *len = 1; return p[0]; }
    if ((p[0] & 0xe0) == 0xc0 && (p[1] & 0xc0) == 0x80) { *len = 2; return ((p[0] & 0x1fUL) << 6) | (p[1] & 0x3f); }
    if ((p[0] & 0xf0) == 0xe0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
        *len = 3; return ((p[0] & 0x0fUL) << 12) | ((p[1] & 0x3fUL) << 6) | (p[2] & 0x3f);
    }
    if ((p[0] & 0xf8) == 0xf0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80 && (p[3] & 0xc0) == 0x80) {
        *len = 4;
        return ((p[0] & 0x07UL) << 18) | ((p[1] & 0x3fUL) << 12) | ((p[2] & 0x3fUL) << 6) | (p[3] & 0x3f);
    }
    *len = 1;
    return 0xfffd;
}

static int is_emoji_cp(unsigned long cp)
{
    return (cp >= 0x1F000 && cp <= 0x1FAFF) || (cp >= 0x2600 && cp <= 0x27BF) ||
           (cp >= 0x2300 && cp <= 0x23FF) || (cp >= 0x2B00 && cp <= 0x2BFF) ||
           cp == 0x3030 || cp == 0x303D || cp == 0x3297 || cp == 0x3299;
}

static int is_emoji_mod(unsigned long cp)      /* belongs to the emoji before it */
{
    return cp == 0xFE0F || cp == 0xFE0E || (cp >= 0x1F3FB && cp <= 0x1F3FF) || cp == 0x20E3 ||
           (cp >= 0xE0020 && cp <= 0xE007F);
}

static void sb_add(char **b, size_t *n, size_t *cap, const char *s, size_t len)
{
    if (*n + len + 1 > *cap) {
        size_t nc = (*cap ? *cap * 2 : 128) + len;
        char *t = realloc(*b, nc);
        if (!t) return;
        *b = t; *cap = nc;
    }
    memcpy(*b + *n, s, len);
    *n += len;
    (*b)[*n] = 0;
}

static void sb_local(char **b, size_t *n, size_t *cap, const char *from, const char *to)
{
    char *u, *l;
    if (to <= from) return;
    if (!(u = malloc((size_t)(to - from) + 1))) return;
    memcpy(u, from, (size_t)(to - from));
    u[to - from] = 0;
    l = local(u);
    sb_add(b, n, cap, l, strlen(l));
    free(l); free(u);
}

/* like local(), but emoji become our pictures: a known one its own, any
 * other the yellow "?" face (instead of a "?" that reads like a question) */
static char *local_emoji_as(const char *utf8, int mui);

static char *local_emoji(const char *utf8)
{
    return local_emoji_as(utf8, 0);
}

/* mui: for MUI's List (ESC A with the ARGB data) instead of NList (ESC o) */
static char *local_emoji_as(const char *utf8, int mui)
{
    const unsigned char *p = (const unsigned char *)(utf8 ? utf8 : ""), *seg = p;
    char *b = NULL;
    size_t n = 0, cap = 0;
    sb_add(&b, &n, &cap, "", 0);
    while (*p) {
        int len, l2;
        unsigned long cp = utf8_cp(p, &len), c2;
        if (is_emoji_cp(cp)) {
            const unsigned char *q = p + len;
            char key[8], esc[24];
            int img;
            sb_local(&b, &n, &cap, (const char *)seg, (const char *)p);
            for (;;) {                                          /* modifiers, ZWJ sequences, flags */
                if (!*q) break;
                c2 = utf8_cp(q, &l2);
                if (is_emoji_mod(c2)) { q += l2; continue; }
                if (c2 == 0x200D && q[l2] && is_emoji_cp(utf8_cp(q + l2, &len))) { q += l2 + len; continue; }
                if (cp >= 0x1F1E6 && cp <= 0x1F1FF && c2 >= 0x1F1E6 && c2 <= 0x1F1FF && q == p + 4) {
                    q += l2; continue;
                }
                break;
            }
            utf8_cp(p, &len);
            memcpy(key, p, (size_t)len);
            key[len] = 0;
            img = emo_index(key);
            if (img < 0) img = EMO_UNKNOWN;
            if (mui) snprintf(esc, sizeof esc, "\33A[%08lx]", (unsigned long)&emo_img[img].ad);
            else snprintf(esc, sizeof esc, "\33o[%d]", img);
            sb_add(&b, &n, &cap, esc, strlen(esc));
            p = seg = q;
        } else if (is_emoji_mod(cp) || cp == 0x200D) {        /* stray: e.g. a keycap's parts */
            sb_local(&b, &n, &cap, (const char *)seg, (const char *)p);
            p = seg = p + len;
        } else
            p += len;
    }
    sb_local(&b, &n, &cap, (const char *)seg, (const char *)p);
    return b;
}

/* a toolbar button: icon only, the text as bubble help */
static Object *icon_button(Alpha *a, const char *help)
{
    static char txt[2][32];
    static int n;
    char *t = txt[n++ & 1];
    snprintf(t, 32, "\33c\33A[%08lx]", (unsigned long)&a->ad);
    return TextObject,
        ButtonFrame,
        MUIA_Background, MUII_ButtonBack,
        MUIA_InputMode, MUIV_InputMode_RelVerify,
        MUIA_Text_Contents, t,
        MUIA_Text_SetMax, TRUE,
        MUIA_ShortHelp, help,
        MUIA_CycleChain, TRUE,
    End;
}

static void make_placeholders(void)
{
    static const unsigned long col[N_PLACEHOLDER] = {
        0x3b6fd4, 0xd0503a, 0x3a9a50, 0xb0479e, 0xd08a2a, 0x2a9aa8, 0x7a5ad0, 0x8a8a40 };
    int i, x, y;
    for (i = 0; i < N_PLACEHOLDER; i++) {
        placeholder[i].ad.width = placeholder[i].ad.height = AV;
        for (y = 0; y < AV; y++)
            for (x = 0; x < AV; x++) {
                int dx = 2 * x - AV + 1, dy = 2 * y - AV + 1;
                placeholder[i].px[y * AV + x] = dx * dx + dy * dy <= AV * AV ? 0xff000000UL | col[i] : 0;
            }
    }
}

static Alpha *image_for(const Room *r)
{
    int i;
    if (r->avatar_url)
        for (i = 0; i < n_imgs; i++)
            if (imgs[i].img && !strcmp(imgs[i].url, r->avatar_url)) return imgs[i].img;
    return &placeholder[hash(r->id) % N_PLACEHOLDER];
}

static void request_avatars(void)
{
    int i, j;
    for (i = 0; i < n_rooms; i++) {
        const char *u = rooms[i].avatar_url;
        Img *t;
        if (!u) continue;
        for (j = 0; j < n_imgs; j++) if (!strcmp(imgs[j].url, u)) break;
        if (j < n_imgs) continue;
        t = realloc(imgs, sizeof *imgs * (n_imgs + 1));
        if (!t) return;
        imgs = t;
        memset(&imgs[n_imgs], 0, sizeof *imgs);
        imgs[n_imgs].url = strdup(u);
        n_imgs++;
        amx_async_avatar(A, u, AV, TAG_AVATAR);
    }
}

static void got_avatar(const AmxMsg *m)
{
    int i;
    for (i = 0; i < n_imgs; i++) {
        if (strcmp(imgs[i].url, m->result) || imgs[i].img || m->status || !m->pixels) continue;
        if (!(imgs[i].img = calloc(1, sizeof *imgs[i].img))) return;
        imgs[i].img->ad.width = imgs[i].img->ad.height = AV;
        memcpy(imgs[i].img->px, m->pixels, sizeof imgs[i].img->px);
        DoMethod(lst_rooms, MUIM_List_Redraw, MUIV_List_Redraw_All);
    }
}

/* --------------------------------------------------------------- room list */

/* the second line of a room in the list (local charset, no codes) */
static void second_line(const Room *r, char *buf, size_t n)
{
    if (r->invited)
        snprintf(buf, n, "Einladung von %s", r->inviter ? r->inviter : "?");
    else if (r->last_body && r->last_own)
        snprintf(buf, n, "Du: %s", r->last_body);
    else if (r->last_body && (r->direct || !r->last_sender))
        snprintf(buf, n, "%s", r->last_body);
    else if (r->last_body)
        snprintf(buf, n, "%s: %s", r->last_sender, r->last_body);
    else if (r->topic)
        snprintf(buf, n, "%s", r->topic);
    else
        snprintf(buf, n, "%d Mitglied%s", r->members, r->members == 1 ? "" : "er");
}

static void time_text(double ms, char *buf, size_t n)
{
    time_t t = (time_t)(ms / 1000), now = time(NULL);
    struct tm tm_t, *tm;
    int today;
    buf[0] = 0;
    if (ms <= 0) return;
    tm = localtime(&now);
    if (!tm) return;
    today = tm->tm_yday + tm->tm_year * 400;
    tm = localtime(&t);
    if (!tm) return;
    tm_t = *tm;
    strftime(buf, n, tm_t.tm_yday + tm_t.tm_year * 400 == today ? "%H:%M" : "%d.%m.", &tm_t);
}

/* three columns: picture | name, second line | time */
HOOKPROTONH(room_disp_func, LONG, char **array, Room *r)
{
    static char img[24], text[400], when[16];
    char second[200];
    if (!r) { array[0] = array[1] = array[2] = ""; return 0; }
    snprintf(img, sizeof img, "\33A[%08lx]", (unsigned long)&image_for(r)->ad);
    second_line(r, second, sizeof second);
    if (r->invited)
        snprintf(text, sizeof text, "\33b%s\33n\n\33i%s", r->name, second);
    else if (r->unread)                  /* unread: count, second line in normal colour */
        snprintf(text, sizeof text, "\33b%s (%d)\33n\n\33i%s", r->name, r->unread, second);
    else
        snprintf(text, sizeof text, "\33b%s\33n\n\33i\33P[808080]%s", r->name, second);
    time_text(r->last_ts, when, sizeof when);
    snprintf(when + strlen(when), sizeof when - strlen(when), "\n ");
    array[0] = img;
    array[1] = text;
    array[2] = when;
    return 0;
}
MakeStaticHook(room_disp_hook, room_disp_func);

static void free_rooms(Room *l, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        free(l[i].id); free(l[i].name); free(l[i].dm_user); free(l[i].inviter); free(l[i].avatar_url);
        free(l[i].topic); free(l[i].last_sender); free(l[i].last_body);
    }
    free(l);
}

static void open_chat(const char *room_id);

static void update_rooms(const AmxMsg *m)
{
    amx_log("room list: %d rooms", m->n);
    Room *nl = calloc(m->n + 1, sizeof *nl), *act = NULL;
    char *active = NULL;
    ChatWin *w;
    int i;
    LONG pos = MUIV_List_Active_Off;

    if (!nl) return;
    DoMethod(lst_rooms, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &act);
    if (act) active = strdup(act->id);
    for (i = 0; i < m->n; i++) {
        const AmxRoomInfo *ri = &m->rooms[i];
        nl[i].id = strdup(ri->room_id);
        nl[i].name = local(ri->name);
        nl[i].dm_user = xstrdup(ri->dm_user);
        nl[i].inviter = ri->inviter_name ? local(ri->inviter_name) : ri->inviter ? local(ri->inviter) : NULL;
        nl[i].avatar_url = xstrdup(ri->avatar_url);
        nl[i].invited = ri->membership == AMX_ROOM_INVITED;
        nl[i].unread = ri->unread;
        nl[i].highlights = ri->highlights;
        nl[i].direct = ri->is_direct;
        nl[i].encrypted = ri->encrypted;
        nl[i].members = ri->joined_members;
        nl[i].topic = ri->topic ? local(ri->topic) : NULL;
        nl[i].last_sender = ri->last_sender ? local(ri->last_sender) : NULL;
        nl[i].last_body = ri->last_body ? local_emoji_as(ri->last_body, 1) : NULL;
        nl[i].last_own = ri->last_own;
        nl[i].last_ts = ri->last_activity;
    }
    set(lst_rooms, MUIA_List_Quiet, TRUE);
    DoMethod(lst_rooms, MUIM_List_Clear);
    free_rooms(rooms, n_rooms);
    rooms = nl;
    n_rooms = m->n;
    for (i = 0; i < n_rooms; i++) {
        DoMethod(lst_rooms, MUIM_List_InsertSingle, &rooms[i], MUIV_List_Insert_Bottom);
        if (active && !strcmp(active, rooms[i].id)) pos = i;
    }
    set(lst_rooms, MUIA_List_Active, pos);
    set(lst_rooms, MUIA_List_Quiet, FALSE);
    free(active);
    request_avatars();
    for (w = chats; w; w = w->next) { w->mark_pending = 0; update_status(w); }
    for (w = chats; w; w = w->next) {      /* window titles follow renames */
        Room *r = room_by_id(w->room_id);
        if (r && (!w->title || strcmp(w->title, r->name))) {
            free(w->title);
            w->title = strdup(r->name);
            set(w->win, MUIA_Window_Title, w->title);
        }
    }
    if (pending_open && room_by_id(pending_open) && !room_by_id(pending_open)->invited) {
        char *id = pending_open;
        pending_open = NULL;
        open_chat(id);
        free(id);
    }
}

/* -------------------------------------------------------------- chat windows */

/* One line of the chat list: the text and the event it belongs to (a
 * message with line breaks becomes several lines with the same event). */
typedef struct {
    char *eid;
    char *text;
    int undec;                    /* placeholder for a message we cannot decrypt (yet) */
    int react;                    /* the reactions line under the message eid */
    int own;                      /* our own message */
    long serial;                  /* reactions line: number for its clickable pictures */
} Line;

static Line *new_line(const char *eid, const char *text, size_t n, int undec)
{
    Line *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    l->eid = xstrdup(eid);
    l->text = malloc(n + 1);
    if (l->text) { memcpy(l->text, text, n); l->text[n] = 0; }
    l->undec = undec;
    return l;
}

static void free_line(Line *l)
{
    if (!l) return;
    free(l->eid); free(l->text); free(l);
}

HOOKPROTONHNO(line_disp_func, LONG, struct NList_DisplayMessage *m)
{
    const Line *l = m->entry;
    m->strings[0] = l && l->text ? l->text : "";
    return 0;
}
MakeStaticHook(line_disp_hook, line_disp_func);

HOOKPROTONH(chat_close_func, LONG, Object *obj, ULONG *param)
{
    ChatWin *w = (ChatWin *)param[0];
    (void)obj;
    w->close_req = 1;
    return 0;
}
MakeStaticHook(chat_close_hook, chat_close_func);

static void chat_send(ChatWin *w)
{
    char *text = (char *)xget(w->input, MUIA_String_Contents), *u;
    if (!text || !*text) return;
    u = amx_from_local(text);
    amx_async_send_text(A, w->room_id, u, TAG_SEND);
    fifo_push(fifo_send, &n_send, 64, w->room_id);
    w->sending++;
    w->err[0] = 0;
    update_status(w);
    free(u);
    set(w->input, MUIA_String_Contents, "");
    set(w->win, MUIA_Window_ActiveObject, w->input);
}

HOOKPROTONH(chat_send_func, LONG, Object *obj, ULONG *param)
{
    (void)obj;
    chat_send((ChatWin *)param[0]);
    return 0;
}
MakeStaticHook(chat_send_hook, chat_send_func);

HOOKPROTONH(chat_older_func, LONG, Object *obj, ULONG *param)
{
    ChatWin *w = (ChatWin *)param[0];
    (void)obj;
    if (!w->loading && !w->at_start) {
        w->loading = 1;
        w->loading_older = 1;
        w->err[0] = 0;
        set(w->older, MUIA_Disabled, TRUE);
        amx_async_load_history(A, w->room_id, OLDER_PAGE, 0, TAG_HIST);
        update_status(w);
    }
    return 0;
}
MakeStaticHook(chat_older_hook, chat_older_func);

static Room *room_by_id(const char *id);

/* toolbar "invite": a small window for the Matrix ID */
static void show_invite(const char *room_id)
{
    static char title[200];
    Room *r = room_by_id(room_id);
    free(invite_room);
    invite_room = strdup(room_id);
    snprintf(title, sizeof title, "Einladen in \33b%s", r ? r->name : room_id);
    set(txt_inviteroom, MUIA_Text_Contents, title);
    set(str_inviteuser, MUIA_String_Contents, "");
    set(win_invite, MUIA_Window_Open, TRUE);
    set(win_invite, MUIA_Window_ActiveObject, str_inviteuser);
}

/* leaves a room for good (or declines an invitation); its chat window is
 * closed.  ask: confirm first. */
static void leave_room(const char *room_id, int ask)
{
    Room *r = room_by_id(room_id);
    ChatWin *w = chat_by_room(room_id);
    if (!A || !r) return;
    if (ask) {
        char *name = r->name ? r->name : r->id;
        LONG ok = r->invited
            ? MUI_Request(app, w ? w->win : win_main, 0, (STRPTR)"Amatrix", (STRPTR)"*_Ablehnen|_Abbrechen",
                          (STRPTR)"Einladung in \33b%s\33n ablehnen?", (ULONG)name)
            : MUI_Request(app, w ? w->win : win_main, 0, (STRPTR)"Amatrix", (STRPTR)"*_Verlassen|_Abbrechen",
                          (STRPTR)"\33b%s\33n wirklich verlassen?\n\n"
                          "Du bekommst dann keine Nachrichten mehr aus diesem Raum.\n"
                          "Zurück geht es nur mit einer neuen Einladung\n"
                          "(öffentliche Räume: über \"Raum betreten\").", (ULONG)name);
        if (ok != 1) return;
    }
    amx_async_leave(A, room_id, TAG_LEAVE);
    if (w) w->close_req = 1;             /* closed in the main loop, not inside its own button */
    set_status("Verlasse %s ...", r->name ? r->name : r->id, NULL);
}

HOOKPROTONH(chat_leave_func, LONG, Object *obj, ULONG *param)
{
    (void)obj;
    leave_room(((ChatWin *)param[0])->room_id, 1);
    return 0;
}
MakeStaticHook(chat_leave_hook, chat_leave_func);

HOOKPROTONH(chat_invite_func, LONG, Object *obj, ULONG *param)
{
    (void)obj;
    show_invite(((ChatWin *)param[0])->room_id);
    return 0;
}
MakeStaticHook(chat_invite_hook, chat_invite_func);

/* our reaction with key (or one like it) to target: take it back if we have
 * one, else send it (the picture's usual key, a heart with U+FE0F like Element) */
static void react_toggle(ChatWin *w, const char *target, const char *key)
{
    int i, img = emo_index(key);
    unsigned k;
    for (i = 0; i < w->n_react; i++)
        if (w->react[i].own && !strcmp(w->react[i].target, target) && same_key(w->react[i].key, key)) {
            amx_async_redact(A, w->room_id, w->react[i].id, TAG_REACT);
            return;
        }
    if (img == EMO_HEART) key = "\xe2\x9d\xa4\xef\xb8\x8f";
    else if (img >= 0)
        for (k = 0; k < sizeof emo_keys / sizeof emo_keys[0]; k++)
            if (emo_keys[k].img == img) { key = emo_keys[k].key; break; }
    amx_async_react(A, w->room_id, target, key, TAG_REACT);
}

/* the message a reaction is for: the marked one, else the newest of someone else */
static const char *react_target(ChatWin *w)
{
    Line *l = NULL;
    LONG n, k;
    DoMethod(w->list, MUIM_NList_GetEntry, MUIV_NList_GetEntry_Active, &l);
    if (l && l->eid) return l->eid;             /* a reactions line: its message */
    n = (LONG)xget(w->list, MUIA_NList_Entries);
    for (k = n - 1; k >= 0; k--) {
        DoMethod(w->list, MUIM_NList_GetEntry, k, &l);
        if (l && !l->react && !l->own && l->eid) return l->eid;
    }
    return NULL;
}

/* a click on a picture in a reactions line */
HOOKPROTONH(chip_func, LONG, Object *obj, ULONG *param)
{
    ChatWin *w = (ChatWin *)param[0];
    long v = (long)param[1], serial = v / 16;
    int group = (int)(v % 16), i, j, g = 0;
    LONG n = (LONG)xget(w->list, MUIA_NList_Entries), k;
    (void)obj;
    for (k = 0; k < n; k++) {
        Line *l = NULL;
        DoMethod(w->list, MUIM_NList_GetEntry, k, &l);
        if (!l || !l->react || l->serial != serial) continue;
        for (i = 0; i < w->n_react; i++) {               /* the groups in reaction_text's order */
            if (strcmp(w->react[i].target, l->eid)) continue;
            for (j = 0; j < i; j++)
                if (!strcmp(w->react[j].target, l->eid) && same_key(w->react[j].key, w->react[i].key)) break;
            if (j < i) continue;
            if (g++ == group) { react_toggle(w, l->eid, w->react[i].key); return 0; }
        }
        return 0;
    }
    return 0;
}
MakeStaticHook(chip_hook, chip_func);

/* the picker: a small window with the pictures, opened by the smiley button */
static Object *win_pick, *pick_btn[N_EMO];
static ChatWin *pick_w;
static char *pick_eid;
static const char *const emo_help[N_EMO] = { "Daumen hoch", "Daumen runter", "Herz", "Lächeln", "Lachen",
                                             "Nachdenklich", "Augen", "Feiern", "Rakete", "" };

static void open_picker(ChatWin *w)
{
    const char *t = react_target(w);
    if (!t || !win_pick) return;
    free(pick_eid);
    pick_eid = strdup(t);
    pick_w = w;
    set(win_pick, MUIA_Window_Open, FALSE);
    set(win_pick, MUIA_Window_Open, TRUE);
}

HOOKPROTONH(pick_func, LONG, Object *obj, ULONG *param)
{
    int i = (int)param[0];
    unsigned k;
    (void)obj;
    set(win_pick, MUIA_Window_Open, FALSE);
    if (!pick_w || !pick_eid || i < 0 || i >= EMO_UNKNOWN) return 0;
    for (k = 0; k < sizeof emo_keys / sizeof emo_keys[0]; k++)
        if (emo_keys[k].img == i) { react_toggle(pick_w, pick_eid, emo_keys[k].key); break; }
    return 0;
}
MakeStaticHook(pick_hook, pick_func);

HOOKPROTONH(chat_react_func, LONG, Object *obj, ULONG *param)
{
    (void)obj;
    open_picker((ChatWin *)param[0]);
    return 0;
}
MakeStaticHook(chat_react_hook, chat_react_func);

static Object *make_picker(void)
{
    Object *g = HGroup, MUIA_Group_Spacing, 2, End;
    int i;
    win_pick = WindowObject,
        MUIA_Window_Title, "Reaktion",
        MUIA_Window_LeftEdge, MUIV_Window_LeftEdge_Moused,
        MUIA_Window_TopEdge, MUIV_Window_TopEdge_Moused,
        MUIA_Window_SizeGadget, FALSE,
        MUIA_Window_DepthGadget, FALSE,
        MUIA_Window_ID, 0,
        WindowContents, g,
    End;
    for (i = 0; win_pick && i < EMO_UNKNOWN; i++) {
        pick_btn[i] = icon_button(&emo_big[i], emo_help[i]);
        if (!pick_btn[i]) continue;
        DoMethod(g, OM_ADDMEMBER, pick_btn[i]);
        DoMethod(pick_btn[i], MUIM_Notify, MUIA_Pressed, FALSE, MUIV_Notify_Application, 3,
                 MUIM_CallHook, &pick_hook, i);
    }
    if (win_pick) {
        DoMethod(win_pick, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, win_pick, 3,
                 MUIM_Set, MUIA_Window_Open, FALSE);
        DoMethod(win_pick, MUIM_Notify, MUIA_Window_Activate, FALSE, win_pick, 3,
                 MUIM_Set, MUIA_Window_Open, FALSE);       /* closes like a popup menu */
    }
    return win_pick;
}

static void open_chat(const char *room_id)
{
    amx_log("open_chat %s", room_id);
    ChatWin *w = chat_by_room(room_id);
    Room *r = room_by_id(room_id);
    unsigned long h;
    LONG left, top, width;

    if (w) {
        set(w->win, MUIA_Window_Open, TRUE);
        DoMethod(w->win, MUIM_Window_ToFront);
        set(w->win, MUIA_Window_Activate, TRUE);
        return;
    }
    if (!(w = calloc(1, sizeof *w))) return;
    w->room_id = strdup(room_id);
    w->title = strdup(r ? r->name : room_id);
    h = hash(room_id);
    {   /* beside the main window (right, else left), cascading */
        struct Screen *scr = (struct Screen *)xget(win_main, MUIA_Window_Screen);
        LONG sw = scr ? scr->Width : 640, ml = (LONG)xget(win_main, MUIA_Window_LeftEdge);
        LONG mw = (LONG)xget(win_main, MUIA_Window_Width);
        ChatWin *c;
        int n = 0;
        for (c = chats; c; c = c->next) n++;
        width = sw * 45 / 100;
        if (width < 300) width = 300;
        if (ml + mw + 4 + width <= sw) left = ml + mw + 4;
        else if (ml - 4 - width >= 0) left = ml - 4 - width;
        else left = sw - width;
        left += (n % 6) * 16;
        if (left + width > sw) left = sw - width;
        if (left < 0) left = 0;
        top = (LONG)xget(win_main, MUIA_Window_TopEdge) + (n % 6) * 16;
    }
    w->win = WindowObject,
        MUIA_Window_Title, w->title,
        MUIA_Window_ID, MAKE_ID('C', (h >> 16) & 255, (h >> 8) & 255, h & 255),
        MUIA_Window_LeftEdge, left,
        MUIA_Window_TopEdge, top,
        MUIA_Window_Width, width,
        MUIA_Window_Height, MUIV_Window_Height_Visible(55),
        WindowContents, VGroup,
            Child, HGroup,                          /* toolbar */
                Child, w->older = icon_button(&icon_up, OLDER_HELP),
                Child, w->reactb = icon_button(&icon_react, REACT_HELP),
                Child, w->invite = icon_button(&icon_invite, INVITE_HELP),
                Child, w->leave = icon_button(&icon_leave, LEAVE_HELP),
                Child, w->busy = TextObject,
                    MUIA_Text_Contents, "",
                    MUIA_Text_PreParse, "\33r",
                    MUIA_Weight, 100,
                End,
            End,
            Child, NListviewObject,
                MUIA_NListview_NList, w->list = NListObject,
                    MUIA_NList_DisplayHook2, &line_disp_hook,
                    MUIA_NList_Input, TRUE,
                    MUIA_NList_AutoVisible, TRUE,
                End,
            End,
            Child, w->input = BetterStringObject,
                StringFrame,
                MUIA_String_MaxLen, 2000,
                MUIA_CycleChain, TRUE,
            End,
        End,
    End;
    if (w->win) {
        int k;
        for (k = 0; k < N_EMO; k++) {
            static char spec[N_EMO][24];
            snprintf(spec[k], sizeof spec[k], "\33A[%08lx]", (unsigned long)&emo_img[k].ad);
            w->emo[k] = TextObject, MUIA_Text_Contents, spec[k], MUIA_Text_SetMax, TRUE,
                        MUIA_InnerLeft, 0, MUIA_InnerRight, 0, MUIA_InnerTop, 0, MUIA_InnerBottom, 0, End;
            if (w->emo[k]) DoMethod(w->list, MUIM_NList_UseImage, w->emo[k], k, 0);
        }
    }
    if (!w->win) { free(w->room_id); free(w->title); free(w); return; }
    DoMethod(app, OM_ADDMEMBER, w->win);
    DoMethod(w->win, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, MUIV_Notify_Application, 3,
             MUIM_CallHook, &chat_close_hook, w);
    DoMethod(w->input, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime, MUIV_Notify_Application, 3,
             MUIM_CallHook, &chat_send_hook, w);
    DoMethod(w->older, MUIM_Notify, MUIA_Pressed, FALSE, MUIV_Notify_Application, 3,
             MUIM_CallHook, &chat_older_hook, w);
    DoMethod(w->invite, MUIM_Notify, MUIA_Pressed, FALSE, MUIV_Notify_Application, 3,
             MUIM_CallHook, &chat_invite_hook, w);
    DoMethod(w->leave, MUIM_Notify, MUIA_Pressed, FALSE, MUIV_Notify_Application, 3,
             MUIM_CallHook, &chat_leave_hook, w);
    DoMethod(w->reactb, MUIM_Notify, MUIA_Pressed, FALSE, MUIV_Notify_Application, 3,
             MUIM_CallHook, &chat_react_hook, w);
    DoMethod(w->list, MUIM_Notify, MUIA_NList_ButtonClick, MUIV_EveryTime, MUIV_Notify_Application, 4,
             MUIM_CallHook, &chip_hook, w, MUIV_TriggerValue);
    set(w->win, MUIA_Window_ActiveObject, w->input);
    w->next = chats;
    chats = w;
    set(w->win, MUIA_Window_Open, TRUE);
    amx_log("chat window open (%ld,%ld %ldx%ld)", (long)left, (long)top, (long)width,
            (long)xget(w->win, MUIA_Window_Height));
    w->loading = 1;
    set(w->older, MUIA_Disabled, TRUE);
    amx_async_load_history(A, room_id, FIRST_PAGE, 1, TAG_HIST);
    update_status(w);
}

static void close_chat(ChatWin *w)
{
    ChatWin **p;
    int i;
    for (p = &chats; *p; p = &(*p)->next) if (*p == w) { *p = w->next; break; }
    if (pick_w == w) { pick_w = NULL; if (win_pick) set(win_pick, MUIA_Window_Open, FALSE); }
    set(w->win, MUIA_Window_Open, FALSE);
    DoMethod(w->list, MUIM_NList_Clear);
    DoMethod(w->list, MUIM_NList_UseImage, NULL, MUIV_NList_UseImage_All, 0);
    DoMethod(app, OM_REMMEMBER, w->win);
    MUI_DisposeObject(w->win);
    for (i = 0; i < N_EMO; i++) if (w->emo[i]) MUI_DisposeObject(w->emo[i]);
    for (i = 0; i < w->n_react; i++) {
        free(w->react[i].id); free(w->react[i].target); free(w->react[i].key); free(w->react[i].sender);
    }
    free(w->react);
    for (i = 0; i < w->n_all; i++) free_line(w->all[i]);    /* each exactly once */
    free(w->all);
    for (i = 0; i < w->n_seen; i++) free(w->seen[i]);
    free(w->seen);
    free(w->room_id); free(w->last_event); free(w->marked); free(w->title);
    free(w);
}

static int seen_before(ChatWin *w, const char *id)
{
    int i;
    char **t;
    if (!id) return 0;
    for (i = 0; i < w->n_seen; i++) if (!strcmp(w->seen[i], id)) return 1;
    if (w->n_seen == w->cap_seen) {
        t = realloc(w->seen, sizeof *t * (w->cap_seen ? w->cap_seen * 2 : 64));
        if (!t) return 0;
        w->seen = t;
        w->cap_seen = w->cap_seen ? w->cap_seen * 2 : 64;
    }
    w->seen[w->n_seen++] = strdup(id);
    return 0;
}

/* one event as text lines for the chat list (local charset, NList codes) */
static char *format_event(const AmxEvent *e)
{
    char *name = local(e->sender_name ? e->sender_name : e->sender), *body, *out, tbuf[16] = "";
    const char *mt = e->msgtype ? e->msgtype : "";
    size_t n;
    if (e->timestamp > 0) {
        time_t t = (time_t)(e->timestamp / 1000);
        struct tm *tm = localtime(&t);
        if (tm) strftime(tbuf, sizeof tbuf, "%H:%M ", tm);
    }
    switch (e->type) {
    case AMX_EV_MESSAGE:
        body = local_emoji(e->body);
        n = strlen(name) + strlen(body) + 64;
        out = malloc(n);
        if (!strcmp(mt, "m.emote"))
            snprintf(out, n, "%s\33i* %s %s", tbuf, name, body);
        else if (!strcmp(mt, "m.notice"))
            snprintf(out, n, "%s\33b%s:\33n \33i%s", tbuf, name, body);
        else if (!strcmp(mt, "m.image") || !strcmp(mt, "m.file") || !strcmp(mt, "m.video") || !strcmp(mt, "m.audio"))
            snprintf(out, n, "%s\33b%s:\33n [%s: %s]", tbuf, name,
                     !strcmp(mt, "m.image") ? "Bild" : !strcmp(mt, "m.video") ? "Video" :
                     !strcmp(mt, "m.audio") ? "Audio" : "Datei", body);
        else
            snprintf(out, n, "%s\33b%s:\33n %s", tbuf, name, body);
        free(body);
        break;
    case AMX_EV_UNDECRYPTABLE:
        n = strlen(name) + 96 + (e->reason ? strlen(e->reason) : 0);
        out = malloc(n);
        if (e->reason && !strcmp(e->reason, "UNKNOWN_MESSAGE_INDEX"))   /* key only from a later message on */
            snprintf(out, n, "%s\33b%s:\33n \33i(verschlüsselt, Schlüssel gilt erst ab einer späteren Nachricht)", tbuf, name);
        else if (!e->reason || !strcmp(e->reason, "no key"))
            snprintf(out, n, "%s\33b%s:\33n \33i(verschlüsselt, kein Schlüssel auf diesem Gerät)", tbuf, name);
        else
            snprintf(out, n, "%s\33b%s:\33n \33i(verschlüsselt, nicht lesbar: %s)", tbuf, name, e->reason);
        break;
    case AMX_EV_MEMBERSHIP: {
        const char *what = e->body ? e->body : "";
        char *who = local(e->state_key ? e->state_key : "");
        n = strlen(who) + 80;
        out = malloc(n);
        snprintf(out, n, "%s\33i%s %s", tbuf, who,
                 !strcmp(what, "join") ? "ist im Raum" : !strcmp(what, "leave") ? "hat den Raum verlassen" :
                 !strcmp(what, "invite") ? "wurde eingeladen" : !strcmp(what, "ban") ? "wurde gesperrt" : what);
        free(who);
        break;
    }
    default:
        out = NULL;
    }
    free(name);
    return out;
}


/* inserts text (split at line breaks) at pos (entry index, or Top/Bottom) */
static void remember_line(ChatWin *w, void *l)
{
    if (w->n_all == w->cap_all) {
        void **t = realloc(w->all, sizeof *t * (w->cap_all ? w->cap_all * 2 : 128));
        if (!t) return;
        w->all = t;
        w->cap_all = w->cap_all ? w->cap_all * 2 : 128;
    }
    w->all[w->n_all++] = l;
}

static void insert_text(ChatWin *w, const char *eid, const char *text, LONG pos, int undec, int own)
{
    const char *p = text, *nl;
    Line *ls[64];
    int n = 0, i;
    do {
        nl = strchr(p, '\n');
        if (n < 64) ls[n++] = new_line(eid, p, nl ? (size_t)(nl - p) : strlen(p), undec);
        p = nl ? nl + 1 : NULL;
    } while (p);
    for (i = 0; i < n; i++) {
        if (!ls[i]) continue;
        ls[i]->own = own;
        remember_line(w, ls[i]);
        DoMethod(w->list, MUIM_NList_InsertSingleWrap, ls[i],
                 pos == MUIV_NList_Insert_Bottom ? pos : pos == MUIV_NList_Insert_Top ? i : pos + i,
                 WRAPCOL0, ALIGN_LEFT);
        w->lines++;
    }
    if (undec) w->n_undec++;
}

/* a message shown as undecryptable was decrypted later (its key arrived):
 * the placeholder line is replaced in place; 1 if there was one */
static int replace_undecryptable(ChatWin *w, const AmxEvent *e, const char *text)
{
    LONG n = (LONG)xget(w->list, MUIA_NList_Entries), k;
    for (k = 0; k < n; k++) {
        Line *l = NULL;
        char *t, *q;
        DoMethod(w->list, MUIM_NList_GetEntry, k, &l);
        if (!l || !l->undec || !l->eid || strcmp(l->eid, e->event_id)) continue;
        /* change the entry in place (nothing is freed while NList uses it);
         * line breaks become "  " in this one entry */
        if (!(t = strdup(text))) return 0;
        for (q = t; *q; q++) if (*q == '\n') *q = ' ';
        free(l->text);
        l->text = t;
        l->undec = 0;
        w->n_undec--;
        DoMethod(w->list, MUIM_NList_ReplaceSingle, l, k, WRAPCOL0, ALIGN_LEFT);
        return 1;
    }
    return 0;
}

/* remembers the newest message shown; the read marker follows it (read_markers) */
static void note_newest(ChatWin *w, const AmxEvent *e)
{
    if (e->type != AMX_EV_MESSAGE || !e->event_id || e->timestamp < w->last_ts) return;
    w->last_ts = e->timestamp;
    free(w->last_event);
    w->last_event = strdup(e->event_id);
}

/* once per round: open chat windows are read up to their newest message */
static void send_read_markers(void)
{
    ChatWin *w;
    for (w = chats; w; w = w->next) {
        Room *r = room_by_id(w->room_id);
        if (!w->last_event || w->loading) continue;
        if (w->marked && !strcmp(w->marked, w->last_event) && !(r && r->unread)) continue;
        if (w->marked && !strcmp(w->marked, w->last_event) && r && r->unread && w->mark_pending) continue;
        amx_async_mark_read(A, w->room_id, w->last_event, TAG_READ);
        free(w->marked);
        w->marked = strdup(w->last_event);
        w->mark_pending = 1;
    }
}

/* ---------------------------------------------------------------- reactions */

/* the reactions line for target: "<picture> 2   <picture> 1", grouped by key in
 * the order they came; our own count in bold.  "" if none. */
/* reactions to target in the group of reaction i: how many people (one
 * person with a heart and a kiss counts once), and whether we are one */
static int group_count(ChatWin *w, const char *target, int i, int *own)
{
    int j, k, count = 0;
    *own = 0;
    for (j = i; j < w->n_react; j++) {
        if (strcmp(w->react[j].target, target) || !same_key(w->react[j].key, w->react[i].key)) continue;
        for (k = i; k < j; k++)
            if (!strcmp(w->react[k].target, target) && same_key(w->react[k].key, w->react[i].key)
                && !strcmp(w->react[k].sender, w->react[j].sender)) break;
        if (k < j) continue;
        count++;
        *own |= w->react[j].own;
    }
    return count;
}

static void reaction_text(ChatWin *w, const char *target, long serial, char *buf, size_t n)
{
    int i, j, any = 0, g = 0;
    size_t len;
    snprintf(buf, n, "      ");
    for (i = 0; i < w->n_react; i++) {
        const React *r = &w->react[i];
        int count = 0, own = 0, img;
        if (strcmp(r->target, target)) continue;
        for (j = 0; j < i; j++)                      /* this key counted already? */
            if (!strcmp(w->react[j].target, target) && same_key(w->react[j].key, r->key)) break;
        if (j < i) continue;
        count = group_count(w, target, i, &own);
        len = strlen(buf);
        if (len + 64 >= n) break;
        any = 1;
        if ((img = emo_index(r->key)) < 0) img = EMO_UNKNOWN;
        /* a click on the picture (button value serial*16+group) toggles ours */
        snprintf(buf + len, n - len, "\33o[%d@%ld] %s%d%s   ", img, serial * 16 + (g < 15 ? g : 15),
                 own ? "\33b" : "", count, own ? "\33n" : "");
        g++;
    }
    if (!any) buf[0] = 0;
}

/* shows, updates or removes the reactions line under the message target
 * (if the message is in the list) */
static void refresh_reactions(ChatWin *w, const char *target)
{
    char text[600];
    LONG n = (LONG)xget(w->list, MUIA_NList_Entries), k, last = -1, rl = -1;
    Line *rline = NULL;
    for (k = 0; k < n; k++) {
        Line *l = NULL;
        DoMethod(w->list, MUIM_NList_GetEntry, k, &l);
        if (!l || !l->eid || strcmp(l->eid, target)) continue;
        if (l->react) { rl = k; rline = l; }
        else last = k;
    }
    if (last < 0) return;                       /* the message is not shown (yet) */
    reaction_text(w, target, rline ? rline->serial : w->next_serial + 1, text, sizeof text);
    if (!text[0]) {
        if (rl >= 0) { DoMethod(w->list, MUIM_NList_Remove, rl); w->lines--; }
        return;
    }
    if (rline) {
        char *t = strdup(text);
        if (!t) return;
        free(rline->text);
        rline->text = t;
        DoMethod(w->list, MUIM_NList_ReplaceSingle, rline, rl, NOWRAP, ALIGN_LEFT);
    } else {
        Line *l = new_line(target, text, strlen(text), 0);
        if (!l) return;
        l->react = 1;
        l->serial = ++w->next_serial;
        remember_line(w, l);
        DoMethod(w->list, MUIM_NList_InsertSingle, l, last + 1);
        w->lines++;
    }
}

static int has_reactions(ChatWin *w, const char *target)
{
    int i;
    for (i = 0; target && i < w->n_react; i++) if (!strcmp(w->react[i].target, target)) return 1;
    return 0;
}

/* a placeholder line whose event turned out to be a reaction: away with it */
static void remove_placeholder(ChatWin *w, const char *eid)
{
    LONG n = (LONG)xget(w->list, MUIA_NList_Entries), k;
    for (k = n - 1; k >= 0; k--) {
        Line *l = NULL;
        DoMethod(w->list, MUIM_NList_GetEntry, k, &l);
        if (l && l->undec && l->eid && !strcmp(l->eid, eid)) {
            DoMethod(w->list, MUIM_NList_Remove, k);
            l->undec = 0;
            w->n_undec--;
            w->lines--;
        }
    }
}

static void reaction_event(ChatWin *w, const AmxEvent *e)
{
    int i;
    if (e->type == AMX_EV_REDACTION) {
        for (i = 0; i < w->n_react; i++) {
            React *r = &w->react[i];
            char *target;
            if (strcmp(r->id, e->relates_to)) continue;
            target = r->target;
            free(r->id); free(r->key); free(r->sender);
            memmove(r, r + 1, sizeof *r * (w->n_react - i - 1));
            w->n_react--;
            refresh_reactions(w, target);
            free(target);
            return;
        }
        return;
    }
    if (!e->event_id || !e->relates_to || !e->body) return;
    if (w->n_undec) remove_placeholder(w, e->event_id);
    for (i = 0; i < w->n_react; i++) if (!strcmp(w->react[i].id, e->event_id)) return;   /* history + live */
    if (w->n_react == w->cap_react) {
        React *t = realloc(w->react, sizeof *t * (w->cap_react ? w->cap_react * 2 : 32));
        if (!t) return;
        w->react = t;
        w->cap_react = w->cap_react ? w->cap_react * 2 : 32;
    }
    w->react[w->n_react].id = strdup(e->event_id);
    w->react[w->n_react].target = strdup(e->relates_to);
    w->react[w->n_react].key = strdup(e->body);
    w->react[w->n_react].sender = strdup(e->sender ? e->sender : "");
    w->react[w->n_react].own = e->own;
    w->n_react++;
    refresh_reactions(w, e->relates_to);
}

static void chat_event(const AmxEvent *e)
{
    ChatWin *w = e->room_id ? chat_by_room(e->room_id) : NULL;
    char *text;
    if (!w) return;
    amx_log("chat_event type %d %s hist %d body %ld", (int)e->type, e->event_id ? e->event_id : "-", e->history,
            (long)(e->body ? strlen(e->body) : -1));
    if (e->type == AMX_EV_REACTION || e->type == AMX_EV_REDACTION) {
        reaction_event(w, e);
        return;
    }
    if (e->type == AMX_EV_MESSAGE && e->event_id && w->n_undec) {
        if ((text = format_event(e)) != NULL && replace_undecryptable(w, e, text)) {
            free(text);
            note_newest(w, e);
            return;
        }
        free(text);
    }
    if (seen_before(w, e->event_id)) return;
    if (e->type == AMX_EV_MEMBERSHIP && e->history) return;     /* keep old history quiet */
    if (!(text = format_event(e))) return;
    insert_text(w, e->event_id, text, e->history ? MUIV_NList_Insert_Top : MUIV_NList_Insert_Bottom,
                e->type == AMX_EV_UNDECRYPTABLE && e->event_id, e->own);
    amx_log("  inserted (%ld lines)", (long)w->lines);
    free(text);
    if (has_reactions(w, e->event_id)) refresh_reactions(w, e->event_id);   /* came first (history) */
    if (!e->history) DoMethod(w->list, MUIM_NList_Jump, MUIV_NList_Jump_Bottom);
    note_newest(w, e);
}

static void history_done(const AmxMsg *m)
{
    ChatWin *w = m->result ? chat_by_room(m->result) : NULL;
    amx_log("history_done %s status %d count %d at_start %d", m->result ? m->result : "-", m->status, m->count,
            m->at_start);
    if (!w) return;
    w->loading = 0;
    w->loading_older = 0;
    if (m->status) snprintf(w->err, sizeof w->err, "Verlauf: %.140s", m->error ? m->error : "Fehler");
    update_status(w);
    w->at_start = m->status == 0 && m->at_start;
    set(w->older, MUIA_Disabled, w->at_start);
    if (w->at_start) set(w->older, MUIA_ShortHelp, OLDER_HELP_END);
    if (m->status) return;
    if (!xget(w->list, MUIA_NList_First) || w->lines <= m->count + 1)
        DoMethod(w->list, MUIM_NList_Jump, MUIV_NList_Jump_Bottom);
    amx_log("history_done finished");
}

/* --------------------------------------------------------------- dialogs */

static void do_login(const char *server, const char *user, const char *password)
{
    char dir[300];
    char *s = strdup(server), *u = strdup(user);     /* server/user may be cfg_* themselves */
    if (A) { amx_async_free(A); A = NULL; }
    free(cfg_server); cfg_server = s;
    free(cfg_user); cfg_user = u;
    server = s; user = u;
    store_dir(dir, sizeof dir, server, user);
    A = amx_async_new(server, dir);
    if (!A) { set_status("Netzprozess konnte nicht starten", NULL, NULL); return; }
    {
        char v[16];
        if (GetVar((STRPTR)"AMX_VERBOSE", (STRPTR)v, sizeof v, 0) > 0) amx_async_set_verbose(A, atoi(v));
        if (debug) amx_async_set_verbose(A, 2);
    }
    store_protected = 0;
    {   /* a protected store needs its passphrase before the login */
        char sj[320];
        BPTR l;
        snprintf(sj, sizeof sj, "%s/store.json", dir);
        if ((l = Lock((STRPTR)sj, ACCESS_READ)) != 0) {
            UnLock(l);
            store_protected = 1;
            free(pending_password);
            pending_password = xstrdup(password);
            set_status("Passwort für die Amatrix-Daten nötig", NULL, NULL);
            set(txt_unlock, MUIA_Text_Contents, "");
            set(b_unlock, MUIA_Disabled, FALSE);
            set(win_unlock, MUIA_Window_Open, TRUE);
            set(win_unlock, MUIA_Window_ActiveObject, str_unlock);
            return;
        }
    }
    set_status("Anmelden als %s ...", user, NULL);
    amx_async_login(A, user, password ? password : "", TAG_LOGIN);
}

static void unlock_go(const char *typed)
{
    char *t = (char *)(typed ? typed : (const char *)xget(str_unlock, MUIA_String_Contents)), *u;
    if (!t || !A) return;
    u = amx_from_local(t);
    amx_async_store_unlock(A, u, TAG_UNLOCK);
    memset(u, 0, strlen(u));
    free(u);
    set(str_unlock, MUIA_String_Contents, "");
    set(txt_unlock, MUIA_Text_Contents, "Wird geprüft ...");
    set(b_unlock, MUIA_Disabled, TRUE);
}

static void storepass_go(int remove_it, const char *typed)
{
    char *p1 = (char *)(typed ? typed : (const char *)xget(str_sp1, MUIA_String_Contents));
    char *p2 = (char *)(typed ? typed : (const char *)xget(str_sp2, MUIA_String_Contents)), *u;
    if (!A || !my_user) return;
    if (!remove_it) {
        if (!p1 || strlen(p1) < 4) { set(txt_sp, MUIA_Text_Contents, "Bitte mindestens 4 Zeichen."); return; }
        if (strcmp(p1, p2)) { set(txt_sp, MUIA_Text_Contents, "Die beiden Eingaben sind verschieden."); return; }
    }
    store_protected_removing = remove_it;
    u = remove_it ? strdup("") : amx_from_local(p1);
    amx_async_store_passphrase(A, u, TAG_STOREPASS);
    memset(u, 0, strlen(u));
    free(u);
    set(str_sp1, MUIA_String_Contents, "");
    set(str_sp2, MUIA_String_Contents, "");
    set(txt_sp, MUIA_Text_Contents, "Daten werden umgeschlüsselt ...");
}

static void login_from_window(void)
{
    char *s = (char *)xget(str_server, MUIA_String_Contents), *u = (char *)xget(str_user, MUIA_String_Contents);
    char *p = (char *)xget(str_pass, MUIA_String_Contents);
    if (!s || !*s || !u || !*u) { set(txt_login, MUIA_Text_Contents, "Server und Benutzer angeben."); return; }
    set(txt_login, MUIA_Text_Contents, "Anmelden ...");
    do_login(s, u, p);
}

static void show_login(const char *msg)
{
    set(str_server, MUIA_String_Contents, cfg_server ? cfg_server : "https://");
    set(str_user, MUIA_String_Contents, cfg_user ? cfg_user : "");
    set(str_pass, MUIA_String_Contents, "");
    set(txt_login, MUIA_Text_Contents, msg ? msg : "");
    set(win_login, MUIA_Window_Open, TRUE);
    set(win_login, MUIA_Window_ActiveObject, cfg_user ? str_pass : str_server);
}

/* "@name:server", completing a bare name with our server */
static char *full_user_id(const char *in)
{
    const char *srv = my_user ? strchr(my_user, ':') : NULL;
    char *o = malloc(strlen(in) + (srv ? strlen(srv) : 0) + 3);
    while (*in == ' ') in++;
    sprintf(o, "%s%s", *in == '@' ? "" : "@", in);
    if (!strchr(o + 1, ':') && srv) strcat(o, srv);
    o[strcspn(o, " ,;")] = 0;
    return o;
}

static void new_chat(const char *who)
{
    char *uid = full_user_id(who);
    const char *inv[2];
    AmxRoomOptions opt;
    int i;
    for (i = 0; i < n_rooms; i++)
        if (!rooms[i].invited && rooms[i].dm_user && !strcmp(rooms[i].dm_user, uid)) {
            open_chat(rooms[i].id);
            free(uid);
            return;
        }
    memset(&opt, 0, sizeof opt);
    inv[0] = uid; inv[1] = NULL;
    opt.invite = inv;
    opt.is_direct = 1;
    opt.encrypted = 1;
    amx_async_create_room(A, &opt, TAG_NEWCHAT);
    set_status("Chat mit %s wird angelegt ...", uid, NULL);
    free(uid);
}

static void new_room(const char *name, int enc, int pub_)
{
    char *u = amx_from_local(name);
    AmxRoomOptions opt;
    memset(&opt, 0, sizeof opt);
    opt.name = u;
    opt.encrypted = enc;
    opt.is_public = pub_;
    amx_async_create_room(A, &opt, TAG_NEWROOM);
    set_status("Raum wird angelegt ...", NULL, NULL);
    free(u);
}

static void recovery_go(const char *typed)
{
    char *t = (char *)(typed ? typed : (const char *)xget(str_recovery, MUIA_String_Contents)), *u;
    if (!t || !*t || !A || !my_user) return;
    u = amx_from_local(t);
    amx_async_recovery(A, u, TAG_RECOVERY);
    memset(u, 0, strlen(u));
    free(u);
    set(str_recovery, MUIA_String_Contents, "");
    set(txt_recovery, MUIA_Text_Contents, "Wird geprüft ... (eine Passphrase kann einige Minuten dauern)");
    set(b_recovery, MUIA_Disabled, TRUE);
    set_status("Sicherheitsschlüssel wird geprüft ...", NULL, NULL);
}

static void invite_go(void)
{
    char *t = (char *)xget(str_inviteuser, MUIA_String_Contents), *uid;
    if (!t || !*t || !invite_room || !A) return;
    uid = full_user_id(t);
    amx_async_invite(A, invite_room, uid, TAG_INVITE);
    fifo_push(fifo_invite, &n_invite, 16, invite_room);
    {
        ChatWin *w = chat_by_room(invite_room);
        if (w) { w->inviting++; w->err[0] = 0; update_status(w); }
    }
    set_status("%s wird eingeladen ...", uid, NULL);
    free(uid);
    set(win_invite, MUIA_Window_Open, FALSE);
}

static void accept_invite(Room *r, int yes)
{
    if (yes) {
        amx_async_join(A, r->id, TAG_ACCEPT);
        free(pending_open);
        pending_open = strdup(r->id);
    } else amx_async_leave(A, r->id, TAG_LEAVE);
}

static void room_double_click(void)
{
    Room *r = NULL;
    DoMethod(lst_rooms, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &r);
    if (!r) return;
    if (r->invited) {
        LONG k = MUI_Request(app, win_main, 0, (STRPTR)"Einladung",
                             (STRPTR)"*_Annehmen|A_blehnen|_Später",
                             (STRPTR)"%s lädt dich ein:\n\n\33b%s", (ULONG)(r->inviter ? r->inviter : "Jemand"),
                             (ULONG)r->name);
        if (k == 1) accept_invite(r, 1);
        else if (k == 2) accept_invite(r, 0);
        return;
    }
    open_chat(r->id);
}

/* public room directory */
HOOKPROTONHNO(pub_disp_func, LONG, struct NList_DisplayMessage *m)
{
    static char b0[200], b1[16], b2[200];
    const AmxPublicRoom *p = m->entry;
    char *t;
    if (!p) { m->strings[0] = "Name"; m->strings[1] = "Mitglieder"; m->strings[2] = "Adresse"; return 0; }
    t = local(p->name ? p->name : p->room_id); snprintf(b0, sizeof b0, "%s", t); free(t);
    snprintf(b1, sizeof b1, "%d", p->members);
    t = local(p->alias ? p->alias : p->room_id); snprintf(b2, sizeof b2, "%s", t); free(t);
    m->strings[0] = b0; m->strings[1] = b1; m->strings[2] = b2;
    return 0;
}
MakeStaticHook(pub_disp_hook, pub_disp_func);

static void join_selected(void)
{
    AmxPublicRoom *p = NULL;
    char *typed = (char *)xget(str_search, MUIA_String_Contents);
    DoMethod(lst_pub, MUIM_NList_GetEntry, MUIV_NList_GetEntry_Active, &p);
    if (p) amx_async_join(A, p->room_id, TAG_JOIN);
    else if (typed && (*typed == '#' || *typed == '!')) amx_async_join(A, typed, TAG_JOIN);
    else return;
    set_status("Raum wird betreten ...", NULL, NULL);
}

/* ------------------------------------------------------------ net messages */

static void handle_result(const AmxMsg *m)
{
    amx_log("result tag %lu cmd %d status %d %s", m->tag, (int)m->cmd, m->status, m->error ? m->error : "");
    switch (m->tag) {
    case TAG_LOGIN:
        if (m->status) {
            set_status("Anmeldung fehlgeschlagen", NULL, NULL);
            show_login(m->error);
            return;
        }
        free(my_user);
        my_user = strdup(m->result);
        verified = m->verified;
        save_prefs();
        set(win_login, MUIA_Window_Open, FALSE);
        set_status("Verbinde als %s ...", my_user, NULL);
        break;
    case TAG_LOGOUT:
        store_protected = 0;
        free(my_user); my_user = NULL;
        verified = 0;
        while (chats) close_chat(chats);
        show_login("Abgemeldet.");
        set_status("Abgemeldet", NULL, NULL);
        break;
    case TAG_NEWCHAT: case TAG_NEWROOM: case TAG_JOIN:
        if (m->status) {
            MUI_Request(app, win_main, 0, (STRPTR)"Amatrix", (STRPTR)"*_OK", (STRPTR)"%s",
                        (ULONG)(m->error ? m->error : "Fehler"));
            set_online();
            return;
        }
        free(pending_open);
        pending_open = strdup(m->result);
        if (room_by_id(pending_open)) { char *id = pending_open; pending_open = NULL; open_chat(id); free(id); }
        set_online();
        break;
    case TAG_UNLOCK:
        set(b_unlock, MUIA_Disabled, FALSE);
        if (m->status) {
            set(txt_unlock, MUIA_Text_Contents, m->error && strstr(m->error, "wrong")
                ? "Falsches Passwort." : (m->error ? m->error : "Fehler"));
            break;
        }
        set(win_unlock, MUIA_Window_Open, FALSE);
        set_status("Anmelden als %s ...", cfg_user, NULL);
        amx_async_login(A, cfg_user, pending_password ? pending_password : "", TAG_LOGIN);
        if (pending_password) { memset(pending_password, 0, strlen(pending_password)); free(pending_password); }
        pending_password = NULL;
        break;
    case TAG_STOREPASS:
        if (m->status) { set(txt_sp, MUIA_Text_Contents, m->error ? m->error : "Fehler"); break; }
        store_protected = !store_protected_removing;
        set(txt_sp, MUIA_Text_Contents, store_protected
            ? "\33bDie Amatrix-Daten sind jetzt mit dem Passwort geschützt.\33n\nBeim Start fragt Amatrix danach."
            : "\33bDer Passwortschutz ist entfernt.\33n");
        break;
    case TAG_RECOVERY: {
        ChatWin *w;
        set(b_recovery, MUIA_Disabled, FALSE);
        if (m->status) {
            set(txt_recovery, MUIA_Text_Contents, m->error && strstr(m->error, "wrong")
                ? "Falscher Schlüssel bzw. falsche Passphrase." : (m->error ? m->error : "Fehler"));
            set_online();
            break;
        }
        if (m->verified) verified = 1;
        {   /* result in the window (a requester would block the program) */
            static char res[300];
            snprintf(res, sizeof res, "\33b%s\33n\n%s",
                     m->verified ? "Dieses Gerät ist jetzt verifiziert."
                                 : "Verifizieren war nicht möglich (Cross-Signing nicht eingerichtet?).",
                     m->backup ? "Alte Nachrichten werden beim Öffnen eines Raums entschlüsselt."
                               : "Auf dem Server gibt es keine Schlüsselsicherung.");
            set(txt_recovery, MUIA_Text_Contents, res);
        }
        set_online();
        /* open windows: reload, which fetches their keys from the backup */
        for (w = chats; w; w = w->next) amx_async_load_history(A, w->room_id, FIRST_PAGE, 1, TAG_HIST);
        break;
    }
    case TAG_INVITE: {
        char *room = fifo_pop(fifo_invite, &n_invite);
        ChatWin *w = room ? chat_by_room(room) : NULL;
        if (w) {
            if (w->inviting) w->inviting--;
            if (m->status) snprintf(w->err, sizeof w->err, "Einladen fehlgeschlagen: %.120s", m->error ? m->error : "");
            update_status(w);
        } else if (m->status)
            MUI_Request(app, NULL, 0, (STRPTR)"Amatrix", (STRPTR)"*_OK",
                        (STRPTR)"Einladen hat nicht geklappt:\n%s", (ULONG)(m->error ? m->error : ""));
        free(room);
        set_online();
        break;
    }
    case TAG_SEND: {
        char *room = fifo_pop(fifo_send, &n_send);
        ChatWin *w = room ? chat_by_room(room) : NULL;
        if (w) {
            if (w->sending) w->sending--;
            if (m->status) snprintf(w->err, sizeof w->err, "Senden fehlgeschlagen: %.120s", m->error ? m->error : "");
            update_status(w);
        } else if (m->status)
            MUI_Request(app, NULL, 0, (STRPTR)"Amatrix", (STRPTR)"*_OK",
                        (STRPTR)"Nachricht nicht gesendet:\n%s", (ULONG)(m->error ? m->error : ""));
        free(room);
        break;
    }
    case TAG_LEAVE:
        if (m->status)
            MUI_Request(app, win_main, 0, (STRPTR)"Amatrix", (STRPTR)"*_OK",
                        (STRPTR)"Verlassen hat nicht geklappt:\n%s", (ULONG)(m->error ? m->error : ""));
        set_online();
        break;
    case TAG_REACT:
        if (m->status) amx_log("reaction failed: %s", m->error ? m->error : "-");
        break;
    case TAG_HIST:
        history_done(m);
        break;
    case TAG_AVATAR:
        got_avatar(m);
        break;
    case TAG_SEARCH:
        set(lst_pub, MUIA_NList_Quiet, TRUE);
        DoMethod(lst_pub, MUIM_NList_Clear);
        amx_free_public_rooms(pub, n_pub);
        pub = NULL; n_pub = 0;
        if (!m->status && m->n) {
            int i;
            /* the message frees its list; keep a copy of the pointers' data */
            pub = calloc(m->n, sizeof *pub);
            for (i = 0; pub && i < m->n; i++) {
                pub[i].room_id = xstrdup(m->pub[i].room_id);
                pub[i].name = xstrdup(m->pub[i].name);
                pub[i].topic = xstrdup(m->pub[i].topic);
                pub[i].alias = xstrdup(m->pub[i].alias);
                pub[i].members = m->pub[i].members;
                DoMethod(lst_pub, MUIM_NList_InsertSingle, &pub[i], MUIV_NList_Insert_Bottom);
            }
            if (pub) n_pub = m->n;
        }
        set(lst_pub, MUIA_NList_Quiet, FALSE);
        break;
    }
}

static void handle_net(void)
{
    AmxMsg *m;
    while (A && (m = amx_async_get(A)) != NULL) {
        switch (m->type) {
        case AMX_MSG_EVENT:  chat_event(&m->ev); break;
        case AMX_MSG_ROOMS:  update_rooms(m); break;
        case AMX_MSG_RESULT: handle_result(m); break;
        case AMX_MSG_STATE:
            if (m->online) set_online();
            else set_status("Offline: %s", m->error, NULL);
            break;
        }
        amx_async_msg_free(m);
    }
}

/* ----------------------------------------------------------------- ARexx */

static char rexx_buf[512];

static void rexx_result(const char *s)
{
    snprintf(rexx_buf, sizeof rexx_buf, "%s", s);
    set(app, MUIA_Application_RexxString, rexx_buf);
}

HOOKPROTONH(rx_openchat_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    (void)obj;
    if (!r) return 5;
    if (r->invited) return 10;
    open_chat(r->id);
    return 0;
}
MakeStaticHook(rx_openchat_hook, rx_openchat_func);

HOOKPROTONH(rx_say_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w;
    (void)obj;
    if (!r) return 5;
    open_chat(r->id);
    if (!(w = chat_by_room(r->id))) return 10;
    set(w->input, MUIA_String_Contents, (char *)arg[1]);   /* as if typed */
    chat_send(w);
    return 0;
}
MakeStaticHook(rx_say_hook, rx_say_func);

HOOKPROTONH(rx_accept_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    (void)obj;
    if (!r || !r->invited) return 5;
    accept_invite(r, 1);
    return 0;
}
MakeStaticHook(rx_accept_hook, rx_accept_func);

HOOKPROTONH(rx_newchat_func, LONG, Object *obj, ULONG *arg)
{
    (void)obj;
    if (!A || !my_user) return 10;
    new_chat((char *)arg[0]);
    return 0;
}
MakeStaticHook(rx_newchat_hook, rx_newchat_func);

HOOKPROTONH(rx_newroom_func, LONG, Object *obj, ULONG *arg)
{
    (void)obj;
    if (!A || !my_user) return 10;
    new_room((char *)arg[0], 1, 0);
    return 0;
}
MakeStaticHook(rx_newroom_hook, rx_newroom_func);

HOOKPROTONH(rx_status_func, LONG, Object *obj, ULONG *arg)
{
    char b[512];
    int i, inv = 0;
    (void)obj; (void)arg;
    for (i = 0; i < n_rooms; i++) inv += rooms[i].invited;
    snprintf(b, sizeof b, "%s|rooms=%d|invites=%d", status_buf, n_rooms, inv);
    rexx_result(b);
    return 0;
}
MakeStaticHook(rx_status_hook, rx_status_func);

HOOKPROTONH(rx_lines_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    char b[16];
    (void)obj;
    snprintf(b, sizeof b, "%ld", w ? (long)xget(w->list, MUIA_NList_Entries) : -1L);
    rexx_result(b);
    return 0;
}
MakeStaticHook(rx_lines_hook, rx_lines_func);

HOOKPROTONH(rx_unread_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    char b[16];
    (void)obj;
    snprintf(b, sizeof b, "%d", w ? w->n_undec : -1);
    rexx_result(b);
    return 0;
}
MakeStaticHook(rx_unread_hook, rx_unread_func);

HOOKPROTONH(rx_older_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    (void)obj;
    if (!w) return 5;
    if (xget(w->older, MUIA_Disabled)) return 10;          /* button greyed out */
    DoMethod(app, MUIM_CallHook, &chat_older_hook, w);      /* exactly what a click does */
    return 0;
}
MakeStaticHook(rx_older_hook, rx_older_func);

HOOKPROTONH(rx_first_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    Line *l = NULL;
    (void)obj;
    if (w) DoMethod(w->list, MUIM_NList_GetEntry, 0, &l);
    rexx_result(l && l->text ? l->text : "");
    return 0;
}
MakeStaticHook(rx_first_hook, rx_first_func);

HOOKPROTONH(rx_unreadcount_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    char b[16];
    (void)obj;
    snprintf(b, sizeof b, "%d", r ? r->unread : -1);
    rexx_result(b);
    return 0;
}
MakeStaticHook(rx_unreadcount_hook, rx_unreadcount_func);

HOOKPROTONH(rx_roomline_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    char b[200];
    (void)obj;
    if (!r) return 5;
    second_line(r, b, sizeof b);
    rexx_result(b);
    return 0;
}
MakeStaticHook(rx_roomline_hook, rx_roomline_func);

/* as if the toolbar button was pressed and the ID typed */
HOOKPROTONH(rx_invite_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    (void)obj;
    if (!r || r->invited) return 5;
    if (!w) { open_chat(r->id); w = chat_by_room(r->id); }
    if (!w) return 10;
    DoMethod(app, MUIM_CallHook, &chat_invite_hook, w);
    set(str_inviteuser, MUIA_String_Contents, (char *)arg[1]);
    invite_go();
    return 0;
}
MakeStaticHook(rx_invite_hook, rx_invite_func);

HOOKPROTONH(rx_recovery_func, LONG, Object *obj, ULONG *arg)
{
    (void)obj;
    if (!A || !my_user) return 10;
    recovery_go((char *)arg[0]);
    return 0;
}
MakeStaticHook(rx_recovery_hook, rx_recovery_func);

HOOKPROTONH(rx_unlock_func, LONG, Object *obj, ULONG *arg)
{
    (void)obj;
    if (!A || !xget(win_unlock, MUIA_Window_Open)) return 5;
    unlock_go((char *)arg[0]);
    return 0;
}
MakeStaticHook(rx_unlock_hook, rx_unlock_func);

HOOKPROTONH(rx_storepass_func, LONG, Object *obj, ULONG *arg)
{
    (void)obj;
    if (!A || !my_user) return 10;
    storepass_go(!arg[0] || !*(char *)arg[0], (char *)arg[0]);
    return 0;
}
MakeStaticHook(rx_storepass_hook, rx_storepass_func);

HOOKPROTONH(rx_chatstatus_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    (void)obj;
    if (!w) return 5;
    rexx_result((char *)xget(w->busy, MUIA_Text_Contents));
    return 0;
}
MakeStaticHook(rx_chatstatus_hook, rx_chatstatus_func);

HOOKPROTONH(rx_closechat_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    (void)obj;
    if (!w) return 5;
    w->close_req = 1;              /* as the close gadget does */
    return 0;
}
MakeStaticHook(rx_closechat_hook, rx_closechat_func);

/* REACT room name: our reaction (thumbsup, heart, ...) to the newest message
 * of someone else; REACTIONS room: the reaction lines, "name count[*]" */
static const char *const emo_names[N_EMO] = { "thumbsup", "thumbsdown", "heart", "smile", "joy",
                                               "confused", "eyes", "tada", "rocket", "unknown" };

HOOKPROTONH(rx_react_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    LONG n, k;
    unsigned i;
    int img = -1;
    (void)obj;
    if (!w) return 5;
    for (i = 0; i < N_EMO; i++) if (!strcmp(emo_names[i], (char *)arg[1])) img = (int)i;
    if (img < 0) return 10;
    n = (LONG)xget(w->list, MUIA_NList_Entries);
    for (k = n - 1; k >= 0; k--) {
        Line *l = NULL;
        DoMethod(w->list, MUIM_NList_GetEntry, k, &l);
        if (!l || l->react || l->undec || !l->eid || l->own || !strstr(l->text, ":\33n")) continue;
        for (i = 0; i < sizeof emo_keys / sizeof emo_keys[0]; i++)
            if (emo_keys[i].img == img) break;
        react_toggle(w, l->eid, emo_keys[i].key);       /* again: takes it back */
        return 0;
    }
    return 10;
}
MakeStaticHook(rx_react_hook, rx_react_func);

HOOKPROTONH(rx_reactions_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    char b[1024] = "";
    LONG n, k;
    (void)obj;
    if (!w) return 5;
    n = (LONG)xget(w->list, MUIA_NList_Entries);
    for (k = 0; k < n; k++) {
        Line *l = NULL;
        int i, j;
        DoMethod(w->list, MUIM_NList_GetEntry, k, &l);
        if (!l || !l->react) continue;
        if (b[0]) strncat(b, " | ", sizeof b - strlen(b) - 1);
        for (i = 0; i < w->n_react; i++) {
            int count = 0, own = 0, img;
            char one[48];
            if (strcmp(w->react[i].target, l->eid)) continue;
            for (j = 0; j < i; j++)
                if (!strcmp(w->react[j].target, l->eid) && same_key(w->react[j].key, w->react[i].key)) break;
            if (j < i) continue;
            count = group_count(w, l->eid, i, &own);
            img = emo_index(w->react[i].key);
            snprintf(one, sizeof one, "%s%s %d%s", strlen(b) && b[strlen(b) - 1] != ' ' ? " " : "",
                     img >= 0 ? emo_names[img] : "other", count, own ? "*" : "");
            strncat(b, one, sizeof b - strlen(b) - 1);
        }
    }
    rexx_result(b);
    return 0;
}
MakeStaticHook(rx_reactions_hook, rx_reactions_func);

HOOKPROTONH(rx_leave_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    (void)obj;
    if (!r) return 5;
    leave_room(r->id, 0);
    return 0;
}
MakeStaticHook(rx_leave_hook, rx_leave_func);

/* LASTLINE room: the text of the newest chat line, ESC shown as "^" */
HOOKPROTONH(rx_last_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    Line *l = NULL;
    char b[500], *q;
    LONG n;
    (void)obj;
    if (!w) return 5;
    n = (LONG)xget(w->list, MUIA_NList_Entries);
    if (n > 0) DoMethod(w->list, MUIM_NList_GetEntry, n - 1, &l);
    snprintf(b, sizeof b, "%s", l && l->text ? l->text : "");
    for (q = b; *q; q++) if (*q == 27) *q = '^';
    rexx_result(b);
    return 0;
}
MakeStaticHook(rx_last_hook, rx_last_func);

HOOKPROTONH(rx_picker_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    (void)obj;
    if (!w) return 5;
    open_picker(w);
    return 0;
}
MakeStaticHook(rx_picker_hook, rx_picker_func);

static struct MUI_Command rexx_cmds[] = {
    { "OPENCHAT",  "ROOM/F",          1, &rx_openchat_hook, { 0 } },
    { "SAY",       "ROOM/A,TEXT/A/F", 2, &rx_say_hook,      { 0 } },
    { "ACCEPT",    "ROOM/F",          1, &rx_accept_hook,   { 0 } },
    { "NEWCHAT",   "USER/A",          1, &rx_newchat_hook,  { 0 } },
    { "NEWROOM",   "NAME/A",          1, &rx_newroom_hook,  { 0 } },
    { "INVITE",    "ROOM/A,USER/A",   2, &rx_invite_hook,   { 0 } },
    { "STATUS",    NULL,              0, &rx_status_hook,   { 0 } },
    { "CHATLINES", "ROOM/F",          1, &rx_lines_hook,    { 0 } },
    { "UNREADABLE","ROOM/F",          1, &rx_unread_hook,   { 0 } },
    { "OLDER",     "ROOM/F",          1, &rx_older_hook,    { 0 } },
    { "UNREAD",    "ROOM/F",          1, &rx_unreadcount_hook, { 0 } },
    { "ROOMLINE",  "ROOM/F",          1, &rx_roomline_hook, { 0 } },
    { "REACT",     "ROOM/A,NAME/A",   2, &rx_react_hook,    { 0 } },
    { "REACTIONS", "ROOM/F",          1, &rx_reactions_hook, { 0 } },
    { "LEAVE",     "ROOM/F",          1, &rx_leave_hook,    { 0 } },
    { "LASTLINE",  "ROOM/F",          1, &rx_last_hook,     { 0 } },
    { "PICKER",    "ROOM/F",          1, &rx_picker_hook,   { 0 } },
    { "RECOVERY",  "KEY/F",           1, &rx_recovery_hook, { 0 } },
    { "UNLOCK",    "PASS/F",          1, &rx_unlock_hook,   { 0 } },
    { "CHATSTATUS","ROOM/F",          1, &rx_chatstatus_hook, { 0 } },
    { "CLOSECHAT", "ROOM/F",          1, &rx_closechat_hook, { 0 } },
    { "STOREPASS", "PASS/F",          1, &rx_storepass_hook, { 0 } },
    { "FIRSTLINE", "ROOM/F",          1, &rx_first_hook,    { 0 } },
    { NULL,        NULL,              0, NULL,              { 0 } }
};

/* ------------------------------------------------------------------- GUI */

static struct NewMenu menu[] = {
    { NM_TITLE, (STRPTR)"Amatrix",           NULL, 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Raum betreten ...", (STRPTR)"R", 0, 0, (APTR)ID_JOINWIN },
    { NM_ITEM,  (STRPTR)"Neuer Chat ...",    (STRPTR)"N", 0, 0, (APTR)ID_NEWCHAT },
    { NM_ITEM,  (STRPTR)"Neuer Raum ...",    NULL, 0, 0, (APTR)ID_NEWROOM },
    { NM_ITEM,  (STRPTR)"Raum verlassen ...", (STRPTR)"L", 0, 0, (APTR)ID_LEAVE },
    { NM_ITEM,  NM_BARLABEL,                 NULL, 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Sicherheitsschlüssel eingeben ...", (STRPTR)"S", 0, 0, (APTR)ID_RECOVERY },
    { NM_ITEM,  (STRPTR)"Passwort für Amatrix-Daten ...", (STRPTR)"P", 0, 0, (APTR)ID_STOREPASS },
    { NM_ITEM,  (STRPTR)"Abmelden",          NULL, 0, 0, (APTR)ID_LOGOUT },
    { NM_ITEM,  (STRPTR)"Über Amatrix ...",  (STRPTR)"?", 0, 0, (APTR)ID_ABOUT },
    { NM_ITEM,  (STRPTR)"Beenden",           (STRPTR)"Q", 0, 0, (APTR)MUIV_Application_ReturnID_Quit },
    { NM_END,   NULL, NULL, 0, 0, NULL }
};

static Object *build_app(void)
{
    Object *b_chat, *b_room, *b_login, *b_go_chat, *b_go_room, *b_search, *b_join, *b_invite;
    Object *a = ApplicationObject,
        MUIA_Application_Title, "Amatrix",
        MUIA_Application_Version, version,
        MUIA_Application_Copyright, "Oliver Urbann",
        MUIA_Application_Author, "Oliver Urbann",
        MUIA_Application_Description, "Matrix-Client",
        MUIA_Application_Base, "AMATRIX",
        MUIA_Application_Commands, rexx_cmds,
        MUIA_Application_Menustrip, MUI_MakeObject(MUIO_MenustripNM, (ULONG)menu, 0),

        SubWindow, win_main = WindowObject,
            MUIA_Window_Title, "Amatrix",
            MUIA_Window_ID, MAKE_ID('A','M','X','M'),
            MUIA_Window_LeftEdge, 8,
            MUIA_Window_TopEdge, MUIV_Window_TopEdge_Centered,
            MUIA_Window_Width, MUIV_Window_Width_Visible(38),
            MUIA_Window_Height, MUIV_Window_Height_Visible(65),
            WindowContents, VGroup,
                Child, lv_rooms = ListviewObject,
                    MUIA_Listview_List, lst_rooms = ListObject,
                        InputListFrame,
                        MUIA_List_DisplayHook, &room_disp_hook,
                        MUIA_List_AutoLineHeight, TRUE,
                        MUIA_List_Format, "P=\33c,MIW=0 W=100,P=\33r",   /* clip, no ellipsis: it eats line 2 */
                        MUIA_List_HScrollerVisibility, MUIV_List_HScrollerVisibility_Never,
                        MUIA_List_AutoVisible, TRUE,
                    End,
                    MUIA_CycleChain, TRUE,
                End,
                Child, HGroup,
                    Child, b_chat = SimpleButton("Neuer _Chat"),
                    Child, b_room = SimpleButton("Neuer _Raum"),
                End,
                Child, txt_status = TextObject,
                    MUIA_Text_Contents, "",
                    MUIA_Font, MUIV_Font_Tiny,
                End,
            End,
        End,

        SubWindow, win_login = WindowObject,
            MUIA_Window_Title, "Amatrix - Anmelden",
            MUIA_Window_ID, MAKE_ID('A','M','X','L'),
            WindowContents, VGroup,
                Child, ColGroup(2),
                    Child, Label2("Server:"),
                    Child, str_server = StringObject, StringFrame, MUIA_CycleChain, TRUE, End,
                    Child, Label2("Benutzer:"),
                    Child, str_user = StringObject, StringFrame, MUIA_CycleChain, TRUE, End,
                    Child, Label2("Passwort:"),
                    Child, str_pass = StringObject, StringFrame, MUIA_String_Secret, TRUE,
                                      MUIA_CycleChain, TRUE, End,
                End,
                Child, txt_login = TextObject, MUIA_Text_Contents, "", End,
                Child, b_login = SimpleButton("_Anmelden"),
            End,
        End,

        SubWindow, win_newchat = WindowObject,
            MUIA_Window_Title, "Neuer Chat",
            MUIA_Window_ID, MAKE_ID('A','M','X','C'),
            WindowContents, VGroup,
                Child, TextObject, MUIA_Text_Contents, "Mit wem? (z.B. @name:server)", End,
                Child, str_chatuser = StringObject, StringFrame, MUIA_CycleChain, TRUE, End,
                Child, b_go_chat = SimpleButton("Chat _starten"),
            End,
        End,

        SubWindow, win_newroom = WindowObject,
            MUIA_Window_Title, "Neuer Raum",
            MUIA_Window_ID, MAKE_ID('A','M','X','R'),
            WindowContents, VGroup,
                Child, ColGroup(2),
                    Child, Label2("Name:"),
                    Child, str_roomname = StringObject, StringFrame, MUIA_CycleChain, TRUE, End,
                    Child, Label1("Verschlüsselt:"),
                    Child, HGroup, Child, chk_enc = CheckMark(TRUE), Child, HSpace(0), End,
                    Child, Label1("Im Verzeichnis:"),
                    Child, HGroup, Child, chk_pub = CheckMark(FALSE), Child, HSpace(0), End,
                End,
                Child, b_go_room = SimpleButton("Raum _anlegen"),
            End,
        End,

        SubWindow, win_join = WindowObject,
            MUIA_Window_Title, "Raum betreten",
            MUIA_Window_ID, MAKE_ID('A','M','X','J'),
            WindowContents, VGroup,
                Child, HGroup,
                    Child, str_search = StringObject, StringFrame, MUIA_CycleChain, TRUE, End,
                    Child, b_search = SimpleButton("_Suchen"),
                End,
                Child, NListviewObject,
                    MUIA_NListview_NList, lst_pub = NListObject,
                        MUIA_NList_DisplayHook2, &pub_disp_hook,
                        MUIA_NList_Format, "BAR,P=\33r BAR,",
                        MUIA_NList_Title, TRUE,
                    End,
                End,
                Child, b_join = SimpleButton("_Betreten"),
            End,
        End,
        SubWindow, win_unlock = WindowObject,
            MUIA_Window_Title, "Amatrix - Passwort",
            MUIA_Window_ID, MAKE_ID('A','M','X','U'),
            WindowContents, VGroup,
                Child, TextObject, MUIA_Text_Contents,
                    "Die Amatrix-Daten (Anmeldung, Schlüssel) sind\nmit einem Passwort geschützt.", End,
                Child, str_unlock = StringObject, StringFrame, MUIA_String_Secret, TRUE,
                                    MUIA_CycleChain, TRUE, End,
                Child, txt_unlock = TextObject, MUIA_Text_Contents, "", End,
                Child, b_unlock = SimpleButton("_Entsperren"),
            End,
        End,

        SubWindow, win_storepass = WindowObject,
            MUIA_Window_Title, "Passwort für Amatrix-Daten",
            MUIA_Window_ID, MAKE_ID('A','M','X','P'),
            WindowContents, VGroup,
                Child, TextObject, MUIA_Text_Contents,
                    "Schützt Anmeldung, Schlüssel und Sitzungen in der\n"
                    "Schublade neben dem Programm.  Ohne das Passwort sind\n"
                    "sie nicht zu gebrauchen - es lässt sich nicht wiederherstellen.", End,
                Child, ColGroup(2),
                    Child, Label2("Passwort:"),
                    Child, str_sp1 = StringObject, StringFrame, MUIA_String_Secret, TRUE, MUIA_CycleChain, TRUE, End,
                    Child, Label2("Noch einmal:"),
                    Child, str_sp2 = StringObject, StringFrame, MUIA_String_Secret, TRUE, MUIA_CycleChain, TRUE, End,
                End,
                Child, txt_sp = TextObject, MUIA_Text_Contents, "", End,
                Child, HGroup,
                    Child, b_sp_set = SimpleButton("_Festlegen"),
                    Child, b_sp_del = SimpleButton("Schutz _entfernen"),
                End,
            End,
        End,

        SubWindow, win_recovery = WindowObject,
            MUIA_Window_Title, "Sicherheitsschlüssel",
            MUIA_Window_ID, MAKE_ID('A','M','X','K'),
            WindowContents, VGroup,
                Child, TextObject, MUIA_Text_Contents,
                    "Der Sicherheitsschlüssel aus Element (z.B. \33bEsTc 9xYz ...\33n)\n"
                    "oder die Sicherheits-Passphrase.  Damit wird dieses Gerät\n"
                    "verifiziert, und alte Nachrichten werden lesbar.", End,
                Child, str_recovery = StringObject, StringFrame, MUIA_String_MaxLen, 300,
                                      MUIA_CycleChain, TRUE, End,
                Child, txt_recovery = TextObject, MUIA_Text_Contents, "", End,
                Child, b_recovery = SimpleButton("_Entsperren"),
            End,
        End,

        SubWindow, win_invite = WindowObject,
            MUIA_Window_Title, "Einladen",
            MUIA_Window_ID, MAKE_ID('A','M','X','I'),
            WindowContents, VGroup,
                Child, txt_inviteroom = TextObject, MUIA_Text_Contents, "", End,
                Child, TextObject, MUIA_Text_Contents, "Wen? (z.B. @name:server)", End,
                Child, str_inviteuser = StringObject, StringFrame, MUIA_CycleChain, TRUE, End,
                Child, b_invite = SimpleButton("_Einladen"),
            End,
        End,
    End;
    if (!a) return NULL;
    if (make_picker()) DoMethod(a, OM_ADDMEMBER, win_pick);
    DoMethod(b_invite, MUIM_Notify, MUIA_Pressed, FALSE, a, 2, MUIM_Application_ReturnID, ID_INVITE_GO);
    DoMethod(b_recovery, MUIM_Notify, MUIA_Pressed, FALSE, a, 2, MUIM_Application_ReturnID, ID_RECOVERY_GO);
    DoMethod(b_unlock, MUIM_Notify, MUIA_Pressed, FALSE, a, 2, MUIM_Application_ReturnID, ID_UNLOCK_GO);
    DoMethod(str_unlock, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime, a, 2,
             MUIM_Application_ReturnID, ID_UNLOCK_GO);
    DoMethod(win_unlock, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, a, 2,
             MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);
    DoMethod(b_sp_set, MUIM_Notify, MUIA_Pressed, FALSE, a, 2, MUIM_Application_ReturnID, ID_STOREPASS_SET);
    DoMethod(str_sp2, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime, a, 2,
             MUIM_Application_ReturnID, ID_STOREPASS_SET);
    DoMethod(b_sp_del, MUIM_Notify, MUIA_Pressed, FALSE, a, 2, MUIM_Application_ReturnID, ID_STOREPASS_DEL);
    DoMethod(win_storepass, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, win_storepass, 3,
             MUIM_Set, MUIA_Window_Open, FALSE);
    DoMethod(str_recovery, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime, a, 2,
             MUIM_Application_ReturnID, ID_RECOVERY_GO);
    DoMethod(win_recovery, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, win_recovery, 3,
             MUIM_Set, MUIA_Window_Open, FALSE);
    DoMethod(str_inviteuser, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime, a, 2,
             MUIM_Application_ReturnID, ID_INVITE_GO);
    DoMethod(win_invite, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, win_invite, 3,
             MUIM_Set, MUIA_Window_Open, FALSE);

    DoMethod(win_main, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, a, 2,
             MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);
    DoMethod(lv_rooms, MUIM_Notify, MUIA_Listview_DoubleClick, TRUE, a, 2,
             MUIM_Application_ReturnID, ID_ROOM_DCLICK);
    DoMethod(b_chat, MUIM_Notify, MUIA_Pressed, FALSE, a, 2, MUIM_Application_ReturnID, ID_NEWCHAT);
    DoMethod(b_room, MUIM_Notify, MUIA_Pressed, FALSE, a, 2, MUIM_Application_ReturnID, ID_NEWROOM);

    DoMethod(b_login, MUIM_Notify, MUIA_Pressed, FALSE, a, 2, MUIM_Application_ReturnID, ID_LOGIN);
    DoMethod(str_pass, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime, a, 2,
             MUIM_Application_ReturnID, ID_LOGIN);
    DoMethod(win_login, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, win_login, 3,
             MUIM_Set, MUIA_Window_Open, FALSE);

    DoMethod(b_go_chat, MUIM_Notify, MUIA_Pressed, FALSE, a, 2, MUIM_Application_ReturnID, ID_NEWCHAT_GO);
    DoMethod(str_chatuser, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime, a, 2,
             MUIM_Application_ReturnID, ID_NEWCHAT_GO);
    DoMethod(win_newchat, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, win_newchat, 3,
             MUIM_Set, MUIA_Window_Open, FALSE);

    DoMethod(b_go_room, MUIM_Notify, MUIA_Pressed, FALSE, a, 2, MUIM_Application_ReturnID, ID_NEWROOM_GO);
    DoMethod(win_newroom, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, win_newroom, 3,
             MUIM_Set, MUIA_Window_Open, FALSE);

    DoMethod(b_search, MUIM_Notify, MUIA_Pressed, FALSE, a, 2, MUIM_Application_ReturnID, ID_JOIN_SEARCH);
    DoMethod(str_search, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime, a, 2,
             MUIM_Application_ReturnID, ID_JOIN_SEARCH);
    DoMethod(b_join, MUIM_Notify, MUIA_Pressed, FALSE, a, 2, MUIM_Application_ReturnID, ID_JOIN_GO);
    DoMethod(lst_pub, MUIM_Notify, MUIA_NList_DoubleClick, MUIV_EveryTime, a, 2,
             MUIM_Application_ReturnID, ID_JOIN_GO);
    DoMethod(win_join, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, win_join, 3,
             MUIM_Set, MUIA_Window_Open, FALSE);
    return a;
}

static void handle_id(ULONG id)
{
    switch (id) {
    case ID_LOGIN:
        login_from_window();
        break;
    case ID_ROOM_DCLICK:
        room_double_click();
        break;
    case ID_NEWCHAT:
        if (!my_user) break;
        set(str_chatuser, MUIA_String_Contents, "");
        set(win_newchat, MUIA_Window_Open, TRUE);
        set(win_newchat, MUIA_Window_ActiveObject, str_chatuser);
        break;
    case ID_NEWCHAT_GO: {
        char *u = (char *)xget(str_chatuser, MUIA_String_Contents);
        if (u && *u) { new_chat(u); set(win_newchat, MUIA_Window_Open, FALSE); }
        break;
    }
    case ID_NEWROOM:
        if (!my_user) break;
        set(str_roomname, MUIA_String_Contents, "");
        set(win_newroom, MUIA_Window_Open, TRUE);
        set(win_newroom, MUIA_Window_ActiveObject, str_roomname);
        break;
    case ID_NEWROOM_GO: {
        char *n = (char *)xget(str_roomname, MUIA_String_Contents);
        if (n && *n) {
            new_room(n, xget(chk_enc, MUIA_Selected), xget(chk_pub, MUIA_Selected));
            set(win_newroom, MUIA_Window_Open, FALSE);
        }
        break;
    }
    case ID_JOINWIN:
        if (!my_user) break;
        set(win_join, MUIA_Window_Open, TRUE);
        set(win_join, MUIA_Window_ActiveObject, str_search);
        amx_async_public_rooms(A, NULL, NULL, 50, TAG_SEARCH);
        break;
    case ID_JOIN_SEARCH: {
        char *s = (char *)xget(str_search, MUIA_String_Contents), *u;
        if (s && (*s == '#' || *s == '!')) { join_selected(); break; }
        u = amx_from_local(s ? s : "");
        amx_async_public_rooms(A, NULL, u, 50, TAG_SEARCH);
        free(u);
        break;
    }
    case ID_INVITE_GO:
        invite_go();
        break;
    case ID_RECOVERY:
        if (!my_user) break;
        set(txt_recovery, MUIA_Text_Contents, "");
        set(b_recovery, MUIA_Disabled, FALSE);
        set(win_recovery, MUIA_Window_Open, TRUE);
        set(win_recovery, MUIA_Window_ActiveObject, str_recovery);
        break;
    case ID_RECOVERY_GO:
        recovery_go(NULL);
        break;
    case ID_UNLOCK_GO:
        unlock_go(NULL);
        break;
    case ID_STOREPASS:
        if (!my_user) break;
        set(txt_sp, MUIA_Text_Contents, store_protected ? "Zurzeit mit Passwort geschützt." : "Zurzeit ohne Passwort.");
        set(b_sp_del, MUIA_Disabled, !store_protected);
        set(win_storepass, MUIA_Window_Open, TRUE);
        set(win_storepass, MUIA_Window_ActiveObject, str_sp1);
        break;
    case ID_STOREPASS_SET:
        storepass_go(0, NULL);
        break;
    case ID_STOREPASS_DEL:
        storepass_go(1, NULL);
        set(b_sp_del, MUIA_Disabled, TRUE);
        break;
    case ID_JOIN_GO:
        join_selected();
        set(win_join, MUIA_Window_Open, FALSE);
        break;
    case ID_LEAVE: {
        Room *r = NULL;
        DoMethod(lst_rooms, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &r);
        if (r) leave_room(r->id, 1);
        else MUI_Request(app, win_main, 0, (STRPTR)"Amatrix", (STRPTR)"*_OK",
                         (STRPTR)"Bitte erst einen Raum in der Liste auswählen.", 0);
        break;
    }
    case ID_LOGOUT:
        if (A && my_user && MUI_Request(app, win_main, 0, (STRPTR)"Amatrix", (STRPTR)"*_Abmelden|_Nein",
                                        (STRPTR)"Abmelden? Dieses Gerät und seine Schlüssel\nwerden gelöscht.", 0) == 1)
            amx_async_logout(A, TAG_LOGOUT);
        break;
    case ID_ABOUT:
        MUI_Request(app, win_main, 0, (STRPTR)"Über Amatrix", (STRPTR)"*_OK",
                    (STRPTR)"\33cAmatrix 0.1\n\nMatrix mit Ende-zu-Ende-Verschlüsselung\nfür den Amiga.\n\n%s",
                    (ULONG)(my_user ? my_user : "nicht angemeldet"));
        break;
    }
}

int main(int argc, char **argv)
{
    struct RDArgs *rda = NULL;
    LONG args[4] = { 0, 0, 0, 0 };
    ULONG sigs = 0, id;
    int i;

    make_icon(&icon_up, art_up);
    make_icon(&icon_invite, art_invite);
    make_icon(&icon_leave, art_leave);
    make_icon(&icon_react, art_react);
    make_emoji();
    if (!(MUIMasterBase = OpenLibrary((STRPTR)"muimaster.library", 20))) {
        printf("Amatrix braucht MUI 5 (muimaster.library V20).\n");
        return 20;
    }
    if (!NameFromLock(GetProgramDir(), (STRPTR)prog_dir, sizeof prog_dir - 2)) strcpy(prog_dir, "PROGDIR:");
    if (prog_dir[strlen(prog_dir) - 1] != ':') strcat(prog_dir, "/");
    load_prefs();
    if (argc > 0) rda = ReadArgs((STRPTR)"SERVER/K,USER/K,PASSWORD/K,DEBUG/S", args, NULL);
    if (args[3]) {
        char lp[300];
        snprintf(lp, sizeof lp, "%sAmatrix.log", prog_dir);
        amx_set_logfile(lp);
        debug = 1;
        amx_log("Amatrix started, %s", version + 6);
    }
    if (args[0]) { free(cfg_server); cfg_server = strdup((char *)args[0]); }
    if (args[1]) { free(cfg_user); cfg_user = strdup((char *)args[1]); }

    make_placeholders();
    /* right after another Amatrix quit, MUI may still be closing its classes:
     * try again for a moment before giving up */
    for (i = 0; i < 3 && !(app = build_app()); i++) Delay(100);
    if (!app) {
        printf("Amatrix: MUI-Oberfläche konnte nicht erstellt werden (NList, BetterString installiert?)\n");
        CloseLibrary(MUIMasterBase);
        return 20;
    }
    set(win_main, MUIA_Window_Open, TRUE);

    if (cfg_server && cfg_user) do_login(cfg_server, cfg_user, (char *)args[2]);
    else { set_status("Nicht angemeldet", NULL, NULL); show_login(NULL); }

    for (;;) {
        ChatWin *w, *nx;
        id = DoMethod(app, MUIM_Application_NewInput, &sigs);
        if (id == MUIV_Application_ReturnID_Quit) break;
        if (id) handle_id(id);
        for (w = chats; w; w = nx) { nx = w->next; if (w->close_req) close_chat(w); }
        handle_net();
        if (A && my_user) send_read_markers();
        if (sigs) {
            sigs = Wait(sigs | (A ? amx_async_sigmask(A) : 0) | SIGBREAKF_CTRL_C);
            if (sigs & SIGBREAKF_CTRL_C) break;
        }
    }

    if (A) amx_async_free(A);
    while (chats) close_chat(chats);
    set(win_main, MUIA_Window_Open, FALSE);
    MUI_DisposeObject(app);
    for (i = 0; i < n_imgs; i++) { free(imgs[i].img); free(imgs[i].url); }
    free(imgs);
    free_rooms(rooms, n_rooms);
    amx_free_public_rooms(pub, n_pub);
    free(my_user); free(cfg_server); free(cfg_user); free(pending_open); free(invite_room); free(pending_password);
    if (rda) FreeArgs(rda);
    CloseLibrary(MUIMasterBase);
    return 0;
}
