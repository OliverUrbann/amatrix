/* Feature: "is typing" and read receipts, both ways.
 * We type -> the peer sees it; the peer types -> we see it, and when it
 * stops; our message read by the peer -> a receipt for it; we mark the
 * peer's message read -> the peer sees our receipt; private receipts
 * (m.read.private) -> the peer sees none. */
#include "mxtest.h"

static const char *ROOM, *PEER;
static const char *want_eid;

static int peer_typing(void *u)
{
    int i;
    (void)u;
    for (i = mxt_n_ev - 1; i >= 0; i--)
        if (mxt_ev[i].type == AMX_EV_TYPING && !strcmp(mxt_ev[i].room_id, ROOM))
            return mxt_ev[i].state_key && strstr(mxt_ev[i].state_key, PEER) && mxt_ev[i].body && *mxt_ev[i].body;
    return 0;
}
static int nobody_typing(void *u)
{
    int i;
    (void)u;
    for (i = mxt_n_ev - 1; i >= 0; i--)
        if (mxt_ev[i].type == AMX_EV_TYPING && !strcmp(mxt_ev[i].room_id, ROOM))
            return mxt_ev[i].state_key && !*mxt_ev[i].state_key;
    return 0;
}
static int have_receipt(void *u)
{
    int i;
    (void)u;
    for (i = 0; i < mxt_n_ev; i++)
        if (mxt_ev[i].type == AMX_EV_RECEIPT && !strcmp(mxt_ev[i].room_id, ROOM) && mxt_ev[i].sender
            && !strcmp(mxt_ev[i].sender, PEER) && mxt_ev[i].event_id && !strcmp(mxt_ev[i].event_id, want_eid))
            return 1;
    return 0;
}

int main(int argc, char **argv)
{
    MxtArgs a;
    AmxClient *c;
    AmxRoomOptions o;
    const char *invite[2];
    char *room = NULL, *ev = NULL;
    MxtMember join;
    MxtMsg m;
    MxtEvent *pm;

    mxt_args(argc, argv, &a);
    c = mxt_client(&a);
    memset(&o, 0, sizeof o);
    invite[0] = a.peer; invite[1] = NULL;
    o.invite = invite;
    o.name = "Tippen";
    MXT_OK(amx_create_room(c, &o, &room) == 0, "create room");
    if (!room) return mxt_done();
    ROOM = room; PEER = a.peer;
    join.room = room; join.user = a.peer; join.membership = "join";
    MXT_OK(mxt_sync_until(c, mxt_have_membership, &join, 180), "peer joined");

    MXT_OK(amx_send_typing(c, room, 30000) == 0, "we type (%s)", amx_error(c));
    m.room = room; m.sender = a.peer; m.body = "tippen gesehen";
    MXT_OK(mxt_sync_until(c, mxt_have_message, &m, 120), "the peer saw us typing");
    MXT_OK(amx_send_typing(c, room, 0) == 0, "we stop typing");

    MXT_OK(mxt_sync_until(c, peer_typing, NULL, 120), "we see the peer typing (with its name)");
    amx_send_text(c, room, "du tippst", NULL);
    MXT_OK(mxt_sync_until(c, nobody_typing, NULL, 120), "we see it stop");

    MXT_OK(amx_send_text(c, room, "lies mich", &ev) == 0 && ev, "message to be read");
    want_eid = ev;
    MXT_OK(mxt_sync_until(c, have_receipt, NULL, 120), "read receipt of the peer for it");
    {   /* remembered: delivered again (a window opened later) */
        int mark = mxt_n_ev, k, again = 0;
        MXT_OK(amx_replay_receipts(c, room) >= 1, "remembered receipts delivered again");
        for (k = mark; k < mxt_n_ev; k++)
            if (mxt_ev[k].type == AMX_EV_RECEIPT && mxt_ev[k].sender && !strcmp(mxt_ev[k].sender, PEER)
                && mxt_ev[k].event_id && !strcmp(mxt_ev[k].event_id, want_eid)) again = 1;
        MXT_OK(again, "with the peer's newest receipt");
    }

    m.body = "gelesen?";
    MXT_OK(mxt_sync_until(c, mxt_have_message, &m, 120), "peer's message");
    pm = mxt_find(AMX_EV_MESSAGE, room, a.peer, "gelesen?");
    MXT_OK(pm && amx_mark_read(c, room, pm->event_id) == 0, "marked it read (public receipt)");
    m.body = "receipt ok";
    MXT_OK(mxt_sync_until(c, mxt_have_message, &m, 120), "the peer saw our receipt");

    amx_set_private_receipts(c, 1);
    m.body = "privat";
    MXT_OK(mxt_sync_until(c, mxt_have_message, &m, 120), "peer's next message");
    pm = mxt_find(AMX_EV_MESSAGE, room, a.peer, "privat");
    MXT_OK(pm && amx_mark_read(c, room, pm->event_id) == 0, "marked it read privately");
    amx_send_text(c, room, "fertig privat", NULL);
    m.body = "privat ok";
    MXT_OK(mxt_sync_until(c, mxt_have_message, &m, 120), "the peer saw no receipt for it");

    free(ev);
    free(room);
    amx_client_free(c);
    amx_cleanup();
    return mxt_done();
}
