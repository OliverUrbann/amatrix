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
#include <proto/wb.h>
#include <proto/icon.h>
#include <workbench/workbench.h>
#include <workbench/startup.h>
#include <proto/asl.h>
#include <proto/rexxsyslib.h>
#include <proto/iffparse.h>
#include <libraries/iffparse.h>
#include <rexx/storage.h>
#include <rexx/rxslib.h>
#include <libraries/asl.h>
#include <dos/dostags.h>
#include <utility/tagitem.h>
#include "openurl_inline.h"
#include "richtext.h"
#include "cJSON.h"
#include "markdown.h"
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
#define REPLY_HELP "Auf die markierte Nachricht antworten"
#define EDIT_HELP "Eigene markierte Nachricht bearbeiten"
#define DELETE_HELP "Eigene markierte Nachricht löschen"
#define ATTACH_HELP "Bild oder Datei senden\n(auch: Icons ins Fenster ziehen)"
#define INFO_HELP "Mitglieder und Raum-Einstellungen"
#define REACT_HELP "Reaktion auf die markierte Nachricht\n(ohne Markierung: auf die neueste)"

enum {
    ID_LOGIN = 1, ID_NEWCHAT, ID_NEWCHAT_GO, ID_NEWROOM, ID_NEWROOM_GO, ID_ROOM_DCLICK,
    ID_JOINWIN, ID_JOIN_SEARCH, ID_JOIN_GO, ID_LOGOUT, ID_ABOUT, ID_INVITE_GO, ID_RECOVERY, ID_RECOVERY_GO, ID_UNLOCK_GO, ID_STOREPASS, ID_STOREPASS_SET, ID_STOREPASS_DEL, ID_LEAVE, ID_MUTE, ID_SETTINGS, ID_ICONIFY, ID_SET_OK, ID_COPY, ID_INFO, ID_INFO_SAVE, ID_INFO_PIC, ID_INFO_KICK, ID_INFO_BAN, ID_INFO_UNBAN,
    ID_INFO_ROLE, ID_INFO_INVITE, ID_INFO_ADMIT, ID_PROFILE, ID_PROF_SAVE, ID_PROF_PIC, ID_SESS, ID_SESS_RENAME, ID_SESS_DELETE, ID_FAV, ID_LOWPRIO, ID_FIND, ID_FIND_GO, ID_MUIPREFS, ID_POLLNEW, ID_POLLSEND, ID_PIN, ID_FWD, ID_FWD_GO, ID_QUOTE, ID_COPYLINK, ID_EDITHIST, ID_UNIGNORE, ID_KW_ADD, ID_KW_DEL, ID_EXPORT, ID_ACCOUNT, ID_PW_GO, ID_DEACT_GO
};
enum { TAG_LOGIN = 1, TAG_NEWCHAT, TAG_NEWROOM, TAG_JOIN, TAG_SEND, TAG_HIST, TAG_AVATAR,
       TAG_READ, TAG_SEARCH, TAG_ACCEPT, TAG_LEAVE, TAG_LOGOUT, TAG_INVITE, TAG_RECOVERY, TAG_UNLOCK, TAG_STOREPASS,
       TAG_REACT, TAG_THUMB, TAG_SAVE, TAG_THREAD, TAG_FILE, TAG_MEMBERS, TAG_RSETTINGS, TAG_ROOMSET, TAG_MODERATE, TAG_PROFILE, TAG_PROFAV, TAG_SETPROF, TAG_PREVIEW, TAG_DEVICES, TAG_DEVCHANGE, TAG_TAG, TAG_FIND, TAG_POLL, TAG_POLLVOTE, TAG_IGNORES, TAG_WHOIS, TAG_CMD, TAG_MEMB2, TAG_PIN, TAG_FETCH, TAG_FWD, TAG_EDITS, TAG_IGNLIST, TAG_KEYWORDS, TAG_PASSWORD, TAG_DEACT };

/* ------------------------------------------------------------------ state */

typedef struct {
    char *id, *name, *dm_user, *inviter, *avatar_url;   /* name/inviter: local charset */
    char *topic, *last_sender, *last_body;              /* local charset */
    int invited, unread, highlights, direct, encrypted, members, last_own;
    double last_ts;
    double read_ts;               /* own messages up to here are read by someone else */
    int favourite, low, is_space;
    char *parents;                /* spaces it belongs to (room IDs, one per line) */
    char *inviter_id;             /* who invited us (user ID) */
    int notify;                   /* AMX_NOTIFY_*: the server's setting for the room */
    char *pinned;                 /* pinned event IDs, one per line */
    char *replacement;            /* upgraded: the new room */
} Room;

#define N_EMO_MAX 24                  /* >= N_EMO (checked below) */

typedef struct React {
    char *id, *target, *key, *sender;   /* key: UTF-8 */
    int own;
} React;

/* an attachment in a chat window: the picture as strips of line height */
typedef struct Media {
    char *eid, *media, *name, *type;
    int idx;                      /* item of the event's attachment list */
    int state;                    /* 0 none, 1 loading, 2 shown, 3 no preview */
    char *path, *saving;          /* saved to T:, being saved */
    int open_after;               /* opening (T:) - else a download where the user chose */
    int n_slices, img0;           /* NList images img0.. */
    Object **obj;
    struct MUI_AlphaData **ad;
} Media;

/* a message shown in a chat window: for quotes, edits, deleting */
typedef struct ChatMsg {
    char *eid, *sender, *name, *body, *msgtype, *reply_to, *thread_root;   /* body: UTF-8 */
    char *formatted;              /* its HTML form, if any */
    int mention;
    double ts;
    int own, edited, redacted;
    double edit_ts;
    int read;                     /* own, read by someone else (a check mark) */
    int thread_count;             /* replies in the thread it starts */
} ChatMsg;

typedef struct ChatWin {
    struct ChatWin *next;
    char *room_id;
    Object *win, *list, *input, *older, *invite, *leave, *reactb, *replyb, *editb, *delb, *attachb, *infob;
    int uploading;                /* files on their way up */
    int n_cached;                 /* messages shown from the local history cache */
    Object *pinbar;               /* "Angepinnt: ..." above the messages */
    Object *tombbar;              /* "replaced by a new room": a click goes there */
    char *pin_shown;              /* the event the bar shows */
    char *quote;                  /* quote mode: this text goes above the message (UTF-8) */
    Object *sugg, *sbtn[5];       /* suggestions above the input: @names, :emojis: */
    char *stext[5];               /* what a suggestion inserts (local charset) */
    char *sid[5];                 /* its user ID (a mention) or NULL */
    int n_sugg;
    AmxMember *mem;               /* the members, for @names (loaded at the first "@") */
    int n_mem, mem_asked;
    char *ment_name[16], *ment_id[16];   /* mentions chosen for the message being written */
    int n_ment;
    char *export_path;            /* export after loading the whole history */
    double jump_ts;               /* /jumptodate: load older pages until this time (ms) */
    int jump_pages;
    int unread_open, unread_done; /* unread messages when the window opened: a marker above them */
    int draft_pending;            /* put the room's draft into the input once the window is up */
    struct Poll *polls;           /* polls in this window, and the votes (also before their poll) */
    int n_polls;
    struct Vote *votes;
    int n_votes;
    char typing[160];             /* who is typing (local charset), "" nobody */
    int typing_on;                /* we told the server we type */
    time_t typing_sent;
    Object *modegrp, *modetext, *modex;   /* "Antwort an ..." / "Bearbeiten" above the input */
    char *reply_eid, *edit_eid;
    ChatMsg *msgs;
    int n_msgs, cap_msgs;
    struct PendingEdit { char *eid, *body, *msgtype, *formatted; double ts; } *pedit;
    int n_pedit, cap_pedit;
    long top_day, bottom_day;     /* date separators: day of the top / bottom message */
    char *thread_root;            /* a thread window: the thread's first message */
    long next_serial;
    struct Media *media;          /* attachments shown in this window */
    int n_media, cap_media;
    char **links;                 /* URLs in the text, for the globe pictures */
    int n_links, cap_links;
    int next_img;                 /* next free NList image number (pictures) */
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
    char err[320];                /* last error, shown until the next action */
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
static Object *app, *win_main, *lv_rooms, *lst_rooms, *txt_status, *str_filter, *grp_space, *cyc_space;
static Object *win_find, *str_find, *cyc_findwhere, *b_find, *lst_hits, *txt_find;
static Object *win_acct, *str_pwold, *str_pwnew, *str_pwnew2, *b_pwgo, *str_dpw, *chk_derase, *b_dgo, *txt_acct;
static Object *win_poll, *str_pollq, *str_polla[6], *chk_pollmulti, *chk_pollhide, *b_pollsend;
static char *poll_room;
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
static Object *win_settings, *chk_notify, *cyc_notify, *chk_previews, *chk_iconified, *txt_herald, *b_set_ok;
static Object *chk_typing, *chk_receipts, *chk_linkprev, *chk_linkprev_enc, *chk_hcache, *chk_avatars, *chk_joins,
              *chk_24h, *chk_compact;
static Object *win_info, *str_iname, *str_itopic, *cyc_ijoin, *cyc_ihist, *chk_ienc, *b_ipic, *b_isave, *lst_mem,
              *b_ikick, *b_iban, *b_iunban, *cyc_irole, *b_irole, *b_iinvite, *txt_istatus, *grp_iset, *b_iadmit;
static Object *win_prof, *txt_pimg, *txt_pid, *str_pname, *b_ppic, *b_psave, *txt_pstatus;
static Object *win_sess, *lst_sess, *str_sname, *b_srename, *str_spass, *b_sdelete, *txt_sstatus;
static const char *notify_modes[] = { "allen Nachrichten", "Direktnachrichten und Erwähnungen", NULL };
static struct DiskObject *dob_app, *dob_new;   /* AppIcon: normal / unread messages */
struct Library *IconBase;
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
    set(txt_status, MUIA_ShortHelp, status_buf);     /* the full text, also when cut off */
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
    for (w = chats; w; w = w->next) if (!w->thread_root && !strcmp(w->room_id, id)) return w;
    return NULL;
}

/* the window of a thread, if open */
static ChatWin *chat_thread(const char *room, const char *root)
{
    ChatWin *w;
    for (w = chats; w; w = w->next)
        if (w->thread_root && !strcmp(w->room_id, room) && !strcmp(w->thread_root, root)) return w;
    return NULL;
}

/* the status text in a chat window's toolbar, and the busy pointer while
 * something runs (the window stays usable) */
static void update_status(ChatWin *w)
{
    static char t[340];
    const Room *r = room_by_id(w->room_id);
    struct Window *iw;
    int busy = w->loading || w->sending || w->inviting || w->uploading;
    if (w->loading) snprintf(t, sizeof t, w->loading_older ? "Lade ältere Nachrichten ..." : "Lade Verlauf ...");
    else if (w->uploading) snprintf(t, sizeof t, w->uploading > 1 ? "Lade %d Dateien hoch ..." : "Lade Datei hoch ...",
                                    w->uploading);
    else if (w->sending) snprintf(t, sizeof t, w->sending > 1 ? "Sende %d Nachrichten ..." : "Sende ...", w->sending);
    else if (w->inviting) snprintf(t, sizeof t, "Lade ein ...");
    else if (w->err[0]) snprintf(t, sizeof t, "\33b%s", w->err);
    else if (w->typing[0]) snprintf(t, sizeof t, "\33i%s %s ...", w->typing, strchr(w->typing, ',') ? "tippen" : "tippt");
    else snprintf(t, sizeof t, "%s", !r ? "" : r->encrypted ? "Ende-zu-Ende-verschlüsselt" : "nicht verschlüsselt");
    set(w->busy, MUIA_Text_Contents, t);
    set(w->busy, MUIA_ShortHelp, w->err[0] ? w->err : NULL);   /* full error text */
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

/* settings (Einstellungen) */
static int opt_notify = 1;            /* notifications through Herald (if it is installed) */
static int opt_notify_all = 1;        /* 1: every message, 0: direct chats and mentions only */
static int opt_previews = 1;          /* picture previews in the chat */
static int opt_iconified = 0;         /* start iconified (AppIcon on the Workbench) */
static int opt_typing = 1;            /* tell the others when we type */
static int opt_receipts = 1;          /* read receipts others see (else m.read.private) */
static int opt_linkprev = 1;          /* link previews (the server fetches the page) */
static int opt_linkprev_enc = 0;      /* ... also in encrypted rooms */
static int opt_hcache = 1;            /* keep the newest messages on disk (window opens at once) */
static int opt_avatars = 1;           /* pictures in the room list */
static int opt_joins = 1;             /* show joins, leaves, invitations in the chat */
static int opt_24h = 1;               /* 14:05 (else 02:05PM) */
static int opt_compact = 0;           /* the name only when the sender changes (within 5 minutes) */
static char **muted;                  /* rooms without notifications */
static int n_muted;

static Room *room_by_id(const char *id);

/* drafts: what was typed but not sent, per room (local charset), kept in the prefs */
typedef struct { char *room, *text; } Draft;
static Draft drafts[40];
static int n_drafts;

static const char *draft_of(const char *room)
{
    int i;
    for (i = 0; room && i < n_drafts; i++) if (!strcmp(drafts[i].room, room)) return drafts[i].text;
    return NULL;
}

static void set_draft(const char *room, const char *text)
{
    int i;
    for (i = 0; i < n_drafts; i++)
        if (!strcmp(drafts[i].room, room)) {
            free(drafts[i].room); free(drafts[i].text);
            memmove(&drafts[i], &drafts[i + 1], sizeof *drafts * (n_drafts - i - 1));
            n_drafts--;
            break;
        }
    if (!text || !*text || strchr(text, '\n')) return;
    if (n_drafts == 40) { free(drafts[0].room); free(drafts[0].text); memmove(drafts, drafts + 1, sizeof *drafts * 39); n_drafts--; }
    drafts[n_drafts].room = strdup(room);
    drafts[n_drafts].text = strdup(text);
    n_drafts++;
}

/* muted: off on the server (all devices), or in our old local list */
static int is_muted(const char *room)
{
    int i;
    const Room *r = room ? room_by_id(room) : NULL;
    if (r && r->notify == AMX_NOTIFY_OFF) return 1;
    for (i = 0; room && i < n_muted; i++) if (!strcmp(muted[i], room)) return 1;
    return 0;
}

static void set_muted(const char *room, int on)
{
    int i;
    for (i = 0; i < n_muted; i++)
        if (!strcmp(muted[i], room)) {
            if (on) return;
            free(muted[i]);
            muted[i] = muted[--n_muted];
            return;
        }
    if (on) {
        char **t = realloc(muted, sizeof *t * (n_muted + 1));
        if (t && (t[n_muted] = strdup(room)) != NULL) { muted = t; n_muted++; }
        else if (t) muted = t;
    }
}

/* config: PROGDIR:Amatrix.prefs - server, user, then key=value lines */
static void load_prefs(void)
{
    char path[300], line[512];
    FILE *f;
    snprintf(path, sizeof path, "%sAmatrix.prefs", prog_dir);
    if (!(f = fopen(path, "r"))) return;
    if (fgets(line, sizeof line, f)) { line[strcspn(line, "\r\n")] = 0; cfg_server = xstrdup(line); }
    if (fgets(line, sizeof line, f)) { line[strcspn(line, "\r\n")] = 0; cfg_user = xstrdup(line); }
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "notify=", 7)) opt_notify = atoi(line + 7);
        else if (!strncmp(line, "notify_all=", 11)) opt_notify_all = atoi(line + 11);
        else if (!strncmp(line, "previews=", 9)) opt_previews = atoi(line + 9);
        else if (!strncmp(line, "iconified=", 10)) opt_iconified = atoi(line + 10);
        else if (!strncmp(line, "typing=", 7)) opt_typing = atoi(line + 7);
        else if (!strncmp(line, "receipts=", 9)) opt_receipts = atoi(line + 9);
        else if (!strncmp(line, "linkprev=", 9)) opt_linkprev = atoi(line + 9);
        else if (!strncmp(line, "linkprev_enc=", 13)) opt_linkprev_enc = atoi(line + 13);
        else if (!strncmp(line, "hcache=", 7)) opt_hcache = atoi(line + 7);
        else if (!strncmp(line, "avatars=", 8)) opt_avatars = atoi(line + 8);
        else if (!strncmp(line, "joins=", 6)) opt_joins = atoi(line + 6);
        else if (!strncmp(line, "draft=", 6) && strchr(line + 6, '\t')) {
            char *tab = strchr(line + 6, '\t');
            *tab = 0;
            set_draft(line + 6, tab + 1);
        }
        else if (!strncmp(line, "24h=", 4)) opt_24h = atoi(line + 4);
        else if (!strncmp(line, "compact=", 8)) opt_compact = atoi(line + 8);
        else if (!strncmp(line, "mute=", 5) && line[5]) set_muted(line + 5, 1);
    }
    fclose(f);
    if (cfg_user && !*cfg_user) { free(cfg_user); cfg_user = NULL; }
}

static void save_prefs(void)
{
    char path[300];
    FILE *f;
    int i;
    snprintf(path, sizeof path, "%sAmatrix.prefs", prog_dir);
    if (!(f = fopen(path, "w"))) return;
    fprintf(f, "%s\n%s\n", cfg_server ? cfg_server : "", cfg_user ? cfg_user : "");
    fprintf(f, "notify=%d\nnotify_all=%d\npreviews=%d\niconified=%d\ntyping=%d\nreceipts=%d\n", opt_notify,
            opt_notify_all, opt_previews, opt_iconified, opt_typing, opt_receipts);
    fprintf(f, "linkprev=%d\nlinkprev_enc=%d\nhcache=%d\n", opt_linkprev, opt_linkprev_enc, opt_hcache);
    fprintf(f, "avatars=%d\njoins=%d\n24h=%d\ncompact=%d\n", opt_avatars, opt_joins, opt_24h, opt_compact);
    {
        int i;
        for (i = 0; i < n_drafts; i++) fprintf(f, "draft=%s\t%s\n", drafts[i].room, drafts[i].text);
    }
    for (i = 0; i < n_muted; i++) fprintf(f, "mute=%s\n", muted[i]);
    fclose(f);
}

/* ---------------------------------------------------------------- notifications (Herald) */

/* Herald (Aminet util/cdity/Herald) shows notifications for any program
 * through its ARexx port HERALD.  Optional: without it, none. */
static int herald_registered;
static time_t started_at;

/* one ARexx command to a port, waiting for the answer; res gets RESULT */
static LONG rexx_cmd(const char *portname, const char *cmd, char *res, size_t n)
{
    struct Library *RexxSysBase = OpenLibrary("rexxsyslib.library", 36);
    struct MsgPort *reply, *port;
    struct RexxMsg *rm;
    LONG rc = -1;
    if (res && n) res[0] = 0;
    if (!RexxSysBase) return -1;
    if ((reply = CreateMsgPort()) != NULL) {
        if ((rm = CreateRexxMsg(reply, NULL, NULL)) != NULL) {
            rm->rm_Args[0] = (STRPTR)CreateArgstring((STRPTR)cmd, strlen(cmd));
            rm->rm_Action = RXCOMM | RXFF_RESULT;
            Forbid();
            if ((port = FindPort((STRPTR)portname)) != NULL) PutMsg(port, &rm->rm_Node);
            Permit();
            if (port) {
                WaitPort(reply);
                GetMsg(reply);
                rc = rm->rm_Result1;
                if (rc == 0 && rm->rm_Result2) {
                    if (res && n) snprintf(res, n, "%s", (char *)rm->rm_Result2);
                    DeleteArgstring((UBYTE *)rm->rm_Result2);
                }
            }
            DeleteArgstring((UBYTE *)rm->rm_Args[0]);
            DeleteRexxMsg(rm);
        }
        DeleteMsgPort(reply);
    }
    CloseLibrary(RexxSysBase);
    return rc;
}

static int herald_here(void)
{
    struct MsgPort *p;
    Forbid();
    p = FindPort((STRPTR)"HERALD");
    Permit();
    return p != NULL;
}

/* a value for Herald's ReadArgs: " -> *", * -> ** */
static void herald_quote(char *out, size_t n, const char *in)
{
    size_t k = 0;
    for (; *in && k + 3 < n; in++) {
        if (*in == '"' || *in == '*') out[k++] = '*';
        out[k++] = *in == '\n' ? ' ' : *in;
    }
    out[k] = 0;
}

static void herald_register(void)
{
    char cmd[400], res[80];
    if (herald_registered || !herald_here()) return;
    snprintf(cmd, sizeof cmd, "REGISTERAPP APP=Amatrix AREXXPORT=AMATRIX.1 ICON=\"%sAmatrix\"", prog_dir);
    rexx_cmd("HERALD", cmd, res, sizeof res);
    herald_registered = 1;                    /* "already registered" is fine too */
    amx_log("Herald: %s", res);
}

static char *local_emoji_as(const char *utf8, int mui);

/* a new message from someone else: a notification, unless the room's window
 * is in front, the room is muted or the user does not want them */
static void notify_event(const AmxEvent *e)
{
    Room *r;
    ChatWin *w;
    char *name, *body, *title, cmd[1200], qt[200], qn[300], res[80], id[16];
    int mention;
    if (e->type != AMX_EV_MESSAGE || e->own || e->history || !e->room_id || e->redacted) return;
    if (e->timestamp > 0 && (time_t)(e->timestamp / 1000) < started_at - 60) return;   /* the backlog */
    if (!(r = room_by_id(e->room_id)) || is_muted(r->id)) return;
    w = chat_by_room(e->room_id);
    if (w && xget(w->win, MUIA_Window_Activate) && !xget(app, MUIA_Application_Iconified)) return;
    mention = 0;
    if (my_user && e->body) {                             /* @user:server or the localpart */
        char lp[100];
        const char *c = strchr(my_user, ':');
        snprintf(lp, sizeof lp, "%.*s", c ? (int)(c - my_user - 1) : (int)strlen(my_user), my_user + 1);
        mention = e->mention || strstr(e->body, my_user) || (lp[0] && strstr(e->body, lp));
    }
    if (r->notify == AMX_NOTIFY_MENTIONS && !mention) return;      /* the server's setting for the room */
    if (!opt_notify_all && !r->direct && !mention && r->notify != AMX_NOTIFY_ALL) return;
    if (!opt_notify || !herald_here()) return;
    herald_register();
    name = local(e->sender_name ? e->sender_name : e->sender ? e->sender : "");
    body = local_emoji_as(e->body ? e->body : "", 2);
    title = local(r->name ? r->name : "Amatrix");
    snprintf(id, sizeof id, "r%08lx", hash(r->id) & 0xffffffffUL);
    herald_quote(qt, sizeof qt, title);
    herald_quote(qn, sizeof qn, r->id);
    snprintf(cmd, sizeof cmd, "NOTIFY APP=Amatrix ID=%s GROUP=%s UPDATE PRI=%d TITLE=\"%s\" "
             "AREXXPORT=AMATRIX.1 BACKRXMSG=\"OPENCHAT %s\" TEXT=%s%s%s",
             id, id, mention ? 6 : 2, qt, qn, r->direct ? "" : name, r->direct ? "" : ": ", body);
    rexx_cmd("HERALD", cmd, res, sizeof res);
    amx_log("Herald notify %s: %s", id, res);
    free(name); free(body); free(title);
}

/* read: away with the room's notification */
static void notify_close(const char *room)
{
    char cmd[120];
    if (!herald_registered || !herald_here()) return;
    snprintf(cmd, sizeof cmd, "HERALD_CLOSE APP=Amatrix ID=r%08lx", hash(room) & 0xffffffffUL);
    rexx_cmd("HERALD", cmd, NULL, 0);
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
static Alpha icon_up, icon_invite, icon_leave, icon_react, icon_reply, icon_edit, icon_trash, icon_attach, icon_info;
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

static const char *const art_reply[IC] = {
    "................",
    "......k.........",
    ".....kbk........",
    "....kbbk........",
    "...kbbbkkkkkk...",
    "..kbbbbbbbbbbk..",
    ".kbbbbbbbbbbbBk.",
    "..kbbbbbbbbbbbBk",
    "...kbbbkkkkkbbBk",
    "....kbbk...kbbBk",
    ".....kbk...kbbBk",
    "......k....kbBk.",
    "..........kbBk..",
    ".........kbBk...",
    "..........kk....",
    "................" };
static const char *const art_edit[IC] = {
    "................",
    "...........kk...",
    "..........kffk..",
    ".........kyfffk.",
    "........kyyYfk..",
    ".......kyyYYk...",
    "......kyyYYk....",
    ".....kyyYYk.....",
    "....kyyYYk......",
    "...kyyYYk.......",
    "..knyYYk........",
    "..knnYk.........",
    "..kkNk..........",
    "..kkk...........",
    "................",
    "................" };
static const char *const art_trash[IC] = {
    "......kkkk......",
    ".....kllllk.....",
    "..kkkkkkkkkkkk..",
    "..kwwwwwwwwwlk..",
    "..kkkkkkkkkkkk..",
    "...kwlwlwlwldk..",
    "...kwldldldldk..",
    "...kwldldldldk..",
    "...kwldldldldk..",
    "...kwldldldldk..",
    "...kwldldldldk..",
    "...kwldldldldk..",
    "...kwldldldldk..",
    "...kllddddddk...",
    "....kkkkkkkkk...",
    "................" };

static const char *const art_info[IC] = {
    ".....kkkkkk.....",
    "...kkbbbbbbkk...",
    "..kbbbbwwbbbBk..",
    ".kbbbbwwwwbbBBk.",
    ".kbbbbbwwbbbbBkd",
    "kbbbbbbbbbbbbBBk",
    "kbbbbwwwwbbbbBBk",
    "kbbbbbwwwbbbbBBk",
    "kbbbbbwwwbbbbBBk",
    "kbbbbbwwwbbbbBBk",
    "kbbbbbwwwbbbbBBk",
    ".kbbbwwwwwbbBBkd",
    ".kBbbbbbbbbBBBkd",
    "..kBBbbbbbBBBkdd",
    "...kkBBBBBBkkdd.",
    ".....kkkkkkddd.." };

static const char *const art_attach[IC] = {
    "..kkkkkkkk......",
    "..kwwwwwwkk.....",
    "..kwwwwwwklk....",
    "..kwwwwwwkkkk...",
    "..kwwwwkwwwlkd..",
    "..kwwwkbkwwlkd..",
    "..kwwkbbBkwlkd..",
    "..kwkbbbbBklkd..",
    "..kwkkbbBkklkd..",
    "..kwwkbbBkwlkd..",
    "..kwwkbbBkwlkd..",
    "..kwwkBBBkwlkd..",
    "..kwwwkkkwwlkd..",
    "..klllllllllkd..",
    "..kkkkkkkkkkkd..",
    "...ddddddddddd.." };

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
enum { EMO_THUMBSUP, EMO_THUMBSDOWN, EMO_HEART, EMO_SMILE, EMO_JOY, EMO_CONFUSED, EMO_EYES, EMO_TADA, EMO_ROCKET, EMO_UNKNOWN, EMO_FILE, EMO_LINK, EMO_THREAD, EMO_CHECK, EMO_RADIO_OFF, EMO_RADIO_ON, EMO_STOP, EMO_STAR, N_EMO };
typedef char n_emo_fits[N_EMO <= N_EMO_MAX ? 1 : -1];
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
    { /* file: a click opens the attachment */
      ".kkkkkkk....",
      ".kwwwwwkk...",
      ".kwddwwklk..",
      ".kwwwwwkkkk.",
      ".kwdddddwwk.",
      ".kwwwwwwwwk.",
      ".kwddddddwk.",
      ".kwwwwwwwwk.",
      ".kwdddddwwk.",
      ".kwwwwwwwwk.",
      ".kkkkkkkkkk.",
      "............" },
    { /* link: a click opens the browser */
      "....kkkk....",
      "..kkcbbckk..",
      ".kcbGGbcbck.",
      ".kbGGGbbcbk.",
      "kcbGGbbbbcck",
      "kccbbbbGGcbk",
      "kbcbbbGGGbck",
      "kcbbbbGGbcbk",
      ".kbcbbbGbck.",
      ".kcbcbbbcck.",
      "..kkbccbkk..",
      "....kkkk...." },
    { /* thread: a click opens its window */
      "............",
      "..kkkkkkk...",
      ".kwwwwwwwk..",
      ".kwkkkkkwk..",
      ".kwwwwwwwkk.",
      ".kwkkkkwwkbk",
      "..kkkkkkkbbk",
      "...kk.kbbbbk",
      "...k..kbbbbk",
      ".......kkkbk",
      "..........kk",
      "............" },
    { /* read: one of the others has read it */
      "............",
      "..........kk",
      ".........kbk",
      "........kbBk",
      ".......kbBk.",
      "kk....kbBk..",
      "kbk..kbBk...",
      "kBbkkbBk....",
      ".kBbbBk.....",
      "..kBBk......",
      "...kk.......",
      "............" },
    { /* radio button, empty: a poll answer not chosen */
      "............",
      "....kkkk....",
      "..kkwwwwkk..",
      "..kwwwwwwk..",
      ".kwwwwwwwwk.",
      ".kwwwwwwwwk.",
      ".kwwwwwwwwk.",
      ".kwwwwwwwwk.",
      "..kwwwwwwk..",
      "..kkwwwwkk..",
      "....kkkk....",
      "............" },
    { /* radio button, chosen */
      "............",
      "....kkkk....",
      "..kkwwwwkk..",
      "..kwwbbwwk..",
      ".kwwbbbbwwk.",
      ".kwbbbbbbwk.",
      ".kwbbbbbbwk.",
      ".kwwbbbbwwk.",
      "..kwwbbwwk..",
      "..kkwwwwkk..",
      "....kkkk....",
      "............" },
    { /* stop: end a poll */
      "............",
      "............",
      "..kkkkkkkk..",
      "..krrrrrrk..",
      "..krrrrrrk..",
      "..krrrrrrk..",
      "..krrrrrrk..",
      "..krrrrrrk..",
      "..krrrrrrk..",
      "..kkkkkkkk..",
      "............",
      "............" },
    { /* star: a favourite room */
      "............",
      ".....kk.....",
      "....kyyk....",
      "....kyyk....",
      "kkkkyyyykkkk",
      "kyyyyyyyyyYk",
      ".kyyyyyyyYk.",
      "..kyyyyyYk..",
      "..kyyyYyYk..",
      ".kyyYkkYYYk.",
      ".kYYk..kYYk.",
      ".kkk....kkk." },
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

/* mui: 1 for MUI's List (ESC A with the ARGB data), 2 for Herald (:shortcode:),
 * 0 for NList (ESC o) */
static char *local_emoji_as(const char *utf8, int mui)
{
    /* a sender's text must not carry MUI codes (clickable pictures, pens ...):
     * every ESC is dropped; our own style marks (richtext.h) become ESC at the end */
    char *clean = strdup(utf8 ? utf8 : ""), *cw, *cr;
    const unsigned char *p, *seg;
    char *b = NULL;
    size_t n = 0, cap = 0;
    if (!clean) return NULL;
    for (cw = cr = clean; *cr; cr++) if (*cr != 27) *cw++ = *cr;
    *cw = 0;
    p = seg = (const unsigned char *)clean;
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
            if (mui == 2) {                               /* Herald: its :shortcodes: */
                static const char *const sc[] = { ":thumbsup:", ":thumbsdown:", ":heart:", ":smile:", ":joy:",
                                                  ":confused:", ":eyes:", ":tada:", ":rocket:" };
                snprintf(esc, sizeof esc, "%s", img >= 0 && img < EMO_UNKNOWN ? sc[img] : "");
            } else {
                if (img < 0) img = EMO_UNKNOWN;
                if (mui) snprintf(esc, sizeof esc, "\33A[%08lx]", (unsigned long)&emo_img[img].ad);
                else snprintf(esc, sizeof esc, "\33o[%d]", img);
            }
            sb_add(&b, &n, &cap, esc, strlen(esc));
            p = seg = q;
        } else if (is_emoji_mod(cp) || cp == 0x200D) {        /* stray: e.g. a keycap's parts */
            sb_local(&b, &n, &cap, (const char *)seg, (const char *)p);
            p = seg = p + len;
        } else
            p += len;
    }
    sb_local(&b, &n, &cap, (const char *)seg, (const char *)p);
    free(clean);
    for (cr = b; cr && *cr; cr++)
        if (*cr == RICH_MARK) {
            if (mui != 2 && cr[1] && strchr("biun", cr[1])) *cr = 27;
            else {                                /* Herald, or not one of ours: away with it */
                memmove(cr, cr + (mui == 2 && cr[1] ? 2 : 1), strlen(cr + (mui == 2 && cr[1] ? 2 : 1)) + 1);
                cr--;
            }
        }
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
    if (!opt_avatars) return;
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
    strftime(buf, n, tm_t.tm_yday + tm_t.tm_year * 400 == today ? (opt_24h ? "%H:%M" : "%I:%M%p") : "%d.%m.", &tm_t);
}

/* three columns: picture | name, second line | time */
HOOKPROTONH(room_disp_func, LONG, char **array, Room *r)
{
    static char img[24], text[640], when[16];
    char second[200];
    if (!r) { array[0] = array[1] = array[2] = ""; return 0; }
    if (opt_avatars) snprintf(img, sizeof img, "\33A[%08lx]", (unsigned long)&image_for(r)->ad);
    else img[0] = 0;
    second_line(r, second, sizeof second);
    if (!r->invited && draft_of(r->id)) snprintf(second, sizeof second, "\33bEntwurf:\33n\33i %.150s", draft_of(r->id));
    {
        const char *mu = r->replacement ? "\33n\33i (ersetzt)" : is_muted(r->id) ? "\33n\33i (stumm)" : "";
        static char star[24];
        char nm[300];
        if (r->favourite) snprintf(star, sizeof star, " \33A[%08lx]", (unsigned long)&emo_img[EMO_STAR].ad);
        else star[0] = 0;
        snprintf(nm, sizeof nm, "%s%s", r->name, star);
        if (r->invited)
            snprintf(text, sizeof text, "\33b%s\33n\n\33i%s", nm, second);
        else if (r->unread)              /* unread: count, second line in normal colour */
            snprintf(text, sizeof text, "\33b%s (%d%s)%s\33n\n\33i%s", nm, r->unread,
                     r->highlights ? " @" : "", mu, second);
        else
            snprintf(text, sizeof text, "%s%s%s\33n\n\33i\33P[808080]%s", r->low ? "" : "\33b", nm, mu, second);
    }
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
        free(l[i].topic); free(l[i].last_sender); free(l[i].last_body); free(l[i].parents);
        free(l[i].inviter_id); free(l[i].pinned); free(l[i].replacement);
    }
    free(l);
}

