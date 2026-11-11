/* Feature: message tools.
 * In two encrypted rooms with the peer: an emote and a formatted message
 * with a mention; our name in one room; pinning the peer's message (the room
 * list knows the pin) and fetching it on its own; forwarding a text and an
 * encrypted file to the other room; a message's edit history; ignoring the
 * peer (its messages stay away) and stopping that. */
#include "mxtest.h"

static const unsigned char DATA[] = "Datei zum Weiterleiten - Amiga rulez\n";
static const char *ROOM, *PEER;

static MxtEvent *last_peer(const char *room, const char *body)
{
    return mxt_find(AMX_EV_MESSAGE, room, PEER, body);
}
static int count_lookup_edits(const char *rel, int from)
{
    int i, n = 0;
    for (i = from; i < mxt_n_ev; i++)
        if (mxt_ev[i].type == AMX_EV_EDIT && mxt_ev[i].lookup && mxt_ev[i].relates_to && !strcmp(mxt_ev[i].relates_to, rel)) n++;
    return n;
}

int main(int argc, char **argv)
{
    MxtArgs a;
    AmxClient *c;
    AmxRoomOptions o;
    AmxOutMsg om;
    AmxRoomInfo *rl = NULL;
    const char *invite[2];
    char *ra = NULL, *rb = NULL, *fwd = NULL, *file_ev = NULL, *path, *ign;
    MxtMember join;
    MxtMsg m;
    MxtEvent *pin, *e;
    int n, i, mark, ok;
    FILE *f;

    mxt_args(argc, argv, &a);
    c = mxt_client(&a);
    PEER = a.peer;
    memset(&o, 0, sizeof o);
    invite[0] = a.peer; invite[1] = NULL;
    o.invite = invite;
    o.encrypted = 1;
    o.name = "Werkzeug A";
    MXT_OK(amx_create_room(c, &o, &ra) == 0, "room A");
    o.name = "Werkzeug B";
    MXT_OK(amx_create_room(c, &o, &rb) == 0, "room B");
    if (!ra || !rb) return mxt_done();
    ROOM = ra;
    join.room = ra; join.user = a.peer; join.membership = "join";
    MXT_OK(mxt_sync_until(c, mxt_have_membership, &join, 180), "peer in A");
    join.room = rb;
    MXT_OK(mxt_sync_until(c, mxt_have_membership, &join, 180), "peer in B");

    memset(&om, 0, sizeof om);
    om.msgtype = "m.emote";
    om.body = "winkt";
    MXT_OK(amx_send_message(c, ra, &om, NULL) == 0, "an emote");
    memset(&om, 0, sizeof om);
    om.body = "**Hallo** Peer";
    om.html = "<strong>Hallo</strong> <a href=\"https://matrix.to/#/x\">Peer</a>";
    om.mentions = a.peer;
    MXT_OK(amx_send_message(c, ra, &om, NULL) == 0, "formatted, with a mention");
    MXT_OK(amx_set_room_nick(c, ra, "Amiga im Raum") == 0, "our name in room A");
    m.room = ra; m.sender = a.peer; m.body = "gesehen";
    MXT_OK(mxt_sync_until(c, mxt_have_message, &m, 120), "the peer saw emote, formatting, mention, room name");

    m.body = "Bitte anpinnen";
    MXT_OK(mxt_sync_until(c, mxt_have_message, &m, 120), "the message to pin");
    pin = last_peer(ra, "Bitte anpinnen");
    MXT_OK(pin && amx_pin_event(c, ra, pin->event_id, 1) == 0, "pinned it (%s)", amx_error(c));
    n = amx_rooms(c, &rl);
    for (i = 0, ok = 0; pin && i < n; i++)
        if (!strcmp(rl[i].room_id, ra) && rl[i].pinned && strstr(rl[i].pinned, pin->event_id)) ok = 1;
    amx_free_rooms(rl, n);
    MXT_OK(ok, "the room list knows the pin");
    mark = mxt_n_ev;
    MXT_OK(pin && amx_fetch_event(c, ra, pin->event_id) == 0, "fetched it on its own");
    for (i = mark, ok = 0; i < mxt_n_ev; i++)
        if (mxt_ev[i].lookup && mxt_ev[i].body && !strcmp(mxt_ev[i].body, "Bitte anpinnen")) ok = 1;
    MXT_OK(ok, "delivered decrypted, as a lookup");

    MXT_OK(pin && amx_forward(c, rb, ra, pin->event_id, &fwd) == 0 && fwd, "text forwarded to B");
    n = (int)strlen(a.store);
    path = malloc(n + 16);
    sprintf(path, "%s%sweiter.txt", a.store, n && (a.store[n - 1] == ':' || a.store[n - 1] == '/') ? "" : "/");
    if ((f = fopen(path, "wb")) != NULL) { fwrite(DATA, 1, sizeof DATA - 1, f); fclose(f); }
    MXT_OK(amx_send_file(c, ra, path, NULL, NULL, &file_ev) == 0 && file_ev, "a file in A");
    free(fwd); fwd = NULL;
    MXT_OK(file_ev && amx_forward(c, rb, ra, file_ev, &fwd) == 0 && fwd, "the encrypted file forwarded to B");
    amx_send_text(c, ra, "pin gesetzt, weitergeleitet", NULL);
    m.body = "pin und weiterleitung ok";
    MXT_OK(mxt_sync_until(c, mxt_have_message, &m, 120), "the peer sees the pin and both copies in B");

    m.body = "Version 1";
    MXT_OK(mxt_sync_until(c, mxt_have_message, &m, 120), "a message the peer edits twice");
    e = last_peer(ra, "Version 1");
    m.body = "editiert";
    MXT_OK(mxt_sync_until(c, mxt_have_message, &m, 120), "edits done");
    mark = mxt_n_ev;
    MXT_OK(e && amx_load_edits(c, ra, e->event_id) >= 2 && count_lookup_edits(e->event_id, mark) == 2,
           "edit history: both versions");

    MXT_OK(amx_ignore_user(c, a.peer, 1) == 0, "ignoring the peer");
    ign = amx_ignored_users(c);
    MXT_OK(ign && strstr(ign, a.peer), "it is on the list");
    free(ign);
    amx_send_text(c, ra, "ich ignoriere dich", NULL);
    for (i = 0; i < 8; i++) amx_sync(c, 1500);
    MXT_OK(!last_peer(ra, "bist du da?"), "its message stays away");
    MXT_OK(amx_ignore_user(c, a.peer, 0) == 0, "no longer ignoring");
    ign = amx_ignored_users(c);
    MXT_OK(ign && !strstr(ign, a.peer), "off the list");
    free(ign);
    amx_send_text(c, ra, "wieder gut", NULL);
    m.body = "wieder da";
    MXT_OK(mxt_sync_until(c, mxt_have_message, &m, 120), "its messages come again");

    free(path); free(fwd); free(file_ev); free(ra); free(rb);
    amx_client_free(c);
    amx_cleanup();
    return mxt_done();
}
