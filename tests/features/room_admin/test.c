/* Feature: member list, room settings, moderation.
 * We create a room (we are its admin) with the peer: the member list with
 * power levels; name, topic, join rule, history visibility, encryption and a
 * picture set and read back (the peer checks them too); the peer made a
 * moderator; then kicked, banned, unbanned and invited again. */
#include "mxtest.h"

static const unsigned char PNG[] =
    "\x89\x50\x4e\x47\x0d\x0a\x1a\x0a\x00\x00\x00\x0d\x49\x48\x44\x52\x00\x00\x00\x03\x00\x00\x00\x02\x08\x02"
    "\x00\x00\x00\x12\x16\xf1\x4d\x00\x00\x00\x14\x49\x44\x41\x54\x78\x5e\x63\x3c\x21\x22\xc2\x00\x06\x4c\x10"
    "\x8a\x81\x81\x01\x00\x11\xd0\x00\xf4\x62\xea\x4f\x10\x00\x00\x00\x00\x49\x45\x4e\x44\xae\x42\x60\x82";

static const char *ROOM, *PEER;
static int member_is(const char *ms)
{
    int i;
    for (i = mxt_n_ev - 1; i >= 0; i--)
        if (mxt_ev[i].type == AMX_EV_MEMBERSHIP && !strcmp(mxt_ev[i].room_id, ROOM) && mxt_ev[i].state_key
            && !strcmp(mxt_ev[i].state_key, PEER))
            return mxt_ev[i].body && !strcmp(mxt_ev[i].body, ms);
    return 0;
}
static int is_leave(void *u) { (void)u; return member_is("leave"); }
static int is_ban(void *u) { (void)u; return member_is("ban"); }
static int is_join(void *u) { (void)u; return member_is("join"); }

static const AmxMember *find(const AmxMember *l, int n, const char *u)
{
    int i;
    for (i = 0; i < n; i++) if (!strcmp(l[i].user_id, u)) return &l[i];
    return NULL;
}

int main(int argc, char **argv)
{
    MxtArgs a;
    AmxClient *c;
    AmxRoomOptions o;
    AmxRoomSettings s;
    AmxMember *ml = NULL;
    const AmxMember *me, *pe;
    const char *invite[2];
    char *room = NULL, *pic;
    MxtMember join;
    MxtMsg m;
    int n;
    FILE *f;

    mxt_args(argc, argv, &a);
    c = mxt_client(&a);
    memset(&o, 0, sizeof o);
    invite[0] = a.peer; invite[1] = NULL;
    o.invite = invite;
    o.name = "Verwaltung";
    MXT_OK(amx_create_room(c, &o, &room) == 0, "create room");
    if (!room) return mxt_done();
    ROOM = room; PEER = a.peer;
    join.room = room; join.user = a.peer; join.membership = "join";
    MXT_OK(mxt_sync_until(c, mxt_have_membership, &join, 180), "peer joined");

    n = amx_room_members(c, room, &ml);
    { int i; for (i = 0; i < n; i++) mxt_diag("member %s %s %s %d", ml[i].user_id, ml[i].name, ml[i].membership, ml[i].power); }
    me = find(ml, n, a.user[0] == '@' ? a.user : amx_user_id(c));
    pe = find(ml, n, a.peer);
    MXT_OK(n == 2 && me && pe && me->power >= 100 && pe->power == 0 && !strcmp(pe->membership, "join")
           && ml[0].power >= 100, "member list: both, we are admin first, the peer 0 (%d)", n);
    amx_free_members(ml, n);

    MXT_OK(amx_room_settings(c, room, &s) == 0 && s.my_power >= 100 && !s.encrypted && s.name
           && !strcmp(s.name, "Verwaltung") && s.level_kick == 50, "settings read (power %d)", s.my_power);
    amx_free_room_settings(&s);

    pic = malloc(strlen(a.store) + 16);
    sprintf(pic, "%s%sbild.png", a.store, a.store[strlen(a.store) - 1] == ':' || a.store[strlen(a.store) - 1] == '/' ? "" : "/");
    f = fopen(pic, "wb");
    if (f) { fwrite(PNG, 1, sizeof PNG - 1, f); fclose(f); }
    MXT_OK(amx_set_room_setting(c, room, "name", "Neuer Name \xc3\xa4") == 0, "name set (%s)", amx_error(c));
    MXT_OK(amx_set_room_setting(c, room, "topic", "Worum es geht") == 0, "topic set");
    MXT_OK(amx_set_room_setting(c, room, "join_rule", "public") == 0, "join rule public");
    MXT_OK(amx_set_room_setting(c, room, "history", "joined") == 0, "history: members since joining");
    MXT_OK(amx_set_room_setting(c, room, "avatar", pic) == 0, "picture set (%s)", amx_error(c));
    MXT_OK(amx_set_room_setting(c, room, "encryption", NULL) == 0, "encryption switched on");
    MXT_OK(amx_set_room_setting(c, room, "join_rule", "bogus") != 0, "unknown join rule refused");
    MXT_OK(amx_set_power(c, room, a.peer, 50) == 0, "peer made moderator");

    MXT_OK(amx_room_settings(c, room, &s) == 0 && s.name && !strcmp(s.name, "Neuer Name \xc3\xa4") && s.topic
           && !strcmp(s.topic, "Worum es geht") && !strcmp(s.join_rule, "public") && !strcmp(s.history, "joined")
           && s.encrypted && s.avatar_url && !strncmp(s.avatar_url, "mxc://", 6), "settings read back");
    amx_free_room_settings(&s);
    n = amx_room_members(c, room, &ml);
    pe = find(ml, n, a.peer);
    MXT_OK(pe && pe->power == 50, "peer has power 50 now");
    amx_free_members(ml, n);
    amx_send_text(c, room, "fertig eingestellt", NULL);
    m.room = room; m.sender = a.peer; m.body = "einstellungen ok";
    MXT_OK(mxt_sync_until(c, mxt_have_message, &m, 120), "the peer sees the new settings");

    MXT_OK(amx_kick(c, room, a.peer, "Test") == 0, "kicked the peer");
    MXT_OK(mxt_sync_until(c, is_leave, NULL, 60), "it is out");
    MXT_OK(amx_ban(c, room, a.peer, "Test") == 0, "banned it");
    MXT_OK(mxt_sync_until(c, is_ban, NULL, 60), "banned");
    n = amx_room_members(c, room, &ml);
    pe = find(ml, n, a.peer);
    MXT_OK(pe && !strcmp(pe->membership, "ban"), "member list shows the ban");
    amx_free_members(ml, n);
    MXT_OK(amx_unban(c, room, a.peer) == 0, "unbanned");
    MXT_OK(mxt_sync_until(c, is_leave, NULL, 60), "no longer banned");
    MXT_OK(amx_invite(c, room, a.peer) == 0, "invited again");
    MXT_OK(mxt_sync_until(c, is_join, NULL, 120), "the peer is back");

    free(pic);
    free(room);
    amx_client_free(c);
    amx_cleanup();
    return mxt_done();
}
