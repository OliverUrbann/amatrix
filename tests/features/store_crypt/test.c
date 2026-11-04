/* Feature: passphrase for the store (amx_store_*).
 * Receive an encrypted message, protect the store, check the files are
 * encrypted, restart: locked until the right passphrase, then the saved
 * login and the Megolm sessions work (history decrypts).  Change the
 * passphrase, remove it again. */
#include "mxtest.h"

static int have_msg(void *u) { (void)u; return mxt_find(AMX_EV_MESSAGE, NULL, NULL, "vorher geheim") != NULL; }

static char *raw(const MxtArgs *a, const char *name)
{
    static char buf[4096];
    char path[300];
    FILE *f;
    size_t n;
    snprintf(path, sizeof path, "%s/%s", a->store, name);
    if (!(f = fopen(path, "r"))) return NULL;
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}

static AmxClient *restart(const MxtArgs *a, AmxClient *c)
{
    amx_client_free(c);
    mxt_n_ev = 0;
    c = amx_client_new(a->server, a->store);
    amx_set_event_handler(c, mxt_record, NULL);
    if (mxt_getenv("AMX_VERBOSE")) amx_set_verbose(c, atoi(mxt_getenv("AMX_VERBOSE")));
    return c;
}

int main(int argc, char **argv)
{
    MxtArgs a;
    AmxClient *c;
    MxtEvent *inv;
    char *room = NULL, device[64], *t;

    mxt_args(argc, argv, &a);
    c = mxt_client(&a);
    snprintf(device, sizeof device, "%s", amx_device_id(c));
    MXT_OK(!amx_store_encrypted(c), "store not encrypted at first");
    MXT_OK(mxt_sync_until(c, mxt_have_invite, NULL, 180), "invited");
    inv = mxt_find(AMX_EV_INVITE, NULL, NULL, NULL);
    MXT_OK(inv && amx_join(c, inv->room_id, &room) == 0, "joined");
    MXT_OK(mxt_sync_until(c, have_msg, NULL, 180), "encrypted message received");

    MXT_OK(amx_store_set_passphrase(c, "Geheim 123") == 0, "passphrase set (%s)", amx_error(c));
    t = raw(&a, "login.json");
    MXT_OK(t && !strncmp(t, "AMXE1 ", 6) && !strstr(t, "access_token"), "login.json encrypted");
    t = raw(&a, "megolm_sessions");
    MXT_OK(t && !strncmp(t, "AMXE1 ", 6), "Megolm sessions encrypted");
    t = raw(&a, "store.json");
    MXT_OK(t && strstr(t, "pbkdf2"), "store.json describes the key derivation");
    amx_sync(c, 1000);                                  /* writes go on encrypted */
    t = raw(&a, "since");
    MXT_OK(t && !strncmp(t, "AMXE1 ", 6), "later writes encrypted too");

    c = restart(&a, c);
    MXT_OK(amx_store_encrypted(c) && amx_store_locked(c), "after a restart: locked");
    MXT_OK(amx_login(c, a.user, a.password) == -1 && strstr(amx_error(c), "locked"),
           "no login while locked (%s)", amx_error(c));
    MXT_OK(amx_store_unlock(c, "falsch") == -1, "wrong passphrase refused (%s)", amx_error(c));
    MXT_OK(amx_store_unlock(c, "Geheim 123") == 0, "right passphrase unlocks");
    MXT_OK(amx_login(c, a.user, a.password) == 0 && !strcmp(amx_device_id(c), device),
           "saved login reused (same device %s)", amx_device_id(c));
    amx_load_history(c, room, 10, 1, NULL);
    MXT_OK(have_msg(NULL), "Megolm session from the encrypted store: history decrypts");

    MXT_OK(amx_store_set_passphrase(c, "Neu 456") == 0, "passphrase changed");
    c = restart(&a, c);
    MXT_OK(amx_store_unlock(c, "Geheim 123") == -1 && amx_store_unlock(c, "Neu 456") == 0,
           "old passphrase refused, new one works");
    MXT_OK(amx_login(c, a.user, a.password) == 0 && !strcmp(amx_device_id(c), device), "logged in");

    MXT_OK(amx_store_set_passphrase(c, NULL) == 0, "passphrase removed");
    t = raw(&a, "login.json");
    MXT_OK(t && strstr(t, "access_token"), "files plain again");
    c = restart(&a, c);
    MXT_OK(!amx_store_encrypted(c) && amx_login(c, a.user, a.password) == 0 &&
           !strcmp(amx_device_id(c), device), "no passphrase needed any more");

    free(room);
    amx_client_free(c);
    amx_cleanup();
    return mxt_done();
}
