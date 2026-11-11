/* Feature: our sessions.
 * A second session of our user logs in; the list shows both (ours first),
 * the other one is renamed and signed out (wrong password refused); our
 * own session cannot be signed out this way.  The password is changed (and
 * back); deactivating with a wrong password is refused. */
#include "mxtest.h"

static const AmxDevice *find(const AmxDevice *l, int n, const char *id)
{
    int i;
    for (i = 0; i < n; i++) if (!strcmp(l[i].device_id, id)) return &l[i];
    return NULL;
}

int main(int argc, char **argv)
{
    MxtArgs a;
    AmxClient *c, *c2;
    AmxDevice *l = NULL;
    const AmxDevice *d;
    char *store2, *other = NULL;
    int n, i;

    mxt_args(argc, argv, &a);
    c = mxt_client(&a);
    store2 = malloc(strlen(a.store) + 8);
    sprintf(store2, "%s-zwei", a.store);
    c2 = amx_client_new(a.server, store2);
    MXT_OK(c2 && amx_login(c2, a.user, a.password) == 0, "second session logged in");

    n = amx_devices(c, &l);
    MXT_OK(n >= 2 && l[0].current && l[0].verified >= 0, "list: %d sessions, ours first, with keys", n);
    for (i = 0; i < n; i++) if (!l[i].current) { other = strdup(l[i].device_id); break; }
    MXT_OK(other != NULL, "the other session is there");
    amx_free_devices(l, n);
    if (!other) return mxt_done();

    MXT_OK(amx_rename_device(c, other, "Zweitgeraet") == 0, "renamed it");
    n = amx_devices(c, &l);
    d = find(l, n, other);
    MXT_OK(d && d->name && !strcmp(d->name, "Zweitgeraet"), "new name in the list (%s)", d && d->name ? d->name : "-");
    amx_free_devices(l, n);

    MXT_OK(amx_delete_device(c, other, "falsch") != 0, "wrong password refused (%s)", amx_error(c));
    MXT_OK(amx_delete_device(c, other, a.password) == 0, "signed it out (%s)", amx_error(c));
    n = amx_devices(c, &l);
    MXT_OK(n >= 1 && !find(l, n, other), "gone from the list (%d)", n);
    MXT_OK(amx_delete_device(c, l[0].device_id, a.password) != 0, "our own session: refused (%s)", amx_error(c));
    amx_free_devices(l, n);
    MXT_OK(amx_sync(c2, 0) != 0, "the signed-out session no longer works");

    /* the account: a new password (wrong old one refused), login with it,
     * back to the old one; deactivating with a wrong password is refused */
    MXT_OK(amx_change_password(c, "falsch", "NeuesPasswort42") != 0, "new password: wrong old one refused");
    MXT_OK(amx_change_password(c, a.password, "kurz") != 0, "new password: too short refused");
    MXT_OK(amx_change_password(c, a.password, "NeuesPasswort42") == 0, "password changed (%s)", amx_error(c));
    {
        char *s3 = malloc(strlen(a.store) + 8);
        AmxClient *c3;
        sprintf(s3, "%s-drei", a.store);
        c3 = amx_client_new(a.server, s3);
        MXT_OK(c3 && amx_login(c3, a.user, "NeuesPasswort42") == 0, "login with the new password");
        if (c3) { amx_logout(c3); amx_client_free(c3); }
        free(s3);
    }
    MXT_OK(amx_change_password(c, "NeuesPasswort42", a.password) == 0, "and back to the old one");
    MXT_OK(amx_deactivate_account(c, "falsch", 0) != 0, "deactivating with a wrong password: refused (%s)", amx_error(c));
    MXT_OK(amx_sync(c, 0) == 0, "the account still works");

    free(other); free(store2);
    amx_client_free(c2);
    amx_client_free(c);
    amx_cleanup();
    return mxt_done();
}
