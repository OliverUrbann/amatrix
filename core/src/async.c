/* async.c - the client in its own process (see amatrix_async.h).
 *
 * AmigaOS: the network process is created with CreateNewProc() and has its
 * own bsdsocket/AmiSSL bases (they are per task, so only it may do network
 * I/O).  Commands travel as Exec messages to its command port; results and
 * events as Exec messages to the GUI's port.  The command port's signal is
 * also the bsdsocket break mask during the /sync long poll, so a command
 * wakes it up (recv() returns EINTR).
 *
 * POSIX (tests): a pthread, two mutex-protected queues and two pipes.
 */
#include "amx_internal.h"
#include "amatrix_async.h"
#include "net.h"

#ifdef __amigaos__
#include <exec/types.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#else
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#endif

#define CMD_QUIT 1000

typedef struct Cmd {
#ifdef __amigaos__
    struct Message msg;
#endif
    struct Cmd *next;
    int cmd;
    unsigned long tag;
    char *s1, *s2, *s3, *s4, *s5, *s6, *s7, *s8;
    int i1, i2;
    AmxRoomOptions opt;
    char **invite;
} Cmd;

typedef struct Out {
#ifdef __amigaos__
    struct Message msg;
#endif
    struct Out *next;
    AmxMsg m;
    char *own[24];                 /* strings owned by the message */
    int n_own;
} Out;

struct AmxAsync {
    char *hs, *store;
    volatile int verbose;
    /* network process side */
    AmxClient *c;
    int logged_in, online, backoff;
    char *rooms_sig;
#ifdef __amigaos__
    struct MsgPort *guiport;      /* created by the GUI task */
    struct MsgPort *cmdport;      /* created by the network process */
    struct Process *proc;
    Cmd *quit;                    /* replied (under Forbid) when the process ends */
#else
    pthread_t th;
    pthread_mutex_t mx;
    Cmd *cmd_head, *cmd_tail;
    Out *out_head, *out_tail;
    int gui_pipe[2], wake_pipe[2];
    int running;
#endif
};

/* ---------------------------------------------------------------- queues */

#define OUT_OF(m) ((Out *)((char *)(m) - offsetof(Out, m)))

#ifdef __amigaos__
static void put_cmd(AmxAsync *a, Cmd *c)
{
    c->msg.mn_Length = sizeof *c;
    PutMsg(a->cmdport, &c->msg);
}
static Cmd *get_cmd(AmxAsync *a) { return (Cmd *)GetMsg(a->cmdport); }
static void wait_cmd(AmxAsync *a) { WaitPort(a->cmdport); }
static int have_cmd(AmxAsync *a)
{
    return a->cmdport->mp_MsgList.lh_TailPred != (struct Node *)&a->cmdport->mp_MsgList;
}
/* clear the wake-up before looking at the queue: a later command sets it again */
static void clear_wake(AmxAsync *a) { SetSignal(0, 1UL << a->cmdport->mp_SigBit); }
static void wait_cmd_timeout(AmxAsync *a, int seconds)
{
    int i;
    for (i = 0; i < seconds * 10 && !have_cmd(a); i++) Delay(5);
}
static void put_out(AmxAsync *a, Out *o)
{
    o->msg.mn_Length = sizeof *o;
    PutMsg(a->guiport, &o->msg);
}
static Out *get_out(AmxAsync *a) { return (Out *)GetMsg(a->guiport); }
#else
static void put_cmd(AmxAsync *a, Cmd *c)
{
    char b = 1;
    c->next = NULL;
    pthread_mutex_lock(&a->mx);
    if (a->cmd_tail) a->cmd_tail->next = c; else a->cmd_head = c;
    a->cmd_tail = c;
    if (write(a->wake_pipe[1], &b, 1) < 0) { /* pipe full: already awake */ }
    pthread_mutex_unlock(&a->mx);
}
static Cmd *get_cmd(AmxAsync *a)
{
    Cmd *c;
    pthread_mutex_lock(&a->mx);
    if ((c = a->cmd_head) != NULL && !(a->cmd_head = c->next)) a->cmd_tail = NULL;
    pthread_mutex_unlock(&a->mx);
    return c;
}
static int have_cmd(AmxAsync *a)
{
    int r;
    pthread_mutex_lock(&a->mx);
    r = a->cmd_head != NULL;
    pthread_mutex_unlock(&a->mx);
    return r;
}
static void clear_wake(AmxAsync *a)
{
    char b[64];
    while (read(a->wake_pipe[0], b, sizeof b) > 0) ;
}
static void wait_cmd_timeout(AmxAsync *a, int seconds)
{
    struct pollfd p;
    if (have_cmd(a)) return;
    p.fd = a->wake_pipe[0]; p.events = POLLIN; p.revents = 0;
    poll(&p, 1, seconds < 0 ? -1 : seconds * 1000);
}
static void wait_cmd(AmxAsync *a) { wait_cmd_timeout(a, -1); }
static void put_out(AmxAsync *a, Out *o)
{
    char b = 1;
    o->next = NULL;
    pthread_mutex_lock(&a->mx);
    if (a->out_tail) a->out_tail->next = o; else a->out_head = o;
    a->out_tail = o;
    if (write(a->gui_pipe[1], &b, 1) < 0) { }
    pthread_mutex_unlock(&a->mx);
}
static Out *get_out(AmxAsync *a)
{
    Out *o;
    char b[64];
    pthread_mutex_lock(&a->mx);
    if ((o = a->out_head) != NULL && !(a->out_head = o->next)) a->out_tail = NULL;
    if (!a->out_head) while (read(a->gui_pipe[0], b, sizeof b) > 0) ;
    pthread_mutex_unlock(&a->mx);
    return o;
}
#endif

/* ------------------------------------------------------------- messages */

static Out *new_out(AmxMsgType t)
{
    Out *o = calloc(1, sizeof *o);
    if (o) o->m.type = t;
    return o;
}

static const char *own(Out *o, const char *s)
{
    char *d;
    if (!s || o->n_own >= (int)(sizeof o->own / sizeof o->own[0])) return NULL;
    if (!(d = strdup(s))) return NULL;
    o->own[o->n_own++] = d;
    return d;
}

void amx_async_msg_free(AmxMsg *m)
{
    Out *o;
    int i;
    if (!m) return;
    o = OUT_OF(m);
    for (i = 0; i < o->n_own; i++) free(o->own[i]);
    if (m->rooms) amx_free_rooms(m->rooms, m->n);
    if (m->pub) amx_free_public_rooms(m->pub, m->n);
    if (m->members) amx_free_members(m->members, m->n);
    if (m->devices) amx_free_devices(m->devices, m->n);
    if (m->hits) amx_free_search(m->hits, m->n);
    if (m->summary) {
        if (m->cmd == AMX_CMD_SPACE_ROOMS) amx_free_space_rooms(m->summary, m->n);
        else { amx_free_room_summary(m->summary); free(m->summary); }
    }
    if (m->settings) { amx_free_room_settings(m->settings); free(m->settings); }
    free(m->pixels);
    free(o);
}

