/* Feature: public room directory (amx_public_rooms).
 * Peer creates three public rooms "Amiga Chat <x>", "Retro Corner <x>",
 * "Kaffeeklatsch <x>" (x = our localpart), the first with topic and alias.
 * We list, search and join from the result. */
#include "mxtest.h"

static int has(const AmxPublicRoom *l, int n, const char *name)
{
    int i;
    for (i = 0; i < n; i++) if (l[i].name && !strcmp(l[i].name, name)) return i + 1;
    return 0;
}

int main(int argc, char **argv)
{
    MxtArgs a;
    AmxClient *c;
    AmxPublicRoom *l = NULL;
    char name_amiga[160], retro[160], kaffee[160], alias[200], *joined = NULL;
    time_t end;
    int n = 0, i;

    mxt_args(argc, argv, &a);
    c = mxt_client(&a);
    snprintf(name_amiga, sizeof name_amiga, "Amiga Chat %s", a.user);
    snprintf(retro, sizeof retro, "Retro Corner %s", a.user);
    snprintf(kaffee, sizeof kaffee, "Kaffeeklatsch %s", a.user);
    snprintf(alias, sizeof alias, "#amigachat-%s:%s", a.user, strchr(amx_user_id(c), ':') + 1);

    /* wait until the peer has created its rooms */
    for (end = time(NULL) + 180; time(NULL) < end; ) {
        n = amx_public_rooms(c, NULL, NULL, 100, &l);
        if (n > 0 && has(l, n, name_amiga) && has(l, n, retro) && has(l, n, kaffee)) break;
        amx_free_public_rooms(l, n > 0 ? n : 0);
        l = NULL;
        amx_sync(c, 2000);
    }
    MXT_OK(l && has(l, n, name_amiga) && has(l, n, retro) && has(l, n, kaffee), "all three rooms listed (%d)", n);
    if (l && (i = has(l, n, name_amiga))) {
        const AmxPublicRoom *r = &l[i - 1];
        MXT_OK(r->topic && !strcmp(r->topic, "Alles \xc3\xbc" "ber den Amiga"), "topic with umlaut");
        MXT_OK(r->alias && !strcmp(r->alias, alias), "alias %s", r->alias ? r->alias : "-");
        MXT_OK(r->members == 1, "member count %d", r->members);
    }
    amx_free_public_rooms(l, n);

    n = amx_public_rooms(c, NULL, "Amiga Chat", 100, &l);
    MXT_OK(n >= 1 && has(l, n, name_amiga) && !has(l, n, kaffee), "search 'Amiga Chat' finds only that room");
    for (i = 0; i < n; i++) if (l[i].name && !strcmp(l[i].name, name_amiga)) joined = mxt_dup(l[i].room_id);
    amx_free_public_rooms(l, n);

    n = amx_public_rooms(c, NULL, "gibtsnicht-xyz", 100, &l);
    MXT_OK(n == 0, "search without match: empty");
    amx_free_public_rooms(l, n);

    MXT_OK(amx_public_rooms(c, "nowhere.invalid", NULL, 10, &l) < 0, "unknown server: error (%s)", amx_error(c));

    MXT_OK(joined && amx_join(c, joined, NULL) == 0, "join a room from the directory");
    free(joined);
    amx_client_free(c);
    amx_cleanup();
    return mxt_done();
}
