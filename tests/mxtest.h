/* mxtest.h - tiny test framework for the feature tests (C side).
 *
 * Output is TAP (ok N - name / not ok N - name / # diag / 1..N), the same on
 * Linux and on the Amiga, so tests/run.py can read it from both.
 *
 * Every feature test is a program:
 *     test SERVER USER PASSWORD PEER_USER STORE_DIR [EXTRA...]
 * It talks to its counterpart (tests/features/<name>/peer.py) only through
 * the Matrix server: invites, rooms and messages.
 *
 * Include this file in exactly one .c file (it defines functions).
 */
#ifndef MXTEST_H
#define MXTEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include "amatrix.h"

#define MXT_FN static __attribute__((unused))

#ifdef __amigaos__
#include <proto/dos.h>
unsigned long __stack = 256 * 1024;
MXT_FN const char *mxt_getenv(const char *name)
{
    static char buf[64];
    return GetVar((STRPTR)name, (STRPTR)buf, sizeof buf, 0) > 0 ? buf : NULL;
}
#else
#define mxt_getenv getenv
#endif

/* ------------------------------------------------------------- TAP output */

static int mxt_n, mxt_failed;

MXT_FN void mxt_ok(int ok, const char *fmt, ...)
{
    va_list ap;
    printf("%sok %d - ", ok ? "" : "not ", ++mxt_n);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
    if (!ok) mxt_failed++;
}
#define MXT_OK(cond, ...) mxt_ok(!!(cond), __VA_ARGS__)

