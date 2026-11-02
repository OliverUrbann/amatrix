/* Feature: create room, invite, membership events, plain messages, join by
 * alias, leave, joined-room list.
 * Peer: joins our room, answers "plain hello" with "plain reply"; has created
 * a public room #pub-<our localpart>:<server> for the alias join. */
#include "mxtest.h"

static int joined_alias(AmxClient *c, const char *alias, char **room)
{
    time_t end = time(NULL) + 120;
    while (time(NULL) < end) {
        if (amx_join(c, alias, room) == 0) return 1;
        amx_sync(c, 2000);          /* peer may not have created it yet */
    }
    return 0;
}

int main(int argc, char **argv)
{
    MxtArgs a;
    AmxClient *c;
    AmxRoomOptions o;
    const char *invite[2];
    char *room = NULL, *pub = NULL, *ev = NULL, alias[200], **rooms;
    const char *server_name;
    MxtMember peer_join;
    MxtMsg reply;
    int i, found;

    mxt_args(argc, argv, &a);
    c = mxt_client(&a);

    memset(&o, 0, sizeof o);
    invite[0] = a.peer; invite[1] = NULL;
    o.name = "Amatrix rooms test";
    o.topic = "created by the rooms feature test";
    o.invite = invite;
    MXT_OK(amx_create_room(c, &o, &room) == 0 && room, "create private room (%s)", room ? room : amx_error(c));

    peer_join.room = room; peer_join.user = a.peer; peer_join.membership = "join";
    MXT_OK(mxt_sync_until(c, mxt_have_membership, &peer_join, 120), "peer joined (membership event)");

    MXT_OK(amx_send_text(c, room, "plain hello", &ev) == 0 && ev, "send unencrypted message (%s)", ev ? ev : amx_error(c));
    reply.room = room; reply.sender = a.peer; reply.body = "plain reply";
    MXT_OK(mxt_sync_until(c, mxt_have_message, &reply, 120), "received unencrypted reply");
    MXT_OK(!mxt_find(AMX_EV_MESSAGE, room, a.peer, "plain reply")->encrypted, "reply marked unencrypted");

    /* join by alias */
    server_name = strchr(amx_user_id(c), ':') + 1;
    snprintf(alias, sizeof alias, "#pub-%s:%s", a.user, server_name);
    MXT_OK(joined_alias(c, alias, &pub), "join by alias %s (%s)", alias, pub ? pub : amx_error(c));

    rooms = amx_joined_rooms(c);
    for (found = 0, i = 0; rooms && rooms[i]; i++)
        if (!strcmp(rooms[i], room) || (pub && !strcmp(rooms[i], pub))) found++;
    MXT_OK(found == 2, "both rooms in joined list");
    amx_free_strings(rooms);

    MXT_OK(pub && amx_leave(c, pub) == 0, "leave the alias room");
    rooms = amx_joined_rooms(c);
    for (found = 0, i = 0; rooms && rooms[i]; i++)
        if (pub && !strcmp(rooms[i], pub)) found = 1;
    MXT_OK(!found, "left room no longer listed");
    amx_free_strings(rooms);
    MXT_OK(amx_leave(c, "!doesnotexist:nowhere") != 0, "leaving an unknown room fails (%s)", amx_error(c));

    free(room); free(pub); free(ev);
    amx_client_free(c);
    amx_cleanup();
    return mxt_done();
}