static void open_chat(const char *room_id);
static void open_chat_ex(const char *room_id, const char *thread_root);

/* unread messages (rooms not muted): main window title and AppIcon */
static void show_unread_total(void)
{
    static char title[40];
    static int shown = -1;
    int i, total = 0;
    for (i = 0; i < n_rooms; i++) if (!rooms[i].invited && !is_muted(rooms[i].id)) total += rooms[i].unread;
    if (total == shown) return;
    shown = total;
    if (total) snprintf(title, sizeof title, "Amatrix (%d)", total);
    else snprintf(title, sizeof title, "Amatrix");
    set(win_main, MUIA_Window_Title, title);
    if (dob_app && dob_new) set(app, MUIA_Application_DiskObject, total ? dob_new : dob_app);
}

static void update_pinbar(ChatWin *w);
static struct Hook pinbar_hook, tomb_hook;
static void refill_rooms(const char *active);
HOOKPROTONHNO(filter_func, LONG, APTR unused)
{
    Room *act = NULL;
    (void)unused;
    DoMethod(lst_rooms, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &act);
    refill_rooms(act ? act->id : NULL);
    return 0;
}
MakeStaticHook(filter_hook, filter_func);

/* ---- the room list as shown: no spaces, only the chosen space's rooms,
 * only names with the filter text */
static char *space_ids[32];
static char *space_names[34];
static int n_spaces;

static int name_has(const char *name, const char *f)
{
    size_t n = strlen(f);
    const char *p;
    if (!n) return 1;
    for (p = name; p && *p; p++) {
        size_t i;
        for (i = 0; i < n && p[i]; i++) {
            unsigned char a = (unsigned char)p[i], b = (unsigned char)f[i];
            if ((a >= 'A' && a <= 'Z') || (a >= 0xc0 && a <= 0xde && a != 0xd7)) a += 32;
            if ((b >= 'A' && b <= 'Z') || (b >= 0xc0 && b <= 0xde && b != 0xd7)) b += 32;
            if (a != b) break;
        }
        if (i == n) return 1;
    }
    return 0;
}

static void refill_rooms(const char *active)
{
    const char *f = str_filter ? (const char *)xget(str_filter, MUIA_String_Contents) : "";
    LONG sel = n_spaces && cyc_space ? (LONG)xget(cyc_space, MUIA_Cycle_Active) : 0, row = 0, pos = MUIV_List_Active_Off;
    const char *sp = sel > 0 && sel <= n_spaces ? space_ids[sel - 1] : NULL;
    int i;
    set(lst_rooms, MUIA_List_Quiet, TRUE);
    DoMethod(lst_rooms, MUIM_List_Clear);
    for (i = 0; i < n_rooms; i++) {
        Room *r = &rooms[i];
        if (r->is_space || !name_has(r->name, f ? f : "")) continue;
        if (sp && !(r->parents && strstr(r->parents, sp))) continue;
        DoMethod(lst_rooms, MUIM_List_InsertSingle, r, MUIV_List_Insert_Bottom);
        if (active && !strcmp(active, r->id)) pos = row;
        row++;
    }
    set(lst_rooms, MUIA_List_Active, pos);
    set(lst_rooms, MUIA_List_Quiet, FALSE);
}

/* the space choice above the list: rebuilt when the spaces change */
static void update_spaces(void)
{
    int i, n = 0, same;
    char *ids[32];
    LONG sel = n_spaces && cyc_space ? (LONG)xget(cyc_space, MUIA_Cycle_Active) : 0;
    char *keep = sel > 0 && sel <= n_spaces ? strdup(space_ids[sel - 1]) : NULL;
    for (i = 0; i < n_rooms && n < 32; i++) if (rooms[i].is_space && !rooms[i].invited) ids[n++] = rooms[i].id;
    same = n == n_spaces;
    for (i = 0; same && i < n; i++) same = !strcmp(ids[i], space_ids[i]);
    if (same && cyc_space) {
        for (i = 0; i < n; i++) {              /* names may change; a new cycle only for other spaces */
            Room *r = room_by_id(ids[i]);
            if (r && strcmp(r->name, space_names[i + 1])) same = 0;
        }
    }
    if (same && cyc_space) { free(keep); return; }
    for (i = 0; i < n_spaces; i++) { free(space_ids[i]); free(space_names[i + 1]); }
    space_names[0] = "Alle Räume";
    for (i = 0; i < n; i++) {
        space_ids[i] = strdup(ids[i]);
        space_names[i + 1] = strdup(room_by_id(ids[i])->name);
    }
    space_names[n + 1] = NULL;
    n_spaces = n;
    DoMethod(grp_space, MUIM_Group_InitChange);
    if (cyc_space) { DoMethod(grp_space, OM_REMMEMBER, cyc_space); MUI_DisposeObject(cyc_space); }
    cyc_space = CycleObject, MUIA_Cycle_Entries, space_names, MUIA_CycleChain, TRUE,
                MUIA_ShortHelp, "Nur die Räume eines Space zeigen", End;
    if (cyc_space) {
        DoMethod(grp_space, OM_ADDMEMBER, cyc_space);
        for (i = 0; keep && i < n; i++) if (!strcmp(space_ids[i], keep)) set(cyc_space, MUIA_Cycle_Active, i + 1);
        DoMethod(cyc_space, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime, app, 2, MUIM_CallHook, &filter_hook);
    }
    set(grp_space, MUIA_ShowMe, n > 0);
    DoMethod(grp_space, MUIM_Group_ExitChange);
    free(keep);
}

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
        nl[i].favourite = ri->favourite;
        nl[i].low = ri->low_priority;
        nl[i].is_space = ri->is_space;
        nl[i].parents = xstrdup(ri->parents);
        nl[i].inviter_id = xstrdup(ri->inviter);
        nl[i].notify = ri->notify;
        nl[i].pinned = xstrdup(ri->pinned);
        nl[i].replacement = xstrdup(ri->replacement);
        {   /* keep what only the GUI knows */
            Room *old = room_by_id(ri->room_id);
            if (old) nl[i].read_ts = old->read_ts;
        }
    }
    free_rooms(rooms, n_rooms);
    rooms = nl;
    n_rooms = m->n;
    (void)pos;
    update_spaces();
    refill_rooms(active);
    {
        ChatWin *cw;
        for (cw = chats; cw; cw = cw->next) update_pinbar(cw);
    }
    show_unread_total();
    free(active);
    request_avatars();
    for (w = chats; w; w = w->next) { w->mark_pending = 0; update_status(w); }
    for (w = chats; w; w = w->next) {      /* window titles follow renames */
        Room *r = room_by_id(w->room_id);
        if (r && !w->thread_root && (!w->title || strcmp(w->title, r->name))) {
            free(w->title);
            w->title = strdup(r->name);
            set(w->win, MUIA_Window_Title, w->title);
        }
    }
    if (pending_open && room_by_id(pending_open) && !room_by_id(pending_open)->invited) {
        char *id = pending_open;
        pending_open = NULL;
        if (!room_by_id(id)->is_space) open_chat(id);     /* a space is no chat: it shows in the choice */
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
    int sep;                      /* a date separator */
    int thr;                      /* "Thread: n Antworten" under the message eid */
    int slice;                    /* a strip of a picture (media entry midx) */
    int prev;                     /* the link preview under the message eid */
    int pollrow;                  /* an answer / the status line of the poll eid */
    int newmark;                  /* "- neue Nachrichten -" */
    int midx;
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

/* back to plain writing (no reply, no edit) */
static void clear_mode(ChatWin *w)
{
    free(w->quote); w->quote = NULL;
    free(w->reply_eid); w->reply_eid = NULL;
    free(w->edit_eid); w->edit_eid = NULL;
    set(w->modegrp, MUIA_ShowMe, FALSE);
}

static char *expand_shortcodes(const char *t);
static void clear_mentions(ChatWin *w);

/* a message to the server from this window (reply / thread / edit mode applied) */
static void send_out(ChatWin *w, const char *msgtype, const char *body, const char *html, const char *mentions)
{
    AmxOutMsg om;
    memset(&om, 0, sizeof om);
    om.msgtype = msgtype;
    om.body = body;
    om.html = html;
    om.mentions = mentions;
    if (w->edit_eid) om.replaces = w->edit_eid;
    else { om.reply_to = w->reply_eid; om.thread_root = w->thread_root; }
    amx_async_send_message(A, w->room_id, &om, TAG_SEND);
    clear_mode(w);
    if (w->thread_root) {                       /* the result finds this window again */
        char key[600];
        snprintf(key, sizeof key, "%s\t%s", w->room_id, w->thread_root);
        fifo_push(fifo_send, &n_send, 64, key);
    } else
        fifo_push(fifo_send, &n_send, 64, w->room_id);
    w->sending++;
    w->err[0] = 0;
    update_status(w);
}

static void chat_say(ChatWin *w, const char *msg)
{
    snprintf(w->err, sizeof w->err, "%s", msg);
    update_status(w);
}

/* "@name:server" from what was typed ("name", "@name", full ID) */
static char *full_user(const char *t)
{
    const char *srv = my_user ? strchr(my_user, ':') : NULL;
    char *u;
    while (*t == ' ') t++;
    if (!*t) return NULL;
    u = malloc(strlen(t) + (srv ? strlen(srv) : 0) + 3);
    if (!u) return NULL;
    sprintf(u, "%s%s", *t == '@' ? "" : "@", t);
    if (!strchr(u, ':') && srv) strcat(u, srv);
    {   /* the ID ends at the first space */
        char *sp = strchr(u, ' ');
        if (sp) *sp = 0;
    }
    return u;
}

/* the part after the user ID ("@a:b reason" -> "reason") */
static const char *after_word(const char *t)
{
    while (*t == ' ') t++;
    while (*t && *t != ' ') t++;
    while (*t == ' ') t++;
    return t;
}

static const char *const help_text =
    "/me Text         - Handlung (\"* Name Text\")\n"
    "/shrug [Text]    - Schulterzucken voran\n"
    "/spoiler Text    - als Spoiler (verdeckt)\n"
    "/plain Text      - ohne Formatierung senden\n"
    "/html Text       - Text ist HTML\n"
    "/nick Name       - eigener Anzeigename\n"
    "/myroomnick Name - Name nur in diesem Raum\n"
    "/topic Text      - Thema des Raums\n"
    "/roomname Name   - Name des Raums\n"
    "/invite @nutzer  - einladen\n"
    "/join #raum:server - Raum betreten\n"
    "/knock #raum:server - um Einlass bitten\n"
    "/upgraderoom [Version] - neue Raumversion (Admins)\n"
    "/part            - Raum verlassen\n"
    "/ignore @nutzer, /unignore @nutzer\n"
    "/whois @nutzer   - wer ist das?\n"
    "/op @nutzer [Stufe], /deop @nutzer\n"
    "/kick @nutzer [Grund], /ban @nutzer [Grund], /unban @nutzer\n"
    "/jumptodate TT.MM.JJJJ - zu einem Datum springen\n"
    "//Text           - Text, der mit / beginnt";

static void jump_to_date(ChatWin *w, const char *date);

/* 1 = a command (done or refused), 0 = send the text as it is */
static int chat_command(ChatWin *w, const char *t)
{
    char cmd[24];
    const char *arg;
    char *u = NULL, *v = NULL;
    size_t k = strcspn(t + 1, " ");
    if (t[0] != '/' || t[1] == '/' || t[1] == ' ' || !t[1]) return 0;
    if (k >= sizeof cmd) k = sizeof cmd - 1;
    memcpy(cmd, t + 1, k);
    cmd[k] = 0;
    for (k = 0; cmd[k]; k++) if (cmd[k] >= 'A' && cmd[k] <= 'Z') cmd[k] += 32;
    arg = t + 1 + strlen(cmd);
    while (*arg == ' ') arg++;
    if (!strcmp(cmd, "me") || !strcmp(cmd, "shrug") || !strcmp(cmd, "spoiler") || !strcmp(cmd, "plain")
        || !strcmp(cmd, "html")) {
        if (!*arg && strcmp(cmd, "shrug")) { chat_say(w, "Bitte noch einen Text dazu."); return 1; }
        v = amx_from_local((char *)arg);
        if (!strcmp(cmd, "me")) send_out(w, "m.emote", v, NULL, NULL);
        else if (!strcmp(cmd, "plain")) send_out(w, NULL, v, NULL, NULL);
        else if (!strcmp(cmd, "shrug")) {
            char *b = malloc(strlen(v) + 24);
            if (b) { sprintf(b, "\xc2\xaf\\_(\xe3\x83\x84)_/\xc2\xaf%s%s", *v ? " " : "", v); send_out(w, NULL, b, NULL, NULL); free(b); }
        } else if (!strcmp(cmd, "spoiler")) {
            char *h = malloc(strlen(v) + 40);
            if (h) { sprintf(h, "<span data-mx-spoiler>%s</span>", v); send_out(w, NULL, v, h, NULL); free(h); }
        } else {                                     /* /html: the plain body without the tags */
            char *b = strdup(v), *q = b, *r;
            int in = 0;
            if (b) {
                for (r = v; *r; r++) { if (*r == '<') in = 1; else if (*r == '>') in = 0; else if (!in) *q++ = *r; }
                *q = 0;
                send_out(w, NULL, b, v, NULL);
                free(b);
            }
        }
        free(v);
    } else if (!strcmp(cmd, "nick") || !strcmp(cmd, "myroomnick") || !strcmp(cmd, "topic") || !strcmp(cmd, "roomname")) {
        if (!*arg && strcmp(cmd, "topic")) { chat_say(w, "Bitte noch einen Namen dazu."); return 1; }
        v = amx_from_local((char *)arg);
        if (!strcmp(cmd, "nick")) amx_async_set_profile(A, "name", v, TAG_CMD);
        else if (!strcmp(cmd, "myroomnick")) amx_async_room_nick(A, w->room_id, v, TAG_CMD);
        else amx_async_room_set(A, w->room_id, !strcmp(cmd, "topic") ? "topic" : "name", v, TAG_CMD);
        chat_say(w, "Bitte warten ...");
        free(v);
    } else if (!strcmp(cmd, "invite") || !strcmp(cmd, "ignore") || !strcmp(cmd, "unignore") || !strcmp(cmd, "whois")
               || !strcmp(cmd, "op") || !strcmp(cmd, "deop") || !strcmp(cmd, "kick") || !strcmp(cmd, "remove")
               || !strcmp(cmd, "ban") || !strcmp(cmd, "unban")) {
        if (!(u = full_user(arg))) { chat_say(w, "Bitte noch einen Nutzer dazu (@name:server)."); return 1; }
        v = amx_from_local((char *)after_word(arg));
        if (!strcmp(cmd, "invite")) amx_async_invite(A, w->room_id, u, TAG_CMD);
        else if (!strcmp(cmd, "ignore") || !strcmp(cmd, "unignore")) amx_async_ignore(A, u, cmd[0] == 'i', TAG_CMD);
        else if (!strcmp(cmd, "whois")) amx_async_profile_of(A, u, TAG_WHOIS);
        else if (!strcmp(cmd, "op")) amx_async_moderate(A, w->room_id, u, "power", NULL, *v ? atoi(v) : 50, TAG_CMD);
        else if (!strcmp(cmd, "deop")) amx_async_moderate(A, w->room_id, u, "power", NULL, 0, TAG_CMD);
        else if (!strcmp(cmd, "kick") || !strcmp(cmd, "remove")) amx_async_moderate(A, w->room_id, u, "kick", v, 0, TAG_CMD);
        else amx_async_moderate(A, w->room_id, u, cmd, v, 0, TAG_CMD);
        if (strcmp(cmd, "whois")) chat_say(w, "Bitte warten ...");
        free(u);
        free(v);
    } else if (!strcmp(cmd, "join")) {
        if (!*arg) { chat_say(w, "Bitte noch einen Raum dazu (#name:server)."); return 1; }
        amx_async_join(A, arg, TAG_JOIN);
    } else if (!strcmp(cmd, "upgraderoom")) {
        if (MUI_Request(app, w->win, 0, (STRPTR)"Amatrix", (STRPTR)"_Upgrade|*_Abbrechen",
                        (STRPTR)"Den Raum auf eine neue Raumversion bringen?\nDer Server legt dafür einen neuen Raum an;\nalle müssen dorthin wechseln.", 0) == 1) {
            amx_async_upgrade(A, w->room_id, *arg ? arg : NULL, TAG_CMD);
            chat_say(w, "Upgrade ...");
        }
    } else if (!strcmp(cmd, "knock")) {
        if (!*arg) { chat_say(w, "Bitte noch einen Raum dazu (#name:server)."); return 1; }
        amx_async_knock(A, arg, NULL, TAG_CMD);
        chat_say(w, "Klopfe an ...");
    } else if (!strcmp(cmd, "part") || !strcmp(cmd, "leave")) {
        if (!w->thread_root) amx_async_leave(A, w->room_id, TAG_LEAVE);
    } else if (!strcmp(cmd, "jumptodate")) {
        jump_to_date(w, arg);
    } else if (!strcmp(cmd, "help")) {
        MUI_Request(app, w->win, 0, (STRPTR)"Amatrix - Befehle", (STRPTR)"*_OK", (STRPTR)"%s", (ULONG)help_text);
    } else {
        char b[100];
        snprintf(b, sizeof b, "Unbekannter Befehl /%s - /help zeigt alle.", cmd);
        chat_say(w, b);
        return 1;                                  /* the text stays in the input */
    }
    set(w->input, MUIA_String_Contents, "");
    return 1;
}

static void chat_send(ChatWin *w)
{
    char *text = (char *)xget(w->input, MUIA_String_Contents), *u;
    if (!text || !*text) return;
    if (chat_command(w, text)) { set(w->win, MUIA_Window_ActiveObject, w->input); return; }
    if (!w->thread_root && draft_of(w->room_id)) {
        set_draft(w->room_id, NULL);
        save_prefs();
        DoMethod(lst_rooms, MUIM_List_Redraw, MUIV_List_Redraw_All);
    }
    u = amx_from_local(text[0] == '/' && text[1] == '/' ? text + 1 : text);
    if (u && w->quote) {                          /* "> quote" above it: markdown makes it a quote */
        char *q = malloc(strlen(w->quote) * 2 + strlen(u) + 8), *d = q;
        const char *s2;
        if (q) {
            *d++ = '>'; *d++ = ' ';
            for (s2 = w->quote; *s2; s2++) { *d++ = *s2; if (*s2 == '\n') { *d++ = '>'; *d++ = ' '; } }
            *d++ = '\n'; *d++ = '\n';
            strcpy(d, u);
            free(u);
            u = q;
        }
    }
    {
        char *body = u ? expand_shortcodes(u) : NULL, *html = body ? md_to_html(body) : NULL, *ids = NULL;
        const char *names[16], *idl[16];
        int i, n = 0;
        size_t len = 1;
        for (i = 0; body && i < w->n_ment; i++)
            if (strstr(body, w->ment_name[i])) { names[n] = w->ment_name[i]; idl[n] = w->ment_id[i]; len += strlen(idl[n]) + 1; n++; }
        if (n) {
            char *base = html ? html : md_escape(body), *withm = md_mentions(base, names, idl, n);
            if (base != html) free(base);
            free(html);
            html = withm;
            if ((ids = malloc(len)) != NULL) {
                ids[0] = 0;
                for (i = 0; i < n; i++) { if (i) strcat(ids, "\n"); strcat(ids, idl[i]); }
            }
        }
        send_out(w, NULL, body ? body : u, html, ids);
        free(ids);
        free(html);
        free(body);
    }
    clear_mentions(w);
    free(u);
    set(w->input, MUIA_String_Contents, "");
    set(w->win, MUIA_Window_ActiveObject, w->input);
}

/* ---- sending files: toolbar (ASL, several at once) or icons dropped on the window */

static char send_dir[256];

static void send_file(ChatWin *w, const char *path)
{
    char *name = amx_from_local((char *)FilePart((STRPTR)path));
    amx_async_send_file(A, w->room_id, path, name, w->thread_root, TAG_FILE);
    free(name);
    if (w->thread_root) {
        char key[600];
        snprintf(key, sizeof key, "%s\t%s", w->room_id, w->thread_root);
        fifo_push(fifo_send, &n_send, 64, key);
    } else
        fifo_push(fifo_send, &n_send, 64, w->room_id);
    w->uploading++;
    w->err[0] = 0;
    update_status(w);
    amx_log("sending file %s", path);
}

/* a lock + name (Workbench argument) as a path; 0 for a drawer/disk */
static int wbarg_path(BPTR lock, const char *name, char *path, size_t n)
{
    if (!name || !*name) return 0;
    if (!lock || !NameFromLock(lock, (STRPTR)path, n)) path[0] = 0;
    return AddPart((STRPTR)path, (STRPTR)name, n) != 0;
}

static void pick_files(ChatWin *w)
{
    struct Library *AslBase = OpenLibrary("asl.library", 38);
    struct FileRequester *fr;
    char path[512];
    if (!AslBase) return;
    set(app, MUIA_Application_Sleep, TRUE);
    fr = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
             ASLFR_Window, (ULONG)xget(w->win, MUIA_Window_Window),
             ASLFR_TitleText, (ULONG)"Bild oder Datei senden",
             ASLFR_InitialDrawer, (ULONG)(send_dir[0] ? send_dir : "RAM:"),
             ASLFR_DoMultiSelect, TRUE, ASLFR_RejectIcons, TRUE, TAG_DONE);
    if (fr && AslRequest(fr, NULL)) {
        snprintf(send_dir, sizeof send_dir, "%s", fr->fr_Drawer);
        if (fr->fr_NumArgs > 0) {
            LONG i;
            for (i = 0; i < fr->fr_NumArgs; i++) {
                snprintf(path, sizeof path, "%s", fr->fr_Drawer);
                if (AddPart((STRPTR)path, fr->fr_ArgList[i].wa_Name, sizeof path) && fr->fr_ArgList[i].wa_Name[0])
                    send_file(w, path);
            }
        } else if (fr->fr_File[0]) {
            snprintf(path, sizeof path, "%s", fr->fr_Drawer);
            if (AddPart((STRPTR)path, (STRPTR)fr->fr_File, sizeof path)) send_file(w, path);
        }
    }
    if (fr) FreeAslRequest(fr);
    CloseLibrary(AslBase);
    set(app, MUIA_Application_Sleep, FALSE);
}

HOOKPROTONH(chat_attach_func, LONG, Object *obj, ULONG *param)
{
    (void)obj;
    pick_files((ChatWin *)param[0]);
    return 0;
}
MakeStaticHook(chat_attach_hook, chat_attach_func);

HOOKPROTONH(chat_drop_func, LONG, Object *obj, ULONG *param)
{
    ChatWin *w = (ChatWin *)param[0];
    struct AppMessage *am = (struct AppMessage *)param[1];
    char path[512];
    LONG i;
    (void)obj;
    if (!am) return 0;
    for (i = 0; i < am->am_NumArgs; i++)
        if (wbarg_path(am->am_ArgList[i].wa_Lock, (char *)am->am_ArgList[i].wa_Name, path, sizeof path))
            send_file(w, path);
    return 0;
}
MakeStaticHook(chat_drop_hook, chat_drop_func);

/* cursor up in an empty input: edit our newest message */
static void start_edit(ChatWin *w, ChatMsg *m);
static void edit_newest_own(ChatWin *w)
{
    char *t = (char *)xget(w->input, MUIA_String_Contents);
    ChatMsg *best = NULL;
    int i;
    if ((t && *t) || w->edit_eid) return;
    for (i = 0; i < w->n_msgs; i++) {
        ChatMsg *m = &w->msgs[i];
        if (!m->own || m->redacted || !m->body || (m->msgtype && strcmp(m->msgtype, "m.text") && strcmp(m->msgtype, "m.emote")))
            continue;
        if (!best || m->ts > best->ts) best = m;
    }
    if (best) start_edit(w, best);
}

HOOKPROTONH(uparrow_func, LONG, Object *obj, ULONG *param)
{
    (void)obj;
    edit_newest_own((ChatWin *)param[0]);
    return 0;
}
MakeStaticHook(uparrow_hook, uparrow_func);

/* ---- completing @names and :emojis: (buttons above the input) */

static const struct { const char *code, *utf8; } shortcodes[] = {
    { "thumbsup", "\xf0\x9f\x91\x8d" }, { "thumbsdown", "\xf0\x9f\x91\x8e" }, { "heart", "\xe2\x9d\xa4\xef\xb8\x8f" },
    { "smile", "\xf0\x9f\x98\x8a" }, { "joy", "\xf0\x9f\x98\x82" }, { "laughing", "\xf0\x9f\x98\x86" },
    { "wink", "\xf0\x9f\x98\x89" }, { "grin", "\xf0\x9f\x98\x81" }, { "confused", "\xf0\x9f\x98\x95" },
    { "cry", "\xf0\x9f\x98\xa2" }, { "eyes", "\xf0\x9f\x91\x80" }, { "tada", "\xf0\x9f\x8e\x89" },
    { "rocket", "\xf0\x9f\x9a\x80" }, { "kiss", "\xf0\x9f\x98\x98" }, { "heart_eyes", "\xf0\x9f\x98\x8d" },
    { "thinking", "\xf0\x9f\xa4\x94" }, { "clap", "\xf0\x9f\x91\x8f" }, { "fire", "\xf0\x9f\x94\xa5" },
    { "ok_hand", "\xf0\x9f\x91\x8c" }, { "wave", "\xf0\x9f\x91\x8b" }, { "pray", "\xf0\x9f\x99\x8f" },
    { "sunglasses", "\xf0\x9f\x98\x8e" }, { "check", "\xe2\x9c\x85" }, { "star", "\xe2\xad\x90" },
};

/* :code: in a UTF-8 text replaced by the emoji (malloc'd) */
static char *expand_shortcodes(const char *t)
{
    size_t n = strlen(t) * 2 + 16, o = 0, i;
    char *out = malloc(n);
    const char *p = t;
    if (!out) return NULL;
    while (*p) {
        int hit = -1;
        if (*p == ':')
            for (i = 0; i < sizeof shortcodes / sizeof shortcodes[0]; i++) {
                size_t k = strlen(shortcodes[i].code);
                if (!strncmp(p + 1, shortcodes[i].code, k) && p[1 + k] == ':') { hit = (int)i; break; }
            }
        if (hit >= 0) {
            size_t k = strlen(shortcodes[hit].utf8);
            if (o + k + 1 >= n) break;
            memcpy(out + o, shortcodes[hit].utf8, k);
            o += k;
            p += strlen(shortcodes[hit].code) + 2;
        } else {
            if (o + 2 >= n) break;
            out[o++] = *p++;
        }
    }
    out[o] = 0;
    return out;
}

static int prefix_ci(const char *s, const char *pre)
{
    for (; *pre; s++, pre++) {
        unsigned char a = (unsigned char)*s, b = (unsigned char)*pre;
        if (!a) return 0;
        if ((a >= 'A' && a <= 'Z') || (a >= 0xc0 && a <= 0xde)) a += 32;
        if ((b >= 'A' && b <= 'Z') || (b >= 0xc0 && b <= 0xde)) b += 32;
        if (a != b) return 0;
    }
    return 1;
}

static void clear_suggestions(ChatWin *w)
{
    int k;
    for (k = 0; k < 5; k++) { free(w->stext[k]); free(w->sid[k]); w->stext[k] = w->sid[k] = NULL; }
    w->n_sugg = 0;
    set(w->sugg, MUIA_ShowMe, FALSE);
}

static void update_suggestions(ChatWin *w)
{
    char *t = (char *)xget(w->input, MUIA_String_Contents), *tok;
    size_t len;
    int k, n = 0;
    clear_suggestions(w);
    if (!t || !*t) return;
    tok = strrchr(t, ' ');
    tok = tok ? tok + 1 : t;
    len = strlen(tok);
    if (tok[0] == '@' && len >= 2) {
        if (!w->mem_asked) { w->mem_asked = 1; amx_async_members(A, w->room_id, TAG_MEMB2); return; }
        for (k = 0; k < w->n_mem && n < 5; k++) {
            char *nm = local(w->mem[k].name);
            const char *lp = w->mem[k].user_id + 1;
            if (strcmp(w->mem[k].membership, "join") || (my_user && !strcmp(w->mem[k].user_id, my_user))) { free(nm); continue; }
            if (nm && (prefix_ci(nm, tok + 1) || prefix_ci(lp, tok + 1))) {
                w->stext[n] = nm;
                w->sid[n] = strdup(w->mem[k].user_id);
                n++;
            } else free(nm);
        }
    } else if (tok[0] == ':' && len >= 3 && !strchr(tok + 1, ':')) {
        for (k = 0; k < (int)(sizeof shortcodes / sizeof shortcodes[0]) && n < 5; k++)
            if (prefix_ci(shortcodes[k].code, tok + 1)) {
                char *c = malloc(strlen(shortcodes[k].code) + 3);
                if (!c) break;
                sprintf(c, ":%s:", shortcodes[k].code);
                w->stext[n++] = c;
            }
    }
    w->n_sugg = n;
    for (k = 0; k < 5; k++) {
        set(w->sbtn[k], MUIA_ShowMe, k < n);
        if (k < n) set(w->sbtn[k], MUIA_Text_Contents, w->stext[k]);
    }
    if (n) set(w->sugg, MUIA_ShowMe, TRUE);
}

/* a suggestion chosen: it replaces the word being typed */
static void complete(ChatWin *w, int k)
{
    char *t = (char *)xget(w->input, MUIA_String_Contents), *tok, *nt;
    char *text, *id;
    if (k < 0 || k >= w->n_sugg || !t) return;
    text = strdup(w->stext[k]);
    id = w->sid[k] ? strdup(w->sid[k]) : NULL;
    tok = strrchr(t, ' ');
    tok = tok ? tok + 1 : t;
    if ((nt = malloc((tok - t) + strlen(text) + 2)) != NULL) {
        memcpy(nt, t, tok - t);
        sprintf(nt + (tok - t), "%s ", text);
        if (id && w->n_ment < 16) {
            w->ment_name[w->n_ment] = amx_from_local(text);
            w->ment_id[w->n_ment] = id;
            w->n_ment++;
            id = NULL;
        }
        set(w->input, MUIA_String_Contents, nt);
        set(w->input, MUIA_String_BufferPos, strlen(nt));
        free(nt);
    }
    free(text);
    free(id);
    clear_suggestions(w);
    set(w->win, MUIA_Window_ActiveObject, w->input);
}

HOOKPROTONH(sugg_func, LONG, Object *obj, ULONG *param)
{
    (void)obj;
    complete((ChatWin *)param[0], (int)param[1]);
    return 0;
}
MakeStaticHook(sugg_hook, sugg_func);

static void got_members2(const AmxMsg *m)
{
    ChatWin *w = m->result ? chat_by_room(m->result) : NULL;
    int i;
    if (!w || m->status) return;
    amx_free_members(w->mem, w->n_mem);
    w->mem = m->n ? calloc(m->n, sizeof *w->mem) : NULL;
    w->n_mem = w->mem ? m->n : 0;
    for (i = 0; i < w->n_mem; i++) {
        w->mem[i] = m->members[i];
        w->mem[i].user_id = xstrdup(m->members[i].user_id);
        w->mem[i].name = xstrdup(m->members[i].name);
        w->mem[i].avatar_url = xstrdup(m->members[i].avatar_url);
        w->mem[i].membership = xstrdup(m->members[i].membership);
    }
    update_suggestions(w);
}

static void clear_mentions(ChatWin *w)
{
    int i;
    for (i = 0; i < w->n_ment; i++) { free(w->ment_name[i]); free(w->ment_id[i]); }
    w->n_ment = 0;
}

/* the input changed: "typing" to the server (renewed every 20 s), or stopped */
static void typing_stop(ChatWin *w)
{
    if (!w->typing_on) return;
    w->typing_on = 0;
    amx_async_typing(A, w->room_id, 0);
}

HOOKPROTONH(chat_typing_func, LONG, Object *obj, ULONG *param)
{
    ChatWin *w = (ChatWin *)param[0];
    char *t = (char *)xget(w->input, MUIA_String_Contents);
    time_t now = time(NULL);
    (void)obj;
    update_suggestions(w);
    if (!t || !*t || w->edit_eid) typing_stop(w);
    else if (opt_typing && (!w->typing_on || now - w->typing_sent >= 20)) {
        w->typing_on = 1;
        w->typing_sent = now;
        amx_async_typing(A, w->room_id, 30000);
    }
    return 0;
}
MakeStaticHook(chat_typing_hook, chat_typing_func);

HOOKPROTONH(chat_send_func, LONG, Object *obj, ULONG *param)
{
    (void)obj;
    chat_send((ChatWin *)param[0]);
    return 0;
}
MakeStaticHook(chat_send_hook, chat_send_func);
static struct Hook chat_reply_hook, chat_edit_hook, chat_del_hook, chat_modex_hook, chat_active_hook;
static void free_msgs(struct ChatWin *w);

