/* Feature: send end-to-end encrypted messages.
 * We create an encrypted room and invite the peer, wait for its join and send
 * three messages.  The peer answers "alle 3 gelesen" once it has decrypted
 * all three (exact text). */
#include "mxtest.h"

static const char *MSGS[] = { "Hallo vom Amiga", "Umlaute: \xc3\xa4\xc3\xb6\xc3\xbc \xc3\x84\xc3\x96\xc3\x9c \xc3\x9f",
                              "Emoji: \xf0\x9f\x90\x84", NULL };

int main(int argc, char **argv)
{
    MxtArgs a;
    AmxClient *c;
    AmxRoomOptions o;
    const char *invite[2];
    char *room = NULL, *ev;
    MxtMember join;
    MxtMsg done;
    int i, own;

    mxt_args(argc, argv, &a);
    c = mxt_client(&a);
    memset(&o, 0, sizeof o);
    invite[0] = a.peer; invite[1] = NULL;
    o.name = "e2ee send test";
    o.invite = invite;
    o.encrypted = 1;
    MXT_OK(amx_create_room(c, &o, &room) == 0, "create encrypted room (%s)", room ? room : amx_error(c));
    if (!room) return mxt_done();
    join.room = room; join.user = a.peer; join.membership = "join";
    MXT_OK(mxt_sync_until(c, mxt_have_membership, &join, 180), "peer joined");

    for (i = 0; MSGS[i]; i++) {
        ev = NULL;
        MXT_OK(amx_send_text(c, room, MSGS[i], &ev) == 0 && ev, "send encrypted message %d (%s)", i + 1,
               ev ? ev : amx_error(c));
        free(ev);
    }
    done.room = room; done.sender = a.peer; done.body = "alle 3 gelesen";
    MXT_OK(mxt_sync_until(c, mxt_have_message, &done, 180), "peer decrypted all three (its encrypted answer)");

    /* our own messages come back in /sync and must decrypt via our inbound copy */
    for (own = 0, i = 0; i < mxt_n_ev; i++)
        if (mxt_ev[i].type == AMX_EV_MESSAGE && mxt_ev[i].own && mxt_ev[i].encrypted) own++;
    MXT_OK(own >= 3, "own messages readable (%d)", own);

    free(room);
    amx_client_free(c);
    amx_cleanup();
    return mxt_done();
}
