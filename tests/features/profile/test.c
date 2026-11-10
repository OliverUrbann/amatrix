/* Feature: our own profile.
 * Display name and picture are set and read back; the peer (in a room
 * with us) sees both. */
#include "mxtest.h"

static const unsigned char PNG[] =
    "\x89\x50\x4e\x47\x0d\x0a\x1a\x0a\x00\x00\x00\x0d\x49\x48\x44\x52\x00\x00\x00\x03\x00\x00\x00\x02\x08\x02"
    "\x00\x00\x00\x12\x16\xf1\x4d\x00\x00\x00\x14\x49\x44\x41\x54\x78\x5e\x63\x3c\x21\x22\xc2\x00\x06\x4c\x10"
    "\x8a\x81\x81\x01\x00\x11\xd0\x00\xf4\x62\xea\x4f\x10\x00\x00\x00\x00\x49\x45\x4e\x44\xae\x42\x60\x82";

int main(int argc, char **argv)
{
    MxtArgs a;
    AmxClient *c;
    AmxRoomOptions o;
    const char *invite[2];
    char *room = NULL, *pic, *name = NULL, *av = NULL;
    size_t n;
    MxtMember join;
    MxtMsg m;
    FILE *f;

    mxt_args(argc, argv, &a);
    c = mxt_client(&a);
    memset(&o, 0, sizeof o);
    invite[0] = a.peer; invite[1] = NULL;
    o.invite = invite;
    o.name = "Profil";
    MXT_OK(amx_create_room(c, &o, &room) == 0, "create room");
    if (!room) return mxt_done();
    join.room = room; join.user = a.peer; join.membership = "join";
    MXT_OK(mxt_sync_until(c, mxt_have_membership, &join, 180), "peer joined");

    n = strlen(a.store);
    pic = malloc(n + 16);
    sprintf(pic, "%s%sich.png", a.store, n && (a.store[n - 1] == ':' || a.store[n - 1] == '/') ? "" : "/");
    if ((f = fopen(pic, "wb")) != NULL) { fwrite(PNG, 1, sizeof PNG - 1, f); fclose(f); }
    MXT_OK(amx_set_displayname(c, "Amiga-Fan \xc3\xa4") == 0, "display name set (%s)", amx_error(c));
    MXT_OK(amx_set_avatar(c, pic) == 0, "picture set (%s)", amx_error(c));
    MXT_OK(amx_set_avatar(c, "does/not/exist") != 0, "a missing picture: error");
    MXT_OK(amx_get_profile(c, NULL, &name, &av) == 0 && name && !strcmp(name, "Amiga-Fan \xc3\xa4") && av
           && !strncmp(av, "mxc://", 6), "profile read back (%s, %s)", name ? name : "-", av ? av : "-");
    amx_send_text(c, room, "profil gesetzt", NULL);
    m.room = room; m.sender = a.peer; m.body = "profil ok";
    MXT_OK(mxt_sync_until(c, mxt_have_message, &m, 120), "the peer sees name and picture");

    free(name); free(av); free(pic); free(room);
    amx_client_free(c);
    amx_cleanup();
    return mxt_done();
}