HOOKPROTONH(chat_older_func, LONG, Object *obj, ULONG *param)
{
    ChatWin *w = (ChatWin *)param[0];
    (void)obj;
    if (!w->loading && !w->at_start) {
        w->loading = 1;
        w->loading_older = 1;
        w->err[0] = 0;
        set(w->older, MUIA_Disabled, TRUE);
        if (w->thread_root) amx_async_load_thread(A, w->room_id, w->thread_root, OLDER_PAGE, 0, TAG_THREAD);
        else amx_async_load_history(A, w->room_id, OLDER_PAGE, 0, TAG_HIST);
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

/* ---- the room window ("Mitglieder und Raum-Einstellungen"): settings,
 * members with their role, kick/ban/unban, roles */

static const char *join_rules[] = { "invite", "public", "knock", NULL };
static const char *join_names[] = { "Nur mit Einladung", "Öffentlich", "Auf Anfrage (anklopfen)", NULL };
static const char *hist_values[] = { "shared", "invited", "joined", "world_readable", NULL };
static const char *hist_names[] = { "Mitglieder: ganzer Verlauf", "Mitglieder: ab der Einladung",
                                    "Mitglieder: ab dem Beitritt", "Jeder (auch ohne Beitritt)", NULL };
static const char *role_names[] = { "Mitglied", "Moderator", "Admin", NULL };
static const char *notify_names[] = { "Standard", "Alle Nachrichten", "Nur Erwähnungen und Stichwörter", "Aus (stumm)", NULL };
static Object *cyc_inotify;
static const int role_levels[] = { 0, 50, 100 };

static char *info_room;
static AmxRoomSettings info_set;
static int info_have;
static AmxMember *info_mem;
static int n_info_mem;
static int info_busy;
static char info_status[300];

static int idx_of(const char *const *list, const char *v)
{
    int i;
    for (i = 0; v && list[i]; i++) if (!strcmp(list[i], v)) return i;
    return 0;
}

static const char *role_of(int power)
{
    return power >= AMX_POWER_CREATOR ? "Gründer" : power >= 100 ? "Admin" : power >= 50 ? "Moderator"
         : power > 0 ? "Mitglied+" : "Mitglied";
}

HOOKPROTONHNO(mem_disp_func, LONG, struct NList_DisplayMessage *m)
{
    static char b0[200], b1[40], b2[200];
    const AmxMember *p = m->entry;
    char *t;
    if (!p) { m->strings[0] = "Name"; m->strings[1] = "Rolle"; m->strings[2] = "Matrix-ID"; return 0; }
    t = local(p->name);
    snprintf(b0, sizeof b0, "%s%s", strcmp(p->membership, "join") ? "\33i" : "", t ? t : "");
    free(t);
    snprintf(b1, sizeof b1, "%s%s", role_of(p->power),
             !strcmp(p->membership, "invite") ? " (eingeladen)" : !strcmp(p->membership, "ban") ? " (gesperrt)"
             : !strcmp(p->membership, "knock") ? " (klopft an)" : "");
    t = local(p->user_id);
    snprintf(b2, sizeof b2, "%s", t ? t : "");
    free(t);
    m->strings[0] = b0; m->strings[1] = b1; m->strings[2] = b2;
    return 0;
}
MakeStaticHook(mem_disp_hook, mem_disp_func);

static void info_set_status(const char *t)
{
    snprintf(info_status, sizeof info_status, "%s", t ? t : "");
    set(txt_istatus, MUIA_Text_Contents, info_status);
    set(txt_istatus, MUIA_ShortHelp, info_status);
}

static AmxMember *info_selected(void)
{
    AmxMember *p = NULL;
    DoMethod(lst_mem, MUIM_NList_GetEntry, MUIV_NList_GetEntry_Active, &p);
    return p;
}

/* what we may do: greyed out by our power level */
static void info_enable(void)
{
    const AmxRoomSettings *st = &info_set;
    AmxMember *p = info_selected();
    int me = info_have ? st->my_power : -1, above = p && me > p->power;
    set(str_iname, MUIA_Disabled, !info_have || me < st->level_name);
    set(str_itopic, MUIA_Disabled, !info_have || me < st->level_topic);
    set(cyc_ijoin, MUIA_Disabled, !info_have || me < st->level_join_rules);
    set(cyc_ihist, MUIA_Disabled, !info_have || me < st->level_history);
    set(chk_ienc, MUIA_Disabled, !info_have || st->encrypted || me < st->level_encryption);
    set(b_ipic, MUIA_Disabled, !info_have || me < st->level_avatar);
    set(b_isave, MUIA_Disabled, !info_have);
    set(b_iinvite, MUIA_Disabled, !info_have || me < st->level_invite);
    set(b_ikick, MUIA_Disabled, !info_have || !p || strcmp(p->membership, "ban") == 0 || !above || me < st->level_kick
                               || (my_user && !strcmp(p->user_id, my_user)));
    set(b_iban, MUIA_Disabled, !info_have || !p || !strcmp(p->membership, "ban") || !above || me < st->level_ban);
    set(b_iunban, MUIA_Disabled, !info_have || !p || strcmp(p->membership, "ban") || me < st->level_ban);
    set(b_iadmit, MUIA_Disabled, !info_have || !p || strcmp(p->membership, "knock") || me < st->level_invite);
    set(cyc_irole, MUIA_Disabled, !info_have || !p || !above || me < st->level_power);
    set(b_irole, MUIA_Disabled, !info_have || !p || !above || me < st->level_power);
    if (p) set(cyc_irole, MUIA_Cycle_Active, p->power >= 100 ? 2 : p->power >= 50 ? 1 : 0);
}

HOOKPROTONH(mem_active_func, LONG, Object *obj, ULONG *param)
{
    (void)obj; (void)param;
    info_enable();
    return 0;
}
MakeStaticHook(mem_active_hook, mem_active_func);

static void info_reload(int settings, int members)
{
    if (!info_room) return;
    if (settings) amx_async_room_settings(A, info_room, TAG_RSETTINGS);
    if (members) amx_async_members(A, info_room, TAG_MEMBERS);
}

static int nnotify_set;          /* setting the cycle ourselves: no command */

HOOKPROTONHNO(inotify_func, LONG, APTR unused)
{
    (void)unused;
    if (!nnotify_set && info_room)
        amx_async_room_notify(A, info_room, (int)xget(cyc_inotify, MUIA_Cycle_Active), TAG_ROOMSET), info_busy++,
            info_set_status("Speichere ...");
    return 0;
}
MakeStaticHook(inotify_hook, inotify_func);

static void open_info(const char *room_id)
{
    static char title[200];
    const Room *r = room_by_id(room_id);
    free(info_room);
    info_room = strdup(room_id);
    amx_free_room_settings(&info_set);
    info_have = 0;
    DoMethod(lst_mem, MUIM_NList_Clear);
    amx_free_members(info_mem, n_info_mem);
    info_mem = NULL;
    n_info_mem = 0;
    snprintf(title, sizeof title, "Raum: %s", r && r->name ? r->name : room_id);
    nnotify_set = 1;
    set(cyc_inotify, MUIA_Cycle_Active, r ? r->notify : 0);
    nnotify_set = 0;
    set(win_info, MUIA_Window_Title, title);
    set(str_iname, MUIA_String_Contents, "");
    set(str_itopic, MUIA_String_Contents, "");
    info_set_status("Lade ...");
    info_enable();
    set(win_info, MUIA_Window_Open, TRUE);
    info_reload(1, 1);
}

static void info_got_settings(const AmxMsg *m)
{
    char *t;
    if (!info_room || !m->result || strcmp(m->result, info_room)) return;
    if (m->status || !m->settings) { info_set_status(m->error); return; }
    amx_free_room_settings(&info_set);
    info_set = *m->settings;                      /* deep copy: the message frees its own */
    info_set.name = xstrdup(m->settings->name);
    info_set.topic = xstrdup(m->settings->topic);
    info_set.avatar_url = xstrdup(m->settings->avatar_url);
    info_set.join_rule = xstrdup(m->settings->join_rule);
    info_set.history = xstrdup(m->settings->history);
    info_have = 1;
    t = local(info_set.name ? info_set.name : ""); set(str_iname, MUIA_String_Contents, t); free(t);
    t = local(info_set.topic ? info_set.topic : ""); set(str_itopic, MUIA_String_Contents, t); free(t);
    set(cyc_ijoin, MUIA_Cycle_Active, idx_of(join_rules, info_set.join_rule));
    set(cyc_ihist, MUIA_Cycle_Active, idx_of(hist_values, info_set.history));
    set(chk_ienc, MUIA_Selected, info_set.encrypted);
    if (!info_busy) info_set_status(info_set.my_power >= 50 ? "" : "Nur Admins und Moderatoren dürfen hier etwas ändern.");
    info_enable();
}

static void info_got_members(const AmxMsg *m)
{
    int i;
    if (!info_room || !m->result || strcmp(m->result, info_room)) return;
    if (m->status) { info_set_status(m->error); return; }
    set(lst_mem, MUIA_NList_Quiet, TRUE);
    DoMethod(lst_mem, MUIM_NList_Clear);
    amx_free_members(info_mem, n_info_mem);
    info_mem = m->n ? calloc(m->n, sizeof *info_mem) : NULL;
    n_info_mem = info_mem ? m->n : 0;
    for (i = 0; i < n_info_mem; i++) {
        info_mem[i].user_id = xstrdup(m->members[i].user_id);
        info_mem[i].name = xstrdup(m->members[i].name);
        info_mem[i].avatar_url = xstrdup(m->members[i].avatar_url);
        info_mem[i].membership = xstrdup(m->members[i].membership);
        info_mem[i].power = m->members[i].power;
        DoMethod(lst_mem, MUIM_NList_InsertSingle, &info_mem[i], MUIV_NList_Insert_Bottom);
    }
    set(lst_mem, MUIA_NList_Quiet, FALSE);
    info_enable();
}

/* the changed settings to the server, one command each */
static void info_save(void)
{
    char *t;
    int k;
    if (!info_room || !info_have) return;
    t = amx_from_local((char *)xget(str_iname, MUIA_String_Contents));
    if (t && strcmp(t, info_set.name ? info_set.name : "")) { amx_async_room_set(A, info_room, "name", t, TAG_ROOMSET); info_busy++; }
    free(t);
    t = amx_from_local((char *)xget(str_itopic, MUIA_String_Contents));
    if (t && strcmp(t, info_set.topic ? info_set.topic : "")) { amx_async_room_set(A, info_room, "topic", t, TAG_ROOMSET); info_busy++; }
    free(t);
    k = (int)xget(cyc_ijoin, MUIA_Cycle_Active);
    if (strcmp(join_rules[k], info_set.join_rule ? info_set.join_rule : "")) {
        amx_async_room_set(A, info_room, "join_rule", join_rules[k], TAG_ROOMSET); info_busy++;
    }
    k = (int)xget(cyc_ihist, MUIA_Cycle_Active);
    if (strcmp(hist_values[k], info_set.history ? info_set.history : "")) {
        amx_async_room_set(A, info_room, "history", hist_values[k], TAG_ROOMSET); info_busy++;
    }
    if (xget(chk_ienc, MUIA_Selected) && !info_set.encrypted) {
        if (MUI_Request(app, win_info, 0, (STRPTR)"Amatrix", (STRPTR)"_Einschalten|*_Abbrechen",
                        (STRPTR)"Verschlüsselung lässt sich danach nicht mehr ausschalten.\nEinschalten?", 0) == 1) {
            amx_async_room_set(A, info_room, "encryption", "", TAG_ROOMSET);
            info_busy++;
        } else set(chk_ienc, MUIA_Selected, FALSE);
    }
    if (info_busy) { info_set_status("Speichere ..."); info_reload(1, 0); }
    else info_set_status("Nichts geändert.");
}

static void info_picture(void)
{
    struct Library *AslBase = OpenLibrary("asl.library", 38);
    struct FileRequester *fr;
    char path[512];
    if (!AslBase || !info_room) { if (AslBase) CloseLibrary(AslBase); return; }
    fr = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
             ASLFR_Window, (ULONG)xget(win_info, MUIA_Window_Window),
             ASLFR_TitleText, (ULONG)"Bild für den Raum (PNG, JPEG, GIF)",
             ASLFR_InitialDrawer, (ULONG)(send_dir[0] ? send_dir : "RAM:"), ASLFR_RejectIcons, TRUE, TAG_DONE);
    if (fr && AslRequest(fr, NULL) && fr->fr_File[0]) {
        snprintf(path, sizeof path, "%s", fr->fr_Drawer);
        if (AddPart((STRPTR)path, (STRPTR)fr->fr_File, sizeof path)) {
            amx_async_room_set(A, info_room, "avatar", path, TAG_ROOMSET);
            info_busy++;
            info_set_status("Lade das Bild hoch ...");
            info_reload(1, 0);
        }
    }
    if (fr) FreeAslRequest(fr);
    CloseLibrary(AslBase);
}

static void info_moderate(const char *action)
{
    AmxMember *p = info_selected();
    char *name;
    int level = 0;
    if (!p || !info_room) return;
    name = local(p->name);
    if (!strcmp(action, "power")) level = role_levels[xget(cyc_irole, MUIA_Cycle_Active)];
    else if (strcmp(action, "unban")
             && MUI_Request(app, win_info, 0, (STRPTR)"Amatrix", (STRPTR)"_Ja|*_Nein",
                            !strcmp(action, "kick") ? (STRPTR)"%s aus dem Raum werfen?"
                                                    : (STRPTR)"%s sperren? (Kann nicht wieder beitreten.)",
                            (ULONG)(name ? name : "")) != 1) {
        free(name);
        return;
    }
    free(name);
    amx_log("moderate %s %s (level %d)", action, p->user_id, level);
    amx_async_moderate(A, info_room, p->user_id, action, NULL, level, TAG_MODERATE);
    info_busy++;
    info_set_status("Bitte warten ...");
    info_reload(0, 1);
}

/* a ROOM_SET / MODERATE result */
static void info_done(const AmxMsg *m)
{
    amx_log("room change done: %d %s", m->status, m->error ? m->error : "");
    if (info_busy) info_busy--;
    if (m->status) {
        char b[300];
        snprintf(b, sizeof b, "\33bNicht geklappt:\33n %.240s", m->error ? m->error : "");
        info_set_status(b);
    } else if (!info_busy) info_set_status("Gespeichert.");
}

HOOKPROTONH(chat_info_func, LONG, Object *obj, ULONG *param)
{
    (void)obj;
    open_info(((ChatWin *)param[0])->room_id);
    return 0;
}
MakeStaticHook(chat_info_hook, chat_info_func);

/* ---- our own profile: name and picture */

static char *prof_name, *prof_av;
static Alpha prof_img;
static char prof_status[300], prof_imgspec[32];
static int prof_busy;

static void prof_set_status(const char *t)
{
    snprintf(prof_status, sizeof prof_status, "%s", t ? t : "");
    set(txt_pstatus, MUIA_Text_Contents, prof_status);
    set(txt_pstatus, MUIA_ShortHelp, prof_status);
}

static void open_profile(void)
{
    static char id[200];
    char *t = local(my_user ? my_user : "");
    snprintf(id, sizeof id, "\33b%s", t ? t : "");
    free(t);
    set(txt_pid, MUIA_Text_Contents, id);
    prof_set_status("Lade ...");
    set(win_prof, MUIA_Window_Open, TRUE);
    amx_async_profile(A, TAG_PROFILE);
}

static void prof_got(const AmxMsg *m)
{
    char *t;
    if (m->status) { prof_set_status(m->error); return; }
    free(prof_name); prof_name = xstrdup(m->result);
    free(prof_av); prof_av = xstrdup(m->result2);
    t = local(prof_name ? prof_name : "");
    set(str_pname, MUIA_String_Contents, t);
    free(t);
    if (!prof_busy) prof_set_status(prof_av ? "" : "Noch kein Bild.");
    if (prof_av) amx_async_avatar(A, prof_av, AV, TAG_PROFAV);
    else set(txt_pimg, MUIA_Text_Contents, "");
}

static void prof_got_av(const AmxMsg *m)
{
    if (m->status || !m->pixels || !prof_av || !m->result || strcmp(m->result, prof_av)) return;
    prof_img.ad.width = prof_img.ad.height = AV;
    memcpy(prof_img.px, m->pixels, sizeof prof_img.px);
    snprintf(prof_imgspec, sizeof prof_imgspec, "\33A[%08lx]", (unsigned long)&prof_img.ad);
    set(txt_pimg, MUIA_Text_Contents, "");
    set(txt_pimg, MUIA_Text_Contents, prof_imgspec);
}

static void prof_save(void)
{
    char *t = amx_from_local((char *)xget(str_pname, MUIA_String_Contents));
    if (t && *t && strcmp(t, prof_name ? prof_name : "")) {
        amx_async_set_profile(A, "name", t, TAG_SETPROF);
        prof_busy++;
        prof_set_status("Speichere ...");
        amx_async_profile(A, TAG_PROFILE);
    } else prof_set_status("Nichts geändert.");
    free(t);
}

static void prof_picture(const char *given)
{
    char path[512];
    if (given) snprintf(path, sizeof path, "%s", given);
    else {
        struct Library *AslBase = OpenLibrary("asl.library", 38);
        struct FileRequester *fr;
        int ok = 0;
        if (!AslBase) return;
        fr = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
                 ASLFR_Window, (ULONG)xget(win_prof, MUIA_Window_Window),
                 ASLFR_TitleText, (ULONG)"Profilbild (PNG, JPEG, GIF)",
                 ASLFR_InitialDrawer, (ULONG)(send_dir[0] ? send_dir : "RAM:"), ASLFR_RejectIcons, TRUE, TAG_DONE);
        if (fr && AslRequest(fr, NULL) && fr->fr_File[0]) {
            snprintf(path, sizeof path, "%s", fr->fr_Drawer);
            ok = AddPart((STRPTR)path, (STRPTR)fr->fr_File, sizeof path) != 0;
        }
        if (fr) FreeAslRequest(fr);
        CloseLibrary(AslBase);
        if (!ok) return;
    }
    amx_async_set_profile(A, "avatar", path, TAG_SETPROF);
    prof_busy++;
    prof_set_status("Lade das Bild hoch ...");
    amx_async_profile(A, TAG_PROFILE);
}

static void prof_done(const AmxMsg *m)
{
    if (prof_busy) prof_busy--;
    if (m->status) {
        char b[300];
        snprintf(b, sizeof b, "\33bNicht geklappt:\33n %.240s", m->error ? m->error : "");
        prof_set_status(b);
    } else if (!prof_busy) prof_set_status("Gespeichert.");
}

/* ---- our sessions (devices): list, rename, sign out */

static AmxDevice *sess;
static int n_sess, sess_busy;
static char sess_status[300];

static void sess_set_status(const char *t)
{
    snprintf(sess_status, sizeof sess_status, "%s", t ? t : "");
    set(txt_sstatus, MUIA_Text_Contents, sess_status);
    set(txt_sstatus, MUIA_ShortHelp, sess_status);
}

HOOKPROTONHNO(sess_disp_func, LONG, struct NList_DisplayMessage *m)
{
    static char b0[160], b1[64], b2[40], b3[40];
    const AmxDevice *d = m->entry;
    char *t;
    if (!d) {
        m->strings[0] = "Name"; m->strings[1] = "Sitzung"; m->strings[2] = "Zuletzt aktiv"; m->strings[3] = "Status";
        return 0;
    }
    t = local(d->name ? d->name : "");
    snprintf(b0, sizeof b0, "%s%s", d->current ? "\33b" : "", t ? t : "");
    free(t);
    snprintf(b1, sizeof b1, "%s", d->device_id);
    if (d->last_seen_ts > 0) {
        time_t tt = (time_t)(d->last_seen_ts / 1000);
        struct tm *tm = localtime(&tt);
        if (tm) strftime(b2, sizeof b2, "%d.%m.%Y %H:%M", tm);
        else b2[0] = 0;
    } else snprintf(b2, sizeof b2, "-");
    snprintf(b3, sizeof b3, "%s", d->current ? "diese" : d->verified > 0 ? "verifiziert"
                                 : d->verified == 0 ? "nicht verifiziert" : "ohne Schlüssel");
    m->strings[0] = b0; m->strings[1] = b1; m->strings[2] = b2; m->strings[3] = b3;
    return 0;
}
MakeStaticHook(sess_disp_hook, sess_disp_func);

static AmxDevice *sess_selected(void)
{
    AmxDevice *d = NULL;
    DoMethod(lst_sess, MUIM_NList_GetEntry, MUIV_NList_GetEntry_Active, &d);
    return d;
}

HOOKPROTONH(sess_active_func, LONG, Object *obj, ULONG *param)
{
    AmxDevice *d = sess_selected();
    char *t;
    (void)obj; (void)param;
    t = local(d && d->name ? d->name : "");
    set(str_sname, MUIA_String_Contents, t ? t : "");
    free(t);
    set(b_srename, MUIA_Disabled, !d);
    set(b_sdelete, MUIA_Disabled, !d || d->current);
    return 0;
}
MakeStaticHook(sess_active_hook, sess_active_func);

static void open_sessions(void)
{
    set(win_sess, MUIA_Window_Open, TRUE);
    set(b_srename, MUIA_Disabled, TRUE);
    set(b_sdelete, MUIA_Disabled, TRUE);
    sess_set_status("Lade ...");
    amx_async_devices(A, TAG_DEVICES);
}

static void sess_got(const AmxMsg *m)
{
    int i;
    if (m->status) { sess_set_status(m->error); return; }
    set(lst_sess, MUIA_NList_Quiet, TRUE);
    DoMethod(lst_sess, MUIM_NList_Clear);
    amx_free_devices(sess, n_sess);
    sess = m->n ? calloc(m->n, sizeof *sess) : NULL;
    n_sess = sess ? m->n : 0;
    for (i = 0; i < n_sess; i++) {
        sess[i] = m->devices[i];
        sess[i].device_id = xstrdup(m->devices[i].device_id);
        sess[i].name = xstrdup(m->devices[i].name);
        sess[i].last_seen_ip = xstrdup(m->devices[i].last_seen_ip);
        DoMethod(lst_sess, MUIM_NList_InsertSingle, &sess[i], MUIV_NList_Insert_Bottom);
    }
    set(lst_sess, MUIA_NList_Quiet, FALSE);
    if (!sess_busy) {
        char b[80];
        snprintf(b, sizeof b, "%d Sitzung%s", n_sess, n_sess == 1 ? "" : "en");
        sess_set_status(b);
    }
}

static void sess_rename(void)
{
    AmxDevice *d = sess_selected();
    char *t;
    if (!d) return;
    t = amx_from_local((char *)xget(str_sname, MUIA_String_Contents));
    amx_async_device_rename(A, d->device_id, t ? t : "", TAG_DEVCHANGE);
    free(t);
    sess_busy++;
    sess_set_status("Speichere ...");
    amx_async_devices(A, TAG_DEVICES);
}

static void sess_delete(void)
{
    AmxDevice *d = sess_selected();
    char *pw = (char *)xget(str_spass, MUIA_String_Contents), *name;
    if (!d || d->current) return;
    if (!pw || !*pw) { sess_set_status("Zum Abmelden bitte dein Passwort eingeben."); return; }
    name = local(d->name ? d->name : d->device_id);
    if (MUI_Request(app, win_sess, 0, (STRPTR)"Amatrix", (STRPTR)"_Abmelden|*_Abbrechen",
                    (STRPTR)"Die Sitzung \"%s\" abmelden?\nDort ist man danach nicht mehr angemeldet.",
                    (ULONG)(name ? name : "")) == 1) {
        amx_async_device_delete(A, d->device_id, pw, TAG_DEVCHANGE);
        sess_busy++;
        sess_set_status("Melde ab ...");
        amx_async_devices(A, TAG_DEVICES);
    }
    free(name);
    set(str_spass, MUIA_String_Contents, "");
}

static void sess_done(const AmxMsg *m)
{
    if (sess_busy) sess_busy--;
    if (m->status) {
        char b[300];
        snprintf(b, sizeof b, "\33bNicht geklappt:\33n %.240s", m->error ? m->error : "");
        sess_set_status(b);
    } else if (!sess_busy) sess_set_status("Erledigt.");
}

/* ---- message search: the server (unencrypted rooms) and the local cache */

static AmxSearchHit *hits;
static int n_hits;
static const char *where_names[] = { "Alle Räume", "Den markierten Raum", NULL };
static char find_status[200];

HOOKPROTONHNO(hit_disp_func, LONG, struct NList_DisplayMessage *m)
{
    static char b0[24], b1[120], b2[400];
    const AmxSearchHit *h = m->entry;
    char *t, *u;
    const Room *r;
    if (!h) { m->strings[0] = "Wann"; m->strings[1] = "Raum"; m->strings[2] = "Nachricht"; return 0; }
    {
        time_t tt = (time_t)(h->timestamp / 1000);
        struct tm *tm = localtime(&tt);
        if (tm) strftime(b0, sizeof b0, "%d.%m.%y %H:%M", tm); else b0[0] = 0;
    }
    r = room_by_id(h->room_id);
    snprintf(b1, sizeof b1, "%s", r ? r->name : h->room_id);
    t = local(h->name);
    u = local(h->body);
    snprintf(b2, sizeof b2, "\33b%s:\33n %s", t ? t : "", u ? u : "");
    free(t); free(u);
    for (t = b2; *t; t++) if (*t == '\n') *t = ' ';
    m->strings[0] = b0; m->strings[1] = b1; m->strings[2] = b2;
    return 0;
}
MakeStaticHook(hit_disp_hook, hit_disp_func);

static void find_set_status(const char *t)
{
    snprintf(find_status, sizeof find_status, "%s", t ? t : "");
    set(txt_find, MUIA_Text_Contents, find_status);
}

static void find_go(void)
{
    char *t = amx_from_local((char *)xget(str_find, MUIA_String_Contents));
    Room *r = NULL;
    if (!t || !*t) { free(t); return; }
    if (xget(cyc_findwhere, MUIA_Cycle_Active) == 1) {
        DoMethod(lst_rooms, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &r);
        if (!r) { find_set_status("Bitte erst einen Raum in der Liste markieren."); free(t); return; }
    }
    amx_async_search(A, r ? r->id : NULL, t, TAG_FIND);
    find_set_status("Suche ...");
    free(t);
}

static void find_got(const AmxMsg *m)
{
    int i;
    char b[160];
    set(lst_hits, MUIA_NList_Quiet, TRUE);
    DoMethod(lst_hits, MUIM_NList_Clear);
    amx_free_search(hits, n_hits);
    hits = NULL;
    n_hits = 0;
    if (m->status) { set(lst_hits, MUIA_NList_Quiet, FALSE); find_set_status(m->error); return; }
    hits = m->n ? calloc(m->n, sizeof *hits) : NULL;
    n_hits = hits ? m->n : 0;
    for (i = 0; i < n_hits; i++) {
        hits[i] = m->hits[i];
        hits[i].room_id = xstrdup(m->hits[i].room_id);
        hits[i].event_id = xstrdup(m->hits[i].event_id);
        hits[i].sender = xstrdup(m->hits[i].sender);
        hits[i].name = xstrdup(m->hits[i].name);
        hits[i].body = xstrdup(m->hits[i].body);
        DoMethod(lst_hits, MUIM_NList_InsertSingle, &hits[i], MUIV_NList_Insert_Bottom);
    }
    set(lst_hits, MUIA_NList_Quiet, FALSE);
    snprintf(b, sizeof b, "%d Treffer - Doppelklick öffnet den Chat", n_hits);
    if (!n_hits) snprintf(b, sizeof b, "Nichts gefunden. (Verschlüsselte Räume: nur der gespeicherte Verlauf.)");
    find_set_status(b);
}

HOOKPROTONHNO(hit_open_func, LONG, APTR unused)
{
    AmxSearchHit *h = NULL;
    (void)unused;
    DoMethod(lst_hits, MUIM_NList_GetEntry, MUIV_NList_GetEntry_Active, &h);
    if (h && room_by_id(h->room_id)) open_chat(h->room_id);
    return 0;
}
MakeStaticHook(hit_open_hook, hit_open_func);

static void toggle_tag(const char *tag)
{
    Room *r = NULL;
    DoMethod(lst_rooms, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &r);
    if (!r || r->invited) {
        MUI_Request(app, win_main, 0, (STRPTR)"Amatrix", (STRPTR)"*_OK",
                    (STRPTR)"Bitte erst einen Raum in der Liste auswählen.", 0);
        return;
    }
    amx_async_set_tag(A, r->id, tag, !strcmp(tag, "m.favourite") ? !r->favourite : !r->low, TAG_TAG);
}

static ChatWin *current_chat(void);

/* ---- a new poll: question, up to six answers */

static void open_pollnew(void)
{
    ChatWin *w = current_chat();
    int i;
    if (!w) {
        MUI_Request(app, win_main, 0, (STRPTR)"Amatrix", (STRPTR)"*_OK",
                    (STRPTR)"Bitte erst das Chatfenster des Raums öffnen.", 0);
        return;
    }
    free(poll_room);
    poll_room = strdup(w->room_id);
    set(str_pollq, MUIA_String_Contents, "");
    for (i = 0; i < 6; i++) set(str_polla[i], MUIA_String_Contents, "");
    set(win_poll, MUIA_Window_Open, TRUE);
    set(win_poll, MUIA_Window_ActiveObject, str_pollq);
}