static void post_state(AmxAsync *a, int online, const char *err)
{
    Out *o = new_out(AMX_MSG_STATE);
    if (!o) return;
    o->m.online = online;
    o->m.error = own(o, err);
    a->online = online;
    put_out(a, o);
}

static void post_result(AmxAsync *a, Cmd *c, int status, const char *result)
{
    Out *o = new_out(AMX_MSG_RESULT);
    if (!o) return;
    o->m.cmd = (AmxCmd)c->cmd;
    o->m.tag = c->tag;
    o->m.status = status;
    o->m.result = own(o, result);
    if (status) o->m.error = own(o, a->c ? amx_error(a->c) : "network not available");
    put_out(a, o);
}

static void on_event(AmxClient *c, const AmxEvent *ev, void *ud)
{
    AmxAsync *a = ud;
    Out *o = new_out(AMX_MSG_EVENT);
    (void)c;
    if (!o) return;
    o->m.ev = *ev;
    o->m.ev.room_id = own(o, ev->room_id);
    o->m.ev.event_id = own(o, ev->event_id);
    o->m.ev.sender = own(o, ev->sender);
    o->m.ev.msgtype = own(o, ev->msgtype);
    o->m.ev.body = own(o, ev->body);
    o->m.ev.state_key = own(o, ev->state_key);
    o->m.ev.reason = own(o, ev->reason);
    o->m.ev.sender_name = own(o, ev->sender_name);
    o->m.ev.relates_to = own(o, ev->relates_to);
    o->m.ev.thread_count = ev->thread_count;
    o->m.ev.media = own(o, ev->media);
    o->m.ev.poll = own(o, ev->poll);
    o->m.ev.reply_to = own(o, ev->reply_to);
    o->m.ev.thread_root = own(o, ev->thread_root);
    o->m.ev.formatted = own(o, ev->formatted);
    put_out(a, o);
}

/* the room list, if it differs from the last one sent */
static void sig_add(char **s, size_t *len, const char *v)
{
    size_t n = v ? strlen(v) : 0;
    char *t = realloc(*s, *len + n + 2);
    if (!t) return;
    *s = t;
    if (n) memcpy(t + *len, v, n);
    t[*len + n] = '\x1f';
    t[*len + n + 1] = 0;
    *len += n + 1;
}

static void post_rooms(AmxAsync *a, int force)
{
    AmxRoomInfo *r;
    int n = a->logged_in ? amx_rooms(a->c, &r) : 0, i;
    char *s = NULL, num[96];
    size_t len = 0;
    Out *o;

    if (n < 0) return;
    if (!a->logged_in) r = NULL;
    for (i = 0; i < n; i++) {
        sig_add(&s, &len, r[i].room_id);
        sig_add(&s, &len, r[i].name);
        sig_add(&s, &len, r[i].topic);
        sig_add(&s, &len, r[i].alias);
        sig_add(&s, &len, r[i].dm_user);
        sig_add(&s, &len, r[i].inviter);
        sig_add(&s, &len, r[i].inviter_name);
        sig_add(&s, &len, r[i].avatar_url);
        sig_add(&s, &len, r[i].last_sender);
        sig_add(&s, &len, r[i].last_body);
        sig_add(&s, &len, r[i].parents);
        sig_add(&s, &len, r[i].pinned);
        sig_add(&s, &len, r[i].replacement);
        sprintf(num, "%d %d %d %d %d %d %d %d %.0f %d %d %d %d", r[i].last_own, (int)r[i].membership, r[i].is_direct,
                r[i].encrypted, r[i].joined_members, r[i].invited_members, r[i].unread,
                r[i].highlights, r[i].last_activity, r[i].favourite, r[i].low_priority, r[i].is_space, r[i].notify);
        sig_add(&s, &len, num);
    }
    if (!s) s = strdup("");
    if (!force && a->rooms_sig && s && !strcmp(s, a->rooms_sig)) {
        free(s);
        if (r) amx_free_rooms(r, n);
        return;
    }
    free(a->rooms_sig);
    a->rooms_sig = s;
    if (!(o = new_out(AMX_MSG_ROOMS))) { if (r) amx_free_rooms(r, n); return; }
    o->m.rooms = r;
    o->m.n = n;
    put_out(a, o);
}

/* -------------------------------------------------------- network process */

static void free_cmd(Cmd *c)
{
    char **p;
    if (!c) return;
    free(c->s1); free(c->s2); free(c->s3); free(c->s4);
    free(c->s5); free(c->s6); free(c->s7); free(c->s8);
    if (c->invite) { for (p = c->invite; *p; p++) free(*p); free(c->invite); }
    free(c);
}

