/* Feature: receive end-to-end encrypted messages.
 * Peer: encrypted room, one message BEFORE inviting us (must stay unreadable),
 * then three messages (umlauts, emoji).  After those we restart the client
 * from its store and send "restart ok"; the peer answers "nach Neustart",
 * which must decrypt with the stored sessions. */
#include "mxtest.h"

static const char *MSGS[] = { "Test", "Gr\xc3\xbc\xc3\x9f" "e aus K\xc3\xb6ln \xe2\x80\x93 \xc3\xa4\xc3\xb6\xc3\xbc \xc3\x9f",
                              "Jo \xf0\x9f\x90\x84", NULL };

typedef struct { const char *room, *peer; } Ctx;
static int all_three(void *p)
{
    Ctx *x = p;
    int i;
    for (i = 0; MSGS[i]; i++)
        if (!mxt_find(AMX_EV_MESSAGE, x->room, x->peer, MSGS[i])) return 0;
    return 1;
}

int main(int argc, char **argv)
{
    MxtArgs a;
    AmxClient *c;
    MxtEvent *inv, *m;
    char *room = NULL, *device;
    Ctx x;
    MxtMsg after;
    int i;

    mxt_args(argc, argv, &a);
    c = mxt_client(&a);
    MXT_OK(mxt_sync_until(c, mxt_have_invite, NULL, 180), "invited by peer");
    inv = mxt_find(AMX_EV_INVITE, NULL, NULL, NULL);
    MXT_OK(inv && inv->sender && !strcmp(inv->sender, a.peer), "invite comes from the peer");
    MXT_OK(inv && amx_join(c, inv->room_id, &room) == 0, "join the encrypted room");
    if (!room) return mxt_done();

    x.room = room; x.peer = a.peer;
    MXT_OK(mxt_sync_until(c, all_three, &x, 180), "three messages decrypted");
    for (i = 0; MSGS[i]; i++) {
        m = mxt_find(AMX_EV_MESSAGE, room, a.peer, MSGS[i]);
        MXT_OK(m && m->encrypted && !m->own, "message %d exact text, encrypted, not own", i + 1);
    }
    MXT_OK(mxt_find(AMX_EV_ROOM_KEY, room, a.peer, NULL) != NULL, "room key event from peer");
    MXT_OK(!mxt_find(AMX_EV_MESSAGE, room, a.peer, "vor der Einladung"),
           "message from before the invite is not readable");
    MXT_OK(mxt_count(AMX_EV_UNDECRYPTABLE, room, a.peer) >= 1, "... and reported as undecryptable");

    /* restart from the store */
    device = mxt_dup(amx_device_id(c));
    amx_client_free(c);
    c = mxt_client(&a);
    MXT_OK(!strcmp(device, amx_device_id(c)), "restart: same device %s", device);
    MXT_OK(amx_send_text(c, room, "restart ok", NULL) == 0, "send encrypted 'restart ok' (%s)", amx_error(c));
    after.room = room; after.sender = a.peer; after.body = "nach Neustart";
    MXT_OK(mxt_sync_until(c, mxt_have_message, &after, 180), "message after restart decrypted");

    free(device); free(room);
    amx_client_free(c);
    amx_cleanup();
    return mxt_done();
}
