/* Feature: TLS certificate verification.
 * The trusted server (private test CA) must work; the same server behind a
 * self-signed certificate (EXTRA 1) must be refused. */
#include "mxtest.h"

int main(int argc, char **argv)
{
    MxtArgs a;
    AmxClient *c, *bad;
    char store[300];
    int r;

    mxt_args(argc, argv, &a);
    if (a.n_extra < 1) { MXT_OK(0, "untrusted URL given"); return mxt_done(); }
    c = mxt_client(&a);
    MXT_OK(1, "login over https with a trusted certificate");
    MXT_OK(amx_sync(c, 0) == 0, "sync over https");
    amx_client_free(c);

    snprintf(store, sizeof store, "%s-bad", a.store);
    bad = amx_client_new(a.extra[0], store);
    r = amx_login(bad, a.user, a.password);
    MXT_OK(r != 0, "login refused with an untrusted certificate");
    MXT_OK(strstr(amx_error(bad), "TLS") || strstr(amx_error(bad), "certificate"),
           "error names the TLS problem: %s", amx_error(bad));
    amx_client_free(bad);
    amx_cleanup();
    return mxt_done();
}