static void poll_send(void)
{
    char *q = amx_from_local((char *)xget(str_pollq, MUIA_String_Contents)), list[1600] = "";
    int i, n = 0;
    for (i = 0; i < 6; i++) {
        char *a = (char *)xget(str_polla[i], MUIA_String_Contents), *u;
        if (!a || !*a) continue;
        u = amx_from_local(a);
        if (u && strlen(list) + strlen(u) + 2 < sizeof list) {
            if (n++) strcat(list, "\n");
            strcat(list, u);
        }
        free(u);
    }
    if (!q || !*q || n < 2 || !poll_room) {
        MUI_Request(app, win_poll, 0, (STRPTR)"Amatrix", (STRPTR)"*_OK",
                    (STRPTR)"Eine Umfrage braucht eine Frage und mindestens zwei Antworten.", 0);
        free(q);
        return;
    }
    amx_async_send_poll(A, poll_room, q, list, xget(chk_pollmulti, MUIA_Selected) ? n : 1,
                        !xget(chk_pollhide, MUIA_Selected), TAG_POLLVOTE);
    free(q);
    set(win_poll, MUIA_Window_Open, FALSE);
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
#define CLICK_MEDIA 0x40000000L         /* button values: picture / file icon, globe, else reactions */
#define CLICK_LINK  0x20000000L
#define CLICK_SAVE  0x10000000L         /* the file icon: download (save as ...) */
#define CLICK_THREAD 0x08000000L        /* "Thread: n Antworten": open the thread's window */
#define CLICK_POLL  0x04000000L         /* a poll answer: (poll << 8) | answer, 255 = end the poll */
static int add_link(ChatWin *w, const char *u, size_t n);
static void remember_line(ChatWin *w, void *l);

/* ---- link previews: title and description of the first link of a message,
 * fetched by the server (cached here, one request per address) */

typedef struct { char *url, *title, *desc; int state; } Preview;   /* state 0 asked, 1 there, 2 none */
static Preview *prevs;
static int n_prevs;

/* the first http(s) address in a UTF-8 text, into buf; 0 if none */
static int first_url(const char *t, char *buf, size_t n)
{
    const char *p = t ? strstr(t, "http") : NULL;
    for (; p; p = strstr(p + 4, "http")) {
        size_t k;
        if (strncmp(p, "http://", 7) && strncmp(p, "https://", 8)) continue;
        k = strcspn(p, " \n\t<>\"");
        while (k > 0 && strchr(".,;:!?)'", p[k - 1])) k--;
        if (k < 10 || k >= n) return 0;
        memcpy(buf, p, k);
        buf[k] = 0;
        return 1;
    }
    return 0;
}

static Preview *preview_of(const char *url)
{
    int i;
    for (i = 0; i < n_prevs; i++) if (!strcmp(prevs[i].url, url)) return &prevs[i];
    return NULL;
}

static int link_index(ChatWin *w, const char *url)
{
    int i;
    for (i = 0; i < w->n_links; i++) if (!strcmp(w->links[i], url)) return i;
    return add_link(w, url, strlen(url));
}

/* the preview line under message m (once) */
static void insert_preview(ChatWin *w, ChatMsg *m, const Preview *pv)
{
    char text[400];
    LONG n = (LONG)xget(w->list, MUIA_NList_Entries), k, last = -1;
    Line *l;
    for (k = 0; k < n; k++) {
        Line *x = NULL;
        DoMethod(w->list, MUIM_NList_GetEntry, k, &x);
        if (!x || !x->eid || strcmp(x->eid, m->eid) || x->react || x->thr) continue;
        if (x->prev) return;                                   /* already there */
        last = k;
    }
    if (last < 0 || m->redacted) return;
    snprintf(text, sizeof text, "      \33o[%d@%ld] \33b%s\33n%s%s", EMO_LINK, CLICK_LINK | (long)link_index(w, pv->url),
             pv->title ? pv->title : "", pv->title && pv->desc ? " - " : "", pv->desc ? pv->desc : "");
    if (!(l = new_line(m->eid, text, strlen(text), 0))) return;
    l->prev = 1;
    remember_line(w, l);
    DoMethod(w->list, MUIM_NList_InsertSingleWrap, l, last + 1, WRAPCOL0, ALIGN_LEFT);   /* no wider list */
    w->lines++;
}

/* local text for a preview line: no codes, no line breaks, at most max chars */
static char *preview_text(const char *utf8, size_t max)
{
    char *t = utf8 ? local(utf8) : NULL, *p;
    if (!t) return NULL;
    for (p = t; *p; p++) if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
    if (strlen(t) > max) strcpy(t + max - 3, "...");
    if (!*t) { free(t); return NULL; }
    return t;
}

/* a message was shown: its link's preview (if wanted, cached or asked for) */
static void want_preview(ChatWin *w, ChatMsg *m)
{
    char url[600];
    const Room *r = room_by_id(w->room_id);
    Preview *pv, *t;
    if (!opt_linkprev || !m || m->redacted || (r && r->encrypted && !opt_linkprev_enc)) return;
    if (!first_url(m->body, url, sizeof url)) return;
    if ((pv = preview_of(url)) != NULL) {
        if (pv->state == 1) insert_preview(w, m, pv);
        return;
    }
    if (!(t = realloc(prevs, sizeof *prevs * (n_prevs + 1)))) return;
    prevs = t;
    memset(&prevs[n_prevs], 0, sizeof *prevs);
    if (!(prevs[n_prevs].url = strdup(url))) return;
    n_prevs++;
    amx_async_url_preview(A, url, url, TAG_PREVIEW);
}

static void got_preview(const AmxMsg *m)
{
    Preview *pv = m->result ? preview_of(m->result) : NULL;
    ChatWin *w;
    int i;
    if (!pv) return;
    if (m->status) { pv->state = 2; return; }
    pv->state = 1;
    pv->title = preview_text(m->title, 70);
    pv->desc = preview_text(m->description, 110);
    if (!pv->title && !pv->desc) { pv->state = 2; return; }
    for (w = chats; w; w = w->next)
        for (i = 0; i < w->n_msgs; i++) {
            char url[600];
            if (first_url(w->msgs[i].body, url, sizeof url) && !strcmp(url, pv->url)) insert_preview(w, &w->msgs[i], pv);
        }
}

/* ---- polls: the answers as lines under the question, a click votes */

typedef struct Poll {
    char *eid, *creator;
    int n, max, disclosed, ended, asked;
    double end_ts;
    char *ids[20], *texts[20];        /* texts in the local charset */
} Poll;
typedef struct Vote { char *eid, *poll, *sender, *answers; double ts; } Vote;

static Poll *poll_by_eid(ChatWin *w, const char *eid)
{
    int i;
    for (i = 0; eid && i < w->n_polls; i++) if (!strcmp(w->polls[i].eid, eid)) return &w->polls[i];
    return NULL;
}

/* the answers a sender chose last (before the end), NULL if none */
static const char *vote_of(ChatWin *w, const Poll *p, const char *sender)
{
    const Vote *best = NULL;
    int i;
    for (i = 0; i < w->n_votes; i++) {
        const Vote *v = &w->votes[i];
        if (strcmp(v->poll, p->eid) || strcmp(v->sender, sender)) continue;
        if (p->ended && p->end_ts > 0 && v->ts > p->end_ts) continue;
        if (!best || v->ts >= best->ts) best = v;
    }
    return best && best->answers[0] ? best->answers : NULL;
}

static int has_answer(const char *list, const char *id)
{
    size_t k = strlen(id);
    const char *p = list;
    while (p && *p) {
        if (!strncmp(p, id, k) && (p[k] == '\n' || !p[k])) return 1;
        p = strchr(p, '\n');
        if (p) p++;
    }
    return 0;
}

/* votes per answer (each voter's newest choice, at most max answers) */
static int tally(ChatWin *w, const Poll *p, int *count)
{
    int i, j, voters = 0;
    const char *seen[200];
    int n_seen = 0;
    for (j = 0; j < p->n; j++) count[j] = 0;
    for (i = 0; i < w->n_votes && n_seen < 200; i++) {
        const char *a;
        int k, dup = 0, used = 0;
        if (strcmp(w->votes[i].poll, p->eid)) continue;
        for (k = 0; k < n_seen; k++) if (!strcmp(seen[k], w->votes[i].sender)) dup = 1;
        if (dup) continue;
        seen[n_seen++] = w->votes[i].sender;
        if (!(a = vote_of(w, p, w->votes[i].sender))) continue;
        for (j = 0; j < p->n && used < p->max; j++)
            if (has_answer(a, p->ids[j])) { count[j]++; used++; }
        if (used) voters++;
    }
    return voters;
}

static void refresh_poll(ChatWin *w, Poll *p)
{
    LONG n = (LONG)xget(w->list, MUIA_NList_Entries), k, last = -1;
    int count[20], voters, j, best = 0, pi = (int)(p - w->polls);
    const char *mine = my_user ? vote_of(w, p, my_user) : NULL;
    char text[400];
    for (k = n - 1; k >= 0; k--) {                    /* the old rows away */
        Line *l = NULL;
        DoMethod(w->list, MUIM_NList_GetEntry, k, &l);
        if (l && l->pollrow && l->eid && !strcmp(l->eid, p->eid)) { DoMethod(w->list, MUIM_NList_Remove, k); w->lines--; }
    }
    n = (LONG)xget(w->list, MUIA_NList_Entries);
    for (k = 0; k < n; k++) {
        Line *l = NULL;
        DoMethod(w->list, MUIM_NList_GetEntry, k, &l);
        if (l && l->eid && !strcmp(l->eid, p->eid) && !l->react && !l->thr) last = k;
    }
    if (last < 0) return;
    voters = tally(w, p, count);
    for (j = 0; j < p->n; j++) if (count[j] > best) best = count[j];
    for (j = 0; j <= p->n; j++) {
        Line *l;
        if (j < p->n) {
            int show = p->disclosed || p->ended, win = p->ended && best > 0 && count[j] == best;
            char num[24] = "";
            if (show) snprintf(num, sizeof num, "  (%d)", count[j]);
            snprintf(text, sizeof text, "      \33o[%d@%ld] %s%s\33n%s", mine && has_answer(mine, p->ids[j]) ? EMO_RADIO_ON
                     : EMO_RADIO_OFF, CLICK_POLL | ((long)pi << 8) | j, win ? "\33b" : "", p->texts[j], num);
        } else {
            int own = p->creator && my_user && !strcmp(p->creator, my_user);
            snprintf(text, sizeof text, "      \33i%d %s%s%s", voters, voters == 1 ? "Stimme" : "Stimmen",
                     p->ended ? " - beendet" : p->disclosed ? "" : " (Ergebnis am Ende)",
                     p->max > 1 ? (p->ended ? "" : ", mehrere wählbar") : "");
            if (own && !p->ended) {
                size_t t = strlen(text);
                snprintf(text + t, sizeof text - t, "  \33n\33o[%d@%ld] beenden", EMO_STOP, CLICK_POLL | ((long)pi << 8) | 255);
            }
        }
        if (!(l = new_line(p->eid, text, strlen(text), 0))) return;
        l->pollrow = 1;
        remember_line(w, l);
        DoMethod(w->list, MUIM_NList_InsertSingle, l, last + 1 + j);
        w->lines++;
    }
}

/* a poll message was shown: remember it, its rows, and fetch its votes once */
static void add_poll(ChatWin *w, const AmxEvent *e)
{
    cJSON *j, *a;
    Poll *p, *t;
    if (!e->poll || !e->event_id) return;
    if ((p = poll_by_eid(w, e->event_id)) == NULL) {
        if (!(j = cJSON_Parse(e->poll))) return;
        if (!(t = realloc(w->polls, sizeof *t * (w->n_polls + 1)))) { cJSON_Delete(j); return; }
        w->polls = t;
        p = &w->polls[w->n_polls++];
        memset(p, 0, sizeof *p);
        p->eid = strdup(e->event_id);
        p->creator = e->sender ? strdup(e->sender) : NULL;
        p->max = cJSON_GetObjectItem(j, "max") ? cJSON_GetObjectItem(j, "max")->valueint : 1;
        p->disclosed = cJSON_IsTrue(cJSON_GetObjectItem(j, "disclosed"));
        cJSON_ArrayForEach(a, cJSON_GetObjectItem(j, "answers")) {
            const cJSON *id = cJSON_GetObjectItem(a, "id"), *tx = cJSON_GetObjectItem(a, "text");
            if (p->n >= 20 || !cJSON_IsString(id)) continue;
            p->ids[p->n] = strdup(id->valuestring);
            p->texts[p->n] = local(cJSON_IsString(tx) ? tx->valuestring : "");
            p->n++;
        }
        cJSON_Delete(j);
        {   /* an end that came first */
            int i;
            for (i = 0; i < w->n_votes; i++)
                if (!strcmp(w->votes[i].poll, p->eid) && !strcmp(w->votes[i].answers, "\001end")
                    && p->creator && !strcmp(w->votes[i].sender, p->creator)) { p->ended = 1; p->end_ts = w->votes[i].ts; }
        }
    }
    refresh_poll(w, p);
    if (!p->asked) { p->asked = 1; amx_async_relations(A, w->room_id, p->eid, TAG_POLL); }
}

/* a vote or an end (kept also when the poll is not here yet) */
static void poll_event(ChatWin *w, const AmxEvent *e)
{
    Vote *t, *v;
    Poll *p = poll_by_eid(w, e->relates_to);
    int i;
    for (i = 0; e->event_id && i < w->n_votes; i++) if (w->votes[i].eid && !strcmp(w->votes[i].eid, e->event_id)) return;
    if (e->event_id && e->sender)                 /* the server's copy replaces what we showed at once */
        for (i = w->n_votes - 1; i >= 0; i--)
            if (!w->votes[i].eid && !strcmp(w->votes[i].poll, e->relates_to) && !strcmp(w->votes[i].sender, e->sender)) {
                free(w->votes[i].poll); free(w->votes[i].sender); free(w->votes[i].answers);
                memmove(&w->votes[i], &w->votes[i + 1], sizeof *w->votes * (w->n_votes - i - 1));
                w->n_votes--;
            }
    if (!(t = realloc(w->votes, sizeof *t * (w->n_votes + 1)))) return;
    w->votes = t;
    v = &w->votes[w->n_votes++];
    v->eid = e->event_id ? strdup(e->event_id) : NULL;
    v->poll = strdup(e->relates_to);
    v->sender = strdup(e->sender ? e->sender : "");
    v->answers = strdup(e->type == AMX_EV_POLL_END ? "\001end" : e->body ? e->body : "");
    v->ts = e->timestamp;
    if (e->type == AMX_EV_POLL_END && p && p->creator && e->sender && !strcmp(p->creator, e->sender)) {
        p->ended = 1;
        p->end_ts = e->timestamp;
    }
    if (p) refresh_poll(w, p);
}

static void free_polls(ChatWin *w)
{
    int i, j;
    for (i = 0; i < w->n_polls; i++) {
        free(w->polls[i].eid); free(w->polls[i].creator);
        for (j = 0; j < w->polls[i].n; j++) { free(w->polls[i].ids[j]); free(w->polls[i].texts[j]); }
    }
    for (i = 0; i < w->n_votes; i++) { free(w->votes[i].eid); free(w->votes[i].poll); free(w->votes[i].sender); free(w->votes[i].answers); }
    free(w->polls); free(w->votes);
    w->polls = NULL; w->votes = NULL; w->n_polls = w->n_votes = 0;
}

/* a click on an answer: vote (one answer: switch / take back; several: toggle) */
static void poll_click(ChatWin *w, int pi, int ai)
{
    Poll *p = pi < w->n_polls ? &w->polls[pi] : NULL;
    const char *mine;
    char list[600] = "";
    int j, n = 0;
    AmxEvent e;
    if (!p || p->ended || !my_user) return;
    if (ai == 255) {
        if (MUI_Request(app, w->win, 0, (STRPTR)"Amatrix", (STRPTR)"_Beenden|*_Abbrechen",
                        (STRPTR)"Die Umfrage beenden? Danach zählt keine Stimme mehr.", 0) == 1)
            amx_async_poll_end(A, w->room_id, p->eid, TAG_POLLVOTE);
        return;
    }
    if (ai >= p->n) return;
    mine = vote_of(w, p, my_user);
    for (j = 0; j < p->n; j++) {
        int on = mine && has_answer(mine, p->ids[j]);
        if (j == ai) on = !on;
        else if (p->max == 1) on = 0;
        if (on && n < p->max && strlen(list) + strlen(p->ids[j]) + 2 < sizeof list) {
            if (n++) strcat(list, "\n");
            strcat(list, p->ids[j]);
        }
    }
    amx_async_poll_vote(A, w->room_id, p->eid, list, TAG_POLLVOTE);
    memset(&e, 0, sizeof e);                          /* shown at once; the server's copy follows */
    e.type = AMX_EV_POLL_VOTE;
    e.relates_to = p->eid;
    e.sender = my_user;
    e.body = list;
    e.timestamp = (double)time(NULL) * 1000.0 + 1;
    poll_event(w, &e);
}

static void open_thread_of(ChatWin *w, int mi);
static void open_media(ChatWin *w, int mi);
static void save_media(ChatWin *w, int mi, const char *to);
static int open_link(ChatWin *w, int li, int quiet);
static void free_media(ChatWin *w);

HOOKPROTONH(chip_func, LONG, Object *obj, ULONG *param)
{
    ChatWin *w = (ChatWin *)param[0];
    long v = (long)param[1], serial = v / 16;
    int group = (int)(v % 16), i, j, g = 0;
    LONG n = (LONG)xget(w->list, MUIA_NList_Entries), k;
    (void)obj;
    if (v & CLICK_MEDIA) { open_media(w, (int)(v & 0xffffff)); return 0; }
    if (v & CLICK_SAVE) { save_media(w, (int)(v & 0xffffff), NULL); return 0; }
    if (v & CLICK_THREAD) { open_thread_of(w, (int)(v & 0xffffff)); return 0; }
    if (v & CLICK_LINK) { open_link(w, (int)(v & 0xffffff), 0); return 0; }
    if (v & CLICK_POLL) { poll_click(w, (int)((v & 0x3ffff00) >> 8), (int)(v & 0xff)); return 0; }
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
                                             "Nachdenklich", "Augen", "Feiern", "Rakete", "", "", "", "" };

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
    open_chat_ex(room_id, NULL);
}

/* a chat window for a room, or for one of its threads */
static void open_chat_ex(const char *room_id, const char *thread_root)
{
    ChatWin *w = thread_root ? chat_thread(room_id, thread_root) : chat_by_room(room_id);
    Room *r = room_by_id(room_id);
    unsigned long h;
    LONG left, top, width;
    amx_log("open_chat %s %s", room_id, thread_root ? thread_root : "");
    if (r && r->is_space && !w) return;                   /* a space has no messages to show */
    notify_close(room_id);

    if (w) {
        set(w->win, MUIA_Window_Open, TRUE);
        DoMethod(w->win, MUIM_Window_ToFront);
        set(w->win, MUIA_Window_Activate, TRUE);
        return;
    }
    if (!(w = calloc(1, sizeof *w))) return;
    w->next_img = 16;                         /* 0..15: the small pictures (emoji, file, link) */
    w->room_id = strdup(room_id);
    w->unread_open = !thread_root && r ? r->unread : 0;
    w->draft_pending = !thread_root && draft_of(room_id) ? 1 : 0;
    if (thread_root) {
        char t[200];
        snprintf(t, sizeof t, "Thread - %s", r ? r->name : room_id);
        w->title = strdup(t);
        w->thread_root = strdup(thread_root);
        h = hash(thread_root);
    } else {
        w->title = strdup(r ? r->name : room_id);
        h = hash(room_id);
    }
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
        MUIA_Window_AppWindow, TRUE,             /* icons dropped here are sent */
        MUIA_Window_ID, MAKE_ID('C', (h >> 16) & 255, (h >> 8) & 255, h & 255),
        MUIA_Window_LeftEdge, left,
        MUIA_Window_TopEdge, top,
        MUIA_Window_Width, width,
        MUIA_Window_Height, MUIV_Window_Height_Visible(55),
        WindowContents, VGroup,
            Child, HGroup,                          /* toolbar */
                Child, w->older = icon_button(&icon_up, OLDER_HELP),
                Child, w->replyb = icon_button(&icon_reply, REPLY_HELP),
                Child, w->editb = icon_button(&icon_edit, EDIT_HELP),
                Child, w->delb = icon_button(&icon_trash, DELETE_HELP),
                Child, w->reactb = icon_button(&icon_react, REACT_HELP),
                Child, w->attachb = icon_button(&icon_attach, ATTACH_HELP),
                Child, w->invite = icon_button(&icon_invite, INVITE_HELP),
                Child, w->infob = icon_button(&icon_info, INFO_HELP),
                Child, w->leave = icon_button(&icon_leave, LEAVE_HELP),
                Child, w->busy = TextObject,
                    MUIA_Text_Contents, "",
                    MUIA_Text_PreParse, "\33r",
                    MUIA_Text_SetMin, FALSE,   /* long errors: no wider window */
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
            Child, w->pinbar = TextObject,
                MUIA_ShowMe, FALSE,
                TextFrame,
                MUIA_Background, MUII_TextBack,
                MUIA_InputMode, MUIV_InputMode_RelVerify,
                MUIA_Text_SetMin, FALSE,
                MUIA_ShortHelp, "Klick: zur angepinnten Nachricht",
            End,
            Child, w->tombbar = TextObject,
                MUIA_ShowMe, FALSE,
                TextFrame,
                MUIA_Background, MUII_TextBack,
                MUIA_InputMode, MUIV_InputMode_RelVerify,
                MUIA_Text_Contents, "\33c\33bDieser Raum wurde durch einen neuen ersetzt.\33n Klick: dorthin",
            End,
            Child, w->modegrp = HGroup,
                MUIA_ShowMe, FALSE,
                /* SetMin FALSE: a long quote must not make the window wider than the
                 * screen - MUI would close it */
                Child, w->modetext = TextObject, MUIA_Text_Contents, "", MUIA_Text_SetMin, FALSE,
                    MUIA_Weight, 100, End,
                Child, w->modex = SimpleButton("_Abbrechen"),
            End,
            Child, w->sugg = HGroup,
                MUIA_ShowMe, FALSE,
                Child, w->sbtn[0] = SimpleButton(""),
                Child, w->sbtn[1] = SimpleButton(""),
                Child, w->sbtn[2] = SimpleButton(""),
                Child, w->sbtn[3] = SimpleButton(""),
                Child, w->sbtn[4] = SimpleButton(""),
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
    DoMethod(w->attachb, MUIM_Notify, MUIA_Pressed, FALSE, MUIV_Notify_Application, 3,
             MUIM_CallHook, &chat_attach_hook, w);
    DoMethod(w->pinbar, MUIM_Notify, MUIA_Pressed, FALSE, MUIV_Notify_Application, 3,
             MUIM_CallHook, &pinbar_hook, w);
    DoMethod(w->tombbar, MUIM_Notify, MUIA_Pressed, FALSE, MUIV_Notify_Application, 3,
             MUIM_CallHook, &tomb_hook, w);
    DoMethod(w->infob, MUIM_Notify, MUIA_Pressed, FALSE, MUIV_Notify_Application, 3,
             MUIM_CallHook, &chat_info_hook, w);
    DoMethod(w->input, MUIM_Notify, MUIA_String_Contents, MUIV_EveryTime, MUIV_Notify_Application, 3,
             MUIM_CallHook, &chat_typing_hook, w);
    DoMethod(w->win, MUIM_Notify, MUIA_Window_InputEvent, "up", MUIV_Notify_Application, 3,
             MUIM_CallHook, &uparrow_hook, w);
    if (w->draft_pending) {
        set(w->input, MUIA_String_Contents, draft_of(w->room_id));
        w->draft_pending = 0;
    }
    {
        int k;
        for (k = 0; k < 5; k++)
            DoMethod(w->sbtn[k], MUIM_Notify, MUIA_Pressed, FALSE, MUIV_Notify_Application, 4,
                     MUIM_CallHook, &sugg_hook, w, k);
    }
    DoMethod(w->list, MUIM_Notify, MUIA_AppMessage, MUIV_EveryTime, MUIV_Notify_Application, 4,
             MUIM_CallHook, &chat_drop_hook, w, MUIV_TriggerValue);
    DoMethod(w->input, MUIM_Notify, MUIA_AppMessage, MUIV_EveryTime, MUIV_Notify_Application, 4,
             MUIM_CallHook, &chat_drop_hook, w, MUIV_TriggerValue);
    DoMethod(w->replyb, MUIM_Notify, MUIA_Pressed, FALSE, MUIV_Notify_Application, 3,
             MUIM_CallHook, &chat_reply_hook, w);
    DoMethod(w->editb, MUIM_Notify, MUIA_Pressed, FALSE, MUIV_Notify_Application, 3,
             MUIM_CallHook, &chat_edit_hook, w);
    DoMethod(w->delb, MUIM_Notify, MUIA_Pressed, FALSE, MUIV_Notify_Application, 3,
             MUIM_CallHook, &chat_del_hook, w);
    DoMethod(w->modex, MUIM_Notify, MUIA_Pressed, FALSE, MUIV_Notify_Application, 3,
             MUIM_CallHook, &chat_modex_hook, w);
    DoMethod(w->list, MUIM_Notify, MUIA_NList_Active, MUIV_EveryTime, MUIV_Notify_Application, 3,
             MUIM_CallHook, &chat_active_hook, w);
    set(w->editb, MUIA_Disabled, TRUE);
    set(w->delb, MUIA_Disabled, TRUE);
    set(w->replyb, MUIA_Disabled, TRUE);
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
    if (thread_root) {
        set(w->invite, MUIA_ShowMe, FALSE);              /* room things: in the room's window */
        set(w->leave, MUIA_ShowMe, FALSE);
        set(w->infob, MUIA_ShowMe, FALSE);
        amx_async_load_thread(A, room_id, thread_root, FIRST_PAGE, 1, TAG_THREAD);
    } else
    amx_async_load_history(A, room_id, FIRST_PAGE, 1, TAG_HIST);
    update_status(w);
}

