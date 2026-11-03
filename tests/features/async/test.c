/* Feature: the client in its own process (amatrix_async.h).
 * Everything goes through commands and messages; the test is the "GUI":
 * login, room list, accept the peer's invitation, receive "ping 1", answer
 * "pong 1" while the network process sits in its 30 s long poll (it must be
 * woken up, not wait for the poll to end), load history, create a room with
 * an invitation, and stop quickly. */
#include "mxtest.h"
#include "amatrix_async.h"
#ifndef __amigaos__
#include <poll.h>
#else
#include <proto/dos.h>
#endif

static AmxAsync *A;
static AmxMsg *results[64];
static int n_results;
static int n_rooms_msgs, last_rooms_n, online = -1;
static int unread_first = -1, unread_now = -1;
static const char *watch_room;
static char *invited_room;

static void wait_msgs(int ms)
{
#ifdef __amigaos__
    Delay(ms / 20 > 0 ? ms / 20 : 1);
#else
    struct pollfd p;
    p.fd = amx_async_fd(A); p.events = POLLIN; p.revents = 0;
    poll(&p, 1, ms);
#endif
}

static void pump(void)
{
    AmxMsg *m;
    while ((m = amx_async_get(A)) != NULL) {
        switch (m->type) {
        case AMX_MSG_EVENT:
            mxt_record(NULL, &m->ev, NULL);
            amx_async_msg_free(m);
            break;
        case AMX_MSG_ROOMS: {
            int i;
            n_rooms_msgs++;
            last_rooms_n = m->n;
            for (i = 0; i < m->n; i++) {
                if (m->rooms[i].membership == AMX_ROOM_INVITED && !invited_room)
                    invited_room = mxt_dup(m->rooms[i].room_id);
                if (watch_room && !strcmp(m->rooms[i].room_id, watch_room)) unread_now = m->rooms[i].unread;
            }
            mxt_diag("rooms: %d", m->n);
            amx_async_msg_free(m);
            break;
        }
        case AMX_MSG_STATE:
            online = m->online;
            mxt_diag("state: %s %s", m->online ? "online" : "offline", m->error ? m->error : "");
            amx_async_msg_free(m);
            break;
        case AMX_MSG_RESULT:
            mxt_diag("result cmd %d tag %lu: %d %s %s", m->cmd, m->tag, m->status,
                     m->result ? m->result : "", m->error ? m->error : "");
            if (n_results < 64) results[n_results++] = m; else amx_async_msg_free(m);
            break;
        }
    }
}

static AmxMsg *find_result(unsigned long tag)
{
    int i;
    for (i = 0; i < n_results; i++) if (results[i]->tag == tag) return results[i];
    return NULL;
}

static AmxMsg *wait_result(unsigned long tag, int secs)
{
    time_t end = time(NULL) + secs;
    AmxMsg *r;
    do {
        pump();
        if ((r = find_result(tag)) != NULL) return r;
        wait_msgs(100);
    } while (time(NULL) < end);
    return NULL;
}

static int wait_for(int (*cond)(void *), void *arg, int secs)
{
    time_t end = time(NULL) + secs;
    do {
        pump();
        if (cond(arg)) return 1;
        wait_msgs(100);
    } while (time(NULL) < end);
    return 0;
}

static int no_unread(void *u) { (void)u; return unread_now == 0; }
static int have_invited(void *u) { (void)u; return invited_room != NULL; }
static int is_online(void *u) { (void)u; return online == 1; }

