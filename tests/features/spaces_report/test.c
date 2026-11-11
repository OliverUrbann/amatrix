/* Feature: rooms before joining, spaces, reporting.
 * The peer has a public room with readable history: we see what it is
 * (summary) and read its history without joining; we create a space, put
 * our own room into it and browse it; after joining we report a message. */
#include "mxtest.h"

static int have_alias(void *u)
{
    (void)u;
    return mxt_find(AMX_EV_INVITE, NULL, NULL, NULL) != NULL;
}

int main(int argc, char **argv)
{
    MxtArgs a;
    AmxClient *c;
    AmxRoomSummary s;
    AmxRoomSummary *l = NULL;
    AmxRoomOptions o;
    char *space = NULL, *room = NULL, alias[200], *joined = NULL, *pub;
    const char *srv;
    MxtEvent *inv, *hello;
    int n, i, found = 0, mark, at_start = 0;

    mxt_args(argc, argv, &a);
    c = mxt_client(&a);
    /* the peer invites us to a tiny room only to tell us its public room's ID */
    MXT_OK(mxt_sync_until(c, have_alias, NULL, 180), "the peer's note arrived");
    inv = mxt_find(AMX_EV_INVITE, NULL, NULL, NULL);
    srv = strchr(a.peer, ':');
    snprintf(alias, sizeof alias, "#offen-%s%s", inv && inv->sender ? "test" : "test", srv ? srv : "");

    memset(&s, 0, sizeof s);
    i = amx_room_summary(c, alias, &s);
    MXT_OK(i == 0 && s.name && !strcmp(s.name, "Offener Raum") && s.members >= 1
           && s.world_readable && !s.is_space && !s.joined, "summary before joining (%s, %d members; %s)",
           s.name ? s.name : "-", s.members, amx_error(c));
    pub = s.room_id ? strdup(s.room_id) : NULL;
    amx_free_room_summary(&s);
    snprintf(alias, sizeof alias, "#gibtsnicht%s", srv ? srv : "");
    MXT_OK(amx_room_summary(c, alias, &s) != 0, "an unknown room: no summary (%s)", amx_error(c));
    mark = mxt_n_ev;
    MXT_OK(pub && amx_load_history(c, pub, 20, 1, &at_start) > 0, "history read without joining");
    hello = NULL;
    for (i = mark; i < mxt_n_ev; i++)
        if (mxt_ev[i].type == AMX_EV_MESSAGE && mxt_ev[i].body && !strcmp(mxt_ev[i].body, "Hallo Welt")) hello = &mxt_ev[i];
    MXT_OK(hello != NULL, "the message is in it");

    memset(&o, 0, sizeof o);
    o.name = "Mein Space";
    o.is_space = 1;
    MXT_OK(amx_create_room(c, &o, &space) == 0, "space created");
    memset(&o, 0, sizeof o);
    o.name = "Im Space";
    MXT_OK(amx_create_room(c, &o, &room) == 0, "room created");
    MXT_OK(space && room && amx_space_add(c, space, room, 1) == 0, "room put into the space (%s)", amx_error(c));
    amx_sync(c, 0);
    n = amx_space_rooms(c, space, 2, &l);
    for (i = 0; i < n; i++) {
        if (i == 0) MXT_OK(l[0].is_space && l[0].name && !strcmp(l[0].name, "Mein Space"), "the space first");
        if (l[i].room_id && !strcmp(l[i].room_id, room) && l[i].joined) found = 1;
    }
    MXT_OK(n == 2 && found, "browsing the space: our room in it, joined (%d)", n);
    amx_free_space_rooms(l, n);
    MXT_OK(amx_space_add(c, space, room, 0) == 0, "taken out again");
    n = amx_space_rooms(c, space, 2, &l);
    MXT_OK(n == 1, "space empty again (%d)", n);
    amx_free_space_rooms(l, n);

    MXT_OK(amx_join(c, pub, &joined) == 0, "joined the public room");
    MXT_OK(hello && amx_report_event(c, pub, hello->event_id, "Test-Meldung") == 0, "message reported (%s)", amx_error(c));
    amx_send_text(c, pub, "gemeldet", NULL);

    free(pub); free(space); free(room); free(joined);
    amx_client_free(c);
    amx_cleanup();
    return mxt_done();
}