static void close_chat(ChatWin *w)
{
    ChatWin **p;
    int i;
    typing_stop(w);
    if (!w->thread_root && !w->edit_eid) {          /* unsent text: a draft for next time */
        char *t = (char *)xget(w->input, MUIA_String_Contents);
        if ((t && *t) || draft_of(w->room_id)) {
            set_draft(w->room_id, t);
            save_prefs();
            DoMethod(lst_rooms, MUIM_List_Redraw, MUIV_List_Redraw_All);
        }
    }
    free_polls(w);
    clear_suggestions(w);
    clear_mentions(w);
    amx_free_members(w->mem, w->n_mem);
    for (p = &chats; *p; p = &(*p)->next) if (*p == w) { *p = w->next; break; }
    if (pick_w == w) { pick_w = NULL; if (win_pick) set(win_pick, MUIA_Window_Open, FALSE); }
    set(w->win, MUIA_Window_Open, FALSE);
    DoMethod(w->list, MUIM_NList_Clear);
    DoMethod(w->list, MUIM_NList_UseImage, NULL, MUIV_NList_UseImage_All, 0);
    DoMethod(app, OM_REMMEMBER, w->win);
    MUI_DisposeObject(w->win);
    for (i = 0; i < N_EMO; i++) if (w->emo[i]) MUI_DisposeObject(w->emo[i]);
    free_media(w);
    free_msgs(w);
    for (i = 0; i < w->n_react; i++) {
        free(w->react[i].id); free(w->react[i].target); free(w->react[i].key); free(w->react[i].sender);
    }
    free(w->react);
    for (i = 0; i < w->n_all; i++) free_line(w->all[i]);    /* each exactly once */
    free(w->all);
    for (i = 0; i < w->n_seen; i++) free(w->seen[i]);
    free(w->seen);
    free(w->room_id); free(w->last_event); free(w->marked); free(w->title); free(w->thread_root);
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
    char *name = local(e->sender_name ? e->sender_name : e->sender), *body, *out, tbuf[32] = "";
    const char *mt = e->msgtype ? e->msgtype : "";
    size_t n;
    if (e->timestamp > 0) {
        time_t t = (time_t)(e->timestamp / 1000);
        struct tm *tm = localtime(&t);
        if (tm) strftime(tbuf, 16, opt_24h ? "%H:%M " : "%I:%M%p ", tm);
    }
    switch (e->type) {
    case AMX_EV_MESSAGE: {
        char *rich = e->formatted && !e->media ? amx_rich_from_html(e->formatted) : NULL;
        body = local_emoji(rich && rich[0] ? rich : e->body);
        free(rich);
        if (e->mention && !e->own) {              /* it mentions us: "@" in front */
            char t2[32];
            snprintf(t2, sizeof t2, "\33b@\33n %s", tbuf);
            strcpy(tbuf, t2);
        }
        n = strlen(name) + strlen(body) + 64;
        out = malloc(n);
        if (!strcmp(mt, "m.emote"))
            snprintf(out, n, "%s\33i* %s %s", tbuf, name, body);
        else if (!strcmp(mt, "m.notice"))
            snprintf(out, n, "%s\33b%s:\33n \33i%s", tbuf, name, body);
        else if (!strcmp(mt, "m.location"))
            snprintf(out, n, "%s\33b%s:\33n Standort: %s", tbuf, name, body);
        else if (!strcmp(mt, "m.poll"))
            snprintf(out, n, "%s\33b%s:\33n Umfrage: \33b%s", tbuf, name, body);
        else if (!strcmp(mt, "m.image") || !strcmp(mt, "m.file") || !strcmp(mt, "m.video") || !strcmp(mt, "m.audio"))
            snprintf(out, n, "%s\33b%s:\33n [%s: %s]", tbuf, name,
                     !strcmp(mt, "m.image") ? "Bild" : !strcmp(mt, "m.video") ? "Video" :
                     !strcmp(mt, "m.audio") ? "Audio" : "Datei", body);
        else
            snprintf(out, n, "%s\33b%s:\33n %s", tbuf, name, body);
        free(body);
        break;
    }
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

/* ---------------------------------------------------------------- attachments, links */

static char save_dir[256];            /* last drawer of "save as" */

#define THUMB_W 128                   /* previews fit into this (small: memory, speed) */
#define THUMB_H 96

static int add_media(ChatWin *w, const AmxEvent *e, int idx)
{
    Media *m;
    char b[256];
    if (w->n_media == w->cap_media) {
        Media *t = realloc(w->media, sizeof *t * (w->cap_media ? w->cap_media * 2 : 16));
        if (!t) return -1;
        w->media = t;
        w->cap_media = w->cap_media ? w->cap_media * 2 : 16;
    }
    m = &w->media[w->n_media];
    memset(m, 0, sizeof *m);
    m->eid = strdup(e->event_id);
    m->media = strdup(e->media);
    m->idx = idx;
    m->type = strdup(amx_media_info(e->media, idx, "type", b, sizeof b) ? b : "m.file");
    m->name = strdup(amx_media_info(e->media, idx, "name", b, sizeof b) ? b : "Datei");
    if (!m->eid || !m->media || !m->type || !m->name) return -1;
    return w->n_media++;
}

static int is_picture(const Media *m)
{
    return !strcmp(m->type, "m.image") || !strcmp(m->type, "m.sticker");
}

/* the caption of a message with attachments, each with a clickable file icon;
 * pictures get their preview requested */
static char *media_text(ChatWin *w, const AmxEvent *e, const char *text)
{
    int n = amx_media_count(e->media), i, single = n == 1 && e->msgtype && strcmp(e->msgtype, "m.sticker");
    size_t len = strlen(text) + 64 * n + 1;
    char *out = malloc(len), *q;
    if (!out) return NULL;
    strcpy(out, text);
    for (i = 0; i < n; i++) {
        int mi = add_media(w, e, i);
        if (mi < 0) break;
        q = out + strlen(out);
        snprintf(q, len - strlen(out), " \33o[%d@%ld]", EMO_FILE, CLICK_SAVE | mi);
        if (opt_previews && is_picture(&w->media[mi])) {
            char key[300];
            snprintf(key, sizeof key, "%s\n%s\n%d", w->room_id, e->event_id, i);
            w->media[mi].state = 1;
            amx_async_media_thumbnail(A, e->media, i, key, THUMB_W, THUMB_H, TAG_THUMB);
        }
    }
    (void)single;
    return out;
}

/* URLs in a line of text: underlined, a globe after each (a click opens the browser) */
static int add_link(ChatWin *w, const char *u, size_t n)
{
    char *c;
    if (w->n_links == w->cap_links) {
        char **t = realloc(w->links, sizeof *t * (w->cap_links ? w->cap_links * 2 : 16));
        if (!t) return -1;
        w->links = t;
        w->cap_links = w->cap_links ? w->cap_links * 2 : 16;
    }
    if (!(c = malloc(n + 1))) return -1;
    memcpy(c, u, n);
    c[n] = 0;
    w->links[w->n_links] = c;
    return w->n_links++;
}

static char *linkify(ChatWin *w, char *text)
{
    char *p = text, *out, *q;
    size_t cap = strlen(text) * 2 + 64, used = 0;
    if (!strstr(text, "http://") && !strstr(text, "https://") && !strstr(text, "www.")) return text;
    if (!(out = malloc(cap))) return text;
    while (*p) {
        char *u = NULL;
        if (!strncmp(p, "http://", 7) || !strncmp(p, "https://", 8)
            || (!strncmp(p, "www.", 4) && (p == text || p[-1] == ' ' || p[-1] == '(')))
            u = p;
        if (u) {
            char *e = u;
            int li;
            size_t n;
            while (*e && *e != ' ' && *e != '\n' && *e != '\t' && *e != '<' && *e != '>' && *e != '"'
                   && *e != 27 && (unsigned char)*e > 32)
                e++;
            while (e > u && strchr(".,;:!?)'", e[-1])) e--;          /* end of sentence */
            n = (size_t)(e - u);
            if (n > 8 && used + n + 40 < cap) {
                char tmp[40];
                q = out + used;
                memcpy(q, "\33u", 2);
                memcpy(q + 2, u, n);
                used += n + 2;
                if (!strncmp(u, "www.", 4)) {
                    char *full = malloc(n + 8);
                    if (full) { sprintf(full, "http://%.*s", (int)n, u); li = add_link(w, full, strlen(full)); free(full); }
                    else li = -1;
                } else li = add_link(w, u, n);
                if (li >= 0) snprintf(tmp, sizeof tmp, "\33n \33o[%d@%ld]", EMO_LINK, CLICK_LINK | li);
                else snprintf(tmp, sizeof tmp, "\33n");
                memcpy(out + used, tmp, strlen(tmp));
                used += strlen(tmp);
                p = e;
                continue;
            }
        }
        if (used + 2 >= cap) break;
        out[used++] = *p++;
    }
    out[used] = 0;
    free(text);
    return out;
}

/* the browser (openurl.library); quiet: no requester (ARexx) */
static int open_link(ChatWin *w, int li, int quiet)
{
    struct Library *OpenURLBase;
    ULONG ok = 0;
    if (li < 0 || li >= w->n_links) return 0;
    amx_log("open link %s", w->links[li]);
    if ((OpenURLBase = OpenLibrary("openurl.library", 0)) != NULL) {
        struct TagItem tags[] = { { 0x81480000UL + 4, TRUE },      /* URL_Launch */
                                  { 0x81480000UL + 1, TRUE },      /* URL_Show */
                                  { 0x81480000UL + 2, TRUE },      /* URL_BringToFront */
                                  { TAG_DONE, 0 } };
        ok = URL_OpenA((STRPTR)w->links[li], tags);
        CloseLibrary(OpenURLBase);
    }
    if (!ok && !quiet)
        MUI_Request(app, w->win, 0, (STRPTR)"Amatrix", (STRPTR)"*_OK",
                    (STRPTR)"Der Link konnte nicht geöffnet werden\n(openurl.library installiert?):\n\n%s",
                    (ULONG)w->links[li]);
    return ok != 0;
}

/* a saved attachment with its default tool: the icon DefIcons gives the file
 * type (no .info needed) names the tool, which Workbench starts with the file
 * as its argument; without one, MultiView (datatypes) */
static void open_file(const char *path)
{
    struct Library *WorkbenchBase = OpenLibrary("workbench.library", 44);
    struct DiskObject *dob = IconBase ? GetDiskObjectNew((STRPTR)path) : NULL;
    char tool[256] = "", cmd[600];
    BOOL ok = FALSE;
    if (dob && dob->do_Type == WBPROJECT && dob->do_DefaultTool && dob->do_DefaultTool[0])
        snprintf(tool, sizeof tool, "%s", dob->do_DefaultTool);
    if (dob) FreeDiskObject(dob);
    amx_log("open file %s with %s", path, tool[0] ? tool : "MultiView");
    if (!tool[0]) snprintf(tool, sizeof tool, "SYS:Utilities/MultiView");
    if (WorkbenchBase) {                         /* like a double-click: the tool gets the file */
        BPTR dir = 0;
        char drawer[300];
        const char *name = (const char *)FilePart((STRPTR)path);
        snprintf(drawer, sizeof drawer, "%.*s", (int)(name - path), path);
        if ((dir = Lock((STRPTR)drawer, ACCESS_READ)) != 0) {
            ok = OpenWorkbenchObject((STRPTR)tool, WBOPENA_ArgLock, dir, WBOPENA_ArgName, name, TAG_DONE);
            UnLock(dir);
        }
        CloseLibrary(WorkbenchBase);
    }
    if (!ok) {                                   /* no Workbench: from the shell */
        BPTR in = Open("NIL:", MODE_OLDFILE), out = Open("NIL:", MODE_NEWFILE);
        LONG rc;
        snprintf(cmd, sizeof cmd, "\"%s\" \"%s\"", tool, path);
        rc = SystemTags(cmd, SYS_Input, in, SYS_Output, out, SYS_Asynch, TRUE, NP_StackSize, 32768, TAG_DONE);
        amx_log("  started from the shell: %ld", (long)rc);
        if (rc == -1) { if (in) Close(in); if (out) Close(out); }
    } else amx_log("  started by Workbench");
}

static void open_media(ChatWin *w, int mi)
{
    Media *m;
    char path[300], *q;
    BPTR l;
    if (mi < 0 || mi >= w->n_media) return;
    m = &w->media[mi];
    if (m->path) { open_file(m->path); return; }
    if (m->saving) return;
    if ((l = Lock("T:Amatrix", ACCESS_READ)) != 0) UnLock(l);
    else if ((l = CreateDir("T:Amatrix")) != 0) UnLock(l);
    snprintf(path, sizeof path, "T:Amatrix/%.8s-%s", m->eid + 1, m->name);
    for (q = path + 10; *q; q++)
        if (*q == ':' || *q == '/' || *q == '"' || (unsigned char)*q < 32) *q = '_';
    if (!(m->saving = strdup(path))) return;
    m->open_after = 1;
    snprintf(w->err, sizeof w->err, "Lade %s ...", m->name);
    update_status(w);
    amx_async_media_save(A, m->media, m->idx, path, TAG_SAVE);
}

/* the file icon: download to a place the user picks (to: without asking) */
static void save_media(ChatWin *w, int mi, const char *to)
{
    Media *m;
    char path[512];
    if (mi < 0 || mi >= w->n_media) return;
    m = &w->media[mi];
    if (m->saving) return;
    if (to) snprintf(path, sizeof path, "%s", to);
    else {
        struct Library *AslBase = OpenLibrary("asl.library", 38);
        struct FileRequester *fr;
        int ok = 0;
        if (!AslBase) return;
        set(app, MUIA_Application_Sleep, TRUE);
        fr = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
                 ASLFR_Window, (ULONG)xget(w->win, MUIA_Window_Window),
                 ASLFR_TitleText, (ULONG)"Datei speichern",
                 ASLFR_InitialFile, (ULONG)m->name, ASLFR_InitialDrawer, (ULONG)(save_dir[0] ? save_dir : "RAM:"),
                 ASLFR_DoSaveMode, TRUE, TAG_DONE);
        if (fr && AslRequest(fr, NULL) && fr->fr_File[0]) {
            snprintf(save_dir, sizeof save_dir, "%s", fr->fr_Drawer);
            snprintf(path, sizeof path, "%s", fr->fr_Drawer);
            ok = AddPart((STRPTR)path, (STRPTR)fr->fr_File, sizeof path);
        }
        if (fr) FreeAslRequest(fr);
        CloseLibrary(AslBase);
        set(app, MUIA_Application_Sleep, FALSE);
        if (!ok) return;
    }
    if (!(m->saving = strdup(path))) return;
    m->open_after = 0;
    snprintf(w->err, sizeof w->err, "Speichere %s ...", m->name);
    update_status(w);
    amx_async_media_save(A, m->media, m->idx, path, TAG_SAVE);
}

/* saved: open it (if a window still waits for it) */
static void media_saved(const AmxMsg *m)
{
    ChatWin *w;
    int i;
    for (w = chats; w; w = w->next)
        for (i = 0; i < w->n_media; i++) {
            Media *x = &w->media[i];
            if (!x->saving || !m->result || strcmp(x->saving, m->result)) continue;
            free(x->saving);
            x->saving = NULL;
            w->err[0] = 0;
            if (m->status) snprintf(w->err, sizeof w->err, "Laden fehlgeschlagen: %.280s", m->error ? m->error : "");
            else if (x->open_after) { x->path = strdup(m->result); open_file(m->result); }
            else snprintf(w->err, sizeof w->err, "Gespeichert: %.140s", m->result);
            update_status(w);
            return;
        }
}

/* the preview arrived: cut into strips of line height, one list entry each,
 * right under the message (after earlier pictures of the same message) */
static void media_thumb(const AmxMsg *msg)
{
    char room[256] = "", eid[256] = "";
    int idx = 0, i, k, mi = -1, lh, n;
    ChatWin *w;
    Media *m;
    LONG entries, pos = -1, e;
    if (!msg->result || sscanf(msg->result, "%255[^\n]\n%255[^\n]\n%d", room, eid, &idx) != 3) return;
    if (!(w = chat_by_room(room))) return;
    for (i = 0; i < w->n_media; i++)
        if (!strcmp(w->media[i].eid, eid) && w->media[i].idx == idx) { mi = i; break; }
    if (mi < 0) return;
    m = &w->media[mi];
    if (msg->status || !msg->pixels || msg->width < 1 || msg->height < 1) {
        m->state = 3;
        amx_log("no preview for %s: %s", m->name, msg->error ? msg->error : "-");
        return;
    }
    lh = (int)xget(w->list, MUIA_NList_LineHeight);
    if (lh < 8) lh = 12;
    n = (msg->height + lh - 1) / lh;
    if (!(m->obj = calloc(n, sizeof *m->obj)) || !(m->ad = calloc(n, sizeof *m->ad))) return;
    /* where: after the message's text and its earlier pictures, before its reactions */
    entries = (LONG)xget(w->list, MUIA_NList_Entries);
    for (e = 0; e < entries; e++) {
        Line *l = NULL;
        DoMethod(w->list, MUIM_NList_GetEntry, e, &l);
        if (!l || !l->eid || strcmp(l->eid, eid) || l->react || l->thr) continue;
        if (l->slice && w->media[l->midx].idx > idx) continue;
        pos = e;
    }
    if (pos < 0) return;                          /* the message is no longer shown */
    m->img0 = w->next_img;
    for (k = 0; k < n; k++) {
        int y, x, rows = (k + 1) * lh <= msg->height ? lh : msg->height - k * lh;
        struct MUI_AlphaData *ad = malloc(sizeof *ad + sizeof(ULONG) * msg->width * lh);
        char spec[24], text[48];
        Line *l;
        if (!ad) break;
        ad->width = msg->width;
        ad->height = lh;
        for (y = 0; y < lh; y++)
            for (x = 0; x < msg->width; x++)
                ad->data[y * msg->width + x] = y < rows ? msg->pixels[(k * lh + y) * msg->width + x] : 0;
        m->ad[k] = ad;
        snprintf(spec, sizeof spec, "\33A[%08lx]", (unsigned long)ad);
        m->obj[k] = TextObject, MUIA_Text_Contents, spec, MUIA_Text_SetMax, TRUE, MUIA_FixHeight, lh,
                    MUIA_InnerLeft, 0, MUIA_InnerRight, 0, MUIA_InnerTop, 0, MUIA_InnerBottom, 0, End;
        if (!m->obj[k]) break;
        DoMethod(w->list, MUIM_NList_UseImage, m->obj[k], w->next_img, 0);
        snprintf(text, sizeof text, "      \33o[%d@%ld]", w->next_img, CLICK_MEDIA | mi);
        w->next_img++;
        if (!(l = new_line(eid, text, strlen(text), 0))) break;
        l->slice = 1;
        l->midx = mi;
        remember_line(w, l);
        DoMethod(w->list, MUIM_NList_InsertSingle, l, pos + 1 + k);
        w->lines++;
        m->n_slices = k + 1;
    }
    m->state = 2;
    if (pos + 1 + m->n_slices >= entries + m->n_slices - 1) DoMethod(w->list, MUIM_NList_Jump, MUIV_NList_Jump_Bottom);
    DoMethod(w->list, MUIM_NList_Redraw, MUIV_NList_Redraw_All);   /* no stale rows between the strips */
}

static void free_media(ChatWin *w)
{
    int i, k;
    for (i = 0; i < w->n_media; i++) {
        Media *m = &w->media[i];
        for (k = 0; k < m->n_slices; k++) {
            if (m->obj && m->obj[k]) MUI_DisposeObject(m->obj[k]);
            if (m->ad) free(m->ad[k]);
        }
        free(m->obj); free(m->ad);
        free(m->eid); free(m->media); free(m->name); free(m->type); free(m->path); free(m->saving);
    }
    free(w->media);
    for (i = 0; i < w->n_links; i++) free(w->links[i]);
    free(w->links);
}

/* the text of a message for the list: pictures/files and links clickable */
static char *decorate(ChatWin *w, const AmxEvent *e, char *text)
{
    if (e->type != AMX_EV_MESSAGE) return text;
    if (e->media && e->event_id) {
        char *t = media_text(w, e, text);
        if (t) { free(text); text = t; }
    }
    return linkify(w, text);
}

/* ---------------------------------------------------------------- messages: quotes, edits, deleting, dates */

static ChatMsg *find_msg(ChatWin *w, const char *eid)
{
    int i;
    for (i = 0; eid && i < w->n_msgs; i++) if (!strcmp(w->msgs[i].eid, eid)) return &w->msgs[i];
    return NULL;
}

static char *dupn(const char *s) { return s ? strdup(s) : NULL; }

static ChatMsg *add_msg(ChatWin *w, const AmxEvent *e)
{
    ChatMsg *m;
    int i;
    if (w->n_msgs == w->cap_msgs) {
        ChatMsg *t = realloc(w->msgs, sizeof *t * (w->cap_msgs ? w->cap_msgs * 2 : 64));
        if (!t) return NULL;
        w->msgs = t;
        w->cap_msgs = w->cap_msgs ? w->cap_msgs * 2 : 64;
    }
    m = &w->msgs[w->n_msgs++];
    memset(m, 0, sizeof *m);
    m->eid = dupn(e->event_id);
    m->sender = dupn(e->sender);
    m->name = dupn(e->sender_name ? e->sender_name : e->sender);
    m->body = dupn(e->body ? e->body : "");
    m->formatted = dupn(e->formatted);
    m->mention = e->mention;
    m->msgtype = dupn(e->msgtype);
    m->reply_to = dupn(e->reply_to);
    m->thread_root = dupn(e->thread_root);
    m->ts = e->timestamp;
    m->own = e->own;
    m->redacted = e->redacted;
    if (m->own) {
        const Room *r = room_by_id(w->room_id);
        m->read = r && m->ts > 0 && m->ts <= r->read_ts;
    }
    for (i = 0; i < w->n_pedit; i++)             /* an edit that came first (history) */
        if (!strcmp(w->pedit[i].eid, m->eid)) {
            free(m->body); m->body = dupn(w->pedit[i].body);
            free(m->formatted); m->formatted = dupn(w->pedit[i].formatted);
            free(m->msgtype); m->msgtype = dupn(w->pedit[i].msgtype);
            m->edited = 1;
            m->edit_ts = w->pedit[i].ts;
        }
    return m;
}

static void free_msgs(ChatWin *w)
{
    int i;
    for (i = 0; i < w->n_msgs; i++) {
        ChatMsg *m = &w->msgs[i];
        free(m->eid); free(m->sender); free(m->name); free(m->body); free(m->msgtype); free(m->formatted);
        free(m->reply_to); free(m->thread_root);
    }
    free(w->msgs);
    for (i = 0; i < w->n_pedit; i++) { free(w->pedit[i].eid); free(w->pedit[i].body); free(w->pedit[i].msgtype); free(w->pedit[i].formatted); }
    free(w->pedit);
    free(w->reply_eid);
    free(w->edit_eid);
}

/* "» Name: the beginning of the message" for a reply */
static char *quote_line(ChatWin *w, const char *eid)
{
    ChatMsg *m = find_msg(w, eid);
    char *name, *body, *out, *q;
    size_t n;
    if (!m) return strdup("\33i  \xbb (eine ältere Nachricht)");
    name = local(m->name ? m->name : "");
    body = local_emoji(m->redacted ? "(gel\xc3\xb6scht)" : m->body);              /* UTF-8 */
    for (q = body; *q; q++) if (*q == '\n') *q = ' ';
    n = strlen(name) + 80 + 1;
    out = malloc(n + strlen(body));
    if (out) {
        if (strlen(body) > 60) { body[57] = 0; strcat(body, "..."); }
        snprintf(out, n + strlen(body), "\33i  \xbb %s: %s", name, body);
    }
    free(name); free(body);
    return out;
}

/* the full text of a message: quote, time, name, body, "(bearbeitet)" */
static char *message_text(ChatWin *w, const AmxEvent *e, int edited, int redacted)
{
    char *text, *quote, *out;
    AmxEvent x = *e;
    if (redacted) { x.msgtype = "m.notice"; x.body = "(Nachricht gel\xc3\xb6scht)"; x.media = NULL; x.formatted = NULL; }   /* UTF-8 */
    if (!(text = format_event(&x))) return NULL;
    if (!redacted) text = decorate(w, &x, text);
    if (opt_compact && text && e->type == AMX_EV_MESSAGE && e->sender && e->timestamp > 0) {
        const ChatMsg *prev = NULL;                 /* the message before it */
        int i;
        for (i = 0; i < w->n_msgs; i++)
            if (w->msgs[i].ts < e->timestamp && w->msgs[i].ts > 0 && (!prev || w->msgs[i].ts > prev->ts)) prev = &w->msgs[i];
        if (prev && prev->sender && !strcmp(prev->sender, e->sender) && e->timestamp - prev->ts < 300000.0) {
            char *b = strstr(text, "\33b"), *c = b ? strstr(b, ":\33n ") : NULL;
            if (b && c && c - b < 200) memmove(b, c + 4, strlen(c + 4) + 1);   /* "\33bName:\33n " away */
        }
    }
    if (edited && !redacted && text) {
        char *t = malloc(strlen(text) + 20);
        if (t) { sprintf(t, "%s \33i(bearbeitet)", text); free(text); text = t; }
    }
    if (!redacted && text && e->own && e->event_id) {          /* read by someone: a check mark */
        ChatMsg *m = find_msg(w, e->event_id);
        if (m && m->read) {
            char *t = malloc(strlen(text) + 20);
            if (t) { sprintf(t, "%s \33n\33o[%d]", text, EMO_CHECK); free(text); text = t; }
        }
    }
    if (!x.reply_to || redacted || !text) return text;
    if (!(quote = quote_line(w, x.reply_to))) return text;
    out = malloc(strlen(quote) + strlen(text) + 2);
    if (out) sprintf(out, "%s\n%s", quote, text);
    free(quote);
    free(text);
    return out;
}

/* a message changed (edited, deleted): its lines are rebuilt in place */
static void rerender(ChatWin *w, ChatMsg *m)
{
    AmxEvent e;
    LONG n, k, first = -1;
    char *text;
    int again;
    memset(&e, 0, sizeof e);
    e.type = AMX_EV_MESSAGE;
    e.room_id = w->room_id;
    e.event_id = m->eid;
    e.sender = m->sender;
    e.sender_name = m->name;
    e.body = m->body;
    e.formatted = m->formatted;
    e.mention = m->mention;
    e.msgtype = m->msgtype;
    e.reply_to = m->reply_to;
    e.timestamp = m->ts;
    e.own = m->own;
    e.history = 1;
    do {                                         /* away with the old lines (and pictures) */
        again = 0;
        n = (LONG)xget(w->list, MUIA_NList_Entries);
        for (k = 0; k < n; k++) {
            Line *l = NULL;
            DoMethod(w->list, MUIM_NList_GetEntry, k, &l);
            if (!l || !l->eid || strcmp(l->eid, m->eid) || l->react || l->thr || (l->slice && !m->redacted)
                || ((l->prev || l->pollrow) && !m->redacted)) continue;
            if (first < 0 || k < first) first = k;
            DoMethod(w->list, MUIM_NList_Remove, k);
            w->lines--;
            again = 1;
            break;
        }
    } while (again);
    if (first < 0) return;
    if (!(text = message_text(w, &e, m->edited, m->redacted))) return;
    insert_text(w, m->eid, text, first, 0, m->own);
    free(text);
}

/* and the replies quoting it */
static void rerender_all(ChatWin *w, ChatMsg *m)
{
    int i;
    char *eid = strdup(m->eid);
    rerender(w, m);
    for (i = 0; eid && i < w->n_msgs; i++)
        if (w->msgs[i].reply_to && !strcmp(w->msgs[i].reply_to, eid)) rerender(w, &w->msgs[i]);
    free(eid);
}

/* every message again (a display option changed) */
static void rerender_window(ChatWin *w)
{
    int i;
    set(w->list, MUIA_NList_Quiet, TRUE);
    for (i = 0; i < w->n_msgs; i++) rerender(w, &w->msgs[i]);
    set(w->list, MUIA_NList_Quiet, FALSE);
}

static void edit_event(ChatWin *w, const AmxEvent *e)
{
    ChatMsg *m = find_msg(w, e->relates_to);
    int i;
    if (w->n_undec) remove_placeholder(w, e->event_id);
    if (m) {
        if (!m->sender || !e->sender || strcmp(m->sender, e->sender)) return;   /* only the author */
        if (m->edited && e->timestamp < m->edit_ts) return;                    /* an older edit */
        free(m->body); m->body = dupn(e->body);
        free(m->formatted); m->formatted = dupn(e->formatted);
        free(m->msgtype); m->msgtype = dupn(e->msgtype);
        m->edited = 1;
        m->edit_ts = e->timestamp;
        rerender_all(w, m);
        return;
    }
    for (i = 0; i < w->n_pedit; i++)             /* the message comes later (history) */
        if (!strcmp(w->pedit[i].eid, e->relates_to)) {
            if (e->timestamp < w->pedit[i].ts) return;
            free(w->pedit[i].body); w->pedit[i].body = dupn(e->body);
            free(w->pedit[i].formatted); w->pedit[i].formatted = dupn(e->formatted);
            free(w->pedit[i].msgtype); w->pedit[i].msgtype = dupn(e->msgtype);
            w->pedit[i].ts = e->timestamp;
            return;
        }
    if (w->n_pedit == w->cap_pedit) {
        struct PendingEdit *t = realloc(w->pedit, sizeof *t * (w->cap_pedit ? w->cap_pedit * 2 : 16));
        if (!t) return;
        w->pedit = t;
        w->cap_pedit = w->cap_pedit ? w->cap_pedit * 2 : 16;
    }
    w->pedit[w->n_pedit].eid = dupn(e->relates_to);
    w->pedit[w->n_pedit].body = dupn(e->body);
    w->pedit[w->n_pedit].formatted = dupn(e->formatted);
    w->pedit[w->n_pedit].msgtype = dupn(e->msgtype);
    w->pedit[w->n_pedit].ts = e->timestamp;
    w->n_pedit++;
}

/* a message was deleted */
static int deleted_event(ChatWin *w, const AmxEvent *e)
{
    ChatMsg *m = find_msg(w, e->relates_to);
    if (!m || m->redacted) return m != NULL;
    m->redacted = 1;
    rerender_all(w, m);
    return 1;
}

/* ---- date separators: "— Samstag, 7. November 2026 —" where the day changes */

static long day_of(double ms)
{
    time_t t = (time_t)(ms / 1000);
    struct tm *tm = localtime(&t);
    return tm ? (tm->tm_year + 1900) * 10000L + (tm->tm_mon + 1) * 100 + tm->tm_mday : 0;
}

static void insert_sep(ChatWin *w, double ms, LONG pos)
{
    static const char *const wd[] = { "Sonntag", "Montag", "Dienstag", "Mittwoch", "Donnerstag", "Freitag", "Samstag" };
    static const char *const mo[] = { "Januar", "Februar", "März", "April", "Mai", "Juni", "Juli", "August",
                                      "September", "Oktober", "November", "Dezember" };
    time_t t = (time_t)(ms / 1000);
    struct tm *tm = localtime(&t);
    char text[96];
    Line *l;
    if (!tm) return;
    snprintf(text, sizeof text, "\33c\33i- %s, %d. %s %d -", wd[tm->tm_wday], tm->tm_mday, mo[tm->tm_mon],
             tm->tm_year + 1900);
    if (!(l = new_line(NULL, text, strlen(text), 0))) return;
    l->sep = 1;
    remember_line(w, l);
    DoMethod(w->list, MUIM_NList_InsertSingle, l, pos);
    w->lines++;
}

/* where a new message goes, with a separator if the day changes */
static LONG place_for(ChatWin *w, const AmxEvent *e)
{
    long d = e->timestamp > 0 ? day_of(e->timestamp) : 0;
    if (!d) return e->history ? MUIV_NList_Insert_Top : MUIV_NList_Insert_Bottom;
    if (!e->history) {
        if (w->bottom_day != d) {
            insert_sep(w, e->timestamp, MUIV_NList_Insert_Bottom);
            w->bottom_day = d;
            if (!w->top_day) w->top_day = d;
        }
        return MUIV_NList_Insert_Bottom;
    }
    if (!w->top_day) {                           /* the first: separator above it */
        insert_sep(w, e->timestamp, MUIV_NList_Insert_Top);
        w->top_day = w->bottom_day = d;
        return 1;
    }
    if (w->top_day == d) return 1;               /* same day: right under the separator */
    insert_sep(w, e->timestamp, MUIV_NList_Insert_Top);   /* an older day: its own separator */
    w->top_day = d;
    return 1;
}

/* ---- toolbar: reply, edit, delete the marked message */

static ChatMsg *marked_msg(ChatWin *w)
{
    Line *l = NULL;
    DoMethod(w->list, MUIM_NList_GetEntry, MUIV_NList_GetEntry_Active, &l);
    return l && l->eid ? find_msg(w, l->eid) : NULL;
}

/* ---- the clipboard (unit 0, IFF FTXT), for "copy text" */

static int clip_write(const char *text)
{
    struct Library *IFFParseBase = OpenLibrary("iffparse.library", 37);
    struct IFFHandle *iff;
    int ok = 0;
    if (!IFFParseBase) return 0;
    if ((iff = AllocIFF()) != NULL) {
        if ((iff->iff_Stream = (ULONG)OpenClipboard(0)) != 0) {
            InitIFFasClip(iff);
            if (!OpenIFF(iff, IFFF_WRITE)) {
                LONG n = strlen(text);
                ok = !PushChunk(iff, MAKE_ID('F','T','X','T'), ID_FORM, IFFSIZE_UNKNOWN)
                     && !PushChunk(iff, 0, MAKE_ID('C','H','R','S'), IFFSIZE_UNKNOWN)
                     && WriteChunkBytes(iff, (APTR)text, n) == n
                     && !PopChunk(iff) && !PopChunk(iff);
                CloseIFF(iff);
            }
            CloseClipboard((struct ClipboardHandle *)iff->iff_Stream);
        }
        FreeIFF(iff);
    }
    CloseLibrary(IFFParseBase);
    return ok;
}

/* the text on the clipboard (first CHRS chunk), malloc'd, or NULL */
static char *clip_read(void)
{
    struct Library *IFFParseBase = OpenLibrary("iffparse.library", 37);
    struct IFFHandle *iff;
    char *t = NULL;
    if (!IFFParseBase) return NULL;
    if ((iff = AllocIFF()) != NULL) {
        if ((iff->iff_Stream = (ULONG)OpenClipboard(0)) != 0) {
            InitIFFasClip(iff);
            if (!OpenIFF(iff, IFFF_READ)) {
                if (!StopChunk(iff, MAKE_ID('F','T','X','T'), MAKE_ID('C','H','R','S')) && !ParseIFF(iff, IFFPARSE_SCAN)) {
                    struct ContextNode *cn = CurrentChunk(iff);
                    if (cn && (t = malloc(cn->cn_Size + 1)) != NULL) {
                        LONG n = ReadChunkBytes(iff, t, cn->cn_Size);
                        t[n > 0 ? n : 0] = 0;
                    }
                }
                CloseIFF(iff);
            }
            CloseClipboard((struct ClipboardHandle *)iff->iff_Stream);
        }
        FreeIFF(iff);
    }
    CloseLibrary(IFFParseBase);
    return t;
}

/* copies a message's text (in the Amiga's character set) */
static void copy_msg(ChatWin *w, ChatMsg *m)
{
    char *l;
    if (!m || m->redacted || !m->body) return;
    l = local(m->body);
    snprintf(w->err, sizeof w->err, clip_write(l) ? "Text kopiert" : "Kopieren hat nicht geklappt");
    free(l);
    update_status(w);
}

/* the chat window the user works in: the active one, else the newest */
static ChatWin *current_chat(void)
{
    ChatWin *w;
    for (w = chats; w; w = w->next) if (xget(w->win, MUIA_Window_Activate)) return w;
    return chats;
}

HOOKPROTONH(chat_active_func, LONG, Object *obj, ULONG *param)
{
    ChatWin *w = (ChatWin *)param[0];
    ChatMsg *m = marked_msg(w);
    (void)obj;
    set(w->replyb, MUIA_Disabled, !m || m->redacted);
    set(w->editb, MUIA_Disabled, !m || !m->own || m->redacted || (m->msgtype && strcmp(m->msgtype, "m.text")
                                                                 && strcmp(m->msgtype, "m.emote")));
    set(w->delb, MUIA_Disabled, !m || !m->own || m->redacted);
    return 0;
}
static struct Hook chat_active_hook = { { NULL, NULL }, (HOOKFUNC)chat_active_func, NULL, NULL };

static void start_reply(ChatWin *w, ChatMsg *m)
{
    char *q = quote_line(w, m->eid), b[300];
    clear_mode(w);
    w->reply_eid = strdup(m->eid);
    snprintf(b, sizeof b, "Antwort auf%s", q ? q + 5 : "");          /* without "\33i  " */
    set(w->modetext, MUIA_Text_Contents, b);
    free(q);
    set(w->modegrp, MUIA_ShowMe, TRUE);
    set(w->win, MUIA_Window_ActiveObject, w->input);
}

static void start_edit(ChatWin *w, ChatMsg *m)
{
    char *l = local(m->body);
    clear_mode(w);
    w->edit_eid = strdup(m->eid);
    set(w->modetext, MUIA_Text_Contents, "Nachricht bearbeiten (Return speichert)");
    set(w->modegrp, MUIA_ShowMe, TRUE);
    set(w->input, MUIA_String_Contents, l);
    free(l);
    set(w->win, MUIA_Window_ActiveObject, w->input);
}

static void delete_msg(ChatWin *w, ChatMsg *m, int ask)
{
    if (ask && MUI_Request(app, w->win, 0, (STRPTR)"Amatrix", (STRPTR)"*_Löschen|_Abbrechen",
                           (STRPTR)"Diese Nachricht für alle löschen?", 0) != 1)
        return;
    amx_async_redact(A, w->room_id, m->eid, TAG_REACT);
}

HOOKPROTONH(chat_reply_func, LONG, Object *obj, ULONG *param)
{
    ChatWin *w = (ChatWin *)param[0];
    ChatMsg *m = marked_msg(w);
    (void)obj;
    if (m && !m->redacted) start_reply(w, m);
    return 0;
}
static struct Hook chat_reply_hook = { { NULL, NULL }, (HOOKFUNC)chat_reply_func, NULL, NULL };

HOOKPROTONH(chat_edit_func, LONG, Object *obj, ULONG *param)
{
    ChatWin *w = (ChatWin *)param[0];
    ChatMsg *m = marked_msg(w);
    (void)obj;
    if (m && m->own && !m->redacted) start_edit(w, m);
    return 0;
}
static struct Hook chat_edit_hook = { { NULL, NULL }, (HOOKFUNC)chat_edit_func, NULL, NULL };

HOOKPROTONH(chat_del_func, LONG, Object *obj, ULONG *param)
{
    ChatWin *w = (ChatWin *)param[0];
    ChatMsg *m = marked_msg(w);
    (void)obj;
    if (m && m->own && !m->redacted) delete_msg(w, m, 1);
    return 0;
}
static struct Hook chat_del_hook = { { NULL, NULL }, (HOOKFUNC)chat_del_func, NULL, NULL };

HOOKPROTONH(chat_modex_func, LONG, Object *obj, ULONG *param)
{
    ChatWin *w = (ChatWin *)param[0];
    (void)obj;
    if (w->edit_eid) set(w->input, MUIA_String_Contents, "");
    clear_mode(w);
    return 0;
}
static struct Hook chat_modex_hook = { { NULL, NULL }, (HOOKFUNC)chat_modex_func, NULL, NULL };

/* "Thread: n Antworten" under a message that starts a thread (room windows) */
static void refresh_thread_line(ChatWin *w, ChatMsg *m)
{
    char text[96];
    LONG n = (LONG)xget(w->list, MUIA_NList_Entries), k, last = -1, tl = -1;
    Line *tline = NULL;
    if (w->thread_root || m->thread_count <= 0) return;
    for (k = 0; k < n; k++) {
        Line *l = NULL;
        DoMethod(w->list, MUIM_NList_GetEntry, k, &l);
        if (!l || !l->eid || strcmp(l->eid, m->eid) || l->react) continue;
        if (l->thr) { tl = k; tline = l; }
        else last = k;
    }
    if (last < 0) return;
    snprintf(text, sizeof text, "      \33o[%d@%ld] \33iThread: %d %s", EMO_THREAD, CLICK_THREAD | (long)(m - w->msgs),
             m->thread_count, m->thread_count == 1 ? "Antwort" : "Antworten");
    if (tline) {
        char *t = strdup(text);
        if (!t) return;
        free(tline->text);
        tline->text = t;
        DoMethod(w->list, MUIM_NList_ReplaceSingle, tline, tl, NOWRAP, ALIGN_LEFT);
    } else {
        Line *l = new_line(m->eid, text, strlen(text), 0);
        if (!l) return;
        l->thr = 1;
        remember_line(w, l);
        DoMethod(w->list, MUIM_NList_InsertSingle, l, last + 1);
        w->lines++;
    }
}

static void open_thread_of(ChatWin *w, int mi)
{
    if (mi < 0 || mi >= w->n_msgs) return;
    open_chat_ex(w->room_id, w->msgs[mi].eid);
}

static void chat_event_in(ChatWin *w, const AmxEvent *e);

/* who is typing: the room's window (and its thread windows) show it */
static void typing_event(const AmxEvent *e)
{
    ChatWin *w;
    char *l = local(e->body ? e->body : ""), *p;
    for (p = l; p && *p; p++) if (*p == '\n') *p = ',';
    for (w = chats; w; w = w->next)
        if (!strcmp(w->room_id, e->room_id)) {
            char *d = w->typing, *q;
            size_t n = 0;
            for (q = l; q && *q && n + 2 < sizeof w->typing; q++) {    /* "a,b" -> "a, b" */
                d[n++] = *q;
                if (*q == ',' && n + 2 < sizeof w->typing) d[n++] = ' ';
            }
            d[n] = 0;
            update_status(w);
        }
    free(l);
}

/* a read receipt of someone else: our messages up to it get a check mark */
static void receipt_event(const AmxEvent *e)
{
    Room *r = room_by_id(e->room_id);
    ChatWin *w;
    double ts = e->timestamp;
    int i;
    if (!r) return;
    for (w = chats; w; w = w->next)
        if (!strcmp(w->room_id, e->room_id)) {
            ChatMsg *m = find_msg(w, e->event_id);
            if (m && m->ts > 0) { ts = m->ts; break; }
        }
    if (ts <= r->read_ts) return;
    r->read_ts = ts;
    for (w = chats; w; w = w->next) {
        if (strcmp(w->room_id, e->room_id)) continue;
        for (i = 0; i < w->n_msgs; i++)
            if (w->msgs[i].own && !w->msgs[i].read && !w->msgs[i].redacted && w->msgs[i].ts > 0
                && w->msgs[i].ts <= ts) {
                w->msgs[i].read = 1;
                rerender(w, &w->msgs[i]);
            }
    }
}

/* an event to the room's window and to its open thread windows */
static void chat_event(const AmxEvent *e)
{
    ChatWin *w, *tw;
    if (!e->room_id) return;
    w = chat_by_room(e->room_id);
    if (e->type == AMX_EV_MESSAGE && e->thread_root) {          /* a message in a thread */
        if (w && !e->history && e->event_id && !seen_before(w, e->event_id)) {
            ChatMsg *root = find_msg(w, e->thread_root);
            if (root) { root->thread_count++; refresh_thread_line(w, root); }
        }
        if ((tw = chat_thread(e->room_id, e->thread_root)) != NULL) chat_event_in(tw, e);
        return;
    }
    if (w) chat_event_in(w, e);
    if (e->type == AMX_EV_MEMBERSHIP) return;
    for (tw = chats; tw; tw = tw->next)                          /* the root, edits, reactions, deleting */
        if (tw->thread_root && !strcmp(tw->room_id, e->room_id)
            && (e->type != AMX_EV_MESSAGE || (e->event_id && !strcmp(e->event_id, tw->thread_root))))
            chat_event_in(tw, e);
}

static void chat_event_in(ChatWin *w, const AmxEvent *e)
{
    char *text;
    amx_log("chat_event type %d %s hist %d body %ld", (int)e->type, e->event_id ? e->event_id : "-", e->history,
            (long)(e->body ? strlen(e->body) : -1));
    if (e->type == AMX_EV_EDIT) {
        edit_event(w, e);
        return;
    }
    if (e->type == AMX_EV_REDACTION && e->relates_to && deleted_event(w, e)) return;
    if (e->type == AMX_EV_REACTION || e->type == AMX_EV_REDACTION) {
        reaction_event(w, e);
        return;
    }
    if (e->type == AMX_EV_POLL_VOTE || e->type == AMX_EV_POLL_END) {
        poll_event(w, e);
        return;
    }
    if (e->type == AMX_EV_MESSAGE && e->event_id && w->n_undec) {
        ChatMsg *m = find_msg(w, e->event_id) ? NULL : add_msg(w, e);
        if ((text = message_text(w, e, m && m->edited, e->redacted)) != NULL
            && replace_undecryptable(w, e, text)) {
            free(text);
            note_newest(w, e);
            return;
        }
        free(text);
    }
    if (seen_before(w, e->event_id)) return;
    if (e->type == AMX_EV_MEMBERSHIP && e->history) return;     /* keep old history quiet */
    if (e->type == AMX_EV_MEMBERSHIP && e->body && !strcmp(e->body, "knock") && e->state_key) {
        if (!opt_joins) {
            char *n = local(e->sender_name ? e->sender_name : e->state_key);
            snprintf(w->err, sizeof w->err, "%s klopft an - im Raum-Fenster (i) einlassen", n ? n : "");
            free(n);
            update_status(w);
            return;
        }
        char *n = local(e->sender_name ? e->sender_name : e->state_key);
        snprintf(w->err, sizeof w->err, "%s klopft an - im Raum-Fenster (i) einlassen", n ? n : "");
        free(n);
        update_status(w);
    }
    if (e->type == AMX_EV_MEMBERSHIP && !opt_joins) return;
    if (e->cached) w->n_cached++;
    if (e->type == AMX_EV_MESSAGE) {
        ChatMsg *m = find_msg(w, e->event_id);
        if (!m) m = add_msg(w, e);
        text = message_text(w, e, m && m->edited, e->redacted);
        if (text && m && m->edited) {                    /* an edit came first: show the new text */
            AmxEvent x = *e;
            x.body = m->body;
            x.formatted = m->formatted;
            x.msgtype = m->msgtype;
            free(text);
            text = message_text(w, &x, 1, e->redacted);
        }
    } else text = format_event(e);
    if (!text) return;
    insert_text(w, e->event_id, text, place_for(w, e), e->type == AMX_EV_UNDECRYPTABLE && e->event_id, e->own);
    amx_log("  inserted (%ld lines)", (long)w->lines);
    free(text);
    if (e->type == AMX_EV_MESSAGE && e->thread_count > 0 && !w->thread_root) {
        ChatMsg *m = find_msg(w, e->event_id);
        if (m) {
            if (e->thread_count > m->thread_count) m->thread_count = e->thread_count;
            refresh_thread_line(w, m);
        }
    }
    if (e->type == AMX_EV_MESSAGE && !e->redacted) want_preview(w, find_msg(w, e->event_id));
    if (e->type == AMX_EV_MESSAGE && e->poll && !e->redacted) add_poll(w, e);
    if (has_reactions(w, e->event_id)) refresh_reactions(w, e->event_id);   /* came first (history) */
    if (!e->history) DoMethod(w->list, MUIM_NList_Jump, MUIV_NList_Jump_Bottom);
    note_newest(w, e);
}

/* ---- jumps: to a date (older pages until there), to the first unread message */

static void load_older_page(ChatWin *w)
{
    w->loading = 1;
    w->loading_older = 1;
    set(w->older, MUIA_Disabled, TRUE);
    amx_async_load_history(A, w->room_id, OLDER_PAGE, 0, TAG_HIST);
    update_status(w);
}

/* the list line where message m starts */
static LONG line_of(ChatWin *w, const ChatMsg *m)
{
    LONG n = (LONG)xget(w->list, MUIA_NList_Entries), k;
    for (k = 0; m && k < n; k++) {
        Line *l = NULL;
        DoMethod(w->list, MUIM_NList_GetEntry, k, &l);
        if (l && l->eid && !strcmp(l->eid, m->eid)) return k;
    }
    return -1;
}

static void jump_step(ChatWin *w)
{
    double oldest = 0;
    ChatMsg *best = NULL;
    int i;
    for (i = 0; i < w->n_msgs; i++)
        if (w->msgs[i].ts > 0 && (oldest == 0 || w->msgs[i].ts < oldest)) oldest = w->msgs[i].ts;
    if (oldest > w->jump_ts && !w->at_start && w->jump_pages < 30) {
        w->jump_pages++;
        load_older_page(w);
        return;
    }
    for (i = 0; i < w->n_msgs; i++)
        if (w->msgs[i].ts >= w->jump_ts && (!best || w->msgs[i].ts < best->ts)) best = &w->msgs[i];
    if (best) {
        LONG k = line_of(w, best);
        if (k > 0) k--;                             /* with the date above it */
        DoMethod(w->list, MUIM_NList_Jump, k);
        set(w->list, MUIA_NList_First, k);
    }
    snprintf(w->err, sizeof w->err, oldest > w->jump_ts ? "So weit zurück geht der Verlauf nicht (oder ist zu lang)."
                                                        : best ? "" : "Danach gibt es keine Nachrichten.");
    w->jump_ts = 0;
    update_status(w);
}

static void jump_to_date(ChatWin *w, const char *date)
{
    int d = 0, mo = 0, y = 0;
    struct tm tm;
    time_t t;
    if ((sscanf(date, "%d.%d.%d", &d, &mo, &y) != 3 && sscanf(date, "%d-%d-%d", &y, &mo, &d) != 3)
        || mo < 1 || mo > 12 || d < 1 || d > 31) {
        chat_say(w, "Datum bitte als TT.MM.JJJJ.");
        return;
    }
    if (y < 100) y += 2000;
    memset(&tm, 0, sizeof tm);
    tm.tm_mday = d; tm.tm_mon = mo - 1; tm.tm_year = y - 1900; tm.tm_isdst = -1;
    if ((t = mktime(&tm)) == (time_t)-1) { chat_say(w, "Das Datum geht nicht."); return; }
    w->jump_ts = (double)t * 1000.0;
    w->jump_pages = 0;
    chat_say(w, "Suche den Tag ...");
    if (!w->loading) jump_step(w);
}

static int by_ts_desc(const void *a, const void *b)
{
    const ChatMsg *x = *(ChatMsg *const *)a, *y = *(ChatMsg *const *)b;
    return x->ts < y->ts ? 1 : x->ts > y->ts ? -1 : 0;
}

/* "- neue Nachrichten -" above the first unread message, and show it */
static void mark_first_unread(ChatWin *w)
{
    ChatMsg **l;
    int i, n = 0;
    LONG k;
    Line *sep;
    static const char text[] = "\33c\33b--- neue Nachrichten ---";
    if (!(l = malloc(sizeof *l * (w->n_msgs + 1)))) return;
    for (i = 0; i < w->n_msgs; i++) if (!w->msgs[i].own && w->msgs[i].ts > 0) l[n++] = &w->msgs[i];
    w->unread_done = 1;
    if (!n) { free(l); return; }
    if (n < w->unread_open && !w->at_start)       /* more unread than loaded: the marker goes on top */
        chat_say(w, "Weitere ungelesene Nachrichten: Pfeil nach oben lädt sie.");
    qsort(l, n, sizeof *l, by_ts_desc);
    k = line_of(w, l[(w->unread_open < n ? w->unread_open : n) - 1]);
    free(l);
    if (k < 0 || !(sep = new_line(NULL, text, strlen(text), 0))) return;
    sep->newmark = 1;
    remember_line(w, sep);
    DoMethod(w->list, MUIM_NList_InsertSingle, sep, k);
    w->lines++;
    DoMethod(w->list, MUIM_NList_Jump, k > 0 ? k - 1 : 0);
    if ((LONG)xget(w->list, MUIA_NList_First) > k) set(w->list, MUIA_NList_First, k);
}

/* ---- export: the loaded messages (or the whole history) as text or HTML */

static int by_ts_asc(const void *a, const void *b)
{
    const ChatMsg *x = *(ChatMsg *const *)a, *y = *(ChatMsg *const *)b;
    return x->ts < y->ts ? -1 : x->ts > y->ts;
}

static void html_put(FILE *f, const char *t)
{
    for (; t && *t; t++) {
        if (*t == '<') fputs("&lt;", f);
        else if (*t == '>') fputs("&gt;", f);
        else if (*t == '&') fputs("&amp;", f);
        else if (*t == '\n') fputs("<br>", f);
        else fputc(*t, f);
    }
}

static void write_export(ChatWin *w, const char *path)
{
    FILE *f = fopen(path, "w");
    ChatMsg **l;
    int i, n = 0, html;
    size_t pl = strlen(path);
    const Room *r = room_by_id(w->room_id);
    html = pl > 5 && (!strcmp(path + pl - 5, ".html") || !strcmp(path + pl - 4, ".htm"));
    if (!f || !(l = malloc(sizeof *l * (w->n_msgs + 1)))) {
        if (f) fclose(f);
        chat_say(w, "Die Datei lässt sich nicht schreiben.");
        return;
    }
    for (i = 0; i < w->n_msgs; i++) if (!w->msgs[i].redacted && w->msgs[i].body) l[n++] = &w->msgs[i];
    qsort(l, n, sizeof *l, by_ts_asc);
    if (html) {
        char *nm = r ? amx_from_local(r->name) : NULL;
        fputs("<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>", f);
        html_put(f, nm ? nm : w->room_id);
        fputs("</title><style>body{font-family:sans-serif}.t{color:#888}.n{font-weight:bold}</style></head><body>\n<h1>", f);
        html_put(f, nm ? nm : w->room_id);
        fputs("</h1>\n", f);
        free(nm);
    } else fprintf(f, "Amatrix - %s\n\n", r ? r->name : w->room_id);
    for (i = 0; i < n; i++) {
        char when[24];
        time_t tt = (time_t)(l[i]->ts / 1000);
        struct tm *tm = localtime(&tt);
        const char *name = l[i]->name ? l[i]->name : l[i]->sender;      /* UTF-8 */
        if (tm) strftime(when, sizeof when, "%d.%m.%Y %H:%M", tm); else when[0] = 0;
        if (html) {
            fprintf(f, "<p><span class=\"t\">%s</span> <span class=\"n\">", when);
            html_put(f, name);
            fputs(":</span> ", f);
            html_put(f, l[i]->body);
            fputs("</p>\n", f);
        } else {
            char *ln = local(name ? name : ""), *lb = local(l[i]->body);
            fprintf(f, "%s %s: %s\n", when, ln ? ln : "", lb ? lb : "");
            free(ln); free(lb);
        }
    }
    if (html) fputs("</body></html>\n", f);
    fclose(f);
    free(l);
    {
        char b[200];
        snprintf(b, sizeof b, "%d Nachrichten exportiert.", n);
        chat_say(w, b);
    }
}

static void export_step(ChatWin *w)
{
    if (!w->at_start && w->jump_pages < 60) { w->jump_pages++; load_older_page(w); return; }
    write_export(w, w->export_path);
    free(w->export_path);
    w->export_path = NULL;
}

static void export_chat(ChatWin *w, const char *path, int all)
{
    if (!all) { write_export(w, path); return; }
    free(w->export_path);
    w->export_path = strdup(path);
    w->jump_pages = 0;
    chat_say(w, "Lade den ganzen Verlauf ...");
    if (!w->loading) export_step(w);
}

static void history_done(const AmxMsg *m)
{
    ChatWin *w = m->result ? chat_by_room(m->result) : NULL;
    amx_log("history_done %s status %d count %d at_start %d", m->result ? m->result : "-", m->status, m->count,
            m->at_start);
    if (!w) return;
    w->loading = 0;
    w->loading_older = 0;
    if (m->status) snprintf(w->err, sizeof w->err, "Verlauf: %.290s", m->error ? m->error : "Fehler");
    update_status(w);
    w->at_start = m->status == 0 && m->at_start;
    set(w->older, MUIA_Disabled, w->at_start);
    if (w->at_start) set(w->older, MUIA_ShortHelp, OLDER_HELP_END);
    if (m->status) return;
    if (w->jump_ts > 0) { jump_step(w); return; }
    if (w->export_path) { export_step(w); return; }
    if (w->unread_open > 0 && !w->unread_done && w->n_msgs) {
        DoMethod(w->list, MUIM_NList_Jump, MUIV_NList_Jump_Bottom);
        mark_first_unread(w);
        return;
    }
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
    if (A && !opt_receipts) amx_async_private_receipts(A, 1);
    if (A && !opt_hcache) amx_async_history_cache(A, 0);
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
                             (STRPTR)"*_Annehmen|A_blehnen|Ablehnen + _blockieren|_Später",
                             (STRPTR)"%s lädt dich ein:\n\n\33b%s", (ULONG)(r->inviter ? r->inviter : "Jemand"),
                             (ULONG)r->name);
        if (k == 1) accept_invite(r, 1);
        else if (k == 2) accept_invite(r, 0);
        else if (k == 3) {                          /* no more invitations / messages from them */
            if (r->inviter_id) amx_async_ignore(A, r->inviter_id, 1, TAG_CMD);
            accept_invite(r, 0);
        }
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

/* ---- pinned messages: the bar above the messages shows the newest pin */

typedef struct { char *eid, *text; } PinText;     /* text: local charset, one line */
static PinText pin_texts[40];
static int n_pin_texts;

static const char *pin_text_of(const char *eid)
{
    int i;
    for (i = 0; i < n_pin_texts; i++) if (!strcmp(pin_texts[i].eid, eid)) return pin_texts[i].text;
    return NULL;
}

static void remember_pin_text(const char *eid, const char *body_utf8)
{
    char *t, *p;
    if (pin_text_of(eid) || !body_utf8) return;
    if (n_pin_texts == 40) { free(pin_texts[0].eid); free(pin_texts[0].text); memmove(pin_texts, pin_texts + 1, sizeof *pin_texts * 39); n_pin_texts--; }
    t = local(body_utf8);
    for (p = t; p && *p; p++) if (*p == '\n') *p = ' ';
    if (t && strlen(t) > 120) strcpy(t + 117, "...");
    pin_texts[n_pin_texts].eid = strdup(eid);
    pin_texts[n_pin_texts].text = t;
    n_pin_texts++;
}

static void update_pinbar(ChatWin *w)
{
    const Room *r = room_by_id(w->room_id);
    set(w->tombbar, MUIA_ShowMe, r && r->replacement && !w->thread_root);
    const char *last, *pins = r && !w->thread_root ? r->pinned : NULL;
    char eid[300], text[300];
    int n = 0;
    ChatMsg *m;
    if (!pins || !*pins) { set(w->pinbar, MUIA_ShowMe, FALSE); free(w->pin_shown); w->pin_shown = NULL; return; }
    for (last = pins; ; ) {                         /* the last one is the newest */
        const char *nl = strchr(last, '\n');
        n++;
        if (!nl) break;
        last = nl + 1;
    }
    snprintf(eid, sizeof eid, "%s", last);
    free(w->pin_shown);
    w->pin_shown = strdup(eid);
    if ((m = find_msg(w, eid)) != NULL) remember_pin_text(eid, m->body);
    if (!pin_text_of(eid)) amx_async_fetch_event(A, w->room_id, eid, TAG_FETCH);
    snprintf(text, sizeof text, "\33bAngepinnt%s:\33n %s", n > 1 ? " (neueste)" : "",
             pin_text_of(eid) ? pin_text_of(eid) : "...");
    set(w->pinbar, MUIA_Text_Contents, text);
    set(w->pinbar, MUIA_ShowMe, TRUE);
}

/* the room was upgraded: to the new one (joined first if need be) */
static void go_replacement(ChatWin *w)
{
    const Room *r = room_by_id(w->room_id), *nr;
    if (!r || !r->replacement) return;
    nr = room_by_id(r->replacement);
    if (nr && !nr->invited) open_chat(nr->id);
    else {
        free(pending_open);
        pending_open = strdup(r->replacement);
        amx_async_join(A, r->replacement, TAG_JOIN);
        chat_say(w, "Trete dem neuen Raum bei ...");
    }
}

HOOKPROTONH(tomb_func, LONG, Object *obj, ULONG *param)
{
    (void)obj;
    go_replacement((ChatWin *)param[0]);
    return 0;
}
MakeStaticHook(tomb_hook, tomb_func);

HOOKPROTONH(pinbar_func, LONG, Object *obj, ULONG *param)
{
    ChatWin *w = (ChatWin *)param[0];
    ChatMsg *m = w->pin_shown ? find_msg(w, w->pin_shown) : NULL;
    LONG k = m ? line_of(w, m) : -1;
    (void)obj;
    if (k >= 0) { DoMethod(w->list, MUIM_NList_Jump, k); set(w->list, MUIA_NList_Active, k); }
    else chat_say(w, "Die angepinnte Nachricht ist älter als der geladene Verlauf (Pfeil nach oben lädt mehr).");
    return 0;
}
MakeStaticHook(pinbar_hook, pinbar_func);

/* ---- forward, quote, copy link, edit history (marked message) */

static Object *win_fwd, *lst_fwd, *b_fwd;
static char *fwd_room, *fwd_eid;
static Object *win_hist, *lst_hist;
static char *hist_eid;
typedef struct { double ts; char *text; } HistRow;
static HistRow hist_rows[40];
static int n_hist;

static void open_forward(ChatWin *w, ChatMsg *m)
{
    int i;
    free(fwd_room); free(fwd_eid);
    fwd_room = strdup(w->room_id);
    fwd_eid = strdup(m->eid);
    DoMethod(lst_fwd, MUIM_List_Clear);
    for (i = 0; i < n_rooms; i++)
        if (!rooms[i].invited && !rooms[i].is_space && strcmp(rooms[i].id, w->room_id))
            DoMethod(lst_fwd, MUIM_List_InsertSingle, &rooms[i], MUIV_List_Insert_Bottom);
    set(win_fwd, MUIA_Window_Open, TRUE);
}

HOOKPROTONHNO(fwd_disp_func, LONG, struct NList_DisplayMessage *m)
{
    const Room *r = m->entry;
    m->strings[0] = r ? r->name : (char *)"Raum";
    return 0;
}
MakeStaticHook(fwd_disp_hook, fwd_disp_func);

static void forward_go(void)
{
    Room *r = NULL;
    DoMethod(lst_fwd, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &r);
    if (!r || !fwd_room || !fwd_eid) return;
    amx_async_forward(A, r->id, fwd_room, fwd_eid, TAG_FWD);
    set(win_fwd, MUIA_Window_Open, FALSE);
}

static void start_quote(ChatWin *w, ChatMsg *m)
{
    char *q = quote_line(w, m->eid), b[300];
    clear_mode(w);
    w->quote = strdup(m->body ? m->body : "");
    snprintf(b, sizeof b, "Zitat:%s", q ? q + 5 : "");
    set(w->modetext, MUIA_Text_Contents, b);
    free(q);
    set(w->modegrp, MUIA_ShowMe, TRUE);
    set(w->win, MUIA_Window_ActiveObject, w->input);
}

static void copy_link(ChatWin *w, ChatMsg *m)
{
    char *l = malloc(strlen(w->room_id) + strlen(m->eid) + 40);
    if (!l) return;
    sprintf(l, "https://matrix.to/#/%s/%s", w->room_id, m->eid);
    snprintf(w->err, sizeof w->err, clip_write(l) ? "Link kopiert" : "Kopieren hat nicht geklappt");
    free(l);
    update_status(w);
}

static void hist_clear(void)
{
    int i;
    for (i = 0; i < n_hist; i++) free(hist_rows[i].text);
    n_hist = 0;
    DoMethod(lst_hist, MUIM_List_Clear);
}

static void open_edit_history(ChatWin *w, ChatMsg *m)
{
    free(hist_eid);
    hist_eid = strdup(m->eid);
    hist_clear();
    set(win_hist, MUIA_Window_Open, TRUE);
    amx_async_fetch_event(A, w->room_id, m->eid, TAG_FETCH);    /* the original */
    amx_async_edits(A, w->room_id, m->eid, TAG_EDITS);
}

static int hist_cmp(const void *a, const void *b)
{
    const HistRow *x = a, *y = b;
    return x->ts < y->ts ? -1 : x->ts > y->ts;
}

static void hist_add(double ts, const char *body)
{
    char when[24], *t, *row;
    time_t tt = (time_t)(ts / 1000);
    struct tm *tm = localtime(&tt);
    int i;
    if (n_hist >= 40) return;
    if (tm) strftime(when, sizeof when, "%d.%m. %H:%M", tm); else when[0] = 0;
    t = local(body ? body : "");
    row = malloc(strlen(t ? t : "") + 32);
    if (!row) { free(t); return; }
    sprintf(row, "\33b%s\33n  %s", when, t ? t : "");
    free(t);
    hist_rows[n_hist].ts = ts;
    hist_rows[n_hist].text = row;
    n_hist++;
    qsort(hist_rows, n_hist, sizeof *hist_rows, hist_cmp);
    DoMethod(lst_hist, MUIM_List_Clear);
    for (i = 0; i < n_hist; i++) DoMethod(lst_hist, MUIM_List_InsertSingle, hist_rows[i].text, MUIV_List_Insert_Bottom);
}

/* events fetched on request: a pin's text, the original / the edits of a message */
static void lookup_event(const AmxEvent *e)
{
    ChatWin *w;
    if (e->type == AMX_EV_MESSAGE && e->event_id) {
        remember_pin_text(e->event_id, e->body);
        for (w = chats; w; w = w->next)
            if (w->pin_shown && !strcmp(w->pin_shown, e->event_id)) update_pinbar(w);
        if (hist_eid && !strcmp(hist_eid, e->event_id)) hist_add(e->timestamp, e->body);
    } else if (e->type == AMX_EV_EDIT && hist_eid && e->relates_to && !strcmp(e->relates_to, hist_eid))
        hist_add(e->timestamp, e->body);
}

/* ---- ignored users: the list in the settings window */

static Object *lst_ign, *b_unign, *lst_kw, *str_kw, *b_kwadd, *b_kwdel;
static char *kw_list;

static void got_keywords(const AmxMsg *m)
{
    char *p;
    if (m->status) return;
    free(kw_list);
    kw_list = m->result ? strdup(m->result) : NULL;
    DoMethod(lst_kw, MUIM_List_Clear);
    for (p = kw_list; p && *p; ) {
        char *nl = strchr(p, '\n'), *l;
        if (nl) *nl = 0;
        l = local(p);                          /* the list keeps its own copy */
        DoMethod(lst_kw, MUIM_List_InsertSingle, l ? l : p, MUIV_List_Insert_Bottom);
        free(l);
        if (!nl) break;
        p = nl + 1;
    }
}
static char *ign_list;

static void got_ignored(const AmxMsg *m)
{
    char *p;
    free(ign_list);
    ign_list = m->status || !m->result ? NULL : strdup(m->result);
    DoMethod(lst_ign, MUIM_List_Clear);
    for (p = ign_list; p && *p; ) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        DoMethod(lst_ign, MUIM_List_InsertSingle, p, MUIV_List_Insert_Bottom);
        if (!nl) break;
        p = nl + 1;
    }
}

