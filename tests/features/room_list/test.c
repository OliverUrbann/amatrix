/* Feature: room list (amx_rooms).
 * Peer (display name "Peer Tester") invites us to an encrypted group room
 * "Amiga Freunde" (topic "Testgruppe") and to a direct chat, writes in both.
 * We check the list for invites, then after joining: names (the DM is named
 * after the peer), direct flag, encryption, member and unread counts, order;
 * we open our own DM with the peer; the list survives a restart; leaving
 * removes a room. */
#include "mxtest.h"

static const AmxRoomInfo *find_room(const AmxRoomInfo *l, int n, const char *id)
{
    int i;
    for (i = 0; i < n; i++) if (!strcmp(l[i].room_id, id)) return &l[i];
    return NULL;
}

static void dump(const AmxRoomInfo *l, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        char *nm = amx_to_local(l[i].name);
        mxt_diag("room %s '%s' %s%s%s members %d+%d unread %d dm %s", l[i].room_id, nm,
                 l[i].membership == AMX_ROOM_INVITED ? "invited" : "joined",
                 l[i].is_direct ? " direct" : "", l[i].encrypted ? " encrypted" : "",
                 l[i].joined_members, l[i].invited_members, l[i].unread, l[i].dm_user ? l[i].dm_user : "-");
        free(nm);
    }
}

static int two_invites(void *unused) { (void)unused; return mxt_count(AMX_EV_INVITE, NULL, NULL) >= 2; }

typedef struct { AmxClient *c; const char *room; int min; } Unread;
static int has_unread(void *p)
{
    Unread *u = p;
    AmxRoomInfo *l;
    int n = amx_rooms(u->c, &l), ok;
    const AmxRoomInfo *r = find_room(l, n, u->room);
    ok = r && r->unread >= u->min;
    amx_free_rooms(l, n);
    return ok;
}

int main(int argc, char **argv)
{
    MxtArgs a;
    AmxClient *c;
    AmxRoomInfo *l;
    const AmxRoomInfo *g, *d, *mine;
    AmxRoomOptions o;
    const char *invite[2];
    char *group = NULL, *dm = NULL, *own_dm = NULL;
    int n, i;
    MxtMsg m;
    Unread u;

    mxt_args(argc, argv, &a);
    c = mxt_client(&a);
    n = amx_rooms(c, &l);
    MXT_OK(n == 0, "empty room list for a new account");
    amx_free_rooms(l, n);

    MXT_OK(mxt_sync_until(c, two_invites, NULL, 180), "two invites arrived");
    n = amx_rooms(c, &l);
    dump(l, n);
    MXT_OK(n == 2, "two rooms in the list");
    for (i = 0; i < n; i++) {
        MXT_OK(l[i].membership == AMX_ROOM_INVITED && l[i].inviter && !strcmp(l[i].inviter, a.peer),
               "room %d: invited by the peer", i + 1);
        MXT_OK(l[i].inviter_name && !strcmp(l[i].inviter_name, "Peer Tester"),
               "room %d: inviter's display name (%s)", i + 1, l[i].inviter_name ? l[i].inviter_name : "-");
        if (!strcmp(l[i].name, "Amiga Freunde")) group = mxt_dup(l[i].room_id);
        else if (l[i].is_direct) dm = mxt_dup(l[i].room_id);
    }
    MXT_OK(group != NULL, "group invite named 'Amiga Freunde'");
    MXT_OK(dm != NULL, "direct-chat invite recognised as direct");
    amx_free_rooms(l, n);
    if (!group || !dm) return mxt_done();

    MXT_OK(amx_join(c, group, NULL) == 0, "join group");
    MXT_OK(amx_join(c, dm, NULL) == 0, "accept direct chat");
    m.room = dm; m.sender = a.peer; m.body = "hallo direkt";
    MXT_OK(mxt_sync_until(c, mxt_have_message, &m, 180), "message in the direct chat");
    u.c = c; u.room = group; u.min = 2;
    MXT_OK(mxt_sync_until(c, has_unread, &u, 120), "unread count of the group >= 2");

    n = amx_rooms(c, &l);
    dump(l, n);
    g = find_room(l, n, group);
    d = find_room(l, n, dm);
    MXT_OK(g && g->membership == AMX_ROOM_JOINED && !strcmp(g->name, "Amiga Freunde"), "group joined, name kept");
    MXT_OK(g && g->topic && !strcmp(g->topic, "Testgruppe"), "group topic");
    MXT_OK(g && g->encrypted, "group encrypted");
    MXT_OK(g && g->joined_members == 2, "group has 2 members (%d)", g ? g->joined_members : -1);
    MXT_OK(g && !g->is_direct, "group is not a direct chat");
    MXT_OK(d && d->is_direct && d->dm_user && !strcmp(d->dm_user, a.peer), "direct chat with the peer");
    MXT_OK(d && !strcmp(d->name, "Peer Tester"), "direct chat named after the peer (%s)", d ? d->name : "-");
    MXT_OK(n == 2 && l[0].last_activity >= l[1].last_activity && l[1].last_activity > 0, "sorted by activity");
    MXT_OK(g && g->last_body && !strcmp(g->last_body, "hallo gruppe 2") && g->last_sender &&
           !strcmp(g->last_sender, "Peer Tester") && !g->last_own,
           "group: newest message for the preview (%s: %s)", g && g->last_sender ? g->last_sender : "-",
           g && g->last_body ? g->last_body : "-");
    MXT_OK(d && d->last_body && !strcmp(d->last_body, "hallo direkt"), "direct chat: newest message");
    amx_free_rooms(l, n);

    /* our own direct chat */
    memset(&o, 0, sizeof o);
    invite[0] = a.peer; invite[1] = NULL;
    o.invite = invite; o.is_direct = 1; o.encrypted = 1;
    MXT_OK(amx_create_room(c, &o, &own_dm) == 0, "create own direct chat");
    amx_sync(c, 1000);
    n = amx_rooms(c, &l);
    mine = own_dm ? find_room(l, n, own_dm) : NULL;
    MXT_OK(mine && mine->is_direct && mine->dm_user && !strcmp(mine->dm_user, a.peer), "own direct chat marked direct");
    MXT_OK(n == 3, "three rooms now");
    amx_free_rooms(l, n);

    /* restart: the list comes from the store, without a sync */
    amx_client_free(c);
    c = mxt_client(&a);
    n = amx_rooms(c, &l);
    g = find_room(l, n, group);
    d = find_room(l, n, dm);
    MXT_OK(n == 3 && g && !strcmp(g->name, "Amiga Freunde") && d && d->is_direct,
           "room list survives a restart");
    amx_free_rooms(l, n);

    MXT_OK(amx_leave(c, group) == 0, "leave group");
    n = amx_rooms(c, &l);
    MXT_OK(n == 2 && !find_room(l, n, group), "left room removed from the list");
    amx_free_rooms(l, n);
    amx_sync(c, 1000);
    n = amx_rooms(c, &l);
    MXT_OK(n == 2 && !find_room(l, n, group), "... and stays removed after a sync");
    amx_free_rooms(l, n);

    free(group); free(dm); free(own_dm);
    amx_client_free(c);
    amx_cleanup();
    return mxt_done();
}