int main(int argc, char **argv)
{
    MxtArgs a;
    AmxMsg *r;
    MxtMsg want;
    char *room = NULL, *room2 = NULL;
    const char *inv[2];
    AmxRoomOptions opt;
    time_t t0;
    int i, hist = 0;
#ifdef __amigaos__
    int slow = 3;
#else
    int slow = 1;
#endif

    mxt_args(argc, argv, &a);
    A = amx_async_new(a.server, a.store);
    MXT_OK(A != NULL, "network process started");
    if (!A) return mxt_done();
    if (mxt_getenv("AMX_VERBOSE")) amx_async_set_verbose(A, atoi(mxt_getenv("AMX_VERBOSE")));

    amx_async_login(A, a.user, a.password, 1);
    r = wait_result(1, 120);
    MXT_OK(r && r->status == 0 && r->result && strchr(r->result, ':'), "login: %s",
           r ? (r->status ? r->error : r->result) : "no answer");
    if (!r || r->status) return mxt_done();
    MXT_OK(wait_for(is_online, NULL, 60), "online after the first sync");
    MXT_OK(n_rooms_msgs >= 1, "room list delivered (%d rooms)", last_rooms_n);

    MXT_OK(wait_for(have_invited, NULL, 180), "invitation shows up in the room list");
    if (!invited_room) return mxt_done();
    amx_async_join(A, invited_room, 2);
    r = wait_result(2, 120);
    MXT_OK(r && r->status == 0 && r->result && !strcmp(r->result, invited_room), "joined");
    room = invited_room;

    want.room = room; want.sender = NULL; want.body = "ping 1";
    i = wait_for(mxt_have_message, &want, 180);
    MXT_OK(i && mxt_find(AMX_EV_MESSAGE, room, NULL, "ping 1")->encrypted, "received 'ping 1', decrypted");

    /* let the network process settle into its long poll, then send */
    for (t0 = time(NULL); time(NULL) < t0 + 4; ) { pump(); wait_msgs(200); }
    t0 = time(NULL);
    amx_async_send_text(A, room, "pong 1", 3);
    r = wait_result(3, 120);
    MXT_OK(r && r->status == 0 && r->result && r->result[0] == '$', "sent 'pong 1': %s",
           r ? (r->status ? r->error : r->result) : "no answer");
    MXT_OK(r && time(NULL) - t0 < 8 * slow, "long poll woken up: sent after %lds (poll is 30s)",
           (long)(time(NULL) - t0));

    want.body = "ping 2";
    watch_room = room;
    MXT_OK(wait_for(mxt_have_message, &want, 120), "received 'ping 2'");
    {
        MxtEvent *e = mxt_find(AMX_EV_MESSAGE, room, NULL, "ping 2");
        for (t0 = time(NULL); time(NULL) < t0 + 3 * slow; ) { pump(); wait_msgs(200); }
        unread_first = unread_now;
        MXT_OK(unread_first > 0, "room list counts unread messages (%d)", unread_first);
        amx_async_mark_read(A, room, e ? e->event_id : "", 7);
        r = wait_result(7, 60);
        MXT_OK(r && r->status == 0, "marked as read");
        MXT_OK(wait_for(no_unread, NULL, 60), "unread count back to 0");
    }

    amx_async_load_history(A, room, 10, 1, 4);
    r = wait_result(4, 120);
    for (i = 0; i < mxt_n_ev; i++)
        if (mxt_ev[i].history && mxt_ev[i].body && !strcmp(mxt_ev[i].body, "ping 1")) hist++;
    MXT_OK(r && r->status == 0 && r->count > 0 && hist == 1,
           "history: %d events, 'ping 1' among them", r ? r->count : -1);

    memset(&opt, 0, sizeof opt);
    opt.name = "async room";
    opt.encrypted = 1;
    inv[0] = a.peer; inv[1] = NULL;
    opt.invite = inv;
    amx_async_create_room(A, &opt, 5);
    r = wait_result(5, 120);
    MXT_OK(r && r->status == 0 && r->result && r->result[0] == '!', "created a room: %s",
           r ? (r->status ? r->error : r->result) : "no answer");
    if (r && r->result) room2 = mxt_dup(r->result);

    amx_async_join(A, "#gibts-nicht:nowhere.invalid", 6);
    r = wait_result(6, 120);
    MXT_OK(r && r->status == -1 && r->error && r->error[0], "a failing command reports its error");

    /* the peer needs a moment to see the invitation; keep the process busy */
    for (t0 = time(NULL); time(NULL) < t0 + 10 * slow; ) { pump(); wait_msgs(200); }
    t0 = time(NULL);
    amx_async_free(A);
    MXT_OK(time(NULL) - t0 < 8 * slow, "stopped after %lds", (long)(time(NULL) - t0));
    for (i = 0; i < n_results; i++) amx_async_msg_free(results[i]);
    free(room);
    free(room2);
    return mxt_done();
}