static void pw_go(void)
{
    char *o = (char *)xget(str_pwold, MUIA_String_Contents), *n1 = (char *)xget(str_pwnew, MUIA_String_Contents),
         *n2 = (char *)xget(str_pwnew2, MUIA_String_Contents), *uo, *un;
    if (!n1 || !n2 || strcmp(n1, n2)) { set(txt_acct, MUIA_Text_Contents, "Die neuen Passwörter sind nicht gleich."); return; }
    uo = amx_from_local(o ? o : "");
    un = amx_from_local(n1);
    amx_async_change_password(A, uo, un, TAG_PASSWORD);
    if (uo) memset(uo, 0, strlen(uo));
    if (un) memset(un, 0, strlen(un));
    free(uo); free(un);
    set(str_pwold, MUIA_String_Contents, "");
    set(str_pwnew, MUIA_String_Contents, "");
    set(str_pwnew2, MUIA_String_Contents, "");
    set(txt_acct, MUIA_Text_Contents, "Bitte warten ...");
}

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
        char *room = fifo_pop(fifo_send, &n_send), *tab = room ? strchr(room, '\t') : NULL;
        ChatWin *w;
        if (tab) { *tab = 0; w = chat_thread(room, tab + 1); }
        else w = room ? chat_by_room(room) : NULL;
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
    case TAG_FILE: {
        char *room = fifo_pop(fifo_send, &n_send), *tab = room ? strchr(room, '\t') : NULL;
        ChatWin *w;
        if (tab) { *tab = 0; w = chat_thread(room, tab + 1); }
        else w = room ? chat_by_room(room) : NULL;
        if (w) {
            if (w->uploading) w->uploading--;
            if (m->status) snprintf(w->err, sizeof w->err, "Datei nicht gesendet: %.280s", m->error ? m->error : "");
            update_status(w);
        } else if (m->status)
            MUI_Request(app, NULL, 0, (STRPTR)"Amatrix", (STRPTR)"*_OK",
                        (STRPTR)"Datei nicht gesendet:\n%s", (ULONG)(m->error ? m->error : ""));
        free(room);
        break;
    }
    case TAG_LEAVE:
        if (m->status)
            MUI_Request(app, win_main, 0, (STRPTR)"Amatrix", (STRPTR)"*_OK",
                        (STRPTR)"Verlassen hat nicht geklappt:\n%s", (ULONG)(m->error ? m->error : ""));
        set_online();
        break;
    case TAG_THREAD: {
        ChatWin *w;
        for (w = chats; w; w = w->next)
            if (w->thread_root && m->result && !strcmp(w->thread_root, m->result)) {
                w->loading = 0;
                w->loading_older = 0;
                if (m->status) snprintf(w->err, sizeof w->err, "Thread: %.140s", m->error ? m->error : "Fehler");
                w->at_start = m->status == 0 && m->at_start;
                set(w->older, MUIA_Disabled, w->at_start);
                update_status(w);
                DoMethod(w->list, MUIM_NList_Jump, MUIV_NList_Jump_Bottom);
            }
        break;
    }
    case TAG_THUMB:
        media_thumb(m);
        break;
    case TAG_SAVE:
        media_saved(m);
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
    case TAG_RSETTINGS:
        info_got_settings(m);
        break;
    case TAG_MEMBERS:
        info_got_members(m);
        break;
    case TAG_ROOMSET:
    case TAG_MODERATE:
        info_done(m);
        break;
    case TAG_PROFILE:
        prof_got(m);
        break;
    case TAG_PREVIEW:
        got_preview(m);
        break;
    case TAG_DEVICES:
        sess_got(m);
        break;
    case TAG_FIND:
        find_got(m);
        break;
    case TAG_POLL:
        break;
    case TAG_MEMB2:
        got_members2(m);
        break;
    case TAG_IGNLIST:
        got_ignored(m);
        break;
    case TAG_KEYWORDS:
        got_keywords(m);
        break;
    case TAG_PASSWORD:
    case TAG_DEACT: {
        static char b[300];
        if (m->status) snprintf(b, sizeof b, "\33bNicht geklappt:\33n %.240s", m->error ? m->error : "");
        else snprintf(b, sizeof b, m->tag == TAG_PASSWORD ? "Passwort geändert." : "Konto deaktiviert.");
        set(txt_acct, MUIA_Text_Contents, b);
        if (!m->status && m->tag == TAG_DEACT) { set(win_acct, MUIA_Window_Open, FALSE); amx_async_logout(A, TAG_LOGOUT); }
        break;
    }
    case TAG_FETCH:
    case TAG_EDITS:
        break;
    case TAG_PIN:
    case TAG_FWD: {
        ChatWin *w = current_chat();
        if (w) {
            if (m->status) snprintf(w->err, sizeof w->err, "Nicht geklappt: %.280s", m->error ? m->error : "");
            else snprintf(w->err, sizeof w->err, m->tag == TAG_FWD ? "Weitergeleitet." : "Erledigt.");
            update_status(w);
        }
        break;
    }
    case TAG_CMD:
    case TAG_WHOIS: {
        ChatWin *w = current_chat();
        if (!w) break;
        if (m->status) snprintf(w->err, sizeof w->err, "Nicht geklappt: %.280s", m->error ? m->error : "");
        else if (m->tag == TAG_WHOIS) {
            char *n = local(m->result ? m->result : "(kein Name)");
            snprintf(w->err, sizeof w->err, "%s%s", n ? n : "", m->result2 ? " - mit Bild" : "");
            free(n);
        } else snprintf(w->err, sizeof w->err, "Erledigt.");
        update_status(w);
        break;
    }
    case TAG_POLLVOTE:
        if (m->status) {
            ChatWin *w = m->result ? chat_by_room(m->result) : NULL;
            if (w) { snprintf(w->err, sizeof w->err, "Umfrage: %.280s", m->error ? m->error : ""); update_status(w); }
        }
        break;
    case TAG_TAG:
        if (m->status)
            MUI_Request(app, win_main, 0, (STRPTR)"Amatrix", (STRPTR)"*_OK", (STRPTR)"Hat nicht geklappt:\n%s",
                        (ULONG)(m->error ? m->error : ""));
        break;
    case TAG_DEVCHANGE:
        sess_done(m);
        break;
    case TAG_PROFAV:
        prof_got_av(m);
        break;
    case TAG_SETPROF:
        prof_done(m);
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
        case AMX_MSG_EVENT:
            if (m->ev.lookup) lookup_event(&m->ev);
            else if (m->ev.type == AMX_EV_TYPING) typing_event(&m->ev);
            else if (m->ev.type == AMX_EV_RECEIPT) receipt_event(&m->ev);
            else { chat_event(&m->ev); notify_event(&m->ev); }
            break;
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

static char rexx_buf[2048];

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
    if (xget(app, MUIA_Application_Iconified)) set(app, MUIA_Application_Iconified, FALSE);
    open_chat(r->id);
    {
        ChatWin *w = chat_by_room(r->id);
        if (w) DoMethod(w->win, MUIM_Window_ToFront);
    }
    notify_close(r->id);
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
    LONG k = 0;
    (void)obj;
    if (w)                                        /* the first message (not a date separator) */
        do { l = NULL; DoMethod(w->list, MUIM_NList_GetEntry, k++, &l); } while (l && l->sep);
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
                                               "confused", "eyes", "tada", "rocket", "unknown", "file", "link", "thread" };

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

/* MEDIA room: "name:state:strips" per attachment ("|"-separated; state 1
 * loading, 2 shown, 3 no preview); SAVEMEDIA room n path; OPENMEDIA room n;
 * LINKS room: the URLs; OPENLINK room n (rc 10 if no browser) */
HOOKPROTONH(rx_media_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    char b[900] = "", one[160];
    int i;
    (void)obj;
    if (!w) return 5;
    for (i = 0; i < w->n_media; i++) {
        snprintf(one, sizeof one, "%s%s:%d:%d", i ? "|" : "", w->media[i].name, w->media[i].state,
                 w->media[i].n_slices);
        strncat(b, one, sizeof b - strlen(b) - 1);
    }
    rexx_result(b);
    return 0;
}
MakeStaticHook(rx_media_hook, rx_media_func);

HOOKPROTONH(rx_savemedia_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    (void)obj;
    if (!w || !arg[1]) return 5;
    save_media(w, (int)*(LONG *)arg[1], (char *)arg[2]);
    return 0;
}
MakeStaticHook(rx_savemedia_hook, rx_savemedia_func);

HOOKPROTONH(rx_openmedia_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    (void)obj;
    if (!w || !arg[1]) return 5;
    open_media(w, (int)*(LONG *)arg[1]);
    return 0;
}
MakeStaticHook(rx_openmedia_hook, rx_openmedia_func);

HOOKPROTONH(rx_links_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    char b[900] = "";
    int i;
    (void)obj;
    if (!w) return 5;
    for (i = 0; i < w->n_links; i++) {
        if (i) strncat(b, " ", sizeof b - strlen(b) - 1);
        strncat(b, w->links[i], sizeof b - strlen(b) - 1);
    }
    rexx_result(b);
    return 0;
}
MakeStaticHook(rx_links_hook, rx_links_func);

HOOKPROTONH(rx_openlink_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    (void)obj;
    if (!w || !arg[1]) return 5;
    return open_link(w, (int)*(LONG *)arg[1], 1) ? 0 : 10;
}
MakeStaticHook(rx_openlink_hook, rx_openlink_func);

/* REPLY room text: answer the newest message of someone else; EDITLAST room
 * text / DELETELAST room: our newest message; TEXTS room n: the last n lines
 * ("|"-separated, ESC as "^") */
static ChatMsg *newest_msg(ChatWin *w, int own)
{
    int i;
    ChatMsg *best = NULL;
    for (i = 0; i < w->n_msgs; i++)
        if (w->msgs[i].own == own && !w->msgs[i].redacted && (!best || w->msgs[i].ts > best->ts)) best = &w->msgs[i];
    return best;
}

HOOKPROTONH(rx_reply_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    ChatMsg *m = w ? newest_msg(w, 0) : NULL;
    (void)obj;
    if (!m) return 5;
    start_reply(w, m);
    set(w->input, MUIA_String_Contents, (char *)arg[1]);
    chat_send(w);
    return 0;
}
MakeStaticHook(rx_reply_hook, rx_reply_func);

HOOKPROTONH(rx_editlast_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    ChatMsg *m = w ? newest_msg(w, 1) : NULL;
    (void)obj;
    if (!m) return 5;
    start_edit(w, m);
    set(w->input, MUIA_String_Contents, (char *)arg[1]);
    chat_send(w);
    return 0;
}
MakeStaticHook(rx_editlast_hook, rx_editlast_func);

HOOKPROTONH(rx_deletelast_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    ChatMsg *m = w ? newest_msg(w, 1) : NULL;
    (void)obj;
    if (!m) return 5;
    delete_msg(w, m, 0);
    return 0;
}
MakeStaticHook(rx_deletelast_hook, rx_deletelast_func);

HOOKPROTONH(rx_openthread_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    int i, best = -1;
    (void)obj;
    if (!w) return 5;
    for (i = 0; i < w->n_msgs; i++)
        if (w->msgs[i].thread_count > 0 && (best < 0 || w->msgs[i].ts > w->msgs[best].ts)) best = i;
    if (best < 0) return 10;
    open_thread_of(w, best);
    return 0;
}
MakeStaticHook(rx_openthread_hook, rx_openthread_func);

/* SAY in the open thread window of a room */
HOOKPROTONH(rx_threadsay_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w;
    (void)obj;
    if (!r) return 5;
    for (w = chats; w; w = w->next) if (w->thread_root && !strcmp(w->room_id, r->id)) break;
    if (!w) return 5;
    set(w->input, MUIA_String_Contents, (char *)arg[1]);
    chat_send(w);
    return 0;
}
MakeStaticHook(rx_threadsay_hook, rx_threadsay_func);

/* SENDFILE room path [THREAD]: as if chosen in the file requester */
HOOKPROTONH(rx_sendfile_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    (void)obj;
    if (r && arg[2])
        for (w = chats; w; w = w->next) if (w->thread_root && !strcmp(w->room_id, r->id)) break;
    if (!w) return 5;
    send_file(w, (char *)arg[1]);
    return 0;
}
MakeStaticHook(rx_sendfile_hook, rx_sendfile_func);

/* COPY room: the marked message, else the newest, to the clipboard */
HOOKPROTONH(rx_copy_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    ChatMsg *m;
    (void)obj;
    if (!w) return 5;
    if (!(m = marked_msg(w))) {
        int i;
        for (i = 0; i < w->n_msgs; i++)
            if (!w->msgs[i].redacted && (!m || w->msgs[i].ts > m->ts)) m = &w->msgs[i];
    }
    if (!m) return 5;
    copy_msg(w, m);
    return 0;
}
MakeStaticHook(rx_copy_hook, rx_copy_func);

/* CLIP: the text on the clipboard */
HOOKPROTONH(rx_clip_func, LONG, Object *obj, ULONG *arg)
{
    char *t = clip_read();
    (void)obj; (void)arg;
    rexx_result(t ? t : "");
    free(t);
    return 0;
}
MakeStaticHook(rx_clip_hook, rx_clip_func);

/* TYPING room: who is typing there (as shown) */
HOOKPROTONH(rx_typing_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    (void)obj;
    if (!w) return 5;
    rexx_result(w->typing);
    return 0;
}
MakeStaticHook(rx_typing_hook, rx_typing_func);

/* READ room: how many of our messages there have a check mark */
HOOKPROTONH(rx_read_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    char b[16];
    int i, n = 0;
    (void)obj;
    if (!w) return 5;
    for (i = 0; i < w->n_msgs; i++) n += w->msgs[i].read;
    sprintf(b, "%d", n);
    rexx_result(b);
    return 0;
}
MakeStaticHook(rx_read_hook, rx_read_func);

/* TYPE room text: into the input field, not sent ("" empties it) */
HOOKPROTONH(rx_type_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    (void)obj;
    if (!w) return 5;
    set(w->input, MUIA_String_Contents, arg[1] ? (char *)arg[1] : "");
    return 0;
}
MakeStaticHook(rx_type_hook, rx_type_func);

/* ROOMINFO room: opens the members/settings window */
HOOKPROTONH(rx_roominfo_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    (void)obj;
    if (!r) return 5;
    open_info(r->id);
    return 0;
}
MakeStaticHook(rx_roominfo_hook, rx_roominfo_func);

/* MEMBERS: the window's member list, "name:role|..." */
HOOKPROTONH(rx_members_func, LONG, Object *obj, ULONG *arg)
{
    char b[1500] = "", one[200];
    LONG i, n = (LONG)xget(lst_mem, MUIA_NList_Entries);
    (void)obj; (void)arg;
    for (i = 0; i < n; i++) {
        AmxMember *p = NULL;
        char *nm;
        DoMethod(lst_mem, MUIM_NList_GetEntry, i, &p);
        if (!p) continue;
        nm = local(p->name);
        snprintf(one, sizeof one, "%s%s:%s%s", i ? "|" : "", nm ? nm : "", role_of(p->power),
                 !strcmp(p->membership, "join") ? "" : !strcmp(p->membership, "ban") ? " gesperrt" : " eingeladen");
        free(nm);
        strncat(b, one, sizeof b - strlen(b) - 1);
    }
    rexx_result(b);
    return 0;
}
MakeStaticHook(rx_members_hook, rx_members_func);

