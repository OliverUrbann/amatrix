/* Feature: replies, edits, deleted messages, threads.  The peer writes
 * "Original"; we answer it, the peer answers our answer (with the old quoted
 * fallback, which we take off), we edit our answer, the peer edits its first
 * message, deletes one and writes in a thread on "Original"; we write in that
 * thread too.  The history must show the deleted message as deleted. */
#include "mxtest.h"

static char *room;

static int have_room(void *arg)
{
    AmxClient *c = arg;
    AmxRoomInfo *l;
    int n = amx_rooms(c, &l), i;
    for (i = 0; i < n && !room; i++)
        if (l[i].membership == AMX_ROOM_INVITED) room = mxt_dup(l[i].room_id);
    amx_free_rooms(l, n);
    return room != NULL;
}

static MxtEvent *by_type_body(int type, const char *body, int history)
{
    int i;
    for (i = 0; i < mxt_n_ev; i++)
        if (mxt_ev[i].type == type && mxt_ev[i].body && !strcmp(mxt_ev[i].body, body)
            && (history < 0 || mxt_ev[i].history == history))
            return &mxt_ev[i];
    return NULL;
}

int main(int argc, char **argv)
{
    MxtArgs a;
    AmxClient *c;
    MxtMsg m;
    MxtEvent *e, *orig;
    char *orig_id, *mine = NULL, *gone_id = NULL;
    int i, ok;

    mxt_args(argc, argv, &a);
    c = mxt_client(&a);
    MXT_OK(mxt_sync_until(c, have_room, c, 240), "invited");
    if (!room) return mxt_done();
    MXT_OK(amx_join(c, room, NULL) == 0, "joined");
    m.room = room; m.sender = a.peer; m.body = "Original";
    MXT_OK(mxt_sync_until(c, mxt_have_message, &m, 180), "the peer's message");
    if (!(orig = mxt_find(AMX_EV_MESSAGE, room, a.peer, "Original"))) return mxt_done();
    orig_id = mxt_dup(orig->event_id);

    MXT_OK(amx_send_reply(c, room, orig_id, NULL, "Antwort", &mine) == 0 && mine, "replied");
    m.body = "Zurück";
    MXT_OK(mxt_sync_until(c, mxt_have_message, &m, 180), "the peer's answer, quoted fallback taken off");
    e = mxt_find(AMX_EV_MESSAGE, room, a.peer, "Zurück");
    MXT_OK(e && e->reply_to && mine && !strcmp(e->reply_to, mine), "it points to our reply");

    MXT_OK(amx_send_edit(c, room, mine, "Antwort korrigiert", NULL) == 0, "edited our reply");
    m.body = "fertig";
    MXT_OK(mxt_sync_until(c, mxt_have_message, &m, 240), "the rest arrived");
    e = by_type_body(AMX_EV_EDIT, "Original bearbeitet", -1);
    MXT_OK(e && e->relates_to && !strcmp(e->relates_to, orig_id), "the peer's edit (AMX_EV_EDIT)");
    e = mxt_find(AMX_EV_MESSAGE, room, a.peer, "gleich weg");
    if (e) gone_id = mxt_dup(e->event_id);
    for (ok = 0, i = 0; i < mxt_n_ev; i++) {
        if (mxt_ev[i].type == AMX_EV_REDACTION && gone_id && mxt_ev[i].relates_to
            && !strcmp(mxt_ev[i].relates_to, gone_id)) ok = 1;
        /* slow sync: message and deletion in one batch - it comes already deleted */
        if (!gone_id && mxt_ev[i].type == AMX_EV_MESSAGE && mxt_ev[i].redacted && mxt_ev[i].event_id) {
            gone_id = mxt_dup(mxt_ev[i].event_id);
            ok = 1;
        }
    }
    MXT_OK(ok, "the peer deleted a message (redaction, or it came already deleted)");
    e = mxt_find(AMX_EV_MESSAGE, room, a.peer, "**fett**");
    MXT_OK(e && e->formatted && !strcmp(e->formatted, "<strong>fett</strong>") && e->mention,
           "a formatted message: its HTML, and it mentions us");
    e = mxt_find(AMX_EV_MESSAGE, room, a.peer, "Original");
    MXT_OK(e && !e->formatted && !e->mention, "a plain one: neither");
    e = mxt_find(AMX_EV_MESSAGE, room, a.peer, "im Thread");
    MXT_OK(e && e->thread_root && !strcmp(e->thread_root, orig_id) && !e->reply_to,
           "a thread message: thread_root set, the fallback reply ignored");
    MXT_OK(amx_send_reply(c, room, NULL, orig_id, "Thread-Antwort", NULL) == 0, "wrote in the thread");
    amx_sync(c, 3000);

    mxt_n_ev = 0;
    amx_load_history(c, room, 30, 1, NULL);
    for (ok = 0, i = 0; i < mxt_n_ev; i++)
        if (mxt_ev[i].type == AMX_EV_MESSAGE && mxt_ev[i].history && mxt_ev[i].redacted && gone_id
            && mxt_ev[i].event_id && !strcmp(mxt_ev[i].event_id, gone_id)) ok = 1;
    MXT_OK(ok, "the history shows the deleted message as deleted");
    MXT_OK(by_type_body(AMX_EV_EDIT, "Antwort korrigiert", 1) != NULL, "edits come with the history too");
    e = by_type_body(AMX_EV_MESSAGE, "Original", 1);
    MXT_OK(e && e->thread_count == 2, "the thread's first message knows its replies (%d)", e ? e->thread_count : -1);

    mxt_n_ev = 0;
    {
        int at_start = 0, n = amx_load_thread(c, room, orig_id, 20, 1, &at_start);
        MXT_OK(n == 3 && at_start, "thread loaded: 2 messages and the root (%d, %d)", n, at_start);
        MXT_OK(by_type_body(AMX_EV_MESSAGE, "im Thread", 1) && by_type_body(AMX_EV_MESSAGE, "Thread-Antwort", 1)
               && by_type_body(AMX_EV_MESSAGE, "Original", 1), "with their text");
    }

    free(orig_id); free(mine); free(gone_id); free(room);
    amx_client_free(c);
    amx_cleanup();
    return mxt_done();
}
