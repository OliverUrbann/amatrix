/* Feature: reactions (m.reaction, m.annotation) and taking them back
 * (m.room.redaction), in an encrypted room.
 *
 * The peer invites us and writes "Reagier mal".  We react with a thumbs up;
 * the peer checks it (decrypted, right message, right key) and reacts with a
 * heart to our message "Und du?".  We take our thumbs up back; the peer sees
 * the redaction and writes "weg ist er".  Afterwards the history must deliver
 * the heart again (history=1). */
#include "mxtest.h"

#define THUMBS "\xf0\x9f\x91\x8d"                /* U+1F44D */
#define HEART  "\xe2\x9d\xa4\xef\xb8\x8f"        /* U+2764 U+FE0F */

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

static const char *target;

static int have_heart(void *arg)
{
    int i, hist = arg != NULL;
    for (i = 0; i < mxt_n_ev; i++)
        if (mxt_ev[i].type == AMX_EV_REACTION && mxt_ev[i].relates_to && !strcmp(mxt_ev[i].relates_to, target)
            && mxt_ev[i].body && !strcmp(mxt_ev[i].body, HEART) && !mxt_ev[i].own && mxt_ev[i].history == hist)
            return 1;
    return 0;
}

int main(int argc, char **argv)
{
    MxtArgs a;
    AmxClient *c;
    MxtMsg m;
    MxtEvent *e;
    char *mine = NULL, *react = NULL, *msg_id;
    int i, ok;

    mxt_args(argc, argv, &a);
    c = mxt_client(&a);
    MXT_OK(mxt_sync_until(c, have_room, c, 240), "invited");
    if (!room) return mxt_done();
    MXT_OK(amx_join(c, room, NULL) == 0, "joined");
    m.room = room; m.sender = a.peer; m.body = "Reagier mal";
    MXT_OK(mxt_sync_until(c, mxt_have_message, &m, 180), "message of the peer");
    e = mxt_find(AMX_EV_MESSAGE, room, a.peer, "Reagier mal");
    if (!e) return mxt_done();
    msg_id = mxt_dup(e->event_id);

    MXT_OK(amx_send_reaction(c, room, msg_id, THUMBS, &react) == 0 && react, "reacted with a thumbs up (%s)",
           react ? react : amx_error(c));
    MXT_OK(amx_send_text(c, room, "Und du?", &mine) == 0 && mine, "sent \"Und du?\"");
    if (!mine) return mxt_done();
    target = mine;
    MXT_OK(mxt_sync_until(c, have_heart, NULL, 180), "the peer's heart on our message (live)");
    for (ok = 0, i = 0; i < mxt_n_ev; i++)
        if (mxt_ev[i].type == AMX_EV_REACTION && mxt_ev[i].own && mxt_ev[i].relates_to
            && !strcmp(mxt_ev[i].relates_to, msg_id) && mxt_ev[i].encrypted) ok = 1;
    MXT_OK(ok, "our own reaction comes back from /sync (decrypted)");

    MXT_OK(amx_redact(c, room, react, NULL) == 0, "took the thumbs up back");
    m.body = "weg ist er";
    MXT_OK(mxt_sync_until(c, mxt_have_message, &m, 180), "the peer saw it go");
    for (ok = 0, i = 0; i < mxt_n_ev; i++)
        if (mxt_ev[i].type == AMX_EV_REDACTION && mxt_ev[i].relates_to && !strcmp(mxt_ev[i].relates_to, react))
            ok = 1;
    MXT_OK(ok, "the redaction comes back from /sync");

    amx_load_history(c, room, 30, 1, NULL);
    MXT_OK(have_heart((void *)1), "the heart in the history too");
    for (ok = 1, i = 0; i < mxt_n_ev; i++)
        if (mxt_ev[i].type == AMX_EV_UNDECRYPTABLE && mxt_ev[i].history) ok = 0;
    MXT_OK(ok, "the history has no undecryptable events (the removed reaction is skipped)");

    free(mine); free(react); free(msg_id); free(room);
    amx_client_free(c);
    amx_cleanup();
    return mxt_done();
}