/* INFOSTATE: what the window shows: name|topic|join|history|encrypted|status */
HOOKPROTONH(rx_infostate_func, LONG, Object *obj, ULONG *arg)
{
    char b[1200];
    (void)obj; (void)arg;
    snprintf(b, sizeof b, "%s|%s|%s|%s|%ld|%s", (char *)xget(str_iname, MUIA_String_Contents),
             (char *)xget(str_itopic, MUIA_String_Contents), join_rules[xget(cyc_ijoin, MUIA_Cycle_Active)],
             hist_values[xget(cyc_ihist, MUIA_Cycle_Active)], (long)xget(chk_ienc, MUIA_Selected), info_status);
    rexx_result(b);
    return 0;
}
MakeStaticHook(rx_infostate_hook, rx_infostate_func);

/* INFOSET name/topic/join/history value: types into the window and saves */
HOOKPROTONH(rx_infoset_func, LONG, Object *obj, ULONG *arg)
{
    const char *what = (const char *)arg[0], *v = arg[1] ? (const char *)arg[1] : "";
    (void)obj;
    if (!strcmp(what, "name")) set(str_iname, MUIA_String_Contents, v);
    else if (!strcmp(what, "topic")) set(str_itopic, MUIA_String_Contents, v);
    else if (!strcmp(what, "join")) set(cyc_ijoin, MUIA_Cycle_Active, idx_of(join_rules, v));
    else if (!strcmp(what, "history")) set(cyc_ihist, MUIA_Cycle_Active, idx_of(hist_values, v));
    else return 5;
    info_save();
    return 0;
}
MakeStaticHook(rx_infoset_hook, rx_infoset_func);

/* INFOMEMBER n action [role]: marks member n, then kick/ban/unban/role */
HOOKPROTONH(rx_infomember_func, LONG, Object *obj, ULONG *arg)
{
    const char *act = (const char *)arg[1];
    (void)obj;
    set(lst_mem, MUIA_NList_Active, *(LONG *)arg[0]);
    amx_log("INFOMEMBER %ld %s: %s", (long)*(LONG *)arg[0], act, info_selected() ? info_selected()->user_id : "-");
    if (!info_selected()) return 5;
    if (!strcmp(act, "role")) {
        set(cyc_irole, MUIA_Cycle_Active, arg[2] ? *(LONG *)arg[2] : 0);
        info_moderate("power");
    } else if (!strcmp(act, "kick") || !strcmp(act, "ban") || !strcmp(act, "unban")) {
        AmxMember *p = info_selected();     /* without the question */
        amx_async_moderate(A, info_room, p->user_id, act, NULL, 0, TAG_MODERATE);
        info_busy++;
        info_reload(0, 1);
    } else return 5;
    return 0;
}
MakeStaticHook(rx_infomember_hook, rx_infomember_func);

/* PROFILE [NAME n] [PICTURE path]: opens the profile window; with NAME /
 * PICTURE also changes them (as typed / chosen) */
HOOKPROTONH(rx_profile_func, LONG, Object *obj, ULONG *arg)
{
    (void)obj;
    open_profile();
    if (arg[0]) set(str_pname, MUIA_String_Contents, (char *)arg[0]);
    if (arg[0]) {                                  /* compare with the name the server has */
        char *t = amx_from_local((char *)arg[0]);
        amx_async_set_profile(A, "name", t, TAG_SETPROF);
        prof_busy++;
        free(t);
        amx_async_profile(A, TAG_PROFILE);
    }
    if (arg[1]) prof_picture((char *)arg[1]);
    return 0;
}
MakeStaticHook(rx_profile_hook, rx_profile_func);

/* PROFILESTATE: name|picture (1/0)|status of the profile window */
HOOKPROTONH(rx_profstate_func, LONG, Object *obj, ULONG *arg)
{
    char b[700];
    (void)obj; (void)arg;
    snprintf(b, sizeof b, "%s|%d|%s", (char *)xget(str_pname, MUIA_String_Contents),
             prof_imgspec[0] && prof_av ? 1 : 0, prof_status);
    rexx_result(b);
    return 0;
}
MakeStaticHook(rx_profstate_hook, rx_profstate_func);

/* PREVIEWS room: the link preview lines of the chat window, "|"-separated */
HOOKPROTONH(rx_previews_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    char b[1500] = "";
    LONG k, n;
    (void)obj;
    if (!w) return 5;
    n = (LONG)xget(w->list, MUIA_NList_Entries);
    for (k = 0; k < n; k++) {
        Line *l = NULL;
        char *t;
        DoMethod(w->list, MUIM_NList_GetEntry, k, &l);
        if (!l || !l->prev || !l->text) continue;
        t = strstr(l->text, "\33b");
        if (b[0]) strncat(b, "|", sizeof b - strlen(b) - 1);
        strncat(b, t ? t + 2 : l->text, sizeof b - strlen(b) - 1);
    }
    rexx_result(b);
    return 0;
}
MakeStaticHook(rx_previews_hook, rx_previews_func);

/* SETOPTION name value: a setting as in the settings window (not saved) */
HOOKPROTONH(rx_setoption_func, LONG, Object *obj, ULONG *arg)
{
    const char *n = (const char *)arg[0];
    int v = (int)*(LONG *)arg[1];
    (void)obj;
    if (!strcmp(n, "linkprev")) opt_linkprev = v;
    else if (!strcmp(n, "linkprev_enc")) opt_linkprev_enc = v;
    else if (!strcmp(n, "typing")) opt_typing = v;
    else if (!strcmp(n, "previews")) opt_previews = v;
    else if (!strcmp(n, "avatars")) { opt_avatars = v; DoMethod(lst_rooms, MUIM_List_Redraw, MUIV_List_Redraw_All); }
    else if (!strcmp(n, "joins") || !strcmp(n, "24h") || !strcmp(n, "compact")) {
        ChatWin *cw;
        if (n[0] == 'j') opt_joins = v; else if (n[0] == '2') opt_24h = v; else opt_compact = v;
        for (cw = chats; cw; cw = cw->next) rerender_window(cw);
    }
    else return 5;
    return 0;
}
MakeStaticHook(rx_setoption_hook, rx_setoption_func);

/* SESSIONS: opens the window; SESSIONLIST: "name:id:state|..." */
HOOKPROTONH(rx_sessions_func, LONG, Object *obj, ULONG *arg)
{
    (void)obj; (void)arg;
    open_sessions();
    return 0;
}
MakeStaticHook(rx_sessions_hook, rx_sessions_func);

HOOKPROTONH(rx_sesslist_func, LONG, Object *obj, ULONG *arg)
{
    char b[1500] = "", one[200];
    int i;
    (void)obj; (void)arg;
    for (i = 0; i < n_sess; i++) {
        snprintf(one, sizeof one, "%s%s:%s:%s", i ? "|" : "", sess[i].name ? sess[i].name : "", sess[i].device_id,
                 sess[i].current ? "current" : sess[i].verified > 0 ? "verified" : "unverified");
        strncat(b, one, sizeof b - strlen(b) - 1);
    }
    rexx_result(b);
    return 0;
}
MakeStaticHook(rx_sesslist_hook, rx_sesslist_func);

/* CHATCACHED room: how many messages came from the local history cache */
HOOKPROTONH(rx_chatcached_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    char b[16];
    (void)obj;
    if (!w) return 5;
    sprintf(b, "%d", w->n_cached);
    rexx_result(b);
    return 0;
}
MakeStaticHook(rx_chatcached_hook, rx_chatcached_func);

/* FILTER text: the room list filter; SPACE name ("" all); ROOMS: the list as shown */
HOOKPROTONH(rx_filter_func, LONG, Object *obj, ULONG *arg)
{
    (void)obj;
    set(str_filter, MUIA_String_Contents, arg[0] ? (char *)arg[0] : "");
    return 0;
}
MakeStaticHook(rx_filter_hook, rx_filter_func);

HOOKPROTONH(rx_space_func, LONG, Object *obj, ULONG *arg)
{
    const char *n = arg[0] ? (const char *)arg[0] : "";
    int i;
    (void)obj;
    if (!*n) { if (cyc_space) set(cyc_space, MUIA_Cycle_Active, 0); return 0; }
    for (i = 0; i < n_spaces; i++)
        if (!strcmp(space_names[i + 1], n)) { set(cyc_space, MUIA_Cycle_Active, i + 1); return 0; }
    return 5;
}
MakeStaticHook(rx_space_hook, rx_space_func);

HOOKPROTONH(rx_rooms_func, LONG, Object *obj, ULONG *arg)
{
    char b[1500] = "";
    LONG i, n = (LONG)xget(lst_rooms, MUIA_List_Entries);
    (void)obj; (void)arg;
    for (i = 0; i < n; i++) {
        Room *r = NULL;
        DoMethod(lst_rooms, MUIM_List_GetEntry, i, &r);
        if (!r) continue;
        if (b[0]) strncat(b, "|", sizeof b - strlen(b) - 1);
        strncat(b, r->name, sizeof b - strlen(b) - 1);
        if (r->favourite) strncat(b, "*", sizeof b - strlen(b) - 1);
    }
    rexx_result(b);
    return 0;
}
MakeStaticHook(rx_rooms_hook, rx_rooms_func);

/* FAVOURITE room [OFF] */
HOOKPROTONH(rx_fav_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    (void)obj;
    if (!r) return 5;
    amx_async_set_tag(A, r->id, "m.favourite", arg[1] == 0, TAG_TAG);
    return 0;
}
MakeStaticHook(rx_fav_hook, rx_fav_func);

/* FIND term: the search window, all rooms; HITS: "room:sender:text|..." */
HOOKPROTONH(rx_find_func, LONG, Object *obj, ULONG *arg)
{
    (void)obj;
    set(win_find, MUIA_Window_Open, TRUE);
    set(str_find, MUIA_String_Contents, arg[0] ? (char *)arg[0] : "");
    set(cyc_findwhere, MUIA_Cycle_Active, 0);
    find_go();
    return 0;
}
MakeStaticHook(rx_find_hook, rx_find_func);

HOOKPROTONH(rx_hits_func, LONG, Object *obj, ULONG *arg)
{
    char b[1500] = "", one[300];
    int i;
    (void)obj; (void)arg;
    for (i = 0; i < n_hits; i++) {
        const Room *r = room_by_id(hits[i].room_id);
        char *t = local(hits[i].body);
        snprintf(one, sizeof one, "%s%s:%s:%s", i ? "|" : "", r ? r->name : "?", hits[i].sender, t ? t : "");
        free(t);
        strncat(b, one, sizeof b - strlen(b) - 1);
    }
    rexx_result(b);
    return 0;
}
MakeStaticHook(rx_hits_hook, rx_hits_func);

/* POLL room question answers ("a|b|c"), MULTI, HIDE: as from the window */
HOOKPROTONH(rx_poll_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    char *q, *list, *p;
    (void)obj;
    if (!r) return 5;
    q = amx_from_local((char *)arg[1]);
    list = amx_from_local((char *)arg[2]);
    for (p = list; p && *p; p++) if (*p == '|') *p = '\n';
    amx_async_send_poll(A, r->id, q, list, arg[3] ? 20 : 1, !arg[4], TAG_POLLVOTE);
    free(q);
    free(list);
    return 0;
}
MakeStaticHook(rx_poll_hook, rx_poll_func);

/* VOTE room n: click on answer n (0 ...; 255 = end) of the newest poll */
HOOKPROTONH(rx_vote_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    (void)obj;
    if (!w || !w->n_polls) return 5;
    if (*(LONG *)arg[1] == 255) amx_async_poll_end(A, w->room_id, w->polls[w->n_polls - 1].eid, TAG_POLLVOTE);
    else poll_click(w, w->n_polls - 1, (int)*(LONG *)arg[1]);
    return 0;
}
MakeStaticHook(rx_vote_hook, rx_vote_func);

/* POLLROWS room: the answer lines of all polls, "|"-separated */
HOOKPROTONH(rx_pollrows_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    char b[1800] = "";
    LONG k, n;
    (void)obj;
    if (!w) return 5;
    n = (LONG)xget(w->list, MUIA_NList_Entries);
    for (k = 0; k < n; k++) {
        Line *l = NULL;
        const char *t;
        DoMethod(w->list, MUIM_NList_GetEntry, k, &l);
        if (!l || !l->pollrow || !l->text) continue;
        t = strchr(l->text, ']');
        if (b[0]) strncat(b, "|", sizeof b - strlen(b) - 1);
        strncat(b, t ? t + 1 : l->text, sizeof b - strlen(b) - 1);
        if (strstr(l->text, "[") && strstr(l->text, "@") && t) {    /* chosen? */
            char num[8];
            sprintf(num, "%d", EMO_RADIO_ON);
            if (!strncmp(strstr(l->text, "\33o[") + 4, num, strlen(num)) && strstr(l->text, "\33o[")[4 + strlen(num)] == '@')
                strncat(b, " <X>", sizeof b - strlen(b) - 1);
        }
    }
    rexx_result(b);
    return 0;
}
MakeStaticHook(rx_pollrows_hook, rx_pollrows_func);

/* VISIBLE room: the first shown line's text (no codes) and where the
 * "neue Nachrichten" marker is ("-1" none): "marker|first line" */
HOOKPROTONH(rx_visible_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    LONG n, k, mark = -1, first;
    Line *l = NULL;
    char b[400], *p, *q;
    (void)obj;
    if (!w) return 5;
    n = (LONG)xget(w->list, MUIA_NList_Entries);
    for (k = 0; k < n; k++) {
        Line *x = NULL;
        DoMethod(w->list, MUIM_NList_GetEntry, k, &x);
        if (x && x->newmark) mark = k;
    }
    first = (LONG)xget(w->list, MUIA_NList_First);
    DoMethod(w->list, MUIM_NList_GetEntry, first, &l);
    snprintf(b, sizeof b, "%ld|%ld|", (long)mark, (long)first);
    for (p = b + strlen(b), q = l && l->text ? l->text : ""; *q && p < b + sizeof b - 1; q++) {
        if (*q == '\033') { q++; if (*q == 'o' || *q == 'P' || *q == 'A') while (*q && *q != ']') q++; continue; }
        *p++ = *q;
    }
    *p = 0;
    rexx_result(b);
    return 0;
}
MakeStaticHook(rx_visible_hook, rx_visible_func);

/* SUGGEST room: the suggestions shown ("|"); COMPLETE room n; SENDINPUT room */
HOOKPROTONH(rx_suggest_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    char b[400] = "";
    int k;
    (void)obj;
    if (!w) return 5;
    for (k = 0; k < w->n_sugg; k++) {
        if (k) strncat(b, "|", sizeof b - strlen(b) - 1);
        strncat(b, w->stext[k], sizeof b - strlen(b) - 1);
    }
    rexx_result(b);
    return 0;
}
MakeStaticHook(rx_suggest_hook, rx_suggest_func);

HOOKPROTONH(rx_complete_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    (void)obj;
    if (!w || *(LONG *)arg[1] >= w->n_sugg) return 5;
    complete(w, (int)*(LONG *)arg[1]);
    return 0;
}
MakeStaticHook(rx_complete_hook, rx_complete_func);

HOOKPROTONH(rx_sendinput_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    (void)obj;
    if (!w) return 5;
    chat_send(w);
    return 0;
}
MakeStaticHook(rx_sendinput_hook, rx_sendinput_func);

/* MARK room text: marks the newest message containing text */
HOOKPROTONH(rx_mark_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    ChatMsg *best = NULL;
    char *u;
    int i;
    LONG k;
    (void)obj;
    if (!w || !arg[1]) return 5;
    u = amx_from_local((char *)arg[1]);
    for (i = 0; u && i < w->n_msgs; i++)
        if (w->msgs[i].body && strstr(w->msgs[i].body, u) && (!best || w->msgs[i].ts > best->ts)) best = &w->msgs[i];
    free(u);
    if (!best || (k = line_of(w, best)) < 0) return 5;
    set(w->list, MUIA_NList_Active, k);
    return 0;
}
MakeStaticHook(rx_mark_hook, rx_mark_func);

/* ACTION room what [to]: pin / quote / copylink / edithist / forward (to a room) on the marked message */
HOOKPROTONH(rx_action_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]), *to;
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    ChatMsg *m = w ? marked_msg(w) : NULL;
    const char *what = (const char *)arg[1];
    (void)obj;
    if (!m) return 5;
    if (!strcmp(what, "pin")) {
        int on = !(r->pinned && strstr(r->pinned, m->eid));
        amx_async_pin(A, w->room_id, m->eid, on, TAG_PIN);
    } else if (!strcmp(what, "quote")) start_quote(w, m);
    else if (!strcmp(what, "copylink")) copy_link(w, m);
    else if (!strcmp(what, "edithist")) open_edit_history(w, m);
    else if (!strcmp(what, "forward") && arg[2] && (to = room_by_name((char *)arg[2])) != NULL)
        amx_async_forward(A, to->id, w->room_id, m->eid, TAG_FWD);
    else return 5;
    return 0;
}
MakeStaticHook(rx_action_hook, rx_action_func);

/* PINBAR room: the bar's text; EDITHIST: the edit history rows ("|") */
HOOKPROTONH(rx_pinbar_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    (void)obj;
    if (!w) return 5;
    rexx_result(xget(w->pinbar, MUIA_ShowMe) ? (char *)xget(w->pinbar, MUIA_Text_Contents) : "");
    return 0;
}
MakeStaticHook(rx_pinbar_hook, rx_pinbar_func);

HOOKPROTONH(rx_edithist_func, LONG, Object *obj, ULONG *arg)
{
    char b[1500] = "";
    int i;
    (void)obj; (void)arg;
    for (i = 0; i < n_hist; i++) {
        if (i) strncat(b, "|", sizeof b - strlen(b) - 1);
        strncat(b, hist_rows[i].text, sizeof b - strlen(b) - 1);
    }
    rexx_result(b);
    return 0;
}
MakeStaticHook(rx_edithist_hook, rx_edithist_func);

/* NOTIFYLEVEL room [n]: sets (0 default, 1 all, 2 mentions, 3 off) and/or returns the level */
HOOKPROTONH(rx_nlevel_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    char b[8];
    (void)obj;
    if (!r) return 5;
    if (arg[1]) amx_async_room_notify(A, r->id, (int)*(LONG *)arg[1], TAG_CMD);
    sprintf(b, "%d", r->notify);
    rexx_result(b);
    return 0;
}
MakeStaticHook(rx_nlevel_hook, rx_nlevel_func);

/* KEYWORD word [OFF]; KEYWORDS: the list as the settings window has it */
HOOKPROTONH(rx_keyword_func, LONG, Object *obj, ULONG *arg)
{
    char *u = amx_from_local((char *)arg[0]);
    (void)obj;
    amx_async_keyword(A, u, arg[1] == 0, TAG_KEYWORDS);
    free(u);
    return 0;
}
MakeStaticHook(rx_keyword_hook, rx_keyword_func);

HOOKPROTONH(rx_keywords_func, LONG, Object *obj, ULONG *arg)
{
    (void)obj; (void)arg;
    rexx_result(kw_list ? kw_list : "");
    return 0;
}
MakeStaticHook(rx_keywords_hook, rx_keywords_func);

/* EXPORT room path [ALL] */
HOOKPROTONH(rx_export_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    (void)obj;
    if (!w) return 5;
    export_chat(w, (char *)arg[1], arg[2] != 0);
    return 0;
}
MakeStaticHook(rx_export_hook, rx_export_func);

/* PASSWORD old new: as typed in the account window; ACCOUNTSTATE: its status line */
HOOKPROTONH(rx_password_func, LONG, Object *obj, ULONG *arg)
{
    (void)obj;
    set(str_pwold, MUIA_String_Contents, (char *)arg[0]);
    set(str_pwnew, MUIA_String_Contents, (char *)arg[1]);
    set(str_pwnew2, MUIA_String_Contents, (char *)arg[1]);
    set(win_acct, MUIA_Window_Open, TRUE);
    pw_go();
    return 0;
}
MakeStaticHook(rx_password_hook, rx_password_func);

HOOKPROTONH(rx_acctstate_func, LONG, Object *obj, ULONG *arg)
{
    (void)obj; (void)arg;
    rexx_result((char *)xget(txt_acct, MUIA_Text_Contents));
    return 0;
}
MakeStaticHook(rx_acctstate_hook, rx_acctstate_func);

/* REPLACED room: the new room's name ("" if not replaced); GOREPLACEMENT room: a click on the bar */
HOOKPROTONH(rx_replaced_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]), *nr;
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    (void)obj;
    if (!r) return 5;
    nr = r->replacement ? room_by_id(r->replacement) : NULL;
    rexx_result(!r->replacement ? "" : !w || !xget(w->tombbar, MUIA_ShowMe) ? "(no bar)" : nr ? nr->name : r->replacement);
    return 0;
}
MakeStaticHook(rx_replaced_hook, rx_replaced_func);

HOOKPROTONH(rx_gorepl_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    (void)obj;
    if (!w) return 5;
    go_replacement(w);
    return 0;
}
MakeStaticHook(rx_gorepl_hook, rx_gorepl_func);

/* EDITUP room: like cursor up in the empty input; DRAFT room: its draft; INPUT room: the input field */
HOOKPROTONH(rx_editup_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    (void)obj;
    if (!w) return 5;
    edit_newest_own(w);
    return 0;
}
MakeStaticHook(rx_editup_hook, rx_editup_func);

HOOKPROTONH(rx_draft_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    (void)obj;
    if (!r) return 5;
    rexx_result(draft_of(r->id) ? draft_of(r->id) : "");
    return 0;
}
MakeStaticHook(rx_draft_hook, rx_draft_func);

HOOKPROTONH(rx_input_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    (void)obj;
    if (!w) return 5;
    rexx_result((char *)xget(w->input, MUIA_String_Contents));
    return 0;
}
MakeStaticHook(rx_input_hook, rx_input_func);


/* SESSIONSET n NAME x / PASSWORD p: rename / sign out entry n (no question) */
HOOKPROTONH(rx_sessset_func, LONG, Object *obj, ULONG *arg)
{
    AmxDevice *d;
    (void)obj;
    set(lst_sess, MUIA_NList_Active, *(LONG *)arg[0]);
    if (!(d = sess_selected())) return 5;
    if (arg[1]) { set(str_sname, MUIA_String_Contents, (char *)arg[1]); sess_rename(); }
    if (arg[2]) {
        if (d->current) return 10;
        amx_async_device_delete(A, d->device_id, (char *)arg[2], TAG_DEVCHANGE);
        sess_busy++;
        amx_async_devices(A, TAG_DEVICES);
    }
    return 0;
}
MakeStaticHook(rx_sessset_hook, rx_sessset_func);

HOOKPROTONH(rx_texts_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    ChatWin *w = r ? chat_by_room(r->id) : NULL;
    if (r && arg[2])                                      /* THREAD: the room's thread window */
        for (w = chats; w; w = w->next) if (w->thread_root && !strcmp(w->room_id, r->id)) break;
    char b[1600] = "", *q;
    LONG n, k, want = arg[1] ? *(LONG *)arg[1] : 10;
    Line *prev = NULL;
    (void)obj;
    if (!w) return 5;
    n = (LONG)xget(w->list, MUIA_NList_Entries);
    for (k = n - want < 0 ? 0 : n - want; k < n; k++) {
        Line *l = NULL;
        DoMethod(w->list, MUIM_NList_GetEntry, k, &l);
        if (!l || l == prev) continue;                   /* wrapped parts: once */
        prev = l;
        if (b[0]) strncat(b, "|", sizeof b - strlen(b) - 1);
        strncat(b, l->text ? l->text : "", sizeof b - strlen(b) - 1);
    }
    for (q = b; *q; q++) if (*q == 27) *q = '^';
    rexx_result(b);
    return 0;
}
MakeStaticHook(rx_texts_hook, rx_texts_func);

