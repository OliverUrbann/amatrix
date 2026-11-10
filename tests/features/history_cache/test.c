/* Feature: the local history cache.
 * The peer writes, edits and deletes in an encrypted room; our cache keeps
 * the newest messages with the edit and the deletion applied; after a
 * restart (new client, same store) they are there without any network call;
 * switched off, the cache is gone. */
#include "mxtest.h"

static int count_cached(int from, const char *room, MxtEvent **zwei, MxtEvent **drei)
{
    int i, n = 0;
    *zwei = *drei = NULL;
    for (i = from; i < mxt_n_ev; i++) {
        MxtEvent *e = &mxt_ev[i];
        if (e->type != AMX_EV_MESSAGE || !e->cached || strcmp(e->room_id, room)) continue;
        n++;
        if (e->body && !strncmp(e->body, "zwei", 4)) *zwei = e;
        if (e->redacted) *drei = e;
    }
    return n;
}

int main(int argc, char **argv)
{
    MxtArgs a;
    AmxClient *c;
    AmxRoomOptions o;
    const char *invite[2];
    char *room = NULL;
    MxtMember join;
    MxtMsg m;
    MxtEvent *zwei, *drei;
    int mark, n, i;

    mxt_args(argc, argv, &a);
    c = mxt_client(&a);
    memset(&o, 0, sizeof o);
    invite[0] = a.peer; invite[1] = NULL;
    o.invite = invite;
    o.name = "Cache";
    o.encrypted = 1;
    MXT_OK(amx_create_room(c, &o, &room) == 0, "create encrypted room");
    if (!room) return mxt_done();
    join.room = room; join.user = a.peer; join.membership = "join";
    MXT_OK(mxt_sync_until(c, mxt_have_membership, &join, 180), "peer joined");
    m.room = room; m.sender = a.peer; m.body = "fertig";
    MXT_OK(mxt_sync_until(c, mxt_have_message, &m, 180), "the peer's messages arrived");
    for (i = 0; i < 2; i++) amx_sync(c, 1000);

    mark = mxt_n_ev;
    n = amx_cached_history(c, room);
    MXT_OK(n >= 4 && count_cached(mark, room, &zwei, &drei) == n, "cached: %d messages", n);
    MXT_OK(zwei && !strcmp(zwei->body, "zwei (neu)"), "the edit applied (%s)", zwei ? zwei->body : "-");
    MXT_OK(drei && drei->body && !*drei->body, "the deletion applied");
    MXT_OK(zwei && zwei->encrypted && zwei->history, "flags kept (encrypted, history)");
    amx_client_free(c);

    c = amx_client_new(a.server, a.store);
    amx_set_event_handler(c, mxt_record, NULL);
    MXT_OK(c && amx_login(c, a.user, a.password) == 0, "restarted with the saved login");
    mark = mxt_n_ev;
    n = amx_cached_history(c, room);
    MXT_OK(n >= 4 && count_cached(mark, room, &zwei, &drei) == n && zwei && !strcmp(zwei->body, "zwei (neu)"),
           "after the restart: the same %d messages, without the network", n);
    amx_set_history_cache(c, 0);
    MXT_OK(amx_cached_history(c, room) == 0, "switched off: nothing cached");
    amx_set_history_cache(c, 1);
    MXT_OK(amx_cached_history(c, room) == 0, "and the file is gone");

    free(room);
    amx_client_free(c);
    amx_cleanup();
    return mxt_done();
}
