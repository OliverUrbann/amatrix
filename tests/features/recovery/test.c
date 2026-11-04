/* Feature: security key / passphrase (secret storage), self-verification,
 * old messages from the key backup.
 *
 * The peer is a second device of OUR user (like Element on the phone): it
 * writes 5 messages into "Vor meiner Zeit" without ever giving this device a
 * key, puts the room key into the key backup, sets up cross-signing and
 * secret storage.  We unlock with the recovery key (EXTRA 1) and the
 * passphrase (EXTRA 2); the history must then be readable, and the peer
 * checks that our device is signed by the self-signing key. */
#include "mxtest.h"

static const char *const msgs[] = {
    "aus der Zeit vor Amatrix 1", "aus der Zeit vor Amatrix 2", "aus der Zeit vor Amatrix 3",
    "aus der Zeit vor Amatrix 4", "aus der Zeit vor Amatrix 5" };

static char *room;

static int have_room(void *arg)
{
    AmxClient *c = arg;
    AmxRoomInfo *l;
    int n = amx_rooms(c, &l), i;
    for (i = 0; i < n && !room; i++)
        if (!strcmp(l[i].name, "Vor meiner Zeit") && l[i].membership == AMX_ROOM_JOINED)
            room = mxt_dup(l[i].room_id);
    amx_free_rooms(l, n);
    return room != NULL;
}

int main(int argc, char **argv)
{
    MxtArgs a;
    AmxClient *c;
    time_t end;
    int r = -1, verified = 0, backup = 0, i, ok, at_start = 0;
    char nospace[128], *q;

    mxt_args(argc, argv, &a);
    if (a.n_extra < 2) { printf("need RECOVERYKEY PASSPHRASE\n"); return 10; }
    c = mxt_client(&a);
    MXT_OK(amx_backup_available(c) == 0, "no backup key before unlocking");
    MXT_OK(mxt_sync_until(c, have_room, c, 300), "room of the other device visible");

    /* the peer sets secret storage up last; until then: "no secret storage" */
    for (end = time(NULL) + 360; time(NULL) < end; ) {
        r = amx_recovery_unlock(c, a.extra[0], &verified, &backup);
        if (r == 0 || !strstr(amx_error(c), "no secret storage")) break;
        amx_sync(c, 3000);
    }
    MXT_OK(r == 0, "unlocked with the recovery key (%s)", r ? amx_error(c) : "ok");
    MXT_OK(verified, "own device signed (verified)");
    MXT_OK(backup && amx_backup_available(c), "backup key taken over");

    MXT_OK(amx_recovery_unlock(c, "EsTc 1111 2222 3333 4444 5555 6666 7777 8888 9999 aaaa bbbb", NULL, NULL) == -1 &&
           strstr(amx_error(c), "wrong"), "wrong key refused (%s)", amx_error(c));
    for (q = nospace, i = 0; a.extra[0][i] && q < nospace + sizeof nospace - 1; i++)
        if (a.extra[0][i] != ' ') *q++ = a.extra[0][i];
    *q = 0;
    MXT_OK(amx_recovery_unlock(c, nospace, NULL, NULL) == 0, "recovery key without spaces accepted");
    MXT_OK(amx_recovery_unlock(c, a.extra[1], NULL, &backup) == 0 && backup,
           "unlocked with the passphrase (PBKDF2)");

    /* the old messages: history fetches the room's keys from the backup */
    MXT_OK(amx_load_history(c, room, 20, 1, &at_start) > 0, "history loaded");
    for (ok = 1, i = 0; i < 5; i++) {
        MxtEvent *e = mxt_find(AMX_EV_MESSAGE, room, NULL, msgs[i]);
        if (!e || !e->encrypted) { ok = 0; mxt_diag("missing: %s", msgs[i]); }
    }
    MXT_OK(ok, "all 5 messages from before this device existed are readable");
    {
        AmxRoomInfo *l;
        int n = amx_rooms(c, &l), k, found = 0;
        for (k = 0; k < n; k++)
            if (!strcmp(l[k].room_id, room)) {
                found = 1;
                MXT_OK(l[k].last_body && !strcmp(l[k].last_body, msgs[4]),
                       "room list preview decrypted too (%s)", l[k].last_body ? l[k].last_body : "-");
            }
        if (!found) MXT_OK(0, "room in the list");
        amx_free_rooms(l, n);
    }

    /* after a restart the imported keys are still there */
    amx_client_free(c);
    mxt_n_ev = 0;
    c = mxt_client(&a);
    amx_load_history(c, room, 20, 1, NULL);
    MXT_OK(mxt_find(AMX_EV_MESSAGE, room, NULL, msgs[0]) != NULL, "after a restart still readable");
    MXT_OK(amx_backup_available(c), "backup key kept in the store");

    free(room);
    amx_client_free(c);
    amx_cleanup();
    return mxt_done();
}