HOOKPROTONH(rx_mute_func, LONG, Object *obj, ULONG *arg)
{
    Room *r = room_by_name((char *)arg[0]);
    (void)obj;
    if (!r) return 5;
    set_muted(r->id, arg[1] == 0);                /* MUTE room [OFF] */
    save_prefs();
    DoMethod(lst_rooms, MUIM_List_Redraw, MUIV_List_Redraw_All);
    return 0;
}
MakeStaticHook(rx_mute_hook, rx_mute_func);

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
    { "MUTE",      "ROOM/A,OFF/S",    2, &rx_mute_hook,     { 0 } },
    { "MEDIA",     "ROOM/F",          1, &rx_media_hook,    { 0 } },
    { "SAVEMEDIA", "ROOM/A,N/N/A,PATH/A", 3, &rx_savemedia_hook, { 0 } },
    { "OPENMEDIA", "ROOM/A,N/N/A",    2, &rx_openmedia_hook, { 0 } },
    { "LINKS",     "ROOM/F",          1, &rx_links_hook,    { 0 } },
    { "OPENLINK",  "ROOM/A,N/N/A",    2, &rx_openlink_hook, { 0 } },
    { "REPLY",     "ROOM/A,TEXT/F",   2, &rx_reply_hook,    { 0 } },
    { "EDITLAST",  "ROOM/A,TEXT/F",   2, &rx_editlast_hook, { 0 } },
    { "DELETELAST", "ROOM/F",         1, &rx_deletelast_hook, { 0 } },
    { "TEXTS",     "ROOM/A,N/N,THREAD/S", 3, &rx_texts_hook, { 0 } },
    { "OPENTHREAD", "ROOM/F",         1, &rx_openthread_hook, { 0 } },
    { "THREADSAY", "ROOM/A,TEXT/F",   2, &rx_threadsay_hook, { 0 } },
    { "COPY",      "ROOM/F",          1, &rx_copy_hook,     { 0 } },
    { "TYPING",    "ROOM/F",          1, &rx_typing_hook,   { 0 } },
    { "READ",      "ROOM/F",          1, &rx_read_hook,     { 0 } },
    { "TYPE",      "ROOM/A,TEXT/F",   2, &rx_type_hook,     { 0 } },
    { "ROOMINFO",  "ROOM/F",          1, &rx_roominfo_hook, { 0 } },
    { "PROFILE",   "NAME/K,PICTURE/K", 2, &rx_profile_hook, { 0 } },
    { "PROFILESTATE", NULL,           0, &rx_profstate_hook, { 0 } },
    { "PREVIEWS",  "ROOM/F",          1, &rx_previews_hook, { 0 } },
    { "SETOPTION", "NAME/A,VALUE/N/A", 2, &rx_setoption_hook, { 0 } },
    { "SESSIONS",  NULL,              0, &rx_sessions_hook, { 0 } },
    { "CHATCACHED", "ROOM/F",         1, &rx_chatcached_hook, { 0 } },
    { "FILTER",    "TEXT/F",          1, &rx_filter_hook,   { 0 } },
    { "SPACE",     "NAME/F",          1, &rx_space_hook,    { 0 } },
    { "ROOMS",     NULL,              0, &rx_rooms_hook,    { 0 } },
    { "FAVOURITE", "ROOM/A,OFF/S",    2, &rx_fav_hook,      { 0 } },
    { "FIND",      "TERM/F",          1, &rx_find_hook,     { 0 } },
    { "HITS",      NULL,              0, &rx_hits_hook,     { 0 } },
    { "POLL",      "ROOM/A,QUESTION/A,ANSWERS/A,MULTI/S,HIDE/S", 5, &rx_poll_hook, { 0 } },
    { "VOTE",      "ROOM/A,N/N/A",    2, &rx_vote_hook,     { 0 } },
    { "POLLROWS",  "ROOM/F",          1, &rx_pollrows_hook, { 0 } },
    { "VISIBLE",   "ROOM/F",          1, &rx_visible_hook,  { 0 } },
    { "SUGGEST",   "ROOM/F",          1, &rx_suggest_hook,  { 0 } },
    { "COMPLETE",  "ROOM/A,N/N/A",    2, &rx_complete_hook, { 0 } },
    { "SENDINPUT", "ROOM/F",          1, &rx_sendinput_hook, { 0 } },
    { "MARK",      "ROOM/A,TEXT/F",   2, &rx_mark_hook,     { 0 } },
    { "ACTION",    "ROOM/A,WHAT/A,TO/F", 3, &rx_action_hook, { 0 } },
    { "PINBAR",    "ROOM/F",          1, &rx_pinbar_hook,   { 0 } },
    { "EDITHIST",  NULL,              0, &rx_edithist_hook, { 0 } },
    { "NOTIFYLEVEL", "ROOM/A,LEVEL/N", 2, &rx_nlevel_hook,  { 0 } },
    { "KEYWORD",   "WORD/A,OFF/S",    2, &rx_keyword_hook,  { 0 } },
    { "KEYWORDS",  NULL,              0, &rx_keywords_hook, { 0 } },
    { "EXPORT",    "ROOM/A,PATH/A,ALL/S", 3, &rx_export_hook, { 0 } },
    { "PASSWORD",  "OLD/A,NEW/A",     2, &rx_password_hook, { 0 } },
    { "ACCOUNTSTATE", NULL,           0, &rx_acctstate_hook, { 0 } },
    { "REPLACED",  "ROOM/F",          1, &rx_replaced_hook, { 0 } },
    { "GOREPLACEMENT", "ROOM/F",      1, &rx_gorepl_hook,   { 0 } },
    { "EDITUP",    "ROOM/F",          1, &rx_editup_hook,   { 0 } },
    { "DRAFT",     "ROOM/F",          1, &rx_draft_hook,    { 0 } },
    { "INPUT",     "ROOM/F",          1, &rx_input_hook,    { 0 } },
    { "SESSIONLIST", NULL,            0, &rx_sesslist_hook, { 0 } },
    { "SESSIONSET", "N/N/A,NAME/K,PASSWORD/K", 3, &rx_sessset_hook, { 0 } },
    { "MEMBERS",   NULL,              0, &rx_members_hook,  { 0 } },
    { "INFOSTATE", NULL,              0, &rx_infostate_hook, { 0 } },
    { "INFOSET",   "WHAT/A,VALUE/F",  2, &rx_infoset_hook,  { 0 } },
    { "INFOMEMBER", "N/N/A,ACTION/A,LEVEL/N", 3, &rx_infomember_hook, { 0 } },
    { "CLIP",      NULL,              0, &rx_clip_hook,     { 0 } },
    { "SENDFILE",  "ROOM/A,FILE/A,THREAD/S", 3, &rx_sendfile_hook, { 0 } },
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
    { NM_ITEM,  (STRPTR)"Raum stummschalten", (STRPTR)"M", 0, 0, (APTR)ID_MUTE },
    { NM_ITEM,  (STRPTR)"Mitglieder und Einstellungen ...", (STRPTR)"T", 0, 0, (APTR)ID_INFO },
    { NM_ITEM,  (STRPTR)"Nachricht kopieren", (STRPTR)"K", 0, 0, (APTR)ID_COPY },
    { NM_ITEM,  (STRPTR)"Link zur Nachricht kopieren", NULL, 0, 0, (APTR)ID_COPYLINK },
    { NM_ITEM,  (STRPTR)"Zitieren",          (STRPTR)"Z", 0, 0, (APTR)ID_QUOTE },
    { NM_ITEM,  (STRPTR)"Weiterleiten ...",  (STRPTR)"W", 0, 0, (APTR)ID_FWD },
    { NM_ITEM,  (STRPTR)"Anpinnen an/aus",   (STRPTR)"P", 0, 0, (APTR)ID_PIN },
    { NM_ITEM,  (STRPTR)"Bearbeitungsverlauf ...", NULL, 0, 0, (APTR)ID_EDITHIST },
    { NM_ITEM,  (STRPTR)"Chat exportieren ...", NULL, 0, 0, (APTR)ID_EXPORT },
    { NM_ITEM,  (STRPTR)"Nachrichten suchen ...", (STRPTR)"D", 0, 0, (APTR)ID_FIND },
    { NM_ITEM,  (STRPTR)"Umfrage erstellen ...", (STRPTR)"U", 0, 0, (APTR)ID_POLLNEW },
    { NM_ITEM,  (STRPTR)"Favorit an/aus",    (STRPTR)"F", 0, 0, (APTR)ID_FAV },
    { NM_ITEM,  (STRPTR)"Niedrige Priorität an/aus", NULL, 0, 0, (APTR)ID_LOWPRIO },
    { NM_ITEM,  NM_BARLABEL,                 NULL, 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Sicherheitsschlüssel eingeben ...", (STRPTR)"S", 0, 0, (APTR)ID_RECOVERY },
    { NM_ITEM,  (STRPTR)"Passwort für Amatrix-Daten ...", (STRPTR)"P", 0, 0, (APTR)ID_STOREPASS },
    { NM_ITEM,  (STRPTR)"Eigenes Profil ...", (STRPTR)"O", 0, 0, (APTR)ID_PROFILE },
    { NM_ITEM,  (STRPTR)"Sitzungen ...",     (STRPTR)"G", 0, 0, (APTR)ID_SESS },
    { NM_ITEM,  (STRPTR)"Konto ...",          NULL, 0, 0, (APTR)ID_ACCOUNT },
    { NM_ITEM,  (STRPTR)"Einstellungen ...", (STRPTR)"E", 0, 0, (APTR)ID_SETTINGS },
    { NM_ITEM,  (STRPTR)"MUI-Einstellungen ...", NULL, 0, 0, (APTR)ID_MUIPREFS },
    { NM_ITEM,  (STRPTR)"Ikonifizieren",     (STRPTR)"I", 0, 0, (APTR)ID_ICONIFY },
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
        MUIA_Application_DiskObject, dob_app,
        MUIA_Application_Menustrip, MUI_MakeObject(MUIO_MenustripNM, (ULONG)menu, 0),

        SubWindow, win_main = WindowObject,
            MUIA_Window_Title, "Amatrix",
            MUIA_Window_ID, MAKE_ID('A','M','X','M'),
            MUIA_Window_LeftEdge, 8,
            MUIA_Window_TopEdge, MUIV_Window_TopEdge_Centered,
            MUIA_Window_Width, MUIV_Window_Width_Visible(38),
            MUIA_Window_Height, MUIV_Window_Height_Visible(65),
            WindowContents, VGroup,
                Child, grp_space = HGroup, MUIA_ShowMe, FALSE, End,
                Child, str_filter = StringObject, StringFrame, MUIA_CycleChain, TRUE,
                    MUIA_ShortHelp, "Räume nach Namen filtern", End,
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
                    MUIA_Text_SetMin, FALSE,         /* a long error must not widen the window */
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

        SubWindow, win_prof = WindowObject,
            MUIA_Window_Title, "Eigenes Profil",
            MUIA_Window_ID, MAKE_ID('A','M','X','O'),
            WindowContents, VGroup,
                Child, HGroup,
                    Child, txt_pimg = TextObject, MUIA_Text_Contents, "", MUIA_FixWidth, AV + 4,
                                      MUIA_FixHeight, AV + 4, End,
                    Child, txt_pid = TextObject, MUIA_Text_Contents, "", MUIA_Text_SetMin, FALSE, End,
                End,
                Child, ColGroup(2),
                    Child, Label2("Name"),
                    Child, str_pname = StringObject, StringFrame, MUIA_String_MaxLen, 250, MUIA_CycleChain, TRUE, End,
                End,
                Child, HGroup,
                    Child, b_ppic = SimpleButton("_Bild ändern ..."),
                    Child, b_psave = SimpleButton("_Speichern"),
                End,
                Child, txt_pstatus = TextObject, MUIA_Text_Contents, "", MUIA_Text_SetMin, FALSE, End,
            End,
        End,

        SubWindow, win_fwd = WindowObject,
            MUIA_Window_Title, "Weiterleiten an ...",
            MUIA_Window_ID, MAKE_ID('A','M','X','W'),
            WindowContents, VGroup,
                Child, ListviewObject,
                    MUIA_Listview_List, lst_fwd = ListObject, InputListFrame,
                        MUIA_List_DisplayHook, &fwd_disp_hook, End,
                End,
                Child, b_fwd = SimpleButton("_Weiterleiten"),
            End,
        End,

        SubWindow, win_hist = WindowObject,
            MUIA_Window_Title, "Bearbeitungsverlauf",
            MUIA_Window_ID, MAKE_ID('A','M','X','H'),
            WindowContents, VGroup,
                Child, ListviewObject,
                    MUIA_Listview_List, lst_hist = ListObject, ReadListFrame, End,
                End,
            End,
        End,

        SubWindow, win_acct = WindowObject,
            MUIA_Window_Title, "Konto",
            MUIA_Window_ID, MAKE_ID('A','M','X','K'),
            WindowContents, VGroup,
                Child, VGroup, GroupFrameT("Passwort ändern"),
                    Child, ColGroup(2),
                        Child, Label2("Altes Passwort"),
                        Child, str_pwold = StringObject, StringFrame, MUIA_String_Secret, TRUE, MUIA_CycleChain, TRUE, End,
                        Child, Label2("Neues Passwort"),
                        Child, str_pwnew = StringObject, StringFrame, MUIA_String_Secret, TRUE, MUIA_CycleChain, TRUE, End,
                        Child, Label2("Noch einmal"),
                        Child, str_pwnew2 = StringObject, StringFrame, MUIA_String_Secret, TRUE, MUIA_CycleChain, TRUE, End,
                    End,
                    Child, b_pwgo = SimpleButton("_Passwort ändern"),
                End,
                Child, VGroup, GroupFrameT("Konto deaktivieren"),
                    Child, TextObject, MUIA_Text_Contents,
                        "\33bEndgültig:\33n das Konto ist danach nicht mehr nutzbar,\nder Name wird nie wieder vergeben.", End,
                    Child, ColGroup(2),
                        Child, Label2("Passwort"),
                        Child, str_dpw = StringObject, StringFrame, MUIA_String_Secret, TRUE, MUIA_CycleChain, TRUE, End,
                        Child, Label1("Meine Nachrichten löschen lassen"),
                        Child, HGroup, Child, chk_derase = CheckMark(FALSE), Child, HSpace(0), End,
                    End,
                    Child, b_dgo = SimpleButton("Konto _deaktivieren ..."),
                End,
                Child, txt_acct = TextObject, MUIA_Text_Contents, "", MUIA_Text_SetMin, FALSE, End,
            End,
        End,

        SubWindow, win_poll = WindowObject,
            MUIA_Window_Title, "Umfrage erstellen",
            MUIA_Window_ID, MAKE_ID('A','M','X','U'),
            WindowContents, VGroup,
                Child, ColGroup(2),
                    Child, Label2("Frage"),
                    Child, str_pollq = StringObject, StringFrame, MUIA_String_MaxLen, 300, MUIA_CycleChain, TRUE, End,
                    Child, Label2("Antwort 1"),
                    Child, str_polla[0] = StringObject, StringFrame, MUIA_String_MaxLen, 200, MUIA_CycleChain, TRUE, End,
                    Child, Label2("Antwort 2"),
                    Child, str_polla[1] = StringObject, StringFrame, MUIA_String_MaxLen, 200, MUIA_CycleChain, TRUE, End,
                    Child, Label2("Antwort 3"),
                    Child, str_polla[2] = StringObject, StringFrame, MUIA_String_MaxLen, 200, MUIA_CycleChain, TRUE, End,
                    Child, Label2("Antwort 4"),
                    Child, str_polla[3] = StringObject, StringFrame, MUIA_String_MaxLen, 200, MUIA_CycleChain, TRUE, End,
                    Child, Label2("Antwort 5"),
                    Child, str_polla[4] = StringObject, StringFrame, MUIA_String_MaxLen, 200, MUIA_CycleChain, TRUE, End,
                    Child, Label2("Antwort 6"),
                    Child, str_polla[5] = StringObject, StringFrame, MUIA_String_MaxLen, 200, MUIA_CycleChain, TRUE, End,
                    Child, Label1("Mehrere wählbar"),
                    Child, HGroup, Child, chk_pollmulti = CheckMark(FALSE), Child, HSpace(0), End,
                    Child, Label1("Ergebnis erst am Ende"),
                    Child, HGroup, Child, chk_pollhide = CheckMark(FALSE), Child, HSpace(0), End,
                End,
                Child, b_pollsend = SimpleButton("_Senden"),
            End,
        End,

        SubWindow, win_find = WindowObject,
            MUIA_Window_Title, "Nachrichten suchen",
            MUIA_Window_ID, MAKE_ID('A','M','X','F'),
            WindowContents, VGroup,
                Child, HGroup,
                    Child, str_find = StringObject, StringFrame, MUIA_CycleChain, TRUE, End,
                    Child, cyc_findwhere = CycleObject, MUIA_Cycle_Entries, where_names, End,
                    Child, b_find = SimpleButton("_Suchen"),
                End,
                Child, NListviewObject,
                    MUIA_NListview_NList, lst_hits = NListObject,
                        MUIA_NList_DisplayHook2, &hit_disp_hook,
                        MUIA_NList_Format, "BAR,BAR,",
                        MUIA_NList_Title, TRUE,
                    End,
                End,
                Child, txt_find = TextObject, MUIA_Text_Contents, "", MUIA_Text_SetMin, FALSE, End,
            End,
        End,

        SubWindow, win_sess = WindowObject,
            MUIA_Window_Title, "Sitzungen",
            MUIA_Window_ID, MAKE_ID('A','M','X','D'),
            WindowContents, VGroup,
                Child, NListviewObject,
                    MUIA_NListview_NList, lst_sess = NListObject,
                        MUIA_NList_DisplayHook2, &sess_disp_hook,
                        MUIA_NList_Format, "BAR,BAR,BAR,",
                        MUIA_NList_Title, TRUE,
                    End,
                End,
                Child, ColGroup(3),
                    Child, Label2("Name"),
                    Child, str_sname = StringObject, StringFrame, MUIA_String_MaxLen, 100, MUIA_CycleChain, TRUE, End,
                    Child, b_srename = SimpleButton("_Umbenennen"),
                    Child, Label2("Passwort"),
                    Child, str_spass = StringObject, StringFrame, MUIA_String_Secret, TRUE, MUIA_CycleChain, TRUE, End,
                    Child, b_sdelete = SimpleButton("_Abmelden"),
                End,
                Child, txt_sstatus = TextObject, MUIA_Text_Contents, "", MUIA_Text_SetMin, FALSE, End,
            End,
        End,

        SubWindow, win_info = WindowObject,
            MUIA_Window_Title, "Raum",
            MUIA_Window_ID, MAKE_ID('A','M','X','I'),
            WindowContents, VGroup,
                Child, grp_iset = VGroup, GroupFrameT("Einstellungen"),
                    Child, ColGroup(2),
                        Child, Label2("Name"),
                        Child, str_iname = StringObject, StringFrame, MUIA_String_MaxLen, 250, MUIA_CycleChain, TRUE, End,
                        Child, Label2("Thema"),
                        Child, str_itopic = StringObject, StringFrame, MUIA_String_MaxLen, 1000, MUIA_CycleChain, TRUE, End,
                        Child, Label1("Beitritt"),
                        Child, cyc_ijoin = CycleObject, MUIA_Cycle_Entries, join_names, MUIA_CycleChain, TRUE, End,
                        Child, Label1("Verlauf lesen"),
                        Child, cyc_ihist = CycleObject, MUIA_Cycle_Entries, hist_names, MUIA_CycleChain, TRUE, End,
                        Child, Label1("Benachrichtigungen"),
                        Child, cyc_inotify = CycleObject, MUIA_Cycle_Entries, notify_names, MUIA_CycleChain, TRUE,
                            MUIA_ShortHelp, "Gilt auf allen deinen Geräten (Push-Regeln auf dem Server)", End,
                        Child, Label1("Verschlüsselt"),
                        Child, HGroup,
                            Child, chk_ienc = CheckMark(FALSE),
                            Child, HSpace(0),
                            Child, b_ipic = SimpleButton("_Bild ..."),
                        End,
                    End,
                    Child, b_isave = SimpleButton("_Speichern"),
                End,
                Child, VGroup, GroupFrameT("Mitglieder"),
                    Child, NListviewObject,
                        MUIA_NListview_NList, lst_mem = NListObject,
                            MUIA_NList_DisplayHook2, &mem_disp_hook,
                            MUIA_NList_Format, "BAR,BAR,",
                            MUIA_NList_Title, TRUE,
                        End,
                    End,
                    Child, HGroup,
                        Child, b_iinvite = SimpleButton("_Einladen ..."),
                        Child, b_iadmit = SimpleButton("Ein_lassen"),
                        Child, b_ikick = SimpleButton("_Rauswerfen"),
                        Child, b_iban = SimpleButton("S_perren"),
                        Child, b_iunban = SimpleButton("_Entsperren"),
                    End,
                    Child, HGroup,
                        Child, Label1("Rolle"),
                        Child, cyc_irole = CycleObject, MUIA_Cycle_Entries, role_names, End,
                        Child, b_irole = SimpleButton("_Zuweisen"),
                    End,
                End,
                Child, txt_istatus = TextObject, MUIA_Text_Contents, "", MUIA_Text_SetMin, FALSE, End,
            End,
        End,

        SubWindow, win_settings = WindowObject,
            MUIA_Window_Title, "Amatrix - Einstellungen",
            MUIA_Window_ID, MAKE_ID('A','M','X','S'),
            WindowContents, VGroup,
                Child, VGroup, GroupFrameT("Benachrichtigungen"),
                    Child, txt_herald = TextObject, MUIA_Text_Contents, "", End,
                    Child, ColGroup(2),
                        Child, Label1("Benachrichtigen"),
                        Child, HGroup,
                            Child, chk_notify = CheckMark(TRUE),
                            Child, HSpace(0),
                        End,
                        Child, Label1("bei"),
                        Child, cyc_notify = CycleObject, MUIA_Cycle_Entries, notify_modes, End,
                    End,
                    Child, TextObject, MUIA_Text_Contents,
                        "\33iEinzelne Räume: Menü \"Raum stummschalten\"", End,
                End,
                Child, VGroup, GroupFrameT("Chat"),
                    Child, ColGroup(2),
                        Child, Label1("Bildvorschau im Chat"),
                        Child, HGroup,
                            Child, chk_previews = CheckMark(TRUE),
                            Child, HSpace(0),
                        End,
                    End,
                    Child, TextObject, MUIA_Text_Contents,
                        "\33iOhne Vorschau öffnet ein Klick auf das Datei-Symbol\nnoch immer das Bild.", End,
                    Child, ColGroup(2),
                        Child, Label1("Link-Vorschau"),
                        Child, HGroup,
                            Child, chk_linkprev = CheckMark(TRUE),
                            Child, HSpace(0),
                        End,
                        Child, Label1("auch in verschlüsselten Räumen"),
                        Child, HGroup,
                            Child, chk_linkprev_enc = CheckMark(FALSE),
                            Child, HSpace(0),
                        End,
                    End,
                    Child, TextObject, MUIA_Text_Contents,
                        "\33iDie Vorschau holt der Server - er sieht dann den Link.", End,
                End,
                Child, VGroup, GroupFrameT("Privatsphäre"),
                    Child, ColGroup(2),
                        Child, Label1("Zeigen, dass ich tippe"),
                        Child, HGroup,
                            Child, chk_typing = CheckMark(TRUE),
                            Child, HSpace(0),
                        End,
                        Child, Label1("Lesebestätigungen senden"),
                        Child, HGroup,
                            Child, chk_receipts = CheckMark(TRUE),
                            Child, HSpace(0),
                        End,
                        Child, Label1("Verlauf lokal speichern"),
                        Child, HGroup,
                            Child, chk_hcache = CheckMark(TRUE),
                            Child, HSpace(0),
                        End,
                    End,
                End,
                Child, VGroup, GroupFrameT("Stichwörter (wie eine Erwähnung)"),
                    Child, ListviewObject, MUIA_Weight, 30,
                        MUIA_Listview_List, lst_kw = ListObject, InputListFrame,
                            MUIA_List_ConstructHook, MUIV_List_ConstructHook_String,
                            MUIA_List_DestructHook, MUIV_List_DestructHook_String, End,
                    End,
                    Child, HGroup,
                        Child, str_kw = StringObject, StringFrame, MUIA_String_MaxLen, 60, MUIA_CycleChain, TRUE, End,
                        Child, b_kwadd = SimpleButton("_Hinzufügen"),
                        Child, b_kwdel = SimpleButton("_Entfernen"),
                    End,
                End,
                Child, VGroup, GroupFrameT("Ignorierte Nutzer"),
                    Child, ListviewObject, MUIA_Weight, 30,
                        MUIA_Listview_List, lst_ign = ListObject, InputListFrame, End,
                    End,
                    Child, b_unign = SimpleButton("Nicht mehr _ignorieren"),
                End,
                Child, VGroup, GroupFrameT("Start und Aussehen"),
                    Child, ColGroup(2),
                        Child, Label1("Ikonifiziert starten"),
                        Child, HGroup,
                            Child, chk_iconified = CheckMark(FALSE),
                            Child, HSpace(0),
                        End,
                        Child, Label1("Bilder in der Raumliste"),
                        Child, HGroup,
                            Child, chk_avatars = CheckMark(TRUE),
                            Child, HSpace(0),
                        End,
                        Child, Label1("Beitritte und Austritte zeigen"),
                        Child, HGroup, Child, chk_joins = CheckMark(TRUE), Child, HSpace(0), End,
                        Child, Label1("24-Stunden-Uhr"),
                        Child, HGroup, Child, chk_24h = CheckMark(TRUE), Child, HSpace(0), End,
                        Child, Label1("Kompakt (Name nur beim Wechsel)"),
                        Child, HGroup, Child, chk_compact = CheckMark(FALSE), Child, HSpace(0), End,
                    End,
                    Child, TextObject, MUIA_Text_Contents,
                        "\33iSchrift und Farben: Menü \"MUI-Einstellungen\".", End,
                    Child, ColGroup(2),
                    End,
                End,
                Child, b_set_ok = SimpleButton("_Speichern"),
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
    DoMethod(win_settings, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, win_settings, 3,
             MUIM_Set, MUIA_Window_Open, FALSE);
    DoMethod(b_set_ok, MUIM_Notify, MUIA_Pressed, FALSE, a, 2, MUIM_Application_ReturnID, ID_SET_OK);
    DoMethod(win_info, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, win_info, 3, MUIM_Set, MUIA_Window_Open, FALSE);
    DoMethod(win_prof, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, win_prof, 3, MUIM_Set, MUIA_Window_Open, FALSE);
    DoMethod(win_sess, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, win_sess, 3, MUIM_Set, MUIA_Window_Open, FALSE);
    DoMethod(win_find, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, win_find, 3, MUIM_Set, MUIA_Window_Open, FALSE);
    DoMethod(win_poll, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, win_poll, 3, MUIM_Set, MUIA_Window_Open, FALSE);
    DoMethod(win_fwd, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, win_fwd, 3, MUIM_Set, MUIA_Window_Open, FALSE);
    DoMethod(win_acct, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, win_acct, 3, MUIM_Set, MUIA_Window_Open, FALSE);
    DoMethod(b_pwgo, MUIM_Notify, MUIA_Pressed, FALSE, a, 2, MUIM_Application_ReturnID, ID_PW_GO);
    DoMethod(b_dgo, MUIM_Notify, MUIA_Pressed, FALSE, a, 2, MUIM_Application_ReturnID, ID_DEACT_GO);
    DoMethod(win_hist, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, win_hist, 3, MUIM_Set, MUIA_Window_Open, FALSE);
    DoMethod(b_fwd, MUIM_Notify, MUIA_Pressed, FALSE, a, 2, MUIM_Application_ReturnID, ID_FWD_GO);
    DoMethod(b_unign, MUIM_Notify, MUIA_Pressed, FALSE, a, 2, MUIM_Application_ReturnID, ID_UNIGNORE);
    DoMethod(b_kwadd, MUIM_Notify, MUIA_Pressed, FALSE, a, 2, MUIM_Application_ReturnID, ID_KW_ADD);
    DoMethod(str_kw, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime, a, 2, MUIM_Application_ReturnID, ID_KW_ADD);
    DoMethod(b_kwdel, MUIM_Notify, MUIA_Pressed, FALSE, a, 2, MUIM_Application_ReturnID, ID_KW_DEL);
    DoMethod(b_pollsend, MUIM_Notify, MUIA_Pressed, FALSE, a, 2, MUIM_Application_ReturnID, ID_POLLSEND);
    DoMethod(b_find, MUIM_Notify, MUIA_Pressed, FALSE, a, 2, MUIM_Application_ReturnID, ID_FIND_GO);
    DoMethod(str_find, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime, a, 2, MUIM_Application_ReturnID, ID_FIND_GO);
    DoMethod(lst_hits, MUIM_Notify, MUIA_NList_DoubleClick, MUIV_EveryTime, a, 2, MUIM_CallHook, &hit_open_hook);
    DoMethod(b_srename, MUIM_Notify, MUIA_Pressed, FALSE, a, 2, MUIM_Application_ReturnID, ID_SESS_RENAME);
    DoMethod(b_sdelete, MUIM_Notify, MUIA_Pressed, FALSE, a, 2, MUIM_Application_ReturnID, ID_SESS_DELETE);
    DoMethod(lst_sess, MUIM_Notify, MUIA_NList_Active, MUIV_EveryTime, a, 2, MUIM_CallHook, &sess_active_hook);
    DoMethod(b_psave, MUIM_Notify, MUIA_Pressed, FALSE, a, 2, MUIM_Application_ReturnID, ID_PROF_SAVE);
    DoMethod(b_ppic, MUIM_Notify, MUIA_Pressed, FALSE, a, 2, MUIM_Application_ReturnID, ID_PROF_PIC);
    DoMethod(str_pname, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime, a, 2, MUIM_Application_ReturnID,
             ID_PROF_SAVE);
    DoMethod(b_isave, MUIM_Notify, MUIA_Pressed, FALSE, a, 2, MUIM_Application_ReturnID, ID_INFO_SAVE);
    DoMethod(cyc_inotify, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime, a, 2, MUIM_CallHook, &inotify_hook);
    DoMethod(b_ipic, MUIM_Notify, MUIA_Pressed, FALSE, a, 2, MUIM_Application_ReturnID, ID_INFO_PIC);
    DoMethod(b_ikick, MUIM_Notify, MUIA_Pressed, FALSE, a, 2, MUIM_Application_ReturnID, ID_INFO_KICK);
    DoMethod(b_iban, MUIM_Notify, MUIA_Pressed, FALSE, a, 2, MUIM_Application_ReturnID, ID_INFO_BAN);
    DoMethod(b_iunban, MUIM_Notify, MUIA_Pressed, FALSE, a, 2, MUIM_Application_ReturnID, ID_INFO_UNBAN);
    DoMethod(b_irole, MUIM_Notify, MUIA_Pressed, FALSE, a, 2, MUIM_Application_ReturnID, ID_INFO_ROLE);
    DoMethod(b_iinvite, MUIM_Notify, MUIA_Pressed, FALSE, a, 2, MUIM_Application_ReturnID, ID_INFO_INVITE);
    DoMethod(b_iadmit, MUIM_Notify, MUIA_Pressed, FALSE, a, 2, MUIM_Application_ReturnID, ID_INFO_ADMIT);
    DoMethod(lst_mem, MUIM_Notify, MUIA_NList_Active, MUIV_EveryTime, a, 2, MUIM_CallHook, &mem_active_hook);
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
    DoMethod(str_filter, MUIM_Notify, MUIA_String_Contents, MUIV_EveryTime, a, 2, MUIM_CallHook, &filter_hook);
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
    case ID_COPY: {
        ChatWin *w = current_chat();
        ChatMsg *m = w ? marked_msg(w) : NULL;
        if (!m)
            MUI_Request(app, w ? w->win : win_main, 0, (STRPTR)"Amatrix", (STRPTR)"*_OK",
                        (STRPTR)"Bitte erst eine Nachricht im Chatfenster markieren.", 0);
        else copy_msg(w, m);
        break;
    }
    case ID_MUTE: {
        Room *r = NULL;
        DoMethod(lst_rooms, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &r);
        if (!r) {
            MUI_Request(app, win_main, 0, (STRPTR)"Amatrix", (STRPTR)"*_OK",
                        (STRPTR)"Bitte erst einen Raum in der Liste auswählen.", 0);
            break;
        }
        {   /* on the server, so all devices are quiet; the old local list goes */
            int off = !is_muted(r->id);
            set_muted(r->id, 0);
            save_prefs();
            amx_async_room_notify(A, r->id, off ? AMX_NOTIFY_OFF : AMX_NOTIFY_DEFAULT, TAG_CMD);
        }
        break;
    }
    case ID_SETTINGS:
        set(txt_herald, MUIA_Text_Contents, herald_here()
            ? "Benachrichtigungen erscheinen über Herald."
            : "\33bHerald läuft nicht\33n - ohne Herald (Aminet: util/cdity/Herald)\nzeigt Amatrix keine Benachrichtigungen.");
        set(chk_notify, MUIA_Selected, opt_notify);
        set(cyc_notify, MUIA_Cycle_Active, opt_notify_all ? 0 : 1);
        set(chk_previews, MUIA_Selected, opt_previews);
        set(chk_iconified, MUIA_Selected, opt_iconified);
        set(chk_typing, MUIA_Selected, opt_typing);
        amx_async_ignored(A, TAG_IGNLIST);
        amx_async_keywords(A, TAG_KEYWORDS);
        set(chk_receipts, MUIA_Selected, opt_receipts);
        set(chk_linkprev, MUIA_Selected, opt_linkprev);
        set(chk_linkprev_enc, MUIA_Selected, opt_linkprev_enc);
        set(chk_hcache, MUIA_Selected, opt_hcache);
        set(chk_avatars, MUIA_Selected, opt_avatars);
        set(chk_joins, MUIA_Selected, opt_joins);
        set(chk_24h, MUIA_Selected, opt_24h);
        set(chk_compact, MUIA_Selected, opt_compact);
        set(win_settings, MUIA_Window_Open, TRUE);
        break;
    case ID_INFO: {
        Room *r = NULL;
        DoMethod(lst_rooms, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &r);
        if (r && !r->invited) open_info(r->id);
        else MUI_Request(app, win_main, 0, (STRPTR)"Amatrix", (STRPTR)"*_OK",
                         (STRPTR)"Bitte erst einen Raum in der Liste auswählen.", 0);
        break;
    }
    case ID_PROFILE:     open_profile(); break;
    case ID_SESS:        open_sessions(); break;
    case ID_FAV:         toggle_tag("m.favourite"); break;
    case ID_LOWPRIO:     toggle_tag("m.lowpriority"); break;
    case ID_FIND:
        set(win_find, MUIA_Window_Open, TRUE);
        set(win_find, MUIA_Window_ActiveObject, str_find);
        break;
    case ID_FIND_GO:     find_go(); break;
    case ID_POLLNEW:     open_pollnew(); break;
    case ID_FWD_GO:      forward_go(); break;
    case ID_ACCOUNT:
        set(txt_acct, MUIA_Text_Contents, "");
        set(win_acct, MUIA_Window_Open, TRUE);
        break;
    case ID_PW_GO: pw_go(); break;
    case ID_DEACT_GO: {
        char *pw = (char *)xget(str_dpw, MUIA_String_Contents), *u;
        if (!pw || !*pw) { set(txt_acct, MUIA_Text_Contents, "Bitte dein Passwort eingeben."); break; }
        if (MUI_Request(app, win_acct, 0, (STRPTR)"Konto deaktivieren", (STRPTR)"_Deaktivieren|*_Abbrechen",
                        (STRPTR)"\33cDas Konto\n\33b%s\33n\nwirklich endgültig deaktivieren?", (ULONG)(my_user ? my_user : "")) != 1)
            break;
        u = amx_from_local(pw);
        amx_async_deactivate(A, u, (int)xget(chk_derase, MUIA_Selected), TAG_DEACT);
        if (u) memset(u, 0, strlen(u));
        free(u);
        set(str_dpw, MUIA_String_Contents, "");
        set(txt_acct, MUIA_Text_Contents, "Bitte warten ...");
        break;
    }
    case ID_EXPORT: {
        ChatWin *w = current_chat();
        struct Library *AslBase;
        struct FileRequester *fr;
        char path[512];
        LONG all;
        if (!w) {
            MUI_Request(app, win_main, 0, (STRPTR)"Amatrix", (STRPTR)"*_OK", (STRPTR)"Bitte erst ein Chatfenster öffnen.", 0);
            break;
        }
        all = MUI_Request(app, w->win, 0, (STRPTR)"Chat exportieren", (STRPTR)"_Ganzer Verlauf|_Nur geladene|*_Abbrechen",
                          (STRPTR)"Was soll in die Datei?\n(Endung .html: als Webseite, sonst Text)", 0);
        if (!all) break;
        if (!(AslBase = OpenLibrary("asl.library", 38))) break;
        fr = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
                 ASLFR_Window, (ULONG)xget(w->win, MUIA_Window_Window), ASLFR_TitleText, (ULONG)"Chat exportieren",
                 ASLFR_DoSaveMode, TRUE, ASLFR_InitialFile, (ULONG)"Chat.txt", TAG_DONE);
        if (fr && AslRequest(fr, NULL) && fr->fr_File[0]) {
            snprintf(path, sizeof path, "%s", fr->fr_Drawer);
            if (AddPart((STRPTR)path, (STRPTR)fr->fr_File, sizeof path)) export_chat(w, path, all == 1);
        }
        if (fr) FreeAslRequest(fr);
        CloseLibrary(AslBase);
        break;
    }
    case ID_KW_ADD: {
        char *t = (char *)xget(str_kw, MUIA_String_Contents), *u;
        if (t && *t) {
            u = amx_from_local(t);
            amx_async_keyword(A, u, 1, TAG_KEYWORDS);
            free(u);
            set(str_kw, MUIA_String_Contents, "");
        }
        break;
    }
    case ID_KW_DEL: {
        char *t = NULL, *u;
        DoMethod(lst_kw, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &t);
        if (t) { u = amx_from_local(t); amx_async_keyword(A, u, 0, TAG_KEYWORDS); free(u); }
        break;
    }
    case ID_UNIGNORE: {
        char *u = NULL;
        DoMethod(lst_ign, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, &u);
        if (u) { amx_async_ignore(A, u, 0, TAG_CMD); amx_async_ignored(A, TAG_IGNLIST); }
        break;
    }
    case ID_PIN: case ID_FWD: case ID_QUOTE: case ID_COPYLINK: case ID_EDITHIST: {
        ChatWin *w = current_chat();
        ChatMsg *m = w ? marked_msg(w) : NULL;
        if (!m || m->redacted) {
            MUI_Request(app, w ? w->win : win_main, 0, (STRPTR)"Amatrix", (STRPTR)"*_OK",
                        (STRPTR)"Bitte erst eine Nachricht im Chatfenster markieren.", 0);
            break;
        }
        if (id == ID_FWD) open_forward(w, m);
        else if (id == ID_QUOTE) start_quote(w, m);
        else if (id == ID_COPYLINK) copy_link(w, m);
        else if (id == ID_EDITHIST) {
            if (!m->edited) chat_say(w, "Diese Nachricht wurde nicht bearbeitet.");
            else open_edit_history(w, m);
        } else {
            const Room *r = room_by_id(w->room_id);
            int on = !(r && r->pinned && strstr(r->pinned, m->eid));
            amx_async_pin(A, w->room_id, m->eid, on, TAG_PIN);
            chat_say(w, on ? "Pinne an ..." : "Löse den Pin ...");
        }
        break;
    }
    case ID_POLLSEND:    poll_send(); break;
    case ID_MUIPREFS:    DoMethod(app, MUIM_Application_OpenConfigWindow, 0, NULL); break;
    case ID_SESS_RENAME: sess_rename(); break;
    case ID_SESS_DELETE: sess_delete(); break;
    case ID_PROF_SAVE:   prof_save(); break;
    case ID_PROF_PIC:    prof_picture(NULL); break;
    case ID_INFO_SAVE:   info_save(); break;
    case ID_INFO_PIC:    info_picture(); break;
    case ID_INFO_KICK:   info_moderate("kick"); break;
    case ID_INFO_BAN:    info_moderate("ban"); break;
    case ID_INFO_UNBAN:  info_moderate("unban"); break;
    case ID_INFO_ROLE:   info_moderate("power"); break;
    case ID_INFO_INVITE: if (info_room) show_invite(info_room); break;
    case ID_INFO_ADMIT: {
        AmxMember *p = info_selected();
        if (p && info_room) {
            amx_async_invite(A, info_room, p->user_id, TAG_MODERATE);
            info_busy++;
            info_set_status("Lasse ein ...");
            info_reload(0, 1);
        }
        break;
    }
    case ID_SET_OK:
        opt_notify = xget(chk_notify, MUIA_Selected) != 0;
        opt_notify_all = xget(cyc_notify, MUIA_Cycle_Active) == 0;
        opt_previews = xget(chk_previews, MUIA_Selected) != 0;
        opt_iconified = xget(chk_iconified, MUIA_Selected) != 0;
        {   /* the look: shown again in all windows */
            int j = xget(chk_joins, MUIA_Selected) != 0, h = xget(chk_24h, MUIA_Selected) != 0,
                k = xget(chk_compact, MUIA_Selected) != 0;
            if (j != opt_joins || h != opt_24h || k != opt_compact) {
                ChatWin *cw;
                opt_joins = j; opt_24h = h; opt_compact = k;
                for (cw = chats; cw; cw = cw->next) rerender_window(cw);
                DoMethod(lst_rooms, MUIM_List_Redraw, MUIV_List_Redraw_All);
            }
        }
        if (opt_avatars != (xget(chk_avatars, MUIA_Selected) != 0)) {
            opt_avatars = !opt_avatars;
            if (opt_avatars) request_avatars();
            DoMethod(lst_rooms, MUIM_List_Redraw, MUIV_List_Redraw_All);
        }
        opt_typing = xget(chk_typing, MUIA_Selected) != 0;
        opt_linkprev = xget(chk_linkprev, MUIA_Selected) != 0;
        opt_linkprev_enc = xget(chk_linkprev_enc, MUIA_Selected) != 0;
        if (opt_hcache != (xget(chk_hcache, MUIA_Selected) != 0)) {
            opt_hcache = !opt_hcache;
            amx_async_history_cache(A, opt_hcache);
        }
        if (opt_receipts != (xget(chk_receipts, MUIA_Selected) != 0)) {
            opt_receipts = !opt_receipts;
            amx_async_private_receipts(A, !opt_receipts);
        }
        save_prefs();
        set(win_settings, MUIA_Window_Open, FALSE);
        break;
    case ID_ICONIFY:
        set(app, MUIA_Application_Iconified, TRUE);
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
    LONG args[5] = { 0, 0, 0, 0, 0 };
    ULONG sigs = 0, id;
    int i;

    make_icon(&icon_up, art_up);
    make_icon(&icon_invite, art_invite);
    make_icon(&icon_leave, art_leave);
    make_icon(&icon_react, art_react);
    make_icon(&icon_reply, art_reply);
    make_icon(&icon_edit, art_edit);
    make_icon(&icon_trash, art_trash);
    make_icon(&icon_attach, art_attach);
    make_icon(&icon_info, art_info);
    make_emoji();
    if (!(MUIMasterBase = OpenLibrary((STRPTR)"muimaster.library", 20))) {
        printf("Amatrix braucht MUI 5 (muimaster.library V20).\n");
        return 20;
    }
    if (!NameFromLock(GetProgramDir(), (STRPTR)prog_dir, sizeof prog_dir - 2)) strcpy(prog_dir, "PROGDIR:");
    if (prog_dir[strlen(prog_dir) - 1] != ':') strcat(prog_dir, "/");
    load_prefs();
    started_at = time(NULL);
    if ((IconBase = OpenLibrary("icon.library", 37)) != NULL) {     /* for iconifying: the AppIcon */
        char ip[320];
        snprintf(ip, sizeof ip, "%sAmatrix", prog_dir);
        dob_app = GetDiskObject((STRPTR)ip);
        snprintf(ip, sizeof ip, "%sIcons/Amatrix-neu", prog_dir);
        dob_new = GetDiskObject((STRPTR)ip);
    }
    if (argc > 0) rda = ReadArgs((STRPTR)"SERVER/K,USER/K,PASSWORD/K,DEBUG/S,HIDE/S", args, NULL);
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
    if ((opt_iconified || args[4]) && dob_app) set(app, MUIA_Application_Iconified, TRUE);

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
    if (dob_app) FreeDiskObject(dob_app);
    if (dob_new) FreeDiskObject(dob_new);
    if (IconBase) CloseLibrary(IconBase);
    CloseLibrary(MUIMasterBase);
    return 0;
}