static void exec_cmd(AmxAsync *a, Cmd *c)
{
    char *out = NULL;
    int r = -1, at_start = 0;

    if (!a->c) { post_result(a, c, -1, NULL); return; }
    amx_set_verbose(a->c, a->verbose);
    LOG(a->c, "command %d (tag %lu) %s", c->cmd, c->tag,
        c->s1 && c->cmd != AMX_CMD_RECOVERY && c->cmd != AMX_CMD_LOGIN && c->cmd != AMX_CMD_STORE_UNLOCK
        && c->cmd != AMX_CMD_STORE_PASS ? c->s1 : "");
    switch (c->cmd) {
    case AMX_CMD_LOGIN:
        r = amx_login(a->c, c->s1, c->s2 ? c->s2 : "");
        if (!r) {
            a->logged_in = 1;
            a->backoff = 0;
            a->online = -1;               /* report the state after the first sync */
        }
        {
            Out *o = new_out(AMX_MSG_RESULT);
            if (o) {
                o->m.cmd = AMX_CMD_LOGIN;
                o->m.tag = c->tag;
                o->m.status = r;
                if (r) o->m.error = own(o, amx_error(a->c));
                else {
                    o->m.result = own(o, amx_user_id(a->c));
                    o->m.verified = amx_device_verified(a->c) == 1;
                }
                put_out(a, o);
            }
        }
        if (!r) post_rooms(a, 1);        /* the saved list, before the first sync */
        return;
    case AMX_CMD_LOGOUT:
        r = amx_logout(a->c);
        a->logged_in = 0;
        post_result(a, c, r, NULL);
        post_rooms(a, 1);
        return;
    case AMX_CMD_SEND_TEXT:
        r = amx_send_text(a->c, c->s1, c->s2, &out);
        break;
    case AMX_CMD_JOIN:
        r = amx_join(a->c, c->s1, &out);
        break;
    case AMX_CMD_LEAVE:
        r = amx_leave(a->c, c->s1);
        break;
    case AMX_CMD_INVITE:
        r = amx_invite(a->c, c->s1, c->s2);
        break;
    case AMX_CMD_MARK_READ:
        r = amx_mark_read(a->c, c->s1, c->s2);
        break;
    case AMX_CMD_LOAD_THREAD: {
        int at_start = 0, n = amx_load_thread(a->c, c->s1, c->s2, c->i1, c->i2, &at_start);
        Out *o = new_out(AMX_MSG_RESULT);
        if (o) {
            o->m.cmd = AMX_CMD_LOAD_THREAD;
            o->m.tag = c->tag;
            o->m.status = n < 0 ? -1 : 0;
            o->m.result = own(o, c->s2);
            o->m.count = n < 0 ? 0 : n;
            o->m.at_start = at_start;
            if (n < 0) o->m.error = own(o, amx_error(a->c));
            put_out(a, o);
        }
        return;
    }
    case AMX_CMD_SEND_REPLY:
        r = amx_send_reply(a->c, c->s1, c->s3 && c->s3[0] ? c->s3 : NULL, c->s4 && c->s4[0] ? c->s4 : NULL,
                           c->s2, &out);
        break;
    case AMX_CMD_MEMBERS: {
        AmxMember *l = NULL;
        int n = amx_room_members(a->c, c->s1, &l);
        Out *o = new_out(AMX_MSG_RESULT);
        if (o) {
            o->m.cmd = AMX_CMD_MEMBERS;
            o->m.tag = c->tag;
            o->m.status = n < 0 ? -1 : 0;
            o->m.result = own(o, c->s1);
            o->m.members = n > 0 ? l : NULL;
            o->m.n = n > 0 ? n : 0;
            if (n < 0) o->m.error = own(o, amx_error(a->c));
            if (n == 0) amx_free_members(l, 0);
            put_out(a, o);
        } else amx_free_members(l, n > 0 ? n : 0);
        return;
    }
    case AMX_CMD_ROOM_SETTINGS: {
        AmxRoomSettings *st = calloc(1, sizeof *st);
        Out *o = new_out(AMX_MSG_RESULT);
        r = st ? amx_room_settings(a->c, c->s1, st) : -1;
        if (o) {
            o->m.cmd = AMX_CMD_ROOM_SETTINGS;
            o->m.tag = c->tag;
            o->m.status = r;
            o->m.result = own(o, c->s1);
            if (r) { o->m.error = own(o, amx_error(a->c)); free(st); }
            else o->m.settings = st;
            put_out(a, o);
        } else if (st) { amx_free_room_settings(st); free(st); }
        return;
    }
    case AMX_CMD_ROOM_SET:
        r = amx_set_room_setting(a->c, c->s1, c->s2, c->s3);
        post_result(a, c, r, c->s1);
        return;
    case AMX_CMD_MODERATE:
        if (!strcmp(c->s3, "kick")) r = amx_kick(a->c, c->s1, c->s2, c->s4);
        else if (!strcmp(c->s3, "ban")) r = amx_ban(a->c, c->s1, c->s2, c->s4);
        else if (!strcmp(c->s3, "unban")) r = amx_unban(a->c, c->s1, c->s2);
        else r = amx_set_power(a->c, c->s1, c->s2, c->i1);
        post_result(a, c, r, c->s1);
        return;
    case AMX_CMD_PROFILE:
    case AMX_CMD_PROFILE_OF: {
        char *nm = NULL, *av = NULL;
        Out *o = new_out(AMX_MSG_RESULT);
        r = amx_get_profile(a->c, c->cmd == AMX_CMD_PROFILE_OF ? c->s1 : NULL, &nm, &av);
        if (!r && c->cmd == AMX_CMD_PROFILE_OF && !nm && !av) r = amx__fail(a->c, "no such user");
        if (o) {
            o->m.cmd = c->cmd;
            o->m.tag = c->tag;
            o->m.status = r;
            o->m.result = own(o, nm);
            o->m.result2 = own(o, av);
            if (r) o->m.error = own(o, amx_error(a->c));
            put_out(a, o);
        }
        free(nm);
        free(av);
        return;
    }
    case AMX_CMD_SET_PROFILE:
        r = !strcmp(c->s1, "avatar") ? amx_set_avatar(a->c, c->s2) : amx_set_displayname(a->c, c->s2);
        post_result(a, c, r, c->s1);
        return;
    case AMX_CMD_URL_PREVIEW: {
        char *t = NULL, *d = NULL, *im = NULL;
        Out *o = new_out(AMX_MSG_RESULT);
        r = amx_url_preview(a->c, c->s1, &t, &d, &im);
        if (o) {
            o->m.cmd = AMX_CMD_URL_PREVIEW;
            o->m.tag = c->tag;
            o->m.status = r;
            o->m.result = own(o, c->s2);
            o->m.result2 = own(o, c->s1);
            o->m.title = own(o, t);
            o->m.description = own(o, d);
            o->m.image = own(o, im);
            if (r) o->m.error = own(o, amx_error(a->c));
            put_out(a, o);
        }
        free(t); free(d); free(im);
        return;
    }
    case AMX_CMD_DEVICES: {
        AmxDevice *l = NULL;
        int n = amx_devices(a->c, &l);
        Out *o = new_out(AMX_MSG_RESULT);
        if (o) {
            o->m.cmd = AMX_CMD_DEVICES;
            o->m.tag = c->tag;
            o->m.status = n < 0 ? -1 : 0;
            o->m.devices = n > 0 ? l : NULL;
            o->m.n = n > 0 ? n : 0;
            if (n < 0) o->m.error = own(o, amx_error(a->c));
            if (n == 0) amx_free_devices(l, 0);
            put_out(a, o);
        } else amx_free_devices(l, n > 0 ? n : 0);
        return;
    }
    case AMX_CMD_DEVICE_RENAME:
        r = amx_rename_device(a->c, c->s1, c->s2);
        post_result(a, c, r, c->s1);
        return;
    case AMX_CMD_DEVICE_DELETE:
        r = amx_delete_device(a->c, c->s1, c->s2);
        if (c->s2) memset(c->s2, 0, strlen(c->s2));
        post_result(a, c, r, c->s1);
        return;
    case AMX_CMD_SET_TAG:
        r = amx_set_room_tag(a->c, c->s1, c->s2, c->i1);
        post_result(a, c, r, c->s1);
        if (!r) post_rooms(a, 0);
        return;
    case AMX_CMD_SEARCH: {
        AmxSearchHit *h = NULL;
        int n = amx_search(a->c, c->s1 && *c->s1 ? c->s1 : NULL, c->s2, &h);
        Out *o = new_out(AMX_MSG_RESULT);
        if (o) {
            o->m.cmd = AMX_CMD_SEARCH;
            o->m.tag = c->tag;
            o->m.status = n < 0 ? -1 : 0;
            o->m.result = own(o, c->s2);
            o->m.hits = n > 0 ? h : NULL;
            o->m.n = n > 0 ? n : 0;
            if (n < 0) o->m.error = own(o, amx_error(a->c));
            if (n == 0) amx_free_search(h, 0);
            put_out(a, o);
        } else amx_free_search(h, n > 0 ? n : 0);
        return;
    }
    case AMX_CMD_SEND_POLL:
    case AMX_CMD_POLL_VOTE: {
        const char *list[32];
        char *t = c->s3 ? c->s3 : "", *p;
        int n = 0;
        for (p = t; *p && n < 32; ) {
            char *nl = strchr(p, '\n');
            list[n++] = p;
            if (!nl) break;
            *nl = 0;
            p = nl + 1;
        }
        if (c->cmd == AMX_CMD_SEND_POLL) r = amx_send_poll(a->c, c->s1, c->s2, list, n, c->i1, c->i2, &out);
        else r = amx_poll_vote(a->c, c->s1, c->s2, list, n);
        post_result(a, c, r, out ? out : c->s1);
        free(out);
        return;
    }
    case AMX_CMD_POLL_END:
        r = amx_poll_end(a->c, c->s1, c->s2);
        post_result(a, c, r, c->s1);
        return;
    case AMX_CMD_RELATIONS:
        r = amx_load_relations(a->c, c->s1, c->s2) < 0 ? -1 : 0;
        post_result(a, c, r, c->s2);
        return;
    case AMX_CMD_SEND_MSG: {
        AmxOutMsg om;
        om.msgtype = c->s2; om.body = c->s3; om.html = c->s4; om.reply_to = c->s5; om.thread_root = c->s6;
        om.replaces = c->s7; om.mentions = c->s8;
        r = amx_send_message(a->c, c->s1, &om, &out);
        post_result(a, c, r, out);
        free(out);
        return;
    }
    case AMX_CMD_IGNORED: {
        char *l = amx_ignored_users(a->c);
        post_result(a, c, l ? 0 : -1, l);
        free(l);
        return;
    }
    case AMX_CMD_IGNORE:
        r = amx_ignore_user(a->c, c->s1, c->i1);
        post_result(a, c, r, c->s1);
        return;
    case AMX_CMD_PIN:
        r = amx_pin_event(a->c, c->s1, c->s2, c->i1);
        post_result(a, c, r, c->s1);
        if (!r) post_rooms(a, 0);
        return;
    case AMX_CMD_FETCH_EVENT:
        r = amx_fetch_event(a->c, c->s1, c->s2);
        post_result(a, c, r, c->s2);
        return;
    case AMX_CMD_ROOM_NOTIFY:
        r = amx_set_room_notify(a->c, c->s1, c->i1);
        post_result(a, c, r, c->s1);
        if (!r) post_rooms(a, 0);
        return;
    case AMX_CMD_KEYWORDS:
    case AMX_CMD_KEYWORD: {
        char *l;
        r = c->cmd == AMX_CMD_KEYWORD ? amx_set_keyword(a->c, c->s1, c->i1) : 0;
        l = amx_keywords(a->c);
        post_result(a, c, r, l);
        free(l);
        return;
    }
    case AMX_CMD_PASSWORD:
        r = amx_change_password(a->c, c->s1, c->s2);
        if (c->s1) memset(c->s1, 0, strlen(c->s1));
        if (c->s2) memset(c->s2, 0, strlen(c->s2));
        post_result(a, c, r, NULL);
        return;
    case AMX_CMD_DEACTIVATE:
        r = amx_deactivate_account(a->c, c->s1, c->i1);
        if (c->s1) memset(c->s1, 0, strlen(c->s1));
        post_result(a, c, r, NULL);
        return;
    case AMX_CMD_UPGRADE:
        r = amx_upgrade_room(a->c, c->s1, c->s2, &out);
        post_result(a, c, r, out);
        free(out);
        if (!r) post_rooms(a, 0);
        return;
    case AMX_CMD_KNOCK:
        r = amx_knock(a->c, c->s1, c->s2);
        post_result(a, c, r, c->s1);
        return;
    case AMX_CMD_EDITS:
        r = amx_load_edits(a->c, c->s1, c->s2) < 0 ? -1 : 0;
        post_result(a, c, r, c->s2);
        return;
    case AMX_CMD_FORWARD:
        r = amx_forward(a->c, c->s1, c->s2, c->s3, &out);
        post_result(a, c, r, out ? out : c->s1);
        free(out);
        return;
    case AMX_CMD_ROOM_NICK:
        r = amx_set_room_nick(a->c, c->s1, c->s2);
        post_result(a, c, r, c->s1);
        return;
    case AMX_CMD_ROOM_SUMMARY: {
        AmxRoomSummary *sm = calloc(1, sizeof *sm);
        Out *o = new_out(AMX_MSG_RESULT);
        r = sm ? amx_room_summary(a->c, c->s1, sm) : -1;
        if (o) {
            o->m.cmd = AMX_CMD_ROOM_SUMMARY;
            o->m.tag = c->tag;
            o->m.status = r;
            o->m.result = own(o, c->s1);
            if (r) { o->m.error = own(o, amx_error(a->c)); free(sm); }
            else o->m.summary = sm;
            put_out(a, o);
        } else if (sm) { amx_free_room_summary(sm); free(sm); }
        return;
    }
    case AMX_CMD_SPACE_ROOMS: {
        AmxRoomSummary *l = NULL;
        int n = amx_space_rooms(a->c, c->s1, 2, &l);
        Out *o = new_out(AMX_MSG_RESULT);
        if (o) {
            o->m.cmd = AMX_CMD_SPACE_ROOMS;
            o->m.tag = c->tag;
            o->m.status = n < 0 ? -1 : 0;
            o->m.result = own(o, c->s1);
            o->m.summary = n > 0 ? l : NULL;
            o->m.n = n > 0 ? n : 0;
            if (n < 0) o->m.error = own(o, amx_error(a->c));
            if (n == 0) amx_free_space_rooms(l, 0);
            put_out(a, o);
        } else amx_free_space_rooms(l, n > 0 ? n : 0);
        return;
    }
    case AMX_CMD_SPACE_ADD:
        r = amx_space_add(a->c, c->s1, c->s2, c->i1);
        post_result(a, c, r, c->s1);
        return;
    case AMX_CMD_REPORT:
        r = amx_report_event(a->c, c->s1, c->s2, c->s3);
        post_result(a, c, r, c->s2);
        return;
    case AMX_CMD_RECEIPTS:
        amx_replay_receipts(a->c, c->s1);
        return;
    case AMX_CMD_HISTORY_CACHE:
        amx_set_history_cache(a->c, c->i1);
        return;
    case AMX_CMD_TYPING:
        amx_send_typing(a->c, c->s1, c->i1);
        return;
    case AMX_CMD_PRIVATE_RECEIPTS:
        amx_set_private_receipts(a->c, c->i1);
        return;
    case AMX_CMD_SEND_FILE:
        r = amx_send_file(a->c, c->s1, c->s2, c->s4 && c->s4[0] ? c->s4 : NULL, c->s3 && c->s3[0] ? c->s3 : NULL,
                          &out);
        break;
    case AMX_CMD_EDIT:
        r = amx_send_edit(a->c, c->s1, c->s3, c->s2, &out);
        break;
    case AMX_CMD_REACT:
        r = amx_send_reaction(a->c, c->s1, c->s2, c->s3, &out);
        break;
    case AMX_CMD_REDACT:
        r = amx_redact(a->c, c->s1, c->s2, NULL);
        break;
    case AMX_CMD_STORE_UNLOCK:
        r = amx_store_unlock(a->c, c->s1 ? c->s1 : "");
        if (c->s1) memset(c->s1, 0, strlen(c->s1));
        break;
    case AMX_CMD_STORE_PASS:
        r = amx_store_set_passphrase(a->c, c->s1 && *c->s1 ? c->s1 : NULL);
        if (c->s1) memset(c->s1, 0, strlen(c->s1));
        break;
    case AMX_CMD_CREATE_ROOM:
        c->opt.invite = (const char **)c->invite;
        r = amx_create_room(a->c, &c->opt, &out);
        break;
    case AMX_CMD_LOAD_HISTORY: {
        int n;
        if (c->i2) amx_cached_history(a->c, c->s1);      /* at once, before the network */
        n = amx_load_history(a->c, c->s1, c->i1, c->i2, &at_start);
        Out *o = new_out(AMX_MSG_RESULT);
        if (o) {
            o->m.cmd = AMX_CMD_LOAD_HISTORY;
            o->m.tag = c->tag;
            o->m.status = n < 0 ? -1 : 0;
            o->m.count = n < 0 ? 0 : n;
            o->m.at_start = at_start;
            o->m.result = own(o, c->s1);
            if (n < 0) o->m.error = own(o, amx_error(a->c));
            put_out(a, o);
        }
        post_rooms(a, 0);       /* keys from the backup may have changed the previews */
        return;
    }
    case AMX_CMD_RECOVERY: {
        int v = 0, b = 0;
        Out *o = new_out(AMX_MSG_RESULT);
        r = amx_recovery_unlock(a->c, c->s1 ? c->s1 : "", &v, &b);
        if (c->s1) memset(c->s1, 0, strlen(c->s1));      /* do not keep the key around */
        if (o) {
            o->m.cmd = AMX_CMD_RECOVERY;
            o->m.tag = c->tag;
            o->m.status = r;
            o->m.verified = v;
            o->m.backup = b;
            if (r) o->m.error = own(o, amx_error(a->c));
            put_out(a, o);
        }
        post_rooms(a, 0);
        return;
    }
    case AMX_CMD_AVATAR: {
        unsigned long *px = NULL;
        Out *o = new_out(AMX_MSG_RESULT);
        r = amx_avatar(a->c, c->s1, c->i1, &px);
        if (o) {
            o->m.cmd = AMX_CMD_AVATAR;
            o->m.tag = c->tag;
            o->m.status = r;
            o->m.result = own(o, c->s1);
            o->m.pixels = px;
            o->m.size = r ? 0 : c->i1;
            if (r) o->m.error = own(o, amx_error(a->c));
            put_out(a, o);
        } else free(px);
        return;
    }
    case AMX_CMD_MEDIA_THUMB: {
        unsigned long *px = NULL;
        int w = 0, h = 0;
        Out *o = new_out(AMX_MSG_RESULT);
        r = amx_media_thumbnail(a->c, c->s1, c->i1 >> 16, c->i1 & 0xffff, c->i2, &px, &w, &h);
        if (o) {
            o->m.cmd = AMX_CMD_MEDIA_THUMB;
            o->m.tag = c->tag;
            o->m.status = r;
            o->m.result = own(o, c->s2);              /* the caller's key (event ID) */
            o->m.pixels = px;
            o->m.width = w;
            o->m.height = h;
            if (r) o->m.error = own(o, amx_error(a->c));
            put_out(a, o);
        } else free(px);
        return;
    }
    case AMX_CMD_MEDIA_SAVE:
        r = amx_media_save(a->c, c->s1, c->i1, c->s2);
        out = r ? NULL : (c->s2 ? strdup(c->s2) : NULL);
        break;
    case AMX_CMD_PUBLIC_ROOMS: {
        AmxPublicRoom *l = NULL;
        int n = amx_public_rooms(a->c, c->s1, c->s2, c->i1, &l);
        Out *o = new_out(AMX_MSG_RESULT);
        if (o) {
            o->m.cmd = AMX_CMD_PUBLIC_ROOMS;
            o->m.tag = c->tag;
            o->m.status = n < 0 ? -1 : 0;
            o->m.pub = n > 0 ? l : NULL;
            o->m.n = n > 0 ? n : 0;
            if (n < 0) o->m.error = own(o, amx_error(a->c));
            put_out(a, o);
        } else if (n > 0) amx_free_public_rooms(l, n);
        return;
    }
    }
    post_result(a, c, r, out);
    free(out);
    if (!r && c->cmd != AMX_CMD_SEND_TEXT) post_rooms(a, 0);
}

