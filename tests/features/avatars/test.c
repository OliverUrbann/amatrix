/* Feature: avatars (AmxRoomInfo.avatar_url, amx_avatar).
 * The peer sets a profile picture (blue JPEG) and invites us to "Bilderraum"
 * with a room picture (PNG 120x60: red centre square, green sides) and to a
 * direct chat without a picture.  The room shows its picture, the direct
 * chat the peer's; amx_avatar() delivers them cropped and scaled; the second
 * time from the store. */
#include "mxtest.h"

static int two_invites(void *u) { (void)u; return mxt_count(AMX_EV_INVITE, NULL, NULL) >= 2; }

static int near(unsigned long px, int r, int g, int b)
{
    int R = (px >> 16) & 255, G = (px >> 8) & 255, B = px & 255, A = (px >> 24) & 255;
    return A == 255 && abs(R - r) < 40 && abs(G - g) < 40 && abs(B - b) < 40;
}

static AmxRoomInfo *find(AmxRoomInfo *l, int n, int direct)
{
    int i;
    for (i = 0; i < n; i++) if (l[i].is_direct == direct) return &l[i];
    return NULL;
}

int main(int argc, char **argv)
{
    MxtArgs a;
    AmxClient *c;
    AmxRoomInfo *l, *room, *dm;
    unsigned long *px = NULL, *px2 = NULL;
    char *room_av = NULL, *dm_av = NULL;
    int n, i, ok, corners;

    mxt_args(argc, argv, &a);
    c = mxt_client(&a);
    MXT_OK(mxt_sync_until(c, two_invites, NULL, 180), "two invitations");
    n = amx_rooms(c, &l);
    room = find(l, n, 0);
    dm = find(l, n, 1);
    MXT_OK(room && room->avatar_url && !strncmp(room->avatar_url, "mxc://", 6),
           "invitation shows the room picture (%s)", room && room->avatar_url ? room->avatar_url : "-");
    MXT_OK(dm && dm->avatar_url && !strncmp(dm->avatar_url, "mxc://", 6),
           "direct chat invitation shows the inviter's picture (%s)", dm && dm->avatar_url ? dm->avatar_url : "-");
    for (i = 0; i < n; i++) amx_join(c, l[i].room_id, NULL);
    amx_free_rooms(l, n);
    amx_sync(c, 2000);
    amx_sync(c, 2000);

    n = amx_rooms(c, &l);
    room = find(l, n, 0);
    dm = find(l, n, 1);
    MXT_OK(room && dm && room->avatar_url && dm->avatar_url && strcmp(room->avatar_url, dm->avatar_url),
           "after joining: both pictures known and different");
    if (room && room->avatar_url) room_av = mxt_dup(room->avatar_url);
    if (dm && dm->avatar_url) dm_av = mxt_dup(dm->avatar_url);
    amx_free_rooms(l, n);
    if (!room_av || !dm_av) return mxt_done();

    ok = amx_avatar(c, room_av, 24, &px) == 0;
    MXT_OK(ok && px, "room picture loaded (%s)", ok ? "ok" : amx_error(c));
    if (px) {
        corners = near(px[0], 220, 20, 20) && near(px[23], 220, 20, 20) &&
                  near(px[23 * 24], 220, 20, 20) && near(px[24 * 24 - 1], 220, 20, 20);
        MXT_OK(near(px[12 * 24 + 12], 220, 20, 20), "centre is red (%08lx)", px[12 * 24 + 12]);
        MXT_OK(corners, "cropped to the centre square: corners red too (%08lx)", px[0]);
    }
    ok = amx_avatar(c, room_av, 24, &px2) == 0;
    MXT_OK(ok && px && px2 && !memcmp(px, px2, 24 * 24 * sizeof *px), "second time: same pixels (store)");
    free(px); free(px2); px = NULL;

    ok = amx_avatar(c, dm_av, 32, &px) == 0;
    MXT_OK(ok && px && near(px[16 * 32 + 16], 20, 40, 230), "direct chat: peer's blue JPEG, 32x32 (%08lx)",
           px ? px[16 * 32 + 16] : 0UL);
    free(px);

    MXT_OK(amx_avatar(c, "https://example.org/x.png", 24, &px) == -1 && !px, "not an mxc URL: error");
    MXT_OK(amx_avatar(c, "mxc://localhost/gibtsnicht", 24, &px) == -1, "unknown media: error (%s)", amx_error(c));

    free(room_av); free(dm_av);
    amx_client_free(c);
    amx_cleanup();
    return mxt_done();
}
