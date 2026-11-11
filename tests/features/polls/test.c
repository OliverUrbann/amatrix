/* Feature: polls (as Element sends them, MSC3381) and locations in an encrypted room.
 * The peer asks; we see question and answers, vote, see the peer's vote and
 * the end; the votes can be loaded again (relations); our own poll reaches
 * the peer with its answers. */
#include "mxtest.h"

static const char *ROOM, *PEER;
static int have_poll(void *u)
{
    (void)u;
    return mxt_find(AMX_EV_MESSAGE, ROOM, PEER, "Pizza oder Pasta?") != NULL;
}
static int count(int type, const char *rel, const char *sender, int from)
{
    int i, n = 0;
    for (i = from; i < mxt_n_ev; i++)
        if (mxt_ev[i].type == type && mxt_ev[i].relates_to && !strcmp(mxt_ev[i].relates_to, rel)
            && (!sender || (mxt_ev[i].sender && !strcmp(mxt_ev[i].sender, sender))))
            n++;
    return n;
}
static const char *POLL;
static int peer_voted(void *u) { (void)u; return count(AMX_EV_POLL_VOTE, POLL, PEER, 0) > 0; }
static int ended(void *u) { (void)u; return count(AMX_EV_POLL_END, POLL, PEER, 0) > 0; }

int main(int argc, char **argv)
{
    MxtArgs a;
    AmxClient *c;
    AmxRoomOptions o;
    const char *invite[2], *ans[3] = { "Amiga 500", "Amiga 1200", "Vampire" }, *mine[1] = { "a2" };
    char *room = NULL, *own = NULL;
    MxtMember join;
    MxtMsg m;
    MxtEvent *p, *v;
    int mark, i;

    mxt_args(argc, argv, &a);
    c = mxt_client(&a);
    memset(&o, 0, sizeof o);
    invite[0] = a.peer; invite[1] = NULL;
    o.invite = invite;
    o.name = "Umfragen";
    o.encrypted = 1;
    MXT_OK(amx_create_room(c, &o, &room) == 0, "create encrypted room");
    if (!room) return mxt_done();
    ROOM = room; PEER = a.peer;
    join.room = room; join.user = a.peer; join.membership = "join";
    MXT_OK(mxt_sync_until(c, mxt_have_membership, &join, 180), "peer joined");

    MXT_OK(mxt_sync_until(c, have_poll, NULL, 120), "the peer's poll arrived");
    MXT_OK(mxt_find(AMX_EV_MESSAGE, room, a.peer, "Brandenburger Tor\nhttps://www.openstreetmap.org/?mlat=52.51627&mlon=13.37769#map=16/52.51627/13.37769") != NULL,
           "a location: its name and a map link");
    p = mxt_find(AMX_EV_MESSAGE, room, a.peer, "Pizza oder Pasta?");
    MXT_OK(p && p->poll && strstr(p->poll, "\"Pizza\"")
           && strstr(p->poll, "\"Pasta\"") && strstr(p->poll, "\"max\":1"), "m.poll with its answers (%s)",
           p && p->poll ? p->poll : "-");
    if (!p) return mxt_done();
    POLL = p->event_id;
    MXT_OK(amx_poll_vote(c, room, POLL, mine, 1) == 0, "we voted");
    MXT_OK(mxt_sync_until(c, peer_voted, NULL, 120), "the peer's vote");
    for (i = mxt_n_ev - 1, v = NULL; i >= 0; i--)
        if (mxt_ev[i].type == AMX_EV_POLL_VOTE && !strcmp(mxt_ev[i].sender, a.peer)) { v = &mxt_ev[i]; break; }
    MXT_OK(v && v->body && !strcmp(v->body, "p1"), "it chose 'Pizza' (%s)", v && v->body ? v->body : "-");
    MXT_OK(mxt_sync_until(c, ended, NULL, 120), "the peer ended the poll");

    mark = mxt_n_ev;
    MXT_OK(amx_load_relations(c, room, POLL) >= 3, "votes and end loaded again");
    MXT_OK(count(AMX_EV_POLL_VOTE, POLL, NULL, mark) >= 2 && count(AMX_EV_POLL_END, POLL, NULL, mark) == 1,
           "both votes and the end among them");

    MXT_OK(amx_send_poll(c, room, "Welcher Amiga?", ans, 3, 1, 1, &own) == 0 && own, "our own poll");
    m.room = room; m.sender = a.peer; m.body = "umfrage ok";
    MXT_OK(mxt_sync_until(c, mxt_have_message, &m, 120), "the peer sees it with three answers");

    free(own);
    free(room);
    amx_client_free(c);
    amx_cleanup();
    return mxt_done();
}
