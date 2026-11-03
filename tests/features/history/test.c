/* Feature: loading room history (amx_load_history).
 * Peer writes 25 messages "msg 01".."msg 25" in an encrypted room after we
 * joined.  We restart (as a client opening a room later would) and page
 * backwards in steps of 10: newest first, all 25 decrypted, flagged as
 * history, in order, then the beginning of the room is reported. */
#include "mxtest.h"

static int have_last(void *room) { return mxt_find(AMX_EV_MESSAGE, room, NULL, "msg 25") != NULL; }

int main(int argc, char **argv)
{
    MxtArgs a;
    AmxClient *c;
    MxtEvent *inv;
    char *room = NULL, want[16];
    int i, got, at_start = 0, calls = 0, first_batch, seen[26], order_ok = 1, last = 26, total = 0;

    mxt_args(argc, argv, &a);
    c = mxt_client(&a);
    MXT_OK(mxt_sync_until(c, mxt_have_invite, NULL, 180), "invited");
    inv = mxt_find(AMX_EV_INVITE, NULL, NULL, NULL);
    MXT_OK(inv && amx_join(c, inv->room_id, &room) == 0, "joined");
    if (!room) return mxt_done();
    MXT_OK(mxt_sync_until(c, have_last, room, 240), "live: 25 messages arrived");

    /* open the room again later: restart and load from the newest */
    amx_client_free(c);
    mxt_n_ev = 0;
    c = mxt_client(&a);
    memset(seen, 0, sizeof seen);
    got = amx_load_history(c, room, 10, 1, &at_start);
    MXT_OK(got == 10, "first page: 10 events (%d)", got);
    first_batch = mxt_n_ev;
    MXT_OK(first_batch > 0 && mxt_ev[0].history && mxt_ev[0].body && !strcmp(mxt_ev[0].body, "msg 25"),
           "first page starts with the newest message");
    while (!at_start && calls++ < 20) {
        got = amx_load_history(c, room, 10, 0, &at_start);
        if (got < 0) { MXT_OK(0, "load history: %s", amx_error(c)); break; }
    }
    MXT_OK(at_start, "beginning of the room reached");
    for (i = 0; i < mxt_n_ev; i++) {
        int k;
        if (mxt_ev[i].type != AMX_EV_MESSAGE || !mxt_ev[i].body || strncmp(mxt_ev[i].body, "msg ", 4)) continue;
        k = atoi(mxt_ev[i].body + 4);
        if (k < 1 || k > 25) continue;
        if (!mxt_ev[i].history || !mxt_ev[i].encrypted) order_ok = 0;
        if (k >= last) order_ok = 0;
        last = k;
        seen[k]++;
        total++;
    }
    for (i = 1; i <= 25; i++) if (seen[i] != 1) { snprintf(want, sizeof want, "msg %02d", i); break; }
    MXT_OK(i == 26, "all 25 messages exactly once%s%s", i == 26 ? "" : ", missing/double: ", i == 26 ? "" : want);
    MXT_OK(order_ok && total == 25, "newest first, decrypted, flagged as history");
    MXT_OK(mxt_count(AMX_EV_UNDECRYPTABLE, room, NULL) == 0, "nothing undecryptable after our join");
    MXT_OK(amx_load_history(c, room, 10, 0, &at_start) == 0 && at_start, "further calls: nothing more");
    MXT_OK(amx_load_history(c, room, 5, 1, NULL) == 5, "from_newest starts over");

    free(room);
    amx_client_free(c);
    amx_cleanup();
    return mxt_done();
}