static void worker(AmxAsync *a)
{
    Cmd *c;
    int first = 1;

    if (amx_init() == 0) {
        a->c = amx_client_new(a->hs, a->store);
        if (a->c) {
            amx_set_event_handler(a->c, on_event, a);
            a->c->interruptible = 1;
        }
    }
    if (!a->c) post_state(a, 0, "network not available (TCP/IP stack, AmiSSL?)");
#ifdef __amigaos__
    net_set_wakeup((long)(1UL << a->cmdport->mp_SigBit));
#else
    net_set_wakeup(a->wake_pipe[0]);
#endif

    for (;;) {
        clear_wake(a);
        while ((c = get_cmd(a)) != NULL) {
            if (c->cmd == CMD_QUIT) {
#ifdef __amigaos__
                a->quit = c;
#else
                free_cmd(c);
#endif
                goto out;
            }
            exec_cmd(a, c);
            free_cmd(c);
        }
        if (!a->logged_in || !a->c) { wait_cmd(a); first = 1; continue; }

        amx_set_verbose(a->c, a->verbose);
        {
            int r;
            LOG2(a->c, "sync (%s)", first ? "first" : "long poll");
            r = amx_sync(a->c, first ? 0 : 30000);
            LOG2(a->c, "sync returned %d", r);
            if (r == 1) continue;                     /* a command woke us up */
            if (r < 0) {
                if (!a->logged_in) continue;
                if (a->online != 0) post_state(a, 0, amx_error(a->c));
                a->backoff = a->backoff ? (a->backoff * 2 > 60 ? 60 : a->backoff * 2) : 2;
                wait_cmd_timeout(a, a->backoff);
                continue;
            }
            first = 0;
            a->backoff = 0;
            if (a->online != 1) post_state(a, 1, NULL);
            post_rooms(a, 0);
        }
    }
out:
    if (a->c) amx_client_free(a->c);
    a->c = NULL;
    amx_cleanup();
}

