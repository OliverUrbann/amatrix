/* Feature: notification settings on the server (push rules).
 * A room set to "off": the peer's messages count as nothing; "mentions
 * only": a plain message does not count, a keyword does (and counts as a
 * mention); "all" and back to default; keywords added and removed. */
#include "mxtest.h"

static AmxClient *cl;
static const char *ROOM;
static void counts(int *unread, int *hl, int *level)
{
    AmxRoomInfo *l = NULL;
    int n = amx_rooms(cl, &l), i;
    *unread = *hl = *level = -1;
    for (i = 0; i < n; i++)
        if (!strcmp(l[i].room_id, ROOM)) { *unread = l[i].unread; *hl = l[i].highlights; *level = l[i].notify; }
    amx_free_rooms(l, n);
}

int main(int argc, char **argv)
{
    MxtArgs a;
    AmxRoomOptions o;
    const char *invite[2];
    char *room = NULL, *kw;
    MxtMember join;
    MxtMsg m;
    MxtEvent *e;
    int u0, h0, lv, u1, h1;

    mxt_args(argc, argv, &a);
    cl = mxt_client(&a);
    memset(&o, 0, sizeof o);
    invite[0] = a.peer; invite[1] = NULL;
    o.invite = invite;
    o.name = "Glocke";
    MXT_OK(amx_create_room(cl, &o, &room) == 0, "create room");
    if (!room) return mxt_done();
    ROOM = room;
    join.room = room; join.user = a.peer; join.membership = "join";
    MXT_OK(mxt_sync_until(cl, mxt_have_membership, &join, 180), "peer joined");

    MXT_OK(amx_set_room_notify(cl, room, AMX_NOTIFY_OFF) == 0, "room set to off (%s)", amx_error(cl));
    counts(&u0, &h0, &lv);
    MXT_OK(lv == AMX_NOTIFY_OFF, "the room list says off (%d)", lv);
    amx_send_text(cl, room, "eins", NULL);
    m.room = room; m.sender = a.peer; m.body = "stumm 1";
    MXT_OK(mxt_sync_until(cl, mxt_have_message, &m, 120), "a message while off");
    amx_sync(cl, 1000);
    counts(&u1, &h1, &lv);
    MXT_OK(u1 == u0, "it does not count (%d -> %d)", u0, u1);

    MXT_OK(amx_set_room_notify(cl, room, AMX_NOTIFY_MENTIONS) == 0, "mentions only");
    MXT_OK(amx_set_keyword(cl, "Kuchen", 1) == 0, "keyword Kuchen");
    kw = amx_keywords(cl);
    MXT_OK(kw && !strcmp(kw, "Kuchen"), "keywords: %s", kw ? kw : "-");
    free(kw);
    counts(&u0, &h0, &lv);
    MXT_OK(lv == AMX_NOTIFY_MENTIONS, "the room list says mentions only (%d)", lv);
    amx_send_text(cl, room, "zwei", NULL);
    m.body = "Wer will Kuchen?";
    MXT_OK(mxt_sync_until(cl, mxt_have_message, &m, 120), "a plain message and one with the keyword");
    amx_sync(cl, 1000);
    counts(&u1, &h1, &lv);
    e = mxt_find(AMX_EV_MESSAGE, room, a.peer, "Wer will Kuchen?");
    MXT_OK(u1 == u0 + 1 && h1 == h0 + 1, "only the keyword counts, as a highlight (%d->%d, %d->%d)", u0, u1, h0, h1);
    MXT_OK(e && e->mention, "and it is marked as a mention for us");

    MXT_OK(amx_set_room_notify(cl, room, AMX_NOTIFY_ALL) == 0, "all messages");
    counts(&u0, &h0, &lv);
    MXT_OK(lv == AMX_NOTIFY_ALL, "the room list says all (%d)", lv);
    MXT_OK(amx_set_room_notify(cl, room, AMX_NOTIFY_DEFAULT) == 0, "default again");
    counts(&u0, &h0, &lv);
    MXT_OK(lv == AMX_NOTIFY_DEFAULT, "the room list says default (%d)", lv);
    MXT_OK(amx_set_keyword(cl, "Kuchen", 0) == 0, "keyword removed");
    kw = amx_keywords(cl);
    MXT_OK(kw && !*kw, "no keywords left");
    free(kw);
    amx_send_text(cl, room, "fertig", NULL);

    free(room);
    amx_client_free(cl);
    amx_cleanup();
    return mxt_done();
}