MXT_FN void mxt_diag(const char *fmt, ...)
{
    va_list ap;
    printf("# ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

MXT_FN int mxt_done(void)
{
    printf("1..%d\n", mxt_n);
    fflush(stdout);
    return mxt_failed ? 1 : 0;
}

/* -------------------------------------------------------------- arguments */

typedef struct {
    const char *server, *user, *password, *peer, *store;
    int n_extra;
    char **extra;
} MxtArgs;

MXT_FN void mxt_args(int argc, char **argv, MxtArgs *a)
{
    if (argc < 6) {
        printf("usage: %s SERVER USER PASSWORD PEER_USER STORE_DIR [EXTRA...]\n", argv[0]);
        exit(10);
    }
    a->server = argv[1]; a->user = argv[2]; a->password = argv[3];
    a->peer = argv[4]; a->store = argv[5];
    a->n_extra = argc - 6; a->extra = argv + 6;
    setvbuf(stdout, NULL, _IOLBF, 0);
}

/* --------------------------------------------------------- recorded events */

typedef struct {
    AmxEventType type;
    char *room_id, *event_id, *sender, *body, *reason, *state_key, *relates_to, *media;
    int encrypted, own, history;
    double timestamp;
} MxtEvent;

#define MXT_MAX_EVENTS 512
static MxtEvent mxt_ev[MXT_MAX_EVENTS];
static int mxt_n_ev;

MXT_FN char *mxt_dup(const char *s) { return s ? strcpy(malloc(strlen(s) + 1), s) : NULL; }

MXT_FN void mxt_record(AmxClient *c, const AmxEvent *e, void *ud)
{
    MxtEvent *m;
    (void)c; (void)ud;
    if (mxt_n_ev >= MXT_MAX_EVENTS) return;
    m = &mxt_ev[mxt_n_ev++];
    m->type = e->type;
    m->room_id = mxt_dup(e->room_id);
    m->event_id = mxt_dup(e->event_id);
    m->sender = mxt_dup(e->sender);
    m->body = mxt_dup(e->body);
    m->reason = mxt_dup(e->reason);
    m->state_key = mxt_dup(e->state_key);
    m->relates_to = mxt_dup(e->relates_to);
    m->media = mxt_dup(e->media);
    m->encrypted = e->encrypted;
    m->own = e->own;
    m->history = e->history;
    m->timestamp = e->timestamp;
    if (e->type == AMX_EV_MESSAGE) {
        char *l = amx_to_local(e->body ? e->body : "");
        mxt_diag("event message %s from %s (%s): %s%s%s", e->room_id, e->sender,
                 e->sender_name ? e->sender_name : "-", l,
                 e->encrypted ? " (encrypted)" : "", e->history ? " (history)" : "");
        free(l);
    } else if (e->type == AMX_EV_UNDECRYPTABLE) {
        mxt_diag("event undecryptable %s from %s: %s", e->event_id ? e->event_id : "?",
                 e->sender ? e->sender : "?", e->reason);
    } else if (e->type == AMX_EV_INVITE) {
        mxt_diag("event invite %s from %s", e->room_id, e->sender ? e->sender : "?");
    } else if (e->type == AMX_EV_ROOM_KEY) {
        mxt_diag("event room key %s from %s", e->room_id, e->sender);
    } else if (e->type == AMX_EV_MEMBERSHIP) {
        mxt_diag("event member %s %s %s", e->room_id, e->state_key ? e->state_key : "?", e->body ? e->body : "?");
    } else if (e->type == AMX_EV_REACTION) {
        mxt_diag("event reaction %s from %s to %s: %d bytes%s%s", e->event_id ? e->event_id : "?",
                 e->sender ? e->sender : "?", e->relates_to ? e->relates_to : "?",
                 e->body ? (int)strlen(e->body) : -1, e->encrypted ? " (encrypted)" : "",
                 e->history ? " (history)" : "");
    } else if (e->type == AMX_EV_REDACTION) {
        mxt_diag("event redaction %s from %s of %s", e->event_id ? e->event_id : "?",
                 e->sender ? e->sender : "?", e->relates_to ? e->relates_to : "?");
    }
}

/* first recorded event matching (any field NULL = don't care), or NULL */
MXT_FN MxtEvent *mxt_find(AmxEventType type, const char *room, const char *sender, const char *body)
{
    int i;
    for (i = 0; i < mxt_n_ev; i++) {
        MxtEvent *m = &mxt_ev[i];
        if (m->type != type) continue;
        if (room && (!m->room_id || strcmp(m->room_id, room))) continue;
        if (sender && (!m->sender || strcmp(m->sender, sender))) continue;
        if (body && (!m->body || strcmp(m->body, body))) continue;
        return m;
    }
    return NULL;
}

MXT_FN int mxt_count(AmxEventType type, const char *room, const char *sender)
{
    int i, n = 0;
    for (i = 0; i < mxt_n_ev; i++) {
        MxtEvent *m = &mxt_ev[i];
        if (m->type != type) continue;
        if (room && (!m->room_id || strcmp(m->room_id, room))) continue;
        if (sender && (!m->sender || strcmp(m->sender, sender))) continue;
        n++;
    }
    return n;
}

/* ----------------------------------------------------------------- client */

/* amx_init + client + login, events recorded.  Exits on failure. */
MXT_FN AmxClient *mxt_client(const MxtArgs *a)
{
    static int inited;
    AmxClient *c;
    if (!inited) {
        if (amx_init()) { mxt_ok(0, "network init"); exit(mxt_done()); }
        inited = 1;
    }
    c = amx_client_new(a->server, a->store);
    if (!c) { mxt_ok(0, "client for store %s", a->store); exit(mxt_done()); }
    if (mxt_getenv("AMX_VERBOSE")) amx_set_verbose(c, atoi(mxt_getenv("AMX_VERBOSE")));
    amx_set_event_handler(c, mxt_record, NULL);
    if (amx_login(c, a->user, a->password)) {
        mxt_ok(0, "login as %s: %s", a->user, amx_error(c));
        exit(mxt_done());
    }
    return c;
}

/* Syncs until cond() is true or secs have passed.  1 if cond was met. */
MXT_FN int mxt_sync_until(AmxClient *c, int (*cond)(void *), void *arg, int secs)
{
    time_t end = time(NULL) + secs;
    while (time(NULL) < end) {
        long left = (long)(end - time(NULL));
        if (cond(arg)) return 1;
        if (amx_sync(c, left > 5 ? 5000 : left * 1000))
            mxt_diag("sync: %s", amx_error(c));
    }
    return cond(arg);
}

/* common conditions */
MXT_FN int mxt_have_invite(void *unused) { (void)unused; return mxt_find(AMX_EV_INVITE, NULL, NULL, NULL) != NULL; }

typedef struct { const char *room, *sender, *body; } MxtMsg;
MXT_FN int mxt_have_message(void *p)
{
    MxtMsg *m = p;
    return mxt_find(AMX_EV_MESSAGE, m->room, m->sender, m->body) != NULL;
}

typedef struct { const char *room, *user, *membership; } MxtMember;
MXT_FN int mxt_have_membership(void *p)
{
    MxtMember *m = p;
    int i;
    for (i = 0; i < mxt_n_ev; i++) {
        MxtEvent *e = &mxt_ev[i];
        if (e->type == AMX_EV_MEMBERSHIP && e->room_id && !strcmp(e->room_id, m->room) &&
            e->state_key && !strcmp(e->state_key, m->user) && e->body && !strcmp(e->body, m->membership))
            return 1;
    }
    return 0;
}

#endif