/* --------------------------------------------------------------- start/stop */

#ifdef __amigaos__
struct StartMsg { struct Message msg; AmxAsync *a; };

static void worker_entry(void)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    struct StartMsg *sm;
    AmxAsync *a;

    WaitPort(&me->pr_MsgPort);
    sm = (struct StartMsg *)GetMsg(&me->pr_MsgPort);
    a = sm->a;
    a->cmdport = CreateMsgPort();
    ReplyMsg(&sm->msg);
    if (!a->cmdport) return;
    worker(a);
    /* reply under Forbid(): the GUI may only unload us after we are gone */
    Forbid();
    DeleteMsgPort(a->cmdport);
    a->cmdport = NULL;
    if (a->quit) ReplyMsg(&a->quit->msg);
}
#else
static void *worker_thread(void *p) { worker(p); return NULL; }
#endif

AmxAsync *amx_async_new(const char *homeserver, const char *store_dir)
{
    AmxAsync *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    a->hs = strdup(homeserver);
    a->store = strdup(store_dir);
#ifdef __amigaos__
    {
        struct StartMsg sm;
        struct MsgPort *reply = CreateMsgPort();
        a->guiport = CreateMsgPort();
        if (!reply || !a->guiport) goto fail;
        a->proc = CreateNewProcTags(NP_Entry, (ULONG)worker_entry,
                                    NP_Name, (ULONG)"Amatrix network",
                                    NP_StackSize, 256 * 1024,
                                    NP_Priority, -1,
                                    TAG_DONE);
        if (!a->proc) { DeleteMsgPort(reply); goto fail; }
        memset(&sm, 0, sizeof sm);
        sm.msg.mn_ReplyPort = reply;
        sm.msg.mn_Length = sizeof sm;
        sm.a = a;
        PutMsg(&a->proc->pr_MsgPort, &sm.msg);
        WaitPort(reply);
        GetMsg(reply);
        DeleteMsgPort(reply);
        if (!a->cmdport) goto fail;
    }
    return a;
fail:
    if (a->guiport) DeleteMsgPort(a->guiport);
#else
    if (pipe(a->gui_pipe) || pipe(a->wake_pipe)) goto fail;
    fcntl(a->gui_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(a->gui_pipe[1], F_SETFL, O_NONBLOCK);
    fcntl(a->wake_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(a->wake_pipe[1], F_SETFL, O_NONBLOCK);
    pthread_mutex_init(&a->mx, NULL);
    if (pthread_create(&a->th, NULL, worker_thread, a)) goto fail;
    a->running = 1;
    return a;
fail:
#endif
    free(a->hs); free(a->store); free(a);
    return NULL;
}

void amx_async_free(AmxAsync *a)
{
    AmxMsg *m;
    if (!a) return;
#ifdef __amigaos__
    {
        struct MsgPort *reply = CreateMsgPort();
        Cmd *q = calloc(1, sizeof *q);
        if (reply && q) {
            q->cmd = CMD_QUIT;
            q->msg.mn_ReplyPort = reply;
            put_cmd(a, q);
            WaitPort(reply);
            GetMsg(reply);
        }
        free(q);
        if (reply) DeleteMsgPort(reply);
    }
    while ((m = amx_async_get(a)) != NULL) amx_async_msg_free(m);
    DeleteMsgPort(a->guiport);
#else
    if (a->running) {
        Cmd *q = calloc(1, sizeof *q);
        q->cmd = CMD_QUIT;
        put_cmd(a, q);
        pthread_join(a->th, NULL);
    }
    while ((m = amx_async_get(a)) != NULL) amx_async_msg_free(m);
    { Cmd *c; while ((c = get_cmd(a)) != NULL) free_cmd(c); }
    close(a->gui_pipe[0]); close(a->gui_pipe[1]);
    close(a->wake_pipe[0]); close(a->wake_pipe[1]);
    pthread_mutex_destroy(&a->mx);
#endif
    free(a->rooms_sig);
    free(a->hs); free(a->store); free(a);
}

unsigned long amx_async_sigmask(AmxAsync *a)
{
#ifdef __amigaos__
    return 1UL << a->guiport->mp_SigBit;
#else
    (void)a;
    return 0;
#endif
}

int amx_async_fd(AmxAsync *a)
{
#ifdef __amigaos__
    (void)a;
    return -1;
#else
    return a->gui_pipe[0];
#endif
}

AmxMsg *amx_async_get(AmxAsync *a)
{
    Out *o = get_out(a);
    return o ? &o->m : NULL;
}

void amx_async_set_verbose(AmxAsync *a, int level) { a->verbose = level; }

/* --------------------------------------------------------------- commands */

static char *dup0(const char *s) { return s ? strdup(s) : NULL; }

static int cmd(AmxAsync *a, int what, unsigned long tag, const char *s1, const char *s2,
               int i1, int i2)
{
    Cmd *c = calloc(1, sizeof *c);
    if (!c) return -1;
    c->cmd = what;
    c->tag = tag;
    c->s1 = dup0(s1);
    c->s2 = dup0(s2);
    c->i1 = i1;
    c->i2 = i2;
    put_cmd(a, c);
    return 0;
}

int amx_async_login(AmxAsync *a, const char *user, const char *password, unsigned long tag)
{ return cmd(a, AMX_CMD_LOGIN, tag, user, password, 0, 0); }
int amx_async_logout(AmxAsync *a, unsigned long tag)
{ return cmd(a, AMX_CMD_LOGOUT, tag, NULL, NULL, 0, 0); }
int amx_async_send_text(AmxAsync *a, const char *room_id, const char *body, unsigned long tag)
{ return cmd(a, AMX_CMD_SEND_TEXT, tag, room_id, body, 0, 0); }
int amx_async_join(AmxAsync *a, const char *room, unsigned long tag)
{ return cmd(a, AMX_CMD_JOIN, tag, room, NULL, 0, 0); }
int amx_async_leave(AmxAsync *a, const char *room_id, unsigned long tag)
{ return cmd(a, AMX_CMD_LEAVE, tag, room_id, NULL, 0, 0); }
int amx_async_invite(AmxAsync *a, const char *room_id, const char *user_id, unsigned long tag)
{ return cmd(a, AMX_CMD_INVITE, tag, room_id, user_id, 0, 0); }
int amx_async_load_history(AmxAsync *a, const char *room_id, int limit, int from_newest,
                           unsigned long tag)
{ return cmd(a, AMX_CMD_LOAD_HISTORY, tag, room_id, NULL, limit, from_newest); }
int amx_async_public_rooms(AmxAsync *a, const char *server, const char *search, int limit,
                           unsigned long tag)
{ return cmd(a, AMX_CMD_PUBLIC_ROOMS, tag, server, search, limit, 0); }

int amx_async_mark_read(AmxAsync *a, const char *room_id, const char *event_id, unsigned long tag)
{ return cmd(a, AMX_CMD_MARK_READ, tag, room_id, event_id, 0, 0); }
int amx_async_store_unlock(AmxAsync *a, const char *pass, unsigned long tag)
{ return cmd(a, AMX_CMD_STORE_UNLOCK, tag, pass, NULL, 0, 0); }
int amx_async_store_passphrase(AmxAsync *a, const char *pass, unsigned long tag)
{ return cmd(a, AMX_CMD_STORE_PASS, tag, pass ? pass : "", NULL, 0, 0); }
int amx_async_recovery(AmxAsync *a, const char *key, unsigned long tag)
{ return cmd(a, AMX_CMD_RECOVERY, tag, key, NULL, 0, 0); }
int amx_async_avatar(AmxAsync *a, const char *mxc_url, int size, unsigned long tag)
{ return cmd(a, AMX_CMD_AVATAR, tag, mxc_url, NULL, size, 0); }

int amx_async_create_room(AmxAsync *a, const AmxRoomOptions *opt, unsigned long tag)
{
    Cmd *c = calloc(1, sizeof *c);
    int n = 0, i;
    if (!c) return -1;
    c->cmd = AMX_CMD_CREATE_ROOM;
    c->tag = tag;
    c->opt = *opt;
    c->opt.name = c->s1 = dup0(opt->name);
    c->opt.topic = c->s2 = dup0(opt->topic);
    if (opt->invite) {
        while (opt->invite[n]) n++;
        c->invite = calloc(n + 1, sizeof *c->invite);
        for (i = 0; i < n && c->invite; i++) c->invite[i] = strdup(opt->invite[i]);
    }
    c->opt.invite = NULL;
    put_cmd(a, c);
    return 0;
}

int amx_async_react(AmxAsync *a, const char *room_id, const char *event_id, const char *key, unsigned long tag)
{
    Cmd *c = calloc(1, sizeof *c);
    if (!c) return -1;
    c->cmd = AMX_CMD_REACT;
    c->tag = tag;
    c->s1 = dup0(room_id);
    c->s2 = dup0(event_id);
    c->s3 = dup0(key);
    put_cmd(a, c);
    return 0;
}

int amx_async_redact(AmxAsync *a, const char *room_id, const char *event_id, unsigned long tag)
{ return cmd(a, AMX_CMD_REDACT, tag, room_id, event_id, 0, 0); }

int amx_async_media_thumbnail(AmxAsync *a, const char *media, int index, const char *key, int max_w,
                              int max_h, unsigned long tag)
{ return cmd(a, AMX_CMD_MEDIA_THUMB, tag, media, key, (index << 16) | (max_w & 0xffff), max_h); }

int amx_async_media_save(AmxAsync *a, const char *media, int index, const char *path, unsigned long tag)
{ return cmd(a, AMX_CMD_MEDIA_SAVE, tag, media, path, index, 0); }

int amx_async_send_reply(AmxAsync *a, const char *room_id, const char *reply_to, const char *thread_root,
                         const char *body, unsigned long tag)
{
    Cmd *c = calloc(1, sizeof *c);
    if (!c) return -1;
    c->cmd = AMX_CMD_SEND_REPLY;
    c->tag = tag;
    c->s1 = dup0(room_id);
    c->s2 = dup0(body);
    c->s3 = dup0(reply_to);
    c->s4 = dup0(thread_root);
    put_cmd(a, c);
    return 0;
}

int amx_async_edit(AmxAsync *a, const char *room_id, const char *event_id, const char *body, unsigned long tag)
{
    Cmd *c = calloc(1, sizeof *c);
    if (!c) return -1;
    c->cmd = AMX_CMD_EDIT;
    c->tag = tag;
    c->s1 = dup0(room_id);
    c->s2 = dup0(body);
    c->s3 = dup0(event_id);
    put_cmd(a, c);
    return 0;
}

int amx_async_load_thread(AmxAsync *a, const char *room_id, const char *root, int limit, int from_newest,
                          unsigned long tag)
{ return cmd(a, AMX_CMD_LOAD_THREAD, tag, room_id, root, limit, from_newest); }

int amx_async_send_file(AmxAsync *a, const char *room_id, const char *path, const char *name,
                        const char *thread_root, unsigned long tag)
{
    Cmd *c = calloc(1, sizeof *c);
    if (!c) return -1;
    c->cmd = AMX_CMD_SEND_FILE;
    c->tag = tag;
    c->s1 = dup0(room_id);
    c->s2 = dup0(path);
    c->s3 = dup0(thread_root);
    c->s4 = dup0(name);
    put_cmd(a, c);
    return 0;
}

int amx_async_typing(AmxAsync *a, const char *room_id, long ms)
{ return cmd(a, AMX_CMD_TYPING, 0, room_id, NULL, (int)ms, 0); }
int amx_async_private_receipts(AmxAsync *a, int on)
{ return cmd(a, AMX_CMD_PRIVATE_RECEIPTS, 0, NULL, NULL, on, 0); }

int amx_async_members(AmxAsync *a, const char *room_id, unsigned long tag)
{ return cmd(a, AMX_CMD_MEMBERS, tag, room_id, NULL, 0, 0); }
int amx_async_room_settings(AmxAsync *a, const char *room_id, unsigned long tag)
{ return cmd(a, AMX_CMD_ROOM_SETTINGS, tag, room_id, NULL, 0, 0); }

int amx_async_room_set(AmxAsync *a, const char *room_id, const char *what, const char *value, unsigned long tag)
{
    Cmd *c = calloc(1, sizeof *c);
    if (!c) return -1;
    c->cmd = AMX_CMD_ROOM_SET;
    c->tag = tag;
    c->s1 = dup0(room_id);
    c->s2 = dup0(what);
    c->s3 = dup0(value);
    put_cmd(a, c);
    return 0;
}

int amx_async_moderate(AmxAsync *a, const char *room_id, const char *user, const char *action,
                       const char *reason, int level, unsigned long tag)
{
    Cmd *c = calloc(1, sizeof *c);
    if (!c) return -1;
    c->cmd = AMX_CMD_MODERATE;
    c->tag = tag;
    c->s1 = dup0(room_id);
    c->s2 = dup0(user);
    c->s3 = dup0(action);
    c->s4 = dup0(reason);
    c->i1 = level;
    put_cmd(a, c);
    return 0;
}

int amx_async_profile(AmxAsync *a, unsigned long tag)
{ return cmd(a, AMX_CMD_PROFILE, tag, NULL, NULL, 0, 0); }
int amx_async_set_profile(AmxAsync *a, const char *what, const char *value, unsigned long tag)
{ return cmd(a, AMX_CMD_SET_PROFILE, tag, what, value, 0, 0); }

int amx_async_url_preview(AmxAsync *a, const char *url, const char *key, unsigned long tag)
{ return cmd(a, AMX_CMD_URL_PREVIEW, tag, url, key, 0, 0); }

int amx_async_devices(AmxAsync *a, unsigned long tag)
{ return cmd(a, AMX_CMD_DEVICES, tag, NULL, NULL, 0, 0); }
int amx_async_device_rename(AmxAsync *a, const char *device_id, const char *name, unsigned long tag)
{ return cmd(a, AMX_CMD_DEVICE_RENAME, tag, device_id, name, 0, 0); }
int amx_async_device_delete(AmxAsync *a, const char *device_id, const char *password, unsigned long tag)
{ return cmd(a, AMX_CMD_DEVICE_DELETE, tag, device_id, password, 0, 0); }

int amx_async_history_cache(AmxAsync *a, int on)
{ return cmd(a, AMX_CMD_HISTORY_CACHE, 0, NULL, NULL, on, 0); }

int amx_async_set_tag(AmxAsync *a, const char *room_id, const char *tag, int on, unsigned long tag_)
{ return cmd(a, AMX_CMD_SET_TAG, tag_, room_id, tag, on, 0); }
int amx_async_search(AmxAsync *a, const char *room_id, const char *term, unsigned long tag)
{ return cmd(a, AMX_CMD_SEARCH, tag, room_id ? room_id : "", term, 0, 0); }

static int cmd4(AmxAsync *a, int what, unsigned long tag, const char *s1, const char *s2, const char *s3, int i1, int i2)
{
    Cmd *c = calloc(1, sizeof *c);
    if (!c) return -1;
    c->cmd = what;
    c->tag = tag;
    c->s1 = dup0(s1);
    c->s2 = dup0(s2);
    c->s3 = dup0(s3);
    c->i1 = i1;
    c->i2 = i2;
    put_cmd(a, c);
    return 0;
}

int amx_async_send_poll(AmxAsync *a, const char *room_id, const char *question, const char *answers, int max,
                        int disclosed, unsigned long tag)
{ return cmd4(a, AMX_CMD_SEND_POLL, tag, room_id, question, answers, max, disclosed); }
int amx_async_poll_vote(AmxAsync *a, const char *room_id, const char *poll_id, const char *answer_ids,
                        unsigned long tag)
{ return cmd4(a, AMX_CMD_POLL_VOTE, tag, room_id, poll_id, answer_ids ? answer_ids : "", 0, 0); }
int amx_async_poll_end(AmxAsync *a, const char *room_id, const char *poll_id, unsigned long tag)
{ return cmd(a, AMX_CMD_POLL_END, tag, room_id, poll_id, 0, 0); }
int amx_async_relations(AmxAsync *a, const char *room_id, const char *event_id, unsigned long tag)
{ return cmd(a, AMX_CMD_RELATIONS, tag, room_id, event_id, 0, 0); }

int amx_async_send_message(AmxAsync *a, const char *room_id, const AmxOutMsg *m, unsigned long tag)
{
    Cmd *c = calloc(1, sizeof *c);
    if (!c) return -1;
    c->cmd = AMX_CMD_SEND_MSG;
    c->tag = tag;
    c->s1 = dup0(room_id);
    c->s2 = dup0(m->msgtype); c->s3 = dup0(m->body); c->s4 = dup0(m->html); c->s5 = dup0(m->reply_to);
    c->s6 = dup0(m->thread_root); c->s7 = dup0(m->replaces); c->s8 = dup0(m->mentions);
    put_cmd(a, c);
    return 0;
}
int amx_async_room_nick(AmxAsync *a, const char *room_id, const char *name, unsigned long tag)
{ return cmd(a, AMX_CMD_ROOM_NICK, tag, room_id, name, 0, 0); }
int amx_async_profile_of(AmxAsync *a, const char *user, unsigned long tag)
{ return cmd(a, AMX_CMD_PROFILE_OF, tag, user, NULL, 0, 0); }

int amx_async_ignored(AmxAsync *a, unsigned long tag)
{ return cmd(a, AMX_CMD_IGNORED, tag, NULL, NULL, 0, 0); }
int amx_async_ignore(AmxAsync *a, const char *user, int on, unsigned long tag)
{ return cmd(a, AMX_CMD_IGNORE, tag, user, NULL, on, 0); }

int amx_async_pin(AmxAsync *a, const char *room_id, const char *event_id, int on, unsigned long tag)
{ return cmd(a, AMX_CMD_PIN, tag, room_id, event_id, on, 0); }
int amx_async_fetch_event(AmxAsync *a, const char *room_id, const char *event_id, unsigned long tag)
{ return cmd(a, AMX_CMD_FETCH_EVENT, tag, room_id, event_id, 0, 0); }

int amx_async_forward(AmxAsync *a, const char *to_room, const char *from_room, const char *event_id, unsigned long tag)
{ return cmd4(a, AMX_CMD_FORWARD, tag, to_room, from_room, event_id, 0, 0); }

int amx_async_edits(AmxAsync *a, const char *room_id, const char *event_id, unsigned long tag)
{ return cmd(a, AMX_CMD_EDITS, tag, room_id, event_id, 0, 0); }

int amx_async_room_notify(AmxAsync *a, const char *room_id, int level, unsigned long tag)
{ return cmd(a, AMX_CMD_ROOM_NOTIFY, tag, room_id, NULL, level, 0); }
int amx_async_keywords(AmxAsync *a, unsigned long tag)
{ return cmd(a, AMX_CMD_KEYWORDS, tag, NULL, NULL, 0, 0); }
int amx_async_keyword(AmxAsync *a, const char *word, int on, unsigned long tag)
{ return cmd(a, AMX_CMD_KEYWORD, tag, word, NULL, on, 0); }

int amx_async_change_password(AmxAsync *a, const char *old_pw, const char *new_pw, unsigned long tag)
{ return cmd(a, AMX_CMD_PASSWORD, tag, old_pw, new_pw, 0, 0); }
int amx_async_deactivate(AmxAsync *a, const char *password, int erase, unsigned long tag)
{ return cmd(a, AMX_CMD_DEACTIVATE, tag, password, NULL, erase, 0); }

int amx_async_knock(AmxAsync *a, const char *room, const char *reason, unsigned long tag)
{ return cmd(a, AMX_CMD_KNOCK, tag, room, reason, 0, 0); }

int amx_async_upgrade(AmxAsync *a, const char *room_id, const char *version, unsigned long tag)
{ return cmd(a, AMX_CMD_UPGRADE, tag, room_id, version, 0, 0); }

int amx_async_room_summary(AmxAsync *a, const char *room, unsigned long tag)
{ return cmd(a, AMX_CMD_ROOM_SUMMARY, tag, room, NULL, 0, 0); }
int amx_async_space_rooms(AmxAsync *a, const char *space_id, unsigned long tag)
{ return cmd(a, AMX_CMD_SPACE_ROOMS, tag, space_id, NULL, 0, 0); }
int amx_async_space_add(AmxAsync *a, const char *space_id, const char *room_id, int on, unsigned long tag)
{ return cmd(a, AMX_CMD_SPACE_ADD, tag, space_id, room_id, on, 0); }
int amx_async_report(AmxAsync *a, const char *room_id, const char *event_id, const char *reason, unsigned long tag)
{
    Cmd *c = calloc(1, sizeof *c);
    if (!c) return -1;
    c->cmd = AMX_CMD_REPORT;
    c->tag = tag;
    c->s1 = dup0(room_id);
    c->s2 = dup0(event_id);
    c->s3 = dup0(reason);
    put_cmd(a, c);
    return 0;
}

int amx_async_receipts(AmxAsync *a, const char *room_id)
{ return cmd(a, AMX_CMD_RECEIPTS, 0, room_id, NULL, 0, 0); }
