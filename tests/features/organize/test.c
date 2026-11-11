/* Feature: favourites, spaces, search.
 * The peer invites us to a space with a room in it and to an encrypted room;
 * the space is recognised and the room knows its space; a room tagged as
 * favourite comes first; searching finds a message in the unencrypted room
 * (server) and one in the encrypted room (local cache). */
#include "mxtest.h"

static int n_joined;
static AmxClient *cl;
static int joined_all(void *u)
{
    int i;
    (void)u;
    for (i = 0; i < mxt_n_ev; i++)
        if (mxt_ev[i].type == AMX_EV_INVITE && mxt_ev[i].room_id && mxt_ev[i].reason == NULL) {
            char *id = NULL;
            mxt_ev[i].reason = mxt_dup("joined");          /* mark: done */
            if (amx_join(cl, mxt_ev[i].room_id, &id) == 0) n_joined++;
            free(id);
        }
    return n_joined >= 3;
}
static int have_both(void *u)
{
    (void)u;
    return mxt_find(AMX_EV_MESSAGE, NULL, NULL, "Kuchenrezept von Oma") && mxt_find(AMX_EV_MESSAGE, NULL, NULL, "Geheimer Kuchen");
}

int main(int argc, char **argv)
{
    MxtArgs a;
    AmxRoomInfo *l = NULL;
    AmxSearchHit *h = NULL;
    const char *space = NULL, *garden = NULL, *secret = NULL;
    char *sp = NULL, *ga = NULL, *se = NULL;
    int n, i, srv = 0, cache = 0, k;

    mxt_args(argc, argv, &a);
    cl = mxt_client(&a);
    MXT_OK(mxt_sync_until(cl, joined_all, NULL, 180), "joined the space and both rooms (%d)", n_joined);
    MXT_OK(mxt_sync_until(cl, have_both, NULL, 180), "the peer's messages arrived");
    for (k = 0; k < 4; k++) amx_sync(cl, 1000);           /* create events fetched for older rooms */

    n = amx_rooms(cl, &l);
    for (i = 0; i < n; i++) {
        if (l[i].is_space) { space = l[i].room_id; sp = strdup(space); }
        else if (!strcmp(l[i].name, "Garten")) { garden = l[i].room_id; ga = strdup(garden); }
        else if (!strcmp(l[i].name, "Geheim")) { secret = l[i].room_id; se = strdup(secret); }
    }
    MXT_OK(space && garden && secret, "the space and both rooms in the list");
    for (i = 0; i < n; i++)
        if (garden && !strcmp(l[i].room_id, garden))
            MXT_OK(l[i].parents && space && strstr(l[i].parents, space), "Garten knows its space");
    amx_free_rooms(l, n);
    if (!sp || !ga || !se) return mxt_done();

    MXT_OK(amx_set_room_tag(cl, se, "m.favourite", 1) == 0, "Geheim tagged as favourite");
    n = amx_rooms(cl, &l);
    for (i = 0; i < n && l[i].membership != AMX_ROOM_JOINED; i++) {}
    MXT_OK(i < n && !strcmp(l[i].room_id, se) && l[i].favourite, "the favourite comes first");
    amx_free_rooms(l, n);
    MXT_OK(amx_set_room_tag(cl, ga, "m.lowpriority", 1) == 0, "Garten: low priority");
    n = amx_rooms(cl, &l);
    for (i = 0, k = -1; i < n; i++) if (!l[i].is_space) k = i;
    MXT_OK(k >= 0 && !strcmp(l[k].room_id, ga) && l[k].low_priority, "low priority comes last");
    amx_free_rooms(l, n);
    MXT_OK(amx_set_room_tag(cl, se, "m.favourite", 0) == 0, "favourite taken away");
    n = amx_rooms(cl, &l);
    for (i = 0; i < n; i++) if (!strcmp(l[i].room_id, se)) MXT_OK(!l[i].favourite, "no longer favourite");
    amx_free_rooms(l, n);

    n = amx_search(cl, NULL, "kuchen", &h);
    for (i = 0; i < n; i++) {
        if (!strcmp(h[i].body, "Kuchenrezept von Oma") && !strcmp(h[i].room_id, ga)) srv++;
        if (!strcmp(h[i].body, "Geheimer Kuchen") && !strcmp(h[i].room_id, se)) cache++;
    }
    MXT_OK(n >= 2 && srv == 1 && cache == 1, "search 'kuchen': both found, once each (%d hits)", n);
    amx_free_search(h, n);
    n = amx_search(cl, se, "kuchen", &h);
    MXT_OK(n == 1 && !strcmp(h[0].body, "Geheimer Kuchen"), "search in one room only (%d)", n);
    amx_free_search(h, n);
    n = amx_search(cl, NULL, "gibtsnicht", &h);
    MXT_OK(n == 0, "nothing found: 0");
    amx_free_search(h, n);

    free(sp); free(ga); free(se);
    amx_client_free(cl);
    amx_cleanup();
    return mxt_done();
}
